# Evidence-driven picker fixes

## Evidence

The one-shot trace `picker-20260821T211953.461Z-58528.jsonl` has SHA-256
`6AE7FC2E378DC647BBD82C2640DA1284FF82B30F532C18C06328434C087002FB`.

- The second picker enumeration observed 42 verified Firefox windows. Eight
  eligible non-Firefox windows (Code, Edge, After Effects, Total Commander,
  and Explorer) returned `S_OK` and the same nonzero desktop GUID, but that
  GUID was absent from the nine-desktop snapshot. They were rejected as
  `skip_desktop_tile_missing` before title/identity fallback could run.
- The Ctrl+Click at sequence 1242 was recognized with `ctrl=true` and tile 8.
  Activation dispatch succeeded, but sequence 1246 recorded
  `GetWindowDesktopId(g_main) == 0x8002802B`; sequence 1269 stopped at
  `popup_desktop_unavailable`. No transition or move effect was published.

## Safety boundary

- Do not guess that every unknown desktop GUID belongs to the current desktop.
- For an otherwise eligible window whose valid GUID has no tile, call the
  documented `IsWindowOnCurrentVirtualDesktop`. Admit it on the current tile
  only for a successful `TRUE` result, and admit it as `DisplayOnly` so the
  substituted desktop never becomes a stable move/cache/persistence identity.
- Keep failed/off-current membership checks fail-closed.
- Do not weaken the popup desktop guard. If the popup desktop is unavailable
  while the current desktop is valid, explicitly assign this process-owned
  popup to the current desktop, read it back, and proceed only after a valid
  nonzero result.
- Do not change primary-monitor positioning, general telemetry, autostart, or
  layout persistence behavior.

## Task 1: Add failing decision and wiring tests

Modify `src/picker_state.hpp`, `src/picker_trace.hpp`, and `tests/vdtest.cpp`.

1. Add RED tests for an exact-tile/current-tile route decision:
   exact match remains actionable; missing tile plus successful current
   membership becomes display-only current; failed/FALSE membership and a
   missing current tile stay skipped.
2. Add RED tests for popup association repair: a valid observed popup is used;
   an unavailable/zero popup with a valid current desktop requests one repair;
   a missing current desktop rejects.
3. Add the typed enum decision
   `DisplayOnlyCurrentDesktopFallback` and its exact JSON name
   `display_only_current_desktop_fallback`.
4. Add narrow source-wiring assertions for the membership call, forced
   display-only admission, popup assignment, second readback, and guard order.
5. Run `build-test.bat` and retain the expected RED evidence before product
   wiring.

## Task 2: Admit confirmed current windows safely

Modify `src/vde.cpp`, `src/picker_state.hpp`, `src/picker_trace.hpp`, and
`src/picker_trace.cpp`.

1. Carry the observed current desktop into `PickerEnumContext`.
2. Preserve exact GUID-to-tile matching as the first route.
3. Only after a valid nonzero GUID misses every tile, call
   `IsWindowOnCurrentVirtualDesktop`.
4. If and only if the call succeeds with `TRUE` and the current desktop has a
   tile, route the row there and force `PickerRowAdmission::DisplayOnly`.
5. Preserve title and identity race checks; an identity proven lost still
   skips the row.
6. Emit `DisplayOnlyCurrentDesktopFallback` for an admitted fallback row.
7. Run the focused and full test suite and commit independently.

## Task 3: Repair the popup desktop association before moving

Modify `src/vde.cpp` and `tests/vdtest.cpp`.

1. Keep the initial popup desktop read and trace event.
2. When the pure decision requests repair, call the documented
   `MoveWindowToDesktop(g_main,currentOrigin)` once and trace its HRESULT.
3. Re-read and trace the popup desktop after the assignment.
4. Continue only when the existing validity/nonzero guard succeeds; otherwise
   retain `PopupDesktopUnavailable`.
5. Run focused/full tests and commit independently.

## Task 4: Verify and review

1. Confirm no development `vde.exe` remains.
2. Run `build-test.bat`, `build.bat`, `git diff --check`, and inspect status.
3. Request independent review of the decision boundary, Win32 call ordering,
   fail-closed behavior, privacy, and regression coverage.

## Task 5: Manual confirmation

1. Record the final executable SHA-256 and launch it with `--trace-picker`.
2. Keep a non-Firefox window open, open the picker once, and verify it appears.
3. Perform one Ctrl+Click to another desktop, wait, and exit from the tray.
4. Confirm the trace contains the display-only fallback, an accepted move,
   move/effect API events, and a terminal event. Do not claim completion before
   the live behavior and terminal route agree.
