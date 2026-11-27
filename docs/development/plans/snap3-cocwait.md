# snap3-cocwait: steps after a coc wait for its world-ready

Branch `snap3/cocwait` (worktree `org-port-worktrees/snap3-cocwait`), based on org_integration 0953f39d7. Stage `s7-cocwait`.
Test runner and cs-test only (`src/Testing`, `tools/cs-test`), plus the contract in `docs/development/testing.md`.

## The problem

Since snap3-jobhang a coc runs at MainTick, shows a real load, and the serving thread presents through it. Loading-screen
Presents count on the step clock, so `cadence-drop-cells`' `frames = 400` from each coc (`at = "previous"`) landed the Riverwood
shot 36 game frames after its world-ready, in the fade-in: mean luma 0.0 in every run since `1126-163118-...s7-jobhang-dropcells-2`.

The configs counted from the coc because "a coc does not record a new world-ready". That was only half true. A coc into another
cell shows the loading menu and did record `load-start` and `world-ready` again. A coc nearby, as drop-cells' first
`coc Whiterun` from the quick save, shows no loading menu (DCLF: "load hold: started (no loading menu: the presents held)"),
so the runner never saw a load and a step at `world-ready` would have waited for ever.

## The fix

- `Lifecycle.cpp`: a teleport is a load. `UpdateLifecycle` already reads the player each Present; it now also keeps the
  player's position, and a move of more than 1024 units between two Presents (the snapshot's own teleport distance) begins a
  load: `load-start`, the world no longer ready, and `world-ready` again once it settles (the same 30 ready game frames, no
  hold). Loads with the loading menu are recorded as before. `loading-screen` stays "the loading menu showed". `held-frame`
  joined the milestones that make the load timeout apply.
- `Steps.cpp`: a step waiting for its milestone while the phase is `world` (world ready, no load under way) fails after
  `wait_s` (else `timeouts.step_wait`, 60 s): `step N: waited 60 s for world-ready after the previous step, and no load started`.
  Through boot, compiling, a load or a menu the timer is reset: the phase timeouts own those waits. Before, such a step waited
  until the harness's run timeout. Run 1 (below) is this message, from the first try.
- No new anchor: `at = "<milestone>"` already means "its first occurrence at or after the previous step". With every load
  recorded, `at = "world-ready"` after a coc is "the next world-ready after the previous step".

### Signals tried

- The load epoch (`Snapshot::LoadEpoch`, from `SetLoadingScreenActive(true)`): the menu-less coc never calls it with true. The
  load coordinator calls only `SetLoadingScreenActive(false)` and posts the loading menu's close (DCLF's hold begins there), so
  the epoch stayed at 2 (run 1 failed with the new timeout message, as it should).
- A count of `SetLoadingScreenActive(false)` would be exact, but it needs a counter in `Snapshot/Publication` (outside this
  item's boundary), so it was not done. Snapshot events (`FirstAfterLoad`, `Teleport`) are render-owner data and would miss
  frames whose presents DCLF holds. The position jump needs nothing outside `src/Testing`.
- Limits: a coc to where the player already stands moves nothing and is not seen (the step then fails after 60 s with the
  reason above); a `player.setpos` of more than 1024 units counts as a load (harmless: the configs that do it count from
  `previous`).

## Configs

Every step right after a coc or a `load` step that counted from it (`at = "previous"`) now anchors at `world-ready`. 39
configs. The new frame counts: a shot 120 (300 where the old count was 900 or more, as those configs gave their scene more
time: tours, cloud and volumetric shadows, skylighting reset, terrain shadows, fg, sky), a console or setting step 60, a
`vram-json` 300, perf marks 600 (perf-dragonsreach) and 2400 (rx6600-perf-whiterun). Comments that described the old counts
are updated, and the fg configs' "plain D3D11 records no world-ready after a coc" line is gone. `cadence-still` already used
`world-ready` after its coc.

`quick` now starts with `tgm` (world-ready, 0 frames), then the shot 60 frames later. Its purpose is the fast smoke shot of the
autoloaded save, a guard fight; god mode keeps the player alive, so a death and reload can no longer put the shot on a loading
screen (s7-shotref-guest-quick). It changes `guest+quick`, `native-vulkan+quick` and `native-d3d12-quick`.

## Runs

Stage `s7-cocwait` (Dev-Fast). cs-test runs the main checkout's configs, so each run passed this branch's steps with
`--set steps=[...]` generated from the branch's config file (the same steps).

| Config | Label | Result |
|---|---|---|
| native-vulkan+cadence-drop-cells | s7-cocwait-dropcells-1 | failed, rightly (first try, load epoch as the signal): `step 2: waited 60 s for world-ready after the previous step, and no load started` |
| native-vulkan+cadence-drop-cells | s7-cocwait-dropcells-2 | passed; shots lit: whiterun 74.2, dragonsreach 78.2, riverwood 63.5 mean luma (riverwood was 0.0). The menu-less `coc Whiterun` was seen as a teleport 3 frames after the step; Dragonsreach and Riverwood loads took 28 s and 26 s (busy machine), and each shot still came 120 frames after its world-ready |
| guest+quick | s7-cocwait-guest-quick | passed; reference check 0 of 144 tiles wrong (lightness 124.8 vs 124.2) |
| native-vulkan+quick | s7-cocwait-nvk-quick | passed; 1 tile wrong (126.0 vs 124.8) |
| native-d3d12-quick | s7-cocwait-d3d12-quick | passed; 0 tiles wrong (126.0 vs 124.2) |
| native-vulkan+native-interior | s7-cocwait-interior | passed; 4 shots, 0-19 tiles wrong |
| guest+imagespace-transitions | s7-cocwait-guest-ist | passed; three cocs, each shot 120 frames after its world-ready, but see open items |
| guest+imagespace-transitions | s7-cocwait-guest-ist2 | passed (with the teleport log line); the first coc moved the player 3705 units with no loading menu |

The `quick` references need no re-accepting: with `tgm` all three runs match them.

Detector numbers: not changed (test runner only). Live engine reads per world frame from the runs: 755 (drop-cells), 745
(native-vulkan quick), 735 (D3D12 quick).

## Decided by the agent, to confirm

- A teleport (a position jump of more than 1024 units) stands for a load with no loading menu. A load-end counter in
  `Snapshot/Publication` (`SetLoadingScreenActive(false)`) would be exact; the director may prefer it. Works in every mode as it
  reads only the player; not run in plain D3D11 here.
- The anchor wait's timeout reuses `wait_s` / `timeouts.step_wait` rather than a new key.
- Settle counts: 120 for a shot, 300 where the old scene got 900 or more frames; perf marks keep most of their settle.

## Open items

- In s7-cocwait-guest-ist, 46 frames after the last coc's world-ready (Dragonsreach back to Whiterun) a second jump of more
  than 1024 units was seen and world-ready came again 29 frames later. The rerun did not show it. The `[TEST] teleport` line
  now logs each jump's positions, so the next one says what moved (a late placement after the coc is the likely cause).
- The other changed configs were not run (39 configs); their steps only move from `previous` to `world-ready`.
