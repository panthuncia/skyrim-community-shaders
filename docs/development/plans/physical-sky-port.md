# Physical Sky on the render graph

Plan for porting upstream's Physical Sky (#3157, `8e6e5ba10`, sync batch 16) to the ORG paths (guest and native). Written
2026-11-18. The D3D11 feature stays as it is for plain D3D11 mode (`CS_ORG=0`). The ORG side lives in
`src/Features/PhysicalSky/ORGPhysicalSky.{h,cpp}`, with one seam per D3D11 entry point in the old feature, as the other ports
do.

## 1. What the feature does today (D3D11)

| Hook | Work | Output | Readers |
| --- | --- | --- | --- |
| EarlyPrepass | disk phase LUT (`DiskPhaseLut.cs`, only when its 12 constants change) | `texDiskPhaseLut` 2048x3 RGBA32F | LUT gen, PS t106, clouds |
| EarlyPrepass | transmittance, multiscatter, sky-view, AP LUTs (`LutGen.cs`, `LUTGEN` 0-3) | `texTrLut` 256x64, `texMsLut` 32x32, `texSvLut` 200x150 (RGBA16F), `texApLut` 32x32x32 3D | PS t61-t63, composite t16, fog CS t61-t63, clouds |
| Prepass | cloud noise (`CloudNoiseGenerate.cs`), NDF/storm/local NDF/cirrus/mid-cloud maps, cross section | noise volumes, NDF maps | clouds |
| Prepass | cloud shadow volume (`Volumetrics.cs`, pass `kShadowVolume`), shadow mean | `texShadowVolume` t112 / composite t20, `texCloudShadowMean` t69 | PS, fog, AP shadow |
| Prepass | sky tail (`LutGen.cs` `SKY_TAIL`, only with the view-atmosphere composite) | `texSkyTail` t66 | sky, view atmosphere |
| Prepass | AP shadow (`ShadowAccum.cs`, full or half res), only without the prepared view atmosphere | `texApShadow` R8 screen | PS t64, composite t17, fog t64 |
| Prepass | volumetric clouds main view and cubemap (`kMainViewAndCubemap`): ambient SH, ambient transport, height bounds, trace, reproject, filter, history, cube trace and reproject, lightning channels and reflection | `texVol{Tr,Lum,Aux}` t110/t111 / composite t18/t19, cube t114/t115/t67, lightning t113/t109 | sky, Lighting, water, grass, composite |
| ReflectionsPrepass | binds t61-t63, t106, t109, t112, t69, t114/t115/t67 for the reflection faces | | the faces' draws |
| Prepass end | PS binds t61-t64, t66, t69, t106, t109-t115 | | every engine draw that has the define |
| BSSky/Grass/Water SetupGeometry | VS b6 for the sky, samplers s3/s4 (sky), s7 (grass), s13 (water) | | engine draws |
| Composite | t16-t20 and s1 under `PHYSICAL_SKY` (D3D11 composite only) | AP on opaque pixels | |
| Lightning | a 1-dispatch candidate query, `CopyResource` into a staging buffer, `Map(DO_NOT_WAIT)` some frames later | the flash source | CPU |

`HasShaderDefine` is true for every shader type, so every engine family compiles with `PHYSICAL_SKY`.
`ShaderCache::GetCompileDefines` strips the define from every render-graph build today. The port ends with that strip removed.

## 2. Anchors and stages

The recipe (`src/RenderGraph/Frame/RecipeTable.h`) names every client at its step, so each new client id is added there.

| Client | Anchor (stage) | Passes, in order |
| --- | --- | --- |
| `cs.physical-sky.luts` | EarlyPrepass (pre-depth) | disk phase (gated, settings change), Tr, Ms, Sv, Ap (gated, every enabled frame) |
| `cs.physical-sky` | Prepass (pre-main) | cloud noise and maps (one-shot or gated on change), shadow volume, shadow mean, sky tail, AP shadow, volumetric clouds main view, cube, lightning |
| `cs.physical-sky.composite` | DeferredComposite (deferred+water), after `cs.deferred` | aerial perspective and cloud composite onto kMAIN |

EarlyPrepass runs after DCLF's shadow views and before the Z-prepass: the LUTs only need the frame's constants. The AP shadow
needs `kPOST_ZPREPASS_COPY`, the directional shadow map and its cascades' data, so it stays at Prepass, after DepthDone, as on
D3D11. The reflection faces (start stage) run before EarlyPrepass, so they read the previous frame's LUTs and cube clouds. They
did on D3D11 too for the cube clouds; for the LUTs this is a one-frame lag that is invisible in practice.

The fog (`cs.exp-height-fog`) is in the same Prepass step as `cs.physical-sky`. It reads the AP shadow and the shadow volume by
published id, so the graph orders it after their writes.

## 3. Resources

All are `GraphResources` twins, published by id, exported to D3D11 (SRV) while engine draws or other D3D11 features read them in
guest mode. Exported means never aliased.

| Id | Shape | Lifetime | Notes |
| --- | --- | --- | --- |
| `PhysicalSky::DiskPhaseLut` | 2048x3 RGBA32F | Persistent | rewritten when its constants change |
| `PhysicalSky::TransmittanceLut` | 256x64 RGBA16F | History | rewritten each enabled frame, read by the next frame's faces |
| `PhysicalSky::MultiscatterLut` | 32x32 RGBA16F | History | only the Sv/Ap LUTs and sky tail read it; unexported |
| `PhysicalSky::SkyViewLut` | 200x150 RGBA16F | History | |
| `PhysicalSky::ApLut` | 32x32x32 RGBA16F 3D | History | |
| `PhysicalSky::SkyTail` | 200x150 RGBA16F | History | view atmosphere composite only |
| `PhysicalSky::ApShadow` | screen R8 | History (read by next-frame faces) | `RequestSize` on a resolution change |
| `PhysicalSky::ShadowVolume`, `ShadowVolumeRaw` | 3D R16F | History | clouds |
| `PhysicalSky::CloudShadowMean` | 2D | History | clouds |
| cloud trace / filter targets (low and full res) | 2D | Transient where written-then-read in one frame | `texVolLow*`, `texVolFiltered*` |
| cloud histories (`texVolHistory*`, `texVolCubeHistory*`, lightning histories) | 2D / cube | History | ping-pong by copy at the end of the pass, as D3D11 |
| cloud results `texVol{Tr,Lum,Aux}`, `texVolCube{Tr,Lum,Aux}`, `texLightningChannels`, `texLightningCubeLum` | 2D / cube | History | sampled by the next frame's faces and by this frame's draws |
| noise and NDF maps | 3D/2D | Persistent | written on a settings or weather change (one-shot passes, `Rearm`) |
| file textures (noise, profile LUTs) | | | `FrameAnchors::ImmutableTexture` |

Samplers are `ShaderBindings::CreateSampler` from the D3D11 descriptions (`sampTr` clamp, `sampSv` wrap-U, `sampNoise` wrap).

Per-frame constants: the LUT and AP shadow shaders read `SharedData::physSkyData` through the shared constants
(`ClientDesc::sharedConstants`). The disk phase constants and the cloud constants (`volCloudCb`, `cloudDispatchCb`,
`cloudAmbientVolumeCB`) become latch regions written at the commit. Group counts and on/off go through one run gate.

## 4. Engine draws and DCLF families

Engine draws in guest mode keep reading the D3D11 binds (`PSSetShaderResources` at Prepass), which now name the exports. In
graph mode those binds are the only D3D11 work left, and they are state only.

Every program that compiles with `PHYSICAL_SKY` falls in one of four groups. Each reads the same registers from the same source.

| Group | Programs | How Physical Sky's resources are bound |
| --- | --- | --- |
| DCLF families | Lighting, Effect, grass, distant tree, pulled and not | frame textures (`DCLF/Integrations/PhysicalSky`), in `Features/Manifest.cpp`'s `kFrameTextureRegisters` |
| Engine draw families | sky, water | the draw's latch (`PhysSkyOrgHandles`, `Common/PhysicalSkyOrgHandles.hlsli`), resolved per batch |
| Menu scenes | Lighting, Effect ORG builds | none: the build undefines `PHYSICAL_SKY` with the other world features |
| ImageSpace | `ISSAOComposite.hlsl` (fog) | the transmittance LUT as a published read (`FeatureBinds`), the rest `UNREAD` |

Physical Sky's header declares its pixel resources with `ORG_HEADER_SRV` and the sky's samplers with `ORG_HEADER_SAMPLER`. FXC
and DCLF builds keep the registers. An ORG build with `ORG_HEADER_RESOURCES` reads `OrgHandle_<name>`, so a program that forgets
one fails to compile.

| Register | Name | Source in guest mode | Source in native mode |
| --- | --- | --- | --- |
| t61, t62, t63, t106 | TrLut, SvLut, ApLut, DiskPhaseLut | the LUT twin's export, read by its published id | the same |
| t64 | ApShadow | the D3D11 texture the feature binds | zero 2D view |
| t66 | SkyTail | the same | zero 2D view |
| t67 | VolCubeDepth | the same | zero cube view |
| t69 | MeanProfile (cloud shadow mean) | the same | zero 2D view |
| t109 | LightningReflection | the same | zero cube view |
| t110, t111 | VolTr, VolLum | the same | zero 2D views |
| t112 | ShadowVolume | the same | zero 3D view |
| t113 | LightningChannels | the same | zero 2D view |
| t114, t115 | VolCubeTr, VolCubeLum | the same | zero cube views |
| t105 (DCLF only) | the view medium's cloud depth (D3D11 t116) | `texVolFilteredAux` with clouds on | zero 2D view |
| s3, s4 (sky), s13 (water) | SampTr, SampSv, ApSampler | ORGPhysicalSky's twins of `sampTr` and `sampSv` | the same |

The feature off gives zero views of the declared kind everywhere. A zero view means no contribution to every reader: the AP
shadow multiplies by `1 - shadow`, the shadow mean's validity flag is `w`, and `enableVolumetricClouds` gates every cloud read.
Native mode reports clouds as off (`VolumetricCloudsEnabled`) until phase 4.

"What the feature binds" is read the way each group reads any D3D11 input: DCLF keeps the colour commit's capture
(`capturedWhenNull`), and the engine draw families read the bound view with `Family::ResolveHeader`.

Collisions checked:

- DCLF's pixel object rows are t116. The view medium's cloud depth (`Common/ViewMedium.hlsli`) moved to t105 in DCLF builds.
- t70: Wetness Effects' precipitation occlusion (`TexPrecipOcclusion`) and Physical Sky's `TexCloudDiffuseTransport`. Only
  compute shaders read the transport (ViewAtmosphere's LightScattering and Reflection, LutGen); pixel shaders declare it unread.
  No clash.
- t125-t127: ViewAtmosphere's, moved to t104, t107 and t108 in DCLF builds by port-viewatmos.

Grass's s7 (`sampTileable`, volumetric cloud shadows only) is still set by the D3D11 SetupGeometry hook. Clouds are off in
native mode, so only guest mode reads it, through the grass family's sampler capture.

## 5. The composite and the fog

**Composite.** The ORG composite's push block is 26 words of 32. Physical Sky's inputs are seven (t16-t19, lightning t113,
which the composite also reads, the shadow volume and s1), and ViewAtmosphere adds three. They do not fit. So on the graph the
aerial perspective and cloud composite run as their own pass right after the composite (`cs.physical-sky.composite`,
`PhysicalSky/ApplyAerialPerspectiveCS.hlsl`). It reads kMAIN, decodes pre-exposure, applies `CompositeAerialPerspective` with
the same code as the D3D11 composite, and writes it back. The ORG composite is built without `PHYSICAL_SKY` (as now). Cost: one
more read and write of kMAIN and a second fp16 rounding of opaque pixels. ORGDeferredComposite gets one accessor for the
frame's main target index, and nothing else in its push block changes, so port-viewatmos's t125-t127 still fit.

As built (phase 2): the pass's push block is 12 words: the shared handles, kMAIN, the depth copy, the AP LUT, the AP shadow,
the cloud transmittance and luminance, the lightning channels and SampSv. `ORGDeferredComposite::GraphComposite()` gives the
frame's main target and whether it is an interior. The pass is gated: it dispatches only when the graph composite writes kMAIN
this frame (the D3D11 composite applies the AP itself on the other frames), outside interiors (the D3D11 interior composite is
built without `PHYSICAL_SKY`), and while ViewAtmosphere has not prepared its medium.

**Fog (phase 2: it fits).** The scattering shader reads three of Physical Sky's resources: TexTrLut t61, the shadow volume
t112 and its mean t69 (the AP LUT and AP shadow are not read). Its push block was 29 words with Light Limit Fix's tail, so the
three EXTERN entries make it exactly 32. The rest of Physical Sky's header is UNREAD. The ORG build adds `PHYSICAL_SKY` when the
feature is loaded, as the D3D11 build does. The LUT is read by its published id. The shadow volume and its mean are D3D11's
until phase 4, so a guest frame with the clouds on runs the fog on D3D11 ("Physical Sky's cloud shadows are on D3D11"), and
otherwise they are handle 0, which the shader never reads with the clouds off. The original text follows.

The ORG fog's scattering programs are built without `PHYSICAL_SKY` today. With the LUTs and the AP shadow on the
graph, the scattering pass adds `PHYSICAL_SKY` and four entries (TexTrLut t61, TexApLut t63 ... as the shader reads them, the shadow
volume t112, the shadow mean t69) as published reads. Its push block size decides whether they fit; if not, the same
indirection as the composite (an extra pass) does not apply, so the fog needs a handle table (open question 3).

## 6. Lightning readback

D3D11 maps a staging copy with `DO_NOT_WAIT` and retries on later frames. On the graph:

- The query is a gated dispatch in `cs.physical-sky` that writes the candidates into a graph buffer, followed by a copy into a
  readback `org::Buffer` from a small ring (one per frame in flight, `kGameFramesInFlight`).
- The commit records which ring entry the run filled. The `executed` callback stores the run's completion point.
- Each frame, `Lightning::Update` asks the ring whether that entry's GPU work has completed (`GraphHost` completed value, a
  non-blocking query). Only then is the buffer read on the CPU. Nothing waits. A query that is not complete is read on a later
  frame, as `DO_NOT_WAIT` does today.

**As decided in phase 5 (2026-11-19).** A recording must be replayable, since a revision-driven epoch submits it again. So the
ring is `RenderGraph::RunReadback` (`src/RenderGraph/RunReadback.h`), shared with Post Processing's readbacks (commit
`76e94d43e`'s pattern, moved out of `ORGPostProcessing.cpp`; `ORGPostChain::RunReadback` now wraps it). It keeps one host
region per run lease. Each run copies a stamp from its latch and the candidates into its lease's region. The region is read
when the lease's next run is committed, since that run is done by then. No completed-value query and no wait.

The lightning query reads the cloud renderer's resources (`findLightningSource` in `Volumetrics.cs.hlsl`, the NDF maps, noise
and storm data). It can be a graph pass only once those are graph resources, so it lands with phase 4. The steps then:

1. ORGPhysicalSky makes `RunReadback::Create("PhysicalSky::LightningCandidates", { SkyLightning::kCandidates * 16 })` once,
   and a graph buffer twin for `SkyLightning::candidates` (UAV u4, 32 float4).
2. At each build of `cs.physical-sky`: `Bind(latch, stampOffset)` with 16 bytes of the client's latch, then the query as a
   gated dispatch (1x1x1) after the cloud passes it reads, then `BufferPass(0, candidates)`.
3. `SkyLightning::QuerySource` on the graph only asks for the query (pendingPhase becomes Readback). The client's commit opens
   the query's gate entry, calls `Commit(run)` and writes the Stamp. The executed callback calls `Executed()`.
4. `SkyLightning::ReadSource` on the graph calls `Take(0)`. Empty means not read yet (as `DXGI_ERROR_WAS_STILL_DRAWING`). The
   first candidate with `w > 0` is the source, as on D3D11.

## 7. Order of work

1. **LUTs** (this branch): disk phase, Tr, Ms, Sv, Ap on `cs.physical-sky.luts`; twins exported for D3D11 readers and
   published. LutGen and DiskPhaseLut become dual-target.
2. **AP shadow and composite**: ShadowAccum dual-target at Prepass (inputs: depth copy, Deferred's shadow map import, the
   directional shadow data in a latch as Skylighting does, Terrain Shadows' published height map, Cloud Shadows' last layer
   import, the shadow volume or a null view); the AP apply pass after the composite; the fog's inputs.
3. **DCLF bindings**: the integration with the frame textures above; remove the `PHYSICAL_SKY` strip.
4. **Volumetric clouds**: the noise and map generators as one-shot passes; the shadow volume and mean; the main view, filter,
   history and cube passes; `Volumetrics.cs` is 1,757 lines and many includes, so it becomes dual-target last. Cloud boundary is a
   raster pass (`RasterProgram`).
5. **Lightning**: the readback ring above.
6. **Sky tail** and the reflection bindings, with port-viewatmos.

## 8. Open questions

1. **Composite split.** The separate AP pass is a choice forced by the 32-word push block. The alternative is a handle table for
   the composite (a latch of descriptor indices written at the commit from `PublishedResources::ShaderResourceIndex`), which
   would also serve ViewAtmosphere and later features. Decided for now: the separate pass (6).
2. **DynamicCubemaps off.** The D3D11 composite only binds depth with Dynamic Cubemaps loaded, so without it AP sees depth 0 and
   does nothing. Decided: match D3D11 (7).
3. **Fog push block.** See section 5.
4. **Half-res AP shadow** changes the texture's use, not its size. Keep one full-size twin as D3D11 does.
5. **Native mode and engine draws.** In native mode engine draws read nothing, so until DCLF registers the frame textures the
   sky, grass and water lose Physical Sky's inputs. Phase 3 is therefore required before native runs with the feature on.
   Done (section 4).
6. **The AP pass's cost (decided 2026-11-19, phase 2).** Option (a): the aerial perspective runs as its own pass right after
   `cs.deferred` (`cs.physical-sky.composite`), reading and writing kMAIN; `cs.deferred`'s push block does not grow. It costs one
   extra full-screen pass and a second fp16 rounding of opaque pixels. That is a known minor cost, to revisit with a handle table.
7. **AP depth (decided 2026-11-19).** Keep upstream: the AP reads depth only when Dynamic Cubemaps is loaded, as the D3D11
   composite binds it. Without Dynamic Cubemaps the AP does nothing. That looks like an upstream bug, or only the cubemap path
   needs the view. Not changed here.
8. **ViewAtmosphere in the AP pass.** t125-t127 stay out of the AP pass for now ("no medium"), as agreed with port-viewatmos.
   The pass is skipped while `ViewAtmosphere::IsPrepared()` (D3D11's medium, where the shader's `Enabled()` would return early).
   In the graph modes ViewAtmosphere prepares nothing, so the pass always runs. Revisit when port-viewatmos ports the medium.
9. **AP depth without Dynamic Cubemaps (decided by the agent, to confirm).** The pass binds the null descriptor for depth, as
   the D3D11 composite's unbound t4 reads 0. Then every pixel passes `depth < 1`: the AP is near zero (near-plane distance), but
   the cloud composite (`CompositeVolumetricClouds`, by pixel) runs on every pixel, sky included. That matches upstream D3D11.
10. **When the AP composite runs (decided by the agent, to confirm).** Its commit opens the gate only when
    `ORGDeferredComposite::GraphComposite()` says the graph composite writes this frame's main target, the frame is not an
    interior, and `FrameAnchors::HeldNow("cs.deferred", DeferredComposite).scheduled` (a lagging composite writes nothing).
    The commit order of the two clients is not fixed, so it does not read the composite's own commit.
11. **AP shadow light data (decided by the agent).** The directional shadow light is in the pass's latch, not a structured
    buffer copy as the fog does. `PhysicalSky/ApShadow.hlsli` reads it under ORG through `ShadowLightCount()`/`ShadowLight()`,
    loading the column-major matrices by hand (the bytes D3D11's buffer holds). A clear flag after the count replaces the D3D11
    map-menu clear. The rewrite of `SampleCascadeShadow`'s guard (an `||` of equality tests, the AMD rule) changes the DXBC of
    `Volumetrics.cs.hlsl`'s main view (behaviour-preserving); `ShadowAccum` and the fog's DXBC are unchanged.
12. **AP shadow size (decided by the agent).** The twin is not versioned. It is made at SetupResources at kMAIN's size, as
    D3D11 makes it, so a resolution change needs what it needs on D3D11.
13. **D3D11 inputs in guest mode (decided by the agent, to confirm).** Until phase 4 the cloud shadow volume and mean are
    D3D11 3D/2D textures. A guest frame with the clouds on runs the AP shadow and the fog's scattering on D3D11. The AP
    composite imports the cloud targets (`texVolTr`, `texVolLum`, `texLightningChannels`) as D3D11 textures. Native mode has
    the clouds off, so these are null there. Terrain Shadows' heights and Cloud Shadows' last layer are read by their published
    ids; a frame where either is not the graph's runs the AP shadow on D3D11.

## 9. Status (2026-11-19)

- Phase 1 done: `ORGPhysicalSky` (`cs.physical-sky.luts`), LutGen and DiskPhaseLut dual-target (FXC bytecode unchanged).
- Phase 3 done (section 4): every Physical Sky pixel register and sampler is bound in every graph program that compiles with
  `PHYSICAL_SKY`, and the `PHYSICAL_SKY` strip in `ShaderCache::GetCompileDefines` is removed. Checked with DXC (SPIR-V and DXIL)
  for the menu scenes, sky, water, SAO composite and DCLF Lighting and Effect builds: no register outside the layouts. FXC
  output is unchanged for every touched shader. Not yet run in the game.
- Phase 2 done (not yet run in the game):
  - `cs.physical-sky` at Prepass: ShadowAccum dual-target (FXC unchanged), full and half resolution entries, a one-shot clear
    of the twin, the light in a latch (open question 11). `PhysicalSky::ApShadow` is a published History twin, exported for
    D3D11, read by DCLF at t64 and by the sky and water families by its id, and listed in `kPreviousFrame` for the faces.
  - `cs.physical-sky.composite` at DeferredComposite after `cs.deferred`: `PhysicalSky/ApplyAerialPerspectiveCS.hlsl`
    (section 5, open questions 9 and 10).
  - The fog's scattering: `PHYSICAL_SKY` with TexTrLut, the shadow volume and its mean (section 5). The push block is full.
- Phase 5, as far as it goes without phase 4: the replayable readback is shared (`RenderGraph::RunReadback`), and the lightning
  steps are written down (section 6). The query reads the clouds' resources, so the query pass and its readback land with
  phase 4. Until then the lightning query stays on D3D11 (guest mode, with the clouds) and native mode has no lightning, since
  `SkyLightning::Update` requires `VolumetricCloudsEnabled()`.
- Native mode treats volumetric clouds as off (`PhysicalSky::VolumetricCloudsEnabled`) until phase 4.
- Not started: clouds, the lightning query pass, sky tail and reflection bindings.
