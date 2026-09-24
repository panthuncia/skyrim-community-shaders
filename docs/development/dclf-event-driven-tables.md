# DCLF: object tables built from events

Status: investigation, 2026-09-24. Nothing here is built. It refines Phase 3 of
[dclf-gpu-driven-frame.md](./dclf-gpu-driven-frame.md) ("a persistent GPU scene").

## The question

DCLF rebuilds its object tables every frame by walking its whole tracked set. Since the walk moved to a worker,
it runs while the engine is still modifying the scene: culling, updating reflections, toggling nodes. Two bugs
have come from that so far:

-   the NPC head drops: transient `kHidden` bits;
-   `Salmon:0` drops, not yet traced.

The walk's own documentation already lists three other values that change under it (fades, billboards, texture
transforms). Each race so far was fixed on its own terms.

The alternative: keep the tables as persistent state, and change them only when the engine reports a change
(an object added, removed, hidden, shown, moved, or given another property). The full walk then becomes a
synchronous validation, not the mechanism. This document covers what that needs.

## What the walk does today

The tracked **set** is already event-driven. `SceneTracker` detours the `NiNode` child functions
(`AttachChild`, `DetachChild*`, `SetAt*`) and pushes events onto a lock-free stack from whichever thread
attaches or detaches. `SceneStore::ProcessEvents` applies them before the walk, and every Present. A slice of
tracked geometry is re-checked against its root each frame, as a safety net for missed detaches.

Everything after membership is per frame. `SceneStore::SceneWalk` iterates **every tracked geometry** (10,021 at
Riverwood, about 2,700 in Dragonsreach) and rebuilds the per-frame vectors from scratch (`objects`, `draws`,
`bones`, `faceStreams`, the shadow columns, and so on). It runs on the `CS DCLF worker` from `Main::Draw` to
`AfterShadowMaps`, and takes 1.0-1.6 ms (max 2.9). The render thread first runs `PrepareSceneJob`
(`GpuResources::Touch` and `UpdateSkin`, 0.14-0.18 ms). For each geometry the walk reads:

| Input | Source | Read how today |
| --- | --- | --- |
| Type, skin instance and partitions, renderer data, vertex and index buffers | the geometry, its skin | `ClassifyStatic`, cached per object for 64 frames behind a witness (renderer data, property, material, fade state) |
| Shader property, its cast, material, material alpha, property flags, lighting descriptors | the property and material | the same classification |
| Hidden, app-culled, switch-node selection, actor ancestry | `NiAVObject::flags` bit 0 on every node up to the category node; `NiSwitchNode` index | `ClassifyFrame`: every frame for face shapes and switch children, otherwise with the 64-frame cache |
| Fade | `BSFadeNode::currentFade`, written by the main camera's cull | `ClassifyFrame`, `FadeStateOf` |
| Skin partitions drawn | the fade node's LOD level, the dismember flags | `SkinPartitionMask`, every frame |
| World, previous world, world bound | the geometry | every frame |
| Bone palettes | `UpdateSkin` (render thread only), then `boneMatrices` | every frame per skinned object; a skin not updated ahead is a "skin miss", and the join rebuilds inline |
| Face positions | `FaceSnapshots`, from the morph job's hook | already an event |
| Shadow caster verdict and technique, alpha-tested diffuse | property flags, alpha property, material | every frame |
| Sun entry node | the ancestor chain | every frame (`SunEntryOf`) |
| Geometry slot | `GpuResources` | every frame, render thread for new buffers ("geometry miss") |

Two per-frame loops consume the engine's own walks and stay per frame whatever the tables do:

-   **The accumulate phase** (render thread, 0.32 ms) captures the main camera's registrations: which objects
    the engine found visible, and their pass descriptors. Replacing it is the GPU-driven plan's Phase 1, not
    part of this.
-   **`RefreshFrameConstants`** resamples per-frame values (technique constants, animated shading, UV extras,
    skin wetness). These are per-frame by nature.

## What actually changes: measured

`CS_DCLF_TEMP_CHANGE_RATES` (a temporary probe, removed) compared each tracked geometry's inputs with the
previous frame's. It ran with the walk on the render thread, before the culls start, so every value was read
at a quiet point. The run had 1,500 frames at Riverwood, then Dragonsreach.

| Per frame | Riverwood (10,021 tracked) | Dragonsreach (2,712) |
| --- | --- | --- |
| World transform changed | 181-207 (max 309), about half actor-owned | 133-178 (max 590 during the load) |
| Distinct geometries that moved in a 300-frame window | 210-222 (416 in the window with the load) | 150 |
| Property, material, renderer data, alpha property, skin instance | 0 (0.01 during the load) | 0 |
| Property flags | 0 (0.05 in the first window) | 0 |
| Effective hidden state, read at a quiet point | 0 | 0 |
| Fade value | 63, then 27, then 0 as the scene settled | 12 during the load, then 0.1 |
| Membership events (attach / detach) | about 0.04 / 0.1 | a burst of about 11,600 / 3,700 at the load |
| Distinct ancestor nodes below the category nodes | 7,195 | 1,790 |

What that says:

-   **About 2% of the tracked set moves per frame, and it is the same 2%.** Actors, animated references,
    critters and physics. The other 98% is re-read every frame to find that nothing changed.
-   **Structural changes are events, not a stream.** In steady state, no property, material, buffer,
    alpha or hidden state changed in 1,200 frames. They happen at loads and at gameplay events (equipping,
    dismemberment, enable and disable).
-   **Polling per node instead of per geometry would not help.** The tracked set has 7,195 distinct
    ancestor nodes against 10,021 geometries.
-   **The hidden races are view-transient.** The bit read at a quiet point never changed. The drops came from
    bits the engine sets for part of one view's rendering, then restores.

## The design: tables as persistent state

### Stable object slots, and a live list

Each tracked, eligible geometry holds one object slot for as long as it stays eligible. Every per-object
column (record, draw, bone offsets, face stream, shadow columns, lights, shading) is indexed by slot. A live
list, maintained on insert and remove, is what the builds iterate.

"The transform witness cannot pay for itself" rejected stable slots as a *performance* change: a record costs
13-26 ns to write. The argument here is different. Slots are what lets the tables persist across frames, so
that per-frame work is proportional to what changed and the worker never has to walk the live scene.

### Every input gets a source

| Input | Event today | What is needed | Per-frame remainder |
| --- | --- | --- | --- |
| Membership | `SceneTracker` attach and detach | done; the rolling re-check stays as an alarm | none |
| Moves (world, previous world, bound) | none | **The movable set.** A slot is movable if it can change transform without a membership event: actor-owned, under the Dynamic category node, physics-driven, or animated by a controller. Only those are read each frame. A static reference is assumed to move only by `TESObjectREFR` position calls (`SetPosition`, `MoveTo`, `Update3DPosition`); hook those, or verify they detach and reattach. The validation walk is the gate: no non-movable slot may ever differ. | about 200 reads a frame instead of 10,000 |
| Bone palettes | none | read for the movable skinned slots only; the GPU-driven plan's bones-on-GPU removes `UpdateSkin` later | per visible skinned object |
| Property, material, alpha property, skin instance, renderer data | materials: `MaterialSources` write hooks; renderer data: `GpuResources` | Reverse-engineer the write sites for the property and alpha pointers and the property flags, and hook them. Most swaps come with new geometry (an equip attaches new shapes), which membership covers already. Renderer-data creation and release come from `GpuResources`' own lifetime tracking. | none |
| Persistent hidden and shown | none | **The hardest input.** The flag is written inline by about 70 engine functions (`or [reg+0xF4], 1`) with no setter to hook. Most are outside the tracked roots (sky, projectiles, inventory and map 3D). The rest need an inventory, including writes through a register (`mov`, `or`, `mov`), which the scan did not cover. Candidates: enable and disable (check whether they detach instead), actor 3D hiding, the player's 3D. | none once hooked |
| Per-view hides | none | A per-slot **view mask** for DCLF's views (main, sun, and later the others), set by rules reverse-engineered per writer: the first-person skeleton is only in the first-person view; the player's 3D is hidden in the cube-map reflection; portal traversal's toggles are internal to the cull and don't count. `CaptureCullHiddenBits` is the first, hand-listed version of this. | none |
| Switch-node selection | none | hook the index writes (`NiSwitchNode` `+0x12C`) | none |
| Fade, partition LOD | written by the main camera's cull | per frame by nature; the GPU-driven plan computes it from distance on the GPU or a worker | per frame on the GPU |
| Face positions | `FaceSnapshots` | done | per morphing head |
| Animated shading, texture transforms | controller write hooks (`MaterialSources`) | done for shading; texture transforms follow the same hooks | none |
| Per-frame globals (fog, eye, wind, clocks) | none | GPU (`RefreshFrameConstants`' job, split out) | per frame, not per object |

### One quiet point, and a worker that reads no scene

The render thread already has a moment when the scene is final and no cull or reflection update runs: the
`Main::Draw` hook where the walk is kicked today. At that point it would:

1.  Drain the event queues (membership, property, hidden, switch) into a delta list.
2.  Copy the movable slots' transforms, bounds and bone rows into plain buffers (about 200 objects; tens of
    microseconds).
3.  Hand the delta and the copies to the worker.

The worker applies them to the tables and builds, reading only what it was handed. It never touches an
`NiAVObject`. The whole class of race goes away: the worker cannot see a transient bit or a half-written
transform, because it reads no engine memory at all.

Events that arrive from other threads during the frame (loader threads, morph jobs, Havok) queue up and apply
at the next quiet point. That is the same one-frame rule membership follows today.

### The full walk as synchronous validation

The current walk stays as the reference. `CS_DCLF_WALK_PARITY=1` would run it on the render thread at the quiet
point every N frames and compare its tables with the incremental ones, slot by slot. `SameSceneTables` already
does this for the asynchronous walk's probe. The gate for every step: 0 differences through the standard
matrix (road, Riverwood, Dragonsreach, a dungeon, cell changes, save and load, the live toggle), with the full
featureset.

## What it needs

**Engineering:**

-   Stable slots and the live list, with every table consumer changed to them. That's the bulk of the work:
    `IndirectDraws`' builds, the shadow build, the accumulate phase's object lookup (`FindObject`, already on
    `Tracked::objectId`), `RefreshFrameConstants`, and the parity checks.
-   The quiet-point capture and the delta hand-off, replacing `PrepareSceneJob` and the walk's kick.
-   The per-slot view mask, and its use in the shadow build and the draws' visibility.
-   `CS_DCLF_WALK_PARITY`.

**Reverse engineering, before the matching step:**

-   **World-transform writers for static references.** Do position changes always detach and reattach, or do
    `SetPosition`, `MoveTo` and `Update3DPosition` move 3D in place? The answer decides whether static slots
    need a move hook.
-   **Persistent `kHidden` writers inside the tracked roots.** Complete the scan (register-mediated writes,
    `and ~1` clears), and for each writer decide whether it is persistent or view-transient.
-   **Property, alpha-property and property-flag writers.** Which paths change them on geometry that stays
    attached.
-   **Switch-index writers.**
-   **What the ~100 non-actor movers are** (animated references, critters such as the salmon, physics), to
    confirm the movable-set rule covers them structurally rather than by observation.

## Phases

1.  **Slots, same walk.** The walk writes into persistent slots instead of rebuilding vectors, and consumers
    move to slots. No behaviour change. Gate: bit-identical tables against a from-scratch rebuild
    (`CS_DCLF_WALK_PARITY`).
2.  **The quiet-point capture.** Movables and bones are copied at the kick. The worker applies deltas and reads
    no scene; statics are read only at attach. Gate: walk parity, including across the matrix's cell changes
    and loads.
3.  **Structural events.** Property, alpha, flags, switch, and persistent hidden: hooks per the RE results.
    Remove the 64-frame classify cache and its witness.
4.  **View masks.** The transient hides become rules. `CaptureCullHiddenBits` is folded in.
5.  **The walk leaves the frame.** It runs only under `CS_DCLF_WALK_PARITY`.

The per-frame worker work drops from 1.0-1.6 ms to the size of the delta plus the builds. On the render thread,
the capture replaces `PrepareSceneJob`, at about the same cost.

## Relation to the GPU-driven plan

This is the CPU side of the GPU-driven plan's Phase 3, stated precisely enough to build. It doesn't remove the
per-frame loops that depend on the engine's culls: the accumulate phase, and fades. Those go when DCLF's objects
leave the engine's walks (that plan's Phases 1 and 2). The two are independent in order, but slots make both
easier: GPU visibility, fades and view masks are all per-slot data.
