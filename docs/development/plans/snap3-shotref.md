# snap3-shotref: cs-test fails runs whose shots are wrong

Lead: `shotref`, branch `snap3/shotref` (base `org_integration` 245ec15e5). Python and README only (`tools/cs-test`).

## Why

Decision 176: for an afternoon every native world frame drew in the top-left 1280x720 corner (decision 175) and every
cs-test run said "passed", because nothing compared shots with a reference.

## What it does

- `expect.shot_refs = ["start"]` in a config names the shots to check. It is a harness key, so the game never sees it.
  Opted in: `quick` (so `guest+quick`, `native-vulkan+quick`, and `native-d3d12-quick`, which extends it), `facegen`
  (`racemenu`), `native-interior` (all four), `menu-scenes` (all but `loading-model`, since the loading screen's model is random).
- References are per run name and shot (`native-vulkan+quick/start`). `tools/cs-test/refs.json` (in the repo) maps each key
  to a sha256, its size, the run it came from and the date. The PNGs live in `C:\Modding\CS-AB\refs\<sha256>.png`
  (`paths.refs`; `CS_TEST_REFS` moves the manifest, which the tests use).
- `cs_test.py ref accept <run> [--shot X]` copies a passed run's shots into the store and writes the manifest. Nothing
  accepts by itself. `ref check <run>` checks a run's shots like a run does; `ref check <png> --against <png|run|key>` does
  it offline; `ref list` lists the references.
- After a passed run, `runner.run` calls `shotref.check_run`. A failure fails the run with the reason, for example
  `shot start: 89 of 144 tiles wrong: 49 dark by 86 (x 0-1320, y 0-720), 39 flat (detail 1.3, reference 20.4; ...)`.
  A shot with no reference only logs the `ref accept` line to run. A reference missing from the store, or an unreadable
  shot, fails the run. `result.json` has `shot_refs` (measures, a 16x9 tile map, failures).

## The metric (`cstest/shotref.py`)

Standard library only (Pillow and numpy are installed, but cs-test uses neither). Decoding a 1080p PNG with
`shotdiff.read_rgb` plus the grid takes about 1 s.

- Each shot becomes a 96x54 grid of cell lightness: 255 * sqrt(mean luma / 255). The square root matters: the current
  native scenes are dark (mean luma 17), so in luma the detail is tiny and a flat grey region looks like the scene.
- Cells are grouped into 16x9 tiles. Per tile: the mean, and the detail (the standard deviation of its 36 cells).
- A tile is wrong when its mean moves by more than 56 (dark or bright), when its detail drops under 4 while the
  reference has at least 2.5 times as much (flat), or the other way round (busy).
- A shot fails when more than 23% of its tiles (33 of 144) are wrong, when its mean lightness moves by more than 20, or
  when it is black (under 20, luma about 1.6) or white.
- The correlation of the two grids goes into the report but is never judged. In the quick save's fight the camera angle
  changes from run to run, and that moves the correlation as much as the quarter frame does: good pairs go down to 0.21,
  bad pairs go up to 0.45.

### Calibration

The calibration used every passed run since 14:00 of the opted-in configs whose steps match the config, about 300 runs.
Native runs were sorted by build: "bad" means water merge fb8387ed3 without fix 75620d32b. "Dark" means
descended from 3f5b50150 (see below), and the rest were "bright". The SE instance, a loading-screen shot and builds whose
scene really differs were left out. That gave 741 good pairs (same class, same config and shot) and 525 bad pairs (good
against bad). A grid search over the five thresholds picked the values above:

- 0 bad pairs pass. The best bad pair has 45 wrong tiles, against a limit of 33.
- 11 good pairs fail, all between builds that really differ: s7-menus2's sw2 build has a darker stats menu, and
  Whiterun's exposure after the coc back (`vl-exterior`) moves by 35-40 between dark-era builds. The worst good
  same-scene pair has 22 wrong tiles.

## References (accepted 2026-11-26)

| Key | From run | Note |
|---|---|---|
| guest+quick/start | 1126-174928 dir-s7sp (0b767f477, head) | |
| guest+menu-scenes/{world,inventory-item,journal,stats} | 1126-180107 dir-s7sp (head) | bonus |
| native-vulkan+quick/start | 1126-145905 dir-s3fg (f672f9012) | before the native darkness |
| native-d3d12-quick/start | 1126-145949 dir-s3fg (f672f9012) | before the native darkness |
| native-vulkan+native-interior/* | 1126-150517 dir-s3fg (f672f9012) | before the native darkness |
| native-vulkan+facegen/racemenu | 1126-142517 s7-facegen-a (7b122a22a) | before the darkness; the face tint is still pinkish (before facenoise), which a coarse check does not see |
| native-vulkan+menu-scenes/* | none, pending | no bright run of the current scene exists (s7-menus ran another save) |

Native references come from before the darkness regression, as the director directed. All of them show the current
scene (the guest shots, mean luma 64, prove the save did not change).

**So every native run of the current head fails the check** with "mean lightness about 60, reference about 124" until
snap3-dark's fix lands. That is intended: the head is wrong.

## Proof (offline, `ref check`)

| Shot | Against | Result |
|---|---|---|
| quarter frame `1126-162304 .../racemenu.png` | native-vulkan+facegen ref | FAIL: lightness 84 vs 139; 86 of 144 tiles (64 dark, 22 flat at x 720-1920) |
| quarter frame `1126-161752 .../start.png` | native-vulkan+quick ref | FAIL: lightness 78 vs 124; 89 of 144 tiles (49 dark x 0-1320 y 0-720, 39 flat) |
| quarter frame start | the fixed head's dark shot (1126-175012) | FAIL: 48 of 144 tiles (25 bright, 21 flat); the framing is caught without the brightness |
| quarter frame racemenu | fixed head 1126-175537 | FAIL: 54 of 144 tiles (36 flat at x 720-1920) |
| head native quick, d3d12, facegen, interior (dir-s7sp) | refs | FAIL: lightness 60 vs 112-139 (the darkness regression) |
| guest+quick 1126-132816 | 1126-120652 (same build 5809202ae) | pass, 0 tiles |
| native-vulkan+facegen 1126-143616 | ref 1126-142517 (same build 7b122a22a) | pass, 0 tiles |
| s7-quarter native quick 1126-170108 | head 1126-175012 (fix in both) | pass, 0 tiles |
| guest+quick 1126-174233 (s7-spin), 1126-172927 | ref | pass |
| s7-water native quick, d3d12, interior | refs | pass (11, 19, 0-17 tiles) |
| s7-menus2 guest+menu-scenes | refs | pass (0-8 tiles) |

Unit tests: `tests/test_shotref.py` builds synthetic pictures: noise plus a small shift passes; the corner frame, a black
frame and a blown-out frame fail; it also covers a size change, the accept/check round trip, a reference missing from
the store, refusing an unpassed run, and the config key. `tests/test_runner.py::test_reference_shots_are_checked`
covers the runner wiring. 134 passed.

## Runs

| Config | Label | Result |
|---|---|---|
| guest+quick | s7-shotref-guest-quick | failed by the new check, rightly: a second load started 45 frames after world-ready (the fight's player death or a reload; the world frames were slow), so the shot shows a loading screen. Reason: `shot start: black frame (mean lightness 11.9, reference 124.2); 138 of 144 tiles wrong: 137 dark by 117` |
| guest+quick | s7-shotref-guest-quick2 | passed; start: 0 of 144 tiles wrong, lightness 124.4 against 124.2 |

The stage `s7-shotref` is an AIO copy of `CS AB dir` (0b767f477), since this change needs no build.

## Findings

- **Native darkness regression** (reported to the director, now snap3-dark): native world shots have been about half as
  bright as guest since 3f5b50150 (the post, dclf and depth merges together). Mean luma on quick went from 64 to 17.5, on
  racemenu from 80 to 41.6, on the menu-scenes world from 108 to 18. None of the three branches alone shows it.
- `quick` has no `tgm`, and a run can reload in the middle of the fight: s7-shotref-guest-quick took its shot on a loading
  screen and the game still said passed. The new check failed it. Adding `tgm` to `quick` would keep the player alive;
  not done here.
- The quick save's scene (a Whiterun guard fight) is not stable: the camera angle, Health Low popups and motion blur
  change from run to run. That is why the tolerance is coarse. A calmer save (god mode, AI off, as in `cadence-still`)
  would allow a tighter check.

## Decided by the agent, to confirm

- References are keyed by run name, not by config file, so `native-vulkan+quick` and `guest+quick` each have their own.
  A run with `--set` changes is still checked, since a gross failure is a failure whatever the switches.
- A missing reference only logs; it never fails, so new configs can opt in before references exist.
- Native references come from the bright f672f9012-era runs, so native head runs fail until the darkness is fixed.
  After the fix, re-accept from a fixed run: `ref accept <run>` for quick, d3d12-quick, native-interior, facegen and
  menu-scenes.

## Proposed decision-log entry

- cs-test checks shots against kept references (`expect.shot_refs`, `refs.json` plus `C:\Modding\CS-AB\refs`, `ref accept`
  only by hand). The check is a coarse tile comparison in lightness and catches framing, black or missing worlds and
  large brightness shifts. Native references predate the 3f5b50150 darkness, so native runs fail until it is fixed.

## Open items

- Accept native references again after snap3-dark's fix, and add native-vulkan+menu-scenes.
- A native vs guest cross-check (same shot, larger tolerance) would have caught the darkness the day it landed. Not done.
