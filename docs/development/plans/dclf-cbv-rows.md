# DCLF scene tables as constant buffers: plan

Why: on the GTX 1070 Ti (Pascal, D3D12) DCLF's main opaque pass costs about 22.5 ms against about 10.6 ms for the same draws
in plain D3D11, mostly per pixel. The pulled Lighting pixel stage makes about 190 storage-buffer loads (77 through device refs)
against 29 in the D3D11 build. In a standalone benchmark on the card (external_refs/pascal-fetch-bench), 80 per-pixel constants
cost 1.77x from a heap ByteAddressBuffer against a root CBV. Heap `ConstantBuffer<T>` recovered about 80% of that, and a
root SRV only 37%. Vertex pulling was not a cost (faster than the input assembler), so vertex and index buffers stay pulled.

Status: investigation, 2026-11-22. Nothing implemented.

## What the stages read today

| Data | Size | Read through | CBV fit |
| --- | --- | --- | --- |
| Frame record (`DrawBindings`) and the frame blocks that are not pass-wide | 1 KB record at the start of a 64 KB frame slot; game cbuffer blocks | ByteAddressBuffer loads: every frame texture, sampler and buffer index (`DCLFFrameTextureIndex`), the record backings, `DCLFSpanRegister` blocks | Fits as is. The pass-wide blocks are already heap CBVs (`FrameBlockViews`, FrameSource's `ConstantBuffer<DCLFRawBlock>`) |
| Material row | 1024 B: VS b1 at 0, PS b1 at 256, header (texture and sampler indices) at 768 | Device ref from the sequence's row address; lazy macros per member (`PulledSource.cpp`), eager statics where the name is assigned | Fits: game cbuffer packing, blocks on 256 B (static_asserts in `DrawPipelines.h`) |
| Pipeline row | 2048 B: b0 VS/PS, b2 VS/PS, b4 permutation, header at 1536 | Same | Fits |
| Shadow material row | 256 B | Same | Fits |
| Animated material row copies | 1024 B, GPU-written each frame (`TextureTransformsCS`) | Redirected by the header's `animatedRow` | Fits; needs a UAV to constant-buffer barrier |
| Objects, extras, placements, shading, skin materials, pixel rows | 16-160 B strides, MB tables | StructuredBuffers already | Only as 64 KB pages; low priority |
| Sequences, indirect draw records, palettes, vertices | | Raw | Stay raw |

The per-pixel device-ref loads split roughly into row members (15-25), header indices (10-15), sequence and backing words (6-10),
and frame-record descriptor indices (the rest). These are estimates. A real count needs the D3D12 source dump plus DXIL.

One more reason to prefer CBVs, beyond raw load cost: DXIL raw and structured buffer loads cannot be merged across UAV stores
(the texture feedback store, `DeviceStore`), while cbuffer loads can. Repeated row loads in the pixel stage may be that.

## Options

**A. Heap CBVs, both backends.**
- The pulled rewrite emits `ConstantBuffer<DCLFMaterialRowCB>` (`float4 r[64]`) and `ConstantBuffer<DCLFPipelineRowCB>`
  (`float4 r[128]`) reads in place of `DCLFRowFloat4` loads, using FrameSource's raw-block member code (swizzles, matrices).
- Every member can go lazy, so `EagerRowNames` and the `DCLF*Statics` functions go away.
- The game's packoffsets stay the source of truth.

**B. Root CBVs per draw, D3D12 only.**
- b0, b1, b2 and b4 stay real `cbuffer` registers, set per draw by an `ExecuteIndirect` constant-buffer argument from the 256 B
  aligned block addresses the sequences already hold.
- This was the benchmark's fastest case (1.0x). It reuses the game's declarations as they are.
- It costs more:
  - BasicRHI D3D12 layouts are root constants only, `FillDx12Arg` has no constant-buffer argument, and `IndirectArg` has no root
    index.
  - The `DrawSequence` layout must be reordered for D3D12's packed arguments.
  - CPU-pipeline records must inline the addresses.
  - Relative rows and animated-row redirection must be resolved before the signature reads them.
- Plain multi-draw on Vulkan cannot rebind per draw.

**Vulkan on Pascal gains nothing from either.** Under BasicRHI's descriptor-sets model, heap CBVs are lowered to read-only storage
buffers (`cbvAsStorageBuffer`: 15 uniform buffers per stage cannot hold a heap). GPUs with descriptor buffers keep real uniform
buffers when their limits allow.

## Descriptors for option A

Two layouts:

- **One CBV per row.** The slot is `cbvBase + rowIndex`, from words the shader already loads (the backing's ref and the row's
  offset), with `cbvBase` in the frame record's `RecordBacking.pad[0]`.
  - Needs a contiguous descriptor range per backing: 65,536 material rows times framesInFlight + 2 backings is up to about 330K
    descriptors, before pipeline and shadow rows.
  - Pascal's view heap is capped at 1,000,000, and NVIDIA D3D12 has already hit view-heap pressure (d3d12-heap-limit-viewrefs).
- **64 KB pages (recommended).** One CBV per 64 rows (material) or 32 (pipeline). The slot is `pageBase + row / 64`, the register
  `(row % 64) * 64 + k`.
  - 1,024 descriptors per 65,536-row backing.
  - The cost is a dynamic register index into the cbuffer. A draw's rows are the same for every lane, so the index is uniform per
    draw. It must be measured on Pascal first (benchmark step below).

Either way:
- **ORG needs a range allocator:** `IDescriptorService` hands out single slots.
- **Descriptor lifetime:** descriptors are made per backing, not per version, since a CBV depends only on resource and offset.
  The places are the payload chains in `PayloadVersions.cpp` and the frame-value pools. Retire them like `FrameBlockViews`.
- **Pass declarations:** passes declare `ConstantBuffer` access beside `ShaderResource`.
- **Relocation and defrag:** CBVs name the native resource, so backings are pinned as `FrameBlockViews` does, or the views are
  rewritten when ORG moves a buffer.

## Order

1. **Benchmark first (an hour).** Add to pascal-fetch-bench:
   - a paged heap CBV with a dynamic register index;
   - a per-row heap CBV picked by a root constant;
   - a per-draw root CBV through `ExecuteIndirect`.
   Run each with draw-varying rows, so the working set is not one cache line. This picks per-row against pages, and A against B.
2. **Frame record and every frame block as CBVs.**
   - Pass-uniform, no allocator, `FrameBlockViews` exists.
   - `DCLFFrameRecord.hlsli` reads the record as `ConstantBuffer`.
   - FrameSource's span blocks use views for every register.
   - Measure on the 1070 Ti with the frozen scene (`set timescale to 0`, `fw 81a`).
3. **Material, pipeline and shadow rows (option A).**
   - Range allocator, views per backing, the base in `RecordBacking`.
   - The rewrite emits CBV reads; the header index helpers read the row CBV.
   - The relative-rows path (`DCLFRowBases`) needs its own bases.
   - Animated copies get a UAV to constant-buffer barrier before the depth build.
4. **Option B**, if step 1 shows root CBVs clearly ahead of heap CBVs on Pascal and step 3 leaves a gap worth it.
5. **Object tables as pages**, only if a measurement points there.

Measure each step against the D3D11 timer of the same draws: the stable-scene numbers above are the baseline. The first run after
a shader change is not a timing run (draws are skipped until their pipelines compile).

## Step 1 result (benchmark, 2026-11-22)

A different 1 KB row per draw, 24 float4 per pixel, 8.29M pixels. GPU ms on the 1070 Ti:

| Variant | 4096 draws | 65536 draws |
| --- | --- | --- |
| Root CBV, same row every draw (floor) | 1.34 | 1.71 |
| Heap ByteAddressBuffer (today) | 3.47 | 5.88 |
| Heap `StructuredBuffer<float4>` | **2.29** | **3.49** |
| Heap CBV per row | 2.58 | 4.12 |
| Heap CBV per 64 KB page (dynamic register index) | 2.58 | 4.03 |
| Root CBV per draw (`ExecuteIndirect` CBV argument) | 2.58 | 8.84 |

- With rows that differ per draw, a heap StructuredBuffer is the fastest path, 13-18% ahead of heap CBVs. CBVs only win when the
  data is the same for every pixel, as the frame record and frame blocks are.
- The dynamic register index into a page costs nothing, and `WaveReadLaneFirst` changes nothing.
- A per-draw root CBV collapses at small draws (65536 draws of about 127 px), so option B is dropped.
- Spreading the same rows over a 64 MB table instead of 4 MB changed nothing.
- Vertex side: the row-read choice barely matters (all within 0.5%), except the per-draw root CBV (+9%).

**Revised step 3:** read material, pipeline and shadow rows as heap `StructuredBuffer<float4>`. One structured SRV (stride 16)
per backing, whole buffer, beside the view-ref pair. Its slot goes in `RecordBacking.pad[0]`, and the shader reads
`rows[(ref.y - backingRef.y) / 16 + reg]`. No range allocator, no 256-byte views, no constant-buffer state. Step 2 (frame record
and blocks as CBVs) stands.

## Notes found on the way

- `GpuLayouts.h:281` says the material/pipeline split of the row word is 20/12 bits; `BuildDrawsCS.hlsl:824` and the docs use 16/16.
- The docs' shading-row table says 96 B; `LightingConstants.h:224` is 128 B.
- An earlier RTX 3090 Ti test of the object row as an address-pushed constant buffer gave no gain (dclf-architecture.md). That was
  Ampere, address-sourced and the object row only, so it does not settle Pascal.

## Step 2 result (2026-11-22)

Implemented (D3D12: the frame record read through per-face record CBVs whose slot replaces frame push word 0; the five
non-pass-wide frame registers and a zero block as 64 KB CBVs), validated, and set aside in `git stash` ("step2-cbv"):

- No gain. Frozen scene: 54.7 and 54.9 ms against 53.7 ms; main opaque 23.3 and 23.9 ms against 22.5 ms.
- Device loss: 3 of 3 native D3D12 tours lost the device (DRED page fault inside a list of direct DrawIndexedInstanced calls,
  once just after entering Dragonsreach). With the stash removed, 2 of 2 tours passed. Not debugged.
- The D3D12 debug layer reported nothing new (id 926 and 1422 are pre-existing).

Taking about 30 uniform raw loads per pixel off the main pass changed nothing measurable. The benchmark's whole row-read difference
(ByteAddressBuffer to StructuredBuffer) is under 1 ms at the main pass's pixel count. So row and record loads are not what
makes the main pass 12 ms slower than D3D11 on Pascal, and step 3 is not expected to close the gap. The colour pass already
declares `[earlydepthstencil]` where it writes texture feedback, so late depth testing is ruled out too.
