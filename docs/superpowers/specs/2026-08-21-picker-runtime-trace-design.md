# Picker Runtime Trace — Design

Date: 2026-08-21
Status: approach approved; design-document review pending
Scope: opt-in diagnostics for the desktop picker only

## Context

Manual verification of the picker-routing changes still shows two defects:

1. Ctrl+Click does not visibly move the captured active window.
2. The picker publishes Firefox rows but no rows for other open applications.

The screenshot contains strings that exist only in the newly built executable,
so this reproduction is not explained by the older root-level executable. The
exact running image must nevertheless identify itself in every diagnostic
session because the product has no build or commit fingerprint in its UI.

The previous design treated process-start access and foreground handoff as the
confirmed causes. The implementation and its tests covered those hypotheses,
but the live symptoms remained. Current source evidence narrows the unknowns
without proving a single root cause:

- non-Firefox rows disappear before row admission or publication, at Alt+Tab
  eligibility, desktop lookup/tile matching, title reading, or a confirmed
  identity loss;
- the mouse route snapshots `MK_CONTROL` correctly, but
  `BeginVerifiedPickerMove` contains several silent entry guards and the
  effect executor discards raw Win32 and COM failure details before the state
  reducer performs retries or rollback.

The existing unit suite exercises pure decisions and source wiring, not the
actual HWNDs, message payloads, private COM calls, desktop readbacks, or focus
handoff in the user's interactive Windows session. An external probe launched
from the development sandbox cannot see that window station and cannot observe
the resident picker's internal transition.

This design adds a temporary, explicit observability path before any further
behavioral fix. It supersedes the previous design's root-cause assertions for
the two unresolved defects; its primary-monitor work remains unaffected.

## Goal

One user reproduction must distinguish all of these boundaries:

- the exact executable content and Windows environment that ran;
- the exact predicate that excludes each missing window;
- whether the click carried `MK_CONTROL` and resolved to a desktop tile;
- the exact guard, if any, that rejects the move before a transition starts;
- the raw result of each move, popup move, desktop switch, and readback;
- the reducer phase, retry, rollback trigger, and terminal outcome.

The diagnostic slice must not change row eligibility, move ordering,
persistence, retry budgets, or user-visible error behavior.

## Alternatives considered

### Directly patch the current hypotheses

Replacing the owner-window predicate and simplifying the controlled move would
be fast, but both changes would be based on inference after an earlier
hypothesis-driven fix already passed tests and failed in the live session.

### Standalone or CLI window probe

A `diagnose-picker` command could explain enumeration when launched in the
interactive window station. It would not see `WM_LBUTTONDOWN`, the resident
target capture, silent move-entry guards, transition effects, or rollback
without adding IPC and duplicating runtime state.

### Opt-in in-process trace — selected

The resident process already owns the HWND enumeration, mouse event, captured
target, COM services, and transition reducer. A bounded trace inside that exact
process is the smallest diagnostic that observes both defects together.

An injectable Win32/COM operations layer remains a useful follow-up for durable
integration tests, but introducing that larger refactor before collecting live
evidence would add another unverified variable.

## Activation and lifecycle

The executable accepts one new GUI flag:

```
build\vde.exe --trace-picker
```

`--trace-picker` starts the normal resident GUI with tracing enabled. It is not
a CLI command, does not bypass the existing single-instance mutex, and is never
written into the autostart registry value. The currently running tray instance
must therefore be exited before starting a trace session.

Tracing is disabled by default and cannot be enabled through settings,
environment variables, or persisted state. There is no network transport,
upload, telemetry endpoint, Windows Event Log registration, or crash reporter.

On startup, VDE creates:

```
%LOCALAPPDATA%\VirtualDesktopsExtention\diagnostics\
    picker-<UTC>-<PID>.jsonl
```

Creation uses a new file name and the profile directory's inherited
current-user access rules. A session is capped at 2 MiB and 10,000 events. When
the first cap is reached, the writer emits one `trace.truncated` event when
space permits and permanently disables further writes for that session.

Retention keeps at most the three newest picker trace files and removes picker
trace files older than seven days. Cleanup is non-recursive, is restricted to
the validated diagnostics directory and the exact
`picker-<UTC>-<PID>.jsonl` filename grammar, and skips reparse points and any
unrecognized entry. Cleanup or write failure only disables diagnostics; it
must never block startup, alter picker behavior, show a modal dialog, or affect
the application's layout files.

The trace handle is closed during normal shutdown. Key boundaries are flushed
after picker model completion and transition terminalization so one completed
reproduction remains readable without forcing every event to stable storage.

## Privacy contract

The trace is local and opt-in. It must not serialize:

- window-title text;
- search text, tab titles, URLs, domains, or sessionstore data;
- command-line arguments of other processes;
- full image paths of enumerated applications;
- layout records, record IDs, or saved browser metadata;
- arbitrary registry values or environment variables.

Per-window records may contain the session-local HWND, PID/TID, class name,
process image basename, title length/read status, styles, desktop GUID, API
status, and decision reason. No title hash is needed because HWND plus the
event sequence provides correlation within one short session.

The `trace.start` provenance record contains a SHA-256 digest of the running
VDE image and a separate SHA-256 digest of its normalized module path, but not
the path text. Known candidate paths can be hashed locally for comparison while
the image digest identifies the exact binary content even after it is copied.
The record also contains the module basename, file size, last-write timestamp,
PE timestamp, application version, PID/TID, process session and
integrity/elevation state, Windows build, trace schema version, and a random
session ID. Failure to hash the image is recorded explicitly and leaves the
remaining provenance fields available.

Privacy enforcement is source-based and schema-based: trace event types accept
only their documented fields and do not accept arbitrary diagnostic text. The
only live foreign-process strings allowed are a length-limited, sanitized class
name and image basename. Tests prove that title/search/session/layout/full-path
sources are never passed to trace APIs; they do not attempt to classify the
literal characters chosen by another application for its permitted class or
basename.

## Trace format

The file is UTF-8 JSON Lines. Each line is independently valid JSON and starts
with common fields:

- `schema`: integer schema version;
- `session`: random session identifier;
- `seq`: strictly increasing event number;
- `ms`: monotonic milliseconds since trace start;
- `event`: stable event name.

Integers that represent HWNDs, styles, Win32 errors, or HRESULTs are formatted
as fixed-width hexadecimal strings where signed JSON conversion would be
ambiguous. GUIDs use canonical text. Event strings and rejection reasons come
from fixed enums rather than live UI text.

## Events

### Provenance and picker opening

`trace.start` records the provenance and environment described above.

`picker.capture` records the foreground HWND/PID, whether the captured identity
is complete, the recapture result, title-read status and length, and whether a
movable target was published. It does not record the title.

`picker.open` records the current desktop lookup, model generation, desktop
snapshot result, target identity presence, and final model/show result.

### Window enumeration

`enum.begin` records the model generation and the ordered desktop GUID snapshot.
`enum.end` records `EnumWindows` success/error, totals, published counts, and
counts grouped by the exact final decision.

When tracing is enabled, every candidate receives one `enum.window` event with:

- enumeration sequence, HWND, PID/TID, class and image basename when readable;
- `IsWindowVisible`, first title-length result, extended styles, tool-window
  predicate, root-owner HWND/self predicate, and the exact Alt+Tab rejection;
- optional owner, last-active-popup, and DWM cloaking observations for diagnosis
  only; these additional facts do not change eligibility in the trace build;
- raw `GetWindowDesktopId` HRESULT, returned GUID, zero-GUID state, and matching
  tile index;
- second title read status and length;
- process-start query/cache status with the immediately captured Win32 error;
- identity completeness and recapture result;
- final `skip(<reason>)`, `display_only`, or `verified` decision.

Existing eligibility calls keep their current order. Values that currently
drive behavior are captured once and reused for logging; tracing must not
re-query them and accidentally choose a different result.

### Pointer activation and move entry

`mouse.down` records raw `wParam`, derived `MK_CONTROL`, client point, resolved
activation kind, selected tile, search-active state, and whether a transition
was already controlled.

`activation.request` and `activation.result` span the route from the resolved
pointer/keyboard action through `Activate` to the call of
`BeginVerifiedPickerMove`. They name early outcomes that occur before
`move.begin`, including an already controlled transition, invalid tile index,
selection publication failure, ordinary-switch routing, and successful
dispatch into move entry. Thus every Ctrl activation has a closed request/result
pair even when `BeginVerifiedPickerMove` is never called.

`move.begin` records either `accepted` with generation, target/origin/
destination/popup/current desktops and first effect, or one fixed rejection
reason for every existing silent exit. Reasons include invalid index or
selection, missing services, target mismatch, invalid HWND, unavailable
current/popup desktop, capture/identity failure, accepted-plan conflict,
reservation staging failure, and no initial effect. A caught exception is a
separate `move.begin.exception` event with an explicit
`transition_published` flag: an exception after transition publication still
leaves the durable pump responsible for completion or rollback and must not be
misreported as an entry rejection.

Instrumentation preserves the existing guard order and return behavior. It
does not relax a safety check in the diagnostic build.

### Effects, Win32/COM calls, and reducer

`effect.queue`, `effect.execute`, `effect.observation`, and `effect.reduce`
record generation, serial, kind, phase before/after, execution route, identity
validity, invocation state, acceptance state, and next effect.

Call-specific events retain raw results before they are reduced to booleans:

- `GetViewForHwnd`, `MoveViewToDesktop`, or `MoveWindowToDesktop` HRESULT;
- each target/popup `GetWindowDesktopId` HRESULT and actual GUID;
- every destination lookup stage, including raw `GetDesktops`, `GetCount`,
  `GetAt`, and `GetID` HRESULTs, examined index/GUID, and the final match or
  failure stage; the existing `GetDesktopByGuid`/`GetDesktopIndexByGuid`
  boolean wrappers must not be the only observation boundary;
- Progman and foreground HWND/thread IDs and the computed handoff plan;
- each `AttachThreadInput`, `SetForegroundWindow`, detach, and Progman cleanup
  return value with the API's real return semantics; `GetLastError` is captured
  immediately only for calls whose contract defines it, while calls such as
  `SetForegroundWindow` explicitly mark extended-error data unavailable and
  `ShowWindow` records previous visibility rather than a success boolean;
- `SwitchDesktop`, `GetCurrentDesktop`, and `GetID` HRESULT plus actual GUID.

`transition.terminal` records success/cancellation/failure, retry counts,
rollback trigger, diagnostic enum/code, and final target/popup/current desktop
readbacks. It does not include the user-facing diagnostic sentence.

`terminalization.attempt` records every runtime-finalization attempt, including
the terminal-acknowledged and pending-effect preconditions, exact reservation
release result or exception, runtime-key presence, readiness decision,
`FinalizePickerTransition` result, and whether delivery was deferred, posted,
timer-backed, or left to a durable kick. `transition.terminal` is emitted only
after finalization succeeds. This preserves evidence when terminalization makes
no progress and no terminal event would otherwise appear.

## Architecture

A small picker-trace module owns:

- the enable flag and validated file lifecycle;
- fixed event/reason enums;
- JSON escaping and serialization;
- the byte/event budget;
- a production file sink and a no-op sink;
- non-throwing helper methods used at the runtime boundaries.

The default sink is a zero-cost no-op apart from one enable check. Trace calls
are `noexcept`; allocation, encoding, file, or cleanup errors disable the sink.
No trace exception crosses into product logic.

Window eligibility is represented as a fact record plus the existing decision,
so the production path can log which already-observed predicate rejected the
candidate. Silent move-entry returns receive named rejection helpers. COM and
Win32 adapters expose raw results to the trace before constructing the existing
`PickerObservation`; the state-machine semantics remain unchanged.

## Testing strategy

Implementation follows test-driven development. Tests are required before the
corresponding production changes for:

- parsing `--trace-picker` as an opt-in GUI mode while ordinary GUI and CLI
  commands remain unchanged;
- deterministic JSON serialization and escaping;
- strictly increasing sequence numbers and monotonic offsets;
- byte/event caps and a single truncation marker;
- write/create/cleanup failures disabling trace without changing a supplied
  product result;
- strict cleanup filename matching and refusal to follow reparse points;
- a privacy allowlist proving titles, search text, URLs, full foreign process
  paths, layout records, and live diagnostic sentences are not trace inputs;
- SHA-256 image/path provenance, including explicit hash failure;
- every window-enumeration final reason;
- every activation result before move entry, every move-entry guard reason, and
  pre-publication/post-publication exception observations;
- raw HRESULT and immediate Win32-error preservation;
- effect phase/serial correlation through retry, rollback, and terminalization;
- terminalization no-progress and durable-kick routes even when no terminal
  event is emitted;
- equivalence tests showing trace-disabled and trace-enabled fake runtime
  operations return the same product decisions and effects.

Source-substring assertions alone are not acceptance evidence for this slice.
The full unit suite and production build must pass before the diagnostic binary
is offered for reproduction.

## Manual reproduction protocol

1. Exit the current VDE tray instance.
2. Start the exact freshly built `build\vde.exe --trace-picker`.
3. Keep at least one known missing non-Firefox application open.
4. Open the picker once and perform one Ctrl+Click to a different desktop.
5. Wait for the picker to settle, then exit VDE from the tray.
6. Inspect the newest picker JSONL file and map both symptoms to their first
   failing runtime boundary.

The trace result becomes evidence for a separate corrective design amendment
and TDD implementation. The final fix is not inferred from this trace design.

## Out of scope

- changing Alt+Tab eligibility or desktop routing in the diagnostic slice;
- weakening active-window identity validation;
- changing controlled-transition ordering, retry, rollback, or persistence;
- adding general telemetry or a permanent debug setting;
- automatic upload or support-bundle collection;
- expanding automatic persistence beyond enabled browser profiles.

## Acceptance criteria

The diagnostic slice is complete when:

- tracing is disabled during every ordinary launch;
- one explicit trace launch identifies its exact executable;
- every absent row has one unambiguous final rejection/admission reason;
- one Ctrl+Click has a complete activation request/result pair and identifies
  modifier capture, move-entry acceptance/rejection, every invoked API result,
  every readback, and terminalization progress or terminal reducer outcome;
- the privacy, cap, retention, fail-open, and behavior-equivalence tests pass;
- the complete unit suite and production build pass;
- a live reproduction produces a readable bounded trace without changing the
  picker's existing behavior.
