# Comment cleanup: what it found

The comments in DCLF, `src/RenderGraph` and the features' ORG ports were cut and reworded in one pass (1,119 files, about
21,000 fewer lines). Only comments changed. The pass also recorded every place where a long comment was the main explanation of
a complicated subsystem or a surprising interaction. Those places are targets for simplification, or at least for a proper doc
section.

Each file in this folder is one slice's notes. Every notes file has the same sections: simplification or documentation targets
(with the original comment quoted where it mattered), an index of the hazards whose comments were kept, possible bugs or dead
code noticed, and coverage.

| Notes file | Covers |
|---|---|
| `core-misc.md` | DCLF `Api`, `Core` outside the folders below, `Integrations`, `Schedule` |
| `render-a.md`, `render-a2.md` | DCLF `Core/Render`: Direct, Epochs, DrawFamilies, Main, Passes |
| `render-b.md` | DCLF `Core/Render`: Payload, Readback, Resources, Revision, Shadow, Snapshot, Views |
| `scene.md` | DCLF `Core/Scene` |
| `gpu-engine-handoff.md` | DCLF `Core/Gpu`, `Core/Engine`, `Core/Handoff` |
| `features-a-l.md`, `features-m-w.md` | DCLF `Features` |
| `diagnostics-shaders.md` | DCLF `Diagnostics` and the DCLF shaders outside `Common/` |
| `rendergraph-top.md`, `rendergraph-top2.md` | `src/RenderGraph` and `src/RenderGraph/Frame` |
| `rendergraph-engine-a.md`, `rendergraph-engine-b-ports.md` | `src/RenderGraph/Engine` and the `ORG*` ports |

## Themes that came up in more than one slice

These are the targets several slices reported independently. The notes files have the details and many more single-site targets.

1. **Layouts shared by C++ and HLSL are kept in step by hand.** `GpuLayouts.h`, `Resources.h` (class plan and bucket map words),
   `DrawPipelines.h` (push words, row headers, registers t116 to t127) and about 30 "must match BuildDrawsCS" constants. One
   header shared by both languages would replace the comments.
2. **Code that must match other code bit for bit.** The fade step exists three times (engine, `FadeStateCS`, a C++ port), as do
   tree wind and the merge seed functions. `RegistrationCensus` re-implements the coordinator's classifier. A shared source or a
   parity test is needed.
3. **Publication and adoption protocols explained only in comments.** The snapshot publication gate and
   `AdoptCurrentSnapshot`, payload version chains (`Versioned.h` defines sealed, adopted, serial and contentEpoch for the whole
   feature), the three copies of "pooled snapshot plus change log plus replay" in Scene, and the order of a structural rebuild.
4. **Stage scheduling and run gates in the render graph.** `FrameAnchors.cpp` keeps each segment's schedule in parallel arrays.
   `RunGate.h` moves staging data through three mechanisms at commit. Seven overlapping "may I draw now" queries.
5. **Queue and lock ordering.** Compute, copy and upload queues against DXVK's stream (`RenderGraphRuntime.cpp`), the copy-queue
   signal against Present (`FrameValues.cpp`), and the retirement clock's two sources (`FrameRetirement.h`). Each records a real
   hang or device loss.
6. **Hook surfaces whose order lives in comments.** `CoordinatorComponents.h` (26 hooks), `Hooks.h` (Observe, Policy, Notify),
   `PassCapture` (six insertion hooks), `FeatureList.cpp` (a 21-line ordering comment was kept whole), and the forward
   refraction hooks that must install together.
7. **Two modes of one thing.** Guest and native CPU-pipeline modes (`GraphConditions`), the DCLF build point at Begin or at
   Main::Draw, Skylighting or the shell driving occlusion maps, three point-light cull modes, three sun skip levels.
8. **Engine draw families repeat the same mechanism and API.** Water, Sky, ShadowMask, MenuScenes and Precipitation each explain
   the same latch. A draw family touches about ten places.
9. **Per-stage engine notes.** `Frame/RecipeTable.h` and `Engine/Skeleton/Skeleton.cpp` held the only record of each stage's
   engine call order, and of where each engine clear or copy runs. That text belongs in `org-native-frame-graph.md`.
10. **Guides in headers.** The porting guide in `ORGPostEffect.h` and the client contract in `FrameAnchors.h` should be doc
    pages.

## Possible bugs noticed, not fixed

- `Features/DecalOcclusion/DecalOcclusionDraws.cpp`: a fifth or later test is clamped onto instance 3 and overwrites its run slot
  and latch entries.
- `Features/LocalLights/LightContextResources.cpp`: `UploadColumn` keeps a function-static mirror shared by all Scene instances.
- `Core/Render/Snapshot`: a removed `PostSnapshot` comment said frames have no epoch after a target format change.
- `Core/Scene`: `Walk.cpp` said geometry slots keep a grace period and `Slots.cpp` says they have none.
- `Features/Lod/Diagnostics/LodGateParity.cpp`: the comment said every 60th pass and the code uses 37.
- `SharedConstants.h`: `kUploadPoints = 19` must match `FrameAnchors`' `kMaxSteps` and nothing checks it.
- `Core/Render/Revision/Producer.cpp`: reuse witness hashes are maintained by hand, so a missed input keeps a stale revision.
- Dead code candidates: `kEffectVariantBlood`, `FadeStateConstants::reservedProgramsWord`, the `collisions` counter in
  `KeptStores.h`, an unreachable `return` in `WriteClassRegion`, a fallback draw path in `BuildDrawsCS.hlsl`, several Scene
  counters, and TEMP set-parity diagnostics.

## Left as they were

- `src/RenderGraph/DxvkOrgInterop.h` must stay identical to its copy in the DXVK fork. Two of its comments sit above the wrong
  declarations.
- `Tripwire/TripwireInterfaces.inl` is generated.
- Shaders under `Common/` were not touched, because editing them invalidates the shader store.
- A few class-level overview blocks of 6 to 10 lines remain where they are the only explanation of a subsystem, for example
  `LodGates.h`, `FaceSnapshots.h`, `GrassMirror.h` and `LightSelection.h`. They are listed as targets in `features-a-l.md`.
