# GTX 980 Ti: raster pass cost, guest slowdown, pipeline binaries (handoff)

Status: 2026-11-24, done: guest and native D3D12 are at native Vulkan's frame time (see "Results" below). Hardware: GTX 980 Ti (Maxwell, driver 582.78), 6 GB, no ReBAR (214 MB host-visible
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

## Results (2026-11-24, after the fixes)

Frozen scene (`tgm`, `set timescale to 0`, `fw 81a`, TAA, a 600-frame `org:gpu-window`), Dev-Fast:

| | Native Vulkan | Guest before | Guest after | Native D3D12 |
|---|---|---|---|---|
| GPU frame (graphics queue) | 45.7 ms | 67.9 | 44.9 | 43.0 |
| Frame time | 45.3 | 68.6 | 45.2 | 43.3 |
| DCLF main opaque | 14.2 | 27.5 | 14.2 | 15.0 |
| DCLF Z-prepass | 4.5 | 5.5 | 4.2 | 4.0 |
| pre-main | 4.0 | 7.3 | 4.1 | 3.3 |
| deferred+water | 7.7 | 9.8 | 7.5 | 6.7 |

raster-perf (unfrozen save scene) frame time: native Vulkan 36-37 ms, guest 35-36, native D3D12 33-35. The 58 ms D3D12 frame
above came from a run with the NvPerf timing trace (it serialises passes); without it D3D12 is the fastest of the three.

## Guest vs native: fixed

Cause: in guest mode BasicRHI read PushIndex constant buffers (DCLF's frame blocks, OMS shared constants) from the heap as
storage buffers. Native binds them as uniform buffers in a push descriptor set. That is the register-pressure problem that
cost Pascal's D3D12 path 10 ms (`dclf-bound-constants.md`). Two things kept it off in guest:
- BasicRHI's `VkInitBoundUniforms` refused bound uniform buffers on any adopted device. DXVK binds its own descriptors again
  after interop commands (`DxvkContext::invalidateExternalCommandState`), so the push set is safe: guard removed (BasicRHI c695c56).
- DXVK never enabled `VK_KHR_push_descriptor`. CS now asks for it with `dxvkRequestDeviceExtensions`, whose requests now add up
  instead of replacing each other (DXVK e8f692d6, CS 9787908d9).
Check in a guest log: `AdoptVulkanDevice: descriptor-sets model: bound uniform buffers on`.

What the earlier candidates showed: DXVK's render targets have no create flags (`flags 0x0`, usage 0x1f, exclusive sharing);
material textures are plain BC images in SHADER_READ_ONLY_OPTIMAL; the main depth target is graph-made in both modes.
None of it mattered.

Left: native D3D12 main opaque is 0.8 ms over native Vulkan (DXIL vs SPIR-V on NVIDIA's compiler), but the whole frame is faster.

## Pipeline binary store and module identifiers: done

- The binary store held one 2 MiB binary per entry with the same key in all 5,101 entries (10.9 GB file). Fixed: binaries kept
  once by driver key, stable store keys. NVIDIA still keeps ~2 MiB of process memory per pipeline made from binaries (guest 25 GB
  vs 12.9 GB without), so the store is off by default on NVIDIA (`BASICRHI_VK_PIPELINE_BINARY=1` turns it on). The blob is
  524,288 x 4 bytes, the resource set's slots per binding; avoiding it would need a different slot mapping.
- Replacement where the binary store is off: the module identifier store (BasicRHI b244ced, `VK_EXT_shader_module_identifier`).
  Each lowered module's identifier is kept by its lowering key in `<pipeline cache>.identifiers` (about 120 KB), with the static
  sampler placements. A pipeline whose stages all have identifiers is made from them through the pipeline cache, with
  `FAIL_ON_PIPELINE_COMPILE_REQUIRED`: no lowering, no modules. A miss lowers as before. DXVK passes the feature to the adopter
  (d3d985d1). Warm precompile of 1,008 DCLF pipelines: guest 15.8 s to 0.6 s, native Vulkan 8.0 s to 0.4 s; 0 misses. Pipelines
  made from identifiers keep normal memory, and the 256 MiB in-memory lowered module cache no longer fills on warm runs.
  `BASICRHI_VK_MODULE_IDENTIFIERS=0` turns it off. Log: `Vulkan module identifiers '...': saved N identifiers ...; M pipelines made
  from them so far, K missed the cache`.
- A persisted lowered-SPIR-V cache was tried first and dropped: ~3,300 modules of ~80 KB each (a 300 MB file).

## Repository state

Everything above is committed and pushed (CS `org_integration`, BasicRHI `dev`, DXVK `org_integration`; OpenRenderGraph,
ORGModuleServices, BasicTelemetry and Streamline fast-forwarded to their tracked branches). `BASICRHI_VK_FORCE_GENERAL` is kept as
a debug switch. The DXVK `DXVK_CS_NO_ROBUSTNESS2` experiment and the per-image import log line were removed.
Worktrees that can go: `org-port-worktrees/{inventory-item,basicrhi-binaries,dxvk-robustness,basicrhi-robustness,cs-robustness}`;
keep `lighting-probes` for the probe work (Lighting fix candidates under "Raster pass cost" are still open).
