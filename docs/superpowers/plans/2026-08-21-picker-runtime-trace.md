# Picker Runtime Trace Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an opt-in, privacy-bounded `--trace-picker` mode that identifies the exact runtime boundary behind missing non-Firefox rows and Ctrl+Click no-ops without changing picker behavior.

**Architecture:** Add a focused `picker_trace` module with a typed event schema, JSONL writer, bounded fail-open storage, retention, and executable provenance. `src/vde.cpp` supplies already-observed runtime facts through narrow typed calls; it keeps the existing enumeration, move, reducer, retry, rollback, and persistence decisions. Pure routing and injected I/O tests establish trace/no-trace equivalence, while one final interactive reproduction remains mandatory because unit tests cannot observe the user's Windows window station.

**Tech Stack:** C++14, Win32, COM HRESULTs, BCrypt SHA-256/randomness, UTF-8 JSON Lines, the existing CHECK-based `vdtest` harness, MSVC x64 batch builds.

---

## Prerequisites and invariants

- Start from local `main` at or after `714ac6d`.
- Read the approved design at
  `docs/superpowers/specs/2026-08-21-picker-runtime-trace-design.md`.
- Baseline verification is `9571/9571 passed` and `Built build\vde.exe`.
- Do not change Alt+Tab eligibility, identity safety, transition ordering,
  retry budgets, rollback, persistence, or primary-monitor placement in this
  plan.
- Do not add a generic `emit(string, map<string,string>)` API. Trace input is
  typed; the only live foreign-process strings are bounded class name and image
  basename values.
- Source-substring tests are supporting wiring checks only. Behavioral and
  injected-operation tests are the acceptance evidence.
- Tasks 1-9 follow RED, GREEN, a full relevant regression run, then one
  focused commit. Task 10 is an intentionally GREEN final audit/review slice;
  do not manufacture a failure after all runtime anchors are already wired.
  Do not combine task commits.

## File map

- Create `src/picker_trace.hpp`: public fixed enums, typed event records,
  privacy-safe string wrappers, injected operation interfaces, writer/session
  declarations, and pure decisions used by tests.
- Create `src/picker_trace.cpp`: enum serialization, JSON escaping, writer,
  caps, Win32 storage, retention, BCrypt hashing/randomness, PE provenance, and
  session lifecycle.
- Modify `src/vde.cpp`: launch wiring and passive runtime instrumentation only.
- Modify `src/lifecycle.hpp`: observer overloads for the existing desktop
  snapshot/lookup templates; four-argument behavior remains unchanged.
- Modify `tests/vdtest.cpp`: all trace unit, injected-I/O, equivalence, and
  supporting wiring tests.
- Modify `build.bat`: compile/link `src\picker_trace.cpp` into `vde.exe`.
- Modify `build-test.bat`: compile/link `src\picker_trace.cpp` into
  `vdtest.exe`.
- Modify `README.md`: document the one-shot diagnostic command, local file
  path, privacy boundary, and current-instance requirement.

## Fixed public vocabulary

Define this vocabulary once in `src/picker_trace.hpp`; later tasks must use
these exact names rather than inventing parallel strings or enums:

```cpp
enum class PickerTraceOpenResult : uint8_t {
    Shown, Degraded, ControlledTransition, WorkAreaUnavailable,
    ModelUnavailable, AdjustRectFailed, OuterSizeInvalid,
    PositionFailed, ClientRectFailed, PaintCacheFailed
};

enum class PickerTraceDesktopSnapshotStatus : uint8_t {
    NotAttempted, Complete, DesktopServiceMissing,
    GetDesktopsFailed, GetCountFailed, InvalidCount, GetAtFailed,
    GetIdFailed, InvalidGuid, AllocationFailure, Exception
};

enum class PickerTraceAltTabReason : uint8_t {
    Eligible, NotVisible, FirstTitleUnavailable,
    ToolWindow, RootOwnerMismatch
};

enum class PickerTraceEnumDecision : uint8_t {
    SkipNotVisible, SkipFirstTitleUnavailable, SkipToolWindow,
    SkipRootOwnerMismatch, SkipDesktopServiceMissing,
    SkipDesktopLookupFailed, SkipDesktopGuidZero,
    SkipDesktopTileMissing, SkipSecondTitleUnavailable,
    SkipSecondTitleReadFailed, SkipIdentityLost,
    DisplayOnlyPidUnavailable, DisplayOnlyProcessStartUnavailable,
    DisplayOnlyIdentityIndeterminate, Verified,
    AllocationFailure, GlobalSnapshotFailure, Count
};

enum class PickerTraceActivationSource : uint8_t {
    Mouse, Keyboard
};

enum class PickerTraceActivationResult : uint8_t {
    AlreadyControlled, InvalidTile, SelectionPublicationFailed,
    RoutedPlainSwitch, DispatchedMoveEntry
};

enum class PickerTraceMoveBeginReason : uint8_t {
    Accepted, AlreadyControlled, InvalidIndex, SelectionIndexMismatch,
    SelectionDesktopMismatch, MainWindowMissing, DesktopManagerMissing,
    DesktopDocumentMissing, TargetMismatch, TargetWindowMissing,
    TargetWindowInvalid, DestinationZero, DestinationLookupFailed,
    CurrentDesktopUnavailable, PopupDesktopUnavailable, FastCaptureFailed,
    TargetDesktopUnavailable, IdentityMismatch, IdentityLost,
    IdentityIndeterminate, AcceptedPlanConflict, BoundRecordConflict,
    SafeOriginUnavailable, AcceptedOperationMissing,
    OperationClaimStageFailed, ReservationHandoffFailed,
    PendingAssociationStageFailed, ProvisionalInsertFailed,
    NoInitialEffect
};

enum class PickerTraceEffectStage : uint8_t {
    Queue, Execute, Observation, Reduce
};

enum class PickerTraceApiKind : uint8_t {
    GetViewForHwnd, MoveViewToDesktop, MoveWindowToDesktop,
    GetWindowDesktopIdTarget, GetWindowDesktopIdPopup,
    GetWindowDesktopIdCapture, GetDesktops, GetCount, GetAt, GetId,
    GetCurrentDesktop, AttachDesktopInput, AttachForegroundInput,
    SetForegroundWindow, DetachForegroundInput, DetachDesktopInput,
    SwitchDesktop, ShowWindowProgmanCleanup
};

enum class PickerTraceDesktopLookupStage : uint8_t {
    ValidateRequest, GetDesktops, GetCount, GetAt, GetId,
    Match, NotFound, Exception
};

enum class PickerTraceDesktopLookupUse : uint8_t {
    MoveEntryDestination, MoveTargetDestination, MovePopupDestination,
    SwitchPrecheckDestination, SwitchHandoffDestination
};

enum class PickerTraceRawResultKind : uint8_t {
    HResult, Win32Bool, PreviousVisibility, NoExtendedError
};

enum class PickerTraceDeliveryRoute : uint8_t {
    None, Posted, TimerArmed, InlineFallback, DelayedTimer,
    DurableExternalKick, ShutdownDrain
};

enum class PickerTraceTerminalizationReason : uint8_t {
    Completed, TerminalNotAcknowledged, PendingEffect,
    ReservationReleaseException, ReservationNotReleased,
    RuntimeKeyMissing, RuntimeNotReady, FinalizeStateFailed
};

enum class PickerTraceTerminalOutcome : uint8_t {
    Succeeded, Cancelled, Failed
};

enum class PickerTraceRollbackTrigger : uint8_t {
    None, Cancellation, TargetMove, TargetVerify,
    PopupMove, PopupVerify, DesktopSwitch,
    IdentityLost, IdentityIndeterminate, ReadUnavailable,
    RetryBudgetExhausted, QueueConflict, Exception
};

enum class PickerTraceDiagnosticCode : uint8_t {
    None, ApiRejected, VerificationMismatch, IdentityLost,
    IdentityIndeterminate, ReadUnavailable, RetryBudgetExhausted,
    QueueConflict, Exception, Cancelled
};

enum class PickerTraceReservationExceptionStage : uint8_t {
    None, FirstDecision, CheckpointCallback, Refind,
    SecondDecision, Erase
};
```

## Task 1: Add the module boundary and exact launch routing

**Files:**

- Create: `src/picker_trace.hpp`
- Create: `src/picker_trace.cpp`
- Modify: `tests/vdtest.cpp:1-35` and the test-registration block near EOF
- Modify: `src/vde.cpp:8616-8622`
- Modify: `build.bat:5-7`
- Modify: `build-test.bat:5-7`

- [ ] **Step 1: Write failing launch-routing tests**

Add the new include and tests:

```cpp
#include "picker_trace.hpp"

static void test_picker_trace_launch_requires_exact_opt_in_flag(){
    const wchar_t* exact[]={L"vde.exe",L"--trace-picker"};
    const wchar_t* none[]={L"vde.exe"};
    const wchar_t* cli[]={L"vde.exe",L"status"};
    const wchar_t* misspelled[]={L"vde.exe",L"--trace-pick"};
    const wchar_t* extra[]={L"vde.exe",L"--trace-picker",L"extra"};
    CHECK(ParseVdeLaunchOptions(2,exact).tracePicker);
    CHECK(!ParseVdeLaunchOptions(1,none).tracePicker);
    CHECK(!ParseVdeLaunchOptions(2,cli).tracePicker);
    CHECK(!ParseVdeLaunchOptions(2,misspelled).tracePicker);
    CHECK(!ParseVdeLaunchOptions(3,extra).tracePicker);
}

static void test_picker_trace_launch_preserves_cli_routing(){
    const wchar_t* status[]={L"vde.exe",L"status"};
    const wchar_t* restore[]={L"vde.exe",L"restore-auto"};
    const wchar_t* traced[]={L"vde.exe",L"--trace-picker"};
    const wchar_t* unknown[]={L"vde.exe",L"unknown"};
    CHECK(ParseVdeLaunchOptions(2,status).cli);
    CHECK(ParseVdeLaunchOptions(2,restore).cli);
    CHECK(!ParseVdeLaunchOptions(2,traced).cli);
    CHECK(!ParseVdeLaunchOptions(2,unknown).cli);
    CHECK(ParseVdeLaunchOptions(2,status).command==L"status");
}
```

Register both functions in `main()` with the picker tests.

- [ ] **Step 2: Run the suite and verify RED**

Run:

```powershell
.\build-test.bat
```

Expected: non-zero exit with `cannot open include file 'picker_trace.hpp'`.

- [ ] **Step 3: Add the minimal launch API**

Create `src/picker_trace.hpp` with a self-contained guard and declaration:

```cpp
#pragma once
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "picker_state.hpp"

struct VdeLaunchOptions {
    std::wstring command;
    bool cli=false;
    bool tracePicker=false;
};

VdeLaunchOptions ParseVdeLaunchOptions(
    int argc,const wchar_t* const* argv) noexcept;
```

Create `src/picker_trace.cpp`:

```cpp
#include "picker_trace.hpp"

static bool PickerTraceIsCliCommand(const std::wstring& command) noexcept {
    return command==L"save" || command==L"restore" ||
        command==L"restore-auto" || command==L"status" ||
        command==L"list" || command==L"-h" ||
        command==L"--help" || command==L"/?";
}

VdeLaunchOptions ParseVdeLaunchOptions(
        int argc,const wchar_t* const* argv) noexcept {
    VdeLaunchOptions result;
    try {
        if(argc>=2 && argv && argv[1]) result.command=argv[1];
        result.cli=PickerTraceIsCliCommand(result.command);
        result.tracePicker=argc==2 && argv && argv[1] &&
            result.command==L"--trace-picker";
    } catch(...) {
        result=VdeLaunchOptions{};
    }
    return result;
}
```

Change both build commands so the second source is compiled:

```bat
cl /nologo /utf-8 /EHsc /W3 /std:c++14 src\vde.cpp src\picker_trace.cpp build\vde.res /Fe:build\vde.exe /Fo:build\ || exit /b 1
```

```bat
cl /nologo /utf-8 /EHsc /W3 /std:c++14 /I src tests\vdtest.cpp src\picker_trace.cpp /Fe:build\vdtest.exe /Fo:build\ || exit /b 1
```

- [ ] **Step 4: Route `wWinMain` through the parser without enabling I/O**

Replace the ad-hoc first-argument parsing with:

```cpp
int argc=0;
LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
const VdeLaunchOptions launch=ParseVdeLaunchOptions(
    argc,argv);
if(argv) LocalFree(argv);
const std::wstring cmd=launch.command;
const bool cli=launch.cli;
const bool tracePicker=launch.tracePicker;
```

Add `(void)tracePicker;` until Task 5 wires the session. Do not change
`SetRunAtLogon`; it must continue writing only the quoted executable path.

- [ ] **Step 5: Run GREEN and the production build**

Run:

```powershell
.\build-test.bat
.\build.bat
```

Expected: test exit code `0` with matching passed/total counts, followed by
`Built build\vde.exe`.

- [ ] **Step 6: Commit**

```powershell
git add src/picker_trace.hpp src/picker_trace.cpp src/vde.cpp tests/vdtest.cpp build.bat build-test.bat
git commit -m "feat(trace): add opt-in launch mode"
```

## Task 2: Define the typed privacy-safe schema and JSON serialization

**Files:**

- Modify: `src/picker_trace.hpp`
- Modify: `src/picker_trace.cpp`
- Modify: `tests/vdtest.cpp`

- [ ] **Step 1: Add failing schema, escaping, and privacy tests**

Add these test groups and register them:

```cpp
static void test_picker_trace_safe_foreign_names_are_bounded(){
    const PickerTraceSafeClassName cls=
        MakePickerTraceSafeClassName(L"Class\"\nName",11);
    const PickerTraceSafeImageBasename image=
        MakePickerTraceSafeImageBasename(L"demo.exe",8);
    const PickerTraceSafeImageBasename path=
        MakePickerTraceSafeImageBasename(L"C:\\Apps\\demo.exe",16);
    CHECK(cls.available());
    CHECK(cls.length()<=128);
    CHECK(image.available());
    CHECK(!path.available());
}

static void test_picker_trace_json_line_escapes_only_allowlisted_text(){
    PickerTraceEnvelope envelope;
    envelope.session.fill(0x11);
    envelope.seq=7;
    envelope.ms=25;
    PickerTraceEnumWindowEvent event;
    event.enumSequence=3;
    event.className=MakePickerTraceSafeClassName(L"A\"B\n",4);
    event.imageBasename=MakePickerTraceSafeImageBasename(L"tool.exe",8);
    event.decision=PickerTraceEnumDecision::Verified;
    std::string line;
    CHECK(SerializePickerTraceLine(envelope,event,line));
    CHECK(line.find("\"seq\":7")!=std::string::npos);
    CHECK(line.find("A\\\"B\\u000a")!=std::string::npos);
    CHECK(line.find("tool.exe")!=std::string::npos);
    CHECK(line.find("title_text")==std::string::npos);
    CHECK(line.find("search_text")==std::string::npos);
    CHECK(line.find("diagnostic_text")==std::string::npos);
    CHECK(!line.empty() && line.back()=='\n');
}

static void test_picker_trace_json_line_has_exact_canonical_shape(){
    PickerTraceEnvelope envelope;
    envelope.session.fill(0x11);
    envelope.seq=7;
    envelope.ms=25;
    PickerTraceActivationResultEvent event;
    event.activationId=9;
    event.result=PickerTraceActivationResult::AlreadyControlled;
    std::string line;
    CHECK(SerializePickerTraceLine(envelope,event,line));
    CHECK(line==
        "{\"schema\":1,\"session\":\"11111111111111111111111111111111\","
        "\"seq\":7,\"ms\":25,\"event\":\"activation.result\","
        "\"activation_id\":9,\"result\":\"already_controlled\"}\n");
}

static void test_picker_trace_enums_have_stable_names(){
    CHECK(std::string(PickerTraceEnumDecisionName(
        PickerTraceEnumDecision::SkipRootOwnerMismatch))==
        "skip_root_owner_mismatch");
    CHECK(std::string(PickerTraceMoveBeginReasonName(
        PickerTraceMoveBeginReason::PopupDesktopUnavailable))==
        "popup_desktop_unavailable");
    CHECK(std::string(PickerTraceApiKindName(
        PickerTraceApiKind::SetForegroundWindow))==
        "set_foreground_window");
}
```

Add a test-only strict JSON object parser that is independent of
`PickerTraceJsonObject`. It must consume exactly one object plus one LF, reject
missing braces/commas, invalid escapes, duplicate keys, trailing bytes, and
non-UTF-8 input. Run one instance of every event serializer through it; the
exact canonical assertion above additionally locks field order and spelling.

- [ ] **Step 2: Run RED**

Run `.\build-test.bat`.

Expected: non-zero compile exit naming `PickerTraceSafeClassName` and the new
event types.

- [ ] **Step 3: Add the fixed schema types**

Add the vocabulary from this plan and these fixed wrappers/records to
`picker_trace.hpp`:

```cpp
class PickerTraceSafeClassName {
public:
    PickerTraceSafeClassName() noexcept=default;
    bool available() const noexcept { return available_; }
    uint16_t length() const noexcept { return length_; }
    const wchar_t* data() const noexcept { return value_.data(); }
private:
    friend PickerTraceSafeClassName MakePickerTraceSafeClassName(
        const wchar_t*,int) noexcept;
    std::array<wchar_t,129> value_{};
    uint16_t length_=0;
    bool available_=false;
};

class PickerTraceSafeImageBasename {
public:
    PickerTraceSafeImageBasename() noexcept=default;
    bool available() const noexcept { return available_; }
    uint16_t length() const noexcept { return length_; }
    const wchar_t* data() const noexcept { return value_.data(); }
private:
    friend PickerTraceSafeImageBasename MakePickerTraceSafeImageBasename(
        const wchar_t*,int) noexcept;
    std::array<wchar_t,261> value_{};
    uint16_t length_=0;
    bool available_=false;
};

PickerTraceSafeClassName MakePickerTraceSafeClassName(
    const wchar_t* value,int length) noexcept;
PickerTraceSafeImageBasename MakePickerTraceSafeImageBasename(
    const wchar_t* value,int length) noexcept;

struct PickerTraceEnvelope {
    std::array<unsigned char,16> session{};
    uint64_t seq=0;
    uint64_t ms=0;
};

struct PickerTraceCaptureEvent {
    uintptr_t hwnd=0;
    DWORD pid=0;
    DWORD tid=0;
    int titleLength=0;
    bool windowValid=false;
    bool titleRead=false;
    bool identityComplete=false;
    WindowIdentityRecapture recapture=WindowIdentityRecapture::Indeterminate;
    bool targetPublished=false;
};

struct PickerTraceOpenEvent {
    PickerTraceOpenResult result=PickerTraceOpenResult::ModelUnavailable;
    PickerTraceDesktopSnapshotStatus desktopSnapshot=
        PickerTraceDesktopSnapshotStatus::NotAttempted;
    HRESULT desktopSnapshotResult=E_NOTIMPL;
    int desktopSnapshotIndex=-1;
    uint32_t desktopSnapshotCount=0;
    GUID currentDesktop{};
    uint64_t modelGeneration=0;
    bool currentDesktopAvailable=false;
    bool targetIdentityPresent=false;
};

struct PickerTraceEnumBeginEvent {
    uint64_t modelGeneration=0;
    std::vector<GUID> desktops;
};

struct PickerTraceEnumWindowEvent {
    uint64_t modelGeneration=0;
    uint64_t enumSequence=0;
    uintptr_t hwnd=0;
    uintptr_t owner=0;
    uintptr_t rootOwner=0;
    uintptr_t lastActivePopup=0;
    DWORD pid=0;
    DWORD tid=0;
    PickerTraceSafeClassName className;
    PickerTraceSafeImageBasename imageBasename;
    bool visibleObserved=false;
    bool visible=false;
    bool firstTitleObserved=false;
    int firstTitleLength=0;
    DWORD firstTitleError=ERROR_SUCCESS;
    bool exStyleObserved=false;
    uint64_t exStyle=0;
    DWORD exStyleError=ERROR_SUCCESS;
    bool toolWindow=false;
    bool rootOwnerObserved=false;
    bool rootOwnerSelf=false;
    bool cloakedObserved=false;
    DWORD cloaked=0;
    HRESULT cloakedResult=E_NOTIMPL;
    PickerTraceAltTabReason altTabReason=
        PickerTraceAltTabReason::FirstTitleUnavailable;
    HRESULT desktopResult=E_NOTIMPL;
    GUID desktop{};
    int tileIndex=-1;
    bool secondTitleObserved=false;
    int secondTitleLength=0;
    int secondTitleCopied=0;
    DWORD secondTitleError=ERROR_SUCCESS;
    bool processStartCacheHit=false;
    bool processStartAvailable=false;
    DWORD processStartError=ERROR_SUCCESS;
    bool identityComplete=false;
    WindowIdentityRecapture recapture=WindowIdentityRecapture::Indeterminate;
    PickerTraceEnumDecision decision=
        PickerTraceEnumDecision::GlobalSnapshotFailure;
};

struct PickerTraceEnumEndEvent {
    uint64_t modelGeneration=0;
    uint64_t candidates=0;
    std::array<uint64_t,static_cast<size_t>(
        PickerTraceEnumDecision::Count)> counts{};
    bool enumWindowsReturned=false;
    DWORD enumWindowsError=ERROR_SUCCESS;
    bool modelPublished=false;
};

struct PickerTraceMouseDownEvent {
    uint64_t rawWparam=0;
    int x=0;
    int y=0;
    bool ctrl=false;
    bool controlled=false;
    bool searchActive=false;
    PickerPointerTarget target=PickerPointerTarget::None;
    int tileIndex=-1;
};

struct PickerTraceActivationRequestEvent {
    uint64_t activationId=0;
    PickerTraceActivationSource source=PickerTraceActivationSource::Mouse;
    bool ctrl=false;
    int tileIndex=-1;
};

struct PickerTraceActivationResultEvent {
    uint64_t activationId=0;
    PickerTraceActivationResult result=
        PickerTraceActivationResult::InvalidTile;
};

struct PickerTraceMoveBeginEvent {
    uint64_t activationId=0;
    uint64_t generation=0;
    int tileIndex=-1;
    PickerTraceMoveBeginReason reason=
        PickerTraceMoveBeginReason::InvalidIndex;
    GUID targetOrigin{};
    GUID popupOrigin{};
    GUID currentOrigin{};
    GUID destination{};
    PickerEffectKind firstEffect=PickerEffectKind::None;
};

struct PickerTraceMoveBeginExceptionEvent {
    uint64_t activationId=0;
    bool transitionPublished=false;
};

struct PickerTraceEffectEvent {
    PickerTraceEffectStage stage=PickerTraceEffectStage::Queue;
    uint64_t generation=0;
    uint64_t effectSerial=0;
    PickerEvent observationEvent=PickerEvent::Timer;
    PickerEffectKind effect=PickerEffectKind::None;
    PickerEffectKind nextEffect=PickerEffectKind::None;
    PickerPhase phaseBefore=PickerPhase::Idle;
    PickerPhase phaseAfter=PickerPhase::Idle;
    PickerIdentityValidity identity=PickerIdentityValidity::Unknown;
    PickerReadValidity targetRead=PickerReadValidity::Unknown;
    PickerReadValidity popupRead=PickerReadValidity::Unknown;
    PickerReadValidity currentRead=PickerReadValidity::Unknown;
    PickerEffectExecutionRoute executionRoute=
        PickerEffectExecutionRoute::Execute;
    PickerTraceDeliveryRoute delivery=PickerTraceDeliveryRoute::None;
    uint32_t deliveryAttempt=0;
    bool executionRouteAvailable=false;
    bool deliveryAvailable=false;
    bool apiInvoked=false;
    bool apiAccepted=false;
};

struct PickerTraceApiResultEvent {
    PickerTraceApiKind api=PickerTraceApiKind::GetDesktops;
    PickerTraceDesktopLookupUse lookupUse=
        PickerTraceDesktopLookupUse::MoveEntryDestination;
    PickerTraceDesktopLookupStage lookupStage=
        PickerTraceDesktopLookupStage::ValidateRequest;
    PickerTraceRawResultKind resultKind=PickerTraceRawResultKind::HResult;
    uint64_t generation=0;
    uint64_t effectSerial=0;
    uintptr_t hwnd=0;
    DWORD sourceThread=0;
    DWORD destinationThread=0;
    int index=-1;
    GUID requestedDesktop{};
    GUID actualDesktop{};
    HRESULT hresult=E_NOTIMPL;
    bool boolResult=false;
    bool invoked=false;
    bool lastErrorAvailable=false;
    DWORD lastError=ERROR_SUCCESS;
};

struct PickerTraceTerminalizationAttemptEvent {
    uint64_t generation=0;
    uint64_t attempt=0;
    PickerTraceTerminalizationReason reason=
        PickerTraceTerminalizationReason::RuntimeNotReady;
    PickerTraceDeliveryRoute incomingDelivery=
        PickerTraceDeliveryRoute::None;
    PickerTraceDeliveryRoute retryDelivery=
        PickerTraceDeliveryRoute::None;
    bool incomingDeliveryAvailable=false;
    bool retryDeliveryAvailable=false;
    bool terminalAcknowledged=false;
    bool pendingEffectNone=false;
    bool runtimeKeyPresent=false;
    PickerTerminalGuardReleaseAction firstReleaseAction=
        PickerTerminalGuardReleaseAction::ResolvedAbsent;
    PickerTerminalGuardReleaseAction retryReleaseAction=
        PickerTerminalGuardReleaseAction::ResolvedAbsent;
    PickerTraceReservationExceptionStage releaseExceptionStage=
        PickerTraceReservationExceptionStage::None;
    bool releaseAttempted=false;
    bool firstReleaseActionAvailable=false;
    bool retryReleaseActionAvailable=false;
    bool releaseExceptionStageAvailable=false;
    bool releaseRetried=false;
    bool releaseThrew=false;
    bool reservationReleased=false;
    bool ready=false;
    bool finalized=false;
};

struct PickerTraceTransitionTerminalEvent {
    uint64_t generation=0;
    PickerTraceTerminalOutcome outcome=
        PickerTraceTerminalOutcome::Failed;
    PickerTraceRollbackTrigger rollbackTrigger=
        PickerTraceRollbackTrigger::None;
    PickerTraceDiagnosticCode diagnosticCode=
        PickerTraceDiagnosticCode::None;
    int forwardTargetAttempts=0;
    int forwardPopupAttempts=0;
    int forwardSwitchAttempts=0;
    int rollbackTargetAttempts=0;
    int rollbackPopupAttempts=0;
    int rollbackSwitchAttempts=0;
    int focusAttempts=0;
    PickerReadValidity targetRead=PickerReadValidity::Unknown;
    PickerReadValidity popupRead=PickerReadValidity::Unknown;
    PickerReadValidity currentRead=PickerReadValidity::Unknown;
    GUID targetDesktop{};
    GUID popupDesktop{};
    GUID currentDesktop{};
};
```

Declare one `SerializePickerTraceLine` overload per event record and these
fixed converters:

```cpp
const char* PickerTraceOpenResultName(PickerTraceOpenResult) noexcept;
const char* PickerTraceDesktopSnapshotStatusName(
    PickerTraceDesktopSnapshotStatus) noexcept;
const char* PickerTraceAltTabReasonName(PickerTraceAltTabReason) noexcept;
const char* PickerTraceEnumDecisionName(PickerTraceEnumDecision) noexcept;
const char* PickerTraceActivationSourceName(
    PickerTraceActivationSource) noexcept;
const char* PickerTraceActivationResultName(
    PickerTraceActivationResult) noexcept;
const char* PickerTraceMoveBeginReasonName(
    PickerTraceMoveBeginReason) noexcept;
const char* PickerTraceEffectStageName(PickerTraceEffectStage) noexcept;
const char* PickerTraceApiKindName(PickerTraceApiKind) noexcept;
const char* PickerTraceDesktopLookupStageName(
    PickerTraceDesktopLookupStage) noexcept;
const char* PickerTraceDesktopLookupUseName(
    PickerTraceDesktopLookupUse) noexcept;
const char* PickerTraceRawResultKindName(
    PickerTraceRawResultKind) noexcept;
const char* PickerTraceDeliveryRouteName(
    PickerTraceDeliveryRoute) noexcept;
const char* PickerTraceTerminalizationReasonName(
    PickerTraceTerminalizationReason) noexcept;
const char* PickerTraceTerminalOutcomeName(
    PickerTraceTerminalOutcome) noexcept;
const char* PickerTraceRollbackTriggerName(
    PickerTraceRollbackTrigger) noexcept;
const char* PickerTraceDiagnosticCodeName(
    PickerTraceDiagnosticCode) noexcept;
const char* PickerTraceReservationExceptionStageName(
    PickerTraceReservationExceptionStage) noexcept;
const char* PickerTraceTerminalGuardReleaseActionName(
    PickerTerminalGuardReleaseAction) noexcept;
const char* PickerTracePhaseName(PickerPhase) noexcept;
const char* PickerTraceEventName(PickerEvent) noexcept;
const char* PickerTraceEffectKindName(PickerEffectKind) noexcept;
const char* PickerTraceEffectExecutionRouteName(
    PickerEffectExecutionRoute) noexcept;
const char* PickerTraceIdentityValidityName(
    PickerIdentityValidity) noexcept;
const char* PickerTraceReadValidityName(PickerReadValidity) noexcept;
const char* PickerTraceRecaptureName(WindowIdentityRecapture) noexcept;
const char* PickerTracePointerTargetName(PickerPointerTarget) noexcept;
```

- [ ] **Step 4: Implement deterministic JSON without a generic public builder**

In `picker_trace.cpp`, keep the JSON object builder private. It must:

```cpp
class PickerTraceJsonObject {
public:
    PickerTraceJsonObject() { bytes_="{"; }
    bool string(const char* key,const std::string& value) noexcept;
    bool boolean(const char* key,bool value) noexcept;
    bool unsignedNumber(const char* key,uint64_t value) noexcept;
    bool signedNumber(const char* key,int64_t value) noexcept;
    bool hex32(const char* key,uint32_t value) noexcept;
    bool hex64(const char* key,uint64_t value) noexcept;
    bool guid(const char* key,const GUID& value) noexcept;
    bool finish(std::string& output) noexcept;
private:
    bool key(const char* value) noexcept;
    static bool appendEscaped(const std::string& value,
                              std::string& output) noexcept;
    std::string bytes_;
    bool first_=true;
};
```

Each public overload writes common `schema`, `session`, `seq`, `ms`, and a fixed
event name, then only fields present in its typed record. Encode the 128-bit
session as exactly 32 lowercase hexadecimal digits. Encode HWND, styles,
HRESULT, and Win32 errors with `hex32`/`hex64`; GUIDs use canonical brace-free
lowercase text. Convert allowed wide strings with this private helper after
their constructors replace unpaired surrogates with U+FFFD; retain C0 controls
so the JSON writer deterministically encodes them as `\u00xx`:

```cpp
static bool PickerTraceWideToUtf8(
        const wchar_t* value,int length,std::string& output) noexcept {
    try {
        output.clear();
        if(!value || length<0) return false;
        if(length==0) return true;
        const int required=WideCharToMultiByte(
            CP_UTF8,WC_ERR_INVALID_CHARS,value,length,
            nullptr,0,nullptr,nullptr);
        if(required<=0) return false;
        std::string converted(static_cast<size_t>(required),'\0');
        const int written=WideCharToMultiByte(
            CP_UTF8,WC_ERR_INVALID_CHARS,value,length,
            &converted[0],required,nullptr,nullptr);
        if(written!=required) return false;
        output.swap(converted);
        return true;
    } catch(...) {
        output.clear();
        return false;
    }
}
```

The enum-window serializer writes title lengths/status only. There is no
serializer parameter for title, search, URL, layout, record ID, runtime key,
application diagnostic, or full foreign image path.

- [ ] **Step 5: Run GREEN and commit**

Run `.\build-test.bat`; expect exit `0` and matching passed/total counts.

```powershell
git add src/picker_trace.hpp src/picker_trace.cpp tests/vdtest.cpp
git commit -m "feat(trace): add typed JSON schema"
```

## Task 3: Add the bounded fail-open writer and no-op behavior

**Files:**

- Modify: `src/picker_trace.hpp`
- Modify: `src/picker_trace.cpp`
- Modify: `tests/vdtest.cpp`

- [ ] **Step 1: Write failing writer tests**

Use an injected memory sink and clock:

```cpp
static void test_picker_trace_writer_sequences_and_clamps_clock(){
    std::string bytes;
    std::vector<uint64_t> clock={100,105,103,110};
    size_t tick=0;
    PickerTraceSinkOps ops;
    ops.write=[&](const void* data,size_t size)->size_t{
        bytes.append((const char*)data,size);
        return size;
    };
    ops.flush=[](){ return true; };
    ops.close=[](){ return true; };
    ops.monotonicMs=[&](){ return clock[tick++]; };
    PickerTraceWriter writer(ops,PickerTraceLimits{4096,8},
                             std::array<unsigned char,16>{});
    PickerTraceOpenEvent event;
    writer.emit(event);
    writer.emit(event);
    writer.emit(event);
    CHECK(bytes.find("\"seq\":1")!=std::string::npos);
    CHECK(bytes.find("\"seq\":2")!=std::string::npos);
    CHECK(bytes.find("\"ms\":5")!=std::string::npos);
    size_t count=0;
    for(size_t at=0;(at=bytes.find("\"ms\":5",at))!=std::string::npos;
        at+=8) ++count;
    CHECK(count==2);
}

static void test_picker_trace_writer_reserves_one_truncation_event(){
    std::string bytes;
    PickerTraceSinkOps ops;
    ops.write=[&](const void* data,size_t size)->size_t{
        bytes.append(static_cast<const char*>(data),size);
        return size;
    };
    ops.flush=[](){ return true; };
    ops.close=[](){ return true; };
    ops.monotonicMs=[](){ return 10ULL; };
    PickerTraceWriter writer(ops,PickerTraceLimits{4096,3},
                             std::array<unsigned char,16>{});
    PickerTraceOpenEvent event;
    writer.emit(event);
    writer.emit(event);
    writer.emit(event);
    writer.emit(event);
    const auto count=[&](const std::string& needle){
        size_t total=0;
        for(size_t at=0;(at=bytes.find(needle,at))!=std::string::npos;
            at+=needle.size()) ++total;
        return total;
    };
    CHECK(count("\"event\":\"picker.open\"")==2);
    CHECK(count("\"event\":\"trace.truncated\"")==1);
    CHECK(!writer.active());
}

static void test_picker_trace_writer_failure_is_fail_open(){
    int writes=0;
    PickerTraceSinkOps ops;
    ops.write=[&](const void*,size_t)->size_t{
        ++writes;
        return 0;
    };
    ops.flush=[](){ return true; };
    ops.close=[](){ return true; };
    ops.monotonicMs=[](){ return 10ULL; };
    PickerTraceWriter writer(ops,PickerTraceLimits{4096,8},
                             std::array<unsigned char,16>{});
    PickerTraceOpenEvent event;
    writer.emit(event);
    writer.emit(event);
    CHECK(writes==1);
    CHECK(!writer.active());
}
```

Add equivalent cases for byte cap, partial writes, callback exceptions, flush
failure, and close failure. Disabled-session callback coverage is added in
Task 5 after `PickerTraceSession` exists.

- [ ] **Step 2: Run RED**

Run `.\build-test.bat`; expect missing `PickerTraceWriter` symbols.

- [ ] **Step 3: Implement the writer contract**

Add:

```cpp
struct PickerTraceLimits {
    explicit PickerTraceLimits(
        uint64_t bytes=2ULL*1024ULL*1024ULL,
        uint32_t events=10000) noexcept
        :maxBytes(bytes),maxEvents(events){}
    uint64_t maxBytes;
    uint32_t maxEvents;
};

struct PickerTraceSinkOps {
    std::function<size_t(const void*,size_t)> write;
    std::function<bool()> flush;
    std::function<bool()> close;
    std::function<uint64_t()> monotonicMs;
};

class PickerTraceWriter {
public:
    PickerTraceWriter(PickerTraceSinkOps ops,PickerTraceLimits limits,
                      const std::array<unsigned char,16>& session) noexcept;
    ~PickerTraceWriter() noexcept;
    bool active() const noexcept;
    void flushBoundary() noexcept;
    void close() noexcept;
    void emit(const PickerTraceCaptureEvent&) noexcept;
    void emit(const PickerTraceOpenEvent&) noexcept;
    void emit(const PickerTraceEnumBeginEvent&) noexcept;
    void emit(const PickerTraceEnumWindowEvent&) noexcept;
    void emit(const PickerTraceEnumEndEvent&) noexcept;
    void emit(const PickerTraceMouseDownEvent&) noexcept;
    void emit(const PickerTraceActivationRequestEvent&) noexcept;
    void emit(const PickerTraceActivationResultEvent&) noexcept;
    void emit(const PickerTraceMoveBeginEvent&) noexcept;
    void emit(const PickerTraceMoveBeginExceptionEvent&) noexcept;
    void emit(const PickerTraceEffectEvent&) noexcept;
    void emit(const PickerTraceApiResultEvent&) noexcept;
    void emit(const PickerTraceTerminalizationAttemptEvent&) noexcept;
    void emit(const PickerTraceTransitionTerminalEvent&) noexcept;
private:
    template<class Event> void emitTyped(const Event&) noexcept;
    void truncate() noexcept;
    bool writeWhole(const std::string&) noexcept;
    PickerTraceSinkOps ops_;
    PickerTraceLimits limits_;
    PickerTraceEnvelope envelope_;
    uint64_t startMs_=0;
    uint64_t lastElapsedMs_=0;
    uint64_t bytesWritten_=0;
    uint32_t eventsWritten_=0;
    bool active_=false;
    bool truncated_=false;
    bool closed_=false;
};
```

Reserve the final event slot and enough byte budget for the exact fixed
`trace.truncated` line before accepting a normal event. Increment `seq`, byte
count, and event count only after `writeWhole` completes. A zero-length partial
write, serializer failure, exception, cap, or flush failure permanently
disables further writes. `close()` attempts the injected close once and never
throws.

- [ ] **Step 4: Run GREEN and commit**

Run `.\build-test.bat`; expect exit `0`.

```powershell
git add src/picker_trace.hpp src/picker_trace.cpp tests/vdtest.cpp
git commit -m "feat(trace): bound the JSONL writer"
```

## Task 4: Add strict storage and retention

**Files:**

- Modify: `src/picker_trace.hpp`
- Modify: `src/picker_trace.cpp`
- Modify: `tests/vdtest.cpp`

- [ ] **Step 1: Write failing filename, retention, and reparse tests**

Add tests for the exact grammar
`picker-YYYYMMDDTHHMMSS.mmmZ-<1..10 decimal PID>.jsonl`, rejecting separators,
`..`, case changes, invalid date/time fields, zero PID, extra suffixes, and
eleven-digit PID. Add this retention test:

```cpp
static void test_picker_trace_retention_is_age_and_count_bounded(){
    const uint64_t day=24ULL*60ULL*60ULL*10000000ULL;
    const uint64_t now=20ULL*day;
    std::vector<PickerTraceDirectoryEntry> entries={
        {L"picker-20260801T000000.000Z-1.jsonl",0,now-8*day},
        {L"picker-20260819T000000.000Z-2.jsonl",0,now-day},
        {L"picker-20260819T000001.000Z-3.jsonl",0,now-day},
        {L"picker-20260819T000002.000Z-4.jsonl",0,now-day},
        {L"keep.txt",0,now-30*day},
        {L"picker-20260818T000000.000Z-5.jsonl",
         FILE_ATTRIBUTE_REPARSE_POINT,now-2*day}
    };
    std::vector<size_t> remove;
    CHECK(PlanPickerTraceRetention(entries,now,2,remove));
    CHECK(remove.size()==2);
    CHECK(remove[0]==0);
    CHECK(remove[1]==1);
}
```

Add injected-storage cases proving:

- `CREATE_NEW` is requested;
- an existing ordinary diagnostics directory is reused, while an existing
  non-directory or reparse diagnostics path disables tracing;
- unrecognized and reparse entries never reach `deleteFile`;
- cleanup is non-recursive;
- create/list/delete failures disable only trace and return normally;
- age exactly seven days is retained and only age greater than seven days is
  expired;
- ties are ordered by filename for deterministic deletion.

- [ ] **Step 2: Run RED**

Run `.\build-test.bat`; expect missing retention/storage symbols.

- [ ] **Step 3: Implement pure retention and injected storage operations**

Add:

```cpp
struct PickerTraceDirectoryEntry {
    PickerTraceDirectoryEntry() noexcept=default;
    PickerTraceDirectoryEntry(std::wstring entryName,DWORD entryAttributes,
                              uint64_t entryLastWrite)
        :name(std::move(entryName)),attributes(entryAttributes),
         lastWrite100ns(entryLastWrite){}
    std::wstring name;
    DWORD attributes=0;
    uint64_t lastWrite100ns=0;
};

bool IsPickerTraceFileName(const std::wstring& value) noexcept;
bool PlanPickerTraceRetention(
    const std::vector<PickerTraceDirectoryEntry>& entries,
    uint64_t now100ns,size_t oldFilesToKeep,
    std::vector<size_t>& remove) noexcept;

struct PickerTraceStorageOps {
    std::function<bool(std::wstring&)> localAppData;
    std::function<DWORD(const std::wstring&)> getAttributes;
    std::function<BOOL(const std::wstring&)> createDirectory;
    std::function<bool(const std::wstring&,
                       std::vector<PickerTraceDirectoryEntry>&)> listDirectory;
    std::function<BOOL(const std::wstring&)> deleteFile;
    std::function<HANDLE(const std::wstring&,DWORD)> createNew;
    std::function<BOOL(HANDLE,const void*,DWORD,DWORD&)> writeFile;
    std::function<BOOL(HANDLE)> flushFile;
    std::function<BOOL(HANDLE)> closeHandle;
    std::function<uint64_t()> utcFileTime100ns;
    std::function<uint64_t()> monotonicMs;
};

PickerTraceStorageOps DefaultPickerTraceStorageOps() noexcept;
```

The production directory code must validate `%LOCALAPPDATA%`, the product
directory, and the diagnostics directory one component at a time. Abort if an
existing component has `FILE_ATTRIBUTE_REPARSE_POINT`, is not a directory, or
cannot be queried. Enumerate only `diagnostics\*`; never recurse. Before
creating the new file, retain the newest two recognized old regular files so
the new session makes at most three. Delete only a child whose basename passes
`IsPickerTraceFileName`, attributes are regular/non-reparse, and joined parent
is the already validated diagnostics directory.

Document in code that this prevents accidental traversal but is not an
adversarial handle-relative TOCTOU sandbox.

- [ ] **Step 4: Run GREEN and commit**

Run `.\build-test.bat`; expect exit `0`.

```powershell
git add src/picker_trace.hpp src/picker_trace.cpp tests/vdtest.cpp
git commit -m "feat(trace): add bounded local storage"
```

## Task 5: Add SHA-256 provenance and wire session lifecycle

**Files:**

- Modify: `src/picker_trace.hpp`
- Modify: `src/picker_trace.cpp`
- Modify: `src/vde.cpp:113-118, 8616-8700`
- Modify: `tests/vdtest.cpp`

- [ ] **Step 1: Write failing digest, normalization, PE, and start tests**

Add the known vector and path equivalence checks:

```cpp
static void test_picker_trace_sha256_and_path_normalization(){
    const PickerTraceDigest digest=PickerTraceSha256Bytes("abc",3);
    CHECK(digest.available);
    CHECK(PickerTraceDigestHex(digest)==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string first,second;
    CHECK(NormalizePickerTraceModulePath(
        L"C:/Apps/VDE.EXE",first));
    CHECK(NormalizePickerTraceModulePath(
        L"\\\\?\\c:\\apps\\vde.exe",second));
    CHECK(first==second);
}
```

Add injected file tests for streaming exact bytes, read failure, truncated DOS
header, invalid `e_lfanew`, truncated PE header, and successful timestamp. Add a
start-event test whose fake module path is
`C:\Users\private\build\vde.exe`; assert the JSON contains image/path digests
and `vde.exe`, but not `Users`, `private`, or the full path.

Add a disabled-session test that counts every storage/provenance callback and
asserts all counts remain zero when `start(false,L"1.1.0")` is called.

- [ ] **Step 2: Run RED**

Run `.\build-test.bat`; expect missing digest/session symbols.

- [ ] **Step 3: Implement BCrypt and provenance types**

Add `#include <bcrypt.h>` and `#pragma comment(lib,"bcrypt.lib")` in
`picker_trace.cpp`. Add:

```cpp
enum class PickerTraceDigestStatus : uint8_t {
    Available, PathUnavailable, NormalizeFailed, OpenFailed,
    MetadataFailed, ReadFailed, CryptoFailed
};

struct PickerTraceDigest {
    PickerTraceDigestStatus status=
        PickerTraceDigestStatus::PathUnavailable;
    std::array<unsigned char,32> bytes{};
    DWORD win32Error=ERROR_SUCCESS;
    LONG cryptoStatus=0;
    bool available=false;
};

struct PickerTraceProvenanceOps {
    std::function<bool(std::wstring&)> modulePath;
    std::function<HANDLE(const std::wstring&)> openRead;
    std::function<BOOL(HANDLE,void*,DWORD,DWORD&)> readFile;
    std::function<BOOL(HANDLE,LONGLONG,DWORD)> seekFile;
    std::function<bool(HANDLE,uint64_t&,uint64_t&)> fileMetadata;
    std::function<BOOL(HANDLE)> closeHandle;
    std::function<bool(void*,size_t)> randomBytes;
    std::function<bool(DWORD&)> processSessionId;
    std::function<bool(DWORD&,bool&)> processIntegrity;
    std::function<DWORD()> processId;
    std::function<DWORD()> threadId;
    std::function<DWORD()> windowsBuild;
};

struct PickerTraceRuntimeOps {
    PickerTraceStorageOps storage;
    PickerTraceProvenanceOps provenance;
};

PickerTraceProvenanceOps DefaultPickerTraceProvenanceOps() noexcept;
PickerTraceRuntimeOps DefaultPickerTraceRuntimeOps() noexcept;

PickerTraceDigest PickerTraceSha256Bytes(
    const void* bytes,size_t size) noexcept;
PickerTraceDigest PickerTraceSha256File(
    const std::wstring& path,const PickerTraceProvenanceOps& ops) noexcept;
std::string PickerTraceDigestHex(const PickerTraceDigest&) noexcept;
bool NormalizePickerTraceModulePath(
    const std::wstring& input,std::string& normalizedUtf8) noexcept;

struct PickerTraceStartEvent {
    PickerTraceDigest imageDigest;
    PickerTraceDigest pathDigest;
    PickerTraceSafeImageBasename moduleBasename;
    uint64_t fileSize=0;
    uint64_t lastWrite100ns=0;
    uint32_t peTimestamp=0;
    DWORD pid=0;
    DWORD tid=0;
    DWORD processSessionId=0;
    DWORD integrityRid=0;
    DWORD windowsBuild=0;
    bool fileMetadataAvailable=false;
    bool peTimestampAvailable=false;
    bool processSessionAvailable=false;
    bool integrityAvailable=false;
    bool elevated=false;
};

bool SerializePickerTraceLine(
    const PickerTraceEnvelope&,const PickerTraceStartEvent&,
    const wchar_t* appVersion,std::string& output) noexcept;
```

Extend `PickerTraceWriter` with
`void emitStart(const PickerTraceStartEvent&,const wchar_t*) noexcept`; it uses
the same sequence/budget rules and is the only writer entry accepted before
the session marks startup complete.

Normalize as: absolute DOS path supplied by `GetModuleFileNameW`; convert
`\\?\UNC\` to `\\`, remove ordinary `\\?\`, replace `/` with `\`, lowercase
with invariant Windows casing, then hash the normalized UTF-8 bytes. Stream the
image in 64 KiB blocks through BCrypt. Never load the whole executable.

Read the PE timestamp by validating `IMAGE_DOS_HEADER`, non-negative bounded
`e_lfanew`, the `IMAGE_NT_SIGNATURE`, and complete
`IMAGE_FILE_HEADER` before using `TimeDateStamp`.

Collect file size/mtime, PID/TID, Windows session ID, integrity RID/elevation,
Windows build, application version, image digest/status, normalized-path
digest/status, safe module basename, and PE timestamp. Every failed field has a
fixed availability/status/error, and no failure removes already available
fields.

- [ ] **Step 4: Implement `PickerTraceSession`**

Declare and implement:

```cpp
class PickerTraceSession {
public:
    PickerTraceSession() noexcept;
    explicit PickerTraceSession(PickerTraceRuntimeOps ops,
                                PickerTraceLimits limits=
                                    PickerTraceLimits()) noexcept;
    ~PickerTraceSession() noexcept;
    bool start(bool requested,const wchar_t* appVersion) noexcept;
    bool active() const noexcept;
    bool requested() const noexcept;
    uint64_t nextCorrelationId() noexcept;
    void flushBoundary() noexcept;
    void close() noexcept;
    void emit(const PickerTraceCaptureEvent&) noexcept;
    void emit(const PickerTraceOpenEvent&) noexcept;
    void emit(const PickerTraceEnumBeginEvent&) noexcept;
    void emit(const PickerTraceEnumWindowEvent&) noexcept;
    void emit(const PickerTraceEnumEndEvent&) noexcept;
    void emit(const PickerTraceMouseDownEvent&) noexcept;
    void emit(const PickerTraceActivationRequestEvent&) noexcept;
    void emit(const PickerTraceActivationResultEvent&) noexcept;
    void emit(const PickerTraceMoveBeginEvent&) noexcept;
    void emit(const PickerTraceMoveBeginExceptionEvent&) noexcept;
    void emit(const PickerTraceEffectEvent&) noexcept;
    void emit(const PickerTraceApiResultEvent&) noexcept;
    void emit(const PickerTraceTerminalizationAttemptEvent&) noexcept;
    void emit(const PickerTraceTransitionTerminalEvent&) noexcept;
    std::wstring pathForLocalInspection() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

`pathForLocalInspection()` is used only by tests/support handoff and is never
serialized. `start(false,L"1.1.0")` performs no callback.
`start(true,L"1.1.0")` validates
storage, cleans recognized files, creates with `CREATE_NEW`, generates a
128-bit session ID with `BCryptGenRandom`, builds provenance, and writes
`trace.start` first.

The default constructor leaves `impl_` null and performs no allocation or
Win32 call. The injected constructor uses nothrow/caught allocation; failure
leaves a permanently inactive session. `start(true,...)` catches allocation
and callback failures, while `start(false,...)` remains a strict zero-work
path.

- [ ] **Step 5: Wire lifecycle after the tray mutex is acquired**

Add one global session near the picker globals:

```cpp
static PickerTraceSession g_pickerTrace;
```

Inside the GUI branch of `dispatch`, before `InitMetrics()` and only after
`RunWithTrayInstanceScope` has acquired the mutex:

```cpp
(void)g_pickerTrace.start(tracePicker,APP_VERSION);
```

Call `g_pickerTrace.close()` after GUI dispatch, in the outer catch before
return, and on the normal path before `CoUninitialize()`. Closing is idempotent.
The CLI branch never starts tracing. Leave `SetRunAtLogon` unchanged.

- [ ] **Step 6: Run GREEN, build, and commit**

Run:

```powershell
.\build-test.bat
.\build.bat
```

Expected: both exit `0`; build ends with `Built build\vde.exe`.

```powershell
git add src/picker_trace.hpp src/picker_trace.cpp src/vde.cpp tests/vdtest.cpp
git commit -m "feat(trace): record executable provenance"
```

## Task 6: Trace picker capture, opening, and every enumeration decision

**Files:**

- Modify: `src/picker_trace.hpp`
- Modify: `src/lifecycle.hpp:1450-1537`
- Modify: `src/vde.cpp:647-674, 5290-5548, 7497-7625`
- Modify: `tests/vdtest.cpp`

- [ ] **Step 1: Write failing pure-decision and equivalence tests**

Add a table test for every current Alt+Tab short-circuit:

```cpp
static void test_picker_trace_alt_tab_reason_preserves_current_decision(){
    struct Case { bool visible; int title; uint64_t ex;
                  uintptr_t hwnd; uintptr_t root;
                  PickerTraceAltTabReason expected; };
    const Case cases[]={
        {false,9,0,1,1,PickerTraceAltTabReason::NotVisible},
        {true,0,0,1,1,PickerTraceAltTabReason::FirstTitleUnavailable},
        {true,9,WS_EX_TOOLWINDOW,1,1,PickerTraceAltTabReason::ToolWindow},
        {true,9,0,1,2,PickerTraceAltTabReason::RootOwnerMismatch},
        {true,9,0,1,1,PickerTraceAltTabReason::Eligible}
    };
    for(const Case& value:cases)
        CHECK(DecidePickerTraceAltTabReason(
            value.visible,value.title,value.ex,value.hwnd,value.root)==
            value.expected);
}
```

Add a table covering every `PickerTraceEnumDecision` and its stable JSON name.
Use injected `PickerTraceAltTabOps` counters to prove each eligibility-driving
Win32 value is read once, later calls remain short-circuited, and traced versus
untraced eligibility is identical. Separately table-test
`DecidePickerTraceEnumDecision` from already-observed desktop/title/PID/
process-start/identity facts. The later product branches remain the existing
branches; each assigns its fixed final decision to one non-driving finalizer,
so tracing never owns title text, admission, or row publication. Add checks
that an enum event has title length/read status but no title-text member or
serialized title sentinel.

Add fake snapshot operations for `GetDesktops`, `GetCount`, every `GetAt`,
every `GetId`, invalid count/GUID, allocation failure, exception, and complete
snapshot. Run each case through the existing unobserved overload and the new
observable overload below; compare bool result, ordered GUID output, release
counts, and COM-call order. Assert the observer yields the exact final status,
raw HRESULT, failing index, and count without changing ownership.

- [ ] **Step 2: Run RED**

Run `.\build-test.bat`; expect missing decision helper/fact types.

- [ ] **Step 3: Replace `IsAltTabWindow` with a fact-producing equivalent**

Add:

```cpp
struct PickerTraceAltTabFacts {
    bool visibleObserved=false;
    bool visible=false;
    bool firstTitleObserved=false;
    int firstTitleLength=0;
    DWORD firstTitleError=ERROR_SUCCESS;
    bool exStyleObserved=false;
    LONG_PTR exStyle=0;
    DWORD exStyleError=ERROR_SUCCESS;
    bool rootOwnerObserved=false;
    HWND rootOwner=nullptr;
    PickerTraceAltTabReason reason=
        PickerTraceAltTabReason::FirstTitleUnavailable;
};

struct PickerTraceAltTabOps {
    void* context=nullptr;
    BOOL (*isVisible)(void*,HWND)=nullptr;
    int (*titleLength)(void*,HWND,DWORD&)=nullptr;
    LONG_PTR (*extendedStyle)(void*,HWND,DWORD&)=nullptr;
    HWND (*rootOwner)(void*,HWND)=nullptr;
};

struct PickerTraceDesktopSnapshotFacts {
    PickerTraceDesktopSnapshotStatus status=
        PickerTraceDesktopSnapshotStatus::NotAttempted;
    HRESULT result=E_NOTIMPL;
    int index=-1;
    uint32_t count=0;
};

PickerTraceAltTabReason DecidePickerTraceAltTabReason(
    bool visible,int titleLength,uint64_t exStyle,
    uintptr_t hwnd,uintptr_t rootOwner) noexcept;

PickerTraceAltTabFacts ObservePickerTraceAltTabWindow(
    HWND hwnd,const PickerTraceAltTabOps& ops) noexcept;

PickerTraceEnumDecision DecidePickerTraceEnumDecision(
    PickerTraceAltTabReason altTabReason,bool desktopServiceAvailable,
    HRESULT desktopResult,bool desktopGuidAvailable,int tileIndex,
    int secondTitleLength,int secondTitleCopied,bool pidAvailable,
    bool processStartAvailable,
    WindowIdentityRecapture recapture) noexcept;
```

In `lifecycle.hpp`, add `<new>` and a product-neutral snapshot observation
overload:

```cpp
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

template<class Array,class Desktop,class Ops>
bool SnapshotDesktopCollectionOwned(
    Ops&,std::vector<DesktopCollectionEntry>&,
    const std::function<void(
        const DesktopCollectionSnapshotObservation&)>* observer) noexcept;
```

Keep the current two-argument overload and delegate to the new overload with
null. Emit after each raw COM result, before every invalid-count/GUID return,
after complete swap, in a distinct `std::bad_alloc` catch, and in the catch-all.
Observer exceptions are swallowed and never affect the snapshot. Paired tests
must prove identical output/ownership/calls for null and active observers.

Implement production `PickerTraceAltTabOps` with non-allocating function
pointers in `vde.cpp` and call
`ObservePickerTraceAltTabWindow` with exactly the current order: visibility,
first title length, extended styles/tool flag, root-owner equality. Each injected
test counter must be one for the calls reached before the first rejection and
zero for later short-circuited calls. Only when trace is active, collect owner,
last-active-popup, class, PID/TID, safe image basename, and DWM cloaking as
non-driving facts after the decision.

- [ ] **Step 4: Emit exactly one final event per enumerated HWND**

Extend `PickerEnumContext` with model correlation, enum sequence, total count,
and decision counters. The context owns one stack-local facts record for the
current callback; it contains no title buffer. Refactor `EnumAll` so every
existing early return assigns one decision and calls one non-throwing finalizer
that emits `PickerTraceEnumWindowEvent` and increments the exact decision
counter. With tracing inactive the finalizer performs no callback and cannot
change the already-selected product return. Preserve these boundaries
separately:

- not visible;
- first title unavailable;
- tool window;
- root-owner mismatch;
- missing desktop service;
- failed `GetWindowDesktopId`;
- zero desktop GUID;
- desktop GUID missing from tile snapshot;
- second title length unavailable;
- second title read failure;
- PID unavailable;
- process start unavailable;
- identity indeterminate;
- identity lost;
- display-only publication;
- verified publication;
- allocation/global snapshot failure.

Change `TryReadProcessStart` to accept an optional facts output containing
`OpenProcess`/`GetProcessTimes` status and immediate error, with a default null
argument so every non-picker caller keeps current behavior.

Change `BuildModel` itself (not the unrelated `CurrentDesktops` callers) to
accept `PickerTraceDesktopSnapshotFacts* snapshotFacts=nullptr`. Replace its
inline `GetDesktops`/`GetCount`/`GetAt`/`GetID` chain with
`SnapshotDesktopCollectionOwned<IObjectArray,IVirtualDesktop>` using the same
`DesktopCollectionComOps`, then build tiles from the returned ordered
`DesktopCollectionEntry` values. Map its neutral observations into
`snapshotFacts`. Paired fake-ops tests and the existing desktop-order tests must
prove the refactor preserves calls, release ownership, GUID order, count
bounds, tile names, and boolean failure.

Construct the active observer before the first COM call; if observer allocation
fails, call the unobserved overload once. Do not retry after any COM stage has
begun. `ShowPicker` owns a facts record, passes it through every `BuildModel`
path, and copies it into its exactly-once `picker.open` event. Emit `enum.begin`
only after the ordered snapshot is complete. Around `EnumWindows`, set last
error to success, capture its BOOL and immediate error, then emit `enum.end`
with totals/counters/model outcome. A snapshot failure before `enum.begin`
therefore still exposes raw HRESULT, failing index, and count in `picker.open`.
Flush after `enum.end`.

- [ ] **Step 5: Trace target capture and every `ShowPicker` outcome**

Emit `picker.capture` once at the end of `CapturePickerTarget`; never pass
`capture.title`, only read status/length. In `ShowPicker`, use one local result
scope that emits exactly one `picker.open` for the existing outcomes listed in
`PickerTraceOpenResult`. Do not change cleanup, publication, or show ordering.

- [ ] **Step 6: Run GREEN, build, and commit**

Run `.\build-test.bat` and `.\build.bat`; both must exit `0`.

```powershell
git add src/picker_trace.hpp src/picker_trace.cpp src/vde.cpp src/lifecycle.hpp tests/vdtest.cpp
git commit -m "feat(trace): explain picker enumeration"
```

## Task 7: Trace mouse activation and every move-entry guard

**Files:**

- Modify: `src/vde.cpp:7165-7496, 7734-7741, 8096-8159`
- Modify: `tests/vdtest.cpp`

- [ ] **Step 1: Write failing activation and move-entry tests**

Add a pure activation dispatcher with fake callbacks and assert these exact
results: already controlled, invalid tile, selection publication failed,
ordinary switch, and move-entry dispatched. For every case, assert the fake
trace transcript contains one request and one result with the same activation
ID. For Ctrl dispatch, also assert the identical ID reaches the begin-move
callback; for every pre-move guard, assert that callback is not invoked.

Add a table iterating every `PickerTraceMoveBeginReason`. Serialize one
`PickerTraceMoveBeginEvent` per value and verify the fixed name. Add two
exception events with `transitionPublished=false/true` and verify distinct
JSON. Add a behavior-equivalence test that runs the dispatcher with a null
session and a memory session and compares selection callback count, chosen
route, and result.

- [ ] **Step 2: Run RED**

Run `.\build-test.bat`; expect missing activation dispatcher/scope symbols.

- [ ] **Step 3: Add a testable activation dispatcher**

Add this template to `picker_trace.hpp`:

```cpp
template<class EmitRequest,class Select,class Refresh,class PlainSwitch,
         class BeginMove,class EmitResult>
PickerTraceActivationResult DispatchPickerActivation(
        uint64_t activationId,PickerTraceActivationSource source,
        bool controlled,int index,int count,bool ctrlMove,
        EmitRequest emitRequest,
        Select select,Refresh refresh,PlainSwitch plainSwitch,
        BeginMove beginMove,EmitResult emitResult) {
    emitRequest(activationId,source,ctrlMove,index);
    const auto finish=[&](PickerTraceActivationResult result){
        emitResult(activationId,result);
        return result;
    };
    if(controlled)
        return finish(PickerTraceActivationResult::AlreadyControlled);
    if(index<0 || index>=count)
        return finish(PickerTraceActivationResult::InvalidTile);
    if(!select(index))
        return finish(
            PickerTraceActivationResult::SelectionPublicationFailed);
    refresh();
    if(ctrlMove){
        beginMove(index,activationId);
        return finish(PickerTraceActivationResult::DispatchedMoveEntry);
    }
    plainSwitch(index);
    return finish(PickerTraceActivationResult::RoutedPlainSwitch);
}
```

Route the existing `Activate` body through it. Change `Activate` to accept the
fixed mouse/keyboard source, allocate exactly one
`activationId=g_pickerTrace.nextCorrelationId()`, and supply typed no-throw
request/result emitter lambdas. Change `BeginVerifiedPickerMove` to accept that
ID and copy it into `move.begin` or `move.begin.exception`; do not allocate a
second ID there. Emit `mouse.down` before the controlled-transition early
return; use `target=None/tileIndex=-1` when hit resolution is intentionally
skipped. Non-tile pointer actions are represented only by `mouse.down` and do
not create a misleading tile activation span.

- [ ] **Step 4: Give every `BeginVerifiedPickerMove` exit one reason**

Keep the function's behavior but return `PickerTraceMoveBeginReason`. Split
grouped conditions only enough to preserve their left-to-right evaluation and
name the exact failed guard. Track a local stage through reservation setup so
the following failures remain distinct: accepted operation missing, claim
stage, reservation handoff, pending association, provisional insertion, and no
initial effect.

Move `transitionPublishedForTrace` outside the `try`. On catch, emit
`move.begin.exception` with its value; do not classify a post-publication
exception as a rejected entry. On accepted begin, emit generation, four desktop
GUIDs, destination tile, and first effect. Flush after rejection/exception or
once the first effect is queued.

- [ ] **Step 5: Run GREEN, build, and commit**

Run `.\build-test.bat` and `.\build.bat`; expect exit `0`.

```powershell
git add src/picker_trace.hpp src/picker_trace.cpp src/vde.cpp tests/vdtest.cpp
git commit -m "feat(trace): expose move entry guards"
```

## Task 8: Preserve raw desktop/API results and trace the reducer

**Files:**

- Modify: `src/vde.cpp:438-486, 5336-5347, 6456-6733, 6738-6960, 7475-7479`
- Modify: `src/lifecycle.hpp:1420-1591`
- Modify: `src/picker_trace.hpp`
- Modify: `src/picker_trace.cpp`
- Modify: `tests/vdtest.cpp`

- [ ] **Step 1: Write failing raw-result and reducer-equivalence tests**

Add tests that assert:

```cpp
template<class Event>
static bool SerializedTraceContains(
        const Event& event,const std::string& needle){
    PickerTraceEnvelope envelope;
    envelope.session.fill(0x33);
    envelope.seq=1;
    std::string line;
    return SerializePickerTraceLine(envelope,event,line) &&
        line.find(needle)!=std::string::npos;
}

static void test_picker_trace_raw_api_contracts_are_exact(){
    PickerTraceApiResultEvent failed;
    failed.api=PickerTraceApiKind::MoveViewToDesktop;
    failed.resultKind=PickerTraceRawResultKind::HResult;
    failed.hresult=(HRESULT)0x80070005L;
    PickerTraceApiResultEvent foreground;
    foreground.api=PickerTraceApiKind::SetForegroundWindow;
    foreground.resultKind=PickerTraceRawResultKind::NoExtendedError;
    foreground.boolResult=false;
    foreground.lastErrorAvailable=false;
    PickerTraceApiResultEvent cleanup;
    cleanup.api=PickerTraceApiKind::ShowWindowProgmanCleanup;
    cleanup.resultKind=PickerTraceRawResultKind::PreviousVisibility;
    cleanup.boolResult=true;
    CHECK(SerializedTraceContains(failed,"0x80070005"));
    CHECK(SerializedTraceContains(foreground,
        "\"last_error_available\":false"));
    CHECK(SerializedTraceContains(cleanup,
        "\"previously_visible\":true"));
}
```

Add a fake `DesktopCollectionComOps` transcript for failures at `GetDesktops`,
`GetCount`, each `GetAt`, `GetId`, not-found, and match. Assert every raw HRESULT
and examined index is emitted before the final lookup stage. Run each lookup
once without an observer and once with a memory observer; compare returned bool,
owned desktop, output index, release counts, and operation-call order.

Add injected foreground-handoff operations that record call order. Cover both
attach branches and exact `attach desktop -> attach foreground -> focus ->
detach foreground -> detach desktop -> switch -> cleanup` ordering. Run with a
null and memory trace observer and compare the returned HRESULT, `invoked`, each
raw BOOL, and every operation count. Assert error-reader callbacks occur
immediately for the generic error-capable BOOL wrapper and are never called for
`AttachThreadInput`, `SetForegroundWindow`, or `ShowWindow`, whose contracts do
not provide the extended error used here.

Add a scheduling table/fake-call test for: no main window, shutdown drain,
delayed timer success, delayed timer failure with durable ownership, posted,
one-millisecond timer fallback, and complete post/timer failure. Compare the
legacy deferred boolean (`result.deferred`) and fake call order, and assert one
exact available delivery route for every result. Cover both external queue and
in-pump callers; neither may guess a route from a collapsed bool.

Create identical `PickerState` copies and feed the same begin, success, retry,
rollback, cancellation, and terminal observations through direct
`AdvancePickerTransition` and `AdvancePickerTransitionTraced`. Compare returned
`PickerEffect`, phase, effect serial, all attempt counters, flags, and desktop
readbacks; only trace bytes may differ. Table-test the fixed rollback-trigger
and diagnostic-code classifiers, including first-trigger-wins behavior, without
reading `PickerTransition::diagnostic`.

- [ ] **Step 2: Run RED**

Run `.\build-test.bat`; expect missing traced reducer/lookup context symbols.

- [ ] **Step 3: Add an observable lookup seam without changing shared COM logic**

In `lifecycle.hpp`, add a product-neutral observation record and an overload of
`LookupDesktopCollectionOwned`:

```cpp
enum class DesktopCollectionLookupObservationStage {
    ValidateRequest, GetDesktops, GetCount, GetAt, GetId,
    Match, NotFound, Exception
};

struct DesktopCollectionLookupObservation {
    DesktopCollectionLookupObservationStage stage=
        DesktopCollectionLookupObservationStage::ValidateRequest;
    int index=-1;
    HRESULT result=E_NOTIMPL;
    GUID actual{};
    bool matched=false;
};

template<class Array,class Desktop,class Ops,class DesktopOwner>
bool LookupDesktopCollectionOwned(
    const DesktopCollectionLookupRequest&,Ops&,DesktopOwner&,int&,
    const std::function<void(
        const DesktopCollectionLookupObservation&)>* observer) noexcept;
```

Keep the current four-argument overload and have it call the observable overload
with null. The observable overload emits `ValidateRequest`, every raw COM stage,
`Match` or `NotFound`, and `Exception`. Observer exceptions are caught at the
emission boundary and never become lookup failures. Its product return,
ownership/reset behavior, short-circuit order, and COM calls must match the
unobserved overload; the paired fake-ops tests lock this equivalence.

Add the narrow public context used by the existing COM-ops adapter:

```cpp
struct PickerTraceDesktopLookupContext {
    PickerTraceSession* trace=nullptr;
    PickerTraceDesktopLookupUse use=
        PickerTraceDesktopLookupUse::MoveEntryDestination;
    uint64_t generation=0;
    uint64_t effectSerial=0;
    GUID requested{};
};

void EmitPickerTraceDesktopLookupStage(
    const PickerTraceDesktopLookupContext* context,
    PickerTraceDesktopLookupStage stage,int index,HRESULT result,
    const GUID& actual,bool matched) noexcept;
```

Map the lifecycle observation into `PickerTraceApiResultEvent` only inside the
picker adapter. Add optional picker-only context parameters to
`GetDesktopByGuid` and `GetDesktopIndexByGuid`; default null keeps every
auto/CLI caller on the original four-argument overload. The adapter passes the
observable overload only when context and an active trace session are present,
so wrapper-level validation, not-found, and exception stages are retained. If
constructing the active observer fails, catch it and rerun the original
unobserved lookup; diagnostic allocation failure must not become a product
lookup failure.

Pass explicit uses at:

- move-entry destination validation;
- target destination lookup;
- popup destination lookup;
- switch precheck lookup;
- switch handoff lookup.

Do not collapse the precheck and handoff lookups into one event. Instrument
`CurrentDesktopGuid` and `ReadPickerWindowDesktop` with optional facts outputs
that retain raw HRESULT, GUID, and validity.

- [ ] **Step 4: Instrument move, switch, and foreground APIs**

Add testable, non-throwing call seams to `picker_trace.hpp`:

```cpp
struct PickerTraceBoolCallResult {
    BOOL value=FALSE;
    DWORD immediateError=ERROR_SUCCESS;
};

template<class Call,class ReadError>
PickerTraceBoolCallResult CallPickerTraceBoolWithImmediateError(
        Call call,ReadError readError) noexcept {
    PickerTraceBoolCallResult result;
    try {
        result.value=call();
        result.immediateError=readError();
    } catch(...) {
        result.value=FALSE;
        result.immediateError=ERROR_GEN_FAILURE;
    }
    return result;
}

struct PickerTraceForegroundHandoffOps {
    void* context=nullptr;
    BOOL (*attachThreadInput)(void*,DWORD,DWORD,BOOL)=nullptr;
    BOOL (*setForegroundWindow)(void*,HWND)=nullptr;
    HRESULT (*switchDesktop)(void*)=nullptr;
    BOOL (*showWindow)(void*,HWND,int)=nullptr;
};

struct PickerTraceApiEventObserver {
    void* context=nullptr;
    void (*emit)(void*,const PickerTraceApiResultEvent&) noexcept=nullptr;
};

struct PickerTraceForegroundHandoffResult {
    HRESULT switchResult=E_FAIL;
    bool invoked=false;
    bool desktopAttachAttempted=false;
    bool desktopAttached=false;
    bool foregroundAttachAttempted=false;
    bool foregroundAttached=false;
    BOOL focusResult=FALSE;
    BOOL foregroundDetachResult=FALSE;
    BOOL desktopDetachResult=FALSE;
    BOOL previousVisibility=FALSE;
};

PickerTraceForegroundHandoffResult ExecutePickerForegroundHandoffCalls(
    const PickerForegroundHandoffPlan&,HWND progman,
    DWORD desktopThread,DWORD foregroundThread,DWORD currentThread,
    const PickerTraceForegroundHandoffOps&,
    const PickerTraceApiEventObserver* observer)
    noexcept;
```

The executor preserves the current conditional attach/detach behavior and exact
order. It calls the observer only after capturing each raw return; observer
failure is swallowed. It explicitly marks extended error unavailable for all
four foreground operation kinds above. Use
`CallPickerTraceBoolWithImmediateError` only at APIs whose contract supports the
captured error, including the `EnumWindows` boundary from Task 6; its injected
test proves the error read occurs before any later call.

In `IssuePickerWindowMove`, record destination lookup, `GetViewForHwnd`, identity
boundary, selected move API, invocation, and raw HRESULT before constructing the
existing boolean observation.

In `SwitchDesktopWithForegroundHandoff`, compute the current plan and call
`ExecutePickerForegroundHandoffCalls` with non-allocating production function
pointers. Record the
Progman/foreground HWND and thread IDs, each attach/detach BOOL, focus BOOL,
raw `SwitchDesktop` HRESULT, and `ShowWindow` return as previous visibility,
not success. Do not call `GetLastError` for APIs whose contract does not define
it. The helper's paired observer/no-observer tests lock
attach/focus/detach/switch/cleanup equivalence.

- [ ] **Step 5: Add the traced reducer wrapper and effect stages**

Declare:

```cpp
struct PickerTraceScheduleResult {
    bool deferred=false;
    bool routeAvailable=false;
    PickerTraceDeliveryRoute route=PickerTraceDeliveryRoute::None;
};

inline PickerTraceScheduleResult MarkPickerTraceDurableKick(
        PickerTraceScheduleResult result) noexcept {
    result.routeAvailable=true;
    result.route=PickerTraceDeliveryRoute::DurableExternalKick;
    return result;
}

template<class ArmTimer,class Post>
PickerTraceScheduleResult SchedulePickerTransitionWork(
        bool mainAvailable,bool shutdownDrain,uint64_t remainingMs,
        ArmTimer armTimer,Post post) noexcept {
    PickerTraceScheduleResult result;
    result.routeAvailable=true;
    if(!mainAvailable){
        result.route=PickerTraceDeliveryRoute::InlineFallback;
        return result;
    }
    if(shutdownDrain){
        result.route=PickerTraceDeliveryRoute::ShutdownDrain;
        return result;
    }
    if(remainingMs!=0){
        const UINT wait=static_cast<UINT>((std::min)(
            remainingMs,static_cast<uint64_t>(UINT_MAX)));
        if(armTimer(wait?wait:1)){
            result.deferred=true;
            result.route=PickerTraceDeliveryRoute::DelayedTimer;
            return result;
        }
        result.deferred=true;
        result.route=PickerTraceDeliveryRoute::DurableExternalKick;
        return result;
    }
    if(post()){
        result.deferred=true;
        result.route=PickerTraceDeliveryRoute::Posted;
        return result;
    }
    if(armTimer(1)){
        result.deferred=true;
        result.route=PickerTraceDeliveryRoute::TimerArmed;
        return result;
    }
    result.route=PickerTraceDeliveryRoute::InlineFallback;
    return result;
}

struct PickerTraceTerminalMetadata {
    uint64_t generation=0;
    PickerTraceRollbackTrigger rollbackTrigger=
        PickerTraceRollbackTrigger::None;
    PickerTraceDiagnosticCode diagnosticCode=
        PickerTraceDiagnosticCode::None;
};

void ObservePickerTraceTerminalMetadata(
    PickerTraceTerminalMetadata&,PickerPhase before,PickerPhase after,
    const PickerObservation&,bool queueConflict,bool caughtException)
    noexcept;

PickerEffect AdvancePickerTransitionTraced(
    PickerState& state,const PickerObservation& observation,
    PickerTraceSession* trace,PickerTraceTerminalMetadata* metadata)
    noexcept;
```

It snapshots privacy-safe phase/generation/serial/counters, calls the existing
`AdvancePickerTransition` exactly once, snapshots the result, emits
`effect.reduce` when `trace && trace->active()`, updates trace-only terminal
metadata on the first rollback transition using fixed inputs (never the
diagnostic sentence), and returns the exact effect. Reset metadata when an
accepted begin publishes a new generation. Replace only the three runtime calls
for begin, observation, and cancellation.

Change `DeferPickerTransitionWork` to return `PickerTraceScheduleResult` and
route its current branches through `SchedulePickerTransitionWork`; every caller
continues branching on `.deferred`, preserving the old bool exactly. Apply the
returned durable-kick ownership to `g_pickerDurableKickPending` at the same
branch as today. When a caller itself sets the durable flag after an otherwise
failed schedule, pass its trace copy through `MarkPickerTraceDurableKick`
without changing `.deferred`. Tests cover API-call order, caller promotion, and
the legacy bool.

Keep `StagePickerScheduledEffect` product-only. After every effect-related
scheduling or re-arm result and before any inline pump call, emit
`effect.queue` with the actual returned route and an incremented
`deliveryAttempt` for the same generation/serial. This includes both external
and in-pump callers; either can truthfully report posted, one-millisecond timer,
delayed timer, durable kick, inline fallback, or shutdown drain. Terminal-only
scheduling callsites preserve their old branch through `.deferred` in this
task; Task 9 stores their route in pending terminal-delivery state instead of an
effect event. No traced callsite infers route from `.deferred` alone.

Emit `effect.execute` after `RoutePickerEffectExecution` is known and before the
selected execute/defer/acknowledge branch, with
`executionRouteAvailable=true`. Emit `effect.observation` after
`ExecutePickerEffect`, including catch. Queue events set
`deliveryAvailable=true`; other stages omit delivery unless that decision is
actually known. Reducer events omit both route fields.

- [ ] **Step 6: Run GREEN, build, and commit**

Run `.\build-test.bat` and `.\build.bat`; expect exit `0`.

```powershell
git add src/picker_trace.hpp src/picker_trace.cpp src/vde.cpp src/lifecycle.hpp tests/vdtest.cpp
git commit -m "feat(trace): retain raw picker results"
```

## Task 9: Trace terminalization attempts that make no progress

**Files:**

- Modify: `src/vde.cpp:2436-2473, 6799-6947`
- Modify: `src/picker_trace.hpp`
- Modify: `src/picker_trace.cpp`
- Modify: `tests/vdtest.cpp`

- [ ] **Step 1: Write failing terminalization tests**

Add injected cases for:

- terminal not acknowledged;
- pending effect still present;
- reservation release throws before the first action, after the first action,
  and after the retry action, with availability asserted at each boundary;
- exact reservation is not released;
- runtime key is empty;
- readiness false;
- `FinalizePickerTransition` false;
- successful finalization;
- failed post/no-timer route becoming durable kick;
- delayed timer, posted, timer-backed, inline shutdown drain routes.

Drive those cases through the public terminalization driver below with fake
release/readiness/finalize callbacks. Feed each result through the public
event-mapping/publishing seam with a null observer and a memory observer.
Compare product callback order/result; assert one attempt and no terminal for
every no-progress result, and attempt-before-terminal for completion. The
static runtime wrapper in `vde.cpp` is not the unit-test seam.

Table-test pending delivery storage: store/consume is one-shot, generation
mismatch clears without returning a route, retry route becomes the next
attempt's incoming route, and reset removes stale delivery.

For every no-progress case, assert one `terminalization.attempt` and no
`transition.terminal`. For success, assert the attempt precedes exactly one
terminal event and a boundary flush. Assert serialized terminal records do not
contain a diagnostic-sentence sentinel.

- [ ] **Step 2: Run RED**

Run `.\build-test.bat`; expect the new terminal facts/helper to be missing.

- [ ] **Step 3: Preserve release/finalization facts before state reset**

Add and table-test these pure classifiers before wiring them into the pump:

```cpp
struct PickerTraceReservationReleaseFacts {
    PickerTerminalGuardReleaseAction firstAction=
        PickerTerminalGuardReleaseAction::ResolvedAbsent;
    PickerTerminalGuardReleaseAction retryAction=
        PickerTerminalGuardReleaseAction::ResolvedAbsent;
    PickerTraceReservationExceptionStage exceptionStage=
        PickerTraceReservationExceptionStage::None;
    bool attempted=false;
    bool firstActionAvailable=false;
    bool retryActionAvailable=false;
    bool exceptionStageAvailable=false;
    bool retried=false;
    bool threw=false;
    bool released=false;
};

struct PickerTraceTerminalizationRunResult {
    PickerTraceTerminalizationReason reason=
        PickerTraceTerminalizationReason::RuntimeNotReady;
    PickerTraceReservationReleaseFacts release;
    bool terminalAcknowledged=false;
    bool pendingEffectNone=false;
    bool runtimeKeyPresent=false;
    bool ready=false;
    bool finalized=false;
    bool completed=false;
};

struct PickerTraceTerminalizationEmission {
    PickerTraceTerminalizationAttemptEvent attempt;
    bool terminalAllowed=false;
};

struct PickerTraceTerminalDeliveryFacts {
    PickerTraceDeliveryRoute incoming=PickerTraceDeliveryRoute::None;
    PickerTraceDeliveryRoute retry=PickerTraceDeliveryRoute::None;
    bool incomingAvailable=false;
    bool retryAvailable=false;
};

struct PickerTracePendingTerminalDelivery {
    uint64_t generation=0;
    PickerTraceDeliveryRoute route=PickerTraceDeliveryRoute::None;
    bool available=false;
};

void StorePickerTracePendingTerminalDelivery(
    PickerTracePendingTerminalDelivery&,uint64_t generation,
    PickerTraceDeliveryRoute) noexcept;
bool ConsumePickerTracePendingTerminalDelivery(
    PickerTracePendingTerminalDelivery&,uint64_t generation,
    PickerTraceDeliveryRoute& route) noexcept;
void ResetPickerTracePendingTerminalDelivery(
    PickerTracePendingTerminalDelivery&) noexcept;

struct PickerTraceTerminalizationEventObserver {
    void* context=nullptr;
    void (*emitAttempt)(
        void*,const PickerTraceTerminalizationAttemptEvent&) noexcept=nullptr;
    void (*emitTerminal)(
        void*,const PickerTraceTransitionTerminalEvent&) noexcept=nullptr;
    void (*flushBoundary)(void*) noexcept=nullptr;
};

PickerTraceTerminalizationReason DecidePickerTraceTerminalizationReason(
    bool terminalAcknowledged,bool pendingEffectNone,
    bool releaseThrew,bool reservationReleased,
    bool runtimeKeyPresent,bool ready,bool finalized) noexcept;

PickerTraceDeliveryRoute DecidePickerTraceDeliveryRoute(
    bool shutdownDrain,bool delayedTimer,bool posted,bool timerArmed,
    bool inlineFallback,bool durableKick) noexcept;

PickerTraceTerminalizationEmission MapPickerTraceTerminalization(
    const PickerTraceTerminalizationRunResult&,uint64_t generation,
    uint64_t attempt,const PickerTraceTerminalDeliveryFacts&) noexcept;

void PublishPickerTraceTerminalization(
    const PickerTraceTerminalizationEmission&,
    const PickerTraceTransitionTerminalEvent* terminal,
    const PickerTraceTerminalizationEventObserver* observer) noexcept;

template<class Release,class Ready,class Finalize>
PickerTraceTerminalizationRunResult DrivePickerTraceTerminalization(
        bool terminalAcknowledged,bool pendingEffectNone,
        bool runtimeKeyPresent,Release release,Ready ready,
        Finalize finalize) noexcept {
    PickerTraceTerminalizationRunResult result;
    result.terminalAcknowledged=terminalAcknowledged;
    result.pendingEffectNone=pendingEffectNone;
    result.runtimeKeyPresent=runtimeKeyPresent;
    if(!terminalAcknowledged){
        result.reason=
            PickerTraceTerminalizationReason::TerminalNotAcknowledged;
        return result;
    }
    if(!pendingEffectNone){
        result.reason=PickerTraceTerminalizationReason::PendingEffect;
        return result;
    }
    result.release.attempted=true;
    try {
        result.release.released=release(result.release);
    } catch(...) {
        result.release.threw=true;
        result.reason=
            PickerTraceTerminalizationReason::ReservationReleaseException;
        return result;
    }
    if(!result.release.released){
        result.reason=
            PickerTraceTerminalizationReason::ReservationNotReleased;
        return result;
    }
    if(!runtimeKeyPresent){
        result.reason=PickerTraceTerminalizationReason::RuntimeKeyMissing;
        return result;
    }
    try { result.ready=ready(); }
    catch(...) { result.ready=false; }
    if(!result.ready){
        result.reason=PickerTraceTerminalizationReason::RuntimeNotReady;
        return result;
    }
    try { result.finalized=finalize(); }
    catch(...) { result.finalized=false; }
    if(!result.finalized){
        result.reason=
            PickerTraceTerminalizationReason::FinalizeStateFailed;
        return result;
    }
    result.completed=true;
    result.reason=PickerTraceTerminalizationReason::Completed;
    return result;
}
```

The terminal classifier evaluates arguments in the written order. Delivery
precedence is shutdown drain, delayed timer, posted, timer armed, inline
fallback, durable kick, then none. The test table supplies one true route flag
at a time and a conflicting-all-true row to lock that precedence.

The serializer always writes `release_attempted`, but writes the first action,
retry action, and exception stage only when their separate availability flags
are true. Early terminal/pending guards serialize `release_attempted:false` and
no action names. A throw before the first decision serializes attempted/throw
without either action; a throw after the first decision includes only that
action and an exception stage when it was captured. Tests reject every
misleading default `resolved_absent` or `none` value.

`MapPickerTraceTerminalization` copies every availability/value from the run
result, including its three captured input preconditions, plus the separately
correlated incoming/retry delivery facts. It sets `terminalAllowed` only for
`completed`. Publishing with null is a no-op. Publishing with an observer
always emits the attempt first; when `terminalAllowed` and a terminal record is
provided, it emits terminal and then calls `flushBoundary` exactly once.
Observer callbacks are non-throwing. Tests pass a non-null sentinel terminal
even for failed runs to prove terminal and flush are suppressed, and assert the
successful order is attempt, terminal, flush.

Add a `PickerTraceReservationReleaseFacts*` output to
`ConsumeCheckpointAndReleaseMoveReservation`. Set `exceptionStage` immediately
before each potentially throwing first-decision/checkpoint/refind/
second-decision/erase boundary in a local current-stage variable. Set
`exceptionStageAvailable` only from a catch. Set each action's availability only
after that decision returns; never infer an unavailable action from its default.
Record retry, exact consume result, and thrown stage without serializing the
runtime key or record ID.

In `FinalizePickerRuntimeTransition`, capture the privacy-safe terminal snapshot
before `FinalizePickerTransition` swaps the transition to idle. Route the
existing guard/release/readiness/finalize ordering through
`DrivePickerTraceTerminalization`; the finalize callback retains the current
claim-marking, runtime-key swap/restore, and `FinalizePickerTransition` logic.
Return its result plus the terminal snapshot and the trace-only metadata from
Task 8 to the pump caller. Map that result at one exit boundary instead of
duplicating partial fields before each return.

- [ ] **Step 4: Emit delivery/no-progress and terminal outcome**

Add one trace-only `PickerTracePendingTerminalDelivery` beside the picker pump
state. Whenever terminal work is scheduled, store the typed scheduling route
with the current generation before an asynchronous return or inline pump. At
the start of a finalization attempt, increment its attempt number and consume
that generation's pending route exactly once as `incoming`; never read the
latest global scheduler result retroactively.

If the attempt makes no progress, retain its run result until retry scheduling
finishes. Put that newly returned route in the current event as `retry` and
store the same route as the next attempt's pending incoming delivery. Thus a
route may appear once as attempt N's retry and once as attempt N+1's incoming,
which is explicit causal continuity rather than accidental duplication. Reset
pending delivery on generation change, successful terminal publication, and
picker teardown; mismatch consumption also clears it.

After incoming/retry correlation is complete, map the run result, build the
optional terminal event only after runtime finalization succeeds, and call
`PublishPickerTraceTerminalization` with a session observer whose flush callback
delegates to `g_pickerTrace.flushBoundary()`. Determine outcome from the
pre-reset snapshot:

```cpp
const PickerTraceTerminalOutcome outcome=
    snapshot.cancelRequested
        ? PickerTraceTerminalOutcome::Cancelled
        : snapshot.failed
            ? PickerTraceTerminalOutcome::Failed
            : PickerTraceTerminalOutcome::Succeeded;
```

Copy `release.attempted` and all three availability flags before their values.
The mapper/serializer omits every unavailable action or stage regardless of its
internal default.

Perform diagnostic-only final target/popup/current readbacks after success;
record them, but do not feed them back into product state. If final readback
fails, terminal truth remains the existing state-machine outcome and the failed
raw read is still logged. Copy the stored
fixed rollback trigger and diagnostic code into `transition.terminal`; never
inspect or serialize `PickerTransition::diagnostic`.

- [ ] **Step 5: Run GREEN, build, and commit**

Run `.\build-test.bat` and `.\build.bat`; expect exit `0`.

```powershell
git add src/picker_trace.hpp src/picker_trace.cpp src/vde.cpp tests/vdtest.cpp
git commit -m "feat(trace): record terminal no-progress"
```

## Task 10: Lock runtime coverage, document reproduction, and verify the binary

**Files:**

- Modify: `tests/vdtest.cpp`
- Modify: `README.md:101-140`

- [ ] **Step 1: Add final runtime-boundary and privacy audits**

Add one narrow source audit that verifies typed trace calls exist at all required
anchors: target capture, picker open, enum begin/window/end, mouse down,
activation request/result, move begin/exception, lookup stages, API execution,
reducer, terminalization attempt, and transition terminal.

Add negative checks that the trace module and `vde.cpp` callsites never pass
`g_targetTitle`, `searchText`, `capturedTitle`, `diagnostic`, `runtimeKey`,
`pendingRecordId`, sessionstore objects, or full `ProcessSnapshot.image` to an
`emit` overload. Allow only `MakePickerTraceSafeImageBasename` after the final
path component is extracted.

This test supplements Tasks 2-9; it does not replace their behavioral checks.
It is intentionally added after wiring and is expected to pass immediately if
the preceding tasks are complete.

- [ ] **Step 2: Run the audit and correct any regression**

Run `.\build-test.bat`. Expected result is exit `0` with matching passed/total
counts. A named CHECK failure means an earlier runtime anchor or privacy guard
regressed; add only the missing typed call or remove the forbidden argument,
then rerun until exit `0`. Do not force an artificial RED change.

- [ ] **Step 3: Document the one-shot user protocol**

Add this README section after Usage:

````markdown
### Picker diagnostics

For an explicitly requested one-shot picker trace, first exit the running tray
instance, then start:

```text
build\vde.exe --trace-picker
```

Reproduce one picker opening and Ctrl+Click, then exit VDE from the tray. The
bounded JSONL file is stored under
`%LOCALAPPDATA%\VirtualDesktopsExtention\diagnostics`. Tracing is off during
ordinary launches and is never added to autostart. It does not record window
titles, searches, URLs, browser-session data, layout records, or full paths of
other applications.
````

- [ ] **Step 4: Run complete verification from a clean process state**

Confirm no `vde.exe` process started by development verification remains. Then
run:

```powershell
.\build-test.bat
.\build.bat
git diff --check
git status --short
```

Expected:

- tests exit `0` with identical numerator and denominator;
- build exits `0` and prints `Built build\vde.exe`;
- `git diff --check` prints nothing;
- status lists only the intended Task 10 files before commit.

- [ ] **Step 5: Request code review before the final commit**

Use `superpowers:requesting-code-review`. Resolve every correctness, privacy,
fail-open, retention, or behavior-equivalence finding. Rerun the complete test
and build commands after any change.

- [ ] **Step 6: Commit the verified diagnostic slice**

```powershell
git add README.md tests/vdtest.cpp src/vde.cpp src/picker_trace.hpp src/picker_trace.cpp
git commit -m "docs(trace): add picker reproduction steps"
```

- [ ] **Step 7: Produce the interactive handoff artifact**

Compute and record the SHA-256 of `build\vde.exe`. Ask the user to:

1. exit the current tray instance;
2. start that exact binary with `--trace-picker`;
3. keep a known missing non-Firefox window open;
4. open the picker once;
5. perform one Ctrl+Click to another desktop;
6. wait for settling and exit VDE from the tray.

Read the newest JSONL file. Do not infer a behavioral fix until the trace shows
the first enum rejection and the complete activation/move/terminal route. The
corrective fix, regression tests, build, and second manual verification are a
separate follow-up plan amendment driven by that evidence.

## Final self-review checklist

- Every design event has a producing task: yes, Tasks 5-9.
- Exact executable provenance without path text: Task 5.
- Every enum exit and every pre-move activation/guard: Tasks 6-7.
- Raw staged lookup/API results: Task 8.
- No-progress terminalization: Task 9.
- Caps, retention, reparse refusal, and fail-open I/O: Tasks 3-4.
- Typed privacy boundary and forbidden-source audit: Tasks 2 and 10.
- Trace/no-trace behavior equivalence: Tasks 6-9.
- Full automated build plus required live reproduction: Task 10.
- No general telemetry, settings toggle, upload, persistence expansion, or
  behavioral picker fix is introduced by this plan.
