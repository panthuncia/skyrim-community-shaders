# VRAM at Riverwood: what can alias, what can shrink (2026-11-21)

Source: `tools/cs-test/configs/vram-riverwood.toml` (Save91, `coc Riverwood`, 900 frames, two `vram-json` dumps; the second is
read). RX 6600, 1920x1080. Runs in `C:\Modding\CS-AB\runs\`:
- native Vulkan: `1121-193756-nfg-native-vulkan-vram-riverwood-vr1` (`CommunityShaders-VRAM-1795307946752.json`);
- native D3D12: `1121-194735-nfg-native-d3d12-vram-riverwood-vr1` (`...1795308819640.json`);
- guest: `1121-200105-nfg-guest-vram-riverwood-vr1` (`...1795309547340.json`). The run is marked failed: the exit hung while the
  shot was being saved. Both dumps came after Riverwood had settled, so they are usable.

Row flags (`Vram::RowFlags`, `src/Vram/VramLedger.h`): 1 aliased (placed in an alias pool, whose own row counts the bytes),
2 not counted, 4 imported, 8 unattributed, 16 internal. So 11 = aliased and not counted (ORG transient), 8 = an ORG
resource with no owner (counted), 18 = BasicRHI ledger site rows (not counted), 16 = the alias arena blocks.

## Reconciliation (MB)

| Line | Vulkan | D3D12 | Guest |
|---|---|---|---|
| Driver device-local usage | 4,654 | 4,762 | 4,558 |
| DXVK allocated | - | - | 96 |
| ORG allocated (BasicRHI ledger) | 4,043 | 4,183 | 3,636 |
| ORG resources | 1,502 | 1,575 | 1,386 |
| ORG alias pools | 116 | 123 | 61 |
| Allocator slack | 409 | 385 | 371 |
| Allocations not tracked | 2,016 | 2,100 | 1,819 |
| Not accounted | 610 | 580 | 826 |

How the Vulkan ledger splits (ORG/Device memory rows): committed textures 1,986 (947 allocations), allocator heaps
1,681 + 312 ReBAR (37 blocks), device buffers 34, descriptor heaps 31. ORG's allocator: 1,016 MB unlabelled plus 602 MB
labelled `Game/Textures/*` (texture streaming's windows).

## 1. Top consumers (device-local, MB, Vulkan / D3D12 / guest)

| Family | Vk | D3D12 | Guest | Notes |
|---|---|---|---|---|
| Committed textures (not in rows) | ~1,986 | ~2,072 | ? | in "not tracked": see section 4 |
| Game textures, streaming windows | 602 | 648 | 636 | labelled allocator rows |
| DCLF buffers | 278 | 282 | 249 | sequences 60, index pools 37, frame-values x3 ~95 |
| SSGI history, declared | 214 | 223 | 160 | four versions of every partner (three in guest): see A1 |
| ORG alias pools | 116 | 123 | 61 | four arena blocks: 55.4, 49.5, 9.9, 1.2 |
| Engine geometry (TriShape VB/IB, rings) | 100 | 103 | 100 | |
| ORG frame arena | 87 | 83 | 70 | 93 pinned chunks (55), 23 oversize (31); 28 MB on 2026-11-17 |
| cs.native.texture (graph-owned) | 53 | 54 | - | two RGBA16F 1080p (381, 12673), an R11G11B10 1080p (380), the 1024 Effects11 chain |
| ImageSpace port (history) | 44 | 46 | 44 | SAO, SAORawAO0/1, TAAHistory0/1: keep |
| Scaleform scratch | 32 | 34 | 32 | not aliased (see A3) |
| Dynamic Cubemaps | 25 | 27 | 25 | |
| SSS | 16 | 17 | 16 | not aliased (see A3) |
| VolumetricLighting::Volume | 11 | 11 | 11 | not aliased (see A3) |

Mode differences:
- D3D12 is about 110 MB over Vulkan. Each texture is rounded to 64 KB (a 1080p RGBA16F target is 16.1 MB, not 15.8), and
  there are 85 MB more committed textures, 47 MB more streaming windows and 7 MB more alias pools. D3D12's 75 MB of
  root-constant pages and its four 16 MB upload pages are in system memory, so they are not VRAM.
- Guest's ORG alias pools are 55 MB smaller, because the post chain's transients (Composite, ColorGrading, CODBloom, lens flare:
  96 MB of aliased rows in native) are not ORG's in guest. Guest has one SSGI history version fewer (one reset fewer). Its
  "not accounted" line is 216 MB larger: DXVK's pipelines and the D3D11 layer.

## 2. Aliasing candidates

### A1. Old versions of declared histories kept alive (not aliasing, but the largest item). Estimate 290-650 MB in native, about 200-430 in guest. Risk low to medium.

Evidence:
- Every SSGI history twin has four live rows at the same 1920x1080 size, with uids from four eras: 620-629, 8478-8486,
  9714-9723, 15460-15468. They come from revisions 1, 6, 17 and the one at 19:38:43. Each of those was raised by a loading
  screen's `Setting.ScreenSpaceGI.historyResets` / `Setting.Skylighting.resets` (the log's "frame revision N: adopted" lines).
- `ORGScreenSpaceGI.cpp:720` makes a reset a new version at the same size ("so the history never carries over a change").
  `ORGSkylighting.cpp:559` does the same for its five 256x256x128 volumes.
- `VersionedResource.h:70`: "whoever holds a version keeps it alive: a revision, a recording, a frame in flight". The coordinator
  lets the old revision go once the GPU passes it (`Revision.cpp:1147`, `ReleaseRetired`). Something else still holds the versions.
- The likely holder is the segment recording cache (`FrameAnchors.cpp:263-280`, `RequestDeclared` at 2093). Its key includes
  `versionsSerial`, and each entry's `SegmentHostData::versions` holds the revision's `IResourceVersions`. The cache drops entries
  of older builds and the least recently used past `kKeep = 6 * kGameFramesInFlight`. A revision that changes only versions
  makes no new build, so its predecessors' entries stay until LRU pushes them out. In a steady scene that never happens. The
  `replaced` check at lookup covers serial-0 entries only, and only when they are looked up.

What is held per stale version: SSGI partners and PrevGeo/AccumFrames 53.4 MB (seen in the rows). The SSGI exported outputs
(`SSGI::AO/IlY/IlCoCg/GiSpecular`, 43.5 MB) and the Skylighting volumes (RGBA16F 64 + R32 32 + three R8 at 8 each = 120 MB)
follow the same path. Being committed exports, they would sit in "not tracked", so the rows cannot show them. Native Vulkan
has three stale versions: 160 MB seen, up to about 650 MB if the committed ones are held too.

Fix (proposal):
- (a) At each revision adoption, once its retirement point completes, erase the segment cache entries that bind versions
  older than the adopted revision (`Cache::EraseIf`). Do the same in DCLF's `Producer` recording cache and anywhere else that
  keys recordings by versions.
- (b) Or have a reset clear the current version in place: a birth clear on the next run, with no new version. That costs one
  clear pass and no memory.
- Check it with `CS_ORG_GRAPH_RESOURCES_FRAME` or a `BASICRHI_MEMORY_DUMP` before and after. The rows should show one
  `SSGI::IlYPartner`, and the committed-texture count should fall by 3 x 15 textures.

Also seen: DCLF's versioned buffers coexist at their old sizes. `cs.dclf.sequences` has 5.05, 21.25 and 34.0 MB
(uids 8761, 15050, 15439); `sort-staging` has 2.1 and 4.25; `face-positions` 1 and 4; the visibility- and fade-events
readbacks have 1, 2 and 4 each. That is about 30 MB device-local if these are stale growth versions held the same way.
They could also be separate roles; check each name's live readers.

### A2. SSGI partners at full resolution are scratch. 53 MB per version. Risk medium.

`ORGScreenSpaceGI.cpp:383-386`: at full resolution each output is the history, and its partner is "the scratch the chain
reprojects into". A partner written and read within deferred+water could be a transient in dw's pool. Blockers:
- the partner is a declared texture;
- `Declare.cpp:318` refuses Transient ("a transient aliases memory and cannot take a version");
- below full resolution the roles swap, and the partner becomes the history.

It needs ORG versioned transients (A3), or two declarations by resolution mode. dw is the arena's peak (the 2026-11-17
survey), so the saving is the amount by which dw's peak grows less than 53 MB. Measure it first.

### A3. Declared textures that lost aliasing in the frame-revision work. 59 MB. Risk medium.

The 2026-11-17 survey had these in the frame arena. Now they are declared (versioned) Persistent textures, which
`DeclareTexture` cannot make Transient:
- `Scaleform::Scratch0..3` (31.6 MB, `Engine/Scaleform/Passes.cpp:1306`). Used only by GFx filter passes of the UI and menu-UI
  stages, and "a draw into one clears it, and nothing reads one before that". The UI stage runs after post, so the memory of
  dw or post is free then: nearly the full 31.6 MB would be saved.
- `SSS::DiffuseNoAlbedo`, `SSS::BlurHorizontalTemp` (15.8 MB, `ORGSubsurfaceScattering.cpp:414-417`). Written and read in dw.
  They join dw's peak, so part of the 15.8 MB.
- `VolumetricLighting::Volume` (11.3 MB, `ORGVolumetricLighting.cpp:421`): "Written by the generate and read by the apply in
  the same run". Post's range, inside dw's bytes: the full 11.3 MB.

Fix: an ORG "versioned transient" lifetime, where a version is an alias identity placed at each build, with a new extent
giving a new identity. Until then, Scaleform's scratch could go back to a per-build transient sized by the display size
read at build time. The display size already rebuilds the UI stage.

### A4. Items still open from the 2026-11-17 survey

- Engine copy textures used only in post (`kREFRACTION_NORMALS Copy`, `kUNDERWATER_MASK Copy`,
  `kIMAGESPACE_VOLUMETRIC_LIGHTING_COPY`): about 12 MB. They are external images, so ORG needs to give them an alias identity.
  Risk medium.
- Terrain Blending and Screen Space Shadows (31 MB): pre-depth to the opaque pass. These are exported or engine-target twins,
  so they need the recipe's span as their lifetime. Risk medium to high.
- `kBOOK_TEXT_0..3` and its depth (45 MB), `kWATER_1/2` (31.6 MB), the rare-path targets: memory bound on demand
  (sparse/reserved). Risk high, and they cannot be tested by a background run.

### Checked, nothing to gain

The image-space histories, the SSGI and Skylighting histories (one version each), `kMAIN`, the shadow maps,
`TerrainShadows`, and the ORG transients already in pools (flag 11: SSGI working set, image-space scratch, post chain,
lens flare).

## 3. Shrink candidates

| # | Item | Saving | Risk | Notes |
|---|---|---|---|---|
| S1 | Allocator slack (409 MB) | 100-250 | medium | see below |
| S2 | ORG frame arena (87 MB, was 28) | 30-55 | low-medium | the 23 oversize chunks, three of them 8 MB, are kept after their frame; the 93 pinned chunks of 1.0-1.34 MB are reused only when wholly empty. Free oversize chunks idle for N frames; return empty pinned chunks past a floor |
| S3 | DCLF frame-values, three copies (`SceneCopies()` = 3) | ~30 | medium | palettes 3x8.6, material rows 3x6.7, animated rows 3x6.6, placements 3x4, shading 3x3.2. Either sized to their high-water mark with the shrink policy, or two copies where the frame fence allows |
| S4 | `cs.dclf.sequences` growth | ~10 | low | `GrowthPolicy::Headroom(0)` already; the 34 MB is Riverwood's draw bound. The old 5/21 MB versions are A1 |
| S5 | `PostProcessing::Composite::Output` is RGBA32F when `preExposed` (31.6 MB) | up to 15.8 of post's pool | medium | `ORGComposite.cpp:102`. RGBA16F would do if pre-exposed values stay under 65504. Aliased, so it saves only if it sets post's peak (epoch 18's pool is 55 MB) |
| S6 | `CODBloom::Bloom` is a full-resolution 9-mip R11G11B10 chain (10.6 MB) | ~8 | medium | mip 0 is kMAIN_COPY's size. Starting the chain at half resolution changes the threshold pass's look; aliased, so the saving is in the pool only |
| S7 | `Screenshot::CleanScene` (7.9, device-local) | 7.9 | low | kept for the session; make it at first use and free it after |
| S8 | Dynamic Cubemaps: the Position cubes are RGBA16F (2 x 3 MB) | ~3 | low-medium | small |
| S9 | `LightLimitFix::LightIndexList` 8 MB fixed | ~6 | low | sized to its maximum |

S1, allocator slack: 409 MB (Vulkan), 385 (D3D12), 371 (guest). BasicRHI's allocator is D3D12MA with 64 MB blocks
(`rhi_allocator_config.h:71`), one block vector per memory type and resource class. Over the run the heaps grew from
392 MB (after load) to 1,681 MB (after the coc): the log's "live device memory" lines at 19:38:35 and 19:38:46. In the
300 presents around the coc there were 2,564 buffer allocations (1,187 MB). The slack is partly-filled blocks left by
that churn: buffers freed after the cell change, at least one empty block kept per vector, and grow-then-shrink DCLF
buffers. What would cut it:
- fix A1 first, since the stale versions pin blocks;
- defragment at a loading screen;
- let a vector release its last empty block after N frames;
- for D3D12, group buffers and textures in one heap class where the tier allows.

Measure it as the "Allocator slack" line before and after.

## 4. The 2,016 MB not tracked and the 610 MB not accounted

Not tracked = the ledger, minus tracked rows, minus slack. In native it is mostly BasicRHI's committed textures
(1,986 MB, 947 dedicated allocations), which no graph row claims:
- the game's immutable textures that are not streaming windows (`NativeDevice.cpp:1066`: an exempt request, an SRV-only
  texture with initial data, is `CreateCommittedResource`);
- the engine's render targets and shadow maps (330 MB of shadow maps by the 2026-11-17 survey);
- exported twins: the SSGI outputs, the Skylighting volumes, Terrain Shadows, Upscaling, and their stale versions (A1);
- textures CS features make through the native device.

To attribute them:
- let the VRAM ledger list BasicRHI's committed resources by `debugName` and size (BasicRHI keeps the name), at least under
  `CS_VRAM_DEBUG`. That turns most of the 2 GB into named rows;
- tag exempt game textures with their path label, as `Vram::Debug` does for DXVK.

To reduce them:
- A1;
- place exempt game textures in allocator blocks rather than one allocation each. Fewer allocations and less driver
  overhead; the saving is small in MB.

Not accounted (610 Vulkan, 580 D3D12, 826 guest) is the driver's own memory: pipelines and shader code, command pools
(about 2 MB each on this driver), descriptor and query pools, the swap chain. It can be attributed with
`VK_EXT_device_memory_report` where the driver has it, and command pools can be counted with
`BASICRHI_COMMAND_POOL_SITES=1`. D3D12 has no equivalent. The 216 MB that guest has over native is DXVK's pipelines and the
D3D11 path.

## Ranking (MB saved against risk)

| Rank | Item | MB (native Vulkan) | Risk |
|---|---|---|---|
| 1 | A1: stale declared versions held by recording caches | 160 seen, up to ~650 | low-medium |
| 2 | S1: allocator slack (defragment, release empty blocks) | 100-250 | medium |
| 3 | A3: versioned transients (Scaleform, SSS, VL) | ~45-59 | medium |
| 4 | S2: frame arena trimming | 30-55 | low-medium |
| 5 | A2: SSGI partners as full-resolution scratch | up to 53 | medium |
| 6 | A1b: DCLF growth versions kept (sequences etc.) | ~30 | low |
| 7 | S3: DCLF frame-values copies and sizing | ~30 | medium |
| 8 | S5/S6: Composite RGBA32F, full-resolution bloom chain (pool peak only) | up to 24 | medium |
| 9 | A4: post-only engine copies | ~12 | medium |
| 10 | S7: screenshot clean-scene on demand | 8 | low |

## Measurement

`native-vulkan+vram-riverwood --set game.save=Save91_00000000_0_507269736F6E6572_Tamriel_000027_20261113033158_1_1`, before and
after. Read the second JSON's reconciliation and count each `SSGI::*Partner` row. Repeat in guest and D3D12 for A1.

## A1 fix (2026-11-21, code only, not yet run)

Root cause, confirmed by reading the code:
- `FrameAnchors.cpp` `RequestDeclared`: a revision's recordings are cache entries keyed on its `versionsSerial`. After the
  adoption no lookup asks for that serial again (`Schedule` asks with serial 0), yet the entry stays. The cache drops an
  entry only for an older build (`Cache::Add`), past `kKeep` (24), or a serial-0 entry when its own key is looked up.
  `InstallRevisionSchedules` erases old entries only for a staged build. A versions-only revision, which is what a
  loading screen's `historyResets` / `Skylighting.resets` raises, changes no build. So each reset left its recordings, and
  through them their versions, in every member stage's cache for good.
- DCLF's `SceneRevisions::recordingCaches` (`Revision/Assemble.cpp`) keeps two entries per epoch. A key holds its versions
  fragment, so the older entry holds the set's buffers at their old sizes. An epoch that stops recording keeps its last two
  for good. That fits the three live sizes of `cs.dclf.sequences`.

Holders checked and left as they are:
- the coordinator's `adopted` and `retiring` revisions: released once the GPU passes the frames submitted with them
  (`ReleaseRetired`). This is the in-flight guard;
- the frame slots' schedules: replaced at the adoption;
- GraphHost's `liveRecordings` (weak) and `liveKept` (one per copy, replaced on its next run);
- exported twins: `AdoptPrepared` drops the old D3D11 side and sends the old graph texture to `ReleaseAfterD3D11`;
- fragment sources: one posted and one held fragment each.

Fix:
- `FrameAnchors::DropStaleVersionRecordings`, called in `Adopt` after `InstallRevisionSchedules`, erases from every stage's
  cache the entries named for a revision and the serial-0 entries whose recording binds a replaced version. Schedules and
  the retiring revision keep their own references, so frames in flight still hold what they run. It logs
  `[ORG] frame revision N: dropped K kept stage recordings of replaced versions`.
- DCLF: when the versions fragment changes, every epoch's cache erases the entries keyed on another versions fragment.
- The proof: `Adopt` watches each version it replaces (twins and declared buffers) by a weak pointer. Each Begin logs
  `[ORG] versions released: N (X MB) at frame F; M replaced versions still held (Y MB); S released this session (Z MB)`,
  and warns `[ORG] versions: <name> (X MB) is still held 600 frames after an adoption replaced it` for any holder missed.

A reset stays a new version. Clearing in place would need a clear pass that runs before the first reader on the reset
frame, in every stage that reads the history, and a gate on the reset. A new version is born zero on the lane at no frame
cost, and the configuration changes that share the key need a new version anyway. The bug was the release, not the reset.

## A1, second pass: the holders the first fix missed (2026-11-21, code only, not yet run)

Run `1121-223609-nfg-native-vulkan-vram-riverwood-m2` after the first fix. The SSGI rows come from four eras: revision 1
(uids 617-624), revision 7 (8497-8506), revision 26 (9764-9773) and the reset at 22:37:00 (15476-15483, current). So every
replaced version was held. Two facts point at the holders:
- revision 1 released the SSGI partners' first versions, but not the first versions of the published twins (`SSGI::AO`,
  `IlY`, `IlCoCg`, `GiSpecular`, `Skylighting::ProbeArray`, every `PhysicalSky::*` LUT, `ImageSpace::AdaptedLuminance0/1`);
- the versions held later match the graphs still alive that were built while those versions were current. The start stage
  (epoch 10) and pre-depth (epoch 11) were last built before revision 7, DCLF's epochs 0-9 between revisions 7 and 26, and
  post (epoch 18) between revision 26 and the last reset.

Holders found:
- **Graph registration** (`OpenRenderGraph/src/Render/RenderGraph/RenderGraph.cpp`, `RegisterResolver`). It resolved every
  resolver and kept the resources in `resourcesByID` and the registry for the graph's life. For a slot that is its current
  version. FrameAnchors registers every client's resources with every stage graph (`FrameAnchors.cpp`, the extension's
  `PrepareForBuild`), and `PublishedResources` registers every published slot with every epoch's graph, DCLF's too. So each
  graph pinned the version current at its build. DCLF's own `Register` (`PassContext.h`) already skipped versioned buffers
  for this reason. Fixed in ORG: a versioned resolver's versions are not registered (late-bound and per-frame ones are).
- **Published entries** (`src/RenderGraph/PublishedResources.cpp`). `Entry::resource` and the snapshot's `resource` kept the
  version current at publish, for the session. Fixed: a slot's entry holds no version, and every reader follows the slot.
  The build snapshot's index lookup now follows a buffer slot too (it read the first version).
- **The graph's last preparation** (`RenderGraphPersistentTickets.cpp`, `PreparePersistentTicket`). The graph kept the
  preparation's host data and capture context until its next preparation. A revision's host data names every version the
  revision made. Fixed in ORG: dropped when the preparation returns; the caller keeps the host data with the recording.
- **Live epochs' kept recordings** (`GraphHost.cpp`, `PrepareTicket`). A copy's kept recording went only when that copy was
  taken again. Fixed in ORG: each preparation drops every copy's recording of another build or backing version.

Holders checked and left as they are:
- the frame slots' schedules, the segment caches, DCLF's caches, the retiring revisions (first pass);
- ImageRegistry (weak), the anchors' imports (weak backings), the native release queue (`ReleaseAfterD3D11`, released once
  the graph submissions before it retire);
- ORG's `VersionedResource` keeps only its current version and a weak reshaped one. OMS `NamedVersions` names only the
  versions its revision made. `RecordingPools` hold CPU memory only. `PipelinePins` hold pipelines;
- GraphHost's revision tickets: each holds its candidates until it is submitted; `liveRecordings` are weak.

Holder left, with its bound:
- **A declaring graph's declaration.** A graph whose passes declare a slot keeps the version its declaration captured: the
  pass's resolver snapshot (`ResolverSnapshot::state`, which owns the version's resource list), the pass's
  `retainedAnonymousKeepAlive`, and the graph's transient pin (`RequestResourceHandle`). Its stored declarations need the
  resource to lower again. A redeclaration or the next build moves it to the version current then. So it is at most one
  extra version per declaring graph, until that graph is built again. DCLF's epochs are built rarely, which may explain the
  oldest `cs.dclf.sequences` size. A root fix is an ORG change: a versioned declaration names the slot, and its stored
  requirements and keep-alives hold no version. See the open questions.

Not a holder but now visible: with versions actually freed, a recording that reads a version through a bindless index it
did not declare (`PublishedResources::ShaderResourceIndex`) would read a freed descriptor after a reset. The river and
shader-reload runs check this.

## Open questions

- Decided by the agent, to confirm: erasing every named-serial entry at adoption. At that point no revision is in
  preparation (`Adopt` takes `preparing`), so no lookup can ask for a named serial again.
- Decided by the agent, to confirm: the "still held" warning waits 600 frames. `ReleaseAfterD3D11` and the retiring
  revision release within a few frames; the margin covers loading screens.
- To confirm by a run: whether DCLF's other growth versions (`sort-staging`, `face-positions`, the readbacks) are held by
  the same cache. They are not adopted through the coordinator, so the watch does not see them; read their rows in the
  second dump.
- To confirm by a run: the size the watch logs is mip 0 plus a third for mips, and 0 for formats with no texel encoding
  (depth). It is for comparison, not for the ledger.
- Decided by the agent, to confirm: a declaring graph keeps the version its declaration captured until it is built or
  redeclared again (bounded at one per declaring graph). Fixing it needs ORG declarations of a versioned resource that hold
  no version, which touches lowering, stored requirements and keep-alives; too wide to change without a build. If the runs
  show a held version from it, the next step is that ORG change.
- Decided by the agent, to confirm: `RegisterResolver` still registers a per-frame resolver's versions (the swapchain's), so
  that path is unchanged. Its versions are host images.
- To confirm by a run: the "still held" warning now prints the reference count. One reference left after these fixes
  points at a declaring graph (above).
