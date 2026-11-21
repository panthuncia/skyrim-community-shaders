# DCLF on native D3D12: draw cost

Native D3D12's DCLF draws against native Vulkan's, on the RX 6600.

## Indirect draws and the first instance

### Measured

Dragonsreach (`perf-dragonsreach`), per-frame GPU ms from the `[GpuEvents]` window.

| Pass | D3D12, indirect | Vulkan, indirect | D3D12, serial |
| --- | --- | --- | --- |
| Shadow view | 4.12 | 2.54 | 2.30 |
| Z-prepass | 0.99 | 0.35 | 0.39 |
| Main opaque | 4.52 | 1.67 | |

"Indirect" is the CPU pipeline's default record mode. "Serial" is `CS_DCLF_CPU_RECORD=serial`, which records direct draws.
With direct draws, D3D12 matches Vulkan.

### Cause

D3D12's `SV_InstanceID` does not count the draw's first instance. Vulkan's does.
So each D3D12 indirect signature had a root constant argument before the draw.
It wrote the draw's first instance into push word `kDrawPushInstance` (23), and the stages added it (`DCLFInstanceWord`).
Vulkan's signatures are a plain DrawIndexed.
On AMD, an `ExecuteIndirect` that sets a root constant per command is much slower than a plain one.

### Fix

Shader Model 6.8 gives the vertex stage `SV_StartInstanceLocation`.

- The D3D12 device creator records whether the device has 6.8 (`Backend::SetD3D12StartInstance`).
  `Backend::D3D12InstanceWord()` is true only on D3D12 below 6.8.
- With 6.8, DCLF's stages compile as `vs_6_8`/`ps_6_8` with `DCLF_START_INSTANCE`.
  Each DCLF vertex entry then takes `SV_StartInstanceLocation` (`DCLF_START_INSTANCE_INPUT`) and adds it to its instance.
  `DCLFDrawSequence` and RunGrass no longer add the push word.
  The pixel stages get the full instance from the vertex stage, as before.
- Every signature that had the Constant argument is then a plain DrawIndexed or Draw, with Vulkan's offsets.
  That covers the refraction, shadow view and Z-prepass signatures, the draw families' instance word signature, and the
  CPU pipeline's indirect runs.
  The records keep their instance word; the draws start past it.
- Direct draws pass first instance 0, so they need nothing more.
- Below 6.8 the root constant path stays.
  `CS_ORG_D3D12_INSTANCE_WORD=1` forces it on a 6.8 device, to compare the two.
- FXC and Vulkan builds are unchanged: everything is behind `DCLF_START_INSTANCE`, which only D3D12 builds get.

The game's DXC is the Vulkan SDK's `dxcompiler.dll` (1.4.357.0 ships DXC 1.9.0.5399), which compiles Shader Model 6.8.

## Open questions

- Decided by the agent, to confirm: a switch, `CS_ORG_D3D12_INSTANCE_WORD`, keeps the old path for A/B runs. It costs one branch at
  start-up.
- Decided by the agent, to confirm: every DCLF D3D12 stage compiles as 6.8 when the device has it, not only the pulled ones.
  One target for all of them keeps one compiler key per request.
