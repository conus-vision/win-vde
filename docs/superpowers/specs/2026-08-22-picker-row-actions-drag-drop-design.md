# Picker Row Actions, Drag and Drop, and Global-Window Safety — Design

Date: 2026-08-22
Status: approved for planning
Scope: desktop picker input, verified window movement, exact activation, and
session-only presentation of globally visible windows

## Context

The picker now enumerates ordinary application windows, resolves their icons,
highlights the active window, and can move the captured active window with
Ctrl+Click. Live testing exposed the next set of interaction and safety
requirements:

1. Moving a window that Windows exposes on every virtual desktop can break the
   global relationship. One window of an application moves to a concrete
   desktop while another remains globally visible.
2. A plain click cannot distinguish a window row from the rest of its desktop
   tile. The picker therefore cannot activate the exact window selected by the
   user.
3. Window rows cannot be dragged between desktop tiles.

The agreed behavior preserves Windows global visibility. A globally visible
window is never physically moved by the picker. Only the selected row may be
shown under another tile, and that visual assignment exists only while the
current popup session remains open.

## Confirmed cause

The current Ctrl+Click transition validates the captured window identity and
then issues the target move through the virtual-desktop APIs. It does not first
query whether the view or its application is pinned, globally visible, or
otherwise immovable.

Consequently, a row can be identity-safe and still be unsafe to move.
PickerRowAdmission::Verified currently proves the identity tuple, not
movability, a concrete desktop origin, or pin state.

The paint cache also stores only the text rectangle and tooltip data for each
row. Mouse dispatch knows which tile was clicked but not which exact painted
window row was clicked. Supporting exact activation and drag-and-drop therefore
requires a stable row hit snapshot rather than a late lookup by tile index.

## User-visible contract

### Plain click

- A plain click on a visible window row switches to the row's displayed desktop,
  closes the popup, and attempts to activate that exact window.
- A plain click on a desktop title or empty tile area only switches desktops.
  It does not request activation of any listed window.
- Clicking a row that has a session-only visual assignment uses the visually
  assigned destination.

### Ctrl+Click

- A stationary Ctrl+Click anywhere in a desktop tile retains the established
  meaning: move the captured active window to that desktop, preserve its
  supported saved assignment, switch to the destination, and keep the popup
  open.
- On a row, a stationary Ctrl+Click takes precedence over plain row activation:
  it still acts on the captured active window. Once the drag threshold is
  crossed, row drag takes precedence and acts on the grabbed row.
- Ctrl state is captured from the original mouse event and does not depend on a
  later keyboard-state query.
- If the captured active window is globally visible or pinned, no target move
  or persistent assignment occurs. Only the one row representing that exact
  captured window is visually assigned to the destination.
- Other windows from the same application remain visually unchanged, including
  when the entire application is pinned.

### Drag and drop

- Pressing a row arms a gesture. Movement beyond the Windows system drag
  threshold starts dragging that exact row.
- Dropping an ordinary movable row on a different desktop tile physically moves
  that window. For Firefox, Chrome, and Edge it also publishes the supported
  saved assignment after move verification.
- Dropping another application's row performs the Windows move for the current
  application/window lifetime but creates no persistent restore record.
- A successful ordinary drop does not switch the current desktop, close the
  popup, or run a foreground-focus handoff.
- Dropping a row with positive global, view-pin, or application-pin evidence
  changes only that selected row's visual tile for the current popup session.
- A non-global Immovable row is rejected without a physical or visual move.
- A visual-only drop does not call a window-move API and does not update layout
  persistence.
- Dropping on the currently displayed source tile is a no-op. For a row that
  already has a visual assignment, dropping on its actual Windows source
  removes that one overlay entry; dropping on another tile updates it.
  Dropping outside a valid tile cancels the gesture.
- A stationary press and release remains a click. A drag gesture always acts on
  the grabbed row; Ctrl does not change the identity of a dragged row.

### Popup-session lifetime

- Visual assignments survive search changes, filtering, icon refresh, and model
  rebuilds while the logical popup session remains open.
- A temporary internal Hide/Show used to relocate or recover the popup is part
  of the same session and preserves all visual assignments.
- Every path that ends the logical popup session, aborts its creation, or
  destroys the popup clears all visual assignments.
- Reopening the popup reconstructs rows solely from actual Windows state.

## Design

### 1. Immutable painted-row hit snapshots

Replace tooltip-only row records with immutable hit snapshots published
transactionally with the paint cache. Each visible row snapshot contains:

- the complete row hit rectangle, including its icon;
- a separate text rectangle and truncation data for the tooltip;
- destination desktop GUID and tile position;
- exact HWND plus stable pid/process-start identity when available;
- the row runtime key;
- admission, desktop-route, and mobility metadata;
- paint/model generation values.

Snapshots contain values only. They do not retain pointers, references, or
vector iterators into the mutable tile model.

Mouse hit testing uses this priority:

1. footer controls;
2. search clear button or search field;
3. visible window row;
4. desktop tile title or empty area;
5. no target.

The complete snapshot is copied on button down before any action can refresh the
model. Every action revalidates the generation, destination existence, and full
window identity before using the HWND.

### 2. Explicit pointer gesture state

Add a small input-only state machine:

- Idle: no pointer operation;
- Armed: a row has mouse capture but has not crossed the drag threshold;
- Dragging: the row is being dragged and a valid drop tile may be highlighted.

Button down on a row enters Armed and calls SetCapture. The system drag metrics
define a rectangle centered on the button-down point. Leaving that rectangle on
either axis starts dragging; the implementation must not require both axes to
cross a full metric. Button up without a drag dispatches a row click only when
it is over the same exact row snapshot; otherwise it cancels. Button up after a
drag commits only a valid tile drop.

WM_CAPTURECHANGED, WM_CANCELMODE, Escape, an invalid generation, or a vanished
identity cancels the gesture. Search mutation, model replacement, keyboard
activation, and other conflicting picker commands are gated while Armed or
Dragging so the visible row cannot change beneath the pointer.

Armed/Dragging also pauses idle active-target adoption, model refresh, and
hit-cache republication. The drop-target highlight is cosmetic: it invalidates
paint without changing the stable model/layout epoch used by the grabbed
snapshot. Gesture validity is tied to that stable epoch, not to an incidental
paint counter.

### 3. Separate identity, route, and mobility

Row classification uses independent facts:

- Identity: Verified or DisplayOnly;
- Desktop route: Exact, session Visual, fallback/global, or Indeterminate;
- Mobility: Movable, ViewPinned, AppPinned, Immovable, or Indeterminate.

Verified continues to mean only that the HWND, pid, and process-start tuple is
complete and stable. It no longer acts as an implicit permission to move.

A real move requires all of the following:

- a freshly revalidated full identity;
- an Exact source desktop that exists in the current snapshot;
- a valid destination different from the source;
- successful evidence that the view can move;
- successful evidence that neither the view nor its application is pinned.

A positive view-pin, application-pin, or global-visibility signal dominates a
failed companion query and routes to the safe visual-only behavior. When no
positive signal exists, any conflicting or failed query produces Indeterminate
and fails closed: no physical move or visual claim is made. A plain Immovable
result without positive global or pin evidence is a no-op/failure; it is not
visually represented on a false desktop.

DisplayOnly rows remain useful for presentation. A plain row click may attempt
to upgrade the identity at dispatch time. If it cannot, the action safely
degrades to desktop-switch-only. DisplayOnly rows cannot be physically dragged
or used to create an identity-keyed visual assignment.

### 4. Read-only global and pin detection

Introduce a read-only adapter for the Windows pinned-apps service. The adapter
queries both the individual application view and the application identity. It
never calls pin or unpin methods.

The adapter is acquired independently from the mandatory desktop and view
services. Failure to acquire it does not fail application or picker startup.
Instead, target-window mobility becomes Indeterminate, which disables physical
target moves while leaving display, search, desktop switching, exact activation,
and already-proven visual-only routes available.

Application identity is obtained through the concrete IApplicationView ABI that
matches the existing IApplicationViewCollection contract. Every HRESULT,
returned pointer, and empty value is checked. Returned application-identity
storage is released through its required COM allocator on every exit path.
Failure to obtain a trustworthy identity makes the application-pin result
Indeterminate; it is never interpreted as not pinned. The optional service is
released independently during teardown.

The routing decision combines:

- the individual-view pin result;
- the application-wide pin result;
- CanViewMoveDesktops;
- known positive global-desktop observations already encountered during picker
  enumeration.

A positive pin or global signal dominates. Both pin checks must return a known
negative result, and the view must report movable, before the result can be
NotPinned/Movable.

The classification is repeated immediately before issuing a target-window move
to close the time-of-check/time-of-use gap. This guard applies only to target
windows, not to the picker popup's own virtual-desktop movement.

The same target-mobility guard is placed at every product call site that can
move an external target window, including automatic, manual, CLI, and rollback
restore paths. This prevents another entry point from bypassing the picker
protection. Popup relocation is explicitly excluded. Global/sentinel desktop
observations are not treated as ordinary destination GUIDs for persistence.

### 5. Session-only visual assignments

PickerState owns a session map from the exact window runtime key to a destination
desktop GUID. The runtime key includes enough stable identity to distinguish two
windows from the same process and to reject HWND reuse.

After enumeration and before filtered rows are published, BuildModel applies
the visual map by relocating only the matching WinItem into the assigned tile.
It does not modify the window's observed Windows desktop, FastWin data, pin
state, automatic-layout records, or saved destination.

Overlay changes are staged together with the rebuilt tile model and paint
inputs. The new visual map and corresponding model publish atomically. If model
construction or publication fails, both the previous visual map and previous
model remain active; no hidden assignment may appear during a later refresh.

Each rebuild prunes entries whose exact identity or destination desktop no
longer exists. Search and lightweight refresh preserve the remaining entries.

One shared EndPickerVisualSession helper is called from:

- the ordinary final HidePicker path;
- a controlled transition hide explicitly marked DismissSession;
- show-preparation aborts;
- cancellation paths that actually dismiss the popup;
- popup destruction and application teardown.

Controlled visibility effects therefore carry an explicit TransientRelocate or
DismissSession reason. TransientRelocate is used by MoveAndFollow,
VisualAndFollow, retry, and rollback paths that will restore the same logical
popup session. It does not clear the map. Refreshing or focusing a still-active
popup session also does not clear the map.

### 6. Unified action dispatcher and transition policies

All row and tile actions enter one dispatcher with an explicit intent and an
immutable target snapshot. The controlled transition reuses the existing
identity checks, serialized effects, bounded retries, readback, reservation,
rollback, and persistence seams, but policy determines which effects are legal.

The dispatcher keeps two identities separate:

- popup active target: the captured active window used to authorize a later
  Ctrl+Click;
- action target: the exact row used only by the current click or drag.

RowMoveOnly and VisualOnly never overwrite the popup active target, its title,
PickerState.activeWindow, or Ctrl-target authorization. Their refresh uses a
separately preserved pre-action active snapshot. Moving a non-active row
therefore cannot change the highlight or break the next Ctrl+Click in the same
popup session.

The principal modes are:

#### ActivateExact

Used for a plain row click.

1. Validate destination and recapture the exact identity.
2. Hide the popup.
3. If the destination differs from the current desktop, switch using the
   existing shell foreground handoff and require successful switch readback. If
   it is already current, skip the switch.
4. Recapture identity again.
5. For a normal row, confirm that the window belongs to the destination. For a
   known globally visible row, confirm its global classification instead.
6. Restore the window if minimized and attempt SetForegroundWindow for that
   exact HWND.
7. Verify that the requested HWND became foreground.

Input queues are never left attached across the desktop switch. There is no
fallback activation of an owner, last-active popup, or another application
window.

#### MoveAndFollow

Used for Ctrl+Click on an ordinary movable captured window. It retains the
existing verified target move, popup-follow, desktop switch, readback,
persistence, refresh, and focus behavior.

#### RowMoveOnly

Used for an ordinary row drop. Its successful effect sequence is:

1. MoveTarget;
2. ReadTarget and verify the destination;
3. SaveExactTarget under the existing Firefox/Chrome/Edge persistence scope, or
   record the existing NotTracked result for another application;
4. Refresh the picker model while preserving the pre-drag active highlight and
   current desktop selection;
5. finish without a desktop or foreground transition.

MovePopup, SwitchDesktop, ReadCurrent, popup Hide, and ShowAndFocus are forbidden
in this mode.

#### VisualAndFollow

Used for Ctrl+Click when the captured window has positive view-pin,
application-pin, or global-visibility evidence. It verifies identity, follows
the existing popup/desktop switch route, verifies the selected desktop, then
commits the one-row visual assignment and refreshes the still-open popup.

It never emits MoveTarget, target readback, target rollback, or
SaveExactTarget.

#### VisualOnly

Used for dropping a row with positive view-pin, application-pin, or
global-visibility evidence. It revalidates the exact identity, commits the
one-row visual assignment, and rebuilds the visible model. It has no external
Windows or persistence mutation.

### 7. Persistence and AutoFix coordination

RowMoveOnly obtains the same short-lived window-operation lease used to exclude
competing automatic moves. For records covered by the existing persistence
policy, the assignment is published only after move readback confirms the
destination. The policy remains limited to Firefox, Chrome, and Edge.
Untracked applications complete after verified physical movement and do not
gain a restore record.

Saved, NotTracked, and a flush failure after in-memory publication are terminal
commit outcomes. A disk flush failure retains the published in-memory
assignment and the existing retry mechanism.

A tracked save failure that did not publish the new assignment triggers
compensating target rollback. This prevents AutoFix from immediately restoring
the old assignment after an apparently successful drop.

VisualOnly and VisualAndFollow never select or update layout records and never
publish a persistent assignment.

## Failure and cancellation semantics

### Before an action starts

- A stale row generation, missing destination, changed identity, or
  Indeterminate mobility causes no target-window mutation.
- A stale plain row click may still perform switch-only if its destination
  remains valid, but it never activates a raw unverified HWND.
- A destination that disappeared leaves the popup open and reports the failure
  only through the existing diagnostics surface.

### RowMoveOnly

- Failure before target mutation terminates with the popup still open.
- Failure after a target move but before a published assignment performs bounded
  rollback to the verified source and readback.
- Escape during a started operation requests cancellation and rollback while
  leaving the popup open.
- An in-flight save is not discarded because publication may already have
  begun. Completion distinguishes non-cancellable save-in-flight from actual
  publication: published means commit; non-published failure means rollback.
- If rollback cannot be verified, the transition terminates with a traceable
  failure and does not publish an unverified destination. Before pointer input
  resumes it performs an authoritative model rebuild from observed Windows
  state. If that rebuild also fails, the affected row/hit snapshot is
  invalidated and remains non-actionable until a successful rebuild.

### ActivateExact

- If a required cross-desktop switch was never invoked or failed, no target
  activation is attempted. A same-current row does not require a switch.
- If the window closes, changes identity, or moves after the desktop switch, the
  user remains on the selected desktop and no other window is activated.
- If Windows rejects SetForegroundWindow, the selected desktop remains current.
  There is no switch rollback and no substitute target.

### Visual modes

- VisualOnly has no external rollback because it changes only picker state.
- VisualAndFollow commits the visual assignment only after destination switch
  readback succeeds.
- Overlay update and model publication are atomic. A failed publication retains
  the previous overlay/model; VisualAndFollow then follows its switch/popup
  failure policy without leaking a pending assignment.
- A failed VisualAndFollow rolls back only popup/current-desktop effects that
  were actually issued. It is structurally forbidden from entering target-move
  rollback.

## Diagnostics

Trace records distinguish:

- user intent and transition mode;
- row/tile hit result and generation validity;
- mobility result: view, application, immovable, movable, or indeterminate;
- effect issue, readback, retry, publication, rollback, and terminal outcome.

Pin-query HRESULTs and boolean outcomes may be logged. Application identity
strings are not written to diagnostics.

## Verification

### Pure and reducer tests

Automated tests will cover:

- hit-test priority and full-row geometry;
- Armed/Dragging thresholds, same-tile no-op, outside drop, capture loss, and
  Escape, plus release-over-a-different-row cancellation;
- stationary Ctrl+Click versus crossed-threshold row-drag precedence;
- stale model generations and exact identity revalidation;
- stable drag epochs despite cosmetic drop-target painting, with idle adoption
  and cache publication paused during the gesture;
- the identity/route/mobility decision matrix;
- positive view pin, positive application pin, immovable, movable, conflicting,
  and failed-query outcomes;
- plain non-global Immovable rejection without a visual assignment;
- VisualOnly and VisualAndFollow containing zero target move, target readback,
  target save, and target rollback effects;
- VisualAndFollow switching desktops while VisualOnly and RowMoveOnly do not;
- RowMoveOnly's exact effect order and forbidden popup/current/focus effects;
- action-target isolation from the popup active target and the next Ctrl+Click;
- cancellation before issue, after move, during readback, and during save;
- save outcomes that publish versus those requiring rollback;
- session overlay application, pruning, search/refresh survival, exact-row
  isolation, atomic map/model publication, return to the actual source,
  reassignment, final-dismiss cleanup, and preservation through transient
  Hide/Show;
- normal and globally visible exact-window activation without fallback to a
  different HWND;
- every external target-move entry point rejecting pinned, global, immovable,
  and indeterminate targets.

### Source-wiring and integration tests

Tests will assert that:

- target moves pass through the mobility guard while popup moves do not;
- optional pin-service failure degrades only physical target movement and does
  not abort picker startup;
- all model and paint publication remains transactional;
- global/sentinel observations cannot become saved concrete desktop
  assignments;
- RowMoveOnly preserves the existing active highlight and desktop selection;
- failed rollback cannot leave a stale row actionable;
- no visual mode calls layout persistence;
- non-browser drag does not broaden the browser-only automatic layout scope.

The complete test suite and production build must pass.

### Manual Windows QA

Manual QA will cover:

1. Click a normal row on another desktop and verify exact activation.
2. Click a desktop title and verify that no listed window is explicitly
   activated.
3. Drag a normal tracked row to another tile and verify actual movement,
   assignment persistence, unchanged current desktop, and an open popup.
4. Drag a normal untracked application row and verify actual movement, unchanged
   current desktop, an open popup, and no new restore record.
5. Drag a globally visible single-window pin and confirm only visual movement.
6. Repeat with an application-wide pin containing multiple windows and confirm
   that only the grabbed row changes tiles.
7. Refresh and search while the popup stays open, then close and reopen it to
   confirm the visual assignment is cleared.
8. Ctrl+Click a global window and verify visual movement, destination switching,
   an open popup, and unchanged global Windows behavior.
9. Cancel drags at each supported stage and verify rollback/no-op behavior.

## Out of scope

- changing Windows pin or unpin state;
- physically moving a globally visible or indeterminate window;
- persisting session-only visual assignments;
- dragging desktop tiles or reordering desktops;
- selecting or moving every window belonging to a pinned application;
- broadening automatic persistence beyond its existing application policy;
- inventing fallback foreground targets when exact activation fails;
- changing popup placement on the primary physical monitor.

## Acceptance criteria

The work is complete when:

- row clicks and tile clicks have the distinct agreed activation behavior;
- ordinary row drag-and-drop moves and saves without switching desktops or
  closing the popup, with persistent assignment restricted to Firefox, Chrome,
  and Edge;
- globally visible windows are never passed to a physical target-move path;
- application-wide pins visually relocate only the selected exact row;
- visual assignments survive only while the popup remains open;
- failure and cancellation paths preserve verified state through rollback;
- regression tests, the complete unit suite, and the production build pass;
- manual QA confirms Windows pin behavior is unchanged.
