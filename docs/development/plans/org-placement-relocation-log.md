# ORG placement and relocation: implementation log (2026-11-22)

Decisions made while implementing `org-whole-frame-placement.md` and `org-relocation.md` without the user available. Each
entry says what was decided and why, so it can be reviewed and reversed.

## Organisation

- Code-only agents, one per lane, each in its own worktree and branch. The main session merges, builds (Dev-Fast), runs the
  game tests in all three modes, and fixes runtime failures.
- Worktrees live in `external_refs/org-port-worktrees/`:

| Lane | Repo | Branch | Plan steps |
|---|---|---|---|
| org-placement | OpenRenderGraph | `placement/frame-placement` | placement 2, 3, ORG side of 6 |
| org-relocation | OpenRenderGraph | `relocation/follow-path` | relocation 2 (ORG side), 3 |
| basicrhi-retain | BasicRHI | `relocation/copy-retain` | relocation 1 |
| dxvk-reloc | dxvk | `relocation/next-location` | relocation 4 (DXVK side) |
| cs-dclf-reads | CS | `placement/dclf-reads` | placement 1 |

## Decisions

1. **`MayNotWrite` is a pass-level flag, not a per-write flag.** The gated region passes are gated as a whole (a merged
   anchor that draws nothing in a run), so every write of such a pass is unguaranteed. A per-write flag would only add
   declaration noise. API: `ExternalPassDesc::MayNotWrite()` and the matching pass-builder call.
2. **Placement step 1 (DCLF reads) narrows by record kind, not by register.** A depth record binds no frame textures, so the
   Z-prepass passes now declare only the record's buffers. Registers are shared across shaders, so a per-register table was
   not reliable. Measured: `cs.dclf.z.depth` and `cs.dclf.depth-phase2` reads went from 120 to 61, without the shadow mask
   or the post-Z-prepass copy. `kPreviousFrame` kept every entry: the read-order check already exempts DCLF stages, and the
   remaining entries are read early by CS stages. The reflection faces in the depth epoch are still over-declared.
3. **Placement step 2 decision ("does packing beat today?").** For the candidates ORG has today, whole-frame packing needs
   116 MB against the frame arena's 136 MB (native Vulkan, Riverwood). Most large targets are versioned and not candidates
   until step 5, which is where the plan's 125 + 46 MB live. I went on to step 3 and kept it off by default.
4. **Resource identity across builds is a key hashed from name, size, alignment, memory class and backend mask** (org-placement
   agent). IDs change on every build; names don't.
5. **A new frame placement version reaches live graphs by an alias re-placement, not by a rebuild.** Each preparation compares
   the host's current version id (a lock-free probe) with the graph's, and re-plans as a structural edit when they differ,
   like the existing packing-strategy re-placement. This is how step 4 ("versioned placements") is done: the frame placement
   is the version, the re-placement is the commit point. Without it, a graph that moved a resource another epoch binds broke
   that epoch's executable ("Binding replacement requires a structural alias edit").
6. **A graph never moves a resource the version knows from other epochs but does not cover here.** It keeps whatever backing
   another graph gave it, bound in its own structural edit. Two graphs would otherwise move it back and forth.
7. **A newer build of an epoch claims a frame placement it finds in place.** The build it replaces skips that resource when it
   gives its placements back at its end. Before this, the old build took the backing away from the new one.
8. **Versions share block memory per memory class and backend set** (`FramePlacementMemory`), with 1.25 headroom; replaced when
   too small or more than twice too big. Graphs on the old version share the heap with the new one for a frame or two; the
   alias ledger orders the GPU work. Content may glitch for that frame at a version switch. Measured: alias memory 280 MB
   (a heap set per version) went to 205 MB.
9. **`ORG_FRAME_PLACEMENT` stays off by default.** With it on, native Vulkan `vram-loads` is about 70-140 MB above off (205 MB of
   frame placement heaps against 136 MB of frame arena). Causes: the headroom, and heaps of early versions that graphs which
   no longer prepare (menu epochs) still hold. Step 7 (switch on, delete the old layers) waits for step 5's candidates.
10. **The relocation copy goes through the first graph's `ExternalClient`** (org-relocation agent), so the state ledgers give
   the real barriers and later users on any queue wait only if they touch the moved resource. This replaces the plan's
   "other queues' first batch waits" rule. `GpuBufferBacking::AdoptRelocated` was not added; an owner-level swap covers it.
11. **DXVK keeps the old relocation struct and adds `dxvkPollInteropRelocations2`** (interop version 8, 7 still accepted), because
   the client allocates the array and the poll has no version input. The ready point is DXVK's tracking timeline value, not a
   CS chunk sequence, since a chunk sequence only says the CS thread processed the work.
12. **BasicRHI `CopyRetain` hands the old allocation back through a caller-owned `pRetained` array**; retained allocations are
   never chosen as move sources. Tests pass on D3D12 and Vulkan (Vulkan needs `VK_LOADER_LAYERS_DISABLE=*nsight*`).
