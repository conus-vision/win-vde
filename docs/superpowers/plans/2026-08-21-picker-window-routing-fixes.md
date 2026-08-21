# Picker Window Routing Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the picker show all ordinary Alt+Tab windows, make Ctrl+Click reliably move and follow the active window, and always place the popup on the primary physical monitor without broadening browser-only persistence.

**Architecture:** Keep stable runtime identity mandatory for moving, highlighting, icon caching, and browser persistence, but make it optional for row presentation. Route ordinary and controlled virtual-desktop switches through one foreground-handoff helper, snapshot mouse Ctrl from `WM_LBUTTONDOWN`, and isolate primary-monitor lookup from popup centering geometry.

**Tech Stack:** C++14, Win32 windowing/GDI, undocumented Windows 11 virtual-desktop COM interfaces, the existing single-binary test harness in `tests/vdtest.cpp`, MSVC build scripts.

---

## File map

- Modify `src/picker_state.hpp`: add pure, unit-testable row-admission,
  foreground-handoff planning, mouse-modifier, and centering helpers.
- Modify `src/vde.cpp`: publish display-only picker rows, guard
  identity-dependent work, share the foreground handoff, and use primary-monitor
  placement.
- Modify `tests/vdtest.cpp`: add behavioral tests and source-wiring regression
  checks for the three reported defects.
- No layout-store, browser-session, lifecycle, or persistence file changes are
  required.

## Execution setup

Create an isolated worktree from the current `main` after this plan commit:

```powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde worktree add .worktrees/picker-window-routing -b fix/picker-window-routing main
```

Run the clean baseline in the worktree:

```powershell
cmd.exe /d /c .\build-test.bat
cmd.exe /d /c .\build.bat
```

Expected: `9408/9408 passed` (or a larger all-passing total if tests were added
before execution) and `Built build\vde.exe`.

### Task 1: Publish windows without weakening stable identity

**Files:**

- Modify: `src/picker_state.hpp:2623-2669`
- Modify: `src/vde.cpp:5149-5150`
- Modify: `src/vde.cpp:5288-5316`
- Modify: `src/vde.cpp:5366-5450`
- Modify: `src/vde.cpp:5940-5959`
- Modify: `src/vde.cpp:6272-6288`
- Test: `tests/vdtest.cpp:1805-1850`
- Test: `tests/vdtest.cpp:11560-11625`
- Test registration: `tests/vdtest.cpp:18703-18707`

- [ ] **Step 1: Write failing admission and capability tests**

Add these tests beside
`test_picker_volatile_rows_skip_but_structural_failures_abort`:

```cpp
static void test_picker_row_admission_keeps_displayable_unverified_windows(){
    CHECK(DecidePickerRowAdmission(
        true,true,true,true,WindowIdentityRecapture::Match)==
          PickerRowAdmission::Verified);
    CHECK(DecidePickerRowAdmission(
        true,true,true,false,WindowIdentityRecapture::Indeterminate)==
          PickerRowAdmission::DisplayOnly);
    CHECK(DecidePickerRowAdmission(
        true,true,true,true,WindowIdentityRecapture::Indeterminate)==
          PickerRowAdmission::DisplayOnly);
    CHECK(DecidePickerRowAdmission(
        true,true,true,true,WindowIdentityRecapture::Lost)==
          PickerRowAdmission::Skip);
    CHECK(DecidePickerRowAdmission(
        false,true,true,true,WindowIdentityRecapture::Match)==
          PickerRowAdmission::Skip);
    CHECK(DecidePickerRowAdmission(
        true,false,true,true,WindowIdentityRecapture::Match)==
          PickerRowAdmission::Skip);
    CHECK(DecidePickerRowAdmission(
        true,true,false,true,WindowIdentityRecapture::Match)==
          PickerRowAdmission::Skip);

    CHECK(PickerRowUsesStableIdentity(PickerRowAdmission::Verified));
    CHECK(!PickerRowUsesStableIdentity(PickerRowAdmission::DisplayOnly));
    CHECK(!PickerRowUsesStableIdentity(PickerRowAdmission::Skip));
}
```

Register it immediately after
`test_picker_volatile_rows_skip_but_structural_failures_abort();` in `main()`.

- [ ] **Step 2: Run the suite and verify RED**

Run:

```powershell
cmd.exe /d /c .\build-test.bat
```

Expected: compilation fails because `PickerRowAdmission`,
`DecidePickerRowAdmission`, and `PickerRowUsesStableIdentity` do not exist.

- [ ] **Step 3: Add the minimal pure row-admission contract**

Add this after `AcceptPickerRowIdentity` in `src/picker_state.hpp`:

```cpp
enum class PickerRowAdmission {
    Skip,
    DisplayOnly,
    Verified
};

inline PickerRowAdmission DecidePickerRowAdmission(
        bool altTabEligible,bool desktopAvailable,bool titleAvailable,
        bool identityComplete,
        WindowIdentityRecapture identityRecapture) noexcept {
    if(!altTabEligible || !desktopAvailable || !titleAvailable)
        return PickerRowAdmission::Skip;
    if(!identityComplete ||
       identityRecapture==WindowIdentityRecapture::Indeterminate)
        return PickerRowAdmission::DisplayOnly;
    return identityRecapture==WindowIdentityRecapture::Match
        ? PickerRowAdmission::Verified : PickerRowAdmission::Skip;
}

inline bool PickerRowUsesStableIdentity(
        PickerRowAdmission admission) noexcept {
    return admission==PickerRowAdmission::Verified;
}
```

- [ ] **Step 4: Run the suite and verify the pure contract is GREEN**

Run:

```powershell
cmd.exe /d /c .\build-test.bat
```

Expected: all tests pass and the total increases by the new assertions.

- [ ] **Step 5: Write a failing source-wiring test for display-only rows**

Add beside `test_picker_icon_loading_is_bounded_and_outside_paint`:

```cpp
static void test_picker_enum_publishes_display_only_rows_safely(){
    const std::string source=ReadSourceFile(L"src\\vde.cpp");
    CHECK(!source.empty());
    const std::string row=SourceSection(
        source,"struct WinItem {","struct Tile {");
    const std::string enumerate=SourceSection(
        source,"static BOOL CALLBACK EnumAll(",
        "static bool PopulatePickerFilteredRows(");
    const std::string loader=SourceSection(
        source,"static HICON LoadWindowIconOutsidePaint(",
        "static HICON CachedWindowIcon(");
    const std::string preload=SourceSection(
        source,"static void PreloadVisiblePickerIcons(",
        "class ScopedPickerMeasureDc");
    const std::string paint=SourceSection(
        source,"static void Paint(","static void TipDeactivate(");

    CHECK(row.find("PickerRowAdmission admission")!=std::string::npos);
    CHECK(enumerate.find("DecidePickerRowAdmission(")!=std::string::npos);
    CHECK(enumerate.find("PickerRowAdmission::Skip")!=std::string::npos);
    CHECK(enumerate.find("PickerRowUsesStableIdentity(admission)")!=
          std::string::npos);
    CHECK(enumerate.find("ActiveProfiles(")==std::string::npos);
    CHECK(enumerate.find("ClassifyBrowserCandidate(")==std::string::npos);
    CHECK(loader.find("PickerRowUsesStableIdentity(window.admission)")!=
          std::string::npos);
    CHECK(preload.find("PickerRowUsesStableIdentity(window.admission)")!=
          std::string::npos);
    CHECK(paint.find("PickerRowUsesStableIdentity(window.admission)")!=
          std::string::npos);
}
```

Register it after `test_picker_icon_loading_is_bounded_and_outside_paint();`.

- [ ] **Step 6: Run the suite and verify the wiring test is RED**

Run:

```powershell
cmd.exe /d /c .\build-test.bat
```

Expected: the new test runs but its source checks fail because `WinItem` and
`EnumAll` still require every row to have a stable identity.

- [ ] **Step 7: Publish display-only rows and guard identity-only work**

Extend `WinItem` in `src/vde.cpp`:

```cpp
struct WinItem {
    HWND hwnd=nullptr;
    WindowIdentityKey identity;
    std::string runtimeKey;
    std::wstring title;
    std::wstring titleLC;
    std::wstring search;
    PickerRowAdmission admission=PickerRowAdmission::DisplayOnly;
};
```

In `EnumAll`, keep `IsAltTabWindow` first, then resolve desktop/tile and title
before attempting identity. Replace the mandatory identity block with this
best-effort block and use the resulting `admission` when constructing `WinItem`:

```cpp
DWORD pid=0;
uint64_t started=0;
WindowIdentityKey identity;
WindowIdentityRecapture identityRecapture=
    WindowIdentityRecapture::Indeterminate;
const bool havePid=GetWindowThreadProcessId(hwnd,&pid)!=0 && pid!=0;
if(havePid){
    auto process=context.processStarts.find(pid);
    if(process!=context.processStarts.end()){
        started=process->second;
    } else if(TryReadProcessStart(pid,started)){
        context.processStarts.emplace(pid,started);
    }
}
if(havePid && started!=0){
    identity.hwnd=reinterpret_cast<uintptr_t>(hwnd);
    identity.pid=pid;
    identity.processStart=started;
    identityRecapture=RecaptureGenericWindowIdentity(identity);
}
const bool identityComplete=SameIdentity(identity,identity);
const PickerRowAdmission admission=DecidePickerRowAdmission(
    true,true,true,identityComplete,identityRecapture);
if(admission==PickerRowAdmission::Skip)
    return HandlePickerRowReadResult(
        context,PickerRowReadResult::IdentityChanged);

WinItem item;
item.hwnd=hwnd;
item.title=title;
item.titleLC=title;
if(!item.titleLC.empty()) CharLowerW(&item.titleLC[0]);
item.search=item.titleLC;
item.admission=admission;
if(PickerRowUsesStableIdentity(admission)){
    FastWin fast;
    fast.hwnd=hwnd;
    fast.pid=pid;
    fast.processStart=started;
    fast.desktop=desktop;
    fast.title=title;
    item.identity=IdentityOf(fast);
    item.runtimeKey=RuntimeKey(fast);
}
if(!context.liveKeys)
    return HandlePickerRowReadResult(
        context,PickerRowReadResult::GlobalSnapshotFailure);
tile->windows.push_back(std::move(item));
if(PickerRowUsesStableIdentity(admission))
    context.liveKeys->insert(tile->windows.back().runtimeKey);
return TRUE;
```

Do not pass identity-unavailable rows to `HandlePickerRowReadResult`; that path
continues enumeration without publishing a row.

Add these guards:

```cpp
// First statement in LoadWindowIconOutsidePaint.
if(!PickerRowUsesStableIdentity(window.admission))
    return g_sharedFallbackIcon;

// In PreloadVisiblePickerIcons, immediately after resolving `window`.
if(!PickerRowUsesStableIdentity(window.admission))
    return IconPreloadStep::Cached;

// Around active-row highlighting in Paint.
if(PickerRowUsesStableIdentity(window.admission) &&
   IsActiveWindow(g_picker,window.identity)){
    // Existing active-row painting body remains unchanged.
}
```

Display-only rows continue through `BuildPickerFilteredIndices`, so title search,
scrolling, and tooltips require no special case. Their empty `runtimeKey` makes
`CachedWindowIcon` return the shared fallback icon.

- [ ] **Step 8: Run tests and production build**

Run:

```powershell
cmd.exe /d /c .\build-test.bat
cmd.exe /d /c .\build.bat
```

Expected: every test passes; production compilation ends with
`Built build\vde.exe`.

- [ ] **Step 9: Commit Task 1**

```powershell
git add -- src/picker_state.hpp src/vde.cpp tests/vdtest.cpp
git commit -m "fix(picker): show unverified window rows"
```

### Task 2: Make Ctrl+Click use a reliable switch path

**Files:**

- Modify: `src/picker_state.hpp:1717-1726`
- Modify: `src/vde.cpp:6438-6471`
- Modify: `src/vde.cpp:6580-6601`
- Modify: `src/vde.cpp:7600-7633`
- Modify: `src/vde.cpp:8006-8052`
- Test: `tests/vdtest.cpp:1759-1778`
- Test: `tests/vdtest.cpp:11921-11931`
- Test registration: `tests/vdtest.cpp:18703-18715`
- Modify: `docs/superpowers/specs/2026-08-21-picker-window-routing-fixes-design.md`
- Modify: `docs/superpowers/plans/2026-08-21-picker-window-routing-fixes.md`

- [ ] **Step 1: Write failing behavior tests for handoff and mouse Ctrl**

Add beside `test_picker_commit_requires_exact_active_identity`:

```cpp
static void test_picker_foreground_handoff_covers_popup_and_external_focus(){
    PickerForegroundHandoffPlan popup=PlanPickerForegroundHandoff(
        true,10,20,20);
    CHECK(popup.focusShell);
    CHECK(popup.attachDesktop);
    CHECK(!popup.attachForeground);

    PickerForegroundHandoffPlan external=PlanPickerForegroundHandoff(
        true,10,30,20);
    CHECK(external.focusShell);
    CHECK(external.attachDesktop);
    CHECK(external.attachForeground);

    PickerForegroundHandoffPlan unavailable=PlanPickerForegroundHandoff(
        false,10,30,20);
    CHECK(!unavailable.focusShell);
    CHECK(!unavailable.attachDesktop);
    CHECK(!unavailable.attachForeground);

    PickerForegroundHandoffPlan sharedQueue=PlanPickerForegroundHandoff(
        true,10,10,20);
    CHECK(sharedQueue.focusShell);
    CHECK(sharedQueue.attachDesktop);
    CHECK(!sharedQueue.attachForeground);

    PickerForegroundHandoffPlan desktopIsCurrent=
        PlanPickerForegroundHandoff(true,20,30,20);
    CHECK(desktopIsCurrent.focusShell);
    CHECK(!desktopIsCurrent.attachDesktop);
    CHECK(desktopIsCurrent.attachForeground);

    PickerForegroundHandoffPlan noDesktop=PlanPickerForegroundHandoff(
        true,0,30,20);
    CHECK(!noDesktop.focusShell);
    CHECK(!noDesktop.attachDesktop);
    CHECK(!noDesktop.attachForeground);

    PickerForegroundHandoffPlan noCurrent=PlanPickerForegroundHandoff(
        true,10,30,0);
    CHECK(!noCurrent.focusShell);
    CHECK(!noCurrent.attachDesktop);
    CHECK(!noCurrent.attachForeground);

    PickerForegroundHandoffPlan noForeground=PlanPickerForegroundHandoff(
        true,10,0,20);
    CHECK(noForeground.focusShell);
    CHECK(noForeground.attachDesktop);
    CHECK(!noForeground.attachForeground);
}

static void test_picker_mouse_ctrl_uses_button_message_snapshot(){
    CHECK(!PickerMouseControlHeld(0));
    CHECK(PickerMouseControlHeld(MK_CONTROL));
    CHECK(!PickerMouseControlHeld(MK_SHIFT));
    CHECK(PickerMouseControlHeld(MK_CONTROL|MK_LBUTTON));
}
```

Register both immediately after
`test_picker_commit_requires_exact_active_identity();`.

- [ ] **Step 2: Run the suite and verify RED**

Run:

```powershell
cmd.exe /d /c .\build-test.bat
```

Expected: compilation fails because the handoff and mouse helpers do not exist.

- [ ] **Step 3: Add the pure handoff plan and mouse snapshot helpers**

Add to `src/picker_state.hpp` near the picker UI routing helpers:

```cpp
struct PickerForegroundHandoffPlan {
    bool focusShell=false;
    bool attachDesktop=false;
    bool attachForeground=false;
};

inline PickerForegroundHandoffPlan PlanPickerForegroundHandoff(
        bool shellFound,uint32_t desktopThread,
        uint32_t foregroundThread,uint32_t currentThread) noexcept {
    PickerForegroundHandoffPlan plan;
    if(!shellFound || desktopThread==0 || currentThread==0) return plan;
    plan.focusShell=true;
    plan.attachDesktop=desktopThread!=currentThread;
    plan.attachForeground=foregroundThread!=0 &&
        foregroundThread!=currentThread &&
        foregroundThread!=desktopThread;
    return plan;
}

inline bool PickerMouseControlHeld(WPARAM buttonState) noexcept {
    return (buttonState&MK_CONTROL)!=0;
}
```

- [ ] **Step 4: Run the suite and verify the pure helpers are GREEN**

Run:

```powershell
cmd.exe /d /c .\build-test.bat
```

Expected: all tests pass.

- [ ] **Step 5: Write a failing source-wiring test for the shared switch**

Add near the other picker source tests:

```cpp
static void test_picker_ctrl_move_uses_shared_foreground_handoff(){
    const std::string source=ReadSourceFile(L"src\\vde.cpp");
    const std::string helper=SourceSection(
        source,"static HRESULT SwitchDesktopWithForegroundHandoff(",
        "static PickerObservation ExecutePickerEffect(");
    const std::string effects=SourceSection(
        source,"static PickerObservation ExecutePickerEffect(",
        "static void PumpPickerTransitionWork()");
    const std::string ordinary=SourceSection(
        source,"static void GoToDesktop(",
        "// Клик = переключение");
    const std::string mouse=SourceSection(
        source,"case WM_LBUTTONDOWN:","case WM_MOUSEMOVE:");

    CHECK(!helper.empty());
    CHECK(helper.find("PlanPickerForegroundHandoff(")!=std::string::npos);
    CHECK(helper.find("SetForegroundWindow(prog)")!=std::string::npos);
    CHECK(helper.find("g_vdmi->SwitchDesktop(desktop.get())")!=
          std::string::npos);
    const size_t desktopAttach=helper.find(
        "desktopAttached=AttachThreadInput(");
    const size_t desktopAttachTrue=helper.find(
        "desktopThread,currentThread,TRUE",desktopAttach);
    const size_t foregroundAttach=helper.find(
        "foregroundAttached=AttachThreadInput(");
    const size_t foregroundAttachTrue=helper.find(
        "foregroundThread,currentThread,TRUE",foregroundAttach);
    const size_t focusShell=helper.find("SetForegroundWindow(prog)");
    const size_t foregroundDetach=helper.find(
        "foregroundThread,currentThread,FALSE)");
    const size_t desktopDetach=helper.find(
        "desktopThread,currentThread,FALSE)");
    const size_t invoked=helper.find("invoked=true;");
    const size_t switchCall=helper.find(
        "g_vdmi->SwitchDesktop(desktop.get())");
    CHECK(desktopAttach!=std::string::npos &&
          desktopAttachTrue!=std::string::npos &&
          foregroundAttach!=std::string::npos &&
          foregroundAttachTrue!=std::string::npos &&
          focusShell!=std::string::npos &&
          foregroundDetach!=std::string::npos &&
          desktopDetach!=std::string::npos && invoked!=std::string::npos &&
          switchCall!=std::string::npos);
    CHECK(desktopAttach<desktopAttachTrue && desktopAttachTrue<focusShell &&
          foregroundAttach<foregroundAttachTrue &&
          foregroundAttachTrue<focusShell && focusShell<foregroundDetach &&
          foregroundDetach<desktopDetach && desktopDetach<invoked &&
          invoked<switchCall);
    CHECK(effects.find("SwitchDesktopWithForegroundHandoff(")!=
          std::string::npos);
    CHECK(effects.find("g_vdmi->SwitchDesktop(")==std::string::npos);
    CHECK(ordinary.find("SwitchDesktopWithForegroundHandoff(")!=
          std::string::npos);
    CHECK(ordinary.find("g_vdmi->SwitchDesktop(")==std::string::npos);
    const size_t ctrlSnapshot=mouse.find(
        "const bool ctrl=PickerMouseControlHeld(wp);");
    const size_t dispatch=mouse.find("DispatchPickerPointerActivation(");
    CHECK(ctrlSnapshot!=std::string::npos && dispatch!=std::string::npos &&
          ctrlSnapshot<dispatch);
    CHECK(mouse.find("GetKeyState(VK_CONTROL)")==std::string::npos);
}
```

Register it with the other picker source tests.

- [ ] **Step 6: Run the suite and verify the wiring test is RED**

Run:

```powershell
cmd.exe /d /c .\build-test.bat
```

Expected: the test compiles but fails while controlled switching still calls
`g_vdmi->SwitchDesktop` directly, mouse Ctrl is re-read later, or attached input
queues remain connected across the COM switch call.

- [ ] **Step 7: Implement the shared foreground-handoff switch**

Define this before `ExecutePickerEffect` in `src/vde.cpp`:

```cpp
static HRESULT SwitchDesktopWithForegroundHandoff(
        const GUID& destinationGuid,bool& invoked) noexcept {
    invoked=false;
    if(GuidIsZero(destinationGuid) || !g_vdmi) return E_INVALIDARG;
    ScopedComPtr<IVirtualDesktop> desktop;
    try { desktop=GetDesktopByGuid(destinationGuid); }
    catch(...) { return E_FAIL; }
    if(!desktop) return E_INVALIDARG;

    HWND prog=FindWindowW(L"Progman",L"Program Manager");
    DWORD ignored=0;
    const DWORD desktopThread=prog
        ?GetWindowThreadProcessId(prog,&ignored):0;
    const DWORD foregroundThread=
        GetWindowThreadProcessId(GetForegroundWindow(),&ignored);
    const DWORD currentThread=GetCurrentThreadId();
    const PickerForegroundHandoffPlan plan=PlanPickerForegroundHandoff(
        prog!=nullptr,desktopThread,foregroundThread,currentThread);
    bool desktopAttached=false;
    bool foregroundAttached=false;
    if(plan.attachDesktop)
        desktopAttached=AttachThreadInput(
            desktopThread,currentThread,TRUE)!=FALSE;
    if(plan.attachForeground)
        foregroundAttached=AttachThreadInput(
            foregroundThread,currentThread,TRUE)!=FALSE;
    if(plan.focusShell) SetForegroundWindow(prog);
    if(foregroundAttached)
        AttachThreadInput(foregroundThread,currentThread,FALSE);
    if(desktopAttached)
        AttachThreadInput(desktopThread,currentThread,FALSE);

    HRESULT result=E_FAIL;
    invoked=true;
    try { result=g_vdmi->SwitchDesktop(desktop.get()); }
    catch(...) { result=E_FAIL; }

    if(prog) ShowWindow(prog,SW_MINIMIZE);
    return result;
}
```

Keep each successful `AttachThreadInput` only through
`SetForegroundWindow(prog)`, then detach foreground and desktop queues in reverse
order before setting `invoked` or entering the undocumented COM call. This
preserves the working ordinary-click sequence and avoids carrying cross-thread
input coupling through a desktop change.

In the `PickerEffectKind::SwitchDesktop` case, retain the existing target
identity and rollback gates, then replace the direct COM call with:

```cpp
if(PickerForwardSwitchInvocationAllowed(
        observation.identity,desktop && g_vdmi) ||
   (rollback && desktop && g_vdmi)){
    bool invoked=false;
    result=SwitchDesktopWithForegroundHandoff(effect.desktop,invoked);
    observation.apiInvoked=invoked;
}
observation.apiAccepted=SUCCEEDED(result);
```

The existing local `desktop` lookup remains useful for the invocation gate; the
shared helper performs a fresh exact GUID lookup at the actual invocation
boundary.

Replace `GoToDesktop`'s inline Progman block with:

```cpp
static void GoToDesktop(int idx){
    if(g_picker.controlledTransition()) return;
    if(idx<0 || idx>=static_cast<int>(g_tiles.size())) return;
    HidePicker();
    bool invoked=false;
    (void)SwitchDesktopWithForegroundHandoff(
        g_tiles[idx].guid,invoked);
}
```

- [ ] **Step 8: Snapshot Ctrl before pointer dispatch**

At the start of `WM_LBUTTONDOWN`, immediately after the point is decoded, add:

```cpp
const bool ctrl=PickerMouseControlHeld(wp);
```

Capture that local in the tile lambda and replace the late `GetKeyState` block
with:

```cpp
Activate(index,ctrl);
```

Keyboard paths retain their current `GetKeyState` behavior.

- [ ] **Step 9: Run tests and production build**

Run:

```powershell
cmd.exe /d /c .\build-test.bat
cmd.exe /d /c .\build.bat
```

Expected: all tests pass; production compilation ends with
`Built build\vde.exe`.

- [ ] **Step 10: Commit Task 2**

```powershell
git add -- src/picker_state.hpp src/vde.cpp tests/vdtest.cpp docs/superpowers/specs/2026-08-21-picker-window-routing-fixes-design.md docs/superpowers/plans/2026-08-21-picker-window-routing-fixes.md
git commit -m "fix(picker): restore reliable ctrl move"
```

### Task 3: Center the popup on the primary physical monitor

**Files:**

- Modify: `src/picker_state.hpp:45-80`
- Modify: `src/vde.cpp:7467-7485`
- Test: `tests/vdtest.cpp:123-170`
- Test: `tests/vdtest.cpp:11734-11740`
- Test registration: `tests/vdtest.cpp:18640-18650`

- [ ] **Step 1: Write the failing centering test**

Add beside the picker sizing tests:

```cpp
static void test_picker_centered_origin_uses_exact_work_area(){
    const POINT primary=PickerCenteredOrigin(
        RECT{0,40,1920,1040},SIZE{720,500});
    CHECK(primary.x==600);
    CHECK(primary.y==290);

    const POINT negative=PickerCenteredOrigin(
        RECT{-1920,-200,0,880},SIZE{800,400});
    CHECK(negative.x==-1360);
    CHECK(negative.y==140);

    const POINT oversized=PickerCenteredOrigin(
        RECT{0,0,640,480},SIZE{800,600});
    CHECK(oversized.x==-80);
    CHECK(oversized.y==-60);
}
```

Register it after the existing picker size/DPI tests.

- [ ] **Step 2: Run the suite and verify RED**

Run:

```powershell
cmd.exe /d /c .\build-test.bat
```

Expected: compilation fails because `PickerCenteredOrigin` does not exist.

- [ ] **Step 3: Add the pure centering helper**

Add after `PickerDesiredClientSize` in `src/picker_state.hpp`:

```cpp
inline POINT PickerCenteredOrigin(const RECT& workArea,
                                  const SIZE& outerSize) noexcept {
    const long long width=static_cast<long long>(workArea.right)-
        workArea.left;
    const long long height=static_cast<long long>(workArea.bottom)-
        workArea.top;
    POINT origin;
    origin.x=PickerSaturatingInt(static_cast<long long>(workArea.left)+
        (width-outerSize.cx)/2);
    origin.y=PickerSaturatingInt(static_cast<long long>(workArea.top)+
        (height-outerSize.cy)/2);
    return origin;
}
```

- [ ] **Step 4: Run the suite and verify the geometry helper is GREEN**

Run:

```powershell
cmd.exe /d /c .\build-test.bat
```

Expected: all tests pass.

- [ ] **Step 5: Write a failing source test for primary-only placement**

Add near the existing `ShowPicker` source assertions:

```cpp
static void test_picker_show_uses_primary_monitor_only(){
    const std::string source=ReadSourceFile(L"src\\vde.cpp");
    const std::string helper=SourceSection(
        source,"static bool GetPrimaryPickerWorkArea(",
        "static void ShowPicker(");
    const std::string show=SourceSection(
        source,"static void ShowPicker(","static void MoveSel(");
    CHECK(!helper.empty());
    CHECK(helper.find("MonitorFromPoint(")!=std::string::npos);
    CHECK(helper.find("MONITOR_DEFAULTTOPRIMARY")!=std::string::npos);
    CHECK(helper.find("GetMonitorInfoW(")!=std::string::npos);
    CHECK(helper.find("SPI_GETWORKAREA")!=std::string::npos);
    CHECK(show.find("GetPrimaryPickerWorkArea(")!=std::string::npos);
    CHECK(show.find("PickerCenteredOrigin(")!=std::string::npos);
    CHECK(show.find("MonitorFromWindow(")==std::string::npos);
    CHECK(show.find("g_target?g_target:g_main")==std::string::npos);
}
```

Register it with the other picker source tests.

- [ ] **Step 6: Run the suite and verify the source test is RED**

Run:

```powershell
cmd.exe /d /c .\build-test.bat
```

Expected: the new test fails because `ShowPicker` still uses
`MonitorFromWindow(g_target ? g_target : g_main, ...)`.

- [ ] **Step 7: Add primary work-area lookup and route ShowPicker through it**

Define immediately before `ShowPicker`:

```cpp
static bool GetPrimaryPickerWorkArea(RECT& workArea) noexcept {
    POINT origin={0,0};
    HMONITOR monitor=MonitorFromPoint(
        origin,MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info={sizeof(info)};
    if(monitor && GetMonitorInfoW(monitor,&info)){
        workArea=info.rcWork;
        return workArea.right>workArea.left &&
               workArea.bottom>workArea.top;
    }
    RECT fallback={0,0,0,0};
    if(SystemParametersInfoW(
            SPI_GETWORKAREA,0,&fallback,0) &&
       fallback.right>fallback.left &&
       fallback.bottom>fallback.top){
        workArea=fallback;
        return true;
    }
    fallback.right=GetSystemMetrics(SM_CXSCREEN);
    fallback.bottom=GetSystemMetrics(SM_CYSCREEN);
    if(fallback.right<=0 || fallback.bottom<=0) return false;
    workArea=fallback;
    return true;
}
```

Replace the monitor-selection and centering block in `ShowPicker` with:

```cpp
RECT workArea={0,0,0,0};
if(!GetPrimaryPickerWorkArea(workArea)) return;
RECT windowRect={0,0,sz.cx,sz.cy};
AdjustWindowRectEx(
    &windowRect,WS_POPUP,FALSE,WS_EX_TOOLWINDOW|WS_EX_TOPMOST);
const SIZE outer={
    windowRect.right-windowRect.left,
    windowRect.bottom-windowRect.top
};
const POINT origin=PickerCenteredOrigin(workArea,outer);
SetWindowPos(g_main,HWND_TOPMOST,origin.x,origin.y,
             outer.cx,outer.cy,SWP_NOACTIVATE);
```

Do not use the target window or cursor for physical-monitor selection.

- [ ] **Step 8: Run tests and production build**

Run:

```powershell
cmd.exe /d /c .\build-test.bat
cmd.exe /d /c .\build.bat
```

Expected: all tests pass; production compilation ends with
`Built build\vde.exe`.

- [ ] **Step 9: Commit Task 3**

```powershell
git add -- src/picker_state.hpp src/vde.cpp tests/vdtest.cpp
git commit -m "fix(picker): pin popup to primary monitor"
```

### Task 4: Cross-cutting regression and completion verification

**Files:**

- Verify: `src/picker_state.hpp`
- Verify: `src/vde.cpp`
- Verify: `tests/vdtest.cpp`
- Verify: `README.md:58-80`
- Verify: `docs/superpowers/specs/2026-08-21-picker-window-routing-fixes-design.md`

- [ ] **Step 1: Verify the browser-only persistence boundary**

Run:

```powershell
rg -n "EnumFastWindow|ClassifyBrowserCandidate|RunPickerPersistenceTransaction|test_ctrl_move_non_browser_never_mutates_auto_layout" src tests
```

Expected: browser classification remains in the fast snapshot/persistence path;
`EnumAll` contains no profile/classifier call; the non-browser no-mutation test
remains registered.

- [ ] **Step 2: Run whitespace and repository checks**

Run:

```powershell
git diff --check
git status --short
```

Expected: `git diff --check` prints nothing; status lists only the intended
source/test modifications before their task commits, or is clean after them.

- [ ] **Step 3: Run the complete verification build**

Run both repository build scripts from the isolated worktree:

```powershell
cmd.exe /d /c .\build-test.bat
cmd.exe /d /c .\build.bat
```

Expected: the full suite reports all tests passed and the production build
reports `Built build\vde.exe` with no compiler errors.

- [ ] **Step 4: Inspect the final diff and commit graph**

Run:

```powershell
git diff main...HEAD -- src/picker_state.hpp src/vde.cpp tests/vdtest.cpp docs/superpowers/specs/2026-08-21-picker-window-routing-fixes-design.md docs/superpowers/plans/2026-08-21-picker-window-routing-fixes.md
git log --oneline --decorate main..HEAD
git status --short --branch
```

Expected: focused task commits (including any review follow-ups), no
persistence-scope expansion, and a clean worktree.

- [ ] **Step 5: Record manual Windows QA as required handoff evidence**

On an interactive Windows desktop:

1. Put a non-browser app and a browser on different virtual desktops.
2. Put the active app on a secondary physical monitor.
3. Open the picker and confirm it appears centered on the primary monitor.
4. Confirm ordinary non-browser Alt+Tab windows appear in their desktop tiles.
5. Ctrl+Click another desktop and confirm the captured active window moves, the
   desktop switches, and the picker stays open.
6. Restart the picker and confirm the automatic layout still contains only
   enabled Firefox/Chrome/Edge windows.

If interactive QA is unavailable in the agent environment, report it explicitly
instead of claiming it passed.

No additional commit is needed when this task makes no file changes.
