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

## 6a. Volumetric clouds on the graph (phase 4 design, 2026-11-19)

**Handle table.** The cloud programs read up to about 60 of 146 resources, and the push block holds 32 words. So every
`Volumetrics.cs.hlsl` program reads its resource handles from a table. `AnchorPasses::Dispatch::table` (and `Draw::table`)
lists one entry per table field, in the shader's order. Srv, Uav and Published entries are declared as bindings are; Handle
entries (samplers, the null descriptor) carry their value. When the pass is recorded it writes its views' indices into a
dedicated buffer (`LatchBlock::CreateDedicated`) that the recording retains, and pushes that buffer as a LATCH entry
(`Bind::Table()`). The table is fixed for a recording and any replay of it, so it obeys the replay rule. A new buffer is made
only when a recording's indices differ from the last one's.

- `PhysicalSky/CloudTableList.hlsli` holds only `#define PHYSSKY_CLOUD_TABLE(X) X(name) ...`, every resource the
  `Volumetrics.cs.hlsl` include closure declares (its own and its headers', `SharedData`'s and `ViewAtmosphere`'s
  included). The C++ side includes the same file for its slot enum, so the two orders cannot drift.
- In the ORG build every declaration in that closure is `ORG_HEADER_SRV`, `ORG_HEADER_UAV` or `ORG_HEADER_SAMPLER` (FXC
  output unchanged), and `OrgHandle_<name>` is a static read from the table.
- Every cloud program has the same push block: the shared constants, the table, then the latches (cloud constants,
  dispatch rectangle, ambient volume, single storm, the AP shadow's light, and the per-frame structured data the D3D11 side
  uploads: storms, local NDF weights, voxel placements, lightning segments). One C++ helper fills it.
- An unbound entry is the null descriptor, as an unbound D3D11 register reads zero.

**Per-frame choices.** Every dispatch the D3D11 code may issue is a gated step registered once. The commit opens the steps
the frame takes, with their group counts. Shader variants are separate gated steps with the same declarations. The D3D11
pointer swaps become copies: reprojection writes the fixed current targets, and a copy at the end of the frame makes them
the next frame's history (the cube targets, the main view, the lightning masks, the ambient sources and layout). The voxel
and upper passes copy their traces back into the low-resolution targets. So declarations never depend on the frame, and
every reader reads one published id.

**Resources.** Every cloud texture is a `GraphResources` twin in the graph modes, named `PhysicalSky::<member>`, Persistent
or History, exported to D3D11 where guest D3D11 still binds it for engine draws. File textures are `ImmutableTexture`
imports (native: exempt resources). Generated maps (noise, NDF, local NDF, storm, voxel, middle and cirrus maps, cross
section, middle bounds) are gated or one-shot steps opened when their manager regenerates.

**Clients.** `cs.physical-sky.cloud-maps` (the generators) and `cs.physical-sky.clouds` (shadow volume and mean, ambient,
bounds, main view, cube, lightning) at Prepass, before `cs.physical-sky` (the AP shadow and sky tail read the shadow
volume and mean by id) and the fog. The D3D11 cloud dispatches never run in the graph modes: before the clients are
scheduled the outputs hold their neutral clears.

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
14. **Sky tail before ViewAtmosphere's medium (decided by the agent, to confirm).** The tail runs on the graph under the same
    condition as on D3D11 (`ViewAtmosphere` enabled and its composite on). In the graph modes `ORGViewAtmosphere::CompositeActive()`
    is off while Physical Sky is on (its plan, 3.4), so the tail does not run there yet, and nothing reads it: the medium says
    "no medium", so `SampleViewSky` uses the sky-view LUT. The pass is ready for port-viewatmos's shared volume. The multiscatter
    LUT it reads is published (`PhysicalSky::MultiscatterLut`, since phase 1), which view-atmosphere-port.md 3.6 lists as missing.
15. **The cloud diffuse transport (decided by the agent).** `Common.hlsli` declares `TexCloudDiffuseTransport` (t70) as a plain
    register. A graph pass that reads it defines `PS_CLOUD_TRANSPORT_EXTERN` and lists it as an EXTERN entry (the sky tail does);
    every other build keeps the plain declaration, so FXC output is unchanged. ViewAtmosphere does not publish the transport yet
    (its plan, 3.7), so the sky tail binds null, which it reads only with the clouds on.

16. **Middle and cirrus maps (decided by the agent, to confirm).** On the graph the last weather state always writes map 0, so
    the renderer reads one texture whatever the state count. With no states a zero-weight dispatch replaces D3D11's clear.
17. **Noise twins stay double-buffered (decided by the agent, to confirm).** The shape noise and adjustment LUT switch twins when
    a regeneration completes, which registers the renderer again. That happens only on noise setting edits.
18. **3D shape noise mips (decided by the agent, to confirm).** The graph has no 3D GenerateMips, so `CloudNoiseGenerate.cs.hlsl`
    has a new `generateShapeMip` entry (a 2x2x2 box filter) used only on the graph.
19. **File textures in guest mode (decided by the agent, to confirm).** 2D files are `ImmutableTexture` imports. 3D and array
    files are SRV-only twins filled once by a D3D11 `CopyResource`, since guest imports only 2D textures. Native mode makes them
    exempt textures (`Native::ImmutableTextureOf`).
20. **Table entries bound for a whole registration (decided by the agent, to confirm).** The middle bounds and upper sun depth are
    bound whenever the middle or cirrus inputs exist, where D3D11 binds them only on some frames; the shaders read them only under
    the same conditions. Main-view programs leave the height tiles (t54) null, which D3D11 binds only on rebuild frames.
21. **Boundary raster under dynamic resolution (decided by the agent, to confirm).** The graph's render area is the whole target,
    so the ORG vertex shader maps the frame onto the active low-resolution rectangle. Pixels outside it may get boundary values that
    D3D11's viewport clipped; nothing reads them.
22. **ViewAtmosphere's call (decided by the agent, to confirm).** In the graph modes the main view always runs from Physical Sky's
    Prepass with the depth copy, and ViewAtmosphere's D3D11 composite call to `RenderVolumetricClouds` returns at once. Its medium is
    off for the clouds' programs ("no medium").
23. **The AP composite's cloud inputs.** As the D3D11 composite binds them (t18, t19), the AP pass reads the reprojected targets
    before the filter (`PhysicalSky::CloudTr`, `CloudLum`), not the filtered ones engine draws read.
24. **Readers wait for the cloud client (decided by the agent).** Until `cs.physical-sky.clouds` first runs (its first run clears
    the outputs), the AP shadow, sky tail, AP composite, fog and DCLF bind no cloud outputs: native mode cannot read a backing nothing
    has written. The cloud renderer waits for its inputs, which take a few frames to generate.

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
- Phase 5 done (2026-11-19): the lightning query is a gated step of `cs.physical-sky.clouds` after the passes it reads, its
  candidates read back through `RenderGraph::RunReadback` (section 6). Built and run in all three graph modes; no lightning
  happened in the test weather, so the readback itself is not yet seen working in the game.
- Phase 6, Physical Sky's side: the sky tail (LutGen `SKY_TAIL`, dual-target, FXC unchanged) runs in
  `cs.physical-sky` at Prepass into `PhysicalSky::SkyTail`, a published History twin read by DCLF at t66 and the sky and water
  families by its id, and listed in `kPreviousFrame` (open questions 14 and 15). With the clouds on it reads their shadow volume
  and mean by id. The reflection faces read the LUTs, the AP shadow, the tail, the cloud cubes (t114, t115, t67) and the lightning
  reflection (t109) through DCLF's frame textures, as the previous frame's (`kPreviousFrame`). ViewAtmosphere's reflection medium (per-epoch views, decided in
  view-atmosphere-port.md open question 2) is port-viewatmos's.
- Phase 4 done (2026-11-19), design in section 6a. The clouds run on the graph in guest, native Vulkan and native D3D12, and
  native mode no longer treats them as off. What it took:
  - Handle tables (`AnchorPasses` `Dispatch::table`, `Bind::Table`): every `Volumetrics.cs.hlsl` program (47) reads its handles
    from a table the recording writes; the push block is 26 words. FXC output is unchanged for all of them.
  - `cs.physical-sky.cloud-maps` (`ORGCloudMaps`): the noise, NDF, middle, cirrus, local NDF and storm generators and the cross
    section, as gated steps the managers' updates open; file textures as imports. Its shaders are dual-target, FXC unchanged.
  - `cs.physical-sky.clouds` (`ORGVolumetricClouds`): both D3D11 passes as gated steps (shadow volume and mean, ambient, bounds,
    the boundary raster, main, voxel and upper views, reprojection, filter, lightning, cube); histories by copy; outputs published
    (`PhysicalSky::ShadowVolume`, `CloudShadowMean`, `VolTr`, `VolLum`, `VolAux`, `VolCubeTr`, `VolCubeLum`, `VolCubeDepth`,
    `LightningChannels`, `LightningReflection`, `CloudTr`, `CloudLum`), and listed in `kPreviousFrame` for the reflection faces.
  - The guest D3D11 fallbacks for the AP shadow, the fog's scattering and the sky tail are gone; they read the shadow volume and
    mean by id. The AP composite reads the cloud targets by id. DCLF reads every cloud output by id in both graph modes.
  - The graph modes compile none of the D3D11 cloud programs (FXC took minutes and nothing used them).
  - An OpenRenderGraph fix (`org-port/exempt-exit-commit`): an exempt texture read by several passes in one execution was
    committed without a seed ("Backing state was not seeded before commit").
  Tested 2026-11-19 at the autoload save (Whiterun exterior, clear weather), `<mode>+clouds`: guest, native Vulkan and native
  D3D12 pass with no tap, recipe or validation errors, and show the same cloud structure. The scene is washed out as Physical
  Sky is without Linear Lighting (dclf-open-defects.md). Not seen yet: storms, voxel clouds, local NDF, the middle and cirrus
  layers, lightning, reflections of the clouds in water.
