// lifecycle.hpp -- pure, per-app restore/save state and per-window move budgets.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "layout.hpp"

enum class LcAction {
    None, BeginRestore, SaveLayout, MarkMissingFromLastSeen,
    // TRANSITIONAL: delete in Task 8
    StartupRestore, DoRestore, AutoSave, FinalSave
};

enum class LcRestoreOutcome { Success, Deferred, Exhausted };

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

    // TRANSITIONAL: delete in Task 8. Disjoint state for the Task 4 vde.cpp shim.
    bool prevPresent = false;
    bool restoredThisAppearance = false;
    bool launchPending = false;
    int stableTicks = 0;
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
           state.deferredSessionStampSignature != completedSession){
            state.deferredAttempts = 0;
            state.deferredWindowSetSignature = completedWindowSet;
            state.deferredSessionStampSignature = completedSession;
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
    (void)sessionStampSignature;
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

    void markExhausted(const RestoreBudgetKey& key){
        if(touchIfPresent(key)) return;
        Entry pending{ops_.copyKey(key)};
        entries_.push_back(std::move(pending));
        if(entries_.size() > kMaximumEntries) entries_.erase(entries_.begin());
    }

    void clearExact(const RestoreBudgetKey& key){
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
            [&](const Entry& entry){ return entry.key == key; }), entries_.end());
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

// TRANSITIONAL: delete in Task 8. Exact Task 4 API retained only so the
// intermediate vde.cpp remains buildable; new code uses LcObserve above.
inline LcAction LcOnStartup(LcState& s, bool present){
    s.prevPresent = present;
    if(present){ s.restoredThisAppearance = true; s.launchPending = false; s.stableTicks = 0; return LcAction::StartupRestore; }
    return LcAction::None;
}

inline LcAction LcOnTick(LcState& s, bool present, int settleTicksNeeded){
    if(present && !s.prevPresent){
        s.prevPresent = true; s.launchPending = true; s.restoredThisAppearance = false; s.stableTicks = 1;
        return LcAction::None;
    }
    if(present && s.prevPresent){
        if(s.launchPending){
            s.stableTicks++;
            if(s.stableTicks >= settleTicksNeeded){ s.launchPending=false; s.restoredThisAppearance=true; return LcAction::DoRestore; }
            return LcAction::None;
        }
        return LcAction::AutoSave;
    }
    if(s.prevPresent){ s.prevPresent=false; s.launchPending=false; s.restoredThisAppearance=false; s.stableTicks=0; }
    return LcAction::None;
}

inline LcAction LcOnExit(const LcState&, bool present){
    return present ? LcAction::FinalSave : LcAction::None;
}
