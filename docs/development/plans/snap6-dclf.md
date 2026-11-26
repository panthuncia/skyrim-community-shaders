# snap6-dclf: DCLF on the retire queue (roadmap phase 6, part 2)

Lead `dclf`, round `snap6`, branch `snap6/dclf` (base `org_integration` 175c008da), stage `s7-dclf`. Design:
[snap6-life.md](snap6-life.md) section 5. Roadmap: [render-thread-snapshots.md](render-thread-snapshots.md), "Phase 6".

## 1. What changed

### EngineReleases on the engine lane

`Core/Engine/EngineReleases.h` keeps its three entry points, now thin adapters to `Snapshot::Retire` (the engine lane):

- `Push(NiPointer)` and `PushOwner(shared_ptr)` push one entry with `Retire::AfterThisFrame` (the newest render frame begun).
- `PushRetired(stamp, references, owners)` pushes a retired batch as one entry. Its destructor lets go of the references and owners,
  then advances `releasedThrough` to the batch's last publication. The coordinator's `RecycleRetired` uses it (before: a
  `NoteReleased` then one push per reference).
- `Release()` at Present, its two `EventQueue`s and their vectors are gone. The engine lane drains at the capture on the main thread.
- `Alive(publication)` keeps its meaning, read under a lease or on the main thread. `releasedThrough` now moves only when a batch is
  actually let go (before, the take-then-read could mark stamps released whose batch was still queued), and only at the capture,
  while the window is closed.

Other render-thread drops of engine references moved to the queue too:
- `LocalLightCull`'s retired category filters (they hold nodes): pushed at retirement (`RetireFilter`) instead of a two-Present
  ring cleared on the render thread. `LocalLightCull::EndFrame` and its `frameEnd` registration are gone.
- `PrimaryCull`'s `listGraveyard` (scene list roots the build's job let go): handed over whole at Present.

### The capture window

`EngineReadWindow` is the game-side capture window:

- `Open` in Main::Update's call of Main::DrawFrame (`MainUpdate_DrawFrame`, `Snapshot/Publication.cpp`), right after the capture,
  the test steps and `SideEffects::AfterCapture` (S1/S4 write the engine). `Close` at the end of that call, after the pacing wait:
  Main::Update's game logic follows the DrawFrame call (Ghidra: AE 36564 calls on past +0xA97). In phase 7 the call becomes
  publish-and-wait, so the window spans the wait.
- The render half still runs inside the window today, and it writes the scene graph outside Main::Draw to Present: DrawFrame
  (AE 0x140643c00) applies the decal manager's pending decals (`FUN_1406458a0`, BGSDecalNode attaches) and drops temp effects
  before Main::Draw, and signals the next update's job lists (`SetJobTaskEvent(8/9)`) around Renderer::End. So the window is
  **held** there: a `kHeld` bit in the state word, set by `HoldForRenderHalf` before each DrawFrame call of the update and at
  Present (`FrameDriver::EndFrame`, which used to `Close`), cleared by `ReleaseRenderHalf` at `FrameDriver::BeginFrame` (which
  used to `Open`) and by `Close`. Both act only on the main thread inside the update's render half (`Snapshot::InGameUpdate`), so a
  loading screen's Present on the serving thread does nothing. Leases are therefore taken exactly where they were before
  (Main::Draw to Present), and a dropped cadence update (no render half) leaves the whole window readable.
- `IsOpen()` means open and not held, so its two users (the render-toggle wake deferral and the in-window edit count) keep their
  meaning.
- A lease taken on the main thread is counted: in today's frame the main thread inside the window is the render side, which must
  read publications only. The engine-read window report line gains `main-thread leases N <- OK / <- RENDER-SIDE LEASE` and counts
  `render-half holds` beside `closes`.

### Held detaches

`SceneTracker::PushHeldDetaches` moves from `FrameDriver::BeginFrame` to a capture DCLF registers (`Snapshot::RegisterCapture`,
`DrawcallLimitFix::Hooks::Install`): the game update is over at its capture, on the game thread. The pins of each detach go to the
engine lane with the batch, as before (`ApplyEvents`).

## 2. Design points checked against the code

- **The scene publication and `FrameSnapshot::scene`.** The design had the publication let go with its snapshot. In the code no
  snapshot carries it yet (`FrameSnapshot::scene` is empty): DCLF's publications reach the frame through the snapshot builder's
  `SceneSnapshot`, held by the epochs' frame owners until the GPU finished the slot, and return to the coordinator, whose
  retirement chain hands each batch over once no publication up to it lives. That chain already is "references held per
  publication, retired when the last reader is done"; only its last hop (the release) was DCLF's own queue, and that is now the
  engine lane. Pairing the publication with the snapshot belongs with phase 7, when the render thread adopts DCLF's scene by
  snapshot. Decided by the agent, to confirm.
- **The GPU wait.** Every DCLF entry names the newest render frame begun at the push, so the lane's GPU rule applies uniformly
  (decision 99). It is conservative: a batch reaches the queue only once its publications' frame owners released after the GPU.
- **The render half is a writer.** The design said `Open` after the publish. With the render half still called that would let
  workers lease while DrawFrame attaches decals; the hold keeps today's lease interval until the render half goes (phase 7).
- **In-place writes** need no new mechanism in DCLF: a reported pointer swap (SetMaterial, texture set, property events) re-takes
  the material, whose owner block (`Lookups::MaterialOwners`) changes identity with any owner, so the new publication names the new
  objects and the old block retires with the publications naming it through the chain. Values written in place (alpha, emittance,
  texture transforms, node transforms, palettes) are copied into the tables or the frame values' rows. The water normal textures are
  the water lead's (`snap3-water`); `Retire`'s API is unchanged apart from `AfterThisFrame` being documented as any-thread.
- **Engine data copied for the frame.** DCLF's reads of the constant mirror (`ConstantMirror::Contents` in `MainEpochs`,
  `ViewEpoch`, `Uploads`, `ShadowCapture`) copy into the frame's blocks or upload rows, which retire with the frame; no snapshot
  points into engine memory. The bytes themselves are the engine render half's output (decision 56) and go with the family ports.
  The dynamic ring is referenced (not copied) only by the menu scenes' latched dynamic draws (`LatchDynamicGeometry`), the
  `snap5-menus` lead's area.

## 3. Runs

(filled in below)

## Proposed decision-log entries

- **DCLF's engine releases are the retire queue's engine lane** (phase 6 part 2): `EngineReleases::Push`, `PushOwner` and
  `PushRetired` push entries waiting for the newest render frame begun; the lane drains at the capture on the main thread; a
  retired batch advances `releasedThrough` when it is let go. No DCLF engine reference is dropped at Present any more (the light
  culls' retired filters and the scene lists' graveyard included).
- **The engine-read window is game-side:** open after the update's capture and side effects, closed at the end of Main::Update's
  call of Main::DrawFrame. While the engine's render half runs inside it, the window is held outside Main::Draw to Present (the
  render half writes the scene graph there), so leases keep today's interval; a dropped update leaves it readable. Main-thread
  leases are counted as render-side leases.
- **Held detaches are pushed at the capture,** on the game thread, in both modes.
- **DCLF's publication stays outside `FrameSnapshot`** until phase 7; its retirement chain feeds the engine lane.

## Decided by the agent, to confirm

- The render half holds the window outside Main::Draw to Present (rather than opening it for the whole render half).
- Held detaches move to the capture in guest too (shared code; a detach and its re-attach happen within one call, so the pairing
  is the same).
- `FrameSnapshot::scene` stays empty (above).
- Engine entries wait for the newest render frame begun at the push, although the chain already waited for the GPU.
