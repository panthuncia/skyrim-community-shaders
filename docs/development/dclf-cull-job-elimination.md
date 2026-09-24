# DCLF: a frame without the engine's culling jobs

Status: phases 1 and 2 done, 2026-09-24; phase 3 next. The reasoning behind it is in [dclf-status.md](./dclf-status.md), "What removing the culling
jobs takes". The current state is in [drawcall-limit-fix.md](./drawcall-limit-fix.md), "The primary's cull without
DCLF's objects".

## Goal

The engine's list jobs (`ListAccumulationJob`) and the per-object work they do go away:
-   for DCLF's objects, entirely;
-   for the residue, replaced by a flat visibility pass with no traversal.

Every view culls on the GPU. The per-object engine state the cull maintained moves to one of two places:
-   events, for what changes rarely: switch selection;
-   the GPU, for what changes per frame with the camera: fade, LOD, and the tree clock.

The per-object state tolerates a frame of latency. The residue's visibility does not.

## Phases

1.  **A stood-in object needs nothing from the CPU per frame for its main pass** (detailed below).
    -   The sun's bits move to the GPU.
    -   The synthetic pass becomes static, rebuilt on change only.
2.  **Visibility feedback, and the per-object state from it.**
    -   A per-entry, frustum-only visibility bitmap from the GPU cull, read back a frame late.
    -   The fade, LOD, leaf and tree updates and the tree clock's `kAccumulated` run on DCLF's worker from that
        bitmap, not in the list jobs; later as GPU state (fade and LOD in a persistent buffer, the cross-fade copy as a
        second dithered draw).
    -   Admission comes from readiness (the pipelines and records exist), not from a draw, so entries never seen in view
        leave the lists too.
3.  **Switch selection as events.**
    -   Hook the writers of `NiSwitchNode`'s index (reverse engineer them: harvest, destruction, activators), plus the
        catch-up update of a newly selected child, run in the event handler.
    -   "Which child is live" becomes table state; the per-frame switch test and the stale-child fallback go away.
4.  **DCLF's entries leave the lists outright.**
    -   With phases 1-3, an admitted entry needs nothing from its job. The list filter is built on the worker from an
        immutable set: the list edit of the first version, but with the decision precomputed.
    -   The `Process1` stand-in shrinks to the entries that still need the engine.
5.  **The residue without traversal.**
    -   Decals as synthetic passes: the order key rebuilt from the traversal position, and depth-writing decals drawn in
        DCLF's Z-prepass.
    -   `BSOrderedNode` subtrees and actors (one entry, `ObjectLODRoot`'s second child) brought in.
    -   What stays native (effects, blended, LOD, billboards) is found visible this frame by a flat frustum pass on a
        worker and handed to the engine's registration (`AppendVirtual`).
    -   Then the list jobs are no longer queued. The extra list, room and portal visibility, and the list processes'
        camera and planes stay.
6.  **The other views.**
    -   The sun's actors join DCLF's shadow set.
    -   Local shadow lights need the shadow-light selection rule (`FUN_1414fcf80`), which also makes the synthetic pass
        valid under local lights. That lifts the primary cut's precondition.
    -   Reflections and the precipitation mask as DCLF-native variants.

## Phase 1: done

[drawcall-limit-fix.md](./drawcall-limit-fix.md), "The sun's bits on the GPU":
-   the static rule plus a per-draw cascade test in BuildDraws;
-   `kObjectSunMiss` on the object word;
-   `UnifySunBits` for engine-registered passes.

GPU = CPU exactly, 0 holes, and pipelines 88-90 -> 72-74. Caching the whole synthetic pass moved to phase 4.

## Phase 2: done

[drawcall-limit-fix.md](./drawcall-limit-fix.md), "Visibility feedback":
-   the frustum stamps from depth phase 1;
-   an asynchronous readback ring (as BasicRenderer's CLod streaming) with DCLF's own timeline;
-   the decode on the worker, kicked after registration and joined at Present;
-   the roots held by the frame's tag.

Admission from readiness moved to phase 4, where the pass becomes table state.

## Next: phase 3, switch selection as events

-   Reverse engineer the writers of `NiSwitchNode`'s index (`+0x12C`).
-   Make "which child is live" table state.
-   Run the newly selected child's catch-up update (`UpdateDownwardPass`) in the event handler.
-   The per-frame switch test and the stand-in's stale-child fallback then go.
