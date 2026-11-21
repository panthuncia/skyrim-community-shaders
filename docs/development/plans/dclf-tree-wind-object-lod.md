# Tree Wind for DynDOLOD object LOD trees in DCLF

Design note, 2026-11-18. Worktree `org-port-worktrees/port-treewind`, branch `org-native/port-treewind` off `org_integration`
`ca4354a8c`. Nothing was built or run.

`DCLF/` is `src/Features/DrawcallLimitFix/`. Upstream is Tree Wind #2991 (`bae20c6e3`): `src/Features/TreeWind.*`,
`src/Features/TreeWind/TreeShapes.*`, `features/Tree Wind/Shaders/TreeWind/TreeWind.hlsli`.

## What upstream does

- `TreeShapes::Classify` picks DynDOLOD's tree shapes: a `BSSubIndexTriShape` whose Lighting property has vertex colours and
  none of `kLODObjects`, `kHDLODObjects`, `kTreeAnim` or `kLODLandscape`. Crowns are the `treepassthru` shape, or two-sided with
  rim or soft lighting. Trunks are all-quad shapes without those flags. Tree-mod support adds mixed trunk shapes and two-sided
  crowns in the LOD4 ring.
- `BendWeights::Get` builds four halves per vertex from the CPU vertex copy (`rawVertexData`, `rawIndexData`): bend weight, tree
  random, sway scale and distance from the trunk. Trunk cards register in their block (keyed by the block's world translate).
  Crowns take the nearest trunk card of their block, so a crown seen before its block's trunks stays still until they are seen.
  In tree-mod mode a crown is rebuilt when its block's generation changes.
- Hooks on `BSLightingShader` and `BSUtilityShader` `SetupGeometry` bind VS b3 (`TreeWindCB`, on or off) and VS t120 (the bend
  data) per draw. `Lighting.hlsl` adds the offset under `TREE_WIND_VS` (VC and not LOD, TreeAnim, landscape, skinned or eye).

## 0. What DCLF draws of these shapes today

DCLF does not draw them. `Eligibility::Static` (`DCLF/Core/Scene/Model/Eligibility.cpp`) takes a `BSSubIndexTriShape` only through
a geometry type contribution's property flags. The only one is Features/Lod's, with `kLODObjects | kHDLODObjects`. A sub-index
shape without them is not flagged, not a tri-shape and not an entry shape, so it is `Ineligible::NotTriShape`.

The engine's draw of a refused object then reaches D3D11. In guest mode the flush latch skips it ("reached D3D11 with the
engine's state latched and nobody vouched for it: skipped", seen in the current `CommunityShaders.log` for Lighting draws). In
native mode the engine's D3D11 calls do nothing. So DynDOLOD's tree LOD shapes, and every other sub-index LOD shape without the
LOD object flags (DynDOLOD's passthru LOD models), are drawn by nobody in the graph modes.

This is not a Tree Wind defect. It must be fixed first, because the sway needs a member to ride on. It needs a check and a
decision (open decision D1 below).

Check (main session): a DynDOLOD exterior in guest-quick with `CS_ORG_RESIDUE_CENSUS=1` and `CS_DCLF_CENSUS=1`. The census
gives each non-member Lighting geometry its verdict. Look for `treepassthru` and other sub-index shapes under `lodLandRoot` with
`NotTriShape`, and for the 3D tree LOD missing on screen.

## 1. Membership

**Classification.** `TreeShapes::Classify` now has a plain form, `Classify(const ShapeFacts&, bool)` (committed, `ae20dcd49`).
`ShapeFacts` holds the property flags, the geometry type (sub-index, tri-shape), the `treepassthru` name test, the vertex and
triangle counts and the world scale. All of these are in DCLF's records: `PropertyRecord` flags, `GeometryRecord` type and
counts, `NodeRecord` name and world transform. The upstream pass form is a wrapper over it, with the same results.

**Admission (D1).** Features/Lod widens its `kSubIndexTriShape` contribution so a sub-index shape under `TES::lodLandRoot`
without the LOD object flags is object LOD too. Proposed shape:

- `Api::GeometryTypeContribution` gains an optional `admits(const LeafFacts&)` beside `propertyFlags`, where `LeafFacts` is the
  plain flags, type, counts, name and category root of the leaf. Eligibility asks it when the flags do not match. Lod answers
  "under `lodLandRoot`".
- Such a shape keeps object LOD's rules: segment ranges (`CaptureDrawnRanges`), the LOD gates, `kObjectNoShadow` (it never
  casts: the caster rule's `ShadowReject::Lod` keys on `IsLodObject`, which is flag-based, so it must also accept the admitted
  shapes), and the LOD fades.
- It draws with the technique the property derives (default Lighting with VC, two-sided, rim or soft lighting), which DCLF
  already draws for tri-shapes.
- The water reflection's membership is by technique (`LodLightingTechnique`: 9, 13, 15, 18). These shapes are not in it. While
  DCLF owns the LOD roots in the reflection (`LodRootsOwned`), they would be missing from the reflection. The reflection rule
  must take "admitted object LOD" too, or the engine must keep them there.

Tree Wind then only marks members. Its coordinator component's residency callback runs `Classify` on the slot's records. A
member classified `Crown` or `Trunk` gets a bend request (section 2).

## 2. Bend data

**Where it is built.** On the preparation pool, never on the scene lane (the lane reads no engine memory) or the render thread.

- The scene lane's component (`residency.resident`) queues a bend request per new tree member: the object slot, its block key
  (`TreeShapes::BlockKey` of the world translate), its kind, and the pinned geometry.
- A pool job (a leased frame-start job) takes the pending requests of a frame. Under an engine-read-window `Lease` per block it
  copies each shape's positions (offset 0, `stride` from the vertex desc) and 16-bit indices. It then runs
  `TreeShapes::BuildBendData` (committed, `ae20dcd49`) per block: trunks first, then crowns, against one `TreeShapes::Block` per
  block key that the job owns. A refused lease leaves the requests for the next window: nothing is dropped.
- The result is an immutable message (`LatestSlot` or `EventQueue`) of `{slot, generation, shared vector of halves}` that wakes a
  scene pass. The lane applies it: allocates the slot's region and writes the table family row (section 3).

**Readiness, without waits.** The member joins the set as soon as object LOD would admit it, and draws still. Its bend row word
stays 0 until the data is built and published, as upstream's crown stays still until its trunks are drawn. A still tree LOD
shape is a correct draw, so nothing gates the claim and nothing waits.

**Crowns that wait on trunks.** A block's shapes attach in one subtree, so the job normally has its trunks with its crowns. A
crown built as `Built::Waiting` stays in the component's per-block waiting list. A trunk result that bumps the block's
generation requeues that block's waiting crowns, and in tree-mod mode its built crowns too (their rebuild, upstream's stale
check). That requeue is the crowns' re-join event. A block's state is freed when its last member leaves (upstream's `Evict`
without the frame clock).

**Settings.** `TreeModSupport` changes the classification, so a change is a live re-classification of the feature's members:
clear the blocks and the rows, requeue every member. This is the only full rebuild, and it runs on the lane like the others.

## 3. GPU storage

**The buffer.** A feature-owned `Draws::Versioned` buffer, `cs.dclf.tree-lod-bend`, registered through
`RegisterSceneResources` as Faces' positions are (`DCLF/Features/Faces/FacePositions.cpp`: `Setup`, `RegisterGraph`, `Require`,
`Fit`). Growth is the scene buffers' growth, published with a sealed set at a frame boundary.

**Layout.** Per region: one header row (16 bytes: the row tag, the vertex count, two zero words), then 8 bytes per vertex (the
four halves as two words). Regions are per object slot (an object LOD shape is one object).

**The row in the record.** A table family `tree LOD bends` (`Api::TableFamily`) with `recordWord = 4`, `recordBias = 1`,
`recordTag = true`. The feature block (record words 4-11) is free for a non-TreeAnim object. DCLF's TreeWind family uses words
10-11 and only for `kObjectTreeAnim`, which these shapes never have. The family's row number is the region's first unit
(64 vertices a unit), so the shader gets the offset from the word with no lookup. `recycle` returns the region to the feature's
allocator, and freed rows return only once no publication names them (the family's retirement). The word is 0 when the shape
has no bend data: that is the per-object enable, in place of `Params.z`. No `kRecord*` bit is used.

**Uploads.** A commit hook (`RegisterCommitHook`) uploads every region the adopted version does not hold at its row's tag, as
`FacePositions::Upload` does: a new version (growth) re-uploads all regions, and a region past the version's capacity waits for
the growth. Because the header's tag is written in the same upload as the data, the shader can tell a region that is not there
yet: the record's tag (word 5) differs from the header's, and the vertex stays still. So a record never draws garbage, and no
frame waits for an upload.

**Why not the alternatives.**

- No new texture register: the vertex stage's run is t121-t127, all taken. t120 is the fade states. t118 is a pixel-stage frame
  texture.
- Not the extras rows (t126): fixed 7-row blocks copied whole into every snapshot.
- Not the palettes (t122): FrameValues rewrites them every frame.
- The second vertex stream would fit (per-object per-vertex float4 rows, versioned, uploaded by generation, waited for by the
  entry class) but Core has one stream for one entry class (Faces). Generalizing it touches `MainBuild`, the shadow epochs,
  `Uploads`, `Snapshot` and the named parity. Recorded as the alternative in D2.

## 4. Shader

**Wind block.** VS b13 (`DCLFFrameFog`, the frame block every DCLF Lighting vertex stage reads) gains rows c8-c12 for
`TreeWindCB` and c13.xy for the bend buffer's ref. They are declared in `Lighting.hlsl`, not in a `Common/` header.
CPU: a VS form of `Api::FrameConstantRow` (a stage field and a sample that gets the adopted set, for the ref), latched with the
other VS b13 rows in `LatchFrameRows` (`DCLF/Core/Render/Resources/FrameRecords.cpp`). The rows come from
`::TreeWind::FrameWind()`, as Features/TreeLod's draw row copies it. Without the feature, or with the map menu open, the block
is zero and `Params.z` reads 0.

**Vertex index.** `DCLFShade` gets the geometry's vertex index as a static set by each main before it calls `DCLFShade`:
`SV_VertexID` under `DCLF_PULLED_VERTICES` (the index pool's indices with vertex offset 0) and the fetched `index` in the pulled
Z-prepass (`DCLFPulledVertex`). Both builds then compute the same offset, which the colour pass's EQUAL depth test needs.

**The block in `Lighting.hlsl`.** A second tree-wind block for `DCLF_BINDLESS` with VC and not LOD, TreeAnim, landscape,
skinned or eye:

- `#define TREE_WIND_ROWS` (statics, no b3 and no t120) and a `TREE_WIND_VERTEX_DATA(v)` that loads the two words at
  `ref + header + (v * 8)` and converts them with `f16tof32`, then includes `TreeWind/TreeWind.hlsli`. The header needs no change:
  `GetOffset` is already compiled whenever `TREE_WIND_VERTEX_DATA` is defined.
- The statics are filled from VS b13 c8-c12 at the top of `DCLFShade`.
- The offset applies when the record word is non-zero, the header tag equals the record tag, and `Params.z > 0.5`, written as
  one `bool` with the work inside it (no early exit, no `||` of equality tests).
- Every value is `precise`, as in the DistantTree port, since two modules must agree to the bit.

**Utility.** Not needed: object LOD never casts in DCLF.

## Decisions (2026-11-18)

- **D1:** admit every LOD shape under the LOD roots, LOD flags or not, kept non-casting, with a matching rule for the water
  reflection. Deferred: first confirm on a DynDOLOD install that these shapes are undrawn today (`dclf-open-defects.md`, "LOD
  gates").
- **D2:** the feature buffer owned by Tree Wind, read by device reference.
- **D3:** bend data built by background jobs on the preparation pool.
- **Improvement target:** this whole design copies vertices and computes bend data on the CPU. A better design would avoid the
  copies and the CPU work in the first place; revisit it before building the copy path.

## Open decisions (as first written)

- **D1. Admitting sub-index LOD shapes without the LOD object flags.** Needed for these shapes to be drawn at all in the graph
  modes, with or without Tree Wind. It covers every DynDOLOD passthru LOD shape, not only trees. Questions: is "under
  `lodLandRoot`" the rule, or only Tree Wind's tree classification (narrower, but then non-tree passthru LOD stays undrawn)? And
  the reflection: take them in by rule, or keep the engine drawing the reflection's LOD.
- **D2. Storage.** The design above (a feature buffer read by device ref, the row in the record's feature block) needs only a VS
  form of the frame constant rows in Core. The alternative is generalizing Core's vertex stream to one stream per entry class,
  and making the tree shapes an entry class. That also solves D1's admission (an entry shape passes the tri-shape check) but is a
  larger Core change.
- **D3. Bend data at capture instead of on the pool.** Copying the positions at attach (the capturing thread, where Lod's
  `ranges` capture already reads the shape) would remove the leased pool job, at the cost of a Core capture hook and the copy on
  the loader thread.

## Status

- Done: the plain classification and build (`ae20dcd49`, `src/Features/TreeWind/TreeShapes.*`). Upstream's engine path is
  unchanged in behaviour.
- Not started, waiting on D1 and D2: the admission, the component and pool job, the family and buffer, the VS b13 rows, the
  `Lighting.hlsl` block.
