// session.hpp — per-window fingerprints from browser session data.
// WinFp is the neutral type both readers emit. Firefox's sessionstore reader
// currently lives in vde.cpp and also produces WinFp; the Chromium SNSS reader
// lives here. Pure logic (no COM/GUI), unit-tested in tests/vdtest.cpp.
#pragma once
#include "str_util.hpp"   // hostOf, etld1, ReadFileBytes
#include <iterator>

struct WinFp { std::string activeTitle, activeDomain; std::map<std::string,int> counts; int tabCount=0; std::string tabsBlob; };  // tabsBlob = all tab titles+domains (UTF-8), for search-all-tabs

// ---- Chromium SNSS session reader ----------------------------------------
// File = "SNSS" + int32 version + repeated [uint16 size][uint8 id][pickle].
// pickle = [uint32 payloadSize][payload]; fields are base::Pickle (4-byte aligned).
// Commands: 0=SetTabWindow[win,tab] 2=SetTabIndexInWindow[tab,idx]
//           6=UpdateTabNavigation[tab,navIdx,url,title] 7=SetSelectedNavigationIndex[tab,idx]
//           8=SetSelectedTabInIndex[win,idx]
struct SnssPR { const uint8_t* p; size_t n; size_t i=0;
    void align(){ i=(i+3)&~size_t(3); }
    bool rInt(int32_t& v){ align(); if(i+4>n)return false; v=(int32_t)(p[i]|(p[i+1]<<8)|(p[i+2]<<16)|((uint32_t)p[i+3]<<24)); i+=4; return true; }
    bool rStr(std::string& s){ int32_t L; if(!rInt(L))return false; if(L<0||i+(size_t)L>n)return false; s.assign((const char*)p+i,(size_t)L); i+=(size_t)L; return true; }
    bool rStr16(std::string& out){ int32_t L; if(!rInt(L))return false; if(L<0) return false; size_t bytes=(size_t)L*2; if(i+bytes>n) return false;
        std::wstring w((size_t)L,0); for(int k=0;k<L;k++) w[k]=(wchar_t)(p[i+2*k]|(p[i+2*k+1]<<8)); i+=bytes; out=W2U8(w); return true; }
};
inline std::vector<WinFp> ParseChromiumSNSS(const std::string& data){
    std::vector<WinFp> out;
    const uint8_t* b=(const uint8_t*)data.data(); size_t sz=data.size();
    if(sz<8 || !(b[0]=='S'&&b[1]=='N'&&b[2]=='S'&&b[3]=='S')) return out;
    std::map<int,int> tabWin, tabIdx, winSel, tabSelNav;
    std::map<int,std::map<int,std::pair<std::string,std::string>>> tabNav;   // tab -> navIdx -> {url,title}
    size_t pos=8;
    while(pos+2<=sz){
        uint16_t cs=(uint16_t)(b[pos]|(b[pos+1]<<8)); pos+=2; if(cs==0||pos+cs>sz)break;
        uint8_t id=b[pos]; const uint8_t* c=b+pos+1; size_t clen=(size_t)cs-1; pos+=cs;
        // Commands 0/2/7/8 are raw fixed structs of two int32 (no pickle header);
        // command 6 (nav) is a base::Pickle ([uint32 hdr] + 4-byte-aligned fields).
        auto raw2=[&](int32_t& a,int32_t& b2)->bool{ if(clen<8) return false;
            a=(int32_t)(c[0]|(c[1]<<8)|(c[2]<<16)|((uint32_t)c[3]<<24));
            b2=(int32_t)(c[4]|(c[5]<<8)|(c[6]<<16)|((uint32_t)c[7]<<24)); return true; };
        if(id==0){ int32_t w,t; if(raw2(w,t)) tabWin[t]=w; }         // [window, tab]
        else if(id==2){ int32_t t,ix; if(raw2(t,ix)) tabIdx[t]=ix; } // [tab, index]
        else if(id==7){ int32_t t,ix; if(raw2(t,ix)) tabSelNav[t]=ix; } // [tab, nav index]
        else if(id==8){ int32_t w,ix; if(raw2(w,ix)) winSel[w]=ix; } // [window, tab index]
        else if(id==6 && clen>=4){ SnssPR pr{c+4,clen-4};           // skip 4-byte pickle header
            int32_t t,ni; std::string url,title;                    // url = UTF-8 WriteString, title = UTF-16 WriteString16
            if(pr.rInt(t)&&pr.rInt(ni)&&pr.rStr(url)&&pr.rStr16(title))
                tabNav[t][ni]={ url, title };                       // keep full URL; domain computed at aggregation
        }
    }
    std::map<int,std::vector<int>> winTabs; for(auto& kv:tabWin) winTabs[kv.second].push_back(kv.first);
    auto curNav=[&](int tab)->std::pair<std::string,std::string>{
        auto it=tabNav.find(tab); if(it==tabNav.end()||it->second.empty()) return {"",""};
        int sel = tabSelNav.count(tab)?tabSelNav[tab]:it->second.rbegin()->first;
        auto e=it->second.find(sel); if(e==it->second.end()) e=std::prev(it->second.end()); return e->second;
    };
    for(auto& kv:winTabs){ int w=kv.first; WinFp fp;
        int selIdx = winSel.count(w)?winSel[w]:-1; int activeTab=-1;
        for(int t:kv.second){ auto nav=curNav(t); std::string dom=etld1(hostOf(nav.first)); if(!dom.empty()) fp.counts[dom]++; fp.tabCount++;
            fp.tabsBlob += nav.second; fp.tabsBlob += ' '; fp.tabsBlob += nav.first; fp.tabsBlob += ' ';   // every tab: title + full URL (address bar)
            if(tabIdx.count(t)&&tabIdx[t]==selIdx) activeTab=t; }
        if(activeTab<0 && !kv.second.empty()) activeTab=kv.second.front();
        auto an=curNav(activeTab); fp.activeTitle=an.second; fp.activeDomain=etld1(hostOf(an.first));
        if(fp.tabCount>0) out.push_back(fp);
    }
    return out;
}
inline std::wstring FindChromiumSessionFile(const std::wstring& userDataDir){
    std::wstring pattern=userDataDir+L"\\Default\\Sessions\\Session_*";
    WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW(pattern.c_str(),&fd); if(h==INVALID_HANDLE_VALUE) return L"";
    std::wstring best; ULARGE_INTEGER bestT; bestT.QuadPart=0;
    do{ if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue;
        if(fd.nFileSizeLow==0&&fd.nFileSizeHigh==0) continue;
        ULARGE_INTEGER t; t.LowPart=fd.ftLastWriteTime.dwLowDateTime; t.HighPart=fd.ftLastWriteTime.dwHighDateTime;
        if(t.QuadPart>bestT.QuadPart){ bestT=t; best=userDataDir+L"\\Default\\Sessions\\"+fd.cFileName; }
    }while(FindNextFileW(h,&fd)); FindClose(h); return best;
}
inline std::vector<WinFp> ReadChromiumWindows(const std::wstring& userDataDir){
    std::wstring f=FindChromiumSessionFile(userDataDir); if(f.empty()) return {};
    std::string bytes; if(!ReadFileBytes(f,bytes)) return {};
    return ParseChromiumSNSS(bytes);
}
