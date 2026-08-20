// session_worker.hpp - bounded asynchronous browser-session acquisition.
#pragma once

#include "appprofile.hpp"
#include "session.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#pragma comment(lib,"user32.lib")

static const UINT WM_SESSION_RESULT=WM_APP+12;
static const size_t MAX_SESSION_CACHE_ENTRIES=16;
static const size_t MAX_SESSION_CACHE_BYTES=256ULL*1024ULL*1024ULL;

enum class SessionDataStatus { Fresh, CachedStale, Unavailable, Superseded };
enum class SessionPurpose {
    AutoReconcile, HeartbeatSave, ManualSave, ManualRestore, Search, MetadataProbe
};
enum class SessionWorkerStep {
    RequestPrepare, ResultPrepare, AfterChoose, ActiveCopy, CacheLookup,
    GenerationPrepare, GenerationPublish
};
enum class SessionCoordinatorStep {
    RequestPrepare, PendingInsert, LatestRequestInsert, AcceptPrepare, LatestResultInsert
};

struct SessionCoordinatorOps {
    std::function<void(SessionCoordinatorStep)> beforeStep;
};

struct SessionRequest {
    uint64_t requestId=0;
    std::string app;
    AppProfile profile;
    SessionPurpose purpose=SessionPurpose::MetadataProbe;
    uint64_t identityGeneration=0;
    SessionRequest(){ profile.session=AppProfile::NONE; }
};

struct SessionResult {
    uint64_t requestId=0;
    std::string app;
    std::wstring path;
    SessionPurpose purpose=SessionPurpose::MetadataProbe;
    uint64_t identityGeneration=0;
    SessionStamp sourceStamp;
    bool sourceStampKnown=false;
    SessionStamp dataStamp;
    uint64_t dataGeneration=0;
    SessionDataStatus status=SessionDataStatus::Unavailable;
    std::shared_ptr<const std::vector<WinFp> > windows;
};

inline bool SessionDataUsable(SessionDataStatus status){
    return status==SessionDataStatus::Fresh || status==SessionDataStatus::CachedStale;
}

struct SessionPolicy {
    bool matchExisting=false;
    bool restoreExisting=false;
    bool createUnmatched=false;
    bool updateFingerprints=false;
    bool markMissing=false;
    bool unmatchedLiveWaits=false;
    bool deferOnce=false;
};

inline SessionPolicy SessionAcceptancePolicy(SessionDataStatus status){
    SessionPolicy policy;
    if(status==SessionDataStatus::Fresh){
        policy.matchExisting=true;
        policy.restoreExisting=true;
        policy.createUnmatched=true;
        policy.updateFingerprints=true;
        policy.markMissing=true;
    } else if(status==SessionDataStatus::CachedStale){
        policy.matchExisting=true;
        policy.restoreExisting=true;
        policy.unmatchedLiveWaits=true;
    } else if(status==SessionDataStatus::Unavailable){
        policy.unmatchedLiveWaits=true;
        policy.deferOnce=true;
    }
    return policy;
}

inline size_t SessionSaturatingAdd(size_t left,size_t right){
    return right>(std::numeric_limits<size_t>::max)()-left ?
        (std::numeric_limits<size_t>::max)() : left+right;
}

inline size_t EstimateSessionPayloadBytes(const std::vector<WinFp>& windows){
    size_t total=sizeof(std::vector<WinFp>);
    total=SessionSaturatingAdd(total,windows.capacity()*sizeof(WinFp));
    for(size_t i=0;i<windows.size();++i){
        const WinFp& window=windows[i];
        total=SessionSaturatingAdd(total,window.activeTitle.capacity());
        total=SessionSaturatingAdd(total,window.activeDomain.capacity());
        total=SessionSaturatingAdd(total,window.tabsBlob.capacity());
        for(std::map<std::string,int>::const_iterator it=window.counts.begin();it!=window.counts.end();++it){
            size_t node=sizeof(std::pair<const std::string,int>)+3*sizeof(void*);
            node=SessionSaturatingAdd(node,it->first.capacity());
            total=SessionSaturatingAdd(total,node);
        }
    }
    return total;
}

class SessionRetainedBudget {
public:
    explicit SessionRetainedBudget(size_t limit):limit_(limit){}
    bool Acquire(size_t bytes){
        std::lock_guard<std::mutex> lock(mutex_);
        if(used_>limit_ || bytes>limit_-used_) return false;
        used_+=bytes;
        return true;
    }
    void Release(size_t bytes){
        std::lock_guard<std::mutex> lock(mutex_);
        used_=bytes>used_?0:used_-bytes;
    }
    size_t Used() const { std::lock_guard<std::mutex> lock(mutex_); return used_; }
    size_t Limit() const { return limit_; }
private:
    size_t limit_;
    mutable std::mutex mutex_;
    size_t used_=0;
};

struct SessionPayloadHolder {
    std::unique_ptr<std::vector<WinFp> > windows;
    std::shared_ptr<SessionRetainedBudget> budget;
    size_t retainedBytes=0;
    std::atomic<bool> accounted;
    SessionPayloadHolder(std::unique_ptr<std::vector<WinFp> >&& value,
                         const std::shared_ptr<SessionRetainedBudget>& retainedBudget,
                         size_t retained)
        :windows(std::move(value)),budget(retainedBudget),retainedBytes(retained),accounted(false){}
    ~SessionPayloadHolder(){
        if(accounted.load(std::memory_order_acquire)) budget->Release(retainedBytes);
    }
};

struct SessionCacheValue {
    std::wstring path;
    SessionStamp stamp;
    uint64_t contentHash=0;
    uint64_t dataGeneration=0;
    size_t retainedBytes=0;
    std::shared_ptr<const std::vector<WinFp> > windows;
};

enum class SessionCachePutStep {
    PayloadStorage, PayloadControl, KeyCopy, PathCopy, StampCopy, OutputCopy,
    ContainerReserve, DuplicateReplacement, ContainerInsert, OrderPublish
};

struct SessionCacheOps {
    std::function<void(SessionCachePutStep)> beforePutStep;
    std::function<void()> beforeLookupOutput;
};

class SessionCache {
public:
    explicit SessionCache(size_t maxEntries=MAX_SESSION_CACHE_ENTRIES,
                          size_t maxBytes=MAX_SESSION_CACHE_BYTES,
                          const SessionCacheOps& ops=SessionCacheOps())
        :maxEntries_(maxEntries),budget_(std::make_shared<SessionRetainedBudget>(maxBytes)),ops_(ops){}

    bool FindExact(const std::string& app,const std::wstring& path,const SessionStamp& stamp,
                   SessionCacheValue& output){
        std::lock_guard<std::mutex> lock(mutex_);
        for(size_t i=0;i<entries_.size();++i){
            if(entries_[i].app==app && entries_[i].value.path==path && entries_[i].value.stamp==stamp){
                SessionCacheValue prepared;
                try {
                    if(ops_.beforeLookupOutput) ops_.beforeLookupOutput();
                    prepared=entries_[i].value;
                } catch(...) { return false; }
                entries_[i].lastUse=++clock_;
                PublishValue(output,prepared);
                return true;
            }
        }
        return false;
    }

    bool FindLatestPath(const std::string& app,const std::wstring& path,SessionCacheValue& output){
        std::lock_guard<std::mutex> lock(mutex_);
        size_t best=entries_.size();
        uint64_t newest=0;
        for(size_t i=0;i<entries_.size();++i){
            if(entries_[i].app==app && entries_[i].value.path==path &&
               (best==entries_.size() || entries_[i].inserted>newest)){
                best=i; newest=entries_[i].inserted;
            }
        }
        if(best==entries_.size()) return false;
        SessionCacheValue prepared;
        try {
            if(ops_.beforeLookupOutput) ops_.beforeLookupOutput();
            prepared=entries_[best].value;
        } catch(...) { return false; }
        entries_[best].lastUse=++clock_;
        PublishValue(output,prepared);
        return true;
    }

    bool Put(const std::string& app,const std::wstring& path,const SessionStamp& stamp,
             std::vector<WinFp>&& windows,uint64_t contentHash,uint64_t dataGeneration,
             SessionCacheValue& output){
        size_t retained=EstimateSessionPayloadBytes(windows);
        if(maxEntries_==0 || retained>budget_->Limit()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<bool> remove;
        try { remove.assign(entries_.size(),false); }
        catch(...) { return false; }
        size_t remaining=entries_.size(),reclaimable=0;
        auto mark=[&](size_t index){
            if(remove[index]) return;
            remove[index]=true;
            --remaining;
            if(entries_[index].value.windows.use_count()==1)
                reclaimable=SessionSaturatingAdd(reclaimable,entries_[index].value.retainedBytes);
        };
        bool duplicate=false;
        for(size_t i=0;i<entries_.size();++i)
            if(entries_[i].app==app && entries_[i].value.path==path && entries_[i].value.stamp==stamp){ mark(i); duplicate=true; }
        size_t used=budget_->Used();
        size_t immediatelyAvailable=used<=budget_->Limit()?budget_->Limit()-used:0;
        size_t needed=retained>immediatelyAvailable?retained-immediatelyAvailable:0;
        while(remaining>=maxEntries_ || reclaimable<needed){
            size_t oldest=entries_.size();
            for(size_t i=0;i<entries_.size();++i)
                if(!remove[i] && (oldest==entries_.size() || entries_[i].lastUse<entries_[oldest].lastUse)) oldest=i;
            if(oldest==entries_.size()) return false;
            mark(oldest);
        }
        std::shared_ptr<SessionPayloadHolder> holder;
        Entry preparedEntry;
        SessionCacheValue preparedOutput;
        try {
            PutStep(SessionCachePutStep::PayloadStorage);
            std::unique_ptr<std::vector<WinFp> > payloadStorage(
                new std::vector<WinFp>(std::move(windows)));
            PutStep(SessionCachePutStep::PayloadControl);
            holder=std::make_shared<SessionPayloadHolder>(std::move(payloadStorage),budget_,retained);
            std::shared_ptr<const std::vector<WinFp> > payload(holder,holder->windows.get());
            PutStep(SessionCachePutStep::PathCopy);
            preparedEntry.value.path=path;
            PutStep(SessionCachePutStep::StampCopy);
            preparedEntry.value.stamp=stamp;
            preparedEntry.value.contentHash=contentHash;
            preparedEntry.value.dataGeneration=dataGeneration;
            preparedEntry.value.retainedBytes=retained;
            preparedEntry.value.windows=payload;
            PutStep(SessionCachePutStep::KeyCopy);
            preparedEntry.app=app;
            PutStep(SessionCachePutStep::OutputCopy);
            preparedOutput=preparedEntry.value;
            PutStep(SessionCachePutStep::ContainerReserve);
            entries_.reserve(entries_.size()+1);
            if(duplicate) PutStep(SessionCachePutStep::DuplicateReplacement);
            PutStep(SessionCachePutStep::ContainerInsert);
            PutStep(SessionCachePutStep::OrderPublish);
        } catch(...) { return false; }

        // All potentially-throwing work is complete. Because Put is serialized
        // by mutex_, and only cache-owned payloads were counted reclaimable,
        // erasing the planned victims makes this preflighted Acquire infallible.
        for(size_t i=entries_.size();i>0;--i)
            if(remove[i-1]) entries_.erase(entries_.begin()+(i-1));
        if(!budget_->Acquire(retained)) std::terminate();
        holder->accounted.store(true,std::memory_order_release);
        preparedEntry.lastUse=++clock_;
        preparedEntry.inserted=clock_;
        entries_.push_back(std::move(preparedEntry));
        output.path.swap(preparedOutput.path);
        output.stamp=preparedOutput.stamp;
        output.contentHash=preparedOutput.contentHash;
        output.dataGeneration=preparedOutput.dataGeneration;
        output.retainedBytes=preparedOutput.retainedBytes;
        output.windows.swap(preparedOutput.windows);
        return true;
    }

    size_t EntryCount() const { std::lock_guard<std::mutex> lock(mutex_); return entries_.size(); }
    size_t RetainedBytes() const { return budget_->Used(); }
    void Clear(){ std::lock_guard<std::mutex> lock(mutex_); entries_.clear(); }

private:
    struct Entry {
        std::string app;
        SessionCacheValue value;
        uint64_t lastUse=0;
        uint64_t inserted=0;
    };
    static_assert(std::is_nothrow_move_constructible<Entry>::value,
                  "cache commit requires nothrow Entry move construction");
    static_assert(std::is_nothrow_move_assignable<Entry>::value,
                  "cache eviction requires nothrow Entry move assignment");
    static void PublishValue(SessionCacheValue& output,SessionCacheValue& prepared){
        output.path.swap(prepared.path);
        output.stamp=prepared.stamp;
        output.contentHash=prepared.contentHash;
        output.dataGeneration=prepared.dataGeneration;
        output.retainedBytes=prepared.retainedBytes;
        output.windows.swap(prepared.windows);
    }
    void PutStep(SessionCachePutStep step){ if(ops_.beforePutStep) ops_.beforePutStep(step); }
    size_t maxEntries_;
    std::shared_ptr<SessionRetainedBudget> budget_;
    SessionCacheOps ops_;
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    uint64_t clock_=0;
};

inline bool SessionProfilesEqual(const AppProfile& left,const AppProfile& right){
    return left.id==right.id && left.classNames==right.classNames && left.exeName==right.exeName &&
           left.titleSuffixes==right.titleSuffixes && left.session==right.session &&
           left.userDataDir==right.userDataDir;
}

inline std::wstring ResolveFirefoxProfileDirectoryFromIni(const std::wstring& base,
                                                           const std::string& ini){
    try {
    typedef std::pair<std::string,std::map<std::string,std::string> > Section;
    std::vector<Section> sections;
    size_t position=0;
    while(position<ini.size()){
        size_t newline=ini.find('\n',position);
        size_t stop=newline==std::string::npos?ini.size():newline;
        std::string line=ini.substr(position,stop-position);
        position=newline==std::string::npos?ini.size():newline+1;
        while(!line.empty()&&(line.back()=='\r'||line.back()==' ')) line.pop_back();
        size_t first=0; while(first<line.size()&&line[first]==' ') ++first;
        if(first) line.erase(0,first);
        if(line.empty()||line[0]==';'||line[0]=='#') continue;
        if(line.size()>=2&&line[0]=='['&&line.back()==']'){
            sections.push_back(Section(line.substr(1,line.size()-2),std::map<std::string,std::string>()));
            continue;
        }
        size_t equals=line.find('=');
        if(equals!=std::string::npos&&!sections.empty()) sections.back().second[line.substr(0,equals)]=line.substr(equals+1);
    }
    auto resolve=[&](const std::string& input,bool relative){
        std::string normalized=input;
        for(size_t i=0;i<normalized.size();++i) if(normalized[i]=='/') normalized[i]='\\';
        std::wstring path=U82W(normalized);
        return relative?base+L"\\"+path:path;
    };
    for(size_t i=0;i<sections.size();++i) if(sections[i].first.find("Install")==0){
        std::map<std::string,std::string>::const_iterator found=sections[i].second.find("Default");
        if(found!=sections[i].second.end()&&!found->second.empty()) return resolve(found->second,true);
    }
    for(size_t i=0;i<sections.size();++i) if(sections[i].first.find("Profile")==0){
        const std::map<std::string,std::string>& values=sections[i].second;
        std::map<std::string,std::string>::const_iterator isDefault=values.find("Default"),path=values.find("Path");
        if(isDefault!=values.end()&&isDefault->second=="1"&&path!=values.end()){
            std::map<std::string,std::string>::const_iterator relative=values.find("IsRelative");
            return resolve(path->second,relative==values.end()||relative->second=="1");
        }
    }
    for(size_t i=0;i<sections.size();++i) if(sections[i].first.find("Profile")==0){
        const std::map<std::string,std::string>& values=sections[i].second;
        std::map<std::string,std::string>::const_iterator path=values.find("Path");
        if(path!=values.end()&&path->second.find(".default-release")!=std::string::npos){
            std::map<std::string,std::string>::const_iterator relative=values.find("IsRelative");
            return resolve(path->second,relative==values.end()||relative->second=="1");
        }
    }
    return std::wstring();
    } catch(const std::bad_alloc&) { return std::wstring(); }
      catch(const std::length_error&) { return std::wstring(); }
}

inline std::wstring FindFirefoxProfileDirectoryForSession(){
    wchar_t appData[MAX_PATH]={0};
    if(!GetEnvironmentVariableW(L"APPDATA",appData,MAX_PATH)) return std::wstring();
    std::wstring base=std::wstring(appData)+L"\\Mozilla\\Firefox";
    FileReadResult read=ReadFileBytesBounded(base+L"\\profiles.ini",4ULL*1024ULL*1024ULL);
    if(read.status!=FileReadStatus::Ok) return std::wstring();
    return ResolveFirefoxProfileDirectoryFromIni(base,read.bytes);
}

inline std::wstring ResolveBrowserSessionPath(const AppProfile& profile){
    if(profile.session==AppProfile::FIREFOX){
        std::wstring directory=FindFirefoxProfileDirectoryForSession();
        if(directory.empty()) return std::wstring();
        const wchar_t* candidates[]={L"\\sessionstore-backups\\recovery.jsonlz4",L"\\sessionstore-backups\\recovery.baklz4",L"\\sessionstore.jsonlz4",L"\\sessionstore-backups\\previous.jsonlz4"};
        for(size_t i=0;i<sizeof(candidates)/sizeof(candidates[0]);++i){
            std::wstring path=directory+candidates[i]; SessionStamp stamp;
            if(GetSessionStamp(path,stamp)) return path;
        }
        return std::wstring();
    }
    if(profile.session!=AppProfile::CHROMIUM || profile.userDataDir.empty()) return std::wstring();
    std::wstring pattern=profile.userDataDir+L"\\Default\\Sessions\\Session_*";
    WIN32_FIND_DATAW found{}; HANDLE search=FindFirstFileW(pattern.c_str(),&found);
    if(search==INVALID_HANDLE_VALUE) return std::wstring();
    std::wstring bestName; ULARGE_INTEGER newest{};
    try {
        do {
            if(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue;
            ULARGE_INTEGER size{}; size.LowPart=found.nFileSizeLow; size.HighPart=found.nFileSizeHigh;
            if(size.QuadPart==0||size.QuadPart>MAX_BROWSER_SESSION_BYTES) continue;
            ULARGE_INTEGER modified{}; modified.LowPart=found.ftLastWriteTime.dwLowDateTime; modified.HighPart=found.ftLastWriteTime.dwHighDateTime;
            std::wstring candidate=found.cFileName;
            if(bestName.empty()||modified.QuadPart>newest.QuadPart||
               (modified.QuadPart==newest.QuadPart&&candidate>bestName)){
                newest=modified; bestName=std::move(candidate);
            }
        } while(FindNextFileW(search,&found));
    } catch(const std::bad_alloc&) { FindClose(search); return std::wstring(); }
      catch(const std::length_error&) { FindClose(search); return std::wstring(); }
    FindClose(search);
    return bestName.empty()?std::wstring():profile.userDataDir+L"\\Default\\Sessions\\"+bestName;
}

inline bool ParseBrowserSessionData(const AppProfile& profile,const std::string& bytes,
                                    std::vector<WinFp>& output){
    output.clear();
    if(profile.session==AppProfile::CHROMIUM) return ParseChromiumSNSS(bytes,output);
    if(profile.session==AppProfile::FIREFOX){
        std::string json;
        if(!MozLz4Decompress(bytes,MAX_BROWSER_SESSION_BYTES,json)) return false;
        return ParseFirefoxSessionJson(json,output);
    }
    return false;
}

struct SessionWorkerOps {
    std::function<std::wstring(const AppProfile&)> resolvePath;
    std::function<bool(const std::wstring&,SessionStamp&)> getStamp;
    std::function<SessionFileReadResult(const std::wstring&)> readFile;
    std::function<bool(const AppProfile&,const std::string&,std::vector<WinFp>&)> parse;
    std::function<bool()> beforePost;
    std::function<bool()> beforeJoinWait;
    std::function<void(SessionWorkerStep)> beforeWorkerStep;
    std::function<std::unique_ptr<SessionResult>()> makeResult;
    std::function<bool(HWND,UINT,WPARAM,LPARAM)> postMessage;
};

inline SessionWorkerOps DefaultSessionWorkerOps(){
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile& profile){ return ResolveBrowserSessionPath(profile); };
    ops.getStamp=[](const std::wstring& path,SessionStamp& stamp){ return GetSessionStamp(path,stamp); };
    ops.readFile=[](const std::wstring& path){
        return ReadBrowserSessionFileBounded(path,MAX_BROWSER_SESSION_BYTES);
    };
    ops.parse=[](const AppProfile& profile,const std::string& bytes,std::vector<WinFp>& output){ return ParseBrowserSessionData(profile,bytes,output); };
    ops.makeResult=[](){ return std::unique_ptr<SessionResult>(new SessionResult()); };
    ops.postMessage=[](HWND window,UINT message,WPARAM wp,LPARAM lp){ return PostMessageW(window,message,wp,lp)!=FALSE; };
    return ops;
}

inline void FillMissingSessionWorkerOps(SessionWorkerOps& ops){
    SessionWorkerOps defaults=DefaultSessionWorkerOps();
    if(!ops.resolvePath) ops.resolvePath=defaults.resolvePath;
    if(!ops.getStamp) ops.getStamp=defaults.getStamp;
    if(!ops.readFile) ops.readFile=defaults.readFile;
    if(!ops.parse) ops.parse=defaults.parse;
    if(!ops.makeResult) ops.makeResult=defaults.makeResult;
    if(!ops.postMessage) ops.postMessage=defaults.postMessage;
}

inline bool PostSessionResultOwned(const SessionWorkerOps& ops,HWND window,
                                   std::unique_ptr<SessionResult> result){
    if(!result) return false;
    SessionResult* raw=result.release();
    bool posted=false;
    try { posted=ops.postMessage&&ops.postMessage(window,WM_SESSION_RESULT,0,(LPARAM)raw); }
    catch(...) { posted=false; }
    if(!posted) delete raw;
    return posted;
}

inline size_t DrainPostedSessionResults(HWND window){
    if(!window) return 0;
    MSG pending{};
    size_t drained=0;
    while(PeekMessageW(&pending,window,WM_SESSION_RESULT,WM_SESSION_RESULT,PM_REMOVE)){
        std::unique_ptr<SessionResult> owned((SessionResult*)pending.lParam);
        if(drained!=(std::numeric_limits<size_t>::max)()) ++drained;
    }
    return drained;
}

inline uint64_t HashSessionWindows(const std::vector<WinFp>& windows){
    uint64_t hash=1469598103934665603ULL;
    auto bytes=[&](const void* data,size_t size){
        const unsigned char* p=(const unsigned char*)data;
        for(size_t i=0;i<size;++i){ hash^=p[i]; hash*=1099511628211ULL; }
    };
    auto string=[&](const std::string& value){ bytes(value.data(),value.size()); unsigned char zero=0; bytes(&zero,1); };
    for(size_t i=0;i<windows.size();++i){
        string(windows[i].activeTitle); string(windows[i].activeDomain); string(windows[i].tabsBlob);
        bytes(&windows[i].tabCount,sizeof(windows[i].tabCount));
        for(std::map<std::string,int>::const_iterator it=windows[i].counts.begin();it!=windows[i].counts.end();++it){ string(it->first); bytes(&it->second,sizeof(it->second)); }
    }
    return hash;
}

inline uint64_t NextSessionDataGeneration(uint64_t current){
    return current==(std::numeric_limits<uint64_t>::max)()?current:current+1;
}

inline int SessionPurposePriority(SessionPurpose purpose){
    if(purpose==SessionPurpose::ManualSave||purpose==SessionPurpose::ManualRestore) return 4;
    if(purpose==SessionPurpose::AutoReconcile||purpose==SessionPurpose::HeartbeatSave) return 3;
    if(purpose==SessionPurpose::Search) return 2;
    return 1;
}

inline bool SessionRequestProfileSupported(const SessionRequest& request){
    if(request.app!=request.profile.id) return false;
    if(request.app=="firefox") return request.profile.session==AppProfile::FIREFOX;
    if(request.app=="chrome" || request.app=="msedge")
        return request.profile.session==AppProfile::CHROMIUM;
    return false;
}

class SessionWorker {
public:
    explicit SessionWorker(HWND window,const SessionWorkerOps& supplied=SessionWorkerOps(),
                           size_t maxCacheEntries=MAX_SESSION_CACHE_ENTRIES,
                           size_t maxCacheBytes=MAX_SESSION_CACHE_BYTES)
        :window_(window),ops_(supplied),cache_(maxCacheEntries,maxCacheBytes){
        FillMissingSessionWorkerOps(ops_);
        generations_.emplace("firefox",GenerationState{});
        generations_.emplace("chrome",GenerationState{});
        generations_.emplace("msedge",GenerationState{});
        thread_=std::thread(&SessionWorker::Run,this);
    }
    ~SessionWorker(){ Stop(); }
    SessionWorker(const SessionWorker&)=delete;
    SessionWorker& operator=(const SessionWorker&)=delete;

    bool Request(const SessionRequest& request){
        if(!SessionRequestProfileSupported(request)) return false;
        Queued incoming;
        try {
            WorkerStep(SessionWorkerStep::RequestPrepare);
            incoming.request=request;
            incoming.activeApp=request.app;
        } catch(...) { return false; }
        std::unique_ptr<SessionResult> superseded;
        bool queued=false;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if(stopping_) return false;
            std::map<std::string,Queued>::iterator found=pending_.find(request.app);
            if(found!=pending_.end()){
                if(SessionPurposePriority(request.purpose)<SessionPurposePriority(found->second.request.purpose))
                    superseded=MakeSuperseded(incoming.request);
                else {
                    superseded=MakeSuperseded(found->second.request);
                    incoming.order=NextQueueOrder();
                    found->second=std::move(incoming);
                    queueClock_=found->second.order;
                    queued=true;
                }
            } else {
                incoming.order=NextQueueOrder();
                std::pair<std::map<std::string,Queued>::iterator,bool> inserted=
                    pending_.emplace(incoming.request.app,std::move(incoming));
                if(!inserted.second) return false;
                queueClock_=inserted.first->second.order;
                queued=true;
            }
        } catch(...) { return false; }
        if(superseded) PostIfRunning(std::move(superseded));
        if(queued) changed_.notify_one();
        return true;
    }

    bool Stop(){
        bool notify=false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if(stopped_) return true;
            if(!stopping_){
                stopping_=true;
                window_=nullptr;
                pending_.clear();
                notify=true;
            }
            if(workerThreadId_==std::this_thread::get_id() || PostingWorkerSlot()==this){
                lock.unlock();
                if(notify) changed_.notify_all();
                return false; // a later external owner performs the required join
            }
            if(joinInProgress_){
                lock.unlock();
                bool shouldWait=true;
                try { shouldWait=!ops_.beforeJoinWait || ops_.beforeJoinWait(); }
                catch(...) { shouldWait=true; }
                lock.lock();
                if(!shouldWait) return false;
                changed_.wait(lock,[&]{ return stopped_; });
                return true;
            }
            joinInProgress_=true;
        }
        if(notify) changed_.notify_all();
        if(thread_.joinable()) thread_.join();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock,[&]{ return postsInFlight_==0; });
        }
        cache_.Clear();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_=true; joinInProgress_=false;
        }
        changed_.notify_all();
        return true;
    }

    size_t PendingCount() const { std::lock_guard<std::mutex> lock(mutex_); return pending_.size(); }
    size_t ActiveCount() const { std::lock_guard<std::mutex> lock(mutex_); return active_?1:0; }
    size_t OutstandingForApp(const std::string& app) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return (active_&&activeApp_==app?1:0)+(pending_.count(app)?1:0);
    }
    size_t CacheEntryCount() const { return cache_.EntryCount(); }
    size_t RetainedBytes() const { return cache_.RetainedBytes(); }

private:
    struct Queued { SessionRequest request; std::string activeApp; uint64_t order=0; };
    static_assert(std::is_nothrow_move_constructible<Queued>::value,
                  "queue dequeue requires nothrow move construction");
    static_assert(std::is_nothrow_move_assignable<Queued>::value,
                  "queue replacement requires nothrow move assignment");
    struct GenerationState {
        std::wstring path; SessionStamp stamp; uint64_t hash=0,generation=0; bool known=false;
    };
    static SessionWorker*& PostingWorkerSlot(){
        static thread_local SessionWorker* current=nullptr;
        return current;
    }
    struct PostingScope {
        SessionWorker* previous;
        explicit PostingScope(SessionWorker* current):previous(PostingWorkerSlot()){
            PostingWorkerSlot()=current;
        }
        ~PostingScope(){ PostingWorkerSlot()=previous; }
    };
    void FinishPost(){
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(postsInFlight_>0) --postsInFlight_;
        }
        changed_.notify_all();
    }

    uint64_t NextQueueOrder() const {
        return queueClock_==(std::numeric_limits<uint64_t>::max)()?queueClock_:queueClock_+1;
    }
    void WorkerStep(SessionWorkerStep step){
        if(ops_.beforeWorkerStep) ops_.beforeWorkerStep(step);
    }
    static void SwapSessionResult(SessionResult& left,SessionResult& right) noexcept {
        using std::swap;
        swap(left.requestId,right.requestId);
        left.app.swap(right.app);
        left.path.swap(right.path);
        swap(left.purpose,right.purpose);
        swap(left.identityGeneration,right.identityGeneration);
        swap(left.sourceStamp,right.sourceStamp);
        swap(left.sourceStampKnown,right.sourceStampKnown);
        swap(left.dataStamp,right.dataStamp);
        swap(left.dataGeneration,right.dataGeneration);
        swap(left.status,right.status);
        left.windows.swap(right.windows);
    }
    static void PrepareUnavailable(const SessionRequest& request,SessionResult& output){
        SessionResult prepared;
        prepared.requestId=request.requestId;
        prepared.app=request.app;
        prepared.purpose=request.purpose;
        prepared.identityGeneration=request.identityGeneration;
        prepared.status=SessionDataStatus::Unavailable;
        SwapSessionResult(output,prepared);
    }
    std::unique_ptr<SessionResult> MakeResult(){
        return ops_.makeResult?ops_.makeResult():std::unique_ptr<SessionResult>();
    }
    static std::unique_ptr<SessionResult> MakeSuperseded(const SessionRequest& request){
        std::unique_ptr<SessionResult> result(new SessionResult());
        PrepareUnavailable(request,*result);
        result->requestId=request.requestId; result->app=request.app; result->purpose=request.purpose;
        result->identityGeneration=request.identityGeneration; result->status=SessionDataStatus::Superseded;
        return result;
    }
    void PostIfRunning(std::unique_ptr<SessionResult> result) noexcept {
        if(!result) return;
        HWND target=nullptr;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if(stopping_ || !window_) return;
            target=window_;
            ++postsInFlight_;
        } catch(...) { return; }
        struct PostCompletion {
            SessionWorker* worker;
            explicit PostCompletion(SessionWorker* value):worker(value){}
            ~PostCompletion(){ worker->FinishPost(); }
        } completion(this);
        try {
            PostingScope posting(this);
            bool proceed=!ops_.beforePost || ops_.beforePost();
            if(proceed) PostSessionResultOwned(ops_,target,std::move(result));
        } catch(...) {}
    }
    bool Choose(Queued& output){
        if(pending_.empty()) return false;
        std::map<std::string,Queued>::iterator best=pending_.begin();
        for(std::map<std::string,Queued>::iterator it=std::next(pending_.begin());it!=pending_.end();++it){
            int candidate=SessionPurposePriority(it->second.request.purpose),current=SessionPurposePriority(best->second.request.purpose);
            if(candidate>current || (candidate==current&&it->second.order<best->second.order)) best=it;
        }
        output=std::move(best->second); pending_.erase(best); return true;
    }
    void ClearActive() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            active_=false;
            activeApp_.clear();
        } catch(...) {}
        changed_.notify_all();
    }
    void ProcessQueued(Queued& queued) noexcept {
        std::unique_ptr<SessionResult> result;
        bool resultIdentified=false;
        try {
            result=MakeResult();
            if(!result) throw std::bad_alloc();
            WorkerStep(SessionWorkerStep::ResultPrepare);
            PrepareUnavailable(queued.request,*result);
            resultIdentified=true;
            WorkerStep(SessionWorkerStep::AfterChoose);
            WorkerStep(SessionWorkerStep::ActiveCopy);
            SessionResult processed;
            PrepareUnavailable(queued.request,processed);
            Process(queued.request,processed);
            SwapSessionResult(*result,processed);
        } catch(...) { if(!resultIdentified) result.reset(); }
        ClearActive();
        PostIfRunning(std::move(result));
    }
    void RunLoop(){
        {
            std::lock_guard<std::mutex> lock(mutex_);
            workerThreadId_=std::this_thread::get_id();
        }
        for(;;){
            Queued queued;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock,[&]{ return stopping_||!pending_.empty(); });
                if(stopping_) break;
                if(!Choose(queued)) continue;
                active_=true; activeApp_.swap(queued.activeApp);
            }
            ProcessQueued(queued);
        }
    }
    void Run() noexcept {
        try { RunLoop(); } catch(...) { ClearActive(); }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            workerThreadId_=std::thread::id();
        }
        changed_.notify_all();
    }
    void ObserveCurrent(SessionResult& result,const AppProfile& profile){
        std::wstring current;
        try { current=ops_.resolvePath(profile); } catch(...) { current.clear(); }
        result.path=current;
        SessionStamp stamp;
        result.sourceStamp=SessionStamp{};
        try { result.sourceStampKnown=!current.empty()&&ops_.getStamp(current,stamp); } catch(...) { result.sourceStampKnown=false; }
        if(result.sourceStampKnown) result.sourceStamp=stamp;
    }
    bool GenerationIdentityMatches(const GenerationState& state,const std::wstring& path,
                                   const SessionStamp& stamp,uint64_t hash) const {
        return state.known && state.path==path && state.stamp==stamp && state.hash==hash;
    }
    void PrepareGeneration(const std::string& app,const std::wstring& path,
                           const SessionStamp& stamp,uint64_t hash,GenerationState& prepared){
        WorkerStep(SessionWorkerStep::GenerationPrepare);
        std::map<std::string,GenerationState>::const_iterator found=generations_.find(app);
        if(found==generations_.end()) throw std::runtime_error("unsupported session generation app");
        GenerationState next;
        next.path=path;
        next.stamp=stamp;
        next.hash=hash;
        next.generation=GenerationIdentityMatches(found->second,path,stamp,hash)?
            found->second.generation:NextSessionDataGeneration(found->second.generation);
        next.known=true;
        WorkerStep(SessionWorkerStep::GenerationPublish);
        prepared.path.swap(next.path);
        prepared.stamp=next.stamp;
        prepared.hash=next.hash;
        prepared.generation=next.generation;
        prepared.known=true;
    }
    void PublishGeneration(const std::string& app,GenerationState& prepared) noexcept {
        std::map<std::string,GenerationState>::iterator found=generations_.find(app);
        if(found==generations_.end()) return;
        found->second.path.swap(prepared.path);
        found->second.stamp=prepared.stamp;
        found->second.hash=prepared.hash;
        found->second.generation=prepared.generation;
        found->second.known=prepared.known;
    }
    uint64_t ObserveCachedGeneration(const std::string& app,const SessionCacheValue& cached){
        GenerationState prepared;
        PrepareGeneration(app,cached.path,cached.stamp,cached.contentHash,prepared);
        uint64_t generation=prepared.generation;
        PublishGeneration(app,prepared);
        return generation;
    }
    void ApplyFallback(SessionResult& result,const std::wstring& attemptedPath){
        SessionCacheValue stale;
        if(!attemptedPath.empty()&&cache_.FindLatestPath(result.app,attemptedPath,stale)){
            result.status=SessionDataStatus::CachedStale; result.windows=stale.windows;
            result.dataStamp=stale.stamp; result.dataGeneration=ObserveCachedGeneration(result.app,stale);
        } else result.status=SessionDataStatus::Unavailable;
    }
    void Process(const SessionRequest& request,SessionResult& result){
        std::wstring before;
        try { before=ops_.resolvePath(request.profile); } catch(...) { before.clear(); }
        result.path=before;
        SessionStamp stampBefore;
        bool beforeKnown=false;
        try { beforeKnown=!before.empty()&&ops_.getStamp(before,stampBefore); } catch(...) { beforeKnown=false; }
        if(beforeKnown){ result.sourceStampKnown=true; result.sourceStamp=stampBefore; }
        SessionCacheValue cached;
        WorkerStep(SessionWorkerStep::CacheLookup);
        if(beforeKnown&&cache_.FindExact(request.app,before,stampBefore,cached)){
            result.status=SessionDataStatus::Fresh; result.windows=cached.windows;
            result.dataStamp=cached.stamp; result.dataGeneration=ObserveCachedGeneration(request.app,cached);
            return;
        }
        if(!beforeKnown){ ObserveCurrent(result,request.profile); ApplyFallback(result,before); return; }
        SessionFileReadResult read;
        try { read=ops_.readFile(before); } catch(...) { read.status=FileReadStatus::Unavailable; }
        if(read.status!=FileReadStatus::Ok || !read.readStampKnown || read.readStamp!=stampBefore){
            ObserveCurrent(result,request.profile); ApplyFallback(result,before); return;
        }
        std::vector<WinFp> parsed;
        bool parsedOk=false;
        try { parsedOk=ops_.parse(request.profile,read.bytes,parsed); } catch(...) { parsed.clear(); parsedOk=false; }
        std::wstring after;
        try { after=ops_.resolvePath(request.profile); } catch(...) { after.clear(); }
        result.path=after;
        SessionStamp stampAfter;
        result.sourceStamp=SessionStamp{};
        try { result.sourceStampKnown=!after.empty()&&ops_.getStamp(after,stampAfter); } catch(...) { result.sourceStampKnown=false; }
        if(result.sourceStampKnown) result.sourceStamp=stampAfter;
        if(parsedOk&&result.sourceStampKnown&&before==after&&
           stampBefore==read.readStamp&&read.readStamp==stampAfter){
            uint64_t hash=HashSessionWindows(parsed);
            GenerationState generation;
            PrepareGeneration(request.app,after,read.readStamp,hash,generation);
            SessionCacheValue inserted;
            if(cache_.Put(request.app,after,read.readStamp,std::move(parsed),hash,generation.generation,inserted)){
                PublishGeneration(request.app,generation);
                result.status=SessionDataStatus::Fresh; result.windows=inserted.windows;
                result.dataStamp=inserted.stamp; result.dataGeneration=inserted.dataGeneration;
                return;
            }
        }
        ApplyFallback(result,before);
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    HWND window_=nullptr;
    SessionWorkerOps ops_;
    SessionCache cache_;
    std::thread thread_;
    bool stopping_=false,stopped_=false,joinInProgress_=false,active_=false;
    size_t postsInFlight_=0;
    std::thread::id workerThreadId_;
    std::string activeApp_;
    std::map<std::string,Queued> pending_;
    uint64_t queueClock_=0;
    std::map<std::string,GenerationState> generations_;
};

class SessionCoordinator {
public:
    typedef std::function<bool(const SessionRequest&)> Submit;
    explicit SessionCoordinator(const Submit& submit,
                                const SessionCoordinatorOps& ops=SessionCoordinatorOps())
        :submit_(submit),ops_(ops){}

    uint64_t RequestSessionData(const AppProfile& profile,uint64_t identityGeneration,
                                SessionPurpose purpose){
        SessionRequest request;
        Pending expected;
        uint64_t requestId=0;
        try {
            Step(SessionCoordinatorStep::RequestPrepare);
            if(nextRequestId_==(std::numeric_limits<uint64_t>::max)()) return 0;
            requestId=nextRequestId_+1;
            request.requestId=requestId;
            request.app=profile.id;
            request.profile=profile;
            request.purpose=purpose;
            request.identityGeneration=identityGeneration;
            expected.app=request.app;
            expected.profile=profile;
            expected.purpose=purpose;
            expected.identityGeneration=identityGeneration;
        } catch(...) { return 0; }

        bool latestInserted=false,latestChanged=false;
        uint64_t previousLatest=0;
        try {
            Step(SessionCoordinatorStep::PendingInsert);
            std::pair<std::map<uint64_t,Pending>::iterator,bool> pendingInserted=
                pending_.emplace(requestId,std::move(expected));
            if(!pendingInserted.second) return 0;
            try {
                Step(SessionCoordinatorStep::LatestRequestInsert);
                std::map<std::string,uint64_t>::iterator latest=latestRequest_.find(request.app);
                if(latest==latestRequest_.end()){
                    std::pair<std::map<std::string,uint64_t>::iterator,bool> inserted=
                        latestRequest_.emplace(request.app,requestId);
                    if(!inserted.second) throw std::runtime_error("session latest insertion failed");
                    latestInserted=true;
                } else {
                    previousLatest=latest->second;
                    latest->second=requestId;
                    latestChanged=true;
                }
            } catch(...) {
                pending_.erase(requestId);
                throw;
            }
        } catch(...) { return 0; }

        bool submitted=false;
        try { submitted=submit_&&submit_(request); } catch(...) { submitted=false; }
        if(!submitted){
            pending_.erase(requestId);
            if(latestInserted) latestRequest_.erase(request.app);
            else if(latestChanged){
                std::map<std::string,uint64_t>::iterator latest=latestRequest_.find(request.app);
                if(latest!=latestRequest_.end()) latest->second=previousLatest;
            }
            return 0;
        }
        nextRequestId_=requestId;
        return requestId;
    }

    bool AcceptSessionResult(std::unique_ptr<SessionResult> result,
                             const AppProfile& currentProfile,uint64_t currentGeneration){
        if(!result) return false;
        std::map<uint64_t,Pending>::iterator found=pending_.find(result->requestId);
        if(found==pending_.end()) return false;
        Pending expected;
        try {
            Step(SessionCoordinatorStep::AcceptPrepare);
            expected=found->second;
        } catch(...) { return false; }
        if(result->app!=expected.app || result->purpose!=expected.purpose ||
           result->identityGeneration!=expected.identityGeneration) return false;
        if(result->status==SessionDataStatus::Superseded){
            pending_.erase(found);
            RefreshLatest(expected.app,result->requestId);
            return false;
        }
        std::map<std::string,uint64_t>::const_iterator latest=latestRequest_.find(expected.app);
        bool isLatest=latest!=latestRequest_.end()&&latest->second==result->requestId;
        if(!isLatest || currentGeneration!=expected.identityGeneration ||
           !SessionProfilesEqual(currentProfile,expected.profile)){
            pending_.erase(found);
            return false;
        }
        std::map<std::string,std::unique_ptr<SessionResult> >::iterator destination=
            latestResults_.find(expected.app);
        if(destination==latestResults_.end()){
            try {
                Step(SessionCoordinatorStep::LatestResultInsert);
                std::pair<std::map<std::string,std::unique_ptr<SessionResult> >::iterator,bool> inserted=
                    latestResults_.emplace(expected.app,std::unique_ptr<SessionResult>());
                if(!inserted.second) return false;
                destination=inserted.first;
            } catch(...) { return false; }
        }
        pending_.erase(found);
        destination->second.swap(result);
        return true;
    }

    const SessionResult* Latest(const std::string& app) const {
        std::map<std::string,std::unique_ptr<SessionResult> >::const_iterator found=latestResults_.find(app);
        return found==latestResults_.end()?nullptr:found->second.get();
    }
    size_t PendingCount() const { return pending_.size(); }

private:
    struct Pending {
        std::string app;
        AppProfile profile;
        SessionPurpose purpose=SessionPurpose::MetadataProbe;
        uint64_t identityGeneration=0;
        Pending(){ profile.session=AppProfile::NONE; }
    };
    static_assert(std::is_nothrow_move_constructible<Pending>::value,
                  "coordinator commit requires nothrow Pending move construction");
    void Step(SessionCoordinatorStep step){ if(ops_.beforeStep) ops_.beforeStep(step); }
    void RefreshLatest(const std::string& app,uint64_t removed){
        std::map<std::string,uint64_t>::iterator latest=latestRequest_.find(app);
        if(latest==latestRequest_.end() || latest->second!=removed) return;
        uint64_t replacement=0;
        for(std::map<uint64_t,Pending>::const_iterator it=pending_.begin();it!=pending_.end();++it)
            if(it->second.app==app && it->first>replacement) replacement=it->first;
        if(replacement) latest->second=replacement;
        else latestRequest_.erase(latest);
    }
    Submit submit_;
    SessionCoordinatorOps ops_;
    uint64_t nextRequestId_=0;
    std::map<uint64_t,Pending> pending_;
    std::map<std::string,uint64_t> latestRequest_;
    std::map<std::string,std::unique_ptr<SessionResult> > latestResults_;
};

inline bool AcceptPostedSessionResult(SessionCoordinator& coordinator,LPARAM value,
                                      const AppProfile& currentProfile,uint64_t currentGeneration){
    std::unique_ptr<SessionResult> owned((SessionResult*)value);
    return coordinator.AcceptSessionResult(std::move(owned),currentProfile,currentGeneration);
}

class SessionUnavailableDeferBudget {
public:
    bool ShouldDefer(const SessionResult& result){
        if(result.status!=SessionDataStatus::Unavailable){
            if(result.status==SessionDataStatus::Fresh) entries_.erase(result.app);
            return false;
        }
        Key current;
        current.path=result.path;
        current.sourceStampKnown=result.sourceStampKnown;
        current.sourceStamp=result.sourceStamp;
        std::map<std::string,Entry>::iterator found=entries_.find(result.app);
        if(found==entries_.end() || !(found->second.key==current)){
            Entry entry; entry.key=current; entry.used=true;
            entries_[result.app]=entry;
            return true;
        }
        if(found->second.used) return false;
        found->second.used=true;
        return true;
    }
private:
    struct Key {
        std::wstring path;
        bool sourceStampKnown=false;
        SessionStamp sourceStamp;
        bool operator==(const Key& other) const {
            return path==other.path && sourceStampKnown==other.sourceStampKnown &&
                   (!sourceStampKnown || sourceStamp==other.sourceStamp);
        }
    };
    struct Entry { Key key; bool used=false; };
    std::map<std::string,Entry> entries_;
};

inline const std::string& SelectSerializedSessionRecords(SessionDataStatus status,
        const std::string& currentBytes,const std::string& proposedBytes){
    return status==SessionDataStatus::Unavailable || status==SessionDataStatus::Superseded ?
        currentBytes : proposedBytes;
}
