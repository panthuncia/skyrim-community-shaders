# P5.13 The flush and the shader binds

The engine's state flush and its shader binds on the way to latch-only ([org-full-port.md](../org-full-port.md) P5.13, R2 in
[engine-d3d11-surface.md](../engine-d3d11-surface.md)). Code: `src/RenderGraph/Engine/Flush/`. Switches:
`CS_ORG_RESIDUE_CENSUS=1` (the residual engine-draw census) and `CS_ORG_ENGINE_FLUSH_LATCH=1` (the latch-only flush). Both are
off by default, and both need `CS_ORG_TRIPWIRE_GUEST=1`. RE done in Ghidra on 2026-11-11, SE 1.5.97 and AE 1.6.1170; AE 1.7.104
is not checked.

## The four functions (RE)

The plan listed SE IDs only. The AE IDs were found from the call sites in `BSShader::BeginTechnique` (SE 101341, AE 108328) and
checked by decompiling both: each AE function is the same code as its SE twin.

| Function | SE | AE | CPU side | D3D11 side |
| --- | --- | --- | --- | --- |
| `BSGraphics::SetDirtyStates(bool isCompute)` | 75580 / 0x140d705b0 | 77386 / 0x140e4b5b0 | see below | see below |
| `Renderer::SetShader(Renderer*, VertexShader*)` | 75550 / 0x140d6f9b0 | 77351 / 0x140e4a6b0 | `RSS.flags \|= 0x400`, `RSS.currentVertexShader (+0x348) = vs` | `VSSetShader(vs->shader, 0, 0)`, a tail jump |
| `Renderer::SetPixelShader(Renderer*, PixelShader*)` | 75555 / 0x140d6fd60 | 77356 / 0x140e4aa60 | `RSS.currentPixelShader (+0x350) = ps` | `PSSetShader(ps ? ps->shader : null, 0, 0)`, a tail jump |
| `Renderer::DispatchCSShader(Renderer*, ComputeShader*, x, y, z)` | 75532 / 0x140d6ef90 | 77329 / 0x140e49cb0 | `SetDirtyStates(true)` | `CSSetShader(cs+0x60)`, `CSSetConstantBuffers(0, 1, &computeCB)` (SE 0x143027e80, AE 0x143288780), `Dispatch(x, y, z)`, a tail jump |

Callers (Ghidra cross references, both runtimes):

- `SetShader` and `SetPixelShader` have two code callers each: `BeginTechnique` (SE +0xC3/+0xD7 and a second `SetPixelShader`
  at SE +0xE2; AE +0xD5/+0xEB) and `BSMovie::DrawTiles` (SE 87907 +0xB7/+0xC7, AE 90279 +0xB6/+0xC6, the Bink tiles). CS
  already replaces the `BeginTechnique` calls with its own thunks (Hooks.cpp), which write the same shadow state and bind
  on DXVK's context; those thunks now bind through `Flush::BindVertexShader` / `BindPixelShader`. So the engine function
  is reached from Bink and from SE's second `BeginTechnique` site only. The tripwire's "shader binds (AE 90279)" lines are
  Bink's.
- `DispatchCSShader` has one code caller: `BSComputeShader::Dispatch` (SE 101641 +0x4C, AE 108723 +0x4C), plus a data
  reference. CS detours it twice already (CSShadersSupport in Hooks.cpp, ImageSpace's batch flush).
- R1's "FUN_1410a2370, a tiled texture/screenshot renderer" is `BSMovie::DrawTiles`.

**SetDirtyStates**, both runtimes the same code. What it writes besides D3D11:

| Step | CPU side (kept by the latch) | D3D11 side (dropped by the latch) |
| --- | --- | --- |
| bit 0x1 | each pending target clear (`setRenderTargetMode[i] == 0`) is issued, then the mode is set to 4; the cube face's likewise (+0x6C); depth modes 0..2 and 6 clear `RendererData+0x22` (read-only depth); a depth clear sets the depth mode to 4 | `ClearRenderTargetView` / `ClearDepthStencilView` (kept: the skeleton's call sites take or forward them), `OMSetRenderTargets` |
| bits 0xC | | `OMSetDepthStencilState(table[depth*0x28 + stencil], ref)` |
| bits 0x1070 | with 0x40: the viewport's Min/MaxDepth reset from the saved pair (RSS +0x5C0/+0x5C4), `MaxDepth -= biasOffset[bias]`, bit 0x2 set | `RSSetState(table[...])` |
| bit 0x2 | | `RSSetViewports(1, &RSS.viewPort)` |
| bit 0x80 | | `OMSetBlendState(table[...], factor, 0xffffffff)` |
| bits 0x300 | | `Map(alphaTestCB, WRITE_DISCARD)`, one float, `Unmap` (kept: a buffer upload, which the constants capture records) |
| bit 0x400 (not with isCompute) | the input layout for `vs->vertexDesc & RSS.vertexDesc`: looked up in the cache, or made with `device->CreateInputLayout` and inserted | `IASetInputLayout` |
| bit 0x800 | | `IASetPrimitiveTopology` |
| last | the flag word set to 0 (or `flags & 0x400` with isCompute); the CS UAV, PS sampler, PS resource, CS sampler and CS resource modified words cleared bit by bit | `CSSetUnorderedAccessViews`, `PSSetSamplers`, `PSSetShaderResources`, `CSSetSamplers`, `CSSetShaderResources`, one slot per bit |

Every draw function (R1) latches `RSS.vertexDesc` and topology, calls `SetDirtyStates(false)`, then binds its index and vertex
buffers and draws with a plain `call` (checked in both runtimes, so the draw's return address is inside the draw function). AE
77275 is SE 75488 with its call to 75489 inlined, so AE has one more draw site.

## The census (`CS_ORG_RESIDUE_CENSUS=1`)

The guest context asks a filter (`Tripwire::SetContextFilter`) before it forwards any engine call. The census counts there every
`Draw*` and `Dispatch*` the engine (or another plugin through the engine's context) issues: what reaches it has passed DCLF's
withholding and every family's taker, so a withheld draw is never counted. CS's own draws on `globals::d3d::context` do not go
through the guest and are not counted.

Each draw is classed by:

- the context method and the draw function it came from (the Address Library ID of its return address; DispatchCSShader for a
  dispatch, whose tail jump hides it);
- the three game frames above it, and the first CS frame between them (DCLF's native draws call the engine's draw function from
  CS code);
- the last BSShader technique (`State::currentShader`: type, file name or ImageSpace shader name) and the descriptors of the
  bound shaders (`RSS.currentVertexShader->id`, `currentPixelShader->id`);
- the view: RSS render target 0 (+0x18), the depth stencil (+0x38) and the cube target (+0x40).

It also counts every `SetDirtyStates` call by caller (empty ones, ones from CS code) and every vertex and pixel shader bind, and
which of them no draw followed on the same thread before the next one of the same kind.

Every `CS_ORG_RESIDUE_CENSUS_FRAMES` main-thread frames (default 600; the scenario uses 300) it logs, per frame of that window:

```text
[ORG] residue census: <n> frames, <z> with no engine draw reaching D3D11: <d> engine draws and dispatches a frame reach D3D11 (DrawIndexed .., Dispatch ..); since the start <total> over <frames> frames, <zero> frames with none
[ORG] residue census: by draw function: DrawTriShape (75477/77263) .., ...
[ORG] residue census: by last technique: Lighting .., ImageSpace .., ...
[ORG] residue census: by view: RT kMAIN DS kMAIN .., ...
[ORG] residue census: flushes .. a frame (.. with nothing to apply, .. from CS code, .. followed by no draw); vertex shader binds .. (..); pixel shader binds .. (..)
[ORG] residue census: draw #1: .. a frame, DrawIndexed in DrawTriShape (75477/77263) at SkyrimSE.exe+0x.. (ID 77263+0x..) <- ... (via CS ...); Lighting "Lighting" vs 0x.. ps 0x..; RT .. DS ..
[ORG] residue census: flush with no draw #1: .. a frame of .. at ...
[ORG] residue census: vertex shader bind with no draw #1: ...
```

`CS_ORG_RESIDUE_CENSUS_TOP` sets how many entries each list shows (default 25). The census walks the stack once per draw
(`RtlCaptureStackBackTrace`), so frame times with it on mean nothing.

## The latch (`CS_ORG_ENGINE_FLUSH_LATCH=1`)

The four functions are detoured (`Flush.cpp`; SetDirtyStates outside Hooks.cpp's detour, DispatchCSShader inside ImageSpace's
and Hooks.cpp's). With the latch on:

- **The engine's code still runs.** Each detour calls the original inside a latch scope (thread-local). In the scope the guest
  context drops every `IASet*`, `VSSet*`, `PSSet*`, `GSSet*`, `HSSet*`, `DSSet*`, `CSSet*`, `OMSet*`, `RSSet*` and `SOSet*`
  call and the dispatch. Everything else still goes through: `Map`/`Unmap` (the alpha-test constant), clears (the skeleton's
  call sites take or forward them), queries. So the shadow state, the dirty and modified bits, the deferred clear modes, the
  viewport depth tweak and the input-layout cache come out exactly as without the latch, and captures that read the shadow
  state keep working.
- **What a latched flush cleared is owed.** The detour keeps the six words it consumed (flags, PS resources, PS samplers, CS
  resources, CS samplers, CS UAVs) and the shaders the latched binds would have bound.
- **A flush CS asks for is real.** A `SetDirtyStates` call whose return address is in CS's own image (DCLF's
  `ApplyPendingState`, Grass Optimizations' grass draws, ImageSpace's withholding) first puts the owed bits back and binds the
  owed vertex and pixel shaders on DXVK's context, then runs as usual. These callers read the D3D11 state back or draw with it.
- **An engine draw that still reaches D3D11 is skipped.** A `Draw*` from one of the BSGraphics draw functions (the R1 list,
  SE 75477-75500 and AE 77263-77290, with AE 77275) or a dispatch inside `DispatchCSShader` would run with state that was never
  bound, so the guest context drops it and logs the site once, as an `[E]` line:
  `[ORG] flush latch: engine DrawIndexed in DrawTriShape (75477/77263) at SkyrimSE.exe+0x.. (ID ..) <- ... reached D3D11 with the engine's state latched: skipped`.
  A draw from elsewhere in the game (Scaleform's renderer, which binds its own state) is forwarded; the census shows it.
- Every `CS_ORG_RESIDUE_CENSUS_FRAMES` frames: `[ORG] flush latch: N frames: .. flushes and .. shader binds a frame latched,
  .. D3D11 binds a frame dropped, .. flushes a frame from CS code applied the owed state, .. engine draws and dispatches skipped`.
- `CS_ORG_ENGINE_FLUSH_LATCH_KEEP=PSSetShaderResources,OMSetRenderTargets,...` names binds that still reach D3D11 inside the
  latched functions, to find which reader below still needs which bind.
- The compute shader and `CSSetConstantBuffers(0)` of a latched dispatch are not owed: nothing in CS dispatches with the
  engine's compute state.

Nothing in this lane registers a graph client, so it causes no graph rebuild.

### Readers of the D3D11 state

CS code that asks D3D11 what the engine bound, outside a flush it asked for, reads stale state under the latch. These have to
read the shadow state (`RSS.PSTexture[]` +0x140, `RSS.renderTargets[]` with the renderer's view tables, `RSS.viewPort` +0x70)
or a published graph resource before the latch can be the default. From `grep` on 2026-11-11:

| Reader | Reads | When |
| --- | --- | --- |
| DCLF `Core/Engine/Family/EngineDrawFamily.cpp` `ResolveHeader` | `PSGetShaderResources(register)` | every family's texture headers |
| DCLF `Core/Render/Resources/FrameRecords.cpp` | `PSGetShaderResources` | frame records |
| DCLF `Core/Render/Resources/FrameSlots.cpp` `CaptureBindings` | PS t0.., `OMGetRenderTargets`, `RSGetViewports` | after `ApplyPendingState` (a real flush, so correct) and wherever else it is called |
| DCLF `Features/MainView/MainViewPoints.cpp` | `OMGetRenderTargets` | the main view's points |
| DCLF `Features/Water/WaterCapture.cpp` | PS directional shadow register | water capture |
| `Skylighting::CaptureShadowCascadeSRV`, `VolumetricShadows`, `ExponentialHeightFog::CaptureDirectionalShadowMap` | PS t4 (the shadow cascades) | from `State::Draw`, right after the engine's flush |
| `CloudShadows.cpp` | `OMGetRenderTargets` | the reflection faces |
| `RenderGraph/Engine/Scaleform/Renderer.cpp` | `OMGetRenderTargets` | the Scaleform renderer's target |

The state backups around CS's own D3D11 passes (Effects11 `D3D11StateBackup`, `Globals.cpp`, HDRDisplay, PostProcessing's
raster pass, the screenshot, ImGui, GPU timers) save and restore whatever is bound and need no engine state.

## What reaching zero needs

The census is the measure. The latch becomes the default only when, over a full tour (`p5-all-census.env`):

1. the census reads zero: every window's `<z> with no engine draw reaching D3D11` equals its frame count, and "flushes from CS
   code" is zero or only callers that read the shadow state;
2. a `p5-all-latch.env` run logs no `[ORG] flush latch: engine ... skipped` line, and its screenshots match `p5-all.env`;
3. the readers above read the shadow state or a graph resource.

Then `SetShader` and `SetPixelShader` can drop their D3D11 call for good (Bink's DrawTiles is their only engine caller once
Bink is on the graph), `SetDirtyStates` keeps only its CPU side, and the guest context's binds become tripwires.

## The census of 2026-11-11

Run `1111-072658-p35-p5-all-census-w3` (`p5-all-census.env`, AE, the p5-all tour: Whiterun, inventory, map, journal, a save
and its load, Blackreach, the race menu). No window had a frame without engine draws reaching D3D11. Per frame: about 95 in
Whiterun's exterior (DrawIndexed 86, Dispatch 9), 199 in the first window, 157 in Blackreach, 180 in the race menu. Flushes:
100-230 a frame, 2 from CS code, about 25 followed by no draw. Vertex and pixel shader binds: 60-100 each, about 28 followed
by no draw, all from `BeginTechnique` (AE 108328): techniques set up for passes that DCLF or a family then withheld. The latch
removes those for free.

Almost every draw comes through `BSBatchRenderer::RenderPassImmediately` (100854 / 107644) and the geometry-type dispatcher
(100847 / 107637: +0x1A2 TriShape, +0x41E the LOD multi-index shapes' alternate index buffer, +0x650 instanced, +0x101 dynamic
and particle shapes) into the R1 draw functions. The caller above those names the subsystem. Callers were named from CS's own
relocations and Ghidra (AE IDs; the SE ID where CS already uses one). The technique column is the last `BeginTechnique`'s,
so it is stale for compute dispatches.

| # | Class (callers above the draw function) | Technique, view | Per frame | Who takes it |
| --- | --- | --- | --- | --- |
| 1 | `BSShaderAccumulator::RenderPersistentPassList` (100840 / 107630), geometry group 13 | Sky techniques 5/6 (CloudsLerp, CloudsFade); the reflection faces (DS `kCUBEMAP_REFLECTIONS`) 15-35, the main view 18 | 35-55 | P5.3: the cloud layers on the graph (`CS_ORG_SKY_CLOUDS`, off in p5-all until its texture fault is fixed; sky.md "The cloud layers are not in the sky batch") |
| 2 | `BSImagespaceShader::Render` (100950 / 107732) and its explicit-target variant (AE 107733, called by the ImageSpaceManager effect runner AE 107193 and AE 0x1414d21b0) | SAO: ISSAOCameraZ, ISMinify for every camera-Z mip, ISSAOBlurH/V, ISSAOCompositeFog; ISDownsampleIgnoreBrightest, ISMinifyContrast, ISWaterBlend, ReflectionsRayTracing (SSR), DistantBlur, Refraction, RadialBlurHigh; the HDR chain (DownSample4, DownSample16Lum, BrightPassBlur15, Blur15, TonemapBlendCinematic, DownSample4RGB2Lum) in the windows where it was not taken | 25-30 | P5.6: the rows imagespace.md marks Next (SSAO, water blend, reflections, DOF and blurs, refraction, radial blur), and the HDR chain's refusals |
| 3 | `BSImagespaceShader::DispatchComputeShader` (100952 / 107734) from the separable compute blur pair (AE 107209 / 107210, through AE 107208 and AE 107207) and `DispatchCopySubRegion` (AE 107217, from `Main::RenderWorld` AE 107142) | HDRDownSample4's blur, the SSR blur (targets 0x6E, 0x6F, 0x6D), ISSAOBlurV's CopySubRegionCS (117) | 4-7 | P5.6's compute form (imagespace.md: latch the dispatch in CSShadersSupport, CS b0 from the capture) |
| 4 | Volumetric lighting: AE 107023 (SE 100309), from `Main::Draw` (AE 36559), dispatching directly (+0x31C, +0x48B) and through its blur (AE 107235) | stale (Sky, BSISWaterDisplacementTexOffset); noise, generate, raymarch, blur | 2-3 | P5.6's VL row (with CS's ISVolumetricLighting* replacements) |
| 5 | `BSBatchRenderer::RenderBatches` (100852 / 107642): batches DCLF does not take | Utility (depth prepass: DS `kMAIN`, no RT) up to 20, Lighting up to 11, DistantTree instanced 7 (through a CS hook at CommunityShaders.dll+0x278130d), Sky 3, shadow maps (DS `kSHADOWMAPS`) 7-11 in Whiterun | 20-60 | P5.2: DCLF residue (unclaimed shapes, the alternate index buffer at +0x41E, instanced DistantTree, residue shadow casters) |
| 6 | Water through `RenderBatches`: RT `kRAW_WATER` and `kREFRACTION_NORMALS` (DS `kMAIN`), dispatcher +0x1A2 and +0x4CE | Water | 3-8 | P5.4: the raw-water and refraction-normal passes |
| 7 | AE 107673 (SE 100877, the pass LLF hooks as RenderPass1) and the dispatcher's dynamic shapes (77275 via +0x101) | Particle up to 10, Effect up to 5 | 0-15 | P5.2: particle and effect families |
| 8 | Skinned partitions: `NiSkinPartition::Unk_25` (AE 71151, through AE 71150) and `BSDismemberSkinInstance::Unk_25` (AE 70776), called from `RenderPassImmediately`+0x1DF, drawing each partition with DrawTriShape or DrawDynamicTriShape (77276) | Effect up to 14 (effect shaders on actors), Utility 3 | 3-17 | P5.2: skinned Effect and Utility passes (DCLF takes the Lighting ones) |
| 9 | The shadow-mask pass (AE 107141, between `Main::RenderDepth` and `Main::RenderWorld`): each shadow-casting light into `kSHADOW_MASK` | Utility | 1-2.5 | No owner yet: a new P5 item (Volumetric Shadows and Screen Space Shadows read the mask) |
| 10 | Menu 3D: `UI3DSceneManager::Render` (AE 52727) through AE 52728 | Utility | 1.7 in the inventory | P5.8: the utility pass it does not take |
| 11 | Bink: `BSMovie::DrawTiles` (87907 / 90279) from `BSMoviePlayer::Tick` (AE 90259) in `PlayBlocking` (AE 90256) | Bink's tiles | 1 frame per movie | P5.11 / P5.12: the startup logo plays on a thread that is not the render owner ("a movie plays on thread ..., not the render thread: the engine draws it"), and the first frame while the tile program builds |
| 12 | Scaleform: `DrawBitmaps` (82835 / 84956) and AE 84961 | | 0.1 | P5.10: the renderer's remaining engine paths |
| 13 | Another plugin: `zz_SkyrimEngineTelemetry.dll`+0xB9097 (a `Draw`) | | 0.1 | Outside CS: org-extension-api-notes.md |

The flushes from CS code (about 2 a frame) are DCLF's `ApplyPendingState` before its captures. They stay real under the latch,
but their readers belong in the list above ("Readers of the D3D11 state").

**The latch run** (`1111-073039-p35-p5-all-latch-w3`): no device loss; 22 000 to 650 000 engine draws skipped per 300 frames;
every screenshot broken (83-99 % of pixels differ), as expected while classes 1-12 and the D3D11-state readers remain. Its
`[E]` sites: DrawInstancedTriShape (77265) from the dispatcher +0x650, the alternate index buffer (77264) from +0x41E,
DrawTriShape (77263) from Bink's DrawTiles +0x146, DrawDynamicTriShape (77275) from +0x101, and the VL blur's dispatch. That
build logged one line per draw-function site, so DrawTriShape's other callers went unnamed; the latch now logs once per site
and caller chain.

## The census after wave 3 (2026-11-11, `1111-131713-p35-p5-all-census-misc-fin`)

`p5-all-census-misc.env` with the wave 3 fixes (shadow masks, family inputs, ImageSpace's end-of-frame effects). Settled Whiterun
reads 40 engine draws a frame, down from about 95; the load windows read 80-205 (DCLF's objects not yet claimed, by design).

| Class (settled Whiterun) | Per frame | Owner |
| --- | --- | --- |
| Sky cloud layers into the reflection cube (15.3) and `kMAIN` (8) | 23.3 | `CS_ORG_SKY_CLOUDS`, off in the scenario: needs its verification run |
| Sky, descriptor 0, through AE 106604 +0x131, into `kMAIN` with `kMAIN` and with `kPOST_ZPREPASS_COPY` | 2 | Not identified yet (the sky family's scope) |
| Utility RENDER_NORMAL into `kREFRACTION_NORMALS` | 3 | DCLF's refraction class |
| Effect through the particle draw (+0x101): master particle systems | 3 | `CS_DCLF_MASTER_PARTICLES`, off by default |
| ImageSpace: DistantBlur (`kFRAMEBUFFER`), HDRDownSample4 into `kLDR_DOWNSAMPLE0`, ISWaterBlend, ISSAOCompositeFog, ISApplyVolumetricLighting (two draws, two compute blurs) | 7 | P5.6 |
| Volumetric lighting's dispatches (AE 107023 +0x31C, +0x48B) | 2 | P5.6's VL row |

Elsewhere: Blackreach's precipitation particles (10 a frame, no port), the map and journal windows' Utility (16-20, the menu
scenes' and the map's own views, P5.8/P5.9), Scaleform's `DrawBitmaps` paths (AE 84956, 84961: 2-5 a frame), and
`zz_SkyrimEngineTelemetry.dll` (7, another plugin). Flushes: 89 a frame, 49 followed by no draw; shader binds 63 each, 49
followed by no draw (all from `BeginTechnique`).

## DCLF's share

The census of 2026-11-11 (`1111-072658-p35-p5-all-census-w3`, `p5-all-census.env`) read for what Drawcall Limit Fix leaves the
engine: the batches through `BSBatchRenderer::RenderBatches` (AE 107642), the grouped alphas (AE 107673), the skin path (AE 71150,
70776) and the particle draw (`DrawDynamicTriShape (DrawData copy)` from the dispatcher's +0x101, geometry type 1: NiParticles).
That run had no DCLF census and listed 25 classes a window, so some causes below are read from the code and Ghidra, and the
scenarios below name the rest. A "load window" is the first 300-frame window after `coc`, a save's load or `cow`; a "settled
window" is the next.

| Class (residue census) | Per frame | Why the engine draws it | State |
| --- | --- | --- | --- |
| Lighting (RT `kMAIN`), Utility depth (DS `kMAIN`, vs `0x2000`, `0x209b`, `0x400209b`), shadow casters (DS `kSHADOWMAPS`), Effect in the grouped alphas (AE 107673), through `RenderBatches` | 15-45 in load windows, 0 settled | Objects not yet members: the engine draws an object until the publication that claims it is installed (dclf-architecture.md, "Load screens" and "Pass capture") | By design. What would shorten it: claims for a load's objects in the first frame their draws are ready. Measure the delay with `CS_DCLF_STATS` (`[DCLF] event to adoption`) in `p5-all-census-dclf.env` |
| Instanced DistantTree (dispatcher +0x650, via CS's tree LOD hook) | 4-7 in the first world window only | Tree LOD before its gates and family are ready after the first load | By design, as above |
| The alternate index buffer (77264, dispatcher +0x41E) | 0.2, one load window | A multi-index shape's layer pass (hint 12) before its base is a member (`CS_DCLF_LAYERS`) | By design, as above |
| Effect through the particle draw (+0x101) | about 3 settled (Whiterun) | The master particle systems (`MPS*`: chimney smoke, candles, torches): `effect: untracked, eligible` in DCLF's census | Built behind `CS_DCLF_MASTER_PARTICLES=1` (default off, dclf-roadmap.md 1.4: needs the unheld roots diff run before it is the default) |
| Effect through the skin path (AE 71150: vs `0x44b`, `0x55b`, `0x4a`) | 3 (dog eyes), 7.5-13.7 and 1.2-2.8 in Blackreach | `effect-variant (skinned)`: the Effect port read Skinned from the renderer data's vertex description, which a skinned shape's is not | Fixed on `org-port/w3-dclf` (`GeometryRecord::shapeVertexDesc`, BSGeometry +0x148 as `FUN_1414bde90` reads it). Should be 0 in `p5-all-census-dclf.env` |
| Effect on a rigid skin (a skin instance, no Skinned bit in the pass) | not seen in this run | `effect-variant (skinned)` | Built behind `CS_DCLF_EFFECT_RIGID_SKINS=1` (default off until its shots are checked) |
| Particle (`BSParticleShader`, vs `0x4`) through the grouped alphas | about 10 settled (Blackreach) | Precipitation (Blackreach's spores): not a DCLF class | Blocked: no precipitation port (dclf-roadmap.md 1.5; DCLF draws only the precipitation occlusion mask) |
| Skinned Utility depth and sun shadows (vs `0x2004`, `0xc004`) through AE 70776 | 3-4 in the map and journal windows | The player's first-person parts (`lighting: hidden` in main depth and a shadow view): DCLF's verdict is Hidden, the engine still draws them there | Blocked: why the engine draws hidden first-person parts into main depth and the sun's shadows behind a menu is not read (RenderDepth's first-person block; Features/FirstPerson covers its own accumulator only) |
| Utility into `kSHADOW_MASK` (AE 107141 +0x1C4/+0x346: the world's; 52728 +0x1E5: a menu scene's, the inventory's 1.7) | 1-2.5 settled, 1.7 in the inventory | Not DCLF's: each shadow-casting light's pass over the screen quad | Built behind `CS_ORG_SHADOW_MASK=1` (p5/shadow-mask.md; `org-port/w3-misc`). Should be 0 in `p5-all-census-misc.env` and `shadow-mask-org1.env` |
| Water (vs `0x4904`, `0x31e`, `0x4906`) into `kRAW_WATER` and `kMAIN` (DS `kMAIN`, through 107637 +0x1A2 and +0x4CE) | 3-8 | The water family's surface view refused every batch: a material slot held a published engine target (the refraction copy, the SSR targets, which the ImageSpace port's reads publish). Later the mask view too: its published inputs changed after its client registered | Fixed on `org-port/w3-misc` (p5/water.md, "Published material slots"): the slot is read by its id, declared by the client. Should be 0 in `p5-all-census-misc.env` and `water-org1-imagespace.env`. It was not (the inputs changed after the clients registered); `org-port/fix-family-inputs` fixes the inputs at registration (p5/water.md, "Published inputs") |
| Utility RENDER_NORMAL (vs `0x61b`, ps `0x203`: FALLOFF, VC, NORMALS, BINORMAL_TANGENT) into `kREFRACTION_NORMALS` (DS `kMAIN`) | 1-3 (Whiterun, Dragonsreach) | Not the water's: Lighting objects' refraction pass (kRefraction or kTempRefraction, hint 4, group 5, which FUN_1414b4860 binds `kREFRACTION_NORMALS` for). BSEffectShaderProperty::GetRenderPasses (AE 0x1414bd890) makes no Utility pass, so these are Lighting geometry DCLF's refraction class (`CS_DCLF_REFRACTION`, on by default) did not claim | Open: DCLF's census (`p5-all-census-dclf.env`) names the objects and the reason (`refraction` among the classes); likely load-window objects or a variant the one refraction pipeline (Utility `0x20A`) does not take |

Scenarios (`tools/cs-ab/scenarios/`): `p5-all-census-dclf.env` (both censuses, `CS_ORG_RESIDUE_CENSUS_TOP=60`),
`p5-all-census-dclf-take.env` (plus `CS_DCLF_MASTER_PARTICLES=1` and `CS_DCLF_EFFECT_RIGID_SKINS=1`) and `dclf-residue-census.env`
(DCLF without the graph families). Against the census above, `p5-all-census-dclf.env` should lose every skinned `Effect` class
through AE 71150 and the census class `effect: effect-variant (skinned)`; `-take.env` should also lose the settled windows'
particle `Effect` draws and `effect: untracked, eligible` for `MPS*` systems, with screenshots matching. Nothing here registers a
graph client, so nothing in it can trigger a graph rebuild.

## What is left

The census classes of `1111-072658-p35-p5-all-census-w3` that are not DCLF's, and where each stands:

| Class | Per frame | State |
| --- | --- | --- |
| Utility into `kSHADOW_MASK` (AE 107141, 52728) | 1-2.5, 1.7 in the inventory | `CS_ORG_SHADOW_MASK=1` (p5/shadow-mask.md), both the world's and a menu scene's |
| Water into `kRAW_WATER` and `kMAIN` | 3-8 | The water family's published material slots (p5/water.md) |
| Utility into `kREFRACTION_NORMALS` | 1-3 | DCLF's refraction class (above): not the water's |
| Menu 3D's Utility in the inventory (AE 52728 under UI3DSceneManager::Render) | 1.7 | The menu scene's shadow masks: `CS_ORG_SHADOW_MASK=1`. The menu model's own Lighting draws are P5.8's view (p5/menus.md, not written) |
| Bink's startup logo, frame 1 of 166 | | The logo's first tick claims rendering and builds the joining client's graph at its start (p5/bink.md, "The first frame") |
| ImageSpace (Refraction, ISWaterBlend, the SAO chain, the water displacement's dispatches, the HDR chain in menus) | | P5.6's lanes (p5/imagespace.md) |
| Sky into the reflection cube (DS `kCUBEMAP_REFLECTIONS`) | 15-55 | P5.3 (p5/sky.md) |

Scenario: `p5-all-census-misc.env` (p5-all-census with `CS_ORG_SHADOW_MASK=1`, 60 entries a list). It needs `org-port/w3-slots`'s frame
slot budget (EngineDraws runs five times a frame).
