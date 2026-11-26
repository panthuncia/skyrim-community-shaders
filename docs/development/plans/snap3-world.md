# snap3-world: native screen blood, RenderWorld, the resize hang (phase 3, milestone 4)

Lead: `world`, branch `snap3/world` (base `org_integration` 78a03f1d7), stage `s3-world`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md); decisions 8, 33-48, 66-70, 87-98 of
[render-thread-decisions.md](render-thread-decisions.md); starting point `snap3-culls.md` "Not done".

| Item | State | Commit |
|---|---|---|
| 1. Native screen blood from game data (observe, then switch) | done | c46c9625b |
| 2. Main::RenderWorld replaced in native | not done: inventory below | |
| 3. DCLF GPU hang after a resize (decision 94) | not fixed: reproduced reliably, narrowed to Light Limit Fix; next steps below | |
| 4. Precipitation, decal occlusion, VL vanilla, cube cameras | not started | |

## 1. Native screen blood (`Features/BloodSplatter/BloodSplatterNative.cpp`)

**Game side** (`CaptureBlock`, native world snapshots, `Snapshot::BlockId::ScreenBlood`): calls RenderWorld's light flare setup
(SE 100269, AE 106979: arithmetic on the world camera and the sun, writes only globals the blood shader reads), then copies LightLoc,
the flare scale, the flare's HDR target index, and ScreenSplatter::Update's Ctrl and alpha scales into the block.

**Render side** (`CollectDraws`, `BuildRun`): at Main::Draw's call of RenderScreenSplatter (SE 35560 +0xA6A, AE 36559 +0xA73, made
when ScreenSplatter's root has children) native walks ScreenSplatter's root (hidden subtrees skipped), takes each
BSBloodSplatterShaderProperty geometry, and makes the draw GetRenderPasses, SetupTechnique and SetupGeometry would: the flare pass
when the property has a flare texture, else the splatter pass; flares first, then splatters (RenderBatches' technique order).
WorldViewProj from the geometry's world and SetCameraData's matrices for ScreenSplatter's camera (`ShadowCameras::CameraMatrices`);
alpha = *alphaSource x property alpha x the block's scales; t0/t1 the splatter's colour and alpha, t2 the flare texture, t3 the
adapted luminance (an `Engine::kHDR_DOWNSAMPLEn` target, read by its published id: the client lists kHDR_DOWNSAMPLE1-13 in native).
CLAMP/LINEAR samplers, blend modes 4 and 5, the whole back buffer as viewport. The run goes to the existing `cs.blood-splatter`
client at the EngineDraws anchor. RenderWorld's splatter cull (SE +0x3D2, AE +0x3FE) and SubmitAccumulator (SE +0x3E4, AE +0x410)
do nothing in native. Guest keeps the latched batch.

**Parity** (`s3-world-bloodpar1`, scaffolding removed): 600 batches, 4,488 draws compared field by field against the latched engine
draws: techniques, geometry, WorldViewProj, LightLoc, Ctrl, alpha and all four views identical; the only differences were the
order of draws inside one technique (the engine's cull order reverses each node's children pairwise). Both blends commute
(multiply and add), so the order was not modelled.

**Found on the way:**
- Before this, native drew no screen blood at all: every batch the hits made in the test was flares, whose t3 is a published graph
  texture, which the latch refused ("the engine draws", and the engine's draws are dropped in native). Guest shows the same
  refusal (`s3-world-bloodguest1`: no blood visible); guest is out of scope and unchanged.
- The latched viewport is the shadow state's (CS's render resolution, 1280x720 with upscaling at 1080p), which clipped the blood to
  the top-left part of the screen (`s3-world-bloodsw1`). The native pass uses the display size.

**Detector** (native Vulkan, screen-blood, per world frame): engine render calls 55.3 (`s3-world-blood0`) and 55.7
(`s3-world-bloodsw1`); the splatter's `BSCullingProcess::Process` and `SubmitAccumulator` sites and AE 107147 now show `hooked`
(the engine still calls the sites, CS's thunks do nothing), so the count moves only when RenderWorld is replaced. The engine's
FinishAccumulatingScreenSplatter, RenderBatches and BSBloodSplatterShader setup are no longer entered in native.

**Config:** `tools/cs-test/configs/screen-blood.toml` (health raised, splatters made to last, a melee bandit at the player, two
shots looking down). Expect `the first screen blood run drawn by the graph`; a streamed texture's first refusal is normal.

## 2. Main::RenderWorld in native: not done

The call-order inventory of every CS hook inside RenderWorld in native is in this lead's session (summarised here); it confirms
snap3-culls' list and adds what each hook reads at that moment:
0. Deferred's `Main_RenderWorld` (inWorld, ExtraShaderDescriptor bit) around the call; FrameAnnotations' perf event.
1. `Sun::UpdateOcclusionQueries`: GetQueryData is detoured to the graph's readback.
2. StartAccumulating's SetCameraData (no CS hook) and the main accumulator's camera and eye (SkyNative reads `eyePosition`).
3. Pre-resolve (AE 106583): `Main_RenderWorld_Start` (+0x84: StartDeferred reads shadow-state RT0-3, PrepassPasses, the Prepass
   anchor, DCLF BeforeOpaquePass which calls SetDirtyStates and reads OMGetRenderTargets), DCLF `SkyBatch` (+0x237, needs the main
   accumulator, `deferredPass`, shadow-state RTs and t17), `Main_RenderWorld_BlendedDecals` (+0x308: AfterOpaquePass, terrain
   blending, EndDeferred -> DeferredPasses, DeferredComposite, PostComposite), the Skeleton's FinishAccumulating depth copy
   (+0x39D), `MaskPassesCheck` (+0x3B4), the stencil clear (+0x3C9), `WaterBatch<Mask>` (+0x492), ApplyState's Skeleton clears
   (+0x556), RenderReflections' detour (+0x55B, calls SetCameraData itself).
4. RenderSSAO #1, snow or the kMAIN copy (ImageSpace's ShaderRender/DispatchComputeShader detours tag them by `snowWindow`),
   RenderSSAO #2 (detours, global 415071 set to 0/1 by RenderWorld).
5. Post-resolve (AE 106584): `Accumulator_BlendedDraw<0>` (+0x51, under water), the EngineCopy (native ContextCopyResource),
   `WaterBatch<Surface>` (+0x143, +0x161), WaterBlend's detour (+0x1BE), `PostWater_SetCamera` (+0x239, ViewAtmosphere's
   CompositeAfterWater), ApplyState clears (+0x4B6), `Accumulator_BlendedDraw<1>` (+0x543: the deferred+water stage's TapPoint,
   `blended.draw`, the forward epoch).
6. The flare setup (now game side), the splatter cull and submit (now nothing), the camera-jump globals the full-screen blur's
   camera test reads, `Sun::SubmitOcclusionQuery` -> DCLF's SunOcclusionQuery detour (needs shadow-state RT0 = kMAIN).

Why it was not done in this round: most of these CS points read engine state that RenderWorld's own calls set just before them
(the shadow state's render targets from SetRenderTarget, the accumulator, SetCameraData's camera data, global 415071), and each is
written as a thunk around the engine call. Replacing the body means turning each into a CS function that takes those values from
CS's frame, then calling them in this order; that is several days of careful work with an observe phase per point. Decided by the
agent, to confirm: item 3 (a device loss) was taken before item 2 once item 2's size was clear.

## 3. The resize hang: narrowed, not fixed

- **Reliable repro:** six shrink/grow cycles in one run (`window-resize-stress`, 1920x1080 <-> 1280x720, 90 frames each) loses the device
  within two cycles: `s3-world-wrstress1` (FSR) and `s3-world-wrstress-taa1` (TAA) both lost it. Plain `window-resize` passed 4 of 4
  on this build (`s3-world-wr-base1..4`), so one resize is a poor test.
- **Not FSR:** TAA hangs too, so Streamline's view destruction on resize (decision 94) is a separate defect.
- **Light Limit Fix is required:** the same stress with `LightLimitFix = false` passed all twelve resizes
  (`s3-world-wrstress-nollf1`).
- **Not DCLF's hand-over alone:** the FSR run hung inside the window (DCLF still on its old-size scene), the TAA run 0.2 s after
  DCLF adopted its new resources. Crash markers differ (`cs.dclf.main-opaque` then `cs.dclf.cull-counters` in flight; the TAA run's
  last finished marker is `cs.shadow-mask.world.draws`). Both are within a few frames of the revision that adopts LLF's resized
  Clusters/LightGrid/LightIndexList versions; the old versions are released over the next ten frames.
- **Validation runs** (snap5's) showed render areas of old builds on new targets only in revision prep recordings (cs.upscaling.*,
  cs.pp.output.*), which are re-recorded on the staged builds before adoption (the clients declare DisplaySize, so the revision
  needs their builds); nothing in DCLF's passes.
- **Leading hypothesis, unproven:** a light loop in a colour shader (Lighting/Effect/Grass/Water: `lightGrid[clusterIndex].lightCount`)
  reads a light grid through a stale bindless index: a run whose latch was not rewritten after the adoption (a replayed slot) still
  names the replaced LightGrid's descriptor, whose slot or memory now holds something else, so the count is garbage and the loop
  runs until the TDR. With LLF off there is no such loop. Next steps: (a) a debug build that checks, at each colour commit and
  replay, that the latched t35-t37 indices equal `PublishedIndices` of the adopted versions; (b) GPU-assisted validation with the
  stress config; (c) if confirmed, rewrite the frame-record indices for every run after an adoption, or hold the replaced versions
  until no latched run names them. Clamping the loop count in the shaders would hide it and was not done.

## Runs (stage `s3-world`, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+screen-blood, detector (base) | `s3-world-blood0` | blood not drawn (flares refused); 55.3 render calls |
| native-vulkan+screen-blood, parity | `s3-world-bloodpar1` | 4,488 draws, identical but for order inside a technique |
| native-vulkan+screen-blood, switch, detector | `s3-world-bloodsw1` | passed; blood drawn, clipped to the render viewport; 55.7 |
| native-vulkan+screen-blood, display viewport | `s3-world-bloodsw2` | passed; blood over the whole screen |
| guest+screen-blood | `s3-world-bloodguest1` | ran; guest unchanged (its flares are refused as before) |
| native-vulkan+window-resize x4 (base) | `s3-world-wr-base1..4` | all passed |
| window-resize-stress, FSR / TAA / LLF off | `s3-world-wrstress1`, `-taa1`, `-nollf1` | device lost, device lost, passed |
| guest+quick | `s3-world-f-guest-quick` | passed |
| native-vulkan+quick, detector | `s3-world-f-nvk-quick` | passed; 65 engine render calls/world frame (snap3-culls 63.0; pre/post resolve vary with the scene) |
| native-d3d12-quick | `s3-world-f-d3d12-quick` | passed |
| native-vulkan+cadence-drop, -drop-cells, -repeat | `s3-world-f-drop`, `-drop-cells`, `-repeat` | all passed |
| native-vulkan+shadows-blend | `s3-world-f-shadows-blend` | passed |
| native-interior, detector | `s3-world-f-interior` | passed; 48 render calls (47 before) |
| native-river | `s3-world-f-river` | passed |
| native-vulkan+post-effects | `s3-world-f-post-effects` | passed |
| native-vulkan+screen-blood | `s3-world-f-blood` | passed |
| native-vulkan+window-resize x5 | `s3-world-f-wr1..5` | all passed (but window-resize-stress still loses the device: not fixed) |
| native-tour | `s3-world-f-tour` | passed |
| cadence.py, native-vulkan+cadence-still | `s3-world-f-cad-*` | all four passed; exterior repeat 16.6 / drop 14.5 against a floor of 3.4 ("outside noise", the same sizes as snap3-culls' 16.5 / 14.3: wind and pose), interior 3.5 / 0.3 within noise |

## Decisions (agent, to confirm)

- **WO-a.** The light flare setup (AE 106979) runs on the game thread in the capture: pure arithmetic on the camera and the sun
  whose outputs only the blood shader reads (decision 8). Native only.
- **WO-b.** The screen blood geometry, its property and ScreenSplatter's camera are read on the render thread from the scene graph,
  as the sky root and water shapes are (decision 90); phase 6/7 moves them into the snapshot.
- **WO-c.** Draw order inside a technique is ScreenSplatter's child order, not the engine cull's; both blends commute.
- **WO-d.** The native blood uses the display size as its viewport (the latch used CS's render-resolution viewport).
- **WO-e.** Item 3 before item 2 (above).

## Open items

- Item 2 (above), item 3's root cause (above), item 4.
- Guest's screen blood flares are refused (published t3) and draw nothing; out of scope here.
- SE is untested (every new address is an SE/AE pair; the SE offsets come from the SE disassembly).
