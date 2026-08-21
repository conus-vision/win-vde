# Picker Window Routing Fixes — Design

Date: 2026-08-21
Status: approved for planning
Scope: desktop picker only

## Context

The picker currently has three user-visible defects:

1. Ctrl+Click may complete as an apparent no-op.
2. The window list may contain only Firefox windows even though the picker is
   intended to show ordinary windows from every application.
3. The popup follows the active window to a secondary physical monitor instead
   of opening on the primary monitor.

Automatic window-memory remains intentionally limited to Firefox, Chrome, and
Edge. Expanding automatic persistence to other applications is out of scope.

## Confirmed causes

### Ctrl+Click desktop switch

The mouse route reaches `Activate` and `BeginVerifiedPickerMove`. The controlled
transition then calls `IVirtualDesktopManagerInternal::SwitchDesktop` directly.
The established ordinary-click path performs a Progman/foreground handoff first
because Windows can otherwise return to the original desktop. When that bounce
happens, the controlled transition correctly detects the mismatch and rolls the
target window, picker, and current desktop back, which appears to the user as no
action.

The mouse handler also re-reads Ctrl with `GetKeyState` after dispatch has begun
instead of preserving the `MK_CONTROL` bit supplied with `WM_LBUTTONDOWN`.

### Missing non-Firefox rows

Picker enumeration is separate from browser persistence and does not explicitly
filter for Firefox. However, each row currently requires a complete stable
identity `{HWND, pid, processStart}`. If querying process creation time is denied
for an elevated, protected, or packaged application, the row is silently
dropped. Firefox-only output is therefore an incidental result of process-query
availability.

### Physical monitor selection

`ShowPicker` calls `MonitorFromWindow` with the captured target HWND. The
`MONITOR_DEFAULTTOPRIMARY` flag is only a fallback for an unmatched window; a
valid window on a secondary monitor selects that secondary monitor.

## Required behavior

- The picker shows every eligible ordinary Alt+Tab window for which a non-empty
  title and a virtual-desktop GUID can be read.
- Inability to obtain a stable process identity does not hide an otherwise
  displayable row.
- Ctrl+Click on a desktop moves the captured active window, switches to the
  destination desktop, and keeps the picker open when the existing safety
  preconditions are met.
- Mouse Ctrl state comes from the button event that triggered activation.
- The picker is centered in the work area of the primary physical monitor only.
- Automatic persistence and browser tab enrichment remain limited to enabled
  Firefox, Chrome, and Edge profiles.

## Design

### 1. Separate row presentation from stable identity

Introduce a small, pure row-admission decision with three outcomes:

- `Skip`: the window is not Alt+Tab eligible, lacks a desktop or title, or a
  completed identity changed during capture.
- `DisplayOnly`: desktop and title are valid, but a stable process identity is
  unavailable or indeterminate.
- `Verified`: desktop, title, and the complete stable identity are valid.

`EnumAll` will collect desktop and title independently from the best-effort
identity capture. Both `DisplayOnly` and `Verified` rows are published.

Identity-dependent behavior is fail-closed:

- full-identity active highlighting and browser-tab search enrichment apply only
  to `Verified` rows;
- the identity-keyed icon cache applies only to `Verified` rows;
- `DisplayOnly` rows use the shared fallback icon and title-only filtering;
- automatic persistence never consumes picker display-only rows.

This preserves the anti-HWND-reuse safety contract while making presentation
independent of cross-process query permissions.

### 2. Use one verified desktop-switch primitive

Extract the known foreground handoff and desktop switch into one helper used by
both ordinary clicks and controlled forward/rollback effects. The helper will:

1. resolve the destination desktop;
2. locate Progman and attach the relevant input queues when available;
3. give the shell foreground ownership;
4. immediately detach every input queue it attached, in reverse order;
5. invoke `SwitchDesktop` exactly once and retain the existing Progman cleanup.

The input queues stay attached only long enough to perform the foreground
handoff. They are detached before the undocumented COM switch call, preserving
the established ordinary-click sequence and avoiding cross-thread queue
coupling while the desktop changes.

The helper reports whether the COM switch was actually invoked and returns its
HRESULT. The existing controlled reducer remains responsible for readback,
bounded retry, rollback, persistence, and focus restoration.

For mouse activation, Ctrl is captured immediately from
`WM_LBUTTONDOWN.wParam & MK_CONTROL` and passed unchanged through pointer
dispatch to `Activate`. Keyboard activation continues to use keyboard state at
the key event.

### 3. Route popup placement to the primary monitor

Add a primary-work-area helper that resolves the monitor containing virtual
screen point `(0, 0)` with `MONITOR_DEFAULTTOPRIMARY`, validates
`GetMonitorInfoW`, and returns `rcWork`. If monitor-info lookup fails, use the
system primary work area; never fall back to the target window's monitor.

A pure centering helper computes the popup origin from the chosen work area and
the adjusted outer window size. `ShowPicker` uses only these helpers and no
longer consults `g_target` for physical placement.

Virtual-desktop routing is unchanged: during a controlled move, the popup may
still be moved to the destination virtual desktop so that it remains visible
after switching.

### 4. Preserve the persistence boundary

The browser-only collector, profile classifier, tab-session enrichment, layout
records, and save/restore paths are unchanged. A non-browser window can be
moved manually by Ctrl+Click when its captured active identity is safe, but that
manual action must not create or mutate an automatic layout record.

## Failure handling

- A volatile row failure affects only that row; allocation failure or a failed
  global desktop snapshot still aborts model publication transactionally.
- An unsafe or lost active target remains non-movable; no weakened identity is
  passed to undocumented move APIs.
- A failed desktop switch is returned to the existing reducer, which verifies,
  retries within its current budget, and rolls back when necessary.
- A failed primary-monitor query uses the primary system work area rather than a
  secondary-monitor target fallback.

## Verification

Automated tests will cover:

- `Verified`, `DisplayOnly`, and `Skip` row-admission decisions;
- display-only rows remaining searchable by title while identity-only behavior
  stays disabled;
- mouse activation preserving `MK_CONTROL` from the original button event;
- ordinary and controlled desktop switching sharing the foreground-handoff
  helper, with no direct controlled `SwitchDesktop` call;
- primary-work-area centering, including taskbar offsets and negative virtual
  screen coordinates;
- source wiring that keeps picker enumeration separate from browser profiles;
- the existing invariant that Ctrl-moving a non-browser never mutates automatic
  layout state.

The complete unit suite and production build must pass. Manual Windows QA should
place the foreground app on a secondary monitor, open the picker, confirm the
popup is on the primary monitor, verify non-browser rows are visible, and perform
Ctrl+Click to a different virtual desktop.

## Out of scope

- automatic persistence for applications other than Firefox, Chrome, and Edge;
- moving an active window whose identity cannot be safely validated;
- reproducing every private Windows Alt+Tab eligibility rule;
- changing popup visuals, layout, search syntax, or virtual-desktop semantics.

## Acceptance criteria

The change is complete when all three reported defects are covered by regression
tests, the full test/build verification passes, and no browser-only persistence
boundary has been broadened.
