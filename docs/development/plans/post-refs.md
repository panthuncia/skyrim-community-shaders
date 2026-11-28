# post-refs: shot references for the suite configs

Lead `post-refs`, branch `post/refs` (base `org_integration` 77010b47d), stage `post-refs`. Problem: decision 234. Resize runs
rendered black materials while window-resize-stress-taa passed, because only 19 config combinations had shot references; every
other config's shots were taken and never compared. This round makes the suite's configs deterministic, compares native Vulkan with
D3D11 (`d3d11+<config>`, the ground truth), and accepts native's shots as references where they match.

RTX 5080, Dev-Fast build of 77010b47d (no C++ change). Runs used the worktree's `cs_test.py` (the same harness as the main
checkout's; the main checkout's `!CSTestLab.dll` copied into the worktree's untracked build folder) so the edited configs and
`refs.json` were the ones in use. Comparison: the harness's check (`cstest/shotref.py`) plus the cell correlation and the share of
cells whose lightness moved by more than 25, and a look at every pair (scratchpad `cmp.py`, `analyze.py`).

## 1. What made the shots differ between runs

The autoloaded save (Save95) is a guard fight next to Honningbrew Meadery at the hour and weather the save holds. A run's shots
depended on when world-ready fired relative to the fight:

- **`tai` does not stop the fight.** With AI toggled off the guard kept attacking (shots 2 s apart show him moving and swinging).
  `tcai` (combat AI off) stops him: he stands where he is. Every pinned config now runs `tgm`, `tai`, `tcai`.
- **The fight moves and turns the player** before AI goes off, so the camera faced a different way each run (cell correlation
  0.2-0.6 between two runs of one build). The pinned configs put the player back where the save has it (`player.setpos` 32216,
  -16692, -4446, read from Save95's global data table, "Player Location") and face one way (`player.setangle x 0`, `z 200`).
- **The Health Low tutorial** can open before god mode. It pauses the game, so the steps after it did not move the player
  (1127-182816, save-screenshot). The prelude restores health and closes `Tutorial Menu` before moving the player.
- **Time and weather:** `set timescale to 0`, `set gamehour to 13`, and the `weather` step (the CS editor's lock, which holds
  against the climate; `fw` depends on frame timing). Configs whose purpose is a weather or an hour keep it: rain keeps its storm
  (locked with `weather 000C8220` before the coc; game time still runs, since precipitation needs it), cloud-shadows keeps the same
  storm, sky-native keeps its own hours and weathers, screen-blood keeps its fight (only time and weather pinned).
- **A death reloads the save.** sky-native had no `tgm`: in 1127-143741 the player died in Riverwood and the "sky-night" shot is the
  autoload scene at noon. That run passed.
- **Eye adaptation:** first-person-switch's two "look down" shots came 60 frames after the turn, mid-adaptation; now 300.

With all of this, two native runs of a pinned config agree at cell correlation 0.95-1.00 (screen-blood and console aside), and
native against D3D11 at 0.87-0.99 (the frozen guard stands a little differently).

## 2. Per config

"Native/d3d11" is native's first run against D3D11's, "native/native" two native runs; correlation of the cell grids, lightness
mean. Run folders are in `C:\Modding\CS-AB\runs`, labelled `post-refs-<n1|n2|d1|g1|x1>-<config>`.

| Config | Determinism changes | Native/d3d11 | Native/native | References |
|---|---|---|---|---|
| first-person | pinned prelude | 0.94-0.97 | 0.999 | both shots |
| first-person-switch | pinned prelude; the later `tai` and second `tgm` removed (both toggle); look-down shots at 300 frames; the Dragonsreach shots face the east aisle (B1) | 0.86-0.98 | 0.997-1.00 | all 7 |
| post-effects | pinned prelude, first shot at 300 frames | 0.95-0.99 | 0.96-1.00 | all 13 |
| lens | pinned prelude | 0.97-0.98 | 0.98 | both |
| shadows-blend | pinned prelude, first shot at 300 frames | 0.96-0.98 | 0.98-1.00 | all 7 |
| upscalers | pinned prelude | 0.87-0.89 (guard placement) | 0.98-1.00 | all 4 (native runs TAA for all, decision log) |
| update-cap30 | pinned prelude, start at 300 frames | 0.80-0.97 | 0.97-1.00 | all 3 |
| update-cap240 | pinned prelude, start at 300 frames | 0.49 turn (timing), 0.96 others | 0.95-1.00 | all 3 |
| save-screenshot | pinned prelude | d3d11 has no `imagespace.save`; guest 0.96 | 0.998 | both |
| ui-menus | pinned prelude, HUD shot at 300 frames | 0.95-0.97 | 0.995-1.00 | all 4 |
| native-tour | pinned prelude | 0.96-0.98; Dragonsreach B1 | 0.995-1.00 | start, whiterun |
| cloud-shadows | pinned prelude with the storm locked (was `fw`) | 0.97-0.99; interior B1 | 0.99 | clouds, clouds-opaque, exterior |
| cadence-drop-cells | pinned prelude (time, weather, AI) before the cocs | 0.96-0.99; Dragonsreach B1 | 0.96-1.00 | whiterun, riverwood |
| native-river (river.toml) | `tgm`, `tai`, `tcai` first | 0.93-0.95; river-east B1 | 0.97-0.99 | river |
| taa-water | as river | 0.94-0.95; river-east B1 | 0.99-1.00 | river |
| rain | `tgm`, `tai`, `tcai`; storm locked before the coc (was `fw` after it) | 0.95-0.96 | 0.98-1.00 | both |
| llf-local-shadows | pinned prelude; faces the east aisle after the coc (straight ahead is B1) | 0.98-0.99 | 0.97-1.00 | all 6 |
| sky-native | `tgm`, `tai`, `tcai` first | 0.95 noon, 0.94 night; B1, B3 | 0.99-1.00 | river-noon, sky-night |
| screen-blood | time 13:00 and weather locked only (the bandit must hit) | 0.37-0.44 (random splatters) | 0.45-0.50, 3-8 wrong tiles | both (coarse check only) |
| map | `tgm`, `tai`, `tcai` first; `shot_refs` added | 0.97-0.98 | 1.00 | whiterun retaken; map, map-b kept |
| map-local | `shot_refs` added (the references existed, nothing checked them) | 0.86 (ISLocalMap explored area, accepted) | refs held | kept |
| menu-main | none possible | 0.84-0.90 | 0.76-0.84, 23-28 wrong tiles | none: the mist differs every run |

Configs that already had references and were left as they were: quick, native-interior (see B1), menu-scenes, facegen,
menu-book, menu-loading; guest+quick, guest+menu-scenes; native-d3d12-quick.

Known accepted differences seen (not counted as bugs): native water brighter and its reflections sharper than D3D11's
(`dclf-open-defects.md`, "Graph-mode water is still brighter"; accepted in the river references, decided by the agent, to confirm);
ISLocalMap's explored area (map-local); the main menu logo brighter in both graph modes than in D3D11 (163 against 90 mean over the
logo; guest matches native), taken as Linear Lighting on menu models; update-cap's turn shot three frames into a turn.

## 3. Bugs found

- **B1 (medium): native draws fire glow as large orange translucent shapes.** Dragonsreach's hearth becomes a ceiling-high orange
  column and the side braziers orange blocks; Riverwood's smelter an orange haze over the building behind it. D3D11 and guest show
  neither; native D3D12 shows it too (orange-ness of the column region 89 D3D12, 77-80 Vulkan, 20 guest and D3D11). Present in every
  native-interior run kept since at least 2026-11-26 06:23, so **native-interior's accepted references contain it** (1127-085427).
  Runs: native 1127-171928 (first-person-switch interior-first), 1127-173705 (native-tour dragonsreach), 1127-180234 (sky-native
  dragonsreach), 1127-175223 (native-river river-east), x1-ni (native D3D12 native-interior); ground truth 1127-180706 (d3d11
  native-tour), d1-ni and g1-ni (d3d11 and guest native-interior). The shots that show it are left out of `shot_refs`.
- **B2 (high, intermittent): native lost the device on cloud-shadows' coc into Dragonsreach** (1127-173821-...-n1-cs, 689 frames
  after world-ready: "the wait for the last render's GPU work failed", then DeviceLost on an exempt transfer). The second run passed.
  Probably the device-loss class snap7-devloss3 owns; not checked further.
- **B3 (low): native does not fade the player when the camera is close.** sky-native's river-sky-noon looks up past the player's
  helmet: D3D11 draws it faded, native opaque (1127-180234 against d1-sky). Left out of `shot_refs`.
- **B4 (high, intermittent): crash in `Snapshot::FrameCamera::WriteMain` during loading** (ui-menus 1127-173428, access violation
  at FrameCamera.cpp:153, a null block, reached from DCLF's water port capture in `FrameDriver::GameFrameStart`). Two later runs of
  the same config passed.

Not a bug: the black materials of decision 234 did not show in any non-resize config.

## 4. Left without references

- menu-main: the main menu's mist is random; two native runs differ by 23-28 of 144 tiles (the limit is 33).
- The shots left out above (B1, B3), and update-cap's nothing; screen-blood's references are coarse by nature.
- Resize configs: snap7-devloss3's.
- guest+map and native-d3d12+map keep whiterun references from before map turned AI off; they still pass the coarse check (native's
  old one did at 15 of 144 tiles) but should be retaken.

## 6. Verification

After the last config edits (the Health Low step, the shot_refs placement fix, the re-aimed Dragonsreach shots), a third native run
of first-person, post-effects, shadows-blend, update-cap30, native-tour, cloud-shadows, screen-blood and llf-local-shadows ran with
the references checked by the harness: all passed, every referenced shot within the check (`post-refs-n3-*`). cloud-shadows did not
lose the device in its other two runs (B2 is 1 in 3).

An early version of the helper that adds `shot_refs` put the line below a later `[expect.graph_frames]` table in four configs, which
then failed to load; fixed in 157fcce14, before any director run used them.

## 5. Notes for the director

- `ref accept` writes the manifest next to the `cs_test.py` that runs it, so a lead using the main checkout's script would write
  the main checkout's `refs.json`. Leads that add references should run their worktree's script (and copy the lab helper DLL), or
  set `CS_TEST_REFS`.
- The harness check is coarse by design: a pinned scene does not make it stricter, it makes failures it does catch reliable.
  Decision 234's resize shots would fail it (most tiles dark).
