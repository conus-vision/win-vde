# Browser-window lifecycle: coverage audit + session reopen (checkpoints)

Date: 2026-08-28
Scope: `src/session.hpp`, `src/session_worker.hpp`, `src/reconcile_worker.hpp`,
`src/lifecycle.hpp`, `src/layout.hpp`, `src/vde.cpp`, new `src/tabsnap.hpp`.

This document has two halves:

1. **Audit** — every way a browser window can appear, change or vanish, and what
   win-vde does (or fails to do) about it.
2. **Design** — the session-checkpoint feature added on this date: remember what
   each window contained, and reopen a whole browsing layout on demand.

---

## 1. How the tracker works today

One monitor tick every **5 s** (`MONITOR_INTERVAL_MS`):

1. `EnumWindows` keeps top-level windows that are `WS_VISIBLE`, whose class is in
   a profile (`MozillaWindowClass`, `Chrome_WidgetWin_1`) and whose process image
   basename matches (`firefox.exe`, `chrome.exe`, `msedge.exe`). Identity is
   `(hwnd, pid, process start time)`.
2. `GetWindowDesktopId` gives each window's virtual desktop GUID.
3. A worker decodes the browser session file — Firefox `recovery.jsonlz4`,
   Chromium `Session_*` (SNSS) — into per-window fingerprints (`WinFp`).
4. `BuildReconcileLivePreparation` associates a decoded session window with a
   live HWND **by exact active-tab title** (after stripping a known title
   suffix), first come first served.
5. `PlanAppReconcile` matches saved records to live windows with a one-to-one
   min-cost assignment over `LayoutScore` (0.40·cosine of domain counts +
   0.25·Jaccard + 0.20·active tab + 0.15·tab-count closeness), acceptance
   threshold **0.55**.
6. Matched + already bound to that HWND → refresh the record. Matched + unbound
   (i.e. the window is *new to this process*) and on the wrong desktop → move it
   back. Unmatched live window → new record. Unmatched saved record → mark
   missing; records expire after **30 days**.

Everything the layout remembers about a window is: app, desktop GUID + index,
active-tab title, active domain, tab count, and a `domain → count` map. It never
remembered the tabs themselves, nor the window geometry.

---

## 2. Audit: situations and coverage

Legend: **OK** — handled; **GAP** — not handled or handled wrongly.

### 2.1 Ordinary lifecycle

| # | Situation | Today |
|---|---|---|
| 1 | Window opens while VDE runs | OK — becomes a new record on its current desktop |
| 2 | Window closes | OK — record marked missing, kept 30 days, then expires |
| 3 | Whole browser closes | OK — `MarkOnlyObservedAppsMissing` marks only that app |
| 4 | Browser restarts and restores its session | OK — records rematched by fingerprint, windows moved back |
| 5 | Machine reboots | OK — restore on start, and ~20 s after a browser launch (`LC_SETTLE_TIMEOUT_MS`) |
| 6 | User moves a window between desktops | OK — the bound record follows the window |
| 7 | User navigates, opens/closes tabs | OK — the bound record's fingerprint is refreshed |
| 8 | Sleep / hibernate / fast startup | OK — HWNDs survive, nothing to do |
| 9 | Log off / shutdown | OK — `WM_QUERYENDSESSION` and `WM_ENDSESSION` both checkpoint |
| 10 | Windows update breaks the undocumented COM | OK — degraded mode with an explanation |
| 11 | Pinned window / "show on all desktops" | OK — never moved physically (`DecideTargetMobility`) |
| 12 | Minimized window | OK — `WS_VISIBLE` stays set while minimized |
| 13 | Thousands of windows / pathological matching | OK — bounded, plan deferred instead of guessing |

### 2.2 Gaps found

**G1 — Session windows are associated to live windows by exact title only. (fixed, see §5.1)**
`BuildReconcileLivePreparation` builds `title → deque<session index>` and pops
the first entry. Two windows whose active tab has the same title (two "New Tab"s,
two GitHub PR pages, two OneDrive folders) get each other's tab sets, so both
fingerprints are wrong and the desktop assignment can be swapped. A stale session
file makes this silently more likely.
*Fix:* score the association instead of trusting the title — require the active
domain to agree too, prefer the candidate with the closest tab count, and leave
the pair **unassociated** when two candidates tie. An unassociated window falls
back to a title-only fingerprint, which is honest; a wrongly associated one is
not.

**G2 — Only one browser profile is ever read. (fixed, see §6.1)**
`ResolveBrowserSessionPath` reads `<UserData>\Default\Sessions\Session_*` for
Chromium and the single default profile from Firefox's `profiles.ini`. Windows
belonging to a second Chrome profile ("Person 2"), an Edge work profile, or a
Firefox `-P` instance never get tab fingerprints at all — they degrade to
title-only matching for as long as they exist.
*Fix:* enumerate every profile directory (`<UserData>\*\Sessions\Session_*`, all
`Profile*` sections of `profiles.ini`), decode each, and tag the resulting
`WinFp`s with their profile so association cannot cross profiles. Cost is linear
in the number of profiles and can be limited to profiles with a running process.

**G3 — PWA, app, DevTools and Picture-in-Picture windows count as browser windows. (partly, see §6.4)**
They share `Chrome_WidgetWin_1` and the browser executable, so they enter the
window set, never appear in the session file, and compete for matches with real
browser windows.
*Fix:* SNSS records `kCommandSetWindowType` (9/32) and `kCommandSetWindowAppName`
(15); decode them and only treat `TYPE_NORMAL` windows as browser windows.
Cheap heuristics help too (a DevTools/PiP window has no tab strip and usually a
distinctive title/owner).

**G4 — Closed Chromium tabs and windows were resurrected. (fixed here)**
SNSS is an append-only command log. The reader handled commands 0/2/6/7/8 and
ignored `kCommandTabClosed` (16) and `kCommandWindowClosed` (17), so every tab
and window closed since Chrome last rewrote the file was still decoded as live.
Ghost windows stole title associations from real ones and inflated tab counts.
*Fixed:* the parser now replays 16/17 and forgets those tabs/windows (including
their retained-text budget).

**G5 — Title decorations are not normalized. (fixed, see §5.2)**
Only a fixed suffix list is stripped. An unread-count prefix (`(3) Inbox …`), a
media-playing glyph, or Edge's `[InPrivate]` marker changes the title, which both
breaks the association above and the `activeTitle` half of `LayoutScore`.
*Fix:* normalize before comparing — drop a leading `(\d+)␠`, leading media
glyphs, and the private-mode markers (`NormalizeProvisionalAdoptionTitle` is the
natural place; it already lowercases and collapses whitespace).

**G6 — Private/incognito windows can capture an unrelated record. (mitigated, see §5.5)**
They are absent from session files, so their fingerprint has empty `counts`, and
`LayoutScore` then returns 1.0 for *any* saved record with the same title and 0
otherwise. Two private windows are indistinguishable.
*Fix:* treat an empty-counts live window as ineligible for restoring an
established (non-provisional) record unless the title match is unique on both
sides — the machinery for "unique title" already exists in the provisional
adoption path.

**G7 — Ambiguous fingerprints can swap windows between desktops. (fixed, see §5.3)**
When several windows share a domain set (two windows on the same site, or two
empty "New Tab" windows), the assignment is decided by a deterministic tie-break,
not by evidence. The result is stable but arbitrary: window A can be sent to B's
desktop and vice versa.
*Fix:* when the top two candidate assignments score within epsilon of each other
and the windows are on different desktops, skip the move rather than gamble, and
keep the record bound to whichever window is already correct.

**G8 — A cold record can yank a brand-new window. (fixed 2026-08-29, see §4)**
A record that has been missing for three weeks still restores at score ≥ 0.55.
Open a link that happens to land on the same site and the window is teleported to
another desktop, seemingly at random, with no undo.
*Fix:* raise the acceptance threshold as a function of how long the record has
been missing (e.g. ≥ 0.75 after a day, plus at least two shared domains), add
**Undo last restore** to the tray menu, and a "Forget this window" action in the
picker so a wrong memory can be deleted.

**G9 — A deleted virtual desktop makes records permanently unrestorable. (fixed, see §5.4)**
`ResolveSavedDesktop` matches by GUID only. If the user deletes desktop 3,
Windows moves its live windows elsewhere (bound records follow), but every
*closed* window's record keeps a GUID that no longer exists, and
`SavedRestoreDestinationAvailable` will refuse forever — silently.
*Fix:* fall back to the saved `deskIndex` when the GUID is gone (this is what the
new reopen path does), and optionally offer to recreate the missing desktops
(`IVirtualDesktopManagerInternal::CreateDesktop` is already declared).

**G10 — Window geometry is not part of "restore".**
Only the virtual desktop is remembered. After a reboot the windows land on the
right desktops with whatever size, position, monitor and maximized state the
browser chose. For a multi-monitor setup this is most of what the user perceives
as "my layout".
*Fix:* store `(monitor device name, normal rect, show command)` per record and
reapply with `SetWindowPlacement` after the desktop move; guard it behind a
setting since browsers also restore geometry themselves.

**G11 — Window split/merge is not modelled. (fixed, see §6.2)**
Dragging a tab out creates a window whose fingerprint is a subset of the original
and shrinks the original at the same instant; merging does the reverse. The
matcher sees one plausible match and one new/missing record, so the derived
window can inherit the parent's record (and its desktop) or the merged window can
leave a duplicate record behind.
*Fix:* detect the subset relationship in the same reconcile pass (a new live
window whose domains are a subset of a record whose tab count just dropped by the
same amount) and create a fresh record for the derived window instead of
matching.

**G12 — The COM services are never re-acquired. (fixed, see §5.6)**
`InitServices()` runs once at startup. If `explorer.exe` restarts (or the shell
crashes), `IServiceProvider`/`IVirtualDesktopManagerInternal` proxies become
disconnected and every subsequent move fails until VDE itself is restarted.
*Fix:* on `RPC_E_DISCONNECTED` / `CO_E_OBJNOTCONNECTED` / `RPC_S_SERVER_UNAVAILABLE`
from any desktop call, release and re-initialize the services once, then retry
the operation; report degraded mode only if that fails.

**G13 — A 5 s poll is the only observation channel. (fixed, see §6.3)**
A window that opens and is moved by the user inside one tick is seen already
moved; a window that opens and closes between ticks is never seen. The launch
settle (~20 s) hides most of it, but the blind window also delays binding, which
is exactly when a wrong restore can fire.
*Fix:* add a `SetWinEventHook` for `EVENT_OBJECT_CREATE`/`DESTROY`/`SHOW`
(out-of-context, filtered by class) as an *edge trigger* for the existing tick —
polling stays as the safety net.

**G14 — An elevated browser cannot be moved. (fixed, see §6.5)**
VDE runs unelevated; moving a window owned by an elevated process fails with
access denied and is reported as a generic failure.
*Fix:* detect the elevation mismatch once per process and say so plainly instead
of retrying.

**G15 — Losing VDE abruptly loses nothing, but there was no "current" session
copy.** The layout file is flushed within 500 ms of a change, so the *layout*
survives a kill. The new session snapshot would not have — hence the periodic
"saved" slot described below.

---

## 3. Design: session checkpoints and reopen

### 3.1 What is stored

`src/tabsnap.hpp` (pure logic, unit-tested) defines a snapshot:

```
SessionSnapshot { kind (saved|exit), capturedUtc, desks[], windows[] }
SnapWindow      { app, recordId, desktop GUID, deskIndex, activeTab,
                  activeTitle, tabs[] }
SnapTab         { url, title }
```

Serialized as tab-separated text with base64 for free text, in the same style as
the v4 layout file:

```
# VDE session snapshot v1
M   exit    1756000000
D   0   {GUID}  <b64 name>
W   firefox {recordId}   0   {GUID}  1   <b64 active title>
T   <b64 url>   <b64 title>
```

Limits: 512 windows, 2000 tabs/window, 20000 tabs total, 8 KiB per URL, 16 MiB
per file. Parsing is strict and total: any malformed line rejects the file.

### 3.2 Where the tabs come from

No new reads. Each reconcile result already carries the decoded session windows
and a `fastIndex → session index` map; `RememberSessionTabs` copies the ordered
tab list of every window that is bound to a layout record into
`g_tabsByRecord`. To make that possible, `WinFp` now carries
`std::vector<SessionTab> tabs` and `activeTab`, filled by both the Firefox and
the SNSS decoder (the SNSS one now also orders tabs by their in-window index).

### 3.3 The five slots

| Slot | File | Written when |
|---|---|---|
| 0 | `sessions\session-saved.txt` | "Save windows layout", and in the background at most every 5 min |
| 1..4 | `sessions\session-exit-N.txt` | On each shutdown checkpoint, rotating oldest-out |

Rotation moves 3→4, 2→3, 1→2 (oldest first, so a failed rename can never
overwrite a newer snapshot with an older one), then writes slot 1. One shutdown
consumes exactly one slot even though Windows checkpoints twice
(`WM_QUERYENDSESSION` then `WM_ENDSESSION`).

Snapshots are a cache, never the authority: a snapshot failure never fails a
layout checkpoint, and the writer is a plain temp-file + `MoveFileEx` replace
rather than the layout store's transactional machinery.

### 3.4 Reopening

Tray menu → **Reopen browser windows...** lists the five slots with their local
date/time, window and tab counts, and which browsers they contain; the browser
checkboxes are limited to the browsers present in the selected checkpoint.

`BuildReopenPlan` turns the selected windows into launch jobs:

- Every URL passes `SanitizeReopenUrl`: `http`, `https`, `ftp`, `file:///` only;
  anything containing a space, quote, backslash or control character is dropped
  rather than escaped, and a URL may never start with `-`. A snapshot must not be
  able to inject extra command-line switches into a browser launch, and
  `javascript:`/`data:` must never be replayed.
- Chromium takes all of a window's URLs in one `--new-window` invocation; Firefox
  gets `-new-window <first url>` and then positional URLs, which it opens as tabs
  of the window it just created. Both are chunked to stay under an 8000-character
  command line.
- The executable is resolved from a *running* window of that browser first (the
  exact install the snapshot came from), then machine-wide `App Paths`, then
  per-user `App Paths`, then the usual install directories. Machine-wide comes
  before per-user because a per-user `chrome.exe` App Paths entry can belong to a
  Chromium fork.

Execution is a timer-driven state machine (`TIMER_REOPEN`, 400 ms):
`Launch → AwaitWindow → Tabs → Move` per window, with a 45 s timeout waiting for
a window to appear. The new window is identified by diffing the app's window set
against the set captured immediately before the launch. It is then moved to the
snapshot's desktop GUID, or — if that desktop no longer exists — to the desktop
now sitting at the saved index (G9's fix, applied here). Automatic observation is
suspended for the duration so the tracker does not fight half-built windows.

### 3.5 Deliberate limitations

- Reopening **adds** windows; it never closes or reuses what is already open, so
  reopening a checkpoint that is still open duplicates it. The confirmation
  dialog says so.
- Tab history, pinned tabs, tab groups, scroll position, form data and private
  windows are not reopened — the session files do not expose them in a form VDE
  can replay through a command line.
- A snapshot only contains windows whose tabs VDE decoded while they were open,
  which excludes private windows and (until G2 is fixed) secondary browser
  profiles.
- Snapshots are only written while automatic layout tracking is enabled.

### 3.6 CLI

`vde checkpoints` lists the five slots (index, kind, capture time, window/tab
counts, apps) for support and debugging.

---

## 4. Restore only when the window identity is actually gone

Added 2026-08-29, after G8 turned out to be the one gap users actually feel.

### 4.1 The distinction that was missing

While VDE runs, a window *is* identified exactly: `(HWND, PID, process start
time)`. A bound window keeps its record through any amount of tab churn — the
content fingerprint is only needed to re-find a window whose identity is gone,
which happens exactly when the browser restarted.

Those bindings lived only in memory, so VDE could not tell three very different
situations apart:

| Situation | What it means | What VDE did | What it must do |
|---|---|---|---|
| Browser restarted (reboot, update, crash) | identities gone, Windows scattered the windows | restore | restore |
| VDE restarted, browser untouched | identities gone *to VDE only*; nothing moved | restore — could re-place windows the user had arranged | adopt silently, move nothing |
| One more window opened while the browser runs | the user's own action | could match a weeks-old record and teleport the window | record it where it is |

### 4.2 Persisted bindings

`bindings.txt` (`src/binding_store.hpp`, pure logic + unit tests) holds one line
per bound window:

```
# VDE bindings v1
B <app> <recordId> <hwnd> <pid> <processStart>
```

It is rewritten from the observation loop whenever the binding set changes
(cheap FNV signature) and at every checkpoint, using the same temp-file +
replace writer as the session snapshots — it is a cache, never the authority.

At startup every entry is verified against the live window set: the app, the
window handle, the PID **and** the process start time must all match, and the
record must still exist. Windows reuses handles after a process dies, so the
handle alone is not enough; the 100 ns process start time makes the triple
unique in practice. Whatever matches is adopted silently, which excludes those
windows from the reconcile plan and therefore from any move.

### 4.3 The restore gate

Per app, VDE is **cold** while it holds no live binding and **warm** once it
does:

- **Cold** — the browser restarted, or VDE started and nothing matched. Moves
  are allowed: match by tabs, put the windows back.
- **Warm** — VDE still holds live bindings. A window appearing now is the user's
  doing, so the plan still runs (records are created and refreshed) but its
  moves are suppressed.
- **Grace** — for `RESTORE_GRACE_MS` (60 s) after an app first warms up, moves
  are still allowed. Firefox restores some windows lazily, seconds apart; without
  the grace those late windows would be classified as user-opened and left
  wherever Windows put them.

The gate is state-based, not event-based, which matters: a browser that closes
and reopens between two 5 s polls leaves no live binding, so the restart is
still recognized even though VDE never observed the transition.

### 4.4 Recording a new window immediately

An unbound window used to be dropped from the reconcile plan until its session
association was accepted as fresh, and new records are only created from a Fresh
plan. Measured on a real machine: a window opened at 22:41 still had no record
at 22:49 and was finally written by the exit checkpoint — as a title-only
record. While an app is warm, a window that stays unbound for
`WARM_RECORD_DELAY_MS` (10 s) is now recorded in place with a title-only
provisional record and bound; the next pass that carries decoded session data
upgrades it in place (`CommitBoundRecordRefresh` clears `provisional`).

### 4.5 Verified live

| Case | Expected | Observed |
|---|---|---|
| `bindings.txt` removed (browser restart) | window matched by tabs and moved to its record's desktop | moved AScanner → Desktop 1 |
| VDE restarted, bindings kept, record pointing elsewhere | window untouched, record follows the window | stayed on Desktop 1; record rewritten to Desktop 1 |
| New window opened while warm, fingerprint matching a record on another desktop | not moved, recorded in place | stayed; record created within ~20 s, then upgraded with its real fingerprint |

---

## 5. Matching and recovery pass (2026-08-29)

### 5.1 Association by evidence, not by title (G1)

`BuildReconcileLivePreparation` no longer pops the first session window with a
matching active-tab title. Order of evidence:

1. **The record the window already owns.** The request now carries a
   `BoundLiveFingerprint` per live window (URL signature, tab count, domain
   counts, taken from that window's record). A session window whose pages match
   it is the association — this is stable across ticks and immune to duplicate
   titles.
2. **A normalized title that is unique on both sides.** Only when exactly one
   live window and exactly one remaining session window carry it.
3. **Nothing.** An ambiguous window is left unassociated and keeps a title-only
   fingerprint. A wrong tab set is worse than no tab set: it corrupts the
   record, and through it every later match.

### 5.2 Titles normalized before comparison (G5)

`StripTitleUnreadCounter` drops a leading `(\d+)` run, and
`NormalizeProvisionalAdoptionTitle` (which already lowercased and collapsed
whitespace) applies it. The counter lives in the page's own title, so it appears
on both sides — but it moves the instant the page changes it, while the browser's
session file still holds the previous value. Normalizing both sides keeps a
window associated with itself while the counter ticks. The stored title stays
raw; only comparisons normalize.

### 5.3 Exact page sets, and never moving a window for nothing (G7)

Two windows holding exactly the same pages are interchangeable: what matters is
that each saved desktop ends up with one of them, not which one.

- `LayoutWin::urlSignature` — an order-independent hash of the window's full tab
  URLs, 0 when unknown. Layout format **v5** carries it in a `U` companion line
  next to the existing `P`; v4 files still load (their records simply have no
  signature until the next refresh), and the legacy migration now installs v5.
- `LayoutScore` returns 1.0 immediately when both signatures are non-zero and
  equal — the same pages are the same window, whatever the title says.
- `LayoutMatch::inPlace` marks a candidate whose window already sits on the
  record's desktop, and `AssignOneToOne` weights it into the deterministic tie
  term. Among equally scoring assignments the min-cost flow now picks the one
  that **moves nothing**, instead of an arbitrary permutation that shuffles
  indistinguishable windows between desktops.

### 5.4 A deleted desktop no longer strands records (G9)

`ResolveRestoreDestination` returns the saved GUID when that desktop still
exists, otherwise the desktop that now occupies the saved *position*. The
restore path uses it as a fallback, which is what the reopen path already did.

### 5.5 No move without evidence (G6)

A live window whose pages VDE could not read (private window, unreadable session
data, or an association refused by §5.1) may still *match* a record that does
know its pages — but the move is skipped. The record follows the window instead
of the window following the record.

### 5.6 Re-acquiring the shell services (G12)

`RepairDesktopServices` releases and re-creates the ImmersiveShell proxies, at
most once per 30 s, when `CurrentDesktops` — the call every other path goes
through — stops working and a sanity check confirms the services are gone. A
successful repair says so once in the tray. Previously an `explorer.exe` restart
silently disabled every move until VDE itself was restarted.

### 5.7 Verified live

Restarting VDE over a 44-window Firefox session with the new matching core moved
**no window** (the one difference in the before/after capture was a window the
user closed meanwhile); all 43 remaining windows were re-adopted from
`bindings.txt`; the layout migrated to v5 in place and started carrying `U`
signatures for the windows whose session data had been decoded.

### 5.8 Still open

| Gap | Why it is not in this pass |
|---|---|
| G2 — one browser profile only | The session worker caches one path and one file stamp per app; reading several profiles means reworking that model, not just the path resolver. |
| G3 — PWA / DevTools / PiP windows | SNSS only describes *session* windows, and a PWA is not one; excluding them needs an OS-side signal Chromium does not expose through Win32. |
| G10 — window geometry | The biggest remaining user-visible gap. Needs a monitor-identity model, `SetWindowPlacement` on the restore path, its own setting, and its own live verification. |
| G11 — window split/merge | Now tractable with URL signatures (a derived window's page set is a subset of the parent's), but needs the previous page set kept per record. |
| G13 — event-driven observation | A `SetWinEventHook` would cut the 5 s blind window; with restores now gated to the cold path its remaining value is faster recording, not correctness. |
| G14 — elevated browser | Cosmetic: report the elevation mismatch instead of a generic move failure. |

---

## 6. Second recovery pass (2026-08-29)

### 6.1 Every profile the browser actually has open (G2)

`ResolveBrowserSessionPaths` enumerates the profiles of an app — Chromium's
`Default` plus every `Profile*` directory, Firefox's full `profiles.ini` — and
keeps the ones that are **running right now**. The signal is exact rather than
heuristic: a browser holds an exclusive lock on one file per open profile
(`<profile>\parent.lock` for Firefox, `<profile>\Local Storage\leveldb\LOCK`
for Chromium), so a failed exclusive open means "in use". Verified on a machine
with four Chrome profiles, of which none was open, and two Firefox profiles, of
which exactly one was.

Ordering is deliberate: the default profile first when it is running, then the
other running ones. The first path stays the worker's verified source (stamp →
read → stamp again, unchanged); the rest are read best effort inside
`ParseBrowserSessionData` and appended, so a second profile's windows get real
fingerprints instead of falling back to their titles. When nothing looks
running, the default profile is used exactly as before.

### 6.2 Split and merge are recognized, not guessed (G11)

Dragging a tab out spreads one record's pages over two windows; merging puts two
records' pages into one. Whichever window then wins the match, moving it would
be a guess about which half "is" the remembered window.
`LooksLikeWindowSplit` / `LooksLikeWindowMerge` detect exactly that shape —
containment of domain counts plus a sibling window (or a second record) holding
the rest — and the plan then keeps the match but drops the move: the record
follows the window instead. An ordinary window that merely lost some tabs, with
nothing else holding the record's pages, still restores.

### 6.3 One rare event instead of a busy hook (G13)

The requirement was speed *without* load. Hooking window creation would mean a
callback for every menu, tooltip and control on the desktop, so VDE hooks
`EVENT_SYSTEM_FOREGROUND` alone: it fires a handful of times a minute, a new
browser window always takes focus, and closing one hands focus to something
else. The callback does two integer tests, one class-name read, and posts at
most one wake per second; the observation body itself is shared with the 5 s
timer (`RunMonitorTick`). Measured after the change: **0.03 s of CPU per minute
idle** (~0.05 % of one core) with 44 windows and 887 records.

### 6.4 Tool windows are not browsing windows (G3, partly)

`EnumFastWindow` now skips `WS_EX_TOOLWINDOW` windows, which is the same rule
that decides picker eligibility. That covers picture-in-picture and similar
browser tool windows. A PWA window is a normal top-level window of the browser
executable and remains indistinguishable through Win32; excluding it would need
a signal Chromium does not expose.

### 6.5 Saying so when the browser is elevated (G14)

On a permanent move failure VDE probes the target process:
`PROCESS_QUERY_LIMITED_INFORMATION` opens across integrity levels but
`OpenProcessToken` does not, so `ERROR_ACCESS_DENIED` from the token open means
the browser runs higher than VDE. The tray then says so once per process instead
of reporting a generic failure.

### 6.6 A bug this pass introduced, and the fix

The first build of §5 wrote URL signatures as unsigned 64-bit values but parsed
them with `ParseI64Strict`, so any signature above 2^63 — half of them — made
the whole layout unreadable. The loader did the right thing (preserved the file
as `layout-auto.txt.corrupt.<stamp>` and started empty) but the effect was a
wiped layout. Fixed by adding `ParseU64Strict` and using it for the `U`
companion and for the binding file's handles and process start times; the
preserved file was restored intact (881 records, byte-identical round trip), and
regression tests now cover a real 2^63+ signature and the top of the range.

### 6.7 Verified live

Restoring the 881-record layout and restarting VDE: the file loads as v5, grows
to 887 records with 39 signatures, all 44 live windows bind, and **no window
moves**.

---

## 7. Selective reopen (2026-09-02)

The first reopen dialog offered a checkpoint and a browser filter, and brought
back everything. In practice that is rarely what is wanted: most of a checkpoint
is still open, and the interesting part is one desktop or one window.

### 7.1 Three cascading columns

`src/reopen_model.hpp` (pure, unit-tested) holds one checkpoint as
desktops → windows → tabs with a check box on every row and an "All" switch per
column, plus the browser filter:

- **Cascade goes down only.** Checking a desktop checks its windows and their
  tabs; checking a window checks its tabs; unchecking clears everything below.
  Unchecking a tab never unchecks its window — a window with 0 of 12 tabs stays
  listed, it just contributes nothing.
- **Visibility follows the checks.** The windows column lists the windows of the
  checked desktops; the tabs column lists the tabs of the checked windows. An
  empty column says what to select instead of looking broken.
- **Already-open tabs are never duplicated by default.** When the dialog opens
  it reads the running browsers' session files (`CollectOpenTabUrls`) and marks
  every checkpoint tab whose URL is open right now; those rows are greyed,
  labelled "open now", and start unchecked. "All" leaves them alone; the user
  can still check one explicitly. After a reopen the launched tabs are marked
  open too.
- **Reopen acts on the checked tabs of checked windows on checked desktops that
  pass the browser filter**, grouped into their original windows
  (`BuildReopenJobsFromSelection`); each window goes to its original desktop
  (falling back to the saved position if that desktop is gone).

### 7.2 The engine, faster and visible

The engine still opens one window at a time: the new window is recognized as
the difference in the browser's window set, and Firefox routes extra tabs to its
most recent window, so launches cannot overlap. (Tested: a single
`-new-window url1 url2` puts url2 into the *previous* window.) What changed:

- polling at 150 ms instead of 400 ms, tab settle 500 ms instead of 900 ms, and
  no idle gap between windows — measured under a second per window;
- the dialog stays open, disables its controls, shows "Opening window i of n:
  title" with a progress bar, and turns Reopen into Cancel (which stops after
  the current window);
- the finish summary lands in the dialog's status line, or in a tray balloon if
  the dialog was closed meanwhile.

### 7.3 Two bugs found on the way

- The SNSS reader required a navigation entry (command 6) to end right after the
  title. Real Chrome entries carry ~1.7 KB more (page state, referrer,
  timestamps), so every current Chrome and Edge session file was rejected as a
  whole. Fixed by reading only the leading fields.
- Edge holds its `Session_*` file with no read sharing while it runs, so its
  session cannot be read at all until Edge exits. Documented as a limitation.

### 7.4 Verified live

Synthetic checkpoint with three windows on two desktops: the four tabs opened
by an earlier reopen showed as "open now" and unchecked; the two fresh tabs were
selected; Reopen brought back exactly that one window with those two tabs on
the right desktop in about a second, the progress bar and Cancel showed, the
summary read "1 of 1 window(s) opened, 1 placed on their desktop", and the two
tabs were then greyed as open.

### 7.5 Second round of the dialog (same day)

- **Resizable, maximizable.** `WS_OVERLAPPEDWINDOW`; `RoLayout` places every
  control from the client size (first column fixed, the other two share the
  rest, last list column fills), with a minimum size via `WM_GETMINMAXINFO`.
- **The selected checkpoint tab is unmistakable.** The tab control is
  owner-drawn (`TCS_OWNERDRAWFIXED`): highlight colour and bold text for the
  current page, item width measured from the longest label.
- **Select-all lives in the list header**, the way file managers do it:
  `HDS_CHECKBOXES` on each header, `HDF_CHECKBOX` on the first column,
  `HDN_ITEMSTATEICONCLICK` flips every visible row of that column, and the
  header state is recomputed after every change. The "All" buttons are gone.
- **A text filter under each column** (`ReopenModel::desktopFilter` /
  `windowFilter` / `tabFilter`, case-insensitive substring over "index name",
  "title browser", "title url"). A filter narrows what a column *shows* — and
  therefore what its header box and the columns to its right act on — but never
  what is selected: `ReopenSelectedWindows` (checks + browser filter only) feeds
  the summary and the jobs, so a checked tab hidden by a filter is still
  reopened and still counted.
- **Reading the open tabs is retried and its failure is visible.** The session
  read is stamp-verified, so it fails whenever the browser is mid-write;
  `CollectOpenTabUrls` retries four times. A browser that still cannot be read
  while it has live windows is listed in the status line ("duplicates cannot
  be detected") and its windows start **unchecked**, so nothing is duplicated
  by default. Seen live for Edge, whose session file is locked while it runs.

### 7.6 Third round (same day)

- **Virtual lists.** The three columns are `LVS_OWNERDATA` lists: a refresh is
  a row-cache rebuild plus `ListView_SetItemCountEx`, and text and check state
  are answered from the cache in `LVN_GETDISPINFO` (with
  `ListView_SetCallbackMask(LVIS_STATEIMAGEMASK)`). Check clicks are taken from
  `NM_CLICK` hit-tested on the state icon and from the space bar
  (`LVN_KEYDOWN`), because the control stores nothing. Every click and every
  filter keystroke used to rebuild up to ~700 rows with three messages each;
  now nothing is inserted at all.
- **Header select-all** needs a clickable header: `LVS_NOSORTHEADER` was in the
  way (no `HDS_BUTTONS`, no state-icon clicks). Removed; the box now checks
  every visible selectable row when at least one is unchecked, and clears them
  all when all are checked.
- **Hide open** per column (`hideOpenDesktops/Windows/Tabs`): a tab is open
  when its URL is shown by the browser now, a window when all its tabs are, a
  desktop when all its listed windows are. View-only, like the text filters;
  fully open windows and desktops are greyed in the lists as well.
- **Desktops that no longer exist.** `BuildReopenModel` receives the current
  desktop list and flags the missing ones ("gone -> desktop 1", greyed);
  `ResolveReopenDestination` now sends such windows to the **first** desktop
  instead of the desktop at the saved position. Verified live with a checkpoint
  pointing at a made-up desktop GUID: the window landed on desktop 1.
