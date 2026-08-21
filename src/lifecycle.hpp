// lifecycle.hpp -- pure, per-app restore/save state and per-window move budgets.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "layout.hpp"
#include "move_queue.hpp"
#include "session_worker.hpp"
#include "window_identity.hpp"

struct FinalizationState {
    bool running = false;
    bool finished = false;

    bool begin(){
        if(running || finished) return false;
        running = true;
        return true;
    }

    void finish(){
        running = false;
        finished = true;
    }

    void retry(){
        running = false;
    }

    void reopen(){
        running = false;
        finished = false;
    }
};

enum class AutoRuntimeStartResult {
    Ready,
    WaitingForRetry,
    MonitorUnavailable,
    LoadUnavailable,
    InitializationUnavailable,
    HeartbeatUnavailable
};

struct AutoLoadRetryState {
    enum : unsigned { kMaxAlternateMonitorRetries=4 };

    bool loaded=false;
    bool layoutPrepared=false;
    bool initialized=false;
    bool monitorStarted=false;
    bool heartbeatStarted=false;
    unsigned retryAttempt=0;
    unsigned alternateMonitorRetries=0;
    unsigned initializationCount=0;
    uint64_t lastAttemptMs=0;
    uint64_t nextAttemptMs=0;

    bool due(uint64_t nowMs) const {
        return !loaded && (nextAttemptMs==0 || nowMs>=nextAttemptMs);
    }

    bool monitorRetryDelayMs(uint64_t nowMs,uint32_t& delayOut) const noexcept {
        if(loaded || monitorStarted || nextAttemptMs==0) return false;
        if(nowMs>=nextAttemptMs){ delayOut=0; return true; }
        const uint64_t remaining=nextAttemptMs-nowMs;
        const uint64_t maximum=static_cast<uint64_t>(UINT32_MAX)-1;
        delayOut=static_cast<uint32_t>((std::min)(remaining,maximum));
        return true;
    }

    void failed(uint64_t nowMs){
        const unsigned shift=(std::min)(retryAttempt,6U);
        const uint64_t delay=(std::min<uint64_t>)(60000,1000ULL<<shift);
        if(retryAttempt!=(std::numeric_limits<unsigned>::max)()) ++retryAttempt;
        lastAttemptMs=nowMs;
        nextAttemptMs=nowMs>(std::numeric_limits<uint64_t>::max)()-delay
            ? (std::numeric_limits<uint64_t>::max)() : nowMs+delay;
    }

    bool monitorFailed(uint64_t nowMs){
        loaded=false;
        monitorStarted=false;
        failed(nowMs);
        if(alternateMonitorRetries>=kMaxAlternateMonitorRetries) return false;
        ++alternateMonitorRetries;
        return true;
    }

    void monitorSucceeded(){
        monitorStarted=true;
        alternateMonitorRetries=0;
    }

    void layoutSucceeded(){
        layoutPrepared=true;
        clearBackoff();
    }

    void initializationSucceeded(){
        if(!initialized){
            initialized=true;
            if(initializationCount!=(std::numeric_limits<unsigned>::max)())
                ++initializationCount;
        }
        loaded=layoutPrepared && initialized && monitorStarted &&
            heartbeatStarted;
        if(loaded) clearBackoff();
    }

    void heartbeatFailed(uint64_t nowMs){
        loaded=false;
        heartbeatStarted=false;
        failed(nowMs);
    }

    void heartbeatSucceeded(){
        heartbeatStarted=true;
        loaded=layoutPrepared && initialized && monitorStarted;
        if(loaded) clearBackoff();
    }

    void clearBackoff(){
        retryAttempt=0;
        lastAttemptMs=0;
        nextAttemptMs=0;
    }

    void reset(){
        loaded=false;
        layoutPrepared=false;
        initialized=false;
        monitorStarted=false;
        heartbeatStarted=false;
        retryAttempt=0;
        alternateMonitorRetries=0;
        lastAttemptMs=0;
        nextAttemptMs=0;
    }
};

template<class ArmMonitor,class LoadLayout,class Initialize,
         class ArmHeartbeat,class PostMonitorRetry>
inline AutoRuntimeStartResult AdvanceAutoRuntimeStart(
        AutoLoadRetryState& state,uint64_t nowMs,ArmMonitor armMonitor,
        LoadLayout loadLayout,Initialize initialize,
        ArmHeartbeat armHeartbeat,PostMonitorRetry postMonitorRetry) noexcept {
    if(state.loaded) return AutoRuntimeStartResult::Ready;
    if(!state.monitorStarted){
        bool started=false;
        try { started=armMonitor(); } catch(...) { started=false; }
        if(!started){
            const bool post=state.monitorFailed(nowMs);
            if(post){
                try { (void)postMonitorRetry(); } catch(...) {}
            }
            return AutoRuntimeStartResult::MonitorUnavailable;
        }
        state.monitorSucceeded();
    }
    if(!state.due(nowMs)) return AutoRuntimeStartResult::WaitingForRetry;
    if(!state.layoutPrepared){
        bool prepared=false;
        try { prepared=loadLayout(); } catch(...) { prepared=false; }
        if(!prepared){
            state.failed(nowMs);
            return AutoRuntimeStartResult::LoadUnavailable;
        }
        state.layoutSucceeded();
    }
    if(!state.heartbeatStarted){
        bool started=false;
        try { started=armHeartbeat(); } catch(...) { started=false; }
        if(!started){
            state.heartbeatFailed(nowMs);
            return AutoRuntimeStartResult::HeartbeatUnavailable;
        }
        state.heartbeatSucceeded();
    }
    if(!state.initialized){
        bool initialized=false;
        try { initialized=initialize(); } catch(...) { initialized=false; }
        if(!initialized){
            state.failed(nowMs);
            return AutoRuntimeStartResult::InitializationUnavailable;
        }
        state.initializationSucceeded();
    }
    return state.loaded ? AutoRuntimeStartResult::Ready
                        : AutoRuntimeStartResult::WaitingForRetry;
}

class OperationAppProfiles {
public:
    OperationAppProfiles()=default;
    explicit OperationAppProfiles(std::vector<AppProfile> profiles)
        : profiles_(std::move(profiles)) {}

    const AppProfile* find(const std::string& app) const {
        for(const AppProfile& profile : profiles_)
            if(profile.id==app) return &profile;
        return nullptr;
    }

    const std::vector<AppProfile>& all() const noexcept { return profiles_; }

private:
    std::vector<AppProfile> profiles_;
};

struct SettingsRuntimeSnapshot {
    unsigned hotkeyVk=0;
    unsigned hotkeyMods=0;
    bool autoFix=false;
    bool runAtLogon=false;
    bool firefox=true;
    bool chrome=true;
    bool edge=true;
};

inline bool SettingsProfilesChanged(
        const SettingsRuntimeSnapshot& current,
        const SettingsRuntimeSnapshot& requested) noexcept {
    return current.firefox!=requested.firefox ||
        current.chrome!=requested.chrome || current.edge!=requested.edge;
}

template<class Checkpoint,class CancelAutoRuntime>
inline bool ApplySettingsRuntimeTransaction(
        SettingsRuntimeSnapshot& current,
        const SettingsRuntimeSnapshot& requested,
        Checkpoint checkpoint,CancelAutoRuntime cancelAutoRuntime) noexcept {
    const bool profilesChanged=SettingsProfilesChanged(current,requested);
    const bool autoChanged=current.autoFix!=requested.autoFix || profilesChanged;
    const bool mustCheckpoint=current.autoFix &&
        (!requested.autoFix || profilesChanged);
    if(mustCheckpoint){
        bool saved=false;
        try { saved=checkpoint(); } catch(...) { saved=false; }
        if(!saved) return false;
    }
    if(autoChanged){
        bool cancelled=false;
        try { cancelled=cancelAutoRuntime(); } catch(...) { cancelled=false; }
        if(!cancelled) return false;
    }
    current=requested;
    return true;
}

enum class CheckpointReason {
    Heartbeat, SettingsChange, QueryEndSession, Finalize
};

enum class MoveCancellationDisposition {
    CancelImmediately, AwaitTerminalAcknowledgement
};

inline MoveCancellationDisposition MoveCancellationDispositionFor(
        bool issueAwaitingVerify,bool exactIdentityStillAlive) noexcept {
    return issueAwaitingVerify && exactIdentityStillAlive
        ? MoveCancellationDisposition::AwaitTerminalAcknowledgement
        : MoveCancellationDisposition::CancelImmediately;
}

inline MoveCancellationDisposition MoveCancellationDispositionFor(
        bool issueAwaitingVerify,WindowIdentityRecapture recapture) noexcept {
    return issueAwaitingVerify && recapture!=WindowIdentityRecapture::Lost
        ? MoveCancellationDisposition::AwaitTerminalAcknowledgement
        : MoveCancellationDisposition::CancelImmediately;
}

enum class IdentityRecaptureRetryAction {
    Continue,
    Retry,
    RetireCancelled
};

class IdentityRecaptureRetryBudget {
public:
    enum : unsigned { kMaxIndeterminateChecks=4 };

    IdentityRecaptureRetryAction observe(
            WindowIdentityRecapture recapture) noexcept {
        if(recapture!=WindowIdentityRecapture::Indeterminate)
            return IdentityRecaptureRetryAction::Continue;
        if(indeterminateChecks_<kMaxIndeterminateChecks)
            ++indeterminateChecks_;
        return indeterminateChecks_>=kMaxIndeterminateChecks
            ? IdentityRecaptureRetryAction::RetireCancelled
            : IdentityRecaptureRetryAction::Retry;
    }

    void reset() noexcept { indeterminateChecks_=0; }

private:
    unsigned indeterminateChecks_=0;
};

template<class MarkRuntime>
inline bool PublishMoveCancellationIntent(
        bool& cancellationPending,const uint64_t* jobIds,size_t jobCount,
        MarkRuntime markRuntime) noexcept {
    static_assert(noexcept(markRuntime(uint64_t{})),
        "move cancellation runtime marking must not throw");
    if(jobCount!=0 && !jobIds) return false;
    cancellationPending=true;
    for(size_t index=0;index<jobCount;++index) markRuntime(jobIds[index]);
    return true;
}

class MoveTerminalOutcomes {
public:
    bool initialize(size_t count) noexcept {
        if(count>MAX_LAYOUT_RECORDS) return false;
        try {
            std::vector<unsigned char> next(count,0);
            values_.swap(next);
            return true;
        } catch(...) { return false; }
    }

    bool markSucceeded(size_t index) noexcept {
        if(index>=values_.size()) return false;
        values_[index]=1;
        return true;
    }

    bool succeeded(size_t index) const noexcept {
        return index<values_.size() && values_[index]!=0;
    }

    size_t size() const noexcept { return values_.size(); }

private:
    std::vector<unsigned char> values_;
};

struct DefaultMoveResultTextAssign {
    void operator()(std::string& destination,
                    const std::string& source) const {
        destination=source;
    }
};

template<class AssignText>
inline bool PrepareCancelledMoveResult(
        const MoveToken& token,const std::string& runtimeKey,
        const std::string& recordId,MoveResult& output,
        AssignText assignText) noexcept {
    static_assert(std::is_nothrow_move_assignable<MoveResult>::value,
        "prepared cancellation results require no-throw publication");
    try {
        MoveResult next;
        next.token=token;
        assignText(next.runtimeKey,runtimeKey);
        assignText(next.recordId,recordId);
        next.completed=true;
        next.terminal=MoveTerminal::Cancelled;
        output=std::move(next);
        return true;
    } catch(...) { return false; }
}

inline bool PrepareCancelledMoveResult(
        const MoveToken& token,const std::string& runtimeKey,
        const std::string& recordId,MoveResult& output) noexcept {
    return PrepareCancelledMoveResult(token,runtimeKey,recordId,output,
                                      DefaultMoveResultTextAssign());
}

template<class Complete,class Fail>
inline bool RunTerminalCompletionOrFail(Complete complete,Fail fail) noexcept {
    try {
        complete();
        return true;
    } catch(...) {
        try { fail(); } catch(...) {}
        return false;
    }
}

template<class Work,class Fail,class Finally>
inline bool RunMessageRouteNoThrow(Work work,Fail fail,Finally finallyWork)
        noexcept {
    bool completed=true;
    try { work(); }
    catch(...) {
        completed=false;
        try { fail(); } catch(...) {}
    }
    try { finallyWork(); }
    catch(...) {
        if(completed){
            completed=false;
            try { fail(); } catch(...) {}
        }
    }
    return completed;
}

enum class IssuedMoveRetirementAction {
    WaitForReadback,
    CancelAfterSafeReadback,
    ConsumeProtectedCheckpointAndCancel
};

class IssuedMoveRetirementTracker {
public:
    enum : unsigned { kMaxUnresolvedReadbacks=4 };

    IssuedMoveRetirementAction observe(
            bool exactIdentityStillAlive,MoveAttemptOutcome readback) noexcept {
        if(!exactIdentityStillAlive)
            return IssuedMoveRetirementAction::CancelAfterSafeReadback;
        if(readback==MoveAttemptOutcome::OnDestination)
            return IssuedMoveRetirementAction::CancelAfterSafeReadback;
        if(readback==MoveAttemptOutcome::PermanentFailure)
            return IssuedMoveRetirementAction::ConsumeProtectedCheckpointAndCancel;
        if(unresolvedReadbacks_<kMaxUnresolvedReadbacks)
            ++unresolvedReadbacks_;
        return unresolvedReadbacks_>=kMaxUnresolvedReadbacks
            ? IssuedMoveRetirementAction::ConsumeProtectedCheckpointAndCancel
            : IssuedMoveRetirementAction::WaitForReadback;
    }

    unsigned unresolvedReadbacks() const noexcept {
        return unresolvedReadbacks_;
    }

private:
    unsigned unresolvedReadbacks_=0;
};

class CheckpointController {
public:
    FinalizationState finalization;
    bool heartbeatDeferred=false;

    bool dispatch(CheckpointReason reason,bool enabled,bool loaded,
                  bool reservationActive,
                  const std::function<bool(CheckpointReason)>& checkpoint){
        if(!enabled) return true;
        if(!loaded) return reason!=CheckpointReason::SettingsChange;
        if(reason==CheckpointReason::Heartbeat && reservationActive){
            heartbeatDeferred=true;
            return true;
        }
        if(reason!=CheckpointReason::Finalize)
            return checkpoint ? checkpoint(reason) : false;
        if(!finalization.begin()) return finalization.finished;
        bool saved=false;
        try { saved=checkpoint && checkpoint(reason); }
        catch(...) {
            finalization.retry();
            return false;
        }
        if(saved) finalization.finish();
        else finalization.retry();
        return saved;
    }

    bool runDeferredHeartbeat(bool enabled,bool loaded,bool reservationActive,
            const std::function<bool(CheckpointReason)>& checkpoint){
        if(!heartbeatDeferred) return true;
        if(!enabled || !loaded){ heartbeatDeferred=false; return true; }
        if(reservationActive) return true;
        heartbeatDeferred=false;
        return checkpoint && checkpoint(CheckpointReason::Heartbeat);
    }

    bool reservationTerminated(bool matchingReservationErased,
            bool reservationActive,bool enabled,bool loaded,
            const std::function<bool(CheckpointReason)>& checkpoint){
        if(!matchingReservationErased) return true;
        return runDeferredHeartbeat(enabled,loaded,reservationActive,checkpoint);
    }

    bool acknowledgeReservationBeforeRelease(
            bool matchingTerminal,bool /*lastReservation*/,
            bool enabled,bool loaded,
            const std::function<bool(CheckpointReason)>& checkpoint){
        if(!matchingTerminal) return true;
        // The caller still exposes the acknowledged reservation while this
        // callback runs, so a checkpoint cannot observe an intermediate move.
        // Consume once even when sibling reservations remain: otherwise this
        // reservation's protected origin would disappear before the eventual
        // deferred checkpoint.
        try {
            return runDeferredHeartbeat(enabled,loaded,false,checkpoint);
        } catch(...) {
            // The acknowledgement is terminal.  Do not let a failing
            // checkpoint strand the reservation or replay the callback.
            return false;
        }
    }
};

template<class Finalize,class Destroy,class Rearm>
inline bool RunTrayExit(Finalize finalize,Destroy destroy,Rearm rearm) noexcept {
    bool finalized=false;
    try { finalized=finalize(); } catch(...) { finalized=false; }
    if(!finalized) return false;
    bool destroyed=false;
    try { destroyed=static_cast<bool>(destroy()); }
    catch(...) { destroyed=false; }
    if(destroyed) return true;
    try { rearm(); }
    catch(...) {}
    return false;
}

class RuntimeQuiescenceState {
public:
    enum class Phase { Running, Quiescing, Quiesced };

    bool begin() noexcept {
        if(phase_==Phase::Quiesced) return false;
        if(phase_==Phase::Running) phase_=Phase::Quiescing;
        return true;
    }
    void finish() noexcept { phase_=Phase::Quiesced; }
    bool acceptsDispatch() const noexcept { return phase_==Phase::Running; }
    bool quiesced() const noexcept { return phase_==Phase::Quiesced; }

private:
    Phase phase_=Phase::Running;
};

template<class Quiesce>
inline bool RunRuntimeQuiescence(RuntimeQuiescenceState& state,
                                 Quiesce quiesce) noexcept {
    if(state.quiesced()) return true;
    if(!state.begin()) return false;
    try {
        quiesce();
        state.finish();
        return true;
    } catch(...) {
        return false;
    }
}

template<class Finalize,class Quiesce>
inline bool FinalizeSessionAndQuiesce(bool ending,Finalize finalize,
                                      Quiesce quiesce) noexcept {
    if(!ending) return false;
    try { (void)finalize(); } catch(...) {}
    try { return quiesce(); }
    catch(...) { return false; }
}

enum class TrayInstanceAcquireStatus { Acquired, AlreadyRunning, Failed };

template<class Acquire,class Body,class Release>
inline int RunWithTrayInstanceScope(bool cli,Acquire acquire,Body body,
                                    Release release){
    if(cli) return body();
    const TrayInstanceAcquireStatus status=acquire();
    if(status==TrayInstanceAcquireStatus::AlreadyRunning) return 0;
    if(status!=TrayInstanceAcquireStatus::Acquired) return 1;
    try {
        const int result=body();
        release();
        return result;
    } catch(...) {
        release();
        throw;
    }
}

enum class MoveDispatchDisposition {
    Stale, Accepted, OperationCompleted
};

struct MoveOperationSummary {
    MoveOwner owner=MoveOwner::AutoReconcile;
    uint64_t operationId=0;
    size_t expected=0;
    size_t outstanding=0;
    size_t succeeded=0;
    size_t cancelled=0;
    size_t permanentFailures=0;
    size_t exhausted=0;

    bool complete() const noexcept {
        return expected!=0 && outstanding==0;
    }
};

struct MoveOperationKey {
    MoveOwner owner=MoveOwner::AutoReconcile;
    uint64_t operationId=0;

    bool operator<(const MoveOperationKey& other) const noexcept {
        if(owner!=other.owner)
            return static_cast<int>(owner)<static_cast<int>(other.owner);
        return operationId<other.operationId;
    }
};

inline bool SameMoveToken(const MoveToken& left,const MoveToken& right) noexcept {
    return left.owner==right.owner &&
        left.operationId==right.operationId &&
        left.jobId==right.jobId && left.itemIndex==right.itemIndex;
}

class MoveOperationDispatcher {
    struct Operation {
        MoveOperationSummary summary;
        std::map<uint64_t,MoveToken> jobs;
    };

    static const size_t kMaxOperations=256;
    static const size_t kMaxJobs=4096;
    std::map<MoveOperationKey,Operation> operations_;
    std::map<uint64_t,MoveOperationKey> jobs_;

    static bool validOwner(MoveOwner owner) noexcept {
        return owner==MoveOwner::AutoReconcile ||
            owner==MoveOwner::ManualTray || owner==MoveOwner::Picker;
    }

    static bool terminal(MoveTerminal value) noexcept {
        return value==MoveTerminal::Succeeded ||
            value==MoveTerminal::Cancelled ||
            value==MoveTerminal::PermanentFailure ||
            value==MoveTerminal::Exhausted;
    }

public:
    bool begin(MoveOwner owner,uint64_t operationId,
               const std::vector<MoveToken>& tokens){
        if(!validOwner(owner) || operationId==0 || tokens.empty() ||
           tokens.size()>kMaxJobs || operations_.size()>=kMaxOperations ||
           jobs_.size()>kMaxJobs-tokens.size()) return false;
        MoveOperationKey key{owner,operationId};
        if(operations_.count(key)) return false;

        try {
            Operation operation;
            operation.summary.owner=owner;
            operation.summary.operationId=operationId;
            operation.summary.expected=tokens.size();
            operation.summary.outstanding=tokens.size();
            std::set<size_t> itemIndices;
            for(const MoveToken& token : tokens){
                if(token.owner!=owner || token.operationId!=operationId ||
                   token.jobId==0 || jobs_.count(token.jobId) ||
                   !itemIndices.insert(token.itemIndex).second ||
                   !operation.jobs.emplace(token.jobId,token).second)
                    return false;
            }
            std::map<MoveOperationKey,Operation> nextOperations=operations_;
            std::map<uint64_t,MoveOperationKey> nextJobs=jobs_;
            if(!nextOperations.emplace(key,operation).second) return false;
            for(const MoveToken& token : tokens)
                if(!nextJobs.emplace(token.jobId,key).second) return false;
            operations_.swap(nextOperations);
            jobs_.swap(nextJobs);
        } catch(...) { return false; }
        return true;
    }

    bool lookup(MoveOwner owner,uint64_t operationId,
                MoveOperationSummary& out) const {
        const auto found=operations_.find(MoveOperationKey{owner,operationId});
        if(found==operations_.end()) return false;
        out=found->second.summary;
        return true;
    }

    bool containsJob(uint64_t jobId) const noexcept {
        return jobs_.count(jobId)!=0;
    }

    MoveDispatchDisposition dispatch(const MoveResult& result,
                                     MoveOperationSummary& out){
        if(!result.completed || !terminal(result.terminal))
            return MoveDispatchDisposition::Stale;
        const auto membership=jobs_.find(result.token.jobId);
        if(membership==jobs_.end()) return MoveDispatchDisposition::Stale;
        const MoveOperationKey supplied{
            result.token.owner,result.token.operationId};
        if(membership->second<supplied || supplied<membership->second)
            return MoveDispatchDisposition::Stale;
        const auto operation=operations_.find(membership->second);
        if(operation==operations_.end()) return MoveDispatchDisposition::Stale;
        const auto item=operation->second.jobs.find(result.token.jobId);
        if(item==operation->second.jobs.end() ||
           !SameMoveToken(item->second,result.token) ||
           operation->second.summary.outstanding==0)
            return MoveDispatchDisposition::Stale;

        switch(result.terminal){
        case MoveTerminal::Succeeded: ++operation->second.summary.succeeded; break;
        case MoveTerminal::Cancelled: ++operation->second.summary.cancelled; break;
        case MoveTerminal::PermanentFailure:
            ++operation->second.summary.permanentFailures; break;
        case MoveTerminal::Exhausted: ++operation->second.summary.exhausted; break;
        default: return MoveDispatchDisposition::Stale;
        }
        --operation->second.summary.outstanding;
        operation->second.jobs.erase(item);
        jobs_.erase(membership);
        out=operation->second.summary;
        if(!out.complete()) return MoveDispatchDisposition::Accepted;
        operations_.erase(operation);
        return MoveDispatchDisposition::OperationCompleted;
    }

    bool cancelOperation(MoveOwner owner,uint64_t operationId,
                         std::vector<uint64_t>& cancelledJobIds,
                         MoveOperationSummary& out){
        cancelledJobIds.clear();
        out=MoveOperationSummary();
        const auto operation=operations_.find(MoveOperationKey{owner,operationId});
        if(operation==operations_.end()) return false;
        std::vector<uint64_t> ids;
        try {
            ids.reserve(operation->second.jobs.size());
            for(const auto& item : operation->second.jobs)
                ids.push_back(item.first);
        } catch(...) { return false; }
        MoveOperationSummary completed=operation->second.summary;
        completed.cancelled+=completed.outstanding;
        completed.outstanding=0;
        for(uint64_t jobId : ids) jobs_.erase(jobId);
        operations_.erase(operation);
        cancelledJobIds.swap(ids);
        out=completed;
        return true;
    }
};

struct MoveReservation {
    MoveToken token;
    WindowIdentityKey identity;
    std::string boundRecordId;
    GUID originDesktop={0};
    int originDeskIndex=-1;
    LayoutWin provisionalOriginRecord;
    bool hasProvisionalOriginRecord=false;
};

enum class MoveReservationUpdate { Rejected, Inserted, Replaced };

class MoveReservationBook {
    static const size_t kMaxReservations=4096;
    std::map<WindowIdentityKey,MoveReservation> byIdentity_;
    std::map<uint64_t,WindowIdentityKey> byJob_;

    static bool valid(const MoveReservation& value) noexcept {
        if(value.token.operationId==0 || value.token.jobId==0 ||
           value.identity.hwnd==0 || value.identity.pid==0 ||
           value.identity.processStart==0) return false;
        switch(value.token.owner){
        case MoveOwner::AutoReconcile:
        case MoveOwner::ManualTray:
        case MoveOwner::Picker:
            return true;
        }
        return false;
    }

public:
    MoveReservationUpdate reserve(const MoveReservation& value,
                                  MoveReservation* displaced=nullptr){
        if(!valid(value)) return MoveReservationUpdate::Rejected;
        const auto old=byIdentity_.find(value.identity);
        if(old==byIdentity_.end() && byIdentity_.size()>=kMaxReservations)
            return MoveReservationUpdate::Rejected;
        const auto duplicateJob=byJob_.find(value.token.jobId);
        if(duplicateJob!=byJob_.end()) return MoveReservationUpdate::Rejected;

        try {
            std::map<WindowIdentityKey,MoveReservation> nextIdentity=byIdentity_;
            std::map<uint64_t,WindowIdentityKey> nextJobs=byJob_;
            MoveReservation prior;
            const bool replacing=old!=byIdentity_.end();
            if(replacing){
                prior=old->second;
                nextJobs.erase(prior.token.jobId);
                nextIdentity.erase(value.identity);
            }
            if(!nextIdentity.emplace(value.identity,value).second ||
               !nextJobs.emplace(value.token.jobId,value.identity).second)
                return MoveReservationUpdate::Rejected;
            if(replacing && displaced) *displaced=prior;
            byIdentity_.swap(nextIdentity);
            byJob_.swap(nextJobs);
            return replacing
                ? MoveReservationUpdate::Replaced
                : MoveReservationUpdate::Inserted;
        } catch(...) { return MoveReservationUpdate::Rejected; }
    }

    bool lookup(const WindowIdentityKey& identity,MoveReservation& out) const {
        const auto found=byIdentity_.find(identity);
        if(found==byIdentity_.end()) return false;
        try {
            MoveReservation copy=found->second;
            out=std::move(copy);
        } catch(...) { return false; }
        return true;
    }

    bool lookupJob(uint64_t jobId,MoveReservation& out) const {
        const auto job=byJob_.find(jobId);
        if(job==byJob_.end()) return false;
        return lookup(job->second,out);
    }

    bool erase(const WindowIdentityKey& identity,const MoveToken& token){
        const auto found=byIdentity_.find(identity);
        if(found==byIdentity_.end() ||
           !SameMoveToken(found->second.token,token)) return false;
        byJob_.erase(found->second.token.jobId);
        byIdentity_.erase(found);
        return true;
    }

    bool erase(const MoveToken& token){
        const auto job=byJob_.find(token.jobId);
        if(job==byJob_.end()) return false;
        return erase(job->second,token);
    }

    bool snapshot(std::vector<MoveReservation>& out) const {
        std::vector<MoveReservation> values;
        try {
            values.reserve(byIdentity_.size());
            for(const auto& item : byIdentity_) values.push_back(item.second);
        } catch(...) { return false; }
        out.swap(values);
        return true;
    }

    size_t size() const noexcept { return byIdentity_.size(); }
    bool empty() const noexcept { return byIdentity_.empty(); }
};

struct AsyncSessionRoute {
    uint64_t requestId=0;
    uint64_t operationId=0;
    std::string app;
    SessionPurpose purpose=SessionPurpose::MetadataProbe;
    uint64_t identityGeneration=0;
    uint64_t deadlineMs=0;
};

enum class AsyncRetirementReason {
    Completed, Failed, Superseded, TimedOut, Cancelled, Rejected
};

struct AsyncSessionRetirement {
    AsyncSessionRoute route;
    AsyncRetirementReason reason=AsyncRetirementReason::Failed;
};

enum class AsyncRouteAdmission {
    Accepted, RejectedProtected, RejectedDeadline, RejectedCapacity,
    Stale, Invalid
};

class AsyncSessionRouteGate {
    static const size_t kMaxOutstanding=16;
    static const uint64_t kMaxLifetimeMs=60000;
    uint64_t requestHighWater_=0;
    std::map<std::string,AsyncSessionRoute> byApp_;
    std::map<uint64_t,std::string> byRequest_;

    static bool validPurpose(SessionPurpose purpose) noexcept {
        switch(purpose){
        case SessionPurpose::AutoReconcile:
        case SessionPurpose::HeartbeatSave:
        case SessionPurpose::ManualSave:
        case SessionPurpose::ManualRestore:
        case SessionPurpose::Search:
        case SessionPurpose::MetadataProbe:
            return true;
        }
        return false;
    }

    static AsyncSessionRetirement retirement(
            const AsyncSessionRoute& route,AsyncRetirementReason reason){
        AsyncSessionRetirement result;
        result.route=route;
        result.reason=reason;
        return result;
    }

    AsyncRouteAdmission reject(const AsyncSessionRoute& route,
            AsyncRouteAdmission admission,AsyncRetirementReason reason,
            std::vector<AsyncSessionRetirement>& retired){
        try {
            std::vector<AsyncSessionRetirement> event;
            event.push_back(retirement(route,reason));
            retired.swap(event);
        } catch(...) { return AsyncRouteAdmission::Invalid; }
        requestHighWater_=route.requestId;
        return admission;
    }

public:
    static uint64_t maxLifetimeMs() noexcept { return kMaxLifetimeMs; }

    AsyncRouteAdmission submit(const AsyncSessionRoute& route,uint64_t nowMs,
                               std::vector<AsyncSessionRetirement>& retired){
        retired.clear();
        if(route.requestId==0 || route.operationId==0 || route.app.empty() ||
           route.app.size()>128 || !validPurpose(route.purpose) ||
           route.identityGeneration==0)
            return AsyncRouteAdmission::Invalid;
        if(route.requestId<=requestHighWater_ || byRequest_.count(route.requestId))
            return AsyncRouteAdmission::Stale;
        if(route.deadlineMs<=nowMs || route.deadlineMs-nowMs>kMaxLifetimeMs)
            return reject(route,AsyncRouteAdmission::RejectedDeadline,
                AsyncRetirementReason::Rejected,retired);

        const auto current=byApp_.find(route.app);
        if(current!=byApp_.end() &&
           SessionPurposePriority(route.purpose)<
               SessionPurposePriority(current->second.purpose)){
            return reject(route,AsyncRouteAdmission::RejectedProtected,
                AsyncRetirementReason::Rejected,retired);
        }
        if(current==byApp_.end() && byApp_.size()>=kMaxOutstanding)
            return reject(route,AsyncRouteAdmission::RejectedCapacity,
                AsyncRetirementReason::Rejected,retired);

        try {
            std::map<std::string,AsyncSessionRoute> nextApps=byApp_;
            std::map<uint64_t,std::string> nextRequests=byRequest_;
            std::vector<AsyncSessionRetirement> events;
            if(current!=byApp_.end()){
                events.push_back(retirement(
                    current->second,AsyncRetirementReason::Superseded));
                nextRequests.erase(current->second.requestId);
                nextApps.erase(route.app);
            }
            if(!nextApps.emplace(route.app,route).second ||
               !nextRequests.emplace(route.requestId,route.app).second)
                return AsyncRouteAdmission::Invalid;
            byApp_.swap(nextApps);
            byRequest_.swap(nextRequests);
            retired.swap(events);
        } catch(...) { return AsyncRouteAdmission::Invalid; }
        requestHighWater_=route.requestId;
        return AsyncRouteAdmission::Accepted;
    }

    bool retire(uint64_t requestId,uint64_t operationId,
                uint64_t identityGeneration,AsyncRetirementReason reason,
                std::vector<AsyncSessionRetirement>& retired){
        retired.clear();
        const auto request=byRequest_.find(requestId);
        if(request==byRequest_.end()) return false;
        const auto route=byApp_.find(request->second);
        if(route==byApp_.end() || route->second.requestId!=requestId ||
           route->second.operationId!=operationId ||
           route->second.identityGeneration!=identityGeneration) return false;
        try {
            std::vector<AsyncSessionRetirement> event;
            event.push_back(retirement(route->second,reason));
            retired.swap(event);
        } catch(...) { return false; }
        byRequest_.erase(request);
        byApp_.erase(route);
        return true;
    }

    bool abandon(uint64_t requestId,uint64_t operationId,
                 uint64_t identityGeneration) noexcept {
        const auto request=byRequest_.find(requestId);
        if(request==byRequest_.end()) return false;
        const auto route=byApp_.find(request->second);
        if(route==byApp_.end() || route->second.requestId!=requestId ||
           route->second.operationId!=operationId ||
           route->second.identityGeneration!=identityGeneration) return false;
        byRequest_.erase(request);
        byApp_.erase(route);
        return true;
    }

    size_t expire(uint64_t nowMs,
                  std::vector<AsyncSessionRetirement>& retired){
        retired.clear();
        std::vector<std::string> apps;
        std::vector<AsyncSessionRetirement> events;
        try {
            apps.reserve(byApp_.size());
            events.reserve(byApp_.size());
            for(const auto& item : byApp_){
                if(nowMs<item.second.deadlineMs) continue;
                apps.push_back(item.first);
                events.push_back(retirement(
                    item.second,AsyncRetirementReason::TimedOut));
            }
        } catch(...) { return 0; }
        for(const std::string& app : apps){
            const auto route=byApp_.find(app);
            if(route==byApp_.end()) continue;
            byRequest_.erase(route->second.requestId);
            byApp_.erase(route);
        }
        retired.swap(events);
        return retired.size();
    }

    size_t cancelOperation(SessionPurpose purpose,uint64_t operationId,
                           std::vector<AsyncSessionRetirement>& retired){
        retired.clear();
        if(operationId==0 || !validPurpose(purpose)) return 0;
        std::vector<std::string> apps;
        std::vector<AsyncSessionRetirement> events;
        try {
            apps.reserve(byApp_.size());
            events.reserve(byApp_.size());
            for(const auto& item : byApp_){
                if(item.second.operationId!=operationId ||
                   item.second.purpose!=purpose) continue;
                apps.push_back(item.first);
                events.push_back(retirement(
                    item.second,AsyncRetirementReason::Cancelled));
            }
        } catch(...) { return 0; }
        for(const std::string& app : apps){
            const auto route=byApp_.find(app);
            if(route==byApp_.end()) continue;
            byRequest_.erase(route->second.requestId);
            byApp_.erase(route);
        }
        retired.swap(events);
        return retired.size();
    }

    size_t outstanding() const noexcept { return byApp_.size(); }
};

class AsyncReconcileDeadlineGate {
    static const uint64_t kMaxLifetimeMs=60000;
    static const size_t kMaxOperations=32;
    static const size_t kMaxPendingPerOperation=4096;
    struct Entry {
        uint64_t dueAtMs=0;
        size_t pending=0;
    };
    std::map<uint64_t,Entry> entries_;

public:
    static uint64_t maxLifetimeMs() noexcept { return kMaxLifetimeMs; }

    bool begin(uint64_t operationId,uint64_t nowMs){
        if(operationId==0) return false;
        auto found=entries_.find(operationId);
        if(found!=entries_.end()){
            if(found->second.pending>=kMaxPendingPerOperation) return false;
            ++found->second.pending;
            return true;
        }
        if(entries_.size()>=kMaxOperations) return false;
        Entry entry;
        entry.dueAtMs=nowMs>(std::numeric_limits<uint64_t>::max)()-
                kMaxLifetimeMs
            ? (std::numeric_limits<uint64_t>::max)()
            : nowMs+kMaxLifetimeMs;
        entry.pending=1;
        try { return entries_.emplace(operationId,entry).second; }
        catch(...) { return false; }
    }

    bool complete(uint64_t operationId) noexcept {
        auto found=entries_.find(operationId);
        if(found==entries_.end() || found->second.pending==0) return false;
        --found->second.pending;
        if(found->second.pending==0) entries_.erase(found);
        return true;
    }

    bool cancel(uint64_t operationId) noexcept {
        return entries_.erase(operationId)!=0;
    }

    size_t expire(uint64_t nowMs,std::vector<uint64_t>& expired){
        expired.clear();
        std::vector<uint64_t> ready;
        try {
            ready.reserve(entries_.size());
            for(const auto& item : entries_)
                if(nowMs>=item.second.dueAtMs) ready.push_back(item.first);
        } catch(...) { return 0; }
        for(uint64_t operationId : ready) entries_.erase(operationId);
        expired.swap(ready);
        return expired.size();
    }

    size_t pending(uint64_t operationId) const noexcept {
        auto found=entries_.find(operationId);
        return found==entries_.end() ? 0 : found->second.pending;
    }

    uint64_t dueAt(uint64_t operationId) const noexcept {
        auto found=entries_.find(operationId);
        return found==entries_.end() ? 0 : found->second.dueAtMs;
    }

    bool empty() const noexcept { return entries_.empty(); }
    size_t size() const noexcept { return entries_.size(); }
};

enum class DirtyFlushResult {
    NotDirty, Deferred, ConflictSuppressed, Succeeded,
    SucceededDirtyAgain, Failed, Cleared
};

class DirtyFlushController {
    static const uint64_t kFlushDelayMs=500;
    static const uint64_t kErrorRepeatMs=5ULL*60ULL*1000ULL;
    bool dirty_=false;
    bool conflict_=false;
    bool flushing_=false;
    bool retryBlocked_=false;
    uint64_t dueAtMs_=0;
    uint64_t retryBlockedAtMs_=0;
    uint64_t stateGeneration_=0;
    bool errorReported_=false;
    std::string lastErrorKey_;
    uint64_t lastErrorAtMs_=0;
    uint64_t lastErrorGeneration_=0;

    static uint64_t addDelay(uint64_t nowMs,uint64_t delay) noexcept {
        return nowMs>(std::numeric_limits<uint64_t>::max)()-delay
            ? (std::numeric_limits<uint64_t>::max)() : nowMs+delay;
    }

    void changed() noexcept {
        if(stateGeneration_==(std::numeric_limits<uint64_t>::max)()){
            stateGeneration_=1;
            errorReported_=false;
        } else {
            ++stateGeneration_;
            if(stateGeneration_==0) stateGeneration_=1;
        }
    }

public:
    void markDirty(uint64_t nowMs) noexcept {
        changed();
        const uint64_t deadline=addDelay(nowMs,kFlushDelayMs);
        if(!dirty_ || flushing_ || retryBlocked_){
            dirty_=true;
            dueAtMs_=deadline;
        }
        retryBlocked_=false;
        retryBlockedAtMs_=0;
    }

    void setConflict(bool conflict,uint64_t nowMs) noexcept {
        if(conflict_==conflict) return;
        conflict_=conflict;
        changed();
        if(!conflict_ && dirty_ && dueAtMs_<=nowMs) dueAtMs_=nowMs;
    }

    DirtyFlushResult flush(uint64_t nowMs,bool force,
                           const std::function<bool()>& writer){
        if(flushing_) return DirtyFlushResult::Deferred;
        if(!dirty_) return DirtyFlushResult::NotDirty;
        if(conflict_ && !force) return DirtyFlushResult::ConflictSuppressed;
        if(retryBlocked_ && !force){
            if(nowMs>=retryBlockedAtMs_)
                return DirtyFlushResult::Deferred;
            retryBlocked_=false;
            retryBlockedAtMs_=0;
            dueAtMs_=addDelay(nowMs,kFlushDelayMs);
            return DirtyFlushResult::Deferred;
        }
        if(!force && nowMs<dueAtMs_)
            return DirtyFlushResult::Deferred;
        const uint64_t startedGeneration=stateGeneration_;
        flushing_=true;
        bool saved=false;
        try { saved=writer && writer(); }
        catch(...) { saved=false; }
        flushing_=false;
        if(stateGeneration_!=startedGeneration && !dirty_){
            retryBlocked_=false;
            retryBlockedAtMs_=0;
            dueAtMs_=0;
            return saved ? DirtyFlushResult::Succeeded
                         : DirtyFlushResult::Cleared;
        }
        if(!saved){
            dirty_=true;
            dueAtMs_=addDelay(nowMs,kFlushDelayMs);
            retryBlocked_=nowMs>
                (std::numeric_limits<uint64_t>::max)()-kFlushDelayMs;
            retryBlockedAtMs_=retryBlocked_ ? nowMs : 0;
            return DirtyFlushResult::Failed;
        }
        if(conflict_) conflict_=false;
        if(stateGeneration_!=startedGeneration){
            if(dirty_) return DirtyFlushResult::SucceededDirtyAgain;
            dueAtMs_=0;
            return DirtyFlushResult::Succeeded;
        }
        dirty_=false;
        retryBlocked_=false;
        retryBlockedAtMs_=0;
        dueAtMs_=0;
        return DirtyFlushResult::Succeeded;
    }

    bool shouldReportError(const std::string& key,uint64_t nowMs){
        if(key.empty()) return false;
        const bool stateChanged=lastErrorGeneration_!=stateGeneration_;
        if(errorReported_ && key==lastErrorKey_ && !stateChanged){
            if(nowMs<lastErrorAtMs_){
                lastErrorAtMs_=nowMs;
                return false;
            }
            if(nowMs-lastErrorAtMs_<kErrorRepeatMs) return false;
        }
        try { lastErrorKey_=key; }
        catch(...) { return true; }
        errorReported_=true;
        lastErrorAtMs_=nowMs;
        lastErrorGeneration_=stateGeneration_;
        return true;
    }

    void clearDirty() noexcept {
        if(dirty_ || conflict_ || flushing_) changed();
        dirty_=false;
        conflict_=false;
        retryBlocked_=false;
        retryBlockedAtMs_=0;
        dueAtMs_=0;
    }

    bool dirty() const noexcept { return dirty_; }
    bool conflicted() const noexcept { return conflict_; }
    uint64_t dueAtMs() const noexcept { return dueAtMs_; }
    uint64_t stateGeneration() const noexcept { return stateGeneration_; }
};

inline bool ArmMoveWorkOrCancel(bool hasWork,
                                const std::function<bool()>& arm,
                                const std::function<void()>& cancelAccepted){
    if(!hasWork) return true;
    bool armed=false;
    try { armed=arm && arm(); }
    catch(...) { armed=false; }
    if(armed) return true;
    try { if(cancelAccepted) cancelAccepted(); }
    catch(...) {}
    return false;
}

inline bool ShouldMaintainDirtyFlush(bool eligible,bool dirty,
                                     bool timerArmed) noexcept {
    return eligible && dirty && !timerArmed;
}

enum class MoveArmFailureCleanup { Completed, Rearmed, Unresolved };

class MoveCancellationRetryState {
public:
    enum : unsigned { kMaxConsecutiveNoProgress=8 };

    template<class ArmTimer,class PostOne>
    bool request(bool pending,ArmTimer armTimer,PostOne postOne) noexcept {
        if(!pending){ clear(); return true; }
        bool armed=false;
        try { armed=armTimer(); } catch(...) { armed=false; }
        if(armed){ clear(); return true; }
        if(postOutstanding_) return true;
        if(consecutiveNoProgress_>=kMaxConsecutiveNoProgress) return false;
        bool posted=false;
        try { posted=postOne(); } catch(...) { posted=false; }
        if(posted) postOutstanding_=true;
        return posted;
    }

    void completePostedAttempt(size_t before,size_t after) noexcept {
        postOutstanding_=false;
        if(after<before) consecutiveNoProgress_=0;
        else if(consecutiveNoProgress_!=
                (std::numeric_limits<unsigned>::max)())
            ++consecutiveNoProgress_;
    }

    void clear() noexcept {
        postOutstanding_=false;
        consecutiveNoProgress_=0;
    }

    bool postOutstanding() const noexcept { return postOutstanding_; }
    unsigned consecutiveNoProgress() const noexcept {
        return consecutiveNoProgress_;
    }

private:
    bool postOutstanding_=false;
    unsigned consecutiveNoProgress_=0;
};

inline MoveArmFailureCleanup RecoverMoveArmFailure(
        const std::function<bool()>& cancelOne,
        const std::function<bool()>& rearm){
    try {
        if(cancelOne && cancelOne()) return MoveArmFailureCleanup::Completed;
    } catch(...) {
        // Queue mutation is transactional; retain ownership and schedule a
        // later cancellation attempt instead of issuing the stale move.
    }
    try {
        if(rearm && rearm()) return MoveArmFailureCleanup::Rearmed;
    } catch(...) {}
    return MoveArmFailureCleanup::Unresolved;
}

template<class Setup,class Rollback>
inline bool RunFailureAtomicMoveSetup(Setup setup,Rollback rollback){
    bool completed=false;
    try { completed=setup(); }
    catch(...) { completed=false; }
    if(completed) return true;
    try { rollback(); }
    catch(...) {}
    return false;
}

template<class StageSuccessor,class PublishGuard,class CancelDisplaced,
         class RollbackSuccessor>
inline bool RunSuccessorFirstReservationHandoff(
        StageSuccessor stageSuccessor,PublishGuard publishGuard,
        CancelDisplaced cancelDisplaced,
        RollbackSuccessor rollbackSuccessor) noexcept {
    static_assert(noexcept(publishGuard()),
        "reservation handoff guard publication must be no-throw");
    bool staged=false;
    try { staged=stageSuccessor(); }
    catch(...) { staged=false; }
    if(!staged){
        try { rollbackSuccessor(); }
        catch(...) {}
        return false;
    }

    // This is the point of no return: all successor owner/runtime/queue state
    // is already visible, so the inherited guard can be swapped without a
    // reservation-free interval.  Cancellation is intentionally last.
    publishGuard();
    try { cancelDisplaced(); }
    catch(...) {
        // The successor remains committed and keeps the stable origin guard.
        // Production cancellation retains retry state for the displaced job.
    }
    return true;
}

inline bool BindReservationToProvisionalOrigin(
        const LayoutWin& origin,std::string& reservationRecordId) noexcept {
    try {
        std::string canonical;
        GUID parsed{};
        if(!ParseNonzeroLayoutGuid(origin.recordId,parsed,&canonical) ||
           !IsSupportedLayoutApp(origin.app) || GuidIsZero(origin.desktop) ||
           origin.lastSeenUtc<=0) return false;
        reservationRecordId.swap(canonical);
        return true;
    } catch(...) { return false; }
}

inline bool RunIdentityGuardedComCall(
        const std::function<bool()>& recaptureIdentity,
        const std::function<HRESULT()>& action,HRESULT& result){
    bool current=false;
    try { current=recaptureIdentity && recaptureIdentity(); }
    catch(...) { current=false; }
    if(!current) return false;
    HRESULT completed=E_FAIL;
    try { completed=action ? action() : E_POINTER; }
    catch(...) { completed=E_FAIL; }
    result=completed;
    return true;
}

inline bool DesktopServicesReady(bool internalManager,
                                 bool applicationViews,
                                 bool documentedManager) noexcept {
    return internalManager && applicationViews && documentedManager;
}

template<class Release>
inline bool ValidateComOutPointerOrRelease(
        HRESULT result,bool pointerPresent,Release release) noexcept {
    if(SUCCEEDED(result) && pointerPresent) return true;
    if(pointerPresent){
        try { release(); }
        catch(...) {}
    }
    return false;
}

enum class DesktopCollectionLookupKind { Index, Guid };
static const UINT MAX_VIRTUAL_DESKTOPS=64U;

struct DesktopCollectionLookupRequest {
    DesktopCollectionLookupKind kind=DesktopCollectionLookupKind::Index;
    UINT index=0;
    GUID guid={0};

    static DesktopCollectionLookupRequest ByIndex(UINT requested) noexcept {
        DesktopCollectionLookupRequest result;
        result.kind=DesktopCollectionLookupKind::Index;
        result.index=requested;
        return result;
    }

    static DesktopCollectionLookupRequest ByGuid(
            const GUID& requested) noexcept {
        DesktopCollectionLookupRequest result;
        result.kind=DesktopCollectionLookupKind::Guid;
        result.guid=requested;
        return result;
    }
};

template<class Array,class Ops>
class DesktopCollectionArrayOutputOwner {
public:
    DesktopCollectionArrayOutputOwner(Array*& value,Ops& ops) noexcept
        :value_(value),ops_(ops){}
    ~DesktopCollectionArrayOutputOwner() noexcept { reset(); }
    DesktopCollectionArrayOutputOwner(
        const DesktopCollectionArrayOutputOwner&)=delete;
    DesktopCollectionArrayOutputOwner& operator=(
        const DesktopCollectionArrayOutputOwner&)=delete;

private:
    void reset() noexcept {
        if(!value_) return;
        Array* value=value_;
        value_=nullptr;
        try { ops_.releaseArray(value); }
        catch(...) {}
    }

    Array*& value_;
    Ops& ops_;
};

template<class Desktop,class Ops>
class DesktopCollectionItemOutputOwner {
public:
    DesktopCollectionItemOutputOwner(Desktop*& value,Ops& ops) noexcept
        :value_(value),ops_(ops){}
    ~DesktopCollectionItemOutputOwner() noexcept { reset(); }
    DesktopCollectionItemOutputOwner(
        const DesktopCollectionItemOutputOwner&)=delete;
    DesktopCollectionItemOutputOwner& operator=(
        const DesktopCollectionItemOutputOwner&)=delete;

    Desktop* release() noexcept {
        Desktop* value=value_;
        value_=nullptr;
        return value;
    }

private:
    void reset() noexcept {
        if(!value_) return;
        Desktop* value=value_;
        value_=nullptr;
        try { ops_.releaseDesktop(value); }
        catch(...) {}
    }

    Desktop*& value_;
    Ops& ops_;
};

struct DesktopCollectionEntry {
    UINT index;
    GUID guid;
};

enum class DesktopCollectionSnapshotObservationStage {
    GetDesktops, GetCount, InvalidCount, GetAt, GetId,
    InvalidGuid, Complete, AllocationFailure, Exception
};

struct DesktopCollectionSnapshotObservation {
    DesktopCollectionSnapshotObservationStage stage=
        DesktopCollectionSnapshotObservationStage::GetDesktops;
    HRESULT result=E_NOTIMPL;
    UINT count=0;
    int index=-1;
    GUID actual{};
};

inline void ObserveDesktopCollectionSnapshot(
        const std::function<void(
            const DesktopCollectionSnapshotObservation&)>* observer,
        const DesktopCollectionSnapshotObservation& observation) noexcept {
    if(!observer) return;
    try { (*observer)(observation); }
    catch(...) {}
}

template<class Array,class Desktop,class Ops>
inline bool SnapshotDesktopCollectionOwned(
        Ops& ops,std::vector<DesktopCollectionEntry>& snapshotOut,
        const std::function<void(
            const DesktopCollectionSnapshotObservation&)>* observer) noexcept {
    DesktopCollectionSnapshotObservation observation;
    try {
        Array* array=nullptr;
        DesktopCollectionArrayOutputOwner<Array,Ops> arrayOwner(array,ops);
        observation=DesktopCollectionSnapshotObservation{};
        observation.stage=DesktopCollectionSnapshotObservationStage::GetDesktops;
        const HRESULT desktopsResult=ops.getDesktops(&array);
        observation.result=desktopsResult;
        ObserveDesktopCollectionSnapshot(observer,observation);
        if(FAILED(desktopsResult) || !array) return false;

        UINT count=0;
        observation=DesktopCollectionSnapshotObservation{};
        observation.stage=DesktopCollectionSnapshotObservationStage::GetCount;
        observation.result=E_NOTIMPL;
        const HRESULT countResult=ops.getCount(array,&count);
        observation.result=countResult;
        observation.count=count;
        ObserveDesktopCollectionSnapshot(observer,observation);
        if(FAILED(countResult)) return false;
        if(count==0 || count>MAX_VIRTUAL_DESKTOPS){
            observation.stage=
                DesktopCollectionSnapshotObservationStage::InvalidCount;
            ObserveDesktopCollectionSnapshot(observer,observation);
            return false;
        }
        std::vector<DesktopCollectionEntry> staged;
        staged.reserve(count);
        for(UINT i=0;i<count;++i){
            Desktop* desktop=nullptr;
            DesktopCollectionItemOutputOwner<Desktop,Ops> desktopOwner(
                desktop,ops);
            observation=DesktopCollectionSnapshotObservation{};
            observation.stage=DesktopCollectionSnapshotObservationStage::GetAt;
            observation.result=E_NOTIMPL;
            observation.count=count;
            observation.index=static_cast<int>(i);
            const HRESULT atResult=ops.getAt(array,i,&desktop);
            observation.result=atResult;
            ObserveDesktopCollectionSnapshot(observer,observation);
            if(FAILED(atResult) || !desktop) return false;
            GUID id={0};
            observation.stage=DesktopCollectionSnapshotObservationStage::GetId;
            observation.result=E_NOTIMPL;
            observation.actual=GUID{};
            const HRESULT idResult=ops.getId(desktop,&id);
            observation.result=idResult;
            observation.actual=id;
            ObserveDesktopCollectionSnapshot(observer,observation);
            if(FAILED(idResult)) return false;
            if(GuidIsZero(id)){
                observation.stage=
                    DesktopCollectionSnapshotObservationStage::InvalidGuid;
                ObserveDesktopCollectionSnapshot(observer,observation);
                return false;
            }
            DesktopCollectionEntry entry={i,id};
            staged.push_back(entry);
        }
        snapshotOut.swap(staged);
        observation.stage=DesktopCollectionSnapshotObservationStage::Complete;
        observation.index=-1;
        observation.actual=GUID{};
        observation.count=static_cast<UINT>(snapshotOut.size());
        ObserveDesktopCollectionSnapshot(observer,observation);
        return true;
    } catch(const std::bad_alloc&) {
        observation.stage=
            DesktopCollectionSnapshotObservationStage::AllocationFailure;
        ObserveDesktopCollectionSnapshot(observer,observation);
        return false;
    } catch(...) {
        observation.stage=DesktopCollectionSnapshotObservationStage::Exception;
        ObserveDesktopCollectionSnapshot(observer,observation);
        return false;
    }
}

template<class Array,class Desktop,class Ops>
inline bool SnapshotDesktopCollectionOwned(
        Ops& ops,std::vector<DesktopCollectionEntry>& snapshotOut) noexcept {
    return SnapshotDesktopCollectionOwned<Array,Desktop>(
        ops,snapshotOut,nullptr);
}

enum class DesktopCollectionLookupObservationStage {
    ValidateRequest,GetDesktops,GetCount,GetAt,GetId,
    Match,NotFound,Exception
};

struct DesktopCollectionLookupObservation {
    DesktopCollectionLookupObservationStage stage=
        DesktopCollectionLookupObservationStage::ValidateRequest;
    int index=-1;
    HRESULT result=E_NOTIMPL;
    GUID actual{};
    bool matched=false;
};

inline void ObserveDesktopCollectionLookup(
        const std::function<void(
            const DesktopCollectionLookupObservation&)>* observer,
        const DesktopCollectionLookupObservation& observation) noexcept {
    if(!observer) return;
    try { (*observer)(observation); }
    catch(...) {}
}

template<class Array,class Desktop,class Ops,class DesktopOwner>
inline bool LookupDesktopCollectionOwned(
        const DesktopCollectionLookupRequest& request,Ops& ops,
        DesktopOwner& desktopOut,int& indexOut,
        const std::function<void(
            const DesktopCollectionLookupObservation&)>* observer) noexcept {
    static_assert(noexcept(desktopOut.reset()),
                  "desktop output owner reset must be noexcept");
    static_assert(noexcept(desktopOut.reset(static_cast<Desktop*>(nullptr))),
                  "desktop output owner adoption must be noexcept");
    desktopOut.reset();
    indexOut=-1;
    DesktopCollectionLookupObservation observation;
    observation.index=request.kind==DesktopCollectionLookupKind::Index
        ? static_cast<int>(request.index) : -1;
    observation.result=
        request.kind==DesktopCollectionLookupKind::Guid &&
        GuidIsZero(request.guid)
            ? E_INVALIDARG : S_OK;
    ObserveDesktopCollectionLookup(observer,observation);
    if(request.kind==DesktopCollectionLookupKind::Guid &&
       GuidIsZero(request.guid)) return false;

    try {
        Array* array=nullptr;
        DesktopCollectionArrayOutputOwner<Array,Ops> arrayOwner(array,ops);
        observation=DesktopCollectionLookupObservation{};
        observation.stage=DesktopCollectionLookupObservationStage::GetDesktops;
        const HRESULT desktopsResult=ops.getDesktops(&array);
        observation.result=desktopsResult;
        ObserveDesktopCollectionLookup(observer,observation);
        if(FAILED(desktopsResult) || !array) return false;

        UINT count=0;
        observation=DesktopCollectionLookupObservation{};
        observation.stage=DesktopCollectionLookupObservationStage::GetCount;
        const HRESULT countResult=ops.getCount(array,&count);
        observation.result=countResult;
        ObserveDesktopCollectionLookup(observer,observation);
        if(FAILED(countResult) || count==0 ||
           count>MAX_VIRTUAL_DESKTOPS) return false;
        if(request.kind==DesktopCollectionLookupKind::Index &&
           request.index>=count){
            observation.stage=
                DesktopCollectionLookupObservationStage::NotFound;
            observation.index=static_cast<int>(request.index);
            observation.result=S_FALSE;
            ObserveDesktopCollectionLookup(observer,observation);
            return false;
        }
        if(request.kind!=DesktopCollectionLookupKind::Index &&
           request.kind!=DesktopCollectionLookupKind::Guid){
            observation.stage=
                DesktopCollectionLookupObservationStage::ValidateRequest;
            observation.index=-1;
            observation.result=E_INVALIDARG;
            ObserveDesktopCollectionLookup(observer,observation);
            return false;
        }

        const UINT begin=request.kind==DesktopCollectionLookupKind::Index
            ? request.index : 0U;
        const UINT end=request.kind==DesktopCollectionLookupKind::Index
            ? request.index+1U : count;
        for(UINT i=begin;i<end;++i){
            Desktop* desktop=nullptr;
            DesktopCollectionItemOutputOwner<Desktop,Ops> desktopOwner(
                desktop,ops);
            observation=DesktopCollectionLookupObservation{};
            observation.stage=DesktopCollectionLookupObservationStage::GetAt;
            observation.index=static_cast<int>(i);
            const HRESULT atResult=ops.getAt(array,i,&desktop);
            observation.result=atResult;
            ObserveDesktopCollectionLookup(observer,observation);
            if(FAILED(atResult) || !desktop) return false;
            GUID id={0};
            observation=DesktopCollectionLookupObservation{};
            observation.stage=DesktopCollectionLookupObservationStage::GetId;
            observation.index=static_cast<int>(i);
            const HRESULT idResult=ops.getId(desktop,&id);
            observation.result=idResult;
            observation.actual=id;
            ObserveDesktopCollectionLookup(observer,observation);
            if(FAILED(idResult) || GuidIsZero(id)) return false;
            const bool matches=
                request.kind==DesktopCollectionLookupKind::Index ||
                IsEqualGUID(id,request.guid);
            if(!matches) continue;
            desktopOut.reset(desktop);
            desktopOwner.release();
            indexOut=static_cast<int>(i);
            observation.stage=DesktopCollectionLookupObservationStage::Match;
            observation.result=S_OK;
            observation.matched=true;
            ObserveDesktopCollectionLookup(observer,observation);
            return true;
        }
        observation.stage=DesktopCollectionLookupObservationStage::NotFound;
        observation.result=S_FALSE;
        observation.matched=false;
        ObserveDesktopCollectionLookup(observer,observation);
        return false;
    } catch(...) {
        observation.stage=DesktopCollectionLookupObservationStage::Exception;
        observation.result=E_FAIL;
        observation.matched=false;
        ObserveDesktopCollectionLookup(observer,observation);
        return false;
    }
}

template<class Array,class Desktop,class Ops,class DesktopOwner>
inline bool LookupDesktopCollectionOwned(
        const DesktopCollectionLookupRequest& request,Ops& ops,
        DesktopOwner& desktopOut,int& indexOut) noexcept {
    return LookupDesktopCollectionOwned<Array,Desktop>(
        request,ops,desktopOut,indexOut,nullptr);
}

template<class Initialize,class Sanity,class Release>
inline bool InitializeServicesWithRollback(Initialize initialize,
                                           Sanity sanity,Release release){
    bool ready=false;
    try { ready=initialize() && sanity(); }
    catch(...) { ready=false; }
    if(ready) return true;
    try { release(); }
    catch(...) {}
    return false;
}

struct RestoreRequest {
    size_t savedIndex = 0;
    size_t liveIndex = 0;
    GUID destination = {0};
};

struct NewRecordRequest {
    size_t liveIndex = 0;
    std::string recordId;
};

enum class ReconcileFreshness { Fresh, CachedStale };

template<class Commit>
inline bool CommitBoundRecordRefresh(
        const LayoutWin& existing,const FastWin& fast,const LayoutWin& live,
        ReconcileFreshness freshness,UnixSeconds nowUtc,
        const std::string& runtimeKey,
        std::map<std::string,std::string>& provisionalByRuntime,
        Commit&& commit) noexcept {
    if(nowUtc<=0 || runtimeKey.empty() || fast.app!=existing.app ||
       live.app!=existing.app) return false;
    try {
        const std::string existingRecordId=existing.recordId;
        LayoutWin desired=existing;
        if(!GuidIsZero(fast.desktop)){
            desired.desktop=fast.desktop;
            desired.deskIndex=live.deskIndex;
        }
        if(freshness==ReconcileFreshness::Fresh){
            desired.activeTitle=live.activeTitle;
            desired.activeDomain=live.activeDomain;
            desired.tabCount=live.tabCount;
            desired.counts=live.counts;
            desired.provisional=false;
        }
        MarkSeen(desired,nowUtc);
        if(!commit(desired)) return false;
        if(freshness==ReconcileFreshness::Fresh){
            const auto provisional=provisionalByRuntime.find(runtimeKey);
            if(provisional!=provisionalByRuntime.end() &&
               provisional->second==existingRecordId)
                provisionalByRuntime.erase(provisional);
        }
        return true;
    } catch(...) { return false; }
}

struct ReconcilePlan {
    std::string app;
    UnixSeconds nowUtc = 0;
    ReconcileFreshness freshness = ReconcileFreshness::Fresh;
    bool deferred = false;
    bool tooComplex = false;
    std::vector<LayoutMatch> matches;
    std::vector<RestoreRequest> restores;
    std::vector<NewRecordRequest> newRecords;
    std::vector<size_t> missingSavedIndices;
};

using ReconcileMatcher = std::vector<LayoutMatch> (*)(
    const std::vector<LayoutWin>&,
    const std::vector<LayoutWin>&,
    double,
    bool*);

inline bool IsExpiredAfterProjectedMarkMissing(
        const LayoutWin& record, UnixSeconds nowUtc){
    const UnixSeconds missingSinceUtc=record.missingSinceUtc!=0
        ? record.missingSinceUtc
        : (record.lastSeenUtc>0 ? record.lastSeenUtc : nowUtc);
    return missingSinceUtc>0 && nowUtc>=missingSinceUtc &&
        nowUtc-missingSinceUtc>=WINDOW_RETENTION_SECONDS;
}

inline bool ProjectedRetainedExistingCount(
        const std::vector<LayoutWin>& existing,
        const std::vector<bool>& markMissing,
        UnixSeconds nowUtc,
        size_t& retainedOut){
    if(markMissing.size()!=existing.size()) return false;
    size_t retained=0;
    for(size_t i=0;i<existing.size();++i){
        const bool expired=markMissing[i]
            ? IsExpiredAfterProjectedMarkMissing(existing[i],nowUtc)
            : IsExpired(existing[i],nowUtc);
        if(!expired) ++retained;
    }
    retainedOut=retained;
    return true;
}

inline ReconcilePlan PlanAppReconcile(
        const std::vector<LayoutWin>& existing,
        const std::vector<LayoutWin>& live,
        const std::string& app,
        UnixSeconds nowUtc,
        const std::set<std::string>& reservedRecordIds={},
        ReconcileFreshness freshness=ReconcileFreshness::Fresh,
        RecordIdGenerator idGenerator=NewRecordId,
        ReconcileMatcher matcher=MatchOneToOne){
    ReconcilePlan plan;
    plan.app=app;
    plan.nowUtc=nowUtc;
    plan.freshness=freshness;
    auto deferredPlan=[&](bool tooComplex=false){
        ReconcilePlan deferred;
        deferred.app=app;
        deferred.nowUtc=nowUtc;
        deferred.freshness=freshness;
        deferred.deferred=true;
        deferred.tooComplex=tooComplex;
        return deferred;
    };
    if(!IsSupportedLayoutApp(app) || nowUtc<=0 ||
            (freshness!=ReconcileFreshness::Fresh &&
             freshness!=ReconcileFreshness::CachedStale) ||
            existing.size()>MAX_LAYOUT_RECORDS || live.size()>MAX_LAYOUT_RECORDS ||
            reservedRecordIds.size()>MAX_LAYOUT_RECORDS || !matcher)
        return deferredPlan();

    std::set<std::string> occupiedRecordIds;
    std::vector<std::string> existingRecordIds;
    existingRecordIds.reserve(existing.size());
    for(const LayoutWin& record : existing){
        GUID id{};
        std::string canonicalId;
        if(!ParseNonzeroLayoutGuid(record.recordId,id,&canonicalId) ||
                !occupiedRecordIds.insert(canonicalId).second)
            return deferredPlan();
        existingRecordIds.push_back(canonicalId);
    }
    std::set<std::string> reservedRecordIdKeys;
    for(const std::string& recordId : reservedRecordIds){
        GUID id{};
        std::string canonicalId;
        if(!ParseNonzeroLayoutGuid(recordId,id,&canonicalId)) return deferredPlan();
        occupiedRecordIds.insert(canonicalId);
        reservedRecordIdKeys.insert(canonicalId);
    }
    auto isReserved=[&](size_t index){
        return reservedRecordIdKeys.count(existingRecordIds[index])!=0;
    };

    std::vector<LayoutWin> eligible;
    std::vector<size_t> originalIndices;
    for(size_t i=0;i<existing.size();++i){
        if(existing[i].app!=app || IsExpired(existing[i],nowUtc) ||
                isReserved(i)) continue;
        eligible.push_back(existing[i]);
        originalIndices.push_back(i);
    }

    size_t fixedRetained=0;
    for(size_t i=0;i<existing.size();++i)
        if(!IsExpired(existing[i],nowUtc) &&
                (existing[i].app!=app || isReserved(i)))
            ++fixedRetained;
    size_t liveAppCount=0;
    for(const LayoutWin& record : live)
        if(record.app==app) ++liveAppCount;
    if(freshness==ReconcileFreshness::Fresh &&
            (fixedRetained>MAX_LAYOUT_RECORDS ||
             liveAppCount>MAX_LAYOUT_RECORDS-fixedRetained))
        return deferredPlan();

    const double acceptScore=0.55;
    bool tooComplex=false;
    std::vector<LayoutMatch> assigned=matcher(eligible,live,acceptScore,&tooComplex);
    if(tooComplex) return deferredPlan(true);
    if(assigned.size()>MAX_LAYOUT_RECORDS || assigned.size()>eligible.size() ||
            assigned.size()>live.size())
        return deferredPlan();
    std::vector<bool> assignedSaved(eligible.size(),false);
    std::vector<bool> assignedLive(live.size(),false);
    for(const LayoutMatch& match : assigned){
        if(match.savedIndex>=eligible.size() || match.liveIndex>=live.size() ||
                live[match.liveIndex].app!=app || !std::isfinite(match.score) ||
                match.score<acceptScore || assignedSaved[match.savedIndex] ||
                assignedLive[match.liveIndex])
            return deferredPlan();
        assignedSaved[match.savedIndex]=true;
        assignedLive[match.liveIndex]=true;
    }

    std::vector<bool> matchedSaved(existing.size(),false);
    std::vector<bool> matchedLive(live.size(),false);
    for(const LayoutMatch& assignedMatch : assigned){
        LayoutMatch match=assignedMatch;
        match.savedIndex=originalIndices[assignedMatch.savedIndex];
        plan.matches.push_back(match);
        matchedSaved[match.savedIndex]=true;
        matchedLive[match.liveIndex]=true;
        if(!GuidEq(existing[match.savedIndex].desktop,live[match.liveIndex].desktop)){
            RestoreRequest restore;
            restore.savedIndex=match.savedIndex;
            restore.liveIndex=match.liveIndex;
            restore.destination=existing[match.savedIndex].desktop;
            plan.restores.push_back(restore);
        }
    }

    if(freshness==ReconcileFreshness::Fresh){
        std::vector<size_t> unmatchedProvisionals;
        std::vector<size_t> unmatchedLive;
        for(size_t i=0;i<existing.size();++i)
            if(existing[i].app==app && existing[i].provisional &&
               !IsExpired(existing[i],nowUtc) && !isReserved(i) &&
               !matchedSaved[i])
                unmatchedProvisionals.push_back(i);
        for(size_t i=0;i<live.size();++i)
            if(live[i].app==app && !matchedLive[i])
                unmatchedLive.push_back(i);
        if(!unmatchedProvisionals.empty() && !unmatchedLive.empty()){
            bool unresolvedEstablished=false;
            for(size_t i=0;i<existing.size();++i)
                if(existing[i].app==app && !existing[i].provisional &&
                   !IsExpired(existing[i],nowUtc) && !isReserved(i) &&
                   !matchedSaved[i])
                    unresolvedEstablished=true;

            std::vector<std::pair<size_t,size_t> > adoptions;
            if(unmatchedProvisionals.size()==1 && unmatchedLive.size()==1){
                adoptions.push_back(std::make_pair(
                    unmatchedProvisionals[0],unmatchedLive[0]));
            } else {
                std::map<std::string,size_t> provisionalTitleCounts;
                std::map<std::string,size_t> liveTitleCounts;
                std::map<std::string,size_t> liveByTitle;
                for(size_t index : unmatchedProvisionals){
                    const std::string title=NormalizeProvisionalAdoptionTitle(
                        existing[index].activeTitle);
                    if(!title.empty()) ++provisionalTitleCounts[title];
                }
                for(size_t index : unmatchedLive){
                    const std::string title=NormalizeProvisionalAdoptionTitle(
                        live[index].activeTitle);
                    if(!title.empty()){
                        ++liveTitleCounts[title];
                        liveByTitle[title]=index;
                    }
                }
                for(size_t index : unmatchedProvisionals){
                    const std::string title=NormalizeProvisionalAdoptionTitle(
                        existing[index].activeTitle);
                    if(!title.empty() && provisionalTitleCounts[title]==1 &&
                       liveTitleCounts[title]==1)
                        adoptions.push_back(std::make_pair(
                            index,liveByTitle[title]));
                }
                if(adoptions.size()<unmatchedProvisionals.size() &&
                   adoptions.size()<unmatchedLive.size()){
                    std::set<size_t> adoptedSaved,adoptedLive;
                    for(const auto& adoption : adoptions){
                        adoptedSaved.insert(adoption.first);
                        adoptedLive.insert(adoption.second);
                    }
                    std::set<std::string> residualProvisionalTitles;
                    for(size_t index : unmatchedProvisionals){
                        if(adoptedSaved.count(index)!=0) continue;
                        const std::string title=NormalizeProvisionalAdoptionTitle(
                            existing[index].activeTitle);
                        if(title.empty()) return deferredPlan();
                        residualProvisionalTitles.insert(title);
                    }
                    for(size_t index : unmatchedLive){
                        if(adoptedLive.count(index)!=0) continue;
                        const std::string title=NormalizeProvisionalAdoptionTitle(
                            live[index].activeTitle);
                        if(title.empty() ||
                           residualProvisionalTitles.count(title)!=0)
                            return deferredPlan();
                    }
                }
            }
            if(unresolvedEstablished)
                for(size_t index : unmatchedLive)
                    if(live[index].counts.empty())
                        return deferredPlan();
            for(const auto& adoption : adoptions){
                LayoutMatch adopted;
                adopted.savedIndex=adoption.first;
                adopted.liveIndex=adoption.second;
                adopted.score=1.0;
                plan.matches.push_back(adopted);
                matchedSaved[adopted.savedIndex]=true;
                matchedLive[adopted.liveIndex]=true;
                if(!GuidEq(existing[adopted.savedIndex].desktop,
                           live[adopted.liveIndex].desktop)){
                    RestoreRequest restore;
                    restore.savedIndex=adopted.savedIndex;
                    restore.liveIndex=adopted.liveIndex;
                    restore.destination=existing[adopted.savedIndex].desktop;
                    plan.restores.push_back(restore);
                }
            }
        }
    }

    if(freshness==ReconcileFreshness::Fresh){
        size_t newRecordCount=0;
        for(size_t i=0;i<live.size();++i)
            if(live[i].app==app && !matchedLive[i]) ++newRecordCount;
        std::vector<bool> projectedMissing(existing.size(),false);
        std::vector<size_t> missingSavedIndices;
        for(size_t i=0;i<existing.size();++i){
            if(existing[i].app!=app || matchedSaved[i] || isReserved(i) ||
                    IsExpired(existing[i],nowUtc))
                continue;
            projectedMissing[i]=true;
            missingSavedIndices.push_back(i);
        }
        size_t projectedRetained=0;
        if(!ProjectedRetainedExistingCount(
                existing,projectedMissing,nowUtc,projectedRetained) ||
                projectedRetained>MAX_LAYOUT_RECORDS ||
                newRecordCount>MAX_LAYOUT_RECORDS-projectedRetained)
            return deferredPlan();
        plan.missingSavedIndices.swap(missingSavedIndices);
        for(size_t i=0;i<live.size();++i){
            if(live[i].app!=app || matchedLive[i]) continue;
            if(!idGenerator) return deferredPlan();
            std::string recordId=idGenerator();
            GUID id{};
            std::string canonicalId;
            if(!ParseNonzeroLayoutGuid(recordId,id,&canonicalId) ||
                    !occupiedRecordIds.insert(canonicalId).second)
                return deferredPlan();
            NewRecordRequest request;
            request.liveIndex=i;
            request.recordId=recordId;
            plan.newRecords.push_back(request);
        }
    }
    return plan;
}

enum class CliRestoreMatchStatus {
    Ready,
    Deferred,
    TooComplex
};

struct CliRestoreMatchPlan {
    CliRestoreMatchStatus status=CliRestoreMatchStatus::Deferred;
    std::vector<LayoutMatch> matches;
};

inline CliRestoreMatchPlan PlanCliCheckpointRestoreMatches(
        bool manual,const std::vector<LayoutWin>& saved,
        const std::vector<LayoutWin>& live,
        const std::vector<std::string>& enabledApps,UnixSeconds nowUtc) noexcept {
    CliRestoreMatchPlan output;
    if(saved.size()>MAX_LAYOUT_RECORDS || live.size()>MAX_LAYOUT_RECORDS ||
       enabledApps.size()>MAX_LAYOUT_RECORDS) return output;
    try {
        if(manual){
            bool tooComplex=false;
            output.matches=MatchOneToOne(saved,live,0.55,&tooComplex);
            output.status=tooComplex ? CliRestoreMatchStatus::TooComplex
                                     : CliRestoreMatchStatus::Ready;
            if(tooComplex) output.matches.clear();
            return output;
        }
        if(nowUtc<=0) return output;
        std::set<std::string> plannedApps;
        for(const std::string& app : enabledApps){
            if(!IsSupportedLayoutApp(app) || !plannedApps.insert(app).second)
                return CliRestoreMatchPlan();
            const ReconcilePlan plan=PlanAppReconcile(
                saved,live,app,nowUtc);
            if(plan.deferred){
                output.status=plan.tooComplex
                    ? CliRestoreMatchStatus::TooComplex
                    : CliRestoreMatchStatus::Deferred;
                output.matches.clear();
                return output;
            }
            if(plan.matches.size()>MAX_LAYOUT_RECORDS-output.matches.size())
                return CliRestoreMatchPlan();
            output.matches.insert(
                output.matches.end(),plan.matches.begin(),plan.matches.end());
        }
        output.status=CliRestoreMatchStatus::Ready;
        return output;
    } catch(...) { return CliRestoreMatchPlan(); }
}

inline std::vector<LayoutWin> CommitAppReconcile(
        const std::vector<LayoutWin>& existing,
        const std::vector<LayoutWin>& live,
        const ReconcilePlan& plan,
        const std::set<size_t>& successfulRestoreLiveIndices,
        UnixSeconds nowUtc){
    if(plan.deferred) return existing;
    if(!IsSupportedLayoutApp(plan.app) || nowUtc<=0 || plan.nowUtc!=nowUtc ||
            (plan.freshness!=ReconcileFreshness::Fresh &&
             plan.freshness!=ReconcileFreshness::CachedStale) ||
            existing.size()>MAX_LAYOUT_RECORDS || live.size()>MAX_LAYOUT_RECORDS ||
            plan.matches.size()>MAX_LAYOUT_RECORDS ||
            plan.restores.size()>MAX_LAYOUT_RECORDS ||
            plan.newRecords.size()>MAX_LAYOUT_RECORDS ||
            plan.missingSavedIndices.size()>MAX_LAYOUT_RECORDS)
        return existing;

    std::set<std::string> occupiedRecordIds;
    for(const LayoutWin& record : existing){
        GUID id{};
        std::string canonicalId;
        if(!ParseNonzeroLayoutGuid(record.recordId,id,&canonicalId) ||
                !occupiedRecordIds.insert(canonicalId).second)
            return existing;
    }

    std::set<size_t> matchedSavedIndices,matchedLiveIndices;
    std::set<std::pair<size_t,size_t>> matchPairs;
    for(const LayoutMatch& match : plan.matches){
        if(match.savedIndex>=existing.size() || match.liveIndex>=live.size() ||
                existing[match.savedIndex].app!=plan.app ||
                live[match.liveIndex].app!=plan.app ||
                IsExpired(existing[match.savedIndex],nowUtc) ||
                !matchedSavedIndices.insert(match.savedIndex).second ||
                !matchedLiveIndices.insert(match.liveIndex).second)
            return existing;
        matchPairs.insert(std::make_pair(match.savedIndex,match.liveIndex));
    }

    std::set<std::pair<size_t,size_t>> restorePairs;
    std::set<size_t> restoreLiveIndices;
    for(const RestoreRequest& restore : plan.restores){
        if(restore.savedIndex>=existing.size() || restore.liveIndex>=live.size())
            return existing;
        const std::pair<size_t,size_t> pair=
            std::make_pair(restore.savedIndex,restore.liveIndex);
        if(matchPairs.count(pair)==0 ||
                GuidEq(existing[restore.savedIndex].desktop,live[restore.liveIndex].desktop) ||
                !GuidEq(restore.destination,existing[restore.savedIndex].desktop) ||
                !restorePairs.insert(pair).second ||
                !restoreLiveIndices.insert(restore.liveIndex).second)
            return existing;
    }
    for(const LayoutMatch& match : plan.matches){
        const bool needsRestore=
            !GuidEq(existing[match.savedIndex].desktop,live[match.liveIndex].desktop);
        const bool hasRestore=restorePairs.count(
            std::make_pair(match.savedIndex,match.liveIndex))!=0;
        if(needsRestore!=hasRestore) return existing;
    }
    for(size_t liveIndex : successfulRestoreLiveIndices)
        if(restoreLiveIndices.count(liveIndex)==0) return existing;

    std::vector<bool> projectedMissing(existing.size(),false);
    if(plan.freshness==ReconcileFreshness::CachedStale){
        if(!plan.missingSavedIndices.empty() || !plan.newRecords.empty())
            return existing;
    } else {
        std::set<size_t> missingSavedIndices;
        for(size_t index : plan.missingSavedIndices){
            if(index>=existing.size() ||
                    !missingSavedIndices.insert(index).second ||
                    existing[index].app!=plan.app || IsExpired(existing[index],nowUtc) ||
                    matchedSavedIndices.count(index)!=0)
                return existing;
            projectedMissing[index]=true;
        }

        std::set<size_t> newLiveIndices;
        for(const NewRecordRequest& request : plan.newRecords){
            if(request.liveIndex>=live.size() ||
                    !newLiveIndices.insert(request.liveIndex).second ||
                    matchedLiveIndices.count(request.liveIndex)!=0 ||
                    live[request.liveIndex].app!=plan.app)
                return existing;
            GUID id{};
            std::string canonicalId;
            if(!ParseNonzeroLayoutGuid(request.recordId,id,&canonicalId) ||
                    !occupiedRecordIds.insert(canonicalId).second)
                return existing;
        }
    }

    size_t projectedRetained=0;
    if(!ProjectedRetainedExistingCount(
            existing,projectedMissing,nowUtc,projectedRetained) ||
            projectedRetained>MAX_LAYOUT_RECORDS ||
            plan.newRecords.size()>MAX_LAYOUT_RECORDS-projectedRetained)
        return existing;

    std::vector<LayoutWin> output=existing;
    for(const LayoutMatch& match : plan.matches){
        LayoutWin& record=output[match.savedIndex];
        const GUID savedDestination=record.desktop;
        const int savedDeskIndex=record.deskIndex;
        const std::string recordId=record.recordId;
        if(plan.freshness==ReconcileFreshness::Fresh){
            record.activeTitle=live[match.liveIndex].activeTitle;
            record.activeDomain=live[match.liveIndex].activeDomain;
            record.tabCount=live[match.liveIndex].tabCount;
            record.counts=live[match.liveIndex].counts;
            record.provisional=false;
        }
        record.recordId=recordId;
        MarkSeen(record,nowUtc);
        if(GuidEq(savedDestination,live[match.liveIndex].desktop)){
            record.desktop=live[match.liveIndex].desktop;
            record.deskIndex=live[match.liveIndex].deskIndex;
        } else {
            record.desktop=savedDestination;
            record.deskIndex=savedDeskIndex;
        }
    }
    for(size_t index : plan.missingSavedIndices)
        MarkMissing(output[index],nowUtc);
    for(const NewRecordRequest& request : plan.newRecords){
        LayoutWin record=live[request.liveIndex];
        record.recordId=request.recordId;
        MarkSeen(record,nowUtc);
        output.push_back(record);
    }
    return PruneExpired(output,nowUtc);
}

enum class FinalProfileQuality { Complete, Incomplete, Disabled };

struct FinalWindowObservation {
    LayoutWin observed;
    std::string boundRecordId;
    std::string pendingRecordId;
    std::string provisionalRecordId;
    bool desktopValid=false;
    bool fingerprintFresh=false;
    bool reserved=false;
    LayoutWin provisionalOriginRecord;
    bool hasProvisionalOriginRecord=false;
};

struct FinalAppObservation {
    std::string app;
    FinalProfileQuality quality=FinalProfileQuality::Incomplete;
    std::vector<FinalWindowObservation> windows;
};

template<class Build>
inline bool StageFinalObservationsAndProvisionals(
        const std::map<std::string,std::string>& currentProvisionals,
        std::vector<FinalAppObservation>& observationsOut,
        std::map<std::string,std::string>& provisionalsOut,
        Build&& build) noexcept {
    if(currentProvisionals.size()>MAX_LAYOUT_RECORDS) return false;
    try {
        std::map<std::string,std::string> stagedProvisionals=
            currentProvisionals;
        std::vector<FinalAppObservation> stagedObservations;
        if(!build(stagedProvisionals,stagedObservations) ||
           stagedObservations.size()>3 ||
           stagedProvisionals.size()>MAX_LAYOUT_RECORDS) return false;
        observationsOut.swap(stagedObservations);
        provisionalsOut.swap(stagedProvisionals);
        return true;
    } catch(...) { return false; }
}

struct FinalSnapshotResult {
    bool valid=false;
    std::vector<LayoutWin> records;
    std::set<std::string> changedRecordIds;
    std::set<std::string> erasedRecordIds;
};

namespace final_snapshot_detail {

inline bool SameRecord(const LayoutWin& left,const LayoutWin& right){
    return left.recordId==right.recordId && left.app==right.app &&
        left.deskIndex==right.deskIndex && GuidEq(left.desktop,right.desktop) &&
        left.activeTitle==right.activeTitle &&
        left.activeDomain==right.activeDomain && left.tabCount==right.tabCount &&
        left.counts==right.counts && left.lastSeenUtc==right.lastSeenUtc &&
        left.missingSinceUtc==right.missingSinceUtc &&
        left.provisional==right.provisional;
}

inline bool CanonicalId(const std::string& value,std::string& canonical){
    GUID parsed{};
    return ParseNonzeroLayoutGuid(value,parsed,&canonical);
}

inline bool ValidRecord(const LayoutWin& record,std::string* canonicalOut=nullptr){
    std::string canonical;
    if(!CanonicalId(record.recordId,canonical) ||
       !IsSupportedLayoutApp(record.app) || GuidIsZero(record.desktop) ||
       record.lastSeenUtc<0 || record.missingSinceUtc<0) return false;
    if(canonicalOut) *canonicalOut=canonical;
    return true;
}

inline void CopyFingerprint(LayoutWin& target,const LayoutWin& source){
    target.activeTitle=source.activeTitle;
    target.activeDomain=source.activeDomain;
    target.tabCount=source.tabCount;
    target.counts=source.counts;
}

inline void BuildIndex(const std::vector<LayoutWin>& records,
                       std::map<std::string,size_t>& index){
    index.clear();
    for(size_t position=0;position<records.size();++position){
        std::string canonical;
        if(CanonicalId(records[position].recordId,canonical))
            index[canonical]=position;
    }
}

} // namespace final_snapshot_detail

inline FinalSnapshotResult CommitFinalSnapshotRecords(
        const std::vector<LayoutWin>& existing,
        const std::vector<FinalAppObservation>& observations,
        UnixSeconds nowUtc){
    FinalSnapshotResult result;
    result.records=existing;
    if(nowUtc<=0 || existing.size()>MAX_LAYOUT_RECORDS ||
       observations.size()>3) return result;

    std::map<std::string,size_t> recordIndex;
    std::set<std::string> occupiedIds;
    for(size_t position=0;position<result.records.size();++position){
        std::string canonical;
        if(!final_snapshot_detail::ValidRecord(
                result.records[position],&canonical) ||
           !occupiedIds.insert(canonical).second) return result;
        recordIndex[canonical]=position;
    }

    std::set<std::string> observedApps;
    for(const FinalAppObservation& appObservation : observations){
        if(!IsSupportedLayoutApp(appObservation.app) ||
           !observedApps.insert(appObservation.app).second ||
           appObservation.windows.size()>MAX_LAYOUT_RECORDS) return result;
        if(appObservation.quality==FinalProfileQuality::Incomplete) continue;

        if(appObservation.quality==FinalProfileQuality::Disabled){
            for(size_t position=result.records.size();position>0;--position){
                const size_t index=position-1;
                if(result.records[index].app==appObservation.app &&
                   IsExpired(result.records[index],nowUtc)){
                    result.erasedRecordIds.insert(result.records[index].recordId);
                    result.records.erase(result.records.begin()+
                        static_cast<std::ptrdiff_t>(index));
                }
            }
            final_snapshot_detail::BuildIndex(result.records,recordIndex);
            continue;
        }

        std::set<std::string> seenIds;
        for(const FinalWindowObservation& window : appObservation.windows){
            if(window.observed.app!=appObservation.app) return result;
            std::string chosenId;
            enum class MatchKind { None, Bound, Pending, Provisional, Title };
            MatchKind matchKind=MatchKind::None;
            auto chooseExisting=[&](const std::string& candidate,MatchKind kind){
                std::string canonical;
                if(candidate.empty() ||
                   !final_snapshot_detail::CanonicalId(candidate,canonical))
                    return;
                auto found=recordIndex.find(canonical);
                if(found!=recordIndex.end() &&
                   result.records[found->second].app==appObservation.app){
                    chosenId=canonical;
                    matchKind=kind;
                }
            };
            chooseExisting(window.boundRecordId,MatchKind::Bound);
            if(matchKind==MatchKind::None)
                chooseExisting(window.pendingRecordId,MatchKind::Pending);
            if(matchKind==MatchKind::None)
                chooseExisting(window.provisionalRecordId,MatchKind::Provisional);
            if(matchKind==MatchKind::None &&
               !window.observed.activeTitle.empty()){
                std::string only;
                size_t matches=0;
                for(size_t position=0;position<result.records.size();++position){
                    const LayoutWin& candidate=result.records[position];
                    if(candidate.app!=appObservation.app ||
                       candidate.activeTitle!=window.observed.activeTitle) continue;
                    std::string canonical;
                    if(!final_snapshot_detail::CanonicalId(
                            candidate.recordId,canonical) ||
                       seenIds.count(canonical)) continue;
                    only=canonical;
                    ++matches;
                }
                if(matches==1){ chosenId=only; matchKind=MatchKind::Title; }
            }

            if(window.reserved){
                if(matchKind!=MatchKind::None){
                    seenIds.insert(chosenId);
                    continue;
                }
                if(!window.hasProvisionalOriginRecord) continue;
                LayoutWin provisional=window.provisionalOriginRecord;
                provisional.provisional=true;
                std::string canonical;
                if(!final_snapshot_detail::ValidRecord(provisional,&canonical) ||
                   provisional.app!=appObservation.app) return result;
                auto found=recordIndex.find(canonical);
                if(found==recordIndex.end()){
                    if(result.records.size()>=MAX_LAYOUT_RECORDS) return result;
                    result.records.push_back(provisional);
                    recordIndex[canonical]=result.records.size()-1;
                    occupiedIds.insert(canonical);
                } else if(result.records[found->second].app!=appObservation.app){
                    return result;
                }
                seenIds.insert(canonical);
                result.changedRecordIds.insert(provisional.recordId);
                continue;
            }

            if(matchKind!=MatchKind::None){
                auto found=recordIndex.find(chosenId);
                if(found==recordIndex.end()) return result;
                LayoutWin& target=result.records[found->second];
                LayoutWin before=target;
                if(window.fingerprintFresh)
                    final_snapshot_detail::CopyFingerprint(target,window.observed);
                if(matchKind==MatchKind::Bound && window.desktopValid &&
                   !GuidIsZero(window.observed.desktop)){
                    target.desktop=window.observed.desktop;
                    target.deskIndex=window.observed.deskIndex;
                }
                MarkSeen(target,nowUtc);
                seenIds.insert(chosenId);
                if(!final_snapshot_detail::SameRecord(before,target))
                    result.changedRecordIds.insert(target.recordId);
                continue;
            }

            if(!window.desktopValid || GuidIsZero(window.observed.desktop)) continue;
            LayoutWin added=window.observed;
            added.recordId=window.provisionalRecordId;
            added.provisional=true;
            std::string canonical;
            if(!final_snapshot_detail::ValidRecord(added,&canonical) ||
               occupiedIds.count(canonical)) return result;
            MarkSeen(added,nowUtc);
            if(result.records.size()>=MAX_LAYOUT_RECORDS) return result;
            result.records.push_back(added);
            recordIndex[canonical]=result.records.size()-1;
            occupiedIds.insert(canonical);
            seenIds.insert(canonical);
            result.changedRecordIds.insert(added.recordId);
        }

        for(size_t position=result.records.size();position>0;--position){
            const size_t index=position-1;
            LayoutWin& record=result.records[index];
            if(record.app!=appObservation.app) continue;
            std::string canonical;
            if(!final_snapshot_detail::CanonicalId(record.recordId,canonical))
                return result;
            if(seenIds.count(canonical)) continue;
            LayoutWin before=record;
            MarkMissing(record,nowUtc);
            if(IsExpired(record,nowUtc)){
                result.erasedRecordIds.insert(record.recordId);
                result.changedRecordIds.erase(record.recordId);
                result.records.erase(result.records.begin()+
                    static_cast<std::ptrdiff_t>(index));
            } else if(!final_snapshot_detail::SameRecord(before,record)){
                result.changedRecordIds.insert(record.recordId);
            }
        }
        final_snapshot_detail::BuildIndex(result.records,recordIndex);
    }
    result.valid=true;
    return result;
}

struct BoundSaveObservation {
    FastWin window;
    bool hasBinding = false;
    WindowIdentityKey expectedIdentity;
    std::string recordId;
    int deskIndex = -1;
    uint64_t causalGeneration = 0;
};

struct BoundSaveUpdate {
    LayoutWin before;
    LayoutWin after;
    uint64_t causalGeneration = 0;
    bool semanticChanged = false;
};

struct SaveObservedAppResult {
    bool valid = false;
    bool needsReconcile = false;
    std::vector<LayoutWin> records;
    std::vector<BoundSaveUpdate> updates;
};

inline SaveObservedAppResult ApplyObservedBoundRecords(
        const std::vector<LayoutWin>& records,const std::string& app,
        const std::vector<BoundSaveObservation>& observations,
        bool profileComplete,UnixSeconds nowUtc){
    SaveObservedAppResult failed;
    if(!profileComplete || !IsSupportedLayoutApp(app) || nowUtc<=0 ||
       records.size()>MAX_LAYOUT_RECORDS ||
       observations.size()>MAX_LAYOUT_RECORDS) return failed;

    SaveObservedAppResult result;
    try {
        result.records=records;
        result.updates.reserve(observations.size());
        std::map<std::string,size_t> byRecordId;
        for(size_t index=0;index<result.records.size();++index){
            GUID id{};
            std::string canonical;
            if(!ParseNonzeroLayoutGuid(
                    result.records[index].recordId,id,&canonical) ||
               !byRecordId.emplace(canonical,index).second) return failed;
        }

        std::set<std::string> claimedRecordIds;
        std::set<WindowIdentityKey> claimedIdentities;
        for(const BoundSaveObservation& observed : observations){
            const WindowIdentityKey actual=IdentityOf(observed.window);
            GUID recordGuid{};
            std::string canonical;
            const bool identityValid=observed.window.hwnd!=nullptr &&
                observed.window.pid!=0 && observed.window.processStart!=0 &&
                observed.window.app==app && observed.hasBinding &&
                SameIdentity(observed.expectedIdentity,actual) &&
                observed.causalGeneration!=0 && observed.deskIndex>=0 &&
                !GuidIsZero(observed.window.desktop) &&
                ParseNonzeroLayoutGuid(
                    observed.recordId,recordGuid,&canonical);
            const auto record=identityValid
                ? byRecordId.find(canonical) : byRecordId.end();
            if(!identityValid || record==byRecordId.end() ||
               result.records[record->second].app!=app ||
               !claimedRecordIds.insert(canonical).second ||
               !claimedIdentities.insert(actual).second){
                result.needsReconcile=true;
                continue;
            }

            BoundSaveUpdate update;
            update.before=result.records[record->second];
            update.after=update.before;
            update.after.desktop=observed.window.desktop;
            update.after.deskIndex=observed.deskIndex;
            MarkSeen(update.after,nowUtc);
            update.causalGeneration=observed.causalGeneration;
            update.semanticChanged=
                update.before.deskIndex!=update.after.deskIndex ||
                !GuidEq(update.before.desktop,update.after.desktop) ||
                update.before.missingSinceUtc!=update.after.missingSinceUtc;
            result.records[record->second]=update.after;
            result.updates.push_back(std::move(update));
        }
    } catch(...) { return failed; }
    result.valid=true;
    return result;
}

enum class LcAction {
    None, BeginRestore, SaveLayout, MarkMissingFromLastSeen
};

enum class LcRestoreOutcome { Success, Deferred, Exhausted };

inline LcRestoreOutcome AutoRestoreCompletionOutcome(
        bool hadExhausted,bool hadFailure) noexcept {
    return (hadExhausted || hadFailure)
        ? LcRestoreOutcome::Exhausted : LcRestoreOutcome::Success;
}

struct LcDecision {
    LcAction action = LcAction::None;
    uint64_t generation = 0;
};

struct RestoreBudgetKey {
    std::string recordId;
    std::string fullRuntimeIdentity;
    std::string destinationGuid;
};

inline bool operator==(const RestoreBudgetKey& a, const RestoreBudgetKey& b){
    return a.recordId == b.recordId &&
           a.fullRuntimeIdentity == b.fullRuntimeIdentity &&
           a.destinationGuid == b.destinationGuid;
}

// Explicit per-instance seam for deterministic allocation-fault tests.
struct RestoreBudgetOps {
    std::function<RestoreBudgetKey(const RestoreBudgetKey&)> copyKey;

    RestoreBudgetOps(){
        copyKey=[](const RestoreBudgetKey& key)->RestoreBudgetKey{ return key; };
    }
};

struct LcState {
    bool initialized = false;
    bool present = false;
    bool restorePending = false;
    bool restoreInFlight = false;
    bool rearmAfterFlight = false;
    bool deferredUntilInputChanges = false;
    int stableSnapshots = 0;
    int deferredAttempts = 0;
    uint64_t nextGeneration = 1;
    uint64_t inFlightGeneration = 0;
    uint64_t windowSetSignature = 0;
    uint64_t settleSignature = 0;
    uint64_t layoutSignature = 0;
    uint64_t sessionStampSignature = 0;
    uint64_t appearanceSinceMs = 0;
    uint64_t retryNotBeforeMs = 0;

    uint64_t completedLayoutSignature = 0;
    uint64_t inFlightWindowSetSignature = 0;
    uint64_t inFlightSessionStampSignature = 0;
    uint64_t deferredWindowSetSignature = 0;
    uint64_t deferredSessionStampSignature = 0;
    uint64_t lastObservedMs = 0;
    bool saveInFlight = false;
    uint64_t saveGeneration = 0;
    uint64_t saveRequestedLayoutSignature = 0;
};

static const uint64_t LC_SETTLE_TIMEOUT_MS = 20000;
static const uint64_t LC_DEFERRED_BACKOFF_MS = 30000;

inline uint64_t LcTakeGeneration(LcState& state){
    uint64_t generation = state.nextGeneration;
    if(generation == 0) return 0;
    state.nextGeneration = generation == UINT64_MAX ? 0 : generation + 1;
    return generation;
}

inline LcDecision LcMissingDecision(LcState& state){
    const uint64_t generation = LcTakeGeneration(state);
    return generation == 0
        ? LcDecision{}
        : LcDecision{LcAction::MarkMissingFromLastSeen, generation};
}

inline void LcRebaseClockIfRolledBack(LcState& state, uint64_t nowMs){
    if(nowMs >= state.lastObservedMs) return;
    if(state.restorePending) state.appearanceSinceMs = nowMs;
    if(state.retryNotBeforeMs != 0){
        if(nowMs > UINT64_MAX - LC_DEFERRED_BACKOFF_MS){
            state.restorePending = false;
            state.stableSnapshots = 0;
            state.deferredUntilInputChanges = true;
            state.retryNotBeforeMs = 0;
        } else {
            state.retryNotBeforeMs = nowMs + LC_DEFERRED_BACKOFF_MS;
        }
    }
}

inline bool LcElapsedAtLeast(uint64_t nowMs, uint64_t sinceMs, uint64_t delayMs){
    return nowMs >= sinceMs && nowMs - sinceMs >= delayMs;
}

inline void LcResetDeferred(LcState& state){
    state.deferredAttempts = 0;
    state.deferredUntilInputChanges = false;
    state.deferredWindowSetSignature = 0;
    state.deferredSessionStampSignature = 0;
    state.retryNotBeforeMs = 0;
}

inline void LcArmPending(LcState& state, uint64_t nowMs, int snapshots){
    state.restorePending = true;
    state.stableSnapshots = snapshots;
    state.appearanceSinceMs = nowMs;
}

inline LcDecision LcObserve(LcState& state,
                            bool present,
                            uint64_t windowSetSignature,
                            uint64_t settleSignature,
                            uint64_t layoutSignature,
                            uint64_t acceptedFreshSessionSignature,
                            uint64_t nowMs){
    if(!state.initialized){
        state.initialized = true;
        state.present = present;
        state.windowSetSignature = windowSetSignature;
        state.settleSignature = settleSignature;
        state.layoutSignature = layoutSignature;
        state.completedLayoutSignature = layoutSignature;
        state.sessionStampSignature = acceptedFreshSessionSignature;
        state.lastObservedMs = nowMs;
        if(!present) return LcMissingDecision(state);
        LcArmPending(state, nowMs, 1);
        return {};
    }

    LcRebaseClockIfRolledBack(state, nowMs);
    const bool wasPresent = state.present;
    const bool windowChanged = state.windowSetSignature != windowSetSignature;
    const bool sessionChanged =
        state.sessionStampSignature != acceptedFreshSessionSignature;
    const bool settleChanged = state.settleSignature != settleSignature;
    state.present = present;
    state.windowSetSignature = windowSetSignature;
    state.settleSignature = settleSignature;
    state.layoutSignature = layoutSignature;
    state.sessionStampSignature = acceptedFreshSessionSignature;
    state.lastObservedMs = nowMs;

    if(!present){
        state.restorePending = false;
        state.stableSnapshots = 0;
        if(!wasPresent) return {};
        if(state.restoreInFlight) state.rearmAfterFlight = true;
        return LcMissingDecision(state);
    }

    if(state.restoreInFlight){
        if(!wasPresent || windowChanged || sessionChanged)
            state.rearmAfterFlight = true;
        return {};
    }

    if(!wasPresent || windowChanged || sessionChanged){
        LcResetDeferred(state);
        LcArmPending(state, nowMs, 1);
        return {};
    }

    if(state.saveInFlight || state.deferredUntilInputChanges) return {};
    if(state.restorePending){
        state.stableSnapshots = settleChanged ? 1 :
            (state.stableSnapshots < 2 ? state.stableSnapshots + 1 : 2);
        const bool settled = state.stableSnapshots >= 2;
        const bool timedOut =
            LcElapsedAtLeast(nowMs, state.appearanceSinceMs, LC_SETTLE_TIMEOUT_MS);
        const bool retryReady = state.retryNotBeforeMs == 0 ||
                                nowMs >= state.retryNotBeforeMs;
        if((settled || timedOut) && retryReady){
            state.restorePending = false;
            state.rearmAfterFlight = false;
            state.stableSnapshots = 0;
            const uint64_t generation = LcTakeGeneration(state);
            if(generation == 0) return {};
            state.restoreInFlight = true;
            state.inFlightGeneration = generation;
            state.inFlightWindowSetSignature = state.windowSetSignature;
            state.inFlightSessionStampSignature = state.sessionStampSignature;
            state.retryNotBeforeMs = 0;
            return {LcAction::BeginRestore, state.inFlightGeneration};
        }
        return {};
    }
    if(state.layoutSignature != state.completedLayoutSignature){
        const uint64_t generation = LcTakeGeneration(state);
        if(generation == 0) return {};
        state.saveInFlight = true;
        state.saveGeneration = generation;
        state.saveRequestedLayoutSignature = state.layoutSignature;
        return {LcAction::SaveLayout, state.saveGeneration};
    }
    return {};
}

enum class InitialLifecycleFaultPoint { AfterApp, BeforePublish };

struct InitialMissingApp {
    std::string app;
    uint64_t generation=0;
};

struct InitialLifecyclePreparation {
    std::map<std::string,LcState> states;
    std::map<std::string,uint64_t> signatures;
    std::vector<InitialMissingApp> missingApps;
};

inline bool PrepareInitialLifecycleStates(
        const std::vector<AppProfile>& profiles,
        const std::map<std::string,AppFastSnapshot>& snapshots,
        uint64_t nowMs,InitialLifecyclePreparation& output,
        const std::function<void(InitialLifecycleFaultPoint)>& injectFault={})
        noexcept {
    try {
        InitialLifecyclePreparation staged;
        if(profiles.size()>3 || snapshots.size()>3) return false;
        for(const AppProfile& profile : profiles){
            const auto found=snapshots.find(profile.id);
            if(found==snapshots.end() ||
               !FastSnapshotCanObserve(found->second)) continue;
            LcState state;
            const bool present=!found->second.windows.empty();
            const LcDecision decision=LcObserve(
                state,present,found->second.windowSetSignature,
                found->second.settleSignature,
                found->second.layoutSignature,0,nowMs);
            if(!state.initialized ||
               !staged.states.emplace(profile.id,std::move(state)).second ||
               !staged.signatures.emplace(profile.id,0).second) return false;
            if(decision.action==LcAction::MarkMissingFromLastSeen){
                if(decision.generation==0 || found->second.generation==0)
                    return false;
                InitialMissingApp missing;
                missing.app=profile.id;
                missing.generation=found->second.generation;
                staged.missingApps.push_back(std::move(missing));
            } else if(decision.action!=LcAction::None) return false;
            if(injectFault) injectFault(InitialLifecycleFaultPoint::AfterApp);
        }
        if(injectFault) injectFault(InitialLifecycleFaultPoint::BeforePublish);
        output.states.swap(staged.states);
        output.signatures.swap(staged.signatures);
        output.missingApps.swap(staged.missingApps);
        return true;
    } catch(...) { return false; }
}

inline void LcRestoreCompleted(LcState& state,
                               uint64_t generation,
                               LcRestoreOutcome outcome,
                               uint64_t layoutSignature,
                               uint64_t sessionStampSignature,
                               uint64_t nowMs){
    if(!state.restoreInFlight || generation == 0 ||
       generation != state.inFlightGeneration) return;
    const uint64_t completedWindowSet = state.inFlightWindowSetSignature;
    const uint64_t completedSession = state.inFlightSessionStampSignature;
    const bool queuedRearm = state.rearmAfterFlight && state.present;
    state.restoreInFlight = false;
    state.inFlightGeneration = 0;
    state.inFlightWindowSetSignature = 0;
    state.inFlightSessionStampSignature = 0;
    state.rearmAfterFlight = false;
    state.lastObservedMs = nowMs;

    if(outcome == LcRestoreOutcome::Deferred){
        if(queuedRearm){
            LcResetDeferred(state);
            LcArmPending(state, nowMs, 0);
            return;
        }
        if(state.deferredWindowSetSignature != completedWindowSet ||
           state.deferredSessionStampSignature != sessionStampSignature){
            state.deferredAttempts = 0;
            state.deferredWindowSetSignature = completedWindowSet;
            state.deferredSessionStampSignature = sessionStampSignature;
        }
        if(state.deferredAttempts < 3) ++state.deferredAttempts;
        if(state.deferredAttempts >= 3){
            state.restorePending = false;
            state.stableSnapshots = 0;
            state.deferredUntilInputChanges = true;
            state.retryNotBeforeMs = 0;
        } else if(state.present){
            if(nowMs > UINT64_MAX - LC_DEFERRED_BACKOFF_MS){
                state.restorePending = false;
                state.stableSnapshots = 0;
                state.deferredUntilInputChanges = true;
                state.retryNotBeforeMs = 0;
            } else {
                state.deferredUntilInputChanges = false;
                state.retryNotBeforeMs = nowMs + LC_DEFERRED_BACKOFF_MS;
                LcArmPending(state, nowMs, 0);
            }
        }
        return;
    }

    if(outcome == LcRestoreOutcome::Success ||
       outcome == LcRestoreOutcome::Exhausted){
        state.completedLayoutSignature = layoutSignature;
        LcResetDeferred(state);
    }
    if(queuedRearm){
        LcArmPending(state, nowMs, 0);
    }
    (void)completedSession;
}

inline bool LcCancelRestore(LcState& state,
                            uint64_t generation,
                            uint64_t nowMs,
                            bool rearm){
    if(!state.restoreInFlight || generation==0 ||
       generation!=state.inFlightGeneration) return false;
    state.restoreInFlight=false;
    state.inFlightGeneration=0;
    state.inFlightWindowSetSignature=0;
    state.inFlightSessionStampSignature=0;
    state.rearmAfterFlight=false;
    state.restorePending=false;
    state.stableSnapshots=0;
    state.retryNotBeforeMs=0;
    state.lastObservedMs=nowMs;
    if(rearm && state.present){
        LcResetDeferred(state);
        LcArmPending(state,nowMs,0);
    }
    return true;
}

inline void LcExplicitSaveCompleted(LcState& state,
                                    uint64_t generation,
                                    uint64_t layoutSignature,
                                    uint64_t sessionStampSignature,
                                    uint64_t nowMs){
    if(!state.saveInFlight || generation == 0 ||
       generation != state.saveGeneration) return;
    const uint64_t completedLayoutSignature = state.saveRequestedLayoutSignature;
    state.saveInFlight = false;
    state.saveGeneration = 0;
    state.saveRequestedLayoutSignature = 0;
    state.completedLayoutSignature = completedLayoutSignature;
    LcResetDeferred(state);
    LcRebaseClockIfRolledBack(state, nowMs);
    state.lastObservedMs = nowMs;
    (void)layoutSignature;
    (void)sessionStampSignature;
}

inline bool LcExplicitSaveNeedsReconcile(LcState& state,
                                         uint64_t generation,
                                         uint64_t layoutSignature,
                                         uint64_t sessionStampSignature,
                                         uint64_t nowMs){
    if(!state.saveInFlight || generation==0 ||
       generation!=state.saveGeneration) return false;
    LcExplicitSaveCompleted(state,generation,layoutSignature,
                            sessionStampSignature,nowMs);
    if(state.present){
        LcResetDeferred(state);
        LcArmPending(state,nowMs,0);
    }
    return true;
}

class RestoreBudgets {
    struct Entry {
        RestoreBudgetKey key;
    };
    static_assert(std::is_nothrow_move_constructible<Entry>::value,
                  "Restore budget commits require no-throw entry moves");
    static_assert(std::is_nothrow_move_assignable<Entry>::value,
                  "Restore budget eviction requires no-throw entry moves");
    static const std::size_t kMaximumEntries = 256;
    mutable std::vector<Entry> entries_;
    RestoreBudgetOps ops_;

    bool touchIfPresent(const RestoreBudgetKey& key) const {
        for(auto it = entries_.begin(); it != entries_.end(); ++it){
            if(it->key == key){
                Entry touched = *it;
                entries_.erase(it);
                entries_.push_back(std::move(touched));
                return true;
            }
        }
        return false;
    }

public:
    RestoreBudgets() = default;
    explicit RestoreBudgets(RestoreBudgetOps ops) : ops_(std::move(ops)) {}

    bool mayAttempt(const RestoreBudgetKey& key) const {
        return !touchIfPresent(key);
    }

    bool prepareTerminalInsert() noexcept {
        try {
            if(entries_.capacity()<kMaximumEntries+1)
                entries_.reserve(kMaximumEntries+1);
            return true;
        } catch(...) { return false; }
    }

    bool markExhaustedPrepared(RestoreBudgetKey&& key) noexcept {
        try {
            for(const Entry& entry : entries_)
                if(entry.key==key) return true;
            if(entries_.capacity()<kMaximumEntries+1) return false;
            entries_.push_back(Entry{std::move(key)});
            if(entries_.size()>kMaximumEntries) entries_.erase(entries_.begin());
            return true;
        } catch(...) { return false; }
    }

    void markExhausted(const RestoreBudgetKey& key){
        if(touchIfPresent(key)) return;
        Entry pending{ops_.copyKey(key)};
        entries_.push_back(std::move(pending));
        if(entries_.size() > kMaximumEntries) entries_.erase(entries_.begin());
    }

    void clearExact(const RestoreBudgetKey& key) noexcept {
        try {
            entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                [&](const Entry& entry){ return entry.key == key; }), entries_.end());
        } catch(...) {}
    }

    void clearForExplicitRetry(const std::string& recordId){
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
            [&](const Entry& entry){ return entry.key.recordId == recordId; }),
            entries_.end());
    }

    void pruneToLiveIdentities(const std::set<std::string>& liveRuntimeKeys){
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
            [&](const Entry& entry){
                return liveRuntimeKeys.count(entry.key.fullRuntimeIdentity) == 0;
            }), entries_.end());
    }

    std::size_t size() const { return entries_.size(); }
};
