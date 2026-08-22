# Picker Row Actions and Drag-and-Drop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add exact row activation and row Drag & Drop while preserving globally visible or pinned windows, keeping visual-only assignments local to one popup session, and retaining the current Firefox/Chrome/Edge-only persistence policy.

**Architecture:** Keep the existing transactional picker model and serialized transition reducer. Add a pure mobility policy header, immutable row hit snapshots, a pointer gesture FSM, explicit transition modes, and a session overlay map. Route every external target-window move through one fresh identity/route/pin guard; keep popup relocation on a separate explicitly exempt path.

**Tech Stack:** C++14, Win32, COM virtual-desktop interfaces, the existing header-only reducer/pure-helper style, the custom CHECK test harness in tests/vdtest.cpp, MSVC batch builds, and JSONL picker tracing.

---

**Approved design:** docs/superpowers/specs/2026-08-22-picker-row-actions-drag-drop-design.md

**ABI reference:** Microsoft TypeAgent’s Windows virtual desktop service documents the IApplicationView method order, pinned-apps service CLSID/IID, and read-only IsViewPinned/IsAppIdPinned calls: https://github.com/microsoft/TypeAgent/blob/main/dotnet/autoShell/Services/WindowsVirtualDesktopService.cs

## Execution precondition

The planning worktree already contains uncommitted foundation changes in:

- src/picker_state.hpp
- src/picker_trace.cpp
- src/picker_trace.hpp
- src/vde.cpp
- tests/vdtest.cpp

Those changes implement the preceding icon, active-row, general-window enumeration, and Ctrl+Click evidence fixes. The feature below touches all five files, so the executor must review and checkpoint that foundation before adding feature edits. Do not stash, reset, checkout, or overwrite it.

All commands in this plan run from F:\_VDESKTOP_FF\win-vde. Git commands use the repository-scoped safe-directory override because the sandbox account does not own the checkout.

## File structure

### New file

- src/window_mobility.hpp
  - Pure identity-independent route and mobility evidence.
  - Fail-closed physical/visual/reject decision.
  - Concrete-desktop membership helper used at persistence boundaries.

### Modified files

- src/picker_state.hpp
  - Row snapshots, pointer target priority, drag FSM, visual assignment state.
  - Transition modes/policies, visual publication effect, hide disposition.
  - Mode-specific success, cancellation, save-commit, and rollback rules.
- src/picker_trace.hpp
  - Typed intent, mode, gesture, route, mobility, and COM API vocabulary.
- src/picker_trace.cpp
  - Exact stable JSON names for the new enums and fields.
  - No AppUserModelID value field.
- src/vde.cpp
  - Optional pinned-apps COM adapter and RAII application-ID storage.
  - Fresh mobility probe and the only guarded target-move primitive.
  - Enumeration/model/overlay/paint wiring.
  - Mouse capture, drag dispatch, exact activation, transition effects.
- tests/vdtest.cpp
  - Pure, reducer, source-wiring, transaction, trace-schema, and regression
    tests. The test binary has no focused selector, so every RED/GREEN run uses
    the complete suite.

## Non-negotiable invariants

- PickerState.activeWindow, g_target, and g_targetTitle remain the popup’s
  captured active target. A clicked or dragged row is a separate action target.
- Only a freshly Verified exact identity with an Exact concrete source,
  CanViewMoveDesktops=TRUE, IsViewPinned=FALSE, and IsAppIdPinned=FALSE may
  reach a target-window move API.
- Positive global, view-pin, or app-pin evidence selects visual-only behavior.
  Unknown/conflicting evidence rejects; non-global Immovable rejects.
- Visual modes issue no target move, target readback, target save, or target
  rollback.
- RowMoveOnly issues no popup move, desktop switch, popup hide, or focus effect.
- Persistent assignment remains limited to Firefox, Chrome, and Edge.
- The overlay map and relocated picker model publish in one transaction.
- Transient popup relocation preserves overlays. Logical session termination,
  show abort, destruction, and application teardown clear overlays.
- Plain tile/title/empty-area click switches desktops without an explicit
  window activation. Plain row click may activate only its exact HWND.
- No code in this feature changes popup placement on the primary monitor.

## TDD execution rule

Every checkbox that adds a test function also adds that function’s explicit
call to the manual main() registration block before the RED run. Confirm the
RED output names the intended new assertion or missing symbol. A test function
that is not called from main() does not count as coverage. After each GREEN
run, require exit code 0 and inspect main() to confirm every named test is
called. The final N/N counter counts CHECK executions, including loop
iterations, rather than test functions.

Unless a task states a narrower expectation, every RED build-test.bat command
must exit 1 because the newly named symbol is missing or the newly registered
CHECK fails at its recorded tests\vdtest.cpp line. Abort if it fails for an
unrelated compile/test reason. Every GREEN build-test.bat command must exit 0
and end with N/N passed; every GREEN build.bat command must exit 0 and print
Built build\vde.exe.

## Index safety for every task commit

At the start of every task and again immediately before its git add command,
run:

~~~powershell
$preStaged=@(git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --name-only)
if($preStaged.Count -ne 0){
    throw "Pre-staged changes found; stop without changing the index."
}
~~~

Every git add command below names exact paths. Inspect
git diff --cached --name-only and stop if it contains any path not named by
that task; do not unstage or reset another user’s work. Every commit uses
git commit --only with the same exact path list, so unrelated staged paths
cannot enter the task commit even if external state changes after inspection.

## Task 0: Checkpoint the existing verified foundation

**Files:** the five pre-existing modified files listed under Execution
precondition.

- [ ] Inspect the exact inherited diff and status:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde status --short --branch
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff -- src/picker_state.hpp src/picker_trace.cpp src/picker_trace.hpp src/vde.cpp tests/vdtest.cpp
$staged=@(git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --name-only)
if($staged.Count -ne 0){
    throw "Pre-staged user changes found; stop without changing the index."
}
~~~

- [ ] Run the inherited suite and production build before changing it:

~~~powershell
.\build-test.bat
.\build.bat
~~~

Expected: build-test.bat exits 0 with every test passed; build.bat exits 0 and
produces build\vde.exe.

- [ ] If either command fails, stop Task 0 without staging anything. Invoke the
  systematic-debugging skill for the inherited failure, obtain a reviewed
  green foundation in a separate bounded fix, then restart Task 0 from the
  status/diff inspection.
- [ ] Stage exactly the inherited files, inspect the staged diff, then create
  one foundation commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/picker_state.hpp src/picker_trace.cpp src/picker_trace.hpp src/vde.cpp tests/vdtest.cpp
$expected=@(
    "src/picker_state.hpp",
    "src/picker_trace.cpp",
    "src/picker_trace.hpp",
    "src/vde.cpp",
    "tests/vdtest.cpp"
)
$actual=@(git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --name-only)
$difference=@(Compare-Object $expected $actual)
if($difference.Count -ne 0){
    throw "Unexpected staged path; stop without committing."
}
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --stat
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "fix: restore picker window evidence" -- src/picker_state.hpp src/picker_trace.cpp src/picker_trace.hpp src/vde.cpp tests/vdtest.cpp
~~~

- [ ] Confirm that no inherited source edit remains uncommitted:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde status --short
~~~

## Task 1: Add the pure route and mobility decision boundary

**Files:**

- Create: src/window_mobility.hpp
- Modify: tests/vdtest.cpp

- [ ] Add the header include and failing matrix tests near the existing picker
  route tests:

~~~cpp
static void test_target_mobility_fails_closed_and_positive_global_wins(){
    TargetMobilityEvidence exact;
    exact.desktopRoute=TargetDesktopRoute::Exact;
    exact.viewPinned=MobilityEvidence::Negative;
    exact.appPinned=MobilityEvidence::Negative;
    exact.canMove=MobilityEvidence::Positive;
    TargetMobilityDecision decision=DecideTargetMobility(exact);
    CHECK(decision.mobility==TargetMobility::Movable);
    CHECK(decision.disposition==TargetMoveDisposition::Physical);

    TargetMobilityEvidence viewPinned=exact;
    viewPinned.viewPinned=MobilityEvidence::Positive;
    decision=DecideTargetMobility(viewPinned);
    CHECK(decision.mobility==TargetMobility::ViewPinned);
    CHECK(decision.disposition==TargetMoveDisposition::VisualOnly);

    TargetMobilityEvidence appPinned=exact;
    appPinned.appPinned=MobilityEvidence::Positive;
    decision=DecideTargetMobility(appPinned);
    CHECK(decision.mobility==TargetMobility::AppPinned);
    CHECK(decision.disposition==TargetMoveDisposition::VisualOnly);

    TargetMobilityEvidence global=exact;
    global.desktopRoute=TargetDesktopRoute::GloballyVisible;
    global.viewPinned=MobilityEvidence::Unknown;
    decision=DecideTargetMobility(global);
    CHECK(decision.mobility==TargetMobility::Indeterminate);
    CHECK(decision.disposition==TargetMoveDisposition::VisualOnly);

    global.viewPinned=MobilityEvidence::Positive;
    decision=DecideTargetMobility(global);
    CHECK(decision.mobility==TargetMobility::ViewPinned);
    CHECK(decision.disposition==TargetMoveDisposition::VisualOnly);

    TargetMobilityEvidence unknown=exact;
    unknown.appPinned=MobilityEvidence::Unknown;
    decision=DecideTargetMobility(unknown);
    CHECK(decision.mobility==TargetMobility::Indeterminate);
    CHECK(decision.disposition==TargetMoveDisposition::Reject);

    TargetMobilityEvidence immovable=exact;
    immovable.canMove=MobilityEvidence::Negative;
    decision=DecideTargetMobility(immovable);
    CHECK(decision.mobility==TargetMobility::Immovable);
    CHECK(decision.disposition==TargetMoveDisposition::Reject);
}

static void test_target_desktop_route_requires_concrete_snapshot_membership(){
    CHECK(DecideTargetDesktopRoute(
        S_OK,true,true,E_FAIL,false)==TargetDesktopRoute::Exact);
    CHECK(DecideTargetDesktopRoute(
        S_OK,false,false,S_OK,true)==
        TargetDesktopRoute::GloballyVisible);
    CHECK(DecideTargetDesktopRoute(
        S_OK,true,false,S_OK,true)==
        TargetDesktopRoute::GloballyVisible);
    CHECK(DecideTargetDesktopRoute(
        E_FAIL,false,false,S_OK,true)==
        TargetDesktopRoute::Indeterminate);
    CHECK(DecideTargetDesktopRoute(
        E_FAIL,false,false,E_FAIL,false)==
        TargetDesktopRoute::Indeterminate);
}
~~~

- [ ] Register both functions in main().
- [ ] Run RED:

~~~powershell
.\build-test.bat
~~~

Expected RED: compilation fails because the mobility types and helpers do not
exist.

- [ ] Create src/window_mobility.hpp with this public vocabulary:

~~~cpp
#pragma once

#include "window_identity.hpp"
#include "str_util.hpp"

#include <cstdint>
#include <vector>

enum class MobilityEvidence : uint8_t {
    Unknown,
    Negative,
    Positive
};

enum class TargetDesktopRoute : uint8_t {
    Exact,
    GloballyVisible,
    Indeterminate
};

enum class TargetMobility : uint8_t {
    Movable,
    ViewPinned,
    AppPinned,
    Immovable,
    Indeterminate
};

enum class TargetMoveDisposition : uint8_t {
    Physical,
    VisualOnly,
    Reject
};

struct TargetMobilityEvidence {
    TargetDesktopRoute desktopRoute=TargetDesktopRoute::Indeterminate;
    MobilityEvidence viewPinned=MobilityEvidence::Unknown;
    MobilityEvidence appPinned=MobilityEvidence::Unknown;
    MobilityEvidence canMove=MobilityEvidence::Unknown;
};

struct TargetMobilityDecision {
    TargetMobility mobility=TargetMobility::Indeterminate;
    TargetMoveDisposition disposition=TargetMoveDisposition::Reject;
};

inline MobilityEvidence MobilityEvidenceFromBoolean(
        HRESULT result,BOOL value) noexcept {
    if(FAILED(result)) return MobilityEvidence::Unknown;
    return value ? MobilityEvidence::Positive : MobilityEvidence::Negative;
}

inline TargetDesktopRoute DecideTargetDesktopRoute(
        HRESULT desktopRead,bool desktopNonzero,bool desktopInSnapshot,
        HRESULT membershipRead,bool onCurrentDesktop) noexcept {
    if(SUCCEEDED(desktopRead) && desktopNonzero && desktopInSnapshot)
        return TargetDesktopRoute::Exact;
    if(SUCCEEDED(desktopRead) &&
       SUCCEEDED(membershipRead) && onCurrentDesktop)
        return TargetDesktopRoute::GloballyVisible;
    return TargetDesktopRoute::Indeterminate;
}
~~~

- [ ] Implement the closed decision:

~~~cpp
inline TargetMobilityDecision DecideTargetMobility(
    const TargetMobilityEvidence& evidence) noexcept {
    TargetMobilityDecision result;
    if(evidence.viewPinned==MobilityEvidence::Positive){
        result.mobility=TargetMobility::ViewPinned;
        result.disposition=TargetMoveDisposition::VisualOnly;
        return result;
    }
    if(evidence.desktopRoute==TargetDesktopRoute::GloballyVisible){
        result.disposition=TargetMoveDisposition::VisualOnly;
        return result;
    }
    if(evidence.appPinned==MobilityEvidence::Positive){
        result.mobility=TargetMobility::AppPinned;
        result.disposition=TargetMoveDisposition::VisualOnly;
        return result;
    }
    if(evidence.desktopRoute!=TargetDesktopRoute::Exact ||
       evidence.viewPinned==MobilityEvidence::Unknown ||
       evidence.appPinned==MobilityEvidence::Unknown ||
       evidence.canMove==MobilityEvidence::Unknown)
        return result;
    if(evidence.canMove==MobilityEvidence::Negative){
        result.mobility=TargetMobility::Immovable;
        return result;
    }
    if(evidence.viewPinned==MobilityEvidence::Negative &&
       evidence.appPinned==MobilityEvidence::Negative &&
       evidence.canMove==MobilityEvidence::Positive){
        result.mobility=TargetMobility::Movable;
        result.disposition=TargetMoveDisposition::Physical;
    }
    return result;
}
~~~
- [ ] Keep route and mobility independent in state and trace. A global route
  with unknown pin/move evidence is
  route=GloballyVisible, mobility=Indeterminate, disposition=VisualOnly; never
  add GloballyVisible to the TargetMobility enum.
- [ ] Add tests for every pairwise conflict: positive pin plus failed companion
  query is VisualOnly; exact plus failed CanViewMoveDesktops is Reject; global
  plus failed pin queries remains VisualOnly.
- [ ] Run GREEN:

~~~powershell
.\build-test.bat
~~~

- [ ] Commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/window_mobility.hpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "feat: classify target window mobility" -- src/window_mobility.hpp tests/vdtest.cpp
~~~

## Task 2: Separate transition mode, popup-active target, and action target

**Files:**

- Modify: src/picker_state.hpp
- Modify: tests/vdtest.cpp

- [ ] Add failing policy-table and authorization tests beside the transition
  fixture tests:

~~~cpp
static void test_picker_transition_policy_table_is_closed(){
    PickerTransitionPolicy move=PickerPolicyFor(
        PickerTransitionMode::MoveAndFollow);
    CHECK(move.requiresCapturedActive && move.movesTarget);
    CHECK(move.movesPopup && move.switchesDesktop);
    CHECK(move.persistsTarget && move.restoresPopupFocus);
    CHECK(!move.publishesVisual && move.cancelDismissesSession);

    PickerTransitionPolicy row=PickerPolicyFor(
        PickerTransitionMode::RowMoveOnly);
    CHECK(!row.requiresCapturedActive && row.movesTarget);
    CHECK(!row.movesPopup && !row.switchesDesktop);
    CHECK(row.persistsTarget && !row.publishesVisual);
    CHECK(!row.restoresPopupFocus && !row.cancelDismissesSession);

    PickerTransitionPolicy visualFollow=PickerPolicyFor(
        PickerTransitionMode::VisualAndFollow);
    CHECK(visualFollow.requiresCapturedActive);
    CHECK(!visualFollow.movesTarget && visualFollow.movesPopup);
    CHECK(visualFollow.switchesDesktop && !visualFollow.persistsTarget);
    CHECK(visualFollow.publishesVisual &&
          visualFollow.restoresPopupFocus);

    PickerTransitionPolicy visual=PickerPolicyFor(
        PickerTransitionMode::VisualOnly);
    CHECK(!visual.requiresCapturedActive && !visual.movesTarget);
    CHECK(!visual.movesPopup && !visual.switchesDesktop);
    CHECK(!visual.persistsTarget && visual.publishesVisual);
}

static void test_picker_row_action_preserves_popup_active_target(){
    PickerState state=PickerTransitionFixture(501);
    const WindowIdentityKey active=IK(0x1111,71,9001);
    const WindowIdentityKey row=IK(0x2222,72,9002);
    state.activeWindow=active;
    state.transition.mode=PickerTransitionMode::RowMoveOnly;
    state.transition.popupActiveTarget=active;
    state.transition.target=row;
    CHECK(PickerTransitionTargetsAuthorized(
        state,state.transition));
    CHECK(SameIdentity(state.activeWindow,active));
    CHECK(SameIdentity(state.transition.target,row));

    state.transition.mode=PickerTransitionMode::MoveAndFollow;
    CHECK(!PickerTransitionTargetsAuthorized(
        state,state.transition));
}

static void test_picker_begin_preconditions_are_mode_specific(){
    PickerState state=PickerTransitionFixture(502);
    state.transition.mode=PickerTransitionMode::RowMoveOnly;
    state.selectedDesktop=state.transition.currentOrigin;
    CHECK(!GuidEq(
        state.selectedDesktop,state.transition.destination));
    CHECK(PickerTransitionBeginPreconditions(
        state,state.transition));

    state.transition.mode=PickerTransitionMode::VisualOnly;
    state.transition.targetOrigin=GUID{};
    state.transition.popupOrigin=GUID{};
    state.transition.currentOrigin=GUID{};
    CHECK(PickerTransitionBeginPreconditions(
        state,state.transition));

    state.transition.mode=PickerTransitionMode::MoveAndFollow;
    CHECK(!PickerTransitionBeginPreconditions(
        state,state.transition));
}
~~~

- [ ] Run RED:

~~~powershell
.\build-test.bat
~~~

- [ ] Add the explicit vocabulary before PickerTransition:

~~~cpp
enum class PickerTransitionMode {
    MoveAndFollow,
    RowMoveOnly,
    VisualAndFollow,
    VisualOnly
};

enum class PickerHideDisposition {
    None,
    TransientRelocate,
    DismissSession
};

struct PickerTransitionPolicy {
    bool requiresCapturedActive=false;
    bool movesTarget=false;
    bool movesPopup=false;
    bool switchesDesktop=false;
    bool persistsTarget=false;
    bool publishesVisual=false;
    bool restoresPopupFocus=false;
    bool cancelDismissesSession=false;
};
~~~

- [ ] Implement the four-case policy:

~~~cpp
inline PickerTransitionPolicy PickerPolicyFor(
        PickerTransitionMode mode) noexcept {
    PickerTransitionPolicy policy;
    switch(mode){
    case PickerTransitionMode::MoveAndFollow:
        policy.requiresCapturedActive=true;
        policy.movesTarget=true;
        policy.movesPopup=true;
        policy.switchesDesktop=true;
        policy.persistsTarget=true;
        policy.restoresPopupFocus=true;
        policy.cancelDismissesSession=true;
        break;
    case PickerTransitionMode::RowMoveOnly:
        policy.movesTarget=true;
        policy.persistsTarget=true;
        break;
    case PickerTransitionMode::VisualAndFollow:
        policy.requiresCapturedActive=true;
        policy.movesPopup=true;
        policy.switchesDesktop=true;
        policy.publishesVisual=true;
        policy.restoresPopupFocus=true;
        policy.cancelDismissesSession=true;
        break;
    case PickerTransitionMode::VisualOnly:
        policy.publishesVisual=true;
        break;
    }
    return policy;
}
~~~
- [ ] Append these fields with compatibility-preserving defaults:

~~~cpp
// PickerTransition
PickerTransitionMode mode=PickerTransitionMode::MoveAndFollow;
WindowIdentityKey popupActiveTarget;
PickerHideDisposition pendingHideDisposition=
    PickerHideDisposition::None;

// PickerEffect, after desktop so four-field aggregate initializers survive
PickerHideDisposition hideDisposition=
    PickerHideDisposition::None;
~~~
- [ ] Update PickerTransition::swap, PickerState::swap tests, transition
  fixtures, and the traced/plain reducer field comparator.
- [ ] After the PickerState definition, implement
  PickerTransitionTargetsAuthorized so:
  - the action target must always be a valid full identity;
  - modes with requiresCapturedActive require a valid popupActiveTarget and
    require both popupActiveTarget==PickerState.activeWindow and
    target==popupActiveTarget;
  - RowMoveOnly and VisualOnly may preserve an empty active snapshot or use a
    different popup-active identity; it is carried unchanged and does not
    authorize or reject the independent row action.

~~~cpp
inline bool PickerTransitionTargetsAuthorized(
        const PickerState& state,
        const PickerTransition& transition) noexcept {
    if(!SameIdentity(transition.target,transition.target))
        return false;
    const PickerTransitionPolicy policy=
        PickerPolicyFor(transition.mode);
    if(!policy.requiresCapturedActive)
        return true;
    const bool hasPopupActive=SameIdentity(
        transition.popupActiveTarget,
        transition.popupActiveTarget);
    return hasPopupActive &&
           SameIdentity(state.activeWindow,
                        transition.popupActiveTarget) &&
           SameIdentity(transition.target,
                        transition.popupActiveTarget);
}
~~~

- [ ] Replace the reducer’s one-size-fits-all Begin guard with
  PickerTransitionBeginPreconditions:
  - MoveAndFollow requires captured-active authorization,
    selectedDesktop==destination, concrete target/current/destination origins,
    and a valid managed or sticky popup route;
  - RowMoveOnly requires a verified action target plus concrete, different
    targetOrigin/destination, but does not require selectedDesktop,
    currentOrigin, or popupOrigin;
  - VisualAndFollow requires captured-active authorization,
    selectedDesktop==destination, concrete current/destination, and a valid
    popup route, but allows zero/sentinel targetOrigin;
  - VisualOnly requires a verified action target and concrete destination, but
    allows destination!=selectedDesktop and zero target/popup/current origins.
  Preserve the existing nonzero generation, pending-effect, and reservation
  integrity checks for every controlled mode.
- [ ] Keep MoveAndFollow as the fixture default and prove its existing effect
  order has not changed.
- [ ] Run GREEN:

~~~powershell
.\build-test.bat
~~~

- [ ] Commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/picker_state.hpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "refactor: separate picker action targets" -- src/picker_state.hpp tests/vdtest.cpp
~~~

## Task 3: Add session visual assignments with atomic model publication

**Files:**

- Modify: src/picker_state.hpp
- Modify: tests/vdtest.cpp

- [ ] Add failing exact-row and strong-transaction tests:

~~~cpp
static void test_picker_visual_assignment_and_model_publish_atomically(){
    const GUID oldDesktop=G(
        L"{231A0000-0000-0000-0000-000000000001}");
    const GUID newDesktop=G(
        L"{231A0000-0000-0000-0000-000000000002}");
    std::vector<int> model{7};
    std::vector<int> paintCache{70};
    PickerState state;
    PickerVisualAssignment oldAssignment;
    oldAssignment.baseDesktop=oldDesktop;
    oldAssignment.destination=oldDesktop;
    state.visualAssignments["old-runtime"]=oldAssignment;

    PickerVisualAssignmentMutation mutation;
    mutation.kind=PickerVisualMutationKind::Upsert;
    mutation.runtimeKey="selected-runtime";
    mutation.baseDesktop=oldDesktop;
    mutation.destination=newDesktop;

    CHECK(!RunPickerVisualRefreshTransaction(
        model,paintCache,state,mutation,
        [&](std::vector<int>& stagedModel,
            std::vector<int>& stagedPaintCache,
            PickerState& stagedState)->bool {
            CHECK(GuidEq(
                stagedState.visualAssignments.at(
                    "selected-runtime").destination,
                newDesktop));
            stagedModel.push_back(8);
            stagedPaintCache.push_back(80);
            return false;
        }));
    CHECK((model==std::vector<int>{7}));
    CHECK((paintCache==std::vector<int>{70}));
    CHECK(state.visualAssignments.count("selected-runtime")==0);
    CHECK(GuidEq(
        state.visualAssignments.at("old-runtime").baseDesktop,
        oldDesktop));

    CHECK(RunPickerVisualRefreshTransaction(
        model,paintCache,state,mutation,
        [&](std::vector<int>& stagedModel,
            std::vector<int>& stagedPaintCache,
            PickerState& stagedState)->bool {
            CHECK(GuidEq(
                stagedState.visualAssignments.at(
                    "selected-runtime").destination,
                newDesktop));
            stagedModel.push_back(9);
            stagedPaintCache.push_back(90);
            return true;
        }));
    CHECK((model==std::vector<int>{9}));
    CHECK((paintCache==std::vector<int>{90}));
    CHECK(GuidEq(
        state.visualAssignments.at(
            "selected-runtime").destination,newDesktop));
}

static void test_picker_visual_assignment_is_exact_runtime_only(){
    const WindowIdentityKey first=IK(0x1001,77,900);
    const WindowIdentityKey sibling=IK(0x1002,77,900);
    PickerState state;
    PickerVisualAssignmentMutation mutation;
    mutation.kind=PickerVisualMutationKind::Upsert;
    mutation.runtimeKey=RuntimeKey(first);
    mutation.baseDesktop=G(
        L"{231A0000-0000-0000-0000-000000000001}");
    mutation.destination=G(
        L"{231A0000-0000-0000-0000-000000000003}");
    CHECK(StagePickerVisualAssignmentMutation(state,mutation));
    CHECK(state.visualAssignments.count(RuntimeKey(first))==1);
    CHECK(state.visualAssignments.count(RuntimeKey(sibling))==0);
}
~~~

- [ ] Add tests for invalid empty runtime key, zero destination, exact-key erase,
  first upsert, reassignment preserving the original baseDesktop, destination
  equal to base erasing the entry, state swap, builder false, builder throw,
  paint-builder failure, and EndPickerVisualSession.
- [ ] Run RED:

~~~powershell
.\build-test.bat
~~~

- [ ] Add these value types before PickerTransition so both
  PickerTransition.visualMutation and the later PickerState map have complete
  types:

~~~cpp
struct PickerVisualAssignment {
    GUID baseDesktop={0};
    GUID destination={0};
};

using PickerVisualAssignments=
    std::map<std::string,PickerVisualAssignment>;

enum class PickerVisualMutationKind {
    None,
    Upsert,
    Erase
};

struct PickerVisualAssignmentMutation {
    PickerVisualMutationKind kind=PickerVisualMutationKind::None;
    std::string runtimeKey;
    GUID baseDesktop={0};
    GUID destination={0};
};
~~~

- [ ] Add PickerVisualAssignments visualAssignments to PickerState and include
  it in PickerState::swap. Place StagePickerVisualAssignmentMutation after the
  complete PickerState definition, then implement the copied-map mutation:

~~~cpp
inline bool StagePickerVisualAssignmentMutation(
        PickerState& state,
        const PickerVisualAssignmentMutation& mutation) noexcept {
    if(mutation.kind==PickerVisualMutationKind::None)
        return true;
    if(mutation.runtimeKey.empty())
        return false;
    if(mutation.kind==PickerVisualMutationKind::Upsert &&
       (GuidIsZero(mutation.baseDesktop) ||
        GuidIsZero(mutation.destination)))
        return false;
    try {
        PickerVisualAssignments staged=state.visualAssignments;
        if(mutation.kind==PickerVisualMutationKind::Erase){
            staged.erase(mutation.runtimeKey);
        } else if(mutation.kind==PickerVisualMutationKind::Upsert){
            PickerVisualAssignment assignment;
            auto existing=staged.find(mutation.runtimeKey);
            assignment.baseDesktop=existing==staged.end()
                ? mutation.baseDesktop : existing->second.baseDesktop;
            if(GuidIsZero(assignment.baseDesktop))
                return false;
            assignment.destination=mutation.destination;
            if(GuidEq(assignment.baseDesktop,
                      assignment.destination))
                staged.erase(mutation.runtimeKey);
            else
                staged[mutation.runtimeKey]=assignment;
        } else {
            return false;
        }
        state.visualAssignments.swap(staged);
        return true;
    } catch(...) {
        return false;
    }
}
~~~
- [ ] Implement EndPickerVisualSession after PickerState. Place
  RunPickerVisualRefreshTransaction after the existing
  RunPickerRefreshTransaction/SwapPickerState definitions:

~~~cpp
inline void EndPickerVisualSession(PickerState& state) noexcept {
    PickerVisualAssignments empty;
    state.visualAssignments.swap(empty);
}

template<class Model,class Cache,class Build>
inline bool RunPickerVisualRefreshTransaction(
        Model& publishedModel,Cache& publishedCache,
        PickerState& publishedState,
        const PickerVisualAssignmentMutation& mutation,
        Build&& build) noexcept {
    try {
        Model stagedModel;
        Cache stagedCache;
        PickerState stagedState=PreservePickerUi(publishedState);
        if(!StagePickerVisualAssignmentMutation(
                stagedState,mutation))
            return false;
        if(!build(stagedModel,stagedCache,stagedState))
            return false;
        static_assert(noexcept(
            publishedModel.swap(stagedModel)),
            "picker model publication must be noexcept");
        static_assert(noexcept(
            publishedCache.swap(stagedCache)),
            "picker paint publication must be noexcept");
        publishedModel.swap(stagedModel);
        publishedCache.swap(stagedCache);
        SwapPickerState(publishedState,stagedState);
        return true;
    } catch(...) {
        return false;
    }
}
~~~

- [ ] Add visualMutation to PickerTransition and include it in swap/equality
  coverage. Do not publish it directly from input handling.
- [ ] Run GREEN:

~~~powershell
.\build-test.bat
~~~

- [ ] Commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/picker_state.hpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "feat: stage picker visual assignments" -- src/picker_state.hpp tests/vdtest.cpp
~~~

## Task 4: Implement RowMoveOnly reducer success, save, cancel, and rollback

**Files:**

- Modify: src/picker_state.hpp
- Modify: tests/vdtest.cpp

- [ ] Add a failing complete happy-path test:

~~~cpp
static void test_picker_row_move_only_has_exact_order_and_keeps_active(){
    PickerState state=PickerTransitionFixture(501);
    const WindowIdentityKey active=IK(0x1111,71,9001);
    const WindowIdentityKey row=IK(0x2222,72,9002);
    state.activeWindow=active;
    state.transition.popupActiveTarget=active;
    state.transition.target=row;
    state.transition.runtimeKey=RuntimeKey(row);
    state.transition.mode=PickerTransitionMode::RowMoveOnly;

    std::vector<PickerEffectKind> order;
    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=state.transition.generation;
    PickerEffect effect=AdvancePickerTransition(state,begin);
    order.push_back(effect.kind);
    CHECK(effect.kind==PickerEffectKind::MoveTarget);

    PickerObservation moved=PickerObservationFor(
        effect,PickerEvent::ApiCompleted);
    moved.identity=PickerIdentityValidity::Match;
    moved.apiInvoked=true;
    moved.apiAccepted=true;
    effect=AdvancePickerTransition(state,moved);
    order.push_back(effect.kind);
    CHECK(effect.kind==PickerEffectKind::ReadTarget);

    PickerObservation read=PickerObservationFor(
        effect,PickerEvent::ReadbackCompleted);
    read.identity=PickerIdentityValidity::Match;
    read.targetRead=PickerReadValidity::Valid;
    read.actualTargetDesktop=state.transition.destination;
    effect=AdvancePickerTransition(state,read);
    order.push_back(effect.kind);
    CHECK(effect.kind==PickerEffectKind::SaveExactTarget);

    PickerObservation saved=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    saved.identity=PickerIdentityValidity::Match;
    saved.saveStatus=PopupSaveStatus::NotTracked;
    effect=AdvancePickerTransition(state,saved);
    order.push_back(effect.kind);
    CHECK(effect.kind==PickerEffectKind::Refresh);

    PickerObservation refreshed=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    refreshed.apiAccepted=true;
    effect=AdvancePickerTransition(state,refreshed);
    CHECK(effect.kind==PickerEffectKind::None);
    CHECK(state.transition.terminalAcknowledged);
    CHECK(SameIdentity(state.activeWindow,active));

    const std::vector<PickerEffectKind> expected={
        PickerEffectKind::MoveTarget,
        PickerEffectKind::ReadTarget,
        PickerEffectKind::SaveExactTarget,
        PickerEffectKind::Refresh
    };
    CHECK(order==expected);
}
~~~

- [ ] Add a forbidden-effect assertion over the collected order for
  ValidateTarget, MovePopup, ReadPopup, SwitchDesktop, ReadCurrent, Hide,
  ShowAndFocus, and ReportFailure.
- [ ] Add a save-result table:
  - Saved/None commits;
  - NotTracked/None commits;
  - `PopupSaveStatus::Failed` / `PopupSaveFailure::FlushFailed` commits because the in-memory publication occurred;
  - every other Failed result enters target-only rollback.
- [ ] Add cancellation cases before issue, after MoveTarget, during delayed
  ReadTarget, during SaveExactTarget, after non-published save failure, and
  after verified rollback.
- [ ] Add committed-move Refresh failure cases for Saved, NotTracked/None, and
  `PopupSaveStatus::Failed` / `PopupSaveFailure::FlushFailed`. They must never roll the target back after publication;
  they request authoritative rebuild and keep the exact row non-actionable
  until that rebuild publishes.
- [ ] Assert every RowMoveOnly cancellation contains no Hide, MovePopup,
  ReadPopup, SwitchDesktop, ReadCurrent, or ShowAndFocus.
- [ ] Seed popupMayHaveMoved and switchMayHaveChanged with true in a corruption
  test and prove RowMoveOnly still emits only target rollback plus Refresh.
- [ ] Run RED:

~~~powershell
.\build-test.bat
~~~

- [ ] Add:

~~~cpp
inline bool PickerRowMoveSaveCommits(
        PopupSaveStatus status,PopupSaveFailure failure) noexcept {
    return (status==PopupSaveStatus::NotTracked &&
            failure==PopupSaveFailure::None) ||
           PickerSavePublishesOperationLifetimeClaim(status,failure);
}
~~~

- [ ] Add a negative case for NotTracked with every non-None failure; it must
  fail closed and take the non-published rollback path.

- [ ] Make Begin mode-aware. RowMoveOnly begins at MoveTarget and never requires
  action target==activeWindow.
- [ ] After a verified ReadTarget on the destination, emit SaveExactTarget;
  after a committing save result emit Refresh; after a non-published failure
  clear the provisional commit cutoff and enter target-only rollback.
- [ ] If Refresh fails after a committing save result, keep
  commitCutoffReached=true, forbid rollback, and terminate only after runtime
  authoritative rebuild succeeds or invalidates the affected row snapshot.
- [ ] Add a RowMoveOnly rollback branch:
  MoveTarget(targetOrigin), ReadTarget, Refresh, terminal. Do not reuse the
  full target→popup→current rollback chain.
- [ ] Treat SaveExactTarget already issued as non-discardable. Wait for its
  acknowledgement, then decide commit versus rollback from the result.
- [ ] Leave currentDesktop, selectedDesktop, selectedIndex, activeWindow, and
  popupActiveTarget unchanged throughout the mode.
- [ ] Run GREEN:

~~~powershell
.\build-test.bat
~~~

- [ ] Commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/picker_state.hpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "feat: add row-only move transition" -- src/picker_state.hpp tests/vdtest.cpp
~~~

## Task 5: Implement visual reducer modes and popup-session hide semantics

**Files:**

- Modify: src/picker_state.hpp
- Modify: src/picker_trace.hpp
- Modify: src/picker_trace.cpp
- Modify: tests/vdtest.cpp

- [ ] Add PublishVisualAssignment to PickerEffectKind and add a matching
  PickerPhase::PublishVisualAssignment. The reducer sets that phase before
  emitting the effect and accepts only its matching generation/serial
  EffectCompleted acknowledgement. Add failing exact sequence tests:

~~~cpp
static void test_picker_visual_only_has_no_external_effects(){
    PickerState state=PickerTransitionFixture(601);
    state.transition.mode=PickerTransitionMode::VisualOnly;
    state.transition.visualMutation.kind=
        PickerVisualMutationKind::Upsert;
    state.transition.visualMutation.runtimeKey=
        state.transition.runtimeKey;
    state.transition.visualMutation.baseDesktop=
        state.transition.targetOrigin;
    state.transition.visualMutation.destination=
        state.transition.destination;

    PickerObservation begin;
    begin.event=PickerEvent::Begin;
    begin.generation=state.transition.generation;
    PickerEffect effect=AdvancePickerTransition(state,begin);
    CHECK(effect.kind==PickerEffectKind::ValidateTarget);

    PickerObservation valid=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    valid.identity=PickerIdentityValidity::Match;
    valid.apiAccepted=true;
    effect=AdvancePickerTransition(state,valid);
    CHECK(effect.kind==PickerEffectKind::PublishVisualAssignment);

    PickerObservation published=PickerObservationFor(
        effect,PickerEvent::EffectCompleted);
    published.apiAccepted=true;
    effect=AdvancePickerTransition(state,published);
    CHECK(effect.kind==PickerEffectKind::None);
    CHECK(state.transition.terminalAcknowledged);
}
~~~

- [ ] Add managed VisualAndFollow expected order:

~~~text
ValidateTarget
MovePopup
ReadPopup
ValidateTarget
SwitchDesktop
ReadCurrent
ReadPopup
PublishVisualAssignment
ShowAndFocus
~~~

- [ ] Add sticky VisualAndFollow expected order:

~~~text
ValidateTarget
SwitchDesktop
ReadCurrent
PublishVisualAssignment
ShowAndFocus
~~~

- [ ] Assert both visual modes contain zero MoveTarget, ReadTarget,
  SaveExactTarget, and target rollback effects. Deliberately seed
  targetMayHaveMoved=true and prove cancellation still cannot emit target
  rollback.
- [ ] Add identity-loss, switch-failure, popup-failure, cancellation-before-
  publication, publication-failure, and shutdown-drain cases.
- [ ] Add hide-disposition tests:
  DismissSession clears assignments; TransientRelocate preserves them; a
  controlled dismissing cancel carries DismissSession.
- [ ] Run RED:

~~~powershell
.\build-test.bat
~~~

- [ ] Route PublishVisualAssignment as EffectCompleted. During shutdown drain
  route it to AcknowledgeWithoutUi only when it has not been issued; an issued
  atomic publication must be acknowledged.
- [ ] Implement VisualOnly as ValidateTarget → PublishVisualAssignment →
  terminal. It never hides or reports a user-facing failure.
- [ ] Implement VisualAndFollow with the managed/sticky sequences above.
  Publication occurs only after successful destination readback.
- [ ] On VisualAndFollow failure, rollback only popup/current effects that the
  mode policy allows. Use TransientRelocate for internal visibility cycles and
  DismissSession only when the logical popup session actually ends.
- [ ] Implement:

~~~cpp
inline bool PickerHideEndsVisualSession(
        PickerHideDisposition disposition) noexcept {
    return disposition==PickerHideDisposition::DismissSession;
}
~~~

- [ ] Extend effect emission, acknowledgement, cancellation, shutdown routing,
  transition swap, and traced/plain reducer comparisons for the new effect and
  disposition.
- [ ] In the same RED/GREEN cycle, add publish_visual_assignment to
  PickerTraceEffectKindName, add publish_visual_assignment to
  PickerTracePhaseName, and update both strict expected-name/schema tests. No
  serialized enum may temporarily fall through to unknown.
- [ ] Run GREEN:

~~~powershell
.\build-test.bat
~~~

- [ ] Commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/picker_state.hpp src/picker_trace.hpp src/picker_trace.cpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "feat: add visual picker transitions" -- src/picker_state.hpp src/picker_trace.hpp src/picker_trace.cpp tests/vdtest.cpp
~~~

## Task 6: Add immutable row hits and the pure drag gesture FSM

**Files:**

- Modify: src/picker_state.hpp
- Modify: tests/vdtest.cpp

- [ ] Move PickerRowAdmission and PickerDesktopTileRoute before the pointer
  types, include window_mobility.hpp, then write failing hit-priority tests:

~~~cpp
static void test_picker_row_hit_priority_is_exact(){
    PickerFooterActivation none;
    PickerPointerActivation row=ResolvePickerPointerActivation(
        none,false,false,3,4);
    CHECK(row.target==PickerPointerTarget::Row);
    CHECK(row.rowIndex==3 && row.tileIndex==4);

    CHECK(ResolvePickerPointerActivation(
        none,true,true,3,4).target==
        PickerPointerTarget::ClearSearch);
}

static void test_picker_row_snapshot_separates_presentation_and_drag(){
    PickerRowActionSnapshot row;
    row.hwnd=0x1111;
    row.admission=PickerRowAdmission::DisplayOnly;
    row.tileIndex=2;
    row.displayedDesktop=G(
        L"{251A0000-0000-0000-0000-000000000001}");
    row.modelGeneration=5;
    row.rowLayoutEpoch=9;
    CHECK(PickerRowPresentationCurrent(row,5,9));
    CHECK(!PickerRowActionableForDrag(row,5,9));

    row.identity=IK(0x1111,7,70);
    row.admission=PickerRowAdmission::Verified;
    CHECK(PickerRowActionableForDrag(row,5,9));
    CHECK(!PickerRowPresentationCurrent(row,6,9));
    CHECK(!PickerRowActionableForDrag(row,5,10));
}
~~~

- [ ] Retain the existing CurrentDesktopFallback enumerator for indeterminate
  DisplayOnly presentation, add GloballyVisibleCurrentDesktopFallback, and add
  the conversion here, after both enum definitions:

~~~cpp
enum class PickerRowAdmission {
    Skip,
    DisplayOnly,
    Verified
};

enum class PickerDesktopTileRoute {
    Exact,
    CurrentDesktopFallback,
    GloballyVisibleCurrentDesktopFallback,
    Skip
};

inline TargetDesktopRoute TargetRouteFromPickerTileRoute(
        PickerDesktopTileRoute route) noexcept {
    switch(route){
    case PickerDesktopTileRoute::Exact:
        return TargetDesktopRoute::Exact;
    case PickerDesktopTileRoute::GloballyVisibleCurrentDesktopFallback:
        return TargetDesktopRoute::GloballyVisible;
    case PickerDesktopTileRoute::CurrentDesktopFallback:
    case PickerDesktopTileRoute::Skip:
        return TargetDesktopRoute::Indeterminate;
    }
    return TargetDesktopRoute::Indeterminate;
}
~~~

- [ ] Add Row to PickerPointerTarget, rowIndex to PickerPointerActivation, and
  resolve in this order with the complete replacement:

~~~cpp
enum class PickerPointerTarget {
    None,
    Footer,
    ClearSearch,
    Search,
    Row,
    Tile
};

struct PickerPointerActivation {
    PickerPointerTarget target=PickerPointerTarget::None;
    PickerFooterActivation footer;
    int rowIndex=-1;
    int tileIndex=-1;
};

inline PickerPointerActivation ResolvePickerPointerActivation(
        const PickerFooterActivation& footer,bool clearSearchHit,
        bool searchHit,int rowIndex,int tileIndex) noexcept {
    PickerPointerActivation activation;
    if(footer.consumed){
        activation.target=PickerPointerTarget::Footer;
        activation.footer=footer;
    } else if(clearSearchHit){
        activation.target=PickerPointerTarget::ClearSearch;
    } else if(searchHit){
        activation.target=PickerPointerTarget::Search;
    } else if(rowIndex>=0 && tileIndex>=0){
        activation.target=PickerPointerTarget::Row;
        activation.rowIndex=rowIndex;
        activation.tileIndex=tileIndex;
    } else if(tileIndex>=0){
        activation.target=PickerPointerTarget::Tile;
        activation.tileIndex=tileIndex;
    }
    return activation;
}
~~~
- [ ] Separate allocation-free action data from presentation strings so WndProc
  and mouse-capture state never copy a std::string/std::wstring:

~~~cpp
struct PickerRowActionSnapshot {
    int tileIndex=-1;
    size_t windowIndex=0;
    uintptr_t hwnd=0;
    GUID displayedDesktop={0};
    GUID observedDesktop={0};
    GUID baseDesktop={0};
    WindowIdentityKey identity;
    PickerRowAdmission admission=PickerRowAdmission::DisplayOnly;
    TargetDesktopRoute desktopRoute=
        TargetDesktopRoute::Indeterminate;
    TargetMobility mobility=TargetMobility::Indeterminate;
    bool visuallyAssigned=false;
    uint64_t modelGeneration=0;
    uint64_t rowLayoutEpoch=0;
    uint64_t paintGeneration=0;
};

struct PickerRowHitSnapshot {
    RECT hitRect={0,0,0,0};
    RECT textRect={0,0,0,0};
    std::wstring fullTitle;
    std::string runtimeKey;
    bool truncated=false;
    PickerRowActionSnapshot action;
};
~~~

The published hit snapshot still owns the title and runtime key needed by
painting/tooltips/model lookup. Button down copies only action, which is
fixed-size. RuntimeKey(action.identity) is recomputed inside the dispatcher’s
exception boundary after fresh identity validation.

- [ ] Add separate presentation/click and verified-drag predicates:

~~~cpp
inline bool PickerRowPresentationCurrent(
        const PickerRowActionSnapshot& row,uint64_t modelGeneration,
        uint64_t rowLayoutEpoch) noexcept {
    return row.hwnd!=0 && row.tileIndex>=0 &&
           !GuidIsZero(row.displayedDesktop) &&
           row.modelGeneration==modelGeneration &&
           row.rowLayoutEpoch==rowLayoutEpoch;
}

inline bool PickerRowActionableForDrag(
        const PickerRowActionSnapshot& row,uint64_t modelGeneration,
        uint64_t rowLayoutEpoch) noexcept {
    return PickerRowPresentationCurrent(
           row,modelGeneration,rowLayoutEpoch) &&
           row.admission==PickerRowAdmission::Verified &&
           row.hwnd==row.identity.hwnd &&
           SameIdentity(row.identity,row.identity);
}
~~~

A current DisplayOnly stationary plain click reaches the dispatch-time identity
upgrade. Only failed upgrade degrades it to TileSwitch. A drag requires
PickerRowActionableForDrag. DisplayOnly is still armed/captured so a stationary
plain click can be distinguished, but crossing the threshold cancels it before
mobility probing, reservation, or transition publication. A stationary Ctrl
row click needs a current destination tile but authorizes the separate popup
active target, not the row identity.

- [ ] Replace DispatchPickerPointerActivation with:

~~~cpp
template<class OnFooter,class OnClearSearch,class OnSearch,
         class OnRow,class OnTile>
inline bool DispatchPickerPointerActivation(
        const PickerPointerActivation& activation,
        OnFooter&& onFooter,OnClearSearch&& onClearSearch,
        OnSearch&& onSearch,OnRow&& onRow,OnTile&& onTile) noexcept {
    try {
        switch(activation.target){
        case PickerPointerTarget::Footer:
            onFooter(activation.footer);
            return true;
        case PickerPointerTarget::ClearSearch:
            onClearSearch();
            return true;
        case PickerPointerTarget::Search:
            onSearch();
            return true;
        case PickerPointerTarget::Row:
            onRow(activation.rowIndex,activation.tileIndex);
            return true;
        case PickerPointerTarget::Tile:
            onTile(activation.tileIndex);
            return true;
        case PickerPointerTarget::None:
            break;
        }
    } catch(...) {
        return activation.target!=PickerPointerTarget::None;
    }
    return false;
}
~~~

- [ ] Keep source compatibility until Task 11 by adding overloads with the old
  four-argument Resolve signature and four-callback Dispatch signature. The
  Resolve overload calls the new function with rowIndex=-1. The Dispatch
  overload delegates to the five-callback function with an onRow callback that
  performs no action; remove both overloads in Task 11 when vde.cpp is migrated
  to explicit row dispatch.

~~~cpp
inline PickerPointerActivation ResolvePickerPointerActivation(
        const PickerFooterActivation& footer,bool clearSearchHit,
        bool searchHit,int tileIndex) noexcept {
    return ResolvePickerPointerActivation(
        footer,clearSearchHit,searchHit,-1,tileIndex);
}

template<class OnFooter,class OnClearSearch,class OnSearch,class OnTile>
inline bool DispatchPickerPointerActivation(
        const PickerPointerActivation& activation,
        OnFooter&& onFooter,OnClearSearch&& onClearSearch,
        OnSearch&& onSearch,OnTile&& onTile) noexcept {
    return DispatchPickerPointerActivation(
        activation,
        std::forward<OnFooter>(onFooter),
        std::forward<OnClearSearch>(onClearSearch),
        std::forward<OnSearch>(onSearch),
        [](int,int) noexcept {},
        std::forward<OnTile>(onTile));
}
~~~

- [ ] Add failing drag tests for X threshold, Y threshold, same-row stationary
  click, release over another row, valid different-tile drop, same displayed
  tile no-op, actual source erase request, outside cancel, epoch mismatch,
  Escape, capture loss, and Ctrl captured on DOWN.

~~~cpp
static PickerRowActionSnapshot GestureRow(
        PickerRowAdmission admission=PickerRowAdmission::Verified){
    PickerRowActionSnapshot row;
    row.tileIndex=2;
    row.windowIndex=3;
    row.hwnd=0x1111;
    row.displayedDesktop=G(
        L"{251A0000-0000-0000-0000-000000000001}");
    row.baseDesktop=row.displayedDesktop;
    row.identity=IK(0x1111,7,70);
    row.admission=admission;
    row.modelGeneration=5;
    row.rowLayoutEpoch=9;
    return row;
}

static void test_picker_drag_threshold_and_drop_are_deterministic(){
    PickerPointerGesture gesture;
    PickerRowActionSnapshot row=GestureRow();
    CHECK(ArmPickerRowGesture(
        gesture,row,POINT{100,100},true,5,9));
    CHECK(UpdatePickerRowGesture(
        gesture,POINT{101,101},4,4,5,9,4)==
        PickerGestureAction::None);
    CHECK(gesture.phase==PickerPointerPhase::Armed);
    CHECK(UpdatePickerRowGesture(
        gesture,POINT{102,100},4,4,5,9,4)==
        PickerGestureAction::DragStarted);
    CHECK(gesture.phase==PickerPointerPhase::Dragging);
    PickerGestureResolution drop=ResolvePickerRowButtonUp(
        gesture,nullptr,true,5,9);
    CHECK(drop.action==PickerGestureAction::Drop);
    CHECK(drop.ctrlAtDown);
    CHECK(drop.row.hwnd==row.hwnd && drop.dropTileIndex==4);
}

static void test_picker_display_only_can_click_but_cannot_drag(){
    PickerPointerGesture gesture;
    PickerRowActionSnapshot row=GestureRow(
        PickerRowAdmission::DisplayOnly);
    row.identity=WindowIdentityKey{};
    CHECK(ArmPickerRowGesture(
        gesture,row,POINT{100,100},false,5,9));
    PickerGestureResolution click=ResolvePickerRowButtonUp(
        gesture,&row,true,5,9);
    CHECK(click.action==PickerGestureAction::Click);

    CHECK(ArmPickerRowGesture(
        gesture,row,POINT{100,100},false,5,9));
    CHECK(UpdatePickerRowGesture(
        gesture,POINT{102,100},4,4,5,9,4)==
        PickerGestureAction::Cancel);
    CHECK(gesture.phase==PickerPointerPhase::Idle);
}

static void test_picker_stale_plain_click_degrades_but_ctrl_cancels(){
    PickerPointerGesture gesture;
    PickerRowActionSnapshot row=GestureRow();
    CHECK(ArmPickerRowGesture(
        gesture,row,POINT{100,100},false,5,9));
    CHECK(ResolvePickerRowButtonUp(
        gesture,&row,true,6,9).action==
        PickerGestureAction::SwitchOnly);
    CHECK(ArmPickerRowGesture(
        gesture,row,POINT{100,100},true,5,9));
    CHECK(ResolvePickerRowButtonUp(
        gesture,&row,true,6,9).action==
        PickerGestureAction::Cancel);
}
~~~
- [ ] Use this threshold contract:

~~~cpp
inline bool PickerDragThresholdCrossed(
        POINT down,POINT current,int dragWidth,int dragHeight) noexcept {
    const long long width=std::max<long long>(1,dragWidth);
    const long long height=std::max<long long>(1,dragHeight);
    const long long left=width/2;
    const long long right=width-left;
    const long long top=height/2;
    const long long bottom=height-top;
    const long long dx=
        static_cast<long long>(current.x)-down.x;
    const long long dy=
        static_cast<long long>(current.y)-down.y;
    return dx < -left || dx >= right ||
           dy < -top || dy >= bottom;
}
~~~

- [ ] Add boundary tests for drag metrics 0 and 1, odd metrics, and POINT
  coordinates at LONG_MIN and LONG_MAX.

- [ ] Add PickerPointerPhase { Idle, Armed, Dragging } and this result enum:

~~~cpp
enum class PickerPointerPhase {
    Idle,
    Armed,
    Dragging
};

enum class PickerGestureAction {
    None,
    DragStarted,
    Click,
    SwitchOnly,
    Drop,
    NoOp,
    Cancel
};
~~~

- [ ] Add the gesture value:

~~~cpp
struct PickerPointerGesture {
    PickerPointerPhase phase=PickerPointerPhase::Idle;
    PickerRowActionSnapshot row;
    POINT down={0,0};
    bool ctrlAtDown=false;
    int dropTileIndex=-1;
    uint64_t rowLayoutEpoch=0;
};

struct PickerGestureResolution {
    PickerGestureAction action=PickerGestureAction::None;
    PickerRowActionSnapshot row;
    bool ctrlAtDown=false;
    int dropTileIndex=-1;
};
~~~

- [ ] Add the complete gesture helpers:

~~~cpp
inline void CancelPickerRowGesture(
        PickerPointerGesture& gesture) noexcept {
    gesture.phase=PickerPointerPhase::Idle;
    gesture.row=PickerRowActionSnapshot{};
    gesture.down=POINT{0,0};
    gesture.ctrlAtDown=false;
    gesture.dropTileIndex=-1;
    gesture.rowLayoutEpoch=0;
}

inline bool SamePickerRowActionSnapshot(
        const PickerRowActionSnapshot& left,
        const PickerRowActionSnapshot& right) noexcept {
    return left.hwnd!=0 && left.hwnd==right.hwnd &&
           left.tileIndex==right.tileIndex &&
           left.windowIndex==right.windowIndex &&
           left.modelGeneration==right.modelGeneration &&
           left.rowLayoutEpoch==right.rowLayoutEpoch;
}

inline bool ArmPickerRowGesture(
        PickerPointerGesture& gesture,
        const PickerRowActionSnapshot& row,POINT down,
        bool ctrlAtDown,uint64_t modelGeneration,
        uint64_t rowLayoutEpoch) noexcept {
    if(!PickerRowPresentationCurrent(
            row,modelGeneration,rowLayoutEpoch))
        return false;
    PickerPointerGesture staged;
    staged.phase=PickerPointerPhase::Armed;
    staged.row=row;
    staged.down=down;
    staged.ctrlAtDown=ctrlAtDown;
    staged.rowLayoutEpoch=rowLayoutEpoch;
    gesture=staged;
    return true;
}

inline PickerGestureAction UpdatePickerRowGesture(
        PickerPointerGesture& gesture,POINT current,
        int dragWidth,int dragHeight,
        uint64_t modelGeneration,uint64_t rowLayoutEpoch,
        int candidateDropTile) noexcept {
    if(gesture.phase==PickerPointerPhase::Idle)
        return PickerGestureAction::None;
    if(!PickerRowPresentationCurrent(
            gesture.row,modelGeneration,rowLayoutEpoch)){
        CancelPickerRowGesture(gesture);
        return PickerGestureAction::Cancel;
    }
    if(gesture.phase==PickerPointerPhase::Armed){
        if(!PickerDragThresholdCrossed(
                gesture.down,current,dragWidth,dragHeight))
            return PickerGestureAction::None;
        if(!PickerRowActionableForDrag(
                gesture.row,modelGeneration,rowLayoutEpoch)){
            CancelPickerRowGesture(gesture);
            return PickerGestureAction::Cancel;
        }
        gesture.phase=PickerPointerPhase::Dragging;
        gesture.dropTileIndex=candidateDropTile;
        return PickerGestureAction::DragStarted;
    }
    gesture.dropTileIndex=candidateDropTile;
    return PickerGestureAction::None;
}

inline PickerGestureResolution ResolvePickerRowButtonUp(
        const PickerPointerGesture& gesture,
        const PickerRowActionSnapshot* releaseRow,
        bool destinationExists,uint64_t modelGeneration,
        uint64_t rowLayoutEpoch) noexcept {
    PickerGestureResolution result;
    result.row=gesture.row;
    result.ctrlAtDown=gesture.ctrlAtDown;
    result.dropTileIndex=gesture.dropTileIndex;
    if(gesture.phase==PickerPointerPhase::Idle){
        result.action=PickerGestureAction::Cancel;
        return result;
    }
    const bool presentationCurrent=PickerRowPresentationCurrent(
        gesture.row,modelGeneration,rowLayoutEpoch);
    if(gesture.phase==PickerPointerPhase::Armed){
        if(!presentationCurrent){
            result.action=!gesture.ctrlAtDown && destinationExists
                ? PickerGestureAction::SwitchOnly
                : PickerGestureAction::Cancel;
            return result;
        }
        result.action=releaseRow &&
            SamePickerRowActionSnapshot(gesture.row,*releaseRow)
                ? PickerGestureAction::Click
                : PickerGestureAction::Cancel;
        return result;
    }
    if(!PickerRowActionableForDrag(
            gesture.row,modelGeneration,rowLayoutEpoch) ||
       !destinationExists || gesture.dropTileIndex<0){
        result.action=PickerGestureAction::Cancel;
        return result;
    }
    result.action=gesture.dropTileIndex==gesture.row.tileIndex
        ? PickerGestureAction::NoOp
        : PickerGestureAction::Drop;
    return result;
}
~~~

A drag always retains gesture.row.identity; later Ctrl state cannot replace it.
Runtime copies PickerGestureResolution, calls CancelPickerRowGesture, then calls
ReleaseCapture, and only afterward dispatches the copied resolution.
- [ ] Add rowLayoutEpoch to PickerState and swap/equality coverage. This epoch
  changes only for model/filter/scroll/layout publication, never for hover or
  drop-highlight repaint.
- [ ] Run RED, implement the helpers, then run GREEN and verify the temporary
  compatibility overloads keep production compiling:

~~~powershell
.\build-test.bat
.\build.bat
~~~

- [ ] Commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/picker_state.hpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "feat: model picker row gestures" -- src/picker_state.hpp tests/vdtest.cpp
~~~

## Task 7: Add the optional read-only pin service and mobility probe

**Files:**

- Modify: src/vde.cpp
- Modify: src/picker_trace.hpp
- Modify: src/picker_trace.cpp
- Modify: tests/vdtest.cpp

- [ ] Add source-wiring RED tests that require:
  - service CLSID B5A399E7-1C87-46B8-88E9-FC5747B171BD;
  - interface IID 4CE81583-1E4C-4632-A621-07A53543148F;
  - optional acquisition independent of mandatory startup;
  - IsViewPinned, GetAppUserModelId, IsAppIdPinned, and
    CanViewMoveDesktops calls;
  - CoTaskMemFree on every non-null returned application ID;
  - no PinView, UnpinView, PinAppID, or UnpinAppID invocation;
  - no trace field containing an application-ID value.
- [ ] Add pure adapter-outcome tests that convert HRESULT/BOOL results into
  MobilityEvidence and cover optional-service absence as Unknown.
- [ ] Run RED:

~~~powershell
.\build-test.bat
~~~

- [ ] Expand the current empty IApplicationView definition through the
  GetAppUserModelId vtable slot in this exact order:

~~~cpp
struct __declspec(uuid("372E1D3B-38D3-42E4-A15B-8AB2B178F513"))
IApplicationView : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetFocus()=0;
    virtual HRESULT STDMETHODCALLTYPE SwitchTo()=0;
    virtual HRESULT STDMETHODCALLTYPE TryInvokeBack(void*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetThumbnailWindow(HWND*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetMonitor(void**)=0;
    virtual HRESULT STDMETHODCALLTYPE GetVisibility(int*)=0;
    virtual HRESULT STDMETHODCALLTYPE SetCloak(int,int)=0;
    virtual HRESULT STDMETHODCALLTYPE GetPosition(REFIID,void**)=0;
    virtual HRESULT STDMETHODCALLTYPE SetPosition(void*)=0;
    virtual HRESULT STDMETHODCALLTYPE InsertAfterWindow(HWND)=0;
    virtual HRESULT STDMETHODCALLTYPE GetExtendedFramePosition(RECT*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetAppUserModelId(PWSTR*)=0;
};
~~~

- [ ] Define IVirtualDesktopPinnedApps with all six vtable slots in order, but
  invoke only the two read methods:

~~~cpp
struct __declspec(uuid("4CE81583-1E4C-4632-A621-07A53543148F"))
IVirtualDesktopPinnedApps : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsAppIdPinned(
        PCWSTR,BOOL*)=0;
    virtual HRESULT STDMETHODCALLTYPE PinAppID(PCWSTR)=0;
    virtual HRESULT STDMETHODCALLTYPE UnpinAppID(PCWSTR)=0;
    virtual HRESULT STDMETHODCALLTYPE IsViewPinned(
        IApplicationView*,BOOL*)=0;
    virtual HRESULT STDMETHODCALLTYPE PinView(IApplicationView*)=0;
    virtual HRESULT STDMETHODCALLTYPE UnpinView(IApplicationView*)=0;
};
~~~

- [ ] Add g_pinnedApps. Query it after mandatory services are ready, but ignore
  query failure for startup. Release it independently and set it to nullptr
  during teardown.

~~~cpp
static const GUID kCLSID_VirtualDesktopPinnedApps=
    {0xB5A399E7,0x1C87,0x46B8,
     {0x88,0xE9,0xFC,0x57,0x47,0xB1,0x71,0xBD}};
static const GUID kIID_IVirtualDesktopPinnedApps=
    {0x4CE81583,0x1E4C,0x4632,
     {0xA6,0x21,0x07,0xA5,0x35,0x43,0x14,0x8F}};

static IVirtualDesktopPinnedApps* g_pinnedApps=nullptr;

static void TryInitializePinnedApps() noexcept {
    if(!g_shell || g_pinnedApps) return;
    IVirtualDesktopPinnedApps* candidate=nullptr;
    HRESULT result=E_NOINTERFACE;
    try {
        result=g_shell->QueryService(
            kCLSID_VirtualDesktopPinnedApps,
            kIID_IVirtualDesktopPinnedApps,
            reinterpret_cast<void**>(&candidate));
    } catch(...) {
        result=E_FAIL;
    }
    if(SUCCEEDED(result) && candidate)
        g_pinnedApps=candidate;
    else if(candidate)
        candidate->Release();
}

static void ReleasePinnedApps() noexcept {
    IVirtualDesktopPinnedApps* prior=g_pinnedApps;
    g_pinnedApps=nullptr;
    if(prior) prior->Release();
}
~~~
- [ ] Add an RAII owner whose destructor calls CoTaskMemFree for every non-null
  PWSTR returned by GetAppUserModelId. Treat failed HRESULT, null pointer, or
  empty string as Unknown.

~~~cpp
class ScopedCoTaskMemString {
public:
    ScopedCoTaskMemString() noexcept = default;
    ~ScopedCoTaskMemString() noexcept {
        if(value_) CoTaskMemFree(value_);
    }
    ScopedCoTaskMemString(const ScopedCoTaskMemString&)=delete;
    ScopedCoTaskMemString& operator=(
        const ScopedCoTaskMemString&)=delete;
    ScopedCoTaskMemString(ScopedCoTaskMemString&&)=delete;
    ScopedCoTaskMemString& operator=(
        ScopedCoTaskMemString&&)=delete;

    PWSTR* out() noexcept { return &value_; }
    PCWSTR get() const noexcept { return value_; }
    bool usable() const noexcept {
        return value_ && value_[0]!=L'\0';
    }

private:
    PWSTR value_=nullptr;
};
~~~
- [ ] Add TargetMobilityProbeFacts containing only HRESULTs, BOOL results,
  invocation flags, route, and final mobility. Do not store the application ID.

~~~cpp
struct TargetMobilityProbeFacts {
    WindowIdentityRecapture identity=
        WindowIdentityRecapture::Indeterminate;
    TargetDesktopRoute route=TargetDesktopRoute::Indeterminate;
    HRESULT viewPinnedResult=E_NOINTERFACE;
    HRESULT appIdResult=E_NOINTERFACE;
    HRESULT appPinnedResult=E_NOINTERFACE;
    HRESULT canMoveResult=E_NOINTERFACE;
    BOOL viewPinned=FALSE;
    BOOL appPinned=FALSE;
    int canMove=0;
    bool viewPinnedInvoked=false;
    bool appIdInvoked=false;
    bool appPinnedInvoked=false;
    bool canMoveInvoked=false;
    TargetMobilityDecision decision;
};

static TargetMobilityDecision QueryTargetWindowMobility(
        const WindowIdentityKey& expected,
        TargetDesktopRoute knownRoute,IApplicationView* view,
        TargetMobilityProbeFacts& facts) noexcept {
    facts=TargetMobilityProbeFacts{};
    facts.route=knownRoute;
    facts.identity=RecaptureGenericWindowIdentity(expected);
    if(facts.identity!=WindowIdentityRecapture::Match || !view)
        return facts.decision;

    TargetMobilityEvidence evidence;
    evidence.desktopRoute=knownRoute;
    if(g_pinnedApps){
        facts.viewPinnedInvoked=true;
        try {
            facts.viewPinnedResult=g_pinnedApps->IsViewPinned(
                view,&facts.viewPinned);
        } catch(...) {
            facts.viewPinnedResult=E_FAIL;
        }
    }
    evidence.viewPinned=MobilityEvidenceFromBoolean(
        facts.viewPinnedResult,facts.viewPinned);

    ScopedCoTaskMemString appId;
    facts.appIdInvoked=true;
    try {
        facts.appIdResult=view->GetAppUserModelId(appId.out());
    } catch(...) {
        facts.appIdResult=E_FAIL;
    }
    if(SUCCEEDED(facts.appIdResult) && appId.usable() &&
       g_pinnedApps){
        facts.appPinnedInvoked=true;
        try {
            facts.appPinnedResult=g_pinnedApps->IsAppIdPinned(
                appId.get(),&facts.appPinned);
        } catch(...) {
            facts.appPinnedResult=E_FAIL;
        }
    }
    evidence.appPinned=MobilityEvidenceFromBoolean(
        facts.appPinnedResult,facts.appPinned);

    if(g_vdmi){
        facts.canMoveInvoked=true;
        try {
            facts.canMoveResult=g_vdmi->CanViewMoveDesktops(
                view,&facts.canMove);
        } catch(...) {
            facts.canMoveResult=E_FAIL;
        }
    }
    evidence.canMove=MobilityEvidenceFromBoolean(
        facts.canMoveResult,facts.canMove!=0);
    facts.decision=DecideTargetMobility(evidence);
    return facts.decision;
}
~~~

The caller obtains the exact IApplicationView through g_avc and owns its
ScopedComPtr for the entire probe. The function never returns, stores, or emits
appId.get().
- [ ] Add trace API kinds and JSON names:

~~~text
get_app_user_model_id
is_view_pinned
is_app_id_pinned
can_view_move_desktops
~~~

- [ ] Update the strict trace schema and privacy tests. Log HRESULT and BOOL
  evidence only.
- [ ] Run GREEN and the production build:

~~~powershell
.\build-test.bat
.\build.bat
~~~

- [ ] Commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/vde.cpp src/picker_state.hpp src/picker_trace.hpp src/picker_trace.cpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "feat: query virtual desktop pin state" -- src/vde.cpp src/picker_trace.hpp src/picker_trace.cpp tests/vdtest.cpp
~~~

## Task 8: Centralize every external target move behind a fresh mobility guard

**Files:**

- Modify: src/window_mobility.hpp
- Modify: src/vde.cpp
- Modify: tests/vdtest.cpp

- [ ] Add failing tests for concrete desktop membership:

~~~cpp
static void test_concrete_desktop_membership_rejects_sentinels(){
    const GUID first=G(
        L"{241A0000-0000-0000-0000-000000000001}");
    const GUID sentinel=G(
        L"{241A0000-0000-0000-0000-000000000099}");
    std::vector<GUID> desktops{first};
    const auto guidOf=[](const GUID& value)->const GUID& {
        return value;
    };
    CHECK(ConcreteDesktopExists(first,desktops,guidOf));
    CHECK(!ConcreteDesktopExists(GUID{},desktops,guidOf));
    CHECK(!ConcreteDesktopExists(sentinel,desktops,guidOf));
}
~~~

- [ ] Add source-wiring RED tests that inspect the IssueWindowMove,
  IssuePickerWindowMove, popup-binding repair, manual/CLI, AutoFix, and rollback
  source sections. Require every external target route to call one guarded
  primitive immediately before the API.
- [ ] Require the final direct-call inventory:
  - exactly one g_vdmi->MoveViewToDesktop occurrence, inside the guard;
  - exactly one target g_vdmDoc->MoveWindowToDesktop occurrence, inside the
    guard;
  - exactly two exempt popup g_vdmDoc->MoveWindowToDesktop occurrences:
    popup transition relocation and initial popup-binding repair.
- [ ] Add a decision/executor test proving no move callback runs for
  GloballyVisible, ViewPinned, AppPinned, Immovable, Indeterminate, identity
  loss, vanished source desktop, or vanished destination.
- [ ] Run RED:

~~~powershell
.\build-test.bat
~~~

- [ ] Add the generic concrete snapshot helper to window_mobility.hpp:

~~~cpp
template<class Desktops,class GuidOf>
inline bool ConcreteDesktopExists(
        const GUID& observed,const Desktops& desktops,
        GuidOf&& guidOf) noexcept {
    if(GuidIsZero(observed)) return false;
    try {
        for(const auto& desktop : desktops){
            const GUID& candidate=guidOf(desktop);
            if(!GuidIsZero(candidate) &&
               GuidEq(candidate,observed))
                return true;
        }
    } catch(...) {
        return false;
    }
    return false;
}
~~~

For vector<DeskRec>, pass a projection that returns desktop.guid. When a real
DeskRec.index is needed, continue to call the existing GetDesktopIndexByGuid;
do not treat vector position as a desktop index.

- [ ] Add the guarded result:

~~~cpp
struct TargetMoveIssueResult {
    HRESULT result=E_FAIL;
    bool invoked=false;
    WindowIdentityRecapture identity=
        WindowIdentityRecapture::Indeterminate;
    TargetDesktopRoute desktopRoute=
        TargetDesktopRoute::Indeterminate;
    TargetMobilityDecision mobility;
};
~~~

- [ ] Add these exact runtime entry points before the existing IssueWindowMove:

~~~cpp
static TargetMoveIssueResult IssueGuardedTargetMove(
    const WindowIdentityKey& expected,
    TargetDesktopRoute observedRoute,
    const GUID& destinationGuid,
    PickerTraceDesktopLookupUse lookupUse,
    uint64_t generation,uint64_t effectSerial) noexcept;

static HRESULT IssuePickerPopupMove(
    HWND popup,const GUID& destinationGuid,bool& invoked,
    PickerTraceDesktopLookupUse lookupUse,
    uint64_t generation,uint64_t effectSerial) noexcept;
~~~

Implement the popup helper as the only controlled-transition popup call:

~~~cpp
static HRESULT IssuePickerPopupMove(
        HWND popup,const GUID& destinationGuid,bool& invoked,
        PickerTraceDesktopLookupUse lookupUse,
        uint64_t generation,uint64_t effectSerial) noexcept {
    invoked=false;
    if(!popup || popup!=g_main || GuidIsZero(destinationGuid) ||
       !g_vdmDoc)
        return E_INVALIDARG;
    PickerTraceDesktopLookupContext lookup;
    lookup.trace=&g_pickerTrace;
    lookup.use=lookupUse;
    lookup.generation=generation;
    lookup.effectSerial=effectSerial;
    lookup.requested=destinationGuid;
    if(GetDesktopIndexByGuid(destinationGuid,&lookup)<0)
        return E_INVALIDARG;
    HRESULT result=E_FAIL;
    try {
        invoked=true;
        result=g_vdmDoc->MoveWindowToDesktop(
            popup,destinationGuid);
    } catch(...) {
        result=E_FAIL;
    }
    EmitPickerTraceHResult(
        PickerTraceApiKind::MoveWindowToDesktop,
        generation,effectSerial,result,invoked,popup,
        destinationGuid);
    return result;
}
~~~

- [ ] Implement IssueGuardedTargetMove(expected, knownRoute, destination,
  trace context). Immediately before the move API it must, in this order:
  verify destination exists in the current desktop snapshot; recapture the full
  identity; re-read the source and current-membership route; require the fresh
  Exact source is concrete and differs from destination; acquire the exact
  view; repeat both pin queries and CanViewMoveDesktops; require
  Physical/Movable/Exact; recapture identity once more; set invoked=true; issue
  the one target move API.
- [ ] Add same-source rejection tests through AutoFix, manual, CLI, picker
  forward move, and every rollback entry point; invoked must remain false.
- [ ] Split popup movement into IssuePickerPopupMove. It may move g_main and is
  exempt from the target pin guard, but it retains destination validation,
  trace emission, and popup readback.
- [ ] Replace the legacy automatic/manual/CLI adapter with:

~~~cpp
static HRESULT IssueWindowMove(
        const MoveRuntimeBinding& binding,
        WindowIdentityRecapture& identity) {
    TargetMoveIssueResult issued=IssueGuardedTargetMove(
        IdentityOf(binding.window),
        TargetDesktopRoute::Indeterminate,
        binding.destination,
        PickerTraceDesktopLookupUse::MoveEntryDestination,
        0,0);
    identity=issued.identity;
    return issued.result;
}
~~~

- [ ] In ExecutePickerEffect(MoveTarget), call IssueGuardedTargetMove with
  transition.target, the freshly staged observed route, effect.desktop,
  the effect’s lookup use, generation, and serial. Copy result.invoked,
  result.identity, result.result acceptance, result.desktopRoute, and
  result.mobility into the observation/trace. In MovePopup, call
  IssuePickerPopupMove(g_main, effect.desktop, invoked, lookupUse, generation,
  serial); never pass g_main through IssueGuardedTargetMove.
- [ ] Replace IssueWindowMove and the picker MoveTarget effect with the guarded
  primitive. Route AutoFix, manual, CLI, and every target rollback through it.
  Do not route MovePopup or initial popup-binding repair through it.
- [ ] At SaveObservedApp, final lifecycle observation, manual save, CLI save,
  provisional-origin creation, and post-move publication, replace
  nonzero-GUID-only acceptance with current concrete-snapshot membership.
  A global/sentinel observation must set needsReconcile or fail publication; it
  must never become a saved destination.
- [ ] Keep already stored records intact unless a normal reconciliation rule
  changes them; this task guards new observations and publications.
- [ ] Run GREEN and inspect direct calls:

~~~powershell
.\build-test.bat
rg -n "MoveViewToDesktop|MoveWindowToDesktop" src\vde.cpp
.\build.bat
~~~

- [ ] Commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/window_mobility.hpp src/vde.cpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "fix: guard all external window moves" -- src/window_mobility.hpp src/vde.cpp tests/vdtest.cpp
~~~

## Task 9: Publish global route evidence, overlays, and unified row geometry

**Files:**

- Modify: src/vde.cpp
- Modify: src/picker_state.hpp
- Modify: tests/vdtest.cpp

- [ ] Add RED model tests for:
  - exact route retains observedDesktop and displayedDesktop;
  - a zero or non-snapshot desktop plus successful current membership becomes
    GloballyVisible rather than being rejected before membership;
  - failed/off-current membership remains Indeterminate and non-actionable;
  - an overlay relocates only its exact runtime key;
  - a sibling HWND from the same process remains in its actual tile;
  - search/filter/model refresh retains valid overlays;
  - missing runtime identity or destination prunes the entry;
  - dropping back to the stored concrete baseDesktop erases only that entry,
    including when observedDesktop is zero/sentinel;
  - a failed model or paint-cache build changes none of published tiles,
    visual assignments, or row hits.
- [ ] Add source-wiring RED tests proving BuildModel applies/prunes the staged
  overlay after enumeration and before PopulatePickerFilteredRows, stages the
  corresponding row paint inputs in the same publication bundle, and refresh
  of an open popup does not assign transition.target to activeWindow.
- [ ] Replace RowRec with a record that owns PickerRowHitSnapshot. Add RED
  geometry/source tests requiring:
  - hitRect covers icon and text;
  - textRect alone drives tooltip hit;
  - Paint reads the same snapshot rectangles used by hit testing;
  - snapshot.action carries modelGeneration, rowLayoutEpoch, and
    paintGeneration.
- [ ] Run RED:

~~~powershell
.\build-test.bat
~~~

- [ ] Extend WinItem with observedDesktop, baseDesktop, displayedDesktop,
  TargetDesktopRoute, and TargetMobility. baseDesktop is the concrete tile
  chosen before overlay application; never overwrite observedDesktop or
  baseDesktop when applying or updating an overlay.
- [ ] In EnumAll, perform exact tile lookup first. If no exact tile exists,
  including zero/sentinel GUIDs, call IsWindowOnCurrentVirtualDesktop. A
  successful desktop read that returned zero/sentinel plus successful TRUE
  membership becomes GloballyVisible on the current tile. A failed desktop
  read plus TRUE membership may remain visible through the existing
  DisplayOnly current-tile presentation route, but its TargetDesktopRoute is
  Indeterminate and it cannot create a visual or physical assignment.
  Failure/FALSE membership remains non-actionable. Preserve the independently
  verified identity only when its own recapture succeeds.
- [ ] Query mobility metadata for published actionable rows. A positive global
  route remains VisualOnly even if the optional pin service is absent; an exact
  row with absent pin service remains Indeterminate/Reject for physical moves.
- [ ] Change BuildModel so opening a new session may seed activeWindow, but a
  refresh of the current session preserves PickerState.activeWindow. Remove the
  RefreshPickerModelPreservingUi substitution of transition.target.
- [ ] Stage visual upsert/erase inside RunPickerVisualRefreshTransaction.
  Relocate exact matching WinItem values before filtering and prune stale
  entries in the staged PickerState. When an entry already exists, restore
  WinItem.baseDesktop from assignment.baseDesktop and set displayedDesktop from
  assignment.destination even if fresh global enumeration initially placed the
  row under the new current desktop. Never recompute an existing assignment’s
  base from that fallback. Require both stored base and destination to exist in
  the current concrete tile snapshot or prune the entry. Build the complete
  PickerPaintCacheState<RowRec> from the relocated staged tiles, including
  device-context-dependent measurements, before publication. If model or cache
  construction fails, release the temporary HDC and return false. Only then
  noexcept-swap g_tiles, g_pickerPaintCache, and g_picker on the GUI thread with
  one shared generation. Do not publish any of the three and rebuild another
  afterward.
- [ ] Build each row hit rectangle once:

~~~cpp
snapshot.hitRect={
    tile.rc.left+S(8),y-S(2),
    rowRight,y+S(20)
};
snapshot.textRect={
    tile.rc.left+S(38),y,
    rowRight,y+S(18)
};
~~~

- [ ] Store full title/truncation and immutable action metadata in the same
  snapshot. Paint icon, active highlight, text, tooltip, and hit testing from
  this published geometry. Do not retain vector pointers or iterators.
- [ ] Increment rowLayoutEpoch only when model/filter/scroll/layout publication
  can change row identity or geometry. Hover and drag-highlight invalidation
  must not increment it.
- [ ] Run GREEN and build:

~~~powershell
.\build-test.bat
.\build.bat
~~~

- [ ] Commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/vde.cpp src/picker_state.hpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "feat: publish picker row snapshots" -- src/vde.cpp src/picker_state.hpp tests/vdtest.cpp
~~~

## Task 10: Generalize transition entry and execute all four action modes

**Files:**

- Modify: src/vde.cpp
- Modify: src/picker_state.hpp
- Modify: src/picker_trace.hpp
- Modify: src/picker_trace.cpp
- Modify: tests/vdtest.cpp

- [ ] Add a PickerActionIntent enum with TileSwitch, ActivateExact,
  MoveAndFollow, RowMoveOnly, VisualAndFollow, and VisualOnly. Add a value-only
  request that holds intent, destination, fixed-size row action snapshot when
  present, the separately captured popup active identity, and Ctrl-at-down.

~~~cpp
enum class PickerActionIntent {
    TileSwitch,
    ActivateExact,
    MoveAndFollow,
    RowMoveOnly,
    VisualAndFollow,
    VisualOnly
};

struct PickerActionRequest {
    PickerActionIntent intent=PickerActionIntent::TileSwitch;
    GUID destination={0};
    bool hasRow=false;
    PickerRowActionSnapshot row;
    WindowIdentityKey popupActiveTarget;
    bool ctrlAtDown=false;
    uint64_t activationId=0;
};

enum class PickerActionDispatchResult {
    Rejected,
    SwitchedOnly,
    ActivationStarted,
    TransitionStarted
};

inline bool PickerActionUsesPopupActiveTarget(
        PickerActionIntent intent) noexcept {
    return intent==PickerActionIntent::MoveAndFollow ||
           intent==PickerActionIntent::VisualAndFollow;
}
~~~
- [ ] Add RED dispatcher tests for this routing table:

| Input | Fresh disposition | Intent |
|---|---|---|
| plain tile/title/empty | any | TileSwitch |
| plain row | identity can upgrade | ActivateExact |
| plain DisplayOnly row | upgrade fails | TileSwitch |
| DisplayOnly row drop | any | no action |
| stationary Ctrl tile/row | Physical | MoveAndFollow on popup active target |
| stationary Ctrl tile/row | VisualOnly | VisualAndFollow on popup active target |
| row drop | Physical | RowMoveOnly on grabbed row |
| row drop | VisualOnly | VisualOnly on grabbed row |
| row drop | Reject | no action |

- [ ] Add entry/source RED tests proving:
  - MoveAndFollow and VisualAndFollow set transition.target and
    popupActiveTarget from PickerState.activeWindow even when the stationary
    click occurred over a row;
  - RowMoveOnly and VisualOnly set transition.target from the grabbed verified
    row and popupActiveTarget from the preserved PickerState.activeWindow;
  - no mode writes the grabbed row into activeWindow, g_target, or
    g_targetTitle;
  - a DisplayOnly drag is rejected before identity upgrade, pin query,
    reservation, or transition publication.
- [ ] Add effect-executor RED tests proving:
  - MoveTarget calls IssueGuardedTargetMove;
  - MovePopup calls IssuePickerPopupMove;
  - PublishVisualAssignment runs one atomic staged model rebuild;
  - RowMoveOnly Refresh preserves active target and selected/current desktops;
  - visual modes never select a layout record;
  - non-browser RowMoveOnly reaches PopupSaveStatus::NotTracked.
- [ ] Run RED:

~~~powershell
.\build-test.bat
~~~

- [ ] Replace BeginVerifiedPickerMove with a shared BeginPickerAction that
  accepts the immutable request and prepares the selected mode. Keep the
  current reservation for physical modes. For visual modes reserve only the
  exact runtime identity and operation lifetime; leave app, record ID, and
  persistence mutation empty.
- [ ] Remove the current early GuidIsZero(fast.desktop) /
  TargetDesktopUnavailable exit. Feed zero/sentinel origins into the route and
  mobility classifier first: Physical modes still require a concrete Exact
  origin, while positive global/pin evidence may select a visual mode.
- [ ] At entry, freshly recapture identity, destination existence, route, and
  mobility. Choose Physical, VisualOnly, or Reject from the single decision.
  Never infer visual behavior from Immovable or Unknown. For a drag, first
  require the copied admission==Verified and
  PickerRowActionableForDrag==true; dispatch-time identity upgrade is reserved
  for stationary plain activation and never upgrades a DisplayOnly drag.
- [ ] Preserve g_target/g_targetTitle/PickerState.activeWindow for the whole
  session. Set transition.popupActiveTarget from that preserved value. Set
  transition.target from the preserved active identity for both Ctrl modes and
  from the grabbed row only for RowMoveOnly/VisualOnly.
- [ ] For VisualAndFollow, locate that exact popup-active runtime key in the
  unfiltered staged picker model, regardless of which tile/row was clicked.
  Set visualMutation.baseDesktop from an existing assignment’s baseDesktop or
  from that WinItem’s concrete pre-overlay baseDesktop, and set destination
  from the clicked tile. If the exact row or concrete base no longer exists,
  reject the visual publication; never substitute the clicked row, an app
  sibling, targetOrigin sentinel, or current desktop.
- [ ] For VisualOnly drag, use only the grabbed action snapshot’s runtime
  identity and baseDesktop. destination==baseDesktop stages Erase;
  destination==displayedDesktop is NoOp; every other concrete destination
  stages Upsert while preserving an existing assignment base.
- [ ] In ExecutePickerEffect:
  - MoveTarget calls the guarded primitive and publishes invoked/identity/API
    facts to the reducer;
  - MovePopup calls the popup-only primitive;
  - PublishVisualAssignment calls the staged BuildModel transaction;
  - SaveExactTarget keeps the existing Firefox/Chrome/Edge classification and
    returns NotTracked for all other applications;
  - Refresh passes the preserved popup-active snapshot, never action target.
- [ ] After RowMoveOnly rollback failure, synchronously request an authoritative
  model rebuild from Windows. If it fails, remove/disable the exact row hit
  snapshot and advance rowLayoutEpoch before pointer input resumes.
- [ ] A failed visual publication retains the prior overlay/model and emits no
  user message. A failed RowMoveOnly before mutation leaves the popup open.
- [ ] Add trace mode/intent/mobility fields with stable lowercase names. Keep
  the existing event correlation generation/effectSerial.
- [ ] Run GREEN and production build:

~~~powershell
.\build-test.bat
.\build.bat
~~~

- [ ] Commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/vde.cpp src/picker_state.hpp src/picker_trace.hpp src/picker_trace.cpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "feat: dispatch picker row actions" -- src/vde.cpp src/picker_state.hpp src/picker_trace.hpp src/picker_trace.cpp tests/vdtest.cpp
~~~

## Task 11: Wire mouse capture, Drag & Drop, and conflicting-input gates

**Files:**

- Modify: src/vde.cpp
- Modify: src/picker_state.hpp
- Modify: src/picker_trace.hpp
- Modify: src/picker_trace.cpp
- Modify: tests/vdtest.cpp

- [ ] Add source-wiring RED tests for WM_LBUTTONUP, WM_CAPTURECHANGED,
  WM_CANCELMODE, SetCapture, GetCapture, ReleaseCapture, SM_CXDRAG,
  SM_CYDRAG, row-first hit testing, and gesture cleanup before ReleaseCapture.
- [ ] Add RED tests proving Armed/Dragging blocks:
  - search text mutation and async filtered-row publication;
  - mouse wheel and keyboard selection/activation;
  - idle active-target adoption and model refresh;
  - hit-cache republication.
- [ ] Add RED tests proving cosmetic drop highlighting changes neither
  selectedIndex nor rowLayoutEpoch.
- [ ] Run RED:

~~~powershell
.\build-test.bat
~~~

- [ ] Add one runtime PickerPointerGesture. Define PickerInteractionBusy as
  controlled transition OR Armed/Dragging and use it at every conflicting
  mutation listed above.

~~~cpp
inline bool PickerInteractionBusy(
        const PickerState& state,
        const PickerPointerGesture& gesture) noexcept {
    return state.controlledTransition() ||
           gesture.phase!=PickerPointerPhase::Idle;
}
~~~
- [ ] Update every vde.cpp call to the five-argument Resolve and five-callback
  Dispatch APIs, then remove the two compatibility overloads introduced in
  Task 6. The build must contain only explicit row-aware dispatch.
- [ ] WM_LBUTTONDOWN:
  - resolve footer/clear/search/row/tile in priority order;
  - on a row, copy the fixed-size hit.action and Ctrl state from the message,
    publish Armed, then SetCapture; do not copy title/runtime strings into the
    gesture;
  - immediately verify GetCapture()==hwnd; otherwise cancel;
  - non-row tile actions may dispatch from their immutable tile hit.
- [ ] WM_MOUSEMOVE:
  - while Armed, compare against centered SM_CXDRAG/SM_CYDRAG metrics;
  - after threshold, enter Dragging and update only dropTileIndex;
  - invalidate paint for highlight without changing selection or epoch;
  - when no gesture exists, retain normal hover/tooltip behavior.
- [ ] WM_LBUTTONUP:
  - copy the pure resolution and reset gesture to Idle first;
  - call ReleaseCapture only after state is Idle because
    WM_CAPTURECHANGED may re-enter;
  - Armed over the exact same row dispatches stationary click;
  - Dragging over a valid different tile dispatches the grabbed-row drop;
  - outside, different row, stale epoch, and same displayed tile cancel/no-op;
  - destination==baseDesktop creates an Erase visual mutation;
  - any other visual destination creates Upsert.
- [ ] WM_CAPTURECHANGED, WM_CANCELMODE, Escape, WM_DESTROY, and session end call
  the same idempotent gesture reset. Escape during a controlled action requests
  reducer cancellation; Escape during Armed/Dragging only cancels the gesture.
- [ ] In WM_ACTIVATE/WA_INACTIVE, keep the existing controlled-transition
  exemption and add the Armed/Dragging exemption. Only an actual idle,
  non-transient deactivation calls HidePicker(DismissSession).
- [ ] Ensure Ctrl on a stationary row routes to the popup captured active
  target, while a crossed-threshold drag always routes to gesture.row.identity.
- [ ] Paint the drop tile from gesture.dropTileIndex. Do not assign it to
  g_picker.selectedIndex.
- [ ] Trace row/tile target, Ctrl-at-down, threshold transition, generation
  validity, drop result, and action mode. Do not trace titles.
- [ ] Run GREEN and build:

~~~powershell
.\build-test.bat
.\build.bat
~~~

- [ ] Commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/vde.cpp src/picker_trace.hpp src/picker_trace.cpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "feat: drag picker rows between desktops" -- src/vde.cpp src/picker_state.hpp src/picker_trace.hpp src/picker_trace.cpp tests/vdtest.cpp
~~~

## Task 12: Activate only the exact clicked row window

**Files:**

- Modify: src/picker_state.hpp
- Modify: src/picker_trace.hpp
- Modify: src/picker_trace.cpp
- Modify: src/vde.cpp
- Modify: tests/vdtest.cpp

- [ ] Add a pure exact-activation plan and RED tests for:
  - same-current destination skips SwitchDesktop and ReadCurrent;
  - another destination requires successful switch plus readback;
  - failed/uninvoked switch never emits foreground activation;
  - identity change after switch activates nobody;
  - normal row must read on the displayed destination;
  - global row must retain fresh positive global/pin evidence;
  - an Exact raw-source row with an app/view pin and a visual destination uses
    fresh VisualOnly evidence instead of requiring raw source==displayed;
  - if the optional pin service disappears after a positive pinned row was
    published, activation-only may use that cached positive session evidence
    plus fresh identity and TRUE current membership; movement still rejects;
  - DisplayOnly identity-upgrade failure degrades to switch-only;
  - stale generation switches only when the copied destination still exists;
    a vanished destination leaves the popup open and activates nobody;
  - no owner, last-active, or fallback HWND is selected.
- [ ] Add a testable call adapter beside
  ExecutePickerForegroundHandoffCalls and RED call-order tests. For a
  cross-desktop action require:

~~~text
recapture exact identity
hide popup with DismissSession
switch desktop
read current desktop
recapture exact identity
read exact route or global evidence
attach input to post-switch foreground and target
restore exact target when minimized
SetForegroundWindow(exact target)
GetForegroundWindow
detach target input
detach foreground input
~~~

- [ ] Assert same-desktop activation omits switch/read-current but retains both
  identity captures and exact foreground verification.
- [ ] Run RED:

~~~powershell
.\build-test.bat
~~~

- [ ] Add a value-only PickerExactActivationRequest containing the copied row
  raw HWND, optional complete identity, displayed desktop, observed desktop,
  base desktop, route, mobility, visuallyAssigned flag, and generation.

~~~cpp
struct PickerExactActivationRequest {
    uintptr_t hwnd=0;
    WindowIdentityKey identity;
    PickerRowAdmission admission=PickerRowAdmission::DisplayOnly;
    GUID displayedDesktop={0};
    GUID observedDesktop={0};
    GUID baseDesktop={0};
    TargetDesktopRoute desktopRoute=
        TargetDesktopRoute::Indeterminate;
    TargetMobility mobility=TargetMobility::Indeterminate;
    bool visuallyAssigned=false;
    uint64_t modelGeneration=0;
    uint64_t rowLayoutEpoch=0;
};

enum class PickerExactActivationDecision {
    RejectKeepPopup,
    SwitchOnly,
    ActivateOnCurrent,
    SwitchThenActivate
};

inline PickerExactActivationDecision DecidePickerExactActivation(
        bool destinationExists,bool presentationCurrent,
        bool identityUpgraded,bool destinationIsCurrent) noexcept {
    if(!destinationExists)
        return PickerExactActivationDecision::RejectKeepPopup;
    if(!presentationCurrent || !identityUpgraded)
        return PickerExactActivationDecision::SwitchOnly;
    return destinationIsCurrent
        ? PickerExactActivationDecision::ActivateOnCurrent
        : PickerExactActivationDecision::SwitchThenActivate;
}
~~~

- [ ] Add PickerTraceExactActivationResult values reject_keep_popup,
  switch_only, identity_lost, desktop_mismatch, global_membership_lost,
  foreground_rejected, and exact_foreground. Serialize them in the same task
  and add the strict name table before runtime emission.
- [ ] Validate destination existence and recapture identity before hiding. If a
  DisplayOnly row cannot upgrade, call only the existing switch-only tile path.
- [ ] Hide with DismissSession. If destination differs from current, use the
  existing shell foreground handoff, require that the switch API was invoked
  and accepted, then require current-desktop readback to equal destination.
  If already current, skip both operations.
- [ ] Do not keep AttachThreadInput connections across the desktop switch.
  After switching, recapture identity again. For a non-visual ordinary Exact
  row require GetWindowDesktopId==displayedDesktop and concrete snapshot
  membership. For visuallyAssigned, global, app-pinned, or view-pinned rows,
  repeat route/pin/mobility classification, require disposition=VisualOnly,
  and require IsWindowOnCurrentVirtualDesktop=TRUE on the selected destination;
  do not require the raw source GUID to equal the visual destination. If the
  optional pin service is newly unavailable, cached positive ViewPinned or
  AppPinned evidence from the same current row snapshot is sufficient only for
  activation after fresh identity and TRUE membership. Never pass that cached
  evidence to a target-move guard.
- [ ] If IsIconic(target) is true, call ShowWindow(target,SW_RESTORE). Do not
  interpret ShowWindow’s return value as success.
- [ ] Attach current-thread input queues only for the exact post-switch
  foreground thread and exact target thread. Skip a self-attachment or a
  duplicate thread pair, and remember which calls actually succeeded. Require
  both SetForegroundWindow(target)!=FALSE and
  GetForegroundWindow()==target. Detach only successful attachments, in
  reverse order, on every exit.
- [ ] Never activate an owner, sibling, prior foreground, or another row when
  exact activation fails. Leave the selected desktop current; do not roll back
  the switch.
- [ ] Keep GoToDesktop as the title/empty tile path; it receives no target HWND
  and performs no explicit row activation.
- [ ] Add trace outcomes for switch-only degradation, exact identity loss,
  destination mismatch, global revalidation, SetForegroundWindow rejection,
  and exact foreground success.
- [ ] Run GREEN and build:

~~~powershell
.\build-test.bat
.\build.bat
~~~

- [ ] Commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/picker_state.hpp src/picker_trace.hpp src/picker_trace.cpp src/vde.cpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "feat: activate exact picker row" -- src/picker_state.hpp src/picker_trace.hpp src/picker_trace.cpp src/vde.cpp tests/vdtest.cpp
~~~

## Task 13: Complete popup-session lifetime, trace exhaustiveness, and regressions

**Files:**

- Modify: src/vde.cpp
- Modify: src/picker_state.hpp
- Modify: src/picker_trace.hpp
- Modify: src/picker_trace.cpp
- Modify: tests/vdtest.cpp

- [ ] Add RED lifetime tests/source assertions for every session-ending path:
  ordinary HidePicker, controlled DismissSession, ShowPicker preparation abort,
  dismissing cancellation, WM_ACTIVATE final dismissal, WM_DESTROY, and
  application teardown.
- [ ] Add RED preservation tests for search, filtering, icon refresh,
  lightweight model refresh, controlled TransientRelocate Hide/Show, rollback
  recovery, and ShowAndFocus inside the same session.
- [ ] Keep the pure EndPickerVisualSession(PickerState&) from Task 3 responsible
  only for the state-owned map. Add this runtime wrapper in vde.cpp:

~~~cpp
static void EndPickerVisualSessionRuntime() noexcept {
    const bool releaseCapture=
        g_main && GetCapture()==g_main;
    CancelPickerRowGesture(g_pickerGesture);
    if(releaseCapture)
        ReleaseCapture();
    EndPickerVisualSession(g_picker);
}
~~~

- [ ] Call EndPickerVisualSessionRuntime exactly once from each logical-session
  termination path. Gesture state becomes Idle before ReleaseCapture can
  synchronously deliver WM_CAPTURECHANGED. A transient hide must not call it.
- [ ] Extend strict enum-name tables, JSON schema tests, wrong-event/serial
  acknowledgement matrices, shutdown routing, whole-object swap tests, and
  traced/plain reducer equivalence for every new field and enum value.
- [ ] Add privacy source/schema assertions that AppUserModelID strings are
  never copied from the local RAII buffer into trace events, diagnostics,
  picker model/state, or persistent layout data.
- [ ] Add regression tests that preserve:
  - Firefox, Chrome, and Edge as the only tracked persistence profiles;
  - non-browser physical move with NotTracked result;
  - Ctrl state captured from the button message;
  - active icon/highlight for non-Firefox windows;
  - all-window enumeration;
  - primary-monitor popup placement;
  - existing MoveAndFollow happy path.
- [ ] Run RED:

~~~powershell
.\build-test.bat
~~~

- [ ] Wire HidePicker(DismissSession), controlled Hide(DismissSession),
  ShowPicker abort, dismissing cancellation, WM_ACTIVATE final dismissal,
  WM_DESTROY, and application teardown to EndPickerVisualSessionRuntime.
- [ ] Wire TransientRelocate, search/filter/icon refresh, lightweight refresh,
  rollback recovery, and ShowAndFocus to preserve visualAssignments and the
  logical session.
- [ ] Add every new enum/value to its explicit trace-name switch and strict
  expected-name table. Add every new state/effect field to swap and
  traced/plain equality assertions.
- [ ] Run build-test.bat and require all named lifetime, privacy, schema, and
  regression tests from this task to pass before continuing.
- [ ] Inspect the direct-move inventory again and prove visual-mode source
  sections contain no move/persistence calls:

~~~powershell
rg -n "MoveViewToDesktop|MoveWindowToDesktop" src\vde.cpp
rg -n "VisualOnly|VisualAndFollow|SaveExactTarget|PublishVisualAssignment" src\picker_state.hpp src\vde.cpp
~~~

- [ ] Run the complete automated gate:

~~~powershell
.\build-test.bat
.\build.bat
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --check
~~~

- [ ] Commit:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde add src/vde.cpp src/picker_state.hpp src/picker_trace.hpp src/picker_trace.cpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde commit --only -m "fix: finish picker visual session lifecycle" -- src/vde.cpp src/picker_state.hpp src/picker_trace.hpp src/picker_trace.cpp tests/vdtest.cpp
~~~

## Task 14: Add whole-row hover feedback and pin QA to the feature binary

**Files:**

- Modify: src/picker_state.hpp
- Modify: src/vde.cpp
- Modify: tests/vdtest.cpp

- [ ] Confirm the reported click/drag failure is a binary-selection problem,
  not a row-hit regression:

~~~powershell
Get-Process vde -ErrorAction SilentlyContinue |
    Select-Object Id,Path
git -c safe.directory=F:/_VDESKTOP_FF/win-vde/.worktrees/picker-row-actions rev-parse --short HEAD
Get-FileHash .\build\vde.exe -Algorithm SHA256
~~~

  Expected: no VDE process before rebuilding. The branch revision is newer
  than main, and the worktree executable has a different hash from
  `F:\_VDESKTOP_FF\win-vde\build\vde.exe`. Do not alter the already-correct
  row-first `WM_LBUTTONDOWN` path or exact `WM_LBUTTONUP` dispatch without a
  reproducing test against the feature source.

- [ ] Add and register a RED pure-state test beside the footer-hover tests:

~~~cpp
static void test_picker_row_hover_tracks_full_row_and_resets(){
    PickerHoverEventState state;
    PickerRowHoverUpdate update=UpdatePickerRowHoverEvent(state,3,41);
    CHECK(update.changed && update.previousRowIndex==-1 &&
          update.currentRowIndex==3);
    CHECK(PickerRowHoverMatches(state,3,41));

    update=UpdatePickerRowHoverEvent(state,3,41);
    CHECK(!update.changed);

    update=UpdatePickerRowHoverEvent(state,7,41);
    CHECK(update.changed && update.previousRowIndex==3 &&
          update.currentRowIndex==7);
    CHECK(!PickerRowHoverMatches(state,3,41));
    CHECK(PickerRowHoverMatches(state,7,41));

    PickerFooterMouseMoveEffects footer=RoutePickerFooterMouseMove(
        state,PickerFooterLink::Repository,false);
    CHECK(footer.invalidateRowHover);
    CHECK(state.hoveredRowIndex==-1 && state.hoveredRowGeneration==0);

    UpdatePickerRowHoverEvent(state,5,42);
    ResetPickerHoverEventState(
        state,PickerHoverResetReason::MouseLeave);
    CHECK(state.hoveredRowIndex==-1 && state.hoveredRowGeneration==0);
}
~~~

- [ ] Add and register a RED source-wiring test proving:
  - idle `WM_MOUSEMOVE` uses `HitPickerRow(pt)` for the visual hover, while the
    tooltip alone still checks `snapshot.textRect`;
  - tile-selection paint-cache refresh occurs before the final row-hover
    update, so cache publication cannot immediately erase the highlight;
  - `Paint` blends a rounded background over `snapshot.hitRect` and applies
    the existing active-row treatment afterward;
  - drag start, footer entry, cache publication, hide, and `WM_MOUSELEAVE`
    clear row hover;
  - hover changes call `InvalidateRect` but neither `BuildModel` nor
    `RefreshPickerPaintCache`.

- [ ] Run RED:

~~~powershell
.\build-test.bat
~~~

  Expected: compilation fails because `PickerRowHoverUpdate`,
  `UpdatePickerRowHoverEvent`, `PickerRowHoverMatches`, and the row fields do
  not exist.

- [ ] Extend `PickerHoverEventState` and its pure helpers in
  src/picker_state.hpp:

~~~cpp
struct PickerHoverEventState {
    PickerFooterLink footerLink=PickerFooterLink::None;
    int hoveredRowIndex=-1;
    uint64_t hoveredRowGeneration=0;
    bool rowTooltipActive=false;
    PickerHoverResetReason lastResetReason=PickerHoverResetReason::None;
    uint64_t resetCount=0;
};

struct PickerRowHoverUpdate {
    int previousRowIndex=-1;
    int currentRowIndex=-1;
    bool changed=false;
};

inline PickerRowHoverUpdate UpdatePickerRowHoverEvent(
        PickerHoverEventState& state,int rowIndex,
        uint64_t generation) noexcept {
    if(rowIndex<0){
        rowIndex=-1;
        generation=0;
    }
    PickerRowHoverUpdate update;
    update.previousRowIndex=state.hoveredRowIndex;
    update.currentRowIndex=rowIndex;
    update.changed=state.hoveredRowIndex!=rowIndex ||
        state.hoveredRowGeneration!=generation;
    if(update.changed){
        state.hoveredRowIndex=rowIndex;
        state.hoveredRowGeneration=generation;
    }
    return update;
}

inline bool PickerRowHoverMatches(
        const PickerHoverEventState& state,int rowIndex,
        uint64_t generation) noexcept {
    return rowIndex>=0 && state.hoveredRowIndex==rowIndex &&
        state.hoveredRowGeneration==generation;
}
~~~

  Add `invalidateRowHover` to `PickerFooterMouseMoveEffects`. Footer
  suppression clears the row through `UpdatePickerRowHoverEvent(state,-1,0)`
  and reports whether repaint is needed. `ResetPickerHoverEventState` clears
  both row fields on every existing reset path.

- [ ] In idle `WM_MOUSEMOVE`, update tile selection first when needed, then
  recompute cache validity and call `HitPickerRow(pt)`. Feed that full-row
  result to `UpdatePickerRowHoverEvent`. Use `snapshot.textRect` only as an
  additional tooltip condition. Repaint on a changed row without rebuilding
  the picker model. Clear hover when drag crosses the system threshold and
  make `WM_MOUSELEAVE` repaint when either footer or row state changed.

- [ ] In `Paint`, iterate rows with their stable paint-cache index and draw the
  inactive hover before the icon/text:

~~~cpp
const bool active=PickerRowUsesStableIdentity(
        snapshot.action.admission) &&
    IsActiveWindow(g_picker,snapshot.action.identity);
const bool hovered=PickerRowHoverMatches(
    g_pickerHoverState,static_cast<int>(rowIndex),
    g_pickerPaintCache.generation);
if(hovered && !active){
    const COLORREF hoverRow=BlendColor(fill,CLR_HEAD,24);
    FillRoundRect(
        hdc,snapshot.hitRect,S(5),hoverRow,hoverRow,S(1));
}
~~~

  Keep the current stronger orange active-row fill and bar after this block so
  active state has precedence.

- [ ] Run GREEN and production build:

~~~powershell
.\build-test.bat
.\build.bat
git -c safe.directory=F:/_VDESKTOP_FF/win-vde/.worktrees/picker-row-actions diff --check
~~~

- [ ] Commit exactly the three task files:

~~~powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde/.worktrees/picker-row-actions add src/picker_state.hpp src/vde.cpp tests/vdtest.cpp
git -c safe.directory=F:/_VDESKTOP_FF/win-vde/.worktrees/picker-row-actions diff --cached --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde/.worktrees/picker-row-actions commit --only -m "feat: highlight hovered picker rows" -- src/picker_state.hpp src/vde.cpp tests/vdtest.cpp
~~~

- [ ] Launch only the newly built worktree executable and verify its path:

~~~powershell
Start-Process -FilePath (Resolve-Path .\build\vde.exe)
Get-Process vde -ErrorAction Stop |
    Select-Object Id,Path
~~~

  Expected path:
  `F:\_VDESKTOP_FF\win-vde\.worktrees\picker-row-actions\build\vde.exe`.
  If the path points to main, stop QA and correct the launched executable; do
  not diagnose feature behavior from the stale build.

## Task 15: Independent review and manual Windows QA

**Files:**

- Review only unless a defect is found.

- [ ] Request an independent code review focused on:
  - fresh identity/route/pin checks immediately before every external move;
  - popup-move exemptions;
  - reducer effect allowlists per mode;
  - action-target isolation from popup active target;
  - atomic overlay/model publication;
  - save publication versus rollback;
  - capture/re-entrancy and stable epoch handling;
  - exact foreground activation without fallback;
  - trace privacy.
- [ ] Apply review findings one at a time with a RED regression test, GREEN
  suite, and a narrowly scoped conventional commit.
- [ ] Run final automated verification and record output:

~~~powershell
.\build-test.bat
.\build.bat
git -c safe.directory=F:/_VDESKTOP_FF/win-vde diff --check
git -c safe.directory=F:/_VDESKTOP_FF/win-vde status --short --branch
Get-FileHash .\build\vde.exe -Algorithm SHA256
~~~

- [ ] Manual QA on Windows:
  1. Confirm the running process path is the feature-worktree `build\vde.exe`.
  2. Hover an inactive row over its icon, text, and remaining row width; verify
     the same subtle background. Move to another row and then outside the list;
     verify immediate transfer and clear. Verify the active row remains orange.
  3. Click a normal row on another desktop; verify desktop switch, popup close,
     and activation of that exact HWND.
  4. Click a desktop title and empty tile area; verify switch without explicit
     listed-window activation.
  5. Drag Firefox, Chrome, or Edge to another tile; verify physical move,
     saved assignment, unchanged current desktop, open popup, and preserved
     active highlight.
  6. Drag a non-browser app; verify physical move, no restore record, unchanged
     current desktop, and open popup.
  7. Drag a globally visible view; verify only the selected row moves visually
     and Windows global state remains unchanged.
  8. Repeat with an app-wide pin containing at least two windows; verify only
     the grabbed exact row changes tile.
  9. Reassign the same visual row, drop it on its displayed tile, and drop it
     back on its observed source; verify update/no-op/erase behavior.
  10. Search and force refresh while popup remains open; verify overlay
     survival. Close and reopen; verify all overlays are gone.
  11. Ctrl+Click a global/pinned captured window; verify visual assignment,
     destination switch, open popup, and unchanged pin/global behavior.
  12. Cancel before threshold, outside a tile, with Escape, and after starting
      RowMoveOnly; verify no false visual claim and verified rollback.
  13. Disable/fail optional pinned service in a diagnostic build; verify picker
      display/search/switch/activation still work and physical target moves fail
      closed.
- [ ] Do not declare completion until automated gates, review, and all manual
  cases pass. If a manual failure occurs, add a reproducing automated test
  where possible and fix it in a separate commit.

## Completion checklist

- [ ] Plain row and tile clicks have distinct behavior.
- [ ] RowMoveOnly effect order is exactly MoveTarget → ReadTarget →
  SaveExactTarget → Refresh.
- [ ] Visual modes contain zero target move/read/save/rollback effects.
- [ ] Every external target move is guarded; popup moves remain independent.
- [ ] Global/app-pinned/view-pinned rows never receive a physical move.
- [ ] Non-global Immovable and Indeterminate rows make no false visual claim.
- [ ] Only the exact selected runtime row receives a visual assignment.
- [ ] Overlay/model publication is atomic and overlays end with the popup
  session.
- [ ] Drag does not switch desktop, close popup, or change popup active target.
- [ ] Persistence remains Firefox/Chrome/Edge only.
- [ ] Exact activation has no fallback HWND.
- [ ] Every inactive row has whole-hit-rectangle hover feedback; active-row
  styling remains stronger and every hover reset path repaints.
- [ ] Full tests, production build, diff checks, independent review, and manual
  QA are complete.
