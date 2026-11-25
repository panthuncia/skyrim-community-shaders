# Round 2 lead `strip`: switch inventory and removal (2026-11-25)

Branch `perf2/strip` (CS), based on `org_integration` 742af7870. No ORG, BasicRHI or DXVK changes.

The user's direction: "strip back old codepaths - we don't need to keep old switches around that just reactivate slower
codepaths." I listed every switch: the rows of `src/Testing/SwitchRegistry.cpp`, every DCLF `RegisterSwitch` call, and the
names read past the registry (`getenv`, `GetEnvironmentVariable`, `Util::SwitchFile::Value`). Switches owned by other leads
(`faces`: `CS_DCLF_REFLECTION_NATIVE`, `CS_DCLF_SKY_NATIVE`; `post`: `CS_ORG_POST_NATIVE`, `CS_ORG_IMAGESPACE_NATIVE`) are
left out.

## Commits

One commit per switch or per small family, so any one can be dropped.

| Commit | Switches |
|---|---|
| `refactor(org): drop CS_ORG_D3D12_INSTANCE_WORD` | `CS_ORG_D3D12_INSTANCE_WORD` |
| `refactor(upscaling): drop CS_UPSCALE_SUBMIT` | `CS_UPSCALE_SUBMIT` |
| `refactor: drop CS_NO_PASS_PREFETCH` | `CS_NO_PASS_PREFETCH` |
| `refactor: drop CS_NO_D3DX_UPGRADE` | `CS_NO_D3DX_UPGRADE` |
| `refactor: drop CS_FG_DISPLAY_SUSPEND` | `CS_FG_DISPLAY_SUSPEND` |
| `refactor: retire fog debug modes 3 and 4` | `CS_ORG_FOG_DEBUG` 3 and 4 (the switch stays) |
| `refactor(llf): drop CS_ORG_LLF_PARITY` | `CS_ORG_LLF_PARITY`, config `llf-parity` |
| `refactor(dclf): drop CS_DCLF_HEAP_FRAME_BLOCKS` | `CS_DCLF_HEAP_FRAME_BLOCKS` |
| `refactor(dclf): drop CS_DCLF_MAP_HOLD` | `CS_DCLF_MAP_HOLD` |
| `refactor(dclf): drop CS_DCLF_PIPELINE_EXPAND` (+ a docs follow-up) | `CS_DCLF_PIPELINE_EXPAND` |
| `refactor(dclf): drop CS_DCLF_WATER` | `CS_DCLF_WATER` |
| `refactor(dclf): drop the effect coverage switches` | `CS_DCLF_OPAQUE_EFFECTS`, `CS_DCLF_EFFECT_CLOUD_LIGHT`, `CS_DCLF_EFFECT_FALLOFF` |
| `refactor(dclf): drop the particle coverage switches` | `CS_DCLF_BILLBOARD_PARTICLES`, `CS_DCLF_MASTER_PARTICLES` |
| `refactor(dclf): drop CS_DCLF_FIRST_PERSON_DECALS` | `CS_DCLF_FIRST_PERSON_DECALS` |
| `refactor(dclf): drop CS_DCLF_EMPTY_FADE_NODE` | `CS_DCLF_EMPTY_FADE_NODE` |
| `refactor(dclf): drop CS_DCLF_LOD_GATES` | `CS_DCLF_LOD_GATES` |

Each commit removes the switch's read and the old branch, its registry row, its `tools/cs-test/switches.json` row, and the docs
that described it as current (`dclf-validation.md`, `render-graph.md`, `dclf-architecture.md`). Plans and dated logs keep their
history. Feature switch anchors that named a deleted switch (`.after`, `.before`) now name `CS_DCLF_PLAIN_SNOW_SPARKLE`, so the
commits do not depend on each other. Fallbacks for a missing capability stay, without a switch: devices without Shader Model 6.8
keep the instance root constant, a DXVK without `dxvkEnqueueInteropSubmission` keeps the waiting ring submission, a runtime with
an unknown address leaves the master particles and the LOD swaps to the engine.

The cs-test tool has no "reduced" bisection mode in this branch: `SwitchReduced` only marks a narrowed featureset in DCLF's
startup log, and the tool does not read it. Nothing in the tool needed a change but the `llf-parity` config and its README row.

## Inventory

Decisions: **deleted**; **kept** (with the rule that keeps it); **kept, to confirm** (decided by the agent, one line why);
**listed** (lives only in a submodule, so not changed here).

### Render-path switches

| Switch | What a non-default value does | Decision | Why |
|---|---|---|---|
| `CS_ORG_D3D12_INSTANCE_WORD` | 1: per-draw root constant on SM 6.8 devices | deleted | A/B for the older, slower path on AMD; the no-6.8 fallback stays |
| `CS_UPSCALE_SUBMIT` | direct: ring submissions flush and wait for DXVK's stream | deleted | "the previous behaviour"; the missing-export fallback stays |
| `CS_NO_PASS_PREFETCH` | 1: no next-pass prefetch detour | deleted | A/B escape hatch for an optimisation |
| `CS_NO_D3DX_UPGRADE` | 1: the engine runs the real D3DX math | deleted | A/B escape hatch for an optimisation |
| `CS_FG_DISPLAY_SUSPEND` | 1: frame generation unloads around display changes | deleted | Workaround for a freeze whose cause was a DXVK lock; the display state is still logged |
| `CS_ORG_FOG_DEBUG` modes 3, 4 | 3: fog's D3D11 path in graph mode; 4: hand out unwritten volumes, as before a fix | deleted (modes only) | Old paths inside a debug switch; modes 1, 2, 5-9 are bisection aids and stay |
| `CS_ORG_LLF_PARITY` | 1: also run LLF's D3D11 culling and compare with the graph's | deleted | Compare mode of a finished port |
| `CS_DCLF_HEAP_FRAME_BLOCKS` | 1: D3D12 frame blocks read by heap index | deleted | A/B for bound tables (main opaque 22 -> 13 ms on Pascal) |
| `CS_DCLF_MAP_HOLD` | 0: no re-entry hold at the map's camera cuts | deleted | Brings back frames drawn with the other camera's set |
| `CS_DCLF_PIPELINE_EXPAND` | 0: the pipeline expander is off | deleted | Older precompile without neighbours; it was not even in the registry |
| `CS_DCLF_WATER` | 0, 1, 3: water not tracked, tracked unclaimed, or left to batch hooks in CPU frames | deleted | Earlier phases of the water port |
| `CS_DCLF_OPAQUE_EFFECTS`, `CS_DCLF_EFFECT_CLOUD_LIGHT`, `CS_DCLF_EFFECT_FALLOFF` | 0: those effects stay the engine's | deleted | Coverage switches whose DCLF path is the default in every mode |
| `CS_DCLF_BILLBOARD_PARTICLES`, `CS_DCLF_MASTER_PARTICLES` | 0: those particles stay the engine's | deleted | The same |
| `CS_DCLF_FIRST_PERSON_DECALS` | 0: first-person decals stay the engine's | deleted | The same |
| `CS_DCLF_EMPTY_FADE_NODE` | 0: the shared empty fade node is a fade root | deleted | The old behaviour wrote a static engine node |
| `CS_DCLF_LOD_GATES` | 0: LOD swaps ungated | deleted | Brings back the swap gap T6b5 fixed |
| `CS_DCLF_CPU_RECORD` | serial: direct draws on DXVK's CS thread | kept, to confirm | Serial's colour pass was 0.5 ms of GPU time faster on the RX 6600; a vendor trade-off and a live toggle |
| `CS_DCLF_PORTAL_EVALUATOR` | batched: count, scan, scatter, evaluate | kept, to confirm | Older and slower, so it should go, but it spans 9 GPU programs, 5 scratch buffers and the bucket sizing in `Render/Epochs` and `Render/Shadow`, which lead `dclf` is editing now. Do it after that merge |
| `CS_DCLF_CAPTURE_PARITY`, `CS_DCLF_PARITY_BOTH` | 1: compare DCLF's tables with the engine's lighting draws, which also draw everything | kept, to confirm | The only reference for DCLF's material constants against the engine's own; DCLF is not a finished port |
| `CS_DCLF_TEX_STREAM_FULL` | 1: every window the full chain at load | kept, to confirm | A test baseline for streaming comparisons, not a replaced path |
| `CS_DCLF_TREE_LIST_ALL` | 1: the T7f verdict for every tree | kept, to confirm | Its 1 is the newer path, off by default |
| `CS_DCLF_EFFECT_RIGID_SKINS` | 1: DCLF draws rigid-skinned effects | kept | New path waiting for shot checks |
| `CS_DCLF_PIPELINE`, `CS_DCLF_CULL` | pipeline and cull choice | kept | Live toggles; Pascal and Maxwell default to the CPU pipeline, the RX 6600 to frustum culling |
| `CS_DCLF_ASYNC_COMPUTE` | queue choice | kept | Differs by mode; timing aid |
| `CS_DCLF_TEX_STREAM`, `CS_DCLF_TEX_DISTANCE`, `CS_DCLF_TEX_FEEDBACK`, `CS_DCLF_PLAIN_SNOW_SPARKLE`, `CS_DCLF` | feature off | kept | Feature toggles and menu overrides |
| `CS_DCLF_SCENE_COPIES`, `CS_DCLF_SHRINK`, `CS_DCLF_CPU_BATCH`, `CS_DCLF_TEX_STREAM_*` tuning, `CS_FRAME_LATENCY`, `CS_FSRFG_QDEPTH` | tuning values | kept | Tuning, not an old path |
| `CS_ORG`, `CS_ORG_NATIVE`, `CS_ORG_NATIVE_BACKEND`, `CS_NATIVE_D3D11` | mode selection | kept | Mode selection |
| `CS_ORG_D3D12_SDK(_PATH)`, `CS_STREAMLINE_DLSS`, `CS_STREAMLINE_REFLEX` | runtime or plugin choice | kept | Driver and hardware workarounds |
| `CS_FORCE_FSR_FG`, `CS_UPSCALE_UNFOCUSED` | test overrides | kept | Test the AMD frame generation path on NVIDIA; unfocused runs |
| `CS_SHADER_DXC` | DXC selection | kept | Rollout in progress |
| `ORG_FRAME_PLACEMENT`, `CS_ORG_DEFRAG`, `ORG_RELOCATION_STRESS` | newer experimental paths, off by default | kept | Not old paths |
| `CS_ORG_SSS_KEEP_APART`, `CS_ORG_SSGI_DEBUG` | keep a transient out of aliasing | kept, to confirm | Aliasing bisection aids; they pick a different lifetime, not an old implementation |
| `CS_ORG_VRAM_SOURCE` | org: budget and attribution from ORG's allocator | kept | Accounting source, not a render path |

### Diagnostics and test hooks (kept)

| Switch | Why kept |
|---|---|
| `CS_DCLF_PARITY`, `CS_DCLF_PLAYER_PARITY` | Read back and compare; no other render path |
| `CS_DCLF_STATS`, `_PASS_STATS`, `_PROFILE`, `_SITE_STATS`, `_RELEASE_GUARD`, `_SHADER_DEBUG`, `_SHADER_SOURCE_DIR`, `_STAGE_DUMP`, `_CAPTURE`, `_TABLE_START`, `_TREE_LOD_TINT`, `_TREE_LOD_STAGES`, `_PORTAL_TRACE`, `_PORTAL_UNIFORM_SCAN`, `_TEX_STREAM_DELAY_MS`, `_LATE_POSE` | Logging, dumps, stress and test hooks |
| `CS_DCLF_PIPELINE_PRECOMPILE`, `CS_DCLF_PROGRAM_MANIFEST`, `CS_ORG_PIPELINE_CACHE` | Cold-cache test hooks (`sky-cold-programs` uses one) |
| `CS_ORG_ENGINE_DRAW_TAP`, `CS_ORG_MISMATCH_FATAL`, `CS_ORG_TAP_ERROR_TEST` | Strictness and harness tests |
| `CS_ORG_SHARED_CONSTANTS_CHECK`, `CS_ORG_RAW_ACCESS_CHECK`, `CS_ORG_ALIAS_LOG`, `CS_ORG_OCCLUSION_QUERY_LOG`, `CS_ORG_RECORDING_TIMESTAMPS`, `CS_ORG_ENGINE_CONSTANTS_LOG_FRAMES`, `CS_ORG_GRAPH_RESOURCES_FRAME`, `CS_ORG_TRIPWIRE_*`, `CS_ORG_NATIVE_VALIDATION` | Checks and logs |
| `CS_ORG_SHOT_TARGET`, `CS_ORG_SHOT_PHYSSKY`, `CS_ORG_BINK_SHOT`, `CS_ORG_VIEW_ATMOSPHERE_DEBUG`, `CS_ORG_FOG_DEBUG` (other modes) | Readbacks and debug views |
| `CS_ORG_PRESENT_COPY_SKIP`, `CS_ORG_NATIVE_SYNC_PRESENT` | Timing and race diagnostics |
| `CS_GPU_*`, `CS_PASS_*`, `CS_NVPERF_*`, `CS_GPA*`, `CS_PROFILER_LOG`, `CS_ENGINE_ZONES`, `CS_FEATURE_GPU_ZONES`, `CS_SUBMIT_TRACE`, `CS_RHI_CENSUS`, `CS_TEX_CENSUS`, `CS_GRASS_COLLISION_STATS`, `CS_VRAM_DEBUG`, `CS_AB_PROBE`, `CS_HANG_WATCHDOG`, `CS_TEST_*`, `CS_FXC_REQUEST_DUMP`, `CS_D3D12_SOURCE_DUMP`, `CS_RENDERDOC_*`, `CS_SL_VERBOSE`, `CS_VK_DRIVER_TRACE`, `CS_VK_PRIORITY`, `CS_VK_CLIENT_PRIORITY`, `CS_VK_SPILL`, `CS_DISABLE_FEATURES`, `CS_ENABLE_FEATURES`, `CS_EDITOR` | Profilers, dumps, harness and residency experiments |
| `DXVK_*`, `VK_*`, `SARP_D3D12_*` | Other projects' own variables |

### In submodules (listed, not changed)

| Switch | Where | Note |
|---|---|---|
| `ORG_BOUND_PUSH_TABLES` | OMS `ProgramBuild.cpp` | 0: graph compute programs read constant buffers from the heap, the A/B for bound push tables. A candidate for deletion, but OMS has `update = none`, so it is not changed here |
| `BASICRHI_VK_BOUND_UBO` | BasicRHI | 0: heap storage CBVs on the descriptor-sets model, the slower path on Pascal and Maxwell. Candidate |
| `BASICRHI_VK_FORCE_GENERAL` | BasicRHI | An experiment (every image in GENERAL). Candidate |
| `BASICRHI_VK_BINDING`, `BASICRHI_DX12_FULL_BARRIERS`, `BASICRHI_SPLIT_SUBMITS`, `BASICRHI_PAD_LISTS`, `BASICRHI_HEAP_BIND_REPEAT`, `BASICRHI_RELEASE_COMMAND_MEMORY` | BasicRHI | Binding-model override (hardware testing) and barrier or submission experiments; check each with BasicRHI's owner |
| `BASICRHI_VK_PIPELINE_BINARY`, `BASICRHI_VK_MODULE_IDENTIFIERS`, `BASICRHI_VK_PIPELINE_ROBUSTNESS` | BasicRHI | Defaults differ by vendor; keep |
| `ORG_GRAPH_REPLAY_OUTPUT` | ORG | Diagnostic |

## Tests

Build: Dev-Fast (unity), warning-clean. Each changed DCLF and render-graph file also compiles alone (outside its unity batch),
since several deletions removed includes. `run list-switches` then `schema` from this worktree left `switches.json` as the
hand edits had it (186 switches).

| Config | Label | Result |
|---|---|---|
| `list-switches` | `p2-strip-switches` | passed |
| `guest+quick` | `p2-strip-guest-quick` | passed |
| `native-vulkan+quick` | `p2-strip-native-vulkan-quick` | passed |
| `native-d3d12-quick` | `p2-strip-native-d3d12-quick` | passed |
| `guest-interior` (coc Dragonsreach and back out) | `p2-strip-guest-interior` | never-loaded: the autoload never left the main menu (no load after the start-up one) |
| `guest-interior` | `p2-strip-guest-interior-2` | passed, 4 shots |

The first interior run sat at the main menu for 120 s, so the save never loaded; the rerun passed. Its engine-draw `[E]` lines
(5) match earlier `guest-interior` runs on other stages (6 and 7). No config starts in an interior, so there is no
interior-start run. No perf capture: no deletion changes a hot path's default.

## Open items

- `CS_DCLF_PORTAL_EVALUATOR`'s batched evaluator: delete after lead `dclf`'s merge (see the table).
- `kEffectVariantCloudLighting` is never set now. The bit stays so the variant names keep their order in reports.
- SE: no switch deleted here defaulted differently on SE. The deletions keep the defaults, which SE already ran (untested at
  runtime: the game folder is AE 1.6.1170).
