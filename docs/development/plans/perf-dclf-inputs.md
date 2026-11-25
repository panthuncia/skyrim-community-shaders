# Render thread: DCLF inputs and CS per-frame overhead (lead dclf-inputs)

Item 4 of `render-thread-window.md`, plus the other CS-side CPU in the window. All timings are Dev-Fast (/Od), guest mode,
the frozen river scene (`trun.sh guest+quick ... --set env.CS_ENGINE_ZONES=1`), per frame. Captures vary by about 0.1 ms and
by more when other leads build at the same time. The baseline is `base4` (unchanged branch, window 6.55 ms). The after
figures come from `wip6` (window 6.17 ms) and `wip7` (window 6.61 ms, a busier machine).

## What changed

1. **Feature loops (`Feature::ForEachLoadedFeature`, `Deferred::*`).**
   - A feature whose frame callback (ReflectionsPrepass, EarlyPrepass, Prepass, PostPrepass) is the empty default is skipped
     after its first call: no VRAM owner scope, no debugger event, no Tracy zone. The defaults set a thread-local flag, and each
     call site keeps its own list. An override must not call the default (none does).
   - The per-feature Tracy GPU zone (a D3D11 query pair) is opt-in: `CS_FEATURE_GPU_ZONES=1`. Without it captures no longer
     carry about 0.2 ms of query cost.
   - The event name is built in a stack buffer, not `std::format`. When the debugger event is on it already opens a Tracy zone
     of the same name, so the loop no longer opens a second one. `State::BeginPerfEvent` widens the title on the stack.
2. **Texture resolves.** `GpuTextures::Resolve` keeps a ready binding for the rest of the frame, per view and call site, and checks
   the cache before its Tracy zone. That replaces Family::SlotTexture's own cache. The zone text is the file name with the line
   as the zone value, not a formatted string.
3. **Forward sort rows.** `ForwardReached` copied each forward object's whole 160-byte placement row for the forward sort on the
   CS thread. It now copies the 32 bytes the sort reads (`ForwardSortRow`). First person's forward rows do the same.
4. **Fade inputs.** The animated-root stamping loop in `SampleFadeInputs` reads through plain pointers.
5. **Zones** for the next round: per commit hook (named by the hook), `CS.DCLF.UploadVertexStream`, `CS.DCLF.Fade.*`,
   `CS.DCLF.Shared.*` in SubmitSharedEpoch, `CS.DCLF.Cpu.ForwardRows` / `ForwardHandOff`.

## Measured (Dev-Fast, ms/frame)

| Bucket or zone | base4 | wip6 | wip7 |
|---|---|---|---|
| Deferred feature loops | 0.447 | 0.249 | 0.270 |
| Other CS / features | 0.573 | 0.286 | 0.318 |
| DCLF CPU (outside tickets) | 0.554 | 0.565 | 0.578 |
| `CS.DCLF.Texture.ResolveBinding` (inclusive, calls) | 0.062, 35 | 0.085, 64 (cache bypassed, see below) | 0.031, 10 |
| Forward rows (ExecuteForward self, then `Cpu.ForwardRows`) | ~0.08 | 0.040 | 0.045 |
| `CS.DCLF.ListJobsWait` | 0.110 | 0.110 | 0.142 |
| `ORG.Host.CommitInputs` (14.5 calls) | 0.634 | 0.637 | 0.693 |
| `CS.DCLF.CommitMainPayload` (2 calls) | 0.395 | 0.392 | 0.430 |

The feature loops and the other-CS bucket together dropped by about 0.45 ms. Most of that was profiler and debugger-event cost,
so it is capture distortion more than shipping cost. DCLF CPU moved by less than the noise: forward rows and resolves went down,
and ListJobsWait varies by 0.03 between captures. wip6 bypassed the resolve cache while the undeclared access check was on (it
always is in graph modes); wip7 keys entries by call site instead.

## Not done, and why

- **DCLF inputs ahead of the commit (item 4 proper).** The Z-prepass ticket's `CommitInputs` is 0.27 ms. Where it goes:
  `fade roots` hook 0.06 (`SampleFadeInputs`: the animated batch copy and stamping), reflection `HostedView.FrameTextures` 0.046
  and `CommitView` 0.031, grass hook 0.023, tree-lod hook 0.012, CullLatch 0.024, CountsAndStats 0.020, TakePoolFirsts 0.017.
  The main opaque ticket adds `CMP.FrameTextures` 0.078 (FT.Registers 0.042) and `ColourCommitted` 0.032. No single piece is
  big. Each reads frame state that settles only near the commit (the animated batch after the list jobs, published indices
  that relocation can move, this frame's latch run), so a pointer-swap commit needs a producer per input on the revision lane
  with its own ordering. That is a design change, not a local edit, and it was not attempted. The frame textures are the best
  next target: colour and reflection resolve the same registered textures through `PublishedResources` string lookups. A
  per-frame answer keyed by the relocation journal head and the published entries' serial would serve both.
- **ListJobsWait (0.11-0.14).** `JobList::Finish` (AE 0x140cf6810) makes the render thread run queued culling jobs itself until
  the list drains, so the wait is culling work (the engine's cull plus DCLF's StandIn), not idle time. There is no independent
  DCLF work at that point that could run first without new ordering. Lead engine-skip is not changing these jobs.
- **Native sky builds (0.08), family capture (~0.08), `CS.Engine.Constants.Capture` (0.045), `State::Draw` (0.026).** Per-draw
  CPU work for engine draws that are still engine-driven. Its cost is per pass, with no repeated lookup to cache. The first
  constants capture of a frame takes a new ring chunk (0.018 in RenderShadowMasks).

## Decisions to confirm

- Decided by the agent, to confirm: a new Diagnostic switch `CS_FEATURE_GPU_ZONES` makes the per-feature Tracy GPU zones
  opt-in. The brief prefers no new switch, but the zones are a measurement tool and their cost distorts every capture.
- Decided by the agent, to confirm: features with an empty default callback get no debugger event any more. RenderDoc and
  Nsight captures lose those empty markers.

## Tests

| Config | Label | Result |
|---|---|---|
| guest+quick | p-dclf-inputs-fguest | passed, shot looks right |
| native-vulkan+quick | p-dclf-inputs-fnvk | passed, shot looks right |
