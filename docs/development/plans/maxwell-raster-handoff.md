# GTX 980 Ti: raster pass cost, guest slowdown, pipeline binaries (handoff)

Status: 2026-11-24, investigation in progress. Hardware: GTX 980 Ti (Maxwell, driver 582.78), 6 GB, no ReBAR (214 MB host-visible
VRAM window). All numbers are Dev-Fast (/Od) builds, 1920x1080 targets with a 1280x720 render area, the base save's exterior scene.

## Tools and how to measure

- **Pass timing:** `guest+raster-perf` / `native-vulkan+raster-perf` / `native-d3d12+raster-perf` (`tools/cs-test/configs/raster-perf.toml`,
  1200 frames after world-ready) with `--set env.CS_NVPERF_TEMPLATE=dclf-timing --set env.CS_NVPERF_START_FRAME=400
  --set env.CS_NVPERF_MAX_SAMPLES=600`. Read the `[NvPerf]` table in `CommunityShaders.log` (the report file
  `Documents/.../SKSE/NvPerf/dclf-timing.txt` is overwritten by each run). Without MAX_SAMPLES a trace may never converge and never report.
- **Frame time:** `[Perf] rendered ... fps (N ms)` lines in the same log.
- **NvPerf does not work on Maxwell:** every capture fails with "failed to build the NVPerf metric configuration". NVIDIA's Vulkan driver
  also returns no pipeline statistics or ISA (`BASICRHI_VK_DUMP_ISA` writes only executable headers).
- **D3D12 replay:** `dclf-replay-capture` config with `--set env.CS_DCLF_PIPELINE=gpu` (the capture hooks only the GPU-driven bucket
  path; this card runs the CPU-pipeline direct passes by default). Dump: `C:\Modding\CS-AB\replay\capture\capture-2170` (validated:
  `--check` matches the game). Replay: `external_refs\pascal-fetch-bench\build\pascal-fetch-bench.exe replay <dump>`.
  Probe variants of Lighting.hlsl: branch `probe/lighting` (f92dc7c1e, worktree `org-port-worktrees/lighting-probes`), all under
  `DCLF_PROBE == n` guards; runner scripts in the session scratchpad `lp/` (`run.py` takes the cs-test lock).
- **Memory:** process private bytes sampled from outside (scratchpad `memsample.ps1`; use `{2:F0}` formatting). Per-resource placement:
  `BASICRHI_MEMORY_DUMP=<dir>\name-{n}.txt` writes a CSV of every BasicRHI resource with its memory kind; in guest it now runs every 6000
  submits (uncommitted BasicRHI edit, see below).
- cs-test only accepts env switches listed in `src/Testing/SwitchRegistry.cpp` (alphabetical, static_assert) and `switches.json`, or
  prefixed `DXVK_`, `VK_`, `CS_NVPERF_`, `CS_GPU_COUNTERS_`. DXVK DLLs must be copied into `C:\Modding\MO2\mods\CS AB pascal\...\bin`
  by hand after `tools/build-dxvk.ps1` and `tools/stage-dxvk-dlls.ps1`.

## Raster pass cost (D3D12 side): done

Pass timestamps, same scene:

| Pass | Guest (DXVK) | Native Vulkan | Native D3D12 |
|---|---|---|---|
| Main colour `cs.dclf.direct` | 17.1 ms | 9.3 ms | 9.5 ms |
| Z-prepass `cs.dclf.z.direct` | 4.0 | 3.2 | 3.2 |
| Forward | 1.9 | 0.82 | 0.94 |
| Shadows | 2.7 | 3.2 | 2.9 |
| Whole frame | 49.4 | 35.5 | 58 |

Replay attribution (D3D12, GPU path, main opaque 8.0 ms, Z 2.2 ms; offline-rebuilt DXIL byte-identical to the game's):
- Trees (bucket 140, 146 draws) 2.33 ms: raster + vertex 0.41, alpha test + material fetches 0.08, lighting 1.84 (Skylighting 0.74 of
  which its coverage test 0.28; point-light loop 0.64 though 99.9% of tree pixels have no lights, 0.21 of it from merely compiling the
  loop in; directional shadows 0.30; row loads ~0.13).
- Plain Lighting (bucket 77, 634 draws) 1.70 ms: floor 0.45; wetness 0.67 (raindrops/wall rain 0.59, the frame was raining); empty light
  loop 0.33; Skylighting 0.23.
- Z trees (bucket 6) 1.00 ms: alpha test 0.43.
- DCLF's per-pixel row loads are 2-6%: the CBV-rows plan (`dclf-cbv-rows.md`) is low priority on this card. Dropping the main-pass
  alpha test for trees gains nothing.

Fix candidates, by saving: split the light loop out when a draw has no lights (~0.5 ms+ per pass); cheaper Skylighting for foliage
(up to 0.74); distance-gate raindrops (0.59 in rain); cheaper Z-prepass tree alpha test (0.43). None implemented.

## Guest vs native: open

The gap is real (frame 49.4 vs 35.5 ms) and specific to DXVK's device: native Vulkan runs the same shaders, lowering and descriptor
model as fast as D3D12.

Ruled out:
- **Robustness, per pipeline.** `VK_EXT_pipeline_robustness` (DXVK enables it, CS requests it, BasicRHI chains `core` behaviour, matching
  native) was confirmed active (`pipelineRobustness : 1`, BasicRHI's "per-pipeline robustness core" line) and main colour stayed 17 ms.
  The device-wide test (DXVK with robustBufferAccess2/robustImageAccess2 off) crashes at boot every time, also with ImprovedCameraSE off
  (crash in engine 77396 on a pathing job; DXVK writes no log), so device-level robustness is not fully excluded.
- **Memory placement.** Per-resource dumps: every BasicRHI resource has the same memory kind in guest and native (frame arena in the BAR
  window, nothing per-pixel in system memory).
- **VRAM pressure.** Guest sits at ~3.8 of 4.4 GB budget; cutting DCLF's texture budget (`raster-perf-texbudget`, -300 MiB) changed nothing.
- **Image layouts.** Guest render targets rest in GENERAL (storage usage; DXVK keeps them there even with
  `dxvk.enableUnifiedImageLayouts = False`), but native with every image forced to GENERAL (`BASICRHI_VK_FORCE_GENERAL=1`) stays 8.8 ms.

Next candidates:
1. **How DXVK creates the textures DCLF samples** (D3D11 textures: MUTABLE_FORMAT, view-format lists, extended usage), which may switch
   off texture compression on NVIDIA. DCLF's pixel stages take ~20 samples and 25-28 texel loads; fits "pixel-heavy passes slow, shadows
   not". Extend the import log line in `src/RenderGraph/DxvkImageImport.cpp` (uncommitted) to print `a_image.flags`, compare with
   native's texture create info.
2. **Samplers:** guest uses the engine's D3D11 samplers through DXVK; compare anisotropy and filter against native's.
3. Anything else DXVK's device enables that BasicRHI's own does not (diff the two devices' enabled features from the DXVK log and the
   BasicRHI create log).

## Pipeline binary store: decided for now

- The store held one 2 MiB binary per entry with the same key in all 5,101 entries (10.9 GB file, read whole at boot). Fixed: binaries are
  kept once by driver key, store keys made stable (static sampler placements persisted). File now 231 MB; warm runs build 0-9 pipelines.
- NVIDIA still keeps ~2 MiB of process memory per pipeline made from binaries (guest 25 GB vs 12.9 GB without), also with one
  `VkPipelineBinaryKHR` kept for all pipelines. The blob is exactly 524,288 x 4 bytes, the resource set's slots per binding: most likely a
  layout-sized table NVIDIA serialises into every self-contained binary, shared through the layout for normally built pipelines. Not
  documented anywhere found; shrinking the set to test it fails the descriptor-sets minimum capacity at boot.
- The store is now off by default on NVIDIA (`BASICRHI_VK_PIPELINE_BINARY=1` turns it on; BasicRHI 20d31ec, CS 918ce0f23). Cost: the
  startup precompile re-lowers SPIR-V every boot (~16 s for 1,008 pipelines). Options for the user: a slot mapping that needs fewer
  slots per binding, or persisting lowered SPIR-V keyed by the pre-lowering hash.

## Repository state (local, nothing pushed)

Committed on `org_integration`: the image-space ports, lens flares, inventory model fix, binary store fixes, cs-test fixes and configs.
BasicRHI commits (e459a49, 20d31ec, ...) must be pushed to BasicRHI's remote before the CS commits that point at them.

Uncommitted / experimental, decide before committing:
- Per-pipeline robustness: merged (`feat/pipeline-robustness` in CS, DXVK 4f752b3d + an uncommitted `HANDLE_EXT(extPipelineRobustness)`
  line that makes it work, BasicRHI 8fb8258); submodule pointers not committed. No measured benefit.
- DXVK `DXVK_CS_NO_ROBUSTNESS2` experiment (dxvk_device_info.cpp): crashes at boot; revert.
- BasicRHI `BASICRHI_VK_FORCE_GENERAL` (rhi_vulkan.cpp) and its switch registry entries: experiment; revert or keep as a debug switch.
- BasicRHI guest memory report from `q_submit` (adopted devices, every 6000 submits): useful; keep or gate.
- `src/RenderGraph/DxvkImageImport.cpp` per-image layout log line: diagnostic; keep while investigating.
- `BASICRHI_VK_PIPELINE_ROBUSTNESS` switch registry entries.
- `tools/cs-test/configs/raster-perf.toml` shortened, `raster-perf-texbudget.toml` new.
- Worktrees that can go: `org-port-worktrees/{inventory-item,basicrhi-binaries,dxvk-robustness,basicrhi-robustness,cs-robustness}`
  once their branches are merged; keep `lighting-probes` for the probe work.
