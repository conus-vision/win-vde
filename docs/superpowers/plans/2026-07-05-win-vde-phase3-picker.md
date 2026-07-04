# win-vde Phase 3 (Picker UX) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. All UI/GDI — verified by building + manual visual check (no unit tests).

**Goal:** Add a search box, per-tile scrolling, truncated-name tooltips, and a highlighted Ctrl+Click hint to the desktop picker (spec R7–R10).

**Architecture:** The picker is one owner-drawn popup (`g_main`) painted in `Paint()` over an off-screen bitmap. Add a child `EDIT` for search (subclassed to forward navigation keys to the grid), give each `Tile` a `scroll` offset consumed by the wheel, record drawn window rows during `Paint` so `WM_MOUSEMOVE` can drive a tracking tooltip, and render the Ctrl+Click hint as an accent rounded-rect badge.

**Tech Stack:** Win32/GDI, common controls v6 (EDIT cue banner, TOOLTIPS), MSVC.

## Global Constraints
- Same build/toolchain as Phases 1–2. UI strings English.
- Search placeholder text (verbatim): `Type window name to search`.

---

## Task 1: Search box

**Files:** modify `src/vde.cpp`.

- [ ] Add globals: `HWND g_search`, `WNDPROC g_searchOrigProc`, `std::wstring g_searchText`, `int SEARCH_H`. Add `int scroll=0;` to `struct Tile`.
- [ ] `InitMetrics`: `SEARCH_H=S(40);`
- [ ] `DesiredClientSize`/`LayoutTiles`: tiles start at `SEARCH_H+HEADER+PAD` (add `SEARCH_H` to `cy` and to each tile's `rc.top`).
- [ ] Add helpers `LowerW` and `bool MatchesSearch(const std::wstring&)` (empty query matches all; else case-insensitive substring on lowercased title; `g_searchText` stored lowercased).
- [ ] `EditProc` subclass: forward `VK_ESCAPE/RETURN/UP/DOWN/LEFT/RIGHT/TAB` to `g_main` via `SendMessage(WM_KEYDOWN)` and swallow; on `VK_CONTROL` down/up invalidate `g_main`; swallow `WM_CHAR` for Enter/Esc/Tab (no beep); else `CallWindowProc`.
- [ ] `ShowPicker`: lazily create the `EDIT` (child of `g_main`, `ES_AUTOHSCROLL|WS_BORDER`), subclass it, set cue banner `Type window name to search`; on show clear its text + `g_searchText`, reset every tile's `scroll=0`, position it across the top, `ShowWindow`, and `SetFocus(g_search)` after `SetForegroundWindow(g_main)`.
- [ ] `WndProc` `WM_COMMAND`: on `HIWORD(wp)==EN_CHANGE && (HWND)lp==g_search`, read text → lowercased `g_searchText`, `InvalidateRect`.
- [ ] `Paint`: filter each tile's windows through `MatchesSearch`; when a query is active, dim tiles with zero matches. Move the header baseline down by `SEARCH_H`.
- [ ] Build; **manual:** hotkey → search box focused with placeholder; typing filters window names live; arrows/Enter still drive the grid.

## Task 2: Per-tile scroll

- [ ] `Paint`: drop the `maxShow=5` cap; draw filtered windows from index `tile.scroll` downward until the tile bottom; draw a `▲`/`▼` indicator when scrolled / more remain. Record each drawn row into `std::vector<RowRec> g_rows` (`{RECT rc; std::wstring full; bool trunc;}`), clearing it at the top of `Paint`.
- [ ] `WndProc` `WM_MOUSEWHEEL`: screen→client point, find the tile under it, `scroll += (delta<0?1:-1)`, clamp to `[0, max(0, matches-visibleRows)]`, `InvalidateRect`.
- [ ] Build; **manual:** a desktop with many windows scrolls inside its tile with the wheel.

## Task 3: Truncated-name tooltip

- [ ] Create a `TOOLTIPS_CLASS` tracking tooltip (`g_tip`, `TTF_TRACK|TTF_ABSOLUTE`) owned by `g_main` (lazily, in `ShowPicker`).
- [ ] In `Paint`, set each `RowRec.trunc` = (full text width via `GetTextExtentPoint32W` > available row width).
- [ ] `WndProc` `WM_MOUSEMOVE`: hit-test `g_rows`; if over a `trunc` row, `TTM_TRACKPOSITION` (cursor + offset), `TTM_UPDATETIPTEXT` full title, `TTM_TRACKACTIVATE TRUE`; else deactivate. Use `TrackMouseEvent` (`TME_LEAVE`) and deactivate on `WM_MOUSELEAVE`.
- [ ] Build; **manual:** hovering a clipped window name shows its full title.

## Task 4: Ctrl+Click badge

- [ ] `Paint`: replace the dim `"Click: switch  Ctrl+Click: move window"` line with `"Click = switch"` (dim) + a rounded-rect **badge** (`RoundRect`) reading `Ctrl+Click = move window`, accent fill `RGB(0,120,215)` when Ctrl held else `RGB(0,90,158)`, white text; width from `GetTextExtentPoint32W`. Place it at the header's right.
- [ ] Build; **manual:** badge is clearly visible and brightens while Ctrl is held.

## Self-Review
- R7 search → Task 1; R8 scroll → Task 2; R9 tooltip → Task 3; R10 badge → Task 4.
- No unit tests (pure GDI); each task ends in a build + a specific manual check.
