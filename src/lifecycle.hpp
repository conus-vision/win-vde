// lifecycle.hpp — pure state machine deciding when to restore/save a tracked app.
// No Win32/COM: the caller supplies `present` (app has >=1 window) each tick and
// executes the returned action. Unit-tested in tests/vdtest.cpp.
#pragma once

enum class LcAction { None, StartupRestore, DoRestore, AutoSave, FinalSave };

struct LcState {
    bool prevPresent=false;
    bool restoredThisAppearance=false;
    bool launchPending=false;
    int  stableTicks=0;
};

// Called once when the utility starts. If the app is already up, restore now.
inline LcAction LcOnStartup(LcState& s, bool present){
    s.prevPresent = present;
    if(present){ s.restoredThisAppearance = true; s.launchPending = false; s.stableTicks = 0; return LcAction::StartupRestore; }
    return LcAction::None;
}

// Called every monitor tick. `settleTicksNeeded` = how many consecutive present
// ticks to wait after an appearance before restoring (launch stabilization).
inline LcAction LcOnTick(LcState& s, bool present, int settleTicksNeeded){
    if(present && !s.prevPresent){                        // absent -> present (launch / session-restore)
        s.prevPresent = true; s.launchPending = true; s.restoredThisAppearance = false; s.stableTicks = 1;
        return LcAction::None;
    }
    if(present && s.prevPresent){
        if(s.launchPending){
            s.stableTicks++;
            if(s.stableTicks >= settleTicksNeeded){ s.launchPending=false; s.restoredThisAppearance=true; return LcAction::DoRestore; }
            return LcAction::None;
        }
        return LcAction::AutoSave;                         // steady state while present
    }
    // !present
    if(s.prevPresent){ s.prevPresent=false; s.launchPending=false; s.restoredThisAppearance=false; s.stableTicks=0; }
    return LcAction::None;                                 // present->absent or staying absent: never wipe
}

// Called once at utility exit. Save only if the app currently has windows.
inline LcAction LcOnExit(const LcState&, bool present){ return present ? LcAction::FinalSave : LcAction::None; }
