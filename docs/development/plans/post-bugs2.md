# post-bugs2: B3 (player not faded up close) and menu scenes with a feature off

Lead `post-bugs2`, branch `post/bugs2` (base `org_integration` 8b424619d, post-bugs merged), stage `post-bugs2`. RTX 5080,
Dev-Fast build. Inputs: post-bugs.md (B3, the Terrain Blending note), post-refs.md, post-fades.md.

## 1. Commits

| Commit | What |
|---|---|
| e7d151beb | fix(dclf): leave out menu Utility draws while building |
| 51958959a | test(cs-test): menu scenes with two features off |
| 2a4885c64 | fix(dclf): leave out local map water while building |

No submodule changes.

## 2. Menu scenes fatal with a feature off (fixed)

Cause: a feature turned off at boot changes the shader defines. CS's shader cache then compiles on demand (the log says
"shaders compile in the background, on demand"). The first native menu-shadow renders asked for a Utility vertex shader that was
not compiled yet. `AddUtility` treated a null vertex shader as a refusal, and a refusal is fatal (decision 149). Lighting and Effect
draws already mapped "no shader" to "its program is still building", which only leaves the draw out of that render. Utility did
not. The hang that followed (post-bugs-exp8, s7-water-f-nodc, f-nodc2) came after the fatal: the fatal's thread stopped and the
watchdog fired 15 s later. It was not a separate compile stall.

It is general, not Terrain Blending's: any feature off at boot does it. snap7-water hit it with Dynamic Cubemaps off
(LoadScreenDSSteed01, LoadScreenTrollCave). Runtime toggles such as `TerrainBlending.Enabled` are frame variables, not defines,
so they do not cause it.

Fix:
- `AddUtility`: a null Utility vertex shader returns `kBuilding` (left out of the render, logged once). A missing BSUtilityShader
  object stays fatal.
- The local map's water draw had the same gap ("no water shader for vs/ps" was fatal). It now returns `kBuilding` too.
- Checked the other lookups in native menu scenes: Lighting and Effect already leave out, the local map land goes through
  Lighting. World water (`WaterNative.cpp`) is snap7-water's area and was not touched.

Test: `menu-scenes-features-off` (menu-scenes with Terrain Blending and Dynamic Cubemaps off at boot, no shot references: it checks
for no fatal). Run post-bugs2-menu-off-nvk passed; the log shows "menu-shadow draw 1stPersonStaff01:0 is left out of this render:
its program is still building (no shader for vs 0x1401a)". The inventory item model is missing in its shot while the background
compile is at 8 of 10; that is the left-out draw, as expected on a cold compile.

## 3. B3: the player drawn solid up close (not fixed, cause narrowed)

d3d11 fades the player when the camera is close (river-sky-noon looks up past the helmet). The faded look needs the player's
fade root node (`currentFade`, +0x130) below 1: `BlendFading` then rebinds the members to the forward fading pass (hint 9).

What the diagnostics showed (temporary logs, not committed; patch kept in the scratchpad `post-bugs2/b3diag.patch`):
- The engine side works. `Actor::SetAlpha` writes fadeAmount 0.1 on both player roots; DCLF's amount events reach the fade root
  row (listed, mirror 0.1, row 0.1). The root is listed and written by DCLF (bits Owned, Written; not stood in, since head parts
  are engine-drawn).
- In a passing run FadeStateCS steps the root from 1 to about 0.11 in under a second, every milestone comes back through the
  readback (no dropped reads), the write-back stores each one on the node, and the shot shows the faded helmet.
- In the failing run (post-bugs2-b3diag-guest) the row had fadeAmount 0.1 for 5 s and the node stayed at 1.000: no step reached
  the node. The one difference in the logs: the root's kAccumulated flag (bit 26, written from DCLF's visibility verdicts) was
  cleared right after the `player.setpos` teleport and never came back before the next coc. In passing runs it also clears at the
  teleport but comes back after 0.5 to 6.5 s (once only 0.1 s before the fade started). So after a teleport the GPU judges the
  player's fade root out of view for a random time, and while it does FadeStateCS runs no OnVisible for it and nothing steps
  the fade.
- The root's node bound is right after the teleport (logged against the player position), so the stale input is on the GPU
  side: the root's portal query or its sampled bound row. Not identified.

Rate: before 22:40 five of seven sky-native runs drew the player solid (post-refs n1, post-bugs ref and guest, the director's
dir-bugs, my first run); after 22:40, 14 of 14 runs faded it (guest and native, diagnostic and clean builds, one cold cache).
Nothing in my branch touches fades, so the trigger looks environmental (lab load or timing). Without a failing run I could not
test a fix, so none is made.

river-sky-noon stays out of `shot_refs` (decided by the agent, to confirm: it would fail whenever B3 shows; post-refs' rule keeps
shots of known bugs out). first-person-switch and third-person shots not rechecked, for the same reason.

Next step for B3: in a failing run, read back the player root's portal query result and its bound row generation on the GPU
(`PreparePortalQueries`, `RootBound` in FadeStateCS.hlsl), or force a full root rejudge and visibility refresh on a camera cut and
see whether the solid case disappears. A teleport test that repeats `player.setpos` a few times before the close-up should
reproduce it more often than one teleport.

## 4. Runs (stage post-bugs2, labels post-bugs2-*)

| Config | Label | Result |
|---|---|---|
| guest+sky-native | b3diag-guest | passed; B3 shown (solid) |
| guest+sky-native | b3diag2, b3diag4, b3diag6 | passed; faded |
| native-vulkan+sky-native | b3diag3, 5, 7, 8, 9, 10 | passed; faded |
| native-vulkan+sky-native (cold cache) | b3diag11-nvk-cold | passed; faded |
| native-vulkan+sky-native (clean build) | b3clean-a, b3clean-b | passed; faded |
| native-vulkan+menu-scenes-features-off | menu-off-nvk | passed; Utility draw left out, no fatal |
| native-vulkan+quick | final-nvk-quick | passed |

Skipped: guest+quick and guest menu scenes (the change is in native menu scene code only), native-d3d12-quick (no ORG,
BasicRHI or device change).

## 5. References

None changed.

## 6. Proposed decision-log entries

- A native menu-scene or local map draw whose shader CS's cache has not compiled yet is left out of that render, for every shader
  (Utility and local map water joined Lighting and Effect). A feature off at boot makes the cache compile on demand, so this is
  the normal path after any feature toggle, not a fatal.
- B3 is not a fade-amount bug: the player's amount reaches DCLF. After a teleport the GPU sometimes judges the player's fade root
  out of view for seconds (kAccumulated cleared), and no fade steps then. Left open.
