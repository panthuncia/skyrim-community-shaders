# Shader precompile proposal

Status: proposal, 2026-11-21. No code has changed.

Read first: `docs/development/shader-cache.md`, the V2 shader store's design.

## Goals

1. Ship Community Shaders with as many shaders precompiled as possible.
2. A change in the CS menu never wipes the cache. It compiles only the variants that are new, and switching back costs nothing.
3. ORG and DCLF shaders are precompiled. Nothing compiles on demand on a warm start.

## Summary

- The V2 store already does the hard part. It is content-addressed and checks every file a variant read, so it never needs a wipe.
- The menu still clears everything in seven places. The cause is an in-memory map keyed by descriptor only, plus a global "reload every ORG pipeline" call. Both can go.
- On a warm start the D3D11 side is clean: 7,700 to 7,850 requests, all hits. ORG builds 2,641 programs at setup, all hits. What is left is DCLF.
- DCLF compiles the same nine Utility shadow stages on demand in almost every run: 277 of the last 300 runs. They are derived shadow techniques the engine never loads, so no precompile asks for them. They can be enumerated in closed form.
- DCLF also builds 340 to 500 pipelines on demand per run. Those are driver pipelines, not shaders, and need a pipeline-key manifest and a driver pipeline cache.
- Shader bytecode can ship for every backend. Driver pipeline caches cannot. They are per driver and per GPU.
- A shipped pack should be a read-only blob pack next to the writable store, checked by the same file hashes, compressed with zstd.
- ORG and DCLF stages carry debug info today. In the store sample, 95% of DXIL bytes are the `ILDB` debug part. Release must build without it, or a pack is ten times larger than it needs to be.

## What happens today

### Who compiles what

| Producer | Compiler | Output | When | Frame waits? |
|---|---|---|---|---|
| CS game shaders (Lighting, Utility, Effect, ...) | FXC through the store (`Utils/FxcShaderStore.cpp`). DXC for a few blurs (`CS_SHADER_DXC`) | DXBC | Startup precompile from the engine's technique lists (`Hooks.cpp:176`), or at first draw | No. The engine shader draws until CS's is ready |
| DCLF Lighting and Utility stages | DXC through OMS `ShaderCompiler` (`ShaderPrograms.cpp:527-554`) | SPIR-V or DXIL | Requested as a side effect of each CS compile task (`ShaderCache.cpp:3311-3319`), else on demand on the DCLF pipeline lane | No. The draw stays native until the pipeline is in the catalog |
| DCLF engine draw families (sky, water, blood, shadow mask, precipitation, menu scenes) | DXC through `RasterProgram` (`EngineDrawProgram.cpp:322-348`) | SPIR-V or DXIL | At each family's `Setup`, from the local manifest (`EngineDrawProgram.cpp:531-557`), else at first use | No |
| ORG programs (compute and raster) | DXC through `ProgramService` (`RenderGraph/ProgramService.cpp`) | SPIR-V or DXIL | Queued at setup and joined once (`ProgramService.cpp:253-263`). Late keys gate the next revision | Setup waits once. Later keys do not stall frames |
| D3D12 DCLF pulled stages | DXC preprocess, then `FrameSource::Rewrite`, then compile (`DxilStages.cpp:60-147`) | DXIL | Same as DCLF stages | No |
| DXVK guest path | DXVK converts DXBC to SPIR-V, then the driver builds pipelines | SPIR-V, then driver code | At shader creation and first draw | Can hitch |

### What a warm start looks like

From `C:\Modding\CS-AB\runs\`, the last 14 runs on 2026-11-21 (all one shared store):

| Measure | Warm run (typical) | Notes |
|---|---|---|
| `Shader store after shader compilation` | 526 requests at boot, then 7,742 to 7,850. All hits, `0 preprocessed` | CS D3D11 shaders plus the DCLF stages they request |
| ORG setup batch | 2,641 programs, all hits. 480 ms waited on D3D12, 2,000 to 2,100 ms on Vulkan | The Vulkan wait is driver pipeline creation, not compiling |
| DCLF `on-demand SPIR-V compile` | 7 to 9 stages per run | Mostly from the disk cache, but each waits about 3 s on the lane |
| DCLF `on-demand pipeline build` | 339 to 506 per run | Driver pipelines for Lighting slots and shadow views |
| DCLF family `was not prebuilt` | 0 to 5 per run | The local manifest lists 161 keys |
| ORG `was not queued ahead` | 0 | ORG is already precompiled at setup |

A cold start, from runs with an empty store:

| Measure | Cold run |
|---|---|
| CS startup precompile | 6 min 22 s to 6 min 51 s, about 6,550 requests: about 4,000 compiled and 2,000 absorbed |
| ORG setup batch | 2,480 programs, 25.1 s to 27.8 s waited |

Two warm runs in the sample compiled 1,224 and 4,053 variants anyway:

- `1121-191427` had 4,082 `root changed`. A different build ran on the same store. That is correct behaviour.
- `1121-192611` had 1,224 `no record`, and the store opened 3,075 evicted. The store held 6.4 GB against its 4 GB limit, so eviction dropped variants that this build still needed. That comes from switching between builds and backends with debug-info artifacts (see "Size" below).

### The DCLF stages that are always on demand

Counted over the last 300 runs:

| Stage | Runs |
|---|---|
| Utility VS, PS of technique `00002001` | 277 |
| Utility VS `04002083`, PS `0401408F`, PS `0400C08F`, PS `0000C004` | 274 each |
| Utility VS `0002C087` | 267 |
| Utility VS `00034087` | 266 |
| Utility PS `00004004`, VS `00024087` | 37 to 38 (Dragonsreach only) |

Why no precompile asks for them:

- `ShaderPrograms::Precompile` (`ShaderPrograms.cpp:798-850`) runs once per CS compile task. For Utility it asks for the descriptor and its `CanonicalShadowTechnique` form.
- The shadow pipelines ask for something else. `MaterialLookups.cpp:195-200` takes a caster's key technique, ORs in the shadow mode bits, then canonicalises. For example, key technique `0000001B` in the shadow views becomes `0000C004`.
- The engine never loads those composed techniques, so no CS compile task exists for them, so no precompile is requested.
- They are found in the store, but the lane only reaches them after other work, so the casters draw natively for about 3 s.

## Goal 2: every path that clears or recompiles everything

The store never needs a wipe. Two other things force mass work:

- **The in-memory D3D11 maps are keyed by descriptor only.** `ShaderCache::GetVertexShader` looks up `vertexShaders[type][descriptor]` (`ShaderCache.cpp:1929-1935`). The defines are not part of the key, so any define change must empty the map.
- **`ShaderCache::Clear` does three expensive things at once** (`ShaderCache.cpp:2091-2146`). It empties every map. It starts a new store generation, so every file is read and hashed again. And it calls every feature's `ClearShaderCache`. DCLF's version calls `RenderGraphRuntime::RequestShaderReload()` (`DrawcallLimitFix.cpp:872-876`). That bumps the sources generation (`PipelineRegistry.cpp:194-197`) and rebuilds every live ORG and DCLF pipeline (`ShaderPrograms.cpp:767-778`, `:874-896`). The bytecode comes from the store, but the driver builds every pipeline again.

### The paths

| # | Trigger | Where | What it does now | What it should do |
|---|---|---|---|---|
| 1 | Shader Defines text box | `Menu/AdvancedSettingsRenderer.cpp:195-197` | `SetDefines`, then full `Clear()` | Change the define-set id (see the fix below). No generation bump, no ORG reload |
| 2 | Half Precision checkbox | `AdvancedSettingsRenderer.cpp:539-542` | Full `Clear()` | The FXC flags are already in the store key (`FxcShaderStore.cpp:172-188`). Fold the flags into the define-set id. Nothing else |
| 3 | Avoid Flow Control checkbox | `AdvancedSettingsRenderer.cpp:556-559` | Full `Clear()` | Same as 2 |
| 4 | Linear Lighting on or off, ACEScg, Apply Game Gamma, and Post Processing mode changes that turn Linear Lighting off | `Features/LinearLighting.cpp:151-157` | `ShaderCache::Reload`, which waits for the compile pool and runs a full `Clear()` | New define-set id. ORG programs whose common defines changed get new keys in the next revision. Everything else stays |
| 5 | Effects 11 preset active or not | `Features/Effects11.cpp:518-519` | `Reload`, full `Clear()` | Same as 4 |
| 6 | Light Limit Fix "Lights Visualisation" | `Features/LightLimitFix.cpp:244-247` | `SetDefines("LLFDEBUG" or "")`, which **overwrites the user's own shader defines**, then `Clear(Lighting)` | A feature define option (`GetShaderDefineOptions`), not `SetDefines`. That fixes the overwrite. The Lighting define-set id changes |
| 7 | "Clear Shader Cache" button | `Menu/SettingsTabRenderer.cpp:719-720` | Full `Clear()` | Keep as a developer tool. Drop the in-memory maps only. No ORG reload unless asked |
| 8 | "Full Recache..." | `Utils/UI.cpp:776-786`, also `DevBenchBridge.cpp:506` | `Clear()` and `DeleteDiskCache()`: the whole store is deleted | Replace with "Repair shader cache": check each artifact's hash and drop the bad ones. A delete only behind a developer flag |
| 9 | Post Processing mode switch | `PostProcessingMode.cpp:189-191` | Sets `recachePending`, which only shows a banner saying a full recache runs at next restart. Nothing reads the flag otherwise | Remove the banner. With the fixes above, a mode switch compiles only new variants, right away |
| 10 | Every post-processing sub-feature's recompile, and its "Recompile" icon | `ReloadShaderSources()` in 13 files, for example `PostProcessing/Border.cpp:103`; button at `PostProcessing.cpp:276-279` | Starts a new store generation, so every shader's files are hashed again on next use | Recompile only that feature's own programs. Start a generation only when files changed (the file watcher) |
| 11 | Dynamic Cubemaps "Enable SSR" | `Features/DynamicCubemaps.cpp:605-612` | `Clear(path)`, or all ImageSpace if the path is not mapped | Define-set id per type. Keep the narrow clear as a fallback |
| 12 | DCLF "Reload shaders", `OrgCommands.cpp:142-144`, the file watcher (`ShaderCache.cpp:3814-3815`) | | Every live pipeline rebuilds | Correct for edited files. The file watcher already passes the changed file. The others are developer tools |
| 13 | Feature toggles ("Disable at Boot") | Next boot | Every loaded feature's define goes into every Lighting variant (`ShaderCache.cpp:190-201`), so one toggle gives every Lighting variant a new key. This is a mass compile, not a wipe | Unavoidable with today's defines. See "Key space" and phase 6 |
| 14 | `CS_DISABLE_FEATURES` / `CS_ENABLE_FEATURES` test switches | `State.cpp:97-104` | Turns the startup precompile off when a feature with defines changes | Keep for now. With a shipped pack it can go |

Screen Space GI shows the right pattern already. It compares its own defines and rebuilds only its own programs (`ScreenSpaceGI.cpp:852-853`).

### The fix: key the in-memory maps by define set

- Each shader type gets a **define-set id**: a hash of everything outside the descriptor that goes into its compile. That is the user defines, each loaded feature's define and options for that type, the common defines and the FXC flags.
- Recompute it when a setting changes. The maps become `[type][(defineSetId, descriptor)]`.
- A lookup with a new id misses, queues a normal task, and the store answers it. A hit costs a file read. A new variant compiles in the background while the engine shader draws.
- Old entries stay alive, so toggling back is instant. Trim entries of an old id after N minutes unused, or at the next loading screen.
- Only a real file change starts a store generation.
- ORG gets the same rule. A program's store key already contains its defines (`ShaderCompiler.cpp:223-231`). A common-define change should re-request the programs whose defines changed, as a new revision. It must not bump the sources generation. Whether `ProgramService`'s own program map needs a define hash in its key is to be checked in phase 1.
- DCLF's `StageKeyOf` (`ShaderPrograms.cpp:570-579`) has no define hash. That is why it needs a sources-generation bump to notice a change. Add the define-set id to the stage key, and a define change stops needing a reload.

## Goal 1: what can be precompiled, per backend

### Bytecode and its key

| Backend | Shippable output | Key inputs that differ between machines | Shippable? |
|---|---|---|---|
| D3D11 FXC | DXBC | `d3dcompiler_47.dll` identity (PE time stamp, size, checksum; `FxcShaderStore.cpp:23-39`). It is the system copy, which differs between Windows builds | Yes, if CS ships its own `d3dcompiler_47.dll` and loads it by path. Otherwise only users with the same Windows build hit |
| DXVK guest | DXVK's converted shaders (`extern/dxvk/src/dxvk/dxvk_shader_cache.cpp`, version `DXVK_VERSION "+cs.1"`) | DXVK version, DXVK options. Driver independent | Probably. It is keyed by DXBC, so it works only if the DXBC pack is exact. Needs a test |
| Guest DCLF stages, native Vulkan | SPIR-V from DXC | `dxcompiler.dll` and `dxil.dll` identity: CS already ships both (`CMakeLists.txt:894-896`). Vendor define `CS_GPU_AMD` | Yes, one set per vendor variant |
| Native D3D12 | DXIL from DXC | DXC identity, vendor defines (`CS_GPU_AMD`, `DEVICE_REF_ASSUME_UNIFORM`), SM 6.8 start-instance support (`ShaderPrograms.cpp:445-446`) | Yes, a few variants |
| Effects 11 presets | Lowered `.fx` text (`ORGFxRuntime.cpp:1095-1112`) | The user's preset | No. It compiles when the preset loads, as now |

Keys also contain relative paths (`Data/Shaders/...`) and the root text. A pack is valid only for the exact shader files it was built from. That is fine: the pack ships with those files, and the store already checks each file's hash.

### Driver pipelines

- Driver pipeline caches cannot ship. A `VkPipelineCache` blob is tied to vendor, device and driver version. An `ID3D12PipelineLibrary1` is the same. A driver update throws them away.
- They can still help locally. BasicRHI already has `Device::OpenPipelineCache` and `SavePipelineCache` for both APIs (`extern/BasicRHI/rhi.h:3779-3788`, `rhi_dx12.cpp:1409-1530`, `rhi_vulkan.cpp:10405-10500`). CS never calls them.
- NVIDIA and AMD keep their own driver disk caches too. So first measure what a CS-owned cache adds: the 2 s Vulkan setup wait and the DCLF pipeline builds are the numbers to watch.
- What can ship is the **list of pipeline keys**. Then they all build at boot behind the loading screen, not at first draw.

### Key space

| Space | Size today | How to enumerate |
|---|---|---|
| CS D3D11 variants | About 7,750 requests per boot for the default feature set. Cold: about 4,000 compiles plus 2,000 absorbed | The engine's technique lists (`shader->vertexShaders` / `pixelShaders`, `Hooks.cpp:176-228`) plus each feature's `GenerateShaderPermutations`. Fixed for one game version and one feature set |
| ORG programs | 2,641 at setup, 12 declared | Only at runtime. They are queued from about 29 files. Inputs: common defines, vendor, backend, formats, streaming |
| DCLF Lighting stages | 7 stages per (VS, PS) pair (`ShaderPrograms.cpp:589-595`) | From the CS precompile today. Pairs seen only at draw time need a manifest |
| DCLF Utility shadow stages | 2 per technique, plus derived forms | Closed form: every Utility technique, times the shadow mode bits, through `CanonicalShadowTechnique` (`Records.h:198`). The canonical form keeps about 7 bits, so the set is small |
| DCLF family programs | 161 keys in the local manifest, at most 512 per family | The manifest (`EngineDrawProgram.cpp:42-46`) |
| DCLF pipelines | 340 to 506 built on demand per run | A new pipeline-key manifest |

Multiply by the configurations:

- Game version: SE and AE. The technique lists are likely almost the same, but this needs a check.
- Backend: guest (DXVK), native Vulkan, native D3D12.
- Vendor: AMD and others, for the ORG and DCLF defines.
- Feature set and define-changing settings. This is the big multiplier. Every loaded feature's define is in every Lighting variant. Shipping every combination is impossible.

The proposal: **ship the default configuration** (every CORE feature at default settings) for each backend and vendor variant. Anything else compiles once on the user's machine and stays in their store.

### Two ways to enumerate, used together

1. **From the engine's technique lists.** The startup precompile already walks them. It covers D3D11 and, through `PrecompileShader`, most DCLF stages.
2. **From recorded manifests, gathered by a cs-test tour.** These cover what only appears at draw time: DCLF pipeline keys, family keys and Lighting pairs.

Neither covers ORG programs ahead of a run. They are simplest to gather by running: boot once per backend and vendor variant, and keep what the store resolved.

## Shipped pack and local store

```
Data/ShaderCache/
  pack/                 read-only, shipped, replaced on update
    cs-<version>-<backend>-<vendor>.pack
    dclf-programs.txt   family manifest
    dclf-pipelines.txt  pipeline-key manifest
  v2/                   writable, the user's store (as now)
```

- **Format.** One blob pack per configuration, in the same format another agent is adding to the store: zstd-compressed artifacts with an index. The index holds what a store record holds: the variant key, the compile key, and the dependency set with each file's path and hash.
- **Lookup order.** Store record, then pack record, then preprocess, then compile. A pack record is checked exactly like a store record. If the user edited a shader file, its hash differs, and the lookup falls through. A hit from the pack is not copied into the store; at most a small record points at it.
- **The absorbed step looks in the pack too**, by compile key. So a pack still helps after small edits that preprocess to the same text.
- **Versioning.** The pack header carries the store schema (`kSchema`), the CS build hash and the compiler identities. A pack that does not match is ignored, never deleted, and never forces a wipe of the user's store.
- **Eviction** never touches the pack. The 4 GB limit counts only `v2/`.
- **Disk cache off** reads nothing from the pack either, as now.
- **Index flushing.** The index is saved only by `FinishShaderStoreBatch` (`SharedShaderStore.cpp:71-78`). A DCLF stage compiled after the last batch keeps its artifact but loses its record if the game is killed. This costs a preprocess on the next start. Flush after late compiles too, for example at each loading screen.

### Size

Measured in `C:\Modding\MO2\overwrite\ShaderCache\v2\cas` (about 6,000 artifacts):

| Format | Count | Average size | zlib -6 ratio | Notes |
|---|---|---|---|---|
| DXBC | 890 | 87 KB | 4.4x | |
| SPIR-V | 2,357 | 497 KB | 3.9x | Built with debug info |
| DXIL | 2,814 | 681 KB | 2.4x | `ILDB` (debug) is 95% of the bytes |

- ORG programs always set `debugInfo = true` (`ProgramBuild.cpp:82`). DCLF stages do too, except in the release profile (`ShaderPrograms.cpp:84-93`).
- Without debug info, a DXIL program is about 35 KB. zstd should do at least as well as the zlib ratios above.
- Rough budget for the default configuration: about 7,000 DXBC at about 20 KB compressed is about 140 MB. 2,641 ORG programs plus DCLF stages, without debug info, are roughly 50 to 100 MB per GPU backend. Two backends and two vendor variants are 200 to 400 MB. These numbers must be confirmed with a real pack.
- That is too big to bundle everything in the main download. See the open questions.

Release builds must compile ORG and DCLF without debug info, or no shipped pack will ever match. Keep debug info under developer mode, RenderDoc or `CS_DCLF_SHADER_DEBUG=1`, like FXC's `IsDebugCompile`. `tools/aftermath-decode --spirv` reads debug info from the store, so maintainers need a separate debug pack, or debug builds for crash work.

## Goal 3: ORG and DCLF precompiled

### Why on-demand compiles still happen

1. **Derived Utility shadow techniques** (the nine stages above). No CS compile task exists for them.
2. **Lighting pairs seen only at draw time.** The precompile pairs come from the engine's lists, not from real draws.
3. **No precompile at all** when background compilation is on, or the disk cache is off (`Hooks.cpp:176`, `ShaderCache.cpp:1900`). Then DCLF gets no request until CS compiles that descriptor at draw time.
4. **Pipelines.** Every DCLF pipeline (Lighting slots, shadow views per view state) is built when first seen. Nothing prebuilds them.
5. **Family keys** not yet in the local manifest.

### How to fix each

| Cause | Fix | Where |
|---|---|---|
| 1 | At boot, enumerate every canonical shadow technique: each Utility technique the engine loaded, with each shadow mode's bits, through `CanonicalShadowTechnique`. Precompile that set as DCLF's own batch, not as a side effect of CS tasks | `ShaderPrograms::Precompile`, `StartPrecompile` |
| 2, 5 | Ship a gathered manifest of Lighting pairs and family keys. Read it with the local one and take the union | `EngineDrawProgram.cpp:257-293`. A new Lighting-pair list in the same format |
| 3 | DCLF and ORG precompile their own sets whatever CS's background setting is. CS's background mode only defers CS's D3D11 work | `Hooks.cpp:159-172` already does this for the families |
| 4 | A pipeline-key manifest, written like the family manifest when a key is first built, and shipped from tours. Prebuild those pipelines at boot behind the loading screen. Then open a driver pipeline cache through BasicRHI | `DrawPipelines`, `MainPipelines.cpp`, `ShadowPipelines.cpp` |

Rules that stay:

- Compiles never run on the render thread. They run on the DCLF build executor and the programs pool, as now.
- Frames never wait. A missing stage keeps its draw native.
- Boot work runs behind the loading screen. It must not delay the main menu more than it does now (480 ms D3D12, 2 s Vulkan for ORG).

### Gathering the full key set with cs-test

Add a cs-test config, `shader-gather.toml`:

1. Start with an empty store (`build.shader_cache=cold`).
2. For each backend (guest, native Vulkan, native D3D12): boot, run the startup precompile, then a short tour: Whiterun exterior, Dragonsreach, Riverwood, a shadowed interior, water, sky, a menu scene and a blood hit.
3. At exit, write the DCLF manifests and a list of every variant key the store resolved.
4. `tools/build-shader-pack` turns that list and the store into a pack per configuration.

The AMD variant needs `CS_GPU_AMD`. Gathering it needs either the RX 6600 machine or a test switch that forces the vendor defines. Forcing them changes only the bytecode, not what runs, since the pack run never draws with it.

## Phased plan

| Phase | Work | Effort | Risk |
|---|---|---|---|
| 0 | Measurement. A one-line end-of-run summary: DCLF on-demand stages and pipelines, family misses, ORG late keys. A cs-test check that reads them | S | Low |
| 1 | Menu changes stop wiping. Define-set ids for the D3D11 maps. No `Clear()` in paths 1 to 6 and 9 to 11. No generation bump outside the file watcher. Fix the LLF define overwrite. Remove the misleading banner. "Repair" instead of "Full Recache" | M | Medium. The map key change touches every lookup. Old entries must be trimmed so memory does not grow |
| 2 | DCLF closure: precompile every canonical shadow technique. Add the define-set id to `StageKeyOf`. Flush the store index after late compiles | S | Low |
| 3 | Release builds compile ORG and DCLF without debug info. Ship and load our own `d3dcompiler_47.dll` | S | Low for debug info. Licence check for the DLL (it is redistributable from the Windows SDK) |
| 4 | Read-only packs in the store. Builds on the zstd blob-pack work. `tools/build-shader-pack`. `shader-gather.toml`. Packaging in `CMakeLists.txt` and `BuildRelease.bat` | L | Medium. Keys must match exactly between the gather machine and users |
| 5 | DCLF pipeline-key manifest, prebuilt at boot. Lighting-pair manifest. Shipped manifests. Driver pipeline cache through BasicRHI, if phase 0 shows it helps | M | Medium. Prebuilding many pipelines costs boot time and memory |
| 6 | Optional. Shrink the key space: move cheap feature presence and setting defines (for example `CS_GAME_GAMMA`, a float in every variant) to constant-buffer values | L | High. Each one needs a performance check |

Phase 1 and phase 2 do not depend on the pack, and they give most of goal 2 and goal 3.

## Checks

All on warm runs unless noted. Count lines in `CommunityShaders.log`.

| Check | Config | Pass |
|---|---|---|
| No DCLF on-demand stages | `perf-dragonsreach`, `river`, `perf-quick` on Vulkan and D3D12 | 0 lines of `on-demand SPIR-V compile` |
| No DCLF on-demand pipelines after phase 5 | same | 0 lines of `on-demand pipeline build` after the first loading screen |
| No family misses | same | 0 lines of `was not prebuilt` |
| Warm boot stays clean | any | Every `Shader store after` line: all hits, `0 preprocessed` |
| A menu change compiles only new variants | New `settings-toggle.toml`: boot, toggle Linear Lighting, toggle back, toggle Half Precision | No `[ORG] shader reload asked`. The second toggle back logs all hits. `source files read` does not jump to the whole tree |
| Cold start with a pack | `build.shader_cache=cold` with the pack installed | Startup precompile under 30 s (today 6 min 22 s). ORG setup under 3 s (today 25 to 28 s). `compiled` near 0 |
| An edited shader still rebuilds | Edit one `.hlsli` with the pack installed | `dependency changed` names that file, and only its variants compile |
| Pack size | `tools/build-shader-pack` output | Within the budget set in open question 1 |
| No eviction thrash | Alternate two builds four times | `evicted` stays 0 for live variants |

## Open questions

1. **Pack size and delivery.** 200 to 400 MB for every backend and vendor variant. Ship the DXBC pack in the main download and the GPU packs as optional downloads? Or only the backend most users run?
2. **Which configuration to ship.** Defaults only, or also the most common feature sets? Every extra set costs another pack.
3. **Where to gather.** CI cannot run the game. Is a maintainer's machine, running `shader-gather.toml` as a release step, acceptable?
4. **AMD variant.** Gather on the RX 6600 machine, or add a switch that forces the vendor defines?
5. **SE and AE.** One gather on each, or does one technique list cover both? Needs a dump to compare.
6. **Debug info.** Release without debug info breaks Aftermath shader names from users' dumps. Is a separate debug pack for maintainers enough?
7. **Shipping `d3dcompiler_47.dll`.** Fine to bundle it, so that DXBC keys match on every Windows build?
8. **DXVK's shader cache.** Is it worth shipping for the guest path, given the DXBC must match exactly?
9. **"Full Recache".** Remove it for users, or keep it under a confirmation as today?
