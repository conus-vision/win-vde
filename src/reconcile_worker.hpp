#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "lifecycle.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>

#pragma comment(lib,"user32.lib")

static const UINT WM_RECONCILE_RESULT=WM_APP+13;
static const size_t MAX_RECONCILE_SESSION_WINDOWS=10000;
static const size_t MAX_RECONCILE_TITLE_SUFFIXES=64;
static const size_t MAX_RECONCILE_TITLE_SUFFIX_CHARS=64ULL*1024ULL;
static const size_t MAX_RECONCILE_COPIED_TEXT_BYTES=32ULL*1024ULL*1024ULL;

enum class ReconcileWorkMode { Plan, PrepareLiveOnly };

struct ReconcileRequest {
    uint64_t operationId=0;
    std::string app;
    uint64_t identityGeneration=0;
    uint64_t contentGeneration=0;
    uint64_t sessionRequestId=0;
    uint64_t sessionDataGeneration=0;
    UnixSeconds nowUtc=0;
    ReconcileFreshness freshness=ReconcileFreshness::Fresh;
    ReconcileWorkMode workMode=ReconcileWorkMode::Plan;
    bool buildLiveFromInputs=false;
    std::vector<LayoutWin> saved;
    std::vector<LayoutWin> live;
    std::set<std::string> reservedRecordIds;
    std::vector<FastWin> fastWindows;
    std::shared_ptr<const std::vector<WinFp> > sessionWindows;
    std::vector<DeskRec> desktops;
    std::vector<std::wstring> titleSuffixes;
};

inline bool AddReconcileTextUnits(size_t units,size_t unitBytes,
                                  size_t maxBytes,size_t& totalBytes) noexcept {
    if(totalBytes>maxBytes || unitBytes==0 ||
       units>(maxBytes-totalBytes)/unitBytes) return false;
    totalBytes+=units*unitBytes;
    return true;
}

inline bool AddReconcileLayoutText(const LayoutWin& record,size_t maxBytes,
                                   size_t& totalBytes) noexcept {
    if(!AddReconcileTextUnits(record.recordId.size(),sizeof(char),maxBytes,totalBytes) ||
       !AddReconcileTextUnits(record.app.size(),sizeof(char),maxBytes,totalBytes) ||
       !AddReconcileTextUnits(record.activeTitle.size(),sizeof(char),maxBytes,totalBytes) ||
       !AddReconcileTextUnits(record.activeDomain.size(),sizeof(char),maxBytes,totalBytes))
        return false;
    for(const auto& count : record.counts)
        if(!AddReconcileTextUnits(
                count.first.size(),sizeof(char),maxBytes,totalBytes)) return false;
    return true;
}

inline bool ReconcileRequestTextWithinBudget(
        const ReconcileRequest& request,
        size_t maxBytes=MAX_RECONCILE_COPIED_TEXT_BYTES) noexcept {
    size_t totalBytes=0;
    if(!AddReconcileTextUnits(
            request.app.size(),sizeof(char),maxBytes,totalBytes)) return false;
    for(const LayoutWin& record : request.saved)
        if(!AddReconcileLayoutText(record,maxBytes,totalBytes)) return false;
    for(const LayoutWin& record : request.live)
        if(!AddReconcileLayoutText(record,maxBytes,totalBytes)) return false;
    for(const std::string& recordId : request.reservedRecordIds)
        if(!AddReconcileTextUnits(
                recordId.size(),sizeof(char),maxBytes,totalBytes)) return false;
    for(const FastWin& fast : request.fastWindows){
        if(!AddReconcileTextUnits(
                fast.app.size(),sizeof(char),maxBytes,totalBytes) ||
           !AddReconcileTextUnits(
                fast.title.size(),sizeof(wchar_t),maxBytes,totalBytes)) return false;
    }
    for(const DeskRec& desktop : request.desktops)
        if(!AddReconcileTextUnits(
                desktop.name.size(),sizeof(wchar_t),maxBytes,totalBytes)) return false;
    size_t suffixChars=0;
    for(const std::wstring& suffix : request.titleSuffixes){
        if(suffix.size()>MAX_RECONCILE_TITLE_SUFFIX_CHARS-suffixChars ||
           !AddReconcileTextUnits(
                suffix.size(),sizeof(wchar_t),maxBytes,totalBytes)) return false;
        suffixChars+=suffix.size();
    }
    return true;
}

struct PreparedReconcileLive {
    std::vector<LayoutWin> live;
    std::vector<int> sessionIndexByFast;
};

static const size_t MAX_CLI_PROFILE_BATCH=3;

struct PreparedCliProfileBatch {
    std::vector<LayoutWin> live;
    std::vector<FastWin> fastWindows;
};

inline bool CliCheckpointInputsStillCurrent(
        const std::map<std::string,AppFastSnapshot>& captured,
        const std::map<std::string,AppFastSnapshot>& current,
        const std::vector<DeskRec>& capturedDesktops,
        const std::vector<DeskRec>& currentDesktops) noexcept {
    if(captured.size()!=current.size() ||
       capturedDesktops.size()!=currentDesktops.size()) return false;
    for(size_t index=0;index<capturedDesktops.size();++index)
        if(capturedDesktops[index].index!=currentDesktops[index].index ||
           !GuidEq(capturedDesktops[index].guid,
                   currentDesktops[index].guid)) return false;
    for(const auto& entry : captured){
        auto found=current.find(entry.first);
        if(found==current.end()) return false;
        const AppFastSnapshot& before=entry.second;
        const AppFastSnapshot& after=found->second;
        if(before.identityGeneration==0 || before.generation==0 ||
           before.identityGeneration!=after.identityGeneration ||
           before.generation!=after.generation ||
           !FastSnapshotCanPersistAll(before) ||
           !FastSnapshotCanPersistAll(after) ||
           before.windows.size()!=after.windows.size()) return false;
        for(size_t window=0;window<before.windows.size();++window){
            const FastWin& left=before.windows[window];
            const FastWin& right=after.windows[window];
            if(!SameIdentity(IdentityOf(left),IdentityOf(right)) ||
               left.app!=right.app || left.title!=right.title ||
               !GuidEq(left.desktop,right.desktop)) return false;
        }
    }
    return true;
}

template<class Write>
inline bool PublishManualSnapshotIfCurrent(
        const std::map<std::string,AppFastSnapshot>& captured,
        const std::map<std::string,AppFastSnapshot>& current,
        const std::vector<DeskRec>& capturedDesktops,
        const std::vector<DeskRec>& currentDesktops,
        const std::string& checkedBytes,
        Write&& write) noexcept {
    if(checkedBytes.empty() ||
       !CliCheckpointInputsStillCurrent(
            captured,current,capturedDesktops,currentDesktops)) return false;
    try { return write(checkedBytes); }
    catch(...) { return false; }
}

struct CliStatusRow {
    FastWin window;
    int deskIndex=-1;
    int tabCount=-1;
    std::string activeTitle;
    bool fingerprintAvailable=false;
};

inline bool BuildCliStatusRows(
        const AppFastSnapshot& snapshot,
        const std::vector<DeskRec>& desktops,
        const PreparedCliProfileBatch* prepared,
        bool fingerprintsFresh,
        std::vector<CliStatusRow>& output){
    if(snapshot.windows.size()>MAX_LAYOUT_RECORDS ||
       desktops.size()>MAX_LAYOUT_RECORDS) return false;
    if(fingerprintsFresh && (!prepared ||
       prepared->fastWindows.size()!=snapshot.windows.size() ||
       prepared->live.size()!=snapshot.windows.size())) return false;
    try {
        std::vector<CliStatusRow> built;
        built.reserve(snapshot.windows.size());
        for(size_t index=0;index<snapshot.windows.size();++index){
            const FastWin& fast=fingerprintsFresh
                ? prepared->fastWindows[index] : snapshot.windows[index];
            if(!SameIdentity(IdentityOf(fast),
                             IdentityOf(snapshot.windows[index]))) return false;
            CliStatusRow row;
            row.window=fast;
            row.fingerprintAvailable=fingerprintsFresh;
            if(fingerprintsFresh){
                row.deskIndex=prepared->live[index].deskIndex;
                row.tabCount=prepared->live[index].tabCount;
                row.activeTitle=prepared->live[index].activeTitle;
            } else {
                for(const DeskRec& desktop : desktops)
                    if(GuidEq(desktop.guid,fast.desktop)){
                        row.deskIndex=desktop.index;
                        break;
                    }
                row.activeTitle=W2U8(fast.title);
            }
            built.push_back(std::move(row));
        }
        output.swap(built);
        return true;
    } catch(...) { return false; }
}

inline std::wstring StripReconcileTitleSuffix(
        std::wstring title,const std::vector<std::wstring>& suffixes){
    for(const std::wstring& suffix : suffixes){
        if(suffix.empty() || title.size()<suffix.size()) continue;
        if(CompareStringOrdinal(
                title.c_str()+title.size()-suffix.size(),
                static_cast<int>(suffix.size()),suffix.c_str(),
                static_cast<int>(suffix.size()),TRUE)==CSTR_EQUAL){
            title.resize(title.size()-suffix.size());
            break;
        }
    }
    return title;
}

inline bool BuildReconcileLivePreparation(
        const ReconcileRequest& request,PreparedReconcileLive& output){
    if(!request.buildLiveFromInputs || !request.sessionWindows ||
       request.fastWindows.size()>MAX_LAYOUT_RECORDS ||
       request.sessionWindows->size()>MAX_RECONCILE_SESSION_WINDOWS ||
       request.desktops.size()>MAX_LAYOUT_RECORDS ||
       request.titleSuffixes.size()>MAX_RECONCILE_TITLE_SUFFIXES ||
       !ReconcileRequestTextWithinBudget(request))
        return false;
    try {
        PreparedReconcileLive built;
        built.live.reserve(request.fastWindows.size());
        built.sessionIndexByFast.assign(request.fastWindows.size(),-1);

        std::map<std::string,std::deque<size_t> > sessionsByTitle;
        for(size_t index=0;index<request.sessionWindows->size();++index)
            sessionsByTitle[request.sessionWindows->at(index).activeTitle]
                .push_back(index);

        for(size_t fastIndex=0;fastIndex<request.fastWindows.size();++fastIndex){
            const FastWin& fast=request.fastWindows[fastIndex];
            LayoutWin live;
            live.app=request.app;
            live.desktop=fast.desktop;
            live.deskIndex=-1;
            for(const DeskRec& desktop : request.desktops){
                if(GuidEq(desktop.guid,fast.desktop)){
                    live.deskIndex=desktop.index;
                    break;
                }
            }
            live.activeTitle=W2U8(StripReconcileTitleSuffix(
                fast.title,request.titleSuffixes));
            auto matching=sessionsByTitle.find(live.activeTitle);
            if(matching!=sessionsByTitle.end() && !matching->second.empty()){
                const size_t sessionIndex=matching->second.front();
                matching->second.pop_front();
                const WinFp& fingerprint=request.sessionWindows->at(sessionIndex);
                built.sessionIndexByFast[fastIndex]=static_cast<int>(sessionIndex);
                if(!fingerprint.activeTitle.empty())
                    live.activeTitle=fingerprint.activeTitle;
                live.activeDomain=fingerprint.activeDomain;
                live.tabCount=fingerprint.tabCount;
                live.counts=fingerprint.counts;
            }
            built.live.push_back(std::move(live));
        }
        output=std::move(built);
        return true;
    } catch(...) { return false; }
}

using CliProfileLiveBuilder=
    std::function<bool(const ReconcileRequest&,PreparedReconcileLive&)>;

inline bool BuildCliProfileBatch(
        const std::vector<ReconcileRequest>& requests,
        PreparedCliProfileBatch& output,
        const CliProfileLiveBuilder& buildLive){
    if(!buildLive || requests.size()>MAX_CLI_PROFILE_BATCH) return false;
    try {
        size_t totalWindows=0;
        for(const ReconcileRequest& request : requests){
            if(request.fastWindows.size()>MAX_LAYOUT_RECORDS-totalWindows)
                return false;
            totalWindows+=request.fastWindows.size();
        }

        PreparedCliProfileBatch built;
        built.live.reserve(totalWindows);
        built.fastWindows.reserve(totalWindows);
        for(const ReconcileRequest& request : requests){
            PreparedReconcileLive prepared;
            if(!buildLive(request,prepared) ||
               prepared.live.size()!=request.fastWindows.size()) return false;
            for(const LayoutWin& live : prepared.live)
                if(live.app!=request.app) return false;
            built.live.insert(built.live.end(),
                              prepared.live.begin(),prepared.live.end());
            built.fastWindows.insert(built.fastWindows.end(),
                                     request.fastWindows.begin(),
                                     request.fastWindows.end());
        }
        output=std::move(built);
        return true;
    } catch(...) { return false; }
}

inline bool BuildCliProfileBatch(
        const std::vector<ReconcileRequest>& requests,
        PreparedCliProfileBatch& output){
    return BuildCliProfileBatch(
        requests,output,
        [](const ReconcileRequest& request,PreparedReconcileLive& prepared){
            return BuildReconcileLivePreparation(request,prepared);
        });
}

template<typename LoadSettings,typename Dispatch>
inline auto RunCliWithLoadedSettings(LoadSettings&& loadSettings,
                                     Dispatch&& dispatch)
    -> decltype(std::forward<Dispatch>(dispatch)()) {
    std::forward<LoadSettings>(loadSettings)();
    return std::forward<Dispatch>(dispatch)();
}

enum class ReconcileResultStatus { Completed, Superseded, Failed };

struct ReconcileResult {
    ReconcileResultStatus status=ReconcileResultStatus::Failed;
    uint64_t operationId=0;
    std::string app;
    uint64_t identityGeneration=0;
    uint64_t contentGeneration=0;
    uint64_t sessionRequestId=0;
    uint64_t sessionDataGeneration=0;
    UnixSeconds nowUtc=0;
    ReconcileFreshness freshness=ReconcileFreshness::Fresh;
    ReconcileWorkMode workMode=ReconcileWorkMode::Plan;
    bool buildLiveFromInputs=false;
    std::vector<LayoutWin> saved;
    std::vector<LayoutWin> live;
    std::set<std::string> reservedRecordIds;
    std::vector<FastWin> fastWindows;
    std::shared_ptr<const std::vector<WinFp> > sessionWindows;
    std::vector<DeskRec> desktops;
    std::vector<std::wstring> titleSuffixes;
    std::vector<int> sessionIndexByFast;
    ReconcilePlan plan;
};

struct ReconcileResultConsumerKey {
    uint64_t operationId=0;
    std::string app;
    ReconcileWorkMode workMode=ReconcileWorkMode::Plan;
    uint64_t identityGeneration=0;
    uint64_t contentGeneration=0;
    uint64_t sessionRequestId=0;
    uint64_t sessionDataGeneration=0;
};

inline bool ReconcileResultIsCurrent(
        const ReconcileResult& result,
        const ReconcileResultConsumerKey& expected) noexcept {
    return result.status==ReconcileResultStatus::Completed &&
        expected.operationId!=0 && result.operationId==expected.operationId &&
        !expected.app.empty() && result.app==expected.app &&
        result.workMode==expected.workMode &&
        expected.identityGeneration!=0 &&
        result.identityGeneration==expected.identityGeneration &&
        expected.contentGeneration!=0 &&
        result.contentGeneration==expected.contentGeneration &&
        expected.sessionRequestId!=0 &&
        result.sessionRequestId==expected.sessionRequestId &&
        expected.sessionDataGeneration!=0 &&
        result.sessionDataGeneration==expected.sessionDataGeneration;
}

template<class Apply>
inline bool ConsumeReconcileResultIfCurrent(
        const ReconcileResult& result,
        const ReconcileResultConsumerKey& expected,Apply&& apply) noexcept {
    if(!ReconcileResultIsCurrent(result,expected)) return false;
    try { apply(); return true; }
    catch(...) { return false; }
}

inline const WinFp* ReconcileSessionForFast(
        const ReconcileResult& result,size_t fastIndex) noexcept {
    if(!result.sessionWindows || fastIndex>=result.sessionIndexByFast.size())
        return nullptr;
    const int sessionIndex=result.sessionIndexByFast[fastIndex];
    if(sessionIndex<0 ||
       static_cast<size_t>(sessionIndex)>=result.sessionWindows->size())
        return nullptr;
    return &result.sessionWindows->at(static_cast<size_t>(sessionIndex));
}

struct ReconcileWorkerOps {
    std::function<bool(const ReconcileRequest&,PreparedReconcileLive&)> buildLive;
    std::function<ReconcilePlan(const ReconcileRequest&)> plan;
    std::function<std::unique_ptr<ReconcileResult>()> makeResult;
    std::function<bool(HWND,UINT,WPARAM,LPARAM)> postMessage;
    std::function<bool()> beforePost;
    std::function<bool()> beforeJoinWait;
};

inline ReconcileWorkerOps DefaultReconcileWorkerOps(){
    ReconcileWorkerOps ops;
    ops.buildLive=[](const ReconcileRequest& request,
                     PreparedReconcileLive& output){
        return BuildReconcileLivePreparation(request,output);
    };
    ops.plan=[](const ReconcileRequest& request){
        return PlanAppReconcile(request.saved,request.live,request.app,
            request.nowUtc,request.reservedRecordIds,request.freshness);
    };
    ops.makeResult=[](){
        return std::unique_ptr<ReconcileResult>(new ReconcileResult());
    };
    ops.postMessage=[](HWND window,UINT message,WPARAM wp,LPARAM lp){
        return PostMessageW(window,message,wp,lp)!=FALSE;
    };
    return ops;
}

inline void FillMissingReconcileWorkerOps(ReconcileWorkerOps& ops){
    ReconcileWorkerOps defaults=DefaultReconcileWorkerOps();
    if(!ops.buildLive) ops.buildLive=defaults.buildLive;
    if(!ops.plan) ops.plan=defaults.plan;
    if(!ops.makeResult) ops.makeResult=defaults.makeResult;
    if(!ops.postMessage) ops.postMessage=defaults.postMessage;
}

inline bool PostReconcileResultOwned(const ReconcileWorkerOps& ops,HWND window,
                                     std::unique_ptr<ReconcileResult> result){
    if(!result) return false;
    ReconcileResult* raw=result.release();
    bool posted=false;
    try {
        posted=ops.postMessage &&
            ops.postMessage(window,WM_RECONCILE_RESULT,0,
                            reinterpret_cast<LPARAM>(raw));
    } catch(...) { posted=false; }
    if(!posted) delete raw;
    return posted;
}

inline size_t DrainPostedReconcileResults(HWND window){
    if(!window) return 0;
    MSG pending{};
    size_t drained=0;
    while(PeekMessageW(&pending,window,WM_RECONCILE_RESULT,
                       WM_RECONCILE_RESULT,PM_REMOVE)){
        std::unique_ptr<ReconcileResult> owned(
            reinterpret_cast<ReconcileResult*>(pending.lParam));
        if(drained!=(std::numeric_limits<size_t>::max)()) ++drained;
    }
    return drained;
}

inline bool ValidReconcileRequest(const ReconcileRequest& request){
    if(request.operationId==0 || request.identityGeneration==0 ||
        request.contentGeneration==0 || request.sessionRequestId==0 ||
        request.sessionDataGeneration==0 || request.nowUtc<=0 ||
        !IsSupportedLayoutApp(request.app) ||
        (request.freshness!=ReconcileFreshness::Fresh &&
         request.freshness!=ReconcileFreshness::CachedStale) ||
        request.saved.size()>MAX_LAYOUT_RECORDS ||
        request.live.size()>MAX_LAYOUT_RECORDS ||
        request.reservedRecordIds.size()>MAX_LAYOUT_RECORDS)
        return false;
    if(!request.buildLiveFromInputs){
        if(request.workMode!=ReconcileWorkMode::Plan ||
           !request.fastWindows.empty() || request.sessionWindows ||
           !request.desktops.empty() || !request.titleSuffixes.empty())
            return false;
        return ReconcileRequestTextWithinBudget(request);
    }
    if(!request.live.empty() || !request.sessionWindows ||
       request.fastWindows.size()>MAX_LAYOUT_RECORDS ||
       request.sessionWindows->size()>MAX_RECONCILE_SESSION_WINDOWS ||
       request.desktops.size()>MAX_LAYOUT_RECORDS ||
       request.titleSuffixes.size()>MAX_RECONCILE_TITLE_SUFFIXES)
        return false;
    if(request.workMode!=ReconcileWorkMode::Plan &&
       request.workMode!=ReconcileWorkMode::PrepareLiveOnly) return false;
    if(!ReconcileRequestTextWithinBudget(request)) return false;
    for(const FastWin& fast : request.fastWindows)
        if(fast.app!=request.app || !fast.hwnd || fast.pid==0 ||
           fast.processStart==0 || GuidIsZero(fast.desktop)) return false;
    return true;
}

class ReconcileWorker {
public:
    explicit ReconcileWorker(HWND window,
            const ReconcileWorkerOps& supplied=ReconcileWorkerOps())
        :window_(window),ops_(supplied){
        FillMissingReconcileWorkerOps(ops_);
        thread_=std::thread(&ReconcileWorker::Run,this);
    }

    ~ReconcileWorker(){ Stop(); }
    ReconcileWorker(const ReconcileWorker&)=delete;
    ReconcileWorker& operator=(const ReconcileWorker&)=delete;

    bool Request(const ReconcileRequest& request){
        if(!ValidReconcileRequest(request)) return false;
        Queued incoming;
        try {
            incoming.request=request;
            incoming.result=MakeResult();
            if(!incoming.result) return false;
        } catch(...) { return false; }

        Queued superseded;
        bool replaced=false;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if(stopping_) return false;
            auto found=pending_.find(request.app);
            if(found==pending_.end()){
                pending_.emplace(request.app,std::move(incoming));
            } else {
                superseded=std::move(found->second);
                found->second=std::move(incoming);
                replaced=true;
            }
        } catch(...) { return false; }
        if(replaced){
            PrepareResult(std::move(superseded.request),
                          ReconcileResultStatus::Superseded,*superseded.result);
            PostIfRunning(std::move(superseded.result));
        }
        changed_.notify_one();
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
            if(workerThreadId_==std::this_thread::get_id() ||
               PostingWorkerSlot()==this){
                lock.unlock();
                if(notify) changed_.notify_all();
                return false;
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
            stopped_=true;
            joinInProgress_=false;
        }
        changed_.notify_all();
        return true;
    }

    size_t PendingCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.size();
    }

    size_t ActiveCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_ ? 1U : 0U;
    }

    size_t OutstandingForApp(const std::string& app) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return (active_ && activeApp_==app ? 1U : 0U) +
               (pending_.count(app) ? 1U : 0U);
    }

private:
    struct Queued {
        ReconcileRequest request;
        std::unique_ptr<ReconcileResult> result;
    };

    static ReconcileWorker*& PostingWorkerSlot(){
        static thread_local ReconcileWorker* current=nullptr;
        return current;
    }

    struct PostingScope {
        ReconcileWorker* previous;
        explicit PostingScope(ReconcileWorker* current)
            :previous(PostingWorkerSlot()){
            PostingWorkerSlot()=current;
        }
        ~PostingScope(){ PostingWorkerSlot()=previous; }
    };

    std::unique_ptr<ReconcileResult> MakeResult(){
        return ops_.makeResult ? ops_.makeResult() :
            std::unique_ptr<ReconcileResult>();
    }

    static void PrepareResult(ReconcileRequest&& request,
                              ReconcileResultStatus status,
                              ReconcileResult& result){
        result.status=status;
        result.operationId=request.operationId;
        result.app.swap(request.app);
        result.identityGeneration=request.identityGeneration;
        result.contentGeneration=request.contentGeneration;
        result.sessionRequestId=request.sessionRequestId;
        result.sessionDataGeneration=request.sessionDataGeneration;
        result.nowUtc=request.nowUtc;
        result.freshness=request.freshness;
        result.workMode=request.workMode;
        result.buildLiveFromInputs=request.buildLiveFromInputs;
        result.saved.swap(request.saved);
        result.live.swap(request.live);
        result.reservedRecordIds.swap(request.reservedRecordIds);
        result.fastWindows.swap(request.fastWindows);
        result.sessionWindows=std::move(request.sessionWindows);
        result.desktops.swap(request.desktops);
        result.titleSuffixes.swap(request.titleSuffixes);
    }

    void FinishPost(){
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(postsInFlight_>0) --postsInFlight_;
        }
        changed_.notify_all();
    }

    void PostIfRunning(std::unique_ptr<ReconcileResult> result) noexcept {
        if(!result) return;
        HWND target=nullptr;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if(stopping_ || !window_) return;
            target=window_;
            ++postsInFlight_;
        } catch(...) { return; }
        struct Completion {
            ReconcileWorker* worker;
            explicit Completion(ReconcileWorker* value):worker(value){}
            ~Completion(){ worker->FinishPost(); }
        } completion(this);
        try {
            PostingScope posting(this);
            bool proceed=!ops_.beforePost || ops_.beforePost();
            if(proceed) PostReconcileResultOwned(
                ops_,target,std::move(result));
        } catch(...) {}
    }

    bool Choose(Queued& output){
        if(pending_.empty()) return false;
        auto found=pending_.begin();
        output=std::move(found->second);
        pending_.erase(found);
        return true;
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
        std::unique_ptr<ReconcileResult> result=std::move(queued.result);
        ReconcilePlan plan;
        PreparedReconcileLive prepared;
        ReconcileResultStatus status=ReconcileResultStatus::Failed;
        try {
            if(queued.request.buildLiveFromInputs){
                if(!ops_.buildLive || !ops_.buildLive(queued.request,prepared) ||
                   prepared.live.size()!=queued.request.fastWindows.size() ||
                   prepared.sessionIndexByFast.size()!=
                       queued.request.fastWindows.size() ||
                   prepared.live.size()>MAX_LAYOUT_RECORDS)
                    throw std::runtime_error("invalid reconcile live preparation");
                queued.request.live=std::move(prepared.live);
            }
            if(queued.request.workMode==ReconcileWorkMode::Plan)
                plan=ops_.plan(queued.request);
            else {
                plan.app=queued.request.app;
                plan.nowUtc=queued.request.nowUtc;
                plan.freshness=queued.request.freshness;
            }
            status=ReconcileResultStatus::Completed;
        } catch(...) {}
        if(result){
            PrepareResult(std::move(queued.request),status,*result);
            if(status==ReconcileResultStatus::Completed){
                result->plan=std::move(plan);
                result->sessionIndexByFast=
                    std::move(prepared.sessionIndexByFast);
            }
        }
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
                changed_.wait(lock,[&]{ return stopping_ || !pending_.empty(); });
                if(stopping_) break;
                if(!Choose(queued)) continue;
                active_=true;
                activeApp_=queued.request.app;
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

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    HWND window_=nullptr;
    ReconcileWorkerOps ops_;
    std::thread thread_;
    bool stopping_=false;
    bool stopped_=false;
    bool joinInProgress_=false;
    bool active_=false;
    size_t postsInFlight_=0;
    std::thread::id workerThreadId_;
    std::string activeApp_;
    std::map<std::string,Queued> pending_;
};
