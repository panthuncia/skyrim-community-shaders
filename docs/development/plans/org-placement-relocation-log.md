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
| org-defrag + cs-defrag | ORG, CS | `relocation/defragmenter`, `relocation/defrag-cs` | relocation 5 |
| cs-dxvk-source | CS | `relocation/dxvk-source` | relocation 4 (CS side) |
| cs-holders | CS | `relocation/holders` | relocation 2-3 (CS side), journal readers for 6 |
| org-versioned + cs-versioned | ORG, CS | `placement/versioned-candidates`, `placement/versioned-cs` | placement 5, 6 |

All lanes are merged into `org_integration` (CS) and `vram/frame-arena` (ORG submodule). The main session fixed runtime
failures itself; those fixes are separate commits.

## Status by plan step

| Plan | Step | State |
|---|---|---|
| Placement | 1 DCLF reads | done, on |
| Placement | 2 usage summaries | done, on (diagnostic only) |
| Placement | 3 host packing | done, `ORG_FRAME_PLACEMENT`, off by default (costs memory today) |
| Placement | 4 versioned placements | done as re-placement on a new version (entry 5) |
| Placement | 5 versioned candidates | done for frame placement; only SSGI's scratch qualifies (entry 20) |
| Placement | 6 MayNotWrite | done, engine-gated passes only (entry 21) |
| Placement | 7 switch on + delete | not done (entry 22) |
| Relocation | 1 CopyRetain | done, tested |
| Relocation | 2 owner records | done, on |
| Relocation | 3 follow path + stress | done, on; `ORG_RELOCATION_STRESS` |
| Relocation | 4 DXVK source | done, on in guest; DCLF keeps re-importing (entry 15) |
| Relocation | 5 loading-screen defrag | done, `CS_ORG_DEFRAG`, off by default (frees nothing yet, entry 19) |
| Relocation | 6 steady-state trickle | not done: no texture slack to win (entry 19) |
| Relocation | 7 streaming-window hook | not done: window blocks are full |
| Relocation | 8 written resources | not done (optional) |

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
    Measured at the second dump after loads 1/3/4 (driver MB, off → on): Vulkan 3728/3732/3796 → 3868/3870/3870; D3D12
    3727/3630/3696 → 3860/3829/3895; guest 4154/4096/4154 → 4233/4180/4238. Correctness with it on: river (all three
    modes) and ssgi (Vulkan) pass and match the baseline by eye.
12. **BasicRHI `CopyRetain` hands the old allocation back through a caller-owned `pRetained` array**; retained allocations are
   never chosen as move sources. Tests pass on D3D12 and Vulkan (Vulkan needs `VK_LOADER_LAYERS_DISABLE=*nsight*`).
13. **A recording whose bound backing moved keeps running while it records again.** The follow path first made
    `EpochRecordingCurrent` false on a move, so FrameAnchors dropped the stage for a frame and Cloud Shadows stopped the
    game under `ORG_RELOCATION_STRESS`. Now ORG has `EpochRecordingBackingsCurrent`; the recording cache records again on a
    move and the schedule switches when the new recording is ready. Safe because the old copy holds the old backing's lease.
    Measured under `ORG_RELOCATION_STRESS=4`: 6,810 moves in a `vram-loads` run, 2 awaiting retirement at any time, no
    failures, shots match.
14. **CS ReadOnly marks are conservative** (cs-holders agent): only Terrain Shadows' heightmap and Volumetric Lighting's noise
    volume. Buffers whose GPU address went out, buffer view refs, DCLF raw CBV buffers and ImGui textures are Pinned.
15. **DCLF stays on its own re-import path for DXVK moves ("Hold"), not ORG's follow path** (cs-dxvk-source agent). DCLF's
    tables and async scene work can record with the old descriptor index after a commit, and `RefreshShared` retires the old
    resource behind the commit's frontier, which would leave them reading destroyed views. Only plain imported textures
    ("Follow") go through ORG's switch. The plan's "DCLF import counts fall" does not happen yet.
16. **DXVK moves carry no ORG `ready` point.** `rhi::Timeline` cannot wrap DXVK's semaphore, and ORG's empty submits would
    mark the compute and copy queues started for the epoch. Ordering comes from DXVK's stream: graphics work is enqueued
    after the poll, and the other queues already wait on a stream point at their first submission of each epoch. Follow
    moves commit only while those queues are idle.
17. **A DXVK move's old storage is held only by the newest import before it.** The first version gave it to every older
    import of the cookie; under `DXVK_ORG_RELOCATE_STRESS=4` held storages grew ~150/s. After the fix they still grow
    (~115/s) under that stress, held by superseded DCLF imports that stay alive. Without stress, `guest+vram-loads` shows
    "Storages retired to ORG" at 0 MB and the driver total equal or lower than before. **Open:** find what keeps superseded
    DCLF imports alive under heavy move rates.
18. **Every guest run before 05:45 used a Nov 6 DXVK DLL.** cs-test's stage copies only the CS DLL and shaders, and the
    Dev-Fast `aio/bin` DLLs are not refreshed by the CS build. I copied the new DXVK DLLs into the aio folder
    (`tools/stage-dxvk-dlls.ps1 -Src extern/dxvk/build/src`) and into the `CS AB nfg` MO2 mod by hand.
19. **The native defragmenter moves textures only, and only at loading screens** (`CS_ORG_DEFRAG=1`, off by default). Buffers
    are off (`moveBuffers=false`) because CS hands out buffer device addresses ORG cannot see. All ORG frees go through a
    release gate while a pass is open. Old ranges are kept until the GPU is past the copy and the old generation's
    published snapshot expires, so no kept recording can read freed memory.
    Measured (native Vulkan `vram-loads`): one campaign at the first loading screen, 99 moves, 59 MB in 4.3 ms, blocks 9 → 9,
    slack unchanged. Native D3D12: 48 moves, 51 MB in 6.5 ms, blocks 7 → 7. A memory dump (Vulkan) shows why: texture blocks are almost full. The free space is in blocks shared with DCLF
    buffers (written every frame, most with handed-out addresses, so pinned), in the frame arena's alias heaps, and in
    mapped ReBAR/system heaps (128 + 73 MB, pinned by the plan). Relocation step 6 (steady-state trickle) is not worth doing
    for textures. The win needs buffer moves, which needs CS to stop caching device addresses (or to follow the journal for
    them), and packing DCLF's per-frame buffers apart from long-lived ones.
20. **Versioned resources are frame-placement candidates only** (placement-versioned agent). With `ORG_FRAME_PLACEMENT` off
    nothing changes for them. Engine targets did not become candidates: they are all exported, and in native mode the native
    device wraps the raw image, so a moved version would leave that wrap behind. The list of frame-scoped targets that could
    be placed once the native device follows moves is in `EngineTargetSet.cpp` above `TwinOptions`. The one new candidate
    is SSGI's full-resolution scratch (`Lifetime::Transient`, no longer fatal). `DeclareTransient*` is not folded in yet:
    with the switch off that would cost VRAM.
21. **MayNotWrite marks only engine-gated passes** (skeleton clear, gated kMAIN/depth copies, ImageSpace draws/dispatches).
    The plan's gated regions no longer exist (`34aee5701`). The per-graph planner first also refused pools for those
    resources, which cost ~30 MB with the switch off (frame arena 136 → 125 MB, 6 resources standalone). I limited the rule
    to frame placement: the readers in the graph share the gate.
22. **Placement step 7 (switch on by default, delete the frame arena, frame positions, private pools, `DeclareTransient*`) is
    not done.** With the switch on, native Vulkan `vram-loads` still uses ~214 MB of alias heaps against 136 MB off. The
    big candidates the plan counted on (G-buffer ~91 MB, Scaleform scratch 33 MB) are blocked: engine targets by the native
    wrap (entry 20), Scaleform by the repeated UI epoch (graphs can't mark a resource as crossing runs).
23. **Known risk, not fixed:** `ExecutionState.cpp` (~298, ~505) enters every aliased backing from Undefined at each execution,
    including later epochs. A frame-placed resource used by several epochs could lose its content at the later epoch's
    entry. Today's frame-placed resources are single-epoch except two; it must be fixed before engine targets are placed.

## Final checks (merged build, switches at their defaults)

Passed: `river` in all three modes; `ssgi` native D3D12 and guest; `dynres` in all three modes; `resize` native Vulkan;
`terrain-shadows` native Vulkan with `ORG_RELOCATION_STRESS=4`; `vram-loads` native Vulkan and D3D12 with `CS_ORG_DEFRAG=1`;
guest `river` with `DXVK_ORG_RELOCATE_STRESS=4`; native Vulkan `ssgi` and `vram-loads` with `ORG_FRAME_PLACEMENT=1`.
Shots compared by eye with the morning's baselines: the differences seen were weather/fog and camera position, which also
vary between two runs of the same build.

Nothing is pushed. Branches of every lane are kept locally.

## Round 2 (2026-11-22 afternoon): engine targets, buffer moves, per-frame pools

Plan: `org-placement-defrag-2.md`. Four code-only lanes (buffer-moves: BasicRHI + ORG; placement-org: ORG; dclf-follow: CS +
OMS; engine-targets: CS), merged into `org_integration` / `vram/frame-arena`. The main session built, tested and fixed the
runtime failures listed below. Switches stay off by default.

### What landed

24. **Native engine targets are ORG textures** (`GraphResources::Options::nativeOwned`, `Native::WrapGraphTexture`). Each
    version is an ORG `PixelBuffer`; its D3D11 handle is a Tracked native record that follows a new image every frame
    (`FollowGraphTextures`, before the journal). The frame-scoped targets (G-buffer, kSHADOW_MASK, kSNOW_SWAP, the copies,
    depth kPOST_ZPREPASS_COPY) are Transient in native mode, so frame placement places them. Guest is unchanged.
25. **Buffers move at idle points.** BasicRHI `RetargetBufferViewRef` keeps a buffer's view ref slot across a move (abi 18).
    ORG's defragmenter moves buffers in idle campaigns, rewrites descriptors in place, retargets the ref pair and journals old
    and new device addresses. CS: refs and growable tables are `FollowAtIdle` (Written) instead of Pinned; DCLF's scene set
    re-reads addresses after a move and frames wait for a set sealed after it. Fixed constant buffers stay pinned in the
    PerFrame group (dclf-follow report). Engine geometry is GPUUpload, pinned by its memory.
26. **PerFrame memory group** (`org::MemoryGroup::PerFrame`, 32 MB blocks, own `rhi::ma` pool per heap type): DCLF's per-frame
    pools (frame values, payload parts, CPU sequences, light contexts, portal programs, fixed constants) live apart from
    long-lived allocations. The shared device-local blocks' slack fell from 26-29 % (round 1) to 11-13 %.
27. **Frame placement correctness**: multi-epoch entry continues from the ledger state after the first epoch (entry 23 fixed);
    `Resource::SetContentAcrossRuns` gives Scaleform's scratch one lifetime over every UI run (now Transient versions);
    versions no longer keep replaced heaps alive.

### Runtime fixes by the main session

28. **DCLF CPU-pipeline sites cached RTVs per resource object** (D3D12 crash, "stale or released resource" at
    OMSetRenderTargets with placement on). The views are now remade when an image changes, and the site pass resolves its
    declared targets in its recording (`SiteImages`), so frames still running older graphs draw into their own images.
29. **Image registry collisions in a defrag pass**: one target moved into an image handle another left in the same pass.
    `ImageRegistry::RekeyAll` takes every old entry out first; the native follow rekeys graph textures and journal moves as
    one batch.
30. **A resource the placement version does not know is no longer pooled per graph.** Epoch 13 pooled
    `ImageSpace::SSRMaskDownsampled` while another epoch gave it memory of its own; a defrag move made epoch 13 record again
    and the binding check refused it. Defrag also waits while a graph awaits a rebuild of its shared pools.
31. **Frame arena blocks**: frame transients get blocks of their own with placement on, and a range does not refill a block
    it would leave under half used. The 59 MB block a 17 MB region kept is now 17 MB.
32. **Placement heaps at their need** (`heapHeadroom = 1.0`): 169 → 146 MB; heaps grow about twice a run.
33. **Defrag start at 10 % slack** (was 25 %, which no longer triggered), and no new campaign after one that freed no block
    until the slack grows by 32 MB (long loading screens started one every 2 s).
34. **Native-owned engine targets are pinned against defrag.** On D3D12 a moved kHDR_DOWNSAMPLE was drawn as a render target
    while still in CopyDest: some user of the native handle takes it as resting in Common. Frame placement still places them.

### Measurements (native Vulkan `vram-loads`, driver MB at the 2nd/3rd load; one sample each unless noted)

| Config | Driver | Alias pools | ORG resources | Slack |
|---|---|---|---|---|
| Off (3 samples) | 3688-3876 / 3746-3940 | 136 | 1975-2010 | 372-549 |
| `ORG_FRAME_PLACEMENT=1` (final) | 3652 / 3775 | 161 / 210 | 1892 | 397-407 |
| `CS_ORG_DEFRAG=1` (final) | 3815 / 3879 | 136 | 2019 | 453 |
| Both (final) | 3745 / 3868 | 161 / 210 | 1930 | 396-407 |

Driver totals vary ±130 MB between identical runs, mostly slack; ORG's own numbers are steadier. Placement: resources
−95 to −120 MB, alias memory +25 MB (+74 MB at the 3rd load, when a 49 MB frame-transient group joins the arena), so a net
win of ~50-90 MB. Defrag: one campaign of 50-110 MB at the first loading screen; it freed one block in one run, none in
others; slack at later loads ends where it was. Native D3D12 with both: 3586 / 3703.

### Open

35. **Intermittent corruption with placement on after a version change**: in `resize` (native Vulkan), the `grid` shot after
    the ViewAtmosphere grid change was dark in one run, green with lens-flare sparkles in another, and correct in a third;
    placement off is correct. It looks like a one-frame glitch at the switch poisoning eye adaptation, which then takes
    seconds to recover. Suspects: the in-place descriptor rewrite of a re-placed resource while frames are in flight, or a
    reader of an engine target outside the graph that follows its image only at the frame's end. Needs registered toggle
    switches (engine targets as candidates; forced new heaps) and repeated runs. **Keep `ORG_FRAME_PLACEMENT` off until fixed.**
36. The CopyDest/Common mismatch behind entry 34.
37. D3D12 debug layer: defrag copies a few render targets nothing wrote yet (`Native::Texture2D`, id 1422); packed-buffer
    cross-queue warnings (id 1047) predate this round and grow with loads.
38. Buffer moves are only exercised by refs and growable tables so far (19 address-holding buffers moved in a campaign, all
    before DCLF's scene set existed). DCLF's scene-set follow path has not seen a move in a test.

### Final checks (this round)

Passed with switches off: `river` in all three modes. With placement on: `river` guest and native D3D12 (debug layer: same
messages as off), `ssgi` and `dynres` native Vulkan, `resize` native Vulkan and D3D12 (but see entry 35), `vram-loads` guest.
With defrag on: `vram-loads` native Vulkan and D3D12 (debug layer). Both on: `vram-loads` native Vulkan and D3D12.
