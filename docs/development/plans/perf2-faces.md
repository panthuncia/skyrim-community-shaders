# Render thread round 2: the native reflection faces and the sky's native path (leads faces, faces2)

Branch `perf2/faces` (CS only, no ORG or BasicRHI change). Lead faces did the main work; lead faces2 merged org_integration
68510f02c, validated, measured and made the further cuts ("Round 2b" below). Timings are Dev-Fast (/Od CS code), guest mode, the frozen river scene,
one 6 s Tracy capture each (`trun.sh guest+quick <label> --set env.CS_ENGINE_ZONES=1`).

## What changed

**The faces' per-face work runs once per face render.** `ReflectionFaces::DrawNative` first prepares every face's sky run
(`Sky::PrepareFace`), then runs CS's reflection prepasses once (`Deferred::BeginReflectionFace`), reads what they bound once
(`Sky::ReadFaceInputs`: the header textures, the published inputs and Physical Sky's `ResolveOrgHandles`), and submits each face
with those inputs (`Sky::SubmitFace(run, inputs)`) followed by DCLF's face camera. Before, each of the two faces ran the prepasses,
the header resolves and `ResolveOrgHandles` itself. The prepasses bind the same views and their graph clients run only on the
frame's first prepass, so nothing a face reads changes between the faces.

- The ReflectionsPrepass anchor now runs once per update instead of once per face; the SkyReflections anchor still runs once per face.
- The reflection cube's size (`NativeFace`) and the cube and depth formats (`PrepareFace`) are read again only when the engine's
  texture or views change, not with `GetDesc` per face.
- A sky draw's t0-t2 and s0-s2 are kept for the frame by the views they sample (`SkyNative.cpp`, `SlotsOf`): the two faces and the
  main batch draw the same sky objects, so most draws skip `SlotTexture` and `SlotSampler`.
- `Deferred::ReflectionsPrepasses` lost its D3D11 Tracy GPU zone (a query pair per call).

**Old paths deleted.**

- `CS_DCLF_REFLECTION_NATIVE`: the switch, the engine-face mode (0), the observe mode (1) and its camera parity (`CompareCamera`) are
  gone. A plain face whose LOD roots DCLF owns is always drawn natively.
- `CS_DCLF_SKY_NATIVE` and the sky's latch path: `SkyCapture.cpp` became `SkyHooks.cpp`. Gone: the draw taker and
  `BSSkyShader::SetupGeometry` watcher, `TryLatchDraw`, the run continuation, the latched sun query path, the observe modes for the
  batch, the cloud layers, the sun and the faces, and every parity helper (`Native::Compare`, `CompareFaceBatch`,
  `CompareFaceClouds`, `ObserveFace`, the parity counts in the status line). The sky batch, the cloud layers and the sun query are
  computed natively in every case.
- The switches' rows in `tools/cs-test/switches.json`. No cs-test config used them.

What the latch path did that native did not: while a draw's program was building, the latch left that draw out and kept the batch
on the graph (handing it to the engine sends a face's cloud layer to Cloud Shadows' D3D11 path, a fatal tap error;
`sky-cold-programs.toml`). The native build used to fail the batch in that case and fall back to the latch. `BuildPass` now returns
`PassResult::LeftOut` for a program still building, and the batch, cloud group, face or sun query drops that draw (logged once).

## Fallbacks that remain

A face goes to the engine's face render when:

- the sky family does not draw now (not the render owner, its clients not set up), or the faces' client cannot take a face;
- Cloud Shadows runs on D3D11 (its occlusion t25 is not on the graph);
- a sky-object effect is in the face (the aurora: an effect shader in the sky root), which the native face does not draw;
- a sky pass cannot be computed (not a BSSkyShader TriShape, a texture that cannot be imported), or every draw of the face is
  left out while its programs build (the face needs a draw to clear it).

These are correctness fallbacks without a switch. The engine's face then reaches the same native batch and cloud hooks. A batch or
cloud group the native build cannot take is drawn by the engine (logged once per reason).

## Measured

Baseline `p2-faces-base1` (unchanged branch 742af7870): window 5.25 ms, DCLF CPU 0.72, `CS.DCLF.Reflection.NativeFaces` 0.335 ms
(PrepareFace 0.120 of which BuildFace 0.109 with SlotTexture 0.035 at 70 calls a frame; ReflectionsPrepasses 0.103; SubmitFace 0.095
of which ReadInputs 0.050 and ResolveHeaders 0.006).

`p2-faces-a1-perf` (6f5aca3e2, faces batched per update call): NativeFaces 0.337 ms, no gain. The capture showed why: the update
renders **one face per call, two calls a frame**, so per-call batching does nothing. SlotTexture calls fell from 70 to 18 a frame but
its time barely moved (0.035). Window 5.51 ms (other leads' runs and builds were going; noise).

b0e418ecb makes the prepasses and the input reads once a **frame** (`ReflectionFaces::DrawNative`, keyed by `Family::GameFrame()`;
`Deferred::BeginReflectionFace(bool a_prepasses)`). Measured by faces2 below
(`c1`, together with the round 2b cuts).

## Test runs

| Config | Label | Result |
|---|---|---|
| guest + scenes, baseline | p2-faces-base-guest | hang after `coc Riverwood` (engine main thread in WaitForFrameJobs; baseline, not this branch); river and river-night shots |
| native-vulkan + scenes, baseline | p2-faces-base-nv | passed |
| guest+quick Tracy | p2-faces-base1, p2-faces-a1-perf | captures above |
| guest + scenes2 (river, 23:00, Dragonsreach), a1 | p2-faces-a1-guest | passed; first native face logged, no engine-face fallbacks, no left-out or tap-error lines |
| native-vulkan + scenes2, a1 | p2-faces-a1-native-vulkan | passed; same log checks |

Shots: river-night and Dragonsreach match the baseline within run noise. The river shot's water brightness differs (half the
water reflection bright, cut at a cube face edge). Round 1's shots show the same thing between two runs of one build and in the
engine-face run (`p-reflections-nat1`/`nat2`/`eng2`): the faces at shot time are from different frames. Not caused by this branch.

## Decisions (by the agent, to confirm)

- The latch path is deleted although the plan asked for observe runs of rain, storms and the aurora first. Reason: the user's
  direction to drop old paths; the aurora sends a face to the engine as before, and the engine's face still reaches the native hooks.
- A batch the native build cannot take is now drawn by the engine (before: the latch path tried). The latch path refused the same
  cases (non-TriShape, non-sky shader), so only a texture slot holding a published graph texture differs.
- The sky draw slot cache keys on the sampled views, per game frame, and appends the owners to every run that uses an entry.

## Round 2b (lead faces2)

**Slot owners once per build.** A sky draw's cached t0-t2/s0-s2 entry (`SlotsOf`) appended its six owners to the run for every draw
that used it. It now appends them once per build: `Frame` carries a build serial and the entry remembers the last build it joined.

**Face cull list kept.** `FacePasses` keeps its registration list between faces instead of a new vector per face. It got a zone
(`CS.DCLF.Sky.Native.FacePasses`).

**Header textures and Physical Sky views from the features.** The sky no longer reads t76/t77/t30/t31 with `PSGetShaderResources`.
`ResolveHeaders` (SkyHooks.cpp) takes each register's view from the DCLF frame texture its feature registered (IBL's t76/t77, Dynamic
Cubemaps' t30/t31; `Extensions::FrameTextureIndices`), and a null view is the null view of its kind. Physical Sky's
`ResolveOrgHandles` takes the graph view (`OrgView`) or else the feature's own texture (`kPhysSkySlots`), never the D3D11 binding.
`Family::ResolveHeader` was split: `ResolveHeaderView` resolves a given view (published id, import or null view), and
`ResolveHeader` reads the binding and calls it (water and precipitation still use it). This is in Core/Engine/Family, outside this
lead's files; only that function changed.

**Not done: one SkyReflections anchor run per frame.** The update renders one face per call, two calls a frame, and the engine works
between the calls. One anchor run for both faces would hold the first face's run until the second call, or predict the next face.
Measured cost of the second submit is about 0.017 ms (Dev-Fast). Decided by the agent, to confirm: not worth the ordering risk.

### Measured (Dev-Fast, guest, frozen river)

| Capture | Window | DCLF CPU | NativeFaces | BuildFace (self) | ReflectionsPrepasses | ReadInputs (faces) | SubmitFace |
|---|---|---|---|---|---|---|---|
| base1 (742af7870) | 5.25 | 0.72 | 0.335 | 0.109 (0.058) | 0.103 (2/f) | 0.050 x2 | 0.095 |
| m1 (merged, before 2b; noisy machine) | 6.79 | 1.13 | 0.370 | 0.140 (0.073) | 0.086 (1/f) | 0.062 (1/f) | 0.046 |
| c1 (final) | 4.63 | 0.72 | **0.267** | 0.096 (0.042) | 0.068 (1/f) | 0.045 (1/f) | 0.034 |

NativeFaces fell 0.335 -> 0.267 ms (-20%). The prepasses and input reads now run once a frame (b0e418ecb), SubmitFace lost its
per-face reads, and BuildFace's self time fell 0.058 -> 0.042. The window difference (5.25 -> 4.63) is mostly other leads' merged
work and machine noise; m1 ran while other games were building.

What is left in NativeFaces: `Deferred::ReflectionsPrepasses` self 0.056 (mostly `State::UpdateSharedData` and the view
atmosphere's reflection setup, State.cpp, not this lead's), BuildFace 0.096 (SlotTexture 0.029 for the frame's six new slot
entries, LatchGeometry 0.013, FacePasses 0.011), ReadInputs 0.038 (Physical Sky's 15 slots by published id).

### Test runs (round 2b)

| Config | Label | Result |
|---|---|---|
| native-vulkan + scenes2, merged | p2-faces-m1-native-vulkan | passed; first native face, no fallbacks, no tap errors |
| guest + scenes2, merged | p2-faces-m1b-guest | passed (m1-guest was not-run: cs-test lab slot bug, since fixed) |
| native-d3d12-quick, merged | p2-faces-m1-d3d12 | passed |
| native-vulkan+sky-cold-programs, merged | p2-faces-m1c-cold | passed; face and main draws left out while building, no tap errors |
| guest+quick Tracy | p2-faces-m1-perf, p2-faces-c1-perf | captures above (cs-test reports "failed" because trun's steps drop quick's shot) |
| guest + scenes2, final | p2-faces-c1-guest | passed; first native face, the aurora at 23:00 sent a face to the engine (expected fallback), no tap errors |
| native-vulkan + scenes2, final | p2-faces-c1-native-vulkan | passed; same log checks |

### Decisions (faces2, to confirm)

- The sky's headers and Physical Sky's views come from the features, not the D3D11 bindings. Where the feature binds nothing (IBL off
  for the scene, Dynamic Cubemaps before its first PostDeferred) the draw reads the null view, where before it read whatever was
  left bound. Guest mode only differs where a binding was stale.
- One anchor run per face kept (above).

## Open items

- `State::UpdateSharedData` in the reflection prepass (~0.05 ms Dev-Fast) is the largest piece left; it is outside this lead's files.
- The slot cache is per game frame; keeping entries across frames would need a lifetime key for the engine's views.
- The latch path's deletion and the engine-drawn batch fallback (lead faces' decisions above) still want the planned rain, storm
  and aurora observation.

Shots of the final runs against the merged-build runs: Dragonsreach within run noise; the river differs (camera a step apart and
the water reflection's brightness), as lead faces saw between two runs of one build. Not attributed to round 2b.
