// desktop_pin.hpp -- pure state machine that keeps the user on their current
// virtual desktop while VDE moves windows in the background.
//
// A window that takes the foreground while it sits on another desktop makes
// Windows switch to that desktop.  Browsers do this while they restore a
// session, so a batch of moves can flip the screen under the user.  The pin
// watches the current desktop during the batch and for a moment after it, and
// asks for a switch back when the desktop changed without the user asking.
#pragma once

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <windows.h>

enum class DesktopPinAction { None, SwitchBack, Released };

class DesktopPin {
    bool armed_=false;
    GUID pinned_={0};
    uint64_t armedMs_=0;
    uint64_t lastWorkMs_=0;
    uint64_t lastIntentMs_=0;
    uint64_t lastSwitchBackMs_=0;
    bool intentSeen_=false;
    bool switchedBack_=false;
    int switchBacks_=0;
    int failures_=0;

    static bool zero(const GUID& value) noexcept {
        const GUID none={0};
        return memcmp(&value,&none,sizeof(GUID))==0;
    }
    static bool same(const GUID& left,const GUID& right) noexcept {
        return memcmp(&left,&right,sizeof(GUID))==0;
    }
    void disarm() noexcept { *this=DesktopPin(); }

public:
    // Browsers keep activating windows for a moment after the last move.
    static const uint32_t kSettleMs=1500;
    // A desktop change this soon after a user gesture is the user's own.
    static const uint32_t kUserIntentMs=800;
    // Explorer reports the old desktop for a while after a switch is issued.
    static const uint32_t kSwitchBackSpacingMs=150;
    static const int kMaxSwitchBacks=64;
    static const int kMaxFailures=3;
    static const uint32_t kMaxPinnedMs=5*60*1000;

    bool armed() const noexcept { return armed_; }
    const GUID& pinned() const noexcept { return pinned_; }

    // Starts a pin on the current desktop; an armed pin only records the work.
    bool arm(const GUID& current,uint64_t nowMs) noexcept {
        if(armed_){ lastWorkMs_=nowMs; return true; }
        if(zero(current)) return false;
        armed_=true;
        pinned_=current;
        armedMs_=nowMs;
        lastWorkMs_=nowMs;
        return true;
    }

    // VDE's own desktop switches move the pin along with the user.
    void retarget(const GUID& destination) noexcept {
        if(armed_ && !zero(destination)) pinned_=destination;
    }

    DesktopPinAction observe(const GUID& current,uint64_t nowMs,
                             bool workPending,bool userIntent) noexcept {
        if(!armed_) return DesktopPinAction::None;
        if(userIntent){ intentSeen_=true; lastIntentMs_=nowMs; }
        if(workPending) lastWorkMs_=nowMs;
        if(nowMs-armedMs_>=kMaxPinnedMs){
            disarm();
            return DesktopPinAction::Released;
        }
        if(!zero(current) && !same(current,pinned_)){
            if(intentSeen_ && nowMs-lastIntentMs_<=kUserIntentMs){
                pinned_=current;
                return DesktopPinAction::None;
            }
            if(switchBacks_>=kMaxSwitchBacks){
                disarm();
                return DesktopPinAction::Released;
            }
            lastWorkMs_=nowMs;
            if(switchedBack_ && nowMs-lastSwitchBackMs_<kSwitchBackSpacingMs)
                return DesktopPinAction::None;
            switchedBack_=true;
            lastSwitchBackMs_=nowMs;
            ++switchBacks_;
            return DesktopPinAction::SwitchBack;
        }
        if(!workPending && nowMs-lastWorkMs_>=kSettleMs){
            disarm();
            return DesktopPinAction::Released;
        }
        return DesktopPinAction::None;
    }

    void onSwitchBackResult(bool succeeded) noexcept {
        if(!armed_) return;
        if(succeeded){ failures_=0; return; }
        if(++failures_>=kMaxFailures) disarm();
    }
};

// The shell windows a user goes through to change desktops on purpose: Task
// View and Alt+Tab, and the taskbar with its thumbnails.
inline bool DesktopPinShellClassShowsIntent(const wchar_t* className) noexcept {
    if(!className) return false;
    static const wchar_t* const classes[]={
        L"XamlExplorerHostIslandWindow",L"MultitaskingViewFrame",
        L"Shell_TrayWnd",L"Shell_SecondaryTrayWnd",L"TaskListThumbnailWnd",
    };
    for(const wchar_t* candidate : classes)
        if(wcscmp(className,candidate)==0) return true;
    return false;
}
