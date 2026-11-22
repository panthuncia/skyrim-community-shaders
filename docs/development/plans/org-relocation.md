# ORG resource relocation and defragmentation (plan, 2026-11-22)

Direction (user, 2026-11-22): not "make everything a versioned resource". An internal ORG relocation and defragmentation
system covers both modes. The re-import path DXVK triggers stops being an interop API and becomes part of ORG. In DXVK
(guest) mode DXVK keeps its own allocations and defragmentation and hands ORG the new locations. In native mode ORG drives
everything through BasicRHI's allocator.

Status: plan only, from a read of `org_integration` with ORG `e20aa19`. Nothing here is implemented.

## 0. What exists today

### DXVK (guest) relocations

- When DXVK moves a resource ORG tracks (a defragmentation move or an eviction), it gives the resource new storage and
  queues the old storage for ORG.
  - The queueing happens in `DxvkContext::invalidateBuffer` (`dxvk_context.cpp:1416`) and `invalidateImageWithUsage`
    (`:1527`), which call `DxvkMemoryAllocator::pushOrgRelocation` (`dxvk_memory.cpp:2343`).
  - That marks the old storage `OrgRetired` and counts it as a lock, so its chunk is not picked again.
  - It queues `{cookie, serial, image, storage}` (`dxvk_memory.h:1172`).
- `pollOrgRelocations` (`:2383`) hands the entries out, and `releaseOrgStorage` (`:2398`) takes them back. The interop
  wrappers are `D3D11VkInterop::PollRelocations` and `ReleaseStorage` (`d3d11_org_interop.cpp:635`, `:665`).
- Only read-only imports may move. `TrackResources` pins anything the GPU writes (`:550-557`, `:580-587`), because after a
  move the client would read the old storage's stale contents.
- An entry carries no new location, so the client has to describe the resource again.

### CS consumers (DCLF only)

- `Hooks.cpp:603` calls `DCLF::ImportRelocations::Poll` once per Present (`ImportRelocations.cpp:35`), through
  `RenderGraphRuntime::PollRelocations` and `ReleaseStorage` (`RenderGraphRuntime.cpp:2892`, `:2901`). The old storage is
  wrapped in a `shared_ptr` whose last holder releases it (`ImportRelocations.cpp:24`).
- Textures: `GpuTextures::NoteRelocations` (`GpuTextures.cpp:1232`) supersedes the entry, drops it from the registry and
  logs `RelocatedViews`.
  - The next use imports the image again, which gives a new ORG resource and a new bindless index.
  - A move that arrives before its import is registered waits up to 2 s in `unclaimed`.
- Buffers: `GpuResources::PostRelocations` and `TakeRelocations` (`GpuResources.cpp:220`, `:232`) drop the entry, which is
  then resolved again.
- Pinned, so they never move: FrameAnchors imports and engine-texture imports (`GetResourceInfo` → `LockImage`,
  `d3d11_org_interop.cpp:270`).
- The VRAM ledger reports old storages still held as "Storages retired to ORG" (`VramLedger.cpp:464`).

### BasicRHI's allocator (a D3D12MA port)

The defragmentation API is present and nothing calls it:

| Piece | Where |
|---|---|
| `Allocator::BeginDefragmentation` | `rhi_allocator.h:757` |
| `Pool::BeginDefragmentation` | `:659` |
| `DefragmentationContext::BeginPass` / `EndPass` | `:674` |
| `DefragmentationPassMoveInfo` | `:622` |
| Copy / Ignore / Destroy move operations | `:379` |
| Fast / Balanced / Full algorithms | `:357` |

- **Blocker: EndPass frees the old resource at once.**
  - On Copy, EndPass calls `pSrcAllocation->SwapBlockAllocation(pDstTmpAllocation)` and then
    `pDstTmpAllocation->ReleaseThis()` (`rhi_allocator.cpp:8602-8610`).
  - After the swap, the temporary allocation holds the old resource and the old range, so EndPass destroys them.
  - ORG's lease (`TrackedHandle::CaptureAllocationLease`, `TrackedAllocation.h:302`) holds the `Allocation` object. After
    the swap that object names the new resource, so frames in flight that recorded the old handle would use a freed one.
- **Finding a move's owner.** `Allocation::Get/SetPrivateData` (`rhi_allocator.h:495`) is unused by ORG.
  - The swap does not exchange `m_privateData`, so an owner pointer set on the source allocation survives the move.
  - The defragmenter writes only the temporary allocation's private data (`:8552`).
- **What the defragmenter can see.**
  - Not in block vectors, so never seen:
    - alias pools and arenas: `AllocationFlagCanAlias` goes to `AllocateHeap`, a committed heap (`FrameAliasArena.cpp:80`,
      `RenderGraphPersistentStructural.cpp:886`, `rhi_allocator.cpp:7117`); their placed transients are never seen;
    - shared buffers (committed);
    - native exempt textures, plain images and native buffers (`CreateCommittedResource`, `NativeDevice.cpp:1123`,
      `:1424`, `:1487`).
  - In the 64 MB blocks:
    - graph-owned `PixelBuffer` and `Buffer` backings;
    - `org::memory::AllocatedResource`, which includes the streaming windows (602 MB, `ORGWindows.cpp:324`);
    - frame arena chunks.

### ORG follow machinery that already exists

- **External resources.** `ExternalTextureResource::RefreshShared` and `ExternalBufferResource::RefreshShared`
  (`ExternalTextureResource.cpp:112`, `ExternalBufferResource.cpp:55`) replace the native resource of the same graph
  resource.
  - They rotate descriptor slots (`GloballyIndexedResource.h:257`).
  - They retire the old resource through `DescriptorHeapManager::RetireNativeResource` (`DescriptorHeapManager.cpp:266`).
    That retirement waits for a frontier taken behind the `AssignmentGate` (`:298`), so tickets still queued on the
    submission thread are covered.
- **Graph textures.** `PixelBuffer` keeps `m_backing` and `m_backingGeneration` (`PixelBuffer.cpp:219-262`). Its
  descriptors are written in place into reserved slots (`UpdateDescriptorContents`, `:250`).
- **Persistent executables.** A `RotationKey` is {resource ID, backing generation, views pointer, API handle}
  (`RenderGraphPersistentState.h:170-192`).
  - Executables poll it every frame (`RenderGraphPersistent.cpp:1277-1317`).
  - A changed key with the same shape becomes a binding edit, not a rebuild.
  - `BindVersion` (`:258`) captures again on a key change.
- **Frames in flight** own their `ResourceBindingSnapshot`s (`PublicationBindingBundle.h:20`): handle, views, allocation and
  descriptor owners.
- **Gap: kept recordings.** Replayable and revision recordings (`GraphHost.h:92-103`, `RenderGraph.h:375-385`) are checked
  for version currency only. A change of backing generation does not make them stale.
- The frame arena's chunks never move, by contract (`FrameArena.h:20`).

### CS code that holds raw handles or indices

- `NativeResource::resource` and `image` (`NativeDevice.cpp:398-411`). Tripwire handles own the record.
- `ImageRegistry`, keyed by the raw image handle (`NativeDevice.cpp:491`, `ORGWindows.cpp:216`).
- Cached bindless indices:
  - `ORGWindows.cpp:219` (`window->index`);
  - `PublishedResources.cpp:446`;
  - DCLF `SceneBuffers.h:66`, `FrameSlots.cpp:43` and `Resources.cpp:76`.
- DCLF's device-address pools write buffer addresses into GPU data (`PayloadVersions.cpp:406`).

## 1. Core abstraction

A new ORG module, `org::relocation`, in `include/Resources/Relocation.h` and `src/Resources/Relocation.cpp`.

```
struct Relocation {
  uint64_t resourceID;                  // stable identity: Resource::GetGlobalResourceID
  uint64_t fromGeneration, toGeneration;
  rhi::ResourcePtr next;                // the new native resource; null when the Allocation already names it
  std::shared_ptr<const void> retired;  // keeps the old backing (resource and memory) alive
  runtime::SubmittedPoint ready;        // the GPU point after which `next` holds the data
  enum class Source { Dxvk, Defragmenter, Owner } source;
};
```

Relocations come from sources, each an `IRelocationSource { size_t Poll(std::span<Relocation>); }`:

- **DXVK** (`DxvkRelocationSource`, in CS `src/RenderGraph/Relocation/DxvkSource.cpp`): DXVK chose the move and already
  copied the data.
- **Defragmenter** (`org::relocation::Defragmenter`, in ORG core): all of native mode. It also covers ORG's own allocations
  in guest mode, which still show 371 MB of slack.
- **Owner**: an owner that holds raw handles rebuilds the resource itself through a hook, and ORG retires the old one.
  Streaming windows are the main case.

### One commit point

`relocation::Service::Commit()` runs on the GraphHost owner thread at the frame boundary, before preparation: the adoption
point, where Present polls today. For each relocation it:

1. Takes `BackedResource::BackingMutex()` (`BackedResource.h:45`), so no capture sees a half-moved backing.
2. Swaps the backing.
   - Graph resources: new `PixelBuffer::AdoptRelocatedBacking` and `GpuBufferBacking::AdoptRelocated`, which bump
     `m_backingGeneration` and call `ForgetPublishedAllocation`.
   - External imports: `ExternalTextureResource::RefreshShared` and `ExternalBufferResource::RefreshShared`.
     `ExternalBackingResource` gets a backing generation too, so the `RotationKey` moves for imports as well.
3. Updates descriptors.
   - In steady state it rotates them (`RotateDescriptorSlotsForPublication` plus `AssignDescriptorSlots`): the index is
     new, and the old slots retire with the frontier.
   - When the GPU is idle it may rewrite them in place (`UpdateDescriptorContents`), keeping the index.
   - It never rewrites in place while frames are in flight.
4. Retires the old backing through `DescriptorHeapManager::RetireRelocated(retired)`, a new `DeferredRelease` kind, after
   the copy's submission (section 2).
5. Appends `{resourceID, toGeneration, reason}` to a `RelocationJournal` (the old `GpuTextures` relocation log, moved into
   ORG). Clients read it with a cursor. A read returns `false` when entries were lost, and then the client re-reads
   everything.

### How each holder follows

- **Nothing new needed:**
  - persistent executables, frame slots and `BindVersion`: the generation bump changes the `RotationKey`, which is a
    binding edit at the next `PollBackings`;
  - frames in flight keep their snapshots;
  - imported DXVK resources: the same `ExternalTextureResource` keeps its identity, and its `VersionedTexture` slot does not
    change.
- **Changes in ORG:** kept, replayable and revision recordings get `PersistentRecordingBackingsCurrent(recording)`.
  - It compares each snapshot's `backingGeneration` with the live one.
  - `EpochRecordingCurrent` and `BindPersistentTicketRecording` treat a mismatch as stale and record again.
  - The same check applies to FrameAnchors schedules (`FrameAnchors.cpp`, `Schedule`).
- **Changes in CS:**
  - `ImageRegistry::Rekey(oldImage, newImage)`;
  - `NativeResource::Rebind(next, imageInfo)` under a record lock, for tracked textures and wrapped images;
  - DCLF's tables, `ORGWindows`, `PublishedResources` and `FrameSlots` read the journal once a frame and refresh their
    cached `GetSRVInfo(...).slot.index`.
- **Identity never changes:** `Resource*`, the global ID and the `VersionedTexture`/`VersionedBuffer` slot stay the same.
  Only the backing generation changes, and in steady state the bindless indices.

### Movability

Each backing has a `relocation::Class`, in a `RelocationRecord` that `Allocation::SetPrivateData` points at. It is set in
`DeviceManager::CreateResourceTracked`.

| Class | When it may move | Covers |
|---|---|---|
| `ReadOnly` | any time | sealed graph resources, sealed versions |
| `Written` | only when the GPU is idle | resources passes write |
| `Pinned` | never | see below |

`Pinned` covers:

- placed transients in CanAlias heaps;
- frame arena chunks;
- mapped heaps (upload, readback, ReBAR);
- shared and exported resources;
- buffers whose device address was handed out;
- resources another API wraps raw, unless their owner registers a hook;
- anything a kept recording names that cannot be recorded again now.

## 2. Lifetime and GPU safety

### The copy

- It runs on the graphics queue, in one relocation batch submitted before the frame's first graph submission. In guest mode
  it goes through the path the frame's first batch already uses.
- Queue order covers the frame's later graphics work. The batch signals the graphics timeline, and in frames with moves the
  first batch on each other queue waits for that point.
- Barriers and copy:
  - the old backing goes from its resting state to CopySource;
  - the new backing goes from Undefined to CopyDest with discard (on D3D12 the full copy initialises placed RT/DS
    textures);
  - `CopyResource`, or a buffer copy;
  - the new backing goes to the resting state.

### Freeing the old backing

- The retire is queued once the relocation batch has its timeline value: `CaptureFencesUnlocked` behind the
  `AssignmentGate`. Any frame or ticket that can still name the old backing was submitted before that point.
- For DXVK, the retired object's deleter calls `releaseOrgStorage`. DXVK frees the storage when its own use ends.

### Async epochs

- Tickets prepared before the commit hold the old snapshots and stay valid.
- Kept copies of live epochs are checked again with `BackingsCurrent`.
- The commit never runs while the owner thread is inside a preparation.
- From `BeginPass` to `EndPass`, `DeletionManager` flushes are held off, so no source allocation is freed while its move is
  pending.

### Budget and triggers

| When | Pass | Limits |
|---|---|---|
| Steady state | Fast or Balanced, at most one pass every 8 frames | at most 8 MB and 32 moves a pass, about 0.1 ms of copy on an RX 6600 |
| Loading screen | Full, repeated until `BeginPass` returns `Ok` | up to 512 MB a pass; the loading loop already ticks frames (`ORGWindows.cpp` `FrameTick`) |

- A campaign starts when a block vector's slack is over 64 MB and over 25 %, read from `CalculateStatistics`.
- It stops after N passes that free no block.

## 3. The native defragmenter

### BasicRHI changes (`rhi_allocator.h/.cpp`)

- A new move operation, `DefragmentationMoveOperation::CopyRetain`. EndPass swaps as Copy does, but hands the temporary
  allocation (old resource, old range) back as an `AllocationPtr` in a new `DefragmentationPassMoveInfo::pRetained` array
  instead of releasing it. Its block stays alive until ORG frees it.
- `Allocation::GetOwnerRecord()`, typed sugar over the private data.

### One pass (owner thread, inside `Service::Commit`)

1. Once per campaign: `Allocator::BeginDefragmentation({Fast or Full, maxBytesPerPass, maxAllocationsPerPass})`.
2. `BeginPass`. Read each move's `pSrcAllocation->GetPrivateData()`. A null or `Pinned` record, or `Written` outside an
   idle point, gets Ignore.
3. For each movable allocation, make the resource at the destination with `CreateAliasingResource(pDstTmpAllocation, 0,
   desc)` and `pDstTmpAllocation->SetResource(...)`.
   - D3D12: `CreatePlacedResource2` with an Undefined layout (`rhi_dx12.cpp:4959`).
   - Vulkan: a new `VkImage` or `VkBuffer` plus `vkBindImageMemory` or `vkBindBufferMemory` at the destination offset
     (`rhi_vulkan.cpp:389-394`).
   - On both APIs a buffer's GPU address changes, so buffers whose address was handed out stay pinned.
   - On D3D12 heap tier 1, buffers, textures and RT/DS textures are in separate vectors; moves stay within a vector.
4. Record the copies into the relocation batch.
5. `EndPass` with `CopyRetain`. The ORG backing's `Allocation*` is unchanged and now names the new resource. Then run
   steps 2-5 of the commit, with `retired = pRetained[i]`.

### Which owners can move

- **Graph textures and buffers** (`PixelBuffer`; `Buffer` with `DynamicBufferBase`): `ReadOnly` once sealed (a published
  version no longer written counts as sealed), `Written` otherwise.
- **`AllocatedResource`:** `Pinned` unless its owner registers a hook.
  - Streaming windows register one. The streamer already swaps windows (`StreamSetWindow`, `NativeDevice.cpp:4669`;
    `GpuTextures::NoteWindowChanged`, `GpuTextures.cpp:1273`).
  - The hook makes the window again in compacted memory and reports an Owner relocation. This works in native and guest
    mode alike.
- **`NativeResource` records** that own a `graphOwned` PixelBuffer follow through `NativeResource::Rebind` and
  `ImageRegistry::Rekey`.
- **Committed exempt textures** are not candidates: they have no slack.

## 4. DXVK mode

- **Stays in DXVK:**
  - choosing chunks and running its own defragmentation (`m_relocations`, `enableDefrag`);
  - the copies, in its stream;
  - pinning written imports;
  - the `OrgRetired` lock accounting and the serials.

  DXVK remains the allocator.
- **DXVK changes:**
  - `DxvkOrgRelocation` and `DxvkOrgInteropRelocation` gain the new storage's description (`DxvkOrgInteropResourceInfo
    next`, filled in `pushOrgRelocation`) and a ready sequence, the CS chunk sequence of the move;
  - `DXVK_ORG_INTEROP_VERSION` is bumped.
- **Moves into ORG/CS core** (`DxvkRelocationSource`):
  - polling, once per commit;
  - the map from cookie to `{weak ExternalTextureResource or ExternalBufferResource, serial}`, which `TrackResources`
    callers register into (today it is DCLF's `byCookie`);
  - the window between a relocation and its registration (today DCLF's `unclaimed` and `importsInFlight`): the source
    checks the serial at registration;
  - building `Relocation{next = ImportDxvkTexture or ImportBufferRef of next}`;
  - the release at retirement.
- After this, `RenderGraphRuntime::PollRelocations` and `ReleaseStorage` are private to the source. DCLF calls
  `TrackResources` and gets back an ORG resource that follows on its own.

## 5. Steps

Each step can be tested on its own. Measure with:

- the VRAM JSON's "Allocator slack" line (`VramLedger.cpp:481`) and "Storages retired to ORG";
- the numbered `BASICRHI_MEMORY_DUMP={n}` resource lists, which show blocks and holes;
- the `vram-loads` cs-test config, in native Vulkan, native D3D12 and guest.

| # | Step | Measure | Risk |
|---|---|---|---|
| 1 | BasicRHI `CopyRetain`, with tests in `extern/BasicRHI/tests` | fragment a pool, defragment, check contents, and check blocks are freed only once the retained allocations go, on D3D12 and Vulkan | D3D12MA's assumptions about the temporary allocation's lifetime |
| 2 | Owner records: `DeviceManager::CreateResourceTracked` and `AllocateMemoryTracked` set the private data; counters only | VRAM JSON relocation counters (moved, ignored by reason, retired bytes) and a ledger line, "Relocations awaiting retirement" | none: no behaviour change |
| 3 | The follow path in ORG, with no producer yet: `AdoptRelocatedBacking`, generations for external backings, `PersistentRecordingBackingsCurrent`, the journal, `RetireRelocated`. A debug switch, `ORG_RELOCATION_STRESS=n`, makes n read-only backings again each frame, like DXVK's `queueOrgStressRelocations` (`dxvk_memory.cpp:2412`) | no image differences in screenshot runs; run with the D3D12 debug layer and Vulkan validation | a missed holder, which would show as a use-after-free or GPU fault |
| 4 | The DXVK source; DCLF moves to ORG's follow path | "Storages retired to ORG" drains within frames in flight + 1; DCLF import counts fall | ordering between DXVK's copy and ORG's first read of the new storage (the ready sequence) |
| 5 | Defragmentation at loading screens, native. Full algorithm, GPU idle, descriptors rewritten in place, so CS index caches need no change. `NativeResource::Rebind` and `ImageRegistry::Rekey` land here | slack after each load in `vram-loads`; target 100-250 MB less | loading-screen stall time: cap it and log ms per pass |
| 6 | A steady-state trickle for `ReadOnly` resources: indices rotate and CS holders read the journal | slack regrowth over a 900-frame `vram-riverwood` run, frame-time p99, `ORG.Persistent.BindingEdits` | binding-edit cost; a missed index cache |
| 7 | The streaming-window owner hook, both modes | slack in the `Game/Textures/*` vectors | |
| 8 | Optional: `Written` resources in steady state, through a graph-declared copy pass | only if step 6 shows slack held by histories | |

**Deleted in step 4:**

- `ImportRelocations.cpp/.h` and the `Hooks.cpp:603` call;
- in `GpuResources`: `PostRelocations`, `TakeRelocations`, `moved`, `byCookie` and `Held::storages`;
- in `GpuTextures`: the relocation branch of `NoteRelocations`/`Supersede` (`unclaimed`, `importsInFlight`).

`RelocatedViews` becomes a journal read.

## 6. Open questions (recommendation in each)

1. Defragment in steady state at all, or only at loading screens? Loading screens first (step 5). Add the trickle
   (step 6) only if slack grows back by more than 100 MB in steady state.
2. Bindless indices: rewrite descriptors in place only when the GPU is idle; rotate and use the journal otherwise.
3. Resources the GPU writes, outside loads: no. Keep DXVK's rule that written resources stay pinned.
4. DCLF's device-address buffers: pin them. Their version rotation already frees memory.
5. Guest streaming windows, which DXVK holds raw: the streamer makes the window again (an owner hook). No new DXVK API.
6. Should a DXVK entry carry the new location (an interop version bump)? Yes. It removes the describe round trip and the
   2 s `unclaimed` window.
7. Recordings that cannot be recorded again now (revision copies mid-revision): Ignore the move and retry at the next
   adoption. Loads reset revisions anyway.
8. Place exempt game textures in blocks so they can be compacted? Not in this plan. They are committed and have no slack;
   that is a separate allocation-count change.
