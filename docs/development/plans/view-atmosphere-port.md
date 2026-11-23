# Atmosphere and Fog (ViewAtmosphere) on the render graph

Status 2026-11-23 (shared volume 3.6 and items 4 and 5 done, not yet run in the game). First written 2026-11-18. Upstream #3157 (`8e6e5ba10`) added ViewAtmosphere as a D3D11-only CORE feature. This note says how it
moves onto the graph modes (guest and native), what is done, and what waits on the Physical Sky port.

## 1. What upstream does

| Work | D3D11 site | Reads | Writes |
|---|---|---|---|
| `Prepare` | `Deferred::PrepassPasses`, before the features' Prepass | settings, screen size | sizes the atlas (`Source`, `History`, `Luminance`, `Transmittance`, RGBA16F, three grids side by side, at most 8 M texels), the parameters buffer |
| `Render`: inject (`LightScattering.cs`, 3 grids) | after the Prepass anchor | history, directional shadow map + t98, Physical Sky LUTs (Tr, Ms), shadow volume, cloud shadow mean, cloud diffuse transport, IBL, Skylighting probes, LLF lights + local shadows + engine shadow maps (b1), terrain heights, cloud layer, cubemap | `Source` |
| `Render`: integrate (`Integrate.cs`, 3 grids) | same | `Source`, Physical Sky medium coefficients, lightning, Skylighting | `Luminance`, `Transmittance` |
| `Bind` | after `Render` and in the D3D11 composite | | PS/CS t125 luminance, t126 transmittance, t127 parameters |
| `RenderReflection` (`Reflection.cs`: sky SH 3x2, then a 32^3 omnidirectional medium) | after the ReflectionsPrepass anchor, once per cubemap render | sky LUTs, cloud cubes, IBL, probes, terrain, clouds | `ReflectionSkySH`, reflection luminance and transmittance, then PS t125-t127 and t66 for the faces |
| `CompositeAfterWater` (`Composite.cs`) | a new hook on the camera-set call after the water (SE 99939 / AE 106584, FinishAccumulatingPostResolveDepth) | kMAIN depth, Physical Sky clouds and lightning, the medium, EHF's volumes, IBL, cubemap | kMAIN, in place |

`SharedData::PostWaterComposite` (State) is `CompositeEnabled()`. With it set, Lighting, Effect, Water, Sky and the SSAO
composite skip their own fog, aerial perspective and clouds, and the post-water composite applies the medium to everything
drawn before it. Forward draws after it apply the medium themselves (`ViewMedium::SampleViewMedium`).

While the shared volume is prepared it replaces Exponential Height Fog's volumes (`ExponentialHeightFog::Prepass` returns
early). The shared volume needs Physical Sky on; the composite needs Physical Sky or the fog.

## 2. What was wrong in the graph modes before this port

1. **D3D11 work inside graph modes.** `Prepare`/`Render`/`RenderReflection`/`CompositeAfterWater` all ran D3D11 dispatches.
   In native mode they reach nothing, so with `PostWaterComposite` set the opaque world had no fog at all.
2. **Fog replaced by D3D11 work.** With Physical Sky on, `Prepare` succeeded on D3D11 and turned the ORG fog off.
3. **Register collision.** DCLF's `DCLF_BINDLESS` builds own t125 (`DCLFCharacterLightNoise`), t126 (`DCLFExtras`) and t127
   (`DCLFObjects`, `DrawPipelines.h`). `Common/ViewAtmosphere.hlsli` declared t125-t127 too, so in every DCLF Lighting or
   Effect build with `EXP_HEIGHT_FOG` (or `PHYSICAL_SKY`), `ViewAtmosphere::Enabled()` and `ReflectionEnabled()` read DCLF's
   object rows as the parameters. Garbage `flags` could switch the shared-volume path on in forward draws and reflections.

## 3. Design

Rules kept: errors fatal, the render thread never waits, versions only at frame boundaries, one path for guest and native, no
inline rebuilds, no D3D11 work in graph modes.

### 3.1 The seam

`ViewAtmosphere` keeps its D3D11 path for plain D3D11 mode. In the graph modes (`ORGViewAtmosphere::Requested()`, i.e.
`Native::GraphMode()`):

- `Prepare` calls `ORGViewAtmosphere::Prepass()`, and `IsPrepared()` and `IsReady()` answer
  `ORGViewAtmosphere::SharedVolumeActive()` (3.6): whether the adopted revision runs the shared volume.
- `RenderReflection`, `Unbind`, `Bind` and `Render` do nothing.
- `CompositeEnabled()` answers `ORGViewAtmosphere::CompositeActive()`, so `PostWaterComposite` follows the graph composite.
- `CompositeAfterWater()` runs the graph composite.
- `SetupResources` / `PostPostLoad` call `Setup` / `Reserve`.

### 3.2 The medium on the graph (client `cs.view-atmosphere`, Prepass anchor, pre-main stage)

Resources, made at Setup and kept for the session:

| Id | Kind | Lifetime | Export | Now |
|---|---|---|---|---|
| `ViewAtmosphere::Luminance` | Texture3D RGBA16F, versioned | Persistent | SRV | 1x1x1, zero |
| `ViewAtmosphere::Transmittance` | Texture3D RGBA16F, versioned | Persistent | SRV | 1x1x1, one |
| `ViewAtmosphere::Parameters` | StructuredBuffer, 1 x 256 B | Persistent | SRV | zero every frame |
| `ViewAtmosphere::NoFog` | Texture3D, not published | Persistent | none | (0,0,0,1) for a fog that is off |

All published. Passes: a per-run copy of the parameters from the client's latch (the commit writes them), and the shared volume's
passes while it is on (3.6). All-zero parameters mean "no medium" (`flags.x == 0`). The atlas is a declared texture on the grid
settings and the display size, with the shared volume as its existence rule, so a quality change or the volume switching is a new
version at a frame boundary and never a build.

**Shader side** (`Common/ViewAtmosphere.hlsli`), one header for all builds:

- ORG pass that opts in (`VIEW_ATMOSPHERE_ORG_HANDLES` with `ORG_HEADER_RESOURCES`): EXTERN handles `ViewAtmosphereLuminance`,
  `ViewAtmosphereTransmittance`, `ViewAtmosphereData`.
- Any other ORG pass with `ORG_HEADER_RESOURCES` (the menu-scene Lighting/Effect builds, the ORG deferred composite): no
  medium. `Enabled()` and `ReflectionEnabled()` are constant false and the reads compile away.
- `DCLF_BINDLESS`: t104, t107, t108, which DCLF's new integration (`Integrations/ViewAtmosphere`) fills as frame textures from
  the published ids through their exports. This fixes the collision in 2.3.
- D3D11: t125-t127 as before.

### 3.3 Coexistence with ORGExponentialHeightFog (done 2026-11-23)

`ORGViewAtmosphere::SharedVolumeAt(Inputs)` is the switch, a function of revision values. `SharedVolumeActive()` is its adopted
value, and `ViewAtmosphere::IsPrepared()` answers it in graph modes. The fog's volume declarations exist only while it is false
(their existence rule adds `SharedVolumeDependencies()`), and the fog's client key is empty while it is true. So the fog's volumes
go to one texel and its passes leave in the same revision in which the atlas sizes up. The fog's ids stay published, so the
readers' graphs do not change. `ExponentialHeightFog::Prepass` returns on `IsPrepared()` before the graph path, as on D3D11, and
`ORGExponentialHeightFog::VolumesOn()` is false while the shared volume runs. Atmosphere and Fog sets up before the fog (the
feature list's order); the fog's setup is fatal otherwise.

### 3.4 The post-water composite (client `cs.view-atmosphere.composite`, EngineDraws)

- **Where on the graph.** The hook fires after the water draws and the water's ImageSpace blend (`ISWaterBlend`, step
  `cs.engine.imagespace.world.water`), before the engine's blended draw. That is the end of the `deferred+water` stage. Under
  water the engine's blended draw comes first and the water steps move into the `sun` stage, so the composite has a step in both,
  right after the water ImageSpace step, as the water's steps do. The hook calls `FrameAnchors::Run(EngineDraws)`; the client is
  active only during that call, and the stage's EngineDraws step that names it takes the tap.
- **Pass.** One gated compute dispatch (`Composite.cs.hlsl`, now dual-target). Reads: shared constants (latched at the hook), kMAIN
  depth, the medium by published id, the fog's volumes by published id (or the no-fog volume), IBL's SH, Dynamic Cubemaps'
  `EnvTextureBC6H`, and with Physical Sky loaded its transmittance LUT, the clouds' filtered targets (`VolTr`, `VolLum`), their
  cloud depth (`VolAux`, t41) and the lightning channels by published id. Writes kMAIN through its UAV. Groups per run from kMAIN's size and the dynamic resolution, through the run gate.
  Once a main-view frame, never in a reflection face, as on D3D11.
- **When it is on.** As D3D11's `CompositeEnabled`: while the fog is on or Physical Sky renders. The client is declared on the fog's
  `enabled`, Physical Sky's `enabled` and `enableVolumetricClouds` (frame revision plan 6.21; `CompositeDependencies`,
  `CompositeKeyAt`). "Physical Sky renders" is its toggle in the revision and `RendersWhenEnabled()` as the last Prepass saw it
  (worldspace, interior, shaders). Its key also needs the program built (ProgramCache, off the render thread) and every input it
  needs published, as the last Prepass observed them. `CompositeActive()`, and so `PostWaterComposite`, is on once the adopted
  revision holds the registration, so the flag and the composite switch at one Begin. Otherwise the shaders do their own fog, as
  before #3157. A change of inputs registers the client again when the next revision starts. The older registration composes
  until then (`matchesById`).
- **Volumetric clouds.** D3D11 renders Physical Sky's main-view clouds inside the composite call
  (`RenderVolumetricClouds(kMainViewAndCubemap)`). On the graph they run at Physical Sky's Prepass (physical-sky-port.md open
  question 22), so the composite reads this frame's clouds, ordered after their writes by the published reads. The composite
  reads them only once a cloud run wrote them (`ORGVolumetricClouds::OutputsWritten()`, that plan's open question 24) and the
  clouds are on in its revision. Until then the cloud transmittance is `ViewAtmosphere::NoCloud` (one texel of 1, the clouds'
  neutral clear) and the luminance, cloud depth and lightning are the null descriptor (zero).
- **What PostWaterComposite turns off, in the graph modes.** Each reads the flag from the frame's shared data, so all switch at
  the Begin that adopts the composite's registration:
  - Physical Sky's AP pass after the deferred composite (`cs.physical-sky.composite`) keeps running. Its shader skips the cloud
    composite under the flag (`CompositeAerialPerspective`), as the D3D11 deferred composite does. No gating change.
  - The sky tail runs while ViewAtmosphere is on and the composite is (`GenerateSkyTail`, physical-sky-port.md open question 14).
  - Lighting and Effect (DCLF), Sky, Water and the ORG SSAO composite read `SharedData::PostWaterComposite` from the shared
    constants or the shared buffer, the same value as D3D11. No shader change.
- **Push block.** 20 words: the shared four, eleven of its own, five of Physical Sky's. No handle table. Every other Physical Sky
  header resource is `UNREAD`; FXC lists none of them in any permutation, and a DXC build with the `UNREAD` handles set to a
  marker value reads none, the debug views included.

### 3.5 The ORG deferred composite's t125-t127

The deferred composite reaches the medium only through `PhysSky::CompositeAerialPerspective`. The Physical Sky port keeps the
ORG composite without `PHYSICAL_SKY` and adds its own pass after `cs.deferred` (push-block budget: 26 of 32 words). So the ORG
composite needs no ViewAtmosphere handles: in its build the header's "no medium" case applies, which is correct while there is no
shared volume. With the shared volume, `CompositeAerialPerspective` returns at once on D3D11 (`Enabled()`), so Physical Sky's
aerial-perspective pass is simply not staged while `IsPrepared()` (physical-sky-port.md open question 8); it needs no handles.

### 3.6 The shared volume (done 2026-11-23, not yet run in the game)

**When it runs.** `SharedVolumeAt`: `ViewAtmosphere.enabled`, Physical Sky loaded with its toggle on and `RendersWhenEnabled`
(the composite's observed `sky`), and the post-water composite's key present (`CompositeKeyAt`). That is what
`ViewAtmosphere::Prepare` needs on D3D11 (`CompositeEnabled()`): only the composite applies the medium to what was drawn before
it, and the shaders' `ViewAtmosphere::Enabled()` needs `PostWaterComposite`. The composite's key functions
(`CompositeDependencies`, `CompositeKeyAt`) are shared with the composite client, so both switch in one revision. Until the
composite composes with Physical Sky on (item 4, `CompositeKeyAt` on the vatm/composite branch), the key is empty whenever
Physical Sky renders, so the shared volume stays off.

**Passes.** They belong to the medium client `cs.view-atmosphere` (`ORGViewAtmosphereVolume`). Its key carries the volume's
inputs while the volume is on. At the Prepass anchor, after the parameters copy (order 410 on): a copy of the shadow light from
the volume's latch into `ViewAtmosphere::DirectionalShadowLights` (t98), then for each direction of the pair the three injects
and the three integrates, as D3D11 dispatches them. The source and history are `ViewAtmosphere::SourceA` and `SourceB`, History
textures with the atlas's shape and existence. Direction a reads A and writes B, direction b the reverse. The commit opens the
direction whose history the last committed run wrote (12 gate entries), so nothing is swapped. A history counts only once the
run that wrote it was submitted (`NoteWritten` in the executed callback); until then its weight is zero.

**Shaders.** `LightScattering.cs.hlsl` and `Integrate.cs.hlsl` are dual-target. Every handle each reads, its own and its
headers', comes from a handle table (`ViewAtmosphere/MediumTableList.hlsli`, included by HLSL and C++). The push blocks are 8
words (shared constants, table, the engine local shadows' latch) and 6 words. The grid index (D3D11's b0) is a table value, so
each dispatch has its own table. Header resources the programs never read are `UNREAD`. FXC output is unchanged (identical DXBC
for the full, fog-only and minimal permutations of LightScattering, and for Integrate). DXC builds both for SPIR-V (spirv-val
clean) and DXIL.

**Inputs**, as `ViewAtmosphere::Render` binds them: the directional shadow map (a `RenderGraph::DepthViewSource`, imported in the
key; Physical Sky's AP shadow now uses the same helper), its light in the latch and the t98 buffer, the engine local shadow maps
(an engine depth slot) and their data (`ExponentialHeightFog::BuildEngineLocalShadowData`, which
`ViewAtmosphere::PrepareLocalShadows` now also uses), Light Limit Fix's lists and local shadow data and cache, Terrain Shadows'
heights, Cloud Shadows' last layer, IBL's SH, Skylighting's probes, Dynamic Cubemaps' `EnvTextureBC6H`, and Physical Sky's
transmittance, multiscatter and disk phase LUTs and, once a cloud run wrote them (`OutputsWritten`), the cloud shadow volume and
mean. Unbound inputs are the null descriptor. flags.z is computed at Prepass as Render computes it, and the commit clears the bits
the executed registration does not bind (`Inputs::Bound`).

**Parameters.** `ViewAtmosphere::ParametersFor` computes the grids, flags.x and flags.y, the history weights and `clipToWorld`
for both paths. `InputFlags::Bits` gives flags.z. A change of the shadow bits drops the history, as on D3D11.

**Decisions to confirm.**
- The grids are laid out for the display size (the atlas's declaration), not the dynamic resolution as on D3D11. The grids
  cover the whole view in clip space, so only the cell size differs under dynamic resolution.
- In the map menu the shared volume keeps running; D3D11 falls back to the separate paths there. A per-frame switch would need a
  revision.
- The cloud diffuse transport (t70, D3D11's `CloudDiffuseTransport()`) is null: `ViewAtmosphere::ReflectionSkySH` is not published
  yet (3.7). The seam is `InjectSlot::TexCloudDiffuseTransport` in `ORGViewAtmosphereVolume::AddPasses`. Null makes
  `GetCloudDiffuseTransmittance` return 1, so with the clouds on, the medium's diffuse light is not darkened under clouds.
- Guest frames where the D3D11 deferred composite still runs get no medium at t125-t127 (`Bind` does nothing in graph modes), so
  that composite applies its own aerial perspective. The cloud programs read no medium either (physical-sky-port.md open
  question 22), where D3D11's clouds read it.

### 3.7 Reflections (ported 2026-11-23, not yet run in the game)

On D3D11, `RenderReflection` builds a 32^3 omnidirectional medium and the sky SH once per cubemap render, and binds them at
t125-t127 for the faces (with `flags.w = 1`). On the graph (`src/Features/ViewAtmosphere/ORGViewAtmosphereReflection.{h,cpp}`):

- **Client** `cs.view-atmosphere.reflection` at the ReflectionsPrepass anchor (start stage, `whole`), after Cloud Shadows' two
  clients in the recipe step. Gated steps: a copy of the run's parameters from the latch (every run), the sky SH (one group), the
  medium (16x32 groups), and a clear of the sky SH. `Reflection.cs.hlsl` is dual-target (24 push words, no handle table; FXC
  output unchanged for both entries).
- **When it runs.** The commit applies RenderReflection's conditions to the frame: the feature on (adopted), the shared volume
  ran (`ORGViewAtmosphere::SharedVolumeActive()`, D3D11's `ready`), the composite on (`CompositeActive()`, D3D11's
  `CompositeEnabled()`), Physical Sky on (`cbData.enabled`), no map menu. Readers wait for the cloud client (physical-sky-port.md
  open question 24): a registration runs only when it binds the cloud outputs exactly while they are on and written. On a run
  without the medium the parameters are all zero (no medium), so the faces do without it, as D3D11's unbound t127; the first such
  run after the medium ran also clears the sky SH to zero (no transport, as D3D11's null `CloudDiffuseTransport()`).
- **Inputs**, by published id: Physical Sky's Tr, Ms, Sv, AP and disk phase LUTs, the shadow volume and its mean and the cloud
  cubes (with the clouds on), IBL's environment SH, Skylighting's probes, Terrain Shadows' heights, Cloud Shadows' last layer
  (cube view). The key is what `ViewAtmosphere::RenderReflection` observed last (called right after the anchor, it only observes
  in the graph modes), plus the feature, sky and cloud toggles. The flags name only what the held registration binds.
- **Parameters** come from `ViewAtmosphere::ReflectionParameters(ReflectionFlags)`, which the D3D11 path uses too.
- **Outputs**, published at setup for the session: `ViewAtmosphere::ReflectionLuminance` (born zero),
  `ViewAtmosphere::ReflectionTransmittance` (born one), `ViewAtmosphere::ReflectionParameters` (the copy), and
  `ViewAtmosphere::ReflectionSkySH` (3x2 RGBA32F, born zero: row 0 the sky SH, row 1 the cloud diffuse transport).
- **The faces** read the reflection ids at t104, t107 and t108 through `Api::FrameTexture::facesGraphResources` (DCLF's
  ViewAtmosphere integration). The main view keeps the main ids.
- **The transport** (`CloudDiffuseTransport()`, t70) is the sky SH's second row. Physical Sky's sky tail binds
  `ViewAtmosphere::ReflectionSkySH` by id with the clouds on (`ORGPhysicalSky.cpp` MakePrepass). The shared volume's light
  scattering binds it the same way: `ORGViewAtmosphereReflection::Get().TransportPublished() ? Bind::Published(
  ORGViewAtmosphereReflection::kSkySHId) : null` at its t70 entry (EXTERN `TexCloudDiffuseTransport`, with
  `PS_CLOUD_TRANSPORT_EXTERN`). Zero contents read as no transport (`w == 0`), so it can be bound whenever it is published.
- **Recipe.** `PhysicalSky::MultiscatterLut`, `Skylighting::ProbeArray` and `TerrainShadows::ShadowHeight` join `kPreviousFrame`:
  the start stage reads the previous frame's, as D3D11 does before EarlyPrepass and Prepass.

## 4. Order of work

1. Done: seam; the medium's resources, client and DCLF frame textures; header split (fixes the collision).
2. Done: the post-water composite on the graph for the fog.
3. Done (2026-11-23): shared volume inject/integrate (3.6) and `SharedVolumeActive`. Not yet run in the game. It switches on
   only once item 4 lets the composite run with Physical Sky on.
4. Done (2026-11-23, not yet run in the game): `PHYSICAL_SKY` in the graph composite (3.4). The clouds stay at Physical Sky's
   Prepass.
5. Done (not yet run in the game): reflections (3.7). They run once the shared volume does (`SharedVolumeActive()`).

## 5. Open questions

1. The composite's step is after the water ImageSpace blend. If the hook fires before `ISWaterBlend` in some frame kinds, the
   graph order would differ from D3D11's. Check one run's schedule against the D3D11 order.
2. Faces vs main view: DCLF's frame textures give one view per register for both. The reflection medium needs either per-epoch
   views or separate registers for the faces. **Decided (2026-11-18): per-epoch views.** The reflection faces are already their
   own epochs, so a frame texture selects its view per epoch: no new registers, and one shader path for faces and main view.
   **Done (2026-11-23):** DCLF's `Api::FrameTexture::facesGraphResources` (the reflection view's epochs, `ResolveViewFrameTextures`)
   names the reflection ids; only `Integrations/ViewAtmosphere/ViewAtmosphereIntegration.cpp` changed.
3. Sky (`Sky.hlsl`) and Water (`Water.hlsl`) DCLF families: whether their graph builds define `DCLF_BINDLESS` and bind frame
   textures. If not, they read t125-t127 unbound. **Answered (2026-11-23):** neither defines `DCLF_BINDLESS`. Their ORG builds
   define `ORG_HEADER_RESOURCES` without `VIEW_ATMOSPHERE_ORG_HANDLES`, so `Common/ViewAtmosphere.hlsli` takes its "no medium"
   case: `Enabled()` and `ReflectionEnabled()` are constant false and nothing reads unbound registers. But the faces' sky then
   skips the reflection medium (`Sky.hlsl` `SampleReflection`, Physical Sky's `CompositeAerialPerspectiveReflection`) and uses the
   AP LUT and cloud cube, where D3D11 uses the medium. Left: the families' latch (`PhysSkyOrgHandles`) takes the three medium
   handles, chosen per epoch as the frame textures are, and their ORG builds define `VIEW_ATMOSPHERE_ORG_HANDLES`.
4. **Done (6.21).** `PostWaterComposite` followed the registration while the composite waited for its build, so those frames had
   no fog. Q12's point fix (`FrameAnchors::ScheduledThisFrame`) is gone: the flag is the registration the adopted revision holds.
5. The D3D11 composite took PS t31 as bound by the engine at the hook. The graph reads Dynamic Cubemaps' `EnvTextureBC6H`, as the
   ORG SSAO composite does. Compare in an interior with `useDynamicCubemaps`.
6. **Cloud inputs before the first cloud run (decided by the agent, to confirm).** D3D11's targets always exist and hold their
   neutral clear. On the graph the composite binds `ViewAtmosphere::NoCloud` for the transmittance until `OutputsWritten()`, so a
   null transmittance (zero) cannot black out the frame. The AP pass and DCLF bind the null view for every cloud input in the same
   window (physical-sky-port.md open question 24). With the composite on, the AP pass skips the clouds, but DCLF's forward draws
   still read them through `ViewMedium::SampleViewMedium`: check the first frames after the clouds switch on.
7. **Physical Sky's other binds (decided by the agent).** D3D11's `CompositeAfterWater` also binds the sky-view and AP LUTs, the
   AP shadow and the shadow volume. The shader reads none of them, so the graph composite does not declare them.
8. **Stale observation on a display-size change.** The clouds' flag in the key is what the last Prepass saw. A new version of the
   cloud targets (display size) is bound in the revision that also registers the cloud client again, so the composite reads it
   in the frame the client first writes it. Check a resolution change with the clouds on in native mode.
9. **Reflection medium choices (decided by the agent, 2026-11-23, to confirm).**
   - The client is always registered (with Physical Sky loaded), so its every-run copy gives the faces "no medium" from the first
     cubemap render on: the parameters buffer has no birth, and the faces' graphs declare it from the start. The main medium's
     parameters buffer has the same window before its client's first run.
   - Cloud Shadows' input is its last layer by id (cube view), as D3D11 binds the layer, not the occlusion copy the fog reads.
   - The disk phase LUT is bound by id. D3D11 binds no CS t106 in RenderReflection and reads whatever an earlier pass left there.
   - The programs are fixed at setup (Physical Sky's pattern): a shader reload does not rebuild them.
   - Physical Sky's sky tail and the light scattering read the sky SH the frame wrote it, after the start stage. D3D11 reads the
     last completed reflection update too, so this matches.

