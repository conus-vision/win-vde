# Picker Drag Preview Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show a semi-transparent copy of the dragged window row, including its application icon and ellipsized title, while row Drag & Drop is active inside the popup.

**Architecture:** Keep the existing `PickerPointerGesture` as the action/dispatch state and add a separate presentation-only snapshot for the drag preview. Capture immutable row presentation data on button-down, reveal it only after the system drag threshold is crossed, follow the pointer with the original grab offset, and render it last into the existing picker double buffer through a reusable compatible scratch buffer and constant-alpha `AlphaBlend`. Preview allocation or paint failure must never cancel, redirect, or otherwise affect the actual click/drag operation.

**Tech Stack:** C++14, Win32/GDI, `AlphaBlend` from Msimg32, the existing `GdiBuffer`, the custom `CHECK` harness in `tests/vdtest.cpp`, and MSVC batch builds.

---

**Approved design:** `docs/superpowers/specs/2026-08-22-picker-row-actions-drag-drop-design.md`

**Execution directory:** `F:\_VDESKTOP_FF\win-vde\.worktrees\picker-row-actions`

**Current branch:** `feat/picker-row-actions`

## Non-negotiable invariants

- The preview is presentation state only. `PickerPointerGesture` remains the sole authority for click versus drag, the selected row, and the drop destination.
- The preview becomes visible only in `PickerPointerPhase::Dragging` while the popup owns mouse capture and the captured row snapshot is still current.
- The copied row keeps the initial pointer-to-row offset, so it does not jump when dragging begins.
- Only the selected row is copied. The source row remains unchanged while dragging.
- The destination tile highlight remains visible below the preview.
- The preview is clipped to the popup client and exists only while that popup is open.
- Button-up, Escape, capture loss, cancel mode, hide, session end, destruction, and teardown clear all preview presentation data.
- Any preview capture, scratch-buffer, clipping-region, icon, or `AlphaBlend` failure is cosmetic and must not change the real row action.
- Do not create a child, layered, or top-level helper window and do not use the shell drag-image APIs.

## TDD and commit discipline

Every added test function must also be called from the manual registration block in `main()` before its RED run. The test program has no focused selector, so all RED and GREEN runs use:

```powershell
.\build-test.bat
```

A RED run is accepted only when it fails at the newly added assertion or missing symbol. A GREEN run must exit 0 and end with `N/N passed`. Before each commit, require an empty pre-existing index, stage only the files named by that task, run `git diff --cached --check`, and inspect `git diff --cached --name-only`.

All Git commands use:

```powershell
git -c safe.directory=F:/_VDESKTOP_FF/win-vde/.worktrees/picker-row-actions ...
```

## Task 1: Add pure preview geometry and eligibility rules

**Files:**

- Modify: `src/picker_state.hpp`
- Modify: `tests/vdtest.cpp`

- [ ] Add and register tests beside the existing pointer-gesture tests for raw bounds, initial grab-offset preservation, partial clipping on every edge, fully clipped previews, old/new dirty-region union, and fail-closed paint eligibility.

Representative assertions:

```cpp
static void test_picker_drag_preview_preserves_grab_offset_and_clips(){
    const SIZE size={140,22};
    const POINT grab={35,10};
    const POINT pointer={300,200};
    const RECT bounds=PickerDragPreviewBounds(pointer,size,grab);
    CHECK(EqualRectValue(bounds,RECT{265,190,405,212}));

    const PickerDragPreviewBlit clipped=
        ResolvePickerDragPreviewBlit(bounds,RECT{0,0,400,300});
    CHECK(clipped.visible);
    CHECK(EqualRectValue(clipped.destination,RECT{265,190,400,212}));
    CHECK(clipped.source.x==0);
    CHECK(clipped.source.y==0);

    const PickerDragPreviewBlit leftClipped=
        ResolvePickerDragPreviewBlit(RECT{-25,40,115,62},RECT{0,0,400,300});
    CHECK(leftClipped.visible);
    CHECK(leftClipped.source.x==25);
    CHECK(leftClipped.source.y==0);
}

static void test_picker_drag_preview_eligibility_fails_closed(){
    CHECK(PickerDragPreviewPaintable(
        PickerPointerPhase::Dragging,true,true,8,8,13,13));
    CHECK(!PickerDragPreviewPaintable(
        PickerPointerPhase::Armed,true,true,8,8,13,13));
    CHECK(!PickerDragPreviewPaintable(
        PickerPointerPhase::Dragging,false,true,8,8,13,13));
    CHECK(!PickerDragPreviewPaintable(
        PickerPointerPhase::Dragging,true,false,8,8,13,13));
    CHECK(!PickerDragPreviewPaintable(
        PickerPointerPhase::Dragging,true,true,7,8,13,13));
    CHECK(!PickerDragPreviewPaintable(
        PickerPointerPhase::Dragging,true,true,8,8,12,13));
}
```

- [ ] Run the full suite and confirm RED for the missing preview helpers.
- [ ] Add small, `noexcept`, side-effect-free helpers after `PickerPointerPhase` in `src/picker_state.hpp`:

```cpp
struct PickerDragPreviewBlit {
    RECT destination={0,0,0,0};
    POINT source={0,0};
    bool visible=false;
};

inline bool PickerDragPreviewGeometryValid(SIZE size,POINT grab) noexcept;
inline RECT PickerDragPreviewBounds(POINT pointer,SIZE size,POINT grab) noexcept;
inline PickerDragPreviewBlit ResolvePickerDragPreviewBlit(
    RECT preview,RECT client) noexcept;
inline RECT PickerDragPreviewDirtyBounds(
    bool oldVisible,RECT oldBounds,bool newVisible,RECT newBounds) noexcept;
inline bool PickerDragPreviewPaintable(
    PickerPointerPhase phase,bool captured,bool identityMatches,
    uint64_t previewGeneration,uint64_t modelGeneration,
    uint64_t previewEpoch,uint64_t rowLayoutEpoch) noexcept;
```

- [ ] Make invalid sizes, invalid grab offsets, empty client rectangles, stale generations/epochs, missing identity, and missing mouse capture return no visible preview.
- [ ] Run `build-test.bat`; require GREEN.
- [ ] Commit only `src/picker_state.hpp` and `tests/vdtest.cpp` with message `test: define picker drag preview geometry`.

## Task 2: Capture and clear presentation-only row state

**Files:**

- Modify: `src/vde.cpp`
- Modify: `tests/vdtest.cpp`

- [ ] Add and register a source-contract test proving that the preview snapshot is separate from `PickerPointerGesture`, captures the full title/runtime icon key/identity/row size/grab offset/generation/epoch from `PickerRowHitSnapshot`, and is not used by the action reducer or drop dispatcher.
- [ ] Add failure-oriented source checks proving `CapturePickerDragPreview` is called only after the row action has successfully armed and that its return value is intentionally cosmetic.
- [ ] Run the full suite and confirm RED at the new source-contract checks.
- [ ] Add a presentation-only state near `g_pickerGesture`:

```cpp
struct PickerDragPreviewState {
    bool captured=false;
    std::wstring fullTitle;
    std::string runtimeKey;
    WindowIdentityKey identity;
    SIZE size={0,0};
    POINT grabOffset={0,0};
    POINT pointer={0,0};
    uint64_t modelGeneration=0;
    uint64_t rowLayoutEpoch=0;
};

static PickerDragPreviewState g_pickerDragPreview;
```

- [ ] Implement `CapturePickerDragPreview(const PickerRowHitSnapshot&, POINT) noexcept` transactionally: build strings and geometry in a local candidate, validate them, then swap into the global state. Catch allocation failures, clear the candidate/global preview, and return `false` without changing `g_pickerGesture`.
- [ ] Implement `ClearPickerDragPreview() noexcept`, an identity-current check, and current raw-bounds lookup. Do not consult this state when resolving the real click/drop action.
- [ ] In the row branch of `WM_LBUTTONDOWN`, arm the action from `hit.action`, then best-effort capture presentation data from the full hit snapshot before `SetCapture`.
- [ ] Clear the preview inside `ResetPickerPointerGesture`. Also clear it directly in `EndPickerVisualSessionRuntime` before the existing direct `CancelPickerRowGesture`/`ReleaseCapture` sequence so the established session-end order and source tests remain intact.
- [ ] Verify all existing reset paths continue to cover button-up, Escape, `WM_CAPTURECHANGED`, `WM_CANCELMODE`, transient hide, logical session end, and destruction.
- [ ] Run `build-test.bat`; require GREEN.
- [ ] Commit only `src/vde.cpp` and `tests/vdtest.cpp` with message `feat: capture picker drag preview state`.

## Task 3: Follow the pointer and invalidate only the changed preview area

**Files:**

- Modify: `src/vde.cpp`
- Modify: `tests/vdtest.cpp`

- [ ] Extend and register the mouse-wiring test to require these operations in `WM_MOUSEMOVE`:

  1. Capture old preview visibility/bounds.
  2. Update the preview pointer for every armed/dragging move.
  3. Run the existing `UpdatePickerRowGesture` reducer.
  4. Capture new visibility/bounds.
  5. Invalidate the union of the old and new preview bounds even when the destination tile did not change.

- [ ] Add a regression source check that the preview does not become paintable while the gesture is only `Armed` and that the initial source row is never dimmed or removed.
- [ ] Run the full suite and confirm RED at the new ordering/invalidation assertion.
- [ ] Add a helper that derives visibility with `PickerDragPreviewPaintable`, exact identity equality, current `g_pickerModelGeneration`, current `g_pickerRowLayoutEpoch`, and `GetCapture()==g_pickerOwner`.
- [ ] Update `WM_MOUSEMOVE` to retain the original grab offset, change only `g_pickerDragPreview.pointer`, and invalidate the clipped dirty union. Keep the existing full invalidation when phase or destination tile changes so the tile highlight still updates.
- [ ] Ensure the first transition from `Armed` to `Dragging` invalidates and reveals the preview in the same mouse-move message.
- [ ] Run `build-test.bat`; require GREEN.
- [ ] Commit only `src/vde.cpp` and `tests/vdtest.cpp` with message `feat: track picker drag preview motion`.

## Task 4: Paint icon and title through a reusable alpha buffer

**Files:**

- Modify: `src/vde.cpp`
- Modify: `tests/vdtest.cpp`

- [ ] Add and register paint/resource source-contract tests requiring:

  - `#pragma comment(lib,"msimg32.lib")` and `AlphaBlend`.
  - A separate reusable `GdiBuffer g_pickerDragBuffer`.
  - Constant source alpha `166` (approximately 65%).
  - The same cached application-icon lookup/fallback path used by normal rows.
  - The same picker row font and ellipsized full title.
  - A rounded destination clipping region.
  - Preview paint after tiles, rows, destination highlight, and footer but before the final double-buffer `BitBlt`.
  - No `CreateWindow*`, `UpdateLayeredWindow`, `ImageList_BeginDrag`, or action dispatch in the preview renderer.
  - One owned cleanup/reset path for both picker buffers.

- [ ] Run the full suite and confirm RED at the missing buffer/renderer checks.
- [ ] Add the linker pragma and reusable scratch buffer:

```cpp
#pragma comment(lib,"msimg32.lib")
static GdiBuffer g_pickerDragBuffer;
```

- [ ] Implement `PaintPickerDragPreview(HDC target, RECT client) noexcept`:

  1. Revalidate the presentation snapshot and compute the raw bounds.
  2. Use `ResolvePickerDragPreviewBlit` for popup-client clipping and matching source offset.
  3. `ensure` the scratch buffer at the full source-row size.
  4. Paint a dark rounded row background/border into the scratch DC.
  5. Draw the cached application icon (or the existing fallback) at the normal row icon size.
  6. Select the normal picker row font and draw the full title with the same vertical alignment and `DT_END_ELLIPSIS` behavior as the source row.
  7. Intersect the target DC with a rounded region based on the raw preview bounds.
  8. Blend the visible source subsection using:

```cpp
BLENDFUNCTION blend={AC_SRC_OVER,0,166,0};
AlphaBlend(target,
    clipped.destination.left,clipped.destination.top,
    clipped.destination.right-clipped.destination.left,
    clipped.destination.bottom-clipped.destination.top,
    scratch,clipped.source.x,clipped.source.y,
    clipped.destination.right-clipped.destination.left,
    clipped.destination.bottom-clipped.destination.top,
    blend);
```

- [ ] Treat every GDI setup/blend failure as `false` from the paint helper and let the normal popup paint complete unchanged.
- [ ] Call the preview renderer last inside `Paint`, immediately before the final `BitBlt` from the picker back buffer to the real DC.
- [ ] Reset `g_pickerDragBuffer` in `CleanupUiResources` alongside `g_pickerBuffer`, assert both report `released()`, then clean up the icon cache/fonts/brushes in the existing order.
- [ ] Run `build-test.bat`; require GREEN.
- [ ] Run `build.bat`; require exit 0 and `Built build\vde.exe`. If a running feature executable locks the output, inspect and stop only the process whose resolved executable path exactly equals this worktree’s `build\vde.exe`, then rerun once.
- [ ] Commit only `src/vde.cpp` and `tests/vdtest.cpp` with message `feat: paint translucent picker drag preview`.

## Task 5: Document and verify the complete interaction

**Files:**

- Modify: `README.md`
- Modify: `src/vde.cpp`
- Modify: `tests/vdtest.cpp` only if documentation/help source contracts already require synchronization

- [ ] Update the README interaction section: during row Drag & Drop, a translucent copy containing the application icon and window title follows the pointer; dropping relocates/assigns the row without switching desktops or closing the popup.
- [ ] Update `HELP_TEXT` with the same concise interaction description while preserving the existing distinction between row click, desktop-title click, Ctrl+Click, physical move, and visual-only assignment.
- [ ] Run `build-test.bat`; require GREEN and record the exact final `N/N passed` count.
- [ ] Run `build.bat`; require exit 0 and `Built build\vde.exe`.
- [ ] Run `git diff --check` and inspect the complete branch diff against `origin/feat/picker-row-actions`.
- [ ] Perform manual QA with at least one Firefox window and one non-Firefox window:

  - Press and release a row without crossing the drag threshold: no preview; exact window activates.
  - Drag after crossing the threshold: icon and ellipsized title appear semi-transparent with no initial jump.
  - Move across several desktop tiles: destination highlight changes below the preview.
  - Move partly beyond each popup edge: preview clips cleanly and remains stable.
  - Release on another desktop: window assignment/move occurs, desktop does not switch, popup remains open, preview disappears.
  - Press Escape or force capture loss during drag: no move occurs and preview disappears.
  - Drag a globally visible/pinned window: only the chosen row’s visual assignment changes for the popup session; the real global window is not moved.
  - Close/reopen the popup: no stale preview remains.

- [ ] Commit documentation/help changes only with message `docs: describe picker drag preview`.
- [ ] Confirm the worktree is clean and the branch contains only the intended commits. Do not push unless the user explicitly asks for this new feature branch update.

## Final acceptance criteria

- The drag copy is visible only after the system drag threshold is crossed.
- It contains the same application icon and window title presentation as the selected source row.
- It is semi-transparent, rounded, follows the pointer with the original grab offset, and clips to the popup.
- Destination highlighting remains visible and the source row does not change.
- Click activation, physical row move, visual-only assignment, popup lifetime, and browser-only persistence behavior are unchanged.
- Cosmetic preview failures cannot change the operation’s semantic result.
- All automated tests and the Release build pass, and the manual QA matrix has been exercised before completion is claimed.
