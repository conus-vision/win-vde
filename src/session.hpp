// session.hpp - bounded, transactional browser-session decoding primitives.
#pragma once

#include "str_util.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct WinFp {
    std::string activeTitle;
    std::string activeDomain;
    std::map<std::string,int> counts;
    int tabCount=0;
    std::string tabsBlob;
};

static const unsigned MAX_JSON_DEPTH=128;
static const size_t MAX_JSON_NODES=2000000;
static const size_t MAX_JSON_DECODED_STRING_BYTES=256ULL*1024ULL*1024ULL;
static const unsigned long long MAX_BROWSER_SESSION_BYTES=512ULL*1024ULL*1024ULL;

struct JsonLimits {
    unsigned maxDepth;
    size_t maxNodes;
    size_t maxDecodedStringBytes;
    JsonLimits(unsigned depth=MAX_JSON_DEPTH,size_t nodes=MAX_JSON_NODES,
               size_t strings=MAX_JSON_DECODED_STRING_BYTES)
        :maxDepth(depth),maxNodes(nodes),maxDecodedStringBytes(strings){}
};

struct JValue {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t=NUL;
    bool b=false;
    double num=0;
    std::string str;
    std::vector<JValue> arr;
    std::map<std::string,JValue> obj;

    const JValue* find(const std::string& key) const {
        if(t!=OBJ) return nullptr;
        std::map<std::string,JValue>::const_iterator found=obj.find(key);
        return found==obj.end()?nullptr:&found->second;
    }
    int asInt(int fallback=0) const {
        if(t!=NUM || num<(double)INT_MIN || num>(double)INT_MAX) return fallback;
        return (int)num;
    }
    const std::string& asStr() const {
        static const std::string empty;
        return t==STR?str:empty;
    }
};

class JParser {
public:
    explicit JParser(const std::string& input,const JsonLimits& limits=JsonLimits())
        :p_(input.data()),end_(input.data()+input.size()),limits_(limits){}

    bool parse(JValue& output){
        output=JValue{};
        JValue parsed;
        try {
            whitespace();
            if(!value(parsed,0)) return false;
            whitespace();
            if(!ok_ || p_!=end_) return false;
            output=std::move(parsed);
            return true;
        } catch(const std::bad_alloc&) {
            output=JValue{};
            ok_=false;
            return false;
        } catch(const std::length_error&) {
            output=JValue{};
            ok_=false;
            return false;
        }
    }

private:
    const char* p_;
    const char* end_;
    JsonLimits limits_;
    size_t nodes_=0;
    size_t decodedStringBytes_=0;
    bool ok_=true;

    void fail(){ ok_=false; }
    void whitespace(){
        while(p_<end_ && (*p_==' ' || *p_=='\t' || *p_=='\n' || *p_=='\r')) ++p_;
    }
    bool reserveDecoded(size_t count){
        if(decodedStringBytes_>limits_.maxDecodedStringBytes ||
           count>limits_.maxDecodedStringBytes-decodedStringBytes_){ fail(); return false; }
        decodedStringBytes_+=count;
        return true;
    }
    bool appendByte(std::string& output,char byte){
        if(!reserveDecoded(1)) return false;
        output.push_back(byte);
        return true;
    }
    static size_t utf8Length(unsigned codePoint){
        return codePoint<=0x7f?1:(codePoint<=0x7ff?2:(codePoint<=0xffff?3:4));
    }
    bool appendUtf8(std::string& output,unsigned codePoint){
        size_t count=utf8Length(codePoint);
        if(!reserveDecoded(count)) return false;
        if(codePoint<=0x7f) output.push_back((char)codePoint);
        else if(codePoint<=0x7ff){
            output.push_back((char)(0xc0|(codePoint>>6)));
            output.push_back((char)(0x80|(codePoint&0x3f)));
        } else if(codePoint<=0xffff){
            output.push_back((char)(0xe0|(codePoint>>12)));
            output.push_back((char)(0x80|((codePoint>>6)&0x3f)));
            output.push_back((char)(0x80|(codePoint&0x3f)));
        } else {
            output.push_back((char)(0xf0|(codePoint>>18)));
            output.push_back((char)(0x80|((codePoint>>12)&0x3f)));
            output.push_back((char)(0x80|((codePoint>>6)&0x3f)));
            output.push_back((char)(0x80|(codePoint&0x3f)));
        }
        return true;
    }
    bool appendRawUtf8(std::string& output,unsigned char first){
        if(first<0x80) return appendByte(output,(char)first);
        unsigned codePoint=0;
        size_t continuation=0;
        unsigned minimum=0;
        if(first>=0xc2 && first<=0xdf){ codePoint=first&0x1f; continuation=1; minimum=0x80; }
        else if(first>=0xe0 && first<=0xef){ codePoint=first&0x0f; continuation=2; minimum=0x800; }
        else if(first>=0xf0 && first<=0xf4){ codePoint=first&0x07; continuation=3; minimum=0x10000; }
        else { fail(); return false; }
        if((size_t)(end_-p_)<continuation){ fail(); return false; }
        unsigned char encoded[4]={first,0,0,0};
        for(size_t i=0;i<continuation;++i){
            unsigned char next=(unsigned char)*p_++;
            if((next&0xc0)!=0x80){ fail(); return false; }
            encoded[i+1]=next;
            codePoint=(codePoint<<6)|(next&0x3f);
        }
        if(codePoint<minimum || codePoint>0x10ffff ||
           (codePoint>=0xd800 && codePoint<=0xdfff)){ fail(); return false; }
        size_t count=continuation+1;
        if(!reserveDecoded(count)) return false;
        output.append((const char*)encoded,count);
        return true;
    }
    bool hex4(unsigned& output){
        if((size_t)(end_-p_)<4){ fail(); return false; }
        unsigned value=0;
        for(int i=0;i<4;++i){
            char c=*p_++;
            value<<=4;
            if(c>='0' && c<='9') value|=(unsigned)(c-'0');
            else if(c>='a' && c<='f') value|=(unsigned)(c-'a'+10);
            else if(c>='A' && c<='F') value|=(unsigned)(c-'A'+10);
            else { fail(); return false; }
        }
        output=value;
        return true;
    }
    bool string(std::string& output){
        if(p_>=end_ || *p_!='\"'){ fail(); return false; }
        ++p_;
        while(p_<end_){
            unsigned char byte=(unsigned char)*p_++;
            if(byte=='\"') return true;
            if(byte<0x20){ fail(); return false; }
            if(byte!='\\'){
                if(!appendRawUtf8(output,byte)) return false;
                continue;
            }
            if(p_>=end_){ fail(); return false; }
            char escaped=*p_++;
            switch(escaped){
                case '\"': if(!appendByte(output,'\"')) return false; break;
                case '\\': if(!appendByte(output,'\\')) return false; break;
                case '/': if(!appendByte(output,'/')) return false; break;
                case 'b': if(!appendByte(output,'\b')) return false; break;
                case 'f': if(!appendByte(output,'\f')) return false; break;
                case 'n': if(!appendByte(output,'\n')) return false; break;
                case 'r': if(!appendByte(output,'\r')) return false; break;
                case 't': if(!appendByte(output,'\t')) return false; break;
                case 'u': {
                    unsigned codePoint=0;
                    if(!hex4(codePoint)) return false;
                    if(codePoint>=0xd800 && codePoint<=0xdbff){
                        if((size_t)(end_-p_)<2 || p_[0]!='\\' || p_[1]!='u'){
                            fail(); return false;
                        }
                        p_+=2;
                        unsigned low=0;
                        if(!hex4(low) || low<0xdc00 || low>0xdfff){ fail(); return false; }
                        codePoint=0x10000+((codePoint-0xd800)<<10)+(low-0xdc00);
                    } else if(codePoint>=0xdc00 && codePoint<=0xdfff){ fail(); return false; }
                    if(!appendUtf8(output,codePoint)) return false;
                    break;
                }
                default: fail(); return false;
            }
        }
        fail();
        return false;
    }
    bool number(JValue& output){
        const char* start=p_;
        if(p_<end_ && *p_=='-') ++p_;
        if(p_>=end_){ fail(); return false; }
        if(*p_=='0') ++p_;
        else if(*p_>='1' && *p_<='9') while(p_<end_ && *p_>='0' && *p_<='9') ++p_;
        else { fail(); return false; }
        if(p_<end_ && *p_=='.'){
            ++p_;
            const char* fraction=p_;
            while(p_<end_ && *p_>='0' && *p_<='9') ++p_;
            if(p_==fraction){ fail(); return false; }
        }
        if(p_<end_ && (*p_=='e' || *p_=='E')){
            ++p_;
            if(p_<end_ && (*p_=='+' || *p_=='-')) ++p_;
            const char* exponent=p_;
            while(p_<end_ && *p_>='0' && *p_<='9') ++p_;
            if(p_==exponent){ fail(); return false; }
        }
        std::string token(start,p_);
        errno=0;
        char* convertedEnd=nullptr;
        double converted=std::strtod(token.c_str(),&convertedEnd);
        if(errno==ERANGE || convertedEnd!=token.c_str()+token.size() || !std::isfinite(converted)){
            fail(); return false;
        }
        output.t=JValue::NUM;
        output.num=converted;
        return true;
    }
    bool value(JValue& output,unsigned depth){
        if(depth>limits_.maxDepth || nodes_>=limits_.maxNodes){ fail(); return false; }
        ++nodes_;
        whitespace();
        if(p_>=end_){ fail(); return false; }
        if(*p_=='\"'){ output.t=JValue::STR; return string(output.str); }
        if(*p_=='{') return object(output,depth);
        if(*p_=='[') return array(output,depth);
        if((size_t)(end_-p_)>=4 && std::memcmp(p_,"true",4)==0){
            p_+=4; output.t=JValue::BOOL; output.b=true; return true;
        }
        if((size_t)(end_-p_)>=5 && std::memcmp(p_,"false",5)==0){
            p_+=5; output.t=JValue::BOOL; output.b=false; return true;
        }
        if((size_t)(end_-p_)>=4 && std::memcmp(p_,"null",4)==0){
            p_+=4; output.t=JValue::NUL; return true;
        }
        return number(output);
    }
    bool object(JValue& output,unsigned depth){
        output.t=JValue::OBJ;
        ++p_;
        whitespace();
        if(p_<end_ && *p_=='}'){ ++p_; return true; }
        while(p_<end_){
            whitespace();
            std::string key;
            if(!string(key)) return false;
            whitespace();
            if(p_>=end_ || *p_!=':'){ fail(); return false; }
            ++p_;
            JValue child;
            if(!value(child,depth+1)) return false;
            output.obj[std::move(key)]=std::move(child);
            whitespace();
            if(p_<end_ && *p_==','){ ++p_; continue; }
            if(p_<end_ && *p_=='}'){ ++p_; return true; }
            fail(); return false;
        }
        fail(); return false;
    }
    bool array(JValue& output,unsigned depth){
        output.t=JValue::ARR;
        ++p_;
        whitespace();
        if(p_<end_ && *p_==']'){ ++p_; return true; }
        while(p_<end_){
            JValue child;
            if(!value(child,depth+1)) return false;
            output.arr.push_back(std::move(child));
            whitespace();
            if(p_<end_ && *p_==','){ ++p_; continue; }
            if(p_<end_ && *p_==']'){ ++p_; return true; }
            fail(); return false;
        }
        fail(); return false;
    }
};

inline bool FirefoxSelectedTabMatches(int selected,size_t tabIndex){
    return selected>0 && tabIndex==(size_t)(selected-1);
}

inline bool ExtractFirefoxWindows(const JValue& root,std::vector<WinFp>& output){
    output.clear();
    std::vector<WinFp> parsed;
    try {
        const JValue* windows=root.find("windows");
        if(!windows || windows->t!=JValue::ARR) return false;
        for(size_t wi=0;wi<windows->arr.size();++wi){
            const JValue& window=windows->arr[wi];
            if(window.t!=JValue::OBJ) return false;
            const JValue* tabs=window.find("tabs");
            if(!tabs || tabs->t!=JValue::ARR || tabs->arr.size()>(size_t)INT_MAX) return false;
            int selected=1;
            if(const JValue* selectedValue=window.find("selected")){
                if(selectedValue->t!=JValue::NUM) return false;
                selected=selectedValue->asInt(1);
            }
            WinFp fingerprint;
            fingerprint.tabCount=(int)tabs->arr.size();
            for(size_t ti=0;ti<tabs->arr.size();++ti){
                const JValue& tab=tabs->arr[ti];
                if(tab.t!=JValue::OBJ) return false;
                const JValue* entries=tab.find("entries");
                if(!entries || entries->t!=JValue::ARR) return false;
                if(entries->arr.empty()) continue;
                int index=(int)(std::min)(entries->arr.size(),(size_t)INT_MAX);
                if(const JValue* indexValue=tab.find("index")){
                    if(indexValue->t!=JValue::NUM) return false;
                    index=indexValue->asInt(index);
                }
                if(index<1) index=1;
                if((size_t)index>entries->arr.size()) index=(int)entries->arr.size();
                const JValue& current=entries->arr[(size_t)index-1];
                if(current.t!=JValue::OBJ) return false;
                const JValue* urlValue=current.find("url");
                const JValue* titleValue=current.find("title");
                if((urlValue && urlValue->t!=JValue::STR) || (titleValue && titleValue->t!=JValue::STR)) return false;
                const std::string url=urlValue?urlValue->str:std::string();
                const std::string title=titleValue?titleValue->str:std::string();
                std::string domain=etld1(hostOf(url));
                if(!domain.empty()) ++fingerprint.counts[domain];
                fingerprint.tabsBlob+=title;
                fingerprint.tabsBlob.push_back(' ');
                fingerprint.tabsBlob+=url;
                fingerprint.tabsBlob.push_back(' ');
                if(FirefoxSelectedTabMatches(selected,ti)){ fingerprint.activeTitle=title; fingerprint.activeDomain=domain; }
            }
            parsed.push_back(std::move(fingerprint));
        }
        output.swap(parsed);
        return true;
    } catch(const std::bad_alloc&) { output.clear(); return false; }
      catch(const std::length_error&) { output.clear(); return false; }
}

inline bool ParseFirefoxSessionJson(const std::string& json,std::vector<WinFp>& output){
    output.clear();
    JValue root;
    if(!JParser(json).parse(root)) return false;
    return ExtractFirefoxWindows(root,output);
}

static const size_t MAX_CHROMIUM_WINDOWS=10000;
static const size_t MAX_CHROMIUM_TABS=100000;
static const size_t MAX_CHROMIUM_NAVIGATIONS=1000000;
static const size_t MAX_CHROMIUM_COMMANDS=2000000;
static const size_t MAX_CHROMIUM_SEARCH_TEXT_PER_WINDOW=4ULL*1024ULL*1024ULL;
static const size_t MAX_CHROMIUM_RETAINED_TEXT_BYTES=256ULL*1024ULL*1024ULL;

struct SnssLimits {
    size_t maxWindows,maxTabs,maxNavigations,maxCommands,maxSearchTextPerWindow,maxRetainedTextBytes;
    SnssLimits(size_t windows=MAX_CHROMIUM_WINDOWS,size_t tabs=MAX_CHROMIUM_TABS,
               size_t navigations=MAX_CHROMIUM_NAVIGATIONS,size_t commands=MAX_CHROMIUM_COMMANDS,
               size_t search=MAX_CHROMIUM_SEARCH_TEXT_PER_WINDOW,
               size_t retained=MAX_CHROMIUM_RETAINED_TEXT_BYTES)
        :maxWindows(windows),maxTabs(tabs),maxNavigations(navigations),maxCommands(commands),
         maxSearchTextPerWindow(search),maxRetainedTextBytes(retained){}
};

inline bool AppendUtf8Scalar(std::string& output,unsigned codePoint){
    if(codePoint>0x10ffff || (codePoint>=0xd800 && codePoint<=0xdfff)) return false;
    if(codePoint<=0x7f) output.push_back((char)codePoint);
    else if(codePoint<=0x7ff){ output.push_back((char)(0xc0|(codePoint>>6))); output.push_back((char)(0x80|(codePoint&0x3f))); }
    else if(codePoint<=0xffff){ output.push_back((char)(0xe0|(codePoint>>12))); output.push_back((char)(0x80|((codePoint>>6)&0x3f))); output.push_back((char)(0x80|(codePoint&0x3f))); }
    else { output.push_back((char)(0xf0|(codePoint>>18))); output.push_back((char)(0x80|((codePoint>>12)&0x3f))); output.push_back((char)(0x80|((codePoint>>6)&0x3f))); output.push_back((char)(0x80|(codePoint&0x3f))); }
    return true;
}

struct SnssPR {
    const uint8_t* p; size_t n; size_t i=0;
    bool align(){
        if(i>(std::numeric_limits<size_t>::max)()-3) return false;
        size_t aligned=(i+3)&~size_t(3);
        if(aligned>n) return false;
        while(i<aligned) if(p[i++]!=0) return false;
        return true;
    }
    bool rInt(int32_t& output){
        if(!align() || n-i<4) return false;
        uint32_t bits=(uint32_t)p[i]|((uint32_t)p[i+1]<<8)|((uint32_t)p[i+2]<<16)|((uint32_t)p[i+3]<<24);
        i+=4; output=(int32_t)bits; return true;
    }
    bool rStr(std::string& output){
        int32_t length=0;
        if(!rInt(length) || length<0 || (size_t)length>n-i) return false;
        output.assign((const char*)p+i,(size_t)length); i+=(size_t)length; return true;
    }
    bool rStr16(std::string& output){
        int32_t length=0;
        if(!rInt(length) || length<0) return false;
        size_t units=(size_t)length;
        if(units>(std::numeric_limits<size_t>::max)()/2 || units*2>n-i) return false;
        size_t stop=i+units*2;
        while(i<stop){
            unsigned first=(unsigned)p[i]|((unsigned)p[i+1]<<8); i+=2;
            unsigned codePoint=first;
            if(first>=0xd800 && first<=0xdbff){
                if(stop-i<2) return false;
                unsigned low=(unsigned)p[i]|((unsigned)p[i+1]<<8); i+=2;
                if(low<0xdc00 || low>0xdfff) return false;
                codePoint=0x10000+((first-0xd800)<<10)+(low-0xdc00);
            } else if(first>=0xdc00 && first<=0xdfff) return false;
            if(!AppendUtf8Scalar(output,codePoint)) return false;
        }
        return true;
    }
    bool finished(){ return align() && i==n; }
};

inline int32_t ReadSnssI32(const uint8_t* bytes){
    uint32_t bits=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|((uint32_t)bytes[2]<<16)|((uint32_t)bytes[3]<<24);
    return (int32_t)bits;
}

inline bool ParseChromiumSNSS(const std::string& data,std::vector<WinFp>& output,
                              const SnssLimits& limits=SnssLimits()){
    output.clear();
    std::vector<WinFp> parsed;
    try {
        if((unsigned long long)data.size()>MAX_BROWSER_SESSION_BYTES) return false;
        const uint8_t* bytes=(const uint8_t*)data.data(); size_t size=data.size();
        if(size<8 || std::memcmp(bytes,"SNSS",4)!=0 || ReadSnssI32(bytes+4)<=0) return false;
        std::map<int32_t,int32_t> tabWindow,tabIndex,windowSelected,tabSelectedNavigation;
        typedef std::pair<std::string,std::string> Navigation;
        std::map<int32_t,std::map<int32_t,Navigation> > tabNavigations;
        std::set<int32_t> windows,tabs;
        size_t navigationCount=0,commandCount=0,retainedText=0;
        auto acceptWindow=[&](int32_t id)->bool{
            if(id<0) return false;
            if(windows.find(id)!=windows.end()) return true;
            if(windows.size()>=limits.maxWindows) return false;
            windows.insert(id); return true;
        };
        auto acceptTab=[&](int32_t id)->bool{
            if(id<0) return false;
            if(tabs.find(id)!=tabs.end()) return true;
            if(tabs.size()>=limits.maxTabs) return false;
            tabs.insert(id); return true;
        };
        size_t position=8;
        while(position<size){
            if(commandCount>=limits.maxCommands || size-position<2) return false;
            ++commandCount;
            uint16_t commandSize=(uint16_t)((uint16_t)bytes[position]|((uint16_t)bytes[position+1]<<8));
            position+=2;
            if(commandSize==0 || (size_t)commandSize>size-position) return false;
            uint8_t id=bytes[position]; const uint8_t* command=bytes+position+1;
            size_t commandLength=(size_t)commandSize-1; position+=(size_t)commandSize;
            auto raw2=[&](int32_t& first,int32_t& second)->bool{
                if(commandLength!=8) return false;
                first=ReadSnssI32(command); second=ReadSnssI32(command+4); return true;
            };
            if(id==0){ int32_t window=0,tab=0; if(!raw2(window,tab)||!acceptWindow(window)||!acceptTab(tab)) return false; tabWindow[tab]=window; }
            else if(id==2){ int32_t tab=0,index=0; if(!raw2(tab,index)||!acceptTab(tab)||index<0) return false; tabIndex[tab]=index; }
            else if(id==7){ int32_t tab=0,index=0; if(!raw2(tab,index)||!acceptTab(tab)||index<0) return false; tabSelectedNavigation[tab]=index; }
            else if(id==8){ int32_t window=0,index=0; if(!raw2(window,index)||!acceptWindow(window)||index<0) return false; windowSelected[window]=index; }
            else if(id==6){
                if(commandLength<4) return false;
                uint32_t declared=(uint32_t)command[0]|((uint32_t)command[1]<<8)|((uint32_t)command[2]<<16)|((uint32_t)command[3]<<24);
                if((size_t)declared!=commandLength-4) return false;
                SnssPR reader{command+4,commandLength-4}; int32_t tab=0,navigation=0; std::string url,title;
                if(!reader.rInt(tab)||!reader.rInt(navigation)||tab<0||navigation<0||!reader.rStr(url)||!reader.rStr16(title)||!reader.finished()||!acceptTab(tab)) return false;
                std::map<int32_t,std::map<int32_t,Navigation> >::iterator tf=tabNavigations.find(tab);
                bool duplicate=tf!=tabNavigations.end() && tf->second.find(navigation)!=tf->second.end();
                if(!duplicate && navigationCount>=limits.maxNavigations) return false;
                size_t oldBytes=0;
                if(duplicate){ Navigation& old=tf->second.find(navigation)->second; oldBytes=old.first.size()+old.second.size(); }
                if(url.size()>(std::numeric_limits<size_t>::max)()-title.size() || oldBytes>retainedText) return false;
                size_t newBytes=url.size()+title.size(),withoutOld=retainedText-oldBytes;
                if(withoutOld>limits.maxRetainedTextBytes || newBytes>limits.maxRetainedTextBytes-withoutOld) return false;
                if(!duplicate) ++navigationCount;
                retainedText=withoutOld+newBytes;
                tabNavigations[tab][navigation]=Navigation(std::move(url),std::move(title));
            }
        }
        std::map<int32_t,std::vector<int32_t> > windowTabs;
        for(std::map<int32_t,int32_t>::const_iterator it=tabWindow.begin();it!=tabWindow.end();++it) windowTabs[it->second].push_back(it->first);
        auto currentNavigation=[&](int32_t tab)->const Navigation*{
            std::map<int32_t,std::map<int32_t,Navigation> >::const_iterator found=tabNavigations.find(tab);
            if(found==tabNavigations.end()||found->second.empty()) return nullptr;
            int32_t selected=found->second.rbegin()->first;
            std::map<int32_t,int32_t>::const_iterator sf=tabSelectedNavigation.find(tab);
            if(sf!=tabSelectedNavigation.end()) selected=sf->second;
            std::map<int32_t,Navigation>::const_iterator nav=found->second.find(selected);
            if(nav==found->second.end()) nav=std::prev(found->second.end());
            return &nav->second;
        };
        for(std::map<int32_t,std::vector<int32_t> >::const_iterator it=windowTabs.begin();it!=windowTabs.end();++it){
            WinFp fingerprint; int32_t selectedIndex=-1,activeTab=-1;
            std::map<int32_t,int32_t>::const_iterator selected=windowSelected.find(it->first);
            if(selected!=windowSelected.end()) selectedIndex=selected->second;
            for(size_t ti=0;ti<it->second.size();++ti){
                int32_t tab=it->second[ti]; const Navigation* navigation=currentNavigation(tab); const std::string empty;
                const std::string& url=navigation?navigation->first:empty; const std::string& title=navigation?navigation->second:empty;
                std::string domain=etld1(hostOf(url)); if(!domain.empty()) ++fingerprint.counts[domain];
                if(url.size()>(std::numeric_limits<size_t>::max)()-title.size()-2) return false;
                size_t added=title.size()+url.size()+2;
                if(fingerprint.tabsBlob.size()>limits.maxSearchTextPerWindow || added>limits.maxSearchTextPerWindow-fingerprint.tabsBlob.size()) return false;
                fingerprint.tabsBlob+=title; fingerprint.tabsBlob.push_back(' '); fingerprint.tabsBlob+=url; fingerprint.tabsBlob.push_back(' ');
                ++fingerprint.tabCount;
                std::map<int32_t,int32_t>::const_iterator index=tabIndex.find(tab);
                if(index!=tabIndex.end()&&index->second==selectedIndex) activeTab=tab;
            }
            if(activeTab<0&&!it->second.empty()) activeTab=it->second.front();
            const Navigation* active=currentNavigation(activeTab);
            if(active){ fingerprint.activeTitle=active->second; fingerprint.activeDomain=etld1(hostOf(active->first)); }
            if(fingerprint.tabCount>0) parsed.push_back(std::move(fingerprint));
        }
        output.swap(parsed); return true;
    } catch(const std::bad_alloc&) { output.clear(); return false; }
      catch(const std::length_error&) { output.clear(); return false; }
}

inline bool Lz4CheckedAdd(size_t& value,size_t increment){
    if(increment>(std::numeric_limits<size_t>::max)()-value) return false;
    value+=increment;
    return true;
}

inline long Lz4BlockDecompress(const uint8_t* source,size_t sourceLength,uint8_t* destination,size_t destinationCapacity){
    if((sourceLength&&!source)||(destinationCapacity&&!destination)) return -1;
    size_t sourcePos=0,destinationPos=0;
    while(sourcePos<sourceLength){
        uint8_t token=source[sourcePos++]; size_t literalLength=(size_t)(token>>4);
        if(literalLength==15){ uint8_t next=0; do { if(sourcePos>=sourceLength) return -1; next=source[sourcePos++]; if(!Lz4CheckedAdd(literalLength,(size_t)next)) return -1; } while(next==255); }
        if(literalLength>sourceLength-sourcePos||literalLength>destinationCapacity-destinationPos) return -1;
        if(literalLength) std::memcpy(destination+destinationPos,source+sourcePos,literalLength);
        sourcePos+=literalLength; destinationPos+=literalLength;
        if(destinationPos==destinationCapacity) return sourcePos==sourceLength?(long)destinationPos:-1;
        if(sourcePos==sourceLength) return (long)destinationPos;
        if(sourceLength-sourcePos<2) return -1;
        size_t offset=(size_t)source[sourcePos]|((size_t)source[sourcePos+1]<<8); sourcePos+=2;
        if(offset==0||offset>destinationPos) return -1;
        size_t matchLength=(size_t)(token&0x0f);
        if(matchLength==15){ uint8_t next=0; do { if(sourcePos>=sourceLength) return -1; next=source[sourcePos++]; if(!Lz4CheckedAdd(matchLength,(size_t)next)) return -1; } while(next==255); }
        if(!Lz4CheckedAdd(matchLength,4)) return -1;
        if(matchLength>destinationCapacity-destinationPos) return -1;
        size_t matchPos=destinationPos-offset;
        for(size_t i=0;i<matchLength;++i) destination[destinationPos+i]=destination[matchPos+i];
        destinationPos+=matchLength;
        if(destinationPos==destinationCapacity) return sourcePos==sourceLength?(long)destinationPos:-1;
    }
    return (long)destinationPos;
}

inline bool MozLz4Decompress(const std::string& data,unsigned long long outputLimit,std::string& output){
    static const uint8_t magic[8]={0x6d,0x6f,0x7a,0x4c,0x7a,0x34,0x30,0x00};
    output.clear();
    if(data.size()<12||(unsigned long long)data.size()>MAX_BROWSER_SESSION_BYTES||std::memcmp(data.data(),magic,8)!=0) return false;
    const uint8_t* bytes=(const uint8_t*)data.data();
    uint32_t declared=(uint32_t)bytes[8]|((uint32_t)bytes[9]<<8)|((uint32_t)bytes[10]<<16)|((uint32_t)bytes[11]<<24);
    unsigned long long effectiveLimit=(std::min)(outputLimit,MAX_BROWSER_SESSION_BYTES);
    if((unsigned long long)declared>effectiveLimit||(size_t)declared>output.max_size()) return false;
    try { output.resize((size_t)declared); }
    catch(const std::bad_alloc&) { output.clear(); return false; }
    catch(const std::length_error&) { output.clear(); return false; }
    if(declared==0){ if(data.size()!=12){ output.clear(); return false; } return true; }
    long decoded=Lz4BlockDecompress(bytes+12,data.size()-12,(uint8_t*)&output[0],(size_t)declared);
    if(decoded<0||(unsigned long long)decoded!=(unsigned long long)declared){ output.clear(); return false; }
    return true;
}

struct SessionStamp {
    unsigned long long size=0,mtime=0;
    bool operator==(const SessionStamp& other) const { return size==other.size&&mtime==other.mtime; }
    bool operator!=(const SessionStamp& other) const { return !(*this==other); }
};

inline bool GetSessionStamp(const std::wstring& path,SessionStamp& output){
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes)||(attributes.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) return false;
    ULARGE_INTEGER size{},mtime{};
    size.LowPart=attributes.nFileSizeLow; size.HighPart=attributes.nFileSizeHigh;
    mtime.LowPart=attributes.ftLastWriteTime.dwLowDateTime; mtime.HighPart=attributes.ftLastWriteTime.dwHighDateTime;
    if(size.QuadPart>MAX_BROWSER_SESSION_BYTES) return false;
    SessionStamp stamp; stamp.size=size.QuadPart; stamp.mtime=mtime.QuadPart; output=stamp; return true;
}
