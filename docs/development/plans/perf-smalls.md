# Render thread: small leftovers (lead smalls)

The leftovers the earlier leads listed (`perf-hazards.md`, `perf-tickets.md`, `perf-commit-inputs.md`, `perf-reflections.md`).
All timings are Dev-Fast (/Od), guest mode, the frozen river scene (`trun.sh guest+quick ... --set env.CS_ENGINE_ZONES=1`), one
6 s Tracy capture each, inclusive time per frame. `base1` is the unchanged branch (3ed746144). `after3` is the final code on
top of the reflections merge (094a61908). Captures vary by about 0.1 ms in the window and a few microseconds per zone.

## Commits

- ORG `perf/smalls` (on 5891ddf): 2da03bc "perf: keep upload uses and checked rebindings", 358597f "perf: copy and move only
  live staging entries".
- CS `perf/smalls`: e1fc4173a bumps ORG, 8a53a9151 "perf(dclf): keep repeated texture index lookups", 566becda6 "perf(dclf):
  zones for the native sky face steps", then a merge of org_integration (post-slots), 0191f0885.

## What changed

1. **Stage copies (`CS.Stage.CopyLatch`, `MoveSlots`).** `LatchBlock` keeps, per lease, the blocks whose region there may be
   non-zero (their `m_stableHigh` above zero). `CopyStableRun` from a host-only staging run and `ZeroStableRun` of one walk
   only those, instead of every block with a region at either lease (sorted each time). `RunReadback::Commit` notes the lease
   it committed at, so `MoveStagingRun` asks only those readbacks instead of every one.
2. **`PlanTicketUploads`.** The uses of each backing in a ticket's batches (batch, compiler resource, aliased) depend only on
   the executable and its backing set. They are kept per (executable, `AdmissionBackingSet::Id`), up to 8, and a steady frame
   looks its destinations up instead of walking every boundary access. Late-bound stand-ins' uses are merged in batch order.
3. **`AdaptedLuminanceIdOf`.** `AdaptedLuminanceTextureIndex` (DCLF side) keeps the frame's first answer per view
   (`Driver::Current().frame`). ImageSpace files are untouched.
4. **LLF graph buffer indices.** The integration's callback reads the three ids through `GraphAnswers`
   (`Extensions::PublishedIndices`, new). `ORGLightCulling::GetShaderResourceIndices` is deleted.
5. **`ApplyFrameRebindings`.** `PrepareAtSubmission` takes the recording's `SharedFrameRebindings`. The admission keeps up to
   16 checked lists (weak references to the publication and the list) with the compiled resources they rebind and their
   rebound set. The same list for the same publication skips the checks and the set search. Per-frame lists (swapchain) are
   checked every time, as before.
6. **Engine families' published inputs.** `PublishedInputs::Resolve` (every engine draw family, the sky's faces among them)
   asked `ShaderResourceIndex` once per id per frame. It now asks `GraphAnswers` (`Extensions::PublishedIndex`), which keeps
   the answer across frames. `GraphAnswers`' published map owns its keys, so any id may be asked. Zones for the native sky face
   steps (`CS.DCLF.Sky.PrepareFace`, `SubmitFace`, `ReadInputs`, `ResolveHeaders`) attribute the reflection faces' own time.

## Measured (Dev-Fast, ms/frame, calls/frame)

| Zone | base1 | after3 |
|---|---|---|
| `ORG.Persistent.ApplyFrameRebindings` | 0.105 (14.5) | 0.000 (runs only for a new list) |
| `ORG.Persistent.PlanTicketUploads` | 0.037 (2, 18.5 us) | 0.018 (2, 9.1 us) |
| `CS.Stage.CopyLatch` | 0.062 (8.5, 7.3 us) | 0.050 (8.5, 5.8 us) |
| `CS.Stage.OpenInputs` | 0.061 | 0.056 |
| `CS.Stage.MoveSlots` | 0.028 | 0.026 |
| `CS.DCLF.CMP.FT.GraphBuffers` | 0.0054 | 0.0045 |
| `CS.DCLF.Sky.ReadInputs` (3 calls) | 0.075 (`diag1`) | 0.068 |
| `ORG.Execution.CommitStates` | 0.195 | 0.191 (unchanged code) |

Together about 0.16 ms a frame. Window: 5.24 ms (base1) against 4.89 (after3), but after3 also has the reflections merge, so
the window is no measure of this work alone.

What the copy costs: a temporary count showed about 10 live blocks and 47 KB a stage copy (Scaleform's gate about 16 KB, the
sky faces' latch about 8 KB). Most of `CopyLatch` is those bytes. A host copy of each target region, to write only the 512-byte
chunks that differ, measured slower (7.7 us a copy: the values change and the compare doubles the work), so it was dropped.

## Not done, and why

- **`CommitStates`** (0.19, 108 calls, 1.8 us each). It is a fixed cost per batch: seed, one backing-set lookup and the cell
  visits. The grids it writes change every frame (stages hand resources to each other), so there is nothing to keep. Hoisting
  the backing-set lookup out of the per-batch loop would save about 0.02; left.
- **Native reflection faces' own time** (`CS.DCLF.Reflection.NativeFaces`, 0.31 for 2 faces). By the new zones: `BuildFace`
  0.10, `Deferred::ReflectionsPrepasses` 0.095 (self 0.08: `UpdateSharedData`, the feature loop and a `TracyD3D11Zone` per
  face), `SubmitFace` 0.087 (`ReadInputs` 0.045: Physical Sky's `ResolveOrgHandles` asks `PublishedResources::ExportedId` per
  LUT view and allocates strings per face; `SkyDraws::Submit` about 0.03). Candidates: Physical Sky's handles once per frame
  (the faces and the main sky bind the same LUTs), and the prepasses' D3D11 zone. Not done: both need checks that the bound
  views cannot change between faces.
- `OpenInputs` (0.056) is mostly zeroing the staging run's 47 KB, which keeps "a block nobody wrote reads zero".

## Decided by the agent, to confirm

- The adapted luminance answer holds for a frame: the chain's held build and the engine's target views change only at a frame
  boundary.
- The kept upload uses trust `AdmissionBackingSet::Id` to name fixed backings, as the hazard ledger's quiet grids do.
- A checked rebinding list is identified by its shared vector (the recording's). Lists are immutable once shared.
- No new switch; the old paths are gone.

## Tests

| Config | Label | Result |
|---|---|---|
| guest+quick, trun captures | p-smalls-base1, after1, diag1, after2, after3 | ran to the exit step (trun marks them failed: no shot) |
| guest+quick | p-smalls-final-guest-quick | passed, shot good |
| native-vulkan+quick | p-smalls-final-native-vulkan-quick | passed, shot good |
| native-d3d12-quick | p-smalls-final-native-d3d12-quick | passed, shot good |

No ORG unit tests were built (the worktree's build has no test targets); the game configs cover the changed paths (stage
copies every frame, uploads on two tickets a frame, rebinding lists on every stage ticket).
