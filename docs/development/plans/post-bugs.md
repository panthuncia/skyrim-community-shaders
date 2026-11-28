# post-bugs: three native bugs from post-refs

Lead `post-bugs`, branch `post/bugs` (base `org_integration` b6fb896dd, merged up to e617f7bcc), stage `post-bugs`. RTX 5080,
Dev-Fast build throughout. Bugs as post-refs.md numbers them.

## B4: crash in FrameCamera::WriteMain while loading

Cause: DCLF's water shading field capture (`WaterSceneRows` CaptureMaterials) runs in DCLF's game half, which is the game thread
in a routed update. It called the render thread's `Port::CaptureFrame`, which reads `Snapshot::Current()` and
`FrameCamera::Main()`. The render thread swaps `Current()` while it adopts a snapshot, so the game thread read a torn or freed
snapshot (null + 0x3C in run 1127-173428). It is the same cause as the `WorldPass::CameraPlanes` crash snap7-resizeui found.

Fix: none here. snap7-resizeui's 195e7cc02 ("water game half reads no render state", merged in fd0dfd703) moves the game half to
`CaptureGameFrame` (the snapshot being built) and makes the frame row per render. I told snap7-resizeui and left that path to
it. Not a null check: the read itself was on the wrong thread.

## B1: fire glow drawn as large orange shapes in native

Cause: native-only code in `FrameValues::Impl::SampleListed` replaces a placement row's PreviousWorld with the world the render
side last drew (so a repeated snapshot draws without motion). A billboard row does not hold a previous transform there:
`BillboardRow` keeps the billboard node's base rotation, the bound radius and the face mode in PreviousWorld, and Effect.hlsl
turns the leaf from them (`DCLFBillboardWorldOfMode`). The overwrite gave the turn a scaled base and a garbage face mode. The fire
glow cards (`Glow02`, `glow03`, `glow04` under NiBillboardNode, scales 1.5 to 4.7) then drew stretched to the ceiling. Heat haze
refraction billboards share the row layout.

How it was found (runs `post-bugs-exp*`): guest and native list the same forward members with the same transforms (a temporary
dump at the blended draw); particle rows zeroed, Volumetric Lighting off, IBLF toggled, LLF particle lights off and soft fades
forced on all left the column; `CS_ORG_SHOT_TARGET=1` showed it in kMAIN (a world draw, not post); collapsing effect billboards
in the shader removed it. Light Limit Fix's particle-light culling was ruled out with d3d11 and guest (culling off draws nothing
visible).

Fix (edfc24580): `FrameValues::SampleRow` returns whether the row's PreviousWorld is a transform (false for a billboard leaf), and
the drawn-previous rule skips billboard rows. Guest never took the rule, so it is unchanged. Native Dragonsreach now matches
d3d11 and guest (post-bugs-fix1-nvk-ni).

## B3: the player drawn solid when the camera is close

Not fixed. Guest has it too (post-bugs-b3-guest-sky, river-sky-noon: the helmet solid, as in native), so it is DCLF's, not
native's, and a fix changes guest. post-fades confirmed it is not in their work.

What is known: `PlayerCamera::CheckCameraCollision` (AE 0x1408e48d0) fades the actor it collides with through the actor
process's fade state (`ActorProcess` start fade-out 0x1406eb4f0, `ActorProcess::FadeIn` 0x1406eb490; state at middle-high
+0x130, alpha +0x134). `Actor::SetAlpha` (0x1406c1a50) writes fadeAmount (+0x100) on both of the player's 3D roots. DCLF follows
fadeAmount through SWF's `SWF_HOOK_ACTOR_SET_ALPHA` and FadeStateCS's amountFade, and its Lighting descriptors read the fade
node's currentFade (`StaticShadowBits`, `BlendFading`). Next step: find which of these the player root misses (whether the
alpha reaches SetAlpha at all, or the player's members count as settled), with a fade-root dump (`FadeRoots.cpp` keeps
amountEvents and per-root rows) in sky-native.

river-sky-noon stays out of shot_refs.

## References (refs.json)

- native-vulkan+native-interior: all four retaken (post-bugs-fix1-nvk-ni; the old ones held B1).
- Added to shot_refs and accepted: native-tour dragonsreach (post-bugs-ref-native-tour), cloud-shadows interior
  (post-bugs-ref2-cloud-shadows), cadence-drop-cells dragonsreach, native-river river-east, taa-water river-east, sky-native
  dragonsreach (post-bugs-ref-*). Each compared with the d3d11 run post-refs kept.
- Retaken whiterun: guest+map (post-bugs-ref-guest-map), native-d3d12+map (post-bugs-ref2-native-d3d12-map). The old D3D12 one
  failed the check (37 of 144 tiles) against a correct shot.
- cloud-shadows interior varies with the storm: one native run had the window light on the left wall and none on the floor
  (post-bugs-ref-cloud-shadows), the next matched d3d11 (accepted). The coarse check passes both.
- first-person-switch and llf-local-shadows keep their turned Dragonsreach shots (decided by the agent, to confirm: their
  references are valid, and turning back costs runs for no new coverage now that native-interior and native-tour check the
  hearth).

## Runs

| Config | Label | Result |
|---|---|---|
| native-vulkan+native-interior | post-bugs-fix1-nvk-ni | passed; B1 gone |
| native-vulkan+native-tour | post-bugs-ref-native-tour | passed |
| native-vulkan+cloud-shadows | post-bugs-ref-cloud-shadows, post-bugs-ref2-cloud-shadows | passed, passed |
| native-vulkan+cadence-drop-cells | post-bugs-ref-cadence-drop-cells | passed |
| native-vulkan+native-river | post-bugs-ref-native-river | passed |
| native-vulkan+taa-water | post-bugs-ref-taa-water | passed |
| native-vulkan+sky-native | post-bugs-ref-sky-native | passed (B3 still in river-sky-noon) |
| guest+map | post-bugs-ref-guest-map | passed (old whiterun reference at 32 of 33 tiles) |
| native-d3d12+map | post-bugs-ref-native-d3d12-map, post-bugs-ref2-native-d3d12-map | failed on the stale reference; passed without it |
| guest+sky-native | post-bugs-b3-guest-sky | passed; shows B3 in guest |
| native-vulkan+quick | post-bugs-final-nvk-quick | passed |
| guest+quick | post-bugs-final-guest-quick | passed (FrameValues is shared code) |

Skipped: native-d3d12-quick (no ORG, BasicRHI or device change; native-d3d12+map ran anyway).

Experiments (`post-bugs-exp*`, temporary configs and code, not committed) are listed under B1.

## Open items

- B3 (above): DCLF's fade of the player at the camera, guest and native.
- snap7-resizeui's per-render water frame row calls `Port::CaptureFrame` on the render thread, which still reads engine globals
  there (`kViewProjJittered`, and `ReadGame` when the snapshot has no GameBlock); the pool contract says a perRender pool reads
  no engine memory. Told snap7-resizeui; not changed here.

## Proposed decision-log entries

- Native's drawn-previous placement rule (repeated snapshots draw without motion) applies only to rows whose PreviousWorld is a
  transform; billboard rows keep their base, radius and face mode there (B1, edfc24580).
- B4's crash was the water capture's game half reading render state; snap7-resizeui's 195e7cc02 fixed it with the CameraPlanes
  crash.
- B3 is DCLF's in both graph modes, not native-only; left open.

- Turning Terrain Blending off in native (`features.TerrainBlending=false`) hit a fatal in menu scenes: "menu-shadow draw
  BladesHelmetGround:0: no Utility vertex shader for its technique (vs 0x1401a)" (post-bugs-exp8-nvk-notb), then a 25 s cold
  Lighting compile tripped the hang watchdog. Not looked at further.
