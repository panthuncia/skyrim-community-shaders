# ORG placement and defrag, second round (2026-11-22)

Follows `org-placement-relocation-log.md`. That round left frame placement costing memory and defrag freeing nothing. This
round removes the blockers it found. Decisions go into the log under "Round 2".

## Goals

1. Native engine targets become ORG-owned textures, so frame placement and defrag can reach them.
2. Buffers can move at loading screens (GPU idle), including buffers whose device address or buffer view ref went out.
3. Per-frame pooled buffers live in blocks of their own, apart from long-lived buffers and textures.
4. Frame placement is correct across epochs and repeated runs, and its memory overhead is understood and cut.

The switches stay off by default until a measurement beats off (`ORG_FRAME_PLACEMENT`, `CS_ORG_DEFRAG`).

## Lanes

| Lane | Repos (worktree) | Branch |
|---|---|---|
| buffer-moves | BasicRHI (`basicrhi-ref-retarget`), ORG (`org-buffer-moves`) | `relocation/ref-retarget`, `relocation/buffer-moves` |
| placement-org | ORG (`org-placement2`) | `placement/epoch-entry-runs` |
| dclf-follow | CS (`cs-dclf-follow`), OMS (`oms-memory-group`) | `relocation/dclf-follow`, `relocation/backing-group` |
| engine-targets | CS (`cs-engine-targets`) | `placement/native-engine-targets` |

Worktrees are in `external_refs/org-port-worktrees/`. Agents write code only. The main session merges, builds, runs the game
and fixes runtime failures.

## Contracts between lanes

Names are fixed here so the lanes can work at once. A lane that must change one says so in its report.

**BasicRHI** (buffer-moves)
- `Result Device::RetargetBufferViewRef(ResourceHandle from, ResourceHandle to)`: gives `from`'s buffer view ref pair to `to`.
  The slot stays the same. The pair's descriptors are rewritten for `to` everywhere they live: the D3D12 staging heap and
  every full-size shader-visible heap, and the Vulkan heap or descriptor buffer. `from` then has no pair and its destruction
  frees none. `to` must not have a pair yet. The caller guarantees that no GPU work reads the slot while this runs (an
  idle GPU).
- `bool Device::HasBufferViewRef(ResourceHandle)`.

**ORG** (buffer-moves)
- The defragmenter can move buffers (`moveBuffers`). A buffer with a view ref pair, or of class `Written`, moves only in an
  idle campaign. Adopting it at idle rewrites its descriptors in place and calls `RetargetBufferViewRef(old, new)`.
- Journal entries for buffers carry `oldAddress` and `newAddress` (device addresses, 0 when none), so followers can rebase.
- `enum class org::MemoryGroup : uint8_t { Default, PerFrame }` and `Buffer::SetMemoryGroup(MemoryGroup)`, called before
  `Materialize`. `UnmaterializedLike` copies it. A `PerFrame` buffer is allocated from its own block pool, never shares a block
  with a `Default` allocation and is never a defrag move target or source.

**ORG** (placement-org)
- A resource used by several epochs in one frame enters only its first epoch from Undefined. Later epochs enter from the
  layout the previous one left it in (the fix for log entry 23).
- `org::Resource::SetContentAcrossRuns(bool)`: the resource keeps its contents between runs of a repeated epoch. Frame
  placement gives it one lifetime over all runs in the frame, and the entry rule treats later runs as continuing.

**OMS** (dclf-follow)
- `BackingFlags::perFrame` (bool). `AllocateGraphHostBacking` then calls `SetMemoryGroup(MemoryGroup::PerFrame)`.

**CS holders** (dclf-follow)
- `RenderGraph::Relocation::FollowAtIdle(const org::Resource&)`: marks the resource `Written`, so it moves only at idle, and
  states that CS follows the journal for it. `Pin` stays for raw handles, raw views, ImGui and DXVK-owned resources.
