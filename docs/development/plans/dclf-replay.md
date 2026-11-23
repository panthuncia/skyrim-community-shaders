# DCLF pass capture and replay (D3D12): plan

Why: probing the cost of DCLF's pulled Lighting stages on the GTX 1070 Ti takes a game launch per probe, and every launch
recompiles every Lighting permutation. The scene and the hangs make it noisy too. Instead, capture one frame of DCLF's Z-prepass
and main-opaque passes on native D3D12, then replay them in a standalone D3D12 app. The app runs the game's DXIL or one variant
recompiled offline with probe defines (`-D DCLF_PROBE=n`), times each bucket, and needs no game.

Status: 2026-11-22, in progress.

## Pieces

1. **Slot table (BasicRHI D3D12).** It records, for each shader-visible slot BasicRHI writes (CBV/SRV/UAV and samplers), the
   resource and the view desc. It is only on when the capture switch is set, behind a debug accessor
   (`Device::DescribeDescriptorSlot` or similar). One choke point in `rhi_dx12.cpp`, where views are written into the shader-visible heaps.
2. **Capture (CS, DCLF).**
   - Switch `CS_DCLF_CAPTURE=<dir>`, registered in SwitchRegistry.cpp and switches.json.
   - cs-test command `dclf:capture`, which arms the capture for the next frame.
   - On that frame:
     - The depth target is copied before the Z-prepass and before main opaque.
     - At frame end the render thread waits for the GPU to go idle, before the frame's resources can retire.
     - It reads back the metadata (sequences, counts, frame record, rows).
     - It walks every slot the data references and copies those resources on an immediate command list.
     - It writes the dump.
   - A one-off hitch is fine.
3. **Offline compile** (`tools/shader-compare/dclf_d3d12_stage.py`). Takes one stage line from the manifest (the
   `CS_DCLF_STAGE_DUMP` stage-line format) and a Shaders tree, and builds DXIL with the game's D3D12 chain:
   1. `dclf-shadertool rewrite`;
   2. prepend `#include "Common/DCLFFrameRecord.hlsli"`;
   3. `dxc -P`;
   4. `dclf-shadertool d3d12`;
   5. dxc compile with `-HV 2018`, the defines and the target.
   
   Extra `-D` flags are passed through. Check: rebuilding the baseline gives the captured DXIL, byte for byte or apart from debug info.
4. **Replay** (`pascal-fetch-bench replay <dir>`, in `replay.cpp`).
   - Recreates every resource, and every view at its original slot, in a 1,000,000-slot resource heap and a 2,048-slot sampler heap.
   - Relocates the listed pointer fields.
   - Builds the root signature, command signature and PSOs, then replays the passes.
   - Times each bucket and each pass over N iterations, with timestamp queries.
   - Options: `--pass`, `--bucket`, `--iterations`, `--override <bucket>:<vs|ps>=<file.dxil>`, and `--check`, which renders once and writes
     the colour targets as PNG next to the dump. Baseline replay must match the game's frame visually.

### Using the tools (written 2026-11-22; replay checked only on a synthetic dump so far)

```bash
# A probe variant of bucket 12's pixel stage, from the dump's psStage line (writes <dump>/override/b12.ps.dxil)
python tools/shader-compare/dclf_d3d12_stage.py --manifest <dump> --bucket 12 --stage-kind ps --define DCLF_PROBE=3
# A stage line from a stages.jsonl; --as-d3d12 converts a Vulkan line (drops -fvk, adds DCLF_D3D12_FRAME to pulled stages)
python tools/shader-compare/dclf_d3d12_stage.py --stage stages.jsonl --line 5 --as-d3d12 -o out.dxil --keep --print

pascal-fetch-bench replay <dump>                       # time every bucket, 100 iterations
pascal-fetch-bench replay <dump> --check               # baseline PNGs and hashes: check-<pass>-colour0.png, -depth.png, check.txt
pascal-fetch-bench replay <dump> --check --override 12:ps=<dump>/override/b12.ps.dxil   # same files with -override
pascal-fetch-bench replay <dump> --bucket 12 --override 12:ps=... --iterations 200 --serialize
pascal-fetch-bench replay <dump> --pipelines-only --debug                               # PSO creation only
```

- Other replay options: `--warmup N` (5), `--tag NAME` for the `--check` file suffix, `--serialize` (a UAV barrier between
  buckets, so their timestamps do not overlap), `--adapter SUBSTR`, `--no-wait` (skip waiting for the game and cs-test.lock).
  With `--debug` the exit code is 2 when the debug layer reported errors.
- Each pass runs as its own command list, so pass uploads land in buffers that are back in `COMMON`.
- A colour target with a `colourBefore` file is loaded from it at the pass start. Any other colour target is cleared to `clear`.
  The replay never reads a bucket's `set`.
- Unlisted heap slots hold null descriptors (a raw buffer SRV); unlisted sampler slots a linear wrap sampler.
- The offline chain uses the Vulkan SDK's `dxc.exe` with the game's `dxcompiler.dll` (1.9.0.5399, from
  `<Shaders>/../SKSE/Plugins/CommunityShaders/bin`). DXC 1.9 signs DXIL itself, so no `dxil.dll` is needed. The root file is the
  intermediate in the work folder, not `Data/Shaders/Lighting.hlsl`, so names in debug info differ from the game's build.
- `pascal-fetch-bench/tools/make_synthetic_dump.py <dir>` writes a test dump; run `replay <dir> --check` and then
  `make_synthetic_dump.py <dir> --verify`.

## Dump format (version 1)

`<dir>/manifest.json` plus binary files under `<dir>/res/`. All binary data is raw little-endian. Textures are written per subresource in
D3D12 placed-footprint order (`GetCopyableFootprints` of the original desc), so the replay can upload them with the same footprints.

```jsonc
{
  "version": 1,
  "frame": 12345,
  "adapter": "NVIDIA GeForce GTX 1070 Ti",
  "resourceHeapSlots": 1000000, "samplerHeapSlots": 2048,
  "resources": [
    // buffer: original GPU VA, so pointer fields can be relocated
    { "id": 0, "kind": "buffer", "name": "...", "bytes": 1048576, "gpuVA": "0x...", "file": "res/0.bin", "uav": true },
    // texture: the D3D12_RESOURCE_DESC fields; file holds every subresource
    { "id": 1, "kind": "texture", "name": "...", "dimension": 3, "format": 98, "width": 2048, "height": 2048, "depthOrArraySize": 1,
      "mips": 12, "flags": 0, "file": "res/1.bin" }
  ],
  // every slot the captured passes can reach; desc fields are the D3D12 view desc, flattened
  "views": [ { "slot": 999000, "type": "srv", "resource": 0, "desc": { "viewDimension": 1, "format": 0, "firstElement": 0, "numElements": 262144,
               "structureByteStride": 0, "flags": 1 } } ],
  "samplers": [ { "slot": 3, "desc": { "filter": 85, "addressU": 1, "addressV": 1, "addressW": 1, "mipLODBias": 0, "maxAnisotropy": 16,
                  "comparisonFunc": 1, "borderColor": [0,0,0,0], "minLOD": 0, "maxLOD": 3.4e38 } } ],
  // 64-bit GPU VAs stored in data; the replay maps each to (resource containing it, offset) and writes the new VA
  "pointers": [ { "resource": 5, "offset": 4 } ],
  "rootSignature": { "drawWords": 24, "drawRegister": 190, "frameWords": 34, "frameRegister": 191 },
  "commandSignature": { "stride": 136, "instanceConstantWord": 23 },  // instanceConstantWord null with SM 6.8 start instance
  "indexBuffer": { "resource": 7, "offset": 0, "format": 57 },
  "viewport": { "width": 1920, "height": 1080 },
  "targets": { "colour": [ { "format": 10 } /* 8 entries, null for unused */ ], "depth": { "format": 40, "width": 1920, "height": 1080 } },
  "passes": [
    {
      "name": "Z-prepass",                                   // or "main opaque"
      "depthBefore": "res/depth-zprepass.bin",               // depth at the pass's start, same footprint rules
      "frameWords": [ /* 34 uint, pointer words already listed in framePointerWords */ ],
      "framePointerWords": [0],                              // word indices holding a 64-bit VA (low word index)
      "buckets": [
        {
          "index": 0, "set": 12, "first": 4096, "capacity": 512, "count": 431,
          "drawWords": [ /* 24 uint */ ], "drawPointerWords": [],
          "argsResource": 3, "argsOffset": 557172, "countResource": 4, "countOffset": 48,
          "pipeline": {
            "key": "vs 0000000F ps 13000019 flags 00000000",
            "vs": "res/pso12.vs.dxil", "ps": "res/pso12.ps.dxil",  // ps null for a depth pipeline without a pixel stage
            "vsStage": { /* the CS_DCLF_STAGE_DUMP "stage" line */ }, "psStage": { },
            "state": { /* D3D12 PSO fields: rasterizer, depth-stencil, blend per target, sample mask, topology, rtv/dsv formats */ }
          }
        }
      ]
    }
  ]
}
```

### Field details (as the replay reads them)

Added 2026-11-22 while writing the replay. Fields marked optional take the default shown.

- **Numbers:** 64-bit values (`gpuVA`, a CBV's `bufferLocation`) may be a JSON number or a string, hex (`"0x..."`) or decimal.
  `//` comments are accepted.
- **`stageOptions`** (top level): the `CS_DCLF_STAGE_DUMP` "options" line (`materialTextures`, `shadowMaskSlot`, `samplerSlots`,
  `lazyRows`). `dclf_d3d12_stage.py --manifest` needs it for the pulled rewrite; without it the script uses the game's defaults.
- **Texture files** (and `depthBefore`): the exact bytes of a readback buffer that `CopyTextureRegion` filled at the
  `GetCopyableFootprints` footprints of the resource desc, starting at offset 0. That is every subresource (mips, then array slices,
  then planes, in subresource-index order) at its footprint `Offset` with its `RowPitch`, and the size is the footprints' total.
  The replay also accepts tightly packed rows (no row or subresource padding) and tells them apart by file size.
  Optional texture fields: `sampleCount` (1).
- **Buffers:** `uav: true` creates the buffer with `ALLOW_UNORDERED_ACCESS`. Every buffer stays in `COMMON` and is promoted
  implicitly, so buffers need no state in the manifest.
- **Views** (`desc` keys are the D3D12 view desc fields in camelCase, flattened across the union):
  - `srv`: `viewDimension` (D3D12_SRV_DIMENSION), `format`, `mapping` (Shader4ComponentMapping, default 5768);
    buffer: `firstElement`, `numElements`, `structureByteStride`, `flags`; textures: `mostDetailedMip`, `mipLevels`,
    `firstArraySlice`, `arraySize`, `planeSlice`, `resourceMinLODClamp`, `first2DArrayFace`, `numCubes`, `firstWSlice`.
  - `uav`: `viewDimension` (D3D12_UAV_DIMENSION), `format`; buffer: `firstElement`, `numElements`, `structureByteStride`,
    `counterOffsetInBytes` (no counter resource), `flags`; textures: `mipSlice`, `firstArraySlice`, `arraySize`, `planeSlice`,
    `firstWSlice`, `wSize`.
  - `cbv`: `offset` and `sizeInBytes`, the offset into `resource`; or `bufferLocation` (the old VA, relocated like a pointer).
  - `"resource": null` writes a null view of that type and desc (BasicRHI's null UAV at a read-only buffer's slot + 1, for example).
    A null view still needs `viewDimension`; it defaults to a raw buffer.
- **Samplers:** `filter`, `addressU`, `addressV`, `addressW`, `mipLODBias`, `maxAnisotropy`, `comparisonFunc`, `borderColor`,
  `minLOD`, `maxLOD`.
- **Bucket `index`** is unique across the whole dump (number buckets across passes), because `--override` and `--bucket` name a
  bucket by it.
- **Pipeline `state`** (all optional, D3D12 defaults otherwise):
  - `rasterizer`: `fillMode`, `cullMode`, `frontCounterClockwise`, `depthBias`, `depthBiasClamp`, `slopeScaledDepthBias`,
    `depthClipEnable`, `multisampleEnable`, `antialiasedLineEnable`, `forcedSampleCount`, `conservativeRaster`;
  - `depthStencil`: `depthEnable`, `depthWriteMask`, `depthFunc`, `stencilEnable`, `stencilReadMask`, `stencilWriteMask`,
    `frontFace` and `backFace` (`stencilFailOp`, `stencilDepthFailOp`, `stencilPassOp`, `stencilFunc`);
  - `blend`: `alphaToCoverageEnable`, `independentBlendEnable`, `renderTarget` (array of `blendEnable`, `logicOpEnable`,
    `srcBlend`, `destBlend`, `blendOp`, `srcBlendAlpha`, `destBlendAlpha`, `blendOpAlpha`, `logicOp`, `renderTargetWriteMask`);
  - `sampleMask`, `primitiveTopologyType` (3), `primitiveTopology` (4, for IASetPrimitiveTopology), `numRenderTargets`,
    `rtvFormats` (8 entries), `dsvFormat`, `sampleCount`, `stencilRef`, `blendFactor`.
  All enum values are the D3D12 numbers.
- **Targets:** `targets.colour[i]` may add `width`, `height` (default: the viewport), `clear` (4 floats, default 0) and
  `resource` (a captured texture to render into instead of a new one; it needs the render-target flag). `targets.depth` may add
  `resourceFormat` (the typeless format the depth texture is created with, when it differs from the DSV format), `clear`
  (default 1.0, used only without `depthBefore`) and `resource`, likewise.
  A pass binds colour targets 0 to n-1, where n is the largest `numRenderTargets` of its pipelines.
  Optional pass field `uploads` (empty), added by the capture: `[ { "resource", "offset", "file", "pointers": [byte offsets in the
  file] } ]`. The Z-prepass and main opaque read the same frame constants buffer, and each epoch rewrites it with its own frame record,
  so the capture copies such buffers on the GPU right before each pass. Before replaying the pass, write each file into its resource at
  `offset` and relocate its listed pointers. A resource's top-level `file` holds its end-of-frame contents, and the top-level `pointers`
  cover that file. Slots the capture lists nothing for get null descriptors.
  Optional pass field `colourBefore` (absent), added by the capture for main opaque: 8 entries, a file per bound colour target
  (null for none), holding the target at the pass's start with the same footprint rules as `depthBefore`. Main opaque pipelines can
  blend with the engine's blend state, so the replay loads these instead of clearing. `targets.colour[i]` then also carries the
  target's `width` and `height`.
  A bucket's `set` is null for a Z-prepass call, which has a Z bucket number (its count word) and no set index.
  Optional pass field `depthReadOnly` (false): bind a read-only DSV, with depth in `DEPTH_READ | shader resource`, for a pass that
  samples the depth it tests against.

Notes:
- **Index buffer:** the pulled stages fetch their own indices, but the index buffer is still bound as the game binds it.
- **Pointer fields** are listed by the capture, which knows the layouts. These are the frame push record address, the record's block
  addresses and backing addresses, and every 64-bit address of each drawn `DrawSequence` (bytes 4, 12, 60, 68, 76, 84, 92 and 100:
  the rows, the block addresses and the index buffer address). They sit in the passes' `uploads`, so the top-level `pointers` is empty.
  The replay stays layout-agnostic.
- **The texture feedback buffer** is captured like any other buffer. The replay may let it accumulate.
- **Counts:** the draw count per bucket is the captured count word. The replay uses the captured count buffer as is.
