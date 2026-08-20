// vdtest.cpp — tiny CHECK-macro harness for win-vde pure logic.
// Build/run via build-test.bat. Exit code 0 = all passed.
#define UNICODE
#define _UNICODE
#include "window_identity.hpp" // must be self-contained at first include
#include "reconcile_worker.hpp"
#include "session_worker.hpp"
#include "move_queue.hpp"
#include "str_util.hpp"
#include "layout.hpp"
#include "layout_store.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include "lifecycle.hpp"

static int g_fail = 0, g_total = 0;
#define CHECK(c) do{ g_total++; if(!(c)){ g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

static void test_etld1(){
    CHECK(etld1("mail.google.com") == "google.com");
    CHECK(etld1("docs.python.org") == "python.org");
    CHECK(hostOf("https://www.GitHub.com/x/y") == "github.com");
}
static void test_b64(){
    std::string s = "Inbox \xE2\x80\x94 Mozilla";   // includes a UTF-8 em dash
    CHECK(b64dec(b64enc(s)) == s);
}
static void test_b64_long_roundtrip(){
    std::string input; input.reserve(1024*1024);
    for(int i=0;i<1024*1024;++i) input.push_back((char)(i&0xFF));
    CHECK(b64dec(b64enc(input))==input);
}
static void test_strict_integer_parsing(){
    long long i64=0;
    CHECK(ParseI64Strict("-9223372036854775808", i64) && i64==LLONG_MIN);
    CHECK(ParseI64Strict("9223372036854775807", i64) && i64==LLONG_MAX);
    i64=17; CHECK(!ParseI64Strict("", i64) && i64==17);
    CHECK(!ParseI64Strict("12junk", i64) && i64==17);
    CHECK(!ParseI64Strict("9223372036854775808", i64) && i64==17);
    int value=0;
    CHECK(ParseIntStrict("-2147483648", value) && value==INT_MIN);
    CHECK(ParseIntStrict("2147483647", value) && value==INT_MAX);
    value=23; CHECK(!ParseIntStrict("2147483648", value) && value==23);
}
static void test_strict_base64_parsing(){
    std::string out;
    CHECK(b64decStrict("", out) && out.empty());
    CHECK(b64decStrict("TQ==", out) && out=="M");
    CHECK(b64decStrict("TWE=", out) && out=="Ma");
    CHECK(b64decStrict("TWFu", out) && out=="Man");
    out="sentinel"; CHECK(!b64decStrict("TQ=", out) && out=="sentinel");
    CHECK(!b64decStrict("TQ$=", out) && out=="sentinel");
    CHECK(!b64decStrict("A===", out) && out=="sentinel");
    CHECK(!b64decStrict("AA=A", out) && out=="sentinel");
    CHECK(!b64decStrict("=AAA", out) && out=="sentinel");
    CHECK(!b64decStrict("TR==", out) && out=="sentinel");
    CHECK(!b64decStrict("TWF=", out) && out=="sentinel");
}
static void test_strict_counts_parsing(){
    std::map<std::string,int> counts={{"old",9}};
    CHECK(ParseCountsStrict("", counts) && counts.empty());
    CHECK(ParseCountsStrict("mail.example:2,[::1]:3", counts));
    CHECK(counts.size()==2 && counts["mail.example"]==2 && counts["[::1]"]==3);
    counts={{"old",9}}; CHECK(!ParseCountsStrict("mail.example:0", counts) && counts["old"]==9);
    CHECK(!ParseCountsStrict("mail.example:-1", counts) && counts["old"]==9);
    CHECK(!ParseCountsStrict("mail.example:1junk", counts) && counts["old"]==9);
    CHECK(!ParseCountsStrict("mail.example:1,mail.example:2", counts) && counts["old"]==9);
    CHECK(!ParseCountsStrict("mail.example:1,", counts) && counts["old"]==9);
    CHECK(!ParseCountsStrict(":1", counts) && counts["old"]==9);
    CHECK(!ParseCountsStrict("missing-count", counts) && counts["old"]==9);
}

static GUID G(const wchar_t* s){ GUID g{}; StringToGuid(s, g); return g; }

static MoveJob MJ(MoveOwner owner, uint64_t operationId,
                  uint64_t jobId, const char* runtimeKey){
    MoveJob job;
    job.token={owner,operationId,jobId,0};
    job.runtimeKey=runtimeKey;
    job.recordId="record-"+std::to_string(jobId);
    job.destination=G(L"{231A0000-0000-0000-0000-000000000001}");
    return job;
}

static void check_move_result(const MoveResult& result,MoveTerminal terminal,
                              const MoveJob& job,int attempts){
    CHECK(result.completed);
    CHECK(result.terminal==terminal);
    CHECK(result.attempts==attempts);
    CHECK(result.token.owner==job.token.owner);
    CHECK(result.token.operationId==job.token.operationId);
    CHECK(result.token.jobId==job.token.jobId);
    CHECK(result.token.itemIndex==job.token.itemIndex);
    CHECK(result.runtimeKey==job.runtimeKey);
    CHECK(result.recordId==job.recordId);
}

static void check_empty_move_result(const MoveResult& result){
    CHECK(!result.completed);
    CHECK(result.terminal==MoveTerminal::None);
    CHECK(result.attempts==0);
    CHECK(result.token.owner==MoveOwner::AutoReconcile);
    CHECK(result.token.operationId==0);
    CHECK(result.token.jobId==0);
    CHECK(result.token.itemIndex==0);
    CHECK(result.runtimeKey.empty());
    CHECK(result.recordId.empty());
}

static_assert(std::is_same<decltype(std::declval<MoveQueue&>().front()),
                           const MoveJob*>::value,
              "MoveQueue::front must expose read-only state");

static void test_move_queue_alternates_issue_verify_and_succeeds(){
    MoveQueue queue;
    CHECK(queue.empty());
    CHECK(queue.front()==nullptr);
    CHECK(queue.nextAction()==MoveAction::None);
    check_empty_move_result(queue.onIssued(MoveAttemptOutcome::Accepted));
    check_empty_move_result(queue.onVerified(MoveAttemptOutcome::OnDestination));
    check_empty_move_result(queue.cancelJob(77));

    MoveJob job=MJ(MoveOwner::Picker,101,1001,"picker-runtime");
    job.token.itemIndex=7;
    job.recordId="{00000000-0000-0000-0000-000000001001}";
    CHECK(queue.enqueue(job));
    CHECK(!queue.empty());
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==1001);
    CHECK(queue.nextAction()==MoveAction::Issue);

    MoveResult issued=queue.onIssued(MoveAttemptOutcome::Accepted);
    CHECK(!issued.completed);
    CHECK(issued.terminal==MoveTerminal::None);
    CHECK(issued.attempts==1);
    CHECK(issued.token.owner==MoveOwner::Picker);
    CHECK(issued.token.operationId==101);
    CHECK(issued.token.jobId==1001);
    CHECK(issued.token.itemIndex==7);
    CHECK(issued.runtimeKey==job.runtimeKey);
    CHECK(issued.recordId==job.recordId);
    CHECK(queue.front()!=nullptr && queue.front()->attempts==1);
    CHECK(queue.front()!=nullptr && queue.front()->waitingForVerify);
    CHECK(queue.nextAction()==MoveAction::Verify);

    MoveResult verified=queue.onVerified(MoveAttemptOutcome::OnDestination);
    check_move_result(verified,MoveTerminal::Succeeded,job,1);
    CHECK(queue.empty());
    CHECK(queue.front()==nullptr);
    CHECK(queue.nextAction()==MoveAction::None);
}

static void test_move_queue_enqueue_validates_identity_state_and_copies_guid(){
    MoveQueue queue;
    MoveJob valid=MJ(MoveOwner::AutoReconcile,111,1101,"");
    valid.recordId.clear();
    GUID expectedDestination=valid.destination;
    CHECK(queue.enqueue(valid));
    valid.token.operationId=999;
    valid.token.jobId=999;
    valid.destination=GUID{};
    CHECK(queue.front()!=nullptr && queue.front()->token.operationId==111);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==1101);
    CHECK(queue.front()!=nullptr && GuidEq(queue.front()->destination,expectedDestination));
    CHECK(queue.front()!=nullptr && queue.front()->runtimeKey.empty());
    CHECK(queue.front()!=nullptr && queue.front()->recordId.empty());

    MoveJob zeroOperation=MJ(MoveOwner::ManualTray,0,1102,"runtime");
    MoveJob zeroJob=MJ(MoveOwner::ManualTray,112,0,"runtime");
    MoveJob invalidOwner=MJ(static_cast<MoveOwner>(-1),112,1102,"runtime");
    MoveJob zeroDestination=MJ(MoveOwner::ManualTray,112,1102,"runtime");
    zeroDestination.destination=GUID{};
    MoveJob attempted=MJ(MoveOwner::ManualTray,112,1102,"runtime");
    attempted.attempts=1;
    MoveJob waiting=MJ(MoveOwner::ManualTray,112,1102,"runtime");
    waiting.waitingForVerify=true;
    MoveJob duplicateId=MJ(MoveOwner::Picker,999,1101,"different-runtime");
    CHECK(!queue.enqueue(zeroOperation));
    CHECK(!queue.enqueue(zeroJob));
    CHECK(!queue.enqueue(invalidOwner));
    CHECK(!queue.enqueue(zeroDestination));
    CHECK(!queue.enqueue(attempted));
    CHECK(!queue.enqueue(waiting));
    CHECK(!queue.enqueue(duplicateId));
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==1101);

    MoveResult cancelled=queue.cancelJob(1101);
    CHECK(cancelled.completed && cancelled.terminal==MoveTerminal::Cancelled);
    CHECK(queue.enqueue(duplicateId)); // uniqueness is required among live jobs
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==1101);
}

static void test_move_queue_allows_bounded_auto_with_manual_and_picker_jobs(){
    MoveQueue queue;
    const size_t autoJobCount=4096;
    bool acceptedAllAuto=true;
    for(size_t i=0;i<autoJobCount;++i){
        MoveJob job=MJ(MoveOwner::AutoReconcile,151,1501+i,"");
        job.recordId.clear();
        if(!queue.enqueue(job)) acceptedAllAuto=false;
    }
    CHECK(acceptedAllAuto);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==1501);

    MoveJob manual=MJ(MoveOwner::ManualTray,152,1000001,"shared-runtime");
    MoveJob picker=MJ(MoveOwner::Picker,153,1000002,"shared-runtime");
    const bool manualAccepted=queue.enqueue(manual);
    const bool pickerAccepted=queue.enqueue(picker);
    CHECK(manualAccepted);
    CHECK(pickerAccepted);
    if(manualAccepted){
        MoveResult cancelled=queue.cancelJob(manual.token.jobId);
        check_move_result(cancelled,MoveTerminal::Cancelled,manual,0);
    }
    if(pickerAccepted){
        MoveResult cancelled=queue.cancelJob(picker.token.jobId);
        check_move_result(cancelled,MoveTerminal::Cancelled,picker,0);
    }
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==1501);
}

static void test_move_queue_phase_guards_and_issue_outcomes(){
    MoveQueue queue;
    MoveJob job=MJ(MoveOwner::AutoReconcile,121,1201,"phase-runtime");
    CHECK(queue.enqueue(job));
    CHECK(queue.nextAction()==MoveAction::Issue);
    CHECK(queue.nextAction()==MoveAction::Issue);
    check_empty_move_result(queue.onVerified(MoveAttemptOutcome::OnDestination));
    CHECK(queue.front()!=nullptr && queue.front()->attempts==0);
    CHECK(queue.nextAction()==MoveAction::Issue);

    MoveResult issued=queue.onIssued(MoveAttemptOutcome::TransientFailure);
    CHECK(!issued.completed && issued.attempts==1);
    CHECK(queue.nextAction()==MoveAction::Verify);
    CHECK(queue.nextAction()==MoveAction::Verify);
    check_empty_move_result(queue.onIssued(MoveAttemptOutcome::Accepted));
    CHECK(queue.front()!=nullptr && queue.front()->attempts==1);
    CHECK(queue.front()!=nullptr && queue.front()->waitingForVerify);
    CHECK(queue.nextAction()==MoveAction::Verify);

    MoveResult retry=queue.onVerified(MoveAttemptOutcome::TransientFailure);
    CHECK(!retry.completed && retry.attempts==1);
    CHECK(queue.nextAction()==MoveAction::Issue);
    MoveResult alreadyThere=queue.onIssued(MoveAttemptOutcome::OnDestination);
    check_move_result(alreadyThere,MoveTerminal::Succeeded,job,2);
    CHECK(queue.empty());
}

static void test_move_queue_four_transient_issues_still_receive_four_verifies(){
    MoveQueue queue;
    MoveJob job=MJ(MoveOwner::AutoReconcile,131,1301,"ambiguous-issue");
    CHECK(queue.enqueue(job));
    for(int attempt=1;attempt<=4;++attempt){
        CHECK(queue.nextAction()==MoveAction::Issue);
        MoveResult issued=queue.onIssued(MoveAttemptOutcome::TransientFailure);
        CHECK(!issued.completed && issued.attempts==attempt);
        CHECK(queue.nextAction()==MoveAction::Verify);
        MoveResult verified=queue.onVerified(MoveAttemptOutcome::TransientFailure);
        if(attempt<4){
            CHECK(!verified.completed && verified.attempts==attempt);
            CHECK(queue.nextAction()==MoveAction::Issue);
        }else{
            check_move_result(verified,MoveTerminal::Exhausted,job,4);
        }
    }
    CHECK(queue.empty());
}

static void test_move_queue_invalid_outcomes_fail_closed(){
    MoveQueue queue;
    MoveJob invalidIssue=MJ(MoveOwner::AutoReconcile,141,1401,"invalid-issue");
    MoveJob acceptedVerify=MJ(MoveOwner::ManualTray,142,1402,"accepted-verify");
    MoveJob invalidVerify=MJ(MoveOwner::Picker,143,1403,"invalid-verify");
    CHECK(queue.enqueue(invalidIssue));
    CHECK(queue.enqueue(acceptedVerify));
    CHECK(queue.enqueue(invalidVerify));

    MoveResult first=queue.onIssued(static_cast<MoveAttemptOutcome>(-1));
    check_move_result(first,MoveTerminal::PermanentFailure,invalidIssue,1);
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    MoveResult second=queue.onVerified(MoveAttemptOutcome::Accepted);
    check_move_result(second,MoveTerminal::PermanentFailure,acceptedVerify,1);
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    MoveResult third=queue.onVerified(static_cast<MoveAttemptOutcome>(99));
    check_move_result(third,MoveTerminal::PermanentFailure,invalidVerify,1);
    CHECK(queue.empty());
}

static void test_move_queue_four_transient_cycles_exhaust_and_unblock_next(){
    MoveQueue queue;
    MoveJob failed=MJ(MoveOwner::AutoReconcile,201,2001,"failed-runtime");
    failed.token.itemIndex=3;
    MoveJob next=MJ(MoveOwner::AutoReconcile,201,2002,"healthy-runtime");
    CHECK(queue.enqueue(failed));
    CHECK(queue.enqueue(next));

    for(int attempt=1;attempt<=4;++attempt){
        CHECK(queue.nextAction()==MoveAction::Issue);
        MoveResult issued=queue.onIssued(MoveAttemptOutcome::Accepted);
        CHECK(!issued.completed && issued.attempts==attempt);
        CHECK(queue.nextAction()==MoveAction::Verify);
        MoveResult verified=queue.onVerified(MoveAttemptOutcome::TransientFailure);
        if(attempt<4){
            CHECK(!verified.completed && verified.attempts==attempt);
            CHECK(queue.nextAction()==MoveAction::Issue);
        }else{
            check_move_result(verified,MoveTerminal::Exhausted,failed,4);
        }
    }

    CHECK(!queue.empty());
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==next.token.jobId);
    CHECK(queue.nextAction()==MoveAction::Issue);
    MoveResult nextIssued=queue.onIssued(MoveAttemptOutcome::Accepted);
    CHECK(!nextIssued.completed && nextIssued.attempts==1);
    MoveResult nextVerified=queue.onVerified(MoveAttemptOutcome::OnDestination);
    check_move_result(nextVerified,MoveTerminal::Succeeded,next,1);
    CHECK(queue.empty());
}

static void test_move_queue_permanent_failure_finishes_and_unblocks_next(){
    MoveQueue queue;
    MoveJob invalidDestination=MJ(MoveOwner::ManualTray,301,3001,"bad-destination");
    MoveJob invalidIdentity=MJ(MoveOwner::Picker,302,3002,"bad-identity");
    MoveJob healthy=MJ(MoveOwner::AutoReconcile,303,3003,"healthy");
    CHECK(queue.enqueue(invalidDestination));
    CHECK(queue.enqueue(invalidIdentity));
    CHECK(queue.enqueue(healthy));

    MoveResult first=queue.onIssued(MoveAttemptOutcome::PermanentFailure);
    check_move_result(first,MoveTerminal::PermanentFailure,invalidDestination,1);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==invalidIdentity.token.jobId);
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    MoveResult second=queue.onVerified(MoveAttemptOutcome::PermanentFailure);
    check_move_result(second,MoveTerminal::PermanentFailure,invalidIdentity,1);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==healthy.token.jobId);

    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    MoveResult third=queue.onVerified(MoveAttemptOutcome::OnDestination);
    check_move_result(third,MoveTerminal::Succeeded,healthy,1);
    CHECK(queue.empty());
}

static void test_move_queue_cancel_job_is_identity_safe_during_verify(){
    MoveQueue queue;
    MoveJob automatic=MJ(MoveOwner::AutoReconcile,401,4001,"shared-runtime");
    MoveJob manual=MJ(MoveOwner::ManualTray,402,4002,"shared-runtime");
    CHECK(queue.enqueue(automatic));
    CHECK(queue.enqueue(manual));
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    CHECK(queue.nextAction()==MoveAction::Verify);

    MoveResult cancelled=queue.cancelJob(automatic.token.jobId);
    check_move_result(cancelled,MoveTerminal::Cancelled,automatic,1);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==manual.token.jobId);
    CHECK(queue.front()!=nullptr && queue.front()->attempts==0);
    CHECK(queue.nextAction()==MoveAction::Issue);

    MoveResult duplicate=queue.cancelJob(automatic.token.jobId);
    CHECK(!duplicate.completed && duplicate.terminal==MoveTerminal::None);
    MoveResult manualCancelled=queue.cancelJob(manual.token.jobId);
    check_move_result(manualCancelled,MoveTerminal::Cancelled,manual,0);
    CHECK(queue.empty());
}

static void test_move_queue_cancel_operation_is_owner_scoped_and_fifo(){
    MoveQueue queue;
    MoveJob autoCurrent=MJ(MoveOwner::AutoReconcile,501,5001,"same-runtime");
    MoveJob manualSameOperation=MJ(MoveOwner::ManualTray,501,5002,"same-runtime");
    MoveJob autoLater=MJ(MoveOwner::AutoReconcile,501,5003,"another-runtime");
    MoveJob autoOtherOperation=MJ(MoveOwner::AutoReconcile,502,5004,"same-runtime");
    MoveJob pickerSameOperation=MJ(MoveOwner::Picker,501,5005,"same-runtime");
    CHECK(queue.enqueue(autoCurrent));
    CHECK(queue.enqueue(manualSameOperation));
    CHECK(queue.enqueue(autoLater));
    CHECK(queue.enqueue(autoOtherOperation));
    CHECK(queue.enqueue(pickerSameOperation));
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    CHECK(queue.nextAction()==MoveAction::Verify);

    std::vector<MoveResult> cancelled=queue.cancelOperation(MoveOwner::AutoReconcile,501);
    CHECK(cancelled.size()==2);
    if(cancelled.size()==2){
        check_move_result(cancelled[0],MoveTerminal::Cancelled,autoCurrent,1);
        check_move_result(cancelled[1],MoveTerminal::Cancelled,autoLater,0);
    }
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==manualSameOperation.token.jobId);
    CHECK(queue.cancelOperation(MoveOwner::AutoReconcile,501).empty());

    MoveResult manual=queue.cancelJob(manualSameOperation.token.jobId);
    check_move_result(manual,MoveTerminal::Cancelled,manualSameOperation,0);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==autoOtherOperation.token.jobId);
    MoveResult other=queue.cancelJob(autoOtherOperation.token.jobId);
    check_move_result(other,MoveTerminal::Cancelled,autoOtherOperation,0);
    CHECK(queue.front()!=nullptr && queue.front()->token.jobId==pickerSameOperation.token.jobId);
    MoveResult picker=queue.cancelJob(pickerSameOperation.token.jobId);
    check_move_result(picker,MoveTerminal::Cancelled,pickerSameOperation,0);
    CHECK(queue.empty());
}

struct FakeMoveOperationKey {
    MoveOwner owner=MoveOwner::AutoReconcile;
    uint64_t operationId=0;
    bool operator<(const FakeMoveOperationKey& other) const {
        if(owner!=other.owner) return static_cast<int>(owner)<static_cast<int>(other.owner);
        return operationId<other.operationId;
    }
};

struct FakeMoveOperationState {
    size_t outstanding=0;
    size_t delivered=0;
    std::set<uint64_t> liveJobIds;
};

struct FakeMoveOwnerDispatcher {
    std::map<FakeMoveOperationKey,FakeMoveOperationState> operations;
    bool dispatch(const MoveResult& result){
        if(!result.completed) return false;
        FakeMoveOperationKey key{result.token.owner,result.token.operationId};
        auto operation=operations.find(key);
        if(operation==operations.end()) return false;
        if(operation->second.liveJobIds.erase(result.token.jobId)!=1) return false;
        if(operation->second.outstanding==0) return false;
        --operation->second.outstanding;
        ++operation->second.delivered;
        return true;
    }
};

static void test_move_queue_duplicate_owner_delivery_is_harmless(){
    MoveQueue queue;
    MoveJob automatic=MJ(MoveOwner::AutoReconcile,601,6001,"shared-runtime");
    MoveJob manual=MJ(MoveOwner::ManualTray,601,6002,"shared-runtime");
    CHECK(queue.enqueue(automatic));
    CHECK(queue.enqueue(manual));

    FakeMoveOwnerDispatcher dispatcher;
    FakeMoveOperationKey automaticKey{MoveOwner::AutoReconcile,601};
    FakeMoveOperationKey manualKey{MoveOwner::ManualTray,601};
    dispatcher.operations[automaticKey].outstanding=1;
    dispatcher.operations[automaticKey].liveJobIds.insert(automatic.token.jobId);
    dispatcher.operations[manualKey].outstanding=1;
    dispatcher.operations[manualKey].liveJobIds.insert(manual.token.jobId);

    MoveResult cancelled=queue.cancelJob(automatic.token.jobId);
    CHECK(dispatcher.dispatch(cancelled));
    CHECK(!dispatcher.dispatch(cancelled));
    CHECK(dispatcher.operations[automaticKey].outstanding==0);
    CHECK(dispatcher.operations[automaticKey].delivered==1);
    CHECK(dispatcher.operations[manualKey].outstanding==1);
    CHECK(dispatcher.operations[manualKey].delivered==0);
    CHECK(dispatcher.operations[manualKey].liveJobIds.count(manual.token.jobId)==1);

    MoveResult manualCancelled=queue.cancelJob(manual.token.jobId);
    CHECK(dispatcher.dispatch(manualCancelled));
    CHECK(dispatcher.operations[manualKey].outstanding==0);
    CHECK(dispatcher.operations[manualKey].delivered==1);
}

static MoveResult TerminalMoveResult(const MoveToken& token,MoveTerminal terminal){
    MoveResult result;
    result.completed=true;
    result.terminal=terminal;
    result.token=token;
    return result;
}

static void test_move_operation_dispatcher_is_job_and_owner_scoped(){
    MoveOperationDispatcher dispatcher;
    MoveToken automaticA{MoveOwner::AutoReconcile,701,7001,0};
    MoveToken automaticB{MoveOwner::AutoReconcile,701,7002,1};
    MoveToken manual{MoveOwner::ManualTray,701,7003,0};
    CHECK(dispatcher.begin(MoveOwner::AutoReconcile,701,{automaticA,automaticB}));
    CHECK(dispatcher.begin(MoveOwner::ManualTray,701,{manual}));

    MoveOperationSummary completion;
    CHECK(dispatcher.dispatch(
        TerminalMoveResult(automaticA,MoveTerminal::Succeeded),completion)==
        MoveDispatchDisposition::Accepted);
    MoveOperationSummary partial;
    CHECK(dispatcher.lookup(MoveOwner::AutoReconcile,701,partial));
    CHECK(partial.outstanding==1 && partial.succeeded==1 && !partial.complete());

    MoveToken forged=automaticB;
    forged.owner=MoveOwner::ManualTray;
    CHECK(dispatcher.dispatch(
        TerminalMoveResult(forged,MoveTerminal::Cancelled),completion)==
        MoveDispatchDisposition::Stale);
    CHECK(dispatcher.containsJob(automaticB.jobId));

    CHECK(dispatcher.dispatch(
        TerminalMoveResult(automaticB,MoveTerminal::PermanentFailure),completion)==
        MoveDispatchDisposition::OperationCompleted);
    CHECK(completion.expected==2 && completion.outstanding==0);
    CHECK(completion.succeeded==1 && completion.permanentFailures==1);
    CHECK(dispatcher.dispatch(
        TerminalMoveResult(automaticB,MoveTerminal::PermanentFailure),completion)==
        MoveDispatchDisposition::Stale);

    CHECK(dispatcher.dispatch(
        TerminalMoveResult(manual,MoveTerminal::Succeeded),completion)==
        MoveDispatchDisposition::OperationCompleted);
    CHECK(completion.expected==1 && completion.succeeded==1);
}

static void test_move_operation_dispatcher_cancellation_completes_each_job_once(){
    MoveOperationDispatcher dispatcher;
    MoveToken first{MoveOwner::Picker,702,7011,0};
    MoveToken second{MoveOwner::Picker,702,7012,1};
    CHECK(dispatcher.begin(MoveOwner::Picker,702,{first,second}));
    std::vector<uint64_t> cancelledJobs;
    MoveOperationSummary completion;
    CHECK(dispatcher.cancelOperation(
        MoveOwner::Picker,702,cancelledJobs,completion));
    CHECK(cancelledJobs.size()==2);
    CHECK(cancelledJobs[0]==first.jobId && cancelledJobs[1]==second.jobId);
    CHECK(completion.complete() && completion.cancelled==2);
    CHECK(!dispatcher.cancelOperation(
        MoveOwner::Picker,702,cancelledJobs,completion));
    CHECK(cancelledJobs.empty());
    CHECK(dispatcher.dispatch(
        TerminalMoveResult(first,MoveTerminal::Cancelled),completion)==
        MoveDispatchDisposition::Stale);

    MoveToken duplicateItemA{MoveOwner::AutoReconcile,703,7021,0};
    MoveToken duplicateItemB{MoveOwner::AutoReconcile,703,7022,0};
    CHECK(!dispatcher.begin(MoveOwner::AutoReconcile,703,
        {duplicateItemA,duplicateItemB}));
    std::vector<MoveToken> oversized;
    for(size_t i=0;i<4097;++i)
        oversized.push_back(MoveToken{
            MoveOwner::AutoReconcile,704,7100+i,i});
    CHECK(!dispatcher.begin(MoveOwner::AutoReconcile,704,oversized));
}

static void test_move_reservation_replacement_requires_exact_terminal_token(){
    MoveReservationBook reservations;
    WindowIdentityKey identity;
    identity.hwnd=0x701;
    identity.pid=1701;
    identity.processStart=2701;

    MoveReservation automatic;
    automatic.token=MoveToken{MoveOwner::AutoReconcile,801,8001,2};
    automatic.identity=identity;
    automatic.boundRecordId="{00000000-0000-0000-0000-000000008001}";
    automatic.hasProvisionalOriginRecord=true;
    automatic.provisionalOriginRecord.recordId=
        "{00000000-0000-0000-0000-000000008002}";
    automatic.provisionalOriginRecord.app="firefox";
    automatic.provisionalOriginRecord.desktop=
        G(L"{231A0000-0000-0000-0000-000000000801}");

    MoveReservation displaced;
    CHECK(reservations.reserve(automatic,&displaced)==
        MoveReservationUpdate::Inserted);
    MoveReservation stored;
    CHECK(reservations.lookup(identity,stored));
    CHECK(SameMoveToken(stored.token,automatic.token));
    CHECK(stored.identity.processStart==2701);
    CHECK(stored.boundRecordId==automatic.boundRecordId);
    CHECK(stored.hasProvisionalOriginRecord &&
        stored.provisionalOriginRecord.recordId==
            automatic.provisionalOriginRecord.recordId);

    MoveReservation manual=automatic;
    manual.token=MoveToken{MoveOwner::ManualTray,802,8002,0};
    manual.boundRecordId.clear();
    manual.hasProvisionalOriginRecord=false;
    CHECK(reservations.reserve(manual,&displaced)==
        MoveReservationUpdate::Replaced);
    CHECK(SameMoveToken(displaced.token,automatic.token));
    CHECK(reservations.size()==1);
    std::vector<MoveReservation> snapshot;
    CHECK(reservations.snapshot(snapshot));
    CHECK(snapshot.size()==1 && SameMoveToken(snapshot[0].token,manual.token));

    CHECK(!reservations.erase(identity,automatic.token));
    MoveToken wrongGeneration=manual.token;
    ++wrongGeneration.operationId;
    CHECK(!reservations.erase(identity,wrongGeneration));
    CHECK(reservations.lookup(identity,stored) &&
        SameMoveToken(stored.token,manual.token));
    CHECK(reservations.erase(identity,manual.token));
    CHECK(!reservations.erase(identity,manual.token));
    CHECK(reservations.empty());
}

static void test_issued_reservation_transfer_has_no_checkpoint_gap(){
    CheckpointController checkpoint;
    int heartbeatCalls=0;
    bool protectionVisible=false;
    auto heartbeat=[&](CheckpointReason reason){
        CHECK(reason==CheckpointReason::Heartbeat);
        CHECK(protectionVisible);
        ++heartbeatCalls;
        return true;
    };

    MoveReservationBook reservations;
    MoveReservation old;
    old.token.owner=MoveOwner::AutoReconcile;
    old.token.operationId=6101;
    old.token.jobId=6102;
    old.identity={0x6103,6104,6105};
    old.boundRecordId="{00000000-0000-0000-0000-000000006101}";
    old.originDesktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    CHECK(reservations.reserve(old)==MoveReservationUpdate::Inserted);
    protectionVisible=true;
    CHECK(checkpoint.dispatch(
        CheckpointReason::Heartbeat,true,true,true,heartbeat));
    CHECK(checkpoint.heartbeatDeferred && heartbeatCalls==0);

    MoveReservation replacement=old;
    replacement.token.owner=MoveOwner::ManualTray;
    replacement.token.operationId=6201;
    replacement.token.jobId=6202;
    MoveReservation displaced;
    CHECK(reservations.reserve(replacement,&displaced)==
          MoveReservationUpdate::Replaced);
    CHECK(SameMoveToken(displaced.token,old.token));
    CHECK(!reservations.erase(old.token));
    CHECK(checkpoint.acknowledgeReservationBeforeRelease(
        false,reservations.size()==1,true,true,heartbeat));
    CHECK(checkpoint.heartbeatDeferred && heartbeatCalls==0);

    // The successor completes with an exact readback.  Normal completion
    // publishes owner state first, erases the guard, then runs the heartbeat.
    CHECK(reservations.erase(replacement.token));
    protectionVisible=false;
    bool ownerStateSafe=true;
    CHECK(checkpoint.reservationTerminated(
        true,!reservations.empty(),true,true,[&](CheckpointReason reason){
            CHECK(reason==CheckpointReason::Heartbeat);
            CHECK(ownerStateSafe && !protectionVisible);
            ++heartbeatCalls;
            return true;
        }));
    CHECK(heartbeatCalls==1);
}

static void test_successor_handoff_publishes_before_issued_displaced_cancel(){
    MoveQueue queue;
    MoveJob displaced=MJ(MoveOwner::AutoReconcile,6151,6152,"same-runtime");
    MoveJob successor=MJ(MoveOwner::Picker,6161,6162,"same-runtime");
    CHECK(queue.enqueue(displaced));
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    CHECK(queue.nextAction()==MoveAction::Verify);

    MoveToken visibleGuard=displaced.token;
    bool displacedOwnerAlive=true,successorOwner=false,successorRuntime=false;
    bool successorQueued=false,displacedRetiring=false;
    int cancellationCalls=0,rollbackCalls=0;

    // Owner publication failure is entirely pre-commit.  The issued job,
    // owner, and old guard remain byte-for-byte intact.
    CHECK(!RunSuccessorFirstReservationHandoff([&](){
        successorRuntime=true;
        return false;
    },[&]() noexcept {
        visibleGuard=successor.token;
    },[&](){ ++cancellationCalls; },[&](){
        ++rollbackCalls;
        successorRuntime=false;
    }));
    CHECK(rollbackCalls==1 && cancellationCalls==0 && !successorRuntime);
    CHECK(displacedOwnerAlive && SameMoveToken(visibleGuard,displaced.token));
    CHECK(queue.front() && SameMoveToken(queue.front()->token,displaced.token) &&
          queue.nextAction()==MoveAction::Verify);

    // A failed final enqueue has the same property: cancellation is not even
    // requested until the successor is wholly published.
    CHECK(!RunSuccessorFirstReservationHandoff([&](){
        successorRuntime=true;
        successorOwner=true;
        return false; // injected enqueue=false
    },[&]() noexcept {
        visibleGuard=successor.token;
    },[&](){ ++cancellationCalls; },[&](){
        ++rollbackCalls;
        successorOwner=false;
        successorRuntime=false;
    }));
    CHECK(rollbackCalls==2 && cancellationCalls==0 && !successorOwner &&
          !successorRuntime && SameMoveToken(visibleGuard,displaced.token));
    CHECK(queue.front() && SameMoveToken(queue.front()->token,displaced.token) &&
          queue.nextAction()==MoveAction::Verify);

    std::vector<std::string> events;
    CHECK(RunSuccessorFirstReservationHandoff([&](){
        events.push_back("owner");
        successorOwner=true;
        successorRuntime=true;
        CHECK(queue.enqueue(successor));
        successorQueued=true;
        events.push_back("enqueue");
        return true;
    },[&]() noexcept {
        CHECK(successorOwner && successorRuntime && successorQueued);
        visibleGuard=successor.token;
        events.push_back("guard");
    },[&](){
        ++cancellationCalls;
        CHECK(SameMoveToken(visibleGuard,successor.token));
        CHECK(queue.front() && SameMoveToken(queue.front()->token,displaced.token));
        CHECK(queue.nextAction()==MoveAction::Verify);
        displacedRetiring=true;
        events.push_back("cancel");
    },[&](){ ++rollbackCalls; }));
    CHECK((events==std::vector<std::string>{"owner","enqueue","guard","cancel"}));
    CHECK(cancellationCalls==1 && rollbackCalls==2 && displacedRetiring);
    CHECK(displacedOwnerAlive && successorOwner && successorRuntime &&
          successorQueued && SameMoveToken(visibleGuard,successor.token));
}

static void test_issued_reservation_rollback_waits_for_terminal_ack(){
    CHECK(MoveCancellationDispositionFor(false,true)==
          MoveCancellationDisposition::CancelImmediately);
    CHECK(MoveCancellationDispositionFor(true,false)==
          MoveCancellationDisposition::CancelImmediately);
    CHECK(MoveCancellationDispositionFor(true,true)==
          MoveCancellationDisposition::AwaitTerminalAcknowledgement);
    CHECK(MoveCancellationDispositionFor(true,WindowIdentityRecapture::Match)==
          MoveCancellationDisposition::AwaitTerminalAcknowledgement);
    CHECK(MoveCancellationDispositionFor(
              true,WindowIdentityRecapture::Indeterminate)==
          MoveCancellationDisposition::AwaitTerminalAcknowledgement);
    CHECK(MoveCancellationDispositionFor(true,WindowIdentityRecapture::Lost)==
          MoveCancellationDisposition::CancelImmediately);

    IdentityRecaptureRetryBudget unknownIdentity;
    for(unsigned check=1;
        check<IdentityRecaptureRetryBudget::kMaxIndeterminateChecks;++check)
        CHECK(unknownIdentity.observe(WindowIdentityRecapture::Indeterminate)==
              IdentityRecaptureRetryAction::Retry);
    CHECK(unknownIdentity.observe(WindowIdentityRecapture::Indeterminate)==
          IdentityRecaptureRetryAction::RetireCancelled);
    CHECK(unknownIdentity.observe(WindowIdentityRecapture::Match)==
          IdentityRecaptureRetryAction::Continue);
    unknownIdentity.reset();
    CHECK(unknownIdentity.observe(WindowIdentityRecapture::Indeterminate)==
          IdentityRecaptureRetryAction::Retry);

    MoveQueue unknownQueue;
    MoveJob unknownFirst,unknownSecond;
    unknownFirst.token={MoveOwner::ManualTray,6250,6251,0};
    unknownFirst.runtimeKey="unknown-first";
    unknownFirst.recordId="record-first";
    unknownFirst.destination=G(L"{231A0000-0000-0000-0000-000000000001}");
    unknownSecond=unknownFirst;
    unknownSecond.token.jobId=6252;
    unknownSecond.token.itemIndex=1;
    unknownSecond.runtimeKey="unknown-second";
    unknownSecond.recordId="record-second";
    CHECK(unknownQueue.enqueue(unknownFirst));
    CHECK(unknownQueue.enqueue(unknownSecond));
    IdentityRecaptureRetryBudget firstBudget;
    for(unsigned check=0;
        check<IdentityRecaptureRetryBudget::kMaxIndeterminateChecks;++check){
        const IdentityRecaptureRetryAction action=
            firstBudget.observe(WindowIdentityRecapture::Indeterminate);
        if(action==IdentityRecaptureRetryAction::RetireCancelled){
            MoveResult retiredUnknown=unknownQueue.cancelJob(
                unknownFirst.token.jobId);
            CHECK(retiredUnknown.completed &&
                  retiredUnknown.terminal==MoveTerminal::Cancelled);
        }
    }
    CHECK(unknownQueue.front() &&
          unknownQueue.front()->token.jobId==unknownSecond.token.jobId);

    CheckpointController checkpoint;
    MoveReservationBook reservations;
    MoveReservation old;
    old.token.owner=MoveOwner::AutoReconcile;
    old.token.operationId=6301;
    old.token.jobId=6302;
    old.identity={0x6303,6304,6305};
    old.boundRecordId="{00000000-0000-0000-0000-000000006301}";
    old.originDesktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    old.hasProvisionalOriginRecord=true;
    old.provisionalOriginRecord.recordId=old.boundRecordId;
    old.provisionalOriginRecord.app="firefox";
    old.provisionalOriginRecord.desktop=old.originDesktop;
    old.provisionalOriginRecord.lastSeenUtc=1700000000;
    CHECK(reservations.reserve(old)==MoveReservationUpdate::Inserted);
    CHECK(checkpoint.dispatch(CheckpointReason::Heartbeat,true,true,true,
        [](CheckpointReason){ return false; }));

    MoveReservation replacement=old;
    replacement.token.owner=MoveOwner::Picker;
    replacement.token.operationId=6401;
    replacement.token.jobId=6402;
    CHECK(reservations.reserve(replacement)==MoveReservationUpdate::Replaced);
    CHECK(reservations.reserve(old)==MoveReservationUpdate::Replaced);

    int heartbeatCalls=0;
    CHECK(checkpoint.acknowledgeReservationBeforeRelease(
        true,reservations.size()==1,true,true,[&](CheckpointReason){
            MoveReservation protectedOrigin;
            CHECK(reservations.lookup(old.identity,protectedOrigin));
            CHECK(protectedOrigin.hasProvisionalOriginRecord);
            CHECK(GuidEq(protectedOrigin.provisionalOriginRecord.desktop,
                         old.originDesktop));
            ++heartbeatCalls;
            return true;
        }));
    CHECK(heartbeatCalls==1 && reservations.erase(old.token));
    CHECK(!reservations.erase(old.token));

    // Failed/throwing checkpoint callbacks are consumed exactly once so the
    // terminal acknowledgement can always release its guard.
    for(int mode=0;mode!=2;++mode){
        CheckpointController failed;
        MoveReservationBook protectedReservations;
        CHECK(protectedReservations.reserve(old)==MoveReservationUpdate::Inserted);
        CHECK(failed.dispatch(CheckpointReason::Heartbeat,true,true,true,
            [](CheckpointReason){ return true; }));
        int calls=0;
        const bool saved=failed.acknowledgeReservationBeforeRelease(
            true,true,true,true,[&](CheckpointReason)->bool{
                ++calls;
                if(mode) throw std::runtime_error("injected checkpoint failure");
                return false;
            });
        CHECK(!saved && calls==1 && !failed.heartbeatDeferred);
        CHECK(protectedReservations.erase(old.token));
        CHECK(failed.acknowledgeReservationBeforeRelease(
            false,true,true,true,[&](CheckpointReason){ ++calls; return true; }));
        CHECK(calls==1);
    }

    MoveQueue queue;
    MoveReservationBook retiringReservations;
    CHECK(retiringReservations.reserve(old)==MoveReservationUpdate::Inserted);
    CheckpointController retiringCheckpoint;
    int retiringHeartbeatCalls=0;
    CHECK(retiringCheckpoint.dispatch(CheckpointReason::Heartbeat,
        true,true,true,[&](CheckpointReason){
            ++retiringHeartbeatCalls;
            return true;
        }));
    MoveJob issued;
    issued.token=old.token;
    issued.runtimeKey=RuntimeKey(old.identity);
    issued.recordId=old.boundRecordId;
    issued.destination=old.originDesktop;
    CHECK(queue.enqueue(issued));
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    CHECK(queue.nextAction()==MoveAction::Verify);
    IssuedMoveRetirementTracker retirement;
    for(unsigned check=1;
        check<IssuedMoveRetirementTracker::kMaxUnresolvedReadbacks;++check){
        CHECK(retirement.observe(true,MoveAttemptOutcome::TransientFailure)==
              IssuedMoveRetirementAction::WaitForReadback);
        CHECK(queue.nextAction()==MoveAction::Verify);
        CHECK(queue.front() && queue.front()->attempts==1);
        MoveReservation stillProtected;
        CHECK(retiringReservations.lookup(old.identity,stillProtected));
        CHECK(retiringHeartbeatCalls==0 &&
              retiringCheckpoint.heartbeatDeferred);
    }
    CHECK(retirement.observe(true,MoveAttemptOutcome::TransientFailure)==
          IssuedMoveRetirementAction::ConsumeProtectedCheckpointAndCancel);
    CHECK(retiringCheckpoint.acknowledgeReservationBeforeRelease(
        true,retiringReservations.size()==1,true,true,[&](CheckpointReason){
            MoveReservation protectedOrigin;
            CHECK(retiringReservations.lookup(old.identity,protectedOrigin));
            CHECK(GuidEq(protectedOrigin.originDesktop,old.originDesktop));
            ++retiringHeartbeatCalls;
            return true;
        }));
    MoveResult retired=queue.cancelJob(old.token.jobId);
    CHECK(retired.completed && retired.terminal==MoveTerminal::Cancelled);
    CHECK(retiringReservations.erase(old.token));
    CHECK(queue.empty() && retiringHeartbeatCalls==1);

    IssuedMoveRetirementTracker exact;
    CHECK(exact.observe(true,MoveAttemptOutcome::OnDestination)==
          IssuedMoveRetirementAction::CancelAfterSafeReadback);
    IssuedMoveRetirementTracker vanished;
    CHECK(vanished.observe(false,MoveAttemptOutcome::TransientFailure)==
          IssuedMoveRetirementAction::CancelAfterSafeReadback);

    // A timer-arm failure uses the same protected terminal path: the guard is
    // present during the single deferred checkpoint and always removed.
    MoveReservationBook timerReservations;
    CHECK(timerReservations.reserve(old)==MoveReservationUpdate::Inserted);
    CheckpointController timerCheckpoint;
    int timerHeartbeatCalls=0,timerCancelCalls=0;
    CHECK(timerCheckpoint.dispatch(CheckpointReason::Heartbeat,
        true,true,true,[](CheckpointReason){ return true; }));
    CHECK(!ArmMoveWorkOrCancel(true,[](){ return false; },[&](){
        ++timerCancelCalls;
        CHECK(timerCheckpoint.acknowledgeReservationBeforeRelease(
            true,timerReservations.size()==1,true,true,[&](CheckpointReason){
                MoveReservation protectedOrigin;
                CHECK(timerReservations.lookup(old.identity,protectedOrigin));
                ++timerHeartbeatCalls;
                return true;
            }));
        CHECK(timerReservations.erase(old.token));
    }));
    CHECK(timerCancelCalls==1 && timerHeartbeatCalls==1 &&
          timerReservations.empty());

    // A failed terminal cannot drop its origin merely because an unrelated
    // reservation remains.  Consume the one deferred checkpoint while both
    // guards are visible; later terminals must not replay it.
    MoveReservation sibling=old;
    sibling.token.operationId=6501;
    sibling.token.jobId=6502;
    sibling.identity.hwnd=0x6503;
    MoveReservationBook multiple;
    CHECK(multiple.reserve(old)==MoveReservationUpdate::Inserted);
    CHECK(multiple.reserve(sibling)==MoveReservationUpdate::Inserted);
    CheckpointController multiCheckpoint;
    CHECK(multiCheckpoint.dispatch(CheckpointReason::Heartbeat,
        true,true,true,[](CheckpointReason){ return true; }));
    int multiCalls=0;
    CHECK(multiCheckpoint.acknowledgeReservationBeforeRelease(
        true,false,true,true,[&](CheckpointReason){
            MoveReservation first,second;
            CHECK(multiple.lookup(old.identity,first));
            CHECK(multiple.lookup(sibling.identity,second));
            ++multiCalls;
            return true;
        }));
    CHECK(multiple.erase(old.token));
    CHECK(multiCheckpoint.reservationTerminated(
        false,true,true,true,[&](CheckpointReason){ ++multiCalls; return true; }));
    CHECK(multiple.erase(sibling.token));
    CHECK(multiCheckpoint.reservationTerminated(
        true,false,true,true,[&](CheckpointReason){ ++multiCalls; return true; }));
    CHECK(multiCalls==1);
}

static AsyncSessionRoute SessionRoute(uint64_t requestId,uint64_t operationId,
        const char* app,SessionPurpose purpose,uint64_t generation,
        uint64_t deadlineMs){
    AsyncSessionRoute route;
    route.requestId=requestId;
    route.operationId=operationId;
    route.app=app;
    route.purpose=purpose;
    route.identityGeneration=generation;
    route.deadlineMs=deadlineMs;
    return route;
}

static void test_async_session_route_protects_manual_work_and_retires_once(){
    AsyncSessionRouteGate routes;
    std::vector<AsyncSessionRetirement> retired;
    AsyncSessionRoute save=SessionRoute(
        9001,901,"firefox",SessionPurpose::ManualSave,11,1000);
    CHECK(routes.submit(save,100,retired)==AsyncRouteAdmission::Accepted);
    CHECK(retired.empty() && routes.outstanding()==1);

    AsyncSessionRoute probe=SessionRoute(
        9002,902,"firefox",SessionPurpose::MetadataProbe,11,1000);
    CHECK(routes.submit(probe,100,retired)==
        AsyncRouteAdmission::RejectedProtected);
    CHECK(retired.size()==1 && retired[0].route.requestId==probe.requestId);
    CHECK(retired[0].reason==AsyncRetirementReason::Rejected);
    CHECK(routes.outstanding()==1);
    CHECK(routes.submit(probe,100,retired)==AsyncRouteAdmission::Stale);
    CHECK(retired.empty());

    AsyncSessionRoute restore=SessionRoute(
        9003,903,"firefox",SessionPurpose::ManualRestore,12,1200);
    CHECK(routes.submit(restore,100,retired)==AsyncRouteAdmission::Accepted);
    CHECK(retired.size()==1 && retired[0].route.requestId==save.requestId);
    CHECK(retired[0].reason==AsyncRetirementReason::Superseded);
    CHECK(!routes.retire(save.requestId,save.operationId,
        save.identityGeneration,AsyncRetirementReason::Completed,retired));
    CHECK(retired.empty());
    CHECK(routes.retire(restore.requestId,restore.operationId,
        restore.identityGeneration,AsyncRetirementReason::Completed,retired));
    CHECK(retired.size()==1 && retired[0].reason==AsyncRetirementReason::Completed);
    CHECK(!routes.retire(restore.requestId,restore.operationId,
        restore.identityGeneration,AsyncRetirementReason::Completed,retired));
    CHECK(retired.empty() && routes.outstanding()==0);
}

static void test_async_session_route_timeout_and_cancel_are_exact(){
    AsyncSessionRouteGate routes;
    std::vector<AsyncSessionRetirement> retired;
    AsyncSessionRoute firefox=SessionRoute(
        9101,911,"firefox",SessionPurpose::AutoReconcile,21,200);
    AsyncSessionRoute chrome=SessionRoute(
        9102,911,"chrome",SessionPurpose::AutoReconcile,22,300);
    AsyncSessionRoute edgeSearch=SessionRoute(
        9103,911,"msedge",SessionPurpose::Search,23,400);
    CHECK(routes.submit(firefox,100,retired)==AsyncRouteAdmission::Accepted);
    CHECK(routes.submit(chrome,100,retired)==AsyncRouteAdmission::Accepted);
    CHECK(routes.submit(edgeSearch,100,retired)==AsyncRouteAdmission::Accepted);
    CHECK(routes.expire(250,retired)==1);
    CHECK(retired.size()==1 && retired[0].route.requestId==firefox.requestId);
    CHECK(retired[0].reason==AsyncRetirementReason::TimedOut);
    CHECK(routes.expire(250,retired)==0 && retired.empty());
    CHECK(routes.cancelOperation(
        SessionPurpose::AutoReconcile,911,retired)==1);
    CHECK(retired.size()==1 && retired[0].route.requestId==chrome.requestId);
    CHECK(retired[0].reason==AsyncRetirementReason::Cancelled);
    CHECK(routes.cancelOperation(
        SessionPurpose::AutoReconcile,911,retired)==0 && retired.empty());
    CHECK(routes.outstanding()==1);
    CHECK(routes.cancelOperation(SessionPurpose::Search,911,retired)==1);
    CHECK(retired.size()==1 && retired[0].route.requestId==edgeSearch.requestId);
    CHECK(!routes.retire(chrome.requestId,chrome.operationId,
        chrome.identityGeneration,AsyncRetirementReason::Completed,retired));
}

static void test_async_session_route_bounds_deadlines_and_retires_capacity(){
    std::vector<AsyncSessionRetirement> retired;
    AsyncSessionRouteGate deadlines;
    const uint64_t now=100;
    AsyncSessionRoute boundary=SessionRoute(
        9201,921,"firefox",SessionPurpose::MetadataProbe,31,
        now+AsyncSessionRouteGate::maxLifetimeMs());
    CHECK(deadlines.submit(boundary,now,retired)==AsyncRouteAdmission::Accepted);
    CHECK(deadlines.cancelOperation(
        SessionPurpose::MetadataProbe,921,retired)==1);
    AsyncSessionRoute farRoute=SessionRoute(
        9202,922,"firefox",SessionPurpose::MetadataProbe,31,
        (std::numeric_limits<uint64_t>::max)());
    CHECK(deadlines.submit(farRoute,now,retired)==
        AsyncRouteAdmission::RejectedDeadline);
    CHECK(retired.size()==1 && retired[0].route.requestId==farRoute.requestId &&
        retired[0].reason==AsyncRetirementReason::Rejected);
    CHECK(deadlines.submit(farRoute,now,retired)==AsyncRouteAdmission::Stale);
    CHECK(retired.empty());

    AsyncSessionRouteGate capacity;
    for(uint64_t i=0;i<16;++i){
        const std::string app="bounded-app-"+std::to_string(i);
        CHECK(capacity.submit(SessionRoute(
            9300+i,9400+i,app.c_str(),SessionPurpose::MetadataProbe,
            40+i,now+1000),now,retired)==AsyncRouteAdmission::Accepted);
    }
    AsyncSessionRoute overflow=SessionRoute(
        9316,9416,"bounded-app-overflow",SessionPurpose::MetadataProbe,
        56,now+1000);
    CHECK(capacity.submit(overflow,now,retired)==
        AsyncRouteAdmission::RejectedCapacity);
    CHECK(retired.size()==1 &&
        retired[0].route.requestId==overflow.requestId &&
        retired[0].reason==AsyncRetirementReason::Rejected);
    CHECK(capacity.submit(overflow,now,retired)==AsyncRouteAdmission::Stale);
    CHECK(retired.empty() && capacity.outstanding()==16);
}

static void test_dirty_flush_is_coalesced_bounded_and_retries_without_spin(){
    DirtyFlushController dirty;
    dirty.markDirty(100);
    CHECK(dirty.dirty() && dirty.dueAtMs()==600);
    dirty.markDirty(400);
    CHECK(dirty.dueAtMs()==600);
    int writes=0;
    CHECK(dirty.flush(599,false,[&]{ ++writes; return true; })==
        DirtyFlushResult::Deferred);
    CHECK(writes==0);
    CHECK(dirty.flush(600,false,[&]{ ++writes; return false; })==
        DirtyFlushResult::Failed);
    CHECK(writes==1 && dirty.dirty() && dirty.dueAtMs()==1100);
    CHECK(dirty.flush(600,false,[&]{ ++writes; return true; })==
        DirtyFlushResult::Deferred);
    CHECK(writes==1);

    dirty.setConflict(true,1000);
    CHECK(dirty.flush(1100,false,[&]{ ++writes; return true; })==
        DirtyFlushResult::ConflictSuppressed);
    CHECK(writes==1 && dirty.dirty());
    CHECK(dirty.flush(1100,true,[&]{ ++writes; return false; })==
        DirtyFlushResult::Failed);
    CHECK(writes==2 && dirty.dirty() && dirty.conflicted());
    CHECK(dirty.flush(1200,true,[&]{ ++writes; return true; })==
        DirtyFlushResult::Succeeded);
    CHECK(writes==3 && !dirty.dirty() && !dirty.conflicted());
}

static void test_move_timer_failure_cancels_accepted_work_once(){
    int armCalls=0,cancelCalls=0;
    CHECK(!ArmMoveWorkOrCancel(true,[&](){ ++armCalls; return false; },
        [&](){ ++cancelCalls; }));
    CHECK(armCalls==1 && cancelCalls==1);
    CHECK(ArmMoveWorkOrCancel(false,[&](){ ++armCalls; return false; },
        [&](){ ++cancelCalls; }));
    CHECK(armCalls==1 && cancelCalls==1);
    CHECK(ArmMoveWorkOrCancel(true,[&](){ ++armCalls; return true; },
        [&](){ ++cancelCalls; }));
    CHECK(armCalls==2 && cancelCalls==1);

    int queued=2,cancelAttempts=0,rearmCalls=0;
    bool injectThrow=true;
    CHECK(RecoverMoveArmFailure([&](){
        ++cancelAttempts;
        if(injectThrow){ injectThrow=false; throw std::bad_alloc(); }
        --queued;
        return queued==0;
    },[&](){ ++rearmCalls; return true; })==
        MoveArmFailureCleanup::Rearmed);
    CHECK(queued==2 && cancelAttempts==1 && rearmCalls==1);
    CHECK(RecoverMoveArmFailure([&](){
        ++cancelAttempts;
        --queued;
        return queued==0;
    },[&](){ ++rearmCalls; return true; })==
        MoveArmFailureCleanup::Rearmed);
    CHECK(queued==1 && rearmCalls==2);
    CHECK(RecoverMoveArmFailure([&](){
        ++cancelAttempts;
        --queued;
        return queued==0;
    },[&](){ ++rearmCalls; return false; })==
        MoveArmFailureCleanup::Completed);
    CHECK(queued==0 && rearmCalls==2);

    CHECK(RecoverMoveArmFailure([](){ throw std::bad_alloc(); return false; },
        [](){ return false; })==MoveArmFailureCleanup::Unresolved);

    MoveToken retainedToken{MoveOwner::ManualTray,9901,9902,0};
    MoveOperationDispatcher retainedOwner;
    CHECK(retainedOwner.begin(MoveOwner::ManualTray,9901,{retainedToken}));
    CHECK(RecoverMoveArmFailure([](){
        throw std::bad_alloc();
        return false;
    },[](){ return true; })==MoveArmFailureCleanup::Rearmed);
    MoveOperationSummary retainedSummary;
    CHECK(retainedOwner.lookup(MoveOwner::ManualTray,9901,retainedSummary));
    CHECK(retainedSummary.outstanding==1 &&
          retainedOwner.containsJob(retainedToken.jobId));
    MoveResult exactCancellation;
    exactCancellation.completed=true;
    exactCancellation.terminal=MoveTerminal::Cancelled;
    exactCancellation.token=retainedToken;
    CHECK(retainedOwner.dispatch(exactCancellation,retainedSummary)==
          MoveDispatchDisposition::OperationCompleted);
    CHECK(!retainedOwner.lookup(MoveOwner::ManualTray,9901,retainedSummary));
}

static void test_move_cancellation_gate_precedes_fallible_cleanup(){
    bool cancellationPending=false;
    bool cancelRequested[3]={false,false,false};
    const uint64_t jobIds[3]={11001,11002,11003};
    bool threw=false;
    try {
        CHECK(PublishMoveCancellationIntent(
            cancellationPending,jobIds,3,[&](uint64_t jobId) noexcept {
                CHECK(jobId>=11001 && jobId<=11003);
                cancelRequested[jobId-11001]=true;
            }));
        throw std::bad_alloc(); // injected route/payload cleanup failure
    } catch(const std::bad_alloc&) { threw=true; }
    CHECK(threw && cancellationPending);
    CHECK(cancelRequested[0] && cancelRequested[1] && cancelRequested[2]);

    cancellationPending=false;
    CHECK(!PublishMoveCancellationIntent(
        cancellationPending,nullptr,1,[](uint64_t) noexcept {}));
    CHECK(!cancellationPending);
}

static void test_move_terminal_state_is_prepared_before_publication(){
    MoveTerminalOutcomes outcomes;
    CHECK(outcomes.initialize(3));
    CHECK(outcomes.size()==3 && !outcomes.succeeded(0));
    CHECK(outcomes.markSucceeded(1));
    CHECK(outcomes.succeeded(1) && !outcomes.succeeded(2));
    CHECK(!outcomes.markSucceeded(3));

    MoveToken token{MoveOwner::ManualTray,12001,12002,0};
    MoveResult output;
    output.token.jobId=999;
    int assignments=0;
    CHECK(!PrepareCancelledMoveResult(token,"runtime","record",output,
        [&](std::string& destination,const std::string& source){
            if(++assignments==2) throw std::bad_alloc();
            destination=source;
        }));
    CHECK(output.token.jobId==999); // transactional preparation
    CHECK(PrepareCancelledMoveResult(token,"runtime","record",output));
    CHECK(output.completed && output.terminal==MoveTerminal::Cancelled &&
          SameMoveToken(output.token,token) && output.runtimeKey=="runtime" &&
          output.recordId=="record");

    int failureCompletions=0;
    CHECK(!RunTerminalCompletionOrFail([&](){
        throw std::bad_alloc();
    },[&]() noexcept { ++failureCompletions; }));
    CHECK(failureCompletions==1);
    CHECK(!RunTerminalCompletionOrFail([&](){
        throw std::length_error("injected terminal commit fault");
    },[&](){ ++failureCompletions; throw std::bad_alloc(); }));
    CHECK(failureCompletions==2);
}

static void test_move_setup_rolls_back_provisional_and_queue_state(){
    std::map<std::string,std::string> provisional;
    std::set<uint64_t> runtimes,reservations,queued;
    int rollbacks=0;
    auto rollback=[&]{
        ++rollbacks;
        provisional.erase("runtime");
        runtimes.erase(77);
        reservations.erase(77);
        queued.erase(77);
    };
    CHECK(!RunFailureAtomicMoveSetup([&]{
        provisional["runtime"]="record";
        runtimes.insert(77);
        reservations.insert(77);
        queued.insert(77);
        return false;
    },rollback));
    CHECK(rollbacks==1 && provisional.empty() && runtimes.empty() &&
          reservations.empty() && queued.empty());

    CHECK(!RunFailureAtomicMoveSetup([&]()->bool{
        provisional["runtime"]="record";
        runtimes.insert(77);
        throw std::bad_alloc();
    },rollback));
    CHECK(rollbacks==2 && provisional.empty() && runtimes.empty());

    CHECK(RunFailureAtomicMoveSetup([&]{
        provisional["runtime"]="record";
        runtimes.insert(77);
        reservations.insert(77);
        queued.insert(77);
        return true;
    },rollback));
    CHECK(rollbacks==2 && provisional.size()==1 && runtimes.count(77)==1 &&
          reservations.count(77)==1 && queued.count(77)==1);

    // Owner membership is staged before queue publication.  If the final
    // enqueue throws, rollback never needs the allocating cancel primitive.
    bool runtimePublished=false,reservationPublished=false,ownerPublished=false;
    bool queuePublished=false;
    int queueCancelCalls=0;
    CHECK(!RunFailureAtomicMoveSetup([&]()->bool{
        runtimePublished=true;
        reservationPublished=true;
        ownerPublished=true;
        throw std::bad_alloc(); // injected final enqueue failure
    },[&](){
        if(queuePublished) ++queueCancelCalls;
        ownerPublished=false;
        reservationPublished=false;
        runtimePublished=false;
    }));
    CHECK(!runtimePublished && !reservationPublished && !ownerPublished &&
          !queuePublished && queueCancelCalls==0);
}

static void test_unbound_manual_reservation_uses_provisional_origin_id(){
    LayoutWin origin;
    origin.recordId="{00000000-0000-0000-0000-000000000771}";
    origin.app="firefox";
    origin.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    origin.lastSeenUtc=1700000000;
    std::string reservationId=
        "{00000000-0000-0000-0000-000000000772}";
    CHECK(BindReservationToProvisionalOrigin(origin,reservationId));
    CHECK(reservationId==origin.recordId);

    LayoutWin invalid=origin;
    invalid.recordId.clear();
    const std::string before=reservationId;
    CHECK(!BindReservationToProvisionalOrigin(invalid,reservationId));
    CHECK(reservationId==before);
}

static void test_auto_restore_failure_never_completes_as_success(){
    CHECK(AutoRestoreCompletionOutcome(false,false)==
          LcRestoreOutcome::Success);
    CHECK(AutoRestoreCompletionOutcome(true,false)==
          LcRestoreOutcome::Exhausted);
    CHECK(AutoRestoreCompletionOutcome(false,true)==
          LcRestoreOutcome::Exhausted);
    CHECK(AutoRestoreCompletionOutcome(true,true)==
          LcRestoreOutcome::Exhausted);
}

static void test_identity_guard_recaptures_immediately_before_issue_or_verify(){
    int captures=0,actions=0;
    HRESULT result=E_UNEXPECTED;
    CHECK(!RunIdentityGuardedComCall([&](){ ++captures; return false; },
        [&](){ ++actions; return S_OK; },result));
    CHECK(captures==1 && actions==0 && result==E_UNEXPECTED);
    CHECK(RunIdentityGuardedComCall([&](){ ++captures; return true; },
        [&](){ ++actions; return S_FALSE; },result));
    CHECK(captures==2 && actions==1 && result==S_FALSE);
}

static void test_desktop_services_require_documented_manager(){
    CHECK(DesktopServicesReady(true,true,true));
    CHECK(!DesktopServicesReady(true,true,false));
    CHECK(!DesktopServicesReady(true,false,true));
    CHECK(!DesktopServicesReady(false,true,true));
}

static void test_service_initialization_releases_every_failed_partial_state(){
    int initialized=0,sanityChecks=0,releases=0;
    CHECK(!InitializeServicesWithRollback([&]{ ++initialized; return false; },
        [&]{ ++sanityChecks; return true; },[&]{ ++releases; }));
    CHECK(initialized==1 && sanityChecks==0 && releases==1);
    CHECK(!InitializeServicesWithRollback([&]{ ++initialized; return true; },
        [&]{ ++sanityChecks; return false; },[&]{ ++releases; }));
    CHECK(initialized==2 && sanityChecks==1 && releases==2);
    CHECK(InitializeServicesWithRollback([&]{ ++initialized; return true; },
        [&]{ ++sanityChecks; return true; },[&]{ ++releases; }));
    CHECK(initialized==3 && sanityChecks==2 && releases==2);
    CHECK(!InitializeServicesWithRollback([&]()->bool{ throw std::bad_alloc(); },
        [&]{ return true; },[&]{ ++releases; }));
    CHECK(releases==3);
}

static void test_reconcile_deadline_retires_dropped_operation_exactly_once(){
    AsyncReconcileDeadlineGate deadlines;
    CHECK(deadlines.begin(101,1000));
    CHECK(deadlines.begin(101,1300));
    CHECK(deadlines.pending(101)==2);
    CHECK(deadlines.dueAt(101)==
          1000+AsyncReconcileDeadlineGate::maxLifetimeMs());
    std::vector<uint64_t> expired;
    CHECK(deadlines.expire(
        1000+AsyncReconcileDeadlineGate::maxLifetimeMs()-1,expired)==0);
    CHECK(deadlines.complete(101) && deadlines.pending(101)==1);
    CHECK(deadlines.expire(
        1000+AsyncReconcileDeadlineGate::maxLifetimeMs(),expired)==1);
    CHECK(expired.size()==1 && expired[0]==101 && deadlines.empty());
    CHECK(!deadlines.complete(101));

    CHECK(deadlines.begin(102,2000));
    CHECK(deadlines.cancel(102));
    CHECK(deadlines.expire(UINT64_MAX,expired)==0 && expired.empty());
}

static void test_dirty_flush_preserves_mutation_during_write_and_limits_errors(){
    DirtyFlushController dirty;
    dirty.markDirty(0);
    CHECK(dirty.flush(500,false,[&]{
        dirty.markDirty(500);
        return true;
    })==DirtyFlushResult::SucceededDirtyAgain);
    CHECK(dirty.dirty() && dirty.dueAtMs()==1000);

    CHECK(dirty.shouldReportError("layout locked",10));
    CHECK(!dirty.shouldReportError("layout locked",300009));
    CHECK(dirty.shouldReportError("layout locked",300010));
    CHECK(dirty.shouldReportError("different",300011));
    dirty.markDirty(600);
    CHECK(dirty.shouldReportError("different",300012));

    DirtyFlushController rollbackErrors;
    CHECK(rollbackErrors.shouldReportError("same",100));
    CHECK(!rollbackErrors.shouldReportError("same",90));
    CHECK(!rollbackErrors.shouldReportError("same",89));
    CHECK(rollbackErrors.shouldReportError("same",300089));

    DirtyFlushController disabledDuringWrite;
    disabledDuringWrite.markDirty(0);
    DirtyFlushResult nested=DirtyFlushResult::Failed;
    int nestedWrites=0;
    CHECK(disabledDuringWrite.flush(500,false,[&]{
        disabledDuringWrite.clearDirty();
        nested=disabledDuringWrite.flush(500,true,[&]{
            ++nestedWrites;
            return true;
        });
        return false;
    })==DirtyFlushResult::Cleared);
    CHECK(nested==DirtyFlushResult::Deferred && nestedWrites==0);
    CHECK(!disabledDuringWrite.dirty());
}

static void test_dirty_flush_clock_ceiling_never_spins(){
    DirtyFlushController dirty;
    const uint64_t ceiling=(std::numeric_limits<uint64_t>::max)();
    int writes=0;
    dirty.markDirty(ceiling);
    CHECK(dirty.flush(ceiling,false,[&]{ ++writes; return false; })==
        DirtyFlushResult::Failed);
    CHECK(dirty.flush(ceiling,false,[&]{ ++writes; return false; })==
        DirtyFlushResult::Deferred);
    CHECK(writes==1);
    CHECK(dirty.flush(ceiling,true,[&]{ ++writes; return false; })==
        DirtyFlushResult::Failed);
    CHECK(writes==2);
    dirty.markDirty(ceiling);
    CHECK(dirty.flush(ceiling,false,[&]{ ++writes; return false; })==
        DirtyFlushResult::Failed);
    CHECK(dirty.flush(ceiling,false,[&]{ ++writes; return false; })==
        DirtyFlushResult::Deferred);
    CHECK(writes==3);

    const uint64_t rolledBack=ceiling-1000;
    CHECK(dirty.flush(rolledBack,false,[&]{ ++writes; return true; })==
        DirtyFlushResult::Deferred);
    CHECK(dirty.dueAtMs()==rolledBack+500 && writes==3);
    CHECK(dirty.flush(rolledBack+500,false,[&]{ ++writes; return true; })==
        DirtyFlushResult::Succeeded);
    CHECK(writes==4 && !dirty.dirty());

    DirtyFlushController saturated;
    int saturatedWrites=0;
    saturated.markDirty(ceiling-600);
    CHECK(saturated.flush(ceiling-100,false,[&]{
        ++saturatedWrites;
        return false;
    })==DirtyFlushResult::Failed);
    CHECK(saturated.flush(ceiling,false,[&]{
        ++saturatedWrites;
        return true;
    })==DirtyFlushResult::Deferred);
    CHECK(saturatedWrites==1);
}

static void test_layout_serializes_v4_header(){
    CHECK(SerializeLayout({}, {}).find("# VDE snapshot v4\n") == 0);
}

static void test_layout_roundtrip_v4(){
    std::vector<DeskRec> d; DeskRec d0; d0.index=0; d0.guid=G(L"{231A0000-0000-0000-0000-000000000001}"); d0.name=L"Work"; d.push_back(d0);
    std::vector<LayoutWin> w; LayoutWin w0;
    w0.recordId="{00000000-0000-0000-0000-000000000101}";
    w0.app="firefox"; w0.deskIndex=0; w0.desktop=d0.guid; w0.activeTitle="PR #42";
    w0.activeDomain="github.com"; w0.tabCount=5; w0.counts={{"github.com",4},{"docs.python.org",1}};
    w0.lastSeenUtc=1700000000; w0.missingSinceUtc=1700000100;
    w.push_back(w0);

    std::string s = SerializeLayout(d, w);
    std::vector<DeskRec> d2; std::vector<LayoutWin> w2; std::string error;
    CHECK(ParseLayout(s, d2, w2, 1800000000, &error));
    CHECK(error.empty());
    CHECK(d2.size()==1); CHECK(w2.size()==1);
    CHECK(w2[0].recordId=="{00000000-0000-0000-0000-000000000101}");
    CHECK(w2[0].app=="firefox"); CHECK(w2[0].deskIndex==0); CHECK(w2[0].activeTitle=="PR #42");
    CHECK(w2[0].activeDomain=="github.com"); CHECK(w2[0].tabCount==5);
    CHECK(w2[0].lastSeenUtc==1700000000); CHECK(w2[0].missingSinceUtc==1700000100);
    CHECK(w2[0].counts["github.com"]==4); CHECK(w2[0].counts["docs.python.org"]==1);
    CHECK(GuidEq(w2[0].desktop, d0.guid));
}
static void test_layout_parse_v2(){
    std::string v2 = "# VDE snapshot v2\n"
        "D\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("Work") + "\n"
        "W\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("PR #42") + "\tgithub.com\t5\tgithub.com:4,docs.python.org:1\n";
    std::vector<DeskRec> d; std::vector<LayoutWin> w;
    CHECK(ParseLayout(v2, d, w, 1800000000));
    CHECK(w.size()==1); CHECK(w[0].app=="firefox");
    CHECK(w[0].lastSeenUtc==1800000000);
    CHECK(w[0].tabCount==5); CHECK(w[0].counts["docs.python.org"]==1);
}

static std::string V4Line(const char* guid, const char* recordId, const char* lastSeen, const char* missing){
    return std::string("W\tfirefox\t") + recordId + "\t0\t" + guid + "\t" + b64enc("Inbox") +
        "\tmail.example\t1\tmail.example:1\t" + lastSeen + "\t" + missing + "\n";
}

static void test_layout_rejects_invalid_desktop_guid_transactionally(){
    std::string data = "# VDE snapshot v4\n" +
        V4Line("not-a-guid", "{00000000-0000-0000-0000-000000000101}", "1700000000", "0");
    std::vector<DeskRec> d; std::vector<LayoutWin> w; std::string error;
    CHECK(!ParseLayout(data, d, w, 1800000000, &error));
    CHECK(!error.empty()); CHECK(d.empty()); CHECK(w.empty());
}

static void test_layout_rejects_progid_as_desktop_guid(){
    std::string data = "# VDE snapshot v4\n" +
        V4Line("Shell.Application", "{00000000-0000-0000-0000-000000000101}", "1700000000", "0");
    std::vector<DeskRec> d; std::vector<LayoutWin> w; std::string error;
    CHECK(!ParseLayout(data, d, w, 1800000000, &error));
    CHECK(!error.empty()); CHECK(d.empty()); CHECK(w.empty());
}

static void test_layout_rejects_integer_trailing_junk_transactionally(){
    std::string data = "# VDE snapshot v4\n" +
        V4Line("{231A0000-0000-0000-0000-000000000001}", "{00000000-0000-0000-0000-000000000101}", "1700000000junk", "0");
    std::vector<DeskRec> d(1); d[0].index=77;
    std::vector<LayoutWin> w(1); w[0].app="sentinel";
    std::string error;
    CHECK(!ParseLayout(data, d, w, 1800000000, &error));
    CHECK(!error.empty()); CHECK(d.size()==1 && d[0].index==77);
    CHECK(w.size()==1 && w[0].app=="sentinel");
}

static void test_layout_migrates_v3_record(){
    std::string data = "# VDE snapshot v3\n"
        "W\tfirefox\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("Inbox") +
        "\tmail.example\t1\tmail.example:1\t2\n";
    std::vector<DeskRec> d; std::vector<LayoutWin> w; std::string error="stale"; int source=0;
    CHECK(ParseLayout(data, d, w, 1800000000, &error, &source));
    CHECK(error.empty()); CHECK(source==3); CHECK(w.size()==1);
    CHECK(!w[0].recordId.empty()); CHECK(w[0].lastSeenUtc==1800000000);
    CHECK(w[0].missingSinceUtc==1800000000);
    GUID id{}; CHECK(StringToGuid(U82W(w[0].recordId), id) && !GuidIsZero(id));
}

static void test_layout_rejects_negative_v3_missing_counter_transactionally(){
    std::string data = "# VDE snapshot v3\n"
        "W\tfirefox\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("Inbox") +
        "\tmail.example\t1\tmail.example:1\t-1\n";
    std::vector<DeskRec> d(1); d[0].index=77; d[0].name=L"sentinel desk";
    std::vector<LayoutWin> w(1); w[0].app="sentinel"; w[0].activeTitle="sentinel title";
    std::string error;
    CHECK(!ParseLayout(data,d,w,1800000000,&error));
    CHECK(!error.empty());
    CHECK(d.size()==1 && d[0].index==77 && d[0].name==L"sentinel desk");
    CHECK(w.size()==1 && w[0].app=="sentinel" && w[0].activeTitle=="sentinel title");
}

static void test_layout_rejects_trailing_columns(){
    std::string data = "# VDE snapshot v4\n" +
        V4Line("{231A0000-0000-0000-0000-000000000001}", "{00000000-0000-0000-0000-000000000101}", "1700000000", "0");
    data.insert(data.size()-1, "\textra");
    std::vector<DeskRec> d; std::vector<LayoutWin> w; std::string error;
    CHECK(!ParseLayout(data, d, w, 1800000000, &error));
    CHECK(!error.empty()); CHECK(d.empty()); CHECK(w.empty());
}

static void test_layout_rejects_duplicate_record_ids(){
    const char* id="{00000000-0000-0000-0000-000000000101}";
    std::string data = "# VDE snapshot v4\n" +
        V4Line("{231A0000-0000-0000-0000-000000000001}", id, "1700000000", "0") +
        V4Line("{231A0000-0000-0000-0000-000000000002}", id, "1700000001", "0");
    std::vector<DeskRec> d; std::vector<LayoutWin> w; std::string error;
    CHECK(!ParseLayout(data, d, w, 1800000000, &error));
    CHECK(!error.empty()); CHECK(d.empty()); CHECK(w.empty());
}

static void test_layout_enforces_total_record_cap_transactionally(){
    const char* desktop="{231A0000-0000-0000-0000-000000000001}";
    std::string data="# VDE snapshot v4\n";
    data.reserve(600000);
    std::string deskLine=std::string("D\t0\t")+desktop+"\t"+b64enc("Desk")+"\n";
    for(int i=0;i<2048;++i) data+=deskLine;
    for(int i=0;i<2048;++i){
        char id[64]; sprintf_s(id,"{00000000-0000-0000-0000-%012d}",i+1);
        data+=V4Line(desktop,id,"1700000000","0");
    }

    std::vector<DeskRec> acceptedDesks; std::vector<LayoutWin> acceptedWins; std::string error="stale";
    CHECK(ParseLayout(data,acceptedDesks,acceptedWins,1800000000,&error));
    CHECK(error.empty()); CHECK(acceptedDesks.size()==2048); CHECK(acceptedWins.size()==2048);
    CHECK(acceptedDesks.size()==2048 && acceptedDesks.front().name==L"Desk");
    CHECK(acceptedWins.size()==2048 && acceptedWins.back().recordId=="{00000000-0000-0000-0000-000000002048}");

    char overflowId[64]; sprintf_s(overflowId,"{00000000-0000-0000-0000-%012d}",2049);
    std::string overflow=data+V4Line(desktop,overflowId,"1700000000","0");
    DeskRec sentinelDesk{}; sentinelDesk.index=77;
    sentinelDesk.guid=G(L"{231A0000-0000-0000-0000-000000000077}"); sentinelDesk.name=L"sentinel desk";
    LayoutWin sentinelWin; sentinelWin.recordId="{00000000-0000-0000-0000-000000000077}";
    sentinelWin.app="chrome"; sentinelWin.deskIndex=-7;
    sentinelWin.desktop=G(L"{231A0000-0000-0000-0000-000000000078}");
    sentinelWin.activeTitle="sentinel title"; sentinelWin.activeDomain="sentinel.example";
    sentinelWin.tabCount=7; sentinelWin.counts={{"sentinel.example",7}};
    sentinelWin.lastSeenUtc=1700000077; sentinelWin.missingSinceUtc=1700000088;
    std::vector<DeskRec> desksOut={sentinelDesk}; std::vector<LayoutWin> winsOut={sentinelWin}; error.clear();

    CHECK(!ParseLayout(overflow,desksOut,winsOut,1800000000,&error));
    CHECK(!error.empty());
    CHECK(desksOut.size()==1 && desksOut[0].index==77);
    CHECK(desksOut.size()==1 && GuidEq(desksOut[0].guid,sentinelDesk.guid));
    CHECK(desksOut.size()==1 && desksOut[0].name==L"sentinel desk");
    CHECK(winsOut.size()==1 && winsOut[0].recordId==sentinelWin.recordId);
    CHECK(winsOut.size()==1 && winsOut[0].app=="chrome");
    CHECK(winsOut.size()==1 && winsOut[0].deskIndex==-7);
    CHECK(winsOut.size()==1 && GuidEq(winsOut[0].desktop,sentinelWin.desktop));
    CHECK(winsOut.size()==1 && winsOut[0].activeTitle=="sentinel title");
    CHECK(winsOut.size()==1 && winsOut[0].activeDomain=="sentinel.example");
    CHECK(winsOut.size()==1 && winsOut[0].tabCount==7);
    CHECK(winsOut.size()==1 && winsOut[0].counts==sentinelWin.counts);
    CHECK(winsOut.size()==1 && winsOut[0].lastSeenUtc==1700000077);
    CHECK(winsOut.size()==1 && winsOut[0].missingSinceUtc==1700000088);
}

static LayoutWin OldStyleRecord(){
    LayoutWin w;
    w.app="firefox"; w.deskIndex=0;
    w.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    w.activeTitle="Inbox"; w.activeDomain="mail.example"; w.tabCount=1;
    w.counts={{"mail.example",1}};
    return w;
}

static LayoutWin StrictV4Record(){
    LayoutWin w=OldStyleRecord();
    w.recordId="{00000000-0000-0000-0000-000000000101}";
    w.lastSeenUtc=1700000000;
    return w;
}

static void test_layout_provisional_marker_roundtrips_strict_v4(){
    LayoutWin provisional=StrictV4Record();
    provisional.provisional=true;
    const std::string marker="P\t"+provisional.recordId+"\n";

    const std::string serialized=SerializeLayout({}, {provisional});
    CHECK(serialized.find(marker)!=std::string::npos);
    CHECK(serialized.find(marker)==serialized.rfind(marker));

    std::vector<DeskRec> desks;
    std::vector<LayoutWin> records;
    std::string error="stale";
    int sourceVersion=0;
    CHECK(ParseLayout(serialized,desks,records,1800000000,&error,
                      &sourceVersion));
    CHECK(error.empty() && sourceVersion==4);
    CHECK(records.size()==1 && records[0].provisional);
    CHECK(records.size()==1 && records[0].recordId==provisional.recordId);
}

static void test_layout_noncanonical_record_id_is_published_canonically(){
    const std::string raw="abcdefab-cdef-abcd-efab-cdefabcdefab";
    const std::string marker="{abcdefab-cdef-abcd-efab-cdefabcdefab}";
    const std::string canonical=
        "{ABCDEFAB-CDEF-ABCD-EFAB-CDEFABCDEFAB}";
    const std::string data="# VDE snapshot v4\n"+
        V4Line("{231A0000-0000-0000-0000-000000000001}",
               raw.c_str(),"1700000000","0")+
        "P\t"+marker+"\n";
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> records;
    std::string error;
    int sourceVersion=0;
    CHECK(ParseLayout(data,desks,records,1800000000,&error,&sourceVersion));
    CHECK(error.empty() && sourceVersion==4 && records.size()==1);
    CHECK(records.size()==1 && records[0].recordId==canonical &&
          records[0].provisional);
    const std::string serialized=SerializeLayout(desks,records);
    CHECK(serialized.find("W\tfirefox\t"+canonical+"\t")!=
          std::string::npos);
    CHECK(serialized.find("P\t"+canonical+"\n")!=std::string::npos);
    CHECK(serialized.find(raw)==std::string::npos);
}

static void test_v4_provisional_extension_preserves_base_window_row(){
    LayoutWin ordinary=StrictV4Record();
    LayoutWin provisional=ordinary;
    provisional.provisional=true;
    const std::string ordinaryBytes=SerializeLayout({}, {ordinary});
    const std::string provisionalBytes=SerializeLayout({}, {provisional});
    const size_t ordinaryWindow=ordinaryBytes.find("W\t");
    const size_t ordinaryEnd=ordinaryBytes.find('\n',ordinaryWindow);
    const size_t provisionalWindow=provisionalBytes.find("W\t");
    const size_t provisionalEnd=provisionalBytes.find('\n',provisionalWindow);
    CHECK(ordinaryWindow!=std::string::npos &&
          provisionalWindow!=std::string::npos);
    CHECK(ordinaryBytes.substr(ordinaryWindow,ordinaryEnd-ordinaryWindow)==
          provisionalBytes.substr(
              provisionalWindow,provisionalEnd-provisionalWindow));
    CHECK(ordinaryBytes.find("\nP\t")==std::string::npos);
    CHECK(provisionalBytes.find("\nP\t"+ordinary.recordId+"\n")!=
          std::string::npos);

    std::vector<DeskRec> desks;
    std::vector<LayoutWin> records;
    CHECK(ParseLayout(ordinaryBytes,desks,records,1800000000));
    CHECK(records.size()==1 && !records[0].provisional);
    CHECK(ParseLayout(provisionalBytes,desks,records,1800000000));
    CHECK(records.size()==1 && records[0].provisional);
}

static void test_layout_provisional_marker_is_strict_and_transactional(){
    const std::string desktop=
        "{231A0000-0000-0000-0000-000000000001}";
    const std::string recordId=
        "{00000000-0000-0000-0000-000000000101}";
    const std::string window=V4Line(
        desktop.c_str(),recordId.c_str(),"1700000000","0");
    const std::vector<std::string> invalid={
        "# VDE snapshot v4\nP\t"+recordId+"\n"+window,
        "# VDE snapshot v4\n"+window+"P\t"+recordId+"\textra\n",
        "# VDE snapshot v4\n"+window+
            "P\t{00000000-0000-0000-0000-000000000102}\n",
        "# VDE snapshot v4\n"+window+"P\t"+recordId+"\nP\t"+
            recordId+"\n"
    };
    for(const std::string& data : invalid){
        DeskRec sentinelDesk{};
        sentinelDesk.index=77;
        LayoutWin sentinel=StrictV4Record();
        sentinel.activeTitle="sentinel";
        std::vector<DeskRec> desks={sentinelDesk};
        std::vector<LayoutWin> records={sentinel};
        std::string error;
        CHECK(!ParseLayout(data,desks,records,1800000000,&error));
        CHECK(!error.empty());
        CHECK(desks.size()==1 && desks[0].index==77);
        CHECK(records.size()==1 && records[0].activeTitle=="sentinel");
        CHECK(!records[0].provisional);
    }
}

static void test_layout_legacy_migration_never_invents_provisional_marker(){
    const std::string data = "# VDE snapshot v3\n"
        "W\tfirefox\t0\t{231A0000-0000-0000-0000-000000000001}\t" +
        b64enc("Inbox")+"\tmail.example\t1\tmail.example:1\t0\n";
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> records;
    std::string error;
    int sourceVersion=0;
    CHECK(ParseLayout(data,desks,records,1800000000,&error,&sourceVersion));
    CHECK(error.empty() && sourceVersion==3 && records.size()==1);
    CHECK(records.size()==1 && !records[0].provisional);
    CHECK(SerializeLayout(desks,records).find("\nP\t")==std::string::npos);
}

static void test_retention_expiration_boundaries(){
    const UnixSeconds now=1700000000;
    LayoutWin w=OldStyleRecord();
    w.missingSinceUtc=now-29LL*24*60*60;
    CHECK(!IsExpired(w,now));
    w.missingSinceUtc=now-WINDOW_RETENTION_SECONDS+1;
    CHECK(!IsExpired(w,now));
    w.missingSinceUtc=now-WINDOW_RETENTION_SECONDS;
    CHECK(IsExpired(w,now));
}

static void test_retention_future_and_zero_missing_are_not_expired(){
    LayoutWin w=OldStyleRecord();
    w.missingSinceUtc=LLONG_MAX;
    CHECK(!IsExpired(w,1700000000));
    w.missingSinceUtc=0;
    CHECK(!IsExpired(w,LLONG_MAX));
}

static void test_retention_mark_seen_clears_missing_and_updates_last_seen(){
    LayoutWin w=OldStyleRecord();
    w.lastSeenUtc=1700000000;
    w.missingSinceUtc=1700000000;
    const UnixSeconds reappearanceUtc=w.missingSinceUtc+WINDOW_RETENTION_SECONDS-1;
    CHECK(!IsExpired(w,reappearanceUtc));
    MarkSeen(w,reappearanceUtc);
    CHECK(w.lastSeenUtc==reappearanceUtc);
    CHECK(w.missingSinceUtc==0);
}

static void test_retention_mark_missing_uses_last_seen_and_is_idempotent(){
    LayoutWin seen=OldStyleRecord();
    seen.lastSeenUtc=1700000000;
    MarkMissing(seen,1800000000);
    CHECK(seen.missingSinceUtc==1700000000);
    MarkMissing(seen,1900000000);
    CHECK(seen.missingSinceUtc==1700000000);

    LayoutWin neverSeen=OldStyleRecord();
    MarkMissing(neverSeen,1800000000);
    CHECK(neverSeen.missingSinceUtc==1800000000);
    MarkMissing(neverSeen,1900000000);
    CHECK(neverSeen.missingSinceUtc==1800000000);
}

static void test_retention_prune_preserves_order_duplicates_and_input(){
    const UnixSeconds now=1700000000;
    LayoutWin first=OldStyleRecord(); first.recordId="duplicate"; first.lastSeenUtc=11; first.missingSinceUtc=0;
    LayoutWin expired=OldStyleRecord(); expired.recordId="expired"; expired.lastSeenUtc=22;
    expired.missingSinceUtc=now-WINDOW_RETENTION_SECONDS;
    LayoutWin duplicate=first; duplicate.lastSeenUtc=33;
    LayoutWin recent=OldStyleRecord(); recent.recordId="recent"; recent.lastSeenUtc=44;
    recent.missingSinceUtc=now-WINDOW_RETENTION_SECONDS+1;
    std::vector<LayoutWin> input={first,expired,duplicate,recent};

    std::vector<LayoutWin> output=PruneExpired(input,now);

    CHECK(output.size()==3);
    CHECK(output[0].recordId=="duplicate" && output[0].lastSeenUtc==11);
    CHECK(output[1].recordId=="duplicate" && output[1].lastSeenUtc==33);
    CHECK(output[2].recordId=="recent" && output[2].lastSeenUtc==44);
    CHECK(input.size()==4);
    CHECK(input[0].recordId=="duplicate" && input[0].lastSeenUtc==11 && input[0].missingSinceUtc==0);
    CHECK(input[1].recordId=="expired" && input[1].lastSeenUtc==22);
    CHECK(input[2].recordId=="duplicate" && input[2].lastSeenUtc==33);
    CHECK(input[3].recordId=="recent" && input[3].lastSeenUtc==44);
}

static void test_startup_expiry_partitions_every_app_transactionally(){
    const UnixSeconds now=1900000000;
    LayoutWin expiredFirefox=OldStyleRecord();
    expiredFirefox.recordId="expired-enabled-firefox";
    expiredFirefox.app="firefox";
    expiredFirefox.lastSeenUtc=now-WINDOW_RETENTION_SECONDS-10;
    expiredFirefox.missingSinceUtc=now-WINDOW_RETENTION_SECONDS;

    LayoutWin recentChrome=OldStyleRecord();
    recentChrome.recordId="recent-chrome";
    recentChrome.app="chrome";
    recentChrome.lastSeenUtc=now-WINDOW_RETENTION_SECONDS+1;
    recentChrome.missingSinceUtc=now-WINDOW_RETENTION_SECONDS+1;

    LayoutWin expiredDisabledEdge=OldStyleRecord();
    expiredDisabledEdge.recordId="expired-disabled-edge";
    expiredDisabledEdge.app="msedge";
    expiredDisabledEdge.lastSeenUtc=now-WINDOW_RETENTION_SECONDS-20;
    expiredDisabledEdge.missingSinceUtc=now-WINDOW_RETENTION_SECONDS-5;

    LayoutWin liveChrome=OldStyleRecord();
    liveChrome.recordId="live-chrome";
    liveChrome.app="chrome";
    liveChrome.lastSeenUtc=now-1;
    liveChrome.missingSinceUtc=0;

    const std::vector<LayoutWin> input={
        expiredFirefox,recentChrome,expiredDisabledEdge,liveChrome
    };
    ExpiredLayoutPartition partition;
    CHECK(PartitionExpiredLayoutRecords(input,now,partition));
    CHECK(partition.expired.size()==2 &&
          partition.expired[0].recordId=="expired-enabled-firefox" &&
          partition.expired[1].recordId=="expired-disabled-edge");
    CHECK(partition.retained.size()==2 &&
          partition.retained[0].recordId=="recent-chrome" &&
          partition.retained[1].recordId=="live-chrome");
    CHECK(input.size()==4 && input[0].recordId=="expired-enabled-firefox" &&
          input[1].recordId=="recent-chrome" &&
          input[2].recordId=="expired-disabled-edge" &&
          input[3].recordId=="live-chrome");

    ExpiredLayoutPartition untouched;
    LayoutWin retainedSentinel=OldStyleRecord();
    retainedSentinel.recordId="retained-sentinel";
    LayoutWin expiredSentinel=OldStyleRecord();
    expiredSentinel.recordId="expired-sentinel";
    untouched.retained.push_back(retainedSentinel);
    untouched.expired.push_back(expiredSentinel);
    int copies=0;
    CHECK(!PartitionExpiredLayoutRecords(
        input,now,untouched,
        [&](std::vector<LayoutWin>& destination,const LayoutWin& record){
            if(++copies==2) throw std::bad_alloc();
            destination.push_back(record);
            return true;
        }));
    CHECK(copies==2);
    CHECK(untouched.retained.size()==1 &&
          untouched.retained[0].recordId=="retained-sentinel");
    CHECK(untouched.expired.size()==1 &&
          untouched.expired[0].recordId=="expired-sentinel");
    CHECK(input.size()==4 && input[0].app=="firefox" &&
          input[1].app=="chrome" && input[2].app=="msedge" &&
          input[3].missingSinceUtc==0);
}

static LayoutWin MatchRecord(const char* app, const char* title, const char* domain, int tabs,
        const std::map<std::string,int>& counts){
    LayoutWin w=OldStyleRecord();
    w.app=app; w.activeTitle=title; w.activeDomain=domain; w.tabCount=tabs; w.counts=counts;
    return w;
}

static void test_reconcile_restores_saved_a_and_creates_new_b(){
    const UnixSeconds now=2000000000;
    LayoutWin savedA=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    savedA.recordId="{00000000-0000-0000-0000-000000000301}";
    savedA.deskIndex=0;
    savedA.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    LayoutWin liveA=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    liveA.deskIndex=3;
    liveA.desktop=G(L"{231A0000-0000-0000-0000-000000000004}");
    LayoutWin liveB=MatchRecord("firefox","B","b.com",1,{{"b.com",1}});
    liveB.deskIndex=4;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");

    ReconcilePlan plan=PlanAppReconcile({savedA},{liveA,liveB},"firefox",now);
    CHECK(plan.restores.size()==1);
    CHECK(plan.restores[0].liveIndex==0);
    CHECK(plan.newRecords.size()==1 && plan.newRecords[0].liveIndex==1);
    CHECK(!plan.newRecords[0].recordId.empty());

    std::vector<LayoutWin> committed=CommitAppReconcile({savedA},{liveA,liveB},plan,{0},now);
    CHECK(committed.size()==2);
    CHECK(GuidEq(committed[0].desktop,savedA.desktop));
    CHECK(committed[0].missingSinceUtc==0);
    CHECK(committed[1].activeTitle=="B");
    CHECK(GuidEq(committed[1].desktop,liveB.desktop));
}

static void test_expired_reappearance_is_new_not_restored(){
    const UnixSeconds now=2000000000;
    LayoutWin expired=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    expired.recordId="{00000000-0000-0000-0000-000000000305}";
    expired.deskIndex=0;
    expired.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    expired.missingSinceUtc=now-WINDOW_RETENTION_SECONDS;
    LayoutWin live=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    live.deskIndex=4;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");

    ReconcilePlan plan=PlanAppReconcile({expired},{live},"firefox",now);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.size()==1);
    CHECK(plan.newRecords[0].recordId!=expired.recordId);

    std::vector<LayoutWin> committed=CommitAppReconcile({expired},{live},plan,{},now);
    CHECK(committed.size()==1);
    CHECK(committed[0].recordId==plan.newRecords[0].recordId);
    CHECK(GuidEq(committed[0].desktop,live.desktop));
}

static void test_cached_stale_edge_preserves_match_and_defers_unmatched(){
    const UnixSeconds now=2000000000;
    LayoutWin saved=MatchRecord("msedge","Saved","saved.example",2,{{"saved.example",2}});
    saved.recordId="{00000000-0000-0000-0000-000000000307}";
    saved.deskIndex=0;
    saved.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    LayoutWin matched=MatchRecord("msedge","Saved","stale.example",9,{{"saved.example",2}});
    matched.deskIndex=0;
    matched.desktop=saved.desktop;
    LayoutWin unmatched=MatchRecord("msedge","New","new.example",1,{{"new.example",1}});
    unmatched.deskIndex=1;
    unmatched.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");

    ReconcilePlan plan=PlanAppReconcile(
        {saved},{matched,unmatched},"msedge",now,{},ReconcileFreshness::CachedStale);
    CHECK(plan.matches.size()==1);
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());

    std::vector<LayoutWin> committed=CommitAppReconcile({saved},{matched,unmatched},plan,{},now);
    CHECK(committed.size()==1);
    CHECK(committed[0].counts==saved.counts);
    CHECK(committed[0].activeTitle==saved.activeTitle);
    CHECK(committed[0].activeDomain==saved.activeDomain);
    CHECK(committed[0].tabCount==saved.tabCount);
}

static LayoutMatch Candidate(size_t savedIndex, size_t liveIndex, double score){
    LayoutMatch match;
    match.savedIndex=savedIndex; match.liveIndex=liveIndex; match.score=score;
    return match;
}

static std::vector<std::pair<size_t,size_t>> MatchPairs(const std::vector<LayoutMatch>& matches){
    std::vector<std::pair<size_t,size_t>> pairs;
    for(const auto& match : matches) pairs.push_back(std::make_pair(match.savedIndex,match.liveIndex));
    return pairs;
}

static bool MatchesAreSortedAndUnique(const std::vector<LayoutMatch>& matches){
    std::set<size_t> saved,live;
    for(size_t i=0;i<matches.size();++i){
        if(i>0 && std::make_pair(matches[i].savedIndex,matches[i].liveIndex)<
                std::make_pair(matches[i-1].savedIndex,matches[i-1].liveIndex)) return false;
        if(!saved.insert(matches[i].savedIndex).second || !live.insert(matches[i].liveIndex).second) return false;
    }
    return true;
}

static bool SameCandidateInput(const std::vector<LayoutMatch>& left, const std::vector<LayoutMatch>& right){
    if(left.size()!=right.size()) return false;
    for(size_t i=0;i<left.size();++i){
        if(left[i].savedIndex!=right[i].savedIndex || left[i].liveIndex!=right[i].liveIndex) return false;
        if(std::isnan(left[i].score) && std::isnan(right[i].score)) continue;
        if(left[i].score!=right[i].score) return false;
    }
    return true;
}

static void test_layout_score_formula_and_fallback(){
    LayoutWin saved=MatchRecord("firefox","Same title","same.example",2,{{"a.example",1},{"b.example",1}});
    LayoutWin live=MatchRecord("firefox","Same title","same.example",2,{{"a.example",1},{"c.example",1}});
    const double expectedTitle=0.40*0.5+0.25*(1.0/3.0)+0.20+0.15;
    CHECK(std::fabs(LayoutScore(saved,live)-expectedTitle)<1e-12);

    live.activeTitle="Different title";
    const double expectedDomain=0.40*0.5+0.25*(1.0/3.0)+0.20*0.5+0.15;
    CHECK(std::fabs(LayoutScore(saved,live)-expectedDomain)<1e-12);

    saved=MatchRecord("firefox","Proportional","same.example",5,{{"a.example",3},{"b.example",4}});
    live=MatchRecord("firefox","Proportional","same.example",10,{{"a.example",6},{"b.example",8}});
    CHECK(std::fabs(LayoutScore(saved,live)-0.925)<1e-12);

    LayoutWin emptySaved=MatchRecord("firefox","Fallback","same.example",1,{});
    LayoutWin countedLive=MatchRecord("firefox","Fallback","other.example",9,{{"other.example",9}});
    CHECK(LayoutScore(emptySaved,countedLive)==1.0);
    countedLive.activeTitle="Other"; countedLive.activeDomain="same.example";
    CHECK(LayoutScore(emptySaved,countedLive)==0.0);
    countedLive.counts.clear(); countedLive.activeTitle.clear(); emptySaved.activeTitle.clear();
    CHECK(LayoutScore(emptySaved,countedLive)==0.0);
}

static void test_layout_score_browser_symmetry_and_cross_app_rejection(){
    const char* apps[]={"firefox","chrome","msedge"};
    for(const char* app : apps){
        LayoutWin saved=MatchRecord(app,"Inbox","mail.example",2,{{"mail.example",2}});
        LayoutWin live=saved;
        CHECK(LayoutScore(saved,live)==1.0);
    }
    LayoutWin firefox=MatchRecord("firefox","Inbox","mail.example",2,{{"mail.example",2}});
    LayoutWin chrome=firefox; chrome.app="chrome";
    CHECK(LayoutScore(firefox,chrome)==0.0);
    CHECK(LayoutScore(chrome,firefox)==0.0);
}

static void test_layout_score_identical_two_domain_is_exact(){
    LayoutWin saved=MatchRecord("firefox","Inbox","mail.example",5,
        {{"docs.example",1},{"mail.example",1}});
    LayoutWin live=saved;
    CHECK(LayoutScore(saved,live)==1.0);
    bool tooComplex=true;
    std::vector<LayoutMatch> matches=MatchOneToOne({saved},{live},1.0,&tooComplex);
    CHECK(!tooComplex);
    CHECK(matches.size()==1 && matches[0].savedIndex==0 && matches[0].liveIndex==0 &&
        matches[0].score==1.0);
}

static void test_match_one_to_one_duplicate_fingerprints_are_unique(){
    LayoutWin fingerprint=MatchRecord("firefox","Inbox","mail.example",2,{{"mail.example",2}});
    std::vector<LayoutWin> saved(3,fingerprint), live(2,fingerprint);
    bool tooComplex=true;
    std::vector<LayoutMatch> matches=MatchOneToOne(saved,live,1.0,&tooComplex);
    CHECK(!tooComplex);
    CHECK(matches.size()==2);
    CHECK(MatchesAreSortedAndUnique(matches));
    CHECK(saved.size()==3 && live.size()==2);
}

static void test_match_one_to_one_browser_apps_and_never_crosses_apps(){
    const char* apps[]={"firefox","chrome","msedge"};
    for(const char* app : apps){
        LayoutWin record=MatchRecord(app,"Inbox","mail.example",1,{{"mail.example",1}});
        bool tooComplex=true;
        std::vector<LayoutMatch> matches=MatchOneToOne({record},{record},1.0,&tooComplex);
        CHECK(!tooComplex && matches.size()==1);
        CHECK(matches.size()==1 && matches[0].savedIndex==0 && matches[0].liveIndex==0);
    }

    LayoutWin firefox=MatchRecord("firefox","Inbox","mail.example",1,{{"mail.example",1}});
    LayoutWin chrome=firefox; chrome.app="chrome";
    bool tooComplex=true;
    std::vector<LayoutMatch> cross=MatchOneToOne({firefox},{chrome},0.0,&tooComplex);
    CHECK(!tooComplex);
    CHECK(cross.empty());
}

static void test_match_one_to_one_score_evaluation_budget(){
    CHECK(MAX_MATCH_SCORE_EVALUATIONS==1000000);
    LayoutWin firefox=MatchRecord("firefox","Inbox","mail.example",1,{});
    std::vector<LayoutWin> saved(4096,firefox),sameAppLive(4096,firefox);
    bool tooComplex=false;
    std::vector<LayoutMatch> result=MatchOneToOne(saved,sameAppLive,2.0,&tooComplex);
    CHECK(result.empty());
    CHECK(tooComplex);
    CHECK(saved.size()==4096 && sameAppLive.size()==4096);
    CHECK(saved[0].app=="firefox" && sameAppLive[0].activeTitle=="Inbox");

    LayoutWin chrome=firefox; chrome.app="chrome";
    std::vector<LayoutWin> crossAppLive(4096,chrome);
    tooComplex=true;
    result=MatchOneToOne(saved,crossAppLive,2.0,&tooComplex);
    CHECK(result.empty());
    CHECK(!tooComplex);

    LayoutWin arbitrary=firefox; arbitrary.app="arbitrary-browser";
    tooComplex=true;
    result=MatchOneToOne({arbitrary},{arbitrary},1.0,&tooComplex);
    CHECK(!tooComplex);
    CHECK(result.size()==1 && result[0].savedIndex==0 && result[0].liveIndex==0);
}

static void test_assignment_maximizes_cardinality_before_score(){
    std::vector<LayoutMatch> candidates={
        Candidate(0,0,0.90),Candidate(0,1,0.80),Candidate(1,0,0.85)
    };
    bool tooComplex=true;
    std::vector<LayoutMatch> matches=AssignOneToOne(2,2,candidates,&tooComplex);
    CHECK(!tooComplex);
    CHECK((MatchPairs(matches)==std::vector<std::pair<size_t,size_t>>({{0,1},{1,0}})));
    CHECK(std::fabs(matches[0].score-0.80)<1e-12 && std::fabs(matches[1].score-0.85)<1e-12);
}

static void test_assignment_maximizes_total_score_at_same_cardinality(){
    std::vector<LayoutMatch> candidates={
        Candidate(0,0,0.90),Candidate(0,1,0.80),Candidate(1,0,0.70),Candidate(1,1,0.10)
    };
    std::vector<LayoutMatch> matches=AssignOneToOne(2,2,candidates);
    CHECK((MatchPairs(matches)==std::vector<std::pair<size_t,size_t>>({{0,1},{1,0}})));
    CHECK(std::fabs(matches[0].score+matches[1].score-1.50)<1e-12);
}

static void test_assignment_ties_are_deterministic_across_input_order(){
    std::vector<LayoutMatch> candidates={
        Candidate(0,0,0.5),Candidate(0,1,0.5),Candidate(1,0,0.5),Candidate(1,1,0.5)
    };
    const std::vector<std::pair<size_t,size_t>> expected={{0,0},{1,1}};
    CHECK(MatchPairs(AssignOneToOne(2,2,candidates))==expected);
    std::mt19937 rng(0x51A81E);
    for(int run=0;run<20;++run){
        std::shuffle(candidates.begin(),candidates.end(),rng);
        CHECK(MatchPairs(AssignOneToOne(2,2,candidates))==expected);
    }

    candidates={
        Candidate(0,0,0.5),Candidate(0,1,0.5),Candidate(0,2,0.5),
        Candidate(1,1,0.5),Candidate(1,2,0.5),Candidate(2,0,0.5)
    };
    const std::vector<std::pair<size_t,size_t>> collisionExpected={{0,2},{1,1},{2,0}};
    CHECK(MatchPairs(AssignOneToOne(3,3,candidates))==collisionExpected);
    std::reverse(candidates.begin(),candidates.end());
    CHECK(MatchPairs(AssignOneToOne(3,3,candidates))==collisionExpected);
}

static void test_assignment_filters_and_deduplicates_without_mutating_input(){
    std::vector<LayoutMatch> candidates={
        Candidate(0,0,0.20),Candidate(0,0,0.90),Candidate(0,0,0.50),
        Candidate(1,1,0.70),Candidate(2,0,1.00),Candidate(0,2,1.00),
        Candidate(0,1,std::numeric_limits<double>::quiet_NaN()),
        Candidate(1,0,std::numeric_limits<double>::infinity()),Candidate(1,0,-0.10)
    };
    const std::vector<LayoutMatch> original=candidates;
    bool tooComplex=true;
    std::vector<LayoutMatch> matches=AssignOneToOne(2,2,candidates,&tooComplex);
    CHECK(!tooComplex);
    CHECK((MatchPairs(matches)==std::vector<std::pair<size_t,size_t>>({{0,0},{1,1}})));
    CHECK(matches.size()==2 && matches[0].score==0.90 && matches[1].score==0.70);
    CHECK(SameCandidateInput(candidates,original));
    CHECK(AssignOneToOne(0,2,candidates).empty());
    CHECK(AssignOneToOne(2,0,candidates).empty());
    CHECK(AssignOneToOne(2,2,{}).empty());
}

struct OracleAssignmentResult {
    bool initialized=false;
    long long totalUnits=0;
    long long deterministicTieSum=0;
    std::vector<std::pair<size_t,size_t>> pairs;
};

static void VisitOracleAssignments(size_t savedIndex, size_t savedCount, size_t liveCount,
        const std::vector<std::vector<long long>>& units, const std::vector<std::vector<long long>>& tieOrders,
        std::vector<bool>& usedLive, std::vector<std::pair<size_t,size_t>>& current,
        long long totalUnits, long long deterministicTieSum,
        OracleAssignmentResult& best){
    if(savedIndex==savedCount){
        if(!best.initialized || current.size()>best.pairs.size() ||
                (current.size()==best.pairs.size() && (totalUnits>best.totalUnits ||
                (totalUnits==best.totalUnits && deterministicTieSum<best.deterministicTieSum)))){
            best.initialized=true; best.totalUnits=totalUnits;
            best.deterministicTieSum=deterministicTieSum; best.pairs=current;
        }
        return;
    }
    VisitOracleAssignments(savedIndex+1,savedCount,liveCount,units,tieOrders,usedLive,current,
        totalUnits,deterministicTieSum,best);
    for(size_t liveIndex=0;liveIndex<liveCount;++liveIndex){
        if(usedLive[liveIndex] || units[savedIndex][liveIndex]<0) continue;
        usedLive[liveIndex]=true;
        current.push_back(std::make_pair(savedIndex,liveIndex));
        VisitOracleAssignments(savedIndex+1,savedCount,liveCount,units,tieOrders,usedLive,current,
            totalUnits+units[savedIndex][liveIndex],
            deterministicTieSum+tieOrders[savedIndex][liveIndex],best);
        current.pop_back();
        usedLive[liveIndex]=false;
    }
}

static OracleAssignmentResult ExhaustiveAssignmentOracle(size_t savedCount, size_t liveCount,
        const std::vector<LayoutMatch>& candidates){
    std::vector<std::vector<long long>> units(savedCount,std::vector<long long>(liveCount,-1));
    std::vector<std::vector<long long>> tieOrders(savedCount,std::vector<long long>(liveCount,-1));
    std::map<std::pair<size_t,size_t>,double> bestScores;
    for(const auto& candidate : candidates){
        if(candidate.savedIndex>=savedCount || candidate.liveIndex>=liveCount ||
                !std::isfinite(candidate.score) || candidate.score<0) continue;
        std::pair<size_t,size_t> key(candidate.savedIndex,candidate.liveIndex);
        auto existing=bestScores.find(key);
        if(existing==bestScores.end() || existing->second<candidate.score) bestScores[key]=candidate.score;
    }
    long long tieOrder=0;
    for(const auto& item : bestScores){
        units[item.first.first][item.first.second]=std::llround(item.second*1000000000.0);
        tieOrders[item.first.first][item.first.second]=++tieOrder;
    }
    OracleAssignmentResult result;
    std::vector<bool> usedLive(liveCount,false);
    std::vector<std::pair<size_t,size_t>> current;
    VisitOracleAssignments(0,savedCount,liveCount,units,tieOrders,usedLive,current,0,0,result);
    return result;
}

static long long AssignmentTieSum(const std::vector<LayoutMatch>& candidates,
        const std::vector<LayoutMatch>& matches){
    std::map<std::pair<size_t,size_t>,double> bestScores;
    for(const auto& candidate : candidates){
        if(!std::isfinite(candidate.score) || candidate.score<0) continue;
        std::pair<size_t,size_t> key(candidate.savedIndex,candidate.liveIndex);
        auto existing=bestScores.find(key);
        if(existing==bestScores.end() || existing->second<candidate.score) bestScores[key]=candidate.score;
    }
    std::set<std::pair<size_t,size_t>> selected;
    for(const auto& match : matches) selected.insert(std::make_pair(match.savedIndex,match.liveIndex));
    long long tieOrder=0,total=0;
    for(const auto& item : bestScores) if(++tieOrder && selected.count(item.first)) total+=tieOrder;
    return total;
}

static void test_assignment_randomized_against_exhaustive_oracle(){
    std::mt19937 rng(0xC0FFEE);
    for(int testCase=0;testCase<240;++testCase){
        size_t savedCount=rng()%5, liveCount=rng()%5;
        std::vector<LayoutMatch> candidates;
        for(size_t savedIndex=0;savedIndex<savedCount;++savedIndex){
            for(size_t liveIndex=0;liveIndex<liveCount;++liveIndex){
                if(rng()%100>=62) continue;
                long long scoreUnits=1+(rng()%7);
                candidates.push_back(Candidate(savedIndex,liveIndex,scoreUnits/1000000000.0));
            }
        }
        std::shuffle(candidates.begin(),candidates.end(),rng);
        OracleAssignmentResult expected=ExhaustiveAssignmentOracle(savedCount,liveCount,candidates);
        std::vector<LayoutMatch> actual=AssignOneToOne(savedCount,liveCount,candidates);
        long long actualUnits=0;
        for(const auto& match : actual) actualUnits+=std::llround(match.score*1000000000.0);
        CHECK(actual.size()==expected.pairs.size());
        CHECK(actualUnits==expected.totalUnits);
        CHECK(AssignmentTieSum(candidates,actual)==expected.deterministicTieSum);
        CHECK(MatchesAreSortedAndUnique(actual));
    }
}

static void test_assignment_candidate_cap_direct_and_generated(){
    std::vector<LayoutMatch> dense;
    dense.reserve(MAX_MATCH_CANDIDATES+1);
    for(size_t savedIndex=0;savedIndex<64;++savedIndex)
        for(size_t liveIndex=0;liveIndex<128;++liveIndex)
            dense.push_back(Candidate(savedIndex,liveIndex,1.0));
    CHECK(dense.size()==MAX_MATCH_CANDIDATES);
    bool tooComplex=true;
    std::vector<LayoutMatch> exact=AssignOneToOne(64,128,dense,&tooComplex);
    CHECK(!tooComplex && exact.size()==64 && MatchesAreSortedAndUnique(exact));

    dense.push_back(Candidate(64,0,1.0));
    tooComplex=false;
    CHECK(AssignOneToOne(65,128,dense,&tooComplex).empty());
    CHECK(tooComplex);

    LayoutWin fingerprint=MatchRecord("firefox","Inbox","mail.example",1,{{"mail.example",1}});
    std::vector<LayoutWin> saved(64,fingerprint),live(128,fingerprint);
    tooComplex=true;
    exact=MatchOneToOne(saved,live,1.0,&tooComplex);
    CHECK(!tooComplex && exact.size()==64 && MatchesAreSortedAndUnique(exact));
    saved.push_back(fingerprint);
    tooComplex=false;
    CHECK(MatchOneToOne(saved,live,1.0,&tooComplex).empty());
    CHECK(tooComplex);

    std::vector<LayoutMatch> sparse;
    sparse.reserve(MAX_MATCH_CANDIDATES);
    for(size_t index=0;index<MAX_MATCH_CANDIDATES;++index)
        sparse.push_back(Candidate(index,index,1.0));
    tooComplex=true;
    std::vector<LayoutMatch> sparseResult=AssignOneToOne(
        MAX_MATCH_CANDIDATES,MAX_MATCH_CANDIDATES,sparse,&tooComplex);
    CHECK(!tooComplex);
    CHECK(sparseResult.size()==MAX_MATCH_CANDIDATES && MatchesAreSortedAndUnique(sparseResult));
}

static void test_assignment_flow_work_budget_rejects_connected_cycle(){
    CHECK(MAX_MATCH_FLOW_WORK==1000000);
    const size_t nodeCount=MAX_MATCH_CANDIDATES/2;
    std::vector<LayoutMatch> candidates;
    candidates.reserve(MAX_MATCH_CANDIDATES);
    for(size_t index=0;index<nodeCount;++index){
        candidates.push_back(Candidate(index,index,1.0));
        candidates.push_back(Candidate(index,(index+1)%nodeCount,0.5));
    }
    CHECK(candidates.size()==MAX_MATCH_CANDIDATES);
    bool tooComplex=false;
    std::vector<LayoutMatch> result=AssignOneToOne(nodeCount,nodeCount,candidates,&tooComplex);
    CHECK(result.empty());
    CHECK(tooComplex);
    CHECK(candidates.size()==MAX_MATCH_CANDIDATES);
    CHECK(candidates.front().savedIndex==0 && candidates.front().liveIndex==0);
    CHECK(candidates.back().savedIndex==nodeCount-1 && candidates.back().liveIndex==0);
}

static void test_assignment_checked_score_scaling_boundary(){
    const double nearLimit=9223372036.0;
    bool tooComplex=true;
    std::vector<LayoutMatch> accepted=AssignOneToOne(1,1,{Candidate(0,0,nearLimit)},&tooComplex);
    CHECK(!tooComplex);
    CHECK(accepted.size()==1 && accepted[0].score==nearLimit);

    const double excessive=9223372037.0;
    for(int run=0;run<2;++run){
        tooComplex=false;
        CHECK(AssignOneToOne(1,1,{Candidate(0,0,excessive)},&tooComplex).empty());
        CHECK(tooComplex);
    }

    std::vector<LayoutMatch> widePath={
        Candidate(0,2,0),Candidate(1,1,0),Candidate(1,2,nearLimit),
        Candidate(2,0,0),Candidate(2,1,nearLimit)
    };
    tooComplex=true;
    std::vector<LayoutMatch> wideResult=AssignOneToOne(3,3,widePath,&tooComplex);
    CHECK(!tooComplex);
    CHECK((MatchPairs(wideResult)==std::vector<std::pair<size_t,size_t>>({{0,2},{1,1},{2,0}})));
}
static std::string FailingRecordIdGenerator(){ return std::string(); }
static std::string ConstantRecordIdGenerator(){ return "{00000000-0000-0000-0000-000000000099}"; }
static std::string MalformedRecordIdGenerator(){ return "not-a-guid"; }
static std::string ZeroRecordIdGenerator(){ return "{00000000-0000-0000-0000-000000000000}"; }

static std::string DeterministicRecordId(size_t ordinal){
    GUID id{};
    id.Data1=static_cast<unsigned long>(ordinal+1);
    return W2U8(GuidToString(id));
}

static size_t& CountingRecordIdGeneratorCalls(){
    static size_t calls=0;
    return calls;
}

static void ResetCountingRecordIdGenerator(){
    CountingRecordIdGeneratorCalls()=0;
}

static std::string CountingRecordIdGenerator(){
    const size_t call=CountingRecordIdGeneratorCalls()++;
    return DeterministicRecordId(MAX_LAYOUT_RECORDS+100+call);
}

static size_t& CountingReconcileMatcherCalls(){
    static size_t calls=0;
    return calls;
}

static void ResetCountingReconcileMatcher(){
    CountingReconcileMatcherCalls()=0;
}

static std::vector<LayoutMatch> CountingReconcileMatcher(
        const std::vector<LayoutWin>& saved,
        const std::vector<LayoutWin>& live,
        double acceptScore,
        bool* tooComplex){
    ++CountingReconcileMatcherCalls();
    return MatchOneToOne(saved,live,acceptScore,tooComplex);
}

static std::vector<LayoutMatch> DuplicateSavedReconcileMatcher(
        const std::vector<LayoutWin>& saved,
        const std::vector<LayoutWin>& live,
        double acceptScore,
        bool* tooComplex){
    (void)saved; (void)live; (void)acceptScore;
    if(tooComplex) *tooComplex=false;
    return {Candidate(0,0,0.9),Candidate(0,1,0.9)};
}

static std::vector<LayoutMatch> DuplicateLiveReconcileMatcher(
        const std::vector<LayoutWin>& saved,
        const std::vector<LayoutWin>& live,
        double acceptScore,
        bool* tooComplex){
    (void)saved; (void)live; (void)acceptScore;
    if(tooComplex) *tooComplex=false;
    return {Candidate(0,0,0.9),Candidate(1,0,0.9)};
}

enum class InjectedMatchMode {
    SavedOutOfRange, LiveOutOfRange, OtherAppLive, NotANumber,
    Infinity, BelowThreshold, TooMany, Valid
};

static InjectedMatchMode& CurrentInjectedMatchMode(){
    static InjectedMatchMode mode=InjectedMatchMode::Valid;
    return mode;
}

static std::vector<LayoutMatch> ConfigurableReconcileMatcher(
        const std::vector<LayoutWin>& saved,
        const std::vector<LayoutWin>& live,
        double acceptScore,
        bool* tooComplex){
    if(tooComplex) *tooComplex=false;
    switch(CurrentInjectedMatchMode()){
    case InjectedMatchMode::SavedOutOfRange:
        return {Candidate(saved.size(),0,acceptScore)};
    case InjectedMatchMode::LiveOutOfRange:
        return {Candidate(0,live.size(),acceptScore)};
    case InjectedMatchMode::OtherAppLive:
        return {Candidate(0,1,acceptScore)};
    case InjectedMatchMode::NotANumber:
        return {Candidate(0,0,std::numeric_limits<double>::quiet_NaN())};
    case InjectedMatchMode::Infinity:
        return {Candidate(0,0,std::numeric_limits<double>::infinity())};
    case InjectedMatchMode::BelowThreshold:
        return {Candidate(0,0,acceptScore-0.01)};
    case InjectedMatchMode::TooMany:
        return std::vector<LayoutMatch>(
            MAX_LAYOUT_RECORDS+1,Candidate(0,0,acceptScore));
    case InjectedMatchMode::Valid:
        return {Candidate(0,0,acceptScore)};
    }
    return {};
}

static bool SameLayoutWinFields(const LayoutWin& left, const LayoutWin& right){
    return left.recordId==right.recordId && left.app==right.app &&
        left.deskIndex==right.deskIndex && GuidEq(left.desktop,right.desktop) &&
        left.activeTitle==right.activeTitle && left.activeDomain==right.activeDomain &&
        left.tabCount==right.tabCount && left.counts==right.counts &&
        left.lastSeenUtc==right.lastSeenUtc &&
        left.missingSinceUtc==right.missingSinceUtc &&
        left.provisional==right.provisional;
}

static bool SameLayoutWinVectors(const std::vector<LayoutWin>& left,
        const std::vector<LayoutWin>& right){
    if(left.size()!=right.size()) return false;
    for(size_t i=0;i<left.size();++i)
        if(!SameLayoutWinFields(left[i],right[i])) return false;
    return true;
}

static LayoutWin ReconcileTestRecord(const std::string& recordId, const char* app,
        const char* title, const char* domain, int deskIndex, const GUID& desktop,
        UnixSeconds lastSeenUtc){
    LayoutWin record=MatchRecord(app,title,domain,1,{{domain,1}});
    record.recordId=recordId;
    record.deskIndex=deskIndex;
    record.desktop=desktop;
    record.lastSeenUtc=lastSeenUtc;
    return record;
}

static ReconcilePlan ValidCommitPlan(const std::string& app, UnixSeconds nowUtc,
        ReconcileFreshness freshness=ReconcileFreshness::Fresh){
    ReconcilePlan plan;
    plan.app=app;
    plan.nowUtc=nowUtc;
    plan.freshness=freshness;
    return plan;
}

static NewRecordRequest PlannedNewRecord(size_t liveIndex, const std::string& recordId){
    NewRecordRequest request;
    request.liveIndex=liveIndex;
    request.recordId=recordId;
    return request;
}

static RestoreRequest PlannedRestore(size_t savedIndex, size_t liveIndex,
        const GUID& destination){
    RestoreRequest request;
    request.savedIndex=savedIndex;
    request.liveIndex=liveIndex;
    request.destination=destination;
    return request;
}

static LayoutWin PersistedProvisionalRecord(
        const std::string& recordId,const GUID& desktop,UnixSeconds lastSeenUtc){
    LayoutWin record;
    record.recordId=recordId;
    record.app="firefox";
    record.deskIndex=1;
    record.desktop=desktop;
    record.lastSeenUtc=lastSeenUtc;
    record.provisional=true;
    return record;
}

static void test_layout_provisional_companions_do_not_consume_record_cap(){
    const GUID desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    std::vector<LayoutWin> records;
    records.reserve(MAX_LAYOUT_RECORDS);
    for(size_t i=0;i<MAX_LAYOUT_RECORDS;++i){
        LayoutWin record=ReconcileTestRecord(
            DeterministicRecordId(14000+i),"firefox","Provisional",
            "provisional.example",1,desktop,1700000000);
        record.provisional=true;
        records.push_back(record);
    }
    std::string bytes;
    std::string error="stale";
    CHECK(BuildCheckedLayoutSnapshot({},records,1700000000,bytes,&error));
    CHECK(error.empty());

    std::vector<DeskRec> parsedDesks;
    std::vector<LayoutWin> parsedRecords;
    int sourceVersion=0;
    CHECK(ParseLayout(bytes,parsedDesks,parsedRecords,1800000000,&error,
                      &sourceVersion));
    CHECK(error.empty() && sourceVersion==4);
    CHECK(parsedRecords.size()==MAX_LAYOUT_RECORDS);
    CHECK(parsedRecords.size()==MAX_LAYOUT_RECORDS &&
          parsedRecords.front().provisional && parsedRecords.back().provisional);

    LayoutWin overflow=records.back();
    overflow.recordId=DeterministicRecordId(14000+MAX_LAYOUT_RECORDS);
    const std::string overflowSnapshot=SerializeLayout({}, {overflow});
    const std::string overflowBody=overflowSnapshot.substr(
        std::string("# VDE snapshot v4\n").size());
    DeskRec sentinelDesk{};
    sentinelDesk.index=77;
    LayoutWin sentinel=StrictV4Record();
    sentinel.activeTitle="sentinel";
    parsedDesks={sentinelDesk};
    parsedRecords={sentinel};
    CHECK(!ParseLayout(bytes+overflowBody,parsedDesks,parsedRecords,
                       1800000000,&error));
    CHECK(!error.empty());
    CHECK(parsedDesks.size()==1 && parsedDesks[0].index==77);
    CHECK(parsedRecords.size()==1 &&
          parsedRecords[0].activeTitle=="sentinel");
}

static void test_fresh_reconcile_adopts_one_persisted_provisional_with_same_id(){
    const UnixSeconds now=2000000050;
    const GUID desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    LayoutWin saved=PersistedProvisionalRecord(
        "{00000000-0000-0000-0000-000000000391}",desktop,now-50);
    LayoutWin live=MatchRecord(
        "firefox","fresh identity","fresh.example",2,
        {{"fresh.example",2}});
    live.deskIndex=1;
    live.desktop=desktop;
    ResetCountingRecordIdGenerator();

    ReconcilePlan plan=PlanAppReconcile(
        {saved},{live},"firefox",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator);

    CHECK(!plan.deferred);
    CHECK(plan.matches.size()==1 && plan.matches[0].savedIndex==0 &&
          plan.matches[0].liveIndex==0);
    CHECK(plan.newRecords.empty() && plan.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    std::vector<LayoutWin> committed=CommitAppReconcile(
        {saved},{live},plan,{},now);
    CHECK(committed.size()==1 && committed[0].recordId==saved.recordId);
    CHECK(committed.size()==1 && !committed[0].provisional);
    CHECK(committed.size()==1 && committed[0].activeTitle==live.activeTitle &&
          committed[0].counts==live.counts);
    ResetCountingRecordIdGenerator();
}

static void test_fresh_reconcile_clears_multiple_matched_provisionals(){
    const UnixSeconds now=2000000060;
    const GUID desktopA=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID desktopB=G(L"{231A0000-0000-0000-0000-000000000002}");
    LayoutWin savedA=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000000392}","firefox","Alpha",
        "alpha.example",1,desktopA,now-50);
    LayoutWin savedB=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000000393}","firefox","Beta",
        "beta.example",2,desktopB,now-50);
    savedA.provisional=true;
    savedB.provisional=true;
    LayoutWin liveA=savedA;
    LayoutWin liveB=savedB;
    liveA.recordId.clear();
    liveB.recordId.clear();
    liveA.provisional=false;
    liveB.provisional=false;

    ReconcilePlan plan=PlanAppReconcile(
        {savedA,savedB},{liveB,liveA},"firefox",now);
    CHECK(!plan.deferred && plan.matches.size()==2);
    CHECK(plan.newRecords.empty() && plan.missingSavedIndices.empty());
    std::vector<LayoutWin> committed=CommitAppReconcile(
        {savedA,savedB},{liveB,liveA},plan,{},now);
    CHECK(committed.size()==2);
    CHECK(committed.size()==2 && committed[0].recordId==savedA.recordId &&
          committed[1].recordId==savedB.recordId);
    CHECK(committed.size()==2 && !committed[0].provisional &&
          !committed[1].provisional);
}

static void test_fresh_reconcile_defers_ambiguous_provisional_adoption(){
    const UnixSeconds now=2000000070;
    const GUID desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    const std::pair<size_t,size_t> cardinalities[]={{1,2},{2,1},{2,2}};
    for(const auto& cardinality : cardinalities){
        std::vector<LayoutWin> saved;
        std::vector<LayoutWin> live;
        for(size_t i=0;i<cardinality.first;++i)
            saved.push_back(PersistedProvisionalRecord(
                DeterministicRecordId(9300+i),desktop,now-50));
        for(size_t i=0;i<cardinality.second;++i){
            LayoutWin observed=MatchRecord(
                "firefox",("Live "+std::to_string(i)).c_str(),
                ("live"+std::to_string(i)+".example").c_str(),1,{});
            observed.deskIndex=1;
            observed.desktop=desktop;
            live.push_back(observed);
        }
        ResetCountingRecordIdGenerator();
        ReconcilePlan plan=PlanAppReconcile(
            saved,live,"firefox",now,{},ReconcileFreshness::Fresh,
            CountingRecordIdGenerator);
        CHECK(plan.deferred);
        CHECK(plan.matches.empty() && plan.restores.empty());
        CHECK(plan.newRecords.empty() && plan.missingSavedIndices.empty());
        CHECK(CountingRecordIdGeneratorCalls()==0);
    }
    ResetCountingRecordIdGenerator();
}

static void test_failed_chrome_restore_retains_saved_destination_and_marks_seen(){
    const UnixSeconds now=2000000100;
    LayoutWin saved=MatchRecord("chrome","A","a.com",1,{{"a.com",1}});
    saved.recordId="{00000000-0000-0000-0000-000000000401}";
    saved.deskIndex=1;
    saved.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    saved.lastSeenUtc=now-50;
    saved.missingSinceUtc=now-20;
    LayoutWin live=MatchRecord("chrome","A","a.com",1,{{"a.com",1}});
    live.deskIndex=4;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");

    ReconcilePlan plan=PlanAppReconcile({saved},{live},"chrome",now);
    CHECK(plan.restores.size()==1);
    CHECK(plan.restores[0].savedIndex==0 && plan.restores[0].liveIndex==0);
    CHECK(GuidEq(plan.restores[0].destination,saved.desktop));
    std::vector<LayoutWin> committed=CommitAppReconcile({saved},{live},plan,{},now);

    CHECK(committed.size()==1);
    CHECK(GuidEq(committed[0].desktop,saved.desktop));
    CHECK(committed[0].deskIndex==saved.deskIndex);
    CHECK(committed[0].recordId==saved.recordId);
    CHECK(committed[0].lastSeenUtc==now && committed[0].missingSinceUtc==0);
}

static void test_empty_chrome_reconcile_marks_only_chrome_missing(){
    const UnixSeconds now=2000000200;
    LayoutWin firefox=MatchRecord("firefox","Firefox","ff.example",3,{{"ff.example",3}});
    firefox.recordId="{00000000-0000-0000-0000-000000000402}";
    firefox.deskIndex=0;
    firefox.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    firefox.lastSeenUtc=now-100;
    LayoutWin chrome=MatchRecord("chrome","Chrome","chrome.example",2,{{"chrome.example",2}});
    chrome.recordId="{00000000-0000-0000-0000-000000000403}";
    chrome.deskIndex=1;
    chrome.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    chrome.lastSeenUtc=now-25;

    ReconcilePlan plan=PlanAppReconcile({firefox,chrome},{},"chrome",now);
    CHECK(plan.matches.empty() && plan.restores.empty() && plan.newRecords.empty());
    CHECK(plan.missingSavedIndices==std::vector<size_t>({1}));
    std::vector<LayoutWin> committed=CommitAppReconcile({firefox,chrome},{},plan,{},now);

    CHECK(committed.size()==2);
    CHECK(SameLayoutWinFields(committed[0],firefox));
    CHECK(committed[1].missingSinceUtc==chrome.lastSeenUtc);
    CHECK(committed[1].lastSeenUtc==chrome.lastSeenUtc);
}

static void test_reserved_chrome_record_cannot_be_stolen_by_duplicate(){
    const UnixSeconds now=2000000300;
    LayoutWin bound=MatchRecord("chrome","Same","same.com",1,{{"same.com",1}});
    bound.recordId="{ABCDEFAB-CDEF-ABCD-EFAB-CDEFABCDEFAB}";
    bound.deskIndex=0;
    bound.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    bound.lastSeenUtc=now-10;
    LayoutWin duplicate=MatchRecord("chrome","Same","same.com",1,{{"same.com",1}});
    duplicate.deskIndex=2;
    duplicate.desktop=G(L"{231A0000-0000-0000-0000-000000000003}");
    const std::set<std::string> reserved={"abcdefab-cdef-abcd-efab-cdefabcdefab"};

    ReconcilePlan plan=PlanAppReconcile(
        {bound},{duplicate},"chrome",now,reserved,ReconcileFreshness::Fresh,
        ConstantRecordIdGenerator);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.missingSavedIndices.empty());
    CHECK(plan.newRecords.size()==1 && plan.newRecords[0].liveIndex==0);
    CHECK(plan.newRecords[0].recordId==ConstantRecordIdGenerator());
    std::vector<LayoutWin> committed=CommitAppReconcile({bound},{duplicate},plan,{},now);

    CHECK(committed.size()==2);
    CHECK(SameLayoutWinFields(committed[0],bound));
    CHECK(committed[1].recordId==ConstantRecordIdGenerator());
    CHECK(committed[1].deskIndex==duplicate.deskIndex);
    CHECK(GuidEq(committed[1].desktop,duplicate.desktop));
}

static void test_same_desktop_match_learns_live_index_without_restore(){
    const UnixSeconds now=2000000400;
    LayoutWin saved=MatchRecord("chrome","A","a.com",1,{{"a.com",1}});
    saved.recordId="{00000000-0000-0000-0000-000000000405}";
    saved.deskIndex=2;
    saved.desktop=G(L"{231A0000-0000-0000-0000-000000000003}");
    LayoutWin live=MatchRecord("chrome","A","a.com",1,{{"a.com",1}});
    live.deskIndex=8;
    live.desktop=saved.desktop;

    ReconcilePlan plan=PlanAppReconcile({saved},{live},"chrome",now);
    CHECK(plan.matches.size()==1);
    CHECK(plan.restores.empty());
    std::vector<LayoutWin> committed=CommitAppReconcile({saved},{live},plan,{},now);

    CHECK(committed.size()==1);
    CHECK(committed[0].recordId==saved.recordId);
    CHECK(GuidEq(committed[0].desktop,live.desktop));
    CHECK(committed[0].deskIndex==live.deskIndex);
    CHECK(committed[0].lastSeenUtc==now);
}

static void test_late_window_after_first_wave_restores_before_save(){
    LcState state;
    CHECK(LcObserve(state,true,1,10,100,1,0).action==LcAction::None);
    LcDecision first=LcObserve(state,true,1,10,100,1,1);
    CHECK(first.action==LcAction::BeginRestore && first.generation!=0);
    LcRestoreCompleted(state,first.generation,LcRestoreOutcome::Success,100,1,2);
    CHECK(LcObserve(state,true,3,30,100,1,3).action==LcAction::None);
    LcDecision late=LcObserve(state,true,3,30,100,1,4);
    CHECK(late.action==LcAction::BeginRestore && late.generation!=first.generation);
    CHECK(LcObserve(state,true,3,30,100,1,5).action==LcAction::None);

    const UnixSeconds now=2000000500;
    LayoutWin savedA=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    savedA.recordId="{00000000-0000-0000-0000-000000000406}";
    savedA.deskIndex=0;
    savedA.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    savedA.lastSeenUtc=now-10;
    LayoutWin savedB=MatchRecord("firefox","B","b.com",1,{{"b.com",1}});
    savedB.recordId="{00000000-0000-0000-0000-000000000407}";
    savedB.deskIndex=1;
    savedB.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    savedB.lastSeenUtc=now-100;
    savedB.missingSinceUtc=now-100;
    LayoutWin liveB=MatchRecord("firefox","B","b.com",1,{{"b.com",1}});
    liveB.deskIndex=4;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");
    const std::set<std::string> reserved={savedA.recordId};

    ReconcilePlan plan=PlanAppReconcile({savedA,savedB},{liveB},"firefox",now,reserved);
    CHECK(plan.matches.size()==1 && plan.matches[0].savedIndex==1 &&
        plan.matches[0].liveIndex==0);
    CHECK(plan.restores.size()==1 && plan.restores[0].savedIndex==1 &&
        plan.restores[0].liveIndex==0);
    CHECK(plan.newRecords.empty() && plan.missingSavedIndices.empty());
    std::vector<LayoutWin> committed=CommitAppReconcile(
        {savedA,savedB},{liveB},plan,{0},now);
    CHECK(committed.size()==2);
    CHECK(SameLayoutWinFields(committed[0],savedA));
    CHECK(GuidEq(committed[1].desktop,savedB.desktop));

    LcRestoreCompleted(state,late.generation,LcRestoreOutcome::Success,100,1,6);
    CHECK(LcObserve(state,true,3,30,100,1,7).action==LcAction::None);
    CHECK(LcObserve(state,true,3,30,100,1,8).action==LcAction::None);
}

static void test_edge_retention_is_independent_while_firefox_stays_open(){
    const UnixSeconds now=2000000600;
    LayoutWin firefoxA=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    firefoxA.recordId="{00000000-0000-0000-0000-000000000408}";
    firefoxA.deskIndex=0;
    firefoxA.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    firefoxA.lastSeenUtc=now-50;
    LayoutWin firefoxB=MatchRecord("firefox","B","b.com",2,{{"b.com",2}});
    firefoxB.recordId="{00000000-0000-0000-0000-000000000409}";
    firefoxB.deskIndex=1;
    firefoxB.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    firefoxB.lastSeenUtc=now-40;
    std::vector<LayoutWin> firefoxRecords={firefoxA,firefoxB};
    const std::vector<LayoutWin> originalFirefox=firefoxRecords;

    LayoutWin edge=MatchRecord("msedge","Edge","edge.example",1,{{"edge.example",1}});
    edge.recordId="{00000000-0000-0000-0000-000000000410}";
    edge.deskIndex=2;
    edge.desktop=G(L"{231A0000-0000-0000-0000-000000000003}");
    edge.lastSeenUtc=now-10;
    std::vector<LayoutWin> edgeRecords={edge};
    LcState firefoxState,edgeState;

    CHECK(LcObserve(firefoxState,true,10,10,10,10,0).action==LcAction::None);
    LcDecision edgeMissing=LcObserve(edgeState,false,0,0,20,20,0);
    CHECK(edgeMissing.action==LcAction::MarkMissingFromLastSeen);
    ReconcilePlan absentPlan=PlanAppReconcile(edgeRecords,{},"msedge",now);
    edgeRecords=CommitAppReconcile(edgeRecords,{},absentPlan,{},now);
    CHECK(edgeRecords.size()==1 && edgeRecords[0].missingSinceUtc==edge.lastSeenUtc);
    CHECK(SameLayoutWinVectors(firefoxRecords,originalFirefox));

    LcDecision firefoxWave=LcObserve(firefoxState,true,10,10,10,10,1);
    CHECK(firefoxWave.action==LcAction::BeginRestore);
    LcRestoreCompleted(firefoxState,firefoxWave.generation,LcRestoreOutcome::Success,10,10,2);
    CHECK(LcObserve(edgeState,false,0,0,20,20,1).action==LcAction::None);
    CHECK(LcObserve(firefoxState,true,10,10,10,10,2).action==LcAction::None);
    CHECK(LcObserve(edgeState,true,30,30,20,21,2).action==LcAction::None);

    LayoutWin liveEdge=MatchRecord("msedge","Edge","edge.example",1,{{"edge.example",1}});
    liveEdge.deskIndex=4;
    liveEdge.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");
    ReconcilePlan returnPlan=PlanAppReconcile(edgeRecords,{liveEdge},"msedge",now+1);
    CHECK(returnPlan.restores.size()==1);
    edgeRecords=CommitAppReconcile(edgeRecords,{liveEdge},returnPlan,{0},now+1);
    LcDecision edgeReturn=LcObserve(edgeState,true,30,30,20,21,3);
    CHECK(edgeReturn.action==LcAction::BeginRestore);
    LcRestoreCompleted(edgeState,edgeReturn.generation,LcRestoreOutcome::Success,20,21,4);
    CHECK(LcObserve(firefoxState,true,10,10,10,10,3).action==LcAction::None);
    CHECK(SameLayoutWinVectors(firefoxRecords,originalFirefox));
}

static void test_firefox_sibling_reappears_while_first_window_stays_open(){
    const UnixSeconds now=2000000700;
    LayoutWin savedA=MatchRecord("firefox","A","a.com",1,{{"a.com",1}});
    savedA.recordId="{00000000-0000-0000-0000-000000000411}";
    savedA.deskIndex=0;
    savedA.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    savedA.lastSeenUtc=now-20;
    LayoutWin savedB=MatchRecord("firefox","B","b.com",1,{{"b.com",1}});
    savedB.recordId="{00000000-0000-0000-0000-000000000412}";
    savedB.deskIndex=1;
    savedB.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    savedB.lastSeenUtc=now-10;
    const std::set<std::string> reserved={savedA.recordId};

    ReconcilePlan missingPlan=PlanAppReconcile({savedA,savedB},{},"firefox",now,reserved);
    CHECK(missingPlan.missingSavedIndices==std::vector<size_t>({1}));
    std::vector<LayoutWin> afterMissing=CommitAppReconcile(
        {savedA,savedB},{},missingPlan,{},now);
    CHECK(afterMissing.size()==2);
    CHECK(SameLayoutWinFields(afterMissing[0],savedA));
    CHECK(afterMissing[1].missingSinceUtc==savedB.lastSeenUtc);

    LayoutWin liveB=MatchRecord("firefox","B","b.com",1,{{"b.com",1}});
    liveB.deskIndex=4;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");
    ReconcilePlan returnPlan=PlanAppReconcile(
        afterMissing,{liveB},"firefox",now+1,reserved);
    CHECK(returnPlan.matches.size()==1 && returnPlan.matches[0].savedIndex==1);
    CHECK(returnPlan.restores.size()==1 && returnPlan.restores[0].savedIndex==1 &&
        returnPlan.restores[0].liveIndex==0);
    CHECK(returnPlan.newRecords.empty() && returnPlan.missingSavedIndices.empty());
    std::vector<LayoutWin> failed=CommitAppReconcile(
        afterMissing,{liveB},returnPlan,{},now+1);

    CHECK(failed.size()==2);
    CHECK(SameLayoutWinFields(failed[0],savedA));
    CHECK(GuidEq(failed[1].desktop,savedB.desktop));
    CHECK(failed[1].deskIndex==savedB.deskIndex);
    CHECK(failed[1].lastSeenUtc==now+1 && failed[1].missingSinceUtc==0);
}

static void test_reconcile_plan_and_commit_preserve_input_vectors(){
    const UnixSeconds now=2000000800;
    LayoutWin saved=MatchRecord("chrome","A","a.com",1,{{"a.com",1}});
    saved.recordId="{00000000-0000-0000-0000-000000000413}";
    saved.deskIndex=0;
    saved.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    saved.lastSeenUtc=now-30;
    saved.missingSinceUtc=now-20;
    LayoutWin liveA=MatchRecord("chrome","A","a.com",1,{{"a.com",1}});
    liveA.deskIndex=3;
    liveA.desktop=G(L"{231A0000-0000-0000-0000-000000000004}");
    LayoutWin liveB=MatchRecord("chrome","B","b.com",1,{{"b.com",1}});
    liveB.deskIndex=4;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");
    std::vector<LayoutWin> existing={saved};
    std::vector<LayoutWin> live={liveA,liveB};
    const std::vector<LayoutWin> originalExisting=existing;
    const std::vector<LayoutWin> originalLive=live;

    ReconcilePlan plan=PlanAppReconcile(
        existing,live,"chrome",now,{},ReconcileFreshness::Fresh,
        ConstantRecordIdGenerator);
    CHECK(SameLayoutWinVectors(existing,originalExisting));
    CHECK(SameLayoutWinVectors(live,originalLive));
    std::vector<LayoutWin> committed=CommitAppReconcile(existing,live,plan,{0},now);
    CHECK(committed.size()==2);
    CHECK(SameLayoutWinVectors(existing,originalExisting));
    CHECK(SameLayoutWinVectors(live,originalLive));
}

static void test_reconcile_empty_generator_defers_transactionally(){
    const UnixSeconds now=2000000900;
    LayoutWin existingFirefox=MatchRecord(
        "firefox","Existing","existing.example",1,{{"existing.example",1}});
    existingFirefox.recordId="{00000000-0000-0000-0000-000000000414}";
    existingFirefox.deskIndex=0;
    existingFirefox.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    existingFirefox.lastSeenUtc=now-10;
    LayoutWin liveChrome=MatchRecord(
        "chrome","New","new.example",1,{{"new.example",1}});
    liveChrome.deskIndex=1;
    liveChrome.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    const std::vector<LayoutWin> existing={existingFirefox};

    ReconcilePlan plan=PlanAppReconcile(
        existing,{liveChrome},"chrome",now,{},ReconcileFreshness::Fresh,
        FailingRecordIdGenerator);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
    std::vector<LayoutWin> committed=CommitAppReconcile(existing,{liveChrome},plan,{},now);
    CHECK(SameLayoutWinVectors(committed,existing));
}

static void test_reconcile_invalid_generators_defer_transactionally(){
    const UnixSeconds now=2000001000;
    LayoutWin existingFirefox=MatchRecord(
        "firefox","Existing","existing.example",1,{{"existing.example",1}});
    existingFirefox.recordId="{00000000-0000-0000-0000-000000000415}";
    existingFirefox.deskIndex=0;
    existingFirefox.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    existingFirefox.lastSeenUtc=now-10;
    LayoutWin liveChrome=MatchRecord(
        "chrome","New","new.example",1,{{"new.example",1}});
    liveChrome.deskIndex=1;
    liveChrome.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    const std::vector<LayoutWin> existing={existingFirefox};
    const RecordIdGenerator generators[]={MalformedRecordIdGenerator,ZeroRecordIdGenerator};

    for(RecordIdGenerator generator : generators){
        ReconcilePlan plan=PlanAppReconcile(
            existing,{liveChrome},"chrome",now,{},ReconcileFreshness::Fresh,generator);
        CHECK(plan.deferred);
        CHECK(plan.matches.empty());
        CHECK(plan.restores.empty());
        CHECK(plan.newRecords.empty());
        CHECK(plan.missingSavedIndices.empty());
        std::vector<LayoutWin> committed=CommitAppReconcile(
            existing,{liveChrome},plan,{},now);
        CHECK(SameLayoutWinVectors(committed,existing));
    }
}

static void test_reconcile_generator_collision_with_any_existing_record_defers(){
    const UnixSeconds now=2000001100;
    LayoutWin liveChrome=MatchRecord(
        "chrome","New","new.example",1,{{"new.example",1}});
    liveChrome.deskIndex=4;
    liveChrome.desktop=G(L"{231A0000-0000-0000-0000-000000000005}");

    for(int variant=0;variant<3;++variant){
        LayoutWin existing=MatchRecord(
            "chrome","Existing","existing.example",1,{{"existing.example",1}});
        existing.recordId="00000000-0000-0000-0000-000000000099";
        existing.deskIndex=0;
        existing.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
        existing.lastSeenUtc=now-10;
        std::set<std::string> reserved;
        if(variant==0) existing.missingSinceUtc=now-WINDOW_RETENTION_SECONDS;
        if(variant==1) existing.app="firefox";
        if(variant==2) reserved.insert(existing.recordId);

        ReconcilePlan plan=PlanAppReconcile(
            {existing},{liveChrome},"chrome",now,reserved,ReconcileFreshness::Fresh,
            ConstantRecordIdGenerator);
        CHECK(plan.deferred);
        CHECK(plan.matches.empty());
        CHECK(plan.restores.empty());
        CHECK(plan.newRecords.empty());
        CHECK(plan.missingSavedIndices.empty());
    }
}

static void test_reconcile_duplicate_generated_ids_defer_transactionally(){
    const UnixSeconds now=2000001200;
    LayoutWin liveA=MatchRecord("chrome","A","a.example",1,{{"a.example",1}});
    liveA.deskIndex=0;
    liveA.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    LayoutWin liveB=MatchRecord("chrome","B","b.example",1,{{"b.example",1}});
    liveB.deskIndex=1;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");

    ReconcilePlan plan=PlanAppReconcile(
        {},{liveA,liveB},"chrome",now,{},ReconcileFreshness::Fresh,
        ConstantRecordIdGenerator);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
}

static void test_reconcile_unique_generated_id_commits_strict_v4(){
    const UnixSeconds now=2000001300;
    LayoutWin live=MatchRecord("chrome","New","new.example",1,{{"new.example",1}});
    live.deskIndex=0;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");

    ReconcilePlan plan=PlanAppReconcile(
        {},{live},"chrome",now,{},ReconcileFreshness::Fresh,
        ConstantRecordIdGenerator);
    CHECK(!plan.deferred);
    CHECK(plan.matches.empty() && plan.restores.empty() &&
        plan.missingSavedIndices.empty());
    CHECK(plan.newRecords.size()==1 &&
        plan.newRecords[0].recordId==ConstantRecordIdGenerator());
    std::vector<LayoutWin> committed=CommitAppReconcile({}, {live}, plan, {}, now);
    CHECK(committed.size()==1);

    std::string snapshot,error;
    CHECK(BuildCheckedLayoutSnapshot({},committed,now,snapshot,&error));
    CHECK(error.empty());
    std::vector<DeskRec> parsedDesks;
    std::vector<LayoutWin> parsed;
    int version=0;
    CHECK(ParseLayout(snapshot,parsedDesks,parsed,now,&error,&version));
    CHECK(error.empty() && version==4 && parsedDesks.empty());
    CHECK(parsed.size()==1 && SameLayoutWinFields(parsed[0],committed[0]));
}

static void test_reconcile_null_generator_defers_transactionally(){
    const UnixSeconds now=2000001400;
    LayoutWin existingFirefox=MatchRecord(
        "firefox","Existing","existing.example",1,{{"existing.example",1}});
    existingFirefox.recordId="{00000000-0000-0000-0000-000000000416}";
    existingFirefox.deskIndex=0;
    existingFirefox.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    existingFirefox.lastSeenUtc=now-10;
    LayoutWin liveChrome=MatchRecord(
        "chrome","New","new.example",1,{{"new.example",1}});
    liveChrome.deskIndex=1;
    liveChrome.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    const std::vector<LayoutWin> existing={existingFirefox};
    RecordIdGenerator generator=nullptr;

    ReconcilePlan plan=PlanAppReconcile(
        existing,{liveChrome},"chrome",now,{},ReconcileFreshness::Fresh,generator);
    CHECK(plan.app=="chrome" && plan.nowUtc==now &&
        plan.freshness==ReconcileFreshness::Fresh);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
    std::vector<LayoutWin> committed=CommitAppReconcile(existing,{liveChrome},plan,{},now);
    CHECK(SameLayoutWinVectors(committed,existing));
}

static void test_reconcile_match_preflight_too_complex_defers_cleanly(){
    const UnixSeconds now=2000001500;
    LayoutWin savedBase=MatchRecord("chrome","Saved","same.example",1,{{"same.example",1}});
    savedBase.deskIndex=0;
    savedBase.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    savedBase.lastSeenUtc=now-10;
    std::vector<LayoutWin> saved;
    saved.reserve(1001);
    for(size_t i=0;i<1001;++i){
        LayoutWin record=savedBase;
        record.recordId=DeterministicRecordId(i);
        saved.push_back(record);
    }
    LayoutWin liveBase=MatchRecord("chrome","Live","other.example",1,{{"other.example",1}});
    liveBase.deskIndex=1;
    liveBase.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    std::vector<LayoutWin> live(1000,liveBase);

    ResetCountingRecordIdGenerator();
    ReconcilePlan plan=PlanAppReconcile(
        saved,live,"chrome",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator);
    CHECK(plan.app=="chrome" && plan.nowUtc==now &&
        plan.freshness==ReconcileFreshness::Fresh);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    std::vector<LayoutWin> committed=CommitAppReconcile(saved,live,plan,{},now);
    CHECK(SameLayoutWinVectors(committed,saved));
    ResetCountingRecordIdGenerator();
}

static void test_reconcile_window_caps_defer_before_generation(){
    const UnixSeconds now=2000001600;
    LayoutWin existingBase=MatchRecord(
        "chrome","Existing","existing.example",1,{{"existing.example",1}});
    existingBase.deskIndex=0;
    existingBase.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    existingBase.lastSeenUtc=now-10;
    std::vector<LayoutWin> existing;
    existing.reserve(MAX_LAYOUT_RECORDS);
    for(size_t i=0;i<MAX_LAYOUT_RECORDS;++i){
        LayoutWin record=existingBase;
        record.recordId=DeterministicRecordId(i);
        existing.push_back(record);
    }
    std::vector<LayoutWin> strictExisting=existing;
    std::string snapshot,error;
    CHECK(BuildCheckedLayoutSnapshot({},strictExisting,now,snapshot,&error));
    CHECK(error.empty());

    LayoutWin liveFirefox=MatchRecord(
        "firefox","New","new.example",1,{{"new.example",1}});
    liveFirefox.deskIndex=1;
    liveFirefox.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");

    ResetCountingRecordIdGenerator();
    ReconcilePlan fullOutput=PlanAppReconcile(
        existing,{liveFirefox},"firefox",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator);
    CHECK(fullOutput.deferred);
    CHECK(fullOutput.matches.empty());
    CHECK(fullOutput.restores.empty());
    CHECK(fullOutput.newRecords.empty());
    CHECK(fullOutput.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existing,{liveFirefox},fullOutput,{},now),existing));

    std::vector<LayoutWin> oversizedExisting=existing;
    LayoutWin extra=existingBase;
    extra.recordId=DeterministicRecordId(MAX_LAYOUT_RECORDS);
    oversizedExisting.push_back(extra);
    ResetCountingRecordIdGenerator();
    ReconcilePlan existingOverflow=PlanAppReconcile(
        oversizedExisting,{liveFirefox},"firefox",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator);
    CHECK(existingOverflow.deferred);
    CHECK(existingOverflow.matches.empty());
    CHECK(existingOverflow.restores.empty());
    CHECK(existingOverflow.newRecords.empty());
    CHECK(existingOverflow.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);

    std::vector<LayoutWin> oversizedLive(MAX_LAYOUT_RECORDS+1,liveFirefox);
    ResetCountingRecordIdGenerator();
    ReconcilePlan liveOverflow=PlanAppReconcile(
        {},oversizedLive,"firefox",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator);
    CHECK(liveOverflow.deferred);
    CHECK(liveOverflow.matches.empty());
    CHECK(liveOverflow.restores.empty());
    CHECK(liveOverflow.newRecords.empty());
    CHECK(liveOverflow.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    ResetCountingRecordIdGenerator();
}

static void test_reconcile_malformed_reserved_id_defers_before_work(){
    const UnixSeconds now=2000001650;
    LayoutWin saved=ReconcileTestRecord(
        DeterministicRecordId(17000),"chrome","Saved","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin live=MatchRecord("chrome","Saved","same.example",1,{{"same.example",1}});
    live.deskIndex=1;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");

    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan plan=PlanAppReconcile(
        {saved},{live},"chrome",now,{"not-a-guid"},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,CountingReconcileMatcher);

    CHECK(plan.app=="chrome" && plan.nowUtc==now &&
        plan.freshness==ReconcileFreshness::Fresh);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
    CHECK(CountingReconcileMatcherCalls()==0);
    CHECK(CountingRecordIdGeneratorCalls()==0);
    ResetCountingReconcileMatcher();
    ResetCountingRecordIdGenerator();
}

static void test_reconcile_reserved_id_cap_is_fail_closed_at_boundary(){
    const UnixSeconds now=2000001675;
    LayoutWin live=MatchRecord("chrome","New","new.example",1,{{"new.example",1}});
    live.deskIndex=1;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    std::set<std::string> oversized;
    for(size_t i=0;i<MAX_LAYOUT_RECORDS+1;++i)
        oversized.insert(DeterministicRecordId(18000+i));

    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan rejected=PlanAppReconcile(
        {},{live},"chrome",now,oversized,ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,CountingReconcileMatcher);
    CHECK(rejected.app=="chrome" && rejected.nowUtc==now &&
        rejected.freshness==ReconcileFreshness::Fresh);
    CHECK(rejected.deferred);
    CHECK(rejected.matches.empty());
    CHECK(rejected.restores.empty());
    CHECK(rejected.newRecords.empty());
    CHECK(rejected.missingSavedIndices.empty());
    CHECK(CountingReconcileMatcherCalls()==0);
    CHECK(CountingRecordIdGeneratorCalls()==0);

    LayoutWin bound=ReconcileTestRecord(
        "{ABCDEFAB-CDEF-ABCD-EFAB-CDEFABCDEFAC}","chrome","Same","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin duplicate=MatchRecord("chrome","Same","same.example",1,{{"same.example",1}});
    duplicate.deskIndex=2;
    duplicate.desktop=G(L"{231A0000-0000-0000-0000-000000000003}");
    std::set<std::string> exact={"abcdefab-cdef-abcd-efab-cdefabcdefac"};
    for(size_t i=0;i<MAX_LAYOUT_RECORDS-1;++i)
        exact.insert(DeterministicRecordId(23000+i));
    CHECK(exact.size()==MAX_LAYOUT_RECORDS);

    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan accepted=PlanAppReconcile(
        {bound},{duplicate},"chrome",now,exact,ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,CountingReconcileMatcher);
    CHECK(!accepted.deferred);
    CHECK(accepted.matches.empty());
    CHECK(accepted.restores.empty());
    CHECK(accepted.missingSavedIndices.empty());
    CHECK(accepted.newRecords.size()==1 && accepted.newRecords[0].liveIndex==0);
    CHECK(CountingReconcileMatcherCalls()==1);
    CHECK(CountingRecordIdGeneratorCalls()==1);
    ResetCountingReconcileMatcher();
    ResetCountingRecordIdGenerator();
}

static void test_reconcile_guaranteed_capacity_defers_before_matcher(){
    const UnixSeconds now=2000001685;
    const size_t chromeSavedCount=64;
    std::vector<LayoutWin> existing;
    existing.reserve(MAX_LAYOUT_RECORDS);
    for(size_t i=0;i<MAX_LAYOUT_RECORDS;++i){
        const bool chrome=i>=MAX_LAYOUT_RECORDS-chromeSavedCount;
        LayoutWin record=ReconcileTestRecord(
            DeterministicRecordId(28000+i),chrome ? "chrome" : "firefox",
            chrome ? "Chrome" : "Firefox",chrome ? "same.example" : "ff.example",0,
            G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
        existing.push_back(record);
    }
    LayoutWin liveChrome=MatchRecord(
        "chrome","Chrome","same.example",1,{{"same.example",1}});
    liveChrome.deskIndex=0;
    liveChrome.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    std::vector<LayoutWin> overflowLive(chromeSavedCount+1,liveChrome);

    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan overflow=PlanAppReconcile(
        existing,overflowLive,"chrome",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,CountingReconcileMatcher);
    CHECK(overflow.app=="chrome" && overflow.nowUtc==now &&
        overflow.freshness==ReconcileFreshness::Fresh);
    CHECK(overflow.deferred);
    CHECK(overflow.matches.empty());
    CHECK(overflow.restores.empty());
    CHECK(overflow.newRecords.empty());
    CHECK(overflow.missingSavedIndices.empty());
    CHECK(CountingReconcileMatcherCalls()==0);
    CHECK(CountingRecordIdGeneratorCalls()==0);

    std::vector<LayoutWin> exactLive(chromeSavedCount,liveChrome);
    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan exact=PlanAppReconcile(
        existing,exactLive,"chrome",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,CountingReconcileMatcher);
    CHECK(!exact.deferred);
    CHECK(exact.matches.size()==chromeSavedCount);
    CHECK(exact.restores.empty());
    CHECK(exact.newRecords.empty());
    CHECK(exact.missingSavedIndices.empty());
    CHECK(CountingReconcileMatcherCalls()==1);
    CHECK(CountingRecordIdGeneratorCalls()==0);

    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan noMatcher=PlanAppReconcile(
        {},{liveChrome},"chrome",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,nullptr);
    CHECK(noMatcher.deferred);
    CHECK(noMatcher.matches.empty());
    CHECK(noMatcher.restores.empty());
    CHECK(noMatcher.newRecords.empty());
    CHECK(noMatcher.missingSavedIndices.empty());
    CHECK(CountingReconcileMatcherCalls()==0);
    CHECK(CountingRecordIdGeneratorCalls()==0);
    ResetCountingReconcileMatcher();
    ResetCountingRecordIdGenerator();
}

static std::vector<LayoutWin> MissingProjectionCapacityRecords(UnixSeconds now){
    std::vector<LayoutWin> existing;
    existing.reserve(MAX_LAYOUT_RECORDS);
    for(size_t i=0;i<MAX_LAYOUT_RECORDS-1;++i){
        existing.push_back(ReconcileTestRecord(
            DeterministicRecordId(40000+i),"firefox","Firefox","ff.example",0,
            G(L"{231A0000-0000-0000-0000-000000000001}"),now-10));
    }
    existing.push_back(ReconcileTestRecord(
        DeterministicRecordId(45000),"chrome","Old","old.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),
        now-WINDOW_RETENTION_SECONDS));
    return existing;
}

static bool HasRecordId(const std::vector<LayoutWin>& records,
        const std::string& recordId){
    for(const LayoutWin& record : records)
        if(record.recordId==recordId) return true;
    return false;
}

static void test_reconcile_projects_mark_missing_expiration_before_capacity(){
    const UnixSeconds now=2100000000;
    const size_t oldIndex=MAX_LAYOUT_RECORDS-1;
    const std::string newId=DeterministicRecordId(MAX_LAYOUT_RECORDS+100);
    std::vector<LayoutWin> existing=MissingProjectionCapacityRecords(now);
    const std::vector<LayoutWin> originalExisting=existing;
    const std::string oldId=existing[oldIndex].recordId;
    LayoutWin live=MatchRecord("chrome","New","new.example",1,{{"new.example",1}});
    live.deskIndex=1;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");

    ResetCountingRecordIdGenerator();
    ResetCountingReconcileMatcher();
    ReconcilePlan planned=PlanAppReconcile(
        existing,{live},"chrome",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator,CountingReconcileMatcher);
    CHECK(!planned.deferred);
    CHECK(planned.matches.empty() && planned.restores.empty());
    CHECK(planned.missingSavedIndices==std::vector<size_t>({oldIndex}));
    CHECK(planned.newRecords.size()==1 && planned.newRecords[0].liveIndex==0 &&
        planned.newRecords[0].recordId==newId);
    CHECK(CountingReconcileMatcherCalls()==1);
    CHECK(CountingRecordIdGeneratorCalls()==1);
    CHECK(SameLayoutWinVectors(existing,originalExisting));

    std::vector<LayoutWin> committed=CommitAppReconcile(
        existing,{live},planned,{},now);
    CHECK(committed.size()==MAX_LAYOUT_RECORDS);
    CHECK(!HasRecordId(committed,oldId));
    CHECK(HasRecordId(committed,newId));

    ReconcilePlan handcrafted=ValidCommitPlan("chrome",now);
    handcrafted.missingSavedIndices.push_back(oldIndex);
    handcrafted.newRecords.push_back(PlannedNewRecord(0,newId));
    std::vector<LayoutWin> handcraftedCommit=CommitAppReconcile(
        existing,{live},handcrafted,{},now);
    CHECK(SameLayoutWinVectors(handcraftedCommit,committed));
    CHECK(handcraftedCommit.size()==MAX_LAYOUT_RECORDS);
    CHECK(!HasRecordId(handcraftedCommit,oldId));
    CHECK(HasRecordId(handcraftedCommit,newId));
    ResetCountingReconcileMatcher();
    ResetCountingRecordIdGenerator();

    struct RetainedControl {
        UnixSeconds lastSeenUtc;
        UnixSeconds missingSinceUtc;
    };
    const RetainedControl controls[]={
        {now-WINDOW_RETENTION_SECONDS+1,0},
        {0,0},
        {now-WINDOW_RETENTION_SECONDS,now-WINDOW_RETENTION_SECONDS+1}
    };
    for(const RetainedControl& control : controls){
        std::vector<LayoutWin> retained=existing;
        retained[oldIndex].lastSeenUtc=control.lastSeenUtc;
        retained[oldIndex].missingSinceUtc=control.missingSinceUtc;
        ResetCountingRecordIdGenerator();
        ResetCountingReconcileMatcher();
        ReconcilePlan rejected=PlanAppReconcile(
            retained,{live},"chrome",now,{},ReconcileFreshness::Fresh,
            CountingRecordIdGenerator,CountingReconcileMatcher);
        CHECK(rejected.deferred);
        CHECK(rejected.matches.empty());
        CHECK(rejected.restores.empty());
        CHECK(rejected.newRecords.empty());
        CHECK(rejected.missingSavedIndices.empty());
        CHECK(CountingReconcileMatcherCalls()==1);
        CHECK(CountingRecordIdGeneratorCalls()==0);

        ReconcilePlan manualControl=ValidCommitPlan("chrome",now);
        manualControl.missingSavedIndices.push_back(oldIndex);
        manualControl.newRecords.push_back(PlannedNewRecord(0,newId));
        CHECK(SameLayoutWinVectors(
            CommitAppReconcile(retained,{live},manualControl,{},now),retained));
    }
    ResetCountingReconcileMatcher();
    ResetCountingRecordIdGenerator();
}

static void test_projected_retained_count_rejects_mismatched_flags(){
    const UnixSeconds now=2100000100;
    LayoutWin record=OldStyleRecord();
    record.lastSeenUtc=now-1;
    const std::vector<LayoutWin> existing={record};

    size_t retained=17;
    CHECK(!ProjectedRetainedExistingCount(
        existing,std::vector<bool>(),now,retained));
    CHECK(retained==17);

    retained=23;
    CHECK(!ProjectedRetainedExistingCount(
        existing,std::vector<bool>({false,true}),now,retained));
    CHECK(retained==23);
}

static void test_reconcile_duplicate_injected_match_ownership_defers_cleanly(){
    const UnixSeconds now=2000001690;
    LayoutWin savedA=ReconcileTestRecord(
        DeterministicRecordId(33000),"chrome","A","a.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin savedB=ReconcileTestRecord(
        DeterministicRecordId(33001),"chrome","B","b.example",1,
        G(L"{231A0000-0000-0000-0000-000000000002}"),now-10);
    LayoutWin liveA=MatchRecord("chrome","A","a.example",1,{{"a.example",1}});
    liveA.deskIndex=savedA.deskIndex;
    liveA.desktop=savedA.desktop;
    LayoutWin liveB=MatchRecord("chrome","B","b.example",1,{{"b.example",1}});
    liveB.deskIndex=savedB.deskIndex;
    liveB.desktop=savedB.desktop;
    const ReconcileMatcher invalidMatchers[]={
        DuplicateSavedReconcileMatcher,DuplicateLiveReconcileMatcher
    };

    for(ReconcileMatcher matcher : invalidMatchers){
        ResetCountingRecordIdGenerator();
        ReconcilePlan plan=PlanAppReconcile(
            {savedA,savedB},{liveA,liveB},"chrome",now,{},ReconcileFreshness::Fresh,
            CountingRecordIdGenerator,matcher);
        CHECK(plan.app=="chrome" && plan.nowUtc==now &&
            plan.freshness==ReconcileFreshness::Fresh);
        CHECK(plan.deferred);
        CHECK(plan.matches.empty());
        CHECK(plan.restores.empty());
        CHECK(plan.newRecords.empty());
        CHECK(plan.missingSavedIndices.empty());
        CHECK(CountingRecordIdGeneratorCalls()==0);
        ResetCountingRecordIdGenerator();
    }
}

static void test_reconcile_rejects_all_malformed_injected_matches(){
    const UnixSeconds now=2000001695;
    LayoutWin saved=ReconcileTestRecord(
        DeterministicRecordId(33002),"chrome","Saved","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin liveChrome=MatchRecord(
        "chrome","Live","same.example",1,{{"same.example",1}});
    liveChrome.deskIndex=saved.deskIndex;
    liveChrome.desktop=saved.desktop;
    LayoutWin liveFirefox=MatchRecord(
        "firefox","Other","other.example",1,{{"other.example",1}});
    liveFirefox.deskIndex=1;
    liveFirefox.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    const InjectedMatchMode invalidModes[]={
        InjectedMatchMode::SavedOutOfRange,InjectedMatchMode::LiveOutOfRange,
        InjectedMatchMode::OtherAppLive,InjectedMatchMode::NotANumber,
        InjectedMatchMode::Infinity,InjectedMatchMode::BelowThreshold,
        InjectedMatchMode::TooMany
    };

    for(InjectedMatchMode mode : invalidModes){
        CurrentInjectedMatchMode()=mode;
        ResetCountingRecordIdGenerator();
        ReconcilePlan plan=PlanAppReconcile(
            {saved},{liveChrome,liveFirefox},"chrome",now,{},
            ReconcileFreshness::Fresh,CountingRecordIdGenerator,
            ConfigurableReconcileMatcher);
        CHECK(plan.app=="chrome" && plan.nowUtc==now &&
            plan.freshness==ReconcileFreshness::Fresh);
        CHECK(plan.deferred);
        CHECK(plan.matches.empty());
        CHECK(plan.restores.empty());
        CHECK(plan.newRecords.empty());
        CHECK(plan.missingSavedIndices.empty());
        CHECK(CountingRecordIdGeneratorCalls()==0);
        ResetCountingRecordIdGenerator();
    }

    CurrentInjectedMatchMode()=InjectedMatchMode::Valid;
    ResetCountingRecordIdGenerator();
    ReconcilePlan valid=PlanAppReconcile(
        {saved},{liveChrome,liveFirefox},"chrome",now,{},
        ReconcileFreshness::Fresh,CountingRecordIdGenerator,
        ConfigurableReconcileMatcher);
    CHECK(!valid.deferred);
    CHECK(valid.matches.size()==1 && valid.matches[0].savedIndex==0 &&
        valid.matches[0].liveIndex==0 && valid.matches[0].score==0.55);
    CHECK(valid.restores.empty());
    CHECK(valid.newRecords.empty());
    CHECK(valid.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    std::vector<LayoutWin> committed=CommitAppReconcile(
        {saved},{liveChrome,liveFirefox},valid,{},now);
    CHECK(committed.size()==1);
    CHECK(committed[0].recordId==saved.recordId);
    CHECK(committed[0].activeTitle==liveChrome.activeTitle);
    CHECK(committed[0].lastSeenUtc==now && committed[0].missingSinceUtc==0);
    ResetCountingRecordIdGenerator();
}

static void test_reconcile_unsupported_app_defers_without_generation(){
    const UnixSeconds now=2000001700;
    LayoutWin existing=ReconcileTestRecord(
        DeterministicRecordId(10000),"firefox","Existing","existing.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin liveOpera=MatchRecord("opera","New","new.example",1,{{"new.example",1}});
    liveOpera.deskIndex=1;
    liveOpera.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");

    ResetCountingRecordIdGenerator();
    ReconcilePlan plan=PlanAppReconcile(
        {existing},{liveOpera},"opera",now,{},ReconcileFreshness::Fresh,
        CountingRecordIdGenerator);
    CHECK(plan.app=="opera" && plan.nowUtc==now &&
        plan.freshness==ReconcileFreshness::Fresh);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    ResetCountingRecordIdGenerator();
}

static void test_commit_reconcile_rejects_out_of_range_mixed_plan_atomically(){
    const UnixSeconds now=2000001800;
    LayoutWin saved=ReconcileTestRecord(
        DeterministicRecordId(10001),"chrome","Saved","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin live=MatchRecord("chrome","Live","same.example",1,{{"same.example",1}});
    live.deskIndex=saved.deskIndex;
    live.desktop=saved.desktop;
    const std::vector<LayoutWin> existing={saved};
    const std::vector<LayoutWin> liveRecords={live};

    for(int variant=0;variant<3;++variant){
        ReconcilePlan plan=ValidCommitPlan("chrome",now);
        plan.matches.push_back(Candidate(0,0,1.0));
        if(variant==0) plan.missingSavedIndices.push_back(existing.size());
        if(variant==1) plan.newRecords.push_back(
            PlannedNewRecord(liveRecords.size(),DeterministicRecordId(11000)));
        if(variant==2) plan.matches.push_back(Candidate(existing.size(),0,1.0));
        CHECK(SameLayoutWinVectors(
            CommitAppReconcile(existing,liveRecords,plan,{},now),existing));
    }
}

static void test_commit_reconcile_rejects_malformed_restore_sets_atomically(){
    const UnixSeconds now=2000001850;
    LayoutWin savedA=ReconcileTestRecord(
        DeterministicRecordId(10010),"chrome","A","a.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin savedB=ReconcileTestRecord(
        DeterministicRecordId(10011),"chrome","B","b.example",1,
        G(L"{231A0000-0000-0000-0000-000000000002}"),now-20);
    LayoutWin liveA=MatchRecord("chrome","A","a.example",1,{{"a.example",1}});
    liveA.deskIndex=2;
    liveA.desktop=G(L"{231A0000-0000-0000-0000-000000000003}");
    LayoutWin liveB=MatchRecord("chrome","B","b.example",1,{{"b.example",1}});
    liveB.deskIndex=3;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000004}");
    const std::vector<LayoutWin> existing={savedA,savedB};
    const std::vector<LayoutWin> live={liveA,liveB};

    ReconcilePlan baseline=ValidCommitPlan("chrome",now);
    baseline.matches.push_back(Candidate(0,0,1.0));
    baseline.restores.push_back(PlannedRestore(0,0,savedA.desktop));
    std::vector<LayoutWin> accepted=CommitAppReconcile(existing,live,baseline,{0},now);
    CHECK(accepted.size()==2 && accepted[0].lastSeenUtc==now);

    for(int variant=0;variant<7;++variant){
        ReconcilePlan malformed=baseline;
        std::set<size_t> successful;
        if(variant==0) malformed.restores[0].savedIndex=existing.size();
        if(variant==1) malformed.restores[0].liveIndex=live.size();
        if(variant==2)
            malformed.restores[0]=PlannedRestore(1,1,savedB.desktop);
        if(variant==3) malformed.restores[0].destination=savedB.desktop;
        if(variant==4) malformed.restores.push_back(malformed.restores[0]);
        if(variant==5) malformed.restores.clear();
        if(variant==6) successful.insert(1);
        CHECK(SameLayoutWinVectors(
            CommitAppReconcile(existing,live,malformed,successful,now),existing));
    }
    CHECK(SameLayoutWinVectors(existing,{savedA,savedB}));
    CHECK(SameLayoutWinVectors(live,{liveA,liveB}));
}

static void test_commit_reconcile_rejects_duplicate_match_ownership(){
    const UnixSeconds now=2000001900;
    LayoutWin first=ReconcileTestRecord(
        DeterministicRecordId(10002),"chrome","Same","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin second=first;
    second.recordId=DeterministicRecordId(10003);
    LayoutWin liveFirst=MatchRecord("chrome","Same","same.example",1,{{"same.example",1}});
    liveFirst.deskIndex=first.deskIndex;
    liveFirst.desktop=first.desktop;
    LayoutWin liveSecond=liveFirst;
    const std::vector<LayoutWin> existing={first,second};
    const std::vector<LayoutWin> live={liveFirst,liveSecond};

    ReconcilePlan duplicateSaved=ValidCommitPlan("chrome",now);
    duplicateSaved.matches={Candidate(0,0,1.0),Candidate(0,1,1.0)};
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existing,live,duplicateSaved,{},now),existing));

    ReconcilePlan duplicateLive=ValidCommitPlan("chrome",now);
    duplicateLive.matches={Candidate(0,0,1.0),Candidate(1,0,1.0)};
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existing,live,duplicateLive,{},now),existing));
}

static void test_commit_reconcile_rejects_app_mismatches(){
    const UnixSeconds now=2000002000;
    LayoutWin chromeSaved=ReconcileTestRecord(
        DeterministicRecordId(10004),"chrome","Saved","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin chromeLive=MatchRecord("chrome","Live","same.example",1,{{"same.example",1}});
    chromeLive.deskIndex=chromeSaved.deskIndex;
    chromeLive.desktop=chromeSaved.desktop;
    LayoutWin firefoxSaved=chromeSaved;
    firefoxSaved.recordId=DeterministicRecordId(10005);
    firefoxSaved.app="firefox";
    LayoutWin firefoxLive=chromeLive;
    firefoxLive.app="firefox";

    ReconcilePlan planAppMismatch=ValidCommitPlan("firefox",now);
    planAppMismatch.matches.push_back(Candidate(0,0,1.0));
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile({chromeSaved},{chromeLive},planAppMismatch,{},now),
        {chromeSaved}));

    ReconcilePlan liveAppMismatch=ValidCommitPlan("chrome",now);
    liveAppMismatch.matches.push_back(Candidate(0,0,1.0));
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile({chromeSaved},{firefoxLive},liveAppMismatch,{},now),
        {chromeSaved}));

    ReconcilePlan existingAppMismatch=ValidCommitPlan("chrome",now);
    existingAppMismatch.matches.push_back(Candidate(0,0,1.0));
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile({firefoxSaved},{chromeLive},existingAppMismatch,{},now),
        {firefoxSaved}));
}

static void test_commit_reconcile_rejects_invalid_new_record_requests(){
    const UnixSeconds now=2000002100;
    LayoutWin existing=ReconcileTestRecord(
        DeterministicRecordId(0xABCDEF),"firefox","Existing","existing.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin liveA=MatchRecord("chrome","A","a.example",1,{{"a.example",1}});
    liveA.deskIndex=1;
    liveA.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    LayoutWin liveB=MatchRecord("chrome","B","b.example",1,{{"b.example",1}});
    liveB.deskIndex=2;
    liveB.desktop=G(L"{231A0000-0000-0000-0000-000000000003}");
    const std::vector<LayoutWin> existingRecords={existing};

    const std::string invalidIds[]={"not-a-guid","{00000000-0000-0000-0000-000000000000}"};
    for(const std::string& invalidId : invalidIds){
        ReconcilePlan plan=ValidCommitPlan("chrome",now);
        plan.newRecords.push_back(PlannedNewRecord(0,invalidId));
        CHECK(SameLayoutWinVectors(
            CommitAppReconcile(existingRecords,{liveA},plan,{},now),existingRecords));
    }

    ReconcilePlan collision=ValidCommitPlan("chrome",now);
    collision.newRecords.push_back(
        PlannedNewRecord(0,existing.recordId.substr(1,existing.recordId.size()-2)));
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existingRecords,{liveA},collision,{},now),existingRecords));

    ReconcilePlan duplicateIds=ValidCommitPlan("chrome",now);
    duplicateIds.newRecords={
        PlannedNewRecord(0,DeterministicRecordId(12000)),
        PlannedNewRecord(1,DeterministicRecordId(12000))
    };
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existingRecords,{liveA,liveB},duplicateIds,{},now),
        existingRecords));

    ReconcilePlan duplicateLive=ValidCommitPlan("chrome",now);
    duplicateLive.newRecords={
        PlannedNewRecord(0,DeterministicRecordId(12001)),
        PlannedNewRecord(0,DeterministicRecordId(12002))
    };
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existingRecords,{liveA,liveB},duplicateLive,{},now),
        existingRecords));
}

static void test_commit_reconcile_rejects_cached_stale_actions(){
    const UnixSeconds now=2000002200;
    LayoutWin existing=ReconcileTestRecord(
        DeterministicRecordId(10006),"chrome","Existing","existing.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin live=MatchRecord("chrome","New","new.example",1,{{"new.example",1}});
    live.deskIndex=1;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    const std::vector<LayoutWin> existingRecords={existing};

    ReconcilePlan staleNew=ValidCommitPlan(
        "chrome",now,ReconcileFreshness::CachedStale);
    staleNew.newRecords.push_back(PlannedNewRecord(0,DeterministicRecordId(12003)));
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existingRecords,{live},staleNew,{},now),existingRecords));

    ReconcilePlan staleMissing=ValidCommitPlan(
        "chrome",now,ReconcileFreshness::CachedStale);
    staleMissing.missingSavedIndices.push_back(0);
    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existingRecords,{},staleMissing,{},now),existingRecords));
}

static void test_commit_reconcile_rejects_planning_clock_mismatch(){
    const UnixSeconds now=2000002300;
    LayoutWin saved=ReconcileTestRecord(
        DeterministicRecordId(10007),"chrome","Saved","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin live=MatchRecord("chrome","Live","same.example",1,{{"same.example",1}});
    live.deskIndex=saved.deskIndex;
    live.desktop=saved.desktop;
    ReconcilePlan plan=ValidCommitPlan("chrome",now-1);
    plan.matches.push_back(Candidate(0,0,1.0));

    CHECK(SameLayoutWinVectors(
        CommitAppReconcile({saved},{live},plan,{},now),{saved}));
}

static void test_reconcile_rejects_nonpositive_planning_clocks(){
    LayoutWin live=MatchRecord("chrome","Live","same.example",1,{{"same.example",1}});
    live.deskIndex=0;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    const UnixSeconds invalidTimes[]={0,-1};

    for(UnixSeconds invalidNow : invalidTimes){
        ResetCountingRecordIdGenerator();
        ReconcilePlan planned=PlanAppReconcile(
            {},{live},"chrome",invalidNow,{},ReconcileFreshness::Fresh,
            CountingRecordIdGenerator);
        CHECK(planned.app=="chrome" && planned.nowUtc==invalidNow &&
            planned.freshness==ReconcileFreshness::Fresh);
        CHECK(planned.deferred);
        CHECK(planned.matches.empty());
        CHECK(planned.restores.empty());
        CHECK(planned.newRecords.empty());
        CHECK(planned.missingSavedIndices.empty());
        CHECK(CountingRecordIdGeneratorCalls()==0);

        LayoutWin saved=ReconcileTestRecord(
            DeterministicRecordId(10008),"chrome","Saved","same.example",0,
            live.desktop,2000002300);
        ReconcilePlan manual=ValidCommitPlan("chrome",invalidNow);
        manual.matches.push_back(Candidate(0,0,1.0));
        CHECK(SameLayoutWinVectors(
            CommitAppReconcile({saved},{live},manual,{},invalidNow),{saved}));
        ResetCountingRecordIdGenerator();
    }
}

static void test_reconcile_rejects_invalid_freshness_before_planning(){
    const UnixSeconds now=2000002350;
    LayoutWin saved=ReconcileTestRecord(
        DeterministicRecordId(10009),"chrome","Saved","same.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    LayoutWin live=MatchRecord("chrome","Saved","same.example",1,{{"same.example",1}});
    live.deskIndex=1;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    const ReconcileFreshness invalid=static_cast<ReconcileFreshness>(99);

    ResetCountingRecordIdGenerator();
    ReconcilePlan plan=PlanAppReconcile(
        {saved},{live},"chrome",now,{},invalid,CountingRecordIdGenerator);

    CHECK(plan.app=="chrome" && plan.nowUtc==now && plan.freshness==invalid);
    CHECK(plan.deferred);
    CHECK(plan.matches.empty());
    CHECK(plan.restores.empty());
    CHECK(plan.newRecords.empty());
    CHECK(plan.missingSavedIndices.empty());
    CHECK(CountingRecordIdGeneratorCalls()==0);
    ResetCountingRecordIdGenerator();
}

static void test_commit_reconcile_rejects_projected_output_overflow(){
    const UnixSeconds now=2000002400;
    LayoutWin base=ReconcileTestRecord(
        DeterministicRecordId(13000),"chrome","Existing","existing.example",0,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    std::vector<LayoutWin> existing;
    existing.reserve(MAX_LAYOUT_RECORDS);
    for(size_t i=0;i<MAX_LAYOUT_RECORDS;++i){
        LayoutWin record=base;
        record.recordId=DeterministicRecordId(13000+i);
        existing.push_back(record);
    }
    LayoutWin live=MatchRecord("firefox","New","new.example",1,{{"new.example",1}});
    live.deskIndex=1;
    live.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    ReconcilePlan plan=ValidCommitPlan("firefox",now);
    plan.newRecords.push_back(
        PlannedNewRecord(0,DeterministicRecordId(13000+MAX_LAYOUT_RECORDS)));

    CHECK(SameLayoutWinVectors(
        CommitAppReconcile(existing,{live},plan,{},now),existing));
}

static void test_layout_legacy_migration_rejects_generated_id_collision_transactionally(){
    std::string data = "# VDE snapshot v3\n"
        "W\tfirefox\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("Inbox") +
        "\tmail.example\t1\tmail.example:1\t0\n"
        "W\tchrome\t1\t{231A0000-0000-0000-0000-000000000002}\t" + b64enc("Calendar") +
        "\tcalendar.example\t1\tcalendar.example:1\t0\n";
    DeskRec sentinelDesk{}; sentinelDesk.index=77; sentinelDesk.name=L"sentinel desk";
    LayoutWin sentinelWin; sentinelWin.app="sentinel"; sentinelWin.activeTitle="sentinel title";
    std::vector<DeskRec> desks={sentinelDesk}; std::vector<LayoutWin> wins={sentinelWin};
    std::string error;
    CHECK(!ParseLayout(data,desks,wins,1800000000,&error,nullptr,ConstantRecordIdGenerator));
    CHECK(!error.empty());
    CHECK(desks.size()==1 && desks[0].index==77 && desks[0].name==L"sentinel desk");
    CHECK(wins.size()==1 && wins[0].app=="sentinel" && wins[0].activeTitle=="sentinel title");
}

static void test_layout_legacy_migration_rejects_invalid_generated_ids_transactionally(){
    std::string data = "# VDE snapshot v2\n"
        "W\t0\t{231A0000-0000-0000-0000-000000000001}\t" + b64enc("Inbox") +
        "\tmail.example\t1\tmail.example:1\n";
    RecordIdGenerator generators[]={MalformedRecordIdGenerator,ZeroRecordIdGenerator};
    for(auto generator : generators){
        DeskRec sentinelDesk{}; sentinelDesk.index=77; sentinelDesk.name=L"sentinel desk";
        LayoutWin sentinelWin; sentinelWin.app="sentinel"; sentinelWin.activeTitle="sentinel title";
        std::vector<DeskRec> desks={sentinelDesk}; std::vector<LayoutWin> wins={sentinelWin};
        std::string error;
        CHECK(!ParseLayout(data,desks,wins,1800000000,&error,nullptr,generator));
        CHECK(!error.empty());
        CHECK(desks.size()==1 && desks[0].index==77 && desks[0].name==L"sentinel desk");
        CHECK(wins.size()==1 && wins[0].app=="sentinel" && wins[0].activeTitle=="sentinel title");
    }
}

static void test_layout_rejects_embedded_carriage_returns_transactionally(){
    const std::string guid="{231A0000-0000-0000-0000-000000000001}";
    const std::string id="{00000000-0000-0000-0000-000000000101}";
    const std::string title=b64enc("Inbox");
    const std::string invalid[]={
        "# VDE snapshot v4\nW\tfirefox\t"+id+"\t0\t"+guid+"\t"+title+"\tmail.example\rhidden\t1\tmail.example:1\t1700000000\t0\n",
        "# VDE snapshot v4\nW\tfirefox\t"+id+"\t0\t"+guid+"\t"+title+"\tmail.example\t1\tmail.example\rhidden:1\t1700000000\t0\n",
        "# VDE snapshot v3\nW\tfirefox\t0\t"+guid+"\t"+title+"\tmail.example\rhidden\t1\tmail.example:1\t0\n",
        "# VDE snapshot v3\nW\tfirefox\t0\t"+guid+"\t"+title+"\tmail.example\t1\tmail.example\rhidden:1\t0\n",
        "# VDE snapshot v2\nW\t0\t"+guid+"\t"+title+"\tmail.example\rhidden\t1\tmail.example:1\n",
        "# VDE snapshot v2\nW\t0\t"+guid+"\t"+title+"\tmail.example\t1\tmail.example\rhidden:1\n"
    };
    for(const auto& data : invalid){
        DeskRec sentinelDesk{}; sentinelDesk.index=77; sentinelDesk.name=L"sentinel desk";
        LayoutWin sentinelWin; sentinelWin.app="sentinel"; sentinelWin.activeTitle="sentinel title";
        std::vector<DeskRec> desks={sentinelDesk}; std::vector<LayoutWin> wins={sentinelWin};
        std::string error;
        CHECK(!ParseLayout(data,desks,wins,1800000000,&error,nullptr,ConstantRecordIdGenerator));
        CHECK(!error.empty());
        CHECK(desks.size()==1 && desks[0].index==77 && desks[0].name==L"sentinel desk");
        CHECK(wins.size()==1 && wins[0].app=="sentinel" && wins[0].activeTitle=="sentinel title");
    }
}

static void test_checked_snapshot_enforces_combined_record_cap(){
    DeskRec desk{}; desk.index=0; desk.guid=G(L"{231A0000-0000-0000-0000-000000000001}"); desk.name=L"Desk";
    std::vector<DeskRec> acceptedDesks(MAX_LAYOUT_RECORDS-1,desk);
    std::vector<LayoutWin> acceptedWins={StrictV4Record()};
    std::string output="sentinel", error="stale";
    CHECK(BuildCheckedLayoutSnapshot(acceptedDesks,acceptedWins,1700000000,output,&error));
    CHECK(error.empty()); CHECK(output.find("# VDE snapshot v4\n")==0);
    CHECK(acceptedWins[0].recordId=="{00000000-0000-0000-0000-000000000101}");
    CHECK(acceptedWins[0].lastSeenUtc==1700000000);

    std::vector<DeskRec> overflowDesks(MAX_LAYOUT_RECORDS,desk);
    std::vector<LayoutWin> overflowWins={OldStyleRecord()};
    output="prior snapshot bytes"; error.clear();
    CHECK(!BuildCheckedLayoutSnapshot(overflowDesks,overflowWins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(overflowWins.size()==1 && overflowWins[0].recordId.empty() && overflowWins[0].lastSeenUtc==0);
}

static void test_checked_snapshot_rejects_zero_desktop_record_transactionally(){
    DeskRec invalidDesk{}; invalidDesk.index=0; invalidDesk.name=L"Invalid";
    std::vector<DeskRec> desks={invalidDesk}; std::vector<LayoutWin> wins;
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot(desks,wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(wins.empty());
    CHECK(GuidIsZero(desks[0].guid) && desks[0].name==L"Invalid");
}

static void test_checked_snapshot_rejects_malformed_record_id_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].recordId="not-a-guid";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId=="not-a-guid" && wins[0].lastSeenUtc==1700000000);
}

static void test_checked_snapshot_rejects_empty_id_and_zero_last_seen(){
    std::vector<LayoutWin> wins={OldStyleRecord()};
    wins[0].lastSeenUtc=1700000000;
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty() && output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId.empty() &&
          wins[0].lastSeenUtc==1700000000);

    wins={StrictV4Record()};
    wins[0].lastSeenUtc=0;
    error.clear();
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty() && output=="prior snapshot bytes");
    CHECK(wins[0].recordId=="{00000000-0000-0000-0000-000000000101}" &&
          wins[0].lastSeenUtc==0);
}

static void test_checked_snapshot_rejects_zero_record_id_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].recordId="{00000000-0000-0000-0000-000000000000}";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId=="{00000000-0000-0000-0000-000000000000}" && wins[0].lastSeenUtc==1700000000);
}

static void test_checked_snapshot_rejects_duplicate_record_ids_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record(),StrictV4Record()};
    wins[0].recordId="{AAAAAAAA-BBBB-CCCC-DDDD-000000000001}";
    wins[1].recordId="aaaaaaaa-bbbb-cccc-dddd-000000000001";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==2 && wins[0].lastSeenUtc==1700000000 && wins[1].lastSeenUtc==1700000000);
    CHECK(wins[0].recordId.front()=='{' && wins[1].recordId.front()=='a');
}

static void test_checked_snapshot_rejects_negative_missing_since_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].missingSinceUtc=-1;
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes");
    CHECK(wins.size()==1 && wins[0].recordId=="{00000000-0000-0000-0000-000000000101}" &&
          wins[0].lastSeenUtc==1700000000 && wins[0].missingSinceUtc==-1);
}

static void test_checked_snapshot_accepts_supported_browser_apps(){
    const char* apps[]={"firefox","chrome","msedge"};
    for(const char* app : apps){
        std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].app=app;
        std::string output="sentinel", error="stale";
        CHECK(BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
        CHECK(error.empty()); CHECK(output.find(std::string("W\t")+app+"\t")!=std::string::npos);
    }
}

static void test_checked_snapshot_rejects_unsupported_app_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].app="opera";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());
}

static void test_checked_snapshot_rejects_negative_tab_count_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].tabCount=-1;
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());
}

static void test_checked_snapshot_rejects_invalid_counts_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].counts={{"mail.example",0}};
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());

    wins={StrictV4Record()}; wins[0].counts={{"mail.example,evil",1}}; error.clear();
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());

    wins={StrictV4Record()}; wins[0].counts={{"mail.example\tinjected",1}}; error.clear();
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());
}

static void test_checked_snapshot_rejects_raw_domain_line_breaks_transactionally(){
    std::vector<LayoutWin> wins={StrictV4Record()}; wins[0].activeDomain="mail.example\tinjected";
    std::string output="prior snapshot bytes", error;
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());

    wins={StrictV4Record()}; wins[0].activeDomain="mail.example\ninjected"; error.clear();
    CHECK(!BuildCheckedLayoutSnapshot({},wins,1700000000,output,&error));
    CHECK(!error.empty()); CHECK(output=="prior snapshot bytes"); CHECK(!wins[0].recordId.empty());
}

static void test_lc_initial_absence_marks_missing_once(){
    LcState s;
    LcDecision first=LcObserve(s,false,0,0,0,0,100);
    CHECK(first.action==LcAction::MarkMissingFromLastSeen && first.generation!=0);
    CHECK(s.nextGeneration!=0 && s.nextGeneration!=first.generation);
    CHECK(LcObserve(s,false,0,0,0,0,101).action==LcAction::None);
}
static void test_lc_two_stable_present_snapshots_begin_restore(){
    LcState s;
    CHECK(LcObserve(s,true,1,11,21,31,100).action==LcAction::None);
    LcDecision begin=LcObserve(s,true,1,11,21,31,101);
    CHECK(begin.action==LcAction::BeginRestore && begin.generation!=0);
    CHECK(s.restoreInFlight && s.inFlightGeneration==begin.generation);
}
static void test_lc_stale_restore_completion_is_ignored(){
    LcState s;
    LcObserve(s,true,1,11,21,31,100);
    LcDecision begin=LcObserve(s,true,1,11,21,31,101);
    LcRestoreCompleted(s,begin.generation+1,LcRestoreOutcome::Success,22,32,102);
    CHECK(s.restoreInFlight && s.inFlightGeneration==begin.generation);
    CHECK(s.layoutSignature==21 && s.sessionStampSignature==31);
}
static void test_restore_budget_is_exact_keyed(){
    RestoreBudgets budgets;
    RestoreBudgetKey failed{"record","runtime-a","desktop-a"};
    CHECK(budgets.mayAttempt(failed));
    budgets.markExhausted(failed);
    CHECK(!budgets.mayAttempt(failed));
    CHECK(budgets.mayAttempt(RestoreBudgetKey{"record","runtime-a","desktop-b"}));
}

static void test_lc_timeout_is_per_wave_and_survives_clock_rollback(){
    LcState s;
    CHECK(LcObserve(s,true,1,10,20,30,1000).action==LcAction::None);
    CHECK(LcObserve(s,true,1,11,20,30,500).action==LcAction::None);
    CHECK(LcObserve(s,true,1,12,20,30,20499).action==LcAction::None);
    LcDecision first=LcObserve(s,true,1,13,20,30,20500);
    CHECK(first.action==LcAction::BeginRestore && first.generation!=0);
    LcRestoreCompleted(s,first.generation,LcRestoreOutcome::Success,20,30,21000);

    CHECK(LcObserve(s,true,2,20,20,30,30000).action==LcAction::None);
    CHECK(LcObserve(s,true,2,21,20,30,49999).action==LcAction::None);
    LcDecision second=LcObserve(s,true,2,22,20,30,50000);
    CHECK(second.action==LcAction::BeginRestore && second.generation!=first.generation);
}

static void test_lc_absence_transitions_mark_once_and_reappearance_rearms(){
    LcState s;
    LcDecision initialMissing=LcObserve(s,false,0,0,0,0,0);
    CHECK(initialMissing.action==LcAction::MarkMissingFromLastSeen &&
          initialMissing.generation!=0);
    CHECK(LcObserve(s,false,0,0,0,0,1).action==LcAction::None);
    CHECK(LcObserve(s,true,1,1,1,1,2).action==LcAction::None);
    LcDecision first=LcObserve(s,true,1,1,1,1,3);
    CHECK(first.action==LcAction::BeginRestore);
    LcRestoreCompleted(s,first.generation,LcRestoreOutcome::Success,1,1,4);
    LcDecision transitionMissing=LcObserve(s,false,0,0,1,1,5);
    CHECK(transitionMissing.action==LcAction::MarkMissingFromLastSeen &&
          transitionMissing.generation!=0 &&
          transitionMissing.generation!=initialMissing.generation);
    CHECK(LcObserve(s,false,0,0,1,1,6).action==LcAction::None);
    CHECK(LcObserve(s,true,1,1,1,1,7).action==LcAction::None);
    CHECK(LcObserve(s,true,1,1,1,1,8).action==LcAction::BeginRestore);
}

static void test_lc_exhausted_generation_suppresses_missing_action(){
    LcState initialAbsent;
    initialAbsent.nextGeneration=0;
    LcDecision first=LcObserve(initialAbsent,false,0,0,0,0,0);
    CHECK(first.action==LcAction::None && first.generation==0);
    CHECK(initialAbsent.initialized && !initialAbsent.present &&
          initialAbsent.nextGeneration==0);

    LcState active;
    active.nextGeneration=UINT64_MAX;
    LcObserve(active,true,1,10,100,20,0);
    LcDecision last=LcObserve(active,true,1,10,100,20,1);
    CHECK(last.action==LcAction::BeginRestore && last.generation==UINT64_MAX &&
          active.nextGeneration==0);
    LcDecision suppressed=LcObserve(active,false,0,0,100,20,2);
    CHECK(suppressed.action==LcAction::None && suppressed.generation==0);
    CHECK(!active.present && active.restoreInFlight &&
          active.inFlightGeneration==UINT64_MAX && active.rearmAfterFlight);
}

static void test_lc_absence_during_flight_clears_rearm_if_still_absent(){
    LcState s;
    LcObserve(s,true,1,10,100,20,0);
    LcDecision active=LcObserve(s,true,1,10,100,20,1);
    LcDecision missing=LcObserve(s,false,0,0,100,20,2);
    CHECK(missing.action==LcAction::MarkMissingFromLastSeen &&
          missing.generation!=0 && s.rearmAfterFlight);
    CHECK(s.restoreInFlight && s.inFlightGeneration==active.generation);
    LcRestoreCompleted(s,active.generation,LcRestoreOutcome::Success,100,20,3);
    CHECK(!s.present && !s.restoreInFlight && !s.restorePending &&
          !s.rearmAfterFlight);
    CHECK(LcObserve(s,false,0,0,100,20,4).action==LcAction::None);
}

static void test_lc_absence_reappearance_during_flight_queues_one_wave(){
    LcState s;
    LcObserve(s,true,1,10,100,20,0);
    LcDecision active=LcObserve(s,true,1,10,100,20,1);
    LcDecision missing=LcObserve(s,false,0,0,100,20,2);
    CHECK(missing.action==LcAction::MarkMissingFromLastSeen &&
          missing.generation!=0 && s.rearmAfterFlight);
    CHECK(LcObserve(s,true,1,11,100,20,3).action==LcAction::None);
    CHECK(s.restoreInFlight && s.inFlightGeneration==active.generation &&
          s.rearmAfterFlight);
    LcRestoreCompleted(s,active.generation,LcRestoreOutcome::Success,100,20,4);
    CHECK(s.present && !s.restoreInFlight && s.restorePending &&
          !s.rearmAfterFlight);
    CHECK(LcObserve(s,true,1,11,100,20,5).action==LcAction::None);
    LcDecision rearmed=LcObserve(s,true,1,11,100,20,6);
    CHECK(rearmed.action==LcAction::BeginRestore &&
          rearmed.generation!=active.generation);
}

static void test_lc_firefox_chrome_edge_states_are_independent(){
    LcState firefox, chrome, edge;
    CHECK(LcObserve(firefox,true,10,10,10,10,0).action==LcAction::None);
    CHECK(LcObserve(chrome,true,20,20,20,20,0).action==LcAction::None);
    CHECK(LcObserve(edge,false,0,0,30,30,0).action==LcAction::MarkMissingFromLastSeen);
    LcDecision ff=LcObserve(firefox,true,10,10,10,10,1);
    LcDecision ch=LcObserve(chrome,true,20,20,20,20,1);
    CHECK(ff.action==LcAction::BeginRestore && ch.action==LcAction::BeginRestore);
    LcRestoreCompleted(firefox,ff.generation,LcRestoreOutcome::Success,10,10,2);
    LcRestoreCompleted(chrome,ch.generation,LcRestoreOutcome::Success,20,20,2);
    CHECK(LcObserve(edge,true,30,30,30,30,3).action==LcAction::None);
    CHECK(LcObserve(firefox,true,10,10,10,10,3).action==LcAction::None);
    CHECK(LcObserve(chrome,true,20,20,20,20,3).action==LcAction::None);
    CHECK(LcObserve(edge,true,30,30,30,30,4).action==LcAction::BeginRestore);
}

static void test_lc_layout_change_saves_but_restore_inputs_restore_first(){
    LcState s;
    LcObserve(s,true,1,1,10,1,0);
    LcDecision initial=LcObserve(s,true,1,1,10,1,1);
    LcRestoreCompleted(s,initial.generation,LcRestoreOutcome::Success,10,1,2);
    LcDecision save=LcObserve(s,true,1,1,11,1,3);
    CHECK(save.action==LcAction::SaveLayout && save.generation!=0);
    CHECK(s.saveInFlight && !s.restorePending && !s.restoreInFlight);

    LcState restoreFirst;
    LcObserve(restoreFirst,true,1,1,10,1,0);
    LcDecision done=LcObserve(restoreFirst,true,1,1,10,1,1);
    LcRestoreCompleted(restoreFirst,done.generation,LcRestoreOutcome::Success,10,1,2);
    CHECK(LcObserve(restoreFirst,true,1,2,11,2,3).action==LcAction::None);
    CHECK(LcObserve(restoreFirst,true,1,2,11,2,4).action==LcAction::BeginRestore);
}

static void test_lc_same_hwnd_new_fresh_session_starts_one_wave(){
    LcState s;
    LcObserve(s,true,7,70,700,1,0);
    LcDecision initial=LcObserve(s,true,7,70,700,1,1);
    LcRestoreCompleted(s,initial.generation,LcRestoreOutcome::Success,700,1,2);
    CHECK(LcObserve(s,true,7,71,700,2,3).action==LcAction::None);
    LcDecision refresh=LcObserve(s,true,7,71,700,2,4);
    CHECK(refresh.action==LcAction::BeginRestore && refresh.generation!=initial.generation);
    LcRestoreCompleted(s,refresh.generation,LcRestoreOutcome::Success,700,2,5);
    CHECK(LcObserve(s,true,7,71,700,2,6).action==LcAction::None);
    CHECK(LcObserve(s,true,7,71,700,2,7).action==LcAction::None);
}

static void test_lc_inflight_changes_queue_exactly_one_latest_rearm(){
    LcState s;
    LcObserve(s,true,1,10,100,1,0);
    LcDecision first=LcObserve(s,true,1,10,100,1,1);
    CHECK(LcObserve(s,true,2,20,101,2,2).action==LcAction::None);
    CHECK(LcObserve(s,true,3,30,102,3,3).action==LcAction::None);
    CHECK(s.restoreInFlight && s.inFlightGeneration==first.generation && s.rearmAfterFlight);
    LcRestoreCompleted(s,first.generation,LcRestoreOutcome::Success,101,1,4);
    CHECK(!s.restoreInFlight && s.restorePending && !s.rearmAfterFlight);
    CHECK(s.windowSetSignature==3 && s.settleSignature==30 &&
          s.layoutSignature==102 && s.sessionStampSignature==3);
    CHECK(LcObserve(s,true,3,30,102,3,5).action==LcAction::None);
    LcDecision latest=LcObserve(s,true,3,30,102,3,6);
    CHECK(latest.action==LcAction::BeginRestore && latest.generation!=first.generation);
    LcRestoreCompleted(s,latest.generation,LcRestoreOutcome::Success,102,3,7);
    CHECK(LcObserve(s,true,3,30,102,3,8).action==LcAction::None);
}

static void test_lc_late_and_returning_sibling_each_start_one_wave(){
    LcState s;
    LcObserve(s,true,1,10,100,1,0);
    LcDecision onlyA=LcObserve(s,true,1,10,100,1,1);
    LcRestoreCompleted(s,onlyA.generation,LcRestoreOutcome::Success,100,1,2);

    CHECK(LcObserve(s,true,3,30,100,1,3).action==LcAction::None); // late B
    LcDecision withB=LcObserve(s,true,3,30,100,1,4);
    CHECK(withB.action==LcAction::BeginRestore);
    LcRestoreCompleted(s,withB.generation,LcRestoreOutcome::Success,100,1,5);
    CHECK(LcObserve(s,true,1,10,100,1,6).action==LcAction::None); // B disappears
    LcDecision withoutB=LcObserve(s,true,1,10,100,1,7);
    CHECK(withoutB.action==LcAction::BeginRestore);
    LcRestoreCompleted(s,withoutB.generation,LcRestoreOutcome::Success,100,1,8);
    CHECK(LcObserve(s,true,3,30,100,1,9).action==LcAction::None); // B returns
    LcDecision returnedB=LcObserve(s,true,3,30,100,1,10);
    CHECK(returnedB.action==LcAction::BeginRestore);
    LcRestoreCompleted(s,returnedB.generation,LcRestoreOutcome::Success,100,1,11);
    CHECK(LcObserve(s,true,3,30,100,1,12).action==LcAction::None);
}

static void test_lc_generation_max_is_issued_once_then_fails_closed(){
    LcState s;
    s.nextGeneration=UINT64_MAX;
    LcObserve(s,true,1,1,1,1,0);
    LcDecision last=LcObserve(s,true,1,1,1,1,1);
    CHECK(last.action==LcAction::BeginRestore && last.generation==UINT64_MAX);
    CHECK(s.nextGeneration==0);
    LcRestoreCompleted(s,last.generation,LcRestoreOutcome::Success,1,1,2);
    LcObserve(s,true,1,2,1,2,3);
    LcDecision exhausted=LcObserve(s,true,1,2,1,2,4);
    CHECK(exhausted.action==LcAction::None && exhausted.generation==0);
    CHECK(!s.restoreInFlight && s.inFlightGeneration==0 &&
          !s.restorePending && s.nextGeneration==0);
    const uint64_t completed=s.completedLayoutSignature;
    LcRestoreCompleted(s,UINT64_MAX,LcRestoreOutcome::Exhausted,999,999,5);
    CHECK(!s.restoreInFlight && s.inFlightGeneration==0 &&
          s.completedLayoutSignature==completed);
    CHECK(LcObserve(s,true,1,2,1,2,6).action==LcAction::None);
}

static LcDecision lc_begin_initial(LcState& s, uint64_t windowSet,
                                   uint64_t settle, uint64_t layout,
                                   uint64_t session, uint64_t now){
    CHECK(LcObserve(s,true,windowSet,settle,layout,session,now).action==LcAction::None);
    LcDecision begin=LcObserve(s,true,windowSet,settle,layout,session,now+1);
    CHECK(begin.action==LcAction::BeginRestore && begin.generation!=0);
    return begin;
}

static void test_lc_deferred_retries_three_times_with_exact_backoff(){
    LcState s;
    LcDecision wave=lc_begin_initial(s,1,10,100,20,0);
    uint64_t completedAt=100;
    for(int attempt=1;attempt<=3;++attempt){
        LcRestoreCompleted(s,wave.generation,LcRestoreOutcome::Deferred,
                           100,20,completedAt);
        CHECK(s.deferredAttempts==attempt);
        if(attempt==3){
            CHECK(s.deferredUntilInputChanges && !s.restorePending &&
                  !s.restoreInFlight && s.retryNotBeforeMs==0);
            break;
        }
        const uint64_t readyAt=completedAt+30000;
        CHECK(s.restorePending && !s.deferredUntilInputChanges &&
              s.retryNotBeforeMs==readyAt);
        CHECK(LcObserve(s,true,1,10,100,20,completedAt+1).action==LcAction::None);
        CHECK(LcObserve(s,true,1,10,100,20,readyAt-1).action==LcAction::None);
        wave=LcObserve(s,true,1,10,100,20,readyAt);
        CHECK(wave.action==LcAction::BeginRestore && wave.generation!=0);
        completedAt=readyAt+100;
    }
    CHECK(LcObserve(s,true,1,10,101,20,1000000).action==LcAction::None);
    CHECK(s.deferredUntilInputChanges && s.deferredAttempts==3);
}

static void test_lc_deferred_key_change_resets_for_window_or_session(){
    for(int changeSession=0;changeSession<2;++changeSession){
        LcState s;
        LcDecision wave=lc_begin_initial(s,1,10,100,20,0);
        uint64_t completedAt=100;
        for(int attempt=1;attempt<=3;++attempt){
            LcRestoreCompleted(s,wave.generation,LcRestoreOutcome::Deferred,
                               100,20,completedAt);
            if(attempt<3){
                const uint64_t readyAt=completedAt+30000;
                LcObserve(s,true,1,10,100,20,completedAt+1);
                wave=LcObserve(s,true,1,10,100,20,readyAt);
                CHECK(wave.action==LcAction::BeginRestore);
                completedAt=readyAt+100;
            }
        }
        const uint64_t changedWindow=changeSession ? 1 : 2;
        const uint64_t changedSession=changeSession ? 21 : 20;
        CHECK(LcObserve(s,true,changedWindow,11,100,changedSession,
                        completedAt+1).action==LcAction::None);
        CHECK(s.restorePending && !s.deferredUntilInputChanges &&
              s.deferredAttempts==0 && s.retryNotBeforeMs==0);
        LcDecision reset=LcObserve(s,true,changedWindow,11,100,changedSession,
                                   completedAt+2);
        CHECK(reset.action==LcAction::BeginRestore);
    }
}

static void test_lc_deferred_key_change_during_backoff_restarts_settle_now(){
    for(int changeSession=0;changeSession<2;++changeSession){
        LcState s;
        LcDecision first=lc_begin_initial(s,1,10,100,20,0);
        LcRestoreCompleted(s,first.generation,LcRestoreOutcome::Deferred,
                           100,20,100);
        CHECK(s.deferredAttempts==1 && s.retryNotBeforeMs==30100);
        const uint64_t windowSet=changeSession ? 1 : 2;
        const uint64_t session=changeSession ? 21 : 20;
        CHECK(LcObserve(s,true,windowSet,11,100,session,1000).action==LcAction::None);
        CHECK(s.restorePending && s.deferredAttempts==0 &&
              s.retryNotBeforeMs==0 && s.stableSnapshots==1);
        LcDecision reset=LcObserve(s,true,windowSet,11,100,session,1001);
        CHECK(reset.action==LcAction::BeginRestore && reset.generation!=first.generation);
    }
}

static void test_lc_inflight_a_to_b_to_a_history_rearms_deferred_wave(){
    LcState s;
    LcDecision first=lc_begin_initial(s,1,10,100,20,0);
    CHECK(LcObserve(s,true,2,11,100,21,2).action==LcAction::None);
    CHECK(LcObserve(s,true,1,12,100,20,3).action==LcAction::None);
    CHECK(s.restoreInFlight && s.inFlightGeneration==first.generation &&
          s.rearmAfterFlight && s.windowSetSignature==1 &&
          s.sessionStampSignature==20);
    LcRestoreCompleted(s,first.generation,LcRestoreOutcome::Deferred,
                       100,20,100);
    CHECK(!s.restoreInFlight && s.restorePending && !s.rearmAfterFlight);
    CHECK(s.deferredAttempts==0 && !s.deferredUntilInputChanges &&
          s.retryNotBeforeMs==0);
    CHECK(LcObserve(s,true,1,12,100,20,101).action==LcAction::None);
    LcDecision rearmed=LcObserve(s,true,1,12,100,20,102);
    CHECK(rearmed.action==LcAction::BeginRestore &&
          rearmed.generation!=first.generation);
}

static void test_lc_deferred_backoff_rebases_after_clock_rollback(){
    LcState s;
    LcDecision first=lc_begin_initial(s,1,10,100,20,10000);
    LcRestoreCompleted(s,first.generation,LcRestoreOutcome::Deferred,
                       100,20,10100);
    CHECK(s.retryNotBeforeMs==40100);
    CHECK(LcObserve(s,true,1,10,100,20,5000).action==LcAction::None);
    CHECK(s.retryNotBeforeMs==35000);
    CHECK(LcObserve(s,true,1,10,100,20,34999).action==LcAction::None);
    CHECK(LcObserve(s,true,1,10,100,20,35000).action==LcAction::BeginRestore);
}

static void test_lc_deferred_backoff_distinguishes_exact_max_from_overflow(){
    const uint64_t exactStart=UINT64_MAX-30000;
    LcState exact;
    LcDecision exactWave=lc_begin_initial(exact,1,10,100,20,0);
    LcRestoreCompleted(exact,exactWave.generation,LcRestoreOutcome::Deferred,
                       100,20,exactStart);
    CHECK(exact.retryNotBeforeMs==UINT64_MAX &&
          !exact.deferredUntilInputChanges);
    CHECK(LcObserve(exact,true,1,10,100,20,UINT64_MAX-1).action==LcAction::None);
    CHECK(LcObserve(exact,true,1,10,100,20,UINT64_MAX).action==LcAction::BeginRestore);

    const uint64_t overflowStart=exactStart+1;
    LcState overflow;
    LcDecision overflowWave=lc_begin_initial(overflow,1,10,100,20,0);
    LcRestoreCompleted(overflow,overflowWave.generation,LcRestoreOutcome::Deferred,
                       100,20,overflowStart);
    CHECK(LcObserve(overflow,true,1,10,100,20,UINT64_MAX).action==LcAction::None);

    LcState released;
    LcDecision releasedWave=lc_begin_initial(released,1,10,100,20,0);
    LcRestoreCompleted(released,releasedWave.generation,LcRestoreOutcome::Deferred,
                       100,20,overflowStart);
    CHECK(LcObserve(released,true,2,11,100,20,UINT64_MAX).action==LcAction::None);
    CHECK(LcObserve(released,true,2,11,100,20,UINT64_MAX).action==LcAction::BeginRestore);
}

static void test_lc_deferred_rollback_to_unrepresentable_deadline_fails_closed(){
    const uint64_t exactStart=UINT64_MAX-30000;
    const uint64_t rolledBackOrigin=exactStart+1;
    LcState s;
    LcDecision wave=lc_begin_initial(s,1,10,100,20,0);
    LcRestoreCompleted(s,wave.generation,LcRestoreOutcome::Deferred,
                       100,20,exactStart);
    CHECK(LcObserve(s,true,1,10,100,20,UINT64_MAX-1).action==LcAction::None);
    CHECK(LcObserve(s,true,1,10,100,20,rolledBackOrigin).action==LcAction::None);
    CHECK(LcObserve(s,true,1,10,100,20,UINT64_MAX).action==LcAction::None);
    CHECK(LcObserve(s,true,2,11,100,20,UINT64_MAX).action==LcAction::None);
    CHECK(LcObserve(s,true,2,11,100,20,UINT64_MAX).action==LcAction::BeginRestore);
}

static void test_lc_deferred_budget_resets_on_new_source_stamp(){
    LcState s;
    LcDecision wave=lc_begin_initial(s,1,10,100,20,0);
    uint64_t completedAt=100;
    const uint64_t sources[4]={99,100,100,100};
    const int expectedAttempts[4]={1,1,2,3};
    for(int attempt=0;attempt<4;++attempt){
        LcRestoreCompleted(s,wave.generation,LcRestoreOutcome::Deferred,
                           100,sources[attempt],completedAt);
        CHECK(s.sessionStampSignature==20 &&
              s.deferredAttempts==expectedAttempts[attempt]);
        CHECK(s.deferredWindowSetSignature==1 &&
              s.deferredSessionStampSignature==sources[attempt]);
        if(attempt==3) break;
        const uint64_t readyAt=completedAt+30000;
        LcObserve(s,true,1,10,100,20,completedAt+1);
        wave=LcObserve(s,true,1,10,100,20,readyAt);
        CHECK(wave.action==LcAction::BeginRestore);
        completedAt=readyAt+100;
    }
    CHECK(s.deferredUntilInputChanges && !s.restorePending &&
          !s.restoreInFlight && s.retryNotBeforeMs==0);
    CHECK(LcObserve(s,true,1,10,100,20,1000000).action==LcAction::None);
}

static void test_lc_cancelled_stale_reconcile_retires_exact_flight_and_rearms(){
    LcState state;
    LcDecision wave=lc_begin_initial(state,1,10,100,20,0);
    CHECK(!LcCancelRestore(state,wave.generation+1,2,true));
    CHECK(state.restoreInFlight && state.inFlightGeneration==wave.generation);
    CHECK(LcCancelRestore(state,wave.generation,3,true));
    CHECK(!state.restoreInFlight && state.inFlightGeneration==0);
    CHECK(state.restorePending && state.stableSnapshots==0);
    CHECK(LcObserve(state,true,1,10,100,20,4).action==LcAction::None);
    LcDecision rearmed=LcObserve(state,true,1,10,100,20,5);
    CHECK(rearmed.action==LcAction::BeginRestore &&
          rearmed.generation!=wave.generation);

    CHECK(LcCancelRestore(state,rearmed.generation,6,false));
    CHECK(!state.restoreInFlight && !state.restorePending);
}

static void test_lc_all_completion_outcomes_honor_one_queued_rearm(){
    const LcRestoreOutcome outcomes[]={
        LcRestoreOutcome::Success,
        LcRestoreOutcome::Deferred,
        LcRestoreOutcome::Exhausted
    };
    for(LcRestoreOutcome outcome:outcomes){
        LcState s;
        LcDecision first=lc_begin_initial(s,1,10,100,20,0);
        CHECK(LcObserve(s,true,2,11,101,21,2).action==LcAction::None);
        CHECK(LcObserve(s,true,3,12,102,22,3).action==LcAction::None);
        LcRestoreCompleted(s,first.generation,outcome,101,20,4);
        CHECK(!s.restoreInFlight && s.restorePending && !s.rearmAfterFlight);
        CHECK(s.windowSetSignature==3 && s.sessionStampSignature==22);
        CHECK(s.deferredAttempts==0 && !s.deferredUntilInputChanges &&
              s.retryNotBeforeMs==0);
        CHECK(LcObserve(s,true,3,12,102,22,5).action==LcAction::None);
        LcDecision latest=LcObserve(s,true,3,12,102,22,6);
        CHECK(latest.action==LcAction::BeginRestore && latest.generation!=first.generation);
    }
}

static void test_lc_exhausted_records_actual_layout_without_save_loop(){
    LcState s;
    LcDecision wave=lc_begin_initial(s,1,10,100,20,0);
    CHECK(LcObserve(s,true,1,10,199,20,2).action==LcAction::None);
    LcRestoreCompleted(s,wave.generation,LcRestoreOutcome::Exhausted,199,20,3);
    CHECK(!s.restorePending && !s.restoreInFlight &&
          s.completedLayoutSignature==199);
    CHECK(LcObserve(s,true,1,10,199,20,4).action==LcAction::None);
    CHECK(LcObserve(s,true,1,10,199,20,5).action==LcAction::None);
}

static void test_lc_explicit_save_completion_is_generation_safe(){
    LcState s;
    LcDecision restore=lc_begin_initial(s,1,10,100,20,0);
    LcRestoreCompleted(s,restore.generation,LcRestoreOutcome::Success,100,20,2);
    LcDecision save=LcObserve(s,true,1,10,101,20,3);
    CHECK(save.action==LcAction::SaveLayout && s.saveInFlight);

    LcExplicitSaveCompleted(s,save.generation+1,101,20,4);
    CHECK(s.saveInFlight && s.saveGeneration==save.generation &&
          s.completedLayoutSignature==100);
    s.deferredAttempts=2;
    s.deferredUntilInputChanges=true;
    s.deferredWindowSetSignature=1;
    s.deferredSessionStampSignature=20;
    s.retryNotBeforeMs=99999;
    LcExplicitSaveCompleted(s,save.generation,101,20,5);
    CHECK(!s.saveInFlight && s.saveGeneration==0 &&
          s.completedLayoutSignature==101);
    CHECK(s.deferredAttempts==0 && !s.deferredUntilInputChanges &&
          s.retryNotBeforeMs==0);
    CHECK(!s.restoreInFlight && s.inFlightGeneration==0);
    CHECK(LcObserve(s,true,1,10,101,20,6).action==LcAction::None);

    LcDecision newer=LcObserve(s,true,1,10,102,20,7);
    CHECK(newer.action==LcAction::SaveLayout && newer.generation!=save.generation);
    LcExplicitSaveCompleted(s,save.generation,102,20,8);
    CHECK(s.saveInFlight && s.saveGeneration==newer.generation);
    LcExplicitSaveCompleted(s,newer.generation,102,20,9);
    CHECK(!s.saveInFlight && s.completedLayoutSignature==102);
}

static void test_lc_explicit_save_completion_commits_captured_layout_only(){
    LcState s;
    LcDecision restore=lc_begin_initial(s,1,10,100,20,0);
    LcRestoreCompleted(s,restore.generation,LcRestoreOutcome::Success,100,20,2);
    LcDecision save101=LcObserve(s,true,1,10,101,20,3);
    CHECK(save101.action==LcAction::SaveLayout &&
          s.saveRequestedLayoutSignature==101);
    CHECK(LcObserve(s,true,1,10,102,20,4).action==LcAction::None);
    LcExplicitSaveCompleted(s,save101.generation,102,20,5);
    LcDecision save102=LcObserve(s,true,1,10,102,20,6);
    CHECK(save102.action==LcAction::SaveLayout &&
          save102.generation!=save101.generation);
}

static void test_lc_explicit_save_completion_rebases_pending_wave_on_rollback(){
    LcState s;
    LcDecision restore=lc_begin_initial(s,1,10,100,20,0);
    LcRestoreCompleted(s,restore.generation,LcRestoreOutcome::Success,100,20,2);
    LcDecision save=LcObserve(s,true,1,10,101,20,1000);
    CHECK(save.action==LcAction::SaveLayout);
    CHECK(LcObserve(s,true,2,20,101,21,100000).action==LcAction::None);
    CHECK(s.saveInFlight && s.restorePending);
    LcExplicitSaveCompleted(s,save.generation,101,21,0);
    CHECK(LcObserve(s,true,2,21,101,21,19999).action==LcAction::None);
    LcDecision timeout=LcObserve(s,true,2,22,101,21,20000);
    CHECK(timeout.action==LcAction::BeginRestore && timeout.generation!=0);
}

static void test_restore_budgets_isolate_siblings_runtime_and_destination(){
    RestoreBudgets budgets;
    const RestoreBudgetKey failedA{"record-a","runtime-a","desktop-a"};
    const RestoreBudgetKey siblingB{"record-b","runtime-b","desktop-a"};
    const RestoreBudgetKey changedRuntime{"record-a","runtime-a-new","desktop-a"};
    const RestoreBudgetKey changedDestination{"record-a","runtime-a","desktop-b"};
    budgets.markExhausted(failedA);
    CHECK(!budgets.mayAttempt(failedA));
    CHECK(budgets.mayAttempt(siblingB));
    CHECK(budgets.mayAttempt(changedRuntime));
    CHECK(budgets.mayAttempt(changedDestination));

    budgets.markExhausted(siblingB);
    budgets.clearExact(siblingB); // verified success clears only B
    CHECK(budgets.mayAttempt(siblingB));
    CHECK(!budgets.mayAttempt(failedA)); // sibling/session churn cannot reset A

    budgets.markExhausted(changedRuntime);
    budgets.markExhausted(changedDestination);
    budgets.markExhausted(siblingB);
    budgets.clearForExplicitRetry("record-a");
    CHECK(budgets.mayAttempt(failedA));
    CHECK(budgets.mayAttempt(changedRuntime));
    CHECK(budgets.mayAttempt(changedDestination));
    CHECK(!budgets.mayAttempt(siblingB));

    RestoreBudgets prepared;
    CHECK(prepared.prepareTerminalInsert());
    RestoreBudgetKey preparedKey{"record-prepared","runtime-prepared","desktop"};
    CHECK(prepared.markExhaustedPrepared(std::move(preparedKey)));
    CHECK(!prepared.mayAttempt(
        {"record-prepared","runtime-prepared","desktop"}));
}

static void test_restore_budgets_prune_only_dead_runtime_identities(){
    RestoreBudgets budgets;
    const RestoreBudgetKey liveA{"record-a","runtime-a","desktop"};
    const RestoreBudgetKey liveB{"record-b","runtime-b","desktop"};
    const RestoreBudgetKey dead{"record-dead","runtime-dead","desktop"};
    budgets.markExhausted(liveA);
    budgets.markExhausted(liveB);
    budgets.markExhausted(dead);
    budgets.pruneToLiveIdentities({"runtime-a","runtime-b"});
    CHECK(!budgets.mayAttempt(liveA));
    CHECK(!budgets.mayAttempt(liveB));
    CHECK(budgets.mayAttempt(dead));
    CHECK(budgets.size()==2);
    budgets.pruneToLiveIdentities({});
    CHECK(budgets.size()==0 && budgets.mayAttempt(liveA));
}

static RestoreBudgetKey numbered_budget_key(int number){
    const std::string suffix=std::to_string(number);
    return {"record-"+suffix,"runtime-"+suffix,"desktop"};
}

static void test_restore_budgets_cap_uses_deterministic_touch_lru(){
    RestoreBudgets budgets;
    for(int i=0;i<256;++i) budgets.markExhausted(numbered_budget_key(i));
    CHECK(budgets.size()==256);
    CHECK(!budgets.mayAttempt(numbered_budget_key(0))); // refresh oldest
    budgets.markExhausted(numbered_budget_key(256));
    CHECK(budgets.size()==256);
    CHECK(!budgets.mayAttempt(numbered_budget_key(0)));
    CHECK(budgets.mayAttempt(numbered_budget_key(1)));  // least-recent untouched evicted
    CHECK(!budgets.mayAttempt(numbered_budget_key(2)));
    CHECK(!budgets.mayAttempt(numbered_budget_key(256)));
}

static void test_restore_budgets_new_key_copy_failure_is_transactional(){
    for(int fault=0;fault<2;++fault){
        bool inject=true;
        const RestoreBudgetKey rejected=numbered_budget_key(256);
        RestoreBudgetOps ops;
        ops.copyKey=[&](const RestoreBudgetKey& key)->RestoreBudgetKey{
            if(inject && key==rejected){
                if(fault==0) throw std::bad_alloc();
                throw std::length_error("injected restore-budget copy fault");
            }
            return key;
        };
        RestoreBudgets budgets(ops);
        for(int i=0;i<256;++i) budgets.markExhausted(numbered_budget_key(i));
        CHECK(!budgets.mayAttempt(numbered_budget_key(0))); // LRU is now key 1

        bool caught=false;
        try {
            budgets.markExhausted(rejected);
        } catch(const std::bad_alloc&) {
            caught=fault==0;
        } catch(const std::length_error&) {
            caught=fault==1;
        } catch(...) {
            caught=false;
        }
        CHECK(caught && budgets.size()==256);
        CHECK(!budgets.mayAttempt(numbered_budget_key(1))); // old key survived
        CHECK(budgets.mayAttempt(rejected));                // new key was not committed

        inject=false;
        budgets.markExhausted(numbered_budget_key(257));
        CHECK(budgets.size()==256);
        CHECK(budgets.mayAttempt(numbered_budget_key(2)));  // order survived; key 1 was touched
        CHECK(!budgets.mayAttempt(numbered_budget_key(3)));
        CHECK(!budgets.mayAttempt(numbered_budget_key(0)));
        CHECK(!budgets.mayAttempt(numbered_budget_key(1)));
        CHECK(!budgets.mayAttempt(numbered_budget_key(257)));
    }
}

// --- minimal SNSS encoder mirroring the REAL format ---
// cmds 0/2/7/8 = raw fixed structs of two int32 (no pickle header); cmd 6 = base::Pickle
// (4-byte-aligned fields; url = UTF-8 WriteString, title = UTF-16 WriteString16).
static void wInt(std::string& p,int v){ uint32_t bits=(uint32_t)(int32_t)v; for(int i=0;i<4;i++)p.push_back((char)((bits>>(8*i))&0xFF)); }   // raw int32
static void pkInt(std::string& p,int v){ while(p.size()%4)p.push_back(0); wInt(p,v); }                 // aligned
static void pkStr(std::string& p,const std::string& s){ pkInt(p,(int)s.size()); p+=s; while(p.size()%4)p.push_back(0); }
static void pkStr16(std::string& p,const std::string& s){ pkInt(p,(int)s.size()); for(char ch:s){ p.push_back(ch); p.push_back(0);} while(p.size()%4)p.push_back(0); }
static void snssFrame(std::string& f,unsigned char id,const std::string& content){
    unsigned sz=(unsigned)(content.size()+1);
    f.push_back((char)(sz&0xFF)); f.push_back((char)((sz>>8)&0xFF)); f.push_back((char)id); f+=content;
}
static void snssRaw(std::string& f,unsigned char id,int a,int b){ std::string c; wInt(c,a); wInt(c,b); snssFrame(f,id,c); }
static void snssPickle(std::string& f,unsigned char id,const std::string& payload){ std::string pk; wInt(pk,(int)payload.size()); pk+=payload; snssFrame(f,id,pk); }
static std::string makeSnss(){
    std::string f="SNSS"; wInt(f,3);
    snssRaw(f,0,10,1); snssRaw(f,0,10,2); snssRaw(f,0,11,3);   // SetTabWindow [win,tab]
    snssRaw(f,2,1,0); snssRaw(f,2,2,1); snssRaw(f,2,3,0);      // SetTabIndexInWindow [tab,idx]
    snssRaw(f,8,10,1); snssRaw(f,8,11,0);                      // SetSelectedTabInIndex [win,idx]
    auto nav=[&](int tab,int idx,const std::string& url,const std::string& title){ std::string p; pkInt(p,tab); pkInt(p,idx); pkStr(p,url); pkStr16(p,title); snssPickle(f,6,p); };
    nav(1,0,"https://github.com/x","GitHub"); nav(2,0,"https://docs.python.org/3","Python"); nav(3,0,"https://example.com/","Example");
    snssRaw(f,7,1,0); snssRaw(f,7,2,0); snssRaw(f,7,3,0);      // SetSelectedNavigationIndex [tab,idx]
    return f;
}
static void test_snss_parse(){
    std::vector<WinFp> w;
    CHECK(ParseChromiumSNSS(makeSnss(),w));
    CHECK(w.size()==2);
    int wi10=-1,wi11=-1; for(int i=0;i<(int)w.size();++i){ if(w[i].counts.count("github.com"))wi10=i; if(w[i].counts.count("example.com"))wi11=i; }
    CHECK(wi10>=0 && wi11>=0);
    CHECK(w[wi10].tabCount==2); CHECK(w[wi10].counts["github.com"]==1); CHECK(w[wi10].counts["python.org"]==1);
    CHECK(w[wi10].activeTitle=="Python"); CHECK(w[wi10].activeDomain=="python.org");
    CHECK(w[wi10].tabsBlob.find("GitHub")!=std::string::npos);     // all-tab blob has BOTH tabs (not just active)
    CHECK(w[wi10].tabsBlob.find("python.org")!=std::string::npos);
    CHECK(w[wi10].tabsBlob.find("github.com/x")!=std::string::npos);  // full URL path is searchable, not just the domain
    CHECK(w[wi11].tabCount==1); CHECK(w[wi11].activeTitle=="Example");
}
static void test_snss_garbage(){ std::vector<WinFp> w(1); CHECK(!ParseChromiumSNSS("not an snss file....",w)); CHECK(w.empty()); }

static void test_snss_truncated_frame_returns_no_partial_windows(){
    std::string bytes=makeSnss(); bytes.resize(bytes.size()-2);
    std::vector<WinFp> windows(1);
    CHECK(!ParseChromiumSNSS(bytes,windows));
    CHECK(windows.empty());
}

static void test_mozlz4_rejects_huge_declared_output(){
    std::string bytes("mozLz40\0",8); bytes.append(4,(char)0xff);
    std::string output="sentinel";
    CHECK(!MozLz4Decompress(bytes,MAX_BROWSER_SESSION_BYTES,output));
    CHECK(output.empty());
}

static void test_session_stamp_detects_change(){
    SessionStamp a; a.size=10; a.mtime=20;
    SessionStamp b=a;
    CHECK(a==b); b.mtime=21; CHECK(a!=b);
    b=a; b.changeTime=22; CHECK(a!=b);
    b=a; b.volumeSerial=23; CHECK(a!=b);
    b=a; b.fileIdLow=24; CHECK(a!=b);
    b=a; b.fileIdHigh=25; CHECK(a!=b);
}

static void test_firefox_json_rejects_trailing_and_excessive_depth(){
    JValue value;
    CHECK(!JParser("{} trailing").parse(value));
    std::string deep(MAX_JSON_DEPTH+1,'['); deep+="null";
    deep+=std::string(MAX_JSON_DEPTH+1,']');
    CHECK(!JParser(deep).parse(value));
}

static bool parseJsonWithLimits(const std::string& input,JValue& output,
                                const JsonLimits& limits){
    return JParser(input,limits).parse(output);
}

static void test_firefox_json_rejects_malformed_unicode_numbers_and_controls(){
    JValue value;
    CHECK(JParser("{\"ok\":true}").parse(value));
    CHECK(value.t==JValue::OBJ && value.find("ok") && value.find("ok")->b);
    const char* invalid[]={
        "\"unterminated", "\"raw\nnewline\"", "\"\\x\"", "\"\\u12\"",
        "\"\\ud800\"", "\"\\ud800\\u0041\"", "\"\\udc00\"",
        "01", "-01", "1.", ".1", "1e", "1e+", "+1", "--1", "1e309",
        "NaN", "Infinity"
    };
    for(const char* text:invalid){
        value.t=JValue::OBJ; value.obj["sentinel"]=JValue{};
        CHECK(!JParser(text).parse(value));
        CHECK(value.t==JValue::NUL && value.obj.empty() && value.arr.empty() && value.str.empty());
    }
    CHECK(JParser("\"\\ud83d\\ude00\"").parse(value));
    CHECK(value.t==JValue::STR && value.str=="\xf0\x9f\x98\x80");
    CHECK(JParser("[-0,0,1.25,-2E-3,1e308]").parse(value));
    const std::string malformedUtf8[]={
        std::string("\"\xc0\x80\"",4), std::string("\"\x80\"",3),
        std::string("\"\xed\xa0\x80\"",5), std::string("\"\xf4\x90\x80\x80\"",6),
        std::string("\"\xe2\x82\"",4)
    };
    for(size_t i=0;i<sizeof(malformedUtf8)/sizeof(malformedUtf8[0]);++i){
        CHECK(!JParser(malformedUtf8[i]).parse(value));
        CHECK(value.t==JValue::NUL);
    }
    CHECK(!JParser("1e-9999").parse(value));
}

static void test_firefox_json_depth_node_and_string_budget_boundaries(){
    JValue value;
    std::string boundary(MAX_JSON_DEPTH,'['); boundary+="null";
    boundary+=std::string(MAX_JSON_DEPTH,']');
    CHECK(JParser(boundary).parse(value));

    JsonLimits twoNodes(MAX_JSON_DEPTH,2,64);
    CHECK(parseJsonWithLimits("[null]",value,twoNodes));
    CHECK(!parseJsonWithLimits("[null,null]",value,twoNodes));
    CHECK(value.t==JValue::NUL);

    CHECK(parseJsonWithLimits("{\"aa\":\"bb\"}",value,JsonLimits(MAX_JSON_DEPTH,8,4)));
    CHECK(!parseJsonWithLimits("{\"aa\":\"bb\"}",value,JsonLimits(MAX_JSON_DEPTH,8,3)));
    CHECK(value.t==JValue::NUL);
    CHECK(parseJsonWithLimits("\"\\ud83d\\ude00\"",value,JsonLimits(MAX_JSON_DEPTH,2,4)));
    CHECK(!parseJsonWithLimits("\"\\ud83d\\ude00\"",value,JsonLimits(MAX_JSON_DEPTH,2,3)));
}

static void test_browser_parser_default_limits_are_exact(){
    CHECK(MAX_JSON_DEPTH==128);
    CHECK(MAX_JSON_NODES==2000000);
    CHECK(MAX_JSON_DECODED_STRING_BYTES==256ULL*1024ULL*1024ULL);
    CHECK(MAX_BROWSER_SESSION_BYTES==512ULL*1024ULL*1024ULL);
    JsonLimits json;
    CHECK(json.maxDepth==128 && json.maxNodes==2000000);
    CHECK(json.maxDecodedStringBytes==256ULL*1024ULL*1024ULL);
    SnssLimits snss;
    CHECK(snss.maxWindows==10000 && snss.maxTabs==100000);
    CHECK(snss.maxNavigations==1000000 && snss.maxCommands==2000000);
    CHECK(snss.maxSearchTextPerWindow==4ULL*1024ULL*1024ULL);
    CHECK(snss.maxRetainedTextBytes==256ULL*1024ULL*1024ULL);
}

static std::string snssWithTabWindows(int count){
    std::string file="SNSS"; wInt(file,3);
    for(int i=0;i<count;++i) snssRaw(file,0,100+i,1000+i);
    return file;
}

static void test_snss_rejects_zero_trailing_and_malformed_known_commands(){
    std::vector<WinFp> output(1);
    std::string trailing="SNSS"; wInt(trailing,3); trailing.push_back('\1');
    CHECK(!ParseChromiumSNSS(trailing,output)); CHECK(output.empty());
    std::string zero="SNSS"; wInt(zero,3); zero.push_back('\0'); zero.push_back('\0');
    CHECK(!ParseChromiumSNSS(zero,output)); CHECK(output.empty());
    std::string shortRaw="SNSS"; wInt(shortRaw,3); snssFrame(shortRaw,0,std::string(7,'x'));
    CHECK(!ParseChromiumSNSS(shortRaw,output)); CHECK(output.empty());
    std::string longRaw="SNSS"; wInt(longRaw,3); snssFrame(longRaw,0,std::string(9,'x'));
    CHECK(!ParseChromiumSNSS(longRaw,output)); CHECK(output.empty());
    std::string badPickle="SNSS"; wInt(badPickle,3); std::string content; wInt(content,100);
    snssFrame(badPickle,6,content);
    CHECK(!ParseChromiumSNSS(badPickle,output)); CHECK(output.empty());
}

static void test_snss_unique_id_and_command_cap_boundaries(){
    std::vector<WinFp> output;
    SnssLimits limits(2,2,2,8,4,1024);
    CHECK(ParseChromiumSNSS(snssWithTabWindows(2),output,limits));
    CHECK(output.size()==2);
    CHECK(!ParseChromiumSNSS(snssWithTabWindows(3),output,limits)); CHECK(output.empty());

    std::string duplicate="SNSS"; wInt(duplicate,3);
    snssRaw(duplicate,0,1,10); snssRaw(duplicate,0,1,10);
    CHECK(ParseChromiumSNSS(duplicate,output,limits));
    std::string negative="SNSS"; wInt(negative,3); snssRaw(negative,0,-1,10);
    CHECK(!ParseChromiumSNSS(negative,output,limits)); CHECK(output.empty());

    std::string commands="SNSS"; wInt(commands,3);
    snssFrame(commands,99,""); snssFrame(commands,99,""); snssFrame(commands,99,"");
    SnssLimits twoCommands(2,2,2,2,64,1024);
    CHECK(!ParseChromiumSNSS(commands,output,twoCommands)); CHECK(output.empty());
}

static void test_snss_window_and_tab_maps_have_independent_exact_caps(){
    std::vector<WinFp> output;
    std::string windows="SNSS"; wInt(windows,3);
    snssRaw(windows,8,1,0); snssRaw(windows,8,2,0);
    CHECK(ParseChromiumSNSS(windows,output,SnssLimits(2,2,2,2,64,64)) && output.empty());
    snssRaw(windows,8,3,0);
    CHECK(!ParseChromiumSNSS(windows,output,SnssLimits(2,3,2,3,64,64)) && output.empty());
    std::string tabs="SNSS"; wInt(tabs,3);
    snssRaw(tabs,2,10,0); snssRaw(tabs,2,20,0);
    CHECK(ParseChromiumSNSS(tabs,output,SnssLimits(2,2,2,2,64,64)) && output.empty());
    snssRaw(tabs,2,30,0);
    CHECK(!ParseChromiumSNSS(tabs,output,SnssLimits(2,2,2,3,64,64)) && output.empty());
}

static void test_snss_navigation_duplicate_and_search_budget_boundaries(){
    std::vector<WinFp> output;
    SnssLimits limits(2,3,2,16,4,1024);
    std::string exact="SNSS"; wInt(exact,3);
    snssRaw(exact,0,1,10); snssRaw(exact,0,1,11);
    CHECK(ParseChromiumSNSS(exact,output,limits));
    CHECK(output.size()==1 && output[0].tabsBlob.size()==4);
    snssRaw(exact,0,1,12);
    CHECK(!ParseChromiumSNSS(exact,output,limits)); CHECK(output.empty());

    auto nav=[&](std::string& file,int tab,int index){
        std::string payload; pkInt(payload,tab); pkInt(payload,index);
        pkStr(payload,"https://example.com/"); pkStr16(payload,"T");
        snssPickle(file,6,payload);
    };
    std::string navigations="SNSS"; wInt(navigations,3);
    nav(navigations,10,0); nav(navigations,10,0); nav(navigations,10,1);
    SnssLimits navLimits(2,3,2,16,128,1024);
    CHECK(ParseChromiumSNSS(navigations,output,navLimits));
    nav(navigations,10,2);
    CHECK(!ParseChromiumSNSS(navigations,output,navLimits)); CHECK(output.empty());
}

static std::string snssWithNavigationText(int window,int tab,int navigation,
                                          const std::string& url,const std::string& title){
    std::string file="SNSS"; wInt(file,3); snssRaw(file,0,window,tab);
    std::string payload; pkInt(payload,tab); pkInt(payload,navigation);
    pkStr(payload,url); pkStr16(payload,title); snssPickle(file,6,payload);
    return file;
}

static void test_snss_per_window_and_global_text_caps_are_exact(){
    std::vector<WinFp> output(1);
    std::string one=snssWithNavigationText(1,10,0,"abc","");
    CHECK(ParseChromiumSNSS(one,output,SnssLimits(1,1,1,2,5,3)));
    CHECK(output.size()==1 && output[0].tabsBlob==" abc ");
    CHECK(!ParseChromiumSNSS(one,output,SnssLimits(1,1,1,2,4,3)) && output.empty());
    CHECK(!ParseChromiumSNSS(one,output,SnssLimits(1,1,1,2,5,2)) && output.empty());

    std::string two="SNSS"; wInt(two,3);
    snssRaw(two,0,1,10); snssRaw(two,0,2,20);
    std::string first; pkInt(first,10); pkInt(first,0); pkStr(first,"abc"); pkStr16(first,"");
    std::string second; pkInt(second,20); pkInt(second,0); pkStr(second,"def"); pkStr16(second,"");
    snssPickle(two,6,first); snssPickle(two,6,second);
    CHECK(ParseChromiumSNSS(two,output,SnssLimits(2,2,2,4,5,6)) && output.size()==2);
    CHECK(!ParseChromiumSNSS(two,output,SnssLimits(2,2,2,4,5,5)) && output.empty());
    CHECK(!ParseChromiumSNSS(two,output,SnssLimits(2,2,2,3,5,6)) && output.empty());
}

static std::string makeMozLiteral(const std::string& decoded){
    std::string bytes("mozLz40\0",8); wInt(bytes,(int)decoded.size());
    if(decoded.empty()) return bytes;
    size_t literal=decoded.size();
    bytes.push_back((char)((literal<15?literal:15)<<4));
    if(literal>=15){ size_t extra=literal-15;
        while(extra>=255){ bytes.push_back((char)255); extra-=255; }
        bytes.push_back((char)extra);
    }
    bytes+=decoded; return bytes;
}

static void test_mozlz4_exact_decode_and_limit_boundaries(){
    std::string output="sentinel";
    CHECK(MozLz4Decompress(makeMozLiteral(""),MAX_BROWSER_SESSION_BYTES,output)); CHECK(output.empty());
    CHECK(MozLz4Decompress(makeMozLiteral("hello"),5,output)); CHECK(output=="hello");
    output="sentinel";
    CHECK(!MozLz4Decompress(makeMozLiteral("hello"),4,output)); CHECK(output.empty());
}

static void test_mozlz4_rejects_malformed_blocks_transactionally(){
    std::vector<std::string> malformed;
    malformed.push_back("");
    malformed.push_back(std::string("mozLz40\0",8));
    malformed.push_back(std::string("badLz400\0\0\0\0",12));
    std::string extension("mozLz40\0",8); wInt(extension,16);
    extension.push_back((char)0xf0); extension.push_back((char)255); malformed.push_back(extension);
    std::string zeroOffset("mozLz40\0",8); wInt(zeroOffset,5);
    zeroOffset.push_back((char)0x10); zeroOffset.push_back('a'); zeroOffset.push_back('\0'); zeroOffset.push_back('\0');
    malformed.push_back(zeroOffset);
    std::string trailing=makeMozLiteral("a"); trailing.push_back('x'); malformed.push_back(trailing);
    std::string emptyTrailing=makeMozLiteral(""); emptyTrailing.push_back('\0'); malformed.push_back(emptyTrailing);
    for(const auto& bytes:malformed){
        std::string output="sentinel";
        CHECK(!MozLz4Decompress(bytes,MAX_BROWSER_SESSION_BYTES,output)); CHECK(output.empty());
    }
}

static void test_lz4_match_offset_and_extension_arithmetic_edges(){
    uint8_t output[32]={0};
    const uint8_t repeated[]={0x10,'a',0x01,0x00};
    CHECK(Lz4BlockDecompress(repeated,sizeof(repeated),output,5)==5);
    CHECK(std::string((char*)output,5)=="aaaaa");
    const uint8_t distant[]={0x10,'a',0x02,0x00};
    CHECK(Lz4BlockDecompress(distant,sizeof(distant),output,5)<0);
    const uint8_t truncatedOffset[]={0x10,'a',0x01};
    CHECK(Lz4BlockDecompress(truncatedOffset,sizeof(truncatedOffset),output,5)<0);
    const uint8_t truncatedMatchExtension[]={0x1f,'a',0x01,0x00,0xff};
    CHECK(Lz4BlockDecompress(truncatedMatchExtension,sizeof(truncatedMatchExtension),output,sizeof(output))<0);
    CHECK(Lz4BlockDecompress(nullptr,1,output,sizeof(output))<0);
    CHECK(Lz4BlockDecompress(repeated,sizeof(repeated),nullptr,5)<0);

    size_t length=(std::numeric_limits<size_t>::max)()-1;
    CHECK(Lz4CheckedAdd(length,1) && length==(std::numeric_limits<size_t>::max)());
    CHECK(!Lz4CheckedAdd(length,1) && length==(std::numeric_limits<size_t>::max)());
}

static bool setSparseSessionFileSize(const std::wstring& path,unsigned long long size){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER position; position.QuadPart=(LONGLONG)size;
    bool ok=SetFilePointerEx(file,position,nullptr,FILE_BEGIN)!=FALSE && SetEndOfFile(file)!=FALSE;
    CloseHandle(file); return ok;
}

static bool writeSessionStampFixture(const std::wstring& path,const std::string& bytes,
                                     const FILETIME& mtime,DWORD disposition){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,
                            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            nullptr,disposition,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER zero{};
    DWORD written=0;
    bool ok=SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)!=FALSE &&
            WriteFile(file,bytes.data(),(DWORD)bytes.size(),&written,nullptr)!=FALSE &&
            written==bytes.size() && SetEndOfFile(file)!=FALSE &&
            SetFileTime(file,nullptr,nullptr,&mtime)!=FALSE && FlushFileBuffers(file)!=FALSE;
    CloseHandle(file);
    return ok;
}

static void test_session_stamp_detects_equal_metadata_replace_and_in_place_rewrite(){
    wchar_t temp[MAX_PATH+1]={0},pathA[MAX_PATH+1]={0},pathB[MAX_PATH+1]={0};
    DWORD length=GetTempPathW(MAX_PATH,temp);
    CHECK(length>0 && length<=MAX_PATH); if(length==0 || length>MAX_PATH) return;
    CHECK(GetTempFileNameW(temp,L"vdi",0,pathA)!=0);
    CHECK(GetTempFileNameW(temp,L"vdi",0,pathB)!=0);
    FILETIME fixedTime{}; GetSystemTimeAsFileTime(&fixedTime);
    CHECK(writeSessionStampFixture(pathA,"AAAA",fixedTime,CREATE_ALWAYS));
    CHECK(writeSessionStampFixture(pathB,"BBBB",fixedTime,CREATE_ALWAYS));
    SessionStamp original,replacementObject;
    CHECK(GetSessionStamp(pathA,original));
    CHECK(GetSessionStamp(pathB,replacementObject));
    CHECK(original.size==replacementObject.size && original.mtime==replacementObject.mtime);
    CHECK(original!=replacementObject);
    CHECK(MoveFileExW(pathB,pathA,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE);
    SessionStamp replaced;
    CHECK(GetSessionStamp(pathA,replaced));
    CHECK(replaced.size==original.size && replaced.mtime==original.mtime && replaced!=original);
    CHECK(replaced.volumeSerial==replacementObject.volumeSerial &&
          replaced.fileIdLow==replacementObject.fileIdLow && replaced.fileIdHigh==replacementObject.fileIdHigh);

    HANDLE delay=CreateWaitableTimerW(nullptr,TRUE,nullptr);
    CHECK(delay!=nullptr);
    if(delay){
        LARGE_INTEGER due{};
        due.QuadPart=-20LL*10000LL;
        CHECK(SetWaitableTimer(delay,&due,0,nullptr,nullptr,FALSE)!=FALSE);
        CHECK(WaitForSingleObject(delay,1000)==WAIT_OBJECT_0);
        CloseHandle(delay);
    }
    CHECK(writeSessionStampFixture(pathA,"CCCC",fixedTime,OPEN_EXISTING));
    SessionStamp rewritten;
    CHECK(GetSessionStamp(pathA,rewritten));
    CHECK(rewritten.size==replaced.size && rewritten.mtime==replaced.mtime &&
          rewritten.volumeSerial==replaced.volumeSerial && rewritten.fileIdLow==replaced.fileIdLow &&
          rewritten.fileIdHigh==replaced.fileIdHigh && rewritten.changeTime!=replaced.changeTime);
    CHECK(rewritten!=replaced);
    DWORD handlesBefore=0,handlesAfter=0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesBefore)!=0);
    for(int attempt=0;attempt<256;++attempt){
        SessionStamp repeated;
        CHECK(GetSessionStamp(pathA,repeated) && repeated==rewritten);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesAfter)!=0);
    CHECK(handlesAfter==handlesBefore);
    CHECK(DeleteFileW(pathA)!=FALSE);
    DeleteFileW(pathB);
}

static void test_get_session_stamp_accepts_exact_cap_and_rejects_over(){
    wchar_t temp[MAX_PATH+1]={0},path[MAX_PATH+1]={0};
    DWORD length=GetTempPathW(MAX_PATH,temp);
    CHECK(length>0 && length<=MAX_PATH);
    if(length==0 || length>MAX_PATH) return;
    CHECK(GetTempFileNameW(temp,L"vds",0,path)!=0);
    if(!path[0]) return;
    CHECK(setSparseSessionFileSize(path,MAX_BROWSER_SESSION_BYTES));
    SessionStamp stamp; stamp.size=7; stamp.mtime=8;
    CHECK(GetSessionStamp(path,stamp) && stamp.size==MAX_BROWSER_SESSION_BYTES);
    CHECK(setSparseSessionFileSize(path,MAX_BROWSER_SESSION_BYTES+1));
    SessionStamp sentinel; sentinel.size=17; sentinel.mtime=18; stamp=sentinel;
    CHECK(!GetSessionStamp(path,stamp) && stamp==sentinel);
    CHECK(DeleteFileW(path)!=FALSE);
}

static void test_firefox_profile_ini_default_release_fallback(){
    const std::string fallback="[Profile0]\nName=release\nIsRelative=1\nPath=Profiles/demo.default-release\n";
    CHECK(ResolveFirefoxProfileDirectoryFromIni(L"C:\\Firefox",fallback)==
          L"C:\\Firefox\\Profiles\\demo.default-release");
    const std::string installed="[Install123]\nDefault=Profiles/main\n[Profile0]\nDefault=1\nPath=Profiles/other\n";
    CHECK(ResolveFirefoxProfileDirectoryFromIni(L"C:\\Firefox",installed)==
          L"C:\\Firefox\\Profiles\\main");
}

static void test_firefox_json_valid_empty_is_distinct_from_failure(){
    std::vector<WinFp> output(1);
    CHECK(ParseFirefoxSessionJson("{\"windows\":[]}",output)); CHECK(output.empty());
    output.push_back(WinFp{});
    CHECK(!ParseFirefoxSessionJson("{\"windows\":[",output)); CHECK(output.empty());
}

static void test_firefox_selected_index_rejects_int_min_without_overflow(){
    CHECK(!FirefoxSelectedTabMatches(INT_MIN,0));
    CHECK(!FirefoxSelectedTabMatches(0,0));
    CHECK(FirefoxSelectedTabMatches(1,0));
    CHECK(FirefoxSelectedTabMatches(INT_MAX,(size_t)INT_MAX-1));
    std::vector<WinFp> output;
    CHECK(ParseFirefoxSessionJson(
        "{\"windows\":[{\"selected\":-2147483648,\"tabs\":[{\"entries\":[{\"url\":\"https://example.com\",\"title\":\"Example\"}]}]}]}",
        output));
    CHECK(output.size()==1 && output[0].activeTitle.empty() && output[0].activeDomain.empty());
}

static AppProfile sessionTestProfile(const std::string& app,AppProfile::Sess session=AppProfile::FIREFOX){
    AppProfile profile;
    profile.id=app;
    profile.classNames.push_back(L"TestWindow");
    profile.exeName=L"test.exe";
    profile.titleSuffixes.push_back(L" - Test");
    profile.session=session;
    profile.userDataDir=L"test-data";
    return profile;
}

struct SessionPathFixture {
    std::wstring root,defaultDir,sessionsDir;
    std::vector<std::wstring> files;
    bool ready=false;
    SessionPathFixture(){
        wchar_t temp[MAX_PATH+1]={0},uniquePath[MAX_PATH+1]={0};
        DWORD length=GetTempPathW(MAX_PATH,temp);
        if(length==0 || length>MAX_PATH || !GetTempFileNameW(temp,L"vdp",0,uniquePath)) return;
        root=uniquePath;
        if(!DeleteFileW(root.c_str()) || !CreateDirectoryW(root.c_str(),nullptr)) return;
        defaultDir=root+L"\\Default"; sessionsDir=defaultDir+L"\\Sessions";
        if(!CreateDirectoryW(defaultDir.c_str(),nullptr) || !CreateDirectoryW(sessionsDir.c_str(),nullptr)) return;
        ready=true;
    }
    bool add(const wchar_t* name,unsigned long long modified,unsigned long long size=1){
        if(!ready) return false;
        std::wstring path=sessionsDir+L"\\"+name;
        HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER end; end.QuadPart=(LONGLONG)size;
        FILETIME time; ULARGE_INTEGER bits; bits.QuadPart=modified;
        time.dwLowDateTime=bits.LowPart; time.dwHighDateTime=bits.HighPart;
        bool ok=SetFilePointerEx(file,end,nullptr,FILE_BEGIN)!=FALSE && SetEndOfFile(file)!=FALSE &&
                SetFileTime(file,nullptr,nullptr,&time)!=FALSE;
        CloseHandle(file);
        if(ok) files.push_back(path); else DeleteFileW(path.c_str());
        return ok;
    }
    ~SessionPathFixture(){
        for(size_t i=0;i<files.size();++i) DeleteFileW(files[i].c_str());
        if(!sessionsDir.empty()) RemoveDirectoryW(sessionsDir.c_str());
        if(!defaultDir.empty()) RemoveDirectoryW(defaultDir.c_str());
        if(!root.empty()) RemoveDirectoryW(root.c_str());
    }
};

static void test_chromium_resolver_tracks_rotation_and_breaks_stamp_ties(){
    SessionPathFixture fixture;
    CHECK(fixture.ready); if(!fixture.ready) return;
    AppProfile profile=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    profile.userDataDir=fixture.root;
    CHECK(fixture.add(L"Session_A",100));
    CHECK(ResolveBrowserSessionPath(profile)==fixture.sessionsDir+L"\\Session_A");
    CHECK(fixture.add(L"Session_B",200));
    CHECK(ResolveBrowserSessionPath(profile)==fixture.sessionsDir+L"\\Session_B");
    CHECK(fixture.add(L"Session_C",200));
    CHECK(ResolveBrowserSessionPath(profile)==fixture.sessionsDir+L"\\Session_C");
    CHECK(fixture.add(L"Session_zero",300,0));
    CHECK(ResolveBrowserSessionPath(profile)==fixture.sessionsDir+L"\\Session_C");
}

struct SessionResultSink {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::unique_ptr<SessionResult> > results;
    std::atomic<bool> wrongMessage{false};
    bool post(HWND,UINT message,WPARAM,LPARAM value){
        if(message!=WM_SESSION_RESULT) wrongMessage=true;
        std::unique_ptr<SessionResult> owned((SessionResult*)value);
        { std::lock_guard<std::mutex> lock(mutex); results.push_back(std::move(owned)); }
        changed.notify_all();
        return true;
    }
    std::unique_ptr<SessionResult> waitFor(uint64_t requestId){
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock,std::chrono::seconds(5),[&]{
            for(size_t i=0;i<results.size();++i) if(results[i]->requestId==requestId) return true;
            return false;
        });
        for(std::deque<std::unique_ptr<SessionResult> >::iterator it=results.begin();it!=results.end();++it){
            if((*it)->requestId==requestId){
                std::unique_ptr<SessionResult> found=std::move(*it);
                results.erase(it);
                return found;
            }
        }
        return std::unique_ptr<SessionResult>();
    }
};

static void test_already_posted_session_results_are_drained_and_freed(){
    HWND window=CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,nullptr,
                                GetModuleHandleW(nullptr),nullptr);
    CHECK(window!=nullptr); if(!window) return;
    std::shared_ptr<const std::vector<WinFp> > payload(new std::vector<WinFp>(1));
    SessionResult* result=new SessionResult(); result->windows=payload;
    CHECK(payload.use_count()==2);
    CHECK(PostMessageW(window,WM_SESSION_RESULT,0,(LPARAM)result)!=FALSE);
    CHECK(DrainPostedSessionResults(window)==1);
    CHECK(payload.use_count()==1);
    CHECK(DrainPostedSessionResults(window)==0);
    CHECK(DestroyWindow(window)!=FALSE);
}

static void test_session_status_and_acceptance_policy_contract(){
    CHECK(SessionDataUsable(SessionDataStatus::Fresh));
    CHECK(SessionDataUsable(SessionDataStatus::CachedStale));
    CHECK(!SessionDataUsable(SessionDataStatus::Unavailable));
    CHECK(!SessionDataUsable(SessionDataStatus::Superseded));
    SessionPolicy fresh=SessionAcceptancePolicy(SessionDataStatus::Fresh);
    CHECK(fresh.matchExisting && fresh.restoreExisting && fresh.createUnmatched);
    CHECK(fresh.updateFingerprints && fresh.markMissing && !fresh.deferOnce);
    SessionPolicy stale=SessionAcceptancePolicy(SessionDataStatus::CachedStale);
    CHECK(stale.matchExisting && stale.restoreExisting && !stale.createUnmatched);
    CHECK(!stale.updateFingerprints && !stale.markMissing && stale.unmatchedLiveWaits);
    SessionPolicy unavailable=SessionAcceptancePolicy(SessionDataStatus::Unavailable);
    CHECK(!unavailable.matchExisting && !unavailable.restoreExisting && !unavailable.createUnmatched);
    CHECK(!unavailable.updateFingerprints && !unavailable.markMissing && unavailable.deferOnce);
    SessionPolicy superseded=SessionAcceptancePolicy(SessionDataStatus::Superseded);
    CHECK(!superseded.matchExisting && !superseded.restoreExisting && !superseded.deferOnce);
}

static void test_session_cache_shares_payload_and_rejects_oversize(){
    std::vector<WinFp> seed(1); seed[0].tabsBlob="payload";
    size_t bytes=EstimateSessionPayloadBytes(seed);
    SessionCache cache(2,bytes);
    SessionCacheValue stored;
    SessionStamp stamp; stamp.size=10; stamp.mtime=20;
    CHECK(cache.Put("firefox",L"one",stamp,std::move(seed),101,1,stored));
    CHECK(stored.windows && cache.EntryCount()==1 && cache.RetainedBytes()==bytes);
    SessionCacheValue hit;
    CHECK(cache.FindExact("firefox",L"one",stamp,hit));
    CHECK(hit.windows.get()==stored.windows.get());

    std::vector<WinFp> second(1); second[0].tabsBlob="second";
    SessionCacheValue rejected;
    CHECK(!cache.Put("chrome",L"two",stamp,std::move(second),102,2,rejected));
    CHECK(!rejected.windows);
    stored.windows.reset(); hit.windows.reset();
    CHECK(cache.RetainedBytes()<=bytes);
}

static SessionFileReadResult successfulSessionRead(const std::string& bytes,
                                                    const SessionStamp& stamp){
    SessionFileReadResult result;
    result.status=FileReadStatus::Ok;
    result.bytes=bytes;
    result.readStamp=stamp;
    result.readStampKnown=true;
    return result;
}

static SessionFileReadResult successfulSessionRead(const std::string& bytes,
        unsigned long long size,unsigned long long mtime){
    SessionStamp stamp; stamp.size=size; stamp.mtime=mtime;
    return successfulSessionRead(bytes,stamp);
}

static void test_session_worker_valid_empty_is_fresh_and_cache_hit_is_shared(){
    SessionResultSink sink;
    std::atomic<int> reads(0),parses(0),stamps(0);
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"session-file"); };
    ops.getStamp=[&](const std::wstring&,SessionStamp& stamp){ ++stamps; stamp.size=5; stamp.mtime=9; return true; };
    ops.readFile=[&](const std::wstring&){ ++reads; return successfulSessionRead("valid",5,9); };
    ops.parse=[&](const AppProfile&,const std::string& bytes,std::vector<WinFp>& output){ ++parses; output.clear(); return bytes=="valid"; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops,16,1024*1024);
    SessionRequest first;
    first.requestId=1; first.app="firefox"; first.profile=sessionTestProfile("firefox");
    first.purpose=SessionPurpose::ManualRestore; first.identityGeneration=7;
    CHECK(worker.Request(first));
    std::unique_ptr<SessionResult> fresh=sink.waitFor(1);
    CHECK(fresh && fresh->status==SessionDataStatus::Fresh && fresh->windows && fresh->windows->empty());
    CHECK(fresh && fresh->purpose==SessionPurpose::ManualRestore && fresh->identityGeneration==7);
    SessionStamp expectedStamp; expectedStamp.size=5; expectedStamp.mtime=9;
    CHECK(fresh && fresh->sourceStampKnown && fresh->sourceStamp==expectedStamp);
    CHECK(fresh && fresh->dataStamp==expectedStamp && fresh->dataGeneration==1);
    const std::vector<WinFp>* identity=fresh?fresh->windows.get():nullptr;
    SessionRequest second=first; second.requestId=2; second.purpose=SessionPurpose::Search;
    CHECK(worker.Request(second));
    std::unique_ptr<SessionResult> cached=sink.waitFor(2);
    CHECK(cached && cached->status==SessionDataStatus::Fresh && cached->windows.get()==identity);
    CHECK(cached && cached->purpose==SessionPurpose::Search && cached->dataGeneration==1);
    CHECK(reads.load()==1 && parses.load()==1);
    worker.Stop();
    SessionRequest rejected=first; rejected.requestId=3;
    CHECK(!worker.Request(rejected));
}

static void test_session_worker_malformed_cold_is_unavailable(){
    SessionResultSink sink;
    std::atomic<int> parses(0);
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"cold"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=11; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("malformed",11,1); };
    ops.parse=[&](const AppProfile&,const std::string&,std::vector<WinFp>& output){ ++parses; output.push_back(WinFp{}); return false; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=20; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.purpose=SessionPurpose::AutoReconcile; request.identityGeneration=2;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> result=sink.waitFor(20);
    SessionStamp current; current.size=11; current.mtime=1;
    CHECK(result && result->status==SessionDataStatus::Unavailable && !result->windows);
    CHECK(result && result->sourceStampKnown && result->sourceStamp==current);
    CHECK(result && result->dataStamp==SessionStamp{} && result->dataGeneration==0);
    CHECK(parses.load()==1);
    worker.Stop();
}

static void test_session_worker_non_ok_reads_never_parse_and_publish_current_stamp(){
    SessionResultSink sink;
    std::atomic<int> reads(0),parses(0);
    const FileReadStatus statuses[]={FileReadStatus::Missing,FileReadStatus::Unavailable,FileReadStatus::TooLarge};
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"read-gated"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=23; stamp.mtime=45; return true; };
    ops.readFile=[&](const std::wstring&){ SessionFileReadResult result; result.status=statuses[reads++]; return result; };
    ops.parse=[&](const AppProfile&,const std::string&,std::vector<WinFp>&){ ++parses; return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    SessionStamp current; current.size=23; current.mtime=45;
    for(int i=0;i<3;++i){
        request.requestId=30+i;
        CHECK(worker.Request(request));
        std::unique_ptr<SessionResult> result=sink.waitFor(request.requestId);
        CHECK(result && result->status==SessionDataStatus::Unavailable && !result->windows);
        CHECK(result && result->path==L"read-gated" && result->sourceStampKnown && result->sourceStamp==current);
        CHECK(result && result->dataStamp==SessionStamp{} && result->dataGeneration==0);
    }
    CHECK(reads.load()==3 && parses.load()==0);
    worker.Stop();
}

static void test_session_worker_disappeared_source_is_not_reported_as_current(){
    SessionResultSink sink;
    std::atomic<int> resolves(0),stampCalls(0);
    SessionWorkerOps ops;
    ops.resolvePath=[&](const AppProfile&){ return ++resolves==1?std::wstring(L"gone"):std::wstring(); };
    ops.getStamp=[&](const std::wstring& path,SessionStamp& stamp){ ++stampCalls; stamp.size=4; stamp.mtime=5; return path==L"gone"; };
    ops.readFile=[](const std::wstring&){ SessionFileReadResult result; result.status=FileReadStatus::Missing; return result; };
    ops.parse=[](const AppProfile&,const std::string&,std::vector<WinFp>&){ return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=35; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> result=sink.waitFor(35);
    CHECK(result && result->status==SessionDataStatus::Unavailable);
    CHECK(result && result->path.empty() && !result->sourceStampKnown && result->sourceStamp==SessionStamp{});
    CHECK(stampCalls.load()==1);
    worker.Stop();
}

static void test_session_worker_stamp_change_uses_exact_path_cached_stale(){
    SessionResultSink sink;
    std::atomic<unsigned long long> mtime(1);
    std::atomic<bool> parseOk(true);
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"same-path"); };
    ops.getStamp=[&](const std::wstring&,SessionStamp& stamp){ stamp.size=9; stamp.mtime=mtime.load(); return true; };
    ops.readFile=[&](const std::wstring&){ return successfulSessionRead("bytes",9,mtime.load()); };
    ops.parse=[&](const AppProfile&,const std::string&,std::vector<WinFp>& output){
        output.clear(); WinFp window; window.activeTitle="cached"; output.push_back(window); return parseOk.load();
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=21; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.purpose=SessionPurpose::HeartbeatSave; request.identityGeneration=4;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> fresh=sink.waitFor(21);
    CHECK(fresh && fresh->status==SessionDataStatus::Fresh && fresh->windows->size()==1);
    const std::vector<WinFp>* identity=fresh?fresh->windows.get():nullptr;
    uint64_t generation=fresh?fresh->dataGeneration:0;

    mtime=2; parseOk=false; request.requestId=22;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> stale=sink.waitFor(22);
    SessionStamp source; source.size=9; source.mtime=2;
    SessionStamp data; data.size=9; data.mtime=1;
    CHECK(stale && stale->status==SessionDataStatus::CachedStale && stale->windows.get()==identity);
    CHECK(stale && stale->sourceStampKnown && stale->sourceStamp==source);
    CHECK(stale && stale->dataStamp==data && stale->dataGeneration==generation);
    worker.Stop();
}

static void test_session_worker_rotation_during_parse_is_never_fresh(){
    SessionResultSink sink;
    std::atomic<int> resolves(0);
    SessionWorkerOps ops;
    ops.resolvePath=[&](const AppProfile&){ return ++resolves==1?std::wstring(L"Session_old"):std::wstring(L"Session_new"); };
    ops.getStamp=[](const std::wstring& path,SessionStamp& stamp){ stamp.size=100; stamp.mtime=path==L"Session_old"?1:2; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("old",100,1); };
    ops.parse=[](const AppProfile&,const std::string&,std::vector<WinFp>& output){ output.clear(); output.push_back(WinFp{}); return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=23; request.app="chrome"; request.profile=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    request.purpose=SessionPurpose::MetadataProbe; request.identityGeneration=8;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> result=sink.waitFor(23);
    SessionStamp current; current.size=100; current.mtime=2;
    CHECK(result && result->status==SessionDataStatus::Unavailable && !result->windows);
    CHECK(result && result->path==L"Session_new" && result->sourceStampKnown && result->sourceStamp==current);
    CHECK(result && result->dataGeneration==0 && result->dataStamp==SessionStamp{});
    worker.Stop();
}

static void test_session_worker_equal_metadata_replacement_never_publishes_old_bytes_fresh(){
    SessionResultSink sink;
    std::atomic<unsigned long long> objectId(1);
    std::atomic<bool> replaceDuringParse(true);
    std::atomic<int> reads(0),parses(0);
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"same-metadata-path"); };
    ops.getStamp=[&](const std::wstring&,SessionStamp& stamp){
        unsigned long long identity=objectId.load();
        stamp.size=4; stamp.mtime=100; stamp.changeTime=1000+identity;
        stamp.volumeSerial=7; stamp.fileIdLow=identity; stamp.fileIdHigh=9;
        return true;
    };
    ops.readFile=[&](const std::wstring&){
        ++reads;
        unsigned long long identity=objectId.load();
        SessionStamp stamp; stamp.size=4; stamp.mtime=100; stamp.changeTime=1000+identity;
        stamp.volumeSerial=7; stamp.fileIdLow=identity; stamp.fileIdHigh=9;
        return successfulSessionRead(std::string(4,(char)('A'+(int)identity-1)),stamp);
    };
    ops.parse=[&](const AppProfile&,const std::string& bytes,std::vector<WinFp>& output){
        ++parses; output.clear(); WinFp window; window.activeTitle=bytes; output.push_back(std::move(window));
        if(replaceDuringParse.exchange(false)) ++objectId;
        return true;
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops,4,1024*1024);
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");

    request.requestId=620; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> coldRotated=sink.waitFor(620);
    CHECK(coldRotated && coldRotated->status==SessionDataStatus::Unavailable && !coldRotated->windows &&
          coldRotated->sourceStampKnown && coldRotated->sourceStamp.fileIdLow==2);

    request.requestId=621; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> freshB=sink.waitFor(621);
    CHECK(freshB && freshB->status==SessionDataStatus::Fresh && freshB->windows->at(0).activeTitle=="BBBB" &&
          freshB->dataStamp.fileIdLow==2 && freshB->dataGeneration==1);
    const std::vector<WinFp>* bIdentity=freshB?freshB->windows.get():nullptr;
    request.requestId=622; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> hitB=sink.waitFor(622);
    CHECK(hitB && hitB->status==SessionDataStatus::Fresh && hitB->windows.get()==bIdentity &&
          hitB->dataGeneration==1 && reads.load()==2 && parses.load()==2);

    objectId=3; replaceDuringParse=true; request.requestId=623; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> staleB=sink.waitFor(623);
    CHECK(staleB && staleB->status==SessionDataStatus::CachedStale && staleB->windows.get()==bIdentity &&
          staleB->dataStamp.fileIdLow==2 && staleB->sourceStampKnown && staleB->sourceStamp.fileIdLow==4);
    request.requestId=624; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> freshD=sink.waitFor(624);
    CHECK(freshD && freshD->status==SessionDataStatus::Fresh && freshD->windows->at(0).activeTitle=="DDDD" &&
          freshD->dataStamp.fileIdLow==4 && freshD->dataGeneration==2);
    worker.Stop();
}

static SessionStamp syntheticSessionStamp(unsigned long long revision){
    SessionStamp stamp;
    stamp.size=1; stamp.mtime=100+revision; stamp.changeTime=200+revision;
    stamp.volumeSerial=300; stamp.fileIdLow=400+revision; stamp.fileIdHigh=500;
    return stamp;
}

static void test_session_worker_rejects_aba_bytes_without_matching_handle_stamp(){
    SessionStamp endpoint=syntheticSessionStamp(1);
    SessionStamp bytesStamp=endpoint;
    std::string bytes="A";
    std::atomic<int> parses(0);
    SessionResultSink sink;
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"aba-path"); };
    ops.getStamp=[&](const std::wstring&,SessionStamp& stamp){ stamp=endpoint; return true; };
    ops.readFile=[&](const std::wstring&){ return successfulSessionRead(bytes,bytesStamp); };
    ops.parse=[&](const AppProfile&,const std::string& input,std::vector<WinFp>& output){
        ++parses;
        output.clear(); WinFp window; window.activeTitle=input; output.push_back(window); return true;
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };

    SessionWorker cachedWorker((HWND)1,ops);
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.requestId=203; CHECK(cachedWorker.Request(request));
    std::unique_ptr<SessionResult> seeded=sink.waitFor(203);
    CHECK(seeded && seeded->status==SessionDataStatus::Fresh && seeded->windows &&
          seeded->windows->at(0).activeTitle=="A" && seeded->dataStamp==endpoint);
    const std::vector<WinFp>* cachedIdentity=seeded?seeded->windows.get():nullptr;

    SessionStamp cachedStamp=endpoint;
    endpoint=syntheticSessionStamp(2);       // A is current at both endpoint observations.
    bytesStamp=syntheticSessionStamp(9);     // The exact read handle belonged to B.
    bytes="B";
    request.requestId=204; CHECK(cachedWorker.Request(request));
    std::unique_ptr<SessionResult> stale=sink.waitFor(204);
    CHECK(stale && stale->status==SessionDataStatus::CachedStale &&
          stale->windows.get()==cachedIdentity && stale->windows->at(0).activeTitle=="A");
    CHECK(stale && stale->sourceStampKnown && stale->sourceStamp==endpoint &&
          stale->dataStamp==cachedStamp);
    CHECK(cachedWorker.Stop());

    SessionResultSink coldSink;
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return coldSink.post(hwnd,message,wp,lp); };
    SessionWorker coldWorker((HWND)1,ops);
    request.requestId=205; CHECK(coldWorker.Request(request));
    std::unique_ptr<SessionResult> cold=coldSink.waitFor(205);
    CHECK(cold && cold->status==SessionDataStatus::Unavailable && !cold->windows &&
          cold->sourceStampKnown && cold->sourceStamp==endpoint && cold->dataGeneration==0);
    CHECK(parses.load()==1);
    CHECK(coldWorker.Stop());
}

static void test_session_worker_rotation_uses_only_exact_attempted_path_cache(){
    SessionResultSink sink;
    std::atomic<int> resolves(0);
    std::atomic<unsigned long long> oldMtime(1);
    SessionWorkerOps ops;
    ops.resolvePath=[&](const AppProfile&){ int call=++resolves; return call<=3?std::wstring(L"Session_old"):std::wstring(L"Session_new"); };
    ops.getStamp=[&](const std::wstring& path,SessionStamp& stamp){ stamp.size=50; stamp.mtime=path==L"Session_old"?oldMtime.load():3; return true; };
    ops.readFile=[&](const std::wstring&){ return successfulSessionRead("bytes",50,oldMtime.load()); };
    ops.parse=[](const AppProfile&,const std::string&,std::vector<WinFp>& output){ output.clear(); WinFp fp; fp.activeTitle="old-cache"; output.push_back(fp); return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=24; request.app="chrome"; request.profile=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    request.purpose=SessionPurpose::AutoReconcile; request.identityGeneration=9;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> fresh=sink.waitFor(24);
    CHECK(fresh && fresh->status==SessionDataStatus::Fresh);
    const std::vector<WinFp>* cachedIdentity=fresh?fresh->windows.get():nullptr;
    uint64_t cachedGeneration=fresh?fresh->dataGeneration:0;
    oldMtime=2; request.requestId=25;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> rotated=sink.waitFor(25);
    SessionStamp current; current.size=50; current.mtime=3;
    SessionStamp data; data.size=50; data.mtime=1;
    CHECK(rotated && rotated->status==SessionDataStatus::CachedStale);
    CHECK(rotated && rotated->path==L"Session_new" && rotated->sourceStampKnown && rotated->sourceStamp==current);
    CHECK(rotated && rotated->windows.get()==cachedIdentity && rotated->dataStamp==data && rotated->dataGeneration==cachedGeneration);
    worker.Stop();
}

struct BlockingSessionParse {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false;
    bool released=false;
    bool parse(const AppProfile&,const std::string&,std::vector<WinFp>& output){
        std::unique_lock<std::mutex> lock(mutex);
        entered=true; changed.notify_all();
        changed.wait(lock,[&]{ return released; });
        output.clear();
        return true;
    }
    bool waitEntered(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(5),[&]{ return entered; });
    }
    void release(){ std::lock_guard<std::mutex> lock(mutex); released=true; changed.notify_all(); }
};

static SessionWorkerOps coalescingWorkerOps(SessionResultSink& sink,BlockingSessionParse& blocker){
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile& profile){ return U82W(profile.id)+L"-session"; };
    ops.getStamp=[](const std::wstring& path,SessionStamp& stamp){ stamp.size=path.size(); stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring& path){ return successfulSessionRead("valid",path.size(),1); };
    ops.parse=[&](const AppProfile& profile,const std::string& bytes,std::vector<WinFp>& output){ return blocker.parse(profile,bytes,output); };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    return ops;
}

static void test_session_worker_ten_rapid_requests_are_active_plus_newest_pending(){
    SessionResultSink sink; BlockingSessionParse blocker;
    SessionWorker worker((HWND)1,coalescingWorkerOps(sink,blocker));
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.identityGeneration=10;
    SessionPurpose purposes[10]={
        SessionPurpose::MetadataProbe,SessionPurpose::Search,SessionPurpose::Search,
        SessionPurpose::AutoReconcile,SessionPurpose::HeartbeatSave,SessionPurpose::ManualSave,
        SessionPurpose::ManualRestore,SessionPurpose::ManualSave,SessionPurpose::ManualRestore,
        SessionPurpose::ManualSave
    };
    request.requestId=100; request.purpose=purposes[0];
    CHECK(worker.Request(request)); CHECK(blocker.waitEntered());
    for(int i=1;i<10;++i){ request.requestId=100+i; request.purpose=purposes[i]; CHECK(worker.Request(request)); }
    CHECK(worker.OutstandingForApp("firefox")<=2);
    blocker.release();
    int fresh=0,superseded=0;
    for(int i=0;i<10;++i){
        std::unique_ptr<SessionResult> result=sink.waitFor(100+i);
        CHECK(result && result->purpose==purposes[i]);
        if(result && result->status==SessionDataStatus::Fresh) ++fresh;
        if(result && result->status==SessionDataStatus::Superseded) ++superseded;
    }
    CHECK(fresh==2 && superseded==8);
    CHECK(!sink.wrongMessage.load());
    worker.Stop();
}

static void test_session_worker_low_probe_cannot_replace_user_pending(){
    SessionResultSink sink; BlockingSessionParse blocker;
    SessionWorker worker((HWND)1,coalescingWorkerOps(sink,blocker));
    SessionRequest active; active.requestId=200; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::Search;
    CHECK(worker.Request(active)); CHECK(blocker.waitEntered());
    SessionRequest manual=active; manual.requestId=201; manual.purpose=SessionPurpose::ManualRestore;
    SessionRequest probe=active; probe.requestId=202; probe.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker.Request(manual)); CHECK(worker.Request(probe));
    std::unique_ptr<SessionResult> dropped=sink.waitFor(202);
    CHECK(dropped && dropped->status==SessionDataStatus::Superseded && dropped->purpose==SessionPurpose::MetadataProbe);
    blocker.release();
    std::unique_ptr<SessionResult> activeResult=sink.waitFor(200);
    std::unique_ptr<SessionResult> manualResult=sink.waitFor(201);
    CHECK(activeResult && activeResult->status==SessionDataStatus::Fresh);
    CHECK(manualResult && manualResult->status==SessionDataStatus::Fresh && manualResult->purpose==SessionPurpose::ManualRestore);
    worker.Stop();
}

struct OrderedBlockingSessionParse {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> order;
    bool released=false;
    bool parse(const AppProfile& profile,std::vector<WinFp>& output){
        std::unique_lock<std::mutex> lock(mutex);
        order.push_back(profile.id); changed.notify_all();
        if(order.size()==1) changed.wait(lock,[&]{ return released; });
        output.clear(); return true;
    }
    bool waitFirst(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(5),[&]{ return !order.empty(); });
    }
    void release(){ std::lock_guard<std::mutex> lock(mutex); released=true; changed.notify_all(); }
    std::vector<std::string> snapshot(){ std::lock_guard<std::mutex> lock(mutex); return order; }
};

static void test_session_worker_cross_app_manual_preempts_pending_metadata(){
    SessionResultSink sink; OrderedBlockingSessionParse parser;
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile& profile){ return U82W(profile.id)+L"-priority"; };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=1; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("ok",1,1); };
    ops.parse=[&](const AppProfile& profile,const std::string&,std::vector<WinFp>& output){ return parser.parse(profile,output); };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest active; active.requestId=210; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker.Request(active)); CHECK(parser.waitFirst());
    SessionRequest probe; probe.requestId=211; probe.app="chrome"; probe.profile=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    probe.purpose=SessionPurpose::MetadataProbe;
    SessionRequest manual; manual.requestId=212; manual.app="msedge"; manual.profile=sessionTestProfile("msedge",AppProfile::CHROMIUM);
    manual.purpose=SessionPurpose::ManualRestore;
    CHECK(worker.Request(probe)); CHECK(worker.Request(manual));
    parser.release();
    CHECK(sink.waitFor(210)!=nullptr); CHECK(sink.waitFor(211)!=nullptr); CHECK(sink.waitFor(212)!=nullptr);
    std::vector<std::string> order=parser.snapshot();
    CHECK(order.size()==3);
    CHECK(order.size()==3 && order[0]=="firefox" && order[1]=="msedge" && order[2]=="chrome");
    worker.Stop();
}

static void test_session_worker_rejects_unsupported_app_queue_amplification(){
    SessionResultSink sink; BlockingSessionParse blocker;
    SessionWorker worker((HWND)1,coalescingWorkerOps(sink,blocker));
    SessionRequest active; active.requestId=300; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker.Request(active)); CHECK(blocker.waitEntered());
    for(int i=0;i<100;++i){
        SessionRequest unsupported; unsupported.requestId=301+i; unsupported.app="other"+std::to_string(i);
        unsupported.profile=sessionTestProfile(unsupported.app); unsupported.purpose=SessionPurpose::MetadataProbe;
        CHECK(!worker.Request(unsupported));
    }
    CHECK(worker.PendingCount()<=3 && worker.ActiveCount()==1);
    blocker.release();
    CHECK(sink.waitFor(300)!=nullptr);
    worker.Stop();
}

static std::unique_ptr<SessionResult> coordinatorResult(uint64_t id,const std::string& app,
        SessionPurpose purpose,uint64_t generation,SessionDataStatus status){
    std::unique_ptr<SessionResult> result(new SessionResult());
    result->requestId=id; result->app=app; result->purpose=purpose;
    result->identityGeneration=generation; result->status=status;
    return result;
}

static void test_session_coordinator_preserves_purpose_and_shared_payload_identity(){
    SessionRequest captured;
    SessionCoordinator coordinator([&](const SessionRequest& request){ captured=request; return true; });
    AppProfile profile=sessionTestProfile("firefox");
    uint64_t requestId=coordinator.RequestSessionData(profile,41,SessionPurpose::ManualSave);
    CHECK(requestId!=0 && captured.requestId==requestId && captured.app=="firefox");
    CHECK(captured.purpose==SessionPurpose::ManualSave && captured.identityGeneration==41);
    CHECK(SessionProfilesEqual(captured.profile,profile));
    std::shared_ptr<const std::vector<WinFp> > payload(new std::vector<WinFp>(1));
    const std::vector<WinFp>* identity=payload.get();
    std::unique_ptr<SessionResult> result=coordinatorResult(requestId,"firefox",SessionPurpose::ManualSave,41,SessionDataStatus::Fresh);
    result->windows=payload;
    CHECK(coordinator.AcceptSessionResult(std::move(result),profile,41));
    const SessionResult* accepted=coordinator.Latest("firefox");
    CHECK(accepted && accepted->purpose==SessionPurpose::ManualSave && accepted->windows.get()==identity);
}

static void test_session_coordinator_rejects_old_generation_profile_purpose_and_request(){
    SessionCoordinator coordinator([](const SessionRequest&){ return true; });
    AppProfile profile=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    uint64_t oldId=coordinator.RequestSessionData(profile,5,SessionPurpose::Search);
    uint64_t latestId=coordinator.RequestSessionData(profile,5,SessionPurpose::Search);
    CHECK(!coordinator.AcceptSessionResult(coordinatorResult(oldId,"chrome",SessionPurpose::Search,5,SessionDataStatus::Fresh),profile,5));
    CHECK(coordinator.AcceptSessionResult(coordinatorResult(latestId,"chrome",SessionPurpose::Search,5,SessionDataStatus::Fresh),profile,5));

    uint64_t generationId=coordinator.RequestSessionData(profile,6,SessionPurpose::AutoReconcile);
    CHECK(!coordinator.AcceptSessionResult(coordinatorResult(generationId,"chrome",SessionPurpose::AutoReconcile,6,SessionDataStatus::Fresh),profile,7));
    uint64_t purposeId=coordinator.RequestSessionData(profile,7,SessionPurpose::HeartbeatSave);
    CHECK(!coordinator.AcceptSessionResult(coordinatorResult(purposeId,"chrome",SessionPurpose::ManualSave,7,SessionDataStatus::Fresh),profile,7));
    uint64_t profileId=coordinator.RequestSessionData(profile,7,SessionPurpose::ManualRestore);
    AppProfile changed=profile; changed.userDataDir=L"changed";
    CHECK(!coordinator.AcceptSessionResult(coordinatorResult(profileId,"chrome",SessionPurpose::ManualRestore,7,SessionDataStatus::Fresh),changed,7));
}

static void test_session_profile_comparison_covers_every_config_field(){
    AppProfile original=sessionTestProfile("firefox");
    CHECK(SessionProfilesEqual(original,original));
    AppProfile changed=original; changed.id="chrome"; CHECK(!SessionProfilesEqual(original,changed));
    changed=original; changed.classNames.push_back(L"OtherClass"); CHECK(!SessionProfilesEqual(original,changed));
    changed=original; changed.exeName=L"other.exe"; CHECK(!SessionProfilesEqual(original,changed));
    changed=original; changed.titleSuffixes.push_back(L" other"); CHECK(!SessionProfilesEqual(original,changed));
    changed=original; changed.session=AppProfile::CHROMIUM; CHECK(!SessionProfilesEqual(original,changed));
    changed=original; changed.userDataDir=L"other-data"; CHECK(!SessionProfilesEqual(original,changed));
}

static void test_manual_operation_profiles_remain_captured_across_settings_changes(){
    AppProfile original=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    original.classNames.push_back(L"CapturedClass");
    original.titleSuffixes.push_back(L" CapturedSuffix");
    original.userDataDir=L"captured-data";
    std::vector<AppProfile> active={original};
    OperationAppProfiles captured(active);

    active[0].classNames.clear();
    active[0].exeName=L"changed.exe";
    active[0].titleSuffixes.clear();
    active[0].session=AppProfile::NONE;
    active[0].userDataDir=L"changed-data";
    active.clear();

    const AppProfile* retained=captured.find("chrome");
    CHECK(retained && SessionProfilesEqual(*retained,original));
    CHECK(captured.find("firefox")==nullptr);
    CHECK(captured.all().size()==1);
}

static void test_session_coordinator_superseded_only_releases_bookkeeping(){
    SessionCoordinator coordinator([](const SessionRequest&){ return true; });
    AppProfile profile=sessionTestProfile("msedge",AppProfile::CHROMIUM);
    uint64_t id=coordinator.RequestSessionData(profile,9,SessionPurpose::ManualRestore);
    CHECK(coordinator.PendingCount()==1);
    CHECK(!coordinator.AcceptSessionResult(coordinatorResult(id,"msedge",SessionPurpose::ManualRestore,9,SessionDataStatus::Superseded),profile,9));
    CHECK(coordinator.PendingCount()==0 && coordinator.Latest("msedge")==nullptr);
}

static void test_session_coordinator_request_faults_are_transactional(){
    const SessionCoordinatorStep steps[]={
        SessionCoordinatorStep::RequestPrepare,
        SessionCoordinatorStep::PendingInsert,
        SessionCoordinatorStep::LatestRequestInsert
    };
    for(size_t index=0;index<sizeof(steps)/sizeof(steps[0]);++index){
        bool armed=false;
        size_t submits=0;
        SessionCoordinatorOps ops;
        ops.beforeStep=[&](SessionCoordinatorStep step){
            if(armed && step==steps[index]){
                if(index==2) throw std::runtime_error("coordinator runtime fault");
                throw std::bad_alloc();
            }
        };
        SessionCoordinator coordinator([&](const SessionRequest&){ ++submits; return true; },ops);
        AppProfile profile=sessionTestProfile("firefox");
        uint64_t first=coordinator.RequestSessionData(profile,30,SessionPurpose::Search);
        CHECK(first==1 && submits==1);
        std::unique_ptr<SessionResult> accepted=coordinatorResult(
            first,"firefox",SessionPurpose::Search,30,SessionDataStatus::Fresh);
        accepted->path=L"sentinel";
        CHECK(coordinator.AcceptSessionResult(std::move(accepted),profile,30));
        const SessionResult* prior=coordinator.Latest("firefox");
        CHECK(prior && prior->path==L"sentinel" && coordinator.PendingCount()==0);

        armed=true;
        bool threw=false;
        uint64_t failed=99;
        try { failed=coordinator.RequestSessionData(profile,31,SessionPurpose::ManualSave); }
        catch(...) { threw=true; }
        armed=false;
        CHECK(!threw && failed==0 && submits==1 && coordinator.PendingCount()==0);
        CHECK(coordinator.Latest("firefox")==prior && coordinator.Latest("firefox")->path==L"sentinel");
        uint64_t next=coordinator.RequestSessionData(profile,31,SessionPurpose::ManualSave);
        CHECK(next==2 && submits==2 && coordinator.PendingCount()==1);
    }

    size_t submits=0;
    bool reject=true,throwSubmit=false;
    SessionCoordinator coordinator([&](const SessionRequest&){
        ++submits;
        if(throwSubmit) throw std::length_error("submit fault");
        return !reject;
    });
    AppProfile profile=sessionTestProfile("chrome",AppProfile::CHROMIUM);
    CHECK(coordinator.RequestSessionData(profile,40,SessionPurpose::Search)==0);
    CHECK(coordinator.PendingCount()==0 && coordinator.Latest("chrome")==nullptr);
    reject=false; throwSubmit=true;
    bool threw=false; uint64_t failed=99;
    try { failed=coordinator.RequestSessionData(profile,40,SessionPurpose::Search); }
    catch(...) { threw=true; }
    CHECK(!threw && failed==0 && coordinator.PendingCount()==0);
    throwSubmit=false;
    CHECK(coordinator.RequestSessionData(profile,40,SessionPurpose::Search)==1);
    CHECK(submits==3 && coordinator.PendingCount()==1);
}

static void test_session_coordinator_accept_faults_preserve_pending_and_latest(){
    const SessionCoordinatorStep steps[]={
        SessionCoordinatorStep::AcceptPrepare,
        SessionCoordinatorStep::LatestResultInsert
    };
    for(size_t index=0;index<sizeof(steps)/sizeof(steps[0]);++index){
        bool armed=false;
        SessionCoordinatorOps ops;
        ops.beforeStep=[&](SessionCoordinatorStep step){
            if(armed && step==steps[index]){
                if(index) throw std::length_error("coordinator result fault");
                throw std::bad_alloc();
            }
        };
        SessionCoordinator coordinator([](const SessionRequest&){ return true; },ops);
        AppProfile firefox=sessionTestProfile("firefox");
        uint64_t oldId=coordinator.RequestSessionData(firefox,50,SessionPurpose::Search);
        std::unique_ptr<SessionResult> oldResult=coordinatorResult(
            oldId,"firefox",SessionPurpose::Search,50,SessionDataStatus::Fresh);
        oldResult->path=L"old";
        CHECK(coordinator.AcceptSessionResult(std::move(oldResult),firefox,50));
        const SessionResult* oldIdentity=coordinator.Latest("firefox");

        AppProfile chrome=sessionTestProfile("chrome",AppProfile::CHROMIUM);
        uint64_t id=coordinator.RequestSessionData(chrome,51,SessionPurpose::ManualRestore);
        CHECK(id!=0 && coordinator.PendingCount()==1);
        armed=true;
        bool threw=false,accepted=true;
        try {
            accepted=coordinator.AcceptSessionResult(
                coordinatorResult(id,"chrome",SessionPurpose::ManualRestore,51,SessionDataStatus::Fresh),chrome,51);
        } catch(...) { threw=true; }
        armed=false;
        CHECK(!threw && !accepted && coordinator.PendingCount()==1);
        CHECK(coordinator.Latest("firefox")==oldIdentity && coordinator.Latest("chrome")==nullptr);
        CHECK(coordinator.AcceptSessionResult(
            coordinatorResult(id,"chrome",SessionPurpose::ManualRestore,51,SessionDataStatus::Fresh),chrome,51));
        CHECK(coordinator.PendingCount()==0 && coordinator.Latest("chrome")!=nullptr);
    }
}

static void test_posted_session_result_is_owned_immediately_on_rejection(){
    SessionCoordinator coordinator([](const SessionRequest&){ return true; });
    AppProfile profile=sessionTestProfile("firefox");
    uint64_t id=coordinator.RequestSessionData(profile,12,SessionPurpose::Search);
    std::shared_ptr<const std::vector<WinFp> > payload(new std::vector<WinFp>(1));
    std::unique_ptr<SessionResult> result=coordinatorResult(id,"firefox",SessionPurpose::Search,11,SessionDataStatus::Fresh);
    result->windows=payload;
    SessionResult* raw=result.release();
    CHECK(payload.use_count()==2);
    CHECK(!AcceptPostedSessionResult(coordinator,(LPARAM)raw,profile,12));
    CHECK(payload.use_count()==1);
}

static void test_unavailable_defer_is_once_per_current_source_and_preserves_bytes(){
    SessionUnavailableDeferBudget budget;
    SessionResult unavailable;
    unavailable.app="firefox"; unavailable.path=L"one"; unavailable.status=SessionDataStatus::Unavailable;
    unavailable.sourceStampKnown=true; unavailable.sourceStamp.size=10; unavailable.sourceStamp.mtime=1;
    CHECK(budget.ShouldDefer(unavailable)); CHECK(!budget.ShouldDefer(unavailable));
    unavailable.sourceStamp.mtime=2;
    CHECK(budget.ShouldDefer(unavailable)); CHECK(!budget.ShouldDefer(unavailable));
    unavailable.status=SessionDataStatus::Superseded;
    CHECK(!budget.ShouldDefer(unavailable));
    const std::string original="serialized-layout-bytes";
    const std::string proposed="mutated";
    CHECK(SelectSerializedSessionRecords(SessionDataStatus::Unavailable,original,proposed)==original);
    CHECK(SelectSerializedSessionRecords(SessionDataStatus::Superseded,original,proposed)==original);
    CHECK(SelectSerializedSessionRecords(SessionDataStatus::Fresh,original,proposed)==proposed);
}

static void test_session_data_generation_is_per_app_and_hash_breaks_stamp_ties(){
    SessionResultSink sink;
    std::mutex contentMutex;
    std::string firefoxTitle="A";
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile& profile){ return U82W(profile.id)+L"-path"; };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=7; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring& path){ return successfulSessionRead(W2U8(path),7,1); };
    ops.parse=[&](const AppProfile& profile,const std::string&,std::vector<WinFp>& output){
        output.clear(); WinFp fp;
        { std::lock_guard<std::mutex> lock(contentMutex); fp.activeTitle=profile.id=="firefox"?firefoxTitle:"C"; }
        output.push_back(fp); return true;
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops,1,1024*1024);
    SessionRequest firefox; firefox.requestId=400; firefox.app="firefox"; firefox.profile=sessionTestProfile("firefox"); firefox.purpose=SessionPurpose::MetadataProbe;
    SessionRequest chrome; chrome.requestId=401; chrome.app="chrome"; chrome.profile=sessionTestProfile("chrome",AppProfile::CHROMIUM); chrome.purpose=SessionPurpose::AutoReconcile;
    CHECK(worker.Request(firefox)); std::unique_ptr<SessionResult> ffA=sink.waitFor(400);
    CHECK(ffA && ffA->dataGeneration==1); ffA.reset();
    CHECK(worker.Request(chrome)); std::unique_ptr<SessionResult> cr=sink.waitFor(401);
    CHECK(cr && cr->dataGeneration==1); cr.reset();

    firefox.requestId=402;
    CHECK(worker.Request(firefox)); std::unique_ptr<SessionResult> ffSame=sink.waitFor(402);
    CHECK(ffSame && ffSame->dataGeneration==1); ffSame.reset();
    chrome.requestId=403;
    CHECK(worker.Request(chrome)); cr=sink.waitFor(403); CHECK(cr && cr->dataGeneration==1); cr.reset();

    { std::lock_guard<std::mutex> lock(contentMutex); firefoxTitle="B"; }
    firefox.requestId=404;
    CHECK(worker.Request(firefox)); std::unique_ptr<SessionResult> ffChanged=sink.waitFor(404);
    CHECK(ffChanged && ffChanged->dataGeneration==2 && ffChanged->windows->at(0).activeTitle=="B");
    SessionStamp stableSource; stableSource.size=7; stableSource.mtime=1;
    CHECK(ffChanged && ffChanged->sourceStampKnown && ffChanged->sourceStamp==stableSource && ffChanged->dataStamp==stableSource);
    const std::vector<WinFp>* changedIdentity=ffChanged?ffChanged->windows.get():nullptr;
    firefox.requestId=405;
    CHECK(worker.Request(firefox)); std::unique_ptr<SessionResult> ffHit=sink.waitFor(405);
    CHECK(ffHit && ffHit->dataGeneration==2 && ffHit->windows.get()==changedIdentity);
    int refreshWaves=0;
    uint64_t acceptedGeneration=1;
    if(ffChanged && ffChanged->dataGeneration!=acceptedGeneration){ ++refreshWaves; acceptedGeneration=ffChanged->dataGeneration; }
    if(ffHit && ffHit->dataGeneration!=acceptedGeneration) ++refreshWaves;
    CHECK(refreshWaves==1);
    worker.Stop();
}

static void test_session_data_generation_is_monotonic_when_historical_cache_returns(){
    SessionResultSink sink;
    std::atomic<unsigned long long> mtime(1);
    std::atomic<bool> parseOk(true);
    std::atomic<int> reads(0),parses(0);
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"generation-history"); };
    ops.getStamp=[&](const std::wstring&,SessionStamp& stamp){ stamp.size=1; stamp.mtime=mtime.load(); return true; };
    ops.readFile=[&](const std::wstring&){
        ++reads; unsigned long long current=mtime.load();
        return successfulSessionRead("S"+std::to_string(current),1,current);
    };
    ops.parse=[&](const AppProfile&,const std::string& bytes,std::vector<WinFp>& output){
        ++parses; output.clear();
        if(!parseOk.load()) return false;
        WinFp window; window.activeTitle=bytes; output.push_back(std::move(window)); return true;
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops,4,1024*1024);
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");

    request.requestId=610; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> s1=sink.waitFor(610);
    CHECK(s1 && s1->status==SessionDataStatus::Fresh && s1->dataGeneration==1);
    const std::vector<WinFp>* s1Identity=s1?s1->windows.get():nullptr;

    mtime=2; request.requestId=611; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> s2=sink.waitFor(611);
    CHECK(s2 && s2->status==SessionDataStatus::Fresh && s2->dataGeneration==2);
    const std::vector<WinFp>* s2Identity=s2?s2->windows.get():nullptr;

    mtime=1; request.requestId=612; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> historicalFresh=sink.waitFor(612);
    CHECK(historicalFresh && historicalFresh->status==SessionDataStatus::Fresh &&
          historicalFresh->windows.get()==s1Identity && historicalFresh->dataGeneration==3);
    request.requestId=613; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> repeatedFresh=sink.waitFor(613);
    CHECK(repeatedFresh && repeatedFresh->windows.get()==s1Identity && repeatedFresh->dataGeneration==3);
    CHECK(reads.load()==2 && parses.load()==2);

    mtime=3; parseOk=false; request.requestId=614; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> historicalStale=sink.waitFor(614);
    SessionStamp s2Stamp; s2Stamp.size=1; s2Stamp.mtime=2;
    CHECK(historicalStale && historicalStale->status==SessionDataStatus::CachedStale &&
          historicalStale->windows.get()==s2Identity && historicalStale->dataStamp==s2Stamp &&
          historicalStale->dataGeneration==4);
    request.requestId=615; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> repeatedStale=sink.waitFor(615);
    CHECK(repeatedStale && repeatedStale->status==SessionDataStatus::CachedStale &&
          repeatedStale->windows.get()==s2Identity && repeatedStale->dataGeneration==4);
    CHECK(reads.load()==4 && parses.load()==4);
    worker.Stop();
}

static void test_session_data_generation_saturates_without_zero_or_rollback(){
    CHECK(NextSessionDataGeneration(0)==1);
    CHECK(NextSessionDataGeneration((std::numeric_limits<uint64_t>::max)()-1)==
          (std::numeric_limits<uint64_t>::max)());
    CHECK(NextSessionDataGeneration((std::numeric_limits<uint64_t>::max)())==
          (std::numeric_limits<uint64_t>::max)());
}

static void test_session_cache_enforces_sixteen_entry_lru_cap(){
    SessionCache cache(16,1024*1024);
    for(int i=0;i<17;++i){
        std::vector<WinFp> windows(1); windows[0].activeTitle=std::to_string(i);
        SessionStamp stamp; stamp.size=1; stamp.mtime=(unsigned long long)i;
        SessionCacheValue stored;
        CHECK(cache.Put("firefox",L"path"+std::to_wstring(i),stamp,std::move(windows),(uint64_t)i,(uint64_t)i+1,stored));
        CHECK(cache.EntryCount()<=16);
        stored.windows.reset();
    }
    SessionCacheValue value;
    SessionStamp first; first.size=1; first.mtime=0;
    SessionStamp last; last.size=1; last.mtime=16;
    CHECK(!cache.FindExact("firefox",L"path0",first,value));
    CHECK(cache.FindExact("firefox",L"path16",last,value));
    CHECK(cache.EntryCount()==16);
}

static void test_session_cache_byte_cap_counts_external_ui_payload(){
    std::vector<WinFp> sample(1); sample[0].tabsBlob=std::string(200,'x');
    size_t bytes=EstimateSessionPayloadBytes(sample);
    SessionStamp stamp; stamp.size=1; stamp.mtime=1;
    SessionCache tooSmall(16,bytes-1);
    SessionCacheValue rejected;
    CHECK(!tooSmall.Put("firefox",L"oversized",stamp,std::move(sample),1,1,rejected));
    CHECK(tooSmall.EntryCount()==0 && tooSmall.RetainedBytes()==0 && !rejected.windows);

    std::vector<WinFp> first(1); first[0].tabsBlob=std::string(200,'a');
    std::vector<WinFp> second(1); second[0].tabsBlob=std::string(200,'b');
    CHECK(EstimateSessionPayloadBytes(first)==bytes && EstimateSessionPayloadBytes(second)==bytes);
    SessionCache exact(16,bytes);
    SessionCacheValue uiOwned;
    CHECK(exact.Put("firefox",L"one",stamp,std::move(first),1,1,uiOwned));
    CHECK(exact.RetainedBytes()==bytes);
    SessionCacheValue cannotFit;
    CHECK(!exact.Put("chrome",L"two",stamp,std::move(second),2,1,cannotFit));
    CHECK(exact.RetainedBytes()==bytes && !cannotFit.windows);
    uiOwned.windows.reset();
    CHECK(exact.RetainedBytes()==bytes); // cache still owns the preserved LRU entry
    CHECK(exact.Put("chrome",L"two",stamp,std::move(second),2,1,cannotFit));
    CHECK(exact.RetainedBytes()==bytes);
}

static void test_post_message_failure_deletes_heap_result(){
    SessionWorkerOps ops;
    std::atomic<int> posts(0);
    ops.postMessage=[&](HWND,UINT,WPARAM,LPARAM){ ++posts; return false; };
    FillMissingSessionWorkerOps(ops);
    std::shared_ptr<const std::vector<WinFp> > payload(new std::vector<WinFp>(1));
    std::unique_ptr<SessionResult> result(new SessionResult()); result->windows=payload;
    CHECK(payload.use_count()==2);
    CHECK(!PostSessionResultOwned(ops,(HWND)1,std::move(result)));
    CHECK(posts.load()==1 && payload.use_count()==1);
}

static void test_session_worker_oversized_payload_is_unavailable(){
    SessionResultSink sink;
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"large"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=5; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("valid",5,1); };
    ops.parse=[](const AppProfile&,const std::string&,std::vector<WinFp>& output){ output.clear(); WinFp fp; fp.tabsBlob=std::string(4096,'x'); output.push_back(std::move(fp)); return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops,16,128);
    SessionRequest request; request.requestId=410; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> result=sink.waitFor(410);
    CHECK(result && result->status==SessionDataStatus::Unavailable && !result->windows);
    CHECK(worker.CacheEntryCount()==0 && worker.RetainedBytes()==0);
    worker.Stop();
}

static void test_session_worker_stop_joins_and_suppresses_unposted_completion(){
    SessionResultSink sink; BlockingSessionParse blocker;
    std::atomic<int> posts(0);
    SessionWorkerOps ops=coalescingWorkerOps(sink,blocker);
    ops.postMessage=[&](HWND,UINT,WPARAM,LPARAM){ ++posts; return false; };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=420; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.purpose=SessionPurpose::ManualSave;
    CHECK(worker.Request(request)); CHECK(blocker.waitEntered());
    std::atomic<bool> stopped(false);
    std::thread stopper([&]{ worker.Stop(); stopped=true; });
    bool rejecting=false;
    for(int i=0;i<10000 && !rejecting;++i){ SessionRequest late=request; late.requestId=421; rejecting=!worker.Request(late); if(!rejecting) std::this_thread::yield(); }
    CHECK(rejecting); CHECK(!stopped.load());
    int postsWhenStopping=posts.load();
    blocker.release();
    stopper.join();
    CHECK(stopped.load() && posts.load()==postsWhenStopping);
    CHECK(worker.PendingCount()==0 && worker.ActiveCount()==0 && worker.RetainedBytes()==0);
    CHECK(!worker.Request(request));
}

struct BlockingSupersededPoster {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false,released=false;
    bool post(HWND,UINT,WPARAM,LPARAM value){
        std::unique_ptr<SessionResult> result((SessionResult*)value);
        if(result && result->requestId==451 && result->status==SessionDataStatus::Superseded){
            std::unique_lock<std::mutex> lock(mutex);
            entered=true; changed.notify_all();
            changed.wait(lock,[&]{ return released; });
        }
        return true;
    }
    bool waitEntered(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(5),[&]{ return entered; });
    }
    void release(){ std::lock_guard<std::mutex> lock(mutex); released=true; changed.notify_all(); }
};

static void test_session_worker_stop_waits_for_inflight_superseded_post(){
    SessionResultSink unusedSink; BlockingSessionParse parser; BlockingSupersededPoster poster;
    SessionWorkerOps ops=coalescingWorkerOps(unusedSink,parser);
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return poster.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest active; active.requestId=450; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker.Request(active)); CHECK(parser.waitEntered());
    SessionRequest queued=active; queued.requestId=451; queued.purpose=SessionPurpose::ManualSave;
    SessionRequest newest=queued; newest.requestId=452; newest.purpose=SessionPurpose::ManualRestore;
    CHECK(worker.Request(queued));
    std::atomic<bool> replacementReturned(false),replacementAccepted(false);
    std::thread replacer([&]{ replacementAccepted=worker.Request(newest); replacementReturned=true; });
    CHECK(poster.waitEntered());
    std::mutex stopMutex; std::condition_variable stopChanged; bool stopped=false;
    std::thread stopper([&]{ worker.Stop(); { std::lock_guard<std::mutex> lock(stopMutex); stopped=true; } stopChanged.notify_all(); });
    for(int i=0;i<10000 && worker.PendingCount()!=0;++i) std::this_thread::yield();
    CHECK(worker.PendingCount()==0);
    SessionRequest late=active; late.requestId=453;
    CHECK(!worker.Request(late));
    parser.release();
    bool returnedWhilePostBlocked=false;
    {
        std::unique_lock<std::mutex> lock(stopMutex);
        returnedWhilePostBlocked=stopChanged.wait_for(lock,std::chrono::milliseconds(500),[&]{ return stopped; });
    }
    CHECK(!returnedWhilePostBlocked);
    poster.release(); replacer.join(); stopper.join();
    CHECK(replacementAccepted.load() && replacementReturned.load() && stopped);
}

struct ReentrantStopState {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false,returned=false;
    SessionWorker* worker=nullptr;
    std::shared_ptr<BlockingSessionParse> parser;
    void markEntered(){ std::lock_guard<std::mutex> lock(mutex); entered=true; changed.notify_all(); }
    void markReturned(){ std::lock_guard<std::mutex> lock(mutex); returned=true; changed.notify_all(); }
    bool waitEntered(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(5),[&]{ return entered; });
    }
    bool waitReturned(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::milliseconds(500),[&]{ return returned; });
    }
};

static SessionWorkerOps reentrantStopWorkerOps(const std::shared_ptr<ReentrantStopState>& state,
                                               bool blockParser,bool stopOnlySuperseded){
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"reentrant"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=1; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("ok",1,1); };
    ops.parse=[state,blockParser](const AppProfile& profile,const std::string& bytes,std::vector<WinFp>& output){
        if(blockParser) return state->parser->parse(profile,bytes,output);
        output.clear(); return true;
    };
    ops.postMessage=[state,stopOnlySuperseded](HWND,UINT,WPARAM,LPARAM value){
        SessionResult* result=(SessionResult*)value;
        if(!stopOnlySuperseded || (result && result->status==SessionDataStatus::Superseded)){
            if(state->parser) state->parser->release();
            state->markEntered();
            state->worker->Stop();
            state->markReturned();
        }
        delete result; // ownership transfers only if the callback returns successfully
        return true;
    };
    return ops;
}

static void test_session_worker_reentrant_requester_poster_stop_completes(){
    std::shared_ptr<ReentrantStopState> state(new ReentrantStopState());
    state->parser.reset(new BlockingSessionParse());
    SessionWorker* worker=new SessionWorker((HWND)1,reentrantStopWorkerOps(state,true,true));
    state->worker=worker;
    SessionRequest active; active.requestId=460; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker->Request(active)); CHECK(state->parser->waitEntered());
    SessionRequest queued=active; queued.requestId=461; queued.purpose=SessionPurpose::ManualSave;
    SessionRequest newest=queued; newest.requestId=462; newest.purpose=SessionPurpose::ManualRestore;
    CHECK(worker->Request(queued));
    std::thread replacer([worker,newest]{ worker->Request(newest); });
    CHECK(state->waitEntered());
    bool completed=state->waitReturned();
    CHECK(completed);
    if(completed){ replacer.join(); worker->Stop(); delete worker; }
    else replacer.detach(); // bounded RED: the isolated heap state lives until process exit
}

static void test_session_worker_reentrant_worker_poster_stop_defers_self_join(){
    std::shared_ptr<ReentrantStopState> state(new ReentrantStopState());
    SessionWorker* worker=new SessionWorker((HWND)1,reentrantStopWorkerOps(state,false,false));
    state->worker=worker;
    SessionRequest request; request.requestId=470; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.purpose=SessionPurpose::ManualRestore;
    CHECK(worker->Request(request)); CHECK(state->waitEntered());
    bool completed=state->waitReturned();
    CHECK(completed);
    if(completed){ worker->Stop(); delete worker; }
    // On RED the worker has exited after the caught self-join exception; keep it isolated.
}

struct DeterministicPostStopInterleave {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread::id requesterThread;
    bool callbackEntered=false;
    bool workerPostEntered=false;
    bool releaseWorkerPost=false;
    bool allowCallbackStop=false;
    bool callbackStopEntered=false;
    bool callbackJoinWaitEntered=false;
    bool releaseCallbackJoinWait=false;
    bool callbackReturned=false;
    bool callbackStopResult=true;
    bool waitForExternalStop=false;
    SessionWorker* worker=nullptr;
    std::shared_ptr<BlockingSessionParse> parser;

    void setRequesterThread(){
        std::lock_guard<std::mutex> lock(mutex);
        requesterThread=std::this_thread::get_id();
    }
    bool beforePost(){
        std::unique_lock<std::mutex> lock(mutex);
        if(callbackEntered && std::this_thread::get_id()!=requesterThread){
            workerPostEntered=true;
            changed.notify_all();
            changed.wait(lock,[&]{ return releaseWorkerPost; });
            return false; // deterministic RED cleanup: never wait on the held legacy fence
        }
        return true;
    }
    bool beforeJoinWait(){
        std::unique_lock<std::mutex> lock(mutex);
        if(waitForExternalStop && std::this_thread::get_id()==requesterThread){
            callbackJoinWaitEntered=true;
            changed.notify_all();
            changed.wait(lock,[&]{ return releaseCallbackJoinWait; });
            return false;
        }
        return true;
    }
    bool post(HWND,UINT,WPARAM,LPARAM value){
        std::unique_ptr<SessionResult> result((SessionResult*)value);
        if(result && result->requestId==481 && result->status==SessionDataStatus::Superseded){
            {
                std::lock_guard<std::mutex> lock(mutex);
                callbackEntered=true;
                changed.notify_all();
            }
            parser->release();
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait_for(lock,std::chrono::seconds(5),[&]{ return workerPostEntered; });
                if(waitForExternalStop)
                    changed.wait_for(lock,std::chrono::seconds(5),[&]{ return allowCallbackStop; });
                callbackStopEntered=true;
                changed.notify_all();
            }
            bool stopResult=worker->Stop();
            {
                std::lock_guard<std::mutex> lock(mutex);
                callbackStopResult=stopResult;
                callbackReturned=true;
                changed.notify_all();
            }
        }
        return true;
    }
    bool waitFlag(bool DeterministicPostStopInterleave::*member,int milliseconds=5000){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::milliseconds(milliseconds),[&]{ return this->*member; });
    }
    void allowStop(){ std::lock_guard<std::mutex> lock(mutex); allowCallbackStop=true; changed.notify_all(); }
    void releasePost(){ std::lock_guard<std::mutex> lock(mutex); releaseWorkerPost=true; changed.notify_all(); }
    void releaseJoinWait(){ std::lock_guard<std::mutex> lock(mutex); releaseCallbackJoinWait=true; changed.notify_all(); }
};

static SessionWorkerOps deterministicPostStopOps(
        const std::shared_ptr<DeterministicPostStopInterleave>& state){
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"deterministic-stop"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=1; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("ok",1,1); };
    ops.parse=[state](const AppProfile& profile,const std::string& bytes,std::vector<WinFp>& output){
        return state->parser->parse(profile,bytes,output);
    };
    ops.beforePost=[state]{ return state->beforePost(); };
    ops.beforeJoinWait=[state]{ return state->beforeJoinWait(); };
    ops.postMessage=[state](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return state->post(hwnd,message,wp,lp); };
    return ops;
}

static SessionWorker* startDeterministicPostStop(
        const std::shared_ptr<DeterministicPostStopInterleave>& state,std::thread& requester){
    SessionWorker* worker=new SessionWorker((HWND)1,deterministicPostStopOps(state));
    state->worker=worker;
    SessionRequest active; active.requestId=480; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker->Request(active)); CHECK(state->parser->waitEntered());
    SessionRequest queued=active; queued.requestId=481; queued.purpose=SessionPurpose::ManualSave;
    SessionRequest newest=queued; newest.requestId=482; newest.purpose=SessionPurpose::ManualRestore;
    CHECK(worker->Request(queued));
    requester=std::thread([state,worker,newest]{ state->setRequesterThread(); worker->Request(newest); });
    return worker;
}

static void test_session_worker_reentrant_stop_waits_for_confirmed_worker_post_path(){
    std::shared_ptr<DeterministicPostStopInterleave> state(new DeterministicPostStopInterleave());
    state->parser.reset(new BlockingSessionParse());
    std::thread requester;
    SessionWorker* worker=startDeterministicPostStop(state,requester);
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::callbackEntered));
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::workerPostEntered));
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::callbackStopEntered));
    bool returned=state->waitFlag(&DeterministicPostStopInterleave::callbackReturned,750);
    CHECK(returned);
    state->releasePost();
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::callbackReturned));
    CHECK(!state->callbackStopResult);
    requester.join();
    CHECK(worker->Stop());
    delete worker;
}

static void test_session_worker_concurrent_external_and_reentrant_stop_do_not_cycle(){
    std::shared_ptr<DeterministicPostStopInterleave> state(new DeterministicPostStopInterleave());
    state->parser.reset(new BlockingSessionParse());
    state->waitForExternalStop=true;
    std::thread requester;
    SessionWorker* worker=startDeterministicPostStop(state,requester);
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::callbackEntered));
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::workerPostEntered));
    std::mutex stoppedMutex; std::condition_variable stoppedChanged; bool externalReturned=false;
    std::thread external([&]{ worker->Stop(); { std::lock_guard<std::mutex> lock(stoppedMutex); externalReturned=true; } stoppedChanged.notify_all(); });
    for(int i=0;i<10000 && worker->PendingCount()!=0;++i) std::this_thread::yield();
    CHECK(worker->PendingCount()==0); // external Stop has closed the gate and cleared newest pending
    state->allowStop();
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::callbackStopEntered));
    bool callbackReturned=state->waitFlag(&DeterministicPostStopInterleave::callbackReturned,750);
    bool stopReturnedBeforeRelease=false;
    {
        std::unique_lock<std::mutex> lock(stoppedMutex);
        stopReturnedBeforeRelease=stoppedChanged.wait_for(lock,std::chrono::milliseconds(100),[&]{ return externalReturned; });
    }
    CHECK(callbackReturned); CHECK(!stopReturnedBeforeRelease);
    state->releasePost();
    state->releaseJoinWait();
    bool stopReturned=false;
    {
        std::unique_lock<std::mutex> lock(stoppedMutex);
        stopReturned=stoppedChanged.wait_for(lock,std::chrono::seconds(5),[&]{ return externalReturned; });
    }
    CHECK(stopReturned);
    CHECK(state->waitFlag(&DeterministicPostStopInterleave::callbackReturned));
    CHECK(!state->callbackStopResult);
    requester.join(); external.join(); delete worker;
}

static void test_session_worker_reentrant_worker_stop_survives_repeated_destruction(){
    for(int attempt=0;attempt<16;++attempt){
        std::shared_ptr<ReentrantStopState> state(new ReentrantStopState());
        SessionWorker* worker=new SessionWorker((HWND)1,reentrantStopWorkerOps(state,false,false));
        state->worker=worker;
        SessionRequest request; request.requestId=490+(uint64_t)attempt; request.app="firefox";
        request.profile=sessionTestProfile("firefox"); request.purpose=SessionPurpose::ManualRestore;
        CHECK(worker->Request(request)); CHECK(state->waitEntered()); CHECK(state->waitReturned());
        delete worker;
    }
}

static void test_worker_retained_budget_includes_posted_ui_ownership(){
    SessionResultSink sink;
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"owned"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=5; stamp.mtime=1; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("valid",5,1); };
    ops.parse=[](const AppProfile&,const std::string&,std::vector<WinFp>& output){ output.clear(); output.push_back(WinFp{}); return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.requestId=430; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> accepted=sink.waitFor(430);
    CHECK(accepted && accepted->windows && worker.RetainedBytes()>0);
    worker.Stop();
    CHECK(worker.RetainedBytes()>0);
    accepted.reset();
    CHECK(worker.RetainedBytes()==0);
}

static void test_failed_cache_replacement_preserves_exact_stale_payload(){
    SessionResultSink sink;
    std::atomic<unsigned long long> mtime(1);
    std::atomic<char> title('A');
    std::vector<WinFp> sizing(1); sizing[0].activeTitle=std::string(200,'A');
    size_t onePayload=EstimateSessionPayloadBytes(sizing);
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"budgeted"); };
    ops.getStamp=[&](const std::wstring&,SessionStamp& stamp){ stamp.size=5; stamp.mtime=mtime.load(); return true; };
    ops.readFile=[&](const std::wstring&){ return successfulSessionRead("valid",5,mtime.load()); };
    ops.parse=[&](const AppProfile&,const std::string&,std::vector<WinFp>& output){ output.clear(); WinFp fp; fp.activeTitle=std::string(200,title.load()); output.push_back(std::move(fp)); return true; };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    SessionWorker worker((HWND)1,ops,16,onePayload);
    SessionRequest request; request.requestId=440; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> oldUi=sink.waitFor(440);
    CHECK(oldUi && oldUi->status==SessionDataStatus::Fresh);
    const std::vector<WinFp>* oldIdentity=oldUi?oldUi->windows.get():nullptr;
    mtime=2; title='B'; request.requestId=441;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> stale=sink.waitFor(441);
    SessionStamp data; data.size=5; data.mtime=1;
    SessionStamp source; source.size=5; source.mtime=2;
    CHECK(stale && stale->status==SessionDataStatus::CachedStale && stale->windows.get()==oldIdentity);
    CHECK(stale && stale->dataStamp==data && stale->sourceStampKnown && stale->sourceStamp==source);
    CHECK(worker.CacheEntryCount()==1 && worker.RetainedBytes()==onePayload);
    worker.Stop();
}

static void test_session_cache_put_is_strongly_transactional_at_every_fault_step(){
    const SessionCachePutStep steps[]={
        SessionCachePutStep::PayloadStorage,
        SessionCachePutStep::PayloadControl,
        SessionCachePutStep::KeyCopy,
        SessionCachePutStep::PathCopy,
        SessionCachePutStep::StampCopy,
        SessionCachePutStep::OutputCopy,
        SessionCachePutStep::ContainerReserve,
        SessionCachePutStep::DuplicateReplacement,
        SessionCachePutStep::ContainerInsert,
        SessionCachePutStep::OrderPublish
    };
    for(size_t stepIndex=0;stepIndex<sizeof(steps)/sizeof(steps[0]);++stepIndex){
        bool enabled=false;
        SessionCacheOps ops;
        ops.beforePutStep=[&](SessionCachePutStep step){
            if(enabled && step==steps[stepIndex]){
                if((stepIndex&1)==0) throw std::bad_alloc();
                throw std::length_error("session cache injected fault");
            }
        };
        SessionCache cache(2,1024*1024,ops);
        SessionStamp stampA; stampA.size=1; stampA.mtime=1;
        SessionStamp stampB; stampB.size=2; stampB.mtime=2;
        SessionStamp stampC; stampC.size=3; stampC.mtime=3;
        std::vector<WinFp> first(1); first[0].activeTitle="A";
        std::vector<WinFp> second(1); second[0].activeTitle="B";
        SessionCacheValue storedA,storedB;
        CHECK(cache.Put("firefox",L"A",stampA,std::move(first),11,1,storedA));
        CHECK(cache.Put("firefox",L"B",stampB,std::move(second),22,2,storedB));
        const std::vector<WinFp>* identityA=storedA.windows.get();
        const std::vector<WinFp>* identityB=storedB.windows.get();
        SessionCacheValue touch;
        CHECK(cache.FindExact("firefox",L"A",stampA,touch)); // B is the LRU victim
        touch.windows.reset();
        const size_t entriesBefore=cache.EntryCount(),bytesBefore=cache.RetainedBytes();

        const bool duplicate=steps[stepIndex]==SessionCachePutStep::DuplicateReplacement;
        std::vector<WinFp> candidate(1); candidate[0].activeTitle=duplicate?"A2":"C";
        SessionCacheValue failed;
        failed.path=L"sentinel-output";
        failed.stamp.size=91; failed.stamp.mtime=92;
        failed.contentHash=93; failed.dataGeneration=94; failed.retainedBytes=95;
        std::shared_ptr<std::vector<WinFp> > sentinelPayload(new std::vector<WinFp>(1));
        sentinelPayload->at(0).activeTitle="sentinel";
        failed.windows=sentinelPayload;
        const std::vector<WinFp>* sentinelIdentity=failed.windows.get();
        enabled=true;
        CHECK(!cache.Put("firefox",duplicate?L"A":L"C",duplicate?stampA:stampC,
                         std::move(candidate),33,3,failed));
        enabled=false;
        CHECK(failed.path==L"sentinel-output" && failed.stamp.size==91 && failed.stamp.mtime==92 &&
              failed.contentHash==93 && failed.dataGeneration==94 && failed.retainedBytes==95 &&
              failed.windows.get()==sentinelIdentity && failed.windows->at(0).activeTitle=="sentinel");
        CHECK(cache.EntryCount()==entriesBefore && cache.RetainedBytes()==bytesBefore);
        SessionCacheValue afterB,afterA;
        CHECK(cache.FindExact("firefox",L"B",stampB,afterB));
        CHECK(afterB.windows.get()==identityB && afterB.windows->at(0).activeTitle=="B");
        CHECK(cache.FindExact("firefox",L"A",stampA,afterA));
        CHECK(afterA.windows.get()==identityA && afterA.windows->at(0).activeTitle=="A");

        std::vector<WinFp> retry(1); retry[0].activeTitle=duplicate?"A2":"C";
        SessionCacheValue inserted;
        CHECK(cache.Put("firefox",duplicate?L"A":L"C",duplicate?stampA:stampC,
                        std::move(retry),33,3,inserted));
        CHECK(inserted.windows && inserted.windows.get()!=identityA && inserted.windows.get()!=identityB);
        if(!duplicate){
            SessionCacheValue evicted;
            CHECK(!cache.FindExact("firefox",L"B",stampB,evicted));
            CHECK(cache.FindExact("firefox",L"A",stampA,evicted) && evicted.windows.get()==identityA);
        }
    }
}

static SessionCacheValue sessionCacheOutputSentinel(){
    SessionCacheValue value;
    value.path=L"preserve-me"; value.stamp.size=71; value.stamp.mtime=72;
    value.contentHash=73; value.dataGeneration=74; value.retainedBytes=75;
    std::shared_ptr<std::vector<WinFp> > payload(new std::vector<WinFp>(1));
    payload->at(0).activeTitle="preserve-me";
    value.windows=payload;
    return value;
}

static bool isSessionCacheOutputSentinel(const SessionCacheValue& value,
                                         const std::vector<WinFp>* identity){
    return value.path==L"preserve-me" && value.stamp.size==71 && value.stamp.mtime==72 &&
           value.contentHash==73 && value.dataGeneration==74 && value.retainedBytes==75 &&
           value.windows.get()==identity && value.windows && value.windows->at(0).activeTitle=="preserve-me";
}

static void test_session_cache_put_preserves_output_on_capacity_and_budget_rejection(){
    SessionStamp stamp; stamp.size=1; stamp.mtime=1;
    SessionCacheValue output=sessionCacheOutputSentinel();
    const std::vector<WinFp>* identity=output.windows.get();
    std::vector<WinFp> rejected(1); rejected[0].activeTitle="rejected";
    SessionCache noEntries(0,1024*1024);
    CHECK(!noEntries.Put("firefox",L"none",stamp,std::move(rejected),1,1,output));
    CHECK(isSessionCacheOutputSentinel(output,identity));
    CHECK(noEntries.EntryCount()==0 && noEntries.RetainedBytes()==0);

    output=sessionCacheOutputSentinel(); identity=output.windows.get();
    std::vector<WinFp> tooLarge(1); tooLarge[0].tabsBlob=std::string(512,'x');
    SessionCache noBytes(1,EstimateSessionPayloadBytes(tooLarge)-1);
    CHECK(!noBytes.Put("firefox",L"large",stamp,std::move(tooLarge),2,1,output));
    CHECK(isSessionCacheOutputSentinel(output,identity));
    CHECK(noBytes.EntryCount()==0 && noBytes.RetainedBytes()==0);

    std::vector<WinFp> first(1); first[0].tabsBlob=std::string(256,'a');
    std::vector<WinFp> second(1); second[0].tabsBlob=std::string(256,'b');
    size_t onePayload=EstimateSessionPayloadBytes(first);
    CHECK(EstimateSessionPayloadBytes(second)==onePayload);
    SessionCache exact(1,onePayload);
    SessionCacheValue uiHeld;
    CHECK(exact.Put("firefox",L"A",stamp,std::move(first),11,1,uiHeld));
    const std::vector<WinFp>* heldIdentity=uiHeld.windows.get();
    CHECK(!exact.Put("chrome",L"B",stamp,std::move(second),22,2,uiHeld));
    CHECK(uiHeld.path==L"A" && uiHeld.windows.get()==heldIdentity &&
          uiHeld.windows->at(0).tabsBlob==std::string(256,'a'));
    CHECK(exact.EntryCount()==1 && exact.RetainedBytes()==onePayload);
    SessionCacheValue stillA;
    CHECK(exact.FindExact("firefox",L"A",stamp,stillA) && stillA.windows.get()==heldIdentity);
}

static bool waitSessionWorkerIdle(SessionWorker& worker,const std::string& app){
    std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline){
        if(worker.OutstandingForApp(app)==0 && worker.ActiveCount()==0) return true;
        std::this_thread::yield();
    }
    return worker.OutstandingForApp(app)==0 && worker.ActiveCount()==0;
}

static SessionWorkerOps exceptionWorkerOps(SessionResultSink& sink,
        const std::function<void(SessionWorkerStep)>& beforeStep=std::function<void(SessionWorkerStep)>()){
    SessionWorkerOps ops;
    ops.resolvePath=[](const AppProfile&){ return std::wstring(L"exception-path"); };
    ops.getStamp=[](const std::wstring&,SessionStamp& stamp){ stamp.size=2; stamp.mtime=3; return true; };
    ops.readFile=[](const std::wstring&){ return successfulSessionRead("ok",2,3); };
    ops.parse=[](const AppProfile&,const std::string&,std::vector<WinFp>& output){
        output.clear(); WinFp window; window.activeTitle="ok"; output.push_back(std::move(window)); return true;
    };
    ops.beforeWorkerStep=beforeStep;
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){ return sink.post(hwnd,message,wp,lp); };
    return ops;
}

static void test_session_worker_contains_internal_allocation_faults_and_continues(){
    const SessionWorkerStep steps[]={
        SessionWorkerStep::AfterChoose,
        SessionWorkerStep::ActiveCopy,
        SessionWorkerStep::CacheLookup,
        SessionWorkerStep::GenerationPrepare,
        SessionWorkerStep::GenerationPublish
    };
    for(size_t index=0;index<sizeof(steps)/sizeof(steps[0]);++index){
        SessionResultSink sink;
        std::atomic<bool> armed(true);
        SessionWorkerOps ops=exceptionWorkerOps(sink,[&](SessionWorkerStep step){
            if(step==steps[index] && armed.exchange(false)){
                if(index==4) throw std::runtime_error("worker runtime fault");
                if((index&1)==0) throw std::bad_alloc();
                throw std::length_error("worker length fault");
            }
        });
        SessionWorker worker((HWND)1,ops);
        SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");
        request.requestId=700+(uint64_t)index*2; request.purpose=SessionPurpose::ManualRestore;
        CHECK(worker.Request(request));
        std::unique_ptr<SessionResult> unavailable=sink.waitFor(request.requestId);
        CHECK(unavailable && unavailable->status==SessionDataStatus::Unavailable &&
              unavailable->requestId==request.requestId && unavailable->purpose==request.purpose && !unavailable->windows);
        CHECK(worker.ActiveCount()==0 && worker.OutstandingForApp("firefox")==0);
        request.requestId++;
        CHECK(worker.Request(request));
        std::unique_ptr<SessionResult> fresh=sink.waitFor(request.requestId);
        CHECK(fresh && fresh->status==SessionDataStatus::Fresh && fresh->windows &&
              fresh->windows->at(0).activeTitle=="ok");
        CHECK(worker.Stop());
    }
}

static void test_session_worker_result_allocation_failure_drops_only_that_request(){
    SessionResultSink sink;
    std::atomic<int> allocations(0);
    SessionWorkerOps ops=exceptionWorkerOps(sink);
    ops.makeResult=[&]()->std::unique_ptr<SessionResult>{
        int call=++allocations;
        if(call==1) throw std::bad_alloc();
        if(call==2) throw std::length_error("result allocation fault");
        return std::unique_ptr<SessionResult>(new SessionResult());
    };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.requestId=720; CHECK(worker.Request(request)); CHECK(waitSessionWorkerIdle(worker,"firefox"));
    request.requestId=721; CHECK(worker.Request(request)); CHECK(waitSessionWorkerIdle(worker,"firefox"));
    {
        std::lock_guard<std::mutex> lock(sink.mutex);
        CHECK(sink.results.empty());
    }
    request.requestId=722; CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> fresh=sink.waitFor(722);
    CHECK(fresh && fresh->status==SessionDataStatus::Fresh && allocations.load()==3);
    CHECK(worker.ActiveCount()==0 && worker.Stop());
}

static void test_session_worker_result_prepare_fault_drops_unidentified_result_and_continues(){
    SessionResultSink sink;
    std::atomic<bool> armed(true);
    SessionWorkerOps ops=exceptionWorkerOps(sink,[&](SessionWorkerStep step){
        if(step==SessionWorkerStep::ResultPrepare && armed.exchange(false)) throw std::bad_alloc();
    });
    ops.makeResult=[](){
        std::unique_ptr<SessionResult> result(new SessionResult());
        result->requestId=9999; result->app="sentinel";
        return result;
    };
    SessionWorker worker((HWND)1,ops);
    SessionRequest request; request.app="firefox"; request.profile=sessionTestProfile("firefox");
    request.requestId=725;
    CHECK(worker.Request(request)); CHECK(waitSessionWorkerIdle(worker,"firefox"));
    {
        std::lock_guard<std::mutex> lock(sink.mutex);
        CHECK(sink.results.empty());
    }
    request.requestId=726;
    CHECK(worker.Request(request));
    std::unique_ptr<SessionResult> fresh=sink.waitFor(726);
    CHECK(fresh && fresh->status==SessionDataStatus::Fresh);
    CHECK(worker.Stop());
}

static void test_session_worker_superseded_result_factory_is_not_called_under_state_lock(){
    SessionResultSink sink; BlockingSessionParse parser;
    const std::thread::id requesterThread=std::this_thread::get_id();
    std::atomic<bool> requesterFactoryCall(false);
    SessionWorkerOps ops=coalescingWorkerOps(sink,parser);
    ops.makeResult=[&](){
        if(std::this_thread::get_id()==requesterThread){
            requesterFactoryCall=true;
            throw std::runtime_error("requester result factory callback");
        }
        return std::unique_ptr<SessionResult>(new SessionResult());
    };
    SessionWorker worker((HWND)1,ops);
    SessionRequest active; active.requestId=750; active.app="firefox";
    active.profile=sessionTestProfile("firefox"); active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker.Request(active)); CHECK(parser.waitEntered());
    SessionRequest original=active; original.requestId=751; original.purpose=SessionPurpose::ManualSave;
    SessionRequest replacement=original; replacement.requestId=752; replacement.purpose=SessionPurpose::ManualRestore;
    CHECK(worker.Request(original));
    bool accepted=worker.Request(replacement);
    CHECK(accepted && !requesterFactoryCall.load());
    parser.release();
    CHECK(sink.waitFor(750)!=nullptr);
    if(accepted){
        std::unique_ptr<SessionResult> superseded=sink.waitFor(751);
        std::unique_ptr<SessionResult> newest=sink.waitFor(752);
        CHECK(superseded && superseded->status==SessionDataStatus::Superseded);
        CHECK(newest && newest->status==SessionDataStatus::Fresh);
    } else CHECK(sink.waitFor(751)!=nullptr);
    CHECK(worker.Stop());
}

static void test_session_worker_request_fault_preserves_existing_pending(){
    SessionResultSink sink; BlockingSessionParse parser;
    std::atomic<bool> armed(false);
    SessionWorkerOps ops=coalescingWorkerOps(sink,parser);
    ops.beforeWorkerStep=[&](SessionWorkerStep step){
        if(step==SessionWorkerStep::RequestPrepare && armed.exchange(false)) throw std::bad_alloc();
    };
    SessionWorker worker((HWND)1,ops);
    SessionRequest active; active.requestId=730; active.app="firefox"; active.profile=sessionTestProfile("firefox");
    active.purpose=SessionPurpose::MetadataProbe;
    CHECK(worker.Request(active)); CHECK(parser.waitEntered());
    SessionRequest original=active; original.requestId=731; original.purpose=SessionPurpose::ManualSave;
    CHECK(worker.Request(original));
    SessionRequest replacement=original; replacement.requestId=732; replacement.purpose=SessionPurpose::ManualRestore;
    armed=true;
    bool replacementThrew=false,replacementAccepted=true;
    try { replacementAccepted=worker.Request(replacement); } catch(...) { replacementThrew=true; }
    CHECK(!replacementThrew && !replacementAccepted);
    CHECK(worker.PendingCount()==1 && worker.OutstandingForApp("firefox")==2);
    parser.release();
    std::unique_ptr<SessionResult> activeResult=sink.waitFor(730);
    std::unique_ptr<SessionResult> originalResult=sink.waitFor(731);
    CHECK(activeResult && activeResult->status==SessionDataStatus::Fresh);
    CHECK(originalResult && originalResult->status==SessionDataStatus::Fresh &&
          originalResult->purpose==SessionPurpose::ManualSave);
    CHECK(worker.Stop());
}

static void test_session_cache_lookup_fault_preserves_output_and_lru(){
    bool armed=false;
    SessionCacheOps ops;
    ops.beforeLookupOutput=[&]{ if(armed){ armed=false; throw std::bad_alloc(); } };
    SessionCache cache(2,1024*1024,ops);
    SessionStamp stampA; stampA.size=1; stampA.mtime=1;
    SessionStamp stampB; stampB.size=2; stampB.mtime=2;
    SessionStamp stampC; stampC.size=3; stampC.mtime=3;
    std::vector<WinFp> a(1),b(1),c(1); a[0].activeTitle="A"; b[0].activeTitle="B"; c[0].activeTitle="C";
    SessionCacheValue outA,outB;
    CHECK(cache.Put("firefox",L"A",stampA,std::move(a),1,1,outA));
    CHECK(cache.Put("firefox",L"B",stampB,std::move(b),2,2,outB));
    outA.windows.reset(); outB.windows.reset();
    SessionCacheValue sentinel=sessionCacheOutputSentinel();
    const std::vector<WinFp>* sentinelIdentity=sentinel.windows.get();
    bool threw=false,found=false;
    armed=true;
    try { found=cache.FindExact("firefox",L"A",stampA,sentinel); } catch(...) { threw=true; }
    CHECK(!threw && !found);
    CHECK(isSessionCacheOutputSentinel(sentinel,sentinelIdentity));
    SessionCacheValue inserted;
    CHECK(cache.Put("firefox",L"C",stampC,std::move(c),3,3,inserted));
    SessionCacheValue lookup;
    CHECK(!cache.FindExact("firefox",L"A",stampA,lookup));
    CHECK(cache.FindExact("firefox",L"B",stampB,lookup) && lookup.windows->at(0).activeTitle=="B");
}

static void test_session_cache_runtime_fault_is_transactional(){
    bool armed=false;
    SessionCacheOps ops;
    ops.beforePutStep=[&](SessionCachePutStep step){
        if(armed && step==SessionCachePutStep::ContainerInsert) throw std::runtime_error("cache runtime fault");
    };
    SessionCache cache(2,1024*1024,ops);
    SessionStamp stampA; stampA.size=1; stampA.mtime=1;
    SessionStamp stampB; stampB.size=2; stampB.mtime=2;
    std::vector<WinFp> a(1),b(1); a[0].activeTitle="A"; b[0].activeTitle="B";
    SessionCacheValue stored;
    CHECK(cache.Put("firefox",L"A",stampA,std::move(a),1,1,stored));
    const std::vector<WinFp>* identity=stored.windows.get();
    SessionCacheValue output=sessionCacheOutputSentinel();
    const std::vector<WinFp>* outputIdentity=output.windows.get();
    bool threw=false,returned=true; armed=true;
    try { returned=cache.Put("firefox",L"B",stampB,std::move(b),2,2,output); } catch(...) { threw=true; }
    armed=false;
    CHECK(!threw && !returned);
    CHECK(isSessionCacheOutputSentinel(output,outputIdentity));
    CHECK(cache.EntryCount()==1);
    SessionCacheValue stillA;
    CHECK(cache.FindExact("firefox",L"A",stampA,stillA) && stillA.windows.get()==identity);
}

static void test_post_message_exception_deletes_heap_result(){
    SessionWorkerOps ops;
    ops.postMessage=[](HWND,UINT,WPARAM,LPARAM)->bool{ throw std::runtime_error("poster fault"); };
    FillMissingSessionWorkerOps(ops);
    std::shared_ptr<const std::vector<WinFp> > payload(new std::vector<WinFp>(1));
    std::unique_ptr<SessionResult> result(new SessionResult()); result->windows=payload;
    CHECK(payload.use_count()==2);
    CHECK(!PostSessionResultOwned(ops,(HWND)1,std::move(result)) && payload.use_count()==1);
}

// ---- failure-atomic layout-store tests ----

static bool IsFixtureRootSafe(const std::wstring& root){
    wchar_t tempBuffer[MAX_PATH+1]={0}, rootBuffer[32768]={0}, tempFullBuffer[32768]={0};
    DWORD tempLength=GetTempPathW(MAX_PATH,tempBuffer);
    if(tempLength==0 || tempLength>MAX_PATH) return false;
    DWORD rootLength=GetFullPathNameW(root.c_str(),32768,rootBuffer,nullptr);
    DWORD tempFullLength=GetFullPathNameW(tempBuffer,32768,tempFullBuffer,nullptr);
    if(rootLength==0 || rootLength>=32768 || tempFullLength==0 || tempFullLength>=32768) return false;
    std::wstring fullRoot(rootBuffer), fullTemp(tempFullBuffer);
    if(fullTemp.back()!=L'\\') fullTemp.push_back(L'\\');
    if(fullRoot.size()<=fullTemp.size() || _wcsnicmp(fullRoot.c_str(),fullTemp.c_str(),fullTemp.size())!=0)
        return false;
    return fullRoot.find(L"vde-layout-test-",fullTemp.size())==fullTemp.size();
}

static void RemoveFixtureTreeNoReparse(const std::wstring& path, const std::wstring& root){
    if(path.size()<root.size() || _wcsnicmp(path.c_str(),root.c_str(),root.size())!=0) return;
    DWORD rootAttributes=GetFileAttributesW(path.c_str());
    if(rootAttributes==INVALID_FILE_ATTRIBUTES) return;
    if(rootAttributes&FILE_ATTRIBUTE_REPARSE_POINT){
        if(rootAttributes&FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(path.c_str());
        else DeleteFileW(path.c_str());
        return;
    }
    std::wstring pattern=path+L"\\*";
    WIN32_FIND_DATAW found{};
    HANDLE search=FindFirstFileW(pattern.c_str(),&found);
    if(search!=INVALID_HANDLE_VALUE){
        do{
            if(wcscmp(found.cFileName,L".")==0 || wcscmp(found.cFileName,L"..")==0) continue;
            std::wstring child=path+L"\\"+found.cFileName;
            if(child.size()<=root.size() || _wcsnicmp(child.c_str(),root.c_str(),root.size())!=0) continue;
            if(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){
                if(found.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(child.c_str());
                else RemoveFixtureTreeNoReparse(child,root);
            } else {
                if(!(found.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
                    SetFileAttributesW(child.c_str(),FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while(FindNextFileW(search,&found));
        FindClose(search);
    }
    RemoveDirectoryW(path.c_str());
}

struct LayoutTempDir {
    std::wstring path;
    LayoutTempDir(){
        wchar_t temp[MAX_PATH+1]={0};
        DWORD length=GetTempPathW(MAX_PATH,temp);
        GUID id{};
        CHECK(length>0 && length<=MAX_PATH);
        CHECK(SUCCEEDED(CoCreateGuid(&id)));
        path=std::wstring(temp)+L"vde-layout-test-"+GuidToString(id);
        CHECK(IsFixtureRootSafe(path));
        CHECK(CreateDirectoryW(path.c_str(),nullptr)!=0);
    }
    ~LayoutTempDir(){
        if(!path.empty() && IsFixtureRootSafe(path)) RemoveFixtureTreeNoReparse(path,path);
    }
    std::wstring file(const wchar_t* name) const { return path+L"\\"+name; }
};

static bool WriteRawFile(const std::wstring& path, const std::string& bytes){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    size_t offset=0;
    bool ok=true;
    while(offset<bytes.size()){
        DWORD requested=(DWORD)(std::min)(bytes.size()-offset,(size_t)65536);
        DWORD written=0;
        if(!WriteFile(file,bytes.data()+offset,requested,&written,nullptr) || written==0 || written>requested){
            ok=false; break;
        }
        offset+=written;
    }
    if(ok && !FlushFileBuffers(file)) ok=false;
    if(!CloseHandle(file)) ok=false;
    return ok;
}

static bool ResizeRawFile(const std::wstring& path, unsigned long long size){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER position{}; position.QuadPart=(LONGLONG)size;
    bool ok=SetFilePointerEx(file,position,nullptr,FILE_BEGIN)!=0 && SetEndOfFile(file)!=0 &&
        FlushFileBuffers(file)!=0;
    if(!CloseHandle(file)) ok=false;
    return ok;
}

static bool SetRawFileMtime(const std::wstring& path,unsigned long long mtime){
    HANDLE file=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    ULARGE_INTEGER combined{};
    combined.QuadPart=mtime;
    FILETIME value{};
    value.dwLowDateTime=combined.LowPart;
    value.dwHighDateTime=combined.HighPart;
    bool ok=SetFileTime(file,nullptr,nullptr,&value)!=0;
    if(!CloseHandle(file)) ok=false;
    return ok;
}

static bool RawFileExists(const std::wstring& path){
    DWORD attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_DIRECTORY);
}

static std::string ReadRawFile(const std::wstring& path){
    FileReadResult read=ReadFileBytesBounded(path,MAX_LAYOUT_FILE_BYTES);
    return read.status==FileReadStatus::Ok ? read.bytes : std::string();
}

static void test_session_bounded_reader_binds_bytes_to_exact_aba_handle(){
    LayoutTempDir temp;
    std::wstring live=temp.file(L"live.bin");
    std::wstring incoming=temp.file(L"incoming.bin");
    std::wstring parkedA=temp.file(L"parked-a.bin");
    std::wstring parkedB=temp.file(L"parked-b.bin");
    CHECK(WriteRawFile(live,"AAAA"));
    CHECK(WriteRawFile(incoming,"BBBB"));
    SessionStamp stampA,stampB;
    CHECK(GetSessionStamp(live,stampA));
    CHECK(GetSessionStamp(incoming,stampB));
    CHECK(stampA!=stampB && stampA.size==stampB.size);
    CHECK(MoveFileExW(live.c_str(),parkedA.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE);
    CHECK(MoveFileExW(incoming.c_str(),live.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE);

    bool restored=false;
    SessionFileReadOps ops;
    ops.afterOpen=[&](HANDLE){
        bool movedB=MoveFileExW(live.c_str(),parkedB.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
        bool movedA=MoveFileExW(parkedA.c_str(),live.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
        restored=movedA&&movedB;
        if(!restored) throw std::runtime_error("ABA restore failed");
    };
    SessionFileReadResult read=ReadBrowserSessionFileBounded(live,MAX_BROWSER_SESSION_BYTES,ops);
    SessionStamp current;
    CHECK(restored && GetSessionStamp(live,current) &&
          current.volumeSerial==stampA.volumeSerial && current.fileIdLow==stampA.fileIdLow &&
          current.fileIdHigh==stampA.fileIdHigh);
    CHECK(read.status==FileReadStatus::Ok && read.bytes=="BBBB" && read.readStampKnown);
    CHECK(read.readStamp.volumeSerial==stampB.volumeSerial &&
          read.readStamp.fileIdLow==stampB.fileIdLow && read.readStamp.fileIdHigh==stampB.fileIdHigh &&
          read.readStamp!=current);
}

static void test_session_bounded_reader_rejects_handle_changes_and_close_failure(){
    LayoutTempDir temp;
    std::wstring path=temp.file(L"session.bin");
    CHECK(WriteRawFile(path,std::string(128*1024,'s')));

    SessionFileReadOps exactOps;
    auto exactRead=exactOps.readFile;
    auto exactStamp=exactOps.getStamp;
    std::vector<DWORD> requested;
    int stampCalls=0;
    exactOps.readFile=[&](HANDLE file,void* bytes,DWORD amount,DWORD& read)->BOOL{
        requested.push_back(amount); return exactRead(file,bytes,amount,read);
    };
    exactOps.getStamp=[&](HANDLE file,SessionStamp& stamp){
        ++stampCalls; return exactStamp(file,stamp);
    };
    SessionFileReadResult exact=ReadBrowserSessionFileBounded(path,MAX_BROWSER_SESSION_BYTES,exactOps);
    SessionStamp pathStamp;
    CHECK(exact.status==FileReadStatus::Ok && exact.readStampKnown &&
          GetSessionStamp(path,pathStamp) && exact.readStamp==pathStamp);
    CHECK(exact.bytes.size()==128*1024 && stampCalls==2 && requested.size()==3);
    CHECK((std::all_of)(requested.begin(),requested.end(),
        [](DWORD amount){ return amount<=64*1024; }));
    CHECK(requested.back()==1);

    SessionFileReadOps sharedWriteOps;
    bool concurrentWriterOpened=false;
    sharedWriteOps.afterOpen=[&](HANDLE){
        HANDLE writer=CreateFileW(path.c_str(),GENERIC_WRITE,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,nullptr);
        concurrentWriterOpened=writer!=INVALID_HANDLE_VALUE;
        if(concurrentWriterOpened) CloseHandle(writer);
    };
    SessionFileReadResult sharedWrite=ReadBrowserSessionFileBounded(
        path,MAX_BROWSER_SESSION_BYTES,sharedWriteOps);
    CHECK(concurrentWriterOpened && sharedWrite.status==FileReadStatus::Ok &&
          sharedWrite.readStampKnown);

    SessionFileReadOps truncatedOps;
    auto truncatedRead=truncatedOps.readFile;
    auto truncatedClose=truncatedOps.closeHandle;
    int truncatedCalls=0;
    int truncatedCloseCalls=0;
    truncatedOps.readFile=[&](HANDLE file,void* bytes,DWORD amount,DWORD& read)->BOOL{
        if(truncatedCalls++==0) return truncatedRead(file,bytes,(std::min)(amount,3UL),read);
        read=0; return TRUE;
    };
    truncatedOps.closeHandle=[&](HANDLE file){
        ++truncatedCloseCalls; return truncatedClose(file);
    };
    SessionFileReadResult truncated=ReadBrowserSessionFileBounded(path,MAX_BROWSER_SESSION_BYTES,truncatedOps);
    CHECK(truncatedCloseCalls==1 && truncated.status==FileReadStatus::Unavailable &&
          truncated.bytes.empty() && !truncated.readStampKnown);

    SessionFileReadOps growthOps;
    auto growthRead=growthOps.readFile;
    auto growthStamp=growthOps.getStamp;
    int growthCalls=0;
    int growthStampCalls=0;
    growthOps.readFile=[&](HANDLE file,void* bytes,DWORD amount,DWORD& read)->BOOL{
        if(++growthCalls==3){ *(char*)bytes='x'; read=1; return TRUE; }
        return growthRead(file,bytes,amount,read);
    };
    growthOps.getStamp=[&](HANDLE file,SessionStamp& stamp){
        ++growthStampCalls; return growthStamp(file,stamp);
    };
    SessionFileReadResult growth=ReadBrowserSessionFileBounded(path,MAX_BROWSER_SESSION_BYTES,growthOps);
    CHECK(growthStampCalls==2 && growth.status==FileReadStatus::Unavailable &&
          growth.bytes.empty() && !growth.readStampKnown);

    SessionFileReadOps changedOps;
    auto changedStamp=changedOps.getStamp;
    int changedCalls=0;
    changedOps.getStamp=[&](HANDLE file,SessionStamp& stamp){
        bool ok=changedStamp(file,stamp);
        if(ok && ++changedCalls==2) ++stamp.changeTime;
        return ok;
    };
    SessionFileReadResult changed=ReadBrowserSessionFileBounded(path,MAX_BROWSER_SESSION_BYTES,changedOps);
    CHECK(changedCalls==2 && changed.status==FileReadStatus::Unavailable &&
          changed.bytes.empty() && !changed.readStampKnown);

    SessionFileReadOps overLimitOps;
    auto overLimitStamp=overLimitOps.getStamp;
    int overLimitCalls=0;
    overLimitOps.getStamp=[&](HANDLE file,SessionStamp& stamp){
        bool ok=overLimitStamp(file,stamp);
        if(ok && ++overLimitCalls==2) stamp.size=128*1024+1;
        return ok;
    };
    SessionFileReadResult overLimit=ReadBrowserSessionFileBounded(path,128*1024,overLimitOps);
    CHECK(overLimitCalls==2 && overLimit.status==FileReadStatus::TooLarge &&
          overLimit.bytes.empty() && !overLimit.readStampKnown);

    SessionFileReadOps closeOps;
    auto realClose=closeOps.closeHandle;
    int closeCalls=0;
    closeOps.closeHandle=[&](HANDLE file)->BOOL{
        ++closeCalls; realClose(file); SetLastError(ERROR_INVALID_HANDLE); return FALSE;
    };
    SessionFileReadResult closeFailed=ReadBrowserSessionFileBounded(path,MAX_BROWSER_SESSION_BYTES,closeOps);
    CHECK(closeCalls==1 && closeFailed.status==FileReadStatus::Unavailable &&
          closeFailed.bytes.empty() && !closeFailed.readStampKnown);

    SessionFileReadOps throwingCloseOps;
    auto throwingRealClose=throwingCloseOps.closeHandle;
    HANDLE reusedHandle=nullptr;
    std::vector<HANDLE> otherHandles;
    int throwingCloseCalls=0;
    throwingCloseOps.closeHandle=[&](HANDLE file)->BOOL{
        ++throwingCloseCalls;
        if(!throwingRealClose(file)) throw std::runtime_error("real close failed");
        for(int attempt=0;attempt<256 && !reusedHandle;++attempt){
            HANDLE candidate=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if(candidate==file) reusedHandle=candidate;
            else if(candidate) otherHandles.push_back(candidate);
        }
        throw std::runtime_error("close callback fault after close");
    };
    SessionFileReadResult closeThrew=ReadBrowserSessionFileBounded(
        path,MAX_BROWSER_SESSION_BYTES,throwingCloseOps);
    DWORD handleFlags=0;
    bool reusedStillValid=reusedHandle && GetHandleInformation(reusedHandle,&handleFlags)!=FALSE;
    CHECK(throwingCloseCalls==1 && reusedHandle!=nullptr && reusedStillValid);
    CHECK(closeThrew.status==FileReadStatus::Unavailable && closeThrew.bytes.empty() &&
          !closeThrew.readStampKnown);
    if(reusedStillValid) CloseHandle(reusedHandle);
    for(size_t i=0;i<otherHandles.size();++i) CloseHandle(otherHandles[i]);

    SessionFileReadOps throwingStampOps;
    auto throwingStampClose=throwingStampOps.closeHandle;
    int throwingStampCloseCalls=0;
    throwingStampOps.getStamp=[](HANDLE,SessionStamp&)->bool{ throw std::bad_alloc(); };
    throwingStampOps.closeHandle=[&](HANDLE file){
        ++throwingStampCloseCalls; return throwingStampClose(file);
    };
    SessionFileReadResult stampThrew=ReadBrowserSessionFileBounded(
        path,MAX_BROWSER_SESSION_BYTES,throwingStampOps);
    CHECK(throwingStampCloseCalls==1 && stampThrew.status==FileReadStatus::Unavailable &&
          stampThrew.bytes.empty() && !stampThrew.readStampKnown);

    DWORD handlesBefore=0,handlesAfter=0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesBefore)!=0);
    for(int attempt=0;attempt<128;++attempt){
        SessionFileReadResult repeated=ReadBrowserSessionFileBounded(path,MAX_BROWSER_SESSION_BYTES);
        CHECK(repeated.status==FileReadStatus::Ok && repeated.readStampKnown);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesAfter)!=0);
    CHECK(handlesAfter==handlesBefore);
}

static std::vector<std::wstring> DiagnosticCopies(const std::wstring& source){
    std::vector<std::wstring> copies;
    size_t slash=source.find_last_of(L"\\/");
    std::wstring directory=slash==std::wstring::npos ? L"." : source.substr(0,slash);
    std::wstring pattern=source+L".corrupt.*";
    WIN32_FIND_DATAW found{};
    HANDLE search=FindFirstFileW(pattern.c_str(),&found);
    if(search==INVALID_HANDLE_VALUE) return copies;
    do{
        if(!(found.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) copies.push_back(directory+L"\\"+found.cFileName);
    } while(FindNextFileW(search,&found));
    FindClose(search);
    std::sort(copies.begin(),copies.end());
    return copies;
}

static std::string ValidLayoutBytes(const std::string& desktopName){
    return std::string("# VDE snapshot v4\n")+
        "D\t0\t{231A0000-0000-0000-0000-000000000001}\t"+b64enc(desktopName)+"\n";
}

static std::string LoadedDesktopName(const LayoutLoadResult& loaded){
    return loaded.desks.empty() ? std::string() : W2U8(loaded.desks.front().name);
}

static void test_bounded_read_exact_limit_and_preallocation_rejection(){
    LayoutTempDir temp;
    std::wstring exact=temp.file(L"exact.bin"), oversized=temp.file(L"oversized.bin");
    CHECK(ResizeRawFile(exact,MAX_LAYOUT_FILE_BYTES));
    LayoutFsOps exactOps;
    auto exactRead=exactOps.readFile;
    std::vector<DWORD> requestedSizes;
    exactOps.readFile=[&](HANDLE handle,void* buffer,DWORD requested,DWORD& read)->BOOL{
        requestedSizes.push_back(requested);
        return exactRead(handle,buffer,requested,read);
    };
    FileReadResult atLimit=ReadFileBytesBounded(exact,MAX_LAYOUT_FILE_BYTES,exactOps);
    CHECK(atLimit.status==FileReadStatus::Ok);
    CHECK(atLimit.bytes.size()==(size_t)MAX_LAYOUT_FILE_BYTES);
    CHECK(requestedSizes.size()==257);
    CHECK((std::count)(requestedSizes.begin(),requestedSizes.end(),64*1024)==256);
    CHECK(!requestedSizes.empty() && requestedSizes.back()==1);
    CHECK((std::all_of)(requestedSizes.begin(),requestedSizes.end(),
        [](DWORD requested){ return requested<=64*1024; }));

    CHECK(ResizeRawFile(oversized,MAX_LAYOUT_FILE_BYTES+1));
    LayoutFsOps ops;
    auto realRead=ops.readFile;
    int readCalls=0;
    ops.readFile=[&](HANDLE handle,void* buffer,DWORD requested,DWORD& read)->BOOL{
        ++readCalls; return realRead(handle,buffer,requested,read);
    };
    FileReadResult tooLarge=ReadFileBytesBounded(oversized,MAX_LAYOUT_FILE_BYTES,ops);
    CHECK(tooLarge.status==FileReadStatus::TooLarge);
    CHECK(tooLarge.bytes.empty());
    CHECK(readCalls==0);
}

static void test_bounded_read_failures_are_transactional_and_status_bearing(){
    LayoutTempDir temp;
    std::wstring path=temp.file(L"input.bin"), missing=temp.file(L"missing.bin");
    CHECK(WriteRawFile(path,"abcdef"));

    LayoutFsOps partialOps;
    auto realRead=partialOps.readFile;
    int calls=0;
    partialOps.readFile=[&](HANDLE handle,void* buffer,DWORD requested,DWORD& read)->BOOL{
        if(calls++==0) return realRead(handle,buffer,(std::min)(requested,3UL),read);
        read=0; SetLastError(ERROR_READ_FAULT); return FALSE;
    };
    FileReadResult partial=ReadFileBytesBounded(path,1024,partialOps);
    CHECK(partial.status==FileReadStatus::Unavailable);
    CHECK(partial.bytes.empty());
    CHECK(!partial.error.empty());

    LayoutFsOps changedOps;
    auto realSize=changedOps.getSize;
    int sizeCalls=0;
    changedOps.getSize=[&](HANDLE handle,unsigned long long& size)->BOOL{
        BOOL ok=realSize(handle,size);
        if(ok && ++sizeCalls>=2) ++size;
        return ok;
    };
    FileReadResult changed=ReadFileBytesBounded(path,1024,changedOps);
    CHECK(changed.status==FileReadStatus::Unavailable);
    CHECK(changed.bytes.empty());

    LayoutFsOps shortOps;
    shortOps.readFile=[](HANDLE,void*,DWORD,DWORD& read)->BOOL{ read=0; return TRUE; };
    FileReadResult shortened=ReadFileBytesBounded(path,1024,shortOps);
    CHECK(shortened.status==FileReadStatus::Unavailable);
    CHECK(shortened.bytes.empty());

    LayoutFsOps growthOps;
    auto growthRealSize=growthOps.getSize;
    int growthSizeCalls=0;
    growthOps.getSize=[&](HANDLE handle,unsigned long long& size)->BOOL{
        BOOL ok=growthRealSize(handle,size);
        if(ok && ++growthSizeCalls>=2) size=7;
        return ok;
    };
    FileReadResult grewOverLimit=ReadFileBytesBounded(path,6,growthOps);
    CHECK(grewOverLimit.status==FileReadStatus::TooLarge);
    CHECK(grewOverLimit.bytes.empty());

    LayoutFsOps mtimeOps;
    mtimeOps.getMtime=[](HANDLE,unsigned long long&)->BOOL{
        SetLastError(ERROR_LOCK_VIOLATION); return FALSE;
    };
    FileReadResult noMtime=ReadFileBytesBounded(path,1024,mtimeOps);
    CHECK(noMtime.status==FileReadStatus::Unavailable);
    CHECK(noMtime.bytes.empty());

    LayoutFsOps changedMtimeOps;
    auto realMtime=changedMtimeOps.getMtime;
    int mtimeCalls=0;
    changedMtimeOps.getMtime=[&](HANDLE handle,unsigned long long& mtime)->BOOL{
        BOOL ok=realMtime(handle,mtime);
        if(ok && ++mtimeCalls>=2) ++mtime;
        return ok;
    };
    FileReadResult changedMtime=ReadFileBytesBounded(path,1024,changedMtimeOps);
    CHECK(mtimeCalls>=2);
    CHECK(changedMtime.status==FileReadStatus::Unavailable);
    CHECK(changedMtime.bytes.empty());

    LayoutFsOps sizeOps;
    sizeOps.getSize=[](HANDLE,unsigned long long&)->BOOL{
        SetLastError(ERROR_CRC); return FALSE;
    };
    FileReadResult noSize=ReadFileBytesBounded(path,1024,sizeOps);
    CHECK(noSize.status==FileReadStatus::Unavailable);
    CHECK(noSize.bytes.empty());

    LayoutFsOps attributeOps;
    attributeOps.getAttributes=[&](const std::wstring& queried)->DWORD{
        if(queried==path){ SetLastError(ERROR_SHARING_VIOLATION); return INVALID_FILE_ATTRIBUTES; }
        return GetFileAttributesW(queried.c_str());
    };
    FileReadResult noAttributes=ReadFileBytesBounded(path,1024,attributeOps);
    CHECK(noAttributes.status==FileReadStatus::Unavailable);
    CHECK(noAttributes.bytes.empty());

    LayoutFsOps closeOps;
    auto realClose=closeOps.closeHandle;
    closeOps.closeHandle=[&](HANDLE handle)->BOOL{
        realClose(handle); SetLastError(ERROR_INVALID_HANDLE); return FALSE;
    };
    FileReadResult closeFailed=ReadFileBytesBounded(path,1024,closeOps);
    CHECK(closeFailed.status==FileReadStatus::Unavailable);
    CHECK(closeFailed.bytes.empty());

    LayoutFsOps accessOps;
    auto realOpen=accessOps.openFile;
    accessOps.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
        if(opened==path){ SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
        return realOpen(opened,access,share,creation,flags);
    };
    FileReadResult unavailable=ReadFileBytesBounded(path,1024,accessOps);
    FileReadResult absent=ReadFileBytesBounded(missing,1024);
    CHECK(unavailable.status==FileReadStatus::Unavailable);
    CHECK(unavailable.win32Error==ERROR_ACCESS_DENIED);
    CHECK(absent.status==FileReadStatus::Missing);
    CHECK(absent.bytes.empty());

    std::string legacy="sentinel";
    CHECK(!ReadFileBytes(missing,legacy));
    CHECK(legacy=="sentinel");
}

static void test_bounded_read_denies_concurrent_in_place_writer(){
    LayoutTempDir temp;
    std::wstring path=temp.file(L"stable.bin");
    CHECK(WriteRawFile(path,std::string(128*1024,'s')));
    LayoutFsOps ops;
    auto realRead=ops.readFile;
    bool attempted=false, writerOpened=false;
    DWORD writerError=ERROR_SUCCESS;
    ops.readFile=[&](HANDLE handle,void* buffer,DWORD requested,DWORD& read)->BOOL{
        if(!attempted){
            attempted=true;
            HANDLE writer=CreateFileW(path.c_str(),GENERIC_WRITE,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,nullptr);
            writerOpened=writer!=INVALID_HANDLE_VALUE;
            writerError=writerOpened ? ERROR_SUCCESS : GetLastError();
            if(writerOpened) CloseHandle(writer);
        }
        return realRead(handle,buffer,requested,read);
    };
    FileReadResult read=ReadFileBytesBounded(path,1024*1024,ops);
    CHECK(read.status==FileReadStatus::Ok && read.bytes.size()==128*1024);
    CHECK(attempted);
    CHECK(!writerOpened);
    CHECK(writerError==ERROR_SHARING_VIOLATION);
}

static void test_layout_load_missing_and_valid_primary(){
    LayoutTempDir temp;
    std::wstring missing=temp.file(L"missing-layout.txt");
    LayoutLoadResult empty=LoadLayoutWithBackup(missing,1700000000);
    CHECK(empty.status==LayoutLoadStatus::Missing);
    CHECK(empty.writesAllowed);
    CHECK(!empty.usable());
    CHECK(empty.desks.empty() && empty.wins.empty());
    CHECK(empty.revision.sourcePath==missing && !empty.revision.exists);

    std::wstring primary=temp.file(L"layout.txt");
    std::string primaryBytes=ValidLayoutBytes("primary");
    CHECK(WriteRawFile(primary,primaryBytes));
    CHECK(WriteRawFile(primary+L".rollback","stale corrupt recovery"));
    LayoutLoadResult loaded=LoadLayoutWithBackup(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::Valid);
    CHECK(loaded.writesAllowed && loaded.usable());
    CHECK(loaded.sourceVersion==4);
    CHECK(LoadedDesktopName(loaded)=="primary");
    CHECK(loaded.revision.exists && loaded.revision.sourcePath==primary);
    CHECK(loaded.revision.size==primaryBytes.size());
    CHECK(DiagnosticCopies(primary+L".rollback").empty());
}

static void test_layout_recovery_prefers_valid_rollback_and_preserves_primary(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt");
    const std::string corrupt="corrupt primary", rollback=ValidLayoutBytes("rollback"), bak=ValidLayoutBytes("older-bak");
    CHECK(WriteRawFile(primary,corrupt));
    CHECK(WriteRawFile(primary+L".rollback",rollback));
    CHECK(WriteRawFile(primary+L".bak",bak));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::Recovered);
    CHECK(loaded.writesAllowed && loaded.usable());
    CHECK(LoadedDesktopName(loaded)=="rollback");
    CHECK(loaded.revision.sourcePath==primary+L".rollback");
    CHECK(ReadRawFile(primary+L".bak")==bak);
    std::vector<std::wstring> diagnostics=DiagnosticCopies(primary);
    CHECK(diagnostics.size()==1);
    CHECK(diagnostics.size()==1 && ReadRawFile(diagnostics.front())==corrupt);
}

static void test_layout_recovery_uses_bak_and_preserves_all_corruption(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    const std::string corruptPrimary="bad primary", corruptRollback="bad rollback", goodBak=ValidLayoutBytes("bak");
    CHECK(WriteRawFile(primary,corruptPrimary));
    CHECK(WriteRawFile(rollback,corruptRollback));
    CHECK(WriteRawFile(bak,goodBak));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::Recovered);
    CHECK(LoadedDesktopName(loaded)=="bak");
    CHECK(loaded.revision.sourcePath==bak);
    std::vector<std::wstring> primaryDiagnostics=DiagnosticCopies(primary);
    std::vector<std::wstring> rollbackDiagnostics=DiagnosticCopies(rollback);
    CHECK(primaryDiagnostics.size()==1 && ReadRawFile(primaryDiagnostics.front())==corruptPrimary);
    CHECK(rollbackDiagnostics.size()==1 && ReadRawFile(rollbackDiagnostics.front())==corruptRollback);
    CHECK(ReadRawFile(primary)==corruptPrimary && ReadRawFile(rollback)==corruptRollback);
}

static void test_two_corrupt_streams_require_verified_diagnostics(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
    const std::string badPrimary="bad-primary", badBak="bad-backup";
    CHECK(WriteRawFile(primary,badPrimary));
    CHECK(WriteRawFile(bak,badBak));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::CorruptPreserved);
    CHECK(loaded.writesAllowed && !loaded.usable());
    CHECK(loaded.desks.empty() && loaded.wins.empty());
    std::vector<std::wstring> first=DiagnosticCopies(primary), second=DiagnosticCopies(bak);
    CHECK(first.size()==1 && ReadRawFile(first.front())==badPrimary);
    CHECK(second.size()==1 && ReadRawFile(second.front())==badBak);
    CHECK(ReadRawFile(primary)==badPrimary && ReadRawFile(bak)==badBak);
}

static void test_transient_primary_open_blocks_backup_recovery(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
    CHECK(WriteRawFile(primary,"corrupt primary"));
    CHECK(WriteRawFile(bak,ValidLayoutBytes("backup")));
    LayoutFsOps ops;
    auto realOpen=ops.openFile;
    int backupOpenCalls=0;
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
        if(opened==primary && creation==OPEN_EXISTING){ SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE; }
        if(opened==bak) ++backupOpenCalls;
        return realOpen(opened,access,share,creation,flags);
    };
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(loaded.status==LayoutLoadStatus::Unavailable);
    CHECK(!loaded.writesAllowed && !loaded.usable());
    CHECK(backupOpenCalls==0);
    CHECK(ReadRawFile(primary)=="corrupt primary");
    CHECK(DiagnosticCopies(primary).empty());
}

static void test_oversized_primary_blocks_all_recovery_without_mutation(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    const std::string rollbackBytes=ValidLayoutBytes("oversize-rollback"),
        backupBytes=ValidLayoutBytes("oversize-backup");
    CHECK(ResizeRawFile(primary,MAX_LAYOUT_FILE_BYTES+1));
    CHECK(WriteRawFile(rollback,rollbackBytes)); CHECK(WriteRawFile(bak,backupBytes));
    LayoutFsOps ops;
    auto realOpen=ops.openFile;
    int recoveryOpens=0;
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==rollback || opened==bak) ++recoveryOpens;
        return realOpen(opened,access,share,creation,flags);
    };
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed && !loaded.usable());
    CHECK(recoveryOpens==0);
    CHECK(ReadRawFile(rollback)==rollbackBytes && ReadRawFile(bak)==backupBytes);
    CHECK(DiagnosticCopies(primary).empty() && DiagnosticCopies(rollback).empty() && DiagnosticCopies(bak).empty());
}

static void test_missing_primary_uses_rollback_priority_then_backup_fallback(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
        const std::string rollbackBytes=ValidLayoutBytes("plain-rollback"),
            backupBytes=ValidLayoutBytes("plain-older-backup");
        CHECK(WriteRawFile(rollback,rollbackBytes)); CHECK(WriteRawFile(bak,backupBytes));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.writesAllowed && loaded.usable());
        CHECK(loaded.revision.sourcePath==rollback && LoadedDesktopName(loaded)=="plain-rollback");
        CHECK(ReadRawFile(rollback)==rollbackBytes && ReadRawFile(bak)==backupBytes);
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        const std::string backupBytes=ValidLayoutBytes("plain-backup");
        CHECK(WriteRawFile(bak,backupBytes));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.writesAllowed && loaded.usable());
        CHECK(loaded.revision.sourcePath==bak && LoadedDesktopName(loaded)=="plain-backup");
        CHECK(ReadRawFile(bak)==backupBytes);
    }
}

static void test_transient_and_corrupt_recovery_states(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
        CHECK(WriteRawFile(rollback,ValidLayoutBytes("rollback")));
        CHECK(WriteRawFile(bak,ValidLayoutBytes("bak")));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        int bakOpens=0;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
            if(opened==rollback && creation==OPEN_EXISTING){ SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
            if(opened==bak) ++bakOpens;
            return realOpen(opened,access,share,creation,flags);
        };
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed);
        CHECK(bakOpens==0);
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback";
        CHECK(WriteRawFile(rollback,"corrupt rollback"));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::CorruptPreserved && loaded.writesAllowed);
        std::vector<std::wstring> diagnostics=DiagnosticCopies(rollback);
        CHECK(diagnostics.size()==1 && ReadRawFile(diagnostics.front())=="corrupt rollback");
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        CHECK(WriteRawFile(primary,"corrupt primary"));
        CHECK(WriteRawFile(bak,ValidLayoutBytes("backup")));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
            if(opened==bak && creation==OPEN_EXISTING){ SetLastError(ERROR_LOCK_VIOLATION); return INVALID_HANDLE_VALUE; }
            return realOpen(opened,access,share,creation,flags);
        };
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed);
        CHECK(ReadRawFile(primary)=="corrupt primary" && ReadRawFile(bak)==ValidLayoutBytes("backup"));
    }
}

static void test_diagnostic_copy_failure_and_readback_mismatch_block_writes(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        CHECK(WriteRawFile(primary,"corrupt primary"));
        CHECK(WriteRawFile(bak,ValidLayoutBytes("backup")));
        LayoutFsOps ops;
        ops.copyFile=[](const std::wstring&,const std::wstring&,BOOL)->BOOL{
            SetLastError(ERROR_ACCESS_DENIED); return FALSE;
        };
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed);
        CHECK(!loaded.error.empty());
        CHECK(ReadRawFile(primary)=="corrupt primary" && ReadRawFile(bak)==ValidLayoutBytes("backup"));
        CHECK(DiagnosticCopies(primary).empty());
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        const std::string corrupt="corrupt primary";
        std::string sameLengthMismatch=corrupt;
        sameLengthMismatch[0]='C';
        CHECK(WriteRawFile(primary,corrupt));
        CHECK(WriteRawFile(bak,ValidLayoutBytes("backup")));
        LayoutFsOps ops;
        auto realCopy=ops.copyFile;
        ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
            BOOL copied=realCopy(from,to,failIfExists);
            if(copied && to.find(L".corrupt.")!=std::wstring::npos)
                WriteRawFile(to,sameLengthMismatch);
            return copied;
        };
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed);
        CHECK(!loaded.error.empty());
        CHECK(ReadRawFile(primary)==corrupt && ReadRawFile(bak)==ValidLayoutBytes("backup"));
        CHECK(DiagnosticCopies(primary).size()==1);
    }

    for(int failRead=0;failRead<2;++failRead){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        const std::string corrupt=failRead ? "corrupt readback read" : "corrupt readback open";
        const std::string recovery=ValidLayoutBytes("diagnostic-readback-recovery");
        CHECK(WriteRawFile(primary,corrupt)); CHECK(WriteRawFile(bak,recovery));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        auto realRead=ops.readFile;
        HANDLE diagnosticHandle=INVALID_HANDLE_VALUE;
        bool injected=false;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened.find(L".corrupt.")!=std::wstring::npos && creation==OPEN_EXISTING){
                if(!failRead){ injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE; }
                diagnosticHandle=realOpen(opened,access,share,creation,flags);
                return diagnosticHandle;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        ops.readFile=[&](HANDLE handle,void* buffer,DWORD requested,DWORD& read)->BOOL{
            if(failRead && handle==diagnosticHandle && !injected){
                injected=true; read=0; SetLastError(ERROR_READ_FAULT); return FALSE;
            }
            return realRead(handle,buffer,requested,read);
        };
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed);
        CHECK(!loaded.usable() && !loaded.error.empty());
        CHECK(ReadRawFile(primary)==corrupt && ReadRawFile(bak)==recovery);
        std::vector<std::wstring> diagnostics=DiagnosticCopies(primary);
        CHECK(diagnostics.size()==1 && ReadRawFile(diagnostics.front())==corrupt);
    }
}

static void test_second_diagnostic_copy_failure_and_collision_never_lose_evidence(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        const std::string badPrimary="bad primary", badBackup="bad backup";
        CHECK(WriteRawFile(primary,badPrimary));
        CHECK(WriteRawFile(bak,badBackup));
        LayoutFsOps ops;
        auto realCopy=ops.copyFile;
        int copyCalls=0;
        ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
            if(++copyCalls==2){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
            return realCopy(from,to,failIfExists);
        };
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed);
        CHECK(copyCalls==2);
        CHECK(ReadRawFile(primary)==badPrimary && ReadRawFile(bak)==badBackup);
        std::vector<std::wstring> primaryCopies=DiagnosticCopies(primary), backupCopies=DiagnosticCopies(bak);
        CHECK(primaryCopies.size()==1 && ReadRawFile(primaryCopies.front())==badPrimary);
        CHECK(backupCopies.empty());
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        std::wstring collision=primary+L".corrupt.1700000000.0";
        const std::string corrupt="corrupt primary", sentinel="preexisting diagnostic";
        CHECK(WriteRawFile(primary,corrupt));
        CHECK(WriteRawFile(bak,ValidLayoutBytes("backup")));
        CHECK(WriteRawFile(collision,sentinel));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.revision.sourcePath==bak);
        CHECK(ReadRawFile(collision)==sentinel);
        CHECK(ReadRawFile(primary+L".corrupt.1700000000.1")==corrupt);
        CHECK(DiagnosticCopies(primary).size()==2);
    }
}

static void test_diagnostic_reuse_never_deletes_changed_corrupt_temp(){
    LayoutTempDir tempDir;
    std::wstring primary=tempDir.file(L"layout.txt"), temp=primary+L".tmp",
        diagnostic=temp+L".corrupt.1700000000.0";
    const std::string firstCorrupt="first corrupt temporary layout",
        changedCorrupt="changed corrupt temporary layout";
    CHECK(WriteRawFile(temp,firstCorrupt));
    CHECK(WriteRawFile(diagnostic,firstCorrupt));

    LayoutFsOps ops;
    auto realCopy=ops.copyFile;
    bool injected=false;
    ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        if(from==temp && to==diagnostic && failIfExists && !injected){
            injected=true;
            CHECK(WriteRawFile(temp,changedCorrupt));
            SetLastError(ERROR_FILE_EXISTS);
            return FALSE;
        }
        return realCopy(from,to,failIfExists);
    };

    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(ReadRawFile(temp)==changedCorrupt);
    CHECK(ReadRawFile(diagnostic)==firstCorrupt);
    CHECK(DiagnosticCopies(temp).size()==1);

    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::CorruptPreserved && retry.writesAllowed);
    CHECK(!RawFileExists(temp));
    std::vector<std::wstring> diagnostics=DiagnosticCopies(temp);
    CHECK(diagnostics.size()==2);
    CHECK(ReadRawFile(diagnostic)==firstCorrupt);
    CHECK(ReadRawFile(temp+L".corrupt.1700000000.1")==changedCorrupt);
}

static void test_diagnostic_reuse_never_overwrites_changed_corrupt_backup(){
    LayoutTempDir tempDir;
    std::wstring primary=tempDir.file(L"layout.txt"), backup=primary+L".bak",
        marker=primary+L".bak.previous.promote",
        diagnostic=backup+L".corrupt.1700000000.0";
    const std::string firstCorrupt="first corrupt backup",
        changedCorrupt="changed corrupt backup",
        recovery=ValidLayoutBytes("authoritative recovery");
    CHECK(WriteRawFile(backup,firstCorrupt));
    CHECK(WriteRawFile(marker,recovery));
    CHECK(WriteRawFile(diagnostic,firstCorrupt));

    LayoutFsOps ops;
    auto realCopy=ops.copyFile;
    bool injected=false;
    ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        if(from==backup && to==diagnostic && failIfExists && !injected){
            injected=true;
            CHECK(WriteRawFile(backup,changedCorrupt));
            SetLastError(ERROR_FILE_EXISTS);
            return FALSE;
        }
        return realCopy(from,to,failIfExists);
    };

    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(ReadRawFile(backup)==changedCorrupt);
    CHECK(ReadRawFile(marker)==recovery);
    CHECK(ReadRawFile(diagnostic)==firstCorrupt);
    CHECK(DiagnosticCopies(backup).size()==1);

    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::Recovered && retry.writesAllowed && retry.usable());
    CHECK(retry.revision.sourcePath==backup && LoadedDesktopName(retry)=="authoritative recovery");
    CHECK(ReadRawFile(backup)==recovery && ReadRawFile(marker)==recovery);
    CHECK(DiagnosticCopies(backup).size()==2);
    CHECK(ReadRawFile(diagnostic)==firstCorrupt);
    CHECK(ReadRawFile(backup+L".corrupt.1700000000.1")==changedCorrupt);
}

static void test_diagnostic_preservation_revalidates_primary_before_backup_recovery(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), backup=primary+L".bak",
        diagnostic=primary+L".corrupt.1700000000.0";
    const std::string captured="captured corrupt primary",
        changed="changed corrupt primary", recovery=ValidLayoutBytes("backup recovery");
    CHECK(WriteRawFile(primary,captured));
    CHECK(WriteRawFile(backup,recovery));
    CHECK(WriteRawFile(diagnostic,captured));

    LayoutFsOps ops;
    auto realCopy=ops.copyFile;
    bool injected=false;
    ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        if(from==primary && to==diagnostic && failIfExists && !injected){
            injected=true;
            CHECK(WriteRawFile(primary,changed));
            SetLastError(ERROR_FILE_EXISTS);
            return FALSE;
        }
        return realCopy(from,to,failIfExists);
    };

    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(ReadRawFile(primary)==changed && ReadRawFile(backup)==recovery);
    CHECK(ReadRawFile(diagnostic)==captured && DiagnosticCopies(primary).size()==1);

    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::Recovered && retry.writesAllowed && retry.usable());
    CHECK(retry.revision.sourcePath==backup && ReadRawFile(primary)==changed);
    CHECK(DiagnosticCopies(primary).size()==2);
    CHECK(ReadRawFile(primary+L".corrupt.1700000000.1")==changed);
    LayoutLoadResult stable=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(stable.status==LayoutLoadStatus::Recovered && DiagnosticCopies(primary).size()==2);
}

static void test_diagnostic_preservation_revalidates_primary_before_displaced_recovery(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
        backup=primary+L".bak", diagnostic=primary+L".corrupt.1700000000.0";
    const std::string captured="captured corrupt beside displaced",
        changed="changed corrupt beside displaced", recovery=ValidLayoutBytes("displaced recovery");
    CHECK(WriteRawFile(primary,captured));
    CHECK(WriteRawFile(displaced,recovery));
    CHECK(WriteRawFile(diagnostic,captured));

    LayoutFsOps ops;
    auto realCopy=ops.copyFile;
    bool injected=false;
    ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        if(from==primary && to==diagnostic && failIfExists && !injected){
            injected=true;
            CHECK(WriteRawFile(primary,changed));
            SetLastError(ERROR_FILE_EXISTS);
            return FALSE;
        }
        return realCopy(from,to,failIfExists);
    };

    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(ReadRawFile(primary)==changed && ReadRawFile(displaced)==recovery);
    CHECK(!RawFileExists(backup) && DiagnosticCopies(primary).size()==1);

    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::Recovered && retry.writesAllowed && retry.usable());
    CHECK(retry.revision.sourcePath==backup && ReadRawFile(backup)==recovery);
    CHECK(!RawFileExists(displaced) && ReadRawFile(primary)==changed);
    CHECK(DiagnosticCopies(primary).size()==2);
    LayoutLoadResult stable=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(stable.status==LayoutLoadStatus::Recovered && DiagnosticCopies(primary).size()==2);
}

static void test_fresh_diagnostic_copy_revalidates_primary_before_recovery(){
    for(int copyReturnsFalse=0;copyReturnsFalse<2;++copyReturnsFalse){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), backup=primary+L".bak",
            diagnostic=primary+L".corrupt.1700000000.0";
        const std::string captured="fresh-copy captured primary",
            changed="fresh-copy changed primary", recovery=ValidLayoutBytes("fresh-copy backup");
        CHECK(WriteRawFile(primary,captured)); CHECK(WriteRawFile(backup,recovery));
        LayoutFsOps ops;
        auto realCopy=ops.copyFile;
        bool injected=false;
        ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
            if(from==primary && to==diagnostic && failIfExists && !injected){
                CHECK(realCopy(from,to,failIfExists)!=0);
                CHECK(WriteRawFile(primary,changed));
                injected=true;
                if(copyReturnsFalse){ SetLastError(ERROR_WRITE_FAULT); return FALSE; }
                return TRUE;
            }
            return realCopy(from,to,failIfExists);
        };

        LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
        CHECK(ReadRawFile(primary)==changed && ReadRawFile(backup)==recovery);
        CHECK(ReadRawFile(diagnostic)==captured && DiagnosticCopies(primary).size()==1);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Recovered && retry.revision.sourcePath==backup);
        CHECK(DiagnosticCopies(primary).size()==2);
        CHECK(ReadRawFile(primary+L".corrupt.1700000000.1")==changed);
    }
}

static void test_diagnostic_source_reverify_transient_failure_retries_without_growth(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), backup=primary+L".bak",
        diagnostic=primary+L".corrupt.1700000000.0";
    const std::string corrupt="transient reverify corrupt",
        recovery=ValidLayoutBytes("transient reverify backup");
    CHECK(WriteRawFile(primary,corrupt)); CHECK(WriteRawFile(backup,recovery));
    LayoutFsOps ops;
    auto realOpen=ops.openFile;
    int primaryOpens=0;
    bool injected=false;
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==primary && ++primaryOpens==2){
            injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };

    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(ReadRawFile(primary)==corrupt && ReadRawFile(backup)==recovery);
    CHECK(ReadRawFile(diagnostic)==corrupt && DiagnosticCopies(primary).size()==1);
    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::Recovered && retry.revision.sourcePath==backup);
    CHECK(DiagnosticCopies(primary).size()==1);
}

static void test_atomic_write_first_and_two_successful_writes(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt");
    std::string first=ValidLayoutBytes("first"), second=ValidLayoutBytes("second"), error="stale";
    CHECK(AtomicWriteText(primary,first,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==first);
    CHECK(!RawFileExists(primary+L".bak") && !RawFileExists(primary+L".rollback"));
    CHECK(AtomicWriteText(primary,second,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==second);
    CHECK(ReadRawFile(primary+L".bak")==first);
    CHECK(!RawFileExists(primary+L".rollback"));
}

static void test_durable_write_captures_revision_without_post_publish_read(){
    LayoutTempDir temp;
    const std::wstring primary=temp.file(L"layout.txt");
    const std::string bytes=ValidLayoutBytes("captured-revision");
    std::string error;
    LayoutFsOps ops;
    const auto realRead=ops.readFile;
    bool atomicReturned=false;
    int readsAfterAtomicReturn=0;
    ops.readFile=[&](HANDLE handle,void* buffer,DWORD requested,
                     DWORD& read)->BOOL {
        if(atomicReturned){
            ++readsAfterAtomicReturn;
            throw std::bad_alloc();
        }
        return realRead(handle,buffer,requested,read);
    };
    LayoutRevision current;
    current.sourcePath=L"old-revision";
    current.exists=true;
    bool dirty=true;
    bool pendingPublication=false;
    CHECK(PublishLayoutWithCapturedRevision(
        [&](LayoutRevision& published){
            const bool wrote=AtomicWriteText(
                primary,bytes,&error,false,ops,&published);
            atomicReturned=wrote;
            return wrote;
        },
        [&](LayoutRevision&) noexcept { pendingPublication=true; },
        [&](LayoutRevision& published) noexcept {
            CommitPublishedLayoutRevisionNoThrow(current,published);
            dirty=false;
        })==CapturedLayoutPublishResult::Succeeded);
    CHECK(atomicReturned && error.empty() && !dirty && !pendingPublication);
    CHECK(readsAfterAtomicReturn==0);
    const LayoutRevision actual=ReadLayoutRevisionLocked(primary);
    CHECK(SameRevision(current,actual));
    CHECK(ReadRawFile(primary)==bytes);
}

static void test_durable_publish_exception_adopts_revision_then_retries(){
    LayoutTempDir temp;
    const std::wstring primary=temp.file(L"layout.txt");
    const std::string prior=ValidLayoutBytes("prior-captured");
    const std::string next=ValidLayoutBytes("next-captured");
    std::string error;
    CHECK(AtomicWriteText(primary,prior,&error));
    LayoutRevision current=ReadLayoutRevisionLocked(primary);
    bool dirty=true,pendingAdopted=false;
    LayoutFsOps throwingOps;
    throwingOps.deleteFile=[](const std::wstring&)->BOOL {
        throw std::bad_alloc();
    };
    const CapturedLayoutPublishResult first=
        PublishLayoutWithCapturedRevision(
            [&](LayoutRevision& published){
                return AtomicWriteText(
                    primary,next,&error,true,throwingOps,&published);
            },
            [&](LayoutRevision& published) noexcept {
                pendingAdopted=true;
                CommitPublishedLayoutRevisionNoThrow(current,published);
            },
            [&](LayoutRevision&) noexcept { dirty=false; });
    CHECK(first==CapturedLayoutPublishResult::PublishedNeedsRetry);
    CHECK(pendingAdopted && dirty && ReadRawFile(primary)==next);
    CHECK(SameRevision(current,ReadLayoutRevisionLocked(primary)));

    const CapturedLayoutPublishResult retry=
        PublishLayoutWithCapturedRevision(
            [&](LayoutRevision& published){
                return AtomicWriteText(
                    primary,next,&error,true,&published);
            },
            [&](LayoutRevision& published) noexcept {
                CommitPublishedLayoutRevisionNoThrow(current,published);
            },
            [&](LayoutRevision& published) noexcept {
                CommitPublishedLayoutRevisionNoThrow(current,published);
                dirty=false;
            });
    CHECK(retry==CapturedLayoutPublishResult::Succeeded);
    CHECK(!dirty && SameRevision(current,ReadLayoutRevisionLocked(primary)));
}

static void test_first_post_publish_verify_throw_recovers_from_armed_candidate(){
    LayoutTempDir temp;
    const std::wstring primary=temp.file(L"layout.txt");
    const std::string prior=ValidLayoutBytes("candidate-prior");
    const std::string next=ValidLayoutBytes("candidate-next");
    std::string error;
    CHECK(AtomicWriteText(primary,prior,&error));
    LayoutRevision current=ReadLayoutRevisionLocked(primary);
    const LayoutRevision priorRevision=current;
    LayoutPublishCandidate candidate;
    CHECK(BuildLayoutPublishCandidate(primary,next,candidate));
    CHECK(candidate.armed);

    LayoutFsOps ops;
    const auto realOpen=ops.openFile;
    int primaryReadOpens=0;
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                     DWORD creation,DWORD flags)->HANDLE {
        if(opened==primary && creation==OPEN_EXISTING &&
           ++primaryReadOpens==2) throw std::bad_alloc();
        return realOpen(opened,access,share,creation,flags);
    };
    LayoutRevision uncaptured;
    bool returned=false;
    try {
        returned=AtomicWriteText(
            primary,next,&error,true,ops,&uncaptured);
    } catch(...) { returned=false; }
    CHECK(!returned && !uncaptured.exists);
    CHECK(ReadRawFile(primary)==next);

    LayoutRevision unavailable;
    unavailable.sourcePath=primary;
    unavailable.exists=true;
    CHECK(ObserveLayoutPublishCandidateNoThrow(
              candidate,LayoutRevisionReadStatus::Unavailable,
              unavailable,current)==
          LayoutPublishCandidateObservation::RetainedUnavailable);
    CHECK(candidate.armed && SameRevision(current,priorRevision));

    LayoutRevision exact=ReadLayoutRevisionLocked(primary);
    CHECK(ObserveLayoutPublishCandidateNoThrow(
              candidate,LayoutRevisionReadStatus::Present,exact,current)==
          LayoutPublishCandidateObservation::Adopted);
    CHECK(!candidate.armed &&
          SameRevision(current,ReadLayoutRevisionLocked(primary)));

    LayoutRevision retried;
    CHECK(AtomicWriteText(primary,next,&error,true,&retried));
    CommitPublishedLayoutRevisionNoThrow(current,retried);
    CHECK(SameRevision(current,ReadLayoutRevisionLocked(primary)));
}

static void test_atomic_write_rejects_oversize_without_touching_destination(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt");
    CHECK(WriteRawFile(primary,"old"));
    std::string oversized((size_t)MAX_LAYOUT_FILE_BYTES+1,'x'), error;
    LayoutFsOps ops;
    int openCalls=0;
    auto realOpen=ops.openFile;
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
        ++openCalls; return realOpen(opened,access,share,creation,flags);
    };
    CHECK(!AtomicWriteText(primary,oversized,&error,false,ops));
    CHECK(!error.empty());
    CHECK(openCalls==0);
    CHECK(ReadRawFile(primary)=="old");
    CHECK(!RawFileExists(primary+L".tmp"));
}

static void test_atomic_write_faults_keep_old_or_recovery_bytes(){
    const std::string prior=ValidLayoutBytes("prior"), next=ValidLayoutBytes("next");
    {
        LayoutTempDir temp; std::wstring primary=temp.file(L"write.txt"); CHECK(WriteRawFile(primary,prior));
        LayoutFsOps ops; auto realWrite=ops.writeFile; int calls=0;
        ops.writeFile=[&](HANDLE handle,const void* buffer,DWORD requested,DWORD& written)->BOOL{
            if(calls++==0) return realWrite(handle,buffer,(std::min)(requested,3UL),written);
            written=0; SetLastError(ERROR_WRITE_FAULT); return FALSE;
        };
        std::string error; CHECK(!AtomicWriteText(primary,next,&error,false,ops));
        CHECK(!error.empty() && ReadRawFile(primary)==prior);
    }
    {
        LayoutTempDir temp; std::wstring primary=temp.file(L"flush.txt"); CHECK(WriteRawFile(primary,prior));
        LayoutFsOps ops;
        ops.flushFile=[](HANDLE)->BOOL{ SetLastError(ERROR_WRITE_FAULT); return FALSE; };
        std::string error; CHECK(!AtomicWriteText(primary,next,&error,false,ops));
        CHECK(!error.empty() && ReadRawFile(primary)==prior);
    }
    {
        LayoutTempDir temp; std::wstring primary=temp.file(L"close.txt");
        LayoutFsOps ops; auto realClose=ops.closeHandle; int closes=0;
        ops.closeHandle=[&](HANDLE handle)->BOOL{
            BOOL closed=realClose(handle);
            if(closes++==0){ SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
            return closed;
        };
        std::string error; CHECK(!AtomicWriteText(primary,next,&error,false,ops));
        CHECK(!error.empty());
        CHECK(!RawFileExists(primary));
        CHECK(!RawFileExists(primary+L".tmp"));
        CHECK(ReadRawFile(primary+L".tmp.stage")==next);
    }
    {
        LayoutTempDir temp; std::wstring primary=temp.file(L"replace.txt"); CHECK(WriteRawFile(primary,prior));
        LayoutFsOps ops;
        ops.replaceFile=[](const std::wstring&,const std::wstring&,const std::wstring&,DWORD)->BOOL{
            SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT); return FALSE;
        };
        std::string error; CHECK(!AtomicWriteText(primary,next,&error,false,ops));
        CHECK(!error.empty() && ReadRawFile(primary)==prior);
    }
}

static void test_preexisting_rollback_promotion_failure_never_touches_primary(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    const std::string current=ValidLayoutBytes("current"), pending=ValidLayoutBytes("pending");
    const std::string retainedRollback=ValidLayoutBytes("retained-rollback"), olderBak=ValidLayoutBytes("older-bak");
    CHECK(WriteRawFile(primary,current));
    CHECK(WriteRawFile(rollback,retainedRollback));
    CHECK(WriteRawFile(bak,olderBak));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        if(from==rollback && to==bak){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
        return realMove(from,to,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(!error.empty());
    CHECK(ReadRawFile(primary)==current);
    CHECK(ReadRawFile(rollback)==retainedRollback);
    CHECK(ReadRawFile(bak)==olderBak);
}

static void test_rollback_promotion_readback_failure_restores_old_bak_and_rollback(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    std::wstring previous=bak+L".previous";
    const std::string current=ValidLayoutBytes("current"), pending=ValidLayoutBytes("pending"),
        retainedRollback=ValidLayoutBytes("retained-rollback"), olderBak=ValidLayoutBytes("older-bak");
    CHECK(WriteRawFile(primary,current));
    CHECK(WriteRawFile(rollback,retainedRollback));
    CHECK(WriteRawFile(bak,olderBak));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    auto realOpen=ops.openFile;
    bool promoted=false, injected=false;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        BOOL moved=realMove(from,to,flags);
        if(moved && from==rollback && to==bak) promoted=true;
        return moved;
    };
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
        if(opened==bak && promoted && !injected){
            injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(ReadRawFile(primary)==current);
    CHECK(ReadRawFile(rollback)==retainedRollback);
    CHECK(ReadRawFile(bak)==olderBak);
    CHECK(!RawFileExists(previous));
}

static void test_preexisting_rollback_promotion_mismatch_recovers_from_verified_marker(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous", promote=previous+L".promote",
        tempPath=primary+L".tmp";
    const std::string current=ValidLayoutBytes("current"), pending=ValidLayoutBytes("pending"),
        retainedRollback=ValidLayoutBytes("retained-rollback"), oldBackup=ValidLayoutBytes("old-backup"),
        poison="poisoned promoted rollback";
    CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(rollback,retainedRollback));
    CHECK(WriteRawFile(bak,oldBackup));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    bool injected=false;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        BOOL moved=realMove(from,to,flags);
        if(moved && from==rollback && to==bak && !injected){
            injected=true; CHECK(WriteRawFile(to,poison));
        }
        return moved;
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(ReadRawFile(primary)==current && ReadRawFile(promote)==retainedRollback);
    CHECK(ReadRawFile(rollback)==poison && ReadRawFile(bak)==oldBackup);
    CHECK(ReadRawFile(previous)==oldBackup);

    LayoutFsOps stopAfterResolveOps;
    auto realOpen=stopAfterResolveOps.openFile;
    stopAfterResolveOps.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==tempPath && creation==CREATE_ALWAYS){
            SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    CHECK(!AtomicWriteText(primary,pending,&error,false,stopAfterResolveOps));
    CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==retainedRollback);
    CHECK(!RawFileExists(rollback) && !RawFileExists(previous) && !RawFileExists(promote));
    CHECK(AtomicWriteText(primary,pending,&error));
    CHECK(error.empty() && ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
}

static void test_post_replace_verification_failure_reports_false_with_bak_readable(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
    const std::string prior=ValidLayoutBytes("prior"), next=ValidLayoutBytes("next");
    CHECK(WriteRawFile(primary,prior));
    LayoutFsOps ops;
    auto realOpen=ops.openFile;
    int primaryReadOpens=0;
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
        if(opened==primary && creation==OPEN_EXISTING && ++primaryReadOpens==2){
            SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,next,&error,false,ops));
    CHECK(!error.empty());
    CHECK(ReadRawFile(primary)==next);
    CHECK(ReadRawFile(bak)==prior);
}

static void test_late_normal_write_failure_retains_staged_old_backup(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous";
    const std::string current=ValidLayoutBytes("current"), pending=ValidLayoutBytes("pending"),
        oldBackup=ValidLayoutBytes("old-backup");
    CHECK(WriteRawFile(primary,current));
    CHECK(WriteRawFile(bak,oldBackup));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    auto realOpen=ops.openFile;
    bool finalPromotion=false, injected=false;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        BOOL moved=realMove(from,to,flags);
        if(moved && from==rollback && to==bak) finalPromotion=true;
        return moved;
    };
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
        if(opened==bak && finalPromotion && !injected){
            injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(ReadRawFile(primary)==pending);
    CHECK(ReadRawFile(bak)==current);
    CHECK(ReadRawFile(previous)==oldBackup);
    CHECK(AtomicWriteText(primary,pending,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
    CHECK(!RawFileExists(previous));
}

static void test_retry_after_publish_finishes_pending_promotion_without_self_replace(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    const std::string current=ValidLayoutBytes("current"), pending=ValidLayoutBytes("pending");
    CHECK(WriteRawFile(primary,current));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    bool injected=false;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        if(from==rollback && to==bak && !injected){
            injected=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
        }
        return realMove(from,to,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(ReadRawFile(primary)==pending && ReadRawFile(rollback)==current);
    CHECK(!RawFileExists(bak));
    CHECK(AtomicWriteText(primary,pending,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
    CHECK(!RawFileExists(rollback));
}

static void test_replace_failure_retains_staged_old_backup_until_retry(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous";
    const std::string current=ValidLayoutBytes("current"), pending=ValidLayoutBytes("pending"),
        retainedRollback=ValidLayoutBytes("retained-rollback"), oldBackup=ValidLayoutBytes("old-backup");
    CHECK(WriteRawFile(primary,current));
    CHECK(WriteRawFile(rollback,retainedRollback));
    CHECK(WriteRawFile(bak,oldBackup));
    LayoutFsOps ops;
    ops.replaceFile=[](const std::wstring&,const std::wstring&,const std::wstring&,DWORD)->BOOL{
        SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT); return FALSE;
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(!error.empty());
    CHECK(ReadRawFile(primary)==current);
    CHECK(!RawFileExists(rollback));
    CHECK(ReadRawFile(bak)==retainedRollback);
    CHECK(ReadRawFile(previous)==oldBackup);
    CHECK(AtomicWriteText(primary,pending,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
    CHECK(!RawFileExists(rollback) && !RawFileExists(previous));
}

static void test_staged_backup_resolver_restores_missing_backup_before_cleanup(){
    for(int withRollback=0;withRollback<2;++withRollback){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
            bak=primary+L".bak", previous=bak+L".previous", tempPath=primary+L".tmp";
        const std::string current=ValidLayoutBytes("current"), staged=ValidLayoutBytes("staged"),
            recovery=ValidLayoutBytes("rollback"), pending=ValidLayoutBytes("pending");
        CHECK(WriteRawFile(primary,current));
        CHECK(WriteRawFile(previous,staged));
        if(withRollback) CHECK(WriteRawFile(rollback,recovery));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,DWORD creation,DWORD flags)->HANDLE{
            if(opened==tempPath && creation==CREATE_ALWAYS){
                SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
        CHECK(!error.empty());
        CHECK(ReadRawFile(primary)==current);
        CHECK(ReadRawFile(bak)==(withRollback ? recovery : staged));
        CHECK(!RawFileExists(previous));
        CHECK(!RawFileExists(rollback));
    }
}

static void test_changed_request_first_reconciles_pending_normal_transaction(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous", tempPath=primary+L".tmp";
    const std::string published=ValidLayoutBytes("published"), prior=ValidLayoutBytes("prior"),
        olderRollback=ValidLayoutBytes("older-rollback"), oldBackup=ValidLayoutBytes("old-backup"),
        changed=ValidLayoutBytes("changed");
    CHECK(WriteRawFile(primary,published));
    CHECK(WriteRawFile(rollback,prior));
    CHECK(WriteRawFile(bak,olderRollback));
    CHECK(WriteRawFile(previous,oldBackup));
    LayoutFsOps stopAfterReconcileOps;
    auto realOpen=stopAfterReconcileOps.openFile;
    stopAfterReconcileOps.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==tempPath && creation==CREATE_ALWAYS){
            SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,changed,&error,false,stopAfterReconcileOps));
    CHECK(!error.empty());
    CHECK(ReadRawFile(primary)==published && ReadRawFile(bak)==prior);
    CHECK(!RawFileExists(rollback) && !RawFileExists(previous));
    CHECK(AtomicWriteText(primary,changed,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==changed && ReadRawFile(bak)==published);
    CHECK(!RawFileExists(rollback) && !RawFileExists(previous));
}

static void test_failed_backup_staging_readback_does_not_poison_retries(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous", stage=previous+L".stage";
    const std::string current=ValidLayoutBytes("current"), recovery=ValidLayoutBytes("recovery"),
        oldBackup=ValidLayoutBytes("old-backup"), pending=ValidLayoutBytes("pending"), poison="poison";
    CHECK(WriteRawFile(primary,current));
    CHECK(WriteRawFile(rollback,recovery));
    CHECK(WriteRawFile(bak,oldBackup));
    LayoutFsOps ops;
    auto realCopy=ops.copyFile;
    bool injected=false;
    ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        BOOL copied=realCopy(from,to,failIfExists);
        if(copied && to==stage && !injected){
            injected=true; CHECK(WriteRawFile(to,poison));
        }
        return copied;
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(ReadRawFile(primary)==current && ReadRawFile(rollback)==recovery && ReadRawFile(bak)==oldBackup);
    CHECK(!RawFileExists(previous));
    CHECK(AtomicWriteText(primary,pending,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
    CHECK(!RawFileExists(rollback) && !RawFileExists(previous) && !RawFileExists(stage));
}

static void test_pending_promotion_marker_recovers_post_move_verification_fault(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous", promote=previous+L".promote",
        tempPath=primary+L".tmp";
    const std::string published=ValidLayoutBytes("published"), prior=ValidLayoutBytes("prior"),
        older=ValidLayoutBytes("older"), oldest=ValidLayoutBytes("oldest"),
        changed=ValidLayoutBytes("changed");
    CHECK(WriteRawFile(primary,published)); CHECK(WriteRawFile(rollback,prior));
    CHECK(WriteRawFile(bak,older)); CHECK(WriteRawFile(previous,oldest));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    auto realOpen=ops.openFile;
    bool moved=false, injected=false;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        BOOL result=realMove(from,to,flags);
        if(result && from==rollback && to==bak) moved=true;
        return result;
    };
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==bak && moved && !injected){
            injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,changed,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(ReadRawFile(primary)==published && ReadRawFile(promote)==prior && ReadRawFile(previous)==oldest);
    CHECK(!RawFileExists(rollback) && ReadRawFile(bak)==prior);

    LayoutFsOps stopAfterResolveOps;
    auto secondRealOpen=stopAfterResolveOps.openFile;
    stopAfterResolveOps.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==tempPath && creation==CREATE_ALWAYS){
            SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
        }
        return secondRealOpen(opened,access,share,creation,flags);
    };
    CHECK(!AtomicWriteText(primary,changed,&error,false,stopAfterResolveOps));
    CHECK(ReadRawFile(primary)==published && ReadRawFile(bak)==prior);
    CHECK(!RawFileExists(rollback) && !RawFileExists(previous) && !RawFileExists(promote));
    CHECK(AtomicWriteText(primary,changed,&error));
    CHECK(error.empty() && ReadRawFile(primary)==changed && ReadRawFile(bak)==published);
}

static void test_promotion_marker_false_after_poison_is_never_authoritative(){
    for(int phase=0;phase<3;++phase){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
            bak=primary+L".bak", previous=bak+L".previous", promote=previous+L".promote",
            promoteStage=promote+L".stage";
        const std::string current=ValidLayoutBytes("current"), recovery=ValidLayoutBytes("recovery"),
            oldBackup=ValidLayoutBytes("old-backup"), pending=ValidLayoutBytes("pending"), poison="poison";
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,oldBackup));
        if(phase<2) CHECK(WriteRawFile(rollback,recovery));
        if(phase==1) CHECK(WriteRawFile(previous,oldBackup));
        LayoutFsOps ops;
        auto realCopy=ops.copyFile;
        bool injected=false;
        ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
            if(to==promoteStage && !injected){
                BOOL copied=realCopy(from,to,failIfExists);
                CHECK(copied!=0); CHECK(WriteRawFile(to,poison));
                injected=true; SetLastError(ERROR_GEN_FAILURE); return FALSE;
            }
            return realCopy(from,to,failIfExists);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,pending,&error,false,ops));
        CHECK(injected && !error.empty());
        CHECK(!RawFileExists(promote));
        CHECK(ReadRawFile(promoteStage)==poison);
        if(phase<2){
            CHECK(ReadRawFile(primary)==current && ReadRawFile(rollback)==recovery &&
                ReadRawFile(bak)==oldBackup);
        } else {
            CHECK(ReadRawFile(primary)==pending && ReadRawFile(rollback)==current &&
                ReadRawFile(bak)==oldBackup);
        }
        CHECK(AtomicWriteText(primary,pending,&error));
        CHECK(error.empty());
        CHECK(ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
        CHECK(!RawFileExists(rollback) && !RawFileExists(previous) &&
            !RawFileExists(promote) && !RawFileExists(promoteStage));
    }
}

static void test_transient_promoted_backup_read_is_retained_and_loads_before_retry(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        bak=primary+L".bak", previous=bak+L".previous", promote=previous+L".promote";
    const std::string prior=ValidLayoutBytes("prior"), older=ValidLayoutBytes("older"),
        oldest=ValidLayoutBytes("oldest"), replacement=ValidLayoutBytes("replacement");
    CHECK(WriteRawFile(rollback,prior)); CHECK(WriteRawFile(bak,older)); CHECK(WriteRawFile(previous,oldest));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    auto realOpen=ops.openFile;
    bool moved=false, injected=false;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        BOOL result=realMove(from,to,flags);
        if(result && from==rollback && to==bak) moved=true;
        return result;
    };
    ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==bak && moved && !injected){
            injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,replacement,&error,false,ops));
    CHECK(injected && !error.empty());
    CHECK(!RawFileExists(primary) && !RawFileExists(rollback));
    CHECK(ReadRawFile(bak)==prior && ReadRawFile(promote)==prior && ReadRawFile(previous)==oldest);
    LayoutLoadResult beforeRetry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(beforeRetry.status==LayoutLoadStatus::Recovered && beforeRetry.writesAllowed);
    CHECK(beforeRetry.revision.sourcePath==bak && LoadedDesktopName(beforeRetry)=="prior");
    CHECK(AtomicWriteText(primary,replacement,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==prior);
}

static void test_loader_recovers_valid_authoritative_internal_marker(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
        promote=primary+L".bak.previous.promote";
    const std::string intended=ValidLayoutBytes("orphan-marker");
    CHECK(WriteRawFile(promote,intended));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.writesAllowed && loaded.usable());
    CHECK(loaded.revision.sourcePath==bak && LoadedDesktopName(loaded)=="orphan-marker");
    CHECK(ReadRawFile(bak)==intended && ReadRawFile(promote)==intended);
}

static void test_loader_never_accepts_stale_bak_beside_authoritative_promotion_marker(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
        previous=bak+L".previous", promote=previous+L".promote";
    const std::string intended=ValidLayoutBytes("intended"), stale=ValidLayoutBytes("stale"),
        oldest=ValidLayoutBytes("oldest");
    CHECK(WriteRawFile(bak,stale)); CHECK(WriteRawFile(previous,oldest));
    CHECK(WriteRawFile(promote,intended));
    LayoutFsOps ops;
    auto realCopy=ops.copyFile;
    bool injected=false;
    ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        if(from==promote && to==bak && !injected){
            injected=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
        }
        return realCopy(from,to,failIfExists);
    };
    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
    CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(ReadRawFile(bak)==stale && ReadRawFile(promote)==intended);

    LayoutLoadResult recovered=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(recovered.status==LayoutLoadStatus::Recovered && recovered.writesAllowed);
    CHECK(recovered.revision.sourcePath==bak && LoadedDesktopName(recovered)=="intended");
    CHECK(ReadRawFile(bak)==intended && ReadRawFile(promote)==intended);
}

static void test_valid_primary_reconciles_authoritative_promotion_marker(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
            bak=primary+L".bak", promote=bak+L".previous.promote";
        const std::string current=ValidLayoutBytes("valid-primary"),
            prior=ValidLayoutBytes("prior-from-rollback"), stale=ValidLayoutBytes("stale-backup");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(rollback,prior));
        CHECK(WriteRawFile(bak,stale)); CHECK(WriteRawFile(promote,prior));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Valid && loaded.writesAllowed && loaded.usable());
        CHECK(loaded.revision.sourcePath==primary && LoadedDesktopName(loaded)=="valid-primary");
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
        CHECK(!RawFileExists(rollback) && !RawFileExists(promote));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            promote=bak+L".previous.promote";
        const std::string current=ValidLayoutBytes("exact-primary"), prior=ValidLayoutBytes("exact-backup");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
        CHECK(WriteRawFile(promote,prior));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Valid && loaded.writesAllowed);
        CHECK(loaded.revision.sourcePath==primary && LoadedDesktopName(loaded)=="exact-primary");
        CHECK(ReadRawFile(bak)==prior && !RawFileExists(promote));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            promote=bak+L".previous.promote";
        const std::string current=ValidLayoutBytes("missing-backup-primary"),
            prior=ValidLayoutBytes("marker-repair");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(promote,prior));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Valid && loaded.writesAllowed);
        CHECK(loaded.revision.sourcePath==primary && LoadedDesktopName(loaded)=="missing-backup-primary");
        CHECK(ReadRawFile(bak)==prior && !RawFileExists(promote));
    }
}

static void test_valid_primary_marker_faults_fail_closed_then_converge(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            promote=bak+L".previous.promote";
        const std::string current=ValidLayoutBytes("corrupt-marker-primary"), corrupt="not a layout";
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(promote,corrupt));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Unavailable && !loaded.writesAllowed && !loaded.usable());
        CHECK(ReadRawFile(primary)==current && ReadRawFile(promote)==corrupt && !RawFileExists(bak));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            promote=bak+L".previous.promote";
        const std::string current=ValidLayoutBytes("transient-marker-primary"),
            prior=ValidLayoutBytes("transient-marker-prior");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(promote,prior));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==promote){ SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE; }
            return realOpen(opened,access,share,creation,flags);
        };
        LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
        CHECK(ReadRawFile(primary)==current && ReadRawFile(promote)==prior && !RawFileExists(bak));
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed);
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior && !RawFileExists(promote));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            promote=bak+L".previous.promote";
        const std::string current=ValidLayoutBytes("repair-fault-primary"),
            prior=ValidLayoutBytes("repair-fault-prior"), stale=ValidLayoutBytes("repair-fault-stale");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,stale));
        CHECK(WriteRawFile(promote,prior));
        LayoutFsOps ops;
        auto realCopy=ops.copyFile;
        bool injected=false;
        ops.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
            if(from==promote && to==bak && !injected){
                injected=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
            }
            return realCopy(from,to,failIfExists);
        };
        LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==stale && ReadRawFile(promote)==prior);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed);
        CHECK(retry.revision.sourcePath==primary && ReadRawFile(bak)==prior && !RawFileExists(promote));
    }
}

static void test_valid_primary_retries_cleanup_after_trigger_delete_false_effect(){
    for(int deletePromotionMarker=0;deletePromotionMarker<2;++deletePromotionMarker){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            previous=bak+L".previous", promote=previous+L".promote",
            promoteStage=promote+L".stage";
        const std::string current=ValidLayoutBytes(deletePromotionMarker ?
                "marker-delete-current" : "stage-delete-current"),
            prior=ValidLayoutBytes(deletePromotionMarker ?
                "marker-delete-prior" : "stage-delete-prior"),
            staged=ValidLayoutBytes("staged-old-backup"), poison="non-authoritative stage";
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
        CHECK(WriteRawFile(previous,staged));
        const std::wstring& trigger=deletePromotionMarker ? promote : promoteStage;
        CHECK(WriteRawFile(trigger,deletePromotionMarker ? prior : poison));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        bool injected=false;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==trigger && !injected){
                CHECK(realDelete(deleted)!=0); injected=true;
                SetLastError(ERROR_ACCESS_DENIED); return FALSE;
            }
            return realDelete(deleted);
        };
        LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && first.status==LayoutLoadStatus::Unavailable && !first.writesAllowed);
        CHECK(!RawFileExists(trigger) && RawFileExists(previous));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);

        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed && retry.usable());
        CHECK(retry.revision.sourcePath==primary && LoadedDesktopName(retry)==
            (deletePromotionMarker ? "marker-delete-current" : "stage-delete-current"));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
        CHECK(!RawFileExists(previous) && !RawFileExists(promote) && !RawFileExists(promoteStage));
    }
}

static void test_first_publish_tmp_recovers_on_restart_after_move_failure(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), tempPath=primary+L".tmp";
    const std::string intended=ValidLayoutBytes("first-publish");
    LayoutFsOps writeOps;
    auto realMove=writeOps.moveFile;
    bool writeMoveFailed=false;
    writeOps.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        if(from==tempPath && to==primary && !writeMoveFailed){
            writeMoveFailed=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
        }
        return realMove(from,to,flags);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,intended,&error,false,writeOps));
    CHECK(writeMoveFailed && !error.empty() && !RawFileExists(primary));
    CHECK(ReadRawFile(tempPath)==intended);

    LayoutFsOps loadOps;
    auto loadRealMove=loadOps.moveFile;
    bool restartMoveFailed=false;
    loadOps.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        if(from==tempPath && to==primary && !restartMoveFailed){
            restartMoveFailed=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
        }
        return loadRealMove(from,to,flags);
    };
    LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,loadOps);
    CHECK(restartMoveFailed && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
    CHECK(!RawFileExists(primary) && ReadRawFile(tempPath)==intended);

    LayoutLoadResult recovered=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(recovered.status==LayoutLoadStatus::Recovered && recovered.writesAllowed);
    CHECK(recovered.revision.sourcePath==primary && LoadedDesktopName(recovered)=="first-publish");
    CHECK(ReadRawFile(primary)==intended && !RawFileExists(tempPath));
    LayoutLoadResult settled=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(settled.status==LayoutLoadStatus::Valid && LoadedDesktopName(settled)=="first-publish");
}

static void test_partial_valid_prefix_is_never_treated_as_committed_first_publish(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), committed=primary+L".tmp",
        stage=committed+L".stage";
    const std::string prefix="# VDE snapshot v4\n", intended=ValidLayoutBytes("must-not-disappear");
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> wins;
    std::string parseError;
    int sourceVersion=0;
    CHECK(ParseLayout(prefix,desks,wins,1700000000,&parseError,&sourceVersion));
    CHECK(desks.empty() && wins.empty() && sourceVersion==4);
    LayoutFsOps ops;
    auto realWrite=ops.writeFile;
    bool injected=false;
    ops.writeFile=[&](HANDLE file,const void* buffer,DWORD requested,DWORD& written)->BOOL{
        if(!injected){
            DWORD prefixWritten=0;
            BOOL wrote=realWrite(file,buffer,(DWORD)prefix.size(),prefixWritten);
            CHECK(wrote!=0 && prefixWritten==prefix.size() && requested>prefixWritten);
            written=prefixWritten;
            injected=true;
            SetLastError(ERROR_WRITE_FAULT);
            return FALSE;
        }
        return realWrite(file,buffer,requested,written);
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,intended,&error,false,ops));
    CHECK(injected && !error.empty() && !RawFileExists(primary));
    CHECK(!RawFileExists(committed) && ReadRawFile(stage)==prefix);

    LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(first.status==LayoutLoadStatus::Missing && first.writesAllowed && !first.usable());
    CHECK(!RawFileExists(primary) && !RawFileExists(committed) && !RawFileExists(stage));
    LayoutLoadResult again=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(again.status==LayoutLoadStatus::Missing && again.writesAllowed);
}

static void test_temporary_commit_move_failure_converges_before_and_after_effect(){
    for(int afterEffect=0;afterEffect<2;++afterEffect){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), committed=primary+L".tmp",
            stage=committed+L".stage";
        const std::string intended=ValidLayoutBytes(afterEffect ? "after-effect" : "before-effect");
        LayoutFsOps ops;
        auto realMove=ops.moveFile;
        bool injected=false;
        ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
            if(from==stage && to==committed && !injected){
                if(afterEffect) CHECK(realMove(from,to,flags)!=0);
                injected=true;
                SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT);
                return FALSE;
            }
            return realMove(from,to,flags);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,intended,&error,false,ops));
        CHECK(injected && !error.empty() && !RawFileExists(primary));
        CHECK(afterEffect ? ReadRawFile(committed)==intended : ReadRawFile(stage)==intended);
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        if(afterEffect){
            CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.writesAllowed);
            CHECK(LoadedDesktopName(loaded)=="after-effect" && ReadRawFile(primary)==intended);
        } else {
            CHECK(loaded.status==LayoutLoadStatus::Missing && loaded.writesAllowed);
            CHECK(!RawFileExists(primary));
        }
        CHECK(!RawFileExists(committed) && !RawFileExists(stage));
    }
}

static void test_idempotent_retry_cleans_orphan_prior_backup_stage(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
        previous=bak+L".previous", stage=previous+L".stage";
    const std::string current=ValidLayoutBytes("current"), oldBackup=ValidLayoutBytes("old-backup");
    CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,oldBackup));
    CHECK(WriteRawFile(previous,oldBackup)); CHECK(WriteRawFile(stage,oldBackup));
    std::string error;
    CHECK(AtomicWriteText(primary,current,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==oldBackup);
    CHECK(!RawFileExists(previous) && !RawFileExists(stage));
}

static void test_orphan_promotion_stage_is_non_authoritative_and_converges(){
    for(int withPrevious=0;withPrevious<2;++withPrevious){
        for(int changedRequest=0;changedRequest<2;++changedRequest){
            LayoutTempDir temp;
            std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
                previous=bak+L".previous", promoteStage=previous+L".promote.stage";
            const std::string current=ValidLayoutBytes("orphan-current"),
                prior=ValidLayoutBytes("orphan-prior"), changed=ValidLayoutBytes("orphan-changed"),
                poison=ValidLayoutBytes("uncommitted-stage-poison");
            CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
            if(withPrevious) CHECK(WriteRawFile(previous,prior));
            CHECK(WriteRawFile(promoteStage,poison));
            const std::string& requested=changedRequest ? changed : current;
            std::string error;
            CHECK(AtomicWriteText(primary,requested,&error));
            CHECK(error.empty() && ReadRawFile(primary)==requested);
            CHECK(ReadRawFile(bak)==(changedRequest ? current : prior));
            CHECK(!RawFileExists(promoteStage) && !RawFileExists(previous));
        }
    }

    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            previous=bak+L".previous", promoteStage=previous+L".promote.stage";
        const std::string current=ValidLayoutBytes("delete-effect-current"),
            prior=ValidLayoutBytes("delete-effect-prior"), changed=ValidLayoutBytes("delete-effect-next"),
            poison=ValidLayoutBytes("delete-effect-stage");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
        CHECK(WriteRawFile(previous,prior)); CHECK(WriteRawFile(promoteStage,poison));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        bool injected=false;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==promoteStage && !injected){
                CHECK(realDelete(deleted)!=0); injected=true;
                SetLastError(ERROR_ACCESS_DENIED); return FALSE;
            }
            return realDelete(deleted);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,changed,&error,false,ops));
        CHECK(injected && !error.empty() && !RawFileExists(promoteStage));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior && ReadRawFile(previous)==prior);
        CHECK(AtomicWriteText(primary,changed,&error));
        CHECK(error.empty() && ReadRawFile(primary)==changed && ReadRawFile(bak)==current);
        CHECK(!RawFileExists(previous));
    }

    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            previous=bak+L".previous", promoteStage=previous+L".promote.stage";
        const std::string current=ValidLayoutBytes("transient-current"),
            prior=ValidLayoutBytes("transient-prior"), poison=ValidLayoutBytes("transient-stage");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
        CHECK(WriteRawFile(promoteStage,poison));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        int backupOpens=0;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==bak && ++backupOpens==2){
                SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,current,&error,false,ops));
        CHECK(!error.empty() && RawFileExists(promoteStage));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
        CHECK(AtomicWriteText(primary,current,&error));
        CHECK(error.empty() && !RawFileExists(promoteStage));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
    }
}

static void test_non_authoritative_stages_are_discarded_without_reading_bytes(){
    for(int stageKind=0;stageKind<2;++stageKind){
        for(int withPrevious=0;withPrevious<2;++withPrevious){
            LayoutTempDir temp;
            std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
                previous=bak+L".previous", stage=stageKind ?
                    previous+L".promote.stage" : previous+L".stage";
            const std::string current=ValidLayoutBytes("oversized-stage-current"),
                prior=ValidLayoutBytes("oversized-stage-prior"), older=ValidLayoutBytes("oversized-stage-older");
            CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
            if(withPrevious) CHECK(WriteRawFile(previous,older));
            CHECK(ResizeRawFile(stage,MAX_LAYOUT_FILE_BYTES+1));
            LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
            CHECK(loaded.status==LayoutLoadStatus::Valid && loaded.writesAllowed && loaded.usable());
            CHECK(loaded.revision.sourcePath==primary && LoadedDesktopName(loaded)=="oversized-stage-current");
            CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
            CHECK(!RawFileExists(stage) && !RawFileExists(previous));
        }

        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            previous=bak+L".previous", stage=stageKind ?
                previous+L".promote.stage" : previous+L".stage";
        const std::string current=ValidLayoutBytes("unreadable-stage-current"),
            prior=ValidLayoutBytes("unreadable-stage-prior"), poison="untrusted poison";
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior)); CHECK(WriteRawFile(stage,poison));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        int stageOpenAttempts=0;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==stage){
                ++stageOpenAttempts; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        std::string error;
        CHECK(AtomicWriteText(primary,current,&error,false,ops));
        CHECK(error.empty() && stageOpenAttempts==0 && !RawFileExists(stage));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
    }
}

static void test_non_authoritative_stage_faults_fail_closed_and_retry(){
    for(int stageKind=0;stageKind<2;++stageKind){
        for(int afterEffect=0;afterEffect<2;++afterEffect){
            LayoutTempDir temp;
            std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
                previous=bak+L".previous", stage=stageKind ?
                    previous+L".promote.stage" : previous+L".stage";
            const std::string current=ValidLayoutBytes("stage-delete-current"),
                prior=ValidLayoutBytes("stage-delete-prior"), poison="stage-delete-poison";
            CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior)); CHECK(WriteRawFile(stage,poison));
            LayoutFsOps ops;
            auto realDelete=ops.deleteFile;
            bool injected=false;
            ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
                if(deleted==stage && !injected){
                    if(afterEffect) CHECK(realDelete(deleted)!=0);
                    injected=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
                }
                return realDelete(deleted);
            };
            LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000,ops);
            CHECK(injected && first.status==LayoutLoadStatus::Unavailable && !first.writesAllowed);
            CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
            CHECK(afterEffect ? !RawFileExists(stage) : RawFileExists(stage));
            LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
            CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed);
            CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior && !RawFileExists(stage));
        }

        {
            LayoutTempDir temp;
            std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
                previous=bak+L".previous", stage=stageKind ?
                    previous+L".promote.stage" : previous+L".stage";
            const std::string current=ValidLayoutBytes("stage-transient-current"),
                prior=ValidLayoutBytes("stage-transient-prior"), poison="stage-transient-poison";
            CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior)); CHECK(WriteRawFile(stage,poison));
            LayoutFsOps ops;
            auto realOpen=ops.openFile;
            int backupOpens=0;
            ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                    DWORD creation,DWORD flags)->HANDLE{
                if(opened==bak && ++backupOpens==2){
                    SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
                }
                return realOpen(opened,access,share,creation,flags);
            };
            LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
            CHECK(blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
            CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior && RawFileExists(stage));
            LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
            CHECK(retry.status==LayoutLoadStatus::Valid && !RawFileExists(stage));
        }

        {
            LayoutTempDir temp;
            std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
                previous=bak+L".previous", stage=stageKind ?
                    previous+L".promote.stage" : previous+L".stage";
            const std::string requested=ValidLayoutBytes("no-stage-survivor"), poison="orphan poison";
            CHECK(WriteRawFile(stage,poison));
            std::string error;
            CHECK(!AtomicWriteText(primary,requested,&error,false));
            CHECK(!error.empty() && !RawFileExists(primary) && !RawFileExists(bak));
            CHECK(ReadRawFile(stage)==poison && !RawFileExists(previous));
        }
    }
}

static void test_previous_stage_cleanup_reverifies_before_consuming_previous(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            previous=bak+L".previous", stage=previous+L".stage";
        const std::string current=ValidLayoutBytes("boundary-current"),
            prior=ValidLayoutBytes("boundary-prior"), poison="boundary poison";
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
        CHECK(WriteRawFile(previous,prior)); CHECK(WriteRawFile(stage,poison));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        auto realOpen=ops.openFile;
        bool stageDeleted=false,injected=false;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            BOOL result=realDelete(deleted);
            if(result && deleted==stage) stageDeleted=true;
            return result;
        };
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==bak && stageDeleted && !injected){
                injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
        CHECK(!RawFileExists(stage) && ReadRawFile(previous)==prior);
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed);
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
        CHECK(!RawFileExists(stage) && !RawFileExists(previous));
    }

    for(int afterEffect=0;afterEffect<2;++afterEffect){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            previous=bak+L".previous", stage=previous+L".stage";
        const std::string current=ValidLayoutBytes("delete-boundary-current"),
            prior=ValidLayoutBytes("delete-boundary-prior"), poison="delete boundary poison";
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(bak,prior));
        CHECK(WriteRawFile(previous,prior)); CHECK(WriteRawFile(stage,poison));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        bool injected=false;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==stage && !injected){
                if(afterEffect) CHECK(realDelete(deleted)!=0);
                injected=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
            }
            return realDelete(deleted);
        };
        LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && first.status==LayoutLoadStatus::Unavailable && !first.writesAllowed);
        CHECK(ReadRawFile(previous)==prior && ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
        CHECK(afterEffect ? !RawFileExists(stage) : RawFileExists(stage));
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed);
        CHECK(!RawFileExists(stage) && !RawFileExists(previous));
        CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==prior);
    }
}

static void test_staged_backup_restore_mismatch_never_discards_intended_bytes_on_retry(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
        previous=bak+L".previous", restore=previous+L".restore", tempPath=primary+L".tmp";
    const std::string current=ValidLayoutBytes("current"), staged=ValidLayoutBytes("staged"),
        pending=ValidLayoutBytes("pending"), corrupted="corrupted restore readback";
    CHECK(WriteRawFile(primary,current));
    CHECK(WriteRawFile(previous,staged));
    LayoutFsOps corruptCopyOps;
    auto realCopy=corruptCopyOps.copyFile;
    bool corruptedCopy=false;
    corruptCopyOps.copyFile=[&](const std::wstring& from,const std::wstring& to,BOOL failIfExists)->BOOL{
        BOOL copied=realCopy(from,to,failIfExists);
        if(copied && from==previous && to==restore){
            corruptedCopy=true;
            CHECK(WriteRawFile(to,corrupted));
        }
        return copied;
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,pending,&error,false,corruptCopyOps));
    CHECK(corruptedCopy && !error.empty());
    CHECK(ReadRawFile(primary)==current && ReadRawFile(previous)==staged);
    CHECK(!RawFileExists(bak));

    LayoutFsOps stopAfterResolveOps;
    auto realOpen=stopAfterResolveOps.openFile;
    stopAfterResolveOps.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
            DWORD creation,DWORD flags)->HANDLE{
        if(opened==tempPath && creation==CREATE_ALWAYS){
            SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
        }
        return realOpen(opened,access,share,creation,flags);
    };
    CHECK(!AtomicWriteText(primary,pending,&error,false,stopAfterResolveOps));
    CHECK(!error.empty());
    CHECK(ReadRawFile(primary)==current && ReadRawFile(bak)==staged);
    CHECK(!RawFileExists(previous));
    CHECK(AtomicWriteText(primary,pending,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==pending && ReadRawFile(bak)==current);
}

static void test_partial_effect_replace_failure_remains_recoverable(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"normal.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
        const std::string prior=ValidLayoutBytes("prior"), next=ValidLayoutBytes("next"),
            changed=ValidLayoutBytes("changed"), older=ValidLayoutBytes("older");
        CHECK(WriteRawFile(primary,prior));
        CHECK(WriteRawFile(bak,older));
        LayoutFsOps ops;
        ops.replaceFile=[&](const std::wstring& replaced,const std::wstring&,
                const std::wstring& backupName,DWORD)->BOOL{
            BOOL moved=MoveFileExW(replaced.c_str(),backupName.c_str(),
                MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
            CHECK(moved!=0);
            SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
            return FALSE;
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,next,&error,false,ops));
        CHECK(!error.empty());
        CHECK(!RawFileExists(primary));
        CHECK(ReadRawFile(rollback)==prior);
        CHECK(ReadRawFile(bak)==older);
        CHECK(ReadRawFile(primary+L".tmp")==next);
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered);
        CHECK(loaded.revision.sourcePath==rollback && LoadedDesktopName(loaded)=="prior");
        CHECK(AtomicWriteText(primary,changed,&error));
        CHECK(error.empty() && ReadRawFile(primary)==changed && ReadRawFile(bak)==prior);
        CHECK(!RawFileExists(rollback) && !RawFileExists(primary+L".tmp"));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"recovery.txt"), bak=primary+L".bak", displaced=primary+L".displaced";
        const std::string corrupt="corrupt primary", good=ValidLayoutBytes("known-good"),
            replacement=ValidLayoutBytes("replacement");
        CHECK(WriteRawFile(primary,corrupt));
        CHECK(WriteRawFile(bak,good));
        LayoutFsOps ops;
        ops.replaceFile=[&](const std::wstring& replaced,const std::wstring&,
                const std::wstring& backupName,DWORD)->BOOL{
            BOOL moved=MoveFileExW(replaced.c_str(),backupName.c_str(),
                MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
            CHECK(moved!=0);
            SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
            return FALSE;
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,replacement,&error,true,ops));
        CHECK(!error.empty());
        CHECK(!RawFileExists(primary));
        CHECK(ReadRawFile(displaced)==corrupt);
        CHECK(ReadRawFile(bak)==good);
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.revision.sourcePath==bak);
        CHECK(AtomicWriteText(primary,replacement,&error,true));
        CHECK(error.empty());
        CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==good);
        CHECK(!RawFileExists(displaced));
    }
}

static void test_first_write_preserves_existing_recovery_artifacts(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    const std::string recovery=ValidLayoutBytes("rollback"), older=ValidLayoutBytes("bak");
    const std::string created=ValidLayoutBytes("created");
    CHECK(WriteRawFile(rollback,recovery));
    CHECK(WriteRawFile(bak,older));
    std::string error;
    CHECK(AtomicWriteText(primary,created,&error));
    CHECK(error.empty());
    CHECK(ReadRawFile(primary)==created);
    CHECK(ReadRawFile(rollback)==recovery);
    CHECK(ReadRawFile(bak)==older);
}

static void test_failed_rollback_promotion_stays_recoverable_before_older_bak(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback", bak=primary+L".bak";
    std::string first=ValidLayoutBytes("first"), second=ValidLayoutBytes("second"), third=ValidLayoutBytes("third"), error;
    CHECK(AtomicWriteText(primary,first,&error));
    CHECK(AtomicWriteText(primary,second,&error));
    LayoutFsOps ops;
    auto realMove=ops.moveFile;
    ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
        if(from==rollback && to==bak){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
        return realMove(from,to,flags);
    };
    CHECK(!AtomicWriteText(primary,third,&error,false,ops));
    CHECK(!error.empty());
    CHECK(ReadRawFile(primary)==third);
    CHECK(ReadRawFile(rollback)==second);
    CHECK(ReadRawFile(bak)==first);
    CHECK(WriteRawFile(primary,"corrupt replacement"));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::Recovered);
    CHECK(LoadedDesktopName(loaded)=="second");
    CHECK(loaded.revision.sourcePath==rollback);
}

static void test_recovery_write_preserves_known_good_backup_and_reports_cleanup_failure(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
        std::string good=ValidLayoutBytes("known-good"), replacement=ValidLayoutBytes("replacement"), error;
        CHECK(WriteRawFile(primary,"corrupt primary")); CHECK(WriteRawFile(bak,good));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.revision.sourcePath==bak);
        CHECK(AtomicWriteText(primary,replacement,&error,true));
        CHECK(error.empty());
        CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==good);
        CHECK(!RawFileExists(primary+L".displaced"));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak", displaced=primary+L".displaced";
        std::string good=ValidLayoutBytes("known-good"), replacement=ValidLayoutBytes("replacement"), error;
        CHECK(WriteRawFile(primary,"corrupt primary")); CHECK(WriteRawFile(bak,good));
        LayoutFsOps ops; auto realDelete=ops.deleteFile;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
            return realDelete(deleted);
        };
        CHECK(!AtomicWriteText(primary,replacement,&error,true,ops));
        CHECK(!error.empty());
        CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==good);
        CHECK(RawFileExists(displaced));
        error="stale";
        CHECK(AtomicWriteText(primary,replacement,&error,true));
        CHECK(error.empty());
        CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==good);
        CHECK(!RawFileExists(displaced));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak", displaced=primary+L".displaced";
        std::string good=ValidLayoutBytes("known-good"), first=ValidLayoutBytes("first-retry"),
            changed=ValidLayoutBytes("changed-retry"), error;
        CHECK(WriteRawFile(primary,"corrupt primary")); CHECK(WriteRawFile(bak,good));
        LayoutFsOps ops; auto realDelete=ops.deleteFile;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
            return realDelete(deleted);
        };
        CHECK(!AtomicWriteText(primary,first,&error,true,ops));
        CHECK(ReadRawFile(primary)==first && ReadRawFile(bak)==good && RawFileExists(displaced));
        CHECK(AtomicWriteText(primary,changed,&error,true));
        CHECK(error.empty());
        CHECK(ReadRawFile(primary)==changed && ReadRawFile(bak)==good);
        CHECK(!RawFileExists(displaced));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak", displaced=primary+L".displaced";
        std::string corrupt="corrupt primary", good=ValidLayoutBytes("known-good"),
            replacement=ValidLayoutBytes("replacement"), error;
        CHECK(WriteRawFile(primary,corrupt)); CHECK(WriteRawFile(bak,good));
        LayoutFsOps ops;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced) return TRUE;
            return DeleteFileW(deleted.c_str());
        };
        CHECK(!AtomicWriteText(primary,replacement,&error,true,ops));
        CHECK(!error.empty());
        CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==good);
        CHECK(ReadRawFile(displaced)==corrupt);
        CHECK(AtomicWriteText(primary,replacement,&error,true));
        CHECK(error.empty() && !RawFileExists(displaced));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak",
            displaced=primary+L".displaced", tempPath=primary+L".tmp.stage";
        std::string corrupt="corrupt displaced", good=ValidLayoutBytes("known-good"),
            replacement=ValidLayoutBytes("replacement"), error;
        CHECK(WriteRawFile(displaced,corrupt)); CHECK(WriteRawFile(bak,good));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==tempPath && creation==CREATE_ALWAYS){
                SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        CHECK(!AtomicWriteText(primary,replacement,&error,true,ops));
        CHECK(!error.empty());
        CHECK(!RawFileExists(primary));
        CHECK(ReadRawFile(displaced)==corrupt && ReadRawFile(bak)==good);
        CHECK(AtomicWriteText(primary,replacement,&error,true));
        CHECK(error.empty());
        CHECK(ReadRawFile(primary)==replacement && ReadRawFile(bak)==good);
        CHECK(!RawFileExists(displaced));
    }
}

static void test_preserve_retry_without_named_recovery_converges(){
    for(int changedRetry=0;changedRetry<2;++changedRetry){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak", rollback=primary+L".rollback";
        const std::string old=ValidLayoutBytes("old"), first=ValidLayoutBytes("first"),
            changed=ValidLayoutBytes("changed");
        CHECK(WriteRawFile(primary,old));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
            return realDelete(deleted);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,first,&error,true,ops));
        CHECK(!error.empty() && ReadRawFile(primary)==first && ReadRawFile(displaced)==old);
        CHECK(!RawFileExists(bak) && !RawFileExists(rollback));
        const std::string& requested=changedRetry ? changed : first;
        CHECK(AtomicWriteText(primary,requested,&error,true));
        CHECK(error.empty() && ReadRawFile(primary)==requested && !RawFileExists(displaced));
        if(changedRetry) CHECK(ReadRawFile(bak)==old);
        else CHECK(!RawFileExists(bak));
    }

    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
        bak=primary+L".bak", rollback=primary+L".rollback", committed=primary+L".tmp";
    const std::string old=ValidLayoutBytes("old-no-primary"), first=ValidLayoutBytes("first-no-primary"),
        changed=ValidLayoutBytes("changed-no-primary");
    CHECK(WriteRawFile(primary,old));
    LayoutFsOps ops;
    ops.replaceFile=[&](const std::wstring& replaced,const std::wstring&,
            const std::wstring& backupName,DWORD)->BOOL{
        CHECK(MoveFileExW(replaced.c_str(),backupName.c_str(),
            MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0);
        SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
        return FALSE;
    };
    std::string error;
    CHECK(!AtomicWriteText(primary,first,&error,true,ops));
    CHECK(!error.empty() && !RawFileExists(primary) && ReadRawFile(displaced)==old);
    CHECK(ReadRawFile(committed)==first && !RawFileExists(bak) && !RawFileExists(rollback));
    CHECK(AtomicWriteText(primary,changed,&error,true));
    CHECK(error.empty() && ReadRawFile(primary)==changed && ReadRawFile(bak)==old);
    CHECK(!RawFileExists(displaced) && !RawFileExists(committed));

    {
        LayoutTempDir direct;
        std::wstring missingPrimary=direct.file(L"layout.txt"),
            onlyDisplaced=missingPrimary+L".displaced", onlyBak=missingPrimary+L".bak";
        const std::string prior=ValidLayoutBytes("displaced-only-prior"),
            requested=ValidLayoutBytes("displaced-only-new");
        CHECK(WriteRawFile(onlyDisplaced,prior));
        CHECK(AtomicWriteText(missingPrimary,requested,&error,true));
        CHECK(error.empty() && ReadRawFile(missingPrimary)==requested && ReadRawFile(onlyBak)==prior);
        CHECK(!RawFileExists(onlyDisplaced));
    }

    for(int afterEffect=0;afterEffect<2;++afterEffect){
        LayoutTempDir interrupted;
        std::wstring retryPrimary=interrupted.file(L"layout.txt"),
            retryDisplaced=retryPrimary+L".displaced", retryBak=retryPrimary+L".bak";
        const std::string published=ValidLayoutBytes("published-before-retry"),
            prior=ValidLayoutBytes("prior-before-retry"), next=ValidLayoutBytes("next-retry");
        CHECK(WriteRawFile(retryPrimary,published)); CHECK(WriteRawFile(retryDisplaced,prior));
        LayoutFsOps moveOps;
        auto realMove=moveOps.moveFile;
        bool injected=false;
        moveOps.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
            if(from==retryDisplaced && to==retryBak && !injected){
                if(afterEffect) CHECK(realMove(from,to,flags)!=0);
                injected=true; SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT); return FALSE;
            }
            return realMove(from,to,flags);
        };
        CHECK(!AtomicWriteText(retryPrimary,next,&error,true,moveOps));
        CHECK(injected && !error.empty() && ReadRawFile(retryPrimary)==published);
        CHECK(afterEffect ? ReadRawFile(retryBak)==prior : ReadRawFile(retryDisplaced)==prior);
        CHECK(AtomicWriteText(retryPrimary,next,&error,true));
        CHECK(error.empty() && ReadRawFile(retryPrimary)==next && ReadRawFile(retryBak)==prior);
        CHECK(!RawFileExists(retryDisplaced));
    }
}

static void test_displaced_restart_and_default_write_converge(){
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak";
        const std::string prior=ValidLayoutBytes("restart-prior"),
            published=ValidLayoutBytes("restart-published");
        CHECK(WriteRawFile(primary,prior));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
            return realDelete(deleted);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,published,&error,true,ops));
        CHECK(ReadRawFile(primary)==published && ReadRawFile(displaced)==prior && !RawFileExists(bak));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Valid && loaded.writesAllowed && loaded.usable());
        CHECK(loaded.revision.sourcePath==primary && LoadedDesktopName(loaded)=="restart-published");
        CHECK(ReadRawFile(primary)==published && !RawFileExists(displaced));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak";
        const std::string prior=ValidLayoutBytes("default-prior"),
            published=ValidLayoutBytes("default-published"), next=ValidLayoutBytes("default-next");
        CHECK(WriteRawFile(primary,prior));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced){ SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
            return realDelete(deleted);
        };
        std::string error;
        CHECK(!AtomicWriteText(primary,published,&error,true,ops));
        CHECK(ReadRawFile(primary)==published && ReadRawFile(displaced)==prior);
        CHECK(AtomicWriteText(primary,next,&error,false));
        CHECK(error.empty() && ReadRawFile(primary)==next && ReadRawFile(bak)==published);
        CHECK(!RawFileExists(displaced));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak";
        const std::string prior=ValidLayoutBytes("sole-displaced-recovery");
        CHECK(WriteRawFile(displaced,prior));
        LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.writesAllowed && loaded.usable());
        CHECK(loaded.revision.sourcePath==bak && LoadedDesktopName(loaded)=="sole-displaced-recovery");
        CHECK(ReadRawFile(bak)==prior && !RawFileExists(displaced) && !RawFileExists(primary));
    }
}

static void test_displaced_reconciliation_faults_retry_safely(){
    for(int afterEffect=0;afterEffect<2;++afterEffect){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced";
        const std::string current=ValidLayoutBytes("delete-current"),
            prior=ValidLayoutBytes("delete-prior");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(displaced,prior));
        LayoutFsOps ops;
        auto realDelete=ops.deleteFile;
        bool injected=false;
        ops.deleteFile=[&](const std::wstring& deleted)->BOOL{
            if(deleted==displaced && !injected){
                if(afterEffect) CHECK(realDelete(deleted)!=0);
                injected=true; SetLastError(ERROR_ACCESS_DENIED); return FALSE;
            }
            return realDelete(deleted);
        };
        LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && first.status==LayoutLoadStatus::Unavailable && !first.writesAllowed);
        CHECK(ReadRawFile(primary)==current);
        CHECK(afterEffect ? !RawFileExists(displaced) : ReadRawFile(displaced)==prior);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && retry.writesAllowed);
        CHECK(ReadRawFile(primary)==current && !RawFileExists(displaced));
    }
    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced";
        const std::string current=ValidLayoutBytes("transient-current"),
            prior=ValidLayoutBytes("transient-displaced");
        CHECK(WriteRawFile(primary,current)); CHECK(WriteRawFile(displaced,prior));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        bool injected=false;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==displaced){
                injected=true; SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
        CHECK(ReadRawFile(primary)==current && ReadRawFile(displaced)==prior);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Valid && !RawFileExists(displaced));
    }
    for(int afterEffect=0;afterEffect<2;++afterEffect){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak";
        const std::string prior=ValidLayoutBytes("move-displaced");
        CHECK(WriteRawFile(displaced,prior));
        LayoutFsOps ops;
        auto realMove=ops.moveFile;
        bool injected=false;
        ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
            if(from==displaced && to==bak && !injected){
                if(afterEffect) CHECK(realMove(from,to,flags)!=0);
                injected=true; SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT); return FALSE;
            }
            return realMove(from,to,flags);
        };
        LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && first.status==LayoutLoadStatus::Unavailable && !first.writesAllowed);
        CHECK(afterEffect ? ReadRawFile(bak)==prior : ReadRawFile(displaced)==prior);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Recovered && retry.writesAllowed);
        CHECK(retry.revision.sourcePath==bak && ReadRawFile(bak)==prior && !RawFileExists(displaced));
    }
}

static void test_corrupt_primary_recovers_from_sole_valid_displaced(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
        bak=primary+L".bak";
    const std::string corrupt="strictly corrupt primary", recovery=ValidLayoutBytes("displaced-recovery");
    CHECK(WriteRawFile(primary,corrupt)); CHECK(WriteRawFile(displaced,recovery));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::Recovered && loaded.writesAllowed && loaded.usable());
    CHECK(loaded.revision.sourcePath==bak && LoadedDesktopName(loaded)=="displaced-recovery");
    CHECK(ReadRawFile(primary)==corrupt && ReadRawFile(bak)==recovery && !RawFileExists(displaced));
    std::vector<std::wstring> diagnostics=DiagnosticCopies(primary);
    CHECK(diagnostics.size()==1 && ReadRawFile(diagnostics.front())==corrupt);

    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::Recovered && retry.revision.sourcePath==bak);
    CHECK(DiagnosticCopies(primary).size()==1);
}

static void test_corrupt_primary_displaced_faults_preserve_once_and_retry(){
    for(int afterEffect=0;afterEffect<2;++afterEffect){
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak";
        const std::string corrupt="corrupt move primary", recovery=ValidLayoutBytes("move recovery");
        CHECK(WriteRawFile(primary,corrupt)); CHECK(WriteRawFile(displaced,recovery));
        LayoutFsOps ops;
        auto realMove=ops.moveFile;
        bool injected=false;
        ops.moveFile=[&](const std::wstring& from,const std::wstring& to,DWORD flags)->BOOL{
            if(from==displaced && to==bak && !injected){
                if(afterEffect) CHECK(realMove(from,to,flags)!=0);
                injected=true; SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT); return FALSE;
            }
            return realMove(from,to,flags);
        };
        LayoutLoadResult first=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(injected && first.status==LayoutLoadStatus::Unavailable && !first.writesAllowed);
        CHECK(ReadRawFile(primary)==corrupt);
        CHECK(afterEffect ? ReadRawFile(bak)==recovery : ReadRawFile(displaced)==recovery);
        std::vector<std::wstring> firstDiagnostics=DiagnosticCopies(primary);
        CHECK(firstDiagnostics.size()==1 && ReadRawFile(firstDiagnostics.front())==corrupt);

        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Recovered && retry.writesAllowed && retry.usable());
        CHECK(retry.revision.sourcePath==bak && ReadRawFile(bak)==recovery && !RawFileExists(displaced));
        CHECK(DiagnosticCopies(primary).size()==1);
    }

    {
        LayoutTempDir temp;
        std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
            bak=primary+L".bak";
        const std::string corrupt="corrupt transient primary", recovery=ValidLayoutBytes("transient recovery");
        CHECK(WriteRawFile(primary,corrupt)); CHECK(WriteRawFile(displaced,recovery));
        LayoutFsOps ops;
        auto realOpen=ops.openFile;
        int displacedOpens=0;
        ops.openFile=[&](const std::wstring& opened,DWORD access,DWORD share,
                DWORD creation,DWORD flags)->HANDLE{
            if(opened==displaced && ++displacedOpens==2){
                SetLastError(ERROR_SHARING_VIOLATION); return INVALID_HANDLE_VALUE;
            }
            return realOpen(opened,access,share,creation,flags);
        };
        LayoutLoadResult blocked=LoadLayoutWithBackupLocked(primary,1700000000,ops);
        CHECK(displacedOpens==2 && blocked.status==LayoutLoadStatus::Unavailable && !blocked.writesAllowed);
        CHECK(ReadRawFile(primary)==corrupt && ReadRawFile(displaced)==recovery && !RawFileExists(bak));
        CHECK(DiagnosticCopies(primary).size()==1);
        LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
        CHECK(retry.status==LayoutLoadStatus::Recovered && retry.revision.sourcePath==bak);
        CHECK(ReadRawFile(bak)==recovery && !RawFileExists(displaced));
        CHECK(DiagnosticCopies(primary).size()==1);
    }
}

static void test_corrupt_displaced_is_preserved_but_never_recovered(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), displaced=primary+L".displaced",
        bak=primary+L".bak";
    const std::string corruptPrimary="corrupt primary evidence",
        corruptDisplaced="corrupt displaced evidence";
    CHECK(WriteRawFile(primary,corruptPrimary)); CHECK(WriteRawFile(displaced,corruptDisplaced));
    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(loaded.status==LayoutLoadStatus::CorruptPreserved && loaded.writesAllowed && !loaded.usable());
    CHECK(ReadRawFile(primary)==corruptPrimary && !RawFileExists(displaced) && !RawFileExists(bak));
    std::vector<std::wstring> primaryDiagnostics=DiagnosticCopies(primary),
        displacedDiagnostics=DiagnosticCopies(displaced);
    CHECK(primaryDiagnostics.size()==1 && ReadRawFile(primaryDiagnostics.front())==corruptPrimary);
    CHECK(displacedDiagnostics.size()==1 && ReadRawFile(displacedDiagnostics.front())==corruptDisplaced);
    LayoutLoadResult retry=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(retry.status==LayoutLoadStatus::CorruptPreserved && retry.writesAllowed);
    CHECK(DiagnosticCopies(primary).size()==1 && DiagnosticCopies(displaced).size()==1);
}

static std::string LegacyV3LayoutBytes(){
    return std::string("# VDE snapshot v3\n")+
        "D\t0\t{231A0000-0000-0000-0000-000000000001}\t"+b64enc("Legacy")+"\n"+
        "W\tfirefox\t0\t{231A0000-0000-0000-0000-000000000001}\t"+
        b64enc("Inbox")+"\tmail.example\t1\tmail.example:1\t0\n";
}

static void test_legacy_migration_failure_preserves_source_and_publishes_nothing(){
    LayoutTempDir temp;
    std::wstring legacy=temp.file(L"layout.txt");
    std::wstring automatic=temp.file(L"layout-auto.txt");
    std::string legacyBytes=LegacyV3LayoutBytes();
    CHECK(WriteRawFile(legacy,legacyBytes));

    LayoutFsOps ops;
    ops.writeFile=[](HANDLE,const void*,DWORD,DWORD& written)->BOOL {
        written=0;
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    };
    LegacyLayoutMigrationResult result=MigrateLegacyLayoutLocked(
        legacy,automatic,1700000000,ops);

    CHECK(result.status==LegacyLayoutMigrationStatus::Failed);
    CHECK(!result.error.empty());
    CHECK(ReadRawFile(legacy)==legacyBytes);
    CHECK(!FileExists(automatic));
    CHECK(!FileExists(automatic+L".tmp"));
    CHECK(!FileExists(automatic+L".bak"));
    CHECK(!FileExists(automatic+L".rollback"));
}

static void test_legacy_migration_parses_before_publishing(){
    LayoutTempDir temp;
    std::wstring legacy=temp.file(L"layout.txt");
    std::wstring automatic=temp.file(L"layout-auto.txt");
    std::string invalid="# VDE snapshot v3\nW\tfirefox\tinvalid\n";
    CHECK(WriteRawFile(legacy,invalid));

    LayoutFsOps ops;
    LegacyLayoutMigrationResult result=MigrateLegacyLayoutLocked(
        legacy,automatic,1700000000,ops);

    CHECK(result.status==LegacyLayoutMigrationStatus::Failed);
    CHECK(!result.error.empty());
    CHECK(ReadRawFile(legacy)==invalid);
    CHECK(!FileExists(automatic));
    CHECK(!FileExists(automatic+L".tmp"));
}

static void test_legacy_migration_never_overlays_recoverable_target(){
    const wchar_t* recoverySuffixes[]={L".rollback",L".bak"};
    for(const wchar_t* suffix : recoverySuffixes){
        LayoutTempDir temp;
        std::wstring legacy=temp.file(L"layout.txt");
        std::wstring automatic=temp.file(L"layout-auto.txt");
        std::wstring recovery=automatic+suffix;
        std::string legacyBytes=LegacyV3LayoutBytes();
        std::string recoveryBytes=ValidLayoutBytes(
            wcscmp(suffix,L".rollback")==0 ? "rollback" : "backup");
        CHECK(WriteRawFile(legacy,legacyBytes));
        CHECK(WriteRawFile(recovery,recoveryBytes));

        LayoutFsOps ops;
        LegacyLayoutMigrationResult result=MigrateLegacyLayoutLocked(
            legacy,automatic,1700000000,ops);

        CHECK(result.status==LegacyLayoutMigrationStatus::NotNeeded);
        CHECK(result.target.status==LayoutLoadStatus::Recovered);
        CHECK(result.target.revision.sourcePath==recovery);
        CHECK(ReadRawFile(legacy)==legacyBytes);
        CHECK(ReadRawFile(recovery)==recoveryBytes);
        CHECK(!FileExists(automatic));
        CHECK(!FileExists(legacy+L".migrated"));
    }
}

static void test_legacy_migration_installs_checked_v4_then_retires_source(){
    LayoutTempDir temp;
    std::wstring legacy=temp.file(L"layout.txt");
    std::wstring automatic=temp.file(L"layout-auto.txt");
    std::string legacyBytes=LegacyV3LayoutBytes();
    CHECK(WriteRawFile(legacy,legacyBytes));

    LayoutFsOps ops;
    LegacyLayoutMigrationResult result=MigrateLegacyLayoutLocked(
        legacy,automatic,1700000000,ops);

    CHECK(result.status==LegacyLayoutMigrationStatus::Migrated);
    CHECK(result.error.empty());
    CHECK(result.target.status==LayoutLoadStatus::Valid);
    CHECK(result.target.sourceVersion==4);
    CHECK(result.target.revision.sourcePath==automatic);
    CHECK(result.target.revision.exists);
    CHECK(!FileExists(legacy));
    CHECK(ReadRawFile(legacy+L".migrated")==legacyBytes);

    std::string installed=ReadRawFile(automatic);
    CHECK(installed.find("# VDE snapshot v4\n")==0);
    std::vector<DeskRec> desks;
    std::vector<LayoutWin> wins;
    std::string error;
    int version=0;
    CHECK(ParseLayout(installed,desks,wins,1700000000,&error,&version));
    CHECK(version==4 && desks.size()==1 && wins.size()==1);
    CHECK(!wins[0].recordId.empty());
    CHECK(wins[0].lastSeenUtc==1700000000 && wins[0].missingSinceUtc==0);
}

static void test_same_revision_compares_every_field(){
    LayoutRevision base;
    base.sourcePath=L"a"; base.exists=true; base.size=12; base.mtime=34; base.contentHash=56;
    CHECK(SameRevision(base,base));
    LayoutRevision changed=base; changed.sourcePath=L"b"; CHECK(!SameRevision(base,changed));
    changed=base; changed.exists=false; CHECK(!SameRevision(base,changed));
    changed=base; changed.size=13; CHECK(!SameRevision(base,changed));
    changed=base; changed.mtime=35; CHECK(!SameRevision(base,changed));
    changed=base; changed.contentHash=57; CHECK(!SameRevision(base,changed));
}

static bool WriteIfCanonicalRevisionUnchanged(const std::wstring& primary,const LayoutRevision& expected,
        const std::string& bytes,bool preserveExistingBackup=false){
    ScopedLayoutLock lock(1000);
    if(!lock.acquired()) return false;
    LayoutRevision current=ReadLayoutRevisionLocked(primary);
    if(!SameRevision(current,expected)) return false;
    return AtomicWriteText(primary,bytes,nullptr,preserveExistingBackup);
}

static void test_missing_primary_corrupt_recovery_revision_allows_empty_publish(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), rollback=primary+L".rollback",
        backup=primary+L".bak";
    const std::string corruptRollback="corrupt rollback",
        corruptBackup="corrupt backup", emptyLayout=ValidLayoutBytes("empty-after-corruption");
    CHECK(WriteRawFile(rollback,corruptRollback));
    CHECK(WriteRawFile(backup,corruptBackup));

    LayoutLoadResult loaded=LoadLayoutWithBackupLocked(primary,1700000000);
    LayoutRevision canonical=ReadLayoutRevisionLocked(primary);
    CHECK(loaded.status==LayoutLoadStatus::CorruptPreserved && loaded.writesAllowed);
    CHECK(!canonical.exists && canonical.sourcePath==primary);
    CHECK(SameRevision(loaded.revision,canonical));
    CHECK(WriteIfCanonicalRevisionUnchanged(primary,loaded.revision,emptyLayout));
    CHECK(ReadRawFile(primary)==emptyLayout);
    CHECK(ReadRawFile(rollback)==corruptRollback && ReadRawFile(backup)==corruptBackup);

    LayoutTempDir tempOnly;
    std::wstring tempPrimary=tempOnly.file(L"layout.txt"), committedTemp=tempPrimary+L".tmp";
    CHECK(WriteRawFile(committedTemp,"corrupt committed temporary"));
    LayoutLoadResult tempLoaded=LoadLayoutWithBackupLocked(tempPrimary,1700000000);
    LayoutRevision tempCanonical=ReadLayoutRevisionLocked(tempPrimary);
    CHECK(tempLoaded.status==LayoutLoadStatus::CorruptPreserved && tempLoaded.writesAllowed);
    CHECK(!RawFileExists(committedTemp));
    CHECK(SameRevision(tempLoaded.revision,tempCanonical));
    CHECK(WriteIfCanonicalRevisionUnchanged(tempPrimary,tempLoaded.revision,emptyLayout));
    CHECK(ReadRawFile(tempPrimary)==emptyLayout);
}

static void test_recovered_conflict_preserves_valid_backup_before_publish(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), backup=primary+L".bak";
    const std::string initial=ValidLayoutBytes("initial"),
        recovery=ValidLayoutBytes("external-recovery"),
        merged=ValidLayoutBytes("merged-after-recovery");
    std::string error;
    CHECK(AtomicWriteText(primary,initial,&error));
    LayoutLoadResult original=LoadLayoutWithBackupLocked(primary,1700000000);
    bool preserveBackup=PreserveExistingBackupForPublish(false,original.status);
    CHECK(original.status==LayoutLoadStatus::Valid && !preserveBackup);

    CHECK(WriteRawFile(primary,"external corrupt primary"));
    CHECK(WriteRawFile(backup,recovery));
    LayoutLoadResult latest=LoadLayoutWithBackupLocked(primary,1700000000);
    CHECK(latest.status==LayoutLoadStatus::Recovered && latest.revision.sourcePath==backup);
    preserveBackup=PreserveExistingBackupForPublish(preserveBackup,latest.status);
    CHECK(preserveBackup);
    CHECK(AtomicWriteText(primary,merged,&error,preserveBackup));
    CHECK(ReadRawFile(primary)==merged);
    CHECK(ReadRawFile(backup)==recovery);
    CHECK(PreserveExistingBackupForPublish(true,LayoutLoadStatus::Valid));
}

static void test_two_actor_stale_save_is_rejected_without_overwrite(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt");
    std::string first=ValidLayoutBytes("AAAA"), fromB=ValidLayoutBytes("BBBB"),
        staleA=ValidLayoutBytes("CCCC"), error;
    CHECK(first.size()==fromB.size() && first.size()==staleA.size());
    CHECK(AtomicWriteText(primary,first,&error));
    LayoutLoadResult actorA=LoadLayoutWithBackup(primary,1700000000);
    CHECK(actorA.status==LayoutLoadStatus::Valid);
    LayoutRevision directA=ReadLayoutRevisionLocked(primary);
    CHECK(actorA.revision.sourcePath==primary && directA.sourcePath==primary);
    CHECK(actorA.revision.exists && directA.exists);
    CHECK(actorA.revision.size==first.size() && directA.size==first.size());
    CHECK(actorA.revision.mtime==directA.mtime && actorA.revision.mtime!=0);
    CHECK(actorA.revision.contentHash==directA.contentHash && actorA.revision.contentHash!=0);
    {
        ScopedLayoutLock actorB(1000); CHECK(actorB.acquired());
        CHECK(actorB.acquired() && AtomicWriteText(primary,fromB,&error));
    }
    CHECK(SetRawFileMtime(primary,actorA.revision.mtime));
    LayoutRevision directB=ReadLayoutRevisionLocked(primary);
    CHECK(directB.sourcePath==actorA.revision.sourcePath);
    CHECK(directB.exists==actorA.revision.exists);
    CHECK(directB.size==actorA.revision.size);
    CHECK(directB.mtime==actorA.revision.mtime);
    CHECK(directB.contentHash!=actorA.revision.contentHash);
    CHECK(!SameRevision(directB,actorA.revision));
    CHECK(!WriteIfCanonicalRevisionUnchanged(primary,actorA.revision,staleA));
    CHECK(ReadRawFile(primary)==fromB);
}

static void test_two_actor_recovered_source_stale_save_is_rejected(){
    LayoutTempDir temp;
    std::wstring primary=temp.file(L"layout.txt"), bak=primary+L".bak";
    std::string base=ValidLayoutBytes("recovery-base"), fromB=ValidLayoutBytes("B"),
        staleA=ValidLayoutBytes("A-stale"), error;
    CHECK(WriteRawFile(primary,"corrupt primary"));
    CHECK(WriteRawFile(bak,base));
    LayoutLoadResult actorA=LoadLayoutWithBackup(primary,1700000000);
    CHECK(actorA.status==LayoutLoadStatus::Recovered && actorA.revision.sourcePath==bak);
    LayoutRevision directRecovery=ReadLayoutRevisionLocked(bak);
    CHECK(actorA.revision.sourcePath==directRecovery.sourcePath);
    CHECK(actorA.revision.exists==directRecovery.exists && actorA.revision.exists);
    CHECK(actorA.revision.size==directRecovery.size && actorA.revision.size==base.size());
    CHECK(actorA.revision.mtime==directRecovery.mtime && actorA.revision.mtime!=0);
    CHECK(actorA.revision.contentHash==directRecovery.contentHash && actorA.revision.contentHash!=0);
    {
        ScopedLayoutLock actorB(1000); CHECK(actorB.acquired());
        CHECK(actorB.acquired() && AtomicWriteText(primary,fromB,&error,true));
    }
    CHECK(ReadRawFile(bak)==base);
    LayoutRevision canonicalAfterB=ReadLayoutRevisionLocked(primary);
    CHECK(canonicalAfterB.sourcePath==primary && canonicalAfterB.exists);
    CHECK(canonicalAfterB.size==fromB.size() && canonicalAfterB.mtime!=0 &&
        canonicalAfterB.contentHash!=0);
    CHECK(!SameRevision(actorA.revision,canonicalAfterB));
    CHECK(!WriteIfCanonicalRevisionUnchanged(primary,actorA.revision,staleA,true));
    CHECK(ReadRawFile(primary)==fromB);

    LayoutTempDir rollbackTemp;
    std::wstring rollbackPrimary=rollbackTemp.file(L"layout.txt"), rollback=rollbackPrimary+L".rollback";
    CHECK(WriteRawFile(rollbackPrimary,"corrupt primary"));
    CHECK(WriteRawFile(rollback,base));
    LayoutLoadResult rollbackActorA=LoadLayoutWithBackup(rollbackPrimary,1700000000);
    CHECK(rollbackActorA.status==LayoutLoadStatus::Recovered && rollbackActorA.revision.sourcePath==rollback);
    {
        ScopedLayoutLock actorB(1000); CHECK(actorB.acquired());
        CHECK(actorB.acquired() && AtomicWriteText(rollbackPrimary,fromB,&error,true));
    }
    CHECK(ReadRawFile(rollback)==base);
    CHECK(!WriteIfCanonicalRevisionUnchanged(rollbackPrimary,rollbackActorA.revision,staleA,true));
    CHECK(ReadRawFile(rollbackPrimary)==fromB);
}

struct LayoutLockThreadContext {
    HANDLE ready=nullptr;
    HANDLE release=nullptr;
    bool acquired=false;
};

static DWORD WINAPI HoldLayoutLockThread(void* opaque){
    LayoutLockThreadContext* context=(LayoutLockThreadContext*)opaque;
    ScopedLayoutLock lock(5000);
    context->acquired=lock.acquired();
    SetEvent(context->ready);
    if(lock.acquired()) WaitForSingleObject(context->release,5000);
    return 0;
}

struct AbandonedLayoutLockContext {
    HANDLE mutex=nullptr;
    HANDLE ready=nullptr;
    bool acquired=false;
};

static DWORD WINAPI AbandonLayoutLockThread(void* opaque){
    AbandonedLayoutLockContext* context=(AbandonedLayoutLockContext*)opaque;
    context->acquired=WaitForSingleObject(context->mutex,5000)==WAIT_OBJECT_0;
    SetEvent(context->ready);
    return 0; // Deliberately exits while owning the mutex.
}

static void test_layout_mutex_zero_timeout_and_acquisition_after_release(){
    LayoutLockThreadContext context;
    context.ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    context.release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    CHECK(context.ready!=nullptr && context.release!=nullptr);
    HANDLE thread=CreateThread(nullptr,0,HoldLayoutLockThread,&context,0,nullptr);
    CHECK(thread!=nullptr);
    CHECK(WaitForSingleObject(context.ready,5000)==WAIT_OBJECT_0);
    CHECK(context.acquired);
    {
        ScopedLayoutLock blocked(0);
        CHECK(!blocked.acquired());
    }
    DWORD handlesBefore=0, handlesAfter=0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesBefore)!=0);
    for(int attempt=0;attempt<64;++attempt){
        ScopedLayoutLock blocked(0);
        CHECK(!blocked.acquired());
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesAfter)!=0);
    CHECK(handlesAfter==handlesBefore);
    CHECK(SetEvent(context.release)!=0);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
    CHECK(CloseHandle(thread)!=0);
    CHECK(CloseHandle(context.ready)!=0);
    CHECK(CloseHandle(context.release)!=0);
    ScopedLayoutLock afterRelease(1000);
    CHECK(afterRelease.acquired());
}


static void test_layout_mutex_treats_abandoned_as_acquired(){
    HANDLE keeper=CreateMutexW(nullptr,FALSE,L"Local\\win-vde.layout-store.v1");
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    CHECK(keeper!=nullptr && ready!=nullptr);
    AbandonedLayoutLockContext context;
    context.mutex=keeper; context.ready=ready;
    HANDLE thread=CreateThread(nullptr,0,AbandonLayoutLockThread,&context,0,nullptr);
    CHECK(thread!=nullptr);
    CHECK(WaitForSingleObject(ready,5000)==WAIT_OBJECT_0);
    CHECK(context.acquired);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
    CHECK(CloseHandle(thread)!=0);
    {
        ScopedLayoutLock recovered(1000);
        CHECK(recovered.acquired());
    }
    CHECK(CloseHandle(ready)!=0);
    CHECK(CloseHandle(keeper)!=0);
}

static void test_layout_fixture_removes_only_its_unique_tree(){
    std::wstring fixturePath;
    {
        LayoutTempDir temp;
        fixturePath=temp.path;
        CHECK(WriteRawFile(temp.file(L"artifact.tmp"),"temporary"));
    }
    DWORD attributes=GetFileAttributesW(fixturePath.c_str());
    DWORD error=GetLastError();
    CHECK(attributes==INVALID_FILE_ATTRIBUTES);
    CHECK(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND);
}

static void test_finalization_runs_once(){
    FinalizationState state;
    CHECK(state.begin());
    CHECK(!state.begin());
    state.finish();
    CHECK(state.finished);
    CHECK(!state.begin());

    FinalizationState retryable;
    CHECK(retryable.begin());
    retryable.retry();
    CHECK(retryable.begin());
}

static void test_window_identity_requires_full_nonzero_process_identity(){
    FastWin window;
    window.hwnd=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));
    window.pid=77;
    window.processStart=9001;
    WindowIdentityKey key=IdentityOf(window);
    CHECK(key.hwnd==0x1234 && key.pid==77 && key.processStart==9001);
    CHECK(SameIdentity(key,key));

    WindowIdentityKey changed=key;
    changed.processStart=9002;
    CHECK(!SameIdentity(key,changed));
    changed=key; changed.pid=78;
    CHECK(!SameIdentity(key,changed));
    changed=key; changed.hwnd=0x1235;
    CHECK(!SameIdentity(key,changed));
    changed=key; changed.processStart=0;
    CHECK(!SameIdentity(changed,changed));
    changed=key; changed.hwnd=0;
    CHECK(!SameIdentity(changed,changed));
    changed=key; changed.pid=0;
    CHECK(!SameIdentity(changed,changed));
    CHECK(RuntimeKey(window)==RuntimeKey(key));
    CHECK(RuntimeKey(key)=="4660:77:9001");
}

static void test_snapshot_versions_change_only_for_changed_inputs(){
    SnapshotVersionTracker tracker;
    SnapshotVersions first=tracker.observe("firefox",11,21);
    CHECK(first.identityGeneration!=0 && first.contentGeneration!=0);
    SnapshotVersions unchanged=tracker.observe("firefox",11,21);
    CHECK(unchanged.identityGeneration==first.identityGeneration);
    CHECK(unchanged.contentGeneration==first.contentGeneration);

    SnapshotVersions content=tracker.observe("firefox",11,22);
    CHECK(content.identityGeneration==first.identityGeneration);
    CHECK(content.contentGeneration>first.contentGeneration);

    SnapshotVersions identity=tracker.observe("firefox",12,23);
    CHECK(identity.identityGeneration>content.contentGeneration);
    CHECK(identity.contentGeneration>identity.identityGeneration);

    SnapshotVersions other=tracker.observe("chrome",11,21);
    CHECK(other.identityGeneration>identity.contentGeneration);
    CHECK(other.contentGeneration>other.identityGeneration);
}

static void test_snapshot_signatures_are_delimiter_safe(){
    SnapshotSignatureBuilder left;
    left.addString("a").addString("bc");
    SnapshotSignatureBuilder right;
    right.addString("ab").addString("c");
    CHECK(left.value()!=right.value());

    SnapshotVersionTracker tracker;
    SnapshotVersions before=tracker.observe("firefox",1,left.value());
    SnapshotVersions after=tracker.observe("firefox",1,right.value());
    CHECK(after.identityGeneration==before.identityGeneration);
    CHECK(after.contentGeneration>before.contentGeneration);
}

static void test_snapshot_generation_wrap_restarts_without_zero(){
    SnapshotVersionTracker tracker(UINT64_MAX);
    SnapshotVersions first=tracker.observe("firefox",1,2);
    CHECK(first.identityGeneration==1);
    CHECK(first.contentGeneration==2);
    SnapshotVersions same=tracker.observe("firefox",1,2);
    CHECK(same.identityGeneration==1 && same.contentGeneration==2);

    SnapshotVersionTracker oneSlot(UINT64_MAX-1);
    SnapshotVersions nearWrap=oneSlot.observe("firefox",1,2);
    CHECK(nearWrap.identityGeneration==UINT64_MAX-1);
    CHECK(nearWrap.contentGeneration==UINT64_MAX);
    SnapshotVersions restarted=oneSlot.observe("firefox",1,2);
    CHECK(restarted.identityGeneration==1 && restarted.contentGeneration==2);
}

static FastWin SnapshotWindow(uintptr_t hwnd,DWORD pid,uint64_t started,
        const wchar_t* title,const GUID& desktop){
    FastWin window;
    window.app="firefox";
    window.hwnd=reinterpret_cast<HWND>(hwnd);
    window.pid=pid;
    window.processStart=started;
    window.title=title;
    window.desktop=desktop;
    return window;
}

static void test_save_observed_bound_app_updates_only_exact_bound_identities(){
    const UnixSeconds now=2000000500;
    const GUID desktopA=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID movedA=G(L"{231A0000-0000-0000-0000-000000000002}");
    const GUID savedB=G(L"{231A0000-0000-0000-0000-000000000003}");
    LayoutWin recordA=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009101}","firefox",
        "A","a.example",0,desktopA,now-100);
    LayoutWin recordB=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009102}","firefox",
        "B","b.example",2,savedB,now-100);
    recordA.missingSinceUtc=now-50;

    BoundSaveObservation bound;
    bound.window=SnapshotWindow(101,1001,10001,L"changed title",movedA);
    bound.hasBinding=true;
    bound.expectedIdentity=IdentityOf(bound.window);
    bound.recordId=recordA.recordId;
    bound.deskIndex=1;
    bound.causalGeneration=81;
    BoundSaveObservation unbound;
    unbound.window=SnapshotWindow(102,1002,10002,L"B",desktopA);
    unbound.hasBinding=false;
    unbound.deskIndex=0;

    const std::vector<LayoutWin> input={recordA,recordB};
    SaveObservedAppResult saved=ApplyObservedBoundRecords(
        input,"firefox",{bound,unbound},true,now);
    CHECK(saved.valid && saved.needsReconcile);
    CHECK(saved.records.size()==2 && saved.updates.size()==1);
    CHECK(GuidEq(saved.records[0].desktop,movedA) &&
          saved.records[0].deskIndex==1);
    CHECK(saved.records[0].activeTitle==recordA.activeTitle &&
          saved.records[0].activeDomain==recordA.activeDomain);
    CHECK(saved.records[0].lastSeenUtc==now &&
          saved.records[0].missingSinceUtc==0);
    CHECK(SameLayoutWinFields(saved.records[1],recordB));
    CHECK(saved.updates[0].semanticChanged &&
          saved.updates[0].causalGeneration==81 &&
          saved.updates[0].after.recordId==recordA.recordId);

    BoundSaveObservation reused=bound;
    reused.expectedIdentity.processStart++;
    SaveObservedAppResult stale=ApplyObservedBoundRecords(
        input,"firefox",{reused},true,now);
    CHECK(stale.valid && stale.needsReconcile && stale.updates.empty());
    CHECK(SameLayoutWinVectors(stale.records,input));

    SaveObservedAppResult incomplete=ApplyObservedBoundRecords(
        input,"firefox",{bound},false,now);
    CHECK(!incomplete.valid && input.size()==2 &&
          SameLayoutWinFields(input[0],recordA));
}

static void test_explicit_save_with_unbound_sibling_rearms_reconcile(){
    LcState state;
    CHECK(LcObserve(state,true,1,1,10,1,0).action==LcAction::None);
    LcDecision restore=LcObserve(state,true,1,1,10,1,1);
    CHECK(restore.action==LcAction::BeginRestore);
    LcRestoreCompleted(state,restore.generation,LcRestoreOutcome::Success,
                       10,1,2);
    LcDecision save=LcObserve(state,true,1,1,11,1,3);
    CHECK(save.action==LcAction::SaveLayout && state.saveInFlight);
    CHECK(LcExplicitSaveNeedsReconcile(
        state,save.generation,11,1,4));
    CHECK(!state.saveInFlight && state.restorePending &&
          !state.restoreInFlight);
    CHECK(!LcExplicitSaveNeedsReconcile(
        state,save.generation,11,1,5));
}

static void test_fast_snapshot_versions_are_order_independent_and_quality_aware(){
    SnapshotVersionTracker tracker;
    AppFastSnapshot first;
    first.windows.push_back(SnapshotWindow(
        2,20,200,L"",G(L"{231A0000-0000-0000-0000-000000000002}")));
    first.windows.push_back(SnapshotWindow(
        1,10,100,L"Inbox",G(L"{231A0000-0000-0000-0000-000000000001}")));
    FinalizeFastSnapshot("firefox",91,tracker,first);
    CHECK(first.windows.size()==2 && first.windows[0].title==L"Inbox");
    CHECK(first.identityGeneration!=0 && first.generation!=0);
    CHECK(FastSnapshotCanObserve(first) && FastSnapshotCanPersistAll(first));

    AppFastSnapshot reordered;
    reordered.windows.push_back(first.windows[1]);
    reordered.windows.push_back(first.windows[0]);
    FinalizeFastSnapshot("firefox",91,tracker,reordered);
    CHECK(reordered.identityGeneration==first.identityGeneration);
    CHECK(reordered.generation==first.generation);

    reordered.windows[0].title=L"Changed";
    FinalizeFastSnapshot("firefox",91,tracker,reordered);
    CHECK(reordered.identityGeneration==first.identityGeneration);
    CHECK(reordered.generation>first.generation);

    AppFastSnapshot desktopOnly=reordered;
    desktopOnly.windows[0].desktop=
        G(L"{231A0000-0000-0000-0000-000000000003}");
    FinalizeFastSnapshot("firefox",91,tracker,desktopOnly);
    CHECK(desktopOnly.identityGeneration==reordered.identityGeneration);
    CHECK(desktopOnly.generation>reordered.generation);

    AppFastSnapshot configOnly=desktopOnly;
    FinalizeFastSnapshot("firefox",92,tracker,configOnly);
    CHECK(configOnly.identityGeneration>desktopOnly.identityGeneration);
    CHECK(configOnly.generation>configOnly.identityGeneration);

    AppFastSnapshot incomplete=reordered;
    incomplete.enumerationComplete=false;
    FinalizeFastSnapshot("firefox",91,tracker,incomplete);
    CHECK(incomplete.identityGeneration>reordered.identityGeneration);
    CHECK(incomplete.generation>incomplete.identityGeneration);
    CHECK(!FastSnapshotCanObserve(incomplete));

    AppFastSnapshot desktopFailed=reordered;
    desktopFailed.desktopLookupsComplete=false;
    desktopFailed.windows[0].desktop=GUID{};
    FinalizeFastSnapshot("firefox",91,tracker,desktopFailed);
    CHECK(FastSnapshotCanObserve(desktopFailed));
    CHECK(!FastSnapshotCanPersistAll(desktopFailed));
}

static void test_resolve_saved_desktop_uses_guid_only(){
    LayoutWin saved;
    saved.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    saved.deskIndex=0;
    std::vector<DeskRec> current={
        {7,G(L"{231A0000-0000-0000-0000-000000000001}"),L"one"},
        {9,G(L"{231A0000-0000-0000-0000-000000000002}"),L"two"}
    };
    CHECK(ResolveSavedDesktop(saved,current)==9);
    saved.desktop=G(L"{231A0000-0000-0000-0000-000000000099}");
    CHECK(ResolveSavedDesktop(saved,current)==-1);
    saved.desktop=GUID{};
    CHECK(ResolveSavedDesktop(saved,current)==-1);
}

static LayoutRevision RebaseRevision(const wchar_t* path,uint64_t hash){
    LayoutRevision revision;
    revision.sourcePath=path;
    revision.exists=true;
    revision.size=100;
    revision.mtime=200;
    revision.contentHash=hash;
    return revision;
}

static RecordDelta RebaseUpsert(const LayoutWin& base,const LayoutWin& desired,
        const LayoutRevision& baseRevision,UnixSeconds changedUtc,uint64_t generation){
    RecordDelta delta;
    delta.kind=RecordDeltaKind::ValidatedRuntimeUpsert;
    delta.record=desired;
    delta.baseRevision=baseRevision;
    delta.baseRecordPresent=true;
    delta.baseRecord=base;
    delta.changedUtc=changedUtc;
    delta.causalGeneration=generation;
    return delta;
}

static void test_rebase_merges_different_ids_and_preserves_external_records(){
    LayoutRevision baseRevision=RebaseRevision(L"layout",1);
    LayoutRevision latestRevision=RebaseRevision(L"layout",2);
    LayoutWin baseA=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009001}","firefox","A","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin desiredA=baseA;
    desiredA.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    desiredA.deskIndex=2;
    desiredA.lastSeenUtc=300;
    LayoutWin externalB=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009002}","chrome","B","b.test",3,
        G(L"{231A0000-0000-0000-0000-000000000003}"),250);
    std::map<std::string,RecordDelta> deltas;
    deltas[baseA.recordId]=RebaseUpsert(baseA,desiredA,baseRevision,300,7);
    RebaseResult result=RebaseRecordDeltas(
        {baseA,externalB},latestRevision,deltas,400);
    CHECK(result.deferredConflictRecordIds.empty());
    CHECK(result.records.size()==2);
    CHECK(SameLayoutWinFields(result.records[0],desiredA));
    CHECK(SameLayoutWinFields(result.records[1],externalB));
}

static void test_rebase_same_id_newer_validated_upsert_wins(){
    LayoutRevision baseRevision=RebaseRevision(L"layout",11);
    LayoutRevision latestRevision=RebaseRevision(L"layout",12);
    LayoutWin base=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009011}","firefox","base","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin disk=base;
    disk.activeTitle="disk";
    disk.lastSeenUtc=200;
    LayoutWin local=base;
    local.activeTitle="local";
    local.lastSeenUtc=301;
    std::map<std::string,RecordDelta> deltas;
    deltas[base.recordId]=RebaseUpsert(base,local,baseRevision,301,9);
    RebaseResult result=RebaseRecordDeltas({disk},latestRevision,deltas,400);
    CHECK(result.deferredConflictRecordIds.empty());
    CHECK(result.records.size()==1 && SameLayoutWinFields(result.records[0],local));
}

static void test_durable_candidate_delta_is_satisfied_by_external_unrelated_revision(){
    LayoutRevision memoryRevision=RebaseRevision(L"layout",13);
    LayoutRevision capturedRevision=RebaseRevision(L"layout",14);
    const LayoutRevision externalRevision=RebaseRevision(L"layout",15);
    LayoutWin base=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009013}","firefox","base","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin desired=base;
    desired.activeTitle="durable-C";
    desired.desktop=G(L"{231A0000-0000-0000-0000-000000000002}");
    desired.deskIndex=2;
    desired.lastSeenUtc=300;
    const RecordDelta delta=RebaseUpsert(
        base,desired,memoryRevision,300,77);
    const std::map<std::string,RecordDelta> retainedDirty={
        {desired.recordId,delta}};
    ValidatedRecordTouch touch;
    touch.recordId=desired.recordId;
    touch.lastSeenUtc=desired.lastSeenUtc;
    touch.causalGeneration=77;

    // C was durable, but the cleanup path retained the A-based journal while
    // adopting C.  An external D then preserves our row and adds another ID.
    CommitPublishedLayoutRevisionNoThrow(memoryRevision,capturedRevision);
    LayoutWin external=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009014}","chrome","external-D","d.test",4,
        G(L"{231A0000-0000-0000-0000-000000000004}"),301);
    const std::vector<std::map<std::string,uint64_t> > causalStates={
        {{touch.recordId,77}},
        {},
        {{touch.recordId,88}}
    };
    for(int newerTouch=0;newerTouch<2;++newerTouch){
        LayoutWin diskDesired=desired;
        diskDesired.lastSeenUtc+=newerTouch;
        for(const auto& causal : causalStates){
            RecordDeltaRebasePreparation prepared;
            CHECK(PrepareRecordDeltasForRebase(
                {diskDesired,external},retainedDirty,causal,prepared));
            CHECK(prepared.deltas.empty());
            CHECK(prepared.deferredRecordIds.empty());

            RebaseResult rebased=RebaseRecordDeltas(
                {diskDesired,external},externalRevision,
                prepared.deltas,400);
            CHECK(rebased.deferredConflictRecordIds.empty());
            CHECK(rebased.records.size()==2);
            CHECK(SameLayoutWinFields(rebased.records[0],diskDesired));
            CHECK(SameLayoutWinFields(rebased.records[1],external));

            TouchRebaseResult touched=ReapplyValidatedTouches(
                rebased.records,{{touch.recordId,touch}},causal);
            CHECK(touched.deferredRecordIds.empty());
            CHECK(touched.records.size()==2);
            CHECK(SameLayoutWinFields(touched.records[0],diskDesired));
            CHECK(SameLayoutWinFields(touched.records[1],external));
        }
    }

    RecordDeltaRebasePreparation blocked;
    CHECK(PrepareRecordDeltasForRebase(
        {base,external},retainedDirty,{},blocked));
    CHECK(blocked.deltas.empty());
    CHECK(blocked.deferredRecordIds.count(desired.recordId)==1);

    // Publish D and its residual journal as one transaction.  A forced write
    // failure after adoption must not leave the old A base behind.  A later
    // MissingMark then rebases cleanly over unrelated external F.
    const std::map<std::string,ValidatedRecordTouch> retainedTouches={
        {touch.recordId,touch}};
    RecordDeltaRebasePreparation prepared;
    CHECK(PrepareRecordDeltasForRebase(
        {desired,external},retainedDirty,{},prepared));
    CHECK(prepared.satisfiedRecordIds.count(desired.recordId)==1);
    RebaseResult rebased=RebaseRecordDeltas(
        {desired,external},externalRevision,prepared.deltas,400);
    TouchRebaseResult touched=ReapplyValidatedTouches(
        rebased.records,retainedTouches,{});
    CHECK(touched.satisfiedRecordIds.count(desired.recordId)==1);
    RebasedResidualJournal residual;
    CHECK(BuildRebasedResidualJournal(
        retainedDirty,retainedTouches,prepared,rebased,touched,
        {desired,external},externalRevision,residual));
    CHECK(residual.deltas.empty() && residual.touches.empty());

    RebasedAutoLayoutPublication publication;
    CHECK(BuildRebasedAutoLayoutPublication(
        touched.records,{},externalRevision,{},residual.deltas,
        residual.touches,publication));
    std::vector<LayoutWin> inMemory={base};
    std::map<std::string,RecordDelta> journal=retainedDirty;
    std::map<std::string,ValidatedRecordTouch> touchJournal=retainedTouches;
    std::map<std::string,DeferredRecordConflict> conflicts;
    inMemory.swap(publication.records);
    journal.swap(publication.deltas);
    touchJournal.swap(publication.touches);
    SwapLayoutRevisionNoThrow(memoryRevision,publication.revision);
    conflicts.swap(publication.conflicts);
    CHECK(SameRevision(memoryRevision,externalRevision));
    CHECK(journal.empty() && touchJournal.empty() && conflicts.empty());

    LayoutWin missing=inMemory[0];
    missing.missingSinceUtc=500;
    RecordDelta missingDelta;
    missingDelta.kind=RecordDeltaKind::MissingMark;
    missingDelta.record=missing;
    missingDelta.baseRevision=memoryRevision;
    missingDelta.baseRecordPresent=true;
    missingDelta.baseRecord=inMemory[0];
    missingDelta.changedUtc=500;
    missingDelta.causalGeneration=88;
    std::vector<LayoutWin> stagedRecords;
    std::map<std::string,RecordDelta> stagedDeltas;
    std::map<std::string,DeferredRecordConflict> stagedConflicts;
    CHECK(StageRecordDeltaMutation(
        inMemory,journal,conflicts,missingDelta,true,
        stagedRecords,stagedDeltas,stagedConflicts)==
        RecordDeltaStageResult::Accepted);
    CHECK(SameRevision(
        stagedDeltas.at(missing.recordId).baseRevision,externalRevision));

    LayoutWin externalF=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009015}","edge","external-F","f.test",5,
        G(L"{231A0000-0000-0000-0000-000000000005}"),501);
    const LayoutRevision revisionF=RebaseRevision(L"layout",16);
    RebaseResult overF=RebaseRecordDeltas(
        {desired,external,externalF},revisionF,stagedDeltas,600);
    CHECK(overF.deferredConflictRecordIds.empty());
    CHECK(overF.records.size()==3);
    CHECK(overF.records[0].missingSinceUtc==500);
    CHECK(SameLayoutWinFields(overF.records[1],external));
    CHECK(SameLayoutWinFields(overF.records[2],externalF));
}

static void test_rebase_tied_or_older_upsert_and_stale_tombstone_defer(){
    LayoutRevision baseRevision=RebaseRevision(L"layout",21);
    LayoutRevision latestRevision=RebaseRevision(L"layout",22);
    LayoutWin base=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009021}","firefox","base","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin disk=base;
    disk.activeTitle="disk";
    disk.lastSeenUtc=250;
    LayoutWin local=base;
    local.activeTitle="local";
    local.lastSeenUtc=250;
    std::map<std::string,RecordDelta> deltas;
    deltas[base.recordId]=RebaseUpsert(base,local,baseRevision,250,10);
    RebaseResult tied=RebaseRecordDeltas({disk},latestRevision,deltas,400);
    CHECK(tied.records.size()==1 && SameLayoutWinFields(tied.records[0],disk));
    CHECK(tied.deferredConflictRecordIds.count(base.recordId)==1);

    deltas[base.recordId].changedUtc=249;
    RebaseResult older=RebaseRecordDeltas({disk},latestRevision,deltas,400);
    CHECK(older.records.size()==1 && SameLayoutWinFields(older.records[0],disk));
    CHECK(older.deferredConflictRecordIds.count(base.recordId)==1);

    RecordDelta missing=deltas[base.recordId];
    missing.kind=RecordDeltaKind::MissingMark;
    missing.record=local;
    missing.record.missingSinceUtc=249;
    deltas[base.recordId]=missing;
    RebaseResult missingResult=RebaseRecordDeltas({disk},latestRevision,deltas,400);
    CHECK(missingResult.records.size()==1 && SameLayoutWinFields(missingResult.records[0],disk));
    CHECK(missingResult.deferredConflictRecordIds.count(base.recordId)==1);

    RecordDelta tombstone=missing;
    tombstone.kind=RecordDeltaKind::ExpireDelete;
    tombstone.erase=true;
    deltas[base.recordId]=tombstone;
    RebaseResult staleDelete=RebaseRecordDeltas({disk},latestRevision,deltas,400);
    CHECK(staleDelete.records.size()==1 && SameLayoutWinFields(staleDelete.records[0],disk));
    CHECK(staleDelete.deferredConflictRecordIds.count(base.recordId)==1);
}

static void test_rebase_expiry_delete_requires_latest_independently_expired(){
    const UnixSeconds now=WINDOW_RETENTION_SECONDS+1000;
    LayoutRevision baseRevision=RebaseRevision(L"layout",31);
    LayoutRevision latestRevision=RebaseRevision(L"layout",32);
    LayoutWin base=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009031}","firefox","base","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin disk=base;
    disk.activeTitle="changed";
    disk.missingSinceUtc=1;
    RecordDelta tombstone;
    tombstone.kind=RecordDeltaKind::ExpireDelete;
    tombstone.erase=true;
    tombstone.record=base;
    tombstone.baseRevision=baseRevision;
    tombstone.baseRecordPresent=true;
    tombstone.baseRecord=base;
    tombstone.changedUtc=now;
    tombstone.causalGeneration=1;
    std::map<std::string,RecordDelta> deltas={{base.recordId,tombstone}};
    RebaseResult expired=RebaseRecordDeltas({disk},latestRevision,deltas,now);
    CHECK(expired.records.empty());
    CHECK(expired.deferredConflictRecordIds.empty());

    disk.missingSinceUtc=now-1;
    RebaseResult retained=RebaseRecordDeltas({disk},latestRevision,deltas,now);
    CHECK(retained.records.size()==1 && SameLayoutWinFields(retained.records[0],disk));
    CHECK(retained.deferredConflictRecordIds.count(base.recordId)==1);
}

static void test_record_delta_chaining_preserves_first_disk_base(){
    LayoutRevision firstRevision=RebaseRevision(L"layout",41);
    LayoutRevision accidentalLocalRevision=RebaseRevision(L"layout",42);
    LayoutWin diskBase=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009041}","firefox","base","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin firstDesired=diskBase;
    firstDesired.activeTitle="first";
    firstDesired.lastSeenUtc=200;
    RecordDelta first=RebaseUpsert(
        diskBase,firstDesired,firstRevision,200,3);
    LayoutWin secondDesired=firstDesired;
    secondDesired.activeTitle="second";
    secondDesired.lastSeenUtc=300;
    RecordDelta later=RebaseUpsert(
        firstDesired,secondDesired,accidentalLocalRevision,300,4);
    RecordDelta chained=ChainRecordDelta(first,later);
    CHECK(SameRevision(chained.baseRevision,firstRevision));
    CHECK(chained.baseRecordPresent);
    CHECK(SameLayoutWinFields(chained.baseRecord,diskBase));
    CHECK(SameLayoutWinFields(chained.record,secondDesired));
    CHECK(chained.changedUtc==300 && chained.causalGeneration==4);
}

static void test_deferred_conflict_survives_repeated_publish_until_newer_causal_upsert(){
    const LayoutRevision baseRevision=RebaseRevision(L"layout",51);
    const LayoutRevision adoptedRevision=RebaseRevision(L"layout",52);
    LayoutWin base=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009051}","firefox","base","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),100);
    LayoutWin disk=base;
    disk.activeTitle="disk";
    disk.lastSeenUtc=250;
    LayoutWin stale=base;
    stale.activeTitle="stale";
    stale.lastSeenUtc=250;
    std::map<std::string,RecordDelta> dirty;
    dirty[base.recordId]=RebaseUpsert(base,stale,baseRevision,250,11);

    const RebaseResult first=RebaseRecordDeltas(
        {disk},adoptedRevision,dirty,300);
    CHECK(first.records.size()==1 && SameLayoutWinFields(first.records[0],disk));
    CHECK(first.deferredConflictRecordIds.count(base.recordId)==1);

    std::map<std::string,DeferredRecordConflict> conflicts;
    CHECK(BuildDeferredRecordConflicts(
        first.deferredConflictRecordIds,first.records,adoptedRevision,conflicts));
    int serializations=0,writes=0;
    auto persist=[&](){
        if(DeferredRecordConflictsBlockPublish(conflicts,adoptedRevision))
            return false;
        ++serializations;
        ++writes;
        return true;
    };
    CHECK(!persist());
    CHECK(!persist());
    CHECK(serializations==0 && writes==0);

    const std::vector<LayoutWin> adoptedRecords=first.records;
    const std::map<std::string,RecordDelta> originalDirty=dirty;
    const std::map<std::string,DeferredRecordConflict> originalConflicts=conflicts;
    std::vector<LayoutWin> stagedRecords;
    std::map<std::string,RecordDelta> stagedDirty;
    std::map<std::string,DeferredRecordConflict> stagedConflicts;

    RecordDelta tied=RebaseUpsert(
        disk,stale,adoptedRevision,250,12);
    CHECK(StageRecordDeltaMutation(
        adoptedRecords,dirty,conflicts,tied,true,
        stagedRecords,stagedDirty,stagedConflicts)==
        RecordDeltaStageResult::DeferredConflict);
    CHECK(stagedRecords.empty() && stagedDirty.empty() && stagedConflicts.empty());
    CHECK(SameLayoutWinFields(adoptedRecords[0],disk));
    CHECK(SameLayoutWinFields(dirty.at(base.recordId).record,
                              originalDirty.at(base.recordId).record));
    CHECK(SameRevision(conflicts.at(base.recordId).adoptedRevision,
                       originalConflicts.at(base.recordId).adoptedRevision));
    CHECK(DeferredRecordConflictsBlockPublish(conflicts,adoptedRevision));

    RecordDelta older=tied;
    older.changedUtc=249;
    CHECK(StageRecordDeltaMutation(
        adoptedRecords,dirty,conflicts,older,true,
        stagedRecords,stagedDirty,stagedConflicts)==
        RecordDeltaStageResult::DeferredConflict);
    RecordDelta destructive=tied;
    destructive.kind=RecordDeltaKind::ExpireDelete;
    destructive.erase=true;
    CHECK(StageRecordDeltaMutation(
        adoptedRecords,dirty,conflicts,destructive,true,
        stagedRecords,stagedDirty,stagedConflicts)==
        RecordDeltaStageResult::DeferredConflict);

    LayoutWin unrelated=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009052}","chrome","other","b.test",2,
        G(L"{231A0000-0000-0000-0000-000000000002}"),251);
    RecordDelta unrelatedDelta;
    unrelatedDelta.kind=RecordDeltaKind::ExplicitUpsert;
    unrelatedDelta.record=unrelated;
    unrelatedDelta.baseRevision=adoptedRevision;
    unrelatedDelta.changedUtc=251;
    unrelatedDelta.causalGeneration=14;
    CHECK(StageRecordDeltaMutation(
        adoptedRecords,dirty,conflicts,unrelatedDelta,true,
        stagedRecords,stagedDirty,stagedConflicts)==
        RecordDeltaStageResult::Accepted);
    CHECK(stagedRecords.size()==2 && stagedConflicts.size()==1);
    CHECK(DeferredRecordConflictsBlockPublish(
        stagedConflicts,adoptedRevision));

    LayoutWin newer=disk;
    newer.activeTitle="newer";
    newer.lastSeenUtc=251;
    RecordDelta accepted=RebaseUpsert(
        disk,newer,adoptedRevision,251,13);
    CHECK(StageRecordDeltaMutation(
        adoptedRecords,dirty,conflicts,accepted,false,
        stagedRecords,stagedDirty,stagedConflicts)==
        RecordDeltaStageResult::DeferredConflict);
    CHECK(StageRecordDeltaMutation(
        adoptedRecords,dirty,conflicts,accepted,true,
        stagedRecords,stagedDirty,stagedConflicts)==
        RecordDeltaStageResult::Accepted);
    CHECK(stagedRecords.size()==1 && SameLayoutWinFields(stagedRecords[0],newer));
    CHECK(stagedConflicts.empty());
    CHECK(stagedDirty.size()==1);
    const RecordDelta& replacement=stagedDirty.at(base.recordId);
    CHECK(SameRevision(replacement.baseRevision,adoptedRevision));
    CHECK(replacement.baseRecordPresent &&
          SameLayoutWinFields(replacement.baseRecord,disk));
    CHECK(SameLayoutWinFields(replacement.record,newer));
    CHECK(!DeferredRecordConflictsBlockPublish(
        stagedConflicts,adoptedRevision));
}

static void test_rebased_publication_captures_adopted_disk_before_any_swap(){
    const LayoutRevision adoptedRevision=RebaseRevision(L"layout-B",62);
    LayoutWin adopted=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009061}","firefox","disk-B","b.test",2,
        G(L"{231A0000-0000-0000-0000-000000000002}"),620);
    DeskRec desktop;
    desktop.index=2;
    desktop.guid=adopted.desktop;
    desktop.name=L"B";
    RebasedAutoLayoutPublication publication;
    CHECK(BuildRebasedAutoLayoutPublication(
        {adopted},{desktop},adoptedRevision,{adopted.recordId},publication));
    CHECK(publication.records.size()==1 &&
          SameLayoutWinFields(publication.records[0],adopted));
    CHECK(publication.desktops.size()==1 &&
          GuidEq(publication.desktops[0].guid,desktop.guid));
    CHECK(SameRevision(publication.revision,adoptedRevision));
    CHECK(publication.conflicts.size()==1);
    const DeferredRecordConflict& conflict=
        publication.conflicts.find(adopted.recordId)->second;
    CHECK(conflict.adoptedRecordPresent &&
          SameLayoutWinFields(conflict.adoptedRecord,adopted));
    CHECK(SameRevision(conflict.adoptedRevision,adoptedRevision));

    RebasedAutoLayoutPublication sentinel=publication;
    sentinel.records[0].activeTitle="sentinel";
    CHECK(!BuildRebasedAutoLayoutPublication(
        {adopted},{desktop},adoptedRevision,{adopted.recordId},sentinel,
        [](){ throw std::bad_alloc(); }));
    CHECK(sentinel.records[0].activeTitle=="sentinel");
    CHECK(SameRevision(sentinel.revision,adoptedRevision));
}

static void test_durable_publish_commits_revision_without_copy_or_rewrite(){
    LayoutRevision current=RebaseRevision(L"layout-A",71);
    LayoutRevision published=RebaseRevision(L"layout-B",72);
    const LayoutRevision expected=published;
    bool dirty=true;
    int writes=0;
    auto persist=[&]{
        if(!dirty) return true;
        ++writes;
        CommitPublishedLayoutRevisionNoThrow(current,published);
        dirty=false;
        return true;
    };
    CHECK(noexcept(CommitPublishedLayoutRevisionNoThrow(current,published)));
    CHECK(persist());
    CHECK(SameRevision(current,expected));
    CHECK(!dirty && writes==1);
    CHECK(persist());
    CHECK(writes==1);
}

static void test_final_checkpoint_mutation_is_transactional_across_fault_matrix(){
    const LayoutRevision revision=RebaseRevision(L"layout",61);
    const UnixSeconds now=2000000600;
    LayoutWin kept=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009061}","firefox","old","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-100);
    LayoutWin erased=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009062}","chrome","gone","b.test",2,
        G(L"{231A0000-0000-0000-0000-000000000002}"),now-100);
    LayoutWin changed=kept;
    changed.activeTitle="new";
    changed.lastSeenUtc=now;
    LayoutWin added=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009063}","firefox","added","c.test",3,
        G(L"{231A0000-0000-0000-0000-000000000003}"),now);
    ValidatedRecordTouch touch;
    touch.recordId=changed.recordId;
    touch.lastSeenUtc=now;
    touch.causalGeneration=61;

    FinalCheckpointMutationState sentinel;
    sentinel.records.push_back(erased);
    RecordDelta sentinelDelta=RebaseUpsert(
        erased,erased,revision,now-1,60);
    sentinel.deltas[erased.recordId]=sentinelDelta;
    sentinel.touches[erased.recordId]=touch;
    sentinel.provisionalRecordByRuntime["sentinel-runtime"]=erased.recordId;
    const std::map<std::string,std::string> finalProvisional={
        {"new-runtime",added.recordId}};
    const std::vector<FinalCheckpointFaultPoint> faults={
        FinalCheckpointFaultPoint::InitialCopies,
        FinalCheckpointFaultPoint::RecordIndexes,
        FinalCheckpointFaultPoint::EraseDelta,
        FinalCheckpointFaultPoint::UpsertDelta,
        FinalCheckpointFaultPoint::ValidatedTouches,
        FinalCheckpointFaultPoint::FinalRecords,
        FinalCheckpointFaultPoint::Publish
    };
    for(FinalCheckpointFaultPoint failedAt : faults){
        FinalCheckpointMutationState output=sentinel;
        CHECK(!BuildFinalCheckpointMutation(
            {kept,erased},{changed,added},{}, {}, {}, {touch},
            finalProvisional,revision,now,61,output,
            [&](FinalCheckpointFaultPoint point){
                if(point==failedAt) throw std::bad_alloc();
            }));
        CHECK(output.records.size()==1 &&
              SameLayoutWinFields(output.records[0],erased));
        CHECK(output.deltas.size()==1 &&
              output.deltas.count(erased.recordId)==1);
        CHECK(output.touches.size()==1 &&
              output.touches.count(erased.recordId)==1);
        CHECK(output.provisionalRecordByRuntime.size()==1 &&
              output.provisionalRecordByRuntime.count("sentinel-runtime")==1);
    }

    FinalCheckpointMutationState staged;
    CHECK(BuildFinalCheckpointMutation(
        {kept,erased},{changed,added},{}, {}, {}, {touch},finalProvisional,
        revision,now,61,staged));
    CHECK(staged.records.size()==2 &&
          SameLayoutWinFields(staged.records[0],changed) &&
          SameLayoutWinFields(staged.records[1],added));
    CHECK(staged.deltas.size()==3);
    CHECK(staged.deltas.at(erased.recordId).erase);
    CHECK(SameLayoutWinFields(staged.deltas.at(changed.recordId).record,changed));
    CHECK(SameLayoutWinFields(staged.deltas.at(added.recordId).record,added));
    CHECK(staged.touches.size()==1 &&
          staged.touches.at(changed.recordId).causalGeneration==61);
    CHECK(staged.provisionalRecordByRuntime==finalProvisional);
}

static void test_expire_delete_discards_validated_touch_before_external_rebase(){
    const LayoutRevision revisionA=RebaseRevision(L"layout",64);
    const LayoutRevision revisionD=RebaseRevision(L"layout",65);
    const UnixSeconds now=2000000650;
    LayoutWin erased=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009064}","firefox","gone","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-100);
    ValidatedRecordTouch staleTouch;
    staleTouch.recordId=erased.recordId;
    staleTouch.lastSeenUtc=now-50;
    staleTouch.causalGeneration=64;
    const std::map<std::string,ValidatedRecordTouch> originalTouches={
        {erased.recordId,staleTouch}};

    FinalCheckpointMutationState checkpoint;
    CHECK(BuildFinalCheckpointMutation(
        {erased},{},{},originalTouches,{}, {}, {},revisionA,now,64,checkpoint));
    CHECK(checkpoint.records.empty());
    CHECK(checkpoint.deltas.size()==1 &&
          checkpoint.deltas.at(erased.recordId).erase);
    CHECK(checkpoint.touches.count(erased.recordId)==0);

    LayoutWin external=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009065}","chrome","external-D","d.test",2,
        G(L"{231A0000-0000-0000-0000-000000000002}"),now);
    RecordDeltaRebasePreparation prepared;
    CHECK(PrepareRecordDeltasForRebase(
        {erased,external},checkpoint.deltas,{},prepared));
    RebaseResult rebased=RebaseRecordDeltas(
        {erased,external},revisionD,prepared.deltas,now+1);
    CHECK(rebased.deferredConflictRecordIds.empty());
    CHECK(rebased.appliedDeleteRecordIds.count(erased.recordId)==1);
    CHECK(rebased.records.size()==1 &&
          SameLayoutWinFields(rebased.records[0],external));

    std::map<std::string,ValidatedRecordTouch> touchesForRebase;
    CHECK(PrepareValidatedTouchesForRebase(
        originalTouches,rebased.appliedDeleteRecordIds,touchesForRebase));
    CHECK(touchesForRebase.empty());
    TouchRebaseResult touched=ReapplyValidatedTouches(
        rebased.records,touchesForRebase,{});
    CHECK(touched.deferredRecordIds.empty());

    RebasedResidualJournal residual;
    CHECK(BuildRebasedResidualJournal(
        checkpoint.deltas,originalTouches,prepared,rebased,touched,
        {erased,external},revisionD,residual));
    CHECK(residual.touches.count(erased.recordId)==0);
}

static void test_final_observation_provisional_map_stages_before_global_publish(){
    const std::map<std::string,std::string> current={
        {"old-runtime","{00000000-0000-0000-0000-000000009071}"}};
    std::map<std::string,std::string> outputMap={
        {"sentinel-runtime","{00000000-0000-0000-0000-000000009072}"}};
    FinalAppObservation sentinel;
    sentinel.app="sentinel";
    std::vector<FinalAppObservation> output={sentinel};
    CHECK(!StageFinalObservationsAndProvisionals(
        current,output,outputMap,
        [](std::map<std::string,std::string>& staged,
           std::vector<FinalAppObservation>& observations)->bool {
            staged["new-runtime"]=
                "{00000000-0000-0000-0000-000000009073}";
            FinalAppObservation app;
            app.app="firefox";
            observations.push_back(app);
            throw std::bad_alloc();
        }));
    CHECK(current.size()==1 && current.count("old-runtime")==1);
    CHECK(output.size()==1 && output[0].app=="sentinel");
    CHECK(outputMap.size()==1 && outputMap.count("sentinel-runtime")==1);

    CHECK(StageFinalObservationsAndProvisionals(
        current,output,outputMap,
        [](std::map<std::string,std::string>& staged,
           std::vector<FinalAppObservation>& observations)->bool {
            staged["new-runtime"]=
                "{00000000-0000-0000-0000-000000009073}";
            FinalAppObservation app;
            app.app="firefox";
            observations.push_back(app);
            return true;
        }));
    CHECK(current.size()==1 && current.count("old-runtime")==1);
    CHECK(output.size()==1 && output[0].app=="firefox");
    CHECK(outputMap.size()==2 && outputMap.count("old-runtime")==1 &&
          outputMap.count("new-runtime")==1);
}

struct ReconcileResultSink {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::unique_ptr<ReconcileResult> > results;
    std::atomic<bool> wrongMessage{false};

    bool post(HWND,UINT message,WPARAM,LPARAM value){
        if(message!=WM_RECONCILE_RESULT) wrongMessage=true;
        std::unique_ptr<ReconcileResult> owned(
            reinterpret_cast<ReconcileResult*>(value));
        {
            std::lock_guard<std::mutex> lock(mutex);
            results.push_back(std::move(owned));
        }
        changed.notify_all();
        return true;
    }

    std::unique_ptr<ReconcileResult> waitFor(
            uint64_t operationId,
            std::chrono::milliseconds timeout=std::chrono::seconds(5)){
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock,timeout,[&]{
            for(const auto& result : results)
                if(result->operationId==operationId) return true;
            return false;
        });
        for(auto it=results.begin();it!=results.end();++it){
            if((*it)->operationId!=operationId) continue;
            std::unique_ptr<ReconcileResult> found=std::move(*it);
            results.erase(it);
            return found;
        }
        return std::unique_ptr<ReconcileResult>();
    }
};

struct BlockingReconcilePlanner {
    std::mutex mutex;
    std::condition_variable changed;
    bool firstEntered=false;
    bool released=false;
    int calls=0;
    std::thread::id workerThread;

    ReconcilePlan plan(const ReconcileRequest& request){
        std::unique_lock<std::mutex> lock(mutex);
        ++calls;
        workerThread=std::this_thread::get_id();
        if(calls==1){
            firstEntered=true;
            changed.notify_all();
            changed.wait(lock,[&]{ return released; });
        }
        ReconcilePlan output;
        output.app=request.app;
        output.nowUtc=request.nowUtc;
        output.freshness=request.freshness;
        return output;
    }

    bool waitEntered(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(5),[&]{
            return firstEntered;
        });
    }

    void release(){
        std::lock_guard<std::mutex> lock(mutex);
        released=true;
        changed.notify_all();
    }
};

static ReconcileRequest WorkerReconcileRequest(uint64_t operationId){
    ReconcileRequest request;
    request.operationId=operationId;
    request.app="firefox";
    request.identityGeneration=11;
    request.contentGeneration=21+operationId;
    request.sessionRequestId=31+operationId;
    request.sessionDataGeneration=41+operationId;
    request.nowUtc=1700000000;
    request.freshness=ReconcileFreshness::Fresh;
    LayoutWin saved=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009101}","firefox","A","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),1699999900);
    request.saved.push_back(saved);
    request.live.push_back(saved);
    return request;
}

static ReconcileRequest WorkerPreparedRequest(uint64_t operationId,
                                               const wchar_t* firstTitle){
    ReconcileRequest request=WorkerReconcileRequest(operationId);
    request.live.clear();
    request.buildLiveFromInputs=true;
    request.workMode=ReconcileWorkMode::Plan;
    DeskRec firstDesktop;
    firstDesktop.index=7;
    firstDesktop.guid=G(L"{231A0000-0000-0000-0000-000000009201}");
    firstDesktop.name=L"Seven";
    DeskRec secondDesktop;
    secondDesktop.index=9;
    secondDesktop.guid=G(L"{231A0000-0000-0000-0000-000000009202}");
    secondDesktop.name=L"Nine";
    request.desktops={firstDesktop,secondDesktop};
    request.titleSuffixes={L" - Browser"};
    request.fastWindows={
        SnapshotWindow(0x9201,9201,19201,firstTitle,firstDesktop.guid),
        SnapshotWindow(0x9202,9202,19202,L"Other - Browser",secondDesktop.guid)
    };
    std::shared_ptr<std::vector<WinFp> > session(new std::vector<WinFp>());
    WinFp other;
    other.activeTitle="Other";
    other.activeDomain="other.test";
    other.tabCount=2;
    other.tabsBlob="other-tabs";
    WinFp first;
    first.activeTitle=W2U8(std::wstring(firstTitle).substr(
        0,std::wstring(firstTitle).size()-std::wstring(L" - Browser").size()));
    first.activeDomain="first.test";
    first.tabCount=3;
    first.tabsBlob="first-tabs";
    session->push_back(other);
    session->push_back(first);
    request.sessionWindows=session;
    return request;
}

static void test_reconcile_live_preparation_is_ordered_and_search_ready(){
    ReconcileRequest request=WorkerPreparedRequest(140,L"Inbox - Browser");
    PreparedReconcileLive prepared;
    CHECK(BuildReconcileLivePreparation(request,prepared));
    CHECK(prepared.live.size()==2);
    CHECK(prepared.sessionIndexByFast.size()==2 &&
        prepared.sessionIndexByFast[0]==1 && prepared.sessionIndexByFast[1]==0);
    CHECK(request.sessionWindows->at(
        static_cast<size_t>(prepared.sessionIndexByFast[0])).tabsBlob=="first-tabs");
    CHECK(prepared.live[0].activeTitle=="Inbox" &&
        prepared.live[0].activeDomain=="first.test" &&
        prepared.live[0].tabCount==3 && prepared.live[0].deskIndex==7);
    CHECK(prepared.live[1].activeTitle=="Other" &&
        prepared.live[1].activeDomain=="other.test" &&
        prepared.live[1].deskIndex==9);
    CHECK(reinterpret_cast<uintptr_t>(request.fastWindows[0].hwnd)==0x9201);
    CHECK(request.sessionWindows && request.sessionWindows->at(1).tabsBlob=="first-tabs");
}

static void test_cli_profile_batch_aborts_transactionally_on_first_prep_failure(){
    ReconcileRequest firefox=WorkerPreparedRequest(146,L"Firefox - Browser");
    firefox.app="firefox";
    ReconcileRequest chrome=WorkerPreparedRequest(147,L"Chrome - Browser");
    chrome.app="chrome";
    std::vector<ReconcileRequest> requests={firefox,chrome};

    PreparedCliProfileBatch output;
    LayoutWin retainedLive;
    retainedLive.app="retained";
    retainedLive.activeTitle="manual-before";
    output.live.push_back(retainedLive);
    FastWin retainedFast;
    retainedFast.app="retained";
    retainedFast.hwnd=reinterpret_cast<HWND>(static_cast<uintptr_t>(0xCAFE));
    output.fastWindows.push_back(retainedFast);

    int preparationCalls=0;
    std::string manualBytes="manual-before";
    size_t moveCount=0;
    bool prepared=BuildCliProfileBatch(
        requests,output,
        [&](const ReconcileRequest& request,PreparedReconcileLive& built){
            ++preparationCalls;
            if(request.app=="firefox") return false;
            return BuildReconcileLivePreparation(request,built);
        });
    if(prepared){
        manualBytes="manual-after";
        moveCount=output.fastWindows.size();
    }

    CHECK(!prepared);
    CHECK(preparationCalls==1);
    CHECK(output.live.size()==1 && output.live[0].app=="retained" &&
          output.live[0].activeTitle=="manual-before");
    CHECK(output.fastWindows.size()==1 &&
          reinterpret_cast<uintptr_t>(output.fastWindows[0].hwnd)==0xCAFE);
    CHECK(manualBytes=="manual-before");
    CHECK(moveCount==0);
}

static void test_cli_loads_settings_before_selecting_active_profiles(){
    bool firefox=true;
    bool chrome=false;
    bool edge=true;
    bool settingsLoaded=false;
    int dispatches=0;

    const int result=RunCliWithLoadedSettings(
        [&]{
            firefox=false;
            chrome=true;
            edge=false;
            settingsLoaded=true;
        },
        [&]{
            ++dispatches;
            CHECK(settingsLoaded);
            const std::vector<AppProfile> profiles=
                BuiltinProfiles(firefox,chrome,edge);
            CHECK(profiles.size()==1 && profiles[0].id=="chrome");
            return 37;
        });

    CHECK(result==37);
    CHECK(dispatches==1);
}

static void test_cli_save_revalidates_snapshot_and_desktops_before_publish(){
    FastWin fast;
    fast.app="firefox";
    fast.hwnd=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x9541));
    fast.pid=9541;
    fast.processStart=954100;
    fast.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    fast.title=L"Inbox";
    AppFastSnapshot snapshot;
    snapshot.windows.push_back(fast);
    snapshot.identityGeneration=41;
    snapshot.generation=42;
    std::map<std::string,AppFastSnapshot> captured={{"firefox",snapshot}};
    std::map<std::string,AppFastSnapshot> current=captured;
    DeskRec desktop;
    desktop.index=0;
    desktop.guid=fast.desktop;
    std::vector<DeskRec> desktops={desktop};
    CHECK(CliCheckpointInputsStillCurrent(
        captured,current,desktops,desktops));

    current["firefox"].generation=43;
    CHECK(!CliCheckpointInputsStillCurrent(
        captured,current,desktops,desktops));
    current=captured;
    current["firefox"].desktopLookupsComplete=false;
    CHECK(!CliCheckpointInputsStillCurrent(
        captured,current,desktops,desktops));
    current=captured;
    current["chrome"]=snapshot;
    CHECK(!CliCheckpointInputsStillCurrent(
        captured,current,desktops,desktops));
    current=captured;
    std::vector<DeskRec> changedDesktops=desktops;
    changedDesktops[0].guid=
        G(L"{231A0000-0000-0000-0000-000000000002}");
    CHECK(!CliCheckpointInputsStillCurrent(
        captured,current,desktops,changedDesktops));
}

static void test_manual_save_incomplete_snapshot_keeps_prior_bytes_without_write(){
    const GUID desktop=G(L"{231A0000-0000-0000-0000-000000009401}");
    const std::vector<DeskRec> desktops={{0,desktop,L"one"}};
    SnapshotVersionTracker tracker;
    AppFastSnapshot capturedSnapshot;
    capturedSnapshot.windows.push_back(
        SnapshotWindow(9401,9402,9403,L"",desktop));
    FinalizeFastSnapshot("firefox",1,tracker,capturedSnapshot);
    std::map<std::string,AppFastSnapshot> captured={
        {"firefox",capturedSnapshot}};
    std::map<std::string,AppFastSnapshot> current=captured;
    std::string manualBytes="prior-manual-layout-bytes";
    const std::string candidate="new-checked-v4-bytes";
    int writes=0;
    auto write=[&](const std::string& bytes){
        ++writes;
        manualBytes=bytes;
        return true;
    };

    current["firefox"].enumerationComplete=false;
    CHECK(!PublishManualSnapshotIfCurrent(
        captured,current,desktops,desktops,candidate,write));
    CHECK(writes==0 && manualBytes=="prior-manual-layout-bytes");

    current=captured;
    current["firefox"].desktopLookupsComplete=false;
    current["firefox"].windows[0].desktop=GUID{};
    CHECK(!PublishManualSnapshotIfCurrent(
        captured,current,desktops,desktops,candidate,write));
    CHECK(writes==0 && manualBytes=="prior-manual-layout-bytes");

    std::vector<DeskRec> changedDesktops=desktops;
    changedDesktops[0].guid=G(L"{231A0000-0000-0000-0000-000000009402}");
    CHECK(!PublishManualSnapshotIfCurrent(
        captured,captured,desktops,changedDesktops,candidate,write));
    CHECK(writes==0 && manualBytes=="prior-manual-layout-bytes");

    CHECK(PublishManualSnapshotIfCurrent(
        captured,captured,desktops,desktops,candidate,write));
    CHECK(writes==1 && manualBytes==candidate);
}

static void test_cli_status_keeps_fast_rows_when_fingerprints_unavailable(){
    FastWin fast;
    fast.app="firefox";
    fast.hwnd=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x9551));
    fast.pid=9551;
    fast.processStart=955100;
    fast.desktop=G(L"{231A0000-0000-0000-0000-000000000001}");
    fast.title=L"Known title";
    AppFastSnapshot snapshot;
    snapshot.windows.push_back(fast);
    snapshot.identityGeneration=51;
    snapshot.generation=52;
    DeskRec desktop;
    desktop.index=7;
    desktop.guid=fast.desktop;
    std::vector<CliStatusRow> rows;
    CHECK(BuildCliStatusRows(snapshot,{desktop},nullptr,false,rows));
    CHECK(rows.size()==1 && SameIdentity(
        IdentityOf(rows[0].window),IdentityOf(fast)));
    CHECK(rows[0].deskIndex==7 && rows[0].activeTitle=="Known title");
    CHECK(!rows[0].fingerprintAvailable && rows[0].tabCount==-1);
}

static void test_reconcile_copied_text_budget_covers_every_owned_string(){
    ReconcileRequest request;
    request.app=std::string(1,'a');
    LayoutWin saved;
    saved.recordId=std::string(2,'b');
    saved.app=std::string(3,'c');
    saved.activeTitle=std::string(4,'d');
    saved.activeDomain=std::string(5,'e');
    saved.counts[std::string(6,'f')]=1;
    request.saved.push_back(saved);
    LayoutWin live;
    live.recordId=std::string(7,'g');
    live.app=std::string(8,'h');
    live.activeTitle=std::string(9,'i');
    live.activeDomain=std::string(10,'j');
    live.counts[std::string(11,'k')]=1;
    request.live.push_back(live);
    request.reservedRecordIds.insert(std::string(12,'l'));
    FastWin fast;
    fast.app=std::string(13,'m');
    fast.title=std::wstring(14,L'n');
    request.fastWindows.push_back(fast);
    DeskRec desktop;
    desktop.name=std::wstring(15,L'o');
    request.desktops.push_back(desktop);
    request.titleSuffixes.push_back(std::wstring(16,L'p'));
    const size_t exactBytes=91+45*sizeof(wchar_t);
    CHECK(ReconcileRequestTextWithinBudget(request,exactBytes));
    CHECK(!ReconcileRequestTextWithinBudget(request,exactBytes-1));

    ReconcileRequest oversized=WorkerPreparedRequest(145,L"Bounded - Browser");
    oversized.titleSuffixes[0].assign(
        MAX_RECONCILE_TITLE_SUFFIX_CHARS+1,L'x');
    PreparedReconcileLive untouched;
    untouched.sessionIndexByFast.push_back(42);
    CHECK(!BuildReconcileLivePreparation(oversized,untouched));
    CHECK(untouched.live.empty() && untouched.sessionIndexByFast.size()==1 &&
        untouched.sessionIndexByFast[0]==42);
}

struct BlockingLivePreparer {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false;
    bool released=false;
    int calls=0;
    std::thread::id workerThread;

    bool build(const ReconcileRequest& request,PreparedReconcileLive& output){
        {
            std::unique_lock<std::mutex> lock(mutex);
            ++calls;
            workerThread=std::this_thread::get_id();
            if(calls==1){
                entered=true;
                changed.notify_all();
                changed.wait(lock,[&]{ return released; });
            }
        }
        return BuildReconcileLivePreparation(request,output);
    }

    bool waitEntered(){
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock,std::chrono::seconds(5),[&]{ return entered; });
    }

    void release(){
        std::lock_guard<std::mutex> lock(mutex);
        released=true;
        changed.notify_all();
    }
};

static void test_reconcile_worker_prepares_live_inputs_off_thread_and_coalesces(){
    ReconcileResultSink sink;
    BlockingLivePreparer blocker;
    std::atomic<int> planned{0};
    std::atomic<bool> plannerSawAligned{true};
    ReconcileWorkerOps ops;
    ops.buildLive=[&](const ReconcileRequest& request,PreparedReconcileLive& output){
        return blocker.build(request,output);
    };
    ops.plan=[&](const ReconcileRequest& request){
        ++planned;
        ReconcilePlan output;
        output.app=request.app;
        output.nowUtc=request.nowUtc;
        output.freshness=request.freshness;
        if(request.live.size()!=request.fastWindows.size())
            plannerSawAligned.store(false);
        return output;
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        return sink.post(hwnd,message,wp,lp);
    };
    ReconcileWorker worker(reinterpret_cast<HWND>(1),ops);
    const std::thread::id caller=std::this_thread::get_id();
    ReconcileRequest firstRequest=WorkerPreparedRequest(141,L"First - Browser");
    const auto firstSession=firstRequest.sessionWindows;
    CHECK(worker.Request(firstRequest));
    firstRequest.fastWindows[0].title=L"caller-mutated";
    firstRequest.titleSuffixes[0]=L"caller-mutated";
    firstRequest.desktops[0].index=99;
    CHECK(blocker.waitEntered());
    CHECK(worker.Request(WorkerPreparedRequest(142,L"Pending - Browser")));
    CHECK(worker.Request(WorkerPreparedRequest(143,L"Newest - Browser")));
    CHECK(worker.ActiveCount()==1 && worker.PendingCount()==1 &&
        worker.OutstandingForApp("firefox")==2);
    std::unique_ptr<ReconcileResult> superseded=sink.waitFor(142);
    CHECK(superseded && superseded->status==ReconcileResultStatus::Superseded);
    CHECK(superseded && superseded->fastWindows.size()==2 &&
        superseded->fastWindows[0].title==L"Pending - Browser");
    blocker.release();
    std::unique_ptr<ReconcileResult> first=sink.waitFor(141);
    std::unique_ptr<ReconcileResult> newest=sink.waitFor(143);
    CHECK(first && first->status==ReconcileResultStatus::Completed);
    CHECK(newest && newest->status==ReconcileResultStatus::Completed);
    CHECK(first && first->fastWindows.size()==2 &&
        first->fastWindows[0].title==L"First - Browser" &&
        first->desktops[0].index==7 &&
        first->titleSuffixes[0]==L" - Browser");
    CHECK(first && first->sessionWindows.get()==firstSession.get());
    CHECK(first && first->live.size()==2 &&
        first->sessionIndexByFast.size()==2);
    const WinFp* firstBound=first ? ReconcileSessionForFast(*first,0) : nullptr;
    CHECK(firstBound && firstBound->tabsBlob=="first-tabs");
    CHECK(blocker.workerThread!=caller && planned.load()==2 && blocker.calls==2 &&
        plannerSawAligned.load());
    CHECK(worker.Stop());
}

static void test_reconcile_worker_prepare_only_skips_planner_for_search(){
    ReconcileResultSink sink;
    std::atomic<int> planned{0};
    ReconcileWorkerOps ops;
    ops.plan=[&](const ReconcileRequest&)->ReconcilePlan{
        ++planned;
        throw std::runtime_error("planner must not run");
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        return sink.post(hwnd,message,wp,lp);
    };
    ReconcileWorker worker(reinterpret_cast<HWND>(1),ops);
    ReconcileRequest request=WorkerPreparedRequest(144,L"Search - Browser");
    request.workMode=ReconcileWorkMode::PrepareLiveOnly;
    CHECK(worker.Request(request));
    std::unique_ptr<ReconcileResult> result=sink.waitFor(144);
    CHECK(result && result->status==ReconcileResultStatus::Completed);
    CHECK(result && result->workMode==ReconcileWorkMode::PrepareLiveOnly);
    CHECK(result && result->fastWindows.size()==2 && result->live.size()==2);
    const WinFp* bound=result ? ReconcileSessionForFast(*result,0) : nullptr;
    CHECK(bound && bound->tabsBlob=="first-tabs");
    CHECK(planned.load()==0);
    CHECK(worker.Stop());
}

static void test_reconcile_worker_is_bounded_coalesced_and_nonblocking(){
    ReconcileResultSink sink;
    BlockingReconcilePlanner blocker;
    ReconcileWorkerOps ops;
    ops.plan=[&](const ReconcileRequest& request){ return blocker.plan(request); };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        return sink.post(hwnd,message,wp,lp);
    };
    ReconcileWorker worker(reinterpret_cast<HWND>(1),ops);
    const std::thread::id caller=std::this_thread::get_id();
    CHECK(worker.Request(WorkerReconcileRequest(100)));
    CHECK(blocker.waitEntered());
    CHECK(worker.Request(WorkerReconcileRequest(101)));
    CHECK(worker.Request(WorkerReconcileRequest(102)));
    CHECK(worker.ActiveCount()==1);
    CHECK(worker.PendingCount()==1);
    CHECK(worker.OutstandingForApp("firefox")==2);

    std::unique_ptr<ReconcileResult> superseded=sink.waitFor(101);
    CHECK(superseded && superseded->status==ReconcileResultStatus::Superseded);
    CHECK(superseded && superseded->contentGeneration==122);
    blocker.release();
    std::unique_ptr<ReconcileResult> first=sink.waitFor(100);
    std::unique_ptr<ReconcileResult> newest=sink.waitFor(102);
    CHECK(first && first->status==ReconcileResultStatus::Completed);
    CHECK(newest && newest->status==ReconcileResultStatus::Completed);
    CHECK(first && first->saved.size()==1 && first->live.size()==1);
    CHECK(newest && newest->saved.size()==1 && newest->live.size()==1);
    CHECK(blocker.workerThread!=caller);
    CHECK(!sink.wrongMessage.load());
    CHECK(worker.Stop());
}

static void test_reconcile_worker_planner_failure_is_owned_and_thread_survives(){
    ReconcileResultSink sink;
    std::atomic<int> calls{0};
    ReconcileWorkerOps ops;
    ops.plan=[&](const ReconcileRequest& request)->ReconcilePlan{
        if(++calls==1) throw std::bad_alloc();
        ReconcilePlan output;
        output.app=request.app;
        output.nowUtc=request.nowUtc;
        output.freshness=request.freshness;
        return output;
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        return sink.post(hwnd,message,wp,lp);
    };
    ReconcileWorker worker(reinterpret_cast<HWND>(1),ops);
    CHECK(worker.Request(WorkerReconcileRequest(110)));
    std::unique_ptr<ReconcileResult> failed=sink.waitFor(110);
    CHECK(failed && failed->status==ReconcileResultStatus::Failed);
    CHECK(worker.Request(WorkerReconcileRequest(111)));
    std::unique_ptr<ReconcileResult> recovered=sink.waitFor(111);
    CHECK(recovered && recovered->status==ReconcileResultStatus::Completed);
    CHECK(worker.Stop());
}

static void test_reconcile_worker_accepted_request_owns_failure_result(){
    ReconcileResultSink sink;
    std::atomic<int> allocations{0};
    ReconcileWorkerOps ops;
    ops.makeResult=[&](){
        if(++allocations!=1) return std::unique_ptr<ReconcileResult>();
        return std::unique_ptr<ReconcileResult>(new ReconcileResult());
    };
    ops.plan=[](const ReconcileRequest&)->ReconcilePlan{
        throw std::bad_alloc();
    };
    ops.postMessage=[&](HWND hwnd,UINT message,WPARAM wp,LPARAM lp){
        return sink.post(hwnd,message,wp,lp);
    };
    ReconcileWorker worker(reinterpret_cast<HWND>(1),ops);
    CHECK(worker.Request(WorkerReconcileRequest(112)));
    std::unique_ptr<ReconcileResult> failed=
        sink.waitFor(112,std::chrono::milliseconds(250));
    CHECK(failed && failed->status==ReconcileResultStatus::Failed);
    CHECK(allocations.load()==1);
    CHECK(worker.Stop());
}

static void test_reconcile_post_failure_deadlines_all_operation_owners(){
    std::mutex mutex;
    std::condition_variable changed;
    size_t postAttempts=0;
    ReconcileWorkerOps ops;
    ops.postMessage=[&](HWND,UINT,WPARAM,LPARAM){
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++postAttempts;
        }
        changed.notify_all();
        return false;
    };
    ReconcileWorker worker(reinterpret_cast<HWND>(1),ops);
    AsyncReconcileDeadlineGate deadlines;
    // Auto, manual save, manual restore and search each own a distinct
    // operation id even when the bounded worker coalesces by application.
    const uint64_t operationIds[]={113,114,115,116};
    for(size_t index=0;index<4;++index){
        CHECK(deadlines.begin(operationIds[index],1000+index));
        CHECK(worker.Request(WorkerReconcileRequest(operationIds[index])));
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock,std::chrono::seconds(5),[&]{
            return postAttempts>=index+1;
        }));
    }
    for(uint64_t operationId : operationIds)
        CHECK(deadlines.pending(operationId)==1);

    std::vector<uint64_t> expired;
    CHECK(deadlines.expire(1003+AsyncReconcileDeadlineGate::maxLifetimeMs(),
                           expired)==4);
    std::sort(expired.begin(),expired.end());
    CHECK(expired==std::vector<uint64_t>({113,114,115,116}));
    for(uint64_t operationId : operationIds)
        CHECK(!deadlines.complete(operationId));
    CHECK(deadlines.expire(UINT64_MAX,expired)==0 && expired.empty());
    CHECK(worker.Stop());
}

static void test_reconcile_worker_rejects_invalid_or_oversized_requests(){
    ReconcileWorker worker(reinterpret_cast<HWND>(1));
    ReconcileRequest request=WorkerReconcileRequest(120);
    request.operationId=0;
    CHECK(!worker.Request(request));
    request=WorkerReconcileRequest(121);
    request.contentGeneration=0;
    CHECK(!worker.Request(request));
    request=WorkerReconcileRequest(122);
    request.app="unsupported";
    CHECK(!worker.Request(request));
    request=WorkerReconcileRequest(123);
    request.saved.resize(MAX_LAYOUT_RECORDS+1);
    CHECK(!worker.Request(request));
    CHECK(worker.Stop());
}

static std::vector<LayoutMatch> TooComplexReconcileMatcher(
        const std::vector<LayoutWin>&,const std::vector<LayoutWin>&,
        double,bool* tooComplex){
    if(tooComplex) *tooComplex=true;
    return {};
}

static void test_reconcile_plan_distinguishes_complexity_deferral(){
    ReconcileRequest request=WorkerReconcileRequest(130);
    ReconcilePlan plan=PlanAppReconcile(
        request.saved,request.live,request.app,request.nowUtc,{},
        request.freshness,NewRecordId,TooComplexReconcileMatcher);
    CHECK(plan.deferred);
    CHECK(plan.tooComplex);
}

static void test_posted_reconcile_results_are_drained(){
    HWND window=CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,
                                nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window!=nullptr);
    if(!window) return;
    ReconcileResult* first=new ReconcileResult();
    ReconcileResult* second=new ReconcileResult();
    CHECK(PostMessageW(window,WM_RECONCILE_RESULT,0,
                       reinterpret_cast<LPARAM>(first))!=FALSE);
    CHECK(PostMessageW(window,WM_RECONCILE_RESULT,0,
                       reinterpret_cast<LPARAM>(second))!=FALSE);
    CHECK(DrainPostedReconcileResults(window)==2);
    CHECK(DrainPostedReconcileResults(window)==0);
    CHECK(DestroyWindow(window)!=FALSE);
}

static void test_reconcile_consumer_ignores_stale_content_generation(){
    ReconcileResultConsumerKey expected;
    expected.operationId=9301;
    expected.app="firefox";
    expected.workMode=ReconcileWorkMode::Plan;
    expected.identityGeneration=9302;
    expected.contentGeneration=9303;
    expected.sessionRequestId=9304;
    expected.sessionDataGeneration=9305;

    ReconcileResult result;
    result.status=ReconcileResultStatus::Completed;
    result.operationId=expected.operationId;
    result.app=expected.app;
    result.workMode=expected.workMode;
    result.identityGeneration=expected.identityGeneration;
    result.contentGeneration=expected.contentGeneration;
    result.sessionRequestId=expected.sessionRequestId;
    result.sessionDataGeneration=expected.sessionDataGeneration;
    int mutations=0;
    auto stale=[&](const ReconcileResult& candidate){
        const int before=mutations;
        CHECK(!ConsumeReconcileResultIfCurrent(
            candidate,expected,[&](){ ++mutations; }));
        CHECK(mutations==before);
    };
    ReconcileResult changed=result;
    changed.operationId++;
    stale(changed);
    changed=result; changed.app="chrome"; stale(changed);
    changed=result; changed.workMode=ReconcileWorkMode::PrepareLiveOnly; stale(changed);
    changed=result; changed.identityGeneration++; stale(changed);
    changed=result; changed.contentGeneration++; stale(changed);
    changed=result; changed.sessionRequestId++; stale(changed);
    changed=result; changed.sessionDataGeneration++; stale(changed);
    changed=result; changed.status=ReconcileResultStatus::Superseded; stale(changed);

    CHECK(ConsumeReconcileResultIfCurrent(
        result,expected,[&](){ ++mutations; }));
    CHECK(mutations==1);
    CHECK(!ConsumeReconcileResultIfCurrent(
        result,expected,[&](){ ++mutations; throw std::bad_alloc(); }));
    CHECK(mutations==2);
}

static FinalWindowObservation FinalObserved(const char* app,const char* title,
        const GUID& desktop,const std::string& provisionalId){
    FinalWindowObservation observation;
    observation.observed=MatchRecord(app,title,"",0,{});
    observation.observed.app=app;
    observation.observed.desktop=desktop;
    observation.observed.deskIndex=1;
    observation.desktopValid=!GuidIsZero(desktop);
    observation.provisionalRecordId=provisionalId;
    return observation;
}

static void test_final_snapshot_captures_immediately_opened_new_window(){
    const UnixSeconds now=1700001000;
    FinalWindowObservation opened=FinalObserved(
        "firefox","opened-now",
        G(L"{231A0000-0000-0000-0000-000000000002}"),
        "{00000000-0000-0000-0000-000000009201}");
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows.push_back(opened);
    FinalSnapshotResult result=CommitFinalSnapshotRecords({}, {app}, now);
    CHECK(result.valid && result.records.size()==1);
    CHECK(result.records[0].recordId==opened.provisionalRecordId);
    CHECK(result.records[0].activeTitle=="opened-now");
    CHECK(GuidEq(result.records[0].desktop,opened.observed.desktop));
    CHECK(result.records[0].lastSeenUtc==now &&
          result.records[0].missingSinceUtc==0);
}

static void test_final_snapshot_marks_unbound_additions_provisional_independent_of_title(){
    const UnixSeconds now=1700001500;
    FinalWindowObservation opened=FinalObserved(
        "firefox","already titled",
        G(L"{231A0000-0000-0000-0000-000000000002}"),
        "{00000000-0000-0000-0000-000000009206}");
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows.push_back(opened);

    FinalSnapshotResult result=CommitFinalSnapshotRecords({}, {app}, now);

    CHECK(result.valid && result.records.size()==1);
    CHECK(result.records[0].provisional);
}

static void test_final_snapshot_failed_reappeared_keeps_destination_and_adds_sibling(){
    const UnixSeconds now=1700002000;
    LayoutWin saved=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009211}","firefox","saved","a.test",2,
        G(L"{231A0000-0000-0000-0000-000000000002}"),now-100);
    saved.missingSinceUtc=now-50;
    FinalWindowObservation failed=FinalObserved(
        "firefox","saved",
        G(L"{231A0000-0000-0000-0000-000000000009}"),"");
    failed.pendingRecordId=saved.recordId;
    FinalWindowObservation sibling=FinalObserved(
        "firefox","new sibling",
        G(L"{231A0000-0000-0000-0000-000000000003}"),
        "{00000000-0000-0000-0000-000000009212}");
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows={failed,sibling};
    FinalSnapshotResult result=CommitFinalSnapshotRecords({saved},{app},now);
    CHECK(result.valid && result.records.size()==2);
    CHECK(result.records[0].recordId==saved.recordId);
    CHECK(GuidEq(result.records[0].desktop,saved.desktop));
    CHECK(result.records[0].lastSeenUtc==now &&
          result.records[0].missingSinceUtc==0);
    CHECK(result.records[1].recordId==sibling.provisionalRecordId);
    CHECK(GuidEq(result.records[1].desktop,sibling.observed.desktop));
}

static void test_final_snapshot_zero_live_marks_and_prunes_from_last_seen(){
    const UnixSeconds now=1700003000;
    LayoutWin recent=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009221}","firefox","recent","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-100);
    LayoutWin expired=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009222}","firefox","expired","b.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),
        now-WINDOW_RETENTION_SECONDS);
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    FinalSnapshotResult result=CommitFinalSnapshotRecords(
        {recent,expired},{app},now);
    CHECK(result.valid && result.records.size()==1);
    CHECK(result.records[0].recordId==recent.recordId);
    CHECK(result.records[0].missingSinceUtc==recent.lastSeenUtc);
    CHECK(result.erasedRecordIds.count(expired.recordId)==1);
}

static void test_final_snapshot_incomplete_profile_is_byte_preserved(){
    const UnixSeconds now=1700004000;
    LayoutWin old=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009231}","firefox","old","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),
        now-WINDOW_RETENTION_SECONDS-1);
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Incomplete;
    FinalSnapshotResult result=CommitFinalSnapshotRecords({old},{app},now);
    CHECK(result.valid && result.records.size()==1);
    CHECK(SameLayoutWinFields(result.records[0],old));
    CHECK(result.changedRecordIds.empty() && result.erasedRecordIds.empty());
}

static void test_final_snapshot_failed_desktop_lookup_preserves_saved_guid(){
    const UnixSeconds now=1700005000;
    LayoutWin saved=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009241}","firefox","bound","a.test",4,
        G(L"{231A0000-0000-0000-0000-000000000004}"),now-20);
    FinalWindowObservation observed=FinalObserved("firefox","bound",GUID{},"");
    observed.boundRecordId=saved.recordId;
    observed.desktopValid=false;
    observed.fingerprintFresh=true;
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows.push_back(observed);
    FinalSnapshotResult result=CommitFinalSnapshotRecords({saved},{app},now);
    CHECK(result.valid && result.records.size()==1);
    CHECK(GuidEq(result.records[0].desktop,saved.desktop));
    CHECK(!GuidIsZero(result.records[0].desktop));
    CHECK(result.records[0].lastSeenUtc==now);
}

static void test_final_snapshot_stale_pending_uses_unique_title_and_preserves_destination(){
    const UnixSeconds now=1700005500;
    LayoutWin saved=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009246}","firefox","stable","a.test",4,
        G(L"{231A0000-0000-0000-0000-000000000004}"),now-20);
    FinalWindowObservation observed=FinalObserved(
        "firefox","stable",
        G(L"{231A0000-0000-0000-0000-000000000009}"),
        "{00000000-0000-0000-0000-000000009248}");
    observed.pendingRecordId=
        "{00000000-0000-0000-0000-000000009247}";
    observed.fingerprintFresh=true;
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows.push_back(observed);

    FinalSnapshotResult result=CommitFinalSnapshotRecords({saved},{app},now);

    CHECK(result.valid && result.records.size()==1);
    CHECK(result.records[0].recordId==saved.recordId);
    CHECK(GuidEq(result.records[0].desktop,saved.desktop));
    CHECK(result.records[0].deskIndex==saved.deskIndex);
    CHECK(result.records[0].lastSeenUtc==now);
    CHECK(result.records[0].missingSinceUtc==0);
}

static void test_final_snapshot_reservations_preserve_bound_and_provisional_origin(){
    const UnixSeconds now=1700006000;
    LayoutWin bound=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009251}","firefox","bound","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-10);
    FinalWindowObservation boundMove=FinalObserved(
        "firefox","bound",
        G(L"{231A0000-0000-0000-0000-000000000009}"),"");
    boundMove.boundRecordId=bound.recordId;
    boundMove.reserved=true;

    FinalWindowObservation unboundMove=FinalObserved(
        "firefox","new",
        G(L"{231A0000-0000-0000-0000-000000000009}"),
        "{00000000-0000-0000-0000-000000009252}");
    unboundMove.reserved=true;
    unboundMove.hasProvisionalOriginRecord=true;
    unboundMove.provisionalOriginRecord=unboundMove.observed;
    unboundMove.provisionalOriginRecord.recordId=unboundMove.provisionalRecordId;
    unboundMove.provisionalOriginRecord.desktop=
        G(L"{231A0000-0000-0000-0000-000000000002}");
    unboundMove.provisionalOriginRecord.deskIndex=2;
    unboundMove.provisionalOriginRecord.lastSeenUtc=now;
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows={boundMove,unboundMove};
    FinalSnapshotResult result=CommitFinalSnapshotRecords({bound},{app},now);
    CHECK(result.valid && result.records.size()==2);
    CHECK(SameLayoutWinFields(result.records[0],bound));
    CHECK(result.records[1].recordId==unboundMove.provisionalRecordId);
    CHECK(result.records[1].provisional);
    CHECK(GuidEq(result.records[1].desktop,
                 unboundMove.provisionalOriginRecord.desktop));
    FinalSnapshotResult repeated=CommitFinalSnapshotRecords(
        result.records,{app},now+1);
    CHECK(repeated.valid && repeated.records.size()==2);
}

static void test_initial_partial_enumeration_suppresses_lifecycle_missing_and_write(){
    const UnixSeconds now=1700006500;
    LayoutWin saved=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009261}","firefox","saved","a.test",1,
        G(L"{231A0000-0000-0000-0000-000000000001}"),now-50);
    AppFastSnapshot partial;
    partial.windows.push_back(SnapshotWindow(
        9261,9262,9263,L"",saved.desktop)); // empty title itself is valid
    partial.enumerationComplete=false;       // EnumWindows failed after it
    partial.desktopLookupsComplete=true;
    SnapshotVersionTracker tracker;
    FinalizeFastSnapshot("firefox",1,tracker,partial);

    LcState lifecycle;
    std::vector<LayoutWin> records={saved};
    int lifecycleCalls=0,missingCalls=0,writeCalls=0;
    auto monitorTick=[&](){
        if(!FastSnapshotCanObserve(partial)) return;
        ++lifecycleCalls;
        LcDecision decision=LcObserve(lifecycle,!partial.windows.empty(),
            partial.windowSetSignature,partial.settleSignature,
            partial.layoutSignature,0,0);
        if(decision.action==LcAction::MarkMissingFromLastSeen) ++missingCalls;
        if(decision.action==LcAction::SaveLayout) ++writeCalls;
    };
    monitorTick();
    CHECK(lifecycleCalls==0 && missingCalls==0 && writeCalls==0);
    CHECK(records.size()==1 && SameLayoutWinFields(records[0],saved));

    FinalAppObservation incomplete;
    incomplete.app="firefox";
    incomplete.quality=FinalProfileQuality::Incomplete;
    FinalSnapshotResult final=CommitFinalSnapshotRecords(records,{incomplete},now);
    CHECK(final.valid && final.records.size()==1 &&
          SameLayoutWinFields(final.records[0],saved));
    CHECK(final.changedRecordIds.empty() && final.erasedRecordIds.empty());
}

static void test_bound_a_save_then_unbound_b_reconcile_never_moves_a(){
    const UnixSeconds now=1700006600;
    const GUID originA=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID movedA=G(L"{231A0000-0000-0000-0000-000000000002}");
    const GUID destinationB=G(L"{231A0000-0000-0000-0000-000000000003}");
    LayoutWin savedA=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009271}","firefox","A","a.test",0,
        originA,now-100);
    LayoutWin savedB=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009272}","firefox","B","b.test",2,
        destinationB,now-100);
    BoundSaveObservation observedA;
    observedA.window=SnapshotWindow(9271,9272,9273,L"A",movedA);
    observedA.hasBinding=true;
    observedA.expectedIdentity=IdentityOf(observedA.window);
    observedA.recordId=savedA.recordId;
    observedA.deskIndex=1;
    observedA.causalGeneration=9274;
    BoundSaveObservation unboundB;
    unboundB.window=SnapshotWindow(9275,9276,9277,L"B",originA);
    unboundB.deskIndex=0;

    SaveObservedAppResult saved=ApplyObservedBoundRecords(
        {savedA,savedB},"firefox",{observedA,unboundB},true,now);
    CHECK(saved.valid && saved.needsReconcile && saved.updates.size()==1);
    CHECK(GuidEq(saved.records[0].desktop,movedA));

    LayoutWin liveB=savedB;
    liveB.desktop=originA;
    liveB.deskIndex=0;
    liveB.lastSeenUtc=now;
    ReconcilePlan plan=PlanAppReconcile(
        saved.records,{liveB},"firefox",now,{savedA.recordId},
        ReconcileFreshness::Fresh);
    CHECK(!plan.deferred && plan.restores.size()==1);
    CHECK(plan.restores[0].savedIndex==1 && plan.restores[0].liveIndex==0);
    for(const RestoreRequest& restore : plan.restores)
        CHECK(restore.savedIndex!=0);
    CHECK(plan.newRecords.empty());
}

static void test_query_end_destroy_full_final_snapshot_chains(){
    const UnixSeconds now=1700006700;
    const GUID first=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID second=G(L"{231A0000-0000-0000-0000-000000000002}");
    const std::vector<DeskRec> desks={{0,first,L"one"},{1,second,L"two"}};

    auto run=[&](std::vector<LayoutWin> initial,
                 const std::vector<FinalAppObservation>& observations,
                 const std::vector<LayoutWin>& expected){
        CheckpointController controller;
        std::vector<LayoutWin> current=initial;
        std::string bytes;
        int writes=0;
        auto checkpoint=[&](CheckpointReason){
            FinalSnapshotResult committed=CommitFinalSnapshotRecords(
                current,observations,now);
            CHECK(committed.valid);
            if(!committed.valid) return false;
            current=committed.records;
            bytes=SerializeLayout(desks,current);
            ++writes;
            return true;
        };
        CHECK(controller.dispatch(
            CheckpointReason::QueryEndSession,true,true,false,checkpoint));
        CHECK(!controller.finalization.finished && writes==1);
        CHECK(controller.dispatch(
            CheckpointReason::Finalize,true,true,false,checkpoint));
        CHECK(controller.finalization.finished && writes==2);
        const std::string afterEnd=bytes;
        CHECK(controller.dispatch(
            CheckpointReason::Finalize,true,true,false,checkpoint));
        CHECK(writes==2 && bytes==afterEnd);
        CHECK(bytes==SerializeLayout(desks,expected));
    };

    FinalWindowObservation opened=FinalObserved(
        "firefox","opened",second,
        "{00000000-0000-0000-0000-000000009281}");
    FinalAppObservation openedApp;
    openedApp.app="firefox";
    openedApp.quality=FinalProfileQuality::Complete;
    openedApp.windows={opened};
    LayoutWin expectedOpened=opened.observed;
    expectedOpened.recordId=opened.provisionalRecordId;
    expectedOpened.lastSeenUtc=now;
    expectedOpened.missingSinceUtc=0;
    expectedOpened.provisional=true;
    run({}, {openedApp}, {expectedOpened});

    LayoutWin moved=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009282}","firefox","moved","a.test",0,
        first,now-100);
    FinalWindowObservation movedObservation=FinalObserved(
        "firefox","moved",second,"");
    movedObservation.boundRecordId=moved.recordId;
    movedObservation.fingerprintFresh=false;
    FinalAppObservation movedApp;
    movedApp.app="firefox";
    movedApp.quality=FinalProfileQuality::Complete;
    movedApp.windows={movedObservation};
    LayoutWin expectedMoved=moved;
    expectedMoved.desktop=second;
    expectedMoved.deskIndex=1;
    expectedMoved.lastSeenUtc=now;
    expectedMoved.missingSinceUtc=0;
    run({moved},{movedApp},{expectedMoved});

    LayoutWin absent=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009283}","firefox","absent","b.test",0,
        first,now-100);
    FinalAppObservation emptyApp;
    emptyApp.app="firefox";
    emptyApp.quality=FinalProfileQuality::Complete;
    LayoutWin expectedAbsent=absent;
    expectedAbsent.missingSinceUtc=absent.lastSeenUtc;
    run({absent},{emptyApp},{expectedAbsent});

    CheckpointController retry;
    int finalizeCalls=0;
    CHECK(retry.dispatch(CheckpointReason::QueryEndSession,true,true,false,
        [](CheckpointReason){ return true; }));
    CHECK(!retry.dispatch(CheckpointReason::Finalize,true,true,false,
        [&](CheckpointReason){ ++finalizeCalls; return false; }));
    CHECK(!retry.finalization.finished);
    CHECK(retry.dispatch(CheckpointReason::Finalize,true,true,false,
        [&](CheckpointReason){ ++finalizeCalls; return true; }));
    CHECK(finalizeCalls==2 && retry.finalization.finished);
}

static void test_issued_move_heartbeat_then_session_end_preserves_origin_and_sibling(){
    const UnixSeconds now=1700006800;
    const GUID origin=G(L"{231A0000-0000-0000-0000-000000000001}");
    const GUID target=G(L"{231A0000-0000-0000-0000-000000000002}");
    LayoutWin sibling=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009291}","firefox","sibling","s.test",0,
        origin,now-100);
    LayoutWin provisional=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009292}","firefox","new","",0,
        origin,now);
    provisional.provisional=true;

    MoveJob issued=MJ(MoveOwner::Picker,9291,9292,"issued-runtime");
    issued.recordId=provisional.recordId;
    issued.destination=target;
    MoveQueue queue;
    CHECK(queue.enqueue(issued));
    CHECK(!queue.onIssued(MoveAttemptOutcome::Accepted).completed);
    CHECK(queue.nextAction()==MoveAction::Verify);
    MoveReservation reservation;
    reservation.token=issued.token;
    reservation.identity={0x9293,9294,9295};
    reservation.originDesktop=origin;
    reservation.provisionalOriginRecord=provisional;
    reservation.hasProvisionalOriginRecord=true;
    MoveReservationBook reservations;
    CHECK(reservations.reserve(reservation)==MoveReservationUpdate::Inserted);

    FinalWindowObservation siblingObserved=FinalObserved(
        "firefox","sibling",target,"");
    siblingObserved.boundRecordId=sibling.recordId;
    FinalWindowObservation moving=FinalObserved(
        "firefox","new",target,provisional.recordId);
    moving.reserved=true;
    moving.hasProvisionalOriginRecord=true;
    moving.provisionalOriginRecord=provisional;
    FinalAppObservation app;
    app.app="firefox";
    app.quality=FinalProfileQuality::Complete;
    app.windows={siblingObserved,moving};

    CheckpointController checkpoint;
    int checkpointCalls=0;
    std::vector<LayoutWin> persisted;
    auto save=[&](CheckpointReason){
        MoveReservation visible;
        CHECK(reservations.lookup(reservation.identity,visible));
        CHECK(queue.nextAction()==MoveAction::Verify);
        FinalSnapshotResult result=CommitFinalSnapshotRecords({sibling},{app},now);
        CHECK(result.valid);
        persisted=result.records;
        ++checkpointCalls;
        return result.valid;
    };
    CHECK(checkpoint.dispatch(
        CheckpointReason::Heartbeat,true,true,true,save));
    CHECK(checkpoint.heartbeatDeferred && checkpointCalls==0);
    CHECK(checkpoint.dispatch(
        CheckpointReason::QueryEndSession,true,true,true,save));
    CHECK(checkpointCalls==1 && persisted.size()==2);
    CHECK(GuidEq(persisted[0].desktop,target));
    CHECK(persisted[1].recordId==provisional.recordId &&
          persisted[1].provisional && GuidEq(persisted[1].desktop,origin));
    CHECK(queue.nextAction()==MoveAction::Verify && reservations.size()==1);
}

static void test_manual_restore_keeps_fixture_bytes_and_reports_once(){
    LayoutTempDir temp;
    const std::wstring path=temp.file(L"manual-restore.vde");
    const std::string prior="manual-layout-byte-for-byte-sentinel";
    CHECK(WriteRawFile(path,prior));

    MoveOperationDispatcher dispatcher;
    const MoveToken first{MoveOwner::ManualTray,9391,9392,0};
    const MoveToken second{MoveOwner::ManualTray,9391,9393,1};
    const MoveToken third{MoveOwner::ManualTray,9391,9394,2};
    CHECK(dispatcher.begin(MoveOwner::ManualTray,9391,{first,second,third}));
    MoveOperationSummary summary;
    int reports=0;
    auto deliver=[&](const MoveToken& token,MoveTerminal terminal){
        MoveDispatchDisposition disposition=dispatcher.dispatch(
            TerminalMoveResult(token,terminal),summary);
        if(disposition==MoveDispatchDisposition::OperationCompleted) ++reports;
        return disposition;
    };
    CHECK(deliver(first,MoveTerminal::Succeeded)==MoveDispatchDisposition::Accepted);
    CHECK(deliver(second,MoveTerminal::PermanentFailure)==MoveDispatchDisposition::Accepted);
    CHECK(deliver(third,MoveTerminal::Exhausted)==
          MoveDispatchDisposition::OperationCompleted);
    CHECK(reports==1 && summary.succeeded==1 &&
          summary.permanentFailures==1 && summary.exhausted==1);
    CHECK(deliver(third,MoveTerminal::Exhausted)==MoveDispatchDisposition::Stale);
    CHECK(reports==1 && ReadRawFile(path)==prior);
}

static void test_auto_load_retry_uses_capped_backoff_and_initializes_once(){
    AutoLoadRetryState state;
    CHECK(state.due(0));
    state.failed(0);
    CHECK(state.nextAttemptMs==1000 && !state.due(999) && state.due(1000));
    state.failed(1000);
    CHECK(state.nextAttemptMs==3000 && !state.due(2999) && state.due(3000));
    for(unsigned attempt=0;attempt<8;++attempt)
        state.failed(state.nextAttemptMs);
    CHECK(state.nextAttemptMs-state.lastAttemptMs==60000);
}

static void test_corrective_initial_observation_is_transactional_before_async_work(){
    std::vector<AppProfile> profiles;
    profiles.push_back(sessionTestProfile("firefox"));
    profiles.push_back(sessionTestProfile("chrome",AppProfile::CHROMIUM));
    SnapshotVersionTracker tracker;
    std::map<std::string,AppFastSnapshot> snapshots;
    AppFastSnapshot firefox;
    firefox.windows.push_back(SnapshotWindow(
        11,101,1001,L"A",G(L"{231A0000-0000-0000-0000-000000000001}")));
    FinalizeFastSnapshot("firefox",1,tracker,firefox);
    snapshots["firefox"]=firefox;
    AppFastSnapshot chrome;
    chrome.windows.push_back(SnapshotWindow(
        22,202,2002,L"B",G(L"{231A0000-0000-0000-0000-000000000002}")));
    FinalizeFastSnapshot("chrome",2,tracker,chrome);
    snapshots["chrome"]=chrome;

    std::map<std::string,LcState> lifecycle;
    lifecycle["prior"].initialized=true;
    std::map<std::string,uint64_t> signatures;
    signatures["prior"]=77;
    std::vector<std::string> stagedApps;
    int preparedApps=0;
    const bool first=PrepareInitialLifecycleStates(
        profiles,snapshots,500,lifecycle,signatures,
        [&](const std::string& app,LcState& state,
            const AppFastSnapshot& snapshot,uint64_t nowMs){
            ++preparedApps;
            const LcDecision decision=LcObserve(
                state,true,snapshot.windowSetSignature,
                snapshot.settleSignature,snapshot.layoutSignature,0,nowMs);
            CHECK(decision.action==LcAction::None);
            stagedApps.push_back(app);
            if(app=="chrome") throw 7;
        });
    CHECK(!first && preparedApps==2);
    CHECK(lifecycle.size()==1 && lifecycle.count("prior")==1 &&
          signatures.size()==1 && signatures["prior"]==77);
    // The first attempt staged A locally, but no owner/route/wave was
    // published, so a hypothetical late generation cannot match anything.
    CHECK((stagedApps==std::vector<std::string>{"firefox","chrome"}));
    CHECK(lifecycle.count("firefox")==0);

    stagedApps.clear();
    CHECK(PrepareInitialLifecycleStates(
        profiles,snapshots,600,lifecycle,signatures,
        [&](const std::string& app,LcState& state,
            const AppFastSnapshot& snapshot,uint64_t nowMs){
            const LcDecision decision=LcObserve(
                state,true,snapshot.windowSetSignature,
                snapshot.settleSignature,snapshot.layoutSignature,0,nowMs);
            CHECK(decision.action==LcAction::None);
            stagedApps.push_back(app);
        }));
    CHECK(lifecycle.size()==2 && signatures.size()==2 &&
          lifecycle.count("firefox")==1 && lifecycle.count("chrome")==1 &&
          lifecycle.count("prior")==0);

    std::map<std::string,uint64_t> routes;
    std::vector<std::string> owners;
    LcState& state=lifecycle["firefox"];
    const AppFastSnapshot& current=snapshots["firefox"];
    const LcDecision wave=LcObserve(
        state,true,current.windowSetSignature,current.settleSignature,
        current.layoutSignature,0,601);
    if(wave.action==LcAction::BeginRestore){
        routes["firefox"]=wave.generation;
        owners.push_back("firefox");
    }
    const LcDecision duplicate=LcObserve(
        state,true,current.windowSetSignature,current.settleSignature,
        current.layoutSignature,0,602);
    CHECK(wave.action==LcAction::BeginRestore && wave.generation!=0);
    CHECK(duplicate.action==LcAction::None && routes.size()==1 &&
          routes["firefox"]==wave.generation && owners.size()==1);
}

static void test_corrective_monitor_arm_failure_backs_off_before_loading(){
    AutoLoadRetryState state;
    int monitorAttempts=0,loadAttempts=0,initializations=0;
    int heartbeatAttempts=0,alternatePosts=0;
    bool monitorAvailable=false;
    const auto attempt=[&](uint64_t nowMs){
        return AdvanceAutoRuntimeStart(state,nowMs,
            [&](){ ++monitorAttempts; return monitorAvailable; },
            [&](){ ++loadAttempts; return true; },
            [&](){ ++initializations; return true; },
            [&](){ ++heartbeatAttempts; return true; },
            [&](){ ++alternatePosts; return true; });
    };

    CHECK(attempt(100)==AutoRuntimeStartResult::MonitorUnavailable);
    CHECK(!state.loaded && !state.monitorStarted &&
          !state.layoutPrepared && !state.heartbeatStarted);
    CHECK(state.nextAttemptMs==1100 && alternatePosts==1);
    CHECK(loadAttempts==0 && initializations==0 && heartbeatAttempts==0);

    monitorAvailable=true;
    CHECK(attempt(100)==AutoRuntimeStartResult::WaitingForRetry);
    CHECK(state.monitorStarted && !state.loaded && loadAttempts==0);
    CHECK(attempt(1099)==AutoRuntimeStartResult::WaitingForRetry);
    CHECK(loadAttempts==0);
    CHECK(attempt(1100)==AutoRuntimeStartResult::Ready);
    CHECK(state.loaded && state.heartbeatStarted &&
          loadAttempts==1 && initializations==1 && heartbeatAttempts==1);
}

static void test_corrective_monitor_alternate_rearm_is_bounded(){
    AutoLoadRetryState state;
    int monitorAttempts=0,alternatePosts=0;
    const auto attempt=[&](){
        return AdvanceAutoRuntimeStart(state,0,
            [&](){ ++monitorAttempts; return false; },
            [](){ return true; },[](){ return true; },
            [](){ return true; },
            [&](){ ++alternatePosts; return true; });
    };
    for(unsigned index=0;
        index<AutoLoadRetryState::kMaxAlternateMonitorRetries+3;++index)
        CHECK(attempt()==AutoRuntimeStartResult::MonitorUnavailable);
    CHECK(alternatePosts==
          static_cast<int>(AutoLoadRetryState::kMaxAlternateMonitorRetries));
    CHECK(monitorAttempts==
          static_cast<int>(AutoLoadRetryState::kMaxAlternateMonitorRetries+3));
    CHECK(!state.loaded && !state.monitorStarted);
}

static void test_corrective_failed_monitor_retry_post_remains_unready(){
    AutoLoadRetryState state;
    int postAttempts=0;
    const AutoRuntimeStartResult result=AdvanceAutoRuntimeStart(
        state,50,[](){ return false; },[](){ return true; },
        [](){ return true; },[](){ return true; },
        [&](){ ++postAttempts; return false; });
    CHECK(result==AutoRuntimeStartResult::MonitorUnavailable);
    CHECK(postAttempts==1 && !state.loaded && !state.monitorStarted &&
          !state.layoutPrepared && !state.initialized &&
          !state.heartbeatStarted);
    CHECK(state.nextAttemptMs==1050 && !state.due(1049));
}

static void test_corrective_unavailable_load_then_heartbeat_retry_initializes_once(){
    AutoLoadRetryState state;
    std::string bytes="prior-valid-bytes";
    int monitorStarts=0,loadAttempts=0,initializations=0;
    int heartbeatAttempts=0,heartbeatStarts=0,alternatePosts=0;
    const auto attempt=[&](uint64_t nowMs){
        return AdvanceAutoRuntimeStart(state,nowMs,
            [&](){ ++monitorStarts; return true; },
            [&](){
                ++loadAttempts;
                if(loadAttempts<3) return false;
                bytes="new-valid-bytes";
                return true;
            },
            [&](){ ++initializations; return true; },
            [&](){
                ++heartbeatAttempts;
                if(heartbeatAttempts==1) return false;
                ++heartbeatStarts;
                return true;
            },
            [&](){ ++alternatePosts; return true; });
    };

    CHECK(attempt(0)==AutoRuntimeStartResult::LoadUnavailable);
    CHECK(bytes=="prior-valid-bytes" && loadAttempts==1);
    CHECK(attempt(999)==AutoRuntimeStartResult::WaitingForRetry);
    CHECK(loadAttempts==1 && bytes=="prior-valid-bytes");
    CHECK(attempt(1000)==AutoRuntimeStartResult::LoadUnavailable);
    CHECK(loadAttempts==2 && bytes=="prior-valid-bytes");
    CHECK(attempt(3000)==AutoRuntimeStartResult::HeartbeatUnavailable);
    CHECK(bytes=="new-valid-bytes" && state.layoutPrepared &&
          !state.initialized && !state.loaded && !state.heartbeatStarted);
    CHECK(initializations==0 && heartbeatAttempts==1);
    CHECK(attempt(3999)==AutoRuntimeStartResult::WaitingForRetry);
    CHECK(loadAttempts==3 && initializations==0 && heartbeatAttempts==1);
    CHECK(attempt(4000)==AutoRuntimeStartResult::Ready);
    CHECK(state.loaded && state.heartbeatStarted &&
          monitorStarts==1 && loadAttempts==3 && initializations==1 &&
          heartbeatAttempts==2 && heartbeatStarts==1 && alternatePosts==0);
}

static void test_checkpoint_controller_heartbeat_and_session_end_chain(){
    CheckpointController controller;
    int calls=0;
    std::vector<CheckpointReason> reasons;
    auto success=[&](CheckpointReason reason){
        ++calls;
        reasons.push_back(reason);
        return true;
    };
    CHECK(controller.dispatch(CheckpointReason::Heartbeat,false,false,false,success));
    CHECK(calls==0);
    CHECK(controller.dispatch(CheckpointReason::Heartbeat,true,true,false,success));
    CHECK(calls==1 && reasons.back()==CheckpointReason::Heartbeat);
    CHECK(controller.dispatch(CheckpointReason::QueryEndSession,true,true,false,success));
    CHECK(calls==2 && !controller.finalization.finished);
    CHECK(controller.dispatch(CheckpointReason::Finalize,true,true,false,success));
    CHECK(calls==3 && controller.finalization.finished);
    CHECK(controller.dispatch(CheckpointReason::Finalize,true,true,false,success));
    CHECK(calls==3);
}

static void test_checkpoint_failed_end_is_retryable_at_destroy(){
    CheckpointController controller;
    int calls=0;
    auto failThenPass=[&](CheckpointReason){ return ++calls>1; };
    CHECK(!controller.dispatch(
        CheckpointReason::Finalize,true,true,false,failThenPass));
    CHECK(!controller.finalization.running && !controller.finalization.finished);
    CHECK(controller.dispatch(
        CheckpointReason::Finalize,true,true,false,failThenPass));
    CHECK(calls==2 && controller.finalization.finished);
}

static void test_settings_checkpoint_rejects_enabled_unloaded_and_preserves_state(){
    CheckpointController controller;
    bool recoveryPending=true;
    bool settingEnabled=true;
    int checkpointCalls=0;
    const bool accepted=controller.dispatch(
        CheckpointReason::SettingsChange,true,false,false,
        [&](CheckpointReason){
            ++checkpointCalls;
            recoveryPending=false;
            return true;
        });
    if(accepted) settingEnabled=false;
    CHECK(!accepted);
    CHECK(checkpointCalls==0);
    CHECK(recoveryPending && settingEnabled);
}

static void test_settings_transaction_rolls_back_and_cancels_only_auto_owner(){
    SettingsRuntimeSnapshot current;
    current.hotkeyVk='D';
    current.hotkeyMods=MOD_CONTROL|MOD_ALT;
    current.autoFix=true;
    current.runAtLogon=false;
    current.firefox=true;
    current.chrome=true;
    current.edge=false;
    SettingsRuntimeSnapshot requested=current;
    requested.hotkeyVk='K';
    requested.hotkeyMods=MOD_SHIFT;
    requested.autoFix=false;
    requested.runAtLogon=true;
    requested.chrome=false;

    bool dialogOpen=true,autoOperation=true,manualOperation=true;
    bool pickerOperation=true,moveTimerArmed=true;
    int checkpoints=0,cancellations=0;
    CHECK(!ApplySettingsRuntimeTransaction(current,requested,
        [&](){ ++checkpoints; return false; },
        [&](){ ++cancellations; autoOperation=false; return true; }));
    CHECK(checkpoints==1 && cancellations==0 && dialogOpen);
    CHECK(current.hotkeyVk=='D' && current.hotkeyMods==(MOD_CONTROL|MOD_ALT) &&
          current.autoFix && !current.runAtLogon && current.firefox &&
          current.chrome && !current.edge);
    CHECK(autoOperation && manualOperation && pickerOperation && moveTimerArmed);

    CHECK(ApplySettingsRuntimeTransaction(current,requested,
        [&](){ ++checkpoints; return true; },
        [&](){
            ++cancellations;
            autoOperation=false;
            return true;
        }));
    CHECK(checkpoints==2 && cancellations==1 && !autoOperation);
    CHECK(manualOperation && pickerOperation && moveTimerArmed);
    CHECK(current.hotkeyVk=='K' && current.hotkeyMods==MOD_SHIFT &&
          !current.autoFix && current.runAtLogon && current.firefox &&
          !current.chrome && !current.edge);

    SettingsRuntimeSnapshot enabled=current;
    enabled.autoFix=true;
    enabled.chrome=true;
    int loadStarts=0;
    CHECK(ApplySettingsRuntimeTransaction(current,enabled,
        [&](){ ++checkpoints; return true; },
        [&](){ ++cancellations; return true; }));
    if(current.autoFix) ++loadStarts;
    CHECK(current.autoFix && current.chrome && loadStarts==1);
    CHECK(checkpoints==2); // enabling a disabled snapshot needs no save

    SettingsRuntimeSnapshot disabled=current;
    disabled.autoFix=false;
    CHECK(ApplySettingsRuntimeTransaction(current,disabled,
        [&](){ ++checkpoints; return true; },
        [&](){ ++cancellations; return true; }));
    CHECK(!current.autoFix && checkpoints==3); // disable only after checkpoint
    CHECK(manualOperation && pickerOperation && moveTimerArmed);
}

static void test_checkpoint_reservation_defers_one_heartbeat_but_not_final(){
    CheckpointController controller;
    int heartbeatCalls=0,finalCalls=0;
    auto count=[&](CheckpointReason reason){
        if(reason==CheckpointReason::Heartbeat) ++heartbeatCalls;
        if(reason==CheckpointReason::Finalize) ++finalCalls;
        return true;
    };
    CHECK(controller.dispatch(CheckpointReason::Heartbeat,true,true,true,count));
    CHECK(controller.dispatch(CheckpointReason::Heartbeat,true,true,true,count));
    CHECK(heartbeatCalls==0 && controller.heartbeatDeferred);
    CHECK(controller.dispatch(CheckpointReason::Finalize,true,true,true,count));
    CHECK(finalCalls==1 && controller.finalization.finished);

    CheckpointController running;
    CHECK(running.dispatch(CheckpointReason::Heartbeat,true,true,true,count));
    CHECK(running.runDeferredHeartbeat(true,true,false,count));
    CHECK(heartbeatCalls==1 && !running.heartbeatDeferred);
    CHECK(running.runDeferredHeartbeat(true,true,false,count));
    CHECK(heartbeatCalls==1);

    CheckpointController terminal;
    CHECK(terminal.dispatch(CheckpointReason::Heartbeat,true,true,true,count));
    CHECK(terminal.reservationTerminated(false,false,true,true,count));
    CHECK(heartbeatCalls==1 && terminal.heartbeatDeferred);
    CHECK(terminal.reservationTerminated(true,true,true,true,count));
    CHECK(heartbeatCalls==1 && terminal.heartbeatDeferred);
    CHECK(terminal.reservationTerminated(true,false,true,true,count));
    CHECK(heartbeatCalls==2 && !terminal.heartbeatDeferred);
    CHECK(terminal.reservationTerminated(false,false,true,true,count));
    CHECK(heartbeatCalls==2);
}

static void test_tray_instance_scope_is_gui_only_and_covers_work_lifetime(){
    std::vector<std::string> events;
    auto acquire=[&](){
        events.push_back("acquire");
        return TrayInstanceAcquireStatus::Acquired;
    };
    auto body=[&](){ events.push_back("body"); return 17; };
    auto release=[&](){ events.push_back("release"); };

    CHECK(RunWithTrayInstanceScope(false,acquire,body,release)==17);
    CHECK((events==std::vector<std::string>{"acquire","body","release"}));

    events.clear();
    CHECK(RunWithTrayInstanceScope(true,acquire,body,release)==17);
    CHECK((events==std::vector<std::string>{"body"}));

    events.clear();
    auto already=[&](){
        events.push_back("already");
        return TrayInstanceAcquireStatus::AlreadyRunning;
    };
    CHECK(RunWithTrayInstanceScope(false,already,body,release)==0);
    CHECK((events==std::vector<std::string>{"already"}));
}

static void test_browser_classifier_requires_enabled_class_and_exact_executable_basename(){
    std::vector<AppProfile> enabled=BuiltinProfiles(true,true,true);
    const AppProfile* chrome=ClassifyBrowserCandidate(
        L"Chrome_WidgetWin_1",L"C:\\Program Files\\Google\\Chrome\\chrome.exe",
        enabled);
    CHECK(chrome && chrome->id=="chrome");
    CHECK(ClassifyBrowserCandidate(
        L"MozillaWindowClass",L"C:\\Program Files\\Google\\Chrome\\chrome.exe",
        enabled)==nullptr);
    CHECK(ClassifyBrowserCandidate(
        L"Chrome_WidgetWin_1",L"C:\\Temp\\notchrome.exe",enabled)==nullptr);
    CHECK(ClassifyBrowserCandidate(
        L"Chrome_WidgetWin_1",L"C:\\Temp\\chrome.exe.backup",enabled)==nullptr);

    std::vector<AppProfile> firefoxOnly=BuiltinProfiles(true,false,false);
    CHECK(ClassifyBrowserCandidate(
        L"Chrome_WidgetWin_1",L"C:\\Program Files\\Google\\Chrome\\chrome.exe",
        firefoxOnly)==nullptr);
    const AppProfile* firefox=ClassifyBrowserCandidate(
        L"MozillaWindowClass",L"C:\\Program Files\\Mozilla Firefox\\FIREFOX.EXE",
        firefoxOnly);
    CHECK(firefox && firefox->id=="firefox");
}

static void test_class_lookup_failure_marks_enabled_profiles_incomplete_but_empty_title_is_valid(){
    const std::vector<AppProfile> profiles=BuiltinProfiles(true,true,false);
    std::map<std::string,AppFastSnapshot> snapshots;
    snapshots["firefox"];
    snapshots["chrome"];
    FastWin emptyTitle=SnapshotWindow(
        931,9301,93001,L"",
        G(L"{231A0000-0000-0000-0000-000000000001}"));
    emptyTitle.app="firefox";
    snapshots["firefox"].windows.push_back(emptyTitle);

    CHECK(AcceptFastClassNameRead(18,profiles,snapshots));
    CHECK(snapshots["firefox"].enumerationComplete);
    CHECK(snapshots["chrome"].enumerationComplete);
    CHECK(snapshots["firefox"].windows.size()==1 &&
          snapshots["firefox"].windows[0].title.empty());

    CHECK(!AcceptFastClassNameRead(0,profiles,snapshots));
    CHECK(!snapshots["firefox"].enumerationComplete);
    CHECK(!snapshots["chrome"].enumerationComplete);
    CHECK(snapshots["firefox"].windows.size()==1 &&
          snapshots["firefox"].windows[0].title.empty());
}

static void test_popup_persistence_recaptures_before_classification_and_reports_storage(){
    const WindowIdentityKey captured{0x941,9401,94001};
    int recaptures=0,classifications=0,readinessChecks=0,writes=0;
    auto lostRecapture=[&](const WindowIdentityKey& expected){
        ++recaptures;
        CHECK(SameIdentity(expected,captured));
        return WindowIdentityRecapture::Lost; // same HWND/PID, reused process start
    };
    auto classify=[&](const WindowIdentityKey&){
        ++classifications;
        return PopupBrowserClassification::Tracked;
    };
    auto readiness=[&](){
        ++readinessChecks;
        return PopupPersistenceReadiness::Ready;
    };
    auto persist=[&](){ ++writes; return true; };

    CHECK(CompletePopupMovePersistence(
              captured,lostRecapture,classify,readiness,persist)==
          PopupPersistenceResult::IdentityLost);
    CHECK(recaptures==1 && classifications==0 && readinessChecks==0 && writes==0);

    auto matchingRecapture=[&](const WindowIdentityKey&){
        ++recaptures;
        return WindowIdentityRecapture::Match;
    };
    CHECK(CompletePopupMovePersistence(
              captured,matchingRecapture,
              [&](const WindowIdentityKey&){
                  ++classifications;
                  return PopupBrowserClassification::NotTracked;
              },readiness,persist)==PopupPersistenceResult::NotTracked);
    CHECK(recaptures==2 && classifications==1 && readinessChecks==0 && writes==0);

    CHECK(CompletePopupMovePersistence(
              captured,matchingRecapture,classify,
              [&](){
                  ++readinessChecks;
                  return PopupPersistenceReadiness::Unavailable;
              },persist)==PopupPersistenceResult::StorageUnavailable);
    CHECK(recaptures==3 && classifications==2 && readinessChecks==1 && writes==0);

    CHECK(CompletePopupMovePersistence(
              captured,matchingRecapture,classify,
              [&](){
                  ++readinessChecks;
                  return PopupPersistenceReadiness::ReadOnly;
              },persist)==PopupPersistenceResult::StorageReadOnly);
    CHECK(recaptures==4 && classifications==3 && readinessChecks==2 && writes==0);

    CHECK(CompletePopupMovePersistence(
              captured,matchingRecapture,classify,readiness,persist)==
          PopupPersistenceResult::Saved);
    CHECK(recaptures==5 && classifications==4 && readinessChecks==3 && writes==1);
    CHECK(CompletePopupMovePersistence(
              captured,matchingRecapture,classify,readiness,[&](){ ++writes; return false; })==
          PopupPersistenceResult::SaveFailed);
    CHECK(recaptures==6 && classifications==5 && readinessChecks==4 && writes==2);
}

static void test_popup_saved_only_completes_exact_lifecycle_save_generation(){
    LcState state;
    state.saveInFlight=true;
    state.saveGeneration=91;
    state.saveRequestedLayoutSignature=901;
    int completions=0;
    const auto complete=[&](uint64_t generation){
        return CompletePopupLifecycleAfterPersistence(
            PopupPersistenceResult::Saved,[&]{
                ++completions;
                LcExplicitSaveCompleted(state,generation,902,903,904);
            });
    };
    const PopupPersistenceResult failures[]={
        PopupPersistenceResult::NotTracked,
        PopupPersistenceResult::IdentityLost,
        PopupPersistenceResult::IdentityIndeterminate,
        PopupPersistenceResult::ClassificationFailed,
        PopupPersistenceResult::StorageUnavailable,
        PopupPersistenceResult::StorageReadOnly,
        PopupPersistenceResult::SaveFailed
    };
    for(PopupPersistenceResult result : failures){
        CHECK(!CompletePopupLifecycleAfterPersistence(result,[&]{
            ++completions;
            LcExplicitSaveCompleted(state,91,902,903,904);
        }));
    }
    CHECK(completions==0 && state.saveInFlight && state.saveGeneration==91);
    CHECK(complete(90));
    CHECK(completions==1 && state.saveInFlight && state.saveGeneration==91);
    CHECK(complete(91));
    CHECK(completions==2 && !state.saveInFlight && state.saveGeneration==0);
}

static void test_popup_uses_exact_pending_saved_id_before_new_provisional(){
    const WindowIdentityKey identity{0x942,9402,94002};
    const std::string savedId=
        "{00000000-0000-0000-0000-000000009402}";
    const std::map<std::string,std::string> pending={
        {RuntimeKey(identity),savedId}};
    std::string selected="sentinel";
    int validations=0;
    auto validate=[&](const std::string& candidate,
                      const std::string& app,std::string& canonical){
        ++validations;
        if(candidate!=savedId || app!="firefox") return false;
        canonical=candidate;
        return true;
    };
    CHECK(SelectPendingPopupRecordId(
        identity,"firefox",pending,validate,selected));
    CHECK(selected==savedId && validations==1);

    WindowIdentityKey reused=identity;
    ++reused.processStart;
    selected="unchanged";
    CHECK(!SelectPendingPopupRecordId(
        reused,"firefox",pending,validate,selected));
    CHECK(selected=="unchanged" && validations==1);
    CHECK(!SelectPendingPopupRecordId(
        identity,"chrome",pending,validate,selected));
    CHECK(selected=="unchanged" && validations==2);
}

static void test_popup_pending_id_bypasses_title_and_origin_provisional_gates(){
    const std::string savedId=
        "{00000000-0000-0000-0000-000000009403}";
    for(int failure=0;failure<2;++failure){
        int pendingCalls=0,provisionalCalls=0;
        std::string selected="sentinel";
        const bool titleComplete=failure!=0;
        const bool originDesktopValid=failure!=1;
        const PopupReservationRecordSource source=
            SelectPopupReservationRecord(
                true,true,titleComplete,originDesktopValid,
                [&](std::string& output){
                    ++pendingCalls;
                    output=savedId;
                    return true;
                },
                [&](std::string& output){
                    ++provisionalCalls;
                    output="{00000000-0000-0000-0000-000000009404}";
                    return true;
                },selected);
        CHECK(source==PopupReservationRecordSource::Pending);
        CHECK(selected==savedId && pendingCalls==1 && provisionalCalls==0);
    }

    int provisionalCalls=0;
    std::string selected="unchanged";
    CHECK(SelectPopupReservationRecord(
        true,true,false,true,
        [](std::string&){ return false; },
        [&](std::string&){ ++provisionalCalls; return true; },selected)==
        PopupReservationRecordSource::None);
    CHECK(selected=="unchanged" && provisionalCalls==0);
    CHECK(SelectPopupReservationRecord(
        true,true,true,false,
        [](std::string&){ return false; },
        [&](std::string&){ ++provisionalCalls; return true; },selected)==
        PopupReservationRecordSource::None);
    CHECK(selected=="unchanged" && provisionalCalls==0);
}

static void test_popup_post_classification_reuses_pending_id_after_initial_untracked_capture(){
    const std::string savedId=
        "{00000000-0000-0000-0000-000000009405}";
    const std::string generatedId=
        "{00000000-0000-0000-0000-000000009406}";
    std::string selected="sentinel";
    int pendingCalls=0,generatorCalls=0;

    // Capture-time class lookup failed, so the generic picker reservation has
    // no record ID.  Terminal identity recapture and classification succeeded:
    // the exact runtime's saved pending ID must win before allocation.
    CHECK(SelectPopupPersistRecordId(
        "",
        [&](std::string& output){
            ++pendingCalls;
            output=savedId;
            return true;
        },
        [&](std::string& output){
            ++generatorCalls;
            output=generatedId;
            return true;
        },selected));
    CHECK(selected==savedId && pendingCalls==1 && generatorCalls==0);

    // A reused process-start cannot select the old runtime's pending row; the
    // terminal path instead creates one new ID and never steals savedId.
    selected="sentinel";
    CHECK(SelectPopupPersistRecordId(
        "",
        [&](std::string&){ ++pendingCalls; return false; },
        [&](std::string& output){
            ++generatorCalls;
            output=generatedId;
            return true;
        },selected));
    CHECK(selected==generatedId && pendingCalls==2 && generatorCalls==1);
}

static void test_validated_touch_rebase_preserves_external_semantics(){
    LayoutWin disk=ReconcileTestRecord(
        "{00000000-0000-0000-0000-000000009301}","firefox","external","a.test",8,
        G(L"{231A0000-0000-0000-0000-000000000008}"),100);
    disk.missingSinceUtc=90;
    ValidatedRecordTouch touch;
    touch.recordId=disk.recordId;
    touch.lastSeenUtc=300;
    touch.causalGeneration=7;
    std::map<std::string,uint64_t> current={{disk.recordId,7}};
    TouchRebaseResult result=ReapplyValidatedTouches({disk},{{disk.recordId,touch}},current);
    CHECK(result.deferredRecordIds.empty() && result.records.size()==1);
    CHECK(result.records[0].activeTitle=="external");
    CHECK(result.records[0].deskIndex==8 &&
          GuidEq(result.records[0].desktop,disk.desktop));
    CHECK(result.records[0].lastSeenUtc==300 &&
          result.records[0].missingSinceUtc==0);

    current[disk.recordId]=8;
    TouchRebaseResult stale=ReapplyValidatedTouches({disk},{{disk.recordId,touch}},current);
    CHECK(stale.records.size()==1 && SameLayoutWinFields(stale.records[0],disk));
    CHECK(stale.deferredRecordIds.count(disk.recordId)==1);
}

int main(){
    test_finalization_runs_once();
    test_window_identity_requires_full_nonzero_process_identity();
    test_snapshot_versions_change_only_for_changed_inputs();
    test_snapshot_signatures_are_delimiter_safe();
    test_snapshot_generation_wrap_restarts_without_zero();
    test_save_observed_bound_app_updates_only_exact_bound_identities();
    test_explicit_save_with_unbound_sibling_rearms_reconcile();
    test_fast_snapshot_versions_are_order_independent_and_quality_aware();
    test_resolve_saved_desktop_uses_guid_only();
    test_rebase_merges_different_ids_and_preserves_external_records();
    test_rebase_same_id_newer_validated_upsert_wins();
    test_durable_candidate_delta_is_satisfied_by_external_unrelated_revision();
    test_rebase_tied_or_older_upsert_and_stale_tombstone_defer();
    test_rebase_expiry_delete_requires_latest_independently_expired();
    test_record_delta_chaining_preserves_first_disk_base();
    test_deferred_conflict_survives_repeated_publish_until_newer_causal_upsert();
    test_rebased_publication_captures_adopted_disk_before_any_swap();
    test_durable_publish_commits_revision_without_copy_or_rewrite();
    test_final_checkpoint_mutation_is_transactional_across_fault_matrix();
    test_expire_delete_discards_validated_touch_before_external_rebase();
    test_final_observation_provisional_map_stages_before_global_publish();
    test_reconcile_worker_is_bounded_coalesced_and_nonblocking();
    test_reconcile_live_preparation_is_ordered_and_search_ready();
    test_cli_profile_batch_aborts_transactionally_on_first_prep_failure();
    test_cli_loads_settings_before_selecting_active_profiles();
    test_cli_save_revalidates_snapshot_and_desktops_before_publish();
    test_manual_save_incomplete_snapshot_keeps_prior_bytes_without_write();
    test_cli_status_keeps_fast_rows_when_fingerprints_unavailable();
    test_reconcile_copied_text_budget_covers_every_owned_string();
    test_reconcile_worker_prepares_live_inputs_off_thread_and_coalesces();
    test_reconcile_worker_prepare_only_skips_planner_for_search();
    test_reconcile_worker_planner_failure_is_owned_and_thread_survives();
    test_reconcile_worker_accepted_request_owns_failure_result();
    test_reconcile_post_failure_deadlines_all_operation_owners();
    test_reconcile_worker_rejects_invalid_or_oversized_requests();
    test_reconcile_plan_distinguishes_complexity_deferral();
    test_posted_reconcile_results_are_drained();
    test_reconcile_consumer_ignores_stale_content_generation();
    test_final_snapshot_captures_immediately_opened_new_window();
    test_final_snapshot_marks_unbound_additions_provisional_independent_of_title();
    test_final_snapshot_failed_reappeared_keeps_destination_and_adds_sibling();
    test_final_snapshot_zero_live_marks_and_prunes_from_last_seen();
    test_final_snapshot_incomplete_profile_is_byte_preserved();
    test_final_snapshot_failed_desktop_lookup_preserves_saved_guid();
    test_final_snapshot_stale_pending_uses_unique_title_and_preserves_destination();
    test_final_snapshot_reservations_preserve_bound_and_provisional_origin();
    test_initial_partial_enumeration_suppresses_lifecycle_missing_and_write();
    test_bound_a_save_then_unbound_b_reconcile_never_moves_a();
    test_query_end_destroy_full_final_snapshot_chains();
    test_issued_move_heartbeat_then_session_end_preserves_origin_and_sibling();
    test_manual_restore_keeps_fixture_bytes_and_reports_once();
    test_auto_load_retry_uses_capped_backoff_and_initializes_once();
    test_corrective_initial_observation_is_transactional_before_async_work();
    test_corrective_monitor_arm_failure_backs_off_before_loading();
    test_corrective_monitor_alternate_rearm_is_bounded();
    test_corrective_failed_monitor_retry_post_remains_unready();
    test_corrective_unavailable_load_then_heartbeat_retry_initializes_once();
    test_checkpoint_controller_heartbeat_and_session_end_chain();
    test_checkpoint_failed_end_is_retryable_at_destroy();
    test_settings_checkpoint_rejects_enabled_unloaded_and_preserves_state();
    test_settings_transaction_rolls_back_and_cancels_only_auto_owner();
    test_checkpoint_reservation_defers_one_heartbeat_but_not_final();
    test_tray_instance_scope_is_gui_only_and_covers_work_lifetime();
    test_browser_classifier_requires_enabled_class_and_exact_executable_basename();
    test_class_lookup_failure_marks_enabled_profiles_incomplete_but_empty_title_is_valid();
    test_popup_persistence_recaptures_before_classification_and_reports_storage();
    test_popup_saved_only_completes_exact_lifecycle_save_generation();
    test_popup_uses_exact_pending_saved_id_before_new_provisional();
    test_popup_pending_id_bypasses_title_and_origin_provisional_gates();
    test_popup_post_classification_reuses_pending_id_after_initial_untracked_capture();
    test_validated_touch_rebase_preserves_external_semantics();
    test_etld1();
    test_b64();
    test_b64_long_roundtrip();
    test_strict_integer_parsing();
    test_strict_base64_parsing();
    test_strict_counts_parsing();
    test_move_queue_alternates_issue_verify_and_succeeds();
    test_move_queue_enqueue_validates_identity_state_and_copies_guid();
    test_move_queue_allows_bounded_auto_with_manual_and_picker_jobs();
    test_move_queue_phase_guards_and_issue_outcomes();
    test_move_queue_four_transient_issues_still_receive_four_verifies();
    test_move_queue_invalid_outcomes_fail_closed();
    test_move_queue_four_transient_cycles_exhaust_and_unblock_next();
    test_move_queue_permanent_failure_finishes_and_unblocks_next();
    test_move_queue_cancel_job_is_identity_safe_during_verify();
    test_move_queue_cancel_operation_is_owner_scoped_and_fifo();
    test_move_queue_duplicate_owner_delivery_is_harmless();
    test_move_operation_dispatcher_is_job_and_owner_scoped();
    test_move_operation_dispatcher_cancellation_completes_each_job_once();
    test_move_reservation_replacement_requires_exact_terminal_token();
    test_issued_reservation_transfer_has_no_checkpoint_gap();
    test_successor_handoff_publishes_before_issued_displaced_cancel();
    test_issued_reservation_rollback_waits_for_terminal_ack();
    test_async_session_route_protects_manual_work_and_retires_once();
    test_async_session_route_timeout_and_cancel_are_exact();
    test_async_session_route_bounds_deadlines_and_retires_capacity();
    test_dirty_flush_is_coalesced_bounded_and_retries_without_spin();
    test_move_timer_failure_cancels_accepted_work_once();
    test_move_cancellation_gate_precedes_fallible_cleanup();
    test_move_terminal_state_is_prepared_before_publication();
    test_move_setup_rolls_back_provisional_and_queue_state();
    test_unbound_manual_reservation_uses_provisional_origin_id();
    test_auto_restore_failure_never_completes_as_success();
    test_identity_guard_recaptures_immediately_before_issue_or_verify();
    test_desktop_services_require_documented_manager();
    test_service_initialization_releases_every_failed_partial_state();
    test_reconcile_deadline_retires_dropped_operation_exactly_once();
    test_dirty_flush_preserves_mutation_during_write_and_limits_errors();
    test_dirty_flush_clock_ceiling_never_spins();
    test_snss_parse();
    test_snss_garbage();
    test_snss_truncated_frame_returns_no_partial_windows();
    test_mozlz4_rejects_huge_declared_output();
    test_session_stamp_detects_change();
    test_firefox_json_rejects_trailing_and_excessive_depth();
    test_firefox_json_rejects_malformed_unicode_numbers_and_controls();
    test_firefox_json_depth_node_and_string_budget_boundaries();
    test_browser_parser_default_limits_are_exact();
    test_snss_rejects_zero_trailing_and_malformed_known_commands();
    test_snss_unique_id_and_command_cap_boundaries();
    test_snss_window_and_tab_maps_have_independent_exact_caps();
    test_snss_navigation_duplicate_and_search_budget_boundaries();
    test_snss_per_window_and_global_text_caps_are_exact();
    test_mozlz4_exact_decode_and_limit_boundaries();
    test_mozlz4_rejects_malformed_blocks_transactionally();
    test_lz4_match_offset_and_extension_arithmetic_edges();
    test_get_session_stamp_accepts_exact_cap_and_rejects_over();
    test_session_stamp_detects_equal_metadata_replace_and_in_place_rewrite();
    test_firefox_profile_ini_default_release_fallback();
    test_firefox_json_valid_empty_is_distinct_from_failure();
    test_firefox_selected_index_rejects_int_min_without_overflow();
    test_chromium_resolver_tracks_rotation_and_breaks_stamp_ties();
    test_already_posted_session_results_are_drained_and_freed();
    test_session_status_and_acceptance_policy_contract();
    test_session_cache_shares_payload_and_rejects_oversize();
    test_session_worker_valid_empty_is_fresh_and_cache_hit_is_shared();
    test_session_worker_malformed_cold_is_unavailable();
    test_session_worker_non_ok_reads_never_parse_and_publish_current_stamp();
    test_session_worker_disappeared_source_is_not_reported_as_current();
    test_session_worker_stamp_change_uses_exact_path_cached_stale();
    test_session_worker_rotation_during_parse_is_never_fresh();
    test_session_worker_equal_metadata_replacement_never_publishes_old_bytes_fresh();
    test_session_worker_rejects_aba_bytes_without_matching_handle_stamp();
    test_session_worker_rotation_uses_only_exact_attempted_path_cache();
    test_session_worker_ten_rapid_requests_are_active_plus_newest_pending();
    test_session_worker_low_probe_cannot_replace_user_pending();
    test_session_worker_cross_app_manual_preempts_pending_metadata();
    test_session_worker_rejects_unsupported_app_queue_amplification();
    test_session_coordinator_preserves_purpose_and_shared_payload_identity();
    test_session_coordinator_rejects_old_generation_profile_purpose_and_request();
    test_session_profile_comparison_covers_every_config_field();
    test_manual_operation_profiles_remain_captured_across_settings_changes();
    test_session_coordinator_superseded_only_releases_bookkeeping();
    test_session_coordinator_request_faults_are_transactional();
    test_session_coordinator_accept_faults_preserve_pending_and_latest();
    test_posted_session_result_is_owned_immediately_on_rejection();
    test_unavailable_defer_is_once_per_current_source_and_preserves_bytes();
    test_session_data_generation_is_per_app_and_hash_breaks_stamp_ties();
    test_session_data_generation_is_monotonic_when_historical_cache_returns();
    test_session_data_generation_saturates_without_zero_or_rollback();
    test_session_cache_enforces_sixteen_entry_lru_cap();
    test_session_cache_byte_cap_counts_external_ui_payload();
    test_post_message_failure_deletes_heap_result();
    test_session_worker_oversized_payload_is_unavailable();
    test_session_worker_stop_joins_and_suppresses_unposted_completion();
    test_session_worker_stop_waits_for_inflight_superseded_post();
    test_session_worker_reentrant_requester_poster_stop_completes();
    test_session_worker_reentrant_worker_poster_stop_defers_self_join();
    test_session_worker_reentrant_stop_waits_for_confirmed_worker_post_path();
    test_session_worker_concurrent_external_and_reentrant_stop_do_not_cycle();
    test_session_worker_reentrant_worker_stop_survives_repeated_destruction();
    test_worker_retained_budget_includes_posted_ui_ownership();
    test_failed_cache_replacement_preserves_exact_stale_payload();
    test_session_cache_put_is_strongly_transactional_at_every_fault_step();
    test_session_cache_put_preserves_output_on_capacity_and_budget_rejection();
    test_session_worker_contains_internal_allocation_faults_and_continues();
    test_session_worker_result_allocation_failure_drops_only_that_request();
    test_session_worker_result_prepare_fault_drops_unidentified_result_and_continues();
    test_session_worker_superseded_result_factory_is_not_called_under_state_lock();
    test_session_worker_request_fault_preserves_existing_pending();
    test_session_cache_lookup_fault_preserves_output_and_lru();
    test_session_cache_runtime_fault_is_transactional();
    test_post_message_exception_deletes_heap_result();
    test_layout_serializes_v4_header();
    test_layout_roundtrip_v4();
    test_layout_provisional_marker_roundtrips_strict_v4();
    test_layout_noncanonical_record_id_is_published_canonically();
    test_v4_provisional_extension_preserves_base_window_row();
    test_layout_provisional_marker_is_strict_and_transactional();
    test_layout_legacy_migration_never_invents_provisional_marker();
    test_layout_provisional_companions_do_not_consume_record_cap();
    test_layout_parse_v2();
    test_layout_rejects_invalid_desktop_guid_transactionally();
    test_layout_rejects_progid_as_desktop_guid();
    test_layout_rejects_integer_trailing_junk_transactionally();
    test_layout_migrates_v3_record();
    test_layout_rejects_negative_v3_missing_counter_transactionally();
    test_layout_legacy_migration_rejects_generated_id_collision_transactionally();
    test_layout_legacy_migration_rejects_invalid_generated_ids_transactionally();
    test_layout_rejects_embedded_carriage_returns_transactionally();
    test_layout_rejects_trailing_columns();
    test_layout_rejects_duplicate_record_ids();
    test_layout_enforces_total_record_cap_transactionally();
    test_retention_expiration_boundaries();
    test_retention_future_and_zero_missing_are_not_expired();
    test_retention_mark_seen_clears_missing_and_updates_last_seen();
    test_retention_mark_missing_uses_last_seen_and_is_idempotent();
    test_retention_prune_preserves_order_duplicates_and_input();
    test_startup_expiry_partitions_every_app_transactionally();
    test_reconcile_restores_saved_a_and_creates_new_b();
    test_fresh_reconcile_adopts_one_persisted_provisional_with_same_id();
    test_fresh_reconcile_clears_multiple_matched_provisionals();
    test_fresh_reconcile_defers_ambiguous_provisional_adoption();
    test_expired_reappearance_is_new_not_restored();
    test_cached_stale_edge_preserves_match_and_defers_unmatched();
    test_failed_chrome_restore_retains_saved_destination_and_marks_seen();
    test_empty_chrome_reconcile_marks_only_chrome_missing();
    test_reserved_chrome_record_cannot_be_stolen_by_duplicate();
    test_same_desktop_match_learns_live_index_without_restore();
    test_late_window_after_first_wave_restores_before_save();
    test_edge_retention_is_independent_while_firefox_stays_open();
    test_firefox_sibling_reappears_while_first_window_stays_open();
    test_reconcile_plan_and_commit_preserve_input_vectors();
    test_reconcile_empty_generator_defers_transactionally();
    test_reconcile_invalid_generators_defer_transactionally();
    test_reconcile_generator_collision_with_any_existing_record_defers();
    test_reconcile_duplicate_generated_ids_defer_transactionally();
    test_reconcile_unique_generated_id_commits_strict_v4();
    test_reconcile_null_generator_defers_transactionally();
    test_reconcile_match_preflight_too_complex_defers_cleanly();
    test_reconcile_window_caps_defer_before_generation();
    test_reconcile_malformed_reserved_id_defers_before_work();
    test_reconcile_reserved_id_cap_is_fail_closed_at_boundary();
    test_reconcile_guaranteed_capacity_defers_before_matcher();
    test_reconcile_projects_mark_missing_expiration_before_capacity();
    test_projected_retained_count_rejects_mismatched_flags();
    test_reconcile_duplicate_injected_match_ownership_defers_cleanly();
    test_reconcile_rejects_all_malformed_injected_matches();
    test_reconcile_unsupported_app_defers_without_generation();
    test_commit_reconcile_rejects_out_of_range_mixed_plan_atomically();
    test_commit_reconcile_rejects_malformed_restore_sets_atomically();
    test_commit_reconcile_rejects_duplicate_match_ownership();
    test_commit_reconcile_rejects_app_mismatches();
    test_commit_reconcile_rejects_invalid_new_record_requests();
    test_commit_reconcile_rejects_cached_stale_actions();
    test_commit_reconcile_rejects_planning_clock_mismatch();
    test_reconcile_rejects_nonpositive_planning_clocks();
    test_reconcile_rejects_invalid_freshness_before_planning();
    test_commit_reconcile_rejects_projected_output_overflow();
    test_layout_score_formula_and_fallback();
    test_layout_score_browser_symmetry_and_cross_app_rejection();
    test_layout_score_identical_two_domain_is_exact();
    test_match_one_to_one_duplicate_fingerprints_are_unique();
    test_match_one_to_one_browser_apps_and_never_crosses_apps();
    test_match_one_to_one_score_evaluation_budget();
    test_assignment_maximizes_cardinality_before_score();
    test_assignment_maximizes_total_score_at_same_cardinality();
    test_assignment_ties_are_deterministic_across_input_order();
    test_assignment_filters_and_deduplicates_without_mutating_input();
    test_assignment_randomized_against_exhaustive_oracle();
    test_assignment_candidate_cap_direct_and_generated();
    test_assignment_flow_work_budget_rejects_connected_cycle();
    test_assignment_checked_score_scaling_boundary();
    test_checked_snapshot_enforces_combined_record_cap();
    test_checked_snapshot_rejects_zero_desktop_record_transactionally();
    test_checked_snapshot_rejects_malformed_record_id_transactionally();
    test_checked_snapshot_rejects_empty_id_and_zero_last_seen();
    test_checked_snapshot_rejects_zero_record_id_transactionally();
    test_checked_snapshot_rejects_duplicate_record_ids_transactionally();
    test_checked_snapshot_rejects_negative_missing_since_transactionally();
    test_checked_snapshot_accepts_supported_browser_apps();
    test_checked_snapshot_rejects_unsupported_app_transactionally();
    test_checked_snapshot_rejects_negative_tab_count_transactionally();
    test_checked_snapshot_rejects_invalid_counts_transactionally();
    test_checked_snapshot_rejects_raw_domain_line_breaks_transactionally();
    test_lc_initial_absence_marks_missing_once();
    test_lc_two_stable_present_snapshots_begin_restore();
    test_lc_stale_restore_completion_is_ignored();
    test_restore_budget_is_exact_keyed();
    test_lc_timeout_is_per_wave_and_survives_clock_rollback();
    test_lc_absence_transitions_mark_once_and_reappearance_rearms();
    test_lc_exhausted_generation_suppresses_missing_action();
    test_lc_absence_during_flight_clears_rearm_if_still_absent();
    test_lc_absence_reappearance_during_flight_queues_one_wave();
    test_lc_firefox_chrome_edge_states_are_independent();
    test_lc_layout_change_saves_but_restore_inputs_restore_first();
    test_lc_same_hwnd_new_fresh_session_starts_one_wave();
    test_lc_inflight_changes_queue_exactly_one_latest_rearm();
    test_lc_late_and_returning_sibling_each_start_one_wave();
    test_lc_generation_max_is_issued_once_then_fails_closed();
    test_lc_deferred_retries_three_times_with_exact_backoff();
    test_lc_deferred_key_change_resets_for_window_or_session();
    test_lc_deferred_key_change_during_backoff_restarts_settle_now();
    test_lc_inflight_a_to_b_to_a_history_rearms_deferred_wave();
    test_lc_deferred_backoff_rebases_after_clock_rollback();
    test_lc_deferred_backoff_distinguishes_exact_max_from_overflow();
    test_lc_deferred_rollback_to_unrepresentable_deadline_fails_closed();
    test_lc_deferred_budget_resets_on_new_source_stamp();
    test_lc_cancelled_stale_reconcile_retires_exact_flight_and_rearms();
    test_lc_all_completion_outcomes_honor_one_queued_rearm();
    test_lc_exhausted_records_actual_layout_without_save_loop();
    test_lc_explicit_save_completion_is_generation_safe();
    test_lc_explicit_save_completion_commits_captured_layout_only();
    test_lc_explicit_save_completion_rebases_pending_wave_on_rollback();
    test_restore_budgets_isolate_siblings_runtime_and_destination();
    test_restore_budgets_prune_only_dead_runtime_identities();
    test_restore_budgets_cap_uses_deterministic_touch_lru();
    test_restore_budgets_new_key_copy_failure_is_transactional();
    test_bounded_read_exact_limit_and_preallocation_rejection();
    test_session_bounded_reader_binds_bytes_to_exact_aba_handle();
    test_session_bounded_reader_rejects_handle_changes_and_close_failure();
    test_bounded_read_failures_are_transactional_and_status_bearing();
    test_bounded_read_denies_concurrent_in_place_writer();
    test_layout_load_missing_and_valid_primary();
    test_layout_recovery_prefers_valid_rollback_and_preserves_primary();
    test_layout_recovery_uses_bak_and_preserves_all_corruption();
    test_two_corrupt_streams_require_verified_diagnostics();
    test_transient_primary_open_blocks_backup_recovery();
    test_oversized_primary_blocks_all_recovery_without_mutation();
    test_missing_primary_uses_rollback_priority_then_backup_fallback();
    test_transient_and_corrupt_recovery_states();
    test_diagnostic_copy_failure_and_readback_mismatch_block_writes();
    test_second_diagnostic_copy_failure_and_collision_never_lose_evidence();
    test_diagnostic_reuse_never_deletes_changed_corrupt_temp();
    test_diagnostic_reuse_never_overwrites_changed_corrupt_backup();
    test_diagnostic_preservation_revalidates_primary_before_backup_recovery();
    test_diagnostic_preservation_revalidates_primary_before_displaced_recovery();
    test_fresh_diagnostic_copy_revalidates_primary_before_recovery();
    test_diagnostic_source_reverify_transient_failure_retries_without_growth();
    test_atomic_write_first_and_two_successful_writes();
    test_durable_write_captures_revision_without_post_publish_read();
    test_durable_publish_exception_adopts_revision_then_retries();
    test_first_post_publish_verify_throw_recovers_from_armed_candidate();
    test_atomic_write_rejects_oversize_without_touching_destination();
    test_atomic_write_faults_keep_old_or_recovery_bytes();
    test_preexisting_rollback_promotion_failure_never_touches_primary();
    test_rollback_promotion_readback_failure_restores_old_bak_and_rollback();
    test_preexisting_rollback_promotion_mismatch_recovers_from_verified_marker();
    test_post_replace_verification_failure_reports_false_with_bak_readable();
    test_late_normal_write_failure_retains_staged_old_backup();
    test_retry_after_publish_finishes_pending_promotion_without_self_replace();
    test_replace_failure_retains_staged_old_backup_until_retry();
    test_staged_backup_resolver_restores_missing_backup_before_cleanup();
    test_changed_request_first_reconciles_pending_normal_transaction();
    test_failed_backup_staging_readback_does_not_poison_retries();
    test_pending_promotion_marker_recovers_post_move_verification_fault();
    test_promotion_marker_false_after_poison_is_never_authoritative();
    test_transient_promoted_backup_read_is_retained_and_loads_before_retry();
    test_loader_recovers_valid_authoritative_internal_marker();
    test_loader_never_accepts_stale_bak_beside_authoritative_promotion_marker();
    test_valid_primary_reconciles_authoritative_promotion_marker();
    test_valid_primary_marker_faults_fail_closed_then_converge();
    test_valid_primary_retries_cleanup_after_trigger_delete_false_effect();
    test_first_publish_tmp_recovers_on_restart_after_move_failure();
    test_partial_valid_prefix_is_never_treated_as_committed_first_publish();
    test_temporary_commit_move_failure_converges_before_and_after_effect();
    test_idempotent_retry_cleans_orphan_prior_backup_stage();
    test_orphan_promotion_stage_is_non_authoritative_and_converges();
    test_non_authoritative_stages_are_discarded_without_reading_bytes();
    test_non_authoritative_stage_faults_fail_closed_and_retry();
    test_previous_stage_cleanup_reverifies_before_consuming_previous();
    test_staged_backup_restore_mismatch_never_discards_intended_bytes_on_retry();
    test_partial_effect_replace_failure_remains_recoverable();
    test_first_write_preserves_existing_recovery_artifacts();
    test_failed_rollback_promotion_stays_recoverable_before_older_bak();
    test_recovery_write_preserves_known_good_backup_and_reports_cleanup_failure();
    test_preserve_retry_without_named_recovery_converges();
    test_displaced_restart_and_default_write_converge();
    test_displaced_reconciliation_faults_retry_safely();
    test_corrupt_primary_recovers_from_sole_valid_displaced();
    test_corrupt_primary_displaced_faults_preserve_once_and_retry();
    test_corrupt_displaced_is_preserved_but_never_recovered();
    test_legacy_migration_failure_preserves_source_and_publishes_nothing();
    test_legacy_migration_parses_before_publishing();
    test_legacy_migration_never_overlays_recoverable_target();
    test_legacy_migration_installs_checked_v4_then_retires_source();
    test_same_revision_compares_every_field();
    test_missing_primary_corrupt_recovery_revision_allows_empty_publish();
    test_recovered_conflict_preserves_valid_backup_before_publish();
    test_two_actor_stale_save_is_rejected_without_overwrite();
    test_two_actor_recovered_source_stale_save_is_rejected();
    test_layout_mutex_zero_timeout_and_acquisition_after_release();
    test_layout_mutex_treats_abandoned_as_acquired();
    test_layout_fixture_removes_only_its_unique_tree();
    printf("%d/%d passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
