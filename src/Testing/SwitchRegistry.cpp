#include "Testing/SwitchRegistry.h"

#include "Feature.h"
#include "Testing/Internal.h"
#include "Utils/SwitchFile.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <set>

namespace Testing::SwitchRegistry
{
	namespace
	{
		using enum Type;
		using enum Kind;

		// Every switch the game reads, in alphabetical order (checked below). DCLF's Core reads its rows through
		// DCLF::Switch. DCLF's features register their own (DCLF::RegisterSwitch). A name read at run time that is in
		// neither is logged (NoteRead), so a new switch shows up. A key makes the row a typed cs-test config key.
		// The game's dump of this table (CS_TEST_LIST_SWITCHES) is cs-test's schema: regenerate it after a change here.
		constexpr Row kSwitches[] = {
			{ .name = "BASICRHI_BARRIER_PROBE" },
			{ .name = "BASICRHI_BARRIER_PROBE_FILE" },
			{ .name = "BASICRHI_COMMAND_POOL_PROBE" },
			{ .name = "BASICRHI_COMMAND_POOL_PROBE_SHARED" },
			{ .name = "BASICRHI_COMMAND_POOL_SITES" },
			{ .name = "BASICRHI_D3D12_RESET_TRACE" },
			{ .name = "BASICRHI_DEVICE_MEMORY_REPORT" },
			{ .name = "BASICRHI_DX12_FULL_BARRIERS" },
			{ .name = "BASICRHI_DX12_SUBMIT_LOG", .description = "1: each D3D12 queue's last 64 submissions in the stall report" },
			{ .name = "BASICRHI_DX12_TEST_NO_DEPTH" },
			{ .name = "BASICRHI_HEAP_BIND_REPEAT" },
			{ .name = "BASICRHI_MEMORY_DUMP" },
			{ .name = "BASICRHI_PAD_LISTS" },
			{ .name = "BASICRHI_RELEASE_COMMAND_MEMORY" },
			{ .name = "BASICRHI_SPLIT_SUBMITS" },
			{ .name = "BASICRHI_VK_BINDING" },
			{ .name = "BASICRHI_VK_BOUND_UBO" },
			{ .name = "BASICRHI_VK_CRASH_MARKERS" },
			{ .name = "BASICRHI_VK_DUMP_ISA" },
			{ .name = "BASICRHI_VK_DUMP_LOWERED" },
			{ .name = "BASICRHI_VK_FORCE_GENERAL", .description = "1: every image in GENERAL but presents (an experiment: the layout images shared with DXVK rest in)" },
			{ .name = "BASICRHI_VK_LIMITS" },
			{ .name = "BASICRHI_VK_MAX_DESCRIPTORS" },
			{ .name = "BASICRHI_VK_MODULE_IDENTIFIERS", .description = "0: no module identifier store; where the pipeline binary store is off, every run lowers its pipelines' modules again" },
			{ .name = "BASICRHI_VK_PIPELINE_BINARY", .description = "0: no pipeline binary store; the lowered binding models lower and compile every pipeline each run, through the pipeline cache. 1: the store on NVIDIA, where it is off by default (2 MiB of process memory per pipeline)" },
			{ .name = "BASICRHI_VK_PIPELINE_ROBUSTNESS", .description = "core (default): the render graph's pipelines get core robustBufferAccess for buffers and none for images, as native Vulkan; off: none; device: the device's robustness (DXVK's robustness2 in guest)" },
			{ .name = "BASICRHI_VK_VALIDATE_LOWERING" },
			{ .name = "CS_AB_PROBE" },
			{ .name = "CS_D3D12_SOURCE_DUMP" },
			{ .name = "CS_DCLF", .key = "dclf.enabled", .type = Bool, .defaultValue = "1", .kind = Feature, .description = "0: DCLF does not install" },
			{ .name = "CS_DCLF_ASYNC_COMPUTE", .type = Bool, .defaultValue = "1", .kind = Feature, .description = "0: the passes that prefer the compute queue (build-draws' queries, fades, portal batches, tree wind) run on the graphics queue; unset: on in guest mode, off in native mode (1 turns it on there)" },
			{ .name = "CS_DCLF_CAPTURE", .kind = Diagnostic, .description = "<dir>: D3D12 native only. Records every view, sampler and pipeline; the cs-test command dclf:capture then dumps one frame of DCLF's Z-prepass and main opaque passes into <dir>/capture-<frame>/ for pascal-fetch-bench replay (docs/development/plans/dclf-replay.md)" },
			{ .name = "CS_DCLF_CAPTURE_PARITY", .key = "dclf.capture_parity", .type = Bool, .defaultValue = "0", .kind = Parity, .description = "1: the tables against the engine's own lighting draws (run with CS_DCLF_PARITY_BOTH=1, so the engine draws what DCLF draws)" },
			{ .name = "CS_DCLF_CPU_BATCH", .kind = Feature, .description = "the CPU pipeline's batch lanes (default 4096, rounded up to a multiple of 8; the live setting's seed: dclf:cpu-batch, the menu)" },
			{ .name = "CS_DCLF_CPU_RECORD", .kind = Feature, .description = "indirect|serial (default indirect on every vendor): the CPU pipeline's per-frame views as indirect draws the cull jobs write, or direct draws DXVK's CS thread records (live toggle's seed)" },
			{ .name = "CS_DCLF_CULL", .key = "dclf.cull", .type = Enum, .defaultValue = "occlusion", .choices = "off|frustum|occlusion", .kind = Feature, .description = "off|frustum|occlusion (default): GPU culling, the live toggle's seed" },
			{ .name = "CS_DCLF_HEAP_FRAME_BLOCKS", .key = "dclf.heap_frame_blocks", .type = Bool, .defaultValue = "0", .kind = Diagnostic, .description = "1: D3D12 pulled stages read the pass-wide frame blocks by heap index instead of a bound descriptor table (A/B for dclf-bound-constants.md)" },
			{ .name = "CS_DCLF_MAP_HOLD" },
			{ .name = "CS_DCLF_PARITY", .key = "dclf.parity", .type = Bool, .defaultValue = "0", .kind = Parity, .description = "1: set parity (the claims against the engine's registrations, and the visibility words read back after the colour epoch against the CPU's decisions), the kept stores against a rebuild every 60 frames, and the scene mirror against the live objects every frame" },
			{ .name = "CS_DCLF_PARITY_BOTH", .key = "dclf.parity_both", .type = Bool, .defaultValue = "0", .kind = Parity, .description = "1: nothing is withheld from the engine, which draws everything while DCLF draws its set as well (capture parity compares the two; it double-draws)" },
			{ .name = "CS_DCLF_PASS_STATS", .key = "dclf.pass_stats", .type = Bool, .defaultValue = "0", .kind = Diagnostic, .description = "1: per-pass counts in the report" },
			{ .name = "CS_DCLF_PIPELINE", .kind = Feature, .description = "auto (default: exteriors GPU-driven, interiors CPU-driven)|manual|gpu|cpu: the draws culled and recorded on the GPU, or culled on the CPU and recorded as direct draws into DXVK's command buffer; gpu and cpu are manual with both kinds of cell (live toggle's seed)" },
			{ .name = "CS_DCLF_PIPELINE_PRECOMPILE", .key = "dclf.pipeline_precompile", .type = Bool, .defaultValue = "1", .kind = Diagnostic, .description = "0: no startup pipeline step; each DCLF pipeline builds when first drawn (the pipeline manifest is still read and saved)" },
			{ .name = "CS_DCLF_PLAIN_SNOW_SPARKLE", .key = "dclf.plain_snow_sparkle", .type = Bool, .defaultValue = "1", .kind = Feature, .description = "0: a snow layer whose material is the plain class draws without sparkle instead of the Snow class's default (live toggle's seed)" },
			{ .name = "CS_DCLF_PLAYER_PARITY", .key = "dclf.player_parity", .type = Bool, .defaultValue = "0", .kind = Parity, .description = "1: the compatibility monitor starts on (the menu switches it live): engine state other plugins change with no hook DCLF sees, by reference and plugin" },
			{ .name = "CS_DCLF_PROFILE", .type = Bool, .defaultValue = "0", .kind = Diagnostic, .description = "1: scene-phase timings in the report" },
			{ .name = "CS_DCLF_PROGRAM_MANIFEST", .type = Bool, .defaultValue = "1", .kind = Diagnostic, .description = "0: the engine draw families queue none of the program manifest's keys at setup, so each program builds at its first draw, as on a cold shader store's first run (the manifest is still read and saved)" },
			{ .name = "CS_DCLF_RELEASE_GUARD", .type = Bool, .defaultValue = "0", .kind = Diagnostic, .description = "1: the engine objects' last releases on DCLF's threads (the scene lane, the pool) counted and named by class (NiRefObject::DeleteThis patched; T6b3e)" },
			{ .name = "CS_DCLF_SCENE_COPIES", .kind = Feature, .description = "the copies a frame's scene values keep per pool class (FrameValues' rows; default 3, 2 to the frames in flight): a frame's producer reuses the copy the oldest in-flight frame bound, its uploads waiting on the GPU for that frame" },
			{ .name = "CS_DCLF_SHADER_DEBUG", .kind = Diagnostic, .description = "default on (off in the release profile, 1 turns it on): SPIR-V with debug info, and the HLSL sources copied beside the log; 0: without" },
			{ .name = "CS_DCLF_SHADER_SOURCE_DIR", .kind = Diagnostic, .description = "where CS_DCLF_SHADER_DEBUG copies the HLSL sources" },
			{ .name = "CS_DCLF_SHRINK", .kind = Feature, .description = "when the growable scene buffers give memory back (one policy for all, PooledBackings::Shrink): default once a buffer was asked at most a quarter of its capacity for 5 s; off: grow-only; eager: at the first such ask (stress and parity runs)" },
			{ .name = "CS_DCLF_SITE_STATS", .type = Bool },
			{ .name = "CS_DCLF_STAGE_DUMP", .kind = Diagnostic, .description = "<dir>: every DCLF stage request's compile inputs, the Lighting programs' descriptor pairs and the pulled rewrite's options, as JSON lines in <dir>/stages.jsonl, for tools/shader-compare" },
			{ .name = "CS_DCLF_STATS", .key = "dclf.stats", .type = Bool, .defaultValue = "0", .kind = Diagnostic, .description = "1: the periodic report" },
			{ .name = "CS_DCLF_TABLE_START", .kind = Diagnostic, .description = "small: the growable GPU tables start with a few rows, so that growth runs early and often" },
			{ .name = "CS_DISABLE_FEATURES", .key = "cs.disable_features", .description = "comma-separated feature short names disabled at boot (write [features] instead)" },
			{ .name = "CS_EDITOR" },
			{ .name = "CS_ENABLE_FEATURES" },
			{ .name = "CS_ENGINE_ZONES", .type = Bool, .kind = Diagnostic, .description = "1: Tracy zones around the engine's render function calls (AE 1.6.1170 only, Tracy builds)" },
			{ .name = "CS_FEATURE_GPU_ZONES", .type = Bool, .kind = Diagnostic, .description = "1: a Tracy GPU zone on each feature's frame callbacks (a D3D11 query pair each, Tracy builds)" },
			{ .name = "CS_FORCE_FSR_FG" },
			{ .name = "CS_FRAME_LATENCY" },
			{ .name = "CS_FSRFG_QDEPTH" },
			{ .name = "CS_FXC_REQUEST_DUMP", .key = "cs.fxc_request_dump", .description = "<folder>: one line per FXC request in fxc-requests.jsonl there, to repeat the build outside the game" },
			{ .name = "CS_GPA", .key = "cs.gpa", .type = Bool, .description = "1: GPUPerfAPI loads at startup (it must exist before the device)" },
			{ .name = "CS_GPA_DIR" },
			{ .name = "CS_GPU_COUNTERS_RANGES", .key = "cs.gpu_counters_ranges", .description = "a headless run's hardware counter capture, queued once the counter backend is ready" },
			{ .name = "CS_GPU_COUNTERS_SAMPLES", .key = "cs.gpu_counters_samples", .type = Int, .description = "that capture's samples (default 3)" },
			{ .name = "CS_GPU_COUNTERS_START_FRAME", .key = "cs.gpu_counters_start_frame", .type = Int, .description = "the frame that capture starts at" },
			{ .name = "CS_GPU_EVENT_STATS" },
			{ .name = "CS_GPU_EVENT_TIMERS", .key = "cs.gpu_event_timers", .type = Int, .description = "<frames>: the GPU event timers on, their counts logged every that many frames" },
			{ .name = "CS_GPU_LOAD" },
			{ .name = "CS_GPU_OP_PROBE" },
			{ .name = "CS_GPU_TIMERS" },
			{ .name = "CS_GRASS_COLLISION_STATS", .type = Bool },
			{ .name = "CS_HANG_WATCHDOG", .key = "cs.hang_watchdog", .type = Int, .description = "seconds without progress before the hang watchdog writes its report (default 15)" },
			{ .name = "CS_NATIVE_D3D11" },
			{ .name = "CS_NVPERF_BASELINE" },
			{ .name = "CS_NVPERF_HALF_WIDTH" },
			{ .name = "CS_NVPERF_LABEL" },
			{ .name = "CS_NVPERF_MAX_REPEATS" },
			{ .name = "CS_NVPERF_MAX_SAMPLES" },
			{ .name = "CS_NVPERF_MIN_SAMPLES" },
			{ .name = "CS_NVPERF_REPEATS" },
			{ .name = "CS_NVPERF_SAMPLES", .type = Int },
			{ .name = "CS_NVPERF_START_FRAME", .type = Int },
			{ .name = "CS_NVPERF_TEMPLATE" },
			{ .name = "CS_ORG", .key = "org.enabled", .type = Bool, .defaultValue = "1", .description = "0: the render graph is off; features: only its interop device features, without the graph" },
			{ .name = "CS_ORG_ALIAS_LOG", .type = Bool, .defaultValue = "0" },
			{ .name = "CS_ORG_BINK_SHOT", .key = "org.bink_shot" },
			{ .name = "CS_ORG_D3D12_SDK" },
			{ .name = "CS_ORG_D3D12_SDK_PATH" },
			{ .name = "CS_ORG_DEFRAG", .key = "org.defrag", .type = Bool, .defaultValue = "0", .description = "native: compact ORG's device-local memory at loading screens (GraphHost::DefragmentAtIdle)" },
			{ .name = "CS_ORG_ENGINE_CONSTANTS_LOG_FRAMES", .type = Int },
			{ .name = "CS_ORG_ENGINE_DRAW_TAP" },
			{ .name = "CS_ORG_FOG_DEBUG" },
			{ .name = "CS_ORG_GRAPH_RESOURCES_FRAME" },
			{ .name = "CS_ORG_IMAGESPACE_NATIVE", .defaultValue = "2", .description = "0: ImageSpace steps captured; 1: param steps also computed and compared with the capture; 2: param steps staged from the engine's params without its draw; 3: diagnostic, computed rows staged at the engine's draw (dclf-native-imagespace.md)" },
			{ .name = "CS_ORG_LLF_PARITY", .key = "org.llf_parity", .description = "1: the parity frame's D3D11 light culling against the render graph's, cluster by cluster" },
			{ .name = "CS_ORG_MISMATCH_FATAL", .key = "org.mismatch_fatal", .description = "0: a frame anchor mismatch is not fatal (it is in every build but the Release profile)" },
			{ .name = "CS_ORG_NATIVE", .key = "org.native", .type = Bool, .defaultValue = "0", .description = "true: native mode, no DXVK; BasicRHI's device and ORG present (render-graph.md, \"Native mode\")" },
			{ .name = "CS_ORG_NATIVE_BACKEND", .key = "org.backend", .type = Enum, .defaultValue = "vulkan", .choices = "vulkan|d3d12", .description = "native mode's backend; d3d12 needs org.native" },
			{ .name = "CS_ORG_NATIVE_SYNC_PRESENT" },
			{ .name = "CS_ORG_NATIVE_VALIDATION", .key = "org.validation", .type = Bool, .defaultValue = "0", .description = "true: BasicRHI's API validation in native mode (Vulkan layers or the D3D12 debug layer)" },
			{ .name = "CS_ORG_OCCLUSION_QUERY_LOG", .type = Bool, .defaultValue = "0" },
			{ .name = "CS_ORG_PIPELINE_CACHE", .key = "org.pipeline_cache", .type = Bool, .defaultValue = "1", .description = "false: no on-disk pipeline cache (Data/ShaderCache/v3/pipelines-*.bin); every pipeline is built again each run" },
			{ .name = "CS_ORG_POST_NATIVE", .key = "org.post_native", .defaultValue = "2", .description = "the native post chain (native-post-chain.md), AE only: 0 the engine's ImageSpace containers; 1 observe, the containers run and CS's predicted steps, rows and state are compared; 2 native, the containers are not called. Optionally :hdr,eof,composites,post to pick families" },
			{ .name = "CS_ORG_PRESENT_COPY_SKIP" },
			{ .name = "CS_ORG_RAW_ACCESS_CHECK", .key = "org.raw_access_check", .type = Bool, .defaultValue = "0", .description = "native: log and count work on graph-tracked images that skips the graph's state tracking ([ORG raw access] lines)" },
			{ .name = "CS_ORG_RECORDING_TIMESTAMPS", .type = Bool, .defaultValue = "0" },
			{ .name = "CS_ORG_SHARED_CONSTANTS_CHECK", .type = Bool, .defaultValue = "0" },
			{ .name = "CS_ORG_SHOT_PHYSSKY" },
			{ .name = "CS_ORG_SHOT_TARGET" },
			{ .name = "CS_ORG_SSGI_DEBUG" },
			{ .name = "CS_ORG_SSS_KEEP_APART" },
			{ .name = "CS_ORG_TAP_ERROR_TEST" },
			{ .name = "CS_ORG_TRIPWIRE_LOG", .key = "org.tripwire_log", .type = Bool, .defaultValue = "0", .description = "1: Tripwire logs every call site with its count" },
			{ .name = "CS_ORG_TRIPWIRE_LOG_INTERVAL", .key = "org.tripwire_log_interval", .type = Int, .description = "seconds between Tripwire's call site reports (default 60)" },
			{ .name = "CS_ORG_TRIPWIRE_SELFTEST", .type = Bool, .defaultValue = "0" },
			{ .name = "CS_ORG_TRIPWIRE_WATCH_ALL", .type = Bool, .defaultValue = "0" },
			{ .name = "CS_ORG_VIEW_ATMOSPHERE_DEBUG" },
			{ .name = "CS_ORG_VRAM_SOURCE", .key = "org.vram_source", .description = "org: ORG's allocator counts beside DXVK's memory, for the VRAM heaps and texture streaming's budget" },
			{ .name = "CS_PASS_ROUNDS", .type = Int },
			{ .name = "CS_PASS_SAMPLE", .type = Bool },
			{ .name = "CS_PASS_SKIP" },
			{ .name = "CS_PROFILER_LOG" },
			{ .name = "CS_RENDERDOC_DLL" },
			{ .name = "CS_RENDERDOC_REF_ALL" },
			{ .name = "CS_RHI_CENSUS" },
			{ .name = "CS_SHADER_DXC", .key = "cs.shader_dxc", .description = "a regular expression on <path>:<target>:<descriptor>: the shaders that build with DXC; empty: every shader with FXC" },
			{ .name = "CS_SL_VERBOSE" },
			{ .name = "CS_STREAMLINE_DLSS", .type = Bool },
			{ .name = "CS_STREAMLINE_REFLEX", .description = "0: Streamline does not load Reflex, PCL or DLSS-G (for Nsight captures)" },
			{ .name = "CS_SUBMIT_TRACE" },
			{ .name = "CS_TEST_CRASH" },
			{ .name = "CS_TEST_HANG" },
			{ .name = "CS_TEST_LIST_SWITCHES", .description = "1: the game writes CommunityShaders-Switches.json (every switch) at kPostPostLoad and exits" },
			{ .name = "CS_TEX_CENSUS", .type = Bool },
			{ .name = "CS_UPSCALE_UNFOCUSED", .type = Bool },
			{ .name = "CS_VK_CLIENT_PRIORITY" },
			{ .name = "CS_VK_DRIVER_TRACE" },
			{ .name = "CS_VK_PRIORITY" },
			{ .name = "CS_VK_SPILL" },
			{ .name = "CS_VRAM_DEBUG", .key = "cs.vram_debug", .type = Bool, .description = "1: the VRAM attribution's debug mode: creation call sites and ORG's allocation ledger" },
			{ .name = "DXVK_CONFIG" },
			{ .name = "DXVK_DEBUG", .key = "dxvk.debug", .description = "DXVK's debug options (hang: its hang markers; DRED on D3D12)" },
			{ .name = "DXVK_DEFER_TEXTURE_ALLOCATION" },
			{ .name = "DXVK_DEVICE_MEMORY_REPORT" },
			{ .name = "DXVK_FILTER_DEVICE_NAME", .key = "dxvk.filter_device_name", .description = "DXVK uses only the adapter whose name contains this" },
			{ .name = "DXVK_HDR" },
			{ .name = "DXVK_HUD" },
			{ .name = "DXVK_LOG_LEVEL" },
			{ .name = "DXVK_LOG_PATH" },
			{ .name = "DXVK_PIPELINE_STATS", .type = Bool },
			{ .name = "DXVK_SHADER_DUMP_PATH" },
			{ .name = "DXVK_VIEW_TRACE" },
			{ .name = "ORG_BOUND_PUSH_TABLES", .description = "0: D3D12 graph compute programs read every constant buffer from the heap (A/B for their bound push tables)" },
			{ .name = "ORG_FRAME_PLACEMENT", .key = "org.frame_placement", .type = Bool, .defaultValue = "0", .description = "true: ORG places graph transients across the whole frame (org-whole-frame-placement.md)" },
			{ .name = "ORG_FRAME_PLACEMENT_LOG", .key = "org.frame_placement_log", .type = Bool, .defaultValue = "0", .description = "true: log every frame-placement resource and slot" },
			{ .name = "ORG_GRAPH_REPLAY_OUTPUT" },
			{ .name = "ORG_RELOCATION_STRESS", .key = "org.relocation_stress", .type = Int, .defaultValue = "0", .description = "<n>: ORG moves n ReadOnly graph backings at each relocation commit, to test the follow path (org-relocation.md, step 3)" },
			{ .name = "SARP_D3D12_DRED" },
			{ .name = "SARP_D3D12_GPU_VALIDATION", .type = Bool },
			{ .name = "VK_INSTANCE_LAYERS" },
			{ .name = "VK_LAYER_PATH" },
			{ .name = "VK_LOADER_DEBUG" },
			{ .name = "VK_LOADER_LAYERS_DISABLE", .key = "vk.loader_layers_disable", .description = "the Vulkan loader leaves out these layers (comma-separated globs)" },
			{ .name = "VK_LOADER_LAYERS_ENABLE" },
		};

		static_assert([] {
			for (std::size_t i = 1; i < std::size(kSwitches); ++i)
				if (!(kSwitches[i - 1].name < kSwitches[i].name))
					return false;
			return true;
		}(), "kSwitches is in alphabetical order, each name once");
		static_assert([] {
			for (const auto& row : kSwitches)
				if ((row.type == Type::Enum) != !row.choices.empty())
					return false;
			return true;
		}(), "an Enum row has choices, and only an Enum row");

		// Families read by a computed name: any name with the prefix is known.
		constexpr std::string_view kPrefixes[] = { "CS_GPU_COUNTERS_", "CS_NVPERF_", "DXVK_", "VK_" };

		std::string_view Family(std::string_view a_name)
		{
			if (a_name.starts_with("CS_DCLF"))
				return "dclf";
			if (a_name.starts_with("CS_ORG") || a_name.starts_with("ORG_"))
				return "org";
			if (a_name.starts_with("BASICRHI_") || a_name.starts_with("SARP_"))
				return "basicrhi";
			if (a_name.starts_with("DXVK_"))
				return "dxvk";
			if (a_name.starts_with("VK_"))
				return "vk";
			return "cs";
		}

		std::string_view TypeName(Type a_type)
		{
			switch (a_type) {
			case Type::Bool:
				return "bool";
			case Type::Int:
				return "int";
			case Type::Enum:
				return "enum";
			default:
				return "string";
			}
		}

		/** @brief Whether DCLF's features registered a_name. */
		bool FeatureSwitch([[maybe_unused]] std::string_view a_name)
		{
			bool found = false;
			DCLF::ForEachFeatureSwitch([&](std::string_view a_row, std::string_view, bool, std::string_view) { found = found || a_row == a_name; });
			return found;
		}

		bool DclfComplete()
		{
			return DCLF::FeatureSwitchesRegistered();
		}
	}

	const Row* Find(std::string_view a_name)
	{
		const auto* end = std::end(kSwitches);
		const auto* row = std::lower_bound(std::begin(kSwitches), end, a_name, [](const Row& a_row, std::string_view a_wanted) { return a_row.name < a_wanted; });
		return row != end && row->name == a_name ? row : nullptr;
	}

	std::string_view KindName(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::Feature:
			return "feature";
		case Kind::Parity:
			return "parity";
		case Kind::Diagnostic:
			return "diagnostic";
		case Kind::Test:
			return "test";
		default:
			return "other";
		}
	}

	Known Check(std::string_view a_name)
	{
		if (Find(a_name) || FeatureSwitch(a_name))
			return Known::Yes;
		for (const auto prefix : kPrefixes)
			if (a_name.starts_with(prefix) && a_name.size() > prefix.size())
				return Known::Yes;
		if (a_name.starts_with("CS_DCLF_") && !DclfComplete())
			return Known::Unchecked;
		return Known::No;
	}

	namespace
	{
		std::mutex readMutex;
		std::set<std::string, std::less<>> readNames;
		std::vector<std::string> pendingReads;  // read before DCLF's registry was complete: checked by CheckPendingReads
		bool pendingChecked = false;            // CheckPendingReads ran: later reads are checked at once
	}

	void NoteRead(std::string_view a_name)
	{
		{
			std::scoped_lock lock(readMutex);
			if (!readNames.emplace(a_name).second)
				return;
			// DCLF's features register on the main thread: their rows are read only once that is done.
			if (!DclfComplete() && !pendingChecked) {
				pendingReads.emplace_back(a_name);
				return;
			}
		}
		if (Check(a_name) == Known::No)
			logger::warn("[TEST] the switch {} was read but no switch registry lists it (src/Testing/SwitchRegistry.cpp)", a_name);
	}

	void CheckPendingReads()
	{
		std::vector<std::string> pending;
		{
			std::scoped_lock lock(readMutex);
			pending.swap(pendingReads);
			pendingChecked = true;
		}
		for (const auto& name : pending)
			if (Check(name) == Known::No)
				logger::warn("[TEST] the switch {} was read but no switch registry lists it (src/Testing/SwitchRegistry.cpp)", name);
	}

	void DumpIfAsked()
	{
		if (Util::SwitchFile::Value("CS_TEST_LIST_SWITCHES", true) != "1")
			return;
		const auto path = Util::SwitchFile::Directory() / "CommunityShaders-Switches.json";
		auto choices = [](std::string_view a_text) {
			json list = json::array();
			for (std::size_t start = 0; start < a_text.size();) {
				const auto end = std::min(a_text.find('|', start), a_text.size());
				list.push_back(std::string(a_text.substr(start, end - start)));
				start = end + 1;
			}
			return list;
		};
		std::vector<json> rows;
		for (const auto& row : kSwitches)
			rows.push_back({ { "name", std::string(row.name) }, { "key", std::string(row.key) }, { "type", std::string(TypeName(row.type)) },
				{ "default", std::string(row.defaultValue) }, { "choices", choices(row.choices) }, { "kind", std::string(KindName(row.kind)) },
				{ "family", std::string(Family(row.name)) }, { "description", std::string(row.description) } });
		// A feature's switch has no key. One on by default is a bool, and so is one whose text starts "1:", which 1 turns on.
		DCLF::ForEachFeatureSwitch([&](std::string_view a_name, std::string_view a_kind, bool a_defaultOn, std::string_view a_description) {
			const bool offByDefault = !a_defaultOn && a_description.starts_with("1:");
			rows.push_back({ { "name", std::string(a_name) }, { "key", "" }, { "type", a_defaultOn || offByDefault ? "bool" : "string" },
				{ "default", a_defaultOn ? "1" : offByDefault ? "0" : "" }, { "choices", json::array() }, { "kind", std::string(a_kind) }, { "family", "dclf" },
				{ "description", std::string(a_description) } });
		});
		std::ranges::sort(rows, {}, [](const json& a_row) { return a_row["name"].get<std::string>(); });
		std::vector<std::string> features;
		for (auto* feature : ::Feature::GetFeatureList())
			features.push_back(feature->GetShortName());
		std::ranges::sort(features);
		std::vector<std::string> prefixes(std::begin(kPrefixes), std::end(kPrefixes));
		const json document{ { "version", 2 }, { "dclf_complete", DclfComplete() }, { "prefixes", prefixes }, { "features", features }, { "switches", rows } };
		const auto temporary = std::filesystem::path(path).concat(".tmp");
		{
			std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
			file << document.dump(1, '\t');
		}
		std::error_code error;
		std::filesystem::rename(temporary, path, error);
		const auto reason = fmt::format("CS_TEST_LIST_SWITCHES: {} switches written to {}{}", rows.size(), path.string(), error ? " (rename failed)" : "");
		logger::info("[TEST] {}; exiting", reason);
		// In a test run the outcome is written first, so cs-test can make the dump (configs/list-switches.toml).
		Testing::Finish(error ? "failed" : "passed", reason, 0u);
		spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& a_logger) { a_logger->flush(); });
		::TerminateProcess(::GetCurrentProcess(), 0);
	}
}
