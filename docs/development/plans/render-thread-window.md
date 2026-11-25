# Render thread: from DCLF's scene tables to the present

Where the render thread's time goes between the end of `CS DCLF: scene tables` and the start of `CS.Present`, and how to cut
it. Measured on 2026-11-25.

## How it was measured

- Setup: RTX 5080, guest mode, FSR, the river quicksave, timescale 0, weather 81a, uncapped. Dev-Fast build (/Od), so CS and
  ORG code runs about 3x slower than in an optimized build. The engine is optimized either way.
- Each capture is 6 s of Tracy, about 680 frames. Commands: the session's `trun.sh guest+quick <label> --set
  env.CS_ENGINE_ZONES=1`, then `buckets.py` (per-frame self time by bucket) and `wtree.py` (call tree) over the main thread.
- `CS_ENGINE_ZONES=1` (new) puts Tracy zones on 136 engine call sites: the frame, the player view, world, depth, culling,
  shadows, water effects and post. With them on, 0.12 ms of the window is in no zone, down from 1.29 ms.
- Traps:
  - `CS DCLF: scene tables` and the epoch zones (`CS stage: ...`, `CS DCLF: ...`) are perf events. They reach Tracy only while
    debugger events are on, so with debug utils off the window has to start at the next zone,
    `Engine AE 76557 (in Main::RenderPlayerView)`, which starts 6 us later.
  - The game folder's `dxvk.conf` sets `dxvk.enableDebugUtils = True`, so debugger events are on in every run, cs-test's and
    hand-started ones alike. `DXVK_DEBUG=hang` (cs-test's default) turns them on too. Turning both off saved about 0.1 ms.
  - With Tracy connected, `ForEachLoadedFeature` puts a Tracy GPU zone (a D3D11 query pair) on every feature, so the
    `Deferred::*` loops read 0.45 ms. Without Tracy, the stack sampler puts them at 0.1-0.2 ms, mostly debugger-event
    formatting.

## Where the time goes

The window averages 6.5 ms (6.46-6.59 over three captures). The user's own run showed about 9 ms, so the scene or the
settings differ. The proportions are what matter here.

| Bucket | ms/frame | What |
|---|---|---|
| ORG tickets | 3.58 | 14.5 tickets a frame at about 250 us each, under `CS.ORG.ExecuteEpoch`. Includes the DCLF input commits that run inside them |
| Engine code | 1.25-1.35 | Self time of the engine zones |
| DCLF CPU | 0.56 | Outside tickets |
| `Deferred::*` loops | 0.45 | Mostly profiler overhead (see the traps) |
| Other CS | 0.36-0.57 | Engine constants capture, `State::Draw`, perf events |
| Not in any zone | 0.12 | |

### ORG tickets, 3.58 ms

By epoch (inclusive): Z-prepass 0.65, deferred+water 0.45, start 0.43, post 0.30, main opaque 0.28, shadow view 0.25,
pre-main 0.23, sun 0.20, pre-depth 0.18, water 0.16, post-water 0.10, water mask 0.10, water surface 0.09, forward 0.09.

By cost (summed over tickets):

| Cost | ms | Notes |
|---|---|---|
| Cross-frame hazards: commit | 0.50 | `CommitCrossFrameHazards`: every ticket, 35 us, walking every boundary access's cells |
| Cross-frame hazards: full resolve | 0.43 | `ResolveCrossFrameHazards`: 6.9 of 14.5 tickets miss the prediction, 61 us each (deferred+water alone 0.19) |
| DCLF inputs in the commit | 0.63 | `CommitInputs`: Z-prepass `CommitMainPayload` 0.26 (scene streams 0.10, reflection view 0.09), others small |
| Uploads | 0.54 | `PlanTicketUploads` 0.28, `RecordTakenUploads` 0.12, `CloseUploadList` 0.10, `RecordCopies` 0.06 |
| State ledger | 0.19 | `CommitStates`, 108 calls |
| Fixed per-ticket overhead | ~1.2 | `FinishTicket`, `ExternalExecution`, `TicketHandoff`, `ApplyFrameRebindings`, `AdmitTicket`, `SubmitTicket` self |
| Wait for the host worker | 0.09 | `WaitTicket`, in the sun stage, every other frame |

The prediction misses because a grid's fingerprint mixes in the timeline values of fixed points (commits outside a
stream). Those values advance every frame, so such a grid never matches its last frame.

### Engine code, 1.3 ms

| Function | Self ms | Notes |
|---|---|---|
| AE 32174, in `Main::RenderWaterEffects` | 0.24 | The engine's water reflection render, around CS's two `ReflectionsPrepasses` |
| `ImageSpaceManager::RenderEndOfFrameEffects` | 0.18 | The engine driving the image-space chain whose steps ORG stages natively |
| `Renderer::StartAccumulating`, in `Main::RenderWorld` | 0.17 | The engine's world accumulation, around CS's prepasses and DCLF's main opaque epoch |
| `MenuManager::DrawInterface` | 0.12 | Scaleform HUD |
| `FinishAccumulatingPostResolveDepth` | 0.06 | |
| SSAO step, shadow caster culling, shadow masks, sun query, the rest | 0.5 | Each 0.05 or less |

### DCLF CPU outside tickets, 0.56 ms

`ListJobsWait` 0.10 (waiting for the engine's culling jobs), `ExecuteForward` 0.08, the native sky builds 0.08, texture
binding resolves 0.05 (27 a frame), family capture for the remaining engine draws 0.08.

## Plan

In order of expected payoff. All the numbers above are Dev-Fast. Confirm the ranking with one Release-profile capture
before starting the ORG items: optimized ORG code shrinks those buckets about 3x, and the engine's share grows to match.

1. **Fewer tickets (about 1.5 ms Dev-Fast).** Six CS stages run back to back with the DCLF epoch they precede, inside one
   `ExecuteEpoch` call: start with shadow view, pre-depth with Z-prepass, pre-main with main opaque, deferred+water with water
   mask, water with water surface, post-water with forward. Admit, commit and finish each pair as one ticket (one ledger
   pass, one upload list, one handoff). That cuts 14.5 tickets to about 8, removing about 6 x 250 us of fixed cost. This is
   ORG work (a ticket over two graphs), and it moves toward the native frame redesign's regions.
2. **Cross-frame hazards (up to 0.9 ms).**
   - Make every ticket commit through a stream, so cells name producers rather than fixed points. Their fingerprints then
     stay the same from frame to frame, and the prediction holds: 0.43 ms.
   - Skip the commit's cell walk for a stream commit on the same backing set. The stream touches the same cells every time,
     so only the producer's signal changes: most of 0.50 ms.
3. **Uploads once a frame (about 0.4 ms).** Plan and record the frame's ticket uploads in one list at the first ticket, not
   one per ticket.
4. **DCLF inputs off the commit (about 0.3 ms).** `CommitMainPayload`'s scene streams and the reflection view's frame
   textures are prepared on the render thread inside the Z-prepass ticket. Prepare them on the revision lane, and leave the
   commit a pointer swap.
5. **Engine work that the native path makes redundant (up to 0.6 ms).** The next round of zones goes inside AE 32174,
   `StartAccumulating` and `RenderEndOfFrameEffects`, to split the engine's culling and accumulation from what DCLF and ORG
   already replace. Then skip those parts in graph modes: the reflection render once DCLF draws reflections, the
   accumulation once DCLF claims the world, and the image-space driver once the native post chain lands. That last one
   waits on the six decisions in `native-post-chain.md`.
6. **Smaller items.**
   - `ListJobsWait` (0.10): move frame-start work ahead of the wait on the engine's culling jobs.
   - The texture binding resolves (0.05): cache them per frame.
   - Measurement: drop `dxvk.enableDebugUtils` from the game folder's `dxvk.conf` unless a capture tool needs it.
