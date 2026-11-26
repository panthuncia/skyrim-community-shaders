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

## Results (2026-11-25, items 1-5 done)

Same capture as the baseline (guest, river, frozen, Dev-Fast, `CS_ENGINE_ZONES=1`), on org_integration a1a6625b8 / ORG
358597f, with the machine idle:

| Bucket | Before (ms) | After (ms) |
|---|---|---|
| Window | 6.53 | 4.86 |
| ORG tickets | 3.59 | 2.44 |
| Engine code | 1.25 | 0.94 (with 259 zoned sites instead of 136, so more profiler cost) |
| DCLF CPU | 0.56 | 0.65 (reflection faces moved from the engine into DCLF) |
| `Deferred::*` loops | 0.45 | 0.25 |
| Other CS | 0.57 | 0.46 |
| Not in any zone | 0.12 | 0.12 |

What each lead did (notes in `perf-<name>.md` beside this file):

- **Item 1, tickets** (`perf-tickets.md`): five CS stages now go to DXVK in one emission with the DCLF epoch they precede
  (13.5 emissions a frame to 8.5). Tickets stay at 13.5: each graph still needs its own admission and commit, and merging
  DCLF's graphs with CS's was ruled out earlier (phase epochs).
- **Item 2, hazards** (`perf-hazards.md`): writer fingerprints, three captures per prediction slot, no source cap, quiet
  grids skipped in stream commits. Full resolves 5.6 a frame to 0.1; hazard zones 0.97 to 0.35 ms.
- **Item 3, uploads** (`perf-tickets.md`, `perf-smalls.md`): upload lists only for tickets with uploads (13.5 to 2 a
  frame), upload uses kept per backing set.
- **Item 4, DCLF inputs** (`perf-dclf-inputs.md`, `perf-commit-inputs.md`): feature loops skip empty callbacks, kept frame
  texture answers, pool firsts skipped in a steady scene. The commit is not a pointer swap: what is left settles there.
- **Item 5, engine work** (`perf-engine-skip.md`, `perf-reflections.md`, `perf-post-native.md`, `perf-post-slots.md`):
  sun occlusion query native; water reflection faces native (`CS_DCLF_REFLECTION_NATIVE`); native post chain families
  0-4 plus slot effects 1, 3 and 5 (round 2, perf2-post.md: slots 6 and 7 native too, native on SE as on AE, `CS_ORG_POST_NATIVE` deleted); native clears for rt12 and
  rt88.
- **Leftovers** (`perf-smalls.md`): rebinding checks kept, latch copies only over live blocks, repeated texture index
  lookups kept.

Open:

- Post chain slots 6 (full-screen blur) and 7 (get hit) still use the engine's RenderEffect; the underwater DoF forms
  and LDR slots were never reached in an observe run.
- SE: native post chain needs one observe run on a 1.5.97 game folder before it becomes SE's default.
- Native reflection faces: about 0.1 ms of per-face lookups left (Physical Sky's `ResolveOrgHandles` per face, a per-face
  D3D11 Tracy zone in `ReflectionsPrepasses`).
- Engine self that is left: Scaleform HUD 0.13, shadow caster culling and shadow maps 0.10, image-space steps around the
  world draw (AE 107146, SSAO, AE 107285) about 0.13.
- Seen once each, not reproduced: a DCLF uncovered-epoch tap error after a Z-prepass rebuild (observe run), and a
  device loss after a teleport with frame generation on (guest+fg).
- `player.setpos` does not move the player in cs-test runs, so the underwater rt12 clear has no readback yet.
- Confirm the ranking in an optimized build: CS and ORG code shrinks about 3x there, the engine does not.

## Round 2 (2026-11-25, evening)

The same capture (guest, river, frozen, Dev-Fast, `CS_ENGINE_ZONES=1`), now on cs-test's lab slots. The window from DCLF's
scene tables to Present:

| Point | Window (ms) |
|---|---|
| Round 1 start | 6.53 |
| Round 1 end | 4.86 |
| org_integration d49225e30 (ledger, ticket, post, faces merged) | 3.84 |
| org_integration 4f2d97b79 (plus dclf, native, frame, ticket3, clients) | 3.43 |

At 4f2d97b79: ORG epochs 1.44, other CS 0.75 (it now holds time that used to show as the Deferred loops, 0.22 -> 0.04,
because lead frame zoned the anchors), engine code 0.59, DCLF CPU 0.50. The shadow mask port (747f16806) came after it.

What each lead did (notes in `perf2-<name>.md` beside this file):

- `strip`: 22 switches and two debug modes deleted with their old paths; the inventory table is in `perf2-strip.md`.
- `ledger`: cross-frame access cells only grow (a read waits on every writer, a write on every writer and reader), a
  ledger-wide change stamp skips the walks, state commits skip unchanged grids, one alias scan per admission. Ledger zones
  0.66 -> 0.24 ms; cross-queue waits unchanged at 2 a frame.
- `ticket`, `ticket3`: the pinned copy step (no WaitTicket), kept handoff lists, early outs for empty uploads, no per-ticket
  allocation, copies recorded without a map. About 0.37 + 0.05 ms.
- `post`, `native`: the whole post chain is native, slots 6-7 and families 5-6 (SSR, SAO, water blend) included;
  `CS_ORG_POST_NATIVE` and the old `CS_ORG_IMAGESPACE_NATIVE` modes deleted; SE gets AE's defaults (not run on SE).
- `faces`: native-only reflection faces and sky (both switches and the sky latch path deleted); prepasses once a frame; sky
  headers from the features. Native faces 0.335 -> 0.267 ms.
- `dclf`: fades counted before the list wait, the batched portal evaluator deleted, skin materials kept until they change
  (0.238 -> 0.099), water members indexed densely, forward sort rows filled by the producer. Scene tables 0.99 -> 0.79.
- `frame`: anchors walk only their clients, shared data packed once, the engine zone install race fixed (it could crash
  captures).
- `clients`: colour space matrices once, Scaleform and overlay commit work kept. Client commits 0.20 -> 0.16.
- `shadow`: native shadow masks (1800/1800 batches matched in a compare build); engine mask self 0.033 -> 0.004.

Other fixes this round: BasicRHI's lowering identity hashes its sources instead of `__TIMESTAMP__` (every worktree's build
discarded the shared pipeline identifier store, about 19 s of rebuild per run); cs-test runs games in parallel lab slots
(README, "Lab"); `stage --wait`.

- `finish`: guest mode finishes external tickets on the submission thread; DXVK gained deferred external commands
  (`dxvkEmitDeferredExternalCommands`, DXVK org_integration 94063a21) so DXVK's CS thread records an epoch once its tickets
  finish. FinishTicket was 0.77 ms a frame on the render thread. Window 3.60 -> 2.67 ms, ORG epochs 1.50 -> 0.73. Guest
  mode now runs DCLF's async passes on the graphics queue (as native does): with them on DXVK's compute queue, FSR frame
  generation deadlocked 8 of 8 runs (its present holds DXVK's submission lock while waiting on that queue). The user chose
  the graphics queue for now; ORG getting its own compute queue would bring async compute back.

Open:

- ORG's own compute queue in guest mode, then async compute back on; a GPU A/B of async on and off is still unmeasured.
- Graph installs at a frame revision block the render thread in `StopFrameProduction` (median 0.26 ms, p99 70 ms, twice
  35-41 s in 31k attaches); root cause unsettled (perf2-finish.md).
- The engine's shadow maps and caster lights (about 0.13 ms): `perf2-shadow.md` has the next step.
- The Scaleform HUD (0.06), the remaining anchor clients (0.13), ORG's thin per-epoch remainder.
- SE: every default now matches AE, and every new address is an SE/AE pair, but nothing has run on SE.
- All numbers are Dev-Fast. An optimised build is still to confirm the ranking.

## Round 3 (2026-11-25, night)

Round 2's open items, plus what the leads found on the way. Notes in `perf3-<name>.md` beside this file. Window (same capture,
Dev-Fast): 2.62 ms on lead anchors' unchanged base, 2.54 ms after it; the other leads did not move the steady window.

- `install`: the 35-41 s installs were priority inversion. The render thread waited in `StopFrameProduction` on executor
  workers at below-normal priority, which a busy machine only runs on Windows' starvation boost. Installs no longer wait:
  the old graph is retired and released once its GPU work is done and its task scopes are idle; revisions hold while 64
  replaced graphs are still held. Attach wait p99 76 -> 9 ms, max 40.5 s -> 11 ms.
- `teardown`: retired graphs are torn down off the render thread (alias placements given back on the host thread, the
  rest on ORG's deleter thread). Any thread waiting on an executor scope raises the workers to its priority
  (`Executor::WaitBoost`). Under 16-process load: attach p99 10.7 -> 4.6 ms, render-thread teardown 16-104 ms -> none.
  The four ORG tests that hung (Leases x3, HeadlessBenchmark) are fixed; all 50 pass.
- `compute`: ORG owns the queue choice; DCLF only marks passes async. A logical queue kind with no queue of its own folds
  onto the queue it shares, same timeline, no semaphore waits. DXVK never hands ORG a queue anything else submits to, so
  one shared compute queue and none look the same to ORG. ORG's submitters take DXVK's narrow submission lock
  (`dxvkLockQueueSubmissions`), not the lock DXVK holds through presents. Guest without a DXVK compute queue no longer
  crashes at boot. `CS_DCLF_ASYNC_COMPUTE` deleted. Own queue vs fold in guest: GPU 6.6 vs 6.8 ms (3%).
- `fg`: the guest+fg hang with the own queue was a whole-device wait while DXVK's submissions were held (DXVK's
  `waitForIdle` during swapchain rebuilds, and FFX's `vkDeviceWaitIdle` inside its present), while ORG's compute batches
  waited on stream points DXVK had not submitted. DXVK now waits only on its own queues; sl.fsr_g's device wait is a no-op
  after FFX waited on its own queues. The device loss at FG off: the swapchain is destroyed with DXVK's queues idle and
  held, FFX's game-queue submits take `dxvkLockPresentSubmissions`. Guest uses DXVK's compute queue when it exports one;
  `CS_ORG_QUEUE_TOPOLOGY` deleted.
- `shadow`: lights whose views DCLF draws whole skip the engine's shadow-map `Render`; CS does its bookkeeping (targets,
  slices, free mask, light transforms, clears). Parity build: 4201 of 4201 lights identical. About 0.016 ms net.
- `anchors`: the graph-frames lookup per commit and the per-tap shared-pointer list are gone; runs list only active
  clients. `CS.FrameAnchors.Run` 0.250 -> 0.212 ms. ORG's per-epoch remainder has no contained cut left outside the
  submission code.
- `se`: first runs on SE 1.5.97 (guest and native Vulkan pass). Every address added since round 1 checked in Ghidra
  against SE. Fixed: the lab helper for SE's SKSE, the full-screen blur adapters on SE (older upstream bug). SE runs use
  `D:\CSLab-SE` (cs-test README).
- `stability`: the profiler's timestamp switch no longer refuses staged builds (the mode is a build input; a change
  requests an async rebuild); it was fatal when a load started as the profiler went idle, on AE too. The "broken
  exterior" on SE was the harness shooting the loading screen; steps now wait for a scripted shot to save.

Release profile (optimised, LTCG; ORG pass timestamps off), final round 3 tree, same capture (`perf-tools/r3-rel.buckets.txt`):

| Bucket | Dev-Fast (anchors after2) | Release |
|---|---|---|
| Window | 2.544 | 1.306 |
| ORG epochs | 0.700 | 0.319 |
| other CS / features | 0.670 | 0.326 |
| engine code | 0.547 | 0.329 |
| DCLF CPU | 0.493 | 0.244 (0.057 of it waiting on the list jobs) |

CS's own code is about 2.2x faster optimised, so the engine's code is now as large as any CS bucket. The top CS zones stay
the same: `CS.Anchors.ClientCommit` 0.049, `CS.PostNative.Stage` 0.037, `CS.Stage.CopyLatch` 0.026, `ORG.Host.CommitTicket`
0.023, `CS.State.UpdateSharedData` 0.024. Engine: the Scaleform HUD 0.042, 106337 0.031, the shadow caster lights 0.030.
The Release build needed fixes first: it compiles each file alone at warning level 4 (C4459 shadowing in five places, one
file that took vulkan.h before volk), and BasicRHI's SPIRV-Tools precompiled header failed under the Visual Studio generator.

Open:

- The attach still holds the async host thread briefly (`HoldAsync`); the host thread could swap graphs itself.
- FFX's presenter thread presents on its own queues without a lock (as in every round). Native FG with the compute queue
  is untested. Native modes now use the compute queue; round 2 measured that 0.1 ms slower there, not re-measured.
- The guest8 load hold on SE (materials waiting for streamed texture windows) did not come back; candidates in
  `perf3-stability.md`.
- The anchor commit bodies (0.10 ms, post chain and HUD largest), `CS.Anchors.SharedConstants` per step, the Scaleform HUD
  (native HUD is the only cut).
