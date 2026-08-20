// move_queue.hpp -- pure owner-aware scheduling for timer-driven window moves.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include <windows.h>

enum class MoveAction { None, Issue, Verify };
enum class MoveOwner { AutoReconcile, ManualTray, Picker };
enum class MoveTerminal {
    None, Succeeded, Cancelled, PermanentFailure, Exhausted
};
enum class MoveAttemptOutcome {
    Accepted, OnDestination, TransientFailure, PermanentFailure
};

struct MoveToken {
    MoveOwner owner = MoveOwner::AutoReconcile;
    uint64_t operationId = 0;
    uint64_t jobId = 0;
    size_t itemIndex = 0;
};

struct MoveJob {
    MoveToken token;
    std::string runtimeKey; // diagnostics/coalescing only; never a dispatch key
    std::string recordId;
    GUID destination = {0};
    int attempts = 0;
    bool waitingForVerify = false;
};

struct MoveResult {
    bool completed = false;
    MoveTerminal terminal = MoveTerminal::None;
    int attempts = 0;
    MoveToken token;
    std::string runtimeKey;
    std::string recordId;
};

// Layout-backed producers are capped at 4096 records. Keeping the same bound
// here also limits duplicate-ID scans and cancellation-copy work.
static const size_t MAX_MOVE_QUEUE_JOBS = 4096;

class MoveQueue {
    static const int kMaxAttempts = 4;
    std::deque<MoveJob> jobs_;

    static bool validOwner(MoveOwner owner) noexcept {
        switch(owner){
        case MoveOwner::AutoReconcile:
        case MoveOwner::ManualTray:
        case MoveOwner::Picker:
            return true;
        }
        return false;
    }

    static bool zeroGuid(const GUID& value) noexcept {
        if(value.Data1!=0 || value.Data2!=0 || value.Data3!=0) return false;
        for(size_t i=0;i<sizeof(value.Data4);++i)
            if(value.Data4[i]!=0) return false;
        return true;
    }

    static MoveResult resultFor(const MoveJob& job,bool completed,
                                MoveTerminal terminal,int attempts){
        MoveResult result;
        result.completed=completed;
        result.terminal=terminal;
        result.attempts=attempts;
        result.token=job.token;
        result.runtimeKey=job.runtimeKey;
        result.recordId=job.recordId;
        return result;
    }

    MoveResult finishFront(MoveTerminal terminal,int attempts){
        MoveResult result=resultFor(jobs_.front(),true,terminal,attempts);
        jobs_.pop_front();
        return result;
    }

public:
    bool enqueue(const MoveJob& job){
        if(!validOwner(job.token.owner) || job.token.operationId==0 ||
                job.token.jobId==0 || zeroGuid(job.destination) ||
                job.attempts!=0 || job.waitingForVerify)
            return false;
        if(jobs_.size()>=MAX_MOVE_QUEUE_JOBS) return false;
        for(const MoveJob& queued : jobs_)
            if(queued.token.jobId==job.token.jobId) return false;
        jobs_.push_back(job);
        return true;
    }

    bool empty() const noexcept { return jobs_.empty(); }

    const MoveJob* front() const noexcept {
        return jobs_.empty() ? nullptr : &jobs_.front();
    }

    MoveAction nextAction() const noexcept {
        if(jobs_.empty()) return MoveAction::None;
        return jobs_.front().waitingForVerify
            ? MoveAction::Verify : MoveAction::Issue;
    }

    MoveResult onIssued(MoveAttemptOutcome outcome){
        if(jobs_.empty() || jobs_.front().waitingForVerify) return MoveResult();
        const int attempts=jobs_.front().attempts+1;
        switch(outcome){
        case MoveAttemptOutcome::Accepted:
        case MoveAttemptOutcome::TransientFailure: {
            MoveResult result=resultFor(
                jobs_.front(),false,MoveTerminal::None,attempts);
            jobs_.front().attempts=attempts;
            jobs_.front().waitingForVerify=true;
            return result;
        }
        case MoveAttemptOutcome::OnDestination:
            return finishFront(MoveTerminal::Succeeded,attempts);
        case MoveAttemptOutcome::PermanentFailure:
            return finishFront(MoveTerminal::PermanentFailure,attempts);
        }
        return finishFront(MoveTerminal::PermanentFailure,attempts);
    }

    MoveResult onVerified(MoveAttemptOutcome outcome){
        if(jobs_.empty() || !jobs_.front().waitingForVerify) return MoveResult();
        const int attempts=jobs_.front().attempts;
        switch(outcome){
        case MoveAttemptOutcome::OnDestination:
            return finishFront(MoveTerminal::Succeeded,attempts);
        case MoveAttemptOutcome::TransientFailure:
            if(attempts>=kMaxAttempts)
                return finishFront(MoveTerminal::Exhausted,attempts);
            else {
                MoveResult result=resultFor(
                    jobs_.front(),false,MoveTerminal::None,attempts);
                jobs_.front().waitingForVerify=false;
                return result;
            }
        case MoveAttemptOutcome::PermanentFailure:
            return finishFront(MoveTerminal::PermanentFailure,attempts);
        case MoveAttemptOutcome::Accepted:
            break;
        }
        return finishFront(MoveTerminal::PermanentFailure,attempts);
    }

    MoveResult cancelJob(uint64_t jobId){
        std::deque<MoveJob>::const_iterator found=jobs_.end();
        for(std::deque<MoveJob>::const_iterator it=jobs_.begin();it!=jobs_.end();++it)
            if(it->token.jobId==jobId){ found=it; break; }
        if(found==jobs_.end()) return MoveResult();

        MoveResult result=resultFor(*found,true,MoveTerminal::Cancelled,found->attempts);
        std::deque<MoveJob> survivors;
        for(const MoveJob& job : jobs_)
            if(job.token.jobId!=jobId) survivors.push_back(job);
        jobs_.swap(survivors);
        return result;
    }

    std::vector<MoveResult> cancelOperation(MoveOwner owner,uint64_t operationId){
        size_t count=0;
        for(const MoveJob& job : jobs_)
            if(job.token.owner==owner && job.token.operationId==operationId) ++count;
        if(count==0) return std::vector<MoveResult>();

        std::vector<MoveResult> results;
        results.reserve(count);
        std::deque<MoveJob> survivors;
        for(const MoveJob& job : jobs_){
            if(job.token.owner==owner && job.token.operationId==operationId)
                results.push_back(resultFor(
                    job,true,MoveTerminal::Cancelled,job.attempts));
            else
                survivors.push_back(job);
        }
        jobs_.swap(survivors);
        return results;
    }
};
