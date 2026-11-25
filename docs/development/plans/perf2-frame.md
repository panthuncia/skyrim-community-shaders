# Render thread round 2: CS frame code (lead frame)

The CS code around the features: `Deferred::*`, `State::UpdateSharedData`, the frame anchors' runs, the engine constants capture and
the feature callbacks. All timings are Dev-Fast (/Od), guest mode, the frozen river scene (`trun.sh guest+quick ...
--set env.CS_ENGINE_ZONES=1`), per frame.

## Where the time was

The baseline (`p2-frame-base`, branch at d49225e30 plus zones only) had a 4.16 ms window, with "other CS / features" at 0.85 ms.
New zones showed what the `Deferred::*` self time hid:

| Zone | ms | Calls | What |
|---|---|---|---|
| `CS.FrameAnchors.Run` | 0.34 | 29.5 | The anchors' taps. No ORG zone under it: the time is CS's |
| of which client commits | 0.18-0.20 | 44.5 | `cs.pp` 0.033, `cs.scaleform` 0.025, `cs.imgui` 0.014, then about 35 clients at 1-7 us each |
| of which the client walk | 0.044 | 29.5 | Every run walked every registered client to find its anchor's |
| of which shared constants | 0.031 | 23.5 | Four blocks into each step's latch, plus a copy of each kept for a check that is off |
| `CS.State.UpdateSharedData` | 0.12 | 4 | Reflection prepass, early prepass, deferred start, and `Upscaling::PostDisplay` |
| of which the feature buffer | 0.067 | 4 | 25 features packed, kept and uploaded on every call; Procedural Aurora's pack alone 0.012 |
| `CS.Engine.Constants.Capture` | 0.042 | ~20 | One shadow mask draw 0.017 (its first capture of the frame), the rest under 4 us each |
| `State::Draw` | 0.023 | ~20 | Per engine draw; already lean |
| `Deferred::*` self | 0.011 each | 1 | Mostly the D3D11 Tracy GPU zone each opens |

## What changed

1. **Anchor runs walk only their own clients.** `Publish` (which follows every change to the client list) now also makes each
   anchor's client list, so `FrameAnchors::Run` no longer walks every registered client per tap.
2. **Shared constants keep no copy unless checked.** `SharedConstants::Commit` copied all four blocks into vectors on every one of
   its 23.5 commits a frame for `CS_ORG_SHARED_CONSTANTS_CHECK`. It copies them only while that check is on.
3. **Feature buffer.** `UpdateSharedData` uploads `featureDataCB` only when the packed bytes differ from the last upload (which
   `lastFeatureData` already mirrors for Drawcall Limit Fix). The pack takes its blocks by reference, and `lastFeatureData` is
   overwritten in place rather than reassigned.
4. **Procedural Aurora packs once a frame.** Its pack advanced the animation clock and ran the OKLCh conversions on each of the
   four packs a frame, so one frame's passes saw slightly different times. Every pack of a frame now returns the frame's first
   block.
5. **CS's D3D11 Tracy GPU zones are opt-in.** The 46 `TracyD3D11Zone`s on CS's own D3D11 work (Deferred, SSGI, SSS, Skylighting,
   Terrain Blending, Upscaling and others; not DCLF's) became `CS_GPU_ZONE`. Each is a D3D11 query pair. They open only with
   `CS_FEATURE_GPU_ZONES=1`, which already gated the per-feature ones. In the graph modes the work they would time runs on the
   graph, so they timed nothing there.
6. **Zones.** `CS.FrameAnchors.Run`, `CS.Anchors.Tap`, `CS.Anchors.ClientCommit` (the client id is the zone text),
   `CS.Anchors.SharedConstants`, `CS.Anchors.Submit`, `CS.State.UpdateSharedData`, `CS.State.SharedData.FeatureData`,
   `CS.Deferred.*` and `CS.ViewAtmosphere.*` around the deferred passes' inner calls.
7. **Engine zones install safely** (`src/EngineZones.cpp`). The install runs at DataLoaded on the main thread while the render
   thread is already drawing the main menu. It used `write_call`, which rewrites a live 5-byte CALL non-atomically. One capture
   (`p2-frame-after1`) crashed on an int3 in the trampoline's padding, a torn call. Now the thunk's target and its jump are written
   first. The displacement then goes in with one 16-byte compare-exchange. When it crosses a 16-byte line (about 1 site in 6),
   the site spins on itself (`jmp $`) while its tail is written, then gets its new head. All 259 sites install as before.

## Measured

Captures were noisy: other leads' games and builds ran during them. `p2-frame-after4` ran about 16% slower across the board,
engine self included (0.93 ms against 0.80). So the figures below compare zones inclusively and give after4 scaled by the engine
self ratio. `p2-frame-after3` ran on a quiet machine but before the install fix, with 44 engine sites unwrapped, so its window and
buckets do not compare. Its per-zone figures do.

| Zone (inclusive) | base | after3 (quiet) | after4 scaled |
|---|---|---|---|
| `CS.State.UpdateSharedData` (4 calls) | 0.122 | 0.093 | 0.093 |
| `CS.State.SharedData.FeatureData` | 0.067 | 0.042 | 0.043 |
| `CS.FrameAnchors.Run` (attr capture, same inner zones: 0.369) | 0.337 | 0.304 | 0.344 |
| `Deferred::EarlyPrepasses` | 0.110 | 0.097 | 0.101 |
| `Deferred::PrepassPasses` | 0.132 | 0.114 | 0.138 |
| `Deferred::DeferredPasses` | 0.055 | 0.049 | 0.055 |
| `Deferred::ReflectionsPrepasses` | 0.070 | 0.065 | 0.067 |
| Other CS / features (bucket) | 0.847 | 0.746 | 0.839 |

The cuts total about 0.06-0.08 ms Dev-Fast. That is below the noise between captures (0.1 ms or more this afternoon), so the
window change was not measurable. The per-zone drops repeat in both after captures. Inside Run, its own time and the client walk
went from 0.091 to 0.060 ms (attr against after3, the same inner zones but two).

## Not done, and why

- **Client commits (0.18 ms).** These do real per-frame work: the post chain's per-step constants (`cs.pp`), the HUD's display
  list with a texture resolve per HUD texture (`cs.scaleform`), the overlay (`cs.imgui`). The rest are 35 clients at 1-7 us each,
  mostly /Od cost of staging a few values. No repeated work stood out. The Scaleform resolves are the best next target: they
  could reuse `GpuTextures`' per-frame cache across frames while the display's textures are unchanged.
- **Four `UpdateSharedData` calls a frame.** Each call reads state that can change between them: the reflection camera,
  TerrainShadows' values after its EarlyPrepass, the upscaled viewport after `PostDisplay`. Packing once a frame would hand some
  readers a frame-old value, and DCLF's next shadow epoch reads `PostDisplay`'s upload. Not changed without a visual check per
  consumer.
- **The deferred composite reads D3D11 views** (`CS.Deferred.CompositeInputs`, 0.014 ms) to decide which inputs are published.
  Moving that to the published registry's own answers is the native direction, but it changes `ORGDeferredComposite`'s key.
  Left for the composite's owner.
- **Engine constants capture (0.04 ms).** Per engine draw that is still engine-driven. Its cost goes as the native ports remove
  those draws.
- **Feature callbacks** (Skylighting's projection, Terrain Depth, LightLimitFix's light gather, Skin, Volumetric Lighting): each
  5-12 us of real work (engine projection calls, light lists). Nothing repeated.

## Decisions to confirm

- Decided by the agent, to confirm: `CS_FEATURE_GPU_ZONES` now also gates CS's own D3D11 Tracy GPU zones (Deferred, SSGI, SSS,
  Upscaling...), not only the per-feature ones. Tracy captures of plain D3D11 mode lose those GPU timings unless the switch is on.
- Decided by the agent, to confirm: Procedural Aurora's block is packed once a frame. Its animation clock advances once a frame
  instead of four times by sub-millisecond steps.
- Decided by the agent, to confirm: `src/EngineZones.cpp` (a measurement tool no lead owns) got the atomic install, since its
  race crashed a capture and would hit every lead's captures.
- Decided by the agent, to confirm: `src/RenderGraph/FrameAnchors.cpp` and `src/RenderGraph/SharedConstants.cpp` were changed.
  The director confirmed FrameAnchors.cpp; SharedConstants is CS's shared-data upload, next to `State::UpdateSharedData`.

## Tests

| Config | Label | Result |
|---|---|---|
| guest+quick, trun capture | p2-frame-base | ran to its exit step (baseline, zones only) |
| guest+quick, trun capture | p2-frame-attr | ran to its exit step (inner zones) |
| guest+quick, trun capture | p2-frame-after1 | crashed at the main menu: the engine zones install race (fixed in item 7) |
| guest+quick, trun capture | p2-frame-after2 | ran to its exit step; machine loaded (window 5.8 ms), not used |
| guest+quick, trun capture | p2-frame-after3 | ran to its exit step; 215 of 262 sites (first install fix) |
| guest+quick, trun capture | p2-frame-after4 | ran to its exit step; final build, 259 of 262 sites |
| guest+quick | p2-frame-v-guest-quick | passed, shot looks right (final build) |
| guest+hdr | p2-frame-v-guest-hdr | passed (final build) |
| native-vulkan+quick | p2-frame-v-native-vulkan-quick | passed, shot looks right (final build) |
| native-d3d12-quick | p2-frame-v-native-d3d12-quick | passed (final build) |
| guest-interior | p2-frame-v-guest-interior | passed (final build) |

Open: SE is untested at runtime (the game folder is AE 1.6.1170). Nothing here is AE-only; engine zones install on AE 1.6.1170 only,
as before.
