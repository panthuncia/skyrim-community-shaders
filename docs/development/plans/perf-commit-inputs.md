# Render thread: DCLF inputs off the commit (lead commit-inputs)

Item 4 of `render-thread-window.md`, after `perf-dclf-inputs.md`. All timings are Dev-Fast (/Od), guest mode, the frozen river
scene (`trun.sh guest+quick ... --set env.CS_ENGINE_ZONES=1`), per frame. The baseline is `base1` (unchanged branch, window
5.70 ms). The final capture is `w5` (window 6.01 ms: other leads were running games and builds, so everything outside the
changed zones reads about 5% high). Captures vary by about 0.1 ms in the window and by a few microseconds per zone.

## What changed

1. **Frame texture lookups are kept while they hold** (`Extensions/FrameBindings.cpp`, `GraphAnswers`). The faces, colour and
   grass commits asked `PublishedResources` the same questions every frame: is this view a published id's export, and what is
   that id's descriptor. Each question was two or three string-hash lookups, an atomic `shared_ptr` load, a `dynamic_cast` and
   a view query. The answers are now kept, keyed by what changes them:
   - `PublishedResources::Serial()` (publishes and snapshots), the relocation journal head, the declared-id generation and
     whether the runtime is active. Any change drops every answer.
   - For a versioned id, its slot's `CurrentNumber()` at the time of the answer (`PublishedResources::VersionedSlotOf`). Most frame
     textures are versioned slots, so without this the answers lasted one frame and every frame missed about 31 times
     (measured with the `CS.DCLF.GraphAnswers.*Miss` zones, 0.041 ms). Now nothing misses in a steady scene.
   - `PublishEntry` bumps the serial only when an entry really changes, so a feature that publishes the same resource again
     does not drop the kept answers (or `PublishedResources`' own export answers).
2. **Index pool firsts are kept for a steady scene** (`Direct/Sites.cpp` `TakePoolFirsts`). The pool gets a `firstsSerial` that
   changes only when a first index or a range moves, or the slot count changes. A view batch set gets a `geometrySerial` from
   its builder thread (`GeometryAddressSerials`): sets with the same index buffer addresses share it. The sets are rebuilt
   every frame even in a frozen scene, so set identity could not be the key. When both serials match, the slot keeps last
   time's firsts. This is the one piece that moved off the render thread: the builders compare the addresses.
3. **Hosted view maps are written once per shape** (`Epochs/ViewEpoch.cpp` `CommitView`). The reflection view rewrote its bucket
   map, class map and six face bucket tables into the latch every frame. They depend only on the shape (whose `map` is made
   with it) and the slot count, so they use `LatchRegionsHeld` like the main commit's bucket and class regions.
4. **The animated fade batch is swapped, not copied twice** (`Features/Fades`). `TakeAnimatedBatch` swaps the drained batch out
   and `KeepAnimatedBatchForObserver` swaps it into the observer copy after the stamping. The vectors rotate their storage.
5. **`TargetsOf` checks the main targets once per frame** (`Direct/Sites.cpp`). Each `NewRun` (four or five per colour commit)
   loaded every target's current version. Images change only at a frame boundary, so later runs of the frame reuse the answer.
6. **Zones** for the next round: `CS.DCLF.Fade.Stamp`, `CS.DCLF.View.Maps/Faces/FrameRecord`,
   `CS.DCLF.ShadowInputs.LatchViews/Occlusion/Arena/Log`, `CS.DCLF.GraphAnswers.IndexMiss/ExportMiss` (only on a miss).

## Measured (Dev-Fast, ms/frame)

| Zone | base1 | w5 |
|---|---|---|
| `ORG.Host.CommitInputs` (14.5 calls) | 0.643 | 0.617 |
| Z-prepass inputs | 0.277 | 0.241 |
| main opaque inputs | 0.129 | 0.123 |
| shadow view inputs | 0.095 | 0.099 (noise) |
| `HostedView.FrameTextures` (reflection) | 0.048 | 0.035 |
| `HostedView.CommitView` | 0.033 | 0.031 |
| main opaque `CMP.ResolveFrameTextures` | 0.050 | 0.039 |
| `Fade.SampleInputs` | 0.050 | 0.042 |
| `Cpu.TakePoolFirsts` | 0.016 | gone (kept) |
| `CMP.SceneStreamsAndHooks` (Z-prepass) | 0.105 | 0.099 |

About 0.05 ms came out of the DCLF commits, against a machine that ran 5% slower in w5. The stage tickets' `CS.Stage.CopyLatch`
and `MoveSlots` (about 0.07 together, ORG) went up with the load and hide part of it in the `CommitInputs` total.

## What stays on the commit, and why

The commit is not a pointer swap yet. Every remaining piece reads state that settles at or just before the commit:

- **Fade stamping (`Fade.Stamp`, 0.028).** The animated batch is the animation job's output, final only at `AfterListJobs`, on the
  render thread just before the Z-prepass ticket. Each stamp carries the scene step, so the words change every frame. A worker
  started at `AfterListJobs` would need a join before the commit, which is a wait. The CPU pipeline already hands the drain to
  its producer; the GPU-driven commit cannot without that join.
- **Reflection frame textures (0.035) and the colour commit's (0.028).** What is left is the registered textures' `view()`
  callbacks and the held bindings. The faces commit runs before the Prepass anchor and the colour commit after it, and some
  callbacks answer differently on either side (IBL's static cubemaps, Skylighting's updated volumes). Sharing the views across
  the two commits would hand the colour pass the wrong texture on such a frame, so each commit asks for itself.
- **Grass (0.020) and tree LOD (0.019) draw rows.** Wind, time, light and the cull camera are sampled per frame. The grass shadow
  mask lookup now hits the kept answers.
- **View faces and frame record (0.029).** Per-face cameras and frame constants for this frame's reflection faces.
- **CountsAndStats, CullLatch, LatchFrameSlots (about 0.035).** This run's latch region and this frame's counts and cameras.
- **`ColourCommitted` (0.029).** Site runs and views for this frame's culls, and the cull frame callbacks that dispatch the culls
  inline when the cull frame is ready.

### `CMP.SceneStreamsAndHooks` (0.10)

It runs `UploadVertexStream` and the commit hooks. Per frame it walks:

| Hook | ms | What it walks | Can a steady scene skip it? |
|---|---|---|---|
| fade roots | 0.050 | the animated batch (stamping), then the fade frame row and readback re-arm | No: the batch and the stamp change every frame. The root rows and portal programs already skip by content epoch |
| grass | 0.020 | mirror changes (empty when steady), then the per-frame draw row | The changes already skip; the row carries wind and time |
| tree-lod | 0.019 | the per-frame draw row and its texture binding | Same as grass |
| tree-wind, water-rows, portals, streaming, SSS, vertex stream | under 0.002 each | changed rows only | Already skip |

So a steady scene already skips the table uploads. What is left is frame-rate state.

## Not done

- **Stage ticket latch copies (`CS.Stage.CopyLatch` about 0.07 over eight tickets, `MoveSlots` about 0.03).** ORG's
  `LatchBlock::CopyStableRun` per stage ticket. That is ticket structure, which lead tickets owns.
- **`AdaptedLuminanceIdOf` in each colour and faces commit.** It unwraps the engine's HDR downsample views every call. It is
  ImageSpace code (lead post-native).
- **`ORGLightCulling::GetShaderResourceIndices`** asks `PublishedResources::ShaderResourceIndex` directly for the light limit
  fix's graph buffers in each colour commit. It could go through the kept answers once its ids are static.

## Decisions to confirm

- Decided by the agent, to confirm: a versioned id's kept answer is checked against its slot's `CurrentNumber()`. A twin that
  dies while its id stays published (a defect) would leave a kept export answer stale until the next publish.
- Decided by the agent, to confirm: `TargetsOf` assumes engine target images change only at frame boundaries, as the frame
  placement and version adoption rules say.

## Tests

| Config | Label | Result |
|---|---|---|
| guest+quick, trun captures | p-commit-inputs-base1, ft1, w2-w5 | world loaded, ran to the exit step (trun marks the run failed because it writes no shot) |
| guest+quick, stack sampler | p-commit-inputs-w2s | ran; samples located the per-frame lookup misses |
| guest+quick | p-commit-inputs-guestf1 | passed (final build), shot looks right, water reflection present |
| native-vulkan+quick | p-commit-inputs-nvkf1 | passed (final build), shot looks right |
| native-d3d12-quick | p-commit-inputs-nd3d12-f1 | passed (final build), shot looks right |

Captures and buckets: `<scratchpad>/commit-inputs/perf/` (`base1`, `w5` buckets and `CommitInputs` trees).
