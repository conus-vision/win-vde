# win-vde — Virtual Desktop Extension for Windows 11

A small tray utility that remembers which **virtual desktop** each browser
window belongs to and puts them back automatically after a reboot or a browser
restart. It also adds a fast searchable picker for moving the active
window between desktops, activating an exact listed window, and dragging window
rows between desktop tiles.

- **Author:** Volodymyr Moskvin — <info@conus.vision>
- **Repository:** https://github.com/conus-vision/win-vde
- **License:** MIT

**➡ [Download the latest `vde.exe`](https://github.com/conus-vision/win-vde/releases/latest)** — a single file, no installer. Or build it from source (see [Quick start](#quick-start)).

> Automatic layout memory supports **Firefox, Chrome, and Edge** today. The
> picker also lists eligible ordinary windows from other applications, with app
> icons, active-window highlighting, search, scrolling, and full-name tooltips.

<p align="center"><img src="docs/overview.svg" alt="How win-vde works: Windows scatters browser windows across virtual desktops after a reboot; win-vde remembers the layout and restores it; plus a fast searchable desktop picker" width="840"></p>

## Quick start

1. **Get `vde.exe`.** Download it from the [Releases](https://github.com/conus-vision/win-vde/releases/latest) page, or build it with `build.bat` (see [Install / Build](#install--build)) — it lands at `build\vde.exe`. Either way it is a single, self-contained executable: no installer, no admin rights, no dependencies.
2. **Put it somewhere permanent.** Move `vde.exe` into a folder you intend to keep (for example `C:\Users\<you>\Apps\win-vde\`). Autostart remembers the exe's *current* path, so choose the folder **before** enabling it — if you move the exe later, just toggle autostart off and on again.
3. **Run it.** Double-click `vde.exe`; a tray icon appears next to the clock and it starts watching your browsers right away.
4. **Turn on autostart.** Right-click the tray icon → **Settings…** → tick **Start with Windows (run at logon)** → **OK**. Your layout will now be restored automatically after every reboot.

That's all — arrange your browser windows across your virtual desktops and forget about it. Press **Ctrl+Alt+D** whenever you want the desktop picker.

## Why this exists

Three things Windows and the browser don't solve on their own:

1. Windows 11 **doesn't remember** which virtual desktop a third-party app's
   window was on across a reboot — everything lands on desktop 1.
2. On session restore the browser recreates its windows, but their **window
   handles (HWND) and PIDs change**, so nothing external can recognize them by
   handle.
3. There is **no public Windows API** for moving another process's window
   between virtual desktops. It requires undocumented COM interfaces.

win-vde bridges that gap from the window's *content*: it remembers the pages a
window holds, matches the old windows to the new ones after a restart, and moves
each of them to the desktop it was saved on. While it runs, a window needs no
fingerprint at all — it is identified exactly by its handle, process ID and
process start time, and those identities are kept on disk, so restarting win-vde
itself is never mistaken for a browser restart and moves nothing.

## Features

- **Multi-browser** — Firefox (via its session store), Chrome and Edge (via
  their SNSS session files); each can be toggled in Settings. After a restart a
  window is re-identified by its pages: an exact match of the whole tab-URL set
  first, then tab domains, then the window title. Windows holding exactly the
  same pages are treated as interchangeable, and among equally good placements
  win-vde picks the one that moves the fewest windows.
- **Automatic restore, only when it is warranted** — windows are put back when
  their identity is actually gone, i.e. after a reboot, a browser restart, or a
  browser crash (with a ~20 s stabilization first). Restarting win-vde itself
  moves nothing: surviving windows are recognized by handle, process ID and
  process start time and simply re-adopted. A window you open while the browser
  is already running is never relocated — it is recorded where you opened it.
- **30-day closed-window memory** — a closed Firefox, Chrome, or Edge window
  keeps its remembered virtual desktop for 30 days. If it reappears before
  expiry, VDE restores it before updating the saved layout.
- **Two layouts** — a rolling *auto* layout plus a manual checkpoint you save on
  demand.
- **Reopen exactly the tabs you pick** — VDE also remembers *what* each window
  contained. The reopen window shows five checkpoints as tabs (the last saved
  state plus the last four shutdowns, each with its date and time) and three
  cascading columns: **desktops → browser windows → browser tabs**, each with
  check boxes, a select-all box in the column header, a **Hide open** switch,
  and a text filter underneath, plus a browser filter. Checking a desktop
  selects its windows and their tabs; uncheck whatever you do not want; type to
  narrow a column or hide what is already open (both only change what is
  shown, never what is selected). Tabs that are open in the browser right now
  are greyed out and left unchecked, so a reopen never duplicates them; a window
  whose every tab is open, or a desktop whose every window is, is greyed too.
  If a browser's open tabs cannot be read, its windows start unchecked and the
  status line says so. A desktop that no longer exists is marked; its windows
  reopen on desktop 1. The window is resizable and maximizes, and the lists are
  virtual, so a checkpoint with hundreds of tabs stays instant. The selected
  tabs come back grouped into their original windows, each on the desktop it
  was on, with a progress bar and a Cancel button while it runs.
- **All-window desktop picker** — the global hotkey (default `Ctrl+Alt+D`)
  opens a grid of desktops on the primary monitor, containing eligible ordinary
  application windows, not only tracked browsers. Rows show application icons,
  the exact active window, a full-row hover highlight, and tooltips for clipped
  names.
- **Exact click behavior** — click a window row to switch to its displayed
  desktop and activate that exact window. Click a desktop title or empty tile
  area to switch desktops without requesting activation of a listed window.
- **Ctrl+Click and row Drag & Drop** — stationary `Ctrl`+click moves the
  captured active window, follows it to the destination, and keeps the picker
  open. Drag an exact row to another desktop to move that window without
  switching desktops or closing the picker.
- **Pinned/global-window safety** — windows shown on every desktop, individually
  pinned views, and application-wide pins are never physically moved. Only the
  selected row is visually assigned to the destination for the current popup
  session; Windows pin state and saved layouts remain unchanged.
- **Searchable window rows** — type to filter by window title. Browser windows
  additionally match **any tab**, not just the active one, by tab title or full
  **URL** (address bar). Scroll individual desktop tiles with the mouse wheel.
- **Start with Windows** — optional run-at-logon toggle.
- **Honest about breakage** — if a Windows update changes the undocumented
  interfaces, the app explains what happened and runs in a limited mode instead
  of failing silently.

### Window memory and desktop picker

- Automatic window memory covers every Firefox, Google Chrome, and Microsoft
  Edge top-level browser window, and no other application.
- The picker displays eligible ordinary top-level windows from other
  applications as well. This does not broaden automatic save/restore beyond
  Firefox, Chrome, and Edge.
- A closed browser window remains remembered for exactly 30 days. Reopening it
  before expiry restores its remembered virtual desktop before the rolling
  layout is updated; records expire at the 30-day boundary.
- Hovering a window row highlights its complete clickable area, including the
  icon; the active-window highlight remains visually stronger.
- A plain click on a window row switches to the row's displayed desktop, closes
  the picker, and attempts to activate that exact window. A click on a desktop
  title or empty tile area only switches desktops and does not explicitly
  activate any listed window.
- Stationary Ctrl+Click anywhere in a desktop tile moves the captured active
  window, switches to the destination, and keeps the picker open with its active
  context highlighted.
- Dragging a row to another desktop moves that exact window while leaving the
  current desktop unchanged and the picker open. Verified Firefox, Chrome, and
  Edge moves update their supported saved assignment; moves for other
  applications affect the current live window but create no restore record.
  During the drag, a translucent copy with the application icon and window title follows the pointer.
  Drop it on another desktop to move or visually assign that window without switching desktops or closing the picker.
- A globally visible, view-pinned, or application-pinned window is never sent
  through a physical move. The picker instead shows only the selected row under
  the destination tile until the popup session ends, then reconstructs the next
  popup from actual Windows state.
- The footer links to [Virtual Desktop Extension](https://github.com/conus-vision/win-vde)
  and [Conus Vision](https://conus.vision).
- Layout v5 is migrated automatically from v2/v3/v4. The legacy
  `%LOCALAPPDATA%\VirtualDesktopsExtention` directory and matching registry key
  keep their historical spelling for compatibility.

## Install / Build

Requires **Visual Studio 2022 or 2017** with the x64 build tools. From a normal
shell in the repo root:

```
build.bat
```

This compiles `src/vde.cpp` (+ `src/vde.rc`) into `build\vde.exe`. There are no
third-party dependencies. To run the unit tests for the pure logic:

```
build-test.bat
```

## Usage

Run `vde.exe` with no arguments to start the tray resident. Right-click the tray
icon for:

| Menu item | Action |
|---|---|
| **Open desktop picker** | Grid of desktops (also via the global hotkey) |
| **Save windows layout** | Save current windows to a manual checkpoint file |
| **Restore saved windows layout** | Restore from that manual checkpoint |
| **Restore last auto saved layout** | Restore from the rolling auto layout |
| **Reopen browser windows…** | Pick desktops, windows and tabs from a saved checkpoint and bring them back |
| **Settings…** | Hotkey, auto-save/restore, start-with-Windows |
| **About…** | Version, author, contact, project link |
| **Exit** | Quit after saving the current automatic layout |

**Picker:** click a window row to switch to its displayed desktop and activate
that exact window. Click a desktop title or empty tile area to switch without
activating a listed window. `Ctrl`+click a tile to move the captured active
window there and keep the picker open. Drag a row to move that exact window
without switching desktops or closing the picker. Arrow keys / number keys
navigate; `Esc` closes.

**Command line:**

```
vde.exe list          list virtual desktops
vde.exe status        desktops + live browser windows and their fingerprints
vde.exe save          save current layout to layout-manual.txt
vde.exe restore       restore from layout-manual.txt
vde.exe restore-auto  restore from the last auto-saved layout
vde.exe checkpoints   list the saved browser-session checkpoints
```

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

## Data files

Stored under `%LOCALAPPDATA%\VirtualDesktopsExtention\`:

- `layout-auto.txt` — the rolling auto layout with 30-day closed-window
  retention.
- `layout-manual.txt` — your manual checkpoint (full snapshot).
- `sessions\session-saved.txt` — the last saved browser session (windows,
  their desktops, and every tab URL/title).
- `sessions\session-exit-1..4.txt` — the same for the last four shutdowns,
  newest first.
- `bindings.txt` — which live window (handle, process ID, process start time)
  currently owns which layout record. It is what lets win-vde tell a browser
  restart from its own restart; a stale or missing file only costs one extra
  restore pass.

A legacy `layout.txt` from earlier builds is migrated to `layout-auto.txt` on
first run.

## Autostart

Enable **Settings → Start with Windows (run at logon)** to have the utility
launch at sign-in (an `HKCU\…\Run` entry). This is what makes after-reboot
restore work unattended.

## Limitations & compatibility

- Moving other apps' windows between desktops uses **undocumented COM
  interfaces** whose identifiers can change between Windows 11 builds. If a
  build isn't recognized, win-vde shows a **compatibility notice** and runs in a
  limited (read-only) mode — it won't move windows blindly. Please report your
  Windows build to <info@conus.vision> so a fix can be published.
- Only the **virtual desktop** of each window is saved/restored — on-screen size
  and position are left to the browser's own session restore.
- Persistent automatic/manual window memory remains limited to Firefox, Chrome,
  and Edge. The picker can display, activate, and move eligible windows from
  other applications, but it does not create restore records for them.
- Globally visible or pinned windows are protected from physical moves. Their
  session-only visual placement in the picker disappears when the popup closes.
- If a saved virtual desktop has been deleted, the window is restored to the
  desktop that now occupies that position rather than staying put forever.
- If the Windows shell (`explorer.exe`) restarts, win-vde reconnects to the
  virtual-desktop services by itself instead of silently failing every move.
- Dragging a tab out of a window, or merging two windows, is recognized as such:
  the layout follows the windows instead of dragging one half to the desktop the
  original was remembered on.
- If a browser runs as administrator, win-vde says so once instead of reporting a
  generic failure — an unelevated app cannot move an elevated window.
- Private Firefox windows aren't in the session store, so they're matched by
  title only, and they are not part of a session checkpoint.
- Reopening **adds** windows; it never closes or rearranges the ones already
  open. Tabs that are already open are skipped by default (you can still check
  one explicitly). Tab history, pinned tabs, tab groups, form data and scroll
  position are not reopened — only the tab URLs, their order, and the window's
  desktop.
- Reopening is sequential by design — about a second per window — because a new
  window is recognized as the difference in the browser's window set, and
  Firefox routes extra tabs to its most recent window.
- Edge keeps its session file exclusively locked while it runs, so Edge windows
  are tracked by title only until Edge is closed.
- Every browser profile that is currently open is read, not just the default
  one: win-vde detects an open profile from the lock the browser holds on it.
  A profile that is merely installed is ignored, so its old windows can never
  confuse the matching.

## Roadmap

- Persistent restore profiles for generic (user-defined) multi-window apps
  beyond the three built-in browsers.
- Optional restore of on-screen geometry (size/position), not just the desktop.
- Telling a browser's PWA / app windows apart from ordinary browsing windows
  (Windows exposes no signal for it today).

## License

MIT — see [LICENSE](LICENSE). © 2026 Volodymyr Moskvin (conus.vision).
