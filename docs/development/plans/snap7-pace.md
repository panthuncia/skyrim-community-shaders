# snap7-pace: game update dips, the portal main-context stamp, frame-start reads (phase 7)

Lead `pace`, round `snap7`, branch `snap7/pace` (base `org_integration` fbf14d20c), stage `s7-pace`. Continues
[snap7-thread2.md](snap7-thread2.md) items 6 and 7 and its portal open item. Dev-Fast (/Od) throughout: frame rates are Dev-Fast,
one sample each.

Status: all three items fixed. The resize device loss stays with snap7-devloss.

## 1. Commits

| Commit | What |
|---|---|
| 4ee897c2e | fix(native): record the render half without the lock (item 1) |
| 7583e4790 | fix(dclf): portal contexts carry the game step (item 2) |
| 77bc66a10 | refactor(dclf): first-person depth from CS targets (item 3) |
| 36a328fcc | fix(dclf): grass fade clock from the frame globals (item 3) |

No submodule changes.

## 2. Item 1: game update dips

Measured with the no-admin stack sampler on the game (main) thread: 1 ms samples for 8-9 s after the "start" mark
(`perf-tools/sampler.py`; a small script grouped the samples whose innermost frame is a wait by their callers).

- perf-taa (base): 85% in the update-rate cap's wait, no lock waits, 60 updates/s steady. perf-taa shows no dip.
- perf-dragonsreach (base): 65% in the cap's wait and **25.3% in `ZwWaitForAlertByThreadId` under `Renderer::Lock`**, called from
  AE 0x1401fad38 in FUN_1401faca0 (AE 15220, BGSDecalManager's update, from Main::Update). It takes the lock whenever decals are
  queued. Game updates 58.8, 49.4, 46.2 a second in the marked windows.
- Cause: the render thread held Renderer::Lock (a critical section, which is not fair) for its whole render half and took it again
  at once. At 220 renders a second the game thread rarely got it. BSTaskPool_HandleTask also takes the lock (task pool threads).

Fix: the render half takes the lock only around `Renderer::Begin` (CS's adoption and frame start, the engine's body) and again around
volumetric lighting, the interface and `Renderer::End`. `Scene::RecordRouted` runs without it. The record reads only the payload,
CS's frame and the snapshot (decision 224: no game-mutated engine reads in the record half, no engine render calls).

After (perf-dragonsreach): lock waits 0.02% of samples, game 60.0 updates/s in every marked window, 222.7 / 226.7 fps (base 219.9 /
218.3 / 219.7). perf-taa: 60 updates/s, 143.6-145.4 fps (base 142.3-143.7). native-interior: 59.9-60.1 updates/s in Dragonsreach
(the base run had 41.2, 6.8 and 29.6).

Not a new deadlock: every producer that parks the render thread (Main::DrawFrame, the loading screen, movie ticks, the game thread's
inline interface) parks before it takes the lock.

## 3. Item 2: portal main-context stamp

What was wrong (two faults):
- `PortalViews::Build` runs in Main::Update's portal walk, before native's game half takes its step. It stamped the main walk with
  `SceneStep()`, which on the game thread outside the game half is the render thread's step. The light selection keeps the main
  context only when its frame equals the selection's (the game half's step), so native's atlas never had the camera context.
- The light context atlas validated its contexts against the commit's frame: the render's step. The contexts carry the game half's
  step, which only a ticket's first render shares when no step was taken since. Every repeated render rejected the atlas:
  native-interior at base logged "portal atlas: mixed context frames" 8192+ times. snap7-thread2's NextSceneStep stamp only made the
  camera context take part, which exposed the same rejection outdoors.

Fix:
- `PortalViews::StampMainStep`: the point shadow source's frame start (game half, in the ticket build, before any selection) stamps the
  main walk and a copy of its context with the game half's step. Native only (`InGameHalf`).
- `LightContextResources::Commit` validates against `selection->sceneFrame`. In guest that equals the commit frame, so guest is unchanged.

After: 0 rejections in native-interior, llf-local-shadows, native-tour, cadence-drop-cells.

## 4. Item 3: frame-start reads

Native quick with `CS_SNAPSHOT_DETECT=1`: frame start **3.01 -> 0.05** per world frame.

- `FirstPersonView.cpp:55`: kPOST_ZPREPASS_COPY's DSV format comes from `Targets::ViewFormat(Depth(kPOST_ZPREPASS_COPY, Dsv))`.
- `Resources.cpp:1106` (`ImportViewDepthTarget`): the import is `Targets::Slot(Depth(kPOST_ZPREPASS_COPY))`, stable for the session.
  `FrameAnchors::ShareImage` already returned this slot for a declared target, so it is the same resource.
- Observed before the switch (perf-dragonsreach): format 0 differences in 4096 checks; slot "same". The observe code is deleted.
- `GrassMirror.cpp:84`: grass's frame globals take a fade clock (`kFadeClock`: the engine's frame time, timerValues[1], summed once per
  capture, which is once per game update). The drain sets the mirror's clock from it (`TakenFadeClock`). This also fixes a fault:
  the drain added the frame time on every render, so repeated renders ran the spawn fade about three times too fast. Guest takes the
  same capture in the same frame before its drain, so its values are unchanged (except under the repeat cadence test mode, which now
  stops counting twice too).

Left (0.05 per world frame; fixed counts per session, not per frame): one-time reads when the shadow catalog and placements are built
(`ShadowCapability.cpp:62/157/166`, `ImportShadowDepth` in `Resources.cpp:1137`, 4 calls) and the reflection faces' format
(`ReflectionView.cpp:97`). The shadow import keys on the texture pointer because Skylighting swaps its own texture into
kPRECIPITATION_OCCLUSION_MAP; moving it to target slots needs that case handled. Open item.

## 5. Runs (stage s7-pace)

| Label | Build | Config | Result |
|---|---|---|---|
| a-perftaa | base | native-vulkan+perf-taa, sampled | device lost at the load's held frame (snap7-devloss's signature, invalid read 0x4b3d000) |
| a-perftaa2 | base | native-vulkan+perf-taa, sampled | passed; 60 updates/s, 142-144 fps, no lock waits |
| a-dr | base | native-vulkan+perf-dragonsreach, sampled | passed; 25.3% lock waits, 46-59 updates/s, 218-220 fps |
| b-dr | items 1-3 + observe | native-vulkan+perf-dragonsreach, sampled | passed; 0.02% lock waits, 60 updates/s, 223-227 fps; observe 0 differences |
| c-quick-detect | 36a328fcc | native-vulkan+quick, detect | passed; frame start 0.05, record game-mutated 0.01, engine render calls 1.0 |
| d-perf-taa | 36a328fcc | native-vulkan+perf-taa | passed; 60 updates/s, 143.6-149 fps |
| d-native-interior | 36a328fcc | native-vulkan+native-interior | passed, 4 shots; 60 updates/s; 0 atlas rejections |
| d-llf-local-shadows | 36a328fcc | native-vulkan+llf-local-shadows | passed, 6 shots; 0 rejections |
| d-cadence-drop-cells | 36a328fcc | native-vulkan+cadence-drop-cells | passed, 3 shots |
| d-update-cap30 | 36a328fcc | native-vulkan+update-cap30 | passed, 3 shots; 30.2 updates/s at 179 fps |
| d-update-cap240 | 36a328fcc | native-vulkan+update-cap240 | passed, 3 shots |
| d-native-tour | 36a328fcc | native-vulkan+native-tour | passed, 3 shots |

Skipped: guest+quick and native D3D12. Guest's paths are unchanged by construction (the lock change is native's render thread only,
the stamp is native only, the atlas check compares equal values in guest, the grass clock takes the same value in the same frame).
Nothing touched ORG, BasicRHI or the native device.

native-interior's frame rate (110 fps) is not comparable with snap7-thread2's run (225 fps): the config changed at 08:56 (AI off, new
references) and is not uncapped. perf-dragonsreach, uncapped, is the A/B.

## 6. Decided by the agent, to confirm

- P-1. Renderer::Lock covers Begin and the volumetric/interface/End part of a routed render, not the record. Why: the engine code that
  takes it (decals, the task pool) touches nothing the record reads, and holding it made the game wait for whole renders. The render
  thread can still wait briefly at its two lock points while a game-side holder runs.
- P-2. The main portal context is restamped with the game half's step at the point source's frame start (native only), and the atlas
  checks its contexts against the selection's step. Why: both sides then carry the step of the game half that walked them. Guest keeps
  its off-by-one camera stamp, so guest's atlas still lacks the camera context; nothing indexes it (lights index their own), so it
  changes no draw, and guest stays unchanged.
- P-3. Grass's spawn fade clock is a frame-globals value summed once per game update. Why: a game-side value read through the ticket's
  globals, which also stops repeats from speeding the fade. First-person target reads use CS's target set (T2-d).

## 7. Open items

- The one-time shadow catalog, shadow import and reflection format reads at the frame start (section 4).
- The render thread can still wait on Renderer::Lock at its two lock points while game-side engine code holds it. Porting Begin's and
  End's bodies (TH-b) would remove the lock from the render side.
- Guest's off-by-one camera context stamp (P-2).
