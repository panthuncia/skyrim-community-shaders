# Atmosphere and Fog (ViewAtmosphere) on the render graph

Status 2026-11-18. Upstream #3157 (`8e6e5ba10`) added ViewAtmosphere as a D3D11-only CORE feature. This note says how it
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

- `Prepare` calls `ORGViewAtmosphere::Prepass()` and returns false, so `IsPrepared()` stays false and the fog's graph path runs.
- `RenderReflection`, `Unbind` and `Render` do nothing (`Render` already returns while not prepared).
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

All published. Passes: a one-shot upload of the first contents, and a per-run copy of the parameters from the client's latch
(the commit writes them). All-zero parameters mean "no medium" (`flags.x == 0`). The atlas extent later follows the settings
through `RequestSize`, so a quality change is a new version at the next frame boundary and never a build.

**Shader side** (`Common/ViewAtmosphere.hlsli`), one header for all builds:

- ORG pass that opts in (`VIEW_ATMOSPHERE_ORG_HANDLES` with `ORG_HEADER_RESOURCES`): EXTERN handles `ViewAtmosphereLuminance`,
  `ViewAtmosphereTransmittance`, `ViewAtmosphereData`.
- Any other ORG pass with `ORG_HEADER_RESOURCES` (the menu-scene Lighting/Effect builds, the ORG deferred composite): no
  medium. `Enabled()` and `ReflectionEnabled()` are constant false and the reads compile away.
- `DCLF_BINDLESS`: t104, t107, t108, which DCLF's new integration (`Integrations/ViewAtmosphere`) fills as frame textures from
  the published ids through their exports. This fixes the collision in 2.3.
- D3D11: t125-t127 as before.

### 3.3 Coexistence with ORGExponentialHeightFog

Until the shared volume runs on the graph, the fog's graph volumes are the fog and the medium says "no medium". When the shared
volume lands (3.6), `ORGViewAtmosphere::SharedVolumeActive()` becomes the switch: `ViewAtmosphere::IsPrepared()` answers it in
graph modes, `ExponentialHeightFog::Prepass` then releases the fog's volumes as on D3D11, and the fog's published ids are
withdrawn (they stay declarable, so the readers' graphs do not change). The switch reads settings and Physical Sky's state at
the frame's Prepass only, and both sides follow it in the same frame.

### 3.4 The post-water composite (client `cs.view-atmosphere.composite`, EngineDraws)

- **Where on the graph.** The hook fires after the water draws and the water's ImageSpace blend (`ISWaterBlend`, step
  `cs.engine.imagespace.world.water`), before the engine's blended draw. That is the end of the `deferred+water` stage. Under
  water the engine's blended draw comes first and the water steps move into the `sun` stage, so the composite has a step in both,
  right after the water ImageSpace step, as the water's steps do. The hook calls `FrameAnchors::Run(EngineDraws)`; the client is
  active only during that call, and the stage's EngineDraws step that names it takes the tap.
- **Pass.** One gated compute dispatch (`Composite.cs.hlsl`, now dual-target). Reads: shared constants (latched at the hook), kMAIN
  depth, the medium by published id, the fog's volumes by published id (or the no-fog volume), IBL's SH, Dynamic Cubemaps'
  `EnvTextureBC6H`. Writes kMAIN through its UAV. Groups per run from kMAIN's size and the dynamic resolution, through the run gate.
  Once a main-view frame, never in a reflection face, as on D3D11.
- **When it is on.** `CompositeActive()`: Exponential Height Fog on, Physical Sky off (its media are not on the graph yet), the
  program built (ProgramCache, off the render thread) and every input it needs published. Otherwise `PostWaterComposite` is 0 and
  the shaders do their own fog, as before #3157. A change of inputs registers the client again; nothing per frame does.
- **Volumetric clouds.** D3D11 renders Physical Sky's clouds first (`RenderVolumetricClouds(kMainViewAndCubemap)`). That is
  Physical Sky's port; its cloud passes go at the same step or just before it.

### 3.5 The ORG deferred composite's t125-t127

The deferred composite reaches the medium only through `PhysSky::CompositeAerialPerspective`. The Physical Sky port keeps the
ORG composite without `PHYSICAL_SKY` and adds its own pass after `cs.deferred` (push-block budget: 26 of 32 words). So the ORG
composite needs no ViewAtmosphere handles: in its build the header's "no medium" case applies, which is correct while there is no
shared volume. With the shared volume, Physical Sky's aerial-perspective pass is the reader that must take the three handles.

### 3.6 The shared volume (not ported: waits on Physical Sky)

Inject and integrate read Physical Sky's LUTs and medium functions (`PhysicalSky/Common.hlsli`) throughout, the shadow volume,
the cloud shadow mean and the cloud diffuse transport. They port once Physical Sky publishes those (ids in
`ORGPhysicalSky.h`: `TransmittanceLut`, `SkyViewLut`, `ApLut`, `DiskPhaseLut`, later `ShadowVolume`, `CloudShadowMean`,
`SkyTail`, `LightningChannels`, `VolCube*`; the multiscatter LUT is not published yet and must be).

- Passes at the Prepass anchor, after `cs.exp-height-fog`'s order: per grid, inject (`Source` from `History`) then integrate;
  `History` and `Source` as a ping-pong pair of History-lifetime textures, swapped by which one each run declares (two pass
  sets, the gate opens one), not by swapping pointers.
- Inputs as `ORGExponentialHeightFog` already reads them (published ids, imported shadow maps, the t98 copy, the engine local
  shadow constants through the latch), plus Physical Sky's.
- The parameters (grids, flags, history weight, `clipToWorld`) are computed as `ViewAtmosphere::Prepare` does and latched.
- Shader lists: LightScattering and Integrate become dual-target with their own X-macro lists, using Physical Sky's
  declaration guards (`#ifndef ...DECLARED` in `Common.hlsli`) rather than `ORG_HEADER_SRV` there.

### 3.7 Reflections (not ported: waits on Physical Sky)

On D3D11, `RenderReflection` builds a 32^3 omnidirectional medium and the sky SH once per cubemap render, and binds them at
t125-t127 for the faces (with `flags.w = 1`). On the graph:

- Passes at the ReflectionsPrepass anchor (start stage, `whole`): sky SH (one group), then the reflection medium (16x32 groups).
- Separate published ids (`ViewAtmosphere::ReflectionLuminance`, `...ReflectionTransmittance`, `...ReflectionParameters`), because
  the faces and the main view read different media in the same frame. DCLF's faces would read them through a second set of
  frame-texture registrations scoped to the faces, which the frame-texture API does not have yet (open question 2).
- `CloudDiffuseTransport()` (the SH texture's second row) also feeds Physical Sky's main-view passes: publish it.

## 4. Order of work

1. Done: seam; the medium's resources, client and DCLF frame textures; header split (fixes the collision).
2. Done: the post-water composite on the graph for the fog.
3. After Physical Sky's LUTs are published: shared volume inject/integrate (3.6), then `SharedVolumeActive`.
4. After Physical Sky's clouds and lightning: `PHYSICAL_SKY` in the graph composite, its cloud passes before it.
5. Reflections (3.7).

## 5. Open questions

1. The composite's step is after the water ImageSpace blend. If the hook fires before `ISWaterBlend` in some frame kinds, the
   graph order would differ from D3D11's. Check one run's schedule against the D3D11 order.
2. Faces vs main view: DCLF's frame textures give one view per register for both. The reflection medium needs either per-epoch
   views or separate registers for the faces. **Decided (2026-11-18): per-epoch views.** The reflection faces are already their
   own epochs, so a frame texture selects its view per epoch: no new registers, and one shader path for faces and main view.
3. Sky (`Sky.hlsl`) and Water (`Water.hlsl`) DCLF families: whether their graph builds define `DCLF_BINDLESS` and bind frame
   textures. If not, they read t125-t127 unbound.
4. `CompositeActive()` turns on when the registration exists. Until the stage's next build holds it, the hook's run is Idle and
   that frame has no fog (logged once). A `declared` callback would let the schedule hold it from the first frame.
5. The D3D11 composite took PS t31 as bound by the engine at the hook. The graph reads Dynamic Cubemaps' `EnvTextureBC6H`, as the
   ORG SSAO composite does. Compare in an interior with `useDynamicCubemaps`.
