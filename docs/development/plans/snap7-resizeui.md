# snap7-resizeui: the HUD, the water and a game-thread crash around a resize

Lead `resizeui`, round `snap7`, branch `snap7/resizeui` (base `org_integration` 7fe8f4a45), stage `s7-resizeui`. Dev-Fast
throughout. RTX 5080.

Status: **HUD fixed in native and guest; the CameraPlanes crash fixed (and post-bugs' B4 with it); the water after a resize is
still broken, cause narrowed to the water surface's stencil test.**

## 1. Commits (CS, branch snap7/resizeui)

| Commit | What |
|---|---|
| df6e304a0 | fix(scaleform): HUD masks and layout after resize |
| 195e7cc02 | fix(dclf): water game half reads no render state |
| e86a39b7a | test(cs-test): native resize stress references |
| (this file) | docs: snap7-resizeui notes |

No submodule change.

## 2. The HUD (df6e304a0)

Two faults, both in the UI replay.

- **Masks off in native since the render thread.** The compass letters (E, S, N) drew outside the compass and the health bar's
  fill drew full width, in every native shot, not only after a resize. Shots date it: native quick was right up to `s7-thui-f` and
  wrong from `s7-thread-s1a` on (1127-053439). The record-only stream reads the engine's targets at its replay
  (`ReadEngineTargets(a_atReplay)`). On the render thread the shadow state's depth is not the frame's (the render side runs none of
  the engine's frame), so `display.depth` was null, `needsDepth` was false, and every mask was dropped without a log line. The replay
  now takes kMAIN's depth, which DrawInterface keeps bound at the engine's UI point. Guest and the producer's own frames are
  unchanged (they read at record time).
- **Layout after a resize (native and guest).** GFx lays a movie out for the size it was loaded at
  (`BSScaleformManager::LoadMovie` reads `State::frameBufferViewport` once). Nothing sets an open movie's viewport again, so after a
  resize the HUD kept its 1920x1080 layout on a 1280x720 target (crosshair at 960,540). The display now keeps its viewport's buffer
  size (`Display::layout`), and `Passes.cpp` maps the viewport from it onto the surface's size at commit. Render texture displays
  and scratch (pushed) segments are not mapped. The result is the 1920x1080 HUD stretched onto the window. Plain D3D11 shows the
  same: it never resizes (Hooks.cpp keeps the window size), so DXGI stretches the frame.

Decided by the agent, to confirm (RU-a): map the old layout onto the target at replay, rather than set each open movie's viewport
again on the game thread. The replay knows the target it draws on; the game side would race the render side's resize. It matches
d3d11's picture and needs no ActionScript re-layout. A new aspect ratio stretches the HUD, as in d3d11. Mouse mapping in menus after
a resize was not checked.

## 3. The CameraPlanes crash (195e7cc02)

r-final1's crash (snap7-devloss3) and post-bugs' B4 (`FrameCamera::WriteMain` during a load, ui-menus run 1127-173428) are one
fault. DCLF's water shading field capture (`WaterSceneRows::CaptureMaterials`) runs in DCLF's game half, which is the game thread
since the render thread. It called `Port::CaptureFrame`, which is render-thread code: `Snapshot::Current()`, the frame camera and
`CameraPlanes`. Those read the render side's adopted snapshot while the render thread adopts, so the game thread read a
`shared_ptr` the render thread was replacing (rax = 0xB0 in the crash). The frame row it made also carried another render's camera
planes and target size.

Fix:
- `Port::CaptureGameFrame`: the game half's inputs for the material rows. Native: the game's fields from the snapshot being built
  (`Snapshot::Building()`'s `GameBlock`) and CS's hook fields; no view fields. Guest: `CaptureFrame` as before.
- The water frame row (t59) is a `perRender` pool in native: each render's upload kicks `CaptureFrame` on the render thread, so its
  VPOSOffset and camera planes are that render's. Guest keeps the game-half kick.
- The material rows' SSR texel size (PS c12.zw) has no source in the game half, so `Water.hlsl` takes it from the frame row's
  VPOSOffset (the same value: the inverse kMAIN size). `EvaluateMaterial` writes 0 for a missing size rather than infinity.

Open (post-bugs' note, agreed): `CaptureFrame` in the per-render kick still reads two engine globals (`kViewProjJittered`, and
`ReadGame` when the adopted snapshot has no `GameBlock`). The shader reads neither the view-projection rows nor anything else that
depends on them. Both reads were render-thread reads before the render thread too. Moving them into the capture is a small
follow-up.

## 4. The water after a resize: not fixed

What it is (native and guest, base and branch alike):
- At the small size the river is missing: the riverbed shows through, as if no water were drawn. Base sometimes shows it partly in
  the first small shot and gone a few frames later.
- After resizing back the river draws, but dark with a ripple pattern. It stays so for 600 frames, through `org.rebuild` and
  through TAA off and on. With TAA off it is the same, so the water blend and its TAA history are not the cause.
- Guest shows the same, so the fault is in shared code (DCLF water, the skeleton or the engine target set).
- It is broken, not flickering: consecutive shots 2 frames apart agree.

Ruled out with a temporary pass/fail output in `Water.hlsl` (`g-dbg2`): before the resize and after the resize back, the surface's
refraction (t1) and depth (t7) are display-sized, SSR is half-sized, VPOSOffset matches the display, the scene depth behind the
water is farther than the water, and the material colours and camera planes are set. Validation (`h-val`) shows only the
transitional render-area messages devloss3 saw.

Narrowed (`i-nostencil`): with the DCLF water surface's stencil test off (`SurfaceState`; `kSurfaceStencilMode` 30 is NOT_EQUAL on
bit 0, reference 1) the water draws at 1280x720 and every check passes. So after a resize kMAIN's stencil has bit 0 set where the
river is. The skeleton's kMAIN clears record at the right extent with their gates open after the resize (temporary logging,
`j-clearlog`: `depth0.0@z-prepass` flags 3 and `depth0.0@engine-draws` flags 2, at 1280x720, then at 1920x1080). Next leads: who
writes stencil bit 0 into kMAIN between the clears and the water surface, and whether the clears and the water passes bind the same
kMAIN version (the new version is "born undefined"). Then the dark shading after the resize back, whose checked inputs all pass.

The native small references (e86a39b7a) are taken with the river missing. Re-accept them when the water is fixed.

## 5. Runs (stage s7-resizeui unless named; labels start s7-resizeui-)

| Label | Config | Result |
|---|---|---|
| a-water | native-vulkan+window-resize-stress-taa, consecutive shots before, after and back | passed; HUD right; river missing small, dark after |
| b-waterbase (stage dir, base 7fe8f4a45) | the same | passed; the same water, old HUD |
| c-waterlong | 600 frames, org.rebuild, upscale again after the resize | passed; water stays broken |
| d-guestwater | guest+window-resize-stress-taa, one resize | passed; guest HUD right; guest water dark after the resize |
| e-notaa | TAA off and on around the resize | passed; water broken either way |
| f-dbg, g-dbg2 | debug water shader (values, then pass/fail) | passed; checks pass; no water drawn at 1280 |
| h-val | org.validation=true | passed; transitional render-area messages only |
| i-nostencil | surface stencil off (debug build) | passed; water draws at 1280 |
| j-clearlog | clear logging (debug build) | passed; clears at the right extent |
| k-final | native-vulkan+window-resize-stress-taa, final branch | passed (start and large against the old refs); all 13 accepted from it |
| l-nvk-quick | native-vulkan+quick | passed |
| m-guest-quick | guest+quick | passed |
| n-nvk-ui-menus | native-vulkan+ui-menus | passed (HUD masked) |
| o-cadence | native-vulkan+cadence-drop-cells | passed |

Skipped: menu-resize and hdr-resize (they resize in menus, which render unrouted on the producer thread; the record-only replay is
not used there, and the layout mapping is covered by the stress run), native-d3d12-quick (no ORG, BasicRHI or device change).

## 6. Decisions to confirm

- RU-a. The UI replay maps GFx's layout onto the target's size after a resize (section 2).
- RU-b. The replayed UI stream masks with kMAIN's depth (section 2).
- RU-c. The water frame row is per render in native; the game half takes only game inputs from the snapshot being built; the
  material rows' SSR texel size comes from the frame row (section 3).
- RU-d. Native's small references are accepted with the river missing, so the resize run still catches black frames; re-accept them
  with the water fix.
