# snap7-devloss3: the resize transition, round 3

Lead `devloss3`, round `snap7`, branch `snap7/devloss3` (base `org_integration` c6bd5950c), stage `s7-devloss3`. Continues
[snap7-devloss2.md](snap7-devloss2.md) and [snap7-devloss.md](snap7-devloss.md). Dev-Fast throughout. RTX 5080.

Status: **resize renders correctly in native now; the device loss is halved but not gone, so the interim wait stays.**

- Found and fixed: after every resize DCLF's shadow mask binding still named the released old `kSHADOW_MASK`, so everything
  DCLF lights went black (native and guest), and reads of the freed memory went through garbage. Without the interim wait,
  window-resize-stress-taa went from 0 of 11 passing to 5 of 10.
- Fixed: a screenshot copy that read past a resized target.
- Added shot references to window-resize-stress-taa, so a black frame now fails the run.
- Tried and dropped: one revision for the stages and DCLF (direction 2), and the exact retirement clock (direction 3, DV2-b).
  Both raised the loss rate even with the interim wait (section 5).
- Not fixed: the rest of the device loss, guest's device loss at the second resize, and the HUD keeping its old size.

## 1. Commits (CS, branch snap7/devloss3)

| Commit | What |
|---|---|
| 7e2acba9f | fix(dclf): bindings follow resized engine targets |
| 27502921c | fix(screenshot): copy the adopted target's size |
| 47ae3f80d | test(cs-test): shot references for resize stress |
| (this file) | docs: snap7-devloss3 notes |

No submodule change.

## 2. The black frames after a resize (7e2acba9f)

The director found that window-resize-stress-taa was visibly broken even when it passed: after the first resize everything DCLF
lights was black (mean luma about 20 against 65 at the start), grass, sky and HUD fine. Guest had it too; plain D3D11 did not.

Cause: a resize does not make new engine target views. The engine target set points the same views at new versions (native),
and DCLF's shadow mask binding was keyed on the view pointer and the GpuTextures generation, neither of which changed. So
`ResolveMaskBinding` kept the import of the old image, and every colour draw read t14 from it. The old version is released a few
frames after the adoption, and its memory is placed again.

Fix:
- `GpuTextures::NoteViewChanged` (was `NoteWindowChanged`, which texture streaming keeps using): any view that names another image
  now has its import superseded and logged.
- DCLF adds a `Frame::Revision::OnVersionsAdopted` listener (FrameDriver.cpp): when the adopted `EngineTargetShapes` change, it
  supersedes the imports of the changed colour targets' SRV and SRVCopy and the changed depth targets' depth and stencil SRVs.
  Material bindings already follow the supersede log.
- The shadow mask bindings now follow it too: a superseded owner no longer "stands", and the frame's mask in the log triggers a
  sweep. While the new import is pending they read the null descriptor rather than hold the old image.

Result: every native resize shot is at the start shot's brightness (luma 61-70 in r-final1..5). Guest's small0 is right too (67).

## 3. The device loss now

Without the interim wait, window-resize-stress-taa passed 3 of 5 (p-nowait, mask fix) and 2 of 5 (q-null, with the null
descriptor), against 0 of 11 in round 2. One of q-null's losses came before the first resize, after the TAA switch's revision. The
losses stop in cs.dclf.z.depth, cs.dclf.cull-counters or cs.dclf.reflection.faces. So stale bindings were most of it but not all.
The interim wait stays (DV3-a).

Base check today (m-base1..3, `org_integration` with the wait): 3 of 3 passed, all black after the first resize. With the mask fix
and the wait (r-final1..5): 4 passed, 1 crashed on the game thread (section 7), no device loss.

## 4. Other findings with Vulkan validation

`--set org.validation=true` on window-resize-stress-taa:
- `vkCmdCopyImageToBuffer` copied 1920x1080 out of a 1280x720 `kFRAMEBUFFER` for a screenshot right after a resize: the copy's size
  came from the engine's texture (already new), while the copy reads the adopted version (still old). Fixed in 27502921c (native
  only; guest's captures read D3D11's own texture).
- `vkCmdBeginRendering` with a 1920x1080 render area on a 1280x720 `kMAIN` during a resize revision's preparation. A BasicRHI
  diagnostic (reverted) named `cs.upscaling.copy-to-main`, `cs.pp.output.main` and `cs.pp.output.main-copy`. They are recorded on
  the stages' old builds against the pending versions, and decision 94 says those recordings are dropped. Not shown to run.
- Synchronization validation (submit-time on) found no hazard in DCLF's passes.

## 5. Tried and dropped

| Change | Result |
|---|---|
| One revision for the stages and DCLF (direction 2): DCLF follows the preparing revision's display size, `FragmentSource::serves` makes the revision wait for a scene made for its variables | worked as designed (DCLF adopted in the same Begin), but without the wait still lost; with the wait 2 of 5 (n-rev) against base's 3 of 3. Dropped |
| Exact retirement clock (DV2-b): own points, each with its GPU point | with the wait 2 of 3 (l-clockonly); with the one-revision change 2 of 5 (j-final). Dropped. It only retires later, so the loss is sensitive to retirement timing: a lead (section 7) |
| `RenderSize` written with `DisplaySize` | did not remove the render-area messages; reverted |
| Frame slot pacing (Begin waits for the frame that used its slot) | never blocked; the 5-frames-ahead peak in lost runs is the hang's effect |
| No byte sharing within a frame-arena lane | broke a graph's own re-plan (fatal); not usable |
| CPU wait for all earlier work at each staged attach | lost 2 of 2 |
| Keep every replaced DCLF main `Resources` alive | lost 2 of 2 |

## 6. Runs (stage s7-devloss3, native-vulkan+window-resize-stress-taa unless named)

| Label | Build | Result |
|---|---|---|
| a-fix1, b-exact1 | one revision; + exact clock; no wait | lost 2 |
| c-nolane1, d-attachwait1..2, e-slots1..3, i-keep1..2 | experiments (section 5), no wait | lost 7, hung 1, passed 1 |
| f-syncval1, g-val2, g-rs1..2, h-diag1 | validation and diagnostics, no wait | lost 5 |
| j-final1..5 | one revision + clock + screenshot, with wait | passed 2, lost 3 |
| l-clockonly1..3 | clock + screenshot, with wait | passed 2, lost 1 |
| m-base1..3 | base, with wait | passed 3 (black after the first resize) |
| n-rev1..5 | one revision + screenshot, with wait | passed 2, lost 2, hang 1 |
| o-mask1, o-maskguest1 | mask fix, with wait; guest | passed (correct shots); guest lost at the second resize |
| p-nowait1..5 | mask fix, no wait | passed 3, lost 2 |
| q-null1..5 | + null descriptor, no wait | passed 2, lost 3 |
| r-final1..5 | final branch, with wait | passed 4, crashed 1 (section 7) |
| s-guest-window-resize-stress-taa | final | lost at the second resize (as base) |
| s-native-vulkan-quick, s-native-vulkan-cadence-drop-cells, s-native-interior, s-native-d3d12-quick, s-guest-quick | final | passed 5 |
| s-native-perf-taa, s-native-vulkan-perf-dragonsreach | final | passed (numbers below) |

Frame rates (Dev-Fast /Od, one sample, steady [Perf] lines):

| Config | Base (interim wait) | Final branch (interim wait) |
|---|---|---|
| native-perf-taa | 144-145 fps | 146-147 fps |
| native-vulkan+perf-dragonsreach | 220-225 fps | 220-224 fps |

## 7. Open items

- The rest of the device loss without the interim wait (section 3). Leads: it is sensitive to retirement timing (the exact clock,
  which only retires later, made it worse even with the wait), so look for a pool or binding that hands out a backing or descriptor
  whose contents or image are not what the reader expects after a revision. Other bindings that cache imports of engine views by
  pointer are worth a sweep like the shadow mask's (shadow textures, projected textures).
- Guest loses the device about 13 ms after the second resize starts (Vulkan timeline idx 3, then an access violation; the crash log
  is cut off). The director's base guest run did the same, so it predates this branch.
- The HUD keeps the old size after a resize (the compass and crosshair laid out for 1920x1080 in the 1280x720 shots). Native's small
  shots have no references until it is fixed.
- r-final1 crashed on the game thread in `WorldPass::CameraPlanes` (null camera data from `Snapshot::Current()`) during a resize
  step: a lifetime race between the halves, not investigated.
- Water flicker after a resize: not checked separately. The water materials' engine-view bindings follow the supersede log, so it
  may be fixed. Worth a look in a background-water shot.

## 8. Decisions to confirm

- DV3-a. The interim wait (DL-a) stays: the device loss is halved, not gone.
- DV3-b. Engine-view imports are superseded at each revision whose `EngineTargetShapes` change, in both modes (guest had the black
  frames too). Decided by the agent: the brief keeps guest's behaviour, but here guest was broken in the same shared path.
- DV3-c. A superseded shadow mask reads the null descriptor until its new import arrives (a frame or two unshadowed), rather than
  the old image.
- DV3-d. Neither the one-revision switch nor the exact retirement clock is committed (section 5). DV2-b stays open.
- DV3-e. The screenshot copy fix is native only.
- DV3-f. Shot references for window-resize-stress-taa: native start and large shots, every d3d11 shot; the config lists all 13.
