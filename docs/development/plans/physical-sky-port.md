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

DCLF reads Physical Sky's PS registers as frame textures (`DCLF/Integrations/PhysicalSky`), each with its view callback
(the export, null when the feature is off) and its published id, so a colour commit reads the graph resource ordered after its
producer:

| Register | Id |
| --- | --- |
| t61 | TransmittanceLut |
| t62 | SkyViewLut |
| t63 | ApLut |
| t64 | ApShadow |
| t66 | SkyTail |
| t67 | VolCubeAux |
| t69 | CloudShadowMean |
| t106 | DiskPhaseLut |
| t109 | LightningCubeLum |
| t110-t112 | VolTr, VolLum, ShadowVolume |
| t113 | LightningChannels |
| t114, t115 | VolCubeTr, VolCubeLum |

Each must be added to `Features/Manifest.cpp`'s `kFrameTextureRegisters`. None clashes with Core (t116-t127 are Core's or
DCLF's); t125-t127 are ViewAtmosphere's (port-viewatmos moves them behind overridable registers).

Samplers: the sky's s3/s4, grass's s7 and water's s13 are set in SetupGeometry hooks. The DCLF sky, grass and water families
capture samplers at the draw (`SlotSampler`), so the hooks keep working as long as they run before the capture. To check per
family. The sky VS's b6 swap is a constant buffer the families mirror through the shared constants already.

The `PHYSICAL_SKY` strip in `ShaderCache::GetCompileDefines` goes once every register above is registered. A register whose
producer is still on D3D11 (clouds before phase 4) is imported in guest mode and reads null in native mode, which the shaders
handle: `enableVolumetricClouds` gates every cloud read.

## 5. The composite and the fog

**Composite.** The ORG composite's push block is 26 words of 32. Physical Sky's inputs are seven (t16-t19, lightning t113,
which the composite also reads, the shadow volume and s1), and ViewAtmosphere adds three. They do not fit. So on the graph the
aerial perspective and cloud composite run as their own pass right after the composite (`cs.physical-sky.composite`,
`PhysicalSky/ApplyAerialPerspectiveCS.hlsl`). It reads kMAIN, decodes pre-exposure, applies `CompositeAerialPerspective` with
the same code as the D3D11 composite, and writes it back. The ORG composite is built without `PHYSICAL_SKY` (as now). Cost: one
more read and write of kMAIN and a second fp16 rounding of opaque pixels. ORGDeferredComposite gets one accessor for the
frame's main target index, and nothing else in its push block changes, so port-viewatmos's t125-t127 still fit.

**Fog.** The ORG fog's scattering programs are built without `PHYSICAL_SKY` today. With the LUTs and the AP shadow on the
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
   would also serve ViewAtmosphere and later features. Decide before more features join the composite.
2. **DynamicCubemaps off.** The D3D11 composite only binds depth with Dynamic Cubemaps loaded, so without it AP sees depth 0 and
   does nothing. The graph pass always reads the depth copy. Keep the difference, or match D3D11?
3. **Fog push block.** See section 5.
4. **Half-res AP shadow** changes the texture's use, not its size. Keep one full-size twin as D3D11 does.
5. **Native mode and engine draws.** In native mode engine draws read nothing, so until DCLF registers the frame textures the
   sky, grass and water lose Physical Sky's inputs. Phase 3 is therefore required before native runs with the feature on.

## 9. Status (2026-11-18)

- Phase 1 done: `ORGPhysicalSky` (`cs.physical-sky.luts`), LutGen and DiskPhaseLut dual-target (FXC bytecode unchanged).
- Phase 3 partly: DCLF registers t61-t63 and t106; the `PHYSICAL_SKY` strip is removed. t64 and the cloud registers are
  still captured. Removing the strip needs port-viewatmos's ViewAtmosphere register move, since DCLF's buffers use t125-t127.
- Native mode treats volumetric clouds as off (`PhysicalSky::VolumetricCloudsEnabled`) until phase 4.
- Not started: the AP shadow, the AP composite pass, the fog's inputs, clouds, lightning.
