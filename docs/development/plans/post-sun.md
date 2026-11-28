# post-sun: the render with no sunlight, the pose jump, the flicker limits

Lead `post-sun`, branch `post/sun` (base `org_integration` fa7c73d2c), stage `post-sun`. RTX 5080, Dev-Fast. Runs are in
`C:\Modding\CS-AB\runs`, labelled `post-sun-*`. The user asked for real corruption to be fixed, one-frame flickers included.

## 1. Commits

| Commit | What |
|---|---|
| 8be8161c0 | feat(dclf): log sun input changes at native masks (the sun watch, section 2) |
| 55a35f41a | fix(sky): publish light colours without a gap (the cause, section 2) |
| a24c74d5f | test(cs-test): per-burst flicker limits; menu-scenes stops the fight (section 4) |
| 524c5a921 | test(cs-test): menu-scenes waits out the swing |
| (this file) | docs |

No submodule change.

## 2. The render with no sunlight (fixed)

What it looked like: one snapshot drawn with the sun about five times dimmer (everything near in shadow, LOD less so), its
repeats too, then about 0.3-0.5 s of recovery. The recovery is the temporal upscaler's history and a small eye-adaptation
overshoot, not a second input: the next snapshot's inputs were already right.

How often: a scan of every burst in the runs folder (428 bursts) found it 5 times, always on a fresh render (render 1 of its
snapshot), in Whiterun, Riverwood and the Meadery, post-small's `ext1` twice in one run. In a still Whiterun exterior it came
about once per 500-1000 snapshots.

Diagnostic: `SunWatch.cpp` logs, at native's world masks, every sun input that changed since the last world render: the mask
flags, the light set's sun (kept, drawn, verdict, maps, fade, diffuse), its cascades (splits, transforms, targets), the core
block's sun colour, fade and sunlight scale, and DCLF's frame globals (deferred-shadow and no-sun-shadow flags, the mask view,
the pipelines' sun colour). A still scene logs nothing. It stays in as the way to name such a frame next time (400 lines a
session at most).

First run with it (`hunt1`, 8 bursts of 600 frames in a still Whiterun exterior): the burst failed at snapshot 1923 (spike
59), and the watch logged exactly that render, plus four more pairs in the 40 s:

    render 5953 (snapshot 1923): sunDiffuse 29.2->2.565 dirColour 29.2->2.565 sunlightScale 1->2.3
    render 5954 (snapshot 1924): sunDiffuse 2.565->29.2 dirColour 2.565->29.2 sunlightScale 2.3->1

Cause: the sun light's colour is Sky Sync's override, made on the game thread in Sky::Update from Physical Sky's working light
colours (`SkySync::workingLightColors`). Physical Sky writes those in its per-frame `Reset`, which is a render-frame step: it
cleared them at its start and set them about 130 lines later. In guest both run on the main thread one after the other. In
native `Reset` runs on the render thread while the game thread runs Sky::Update, so an update that read inside the gap found no
override and kept the weather's sunlight (2.565 at HDR scale 2.3, against the override's 29.2 at scale 1). It was also a data
race on a `std::optional`.

Fix: `Reset` builds the colours locally and publishes them once, at its end (a scope guard covers the early return). Sky Sync
holds them under a lock (`PublishWorkingLightColors`, `WorkingLightColors`); `ApplyLightColor` takes a copy. Guest's order is
unchanged.

After (`hunt2`, same config): all 8 bursts under 1.9, and the watch logged only the load's own changes (the first drawn sun,
the first override). Five glitches in `hunt1`, none in `hunt2`.

The director's reproduction (1128-091608-dir-native-vulkan-flicker-scenes, river-east f031, org_integration 16f1d2167 without
this fix) is the same signature. By the grid the repeat f032 is darker still (frame mean 80.8, then 69.5 on the fresh render,
67.9 on its repeat, then 75.4 and recovering). The repeat looks right by eye only because the shadowed river scene changes
little. The fix is not in that build.

## 3. The one-frame pose jump (real game content)

The guest and D3D12 load-save bursts spiked 16-18 on one tile (x 1040-1080, y 720-760), always near snapshot 235 after the
save's load: the player's right arm is in another pose for one frame. Native Vulkan never showed it only because its 120-frame
burst ended near snapshot 207-212 (it renders faster); d3d11's ended at 192.

A d3d11 run with a 300-frame load-save burst (`pose-d3d11`, the engine renders everything) shows the same jump at the same tile
at snapshot 234: spike 13.2. So it is the game's own animation, not a stale or skipped pose. Nothing to fix.

## 4. Flicker limits

- **menu-scenes exit-inventory (15.9):** a guard swinging his sword (the save's fight; frames 45-48 of
  1128-082516-dir-...-menu-scenes). Real motion, but avoidable: menu-scenes now turns AI and combat off (`tai`, `tcai`) right
  after its world shot and waits 180 frames before the inventory: AI off does not stop a swing already playing (the first try,
  with no wait, caught the guard's sword and its shadow mid-swing at 26.0, `v-native-vulkan+menu-scenes`). The menus pause the
  game, so the menu shots do not change (all five held their references). exit-stats (9-13) is the stats menu's camera
  coming down through the nebula: real transitional motion, under the limit, left as it is.
- **cadence-drop-cells (about 21):** the 20.6 was `k-noanim1` load-riverwood f054: the camera settling behind the player right
  after the coc, in drop:3's steps of three updates, the dark back and bright arms crossing tiles in a step that comes back.
  Real motion. Every cadence run since F2's fix scored 0.6-3.9 there. cs-test now takes per-burst limits
  (`[expect.flicker.bursts.<name>]`, over the shared ones; README), and `load-riverwood` gets `max_spike = 35`. The missing
  mist this burst caught (post-corrupt F2) scored 57-79, so it stays caught. The other bursts keep 20 (the purple fire in
  load-dragonsreach scored 16-23).

Decided by the agent, to confirm: a per-burst limit for load-riverwood instead of a cadence-aware score (comparing a repeat with
its own fresh render), which would be the stronger check but a bigger change to the metric.

## 5. Runs (stage post-sun)

| Label | Config | Result |
|---|---|---|
| hunt1 | sunhunt (scratch: still Whiterun exterior, 8 x 600-frame grid bursts), sun watch only | failed: s2 spike 59.2 at snapshot 1923; 5 glitches logged |
| hunt2 | sunhunt, with the fix | passed, every burst under 1.9, no glitch logged |
| pose-d3d11 | d3d11 + native-interior's prelude with a 300-frame save burst | passed; the pose jump at snapshot 234 (13.2) |
| v-native-vulkan+quick | native-vulkan+quick | passed |
| v-guest+quick | guest+quick (Physical Sky and Sky Sync are shared code) | passed |
| v-native-d3d12-quick | native-d3d12-quick | passed |
| v-native-vulkan+cadence-drop-cells | cadence-drop-cells | passed, every burst under 1.4 |
| v-native-vulkan+native-tour | native-tour | passed, bursts 0.26 and 1.07 |
| v-native-vulkan+menu-scenes | menu-scenes, tai/tcai with no wait | failed: exit-inventory 26.0, the guard's swing still playing |
| v-menu-scenes2 | menu-scenes, 180-frame wait | passed: exit-inventory 0.23, exit-journal 1.07, exit-stats 12.3 |

CPU load: `hunt1` had the CPUs saturated by another lead's build for 92 s; `hunt2` did not report saturation.

## 6. Decisions to confirm and open items

- PS-a. The sun watch stays in (logs on change only, capped). Decided by the agent, to confirm.
- PS-b. Per-burst limit for cadence-drop-cells load-riverwood (35); menu-scenes stops the fight after its world shot.
- Open: other render-frame `Reset` steps that write values the game thread reads in native would race the same way. Physical
  Sky's lightning and the cloud wind step from the snapshot (`StepOnce`), so they are fine; a sweep of every feature's `Reset`
  for game-side readers was not done.

Proposed decision-log entry: the rare native render with no sunlight was a race between Physical Sky's per-render `Reset`
(render thread) and Sky Sync's Sky::Update override (game thread): `Reset` cleared the working light colours before refilling
them, and an update reading in the gap kept the weather's sunlight for one snapshot. The colours are now published once under a
lock; a sun watch logs any sun input change at native's world masks. The guest/D3D12 pose jump after the save's load is the
game's own animation (d3d11 shows it). cs-test takes per-burst flicker limits; cadence-drop-cells' Riverwood burst has 35 (real
camera motion at drop:3), and menu-scenes stops the guard fight after its world shot.
