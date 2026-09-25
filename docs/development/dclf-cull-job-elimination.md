# DCLF: a frame without the engine's culling jobs

Status: phases 1-3 and phase 4's step 1 done, 2026-09-24; phase 4's step 2 next. The reasoning behind it is in [dclf-status.md](./dclf-status.md), "What removing the culling
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

## Phase 3: done

[drawcall-limit-fix.md](./drawcall-limit-fix.md), "Switch selection by event":
-   the ten index stores patched (the tree manager, the local map, harvesting) and `NiSwitchNode`'s child edits detoured;
-   the catch-up (`CatchUpSwitch`) when the walk applies an event, and at attach;
-   the switch trait off the per-frame set (1,707 -> 1,057 entries at Riverwood);
-   `memberLive` in the stand-in, and the stale-child fallback gone.

## Phase 4 in detail: resident entries

**Why not a list edit.** The engine rebuilds the scene lists every frame (`DrawWorld_BuildSceneLists`, `0x14064bc20`,
appending each reference root round robin through `FUN_14021cf40`). Filtering there would also take the entries out of
the sun's full-frustum cull and the precipitation mask, which read the same lists. The jobs keep the lists; what goes is
the work an entry needs from its job.

**What an entry still takes from its job** (the stand-in, per frame):
-   the frustum test that picks which synthetic passes are built;
-   the root's settled check;
-   the hidden walk and switch selection per member;
-   the hand-over of the engine's members.

The synthetic pass itself also goes through the accumulate phase every frame, which restores what it patched at the next
walk. So nothing about an entry persists from one frame to the next except its admission.

**Resident entries.** An entry whose record needs nothing per frame is made resident:
-   **Its objects' accumulated half persists.** It is built once, through the accumulate phase's own patch, from its
    synthetic pass. It is never restored per frame, and it keeps `kObjectNativeVisible`, so BuildDraws draws it
    whenever the GPU's cull finds it (frustum, occlusion, the sun's cascade test). No CPU visibility is involved.
-   **Its list job returns at once**, from a lookup by root.
-   **Its fade, LOD and `kAccumulated`** come from the visibility feedback, as today for stood-in entries.
-   **The slots stay alive.** Each frame the accumulate phase keeps a resident's pipeline and material slots alive
    (`lastUsed`, and the lighting template when no other object used the pipeline). That is O(residents) writes, not
    patches.

**Joining** (render thread, before the list jobs, when the cut applies, a bounded number a frame):
-   admitted;
-   plan `Plain`, `FadeRoot` or `LeafRoot` (trees wait for the height test on the GPU);
-   every member DCLF's or unselected;
-   the root settled;
-   every DCLF member with a record that is written only by events: no face, actor, skin or animated shading, and not
    written in full every frame.

**Leaving** (immediately, in the frame it happens):
-   **The walk rewrites or releases a resident's record** (`WriteObject`, `ReleaseObjectSlot`, the slot check): an
    event changed it, so the entry goes back to the stand-in and may join again.
-   **The patch fails** (a verdict of the frame, a material not ready, extras rows needed).
-   **The feedback decode finds the root no longer settled** (it started to fade or cross-fade).
-   **A frame where the cut does not apply** (a local shadow light, the sun's exclusion not live, the menu toggle):
    every resident leaves. A stale snapshot does not end residency, since residents do not use it.
-   **The frame globals the static sun bits read change**: every resident leaves.

**Entry 0 of a list** goes through the engine's `Process2`, so the engine registers it even when it is resident. The
accumulate phase leaves a resident's record alone in that case, and static ownership withholds the engine's pass.

**Checks:**
-   A resident parity (`CS_DCLF_RESIDENT_PARITY`, every 60 frames): each resident's synthetic pass is built again and
    compared with the one it was patched with, and its record with the patch.
-   Walk parity, and 0 holes.
-   Main-pass objects: the engine's kept ones plus the residents in view match a run without the cut.

**Steps:**
1.  Resident entries as above, with admission from a draw as today. **Done**
    ([drawcall-limit-fix.md](./drawcall-limit-fix.md), "Resident entries"). At Riverwood about 580 of the 827
    stood-in entries are resident; the render thread saves about 0.1 ms; the list jobs cost the same.
2.  Admission from readiness, so entries never seen in view become resident too. That needs the fade distance on the GPU:
    an out-of-view resident is not serviced, and one that drifted past its fade distance would draw for a frame when it
    comes into view. That frame of latency exists already for stood-in entries.
3.  Trees: the height test and the tree animation on the GPU.
4.  Entries with engine members stay in the stand-in until phase 5.
