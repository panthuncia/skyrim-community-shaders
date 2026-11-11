# P5.12: the loading screen and the startup logo on the render graph

Status: written, not built or run yet. Switch: `CS_ORG_LOADING_THREAD=1` (default off: anchors run on the main thread only,
as before).

## What the engine does (Ghidra, SE 1.5.97 and AE 1.6.1170)

| Function | SE ID / addr | AE ID / addr | What this port relies on |
| --- | --- | --- | --- |
| `Renderer::Lock` | 75458 / 0x140d6a080 | 77243 / 0x140e44550 | `EnterCriticalSection(Renderer + 0x2790)` (SE) / `+ 0x27f0` (AE); nothing else. CommonLib's `Renderer::GetLock()` reads the same offsets |
| `ServingThread::SetLoadingScreenActive(bool)` | 34552 / 0x140576410 | 35407 / 0x1405fc050 | `true` (mode 0 only): state +0x68 = 2, mode +0x6c = 2, `SetEvent(+0x58)`, return. `false` (mode 2 only): +0x68 = 0, then `WaitForSingleObjectEx(+0x60, 1)` until the loop returns, pumping window messages meanwhile (SE `FUN_1405b5fc0`, AE `FUN_1406499f0`: `PeekMessage`/`Dispatch` only, no drawing), then mode = 0. UI's spin lock is let go around the wait and taken again |
| Callers | 13214 / 0x140157710 (+0x11A, +0x401) | 13363 / 0x1401a1150 (+0x11A, +0x401) | the load coordinator: `true` before the load, `false` after |
| The loading loop | 38085 / 0x14063fa50 | 39039 / 0x1406d2120 | per iteration: `Renderer::Lock`, `Begin(0)`, the clears, `UI::Render`, `End` (Present), `Renderer::Unlock`. `MoviePlayer::WaitForSequenceToEnd` before the loop |
| `MoviePlayer::PlaySequence` | via 87896 / 0x1410a14d0 | via 90265 / 0x141196190 | the startup logo: holds `Renderer::Lock` for the whole sequence on the "MoviePlayer" thread (p5/bink.md, "Threads") |

So every frame the engine renders is inside `Renderer::Lock`, on one thread at a time:

- the main thread's world and menu frames (35556 / 36555 hold it from before `Begin` to after `End`);
- the loading loop's iterations on the ServingThread, between `SetLoadingScreenActive(true)` and the return of
  `SetLoadingScreenActive(false)`; the main thread does not render then (it loads), and after `false` returns the loop has ended;
- the startup logo's frames on the MoviePlayer thread, which holds the lock from the first frame to the last.

The lock is recursive and is also taken briefly outside frames (memory free, screenshots, console commands, R6.3). None of those
reach an anchor.

## The rule: the render owner

`FrameAnchors::OnMainThread` becomes `FrameAnchors::OnRenderThread`: true on the thread that owns rendering now.

- **Switch off:** the main thread only, as before (the one-time `a Present on thread N ... not the main thread` line).
- **Switch on:** one owner at a time, the main thread at the start.
  - The owner answers true unless another thread holds `Renderer::Lock` (that thread renders now; the owner is not inside a frame).
  - Another thread answers true when it holds `Renderer::Lock` itself (`OwningThread` of the critical section is its id): it
    takes ownership. The loading loop's first anchor, Scaleform display or Present of an iteration, or the logo's first frame,
    does this.
  - The main thread takes ownership back at the first call it makes holding the lock (its next frame), or with the lock free,
    and at once when `SetLoadingScreenActive(false)` returns (the handoff hook, below).
  - Every other call answers false. A false answer is always safe: the caller keeps the engine's or D3D11's path for that call.
- **The claim** happens under the anchors' mutex (`FrameAnchors`), so no claim lands inside another thread's anchor run.

Why the lock is enough: the engine never renders on two threads at once, and the lock's release and acquire order every write
the previous owner made before the next owner reads it (the anchors' clients, the host's slot rings and tickets, the upload
instance's queue, CS's runtime state).

### What moves with ownership

- **The frame upload instance.** ORG's `UploadInstance` keeps an owner thread and logs a queue operation off it
  (`UploadInstance '...': ... called off the owner thread`). The owner is the submitting thread (`GraphHost::SubmitTicket`
  records and retires uploads there). A claim calls `RenderGraphRuntime::MoveOwnerThread`, which calls
  `GraphHost::Uploads()->SetOwnerThread()` on the new owner. No ORG change is needed: `IUploadService::SetOwnerThread` exists.
  The old owner then reaches the instance only through its mailboxes (`UploadData` off the owner posts), which is ORG's
  supported producer path.
- **The host and the anchors' state.** `GraphHost` has no thread id of its own: "the render thread" is whoever submits.
  `RenderGraphRuntime::ExecuteEpoch` already takes the calling thread as DXVK's stream thread (`streamThread`) at each epoch,
  so the in-stream submission hook follows the owner.
- **Per-Present work.** `SignalFramePace`, `SealPresented`, `WaitFramePace` and DCLF's `BeforePresent`/`EndFrame` already run on
  every Present, the loading thread's included; nothing changes there. Frame pacing counts the loading loop's Presents like any.
- **Frame slots.** Each run of an anchor takes the next slot of its epoch's ring; the rings count Presents (`SealPresented`),
  which the loading loop makes. A loading iteration is one frame like any other.

Ownership itself requests no graph build: the claim publishes nothing and touches no client.

### The handoff hook

`SetLoadingScreenActive` (34552 / 35407) is detoured when the switch is on (`RenderGraph/Engine/LoadingScreen`):

- `true`: logged (`[ORG] loading screen: the serving thread renders until the load ends`). The serving thread claims at its
  first call under the lock.
- `false`: after the engine's function returns (the loop has stopped), the main thread takes ownership back and logs how many
  claims the load made.

## Explicit builds and Drawcall Limit Fix during a load

- DCLF draws only in world frames. A loading iteration runs no world frame (no `Main_Draw_Early`, no `FrameDriver::BeginFrame`),
  so DCLF's epochs never run there and its build point (`IndirectDraws::BuildPoint`) does not run either. Its Present work
  (`BeforePresent`, `EndFrame` with `Running()` false) is what it does at any menu Present: no change.
- With DCLF on, builds are explicit (`SetExplicitBuilds`): a rebuild an anchor asks for during the load waits for DCLF's next
  build point, the first world frame after the load. The loading thread builds nothing by itself: a synchronous build on a
  frame's path is what the build rule forbids, and the handoff must not cause one.
  - A client already in an anchor's built graph runs on the loading thread at once (Scaleform's framebuffer surface from the main
    menu, the Present anchor's clients).
  - A client that registers during the load waits (the anchors' joining rule still runs the epoch so the host can build it where
    builds are not explicit; with DCLF on it waits for the build point). Its caller keeps the D3D11 path meanwhile, as everywhere.
- Bink is the exception that already exists: a blocking movie runs no world frame, so `Bink::Graph` calls
  `host->BuildIfRequested()` once while its client waits to join (p5/bink.md). On the MoviePlayer thread this is one build at
  the logo's start, before any world frame exists.

## Rebuilds in this lane

| Source | What it does | Handling |
| --- | --- | --- |
| The ownership handoff | nothing | no build, no publish (above) |
| Bink's join at the logo's first graph tick | one `BuildIfRequested` | unchanged (p5/bink.md); before the first world frame |
| Texture streaming's copy pass per batch (`ORGWindows::Register`, one-shot) | asks for a Present-anchor build per batch and one after it ran | pre-existing (p4/texture-streaming.md, "Risks"). On the loading thread, with DCLF on, those builds wait for the build point, so a batch staged during a load is uploaded in the first world frame, as today; with DCLF off they are built at the next submission, as in any frame. Removing them at the source (a kept pass with a gated copy list in place of a one-shot pass per batch) is texture streaming's change, listed as open |
| Scaleform's new surface (a display onto a target with no pass yet) | registers the client again | p5/scaleform.md's design; on the loading thread the display goes to D3D11 until the graph holds it |

## Hazards

- **Two threads in the graph at once.** Excluded by the engine (one frame at a time, under the lock) and by the rule (a
  non-owner is refused; the claim takes the anchors' mutex). Calls the main thread makes during a load outside any frame
  (`Register`, `Unregister`, `Refresh`, `Rearm`, imports from a feature's setup) take the anchors' mutex too, so they never
  interleave with a loading-thread run. They only publish a new client list, which the next build takes.
- **Main-thread host use during a load outside FrameAnchors** (DCLF's scene coordinator, LLF's epoch): none runs in a load,
  since both are inside world frames. Producers on job threads post uploads, as they do in any frame.
- **Lock order.** The anchors' mutex is taken inside `Renderer::Lock` on the rendering thread and never the other way round:
  nothing under the mutex takes `Renderer::Lock`.
- **A display or tick that starts on one thread and ends on another** cannot happen: a Scaleform display and a Bink tick both
  start and end inside one locked frame.
- **The main thread calling an anchor before its first locked call after the logo** is refused and draws through D3D11 for
  that call.

## What follows the rule

| Caller | Before | Now |
| --- | --- | --- |
| `FrameAnchors::Run` | main thread only | the owner |
| Texture streaming's `FrameTick` (`ORGWindows.cpp`) | skipped on the loading thread | runs on the owner: retired batches and windows are serviced and new batches registered there |
| Scaleform's capture (`BeginDisplay`, `EndDisplay`, `Capturing`) | D3D11 off the main thread | captured on the owner, **but kept on D3D11 on the loading thread** (below) |
| Bink's tick, shots and movie end | engine off the main thread | the owner: the startup logo takes the graph path |
| Engine constants, the engine skeleton, DCLF's sky capture | main thread only | the owner (the same call, renamed) |
| `PublishedResources`' thread check | the first thread that used it | the owner |

## Scaleform on the loading thread (device loss, open)

First runs (2026-11-11, RX 6600, `loading-org1.env`): ownership moved as designed, the logo drew 165 of 166 frames on the graph,
and no off-owner warning appeared. But the device was lost about 6 s after the first save load ended, in Drawcall Limit Fix's
first world frames, on 3 of 3 runs. With `CS_ORG_SCALEFORM=0` the run was clean; with `CS_ORG_TEXTURE_STREAMING=0` it still
failed. So the trigger is Scaleform's displays on the serving thread (about 790 Interface epochs during a load). DXVK's hang
report names no D3D11 work.

Until the cause is found, `BeginDisplay` keeps the loading thread's displays on D3D11 (counted as `on the loading thread
(loading-screen displays stay on D3D11)`). `CS_ORG_SCALEFORM_LOADING_THREAD=1` puts them back on the graph, for diagnosis
only. The logo, texture streaming and the other anchors keep the owner rule.

What was checked and ruled out by reading the code:

- **GpuTextures' cleanup queue.** The active descriptor service is thread-local
  (ORG `DescriptorServiceAccess.h`), but every graph takes the runtime's services (`RenderGraph::UseRuntimeServices`), so an
  entry made on the serving thread lands in the same queue as one made on the main thread. And `ResourceCleanupQueue` only moves
  destruction elsewhere (`ResourceCleanupQueue.h`, `Make`): GPU lifetime comes from the recordings' `Retain`, not from the queue.
- **The frame upload instance.** Moving its owner makes the serving thread record the uploads other threads posted. Drawcall
  Limit Fix does not post there: it stages batches (`Payload/Uploads.h`) and uses the dedicated streaming queue
  (`FrameValues.cpp`). The engine's buffers are persistently mapped (`Buffers/EngineBuffers.cpp`), and the engine's textures
  get their contents in DXVK's stream (`Textures/EngineTextures.cpp`).
- **Scaleform's frame slots.** `Commit` stores at `CurrentFrameSlot()`, which is the slot `SubmitTicket` sets for the ticket
  that `Record` then runs for (`GraphHost.cpp`, `m_asyncSlot`). A frame, its staging buffer, its descriptors and views are held
  by shared owners until the recording that names them retires. Nothing here depends on the thread.

What differs only when the loading thread draws Scaleform, and is the next thing to test:

- **Pinless imports during a load.** `ResolveBinding` imports each display texture pinless (`GpuTextures.cpp`, `Import`:
  `TrackResources`). DXVK may move a tracked image while it stays imported (`DxvkOrgInterop.h`, "Pinless imports"), and
  moves are polled only at DCLF's frame start (`FrameDriver.cpp`, `ImportRelocations::Poll`), so during a whole load the
  serving thread's draws read old storages and the moves pile up. At the first world frame they are all taken at once; a move
  with no registered import is held only 2 s (`NoteRelocations`, `kUnclaimedFor`). In the switch-off runs no import is made
  during a load, so this path never sees a load's worth of moves. **Test:** `loading-org1-sfload.env` (the displays on the
  graph again) against `loading-org1-sfload-pinned.env` (the same with `CS_ORG_PINNED_IMPORTS=1`). If pinned imports are clean,
  the fix is in the pinless path. The likely fix would be to poll relocations at every Present of the render owner, not only at
  DCLF's frame start.
- **The first displays of the session on the serving thread.** In the failing runs the Scaleform surfaces, samplers,
  pipelines, the GpuTextures registry, and the publication of `Engine::kMAIN` / `Engine::Depth::kMAIN` were all made on the
  serving thread during the initial data load (before the main menu). In the switch-off runs they are made in the main menu.
  If the pinned run also fails, the next test is to make these on the main thread first: keep the displays on D3D11 until the
  first main-thread display has set up the path.

## Testing

Scenarios (`tools/cs-ab/scenarios/`): `loading-org0.env` (switch off) and `loading-org1.env` (switch on), both with the P4
switches and `CS_ORG_TEXTURE_STREAMING=1`, `CS_ORG_SCALEFORM=1`, `CS_ORG_BINK=1`, loads by `coc Whiterun`,
`coc WhiterunDragonsreach`, `cow Blackreach 0 0`, `dclf:load=recent`, and a shot after each.

In org1's log:

1. `[ORG] render ownership: the CS_ORG_LOADING_THREAD rule is on`.
2. At the logo: `[ORG] render ownership: thread N (holding Renderer::Lock) takes it from thread M`, then Bink's
   `a WxH movie ended: G frames drawn by the render graph` with G near 166.
3. Per load: `[ORG] loading screen: the serving thread renders until the load ends`, a claim by the serving thread, then
   `[ORG] loading screen: the load ended (K ownership changes); the main thread renders again`.
4. **No** `UploadInstance '...' called off the owner thread` line, **no** `PublishedResources used on thread` line, and no
   `a Present on thread ... not the main thread` line (that is the switch-off message).
5. Scaleform's counts: displays "off the main thread (loading screen)" fall to 0; loading-screen displays are counted as the
   graph's.
6. No `[E]` beyond the known one, no device loss, every shot equal to org0's.
