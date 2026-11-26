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
- `Release()` at Present, its two `EventQueue`s and their vectors are gone. The engine lane drains on the main thread after the
  update's frame jobs (section 2).
- `Alive(publication)` keeps its meaning, read under a lease or on the main thread. `releasedThrough` now moves only when a batch is
  actually let go (before, the take-then-read could mark stamps released whose batch was still queued), and only at the drain,
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

- **The engine lane drains after the update's frame jobs, not at the capture (design fixed).** Main::Update joins its frame jobs
  (the frame-job wait, SE 34554 / AE 35409, `RenderGraph/Engine/MainTick`) right after its call of Main::DrawFrame, so those jobs
  (the cell animations, Havok, animation) run beside the capture and the render half. The first cadence-drop-cells run hung at the
  third coc: the cell animations job looped forever in a corrupted engine list (AE `FUN_140cf7c70`) while the main thread waited
  for it, because the capture (no render half on a dropped update, so the job's update ran at once) dropped DCLF's last references
  beside it. Before this change DCLF released at Present, where the job had normally finished. `Retire::DrainEngine` now runs as a
  `MainTick` callback, where the engine is back to one thread and the engine-read window is closed, and `Retire::LetGo` always goes
  through the lane (the snapshot it lets go may hold sky keys). Decision 99's "drained at the capture" becomes "drained at the
  frame-job wait after Main::DrawFrame".

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

Stage `s7-dclf`, Dev-Fast. First build 4dcac6f74 (drain at the capture), then 812cfad3e (drain after the frame jobs).
`CS_DCLF_STATS=1` on most runs for the report lines; `CS_DCLF_RELEASE_GUARD=1` on the first ones.

| Config | Label | Build | Result |
|---|---|---|---|
| native-vulkan+quick | s7-dclf-nvk-quick | 4dcac6f74 | never-loaded: boot took over 120 s in State::Setup while two games shared the machine; rerun below |
| native-vulkan+quick | s7-dclf-nvk-quick2 | 4dcac6f74 | passed (683 live reads, 33 engine render calls per world frame) |
| native-vulkan+native-tour | s7-dclf-nvk-tour | 4dcac6f74 | passed (517 / 20) |
| guest+quick | s7-dclf-guest-quick | 4dcac6f74 | failed: 1-5 fps from the load on (machine contention), shot timed out; passed on rerun |
| native-d3d12-quick | s7-dclf-nd12-quick | 4dcac6f74 | passed |
| native-vulkan+cadence-drop-cells | s7-dclf-nvk-dropcells | 4dcac6f74 | **hang** at the third coc: the cell-animations list loop (section 2), fixed by 812cfad3e |
| native-vulkan+cadence-repeat | s7-dclf-nvk-repeat | 4dcac6f74 | passed |
| native-vulkan+window-resize-stress-taa | s7-dclf-nvk-resize-taa | 4dcac6f74 | passed |
| native-vulkan+cadence-drop-cells | s7-dclf-nvk-dropcells-b, -c | 812cfad3e | passed, both (three cocs each) |
| guest+quick | s7-dclf-guest-quick-b | 812cfad3e | passed |
| native-vulkan+quick | s7-dclf-nvk-quick3 | 812cfad3e | passed (763 / 33) |
| native-d3d12-quick | s7-dclf-nd12-quick3 | 812cfad3e | passed |
| native-vulkan+cadence-repeat | s7-dclf-nvk-repeat3 | 812cfad3e | passed |
| native-vulkan+native-tour | s7-dclf-nvk-tour3 | 812cfad3e | passed (523 / 20; org_integration's s3-accum tour: 519 / 20) |
| native-vulkan+window-resize-stress-taa | s7-dclf-nvk-resize-taa3 | 812cfad3e | passed |
| native-vulkan+vram-loads | s7-dclf-nvk-vram-loads | 812cfad3e | passed |
| native-vulkan+vram-loads on stage `dir` (org_integration 34b52b99b) | s7-dclf-base-vram-loads | base | passed |

In every run with report lines: `main-thread leases 0 <- OK`, `scene work engine accesses without a lease 0 <- OK`, the release
guard 0 on the scene lane and pool, no "readback writes of a released publication" or "fade write-back: store dropped" lines.
The window reports 300 closes and 600 render-half holds per 300 frames, none of which waited for a lease.

**Per-load memory (vram-loads, the second dump of each pair, device-local usage in MiB):** branch 4043, 4173, 4303; org_integration
4037, 4103, 4329. Growth over the two later loads: branch 259, base 292. Leak lines match: both keep 2 snapshots alive and 36 engine
references after every load, both list only the known `Skylighting::ProbeArray` and `ViewAtmosphere` "still held" warnings, and
DCLF's "snapshots held past the pool" stays at 3-5 in both. The engine lane now carries DCLF's entries (tens of thousands a
window, at most about 530 waiting for the GPU) and its retired count follows its queued count.

Detector numbers do not move (lifetimes only): native tour 523 live reads and 20 engine render calls per world frame against 519
and 20 on org_integration.

## Proposed decision-log entries

- **DCLF's engine releases are the retire queue's engine lane** (phase 6 part 2): `EngineReleases::Push`, `PushOwner` and
  `PushRetired` push entries waiting for the newest render frame begun; the lane drains on the main thread at the frame-job wait
  after Main::DrawFrame (refines 99: the capture runs beside the update's frame jobs); a
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
- The engine lane drains at the frame-job wait after Main::DrawFrame instead of the capture (fixes decision 99's point; section 2).
- The two first-build failures (never-loaded boot, slow guest) are machine contention: both passed on rerun, and the boot stall was
  in State::Setup before any changed code runs.

## Open items

- `FrameSnapshot::scene` (DCLF's publication paired with the snapshot) for phase 7.
- During a load the main thread is inside the load, so DCLF's released references wait until its first frame-job wait after it
  (before, the serving thread's Present released them off the main thread). Peak memory inside a long load may be a little higher;
  the per-load numbers above are taken after loads.
- The render half's hold goes with the render half (phase 7): then the window is readable from the publish to the next update.
