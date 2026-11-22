# DCLF on native D3D12: draw cost

Native D3D12's DCLF draws against native Vulkan's, on the RX 6600.

## Colour pass

The pulled Lighting pixel stages' own cost, apart from the indirect signatures' root constant.

### Measured

Dragonsreach (`perf-dragonsreach`, save `Save91_...20261113033158_1_1`), per-frame GPU ms from the `[GpuEvents] graph window 'dr'`
block. One sample per row.

| Pass | D3D12 indirect, before | D3D12 indirect, after | D3D12 serial, after | Vulkan indirect |
| --- | --- | --- | --- | --- |
| `CS DCLF: main opaque` | 4.44 | 2.51 | 2.04 | 1.73 |
| `CS DCLF: forward` | 0.62 | 0.61 | 0.60 | 0.32 |

Before the fix, D3D12 serial was 3.61 ms; Vulkan serial (pushed engine-native pipelines) is 1.47 ms. Both from earlier runs.

### Cause: scratch copies of pass block arrays

D3D12's layouts hold only root constants, so `FrameSource` rewrites each pass-wide constant buffer into reads through the frame
record. A block without `packoffset` (Light Limit Fix's `StrictLightData`, `SharedData`) became a `ConstantBuffer<T>` view, and
each member a static copy of the view's member:

```hlsl
static const Light LightLimitFix_StrictLights[15] = DCLFBlockView_StrictLightData.LightLimitFix_StrictLights;
static const float4 SharedData_WaterData[25] = DCLFBlockView_SharedData.SharedData_WaterData;
```

Both arrays are indexed at run time (`StrictLights[lightIndex]`, `WaterData[waterTile]`). DXC keeps such a copy as an internal
global array (the DXIL has `@"...StrictLights...B.0.0" = internal global [15 x float]` and the like), and AMD's D3D12 compiler
puts it in scratch. Every colour wave loaded the whole of both arrays and stored them to scratch before shading.

RGA (`rga -s dx12 -c gfx1032`, the rewritten sources from `CS_D3D12_SOURCE_DUMP`, a root signature of the two root constant
ranges):

| Program | Scratch per lane | VGPRs | `scratch_*` instructions |
| --- | --- | --- | --- |
| `00000011` colour, before | 1296 bytes | 256 | 278 stores, 19 loads |
| `00000011` colour, after | 0 | 256 | 0 |
| `08010019` terrain colour, before | 1648 bytes | 256 | 423 stores, 97 loads |
| `08010019` terrain colour, after | 384 bytes | 256 | 128 stores, 77 loads |

Vulkan keeps the cbuffer, so the same programs have no scratch there (`BASICRHI_VK_DUMP_ISA`). The terrain program's remaining
384 bytes are the shader's own small local arrays (nine `alloca`s of 2 to 4 elements in the DXIL), which Vulkan's compiler keeps
in registers.

### Fix

`FrameSource` writes an array member as a macro for the view's member, `#define <name> DCLFBlockView_<block>.<name>`, instead of a
static copy. DXC then folds the source's own alias (`NSCB_ARRAY_ALIAS`, LLF's `StrictLights`) to direct constant buffer loads.
Only D3D12 pulled builds are rewritten, so FXC and Vulkan output are unchanged.

Correctness: `native-d3d12+river` and `native-vulkan+river` shots match (mean channel difference 2.2 and 0.7 between the backends,
against 5.3 and 1.2 between the `nfg` stage's own D3D12 and Vulkan runs; the water wheel's angle and the eye adaptation differ).

### What is left: 2.0 against 1.7 ms

The same `00000011` program, D3D12 (RGA, after the fix) against Vulkan (the driver's ISA):

| | D3D12 | Vulkan |
| --- | --- | --- |
| Wave size | 64 | 32 |
| VGPRs | 256 (246 live at most) | 253 |
| Waves per SIMD at that size | 2 | 4 |
| Vector memory loads | 151, 86 of them typed `tbuffer_load_format` | 67, 2 typed |
| Scalar buffer loads | 36 | 130 |
| `v_readfirstlane` | 63 | 162 |
| `s_cbranch_execz` | 354 | 306 |
| Image samples | 85 | 85 |

- **Wave size.** AMD's Vulkan driver compiles these pixel stages as Wave32; its D3D12 driver compiles every pixel stage as
  Wave64 (every RGA build here, small or large, had `UC_VERSION_W64_BIT`). D3D12 has no wave size control for pixel stages
  (`[WaveSize]` is compute, mesh and node only). At 256 VGPRs that is 2 Wave64 waves per SIMD against 4 Wave32 waves: the same
  lanes in flight, but half the waves to hide latency, and each divergent branch costs both halves.
  Forcing Wave64 on Vulkan to measure it (`VkPipelineShaderStageRequiredSubgroupSizeCreateInfo` on BasicRHI's graphics stages)
  did not take: the dumped colour programs stayed Wave32. Not pursued.
- **Heap constant buffer views are vector loads on AMD's D3D12.** A `ConstantBuffer` taken from `ResourceDescriptorHeap` reads
  through `tbuffer_load_format` with a VGPR index, even at a constant offset (a 4-line test shows it). A `ByteAddressBuffer` at a
  uniform offset, or a root CBV, reads through `s_buffer_load`. The pass-wide blocks (`PerFrame`, `SharedData`, `FeatureData`,
  `DCLFFrameLighting`) are such views. Read through a raw buffer instead (an offline variant of the program), the program drops to
  248 VGPRs allocated (227 live), 74 vector loads and 176 scalar ones. That does not change the wave count, so it is not done.
- **The draw's rows are per lane.** The pixel stage's draw comes from the `DCLFDrawInstance` attribute, so every row read
  after it (sequence, rows, material texture indices) is a vector load with a VGPR offset, and the heap index is taken with
  `v_readfirstlane` (`DEVICE_REF_ASSUME_UNIFORM`). `WaveReadLaneFirst(input.DCLFDrawInstance)` turns those into scalar loads
  (`v_readfirstlane` 63 to 5) but leaves the VGPR peak at 246; Effect's loop form (`for (;;) { WaveReadLaneFirst ... break; }`)
  brings 1440 bytes of scratch back, because the row statics are assigned inside the loop. Neither is done.
- The pass's 2.5 ms still includes the indirect signatures' root constant (the other lead's fix). The serial number above
  is the pass without it.

## Open questions

- Decided by the agent, to confirm: the frame rewrite's array members are macros. A macro replaces every later use of the
  name, so a struct member or local of the same name elsewhere in the program would be rewritten too. The only arrays it applies
  to today are LLF's `LightLimitFix_StrictLights` and `SharedData_WaterData`, whose names are namespaced.

## Open items

- An RGP capture of the D3D12 colour pass would show whether its time goes to latency (few waves) or to the typed constant
  loads. Capture: `RadeonDeveloperPanelCLI -m profiling` around `native-d3d12+perf-dragonsreach`; look at `CS DCLF: main
  opaque`'s draws in the Wavefront occupancy and Instruction timing views, and compare one Lighting draw's `tbuffer_load_format`
  and `image_sample` latency with the same draw on Vulkan.
- The terrain programs' local arrays still go to scratch on D3D12 (384 bytes in `08010019`). Finding which arrays, and
  writing them as separate values, is a source change in the shared terrain code.
- Forward (0.61 against 0.32 ms) was not looked at. Its Vulkan programs are Wave64 too, so the wave size is not the reason.
- Reading the pass-wide blocks as raw buffers on D3D12 would take the record's block refs (one more `DrawBindings` field, and the
  cube faces' camera blocks, which live in another buffer). Worth it only with a register cut that brings the colour programs
  under 128 VGPRs, where Wave64 gets 4 waves.
