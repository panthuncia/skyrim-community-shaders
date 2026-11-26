# snap6-life: snapshot lifetimes and engine resources, part 1 (roadmap phase 6)

Lead `life`, branch `snap6/life` (base `org_integration` ab2f89267), stage `s6-life`. Roadmap:
[render-thread-snapshots.md](render-thread-snapshots.md), "Phase 6". Part 1 is everything outside DCLF; part 2 (DCLF) is
designed in section 5 for the `culls` lead's code.

## 1. Inventory: what a published snapshot references

Read from every block declaration (`Snapshot::BlockId` users), the core fields, the cameras and the UI stream.

| Snapshot part | Engine object or memory it names | How (before) | Dereferenced on the render side? | Now |
|---|---|---|---|---|
| Core fields (`id`, times, kind, menus, events, window size) | none | values | - | - |
| `cameras` (`CameraView`: matrices, frustum, port) | none | copied values | - | - |
| `CoreFrame` | cloud layer 28's geometry, Secunda's moon and shadow meshes | bare addresses (`uintptr_t`) as keys | no: compared with `BSRenderPass::geometry` in `State::UpdateSkyShaderPermutation` | held `EngineRef` |
| `CoreFrame` (rest: light, DALC, sky, water tiles) | none | copied values | - | - |
| `CloudShadows::Block` | each drawn cloud layer's geometry | bare addresses as keys | no: `FindCloudLayer` compares with the pass's geometry | held `EngineRef` |
| `PhysicalSky`, `TerrainShadows` | worldspace editor IDs | `std::string` copies | - | - |
| `Effects11`, `PostProcessing` | `ImageSpaceData` | copied struct | - | - |
| `GrassCollision` | colliders | copied vectors | - | - |
| `ImageSpace` (inputs, world settings, counts, screenshot paths) | none | copied values | - | - |
| Every other block (SkySync, ProceduralSun/Aurora, Skylighting, InteriorSun, VolumetricLighting, Wetness, Height Fog, UnifiedWater, DynamicCubemaps, SSS shadows, TreeWind) | none | copied values | - | - |
| `ui` (`UiCommandStream` -> `UiStream`) | GFx textures' D3D11 views (`TextureInfo::view`), display target textures (`DisplayTargets`), copied vertex/index arrays and matrices (payload) | `com_ptr` references; arrays copied into the stream's payload | views through the stream's own references; `GTexture*` keys only index the stream's texture table | released through the retire queue (render lane) |
| UI uploads (not in the snapshot; queued by stream sequence) | GFx texture D3D11 textures | `com_ptr` per upload | applied once, before the stream's draws | released through the retire queue (render lane) |
| `scene` (`DCLF::ScenePublication`) | geometry, nodes, materials, properties | DCLF's own references (part 2) | yes, by DCLF | part 2 |
| Producer (not published): the player's previous cell | `TESObjectCELL` | bare address, compared for the teleport event | game side only | form id |

Result: outside DCLF no published snapshot dereferenced a bare engine pointer at record time. The engine objects named were
named by address only, and an address can be reused once the object is freed, so a key could match a new object (the sky
replaces cloud geometry on a weather change). The keys now hold a reference for as long as any snapshot names them.

Not snapshot references, listed for phase 7: CS still reads live engine state at record time (the detector's RecordTime and
BoundState rows: ImageSpace 14 sites, Light Limit Fix 12, Skeleton 10, Cinematic Camera 10, Deferred 9, ...). Those are reads of
the engine's render half or of live objects in the same frame (decisions 25, 32, 56, 90), and they go with the phase 3/5 ports;
in phase 7 the render thread must make none.

## 2. The retire queue (`src/Snapshot/Retire.{h,cpp}`)

- **What the engine requires.** `NiRefObject` counts atomically, but the last `DecRefCount` runs `DeleteThis` on the dropping
  thread, and the destructors touch scene graph, renderer and texture database state without a lock. DCLF already releases its
  references on the main thread for this reason (`EngineReleases`). D3D11 handles (CS's tripwire handles in native, DXVK in guest)
  may be released on any thread: a tripwire handle's resource goes through `ServiceRetired`.
- **Two lanes.** The engine lane is drained on the main thread at the capture (game side, before the snapshot is built). The
  render lane is drained by the render side at adoption. Each entry names the render frame whose GPU work must finish
  (`FrameClock::GpuCompleted` in graph mode; outside graph mode nothing waits). Frame 0 retires at the next drain.
- **Snapshots** are let go only on the main thread. At adoption the render side hands the snapshot it rendered last to
  `Retire::RenderedSnapshot` with its last render frame: its UI stream goes to the render lane, the snapshot to the engine lane.
  A snapshot replaced unadopted (`Publish`) and the producer's previous one go through `Retire::LetGo`: dropped at once on the
  main thread, handed to the engine lane from the serving thread.
- **UI references:** the replay's previous stream, a stream no snapshot took, a repeated render's open stream, and applied
  uploads' textures go to the render lane with the frame that read them. A loading screen's streams therefore retire on the
  serving thread while the main thread is in the load, instead of piling up.
- **`Snapshot::EngineRef`:** a held key (`Key()`, `Names()`), never dereferenced. Taken, copied and let go only on the main thread;
  anywhere else is fatal. Used by `CoreFrame` (three sky keys) and `CloudShadows::Block` (layer keys).
- **Teardown:** the producer, the consumer, the slot and the lanes are never destroyed, so no engine reference is released while
  the process ends (as `RenderGraph::Frame::EventQueue` already does).
- **Telemetry:** `FrameSnapshot::live` counts snapshots alive; `[Snapshot] lifetimes: N snapshots alive, M engine references
  held; engine lane ...; render lane ...` comes with each load line and every 1,800 game updates.

## 3. In-place engine writes outside DCLF

None reaches snapshot data: blocks are copies, and the only engine objects they name are held keys, never read. The UI side was
already versioned: GFx's in-place writes (glyph cache `Update`, `Map`/`Unmap`) are uploads queued by stream sequence and applied
before that stream's draws, so a repeated stream never sees a newer stream's writes; a re-initialised GFx texture gets a new
D3D11 texture, and streams that drew the old one keep it by reference until they retire. The roadmap's example, the water normal
textures `TESWaterSystem::Update` assigns (reported by SceneWriterFramework), is DCLF's water scene: part 2.

## 4. Engine data copied for the frame, outside DCLF

- UI vertex and index arrays, matrices and texgen data are copied into the stream's payload, which the snapshot owns.
- The constant mirrors (`Engine/Constants`) and the dynamic ring are written by the engine's render half. Outside DCLF only the
  ImageSpace explicit steps (SAO, SSR rows, `BoundRows`) and the lens flare's capture path read them, at record time, into CS's
  per-frame ring chunks, which retire after the GPU frame. They are render-half output (decision 56) and go with the ImageSpace
  render-half port; CS-computed bytes (`FrameLocation`, the frame camera) are already CS memory.

## 5. Part 2 design: DCLF on the retire queue

- **`EngineReleases`** (`queue`, `owners`, released at Present) becomes `Retire::Push(Lane::Engine, ...)`. References carry the last
  render frame that drew them, so the GPU wait comes from the queue (today DCLF's owners cover it separately). `pushedThrough` /
  `releasedThrough` / `Alive(publication)` move with it: the drain advances `releasedThrough` on the main thread, before the capture
  window opens, so a worker under a lease never sees an object released mid-item.
- **The scene publication** (`FrameSnapshot::scene`) is let go with its snapshot, on the main thread. SceneSet's retirement chain
  then releases a batch's references directly on the main thread (or through the engine lane from any other thread), with no
  second queue.
- **Held detaches** (`SceneTracker::PushHeldDetaches`, render frame start today) are pushed at the capture, on the game thread; an
  event's `pins` go to the engine lane with the batch.
- **`EngineReadWindow`** becomes the game-side capture window: `Open` right after the publish (the update is done), `Close` at the
  next update's start (before game logic writes), both on the main thread. Scene workers lease inside it; the render thread never
  takes a lease and reads only publications. `Suspend`/`Resume` (engine writers inside the window) stay as they are.
- **In-place writes** (SWF reports, e.g. water normal textures, material texture swaps): the publication holds the texture or
  property it drew with as a reference; a reported write makes the next publication take the new object, and the old one retires
  with the publications that name it. Contents written in place (not a pointer swap) are copied into the publication.

## Runs

(filled in below)

## Decided by the agent, to confirm

- Two lanes rather than one: D3D11 handles and CS objects retire on the render side so loading screens don't pile up streams.
- Entries wait for `GpuCompleted` in graph mode even when the payload is CPU-only (snapshot blocks): one rule for part 2's geometry.
- The engine lane drains at the capture only (once per game update, plus main-thread frame-only captures), not inside the render half.
- `EngineRef` is fatal off the main thread, including copies (blocks are never copied on the render side).
- The producer keys the player's cell by form id.
