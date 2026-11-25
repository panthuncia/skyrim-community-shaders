# Render thread round 2: DCLF CPU (leads dclf, dclf2, dclf3)

All timings are Dev-Fast (/Od), guest mode, the frozen river scene (`trun.sh guest+quick ... --set env.CS_ENGINE_ZONES=1`), per
frame. Lead dclf started the item, dclf2 took it over briefly, and dclf3 finished it. No ORG or BasicRHI commits.

## Commits

- perf(dclf): count animated fades before list wait (lead dclf)
- refactor(dclf): delete the batched portal evaluator (lead dclf, `CS_DCLF_PORTAL_EVALUATOR`)
- perf(dclf): keep published input declared bits (lead dclf)
- perf(dclf): keep skin materials until they change (lead dclf3; adds `Skin::MaterialRevision`, outside DCLF's folder)
- perf(dclf): index water members densely (lead dclf3)
- perf(dclf): fill forward sort rows in the producer (lead dclf3)
- Merges of org_integration (73fb9323b, 719c5d2a0, 68510f02c with ORG 46baf56).

## Measuring

The machine's speed changed between captures because other leads ran games in parallel lab slots, so the baseline is scaled by
the engine self ratio. dclf3's baseline (`d3base`, engine self 1.666 ms) is compared with the final build's capture (`d3final`,
1.563 ms, scale 0.938). An earlier after-capture on a quiet machine (`d3after1`, 0.919 ms, scale 0.552, with the reverted
frame texture masks) agrees. Raw numbers are in `scratchpad/dclf3/perf`.

`CS DCLF: scene tables` runs before the window `buckets.py` measures, so it is reported on its own
(`epochtree.py <pkl> "CS DCLF: scene tables"`).

| Zone | base raw | base x0.938 | d3final | base x0.552 | d3after1 |
|---|---|---|---|---|---|
| `CS DCLF: scene tables` (inclusive) | 1.052 | 0.987 | 0.788 | 0.580 | 0.451 |
| `CS.DCLF.Capture.SkinMaterials` | 0.254 | 0.238 | 0.099 | 0.140 | 0.047 |
| water material capture (`ShadingField` self less its overhead) | about 0.104 | 0.098 | about 0.029 | 0.057 | about 0.014 |
| `CS.DCLF.Cpu.ForwardRows` (window) | 0.078 | 0.073 | gone | 0.043 | gone |
| window DCLF CPU | 1.235 | 1.158 | 1.020 | 0.682 | 0.547 |
| window total | 7.855 | 7.368 | 7.258 | 4.336 | 4.255 |

The window DCLF figure includes `CS.DCLF.ListJobsWait` (0.330 raw in the baseline, 0.227 in d3final). That is the render thread
idling on the engine's list job, so it moves with machine load more than with these changes.

Lead dclf's base1 (window 5.05 ms) and its findings stay valid: `ListJobsWait` is the engine's FirstListAccumulationJob, not
DCLF work, and leaves about 125 us of render-thread idle a frame.

## What changed

1. **Fades before the list wait** (dclf). The animation job's fade queue is drained and counted per root slot before the render
   thread waits for the list job. `Fade.Stamp` went from 0.029 to 0.006 ms. Late updates still join.
2. **Batched portal evaluator deleted** (dclf). Queries, results, claims and traces stay. The pass's `Revision()` override is
   kept: without it the Z-prepass recording is not replayable.
3. **Published inputs** (dclf). `PublishedInputs::Resolve` works out declared bits once per declared set.
4. **Skin materials kept** (dclf3). The capture resolved every actor-owned slot through `Skin::ResolveGeometryMaterial` every
   frame (58 compatible slots here; each takes a lock, two `MakeData(Resolve(...))` calls and an owner walk). A slot now keeps
   its row while its surface key and `Skin::MaterialRevision()` hold. The key is the geometry, property, property flags,
   material, texture set, diffuse, normal, parent, alpha property and blending. The revision changes with Skin's revision, its
   character revision, any Prepass change to a geometry entry or texture, the settings the resolve reads and the editor's
   preview fields. A slot is still resolved every 120 captures, staggered, so Skin keeps observing its geometry (it prunes after
   600 frames) and its views stay in use. Slots still waiting for a texture import resolve every frame, and a relocated view
   the capture holds makes every slot resolve. Row 0 follows the revision too.
5. **Water members dense** (dclf3). The water material capture kept its members in a hash map, copied the keys every frame and
   looked each one up again, with an RTTI call per slot. Members are now a slot-indexed array plus a list. A member whose
   geometry, property and material pointer are unchanged skips the RTTI test.
6. **Forward sort rows in the producer** (dclf3). `SetViewBatches` hands the frame slot's sort rows to the next FrameValues
   job, which fills them from the Forward and Refraction lists once its rows are final (beside `StoreLiveSpheres`).
   `ForwardReached` uses them when `ForwardRowsFilled` says that job filled this slot's rows. Otherwise the sort runs without
   them, which it already handles. The slot's rows are touched again only after the slot retires.

## Tried and reverted

- **Frame texture register masks** (dclf3). Visiting only the registered registers, the character light's and the ones a
  capture named (a bitset) measured no gain: `CMP.FT.Registers` 0.024 scaled against 0.025, `HostedView.FrameTextures` 0.032
  against 0.032. The cost is in the registered registers' lookups, not the empty ones. Reverted.

## Decided by the agent, to confirm

- `Skin::MaterialRevision` and the Prepass counters are a small change in `src/Features/Skin.h` and
  `src/Features/Skin/Runtime/Renderer.cpp`, outside DCLF's folder: the change key needs Skin's private state.
- A new owner on the same parent chain, with every other key field unchanged, shows within 120 captures rather than at once.
  DCLF's membership changes already force a slot whose owner identity changes, so this only covers the engine reparenting a
  node without that.
- Editor and settings changes reach DCLF's rows at the next capture, as before (they are compared when the capture asks).
- The fade queue is drained before the list wait. This assumes nothing pushes between the frame start and the list jobs' end
  (late updates are handled anyway).
- The fade batch keeps a claims buffer it never uses, so both batches have the same shape.
- The bench's batched variants (V1, V2, B1b) are gone. `dclf-portal-query-batches.md` keeps their numbers.

## Not done

- `ResolveViewFrameTextures` into the `ListJobsWait` idle: not shown safe (features may change views between the shadow caster
  lights and the depth pass).
- Shadow mask native port (0.07 ms of capture plus the engine's mask draw): it needs `SetupGeometry`'s maths per light kind.
  Follow `dclf-native-sky.md`.
- Other scene-table items (d3after1, quiet machine): `FrameGlobals.Capture` 0.042, `Landscape.Seams` 0.031, `FV.K.Dispatch`
  0.030, `Capture.Wetness` 0.025 (it must call `GetWetness` per actor each frame to advance fades), `FV.K.LeaseBackings` 0.020.
- The first-person forward rows (`ViewSites.cpp`) are still copied on the render thread. They were not measurable here.
- No other DCLF switch only brings back an older path.

## Tests

The p2-dclf-v1 runs at 14:01-14:11 ran DLL hash 6018458d3 (dirty), which is not in the branch's history, so they do not count.

| Config | Label | Result |
|---|---|---|
| guest-interior (final build) | p2-dclf-v4-guest-interior | passed |
| guest+quick (final build) | p2-dclf-v4-guest+quick | passed |
| native-vulkan+quick (final build) | p2-dclf-v4-native-vulkan+quick | passed |
| native-d3d12-quick (final build) | p2-dclf-v4-native-d3d12-quick | passed |
| list-switches (final build); `schema` left switches.json unchanged | p2-dclf-v4-list-switches | passed |
| the same five with the reverted masks | p2-dclf-v3-* | passed, except guest-interior: load timeout while four games shared the machine |
| trun captures | p2-dclf-d3base, p2-dclf-d3after1, p2-dclf-d3final | ran to the exit step (trun marks them failed: no shot) |
