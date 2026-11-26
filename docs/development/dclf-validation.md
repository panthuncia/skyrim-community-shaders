# DCLF validation

For compact portal bytecode and fade-root/member query batching tests and current integration limits, see
[Portal query batches](./dclf-portal-query-batches.md).

How Drawcall Limit Fix (DCLF) is checked: its switches, the parity checks and what each report line means, the diagnostics
and probes, how a change is validated in game, crash triage, and the bugs parity found in the engine and in other features.

-   [dclf-architecture.md](./dclf-architecture.md) is how DCLF works. This document names its mechanisms only where a check
    needs them.
-   [dclf-open-defects.md](./dclf-open-defects.md) lists the checks that fail or flake today, the open crashes and the accepted
    differences. A flag listed there is not a regression.
-   [dclf-layout.md](./dclf-layout.md) is the code layout, including the diagnostics contract (`Diagnostics/`).
-   `dclf-roadmap.md` lists diagnostics not built yet.
-   [skyrim-engine-notes.md](./skyrim-engine-notes.md) holds the engine behaviour the engine-bug rules below rest on.

Every engine address here is Skyrim AE 1.6.1170's, the only runtime DCLF installs on.

## Contents

-   [Switches](#switches)
-   [Parity checks](#parity-checks)
-   [Diagnostics and probes](#diagnostics-and-probes)
-   [How to validate a change](#how-to-validate-a-change)
-   [Crash triage](#crash-triage)
-   [Bugs found by parity](#bugs-found-by-parity)

## Switches

### How switches are read

-   A switch is read from the environment first, then from `<Documents>\My Games\Skyrim Special Edition\SKSE\CommunityShaders-DCLF.ini`,
    a flat `NAME=value` file beside `CommunityShaders.log` (`Core/Settings/Switches.h`, `Utils/SwitchFile.h`). The file exists
    because Mod Organizer 2 hands the game its own environment block, so variables a launcher exports may never arrive.
-   Some switches are read from the environment only: the render graph's `CS_ORG_*` (and everything else read through
    `RenderGraph/Switches.h`), `CS_GPU_*`, `CS_NVPERF_*`, and DXVK's and the Vulkan loader's variables. The switches file does not
    carry them. `CommunityShaders.env` beside the log does: the plugin applies it to the process environment and the C runtime's
    copy at load (`XSEPlugin.cpp`, `[DevEnv]` lines), so `GetEnvironmentVariable` and `getenv` both see it. A switch read before
    that (`CS_TEST_CRASH`) needs the real environment.
-   Every switch is read once, on first use, so none changes under a running frame. Feature switches register at install
    (`RegisterSwitch`, `Api::FeatureSwitch`). Core's are listed in `Core/Settings/Switches.cpp` and described in the one switch
    table (`kSwitches`, `src/Testing/SwitchRegistry.cpp`).
-   The startup log prints `[DCLF] switches: ...`, grouped as features, parity, diagnostics, test and other. A `CS_DCLF_*` name
    that nothing registered is listed as `UNKNOWN (read by nothing)`, so a misspelt or removed switch does not silently do nothing.
-   It also prints `[DCLF] featureset: full ...`, or warns `[DCLF] featureset: REDUCED by ...` naming each feature switch set to
    a narrowing value. A validation or timing run must show `full`.
-   Most feature switches are the seeds of live toggles: the DCLF menu, or a `dclf:<toggle name>=0|1` cs-test step, changes
    them in a running game.

Defaults: a feature marked "on" is on when unset. "Unless 1" means any value other than `1` turns it off; "unless 0" means
only `0` turns it off. Parity, diagnostic and test switches are off unless set.

### Feature toggles

| Switch | Default | Meaning |
| --- | --- | --- |
| `CS_DCLF` | on (unless 0) | `0`: DCLF does not install. Graph mode cannot run without it, and outside graph mode DCLF never installs. |
| `CS_DCLF_CULL` | `occlusion` (`frustum` on AMD) | `off`, `frustum` or `occlusion`: GPU culling. `off` and `frustum` count as a reduced featureset. AMD defaults to `frustum` while its HZB reads back empty (dclf-open-defects.md). |
| `CS_DCLF_PIPELINE` | `auto` | `auto`: exteriors GPU-driven, interiors CPU-driven; `manual`, `gpu` or `cpu` (dclf-architecture.md, "The CPU-driven pipeline"). Live: `dclf:pipeline=auto\|manual\|gpu\|cpu`, from the next build point. |
| `CS_DCLF_CPU_RECORD` | `indirect` on every vendor | `serial`: the CS thread records each draw. `indirect`: the cull jobs write draws (needs pulled colour, 0.21 ms of GPU time on an RTX 5080). Live: `dclf:cpu-record=indirect\|serial`. |
| `CS_DCLF_CPU_BATCH` | 4096 | The CPU pipeline's batch lanes, rounded up to a multiple of 8. Live: `dclf:cpu-batch=`. |
| `CS_DCLF_PLAIN_SNOW_SPARKLE` | on (unless 1) | `0`: a snow layer with a plain material (vanilla glaciers and ice) draws without sparkle instead of the Snow class's default. See [the engine bugs](#in-the-engine). |
| `CS_DCLF_TREE_LIST_ALL` | **off** | `1`: the tree manager's list loop gets DCLF's verdict for every `BSTreeNode` DCLF lists; off: for the stood-in trees alone. |
| `CS_DCLF_EFFECT_RIGID_SKINS` | **off** | `1`: effect meshes with a skin instance whose pass has no Skinned bit (the property without `kSkinned`) are DCLF's, each partition drawn with the unskinned vertex stage and the geometry's world, as the engine draws them; `0` they stay the engine's (`effect-variant (skinned)`). |
| `CS_DCLF_TEX_STREAM` | on (unless 0) | `0`: the game's textures stay whole, not shells with streamed mip windows. Read at the first texture load ([texture-streaming.md](./texture-streaming.md)). |
| `CS_DCLF_TEX_DISTANCE` | the menu's | `0`: texture streaming has no distance floor, over the menu's setting. |
| `CS_DCLF_TEX_FEEDBACK` | the menu's | `0`: texture streaming has no shader feedback, over the menu's setting. |

### Parity checks and observers

Each compares a live path with a reference built from scratch and flags every mismatch with a `<-` tag. The sections under
[Parity checks](#parity-checks) say what each reports.

| Switch | Meaning |
| --- | --- |
| `CS_DCLF_PARITY` | `1`: set parity (the claims against the engine's registrations, and the visibility words read back after the colour epoch against the CPU's decisions), the kept stores against a rebuild every 60 frames, and the scene mirror against the live objects every frame. |
| `CS_DCLF_PLAYER_PARITY` | `1`: the [compatibility monitor](#the-compatibility-monitor) starts on. The menu switches it live. |
| `CS_DCLF_CAPTURE_PARITY` | `1`: the tables against the engine's own Lighting draws. Run with `CS_DCLF_PARITY_BOTH=1`. |
| `CS_DCLF_PARITY_BOTH` | `1`: nothing is withheld from the engine, which draws everything while DCLF draws its set too (double draws). |

### Probes and diagnostics

| Switch | Meaning |
| --- | --- |
| `CS_DCLF_STATS` | `1`: the periodic report, every 300 frames. |
| `CS_DCLF_PASS_STATS` | `1`: per-pass counts in the report. |
| `CS_DCLF_PROFILE` | `1`: scene-phase timings in the report. The profile perturbs what it measures. |
| `CS_DCLF_SHADER_DEBUG` | Default on (off in the release profile, where `1` turns it on): SPIR-V with debug info, and the HLSL sources copied beside the log. `0`: without. It inflates the depth pipeline groups. |
| `CS_DCLF_SHADER_SOURCE_DIR` | Where `CS_DCLF_SHADER_DEBUG` copies the HLSL sources. |
| `CS_DCLF_RELEASE_GUARD` | `1`: the engine objects' last releases on DCLF's threads (the scene lane, the pool), counted and named by class (`NiRefObject::DeleteThis` patched). |
| `CS_DCLF_TREE_LOD_TINT` | `1`: DCLF's tree LOD colour draws write solid magenta, so screen captures find its pixels. |
| `CS_DCLF_TREE_LOD_STAGES` | `1`: each stage of the main tree LOD draws stamps the frame; frames a stage missed are logged. |
| `CS_DCLF_PORTAL_TRACE` | `1`: inline portal batches write each query's decision trace. |
| `CS_DCLF_PORTAL_UNIFORM_SCAN` | `1`: portal batches are evaluated with the diagnostic uniform instruction scan. |
| `CS_DCLF_SITE_STATS` | `1`: pipeline statistics per CPU pipeline site, logged periodically (`[DCLF] site stats on`). Read from the environment only. |
| `CS_DCLF_STAGE_DUMP` | `<dir>`: every DCLF stage request's compile inputs, the Lighting programs' descriptor pairs and the pulled rewrite's options, as JSON lines in `<dir>/stages.jsonl`, for `tools/shader-compare`. |
| `CS_DCLF_TABLE_START` | `small`: the growable GPU tables start with a few rows, so growth runs early and often. |

### Test harness

Runs are cs-test configs (`tools/cs-test/README.md`, contract in [testing.md](./testing.md)). Their steps are the
test commands: the generic ones (`shot`, `move`, `turn`, `feature`, `pov`, `draw`, `cast`, `face-female`, `morph-female`,
`mark`, `signal`, `exit`, ...), the render graph's `org:<name>` and DCLF's `dclf:<name>` (`Diagnostics/TestHarness.cpp`):
`dclf:<toggle name>=0|1` (a live toggle), `dclf:cull=`, `dclf:pipeline=`, `dclf:cpu-record=`,
`dclf:compat=0|1` and `dclf:compat-report`, `dclf:foreign-write=<n>`. `capture` and `telemetry` are generic.
The other switches a test run sets:

| Switch | Format | Effect |
| --- | --- | --- |
| `CS_DISABLE_FEATURES` | `<short name>,...` | Those features are disabled at boot for this run only (the saved "Disable at Boot" settings are not changed). Read through `Switches::List` (`State.cpp`): the environment or `CommunityShaders.env`, never the switches file, so a scenario sets it and it does not carry over to later runs. Such a run compiles on demand: give its first shot `wait = "shaders"`. |
| `CS_DCLF_LATE_POSE` | `0\|1` | `0`: the player's parts Improved Camera hides after the shadow maps are not looked for; `1`: they are looked for in first person without the plugin too. |
| `CS_DCLF_TEX_STREAM_FULL` | `1` | Every texture window is the full chain, made as the texture loads (the streaming baseline). |
| `CS_DCLF_TEX_STREAM_DELAY_MS` | `<ms>` | A delay before each background window build, to see late windows. |

### Tuning

| Switch | Default | Meaning |
| --- | --- | --- |
| `CS_DCLF_SCENE_COPIES` | 3 | The copies FrameValues' pools keep a class, 2 to the frames in flight: a frame's producer reuses the copy the oldest in-flight frame bound, its uploads waiting for that frame on the GPU. |
| `CS_DCLF_SHRINK` | on | When the growable scene buffers give memory back (one policy, `PooledBackings::Shrink`): a buffer asked at most a quarter of its capacity for 5 s gets a version at the fresh size for that window's peak. `off`: grow-only; `eager`: at the first such ask, for stress and parity runs. |
| `CS_DCLF_TEX_STREAM_BASE` | 128 | Texture streaming's base window, its largest side. |
| `CS_DCLF_TEX_STREAM_BUDGET_MB` | the driver's budget | The memory texture streaming keeps to, in MiB. |
| `CS_DCLF_TEX_STREAM_RATE_MB` | 256 | Texture upgrades started per second, in MiB. |
| `CS_DCLF_TEX_STREAM_VISIBLE_OFFSET` | 0 | Over the budget, how many mips what is on screen may step down (0: it loads what it asks for). |
| `CS_DCLF_TEX_STREAM_DISTANCE_SCALE` | 1 | The distance floor's sharpness: above 1, finer mips. |

### Other switches DCLF runs depend on

Read from the environment only. The epoch and submission switches are registered in `src/Testing/SwitchRegistry.cpp`, not in
DCLF's registry, so DCLF's startup summary does not list them.

| Switch | Where | Meaning |
| --- | --- | --- |
| `CS_ORG_RECORDING_TIMESTAMPS` | `RenderGraphRuntime.cpp` | `1`: the recordings kept across frames carry pass and barrier timestamps (`GraphHost::SetRecordingTimestamps`), as a timestamp trace turns on; the others are always timed. |
| `CS_ORG` | `RenderGraphRuntime.cpp` | `0`: no render graph; `features`: the interop device features are requested but no graph is made. |
| `CS_STREAMLINE_REFLEX` | registry | `0`: Streamline does not load Reflex, PCL or DLSS-G, for Nsight Graphics captures. FSR-FG then holds 32 frames of input copies in VRAM. |
| `CS_VK_DRIVER_TRACE` | `VkDriverTrace.cpp` | `1`: logs the real present rate and each module's device memory, as the driver sees them, every 5 s. The overlay's DLSS-G "true" rate is an estimate, so this is how to confirm frames are generated. |
| `CS_GPU_EVENT_TIMERS` | `GpuEventTimers.cpp` | `<frames>`: GPU time per engine event, logged every that many frames. Works with DCLF off. |
| `CS_GPU_TIMERS` | `RenderGraph/GpuTimingSource.cpp` | `graph`: the profiler, `CS_PROFILER_LOG` and `CS_GPU_EVENT_TIMERS` read GPU time from the render graph only, with no D3D11 query. See [render-graph.md](./render-graph.md#gpu-timers-from-the-graph). |
| `CS_NVPERF_RANGES` | `Features/RemoteControl/NvPerf.cpp` | A headless Nsight Perf range capture (with `CS_NVPERF_START_FRAME`, `CS_NVPERF_SAMPLES`, `CS_NVPERF_METRICS`, `CS_NVPERF_CSV`). See [nsight-perf.md](./nsight-perf.md). |
| `CS_NVPERF_TEMPLATE` | `Features/RemoteControl/NvPerfTrace.cpp` | A headless Nsight Perf trace of a template (with `CS_NVPERF_LABEL`, `CS_NVPERF_BASELINE`, `CS_NVPERF_START_FRAME`, `CS_NVPERF_SAMPLES`, `CS_NVPERF_REPEATS`). See [nsight-perf.md](./nsight-perf.md). |

### Other Community Shaders switches

Not DCLF's, but read in the same runs. "Env" means the environment (and `CommunityShaders.env`) only; "file" means the
switches file too (`Util::SwitchFile`).

| Switch | Where | Meaning |
| --- | --- | --- |
| `CS_TEST_CRASH` | `CrashDump.cpp`, env (the real environment only) | `<seconds>`: a worker thread faults that long after load, to test the crash dump path ([Crash triage](#crash-triage)). |
| `CS_HANG_WATCHDOG`, `CS_TEST_HANG` | `HangWatchdog.cpp`, env | See [Hangs](#hangs). |
| `CS_AB_PROBE` | `Vram/MemProbe.h`, env | `1`: a row of memory figures a second in `CS-AB-memory.csv`, which `tools/cs-test/compare.py` reads. |
| `CS_NATIVE_D3D11` | `DxvkLoader.cpp`, env | `1`: the native D3D11 runtime instead of DXVK (no render graph). |
| `CS_PASS_SKIP`, `CS_PASS_ROUNDS` | `GpuEventTimers.cpp`, env | Cost attribution by dropping events' draws ([Cost attribution](#cost-attribution-cutting-pieces-out)); `CS_PASS_ROUNDS` is how many times the list is cycled (default 2). |
| `CS_PASS_SAMPLE` | `GpuEventTimers.cpp`, env | Set: a sampler thread profiles the main thread during `CS_PASS_SKIP` runs and logs each thread's share of wall time (`[PassSkip] thread ...`). |
| `CS_GPU_EVENT_STATS` | `GpuEventTimers.cpp`, env | `<event>\|<event>...`: D3D11 pipeline statistics for those engine events, logged every `CS_GPU_EVENT_TIMERS` frames (`[GpuStats]`). |
| `CS_GPU_OP_PROBE` | `GpuEventTimers.cpp`, env | Set: each D3D11 call is an event of its own, unless it sits inside a native batch. |
| `CS_SHADER_DXC` | `Utils/DxcShaderStore.cpp`, env | A regex over `<path>:<target>` choosing the game shaders built with DXC; unset: the image-space blurs only; empty: none ([dxc-game-shaders.md](./dxc-game-shaders.md)). |
| `CS_FXC_REQUEST_DUMP` | `Utils/FxcShaderStore.cpp`, env | `<folder>`: one line per FXC request in `fxc-requests.jsonl`, for `tools/shader-compare/dxc_survey.py`. |
| `CS_RENDERDOC_DLL` | `Features/RenderDoc.cpp`, env | The RenderDoc DLL to load instead of the installed one. |
| `CS_RENDERDOC_REF_ALL` | `Features/RenderDoc.cpp`, env | `0`: RenderDoc's "reference all resources" option off (on by default: DXVK resources outlive its frame tracking). |
| `CS_STREAMLINE_DLSS` | `Upscaling/Streamline.cpp`, file | `0`: Streamline loads neither DLSS nor DLSS-G (DLSS-G's plugin adds `VK_NV_optical_flow` even when DXVK runs on another vendor's GPU). |
| `CS_TEX_CENSUS` | `TextureStreaming/Census.cpp`, file | `1`: the texture survey; `dclf:tex-census` writes `texture-census-<n>.json` ([texture-streaming.md](./texture-streaming.md)). |
| `CS_VRAM_DEBUG` | `Vram/Debug.cpp`, file | `1`: VRAM attribution's debug hooks (`memory-usage.md`). |
| `CS_GPA`, `CS_GPA_DIR` | `RemoteControl/Gpa.cpp`, env | `1` (or a GPU counter request): AMD's GPUPerfAPI counters; `0` never. `CS_GPA_DIR` is where its DLLs are (default the DXVK runtime folder). |
| `CS_GPU_COUNTERS_<suffix>` | `RemoteControl/GpuCounters.cpp`, env | The vendor-neutral names of the range capture's `CS_NVPERF_<suffix>` (`RANGES`, `METRICS`, `START_FRAME`, `SAMPLES`, `CSV`); either works. |
| `CS_NVPERF_MIN_SAMPLES`, `CS_NVPERF_MAX_SAMPLES`, `CS_NVPERF_MAX_REPEATS`, `CS_NVPERF_HALF_WIDTH` | `RemoteControl/GpuCountersTrace.cpp`, env | A template trace's statistics bounds ([nsight-perf.md](./nsight-perf.md)). |
| `DXVK_SHADER_DUMP_PATH` | DCLF's `DrawPipelinesImpl.h`, env | DXVK's own variable; DCLF also writes each pipeline's pixel SPIR-V there, named by its label. |

The render graph's own switches, and BasicRHI's `BASICRHI_VK_CRASH_MARKERS`, are in [render-graph.md](./render-graph.md#switches).

## Parity checks

### How the checks work

-   **The normal path does no validation.** It trusts its producers. Every check is an observer the run installs at
    `Diagnostics::Install()`, before DCLF's threads start (`Diagnostics/Observation.h`, `Hooks.h`). The main path knows the
    observers only through `Diagnostics::Observe()` and the questions in `Diagnostics/Policy.h`, whose answers with every switch
    off are the normal path's.
-   **Some checks change what the main path does,** and `Diagnostics/Policy.cpp` names each change:
    -   `CS_DCLF_PARITY_BOTH` withholds nothing (`WithholdFromEngine`), so the engine draws what DCLF draws;
    -   `CS_DCLF_PARITY` checks a slice of the tracked set against the scene graph and drops entries whose detach was missed
        (`ValidateTrackedSlice`), and neutralises a bound record that names a slot no member holds (`NeutraliseDanglingSlots`).
-   **Schedule.** The rebuild checks run every 60 frames or scene passes, each at its own offset so their rebuilds do not share
    a frame (`Diagnostics::ParityDue`). Report lines come every 300 frames (`Reports::kInterval`). GPU readbacks are read a few
    frames after their epoch: set parity three frames later.
-   **Verdicts.** A line ends in ` <- OK`, or in a tag naming what differed, often followed by `first: ...` naming the first
    case. A line with nothing checked carries no verdict. `<- DIFFER` is the generic tag of many stores' checks
    (`Diagnostics/ParityCounter.h`). Some capture-parity lines use `OK` or `MISMATCH` in place of a tag.

Reading a log:
-   Read every report interval, not only the tail. A run that visits two cells reports two answers.
-   A check that suppresses a computation also suppresses everything downstream of it.
-   A flag listed in [dclf-open-defects.md](./dclf-open-defects.md), "Failing or flaky parity checks", is known. Anything else is
    a regression until shown otherwise.

### Set parity

`CS_DCLF_PARITY=1`. Each colour epoch's visibility words (stamps and verdicts) are copied with the CPU's side that explains
them and read back three frames later (`Diagnostics/Draws/SetParity.cpp`).

| Line | Tag | Means |
| --- | --- | --- |
| `[DCLF] set parity over N frames (...): depth without colour, colour without depth, in the set and drawn by nobody, in the set and GPU-culled, ...` | `<- SET PARITY` | A member drawn by nobody, an object drawn outside the set, the depth and colour segments disagreeing, or a record disagreeing with the set. |
| `[DCLF] set parity over N frames: members rejected with their live bound in view: ... by the frustum, ... by the fade test ...` | `<- FADE` | A member the fade test rejected while the engine's fade says it is shown. |
| `[DCLF] set parity over N frames: the registration hooks' main claims against the set: ...` | `<- CLAIMS` | A claim outside the set (drawn by nobody), or a member left unclaimed. |
| `[DCLF] terrain under Terrain Blending (sampled frame): ...` | `<- TB NEAR` | The GPU's near test for Terrain Blending's terrain against the CPU's over the same inputs. |
| `[DCLF] forward (T7d, sampled frame): ...` | `<- FORWARD` | Fewer forward inputs in the frustum than blended passes of members the engine registered: a member DCLF dropped. |
| refraction line | `<- REFRACTION` | The same for refraction passes. |
| `[DCLF] first person's registrations ...` | `<- FIRST PERSON` | A first-person member's registration the engine kept while DCLF drew the view. |
| first-person filing line | `<- FIRST-PERSON FILING` | A first-person slot filed under another category node. |

Set parity also checks the main set's state classes: an indirect execution set's pipelines must agree in everything but their
shaders, so DCLF keeps one set and signature per state class (both variants' fixed-function state and fragment interface) and draws
each class's sub-range of every colour range with its own call. A draw in another class's sub-range would be drawn with that class's
fixed-function state.

| Line | Tag | Means |
| --- | --- | --- |
| `[DCLF] set class parity (version of N pipelines, N classes): ...` (logged when found) | `<- SET CLASS` | A published set version's class failed a check (`CheckClasses`, `Core/Gpu/Pipelines/MainPipelines.cpp`, after each publish): a member's class word is not its (class, index); its state differs from its class's initial pipeline's (the differing fields named: what the original single-set defect looked like); the handle written at its index is not its own pipeline's; a pipeline is in no class or two; two classes have equal state. |
| `[DCLF] set classes (CPU, published versions so far): N checks, N failed` | `<- SET CLASS` | The count of the above over the run (`Reports/Frame.cpp`, with `CS_DCLF_STATS`). Cumulative: the failures were logged when found. It checks the keys the builds made (`PipelineDesc::State`); a field the key leaves out is invisible to it. |
| `[DCLF] set classes (GPU, colour epoch N): N classes, N sub-ranges, N draws checked (...), N without a class sub-range, N over capacity; N checks, N failed` | `<- SET CLASS; first: ...` | Every 300th colour epoch (`Diagnostics/Draws/SetClassParity.cpp`): the sequences, count words and `classCounts` copied after the epoch and read without waiting a few colour epochs later, against the shape's class plan and indirect state. Per (range, class) sub-range, every sequence below its count with an index count (decals: the group's count; forward: the staging half and the sorted half) names a pipeline its class's sets hold, at the index one of its object's inputs' set indices maps to in that class; no count is past its capacity, no class without a sub-range counted a draw, the unplanned and overflow words are 0, the plan matches the shape (classes, map, sizes), and with DrawSort each class's main sub-range is in pipeline order. It does not cover the first-person view's forward range. |
| `[DCLF] set classes (GPU, colour epoch N): the readback could not be mapped ...` | none | The copy was still in flight after 60 colour epochs, or a map failed: that window is not checked. |
| `[DCLF] state classes (sampled frame): N colour draws without a class sub-range, N past their sub-range's capacity` | `<- SET CLASS DROP` | Every run with `CS_DCLF_STATS`, from the culling counters: BuildDraws dropped a colour draw (each one an object not drawn) for want of a class map word or sub-range, or past its sub-range's capacity. |


### Kept-store checks

`CS_DCLF_PARITY=1`, every 60 frames (`Diagnostics/Draws/KeptChecks.cpp`): object records, geometry draws, draw bounds, extras and
the kept shadow build, each against one computed again. A difference is `<- DIFFER`. Set parity's shading-row check also runs
every 60 colour epochs: `<- UNSAMPLED` is a resident slot whose shading row no producer sampled at two checks running (it draws
with alpha 0).

### Mirror parity

`CS_DCLF_PARITY=1` (or the compatibility monitor): a slice of the tracked set read live at the frame start against the mirror,
every frame (`Diagnostics/Scene/SceneMirrorParity.cpp`). `[DCLF] mirror parity: ...` flags `<- MIRROR` for a record
absent from the mirror or a field the events missed. Fields are classed evented, late or missed. The frame's cull-hidden nodes
(`Api::CullHiddenSource`: the first-person skeleton, the water shapes and the player's 3D) have no hidden bit compared. The
engine hides and shows them within the frame with an event for each store (`Main::Draw`, `TESWaterReflections::Update`; SWF's
README, "Hides within a frame"), and the slice could otherwise land between the two.

### The compatibility monitor

The player-facing parity mode (`Diagnostics/PlayerParity.cpp`): the menu's "Watch for changes other plugins make",
`CS_DCLF_PLAYER_PARITY=1` at the start, or `dclf:compat=0|1`. It runs only checks that leave the main path as it is. It uses
mirror parity's probe and check. It also takes the leak observer's set
members drawn natively, and the mirror's evictions (a subtree that left the world by no hooked detach). Its observers are
switchable (`Diagnostics/Hooks.h`, `SubscribeSwitchable`). The render thread switches them at `FrameBegin`. While the monitor is
off, each of their events costs its one flag test.

Each missed difference, leak or eviction is an offender. A node or geometry offender is keyed by its owning reference: the
probe tags every record with the form ID of the nearest ancestor with user data. A property or alpha offender is keyed by the
shared object, since one write shows on every instance of the mesh, and lists the references it was found under. Each new
offender logs one `<- OFFENDER` line. The menu lists them. The [diagnostic capture](#the-diagnostic-capture) writes the report
as `compat.txt` while the monitor is on, and `dclf:compat-report` writes `CommunityShaders-DCLF-Compat-<time>.txt` beside the log. The report holds the build, the game version, the offenders with
reference, base object and plugin, the load order and the SKSE DLLs. Turning the monitor on starts a fresh list.
`dclf:foreign-write=<n>` makes offenders to check it with.

### Capture parity

`CS_DCLF_CAPTURE_PARITY=1` with `CS_DCLF_PARITY_BOTH=1` (`Diagnostics/Scene/CaptureParity.cpp`). The engine draws everything, and
every native main-pass Lighting draw is compared with the tables DCLF draws the same object with. Reported every 300 frames at
the end of the prepass.

| Line | Means |
| --- | --- |
| `[DCLF] location: cell ...` | Where the window was. |
| `[DCLF] capture parity OK\|MISMATCH: N native main-pass lighting draws, N checked against the tables, N mismatched (material, per-geometry, technique), N untracked eligible, N tracked but excluded, N native-only passes of DCLF objects, N whose pass differs only in the bits the draw decides ...` | The summary. |
| `[DCLF] capture parity by technique (mismatched/checked): ...` and `first <technique> mismatch: ...` | Per technique. |
| `[DCLF] parity mismatches by variable: ...` | The constants that differ, by stage, block and variable. |
| `[DCLF] mismatched materials: ...` | Materials that mismatched, with `written after its last mismatch` when a write event explains it. |
| `[DCLF] capture parity: tracked but excluded, by the verdict now: ...`, `untracked and ineligible, by the verdict: ...` | Draws DCLF does not model, by reason. An `eligible` entry is a verdict DCLF kept stale. |
| `[DCLF] capture parity: N draws' shading changed since the frame's start sampled it (accepted: ...)` | Shading written during the frame (see the accepted differences). |
| `[DCLF] draw parity OK\|MISMATCH: ...` | Draw arguments and bound buffers. |
| `[DCLF] permutation parity OK\|MISMATCH: ...` | The permutation buffer's bits Lighting reads. `IsBeastRace` is compared on face techniques only. |
| `[DCLF] feature binding parity OK\|MISMATCH: ...` | Feature bindings that differ within a frame's eligible draws. |
| `[DCLF] bone palette parity OK\|MISMATCH: ...` | Skinned draws' palettes. |
| `[DCLF] skin parity OK\|MISMATCH: ...` | Advanced Skin's t71/t74 and wetness. |
| `[DCLF] light data parity ...`, `[DCLF] TruePBR texture parity ...` | The integrations' checks (`Integrations/LightLimitFix/Diagnostics`, `Integrations/TruePBR/Diagnostics`). Its `light data parity, shadow lights` part read build parity's readback, which is gone, so it checks nothing. A room mismatch prints up to four `light data room` lines: the live parent chain to the native room node, and the node DCLF's index names. |
| `[DCLF] face capture parity: ...` | Reflection face Lighting draws against their objects' main rows (`CaptureParity::OnFaceLightingDraw`). |
| `[DCLF] constant block coverage: ...`, `[DCLF] variables the native shaders have that DCLF leaves unwritten: ...` | What was compared, and what DCLF leaves unwritten. |
| `[DCLF] render state ... (VARIES)` | Render states seen, flagged when they vary. |

Rules of the comparison:
-   Only components the engine writes are compared. Constant blocks compare bit for bit (`ConstantBlock::SameBits`); unwritten
    components hold `kUnwrittenBits`, a NaN.
-   Bits the draw decides (`PerDrawBits`: the sun bits on a cascade miss, LOD-faded features) are left to their own checks. Such
    a draw is counted as "differs only in the bits the draw decides" and compared on descriptors alone.
-   Hair's ProjectedUV bit is left out (`HairProjection`; see [the engine bugs](#in-the-engine)).

Clean is `OK` on every line. Today's known mismatches (IBLParams, `TexcoordOffset`, fog, terrain LOD techniques) are in
[dclf-open-defects.md](./dclf-open-defects.md), "Failing or flaky parity checks".

### Lines in every report

These need no parity switch. Most come with `CS_DCLF_STATS=1`.

| Line | Tag | Means |
| --- | --- | --- |
| `[DCLF] main camera since the last report: N native passes drawn, N of them set members' ...` | `<- LEAK` | A set member the engine drew in the main camera. Must be 0. Printed in every run. |
| the same line's `drawn by nobody: N lit passes in N frames, at most N a frame (N waiting, N kept by the engine with nothing to draw them)` | `<- NOBODY` | Lit passes into the main views that nobody drew: a waiting object's (withheld) and, in graph mode, a kept one's. Nonzero after a load and for the few frames before a newly attached object's publication is installed (dclf-open-defects.md). Anything else is a regression. |
| `[DCLF] split move: 'name' ... erased by a detach at frame N and tracked again at frame N` | | A geometry a detach erased and an attach tracked again within two frames outside a load. Its parts were drawn by nobody in between. Must not appear in play. |
| `[DCLF] persistent draws (last frame, colour): N kept inputs, N sequences, ...` | | Sequences must be thousands in a populated cell, never 0. |
| `[DCLF] claims published that their records do not draw: N (first: ...)` | `<- UNDRAWN CLAIM` | A claim its publication's tables do not draw (`CheckClaimsDrawable`). Printed only when N > 0. Must never appear: such an object is drawn by nobody until the next publication. |
| `[DCLF] indirect draws (last frame): N candidates ..., skipped: ...` | | Per reason: `pipeline`, `geometry`, `texture`, `sampler`, `constants`, `capacity`, `not-in-set`, `candidate-only`. `constants` and `texture` must be 0. |
| `[DCLF] N pairs read constant registers no frame slot supplies (...): their members are not drawn` | (warning) | Every pair of a pipeline whose constant register the commit does not supply is rejected: its members are withheld and drawn by nobody. |
| `[DCLF] frame textures (last colour epoch): N reads of registers the commit could not resolve, which read zero: ...` | (warning) | A frame texture a pipeline reads that the commit could not resolve. |
| `[DCLF] SPIR-V programs: ...`, `[DCLF] indirect pipelines: ...` | `<- FAILED` | Program and pipeline builds; a failed one is flagged. |
| `[DCLF] on-demand SPIR-V compile ...`, `[DCLF] on-demand pipeline build ...` | | Precompile misses. Expected only in the first frames. |
| `[DCLF] shadow views by the set: ...` | `<- SET SHADOW`, `<- HOLES` | Members a shadow build could not draw; views not ready that have withheld casters (a missing shadow). |
| `[DCLF] shadow capability: ...` | `<- PHASES` | A shadow view the capability did not foresee. |
| `[DCLF] colour sun test: N of 300 frames without the sun's cascades` | `<- NO CASCADES` | Expect 0 outdoors. |
| `[DCLF] sun on the GPU (sampled frame): ...` | `<- DIFFERS` | The GPU's cascade test against the CPU's over the same inputs. |
| producer and payload lines | `<- UNBUILT`, `<- UNREVISED`, `<- LATCH`, `<- POOL LEAK` | A covered epoch with no payload in the installed publication (not submitted), an epoch with no shape or recording in the selected revision, values past their latch, and a pool that kept growing over the windows. |
| `[DCLF] the <epoch> epoch of frame N has no payload ... not submitted <- UNBUILT` | (warning) | The same, per epoch. |
| first person, forward and reflection lines | `<- EPOCH FAILED`, `<- NO DEPTH EPOCH`, `<- NO FORWARD RECORDING`, `<- NO REFRACTION DRAW`, `<- LATE FACES`, `<- FIRST-PERSON PARTICLE SPACE` | A view epoch that failed or had no depth epoch, a forward epoch without a recording, refraction members with nothing drawn, reflection faces captured late, and first-person particles placed more than 2000 units away. |
| `[DCLF] effects ...` | `<- EFFECT PIPELINE FAILED` | An effect pipeline that failed to build. |
| `[DCLF] tree LOD draws: ...` | `<- TREE LOD HOLES` | Tree LOD commits that could not draw a frame whose tree LOD was withheld. |
| `[DCLF] sun whole skip ...`, `[DCLF] point lights' empty culls ...` | `<- STAMPS` | Geometry given DCLF's light bits without a stamp. |
| `[DCLF] sun entry exclusion: ...` | `<- VETO` (warning) | Excluded entries left in the full-frustum arrays after the veto. |
| `[DCLF] engine-read window: ...` | `<- LANE ENGINE ACCESS` | Engine memory read by the scene lane without a lease. Must be absent. |
| `[DCLF] release guard (T6b3e, CS_DCLF_RELEASE_GUARD): ...` | `<- LANE ENGINE RELEASE` | An engine object's last release on a DCLF thread. |
| `[DCLF] scene pump (T6b3d): ...`, `[DCLF] scene pump wakes by source ...`, `[DCLF] event to adoption ...` | | Passes a frame, passes that changed nothing, publications, and the delay from an event to the frame that installs it. |
| `[DCLF] [CPU] N frames, record ..., batches of N lanes (the setting); a frame's cull (pool) by view: ...; record (DXVK's CS thread) by site: ...` | `<- <site> DREW NOTHING`, `<- NOT QUEUED`, `<- UPLOADS NOT TAKEN` | The CPU pipeline's window (only while CPU frames ran): per view a frame's cull time, batches and draws; per site the callback's record time and how many drew nothing; secondaries, the callbacks' waits for a batch, the frame slots' waits. A site whose callbacks all drew nothing is the CPU path's "sequences 0". `NOT QUEUED`: DXVK took no external commands. |

### Accepted differences

These are known, measured and left as they are. [dclf-open-defects.md](./dclf-open-defects.md), "Accepted differences", has
the measurement and what would remove each; this list says how each shows in the checks.

| Difference | How it shows |
| --- | --- |
| A root seen again is drawn for one frame with its old fade (`FadeStateCS` runs a frame ahead). Fade verdicts lag one frame by design. | One-frame draws of roots that snap out; no flag. |
| Shading written during a frame (a controller on the animation job, a cull's `GetRenderPasses`) is drawn from the next frame. | Capture parity's "shading changed since the frame's start sampled it" line. |
| Tree LOD instances the engine leaves hidden after `allVisible`: DCLF draws by the intended rule. | The tree LOD mirror line's `shown by allVisible without a refill`. |
| Reflection faces render early in the frame (`Main::Draw`, before the world), so they light with another moment's sun. | Face capture parity differs on PS PerGeometry `DirLightDirection` in about the fifth digit; everything else is equal. DCLF reads the sun direction from the frame lighting block (PS b13), not from a row. |
| Terrain Blending leaks write mode 1 into the blended decals after its hook; DCLF derives write mode 11. | An RT0 alpha difference the composite never reads. |
| `IsBeastRace` and Hair's ProjectedUV (see [Bugs found by parity](#bugs-found-by-parity)). | Not compared outside faces, and left out for hair. |
| Hint-10 passes DCLF does not model. | Capture parity's "native-only passes". |

## Diagnostics and probes

### The diagnostic capture

The player's report of a problem (`Diagnostics/Capture/`). The menu's "Create diagnostic capture" arms it, and it starts two seconds
after the menu closes, so the menu is in neither the screenshot nor the timings. `capture[=<seconds>]` (a test step) starts it at the next
Present. It records for the slider's time (10 s by default), then a worker writes `DCLF-Capture-<time>.zip` beside the log with
Windows' `tar.exe`. If the zip fails, the folder is kept. It runs with DCLF off, not installed or unavailable too. The parts that
need DCLF's scene work say "skipped" in the manifest.

How it reuses the reports:

- The log tee (`Utils/LogTee.h`) copies every line logged during the capture into `capture.log`.
- At the start and the end the capture calls `Reports::Flush`, which ends the report window on every clock (`Due`, `Crossing`,
  `Window`). So `capture.log` begins with the window before the capture (the time leading up to the click). Then comes one
  window that covers exactly the capture.
- While it records, `Reports::FullStatistics()` is true, so that window holds the full `CS_DCLF_STATS` report. Counters that are
  only kept under `CS_DCLF_STATS` (the frame data writes, the MainBuild part timings, the shadow cull counters) stay empty.
- The capture's telemetry phase replaces a running `telemetry` one.

The bundle:

| File | What it answers |
|---|---|
| `manifest.json` | The window's frames and times, and each section's state: ok, skipped or failed and why. |
| `environment.json` | Build, game runtime, GPU, DXVK or native, DCLF installed / on / faulted / unavailable and why, every switch set, observers left on (a `warning` when any is: they slow the normal path), requested and active toggles, the graph's pipeline and cell kind, uptime and CPU time. |
| `features.json` | Every CS feature (loaded, version, disabled) and each DCLF integration's status line. |
| `settings.json` | The whole CS settings (`State::SaveToJson`). |
| `loadorder.txt`, `location.json` | The plugins in order; the player's cell, worldspace and position (read on the main thread). |
| `modules.txt` | Every loaded DLL with its file version. |
| `start/`, `end/` | `snapshot.txt` (the menu's "This frame" lines, the scene executor's queues and `ParallelFor` waits, the integrations, the render-thread growth census), `fades.json` (the fade census), `scene-growth.txt` (the scene lane's growth census) and the VRAM attribution JSON. |
| `frames.csv` | One row per Present: Present-to-Present ms, DCLF's render-thread µs in the last finished frame, the wait for a scene snapshot, frames since the last scene pass, epochs not ready, the executor's queues by domain, loading, running, CPU pipeline, interior. |
| `telemetry/` | The BasicTelemetry phase `capture`: frames, scopes (the render graph's and DCLF's own zones), stalls of 2 ms or more, and `capture.threads.csv`, each DCLF zone's count, total and longest time on each thread, by name. `top_level_ns` adds up to a thread's DCLF time; the log line `DCLF zone time by thread` lists the busiest threads. |
| `capture.log` | Every line logged during the capture, with the two flushed report windows. |
| `compat.txt` | The compatibility monitor's report, when it is on. |
| `screenshot.*` | The first frame of the window. |
| `logs/` | `CommunityShaders.log` (its first 1 MB and last 32 MB), the DXVK logs and device fault dump, `CommunityShaders-DCLF.ini`, `CommunityShaders.env`, and the [hang reports](#hangs) (`.txt` only). |

The text files have the user's profile path replaced with `%USERPROFILE%`.

Reading a capture:

- **Slow frames.** `telemetry/capture.threads.csv` says which thread spends DCLF's time and in which zone. In `frames.csv`, compare `present_ms` with `dclf_render_thread_us`. A long Present with
  little DCLF time is not DCLF's render thread. A growing `scene_lag_frames` or queues that stay full mean the scene work falls
  behind. In `environment.json`, check `graph.cpuPipeline`, any `observers` and `warning`. Then compare the two growth censuses for
  containers that grew during the window. `telemetry/capture.stalls.csv` names the scopes that were off the CPU.
- **See-through actors.** `end/fades.json` lists every root whose fade is below 1, or whose node, mirror and row disagree. Each
  row has the actor's form ID and name, the three values, the root's bits and generation, the frame of DCLF's last store, and
  `lastWriter` (the path that last wrote its fade). `totals.refreshNoMirror` counts the fade refreshes dropped because the mirror
  had no record.

### The periodic report

`CS_DCLF_STATS=1` logs DCLF's full report every 300 frames into `CommunityShaders.log`. Its always-on lines (the culling, the leaks,
the scene pump and others) are logged every 300 frames while any parity or diagnostic switch is set, and every 18,000 frames in
normal play (`Reports::Interval`). Each window's averages cover that window's own frames. Lines start `[DCLF]`; the render graph's start
`[ORG]`. The [lines in every report](#lines-in-every-report) say which ones show that DCLF draws at all. The report also covers
the scene pump, the set and its leaves by cause, the tables, the pooled payload, the frame values, the shadow views and each
feature's own lines (`Api::ReportPoint`, registered by `RegisterReportLines`). `CS_DCLF_PASS_STATS` and `CS_DCLF_PROFILE` add
per-pass counts and scene-phase timings.

### The layout check

`python tools/dclf-layout-check.py` checks the rules in [dclf-layout.md](./dclf-layout.md), "The rules" (include directions, the
dependency guard in one place, the closed top level, passes through the schedule's executor alone, features owning no part of the
renderer), and baselines for function complexity (lizard, when installed). It exits 1 on a broken rule or an exceeded baseline.
`--update` rewrites the baselines (`tools/dclf-layout-baseline.json`) after a step that shrank them.

### The release guard

`CS_DCLF_RELEASE_GUARD` (`Diagnostics/Engine/ReleaseGuard.cpp`): engine objects' last releases on DCLF's threads, by class. An
engine destructor run off the main thread can run Havok teardown and crash.

### GPU timing

-   DCLF's epochs replay kept recordings, which record no ORG pass timestamps, so ORG's GPU timing report is empty for them.
    Compare whole frames (`[Perf] rendered N fps (N ms)` in the log) and Tracy's CPU zones.
-   `CS_GPU_EVENT_TIMERS=<frames>` times the engine's events; select them by exact name.
-   Nsight Perf's range profiler drains around each range, so many small native ranges each pay a drain and DCLF's few large ones
    look cheaper than they are. Compare whole frames first ([nsight-perf.md](./nsight-perf.md)).
-   Tracy zone exports are large (gigabytes a run). Export them only when needed.

## How to validate a change

### The validation run

One run of about 60 seconds validates a step:
-   The camera moves and turns, so cells load and LOD swaps happen: `move` and `turn` steps (units and degrees per frame). A still camera hides alpha-test holes and most culling cases; rotation alone reaches
    few cases.
-   `CS_DCLF_PARITY=1` with `CS_DCLF_STATS=1`.
-   The full featureset: the log shows `[DCLF] featureset: full`, and no `UNKNOWN` switch.
-   Clean: every flag `<- OK`, or listed in [dclf-open-defects.md](./dclf-open-defects.md); `<- LEAK` 0; no member drawn by the
    engine; no validation messages about generated commands when the layer is on.

Add capture parity when the change touches shading.

### Before trusting parity or performance: DCLF draws

A run in which DCLF draws nothing can look clean and fast: every store's parity can say "0 differ" while the members are
withheld and drawn by nobody. Before reading parity or timing, check in each run's log:
-   `[DCLF] persistent draws (last frame, colour)`: sequences in the thousands in a populated cell, not 0;
-   `[DCLF] indirect draws (last frame) ... skipped:`: no `constants` or `texture` skips, and no `pairs read constant registers no
    frame slot supplies` warning;
-   set parity's `in the set and drawn by nobody`: 0.

### Timing runs

-   **Measure from frame 3000 on.** DCLF's SPIR-V programs and pipelines compile in the first frames of gameplay (Community Shaders'
    own shaders compile before the load). No long warm-up is needed beyond that.
-   **The game window must have focus.** Community Shaders skips its upscaler while another window has the foreground
    (`Upscaling::IsWindowUnusable`), which makes a run 1.5-2 ms faster at 4K. Compare only runs whose log has
    `[Streamline] DLSS evaluate`.
-   **Nothing else heavy runs.** Builds or other heavy processes on the machine inflated every Tracy zone by about 1.6x. Background capture (Discord Clips at 30 fps) adds 1-1.5 ms to one frame every 33 ms.
-   **No validation layer.** Check that no Vulkan layer is forced on before any performance conclusion: the Vulkan Configurator
    can force `VK_LAYER_KHRONOS_validation` on for every application through
    `%LOCALAPPDATA%\LunarG\vulkan\vk_loader_settings.json`, and `VK_LOADER_LAYERS_DISABLE` does not override it. With it, DXVK's
    command-stream thread saturates and native looked 3x slower. Confirm that `VkLayer_khronos_validation.dll` is not loaded in
    the game process.
-   **No parity switches** in a timing run, and the full featureset.
-   Compare alternating runs, never single runs: two runs of one build differed by more than a step's gain.
-   **Sequence the run in game, never from outside.** Every step goes in the cs-test config; a script that watches the log
    to swap files or call DevBench races the frames (a reload asked 10 frames after a file swap ran before the swap landed).
    Steps that help:
    -   A step's `wait` holds it until a condition holds: `reload` (the last `org:reload-shaders` finished and its rebuilt
        pipelines swapped in; one takes ~20 s), `settled` (the frames draw under the requested toggles on the pipeline they
        choose; at once with the feature off), `nvperf` (the last NvPerf capture done), `signal:<name>` (a remote client's
        `signal=<name>`) and `shaders` (Community Shaders' compile queue is empty: a run that compiles on demand, as
        `CS_DISABLE_FEATURES` makes one, has every shader in view built).
    -   `capture[=<seconds>]` takes a [diagnostic capture](#the-diagnostic-capture) at the next Present (no Explorer window, nothing on the clipboard).
    -   `graph-trace` writes every attached epoch's schedule at the next frame (passes in order, their barriers with each
        resource's state before and after, and cross-queue waits) to `<log directory>/graph-trace-<frame>/epoch-<n>.txt`, to see
        which pass writes what another epoch reads. `CS_ORG_GRAPH_RESOURCES_FRAME=<n>` does the same at frame n.
        `configs/river-trace.toml` takes one just before its first shot.
    -   `telemetry=<label>` starts a BasicTelemetry session for a phase, and `telemetry=off` ends the last. Each writes per-Present frame times, scope totals, off-CPU stalls and DCLF's zones by thread to `<log directory>/dclf-telemetry`, and a frame time summary and each thread's DCLF zone time to the log. DCLF's zones are `DCLF_ZONE` / `DCLF_ZONE_NAMED` (`Diagnostics/Zones.h`), which open a Tracy zone and, during a phase only, a BasicTelemetry scope and a per-thread tally, so they record in builds without Tracy too. Use them in place of `ZoneScopedN` / `ZoneNamedN` under the tree.
    -   `mark=<label>` marks a phase's boundary in the log; `org:gpu-window=<label>` makes the GPU event timers
        (`CS_GPU_EVENT_TIMERS`, with an interval too large to reach) log the frames from there to the next window as one block,
        frames in flight included ("[GpuEvents] window '<label>'"; `-` is not logged).
    -   `org:nvperf=<range>|<range>...[#<samples>]` starts an NvPerf capture at that frame.

    DevBench's `communityshaders.dclf` queues one such command line (`<command>[=<arg>]`, or a console command) for the next
    Present.

### The Vulkan validation layer

Through Mod Organizer 2, `VK_INSTANCE_LAYERS` alone loads no layer; `VK_LOADER_LAYERS_ENABLE=*validation` does. The messages do
not reach `CommunityShaders.log`: write them to a file with `VK_LAYER_DEBUG_ACTION=VK_DBG_LAYER_ACTION_LOG_MSG` and
`VK_LAYER_LOG_FILENAME=<path>`. Turn on extra validation with the `VK_LAYER_*` setting names (`VK_LAYER_VALIDATE_SYNC=1`,
`VK_LAYER_GPUAV_ENABLE=1`).

-   GPU-assisted validation disables itself on DCLF's indirect-bindable pipelines ("Unable to instrument shader using
    VkIndirectExecutionSetEXT"), and synchronization validation does not see descriptor-heap accesses. Neither checks DCLF's
    passes.
-   Messages that are not DCLF's and appear in every run: `VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-03328`,
    `VUID-vkCreatePrivateDataSlot-privateData-04564`, `VUID-VkImageMemoryBarrier-newLayout-01198` (Streamline's barrier to an
    undefined layout at startup), `VUID-vkQueueSubmit2-semaphore-03868` (swapchain semaphore reuse) and
    `VUID-vkAcquireNextImageKHR-semaphore-01779`.
-   DCLF's own open validation errors are in [dclf-open-defects.md](./dclf-open-defects.md).

### The CPU-driven pipeline

A/B it within one launch ([Timing runs](#timing-runs)): steps switch `dclf:pipeline=cpu` and back, then
`dclf:cpu-record=serial`, each at its own frame. A pipeline switch rebuilds the graph at the next build point,
so expect one frame-start wait line per switch. Frame time is the `[Perf]` line's; the `[CPU]` line gives the cull time on the pool
and the record time on DXVK's CS thread, which the CPU pipeline moves the GPU's cost to, and the render-thread budget line its
render-thread share. Before trusting a CPU run, check the `[CPU]` line: every site with callbacks (shadow, depth, colour,
forward, first person when it is on) and none tagged `DREW NOTHING`, and each view type the scene has (sun or point, sky,
reflection with water in view) with batches and draws. Run the Vulkan validation layer once per change to the sites (the external commands' barriers
and state); with the shadow site on it loses the device (dclf-architecture.md, "The CPU-driven pipeline", Open).

### Other runs

-   **Capture parity:** `CS_DCLF_CAPTURE_PARITY=1 CS_DCLF_PARITY_BOTH=1`. It double-draws, so it is never a timing run.
-   **Cell-load stress:** a flight across cells (a `move` step), or `console` steps with
    `coc` (for example `coc WhiterunDragonsreach`, then `coc Riverwood`).
-   **Graph rebuild:** an `org:rebuild` step. Expect one frame-start wait line, no withdrawal, and sequences
    above 0 after it.
-   **Growth:** `CS_DCLF_TABLE_START=small` makes every growable table grow early and often.
-   **Weather and feature paths:** a `console` step `fw 10a241` makes it rain (the precipitation mask);
    `player.damageav stamina 1000` exercises Advanced Skin's wetness; `pov=first` and `draw=1` steps exercise first person;
    `player.sendanimevent attackStart` with spawned enemies gives blood decals.
-   **Places** that cover the classes: Riverwood (waterfall foam, mist, the river, bridge spray), an inn with candles and a fire
    (refraction, particles, light cards), Whiterun and Dragonsreach (an interior with portals), a dungeon, and save and load.
-   **An intermittent bisect** re-runs the control at the end as well as at the start: a GPU fault from an earlier crash can
    persist.

## Crash triage

### CPU crashes

-   **The crash logger's logs** are `Documents\My Games\Skyrim Special Edition\SKSE\crash-*.log`. They give addresses as
    `SkyrimSE+offset` with the Address Library ID; the code names engine functions by their AE 1.6.1170 address
    (`FUN_14xxxxxxx`) and `RELOCATION_ID`. Decompile the faulting function before attributing a crash: the logger's labels can
    be wrong (it labels by the nearest symbol), and a probable frame is not the same as a stack-scan hit.
-   **Community Shaders' minidump**, `CommunityShaders.dmp` beside the game executable, is written from outside the game by
    `tools/crash-dump-helper` (`CommunityShadersCrashDump.exe`). The plugin starts it at load (`CrashDump.cpp`,
    `CrashDump::Start`). The dump is for the exception that ends the process: an unhandled exception filter (chained to the
    one installed before it) signals the helper and logs `[CRASH] unhandled exception <code> at ... (<module>+<offset>) ...
    Writing CommunityShaders.dmp`. A vectored handler logs the session's first 16 faults at first chance (`[CRASH]
    first-chance exception ...; a handler may still catch it`) without a dump: a handler may still catch them, and the
    game goes on. A crash logger installed after Community Shaders that does not chain its filter replaces this one, and
    then there is no dump. `MiniDumpWriteDump` run inside the game deadlocked on the heap lock. Read the dump with cdb or
    WinDbg. `CS_TEST_CRASH=<seconds>` faults a worker thread that long after load, to test the path end to end; it is
    read before the dev environment file is applied, so set it in the process environment.
-   **Heap corruption** shows its fault later and elsewhere. Typical DCLF causes are an engine object read through a raw pointer
    nothing held, and a last release on the wrong thread. `CS_DCLF_RELEASE_GUARD` names the thread.

### Hangs

-   **The render thread watchdog** (`HangWatchdog.cpp`) writes a report when no Present has happened for 15 s
    (`CS_HANG_WATCHDOG=<seconds>`, 0 turns it off). It starts at data load and only counts while the game has the foreground,
    or once it has seen the game present without it (`bAlwaysActive`, test runs on another monitor). It does nothing while a
    debugger is attached. The crash dump helper writes the report from outside the game: `CommunityShaders-Hang-<time>.txt`
    beside the log, with every thread's stack and the stuck thread first (symbols from the PDBs beside the modules), and a
    minidump `CommunityShaders-Hang-<time>.dmp` for cdb or WinDbg. The log has `[HANG]` lines: the stall, the report written,
    one line a minute while it lasts, and the recovery if it ends. There is one report per stall and at most three a session,
    and the six newest are kept. The [diagnostic capture](#the-diagnostic-capture) copies the reports into `logs/`.
-   **The stuck thread is the last one that presented.** A loading screen presents from one of the engine's job threads, the
    game from `Skyrim Main`.
-   **`CS_TEST_HANG=<seconds>`** makes the render thread sleep past the threshold once, that many seconds after the watch
    starts, to check the watchdog and the helper end to end. Both switches are read from the environment (`CommunityShaders.env`
    beside the log), not from `CommunityShaders-DCLF.ini`.

### GPU crashes

-   **Aftermath dumps** are armed whenever Community Shaders is built with `AFTERMATH=ON` (the default; it needs the Nsight
    Aftermath SDK). A GPU fault writes `gpu-crash-<stamp>.nv-gpudmp`, plus `gpu-crash-<stamp>-shader-<id>.nvdbg` shader debug
    info, into the SKSE log directory, and logs `[Aftermath] GPU crash dump written to ...`.
-   **Fault details need a diagnostic configuration.** With `DXVK_DEBUG=hang` DXVK records checkpoints and debug-utils labels
    for its work and ORG's, and Community Shaders turns its crash markers on (`DxvkLoader.cpp`). It serialises command recording, so it is a diagnostic configuration, not a
    default. Without it a dump has no fault details or markers. The fault address, resources and warp PCs also need
    `DXVK_AFTERMATH_RESOURCE_TRACKING=1`, `DXVK_AFTERMATH_SHADER_DEBUG_INFO=1` and `DXVK_AFTERMATH_SHADER_ERROR_REPORTING=1`
    (`extern/dxvk/src/dxvk/dxvk_adapter.cpp`).
-   **Decoding.** `tools/aftermath-decode` builds `aftermath-decode <dump.nv-gpudmp> [--debug-info <dir>] [--spirv <dir>]
    [--json <out.json>]`. It prints the device status, the page fault (address, access, client, resources), the active shaders and
    the last markers per context, and writes the SDK's full JSON (by default `<dump>.json`). `--spirv` names ORGModuleServices'
    shader cache (`Data/ShaderCache/v3`, in MO2's overwrite folder), so the faulting warps map to SPIR-V, and to HLSL lines with
    `CS_DCLF_SHADER_DEBUG` on (the default). Without shader debug info it cannot name the shader.
-   **DXVK's own output.** Community Shaders points `DXVK_LOG_PATH` at the SKSE log directory, so `SkyrimSE_d3d11.log`,
    `SkyrimSE_dxgi.log` and, on a device loss with `VK_EXT_device_fault`, `SkyrimSE_device_fault.bin` land beside
    `CommunityShaders.log`.

### Reproducing and bisecting

-   **Startup loops** reproduce faults in the first DCLF frames: a cs-test config with an `exit` step soon after `world-ready`,
    the game relaunched at once.
-   **Bisect by arms and rates.** Run each arm until its first loss and compare counts; an intermittent fault needs many runs per
    arm before an arm is cleared. Re-run the control at the end.
-   **Reduce the configuration** with `CS_DCLF_PIPELINE`, to tell
    DCLF's work from the render graph's submission paths.
-   Open crashes are in [dclf-open-defects.md](./dclf-open-defects.md), "Open crashes".

Not verified here: the crash logger's exact log format (it is a separate SKSE plugin), and that a release build of
`tools/aftermath-decode` sits at a fixed path (it builds with CMake from `tools/aftermath-decode/CMakeLists.txt`).

## Bugs found by parity

Bugs DCLF's parity checks found in other Community Shaders features, in the engine, and in the libraries DCLF runs on.
-   **Feature bugs** are each their own fix, in that feature. DCLF imitates none of them: it binds and computes what the feature
    intends, and the parity checks report the difference. An engine or feature shader that depends on the previous draw's
    bindings is a bug to fix there, not an order for DCLF to reproduce: DCLF registers and draws in its own order.
-   **Engine bugs** cannot be fixed at the source. Where the native result is undefined, DCLF picks a deterministic rule, stated in
    the entry, and the parity checks compare against that rule.

### In other Community Shaders features

**TruePBR: a loader thread writes MATO data into materials that are being drawn.** Still present.
-   Where: `TESBoundObject_Clone3D` in `src/TruePBR.cpp` (`TESObjectSTAT`'s vtable, slot 0x4A) applies a static's material object
    to every geometry of the freshly cloned model with `ApplyMaterialObjectData`.
-   The fork-before-write forks only when the material already has a different owner with a different MATO
    (`prevOwnerRefID != 0 && prevOwnerRefID != ref && materialObjectData != pbrData`). A material nobody has claimed has owner 0,
    so it is written in place, and every reference sharing it picks up this reference's values. The write runs on a loader thread
    in the middle of a frame.
-   Evidence: capture parity's `PS PerMaterial 22` (`ParallaxOccData`, filled from the projected material's roughness and specular
    level) differed on terrain cliffs that shared a material with a newly loaded reference: DCLF 1, native 0.7.
-   Proposed fix: fork whenever the material is not exclusively this reference's (its reference count, or
    `BSLightingShaderMaterialPBR::All`). A fresh copy is drawn by nothing until the clone is attached, so writing it from the
    loader thread is then safe.
-   DCLF's side: nothing to change. `ApplyMaterialObjectData` reports the write (`NoteWritten`) and DCLF corrects the record on the
    next frame. Capture parity reports a remaining case as `mismatched materials: ... written after its last mismatch`.

**Subsurface Scattering: `IsBeastRace` stays set from the previous face draw.** Still present.
-   Where: `SubsurfaceScattering::BSLightingShader_SetupSkin` in `src/Features/SubsurfaceScattering.cpp` sets or clears
    `ExtraShaderDescriptors::IsBeastRace` only for properties with `kFace` or `kFaceGenRGBTint`; every other draw inherits the
    last value. Lighting.hlsl writes it into `Masks.y` for the `SSS` + `SKIN` permutation, and `SeparableSSSCS.hlsl` picks the human
    or beast profile by it. So a body skin draw gets the last face's profile. The actor comes from `geometry->GetUserData()` and
    the bit defaults to beast when that is not an `Actor`.
-   Proposed fix: decide the bit for every Lighting draw in the deferred pass: skin by the owning actor's race (walking up to the
    reference), everything else cleared.
-   DCLF's rule: the bit is per object, decided as SSS decides it today, for faces only
    (`Integrations/SubsurfaceScattering`: `kObjectBeastRace`, `kRecordBeastRace`). Capture parity compares `IsBeastRace` on face
    techniques only (`CaptureParity::ComparePermutation`). Once SSS decides it per draw, DCLF needs the per-object bit for skin
    objects too.

**Advanced Skin: the RFAOS and wetness textures are never loaded.** Fixed.
-   Was: `Skin::SetupExtraTexture` derived `_rfaos.dds` and `_wet.dds` paths but bound the default white texture, and Lighting.hlsl
    treated a texture of 32 texels or less as absent, so t71 and t74 were never used.
-   Fix: Skin's materials name their textures (`src/Features/Skin/Material/Material.h`), and `Skin::ResolveGeometryMaterial`
    (`src/Features/Skin/Runtime/Renderer.cpp`) gives their views and `materialFlags` (t71 bound, t74 bound), which Lighting.hlsl
    reads as `hasSkinExtra` and `hasSkinWetness`.
-   DCLF's rule: bind what Skin's own resolve gives the material (`Integrations/AdvancedSkin/SkinMaterials.cpp`, through
    `ResolveGeometryMaterial`). Capture parity's `skin parity` line compares t71, t74 and t72.

**Wetness Effects: the shore term darkens menu and loading-screen models in D3D11.** Still present. Left as it is for now
(decided 2026-11-18).
-   Where: the shore wetness in `package/Shaders/Lighting.hlsl` (`shoreFactor` from `wetnessDistToWater`, then `shoreWetness` and
    `wetnessGlossinessAlbedo`). It reads the world's water table at the draw's coordinates and is not gated by `inWorld`, so a
    menu scene's model, which is not in the world, is treated as standing on a shore. The rain term beside it is gated
    (`wetnessOcclusion` is 0 outside the world).
-   Evidence: the loading-screen model's mean luminance at the same shot point in plain D3D11 is 16.2 with Wetness Effects on and
    21.1 with it off. The pale war paint on loading-screen actors is the same effect.
-   Found by the loading screen port's comparison against D3D11 (org-native-frame-graph.md, "Loading screens"). The ORG build has
    no wetness on these draws, so the two builds differ there. Model area mean in that comparison: ORG 14.1 to 14.3, D3D11 15.2
    with Wetness Effects on.
-   Proposed fix: gate the shore term by `inWorld`, as the rain term is. The D3D11 build then matches the ORG build on menu models.
-   The ORG build's rule: no wetness on models outside the world. The difference against D3D11 is accepted until the fix.

**ShaderCache: the shadow masks' filter selector 3 is unsupported, so every shadow mask builds SHADOWFILTER 0.** Still present.
-   Where: `ShaderCache.cpp`'s Utility defines take the shadow mask filter from the descriptor's bits 17-20 and accept 0, 1, 2, 4
    and 8. The engine's sun, spot and point light masks carry 3 there (bits 17 and 18: descriptors `0x262102`, `0x462102`,
    `0x1062002`, `0x462002`), so each build logs `Unsupported Utility shadow-filter selector 0x3` and falls back to SHADOWFILTER 0:
    one plain sample of t3 and a hand compare, with no PCF. Both the D3D11 and the ORG builds take the same defines.
-   Unverified: what the engine means by 3. The table treats the field as one bit per filter; the field may be a number, and 3 a
    filter the table maps to nothing. The vanilla shader's own handling (the engine's Utility PS for these descriptors) is the
    reference to read before choosing a fix.
-   Found by the shadow mask port's parity (engine/shadow-mask.md, "The shadow map at t3 and t4"): the ORG build read t3 as the engine
    latched it, a null view, and every sun mask came out black. The ORG build now reads the shadow map through t4's index, so it
    matches the D3D11 build whatever the fix here turns out to be.

**Terrain Blending leaks write mode 1 into the blended decals that follow its hook.** The difference is on RT0 alpha only, which
the composite never reads. DCLF's rule: derive write mode 11 for those decals; the difference is accepted.

**TruePBR swaps a land quad's shader property after the land's setup returns.** Fixed.
-   Where: `PBR_TESObjectLAND_SetupMaterial` (`src/TruePBR.cpp`) hooks the call of `TESObjectLAND::SetupMaterial` (`0x1402ad800`),
    outside SceneWriterFramework's detour of the function, and after it replaces each PBR quad's property with a
    `BSLightingShaderMaterialPBRLandscape` one. SWF's event had fired inside, so the mirror kept the engine's property.
-   Evidence: mirror parity missed 144-324 `geometry property` changes a window on `Block (3, 0)` terrain quads, with as many
    properties absent from the mirror and 100 `<- UNPINNED PROPERTY` a window; `CS_DCLF_MIRROR_WATCH=geometry:Block (3, 0)` named
    `TruePBR::TESObjectLAND_SetupMaterial`. The decal setup's property and alpha flags writes are the same kind.
-   Fix: CS's own writes to objects in the scene go through SWF's `Write` (`DrawcallLimitFix::WriteShaderProperty`, `WriteAlphaFlags`,
    `WriteHidden`: `SWF_EVENT_SCENE_WRITE` around the store), as do Unified Water's and Wetness Effects' `SetAppCulled` (inline
    hidden stores no SWF site covers). Afterwards: 0 property misses, 0 absent, 0 unpinned.

**Community Shaders swaps the blend table** for its deferred variants between `StartDeferred` and `ResetBlendStates`. Not a bug,
but a trap: DCLF captures the engine's state objects inside the deferred pass (`CaptureEngineStates`), and opaque pipelines take
their write masks from the engine's state for their write mode (`kOpaqueWriteMode`), never a default blend state.

**Physical Sky: the deferred composite's aerial perspective has no depth without Dynamic Cubemaps.** Possible bug, found by
the ORG Physical Sky port, not by a parity check.
-   Where: `Deferred.cpp` binds the composite's t4 `DepthTexture` only when Dynamic Cubemaps is loaded (upstream does the same,
    `a03587448`, `Deferred.cpp:386`). `DeferredCompositeCS.hlsl` applies the aerial perspective only for `depth < 1`, so with t4
    unbound it reads 0 and adds nothing.
-   It may be intended: only the cubemap path may need that view, with the aerial perspective covered elsewhere. Not confirmed.
-   The ORG port keeps upstream's behaviour: its aerial perspective reads depth only when Dynamic Cubemaps is loaded.

**Physical Lens and Camera FX: generated frames do not follow lens effects.** Accepted, found while planning native frame
generation, not by a parity check.
-   Where: the hudless image frame generation reads holds Physical Lens's image mapping (distortion, chromatic split), ghosts and
    starburst, and Camera FX's distortion and grain. The engine's motion vectors describe the undistorted scene, and ghosts move
    against their light sources, not with the geometry. Generated frames therefore misplace these effects. Guest has the same
    mismatch today.
-   Decided (2026-11-19): accept it for the first version (`native-frame-generation-plan.md`, decision 8). If distortion shows,
    the inputs pass can warp the motion vectors by the lens's image mapping.

### In the engine

| Bug | DCLF's rule |
| --- | --- |
| Hair with ProjectedUV shades another object's snow: `BSLightingShader::SetupGeometry` (`0x1414dd040`) writes `TextureProj` (VS PerGeometry 6) and `ProjectedUVParams` 1-3 (PS PerGeometry 12-14) for every technique but Hair (6), while hair passes can carry bit 15. The draw reads the previous projected draw's constants. | Hair draws without the projection: `DrawnPassDescriptor` drops bit 15 from a Hair pass (`HairProjection`, `Core/Shading/LightingDescriptors.h`). Capture parity leaves that bit out for such passes. |
| `BSOcclusionPlane::WithinFrustumDistFirst` (`0x140e23e10`) tests the camera's foot point against the plane's rectangle along rows 1 and 2 of its rotation, while the corners (`BSPortal::sub`) lie along columns 0 and 2: an axis-aligned plane's rectangle is an infinite horizontal band. After a failed frustum test the walk then takes a plane wholly outside the view, with the stale edge flags of an earlier test; with all four clipped, its occluder is the infinite plane, and everything behind it is culled. The engine's frames rarely show it: the actor culler (`FUN_1414a8ba0`) judges the graph's planes first, with no rescue, and the walk skips those it rejected. Found 2026-10-23 in Whiterun (SurWR): two planes 8 and 26 units beside the camera hid the wall, the floor and about 5,000 objects a frame; set parity's in-view rejections and portal parity's "matching with the engine's occluder states" counted it without flagging it. | Judge every graph plane at the walk's start as the actor culler does (`PortalViews::PlaneJudged`: the corners against the camera-relative frustum, a one-sided plane only from behind), and a start room's untested planes the same way. Portal parity rebuilds the engine's program with its states and its walk's test before counting a difference. |
| Glacier and ice snow layers read SparkleParams past their material's end: vanilla `Landscape\Ice\*.nif` (IcebergLedge01, GlacierRubbleTrim01/02, GlacierSlab01, GlacierEndLg01, GlacierStraightLg01, IcePileL01) are BSMultiIndexTriShapes whose snow layer property (kMultiIndexSnow and kProjectedUV: MultiIndexSparkle, technique 0xe) carries a plain `BSLightingShaderMaterial` (NIF shader type 0; the loader, SE `FUN_1412cfbc0` / AE `0x1414b7f70`, makes the 0xb0-byte Snow class only for types 10 and 0xe). `SetupMaterial` (AE `0x1414dc310`) reads SparkleParams at +0xa0..+0xac on any class, so it takes the next heap object's bytes (pointers, reference counts, once a float moving every frame); the sparkle test (`z * sparkleColor >= 0.5`) is off for nearly every value seen, so the engine draws them without sparkle, by accident, on both runtimes. Found 2026-10-23 by the material port's `<- MATERIAL PORT` on an SE route past the glaciers. | Toggle `plain snow sparkle` (`CS_DCLF_PLAIN_SNOW_SPARKLE`, default on): a plain material at technique 0xe gets the Snow class's constructor default (1, 1, 1, 1: the sparkle intended); off, zeros (the engine's look without the bug). `MaterialPortVanilla`, technique 0xe. |
| `bhkWorld::InitHavok` (`0x140e86ad0`, `Hazard::InitHavok`'s: a projectile or a quiver) moves a collision object between nodes: `SetCollisionObject` on the new one, the old one's `+0x40` cleared inline (`0x140e86fb7`, `0x140e870c6`). | SWF's `SWF_HOOK_HAVOK_INIT`; both nodes' bodies after the call. Mirror parity's 7-12 missed `node body` a window (`ArrowQuiver`, the soul gem) went to 0. |
| A flora's harvested look (`FUN_1401e95a0`, from the harvest `FUN_1401e8ef0` and the harvestable 3D's setup `FUN_1401e9450`) swaps each alpha's threshold (+0x32) inline between the unharvested and harvested values (128 -> 160), recursing over the 3D's geometries. | SWF's `SWF_HOOK_FLORA_HARVEST_ALPHAS`; each geometry's alpha threshold after the call. Missed `alpha threshold` (35-60 a window) went to 0; `alphas:` named the writer. The flowers still drew unharvested until a rebuild: an alpha update reached only the mirror, and the join's binding check (`MemberBindingStands`, the derivation's witnesses) did not cover the alpha. Now each alpha writer also pushes a property event on the alpha's key on a change (its dependents joined again), and the witnesses hold `LeafView::AlphaState` (flags and threshold). Harvests show at once (in game, 2026-10-23). |
| Papyrus `Disable` with a fade (`FUN_140a3db90`) sets the 3D's LOD type to 9 inline before queueing the disable; the script command `Enable` (`0x14034c9f0`) resets a type of 9 to the model's. Critter scripts disable themselves with a fade. | SWF's `SWF_HOOK_REFERENCE_DISABLE` and `SWF_HOOK_ENABLE_COMMAND`; the fade node's LOD type after each. Missed `node fade type` (critters, 1 -> 9) went to 0. |
| The main registration reads `activeLightMask` and then clears it (`+0x160 == 0xFFFF`). | Write sun and point-light bits only until a geometry's first clearing registration of the frame (`FrameAddressSet`); keep owned members' masks 0. |
| `SetupMask` culls without `cameraRelatedUpdates`, so faded-out roots and new cells' roots are missing from the engine's occlusion maps. | Apply the same rule (`kCullFadeOnVisible`). |
| Utility's cull mode is per pass (0 for two-sided), so the state left at `FinishAccumulating` is the last pass's. | A shadow or occlusion view's raster state is its table entry with back-face culling; two-sided casters draw unculled by key. |
| The engine alternates a shadow viewport's max depth (1, 0.99997, 0.999985). | The depth range is a value in the view block (`DCLFDepthRange`), not part of the shape. |
| `RenderShadowmap` binds the target only for a descriptor that clears. | A point light's hemisphere 1 draws into hemisphere 0's slice (`PredictTargets`). |
| Indoors the sun does not draw (`0x14338c911`), so no full-frustum cull runs. | Clear the sun's frame state on such frames (`EndFullFrustumWindow`); do not require the sun's exclusion when the sun is off. |
| Main::Draw's light-mask clear (`FUN_1414cb640`) runs only while the player's third-person 3D is drawn. | Key the frame's lights on the selection, not on the clear. |
| The engine stamps actors' animation frame only from depth and shadow registrations. | DCLF stamps it from its own shadow views (`UpdateActorRate`). |
| `SetupGeometry` case 0xc advances a tree node's previous timer; model `+0xB0` is never read for trees outside the near list and can hold garbage (2.29e36). | Never evaluate tree setup per tree: the GPU's tree wind does the write's job. Animate, and scale the gust of, only a tree whose model has bones and that `FUN_14147d640` selects. |
| A tree's near-list test reads a cull result (`FUN_14147d640`); a fade root sampled during the frame is a step ahead (its cull updated the node). | Tree seeds (`TreeStaticOfNode`) and fade seeds are read on the render thread at `Kick`, before the culls. |
| The engine's fast square root shifts arithmetically, and some tree-animated ferns hold an uninitialised negative squared distance. | `FastSqrt` matches the engine's shift. |
| The engine's empty fade node (a static `BSFadeNode` at `0x14332a2a0`, read as null by engine readers). | Treat it as no node everywhere a fade node is read (`Engine::EmptyFadeNode`). |
| `BSMultiBoundNode::OnVisible` reuses a cached cull result for modes 0 and 2 while `lastAccumTime` matches. | Restore any value DCLF writes into a multibound for the tree loop right after the loop. |
| The actor pre-cull (`BSFadeNodeCuller`) sets bit 12 and, off screen, `kNotVisible` every frame; Process1 passes a preprocessed node unless `kNotVisible`, whatever its room. | `FadeStateCS` makes the pre-cull's test itself (frustum only, no room program). |
| The animation job runs fade updates and controllers beside the render thread; the engine writes inside the read window (`currentFade`, billboards, texture transforms, Havok transforms). The "Cell animations" job ("Main render scene" list) updates the animated references once the shadow cull is done, inside Main::Draw, while FrameValues samples them: texture transforms parity's falloff and base colour differed in most SE windows. | `FadeStateCS` models the animated fade updates (`AnimatedFade`); per-frame values are sampled by FrameValues, structural ones come from events. Parity drains the controller queues again after its compare and treats late events as queued, not missed. The cell animations update waits for the frame's sampling and runs with the window suspended (`WindowWriters`): 0 texture transforms differences after. The "Animated cell object update" job (Havok's node transforms) does the same, from its queuing call to the end of its wait. |
| 11 inline stores reset `currentFade` outside any hooked function (cell and sky placement). | All patched (SceneWriterFramework's `SWF_EVENT_FADE_RESET`): the mirror must see every write. |
| A property's fade node is set after its geometry's attach; clones point shared properties at the newest clone's fade node. | `PropertiesSetFadeNode` pushes a leaf update with pins; an unpinned fade node is acquired from a root owner or asked of the render thread (`AcquireFadeNode`). |
| Fade-node statics are reached through `AsFadeNode` (vtable); `netimmerse_cast<BSFadeNode*>` misses some. | Captures use `AsFadeNode`. |
| `TESWaterReflections::Update` hides the player's 3D and water objects around the cube faces; `Main::Draw` hides the first-person skeleton. | Capture those hidden bits with the frame (`FrameGlobals::cullHidden`); never publish them as scene changes. |
| The behaviour graph shows and hides an actor's parts during `Main::Draw`. | A hidden actor part, or a fading non-member, blocks its light and sun entry's exclusion (`Core/Scene/Model/Eligibility.h`). |
| The engine moves the first-person skeleton into root space mid-frame (`FUN_1414cb020`). | First-person rows and palettes are sampled at `Kick`, before the shift. |
| `Skin::GetWetness` advances an actor's fade on its first call each frame and is not thread-safe. | Wetness is captured on the render thread at the frame start, once per actor. |
| Detached roots are freed before the frame starts while a list filter made earlier names them. | Readers keyed by sun candidates may dereference them only because the candidates hold their entry nodes; the last owners release through `EngineReleases` at Present. |
| Engine destructors run Havok teardown and crash off the main thread. | No last release of an engine reference on a DCLF thread (`HandBack`, `EngineReleases`; observer `CS_DCLF_RELEASE_GUARD`). |
| A `BSTSmartPointer` release of an engine material deletes it without removing it from the material database. | Release through the engine's manager (`MaterialReference`, `kMaterialRelease`). |
| `FUN_14147a430` takes the LOD output as a float in XMM1. | Pass a float. |
| `BSLightingShader::SetupTechnique` writes `VPOSOffset` after unmapping PerTechnique (it works on DXVK). | DCLF reproduces the value; `ConstantMirror` re-reads mapped buffers from their last mapping. |
| `BSLightingShader::SetupMaterial` takes IBLParams from the shader object, not the material; IBLParams (PS 29) and FogColor drift with time of day, and no Lighting stage reads them. | Kept at first evaluation (`KeepUnreadFloats`, `KeepTechniqueFog`); not material state. |
| MaterialData.x (the envmap LOD fade) is `SetupGeometry`'s for techniques 1, 0xB and 0x10. | `SampleShading` writes it for all three. |
| SSE Engine Fixes writes PS 6 in `SetupGeometry` and NOPs `SetupMaterial`'s write. | Detected by instruction and ported in `GeometryPort`. |
| Weapon blood's AlphaTestRef comes from two globals `BSTempEffectWeaponBlood` writes each update. | Sample them as a frame value. |
| Some node classes implement child edits themselves (`BSParticleSystemManager`, `BGSDecalNode`, `BSFaceGenNiNode`); ShadowSceneNode's queued light removal detaches through no hooked function. | Each is detoured (`SceneEvents.cpp`, `Features/Decals/DecalHooks.cpp`); an out-of-world attach naming a held record evicts it (`SceneMirror::Evict`). |
| The engine reattaches a held outgoing LOD block from a cached handle; no detach erases it. | A reattach posts a kept notice that releases the hold-out (`ApplyGateKept`). |
| A decal with no z-write and bit 18 would cast into a paraboloid light by the code, but never does. | Its own reason, `ShadowReject::DecalPointLight`; parity reports any registration of one. |
| Box corners' grouping (`FUN_140e209e0`): corners 0 and 4 are `((X + C) + Y) +- Z` in the disassembly; the decompiler showed another order. | Port float arithmetic from the disassembly when the decompiler's order differs. |
| The address of a freed buffer is reused at once (DXVK). | A buffer address is never an identity (index pool ranges per slot; `GpuResources::Prefetch` holds a reference). |
| `FogFarColor.w` is copied from uninitialised stack memory; the permutation buffer's `IsSun` and `GrassSphereNormal` stay set from the last sky or grass draw. | Harmless: Lighting reads none of them. Capture parity's permutation check covers only the bits Lighting reads. |

Engine screen blood (`BloodSplatter`) and the first-person fire's changing size are engine behaviour, not DCLF defects.

### In CommonLibSSE-NG

| Bug | DCLF's rule |
| --- | --- |
| `NiSwitchNode`'s fields, and other members declared after a runtime-sized base, sit at VR's offsets in a multi-runtime build. | Read them at AE offsets (`Engine::ReadSwitch`). Never read members CommonLib declares after a runtime-sized base. |
| `NiBillboardNode::userFlags` is not at AE's `+0x128`. | Read the face mode at the AE offset (`PlacementRows::BillboardFaceMode`). |
| `NiTArray::size()` counts only non-null entries. | Iterate children to `free_idx()`. |
| `VertexDesc::GetSize` omits landscape data. | Take the stride as the description's low nibble times four. |
| `BSBatchRenderer::renderPass` is declared `BSTArray<PassGroup*>` but holds `PassGroup` structs inline. | DCLF reads it as inline structs. |

### In DXVK, BasicRHI and the render graph

Fixed in the forks; each fix states an invariant DCLF depends on.

| Bug | Invariant |
| --- | --- |
| DXVK's command-stream thread woke one waiter per synchronized chunk (`notify_one`); the ORG interop lets several threads wait for their own sequence numbers, so the right one could sleep forever (`extern/dxvk/src/dxvk/dxvk_cs.cpp`). | Interop waits are multi-waiter: wake all (`notify_all`). |
| DXVK's import paths (`importBufferResource`, `importImageResource`, the sparse `createAllocation`) popped the shared allocation pool unlocked, while loaders created buffers (`extern/dxvk/src/dxvk/dxvk_memory.cpp`). | Every import path takes the allocator's mutex. |
| A second `vkQueueSubmit2` concurrent with DXVK's submission thread lost the device (a page fault at 0 on the first DCLF frames). | Every submission ORG makes to DXVK's graphics queue goes under DXVK's submission lock (`RenderGraphRuntime.cpp`). ORG's own compute, copy and upload queues take `dxvkLockQueueSubmissions` instead since 2026-11-26 (held around DXVK's submissions, never across a present): frame generation's present holds that lock while it waits for the frame, which may wait for a compute batch or an upload (perf3-compute.md). The epoch's end waits on a semaphore of the runtime's own (`computeExit`), not on a graph timeline a rebuild destroys. |
| BasicRHI's copy service made command pools for the graphics family while submitting to a spare transfer queue. | Command allocators are made for their queue (`Device::CreateCommandAllocator(const Queue&)`). |
| BasicRHI's Vulkan push data: a 20-byte token lost the device on NVIDIA GA102. | Push data is split into tokens of at most 16 bytes (`kMaxPushDataTokenBytes`). |
| BasicRHI's Vulkan `frontCCW` was inverted relative to D3D12. | The D3D meaning on both backends; DCLF passes the engine's winding. |
| External Vulkan work (Streamline) right behind a graph epoch computed garbage. | It is separated by `Upscaling::SeparateFromGraphEpochs` ([render-graph.md](./render-graph.md)). |
