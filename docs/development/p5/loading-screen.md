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

`FrameAnchors::OnMainThread` became `FrameAnchors::IsRenderOwner`: true on the thread that owns rendering now. It is a pure
query; the claim is a separate, single point (reworked in the Phase 5 review: the first version claimed inside the query, from
the lock's `OwningThread`, so a mere thread check handed rendering to its caller).

- **Switch off:** the main thread only, as before (the one-time `a call on thread N ... not the main thread` line).
- **Switch on:** one owner at a time, the main thread at the start.
  - **The claim point** is `Renderer::Begin` (SE 75460 / 0x140d6a0c0, AE 77245), detoured: `FrameAnchors::ClaimRenderOwnership`
    makes the calling thread the owner. Its only callers are the main frame (SE 0x1405b1020, which takes `Renderer::Lock` just
    before), the loading loop (38085) and `BSMoviePlayer::Tick` when no frame is open (the logo). So the thread that begins an
    engine frame owns rendering until another thread begins one.
  - `IsRenderOwner` answers true on the owner only. Every other call answers false. A false answer is always safe: the caller
    keeps the engine's or D3D11's path for that call (the main thread between a load's end and its next `Begin`, a movie's
    clear before its `Begin`).
- **The claim** happens under the anchors' mutex (`FrameAnchors`), and `Run` checks ownership under it too, so no claim lands
  inside another thread's anchor run.

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
  first `Renderer::Begin`.
- `false`: after the engine's function returns (the loop has stopped), logs how many claims the load made. The main thread
  claims back at its next frame's `Renderer::Begin`; the hook itself claims nothing.

## Explicit builds and Drawcall Limit Fix during a load

- DCLF draws only in world frames. A loading iteration runs no world frame (no `Main_Draw_Early`, no `FrameDriver::BeginFrame`),
  so DCLF's epochs never run there and its build point (`IndirectDraws::BuildPoint`) does not run either. Its Present work
  (`BeforePresent`, `EndFrame` with `Running()` false) is what it does at any menu Present: no change.
- With DCLF on, builds are explicit (`SetExplicitBuilds`): a rebuild an anchor asks for waits for a build point. A loading
  iteration has no DCLF build point, so the frame's end is one (`FrameAnchors::OnFrameEnd`, after the Present's seal): it builds
  only when a client waits to join an anchor's graph and nothing else built during the frame. That build is between frames,
  never on a run's path, and the handoff itself never causes one.
  - A client already in an anchor's built graph runs on the loading thread at once (Scaleform's framebuffer surface from the main
    menu, the Present anchor's clients).
  - A client that registers during the load joins at the end of the iteration it registered in (`FrameAnchors::RequestJoin` is
    the gate). A joining client no longer runs its anchor's epoch. Its caller keeps the D3D11 path meanwhile, as everywhere.
- Bink no longer builds by itself: its one client registers at `State::Setup` and joins through `RequestJoin`, built at a
  frame's end like any joining client (p5/bink.md).

## Rebuilds in this lane

| Source | What it does | Handling |
| --- | --- | --- |
| The ownership handoff | nothing | no build, no publish (above) |
| A client that joins during a load | one build at the iteration's end (`OnFrameEnd`) | only while a client waits; a client that registers every iteration would rebuild every iteration (none does) |
| Bink's client | registered once at Setup | no build of its own; joins at a frame's end (p5/bink.md) |
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
| Scaleform's capture (`BeginDisplay`, `EndDisplay`, `Capturing`) | D3D11 off the main thread | captured on the owner |
| Bink's tick, shots and movie end | engine off the main thread | the owner: the startup logo takes the graph path |
| Engine constants, the engine skeleton, DCLF's sky capture | main thread only | the owner (the same call, renamed) |
| `PublishedResources`' thread check | the first thread that used it | the owner |
| Pinless import relocations (`ImportRelocations::Poll`) | DCLF's frame start | every Present of the owner |

## Scaleform on the loading thread: pinless imports

First runs (2026-11-11, RX 6600, `loading-org1.env`): ownership moved as designed, the logo drew 165 of 166 frames on the graph,
and no off-owner warning appeared. But the device was lost about 6 s after the first save load ended, in Drawcall Limit Fix's
first world frames, on 3 of 3 runs. It was clean with `CS_ORG_SCALEFORM=0`, and still failed with
`CS_ORG_TEXTURE_STREAMING=0`. With Scaleform's loading-screen displays on the graph and `CS_ORG_PINNED_IMPORTS=1`, it ran clean
(759 loading-screen displays on the graph, all shots). **The cause is in the pinless import path.**

- `GpuTextures::ResolveBinding` imports every display texture pinless (`Import`, `TrackResources`). DXVK may move a tracked
  image while it stays imported, copying its contents, and reports each move as a relocation that holds the old storage
  (`DxvkOrgInterop.h`, "Pinless imports"; DXVK `pushOrgRelocation`).
- The relocations were taken only at Drawcall Limit Fix's frame start (`FrameDriver.cpp`, `ImportRelocations::Poll`). A load
  runs no DCLF frame. So for the whole load the serving thread drew with imports whose moves were never taken: the descriptors
  read old storages, and DXVK held every old storage as a locked allocation through the load's VRAM churn.
- At the first world frame DCLF took the whole load's moves at once. Moves whose import had gone were dropped after 2 s.
- Without a load's worth of graph draws from imports (switch off, or Scaleform off), this never happens: DCLF's own imports
  are taken every world frame.

The exact access that faults was not isolated. The fix removes the gap: the moves are taken where the imports are drawn.

### The fix

- **One poll per Present, on the render owner.** `IDXGISwapChain_Present` (`Hooks.cpp`) calls `DCLF::ImportRelocations::Poll`
  when `FrameAnchors::IsRenderOwner()`, before the Present anchor. That covers the main thread's world frames and menus, the
  loading loop, and the logo. DCLF's frame start no longer polls. Its next frame sees the same state, taken at the Present
  before it: a move seen a frame later is safe, because the old storage stays valid until it is released.
- **Safe outside DCLF's frame.** `GpuTextures::NoteRelocations` works under the registry's lock. The buffer moves go to the two
  `GpuResources` inboxes (their own lock), which DCLF takes at its next `BeginFrame`. Nothing else in `Poll` reads DCLF's frame
  state.
- **The 2 s hold can no longer drop a move an import still needs.** A move nobody claims at the poll is kept for an import that
  was described before the move but registers after it.
  - Any registered import that reads the old storage gets it from `Supersede`, and holds it behind its recordings.
  - The gap was an import whose description took longer than 2 s. That is `TrackResources`, a synchronous chunk on DXVK's
    CS thread, which a load keeps busy.
  - Every import now holds a ticket from before its description until after its registration (`Registry::importsInFlight`). An
    unclaimed move is dropped only once it is 2 s old **and** no import that began before it was taken is still in flight.

The loading thread's Scaleform displays go on the graph again (the D3D11 fallback and its switch are removed).

## Testing

Scenarios (`tools/cs-ab/scenarios/`): `loading-org0.env` (switch off) and `loading-org1.env` (switch on), both with the P4
switches and `CS_ORG_TEXTURE_STREAMING=1`, `CS_ORG_SCALEFORM=1`, `CS_ORG_BINK=1`, loads by `coc Whiterun`,
`coc WhiterunDragonsreach`, `cow Blackreach 0 0`, `dclf:load=recent`, and a shot after each.

In org1's log:

1. `[ORG] render ownership: the CS_ORG_LOADING_THREAD rule is on`.
2. At the logo: `[ORG] render ownership: thread N (holding Renderer::Lock) takes it from thread M`. Bink's `a WxH movie
   ended: G frames drawn by the render graph` has G near 166 only if the client was registered before the logo (State::Setup);
   not run in game yet with the session-long client.
3. Per load: `[ORG] loading screen: the serving thread renders until the load ends`, a claim by the serving thread, then
   `[ORG] loading screen: the load ended (K ownership changes); the main thread renders again`.
4. **No** `UploadInstance '...' called off the owner thread` line, **no** `PublishedResources used on thread` line, and no
   `a Present on thread ... not the main thread` line (that is the switch-off message).
5. Scaleform's counts: displays "off the main thread (loading screen)" fall to 0; loading-screen displays are counted as the
   graph's.
6. No `[E]` beyond the known one, no device loss, every shot equal to org0's.
