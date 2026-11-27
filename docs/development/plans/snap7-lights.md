# snap7-lights: shadow casters and shadow maps from a captured light set (phase 7, item 2)

Lead `lights`, branch `snap7/lights` (base `org_integration` a9182dd5f), stage `s7-lights`. Open item 2 of
[snap7-split.md](snap7-split.md) ("a captured shadow light set"); decisions 8, 44-47, 66-70, 142-145, 170-174, 189 and 192-200 of
[render-thread-decisions.md](render-thread-decisions.md). Still today's single-threaded frame: no render thread.

Status: **done** for the shadow maps, the world masks, the lens flares and DCLF's local shadow light inputs. Light Limit Fix,
Exponential Height Fog and Deferred still read the shadow lights at record time (open items).

## 1. What was read live

Before this branch, native's render half (snap7-split step 1: `RecordFrame`) read the engine's shadow lights while recording:

| Site | What it read |
|---|---|
| `NativeCasters::RenderShadowMaps` | `shadowLightsAccum`, walked light by light; the main accumulator and world camera (written) |
| `ShadowMapsNative` (per light) | descriptors, their accumulators and cameras, ShadowViews' coverage; then the Render bookkeeping written into the descriptors (targets, slices, light transforms, the free mask, the scene accumulate array) under the engine's TLS memory context |
| `ShadowMask::DrawWorldMasks` / `Native::Build` | the world node's `shadowLightsAccum`, each light's kind, maps, mask index, bias, +0x63, falloff, NiLight radius, the descriptors' transforms, targets and slices, the sun's splits; the masks' settings globals; then `ReturnShadowmaps` per light (written) |
| `ImageSpace` lens flares | a queue of `NiPointer<BSShadowLight>`: each light's NiLight position, radius, diffuse, lens flare data |
| `ShadowCapture::CaptureCasterVolume` | each view's descriptor culling process planes (overwritten by DCLF's own camera for every native view) |
| `LocalShadowLights::Sample` / `SamplePointLights` | the selected local lights' NiLight, +0x61, descriptors (process, enabled), fade, lodDimmer |

The live-read detector did not see any of these (they are raw reads). A new detector object, `ShadowLights`
(`Snapshot/EngineReads.h`), counts them: the observe commit (b9d10677e) marked each site, and the final build marks the capture.

## 2. The light set (`Features/Shadows/Native/LightSet.{h,cpp}`)

`Shadows::Native::LightSet`: the frame's kept shadow lights in `shadowLightsAccum`'s walk (each light once, in slot order), as
values (`ShadowLight`: kind, omni, maps, mask index, focus flag, +0x63, land flag, bias, spot falloff, the NiLight's position,
diffuse, radius, fade, lodDimmer and hidden bit, each descriptor's target, slice, light transform and cull flag, the draws'
DCLF views, modes, parabola values and slice clears, the state the last draw leaves, the lens flare data), the sun's cascade
count and splits, and the masks' settings (filter, Poisson radius, resolution, biases, far value, scene shadows, focus targets).
Lights are keys (`const BSShadowLight*`): the render side never reads through them.

**Game side** (the native driver's game half, at the casters point, decision 8): `SelectCasters` (as before), DCLF's
`ShadowCastersSelected` (views listed, targets predicted), then `CaptureSet`: per light `Engine::PrepareLightMaps` (ListDraws, the
coverage verdict, the Render bookkeeping into the descriptors, under the TLS memory context), the values read, then CS's port of
`ReturnShadowmaps` for every light (what the masks' loop did after its draws). The main accumulator's camera and flag are written
here too. The whole casters point opens a `CaptureScope` in native, so its reads count as captures.

**Hand-over**: `Native::Scene::Prepared::shadowLights` (a `shared_ptr<const LightSet>`); `RecordFrame` adopts it first
(`AdoptSet`); a frame the engine's own skeleton draws (the frozen menu capture) adopts the set its selection just made at the shadow
maps point.

**Render side** reads only `RenderSet()`:
- `RenderShadowMaps`: per light `Engine::RenderLightNatively(const ShadowLight&)`: the verdict, the views captured
  (`CaptureNativeShadowView`), the slice clears, the state the last draw leaves (SetCameraVectors, the current accumulator, the
  render mode, the paraboloid radius: render-owned renderer state).
- `DrawWorldMasks`: `Native::Build(view, run, lights, settings, sun, ...)`. A menu scene's masks and guest's engine-call masks read
  their lights at the call (`ReadLights`, `ReadMaskSettings`, `Native::ReadSun`): they render where the lights are kept.
- Lens flares: the gather walks the set's lights with lens flare data (no queue, no references); a flare's history is keyed by the
  light's address.
- `LocalShadowLights`: native takes each selected light's values from the set; guest reads them as before.
- `ShadowCapture`: the light's caster volume is read only for a view without DCLF's own camera (never in native; the same result in
  guest, where ApplyOwnCamera overwrote it).

Guest is unchanged: the engine selects and renders; the masks' engine path reads the same values at the same point.

## 3. Write-backs into the engine's light objects

Kept, on the game side: the selection's camera ports and bookkeeping (cameras, accumulators, processes, the sun's vector, splits and
smoothing, lodDimmer, frustrumCull, `shadowLightsAccum`, mask indices, the selection globals, `firstPersonShadowMask`) and the maps'
bookkeeping (descriptor targets, slices, light transforms, the free mask). They are the selection's working storage across updates
(the sun's smoothing, accumulators made on first use, the slice allocator), and Light Limit Fix, Exponential Height Fog and Deferred
still read them at record time. No game code reads them (decision 47).

Moved off the render side: the descriptors' bookkeeping, the scene accumulate arrays' release and the TLS memory context
(`RenderShadowMaps`), `ReturnShadowmaps` (the masks), the lens flare queue's references, the main accumulator's camera and flag.

## 4. Cadence

The selection is game-side work and runs once per update in phase 7. Today a repeated snapshot's render runs the driver's game half
again (snap7-split step 1), so the selection runs again with the same inputs. The sun direction's smoothing (the only history in
the selection) now steps once per update: `SunCamera`'s real delta is 0 on a repeated render, so the cameras, and the set, come out
the same. Dropped renders select only on rendered updates, as before.

## 5. Detector

Live engine reads of the `ShadowLights` object per native world frame (Dev-Fast), the observe build (b9d10677e, the base plus
marks) against the switched build. After the switch the only mark is the capture's, inside a `CaptureScope`: counted as captured.

| Site | quick before | interior before | after (every run) |
|---|---|---|---|
| `ShadowCapture.cpp` CaptureCasterVolume (capture-point) | 2.58 | 6.83 | 0 (not called for owned views) |
| `NativeCasters.cpp` RenderShadowMaps' walk | 1.00 | 2.90 | 0 |
| `ShadowMapsNative.cpp` RenderLightNatively | 1.00 | 2.90 | 0 |
| `ShadowMaskNative.cpp` Build's walk | 1.00 | 2.90 | 0 |
| `ShadowMaskHooks.cpp` the masks' ReturnShadowmaps | 1.00 | 2.90 | 0 |
| `LocalShadowLights.cpp` Sample | - | 2.54 | 0 |
| lens flare gather and placement | - (no light with lens flare data in view) | - | 0 |
| **ShadowLights total** | **6.6** | **21.0** | **0** in quick, interior, tour, post-effects, drop-cells |

Whole table, live reads per world frame: quick 624.7 -> 625.6, interior 520.4 -> 495.1 (scene noise; the marks only exist on these
two builds). The other annotated shadow sites the table lists (`ShadowMapsNative.cpp` Renderer target descs, `ShadowMaskNative.cpp`
Shaders/ImageSpace/ShadowState/Renderer) are render-owned renderer memory, not light state; the masks' ShaderManagerState read of the
sun moved to the capture. Engine render calls stay 1 per world frame (3 in drop-cells, DrawFrame per update).

## 6. Runs

Stage `s7-lights`, Dev-Fast, `CS_SNAPSHOT_DETECT=1` on native runs. All with the final code (44d4bfed9) unless named.

| Config | Label | Result |
|---|---|---|
| native-vulkan+quick (observe build) | s7-lights-a-obs-nvk-quick | passed; 6.6 ShadowLights reads per world frame |
| native-vulkan+native-interior (observe build) | s7-lights-a-obs-nvk-interior | passed; 21.0 |
| native-vulkan+native-interior (first switch, with references) | s7-lights-b-nvk-interior | crashed at the coc: the ImageSpace capture read the sun shadow light's null NiLight (LS-d); fixed |
| native-vulkan+native-interior | s7-lights-c-nvk-interior | passed with shot references; 0 live ShadowLights; 2,400 lights native per 600 frames, none to the engine |
| native-vulkan+quick | s7-lights-d-native-vulkan-quick | passed with the shot reference; sun shadows drawn |
| guest+quick | s7-lights-d-guest-quick | passed |
| native-d3d12-quick | s7-lights-d-native-d3d12-quick | passed with the shot reference |
| native-vulkan+shadows-blend | s7-lights-d-native-vulkan-shadows-blend | passed |
| native-vulkan+llf-local-shadows | s7-lights-d-native-vulkan-llf-local-shadows | passed (LLF follows DCLF's views; slice mapping logged) |
| native-vulkan+first-person-switch | s7-lights-d-native-vulkan-first-person-switch | passed |
| native-vulkan+post-effects | s7-lights-d-native-vulkan-post-effects | passed |
| native-vulkan+lens | s7-lights-d-native-vulkan-lens | passed |
| native-vulkan+native-tour | s7-lights-e-native-vulkan-native-tour | passed |
| native-vulkan+menu-loading | s7-lights-e-native-vulkan-menu-loading | passed (loading model shadowed; menu masks read at the call) |
| native-vulkan+cadence-drop-cells | s7-lights-e-native-vulkan-cadence-drop-cells | passed |
| native-vulkan+update-cap30 | s7-lights-e-native-vulkan-update-cap30 | passed |
| native-vulkan+update-cap240 | s7-lights-e-native-vulkan-update-cap240 | passed |
| native-vulkan+native-interior | s7-lights-e-native-vulkan-native-interior | passed with shot references |

Several shots show the Health Low tutorial (decision 92's test setup), so they only check that the frame drew.

## 7. Decided by the agent, to confirm

- LS-a. The selection, the light cameras and the maps' bookkeeping run on the game side (the driver's game half): the camera ports
  call the engine's allocator, constructors, TLS memory context and `NiAVObject::Update` (decision 68), which stay on the game
  thread, and the selection writes state the next update's selection reads (the sun's smoothing, the slice allocator). Nothing
  game-side reads the results (decision 47), so decision 8 does not force it; the engine calls do.
- LS-b. The engine's shadow light objects stay the selection's working storage; the render side reads only the set's copies. Moving
  that storage into CS (render-owned shadow cameras) is not needed for the split.
- LS-c. The maps are given back at the end of the capture, before the render half, instead of after the masks' draws: the render side
  no longer reads the descriptors, and a light kept on one update could be removed by the next update's game logic. Readers after
  the masks (Light Limit Fix's slice info, Exponential Height Fog) saw returned descriptors before and still do.
- LS-d. The set holds no engine references. The first build held `EngineRef`s; a coc to Dragonsreach then crashed in the ImageSpace
  capture reading the sun shadow light's null NiLight: the engine's teardown of the sun light depends on its reference count.
- LS-e. The coverage verdict (`Whole`) is taken on the game side, with the set. Nothing registers into the shadow accumulators in
  native between the selection and the maps.
- LS-f. A light whose views cannot be captured still has its slices assigned (the bookkeeping ran on the game side); it casts no
  shadow that frame, as before.
- LS-g. The sun's smoothing steps once per update (section 4).

## 8. Open items

- Light Limit Fix (`ReadLocalShadowRenderInfo`, `TryAssignShadowMask`, its light walk), Exponential Height Fog
  (`BuildEngineLocalShadowData`) and Deferred (the sun's light transforms) still read the shadow lights at record time
  (snap7-split item 4). The set has every value they read.
- Menu scene shadows (`MenuScenesShadows.cpp`, decision 189) select and render where they record (their frame kinds stay on the
  producing thread, SPLIT-e); their masks read the menu node at the call.
- The per-light lens flare path (a local light with lens flare data) was not in view in any run; the sun's flare path is unchanged.
- The renderer state the last shadow draw leaves (SetCameraVectors, current accumulator, render mode) is still written on the render
  side for engine readers after it; it is render-owned memory.

## 9. Proposed decision-log entries

- **Native shadow rendering draws from a captured light set** (phase 7 item 2, refines 66 and 174): the casters point is game-side
  work. After the selection and DCLF's view list, `CaptureSet` does the shadow maps' bookkeeping, copies each kept light's values
  (`Shadows::Native::LightSet`) and gives the maps back (the masks' loop did). The render half adopts the set through
  `Native::Scene::Prepared`; the shadow maps, the world masks, the lens flares and DCLF's local shadow light inputs read only it.
  `ShadowLights` live reads per world frame: quick 6.6 -> 0, interior 21.0 -> 0.
- **The light set holds no engine references** (keys only): a held reference changed the engine's teardown of the sun light at a
  cell change (crash in the ImageSpace capture).
- **The engine's shadow light objects stay the selection's game-side working storage** (refines 66): the camera ports' allocation,
  constructors and TLS memory context stay on the game thread (68). No game code reads the results (47).
- **The sun's smoothing steps once per update** (a repeated render's real delta is 0).
- **Menu scene masks and guest's engine-call masks read their lights at the call** through the same value reader (`ReadLights`).
