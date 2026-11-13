#include "Testing/SwitchRegistry.h"

#include "Utils/SwitchFile.h"

#if defined(CS_HAS_DCLF)
#	include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#endif

#include <algorithm>
#include <fstream>
#include <mutex>
#include <set>

namespace Testing::SwitchRegistry
{
	namespace
	{
		// The switches outside DCLF's registry. No table of them exists in the code: these are the names the sources read.
		// A name read at run time that is in no table is logged (NoteRead), so a new switch shows up.
		constexpr std::string_view kOther[] = {
			"BASICRHI_COMMAND_POOL_PROBE",
			"BASICRHI_COMMAND_POOL_PROBE_SHARED",
			"BASICRHI_D3D12_RESET_TRACE",
			"BASICRHI_DX12_TEST_NO_DEPTH",
			"BASICRHI_RELEASE_COMMAND_MEMORY",
			"BASICRHI_SHADER_API_D3D12",
			"BASICRHI_SHADER_API_VULKAN",
			"BASICRHI_VK_BINDING",
			"BASICRHI_VK_CRASH_MARKERS",
			"BASICRHI_VK_DUMP_ISA",
			"BASICRHI_VK_DUMP_LOWERED",
			"BASICRHI_VK_LIMITS",
			"BASICRHI_VK_MAX_DESCRIPTORS",
			"BASICRHI_VK_VALIDATE_LOWERING",
			"CS_AB_PROBE",
			"CS_DCLF_MAP_HOLD",
			"CS_DCLF_SITE_STATS",
			"CS_DISABLE_FEATURES",
			"CS_EDITOR",
			"CS_FFT",
			"CS_FG_DISPLAY_SUSPEND",
			"CS_FORCE_FSR_FG",
			"CS_FRAME_LATENCY",
			"CS_FSRFG_QDEPTH",
			"CS_FXC_REQUEST_DUMP",
			"CS_GAME_GAMMA",
			"CS_GPA",
			"CS_GPA_DIR",
			"CS_GPU_AMD",
			"CS_GPU_CRASH_ANALYSIS",
			"CS_GPU_EVENT_STATS",
			"CS_GPU_EVENT_TIMERS",
			"CS_GPU_IDLE_MIN_GAP_US",
			"CS_GPU_IDLE_SOURCE",
			"CS_GPU_IDLE_TRACE",
			"CS_GPU_LOAD",
			"CS_GPU_OP_PROBE",
			"CS_GPU_TIMERS",
			"CS_GRASS_COLLISION_STATS",
			"CS_HAIR",
			"CS_HANG_WATCHDOG",
			"CS_NATIVE_D3D11",
			"CS_NO_D3DX_UPGRADE",
			"CS_NO_PASS_PREFETCH",
			"CS_NVPERF_BASELINE",
			"CS_NVPERF_HALF_WIDTH",
			"CS_NVPERF_LABEL",
			"CS_NVPERF_MAX_REPEATS",
			"CS_NVPERF_MAX_SAMPLES",
			"CS_NVPERF_MIN_SAMPLES",
			"CS_NVPERF_REPEATS",
			"CS_NVPERF_SAMPLES",
			"CS_NVPERF_START_FRAME",
			"CS_NVPERF_TEMPLATE",
			"CS_ORG",
			"CS_ORG_ALIAS_LOG",
			"CS_ORG_ANCHOR_SELFTEST",
			"CS_ORG_ANCHOR_SELFTEST_FAIL",
			"CS_ORG_BINDLESS",
			"CS_ORG_BINK",
			"CS_ORG_BINK_SHOT",
			"CS_ORG_BLOOD_SPLATTER",
			"CS_ORG_CLOUD_SHADOWS",
			"CS_ORG_COMPOSITE",
			"CS_ORG_COMPUTE_QUEUE",
			"CS_ORG_COPY_QUEUE",
			"CS_ORG_CRASH_MARKERS",
			"CS_ORG_D3D12_DEBUG",
			"CS_ORG_D3D12_SDK",
			"CS_ORG_D3D12_SDK_PATH",
			"CS_ORG_D3D12_SELFTEST",
			"CS_ORG_D3D12_SELFTEST_HDR",
			"CS_ORG_DETAILED_CHECKPOINTS",
			"CS_ORG_DYNAMIC_CUBEMAPS",
			"CS_ORG_E11_RAYS",
			"CS_ORG_ENGINE_BUFFERS",
			"CS_ORG_ENGINE_CONSTANTS",
			"CS_ORG_ENGINE_CONSTANTS_CHECK",
			"CS_ORG_ENGINE_CONSTANTS_LOG_FRAMES",
			"CS_ORG_ENGINE_FLUSH_LATCH",
			"CS_ORG_ENGINE_FLUSH_LATCH_KEEP",
			"CS_ORG_ENGINE_FLUSH_LATCH_UNBIND",
			"CS_ORG_ENGINE_SHADERS",
			"CS_ORG_ENGINE_SKELETON",
			"CS_ORG_ENGINE_TARGETS",
			"CS_ORG_ENGINE_TARGETS_DCLF",
			"CS_ORG_ENGINE_TARGET_HANDLES",
			"CS_ORG_ENGINE_TEXTURES",
			"CS_ORG_EPOCH_STATS",
			"CS_ORG_EXPONENTIAL_HEIGHT_FOG",
			"CS_ORG_FX",
			"CS_ORG_GLINTS",
			"CS_ORG_GRAPHRES_SELFTEST",
			"CS_ORG_GRASS_COLLISION",
			"CS_ORG_HDR_DISPLAY",
			"CS_ORG_IBL",
			"CS_ORG_IMAGESPACE",
			"CS_ORG_IMAGESPACE_EFFECTS",
			"CS_ORG_IMAGESPACE_LOG",
			"CS_ORG_IMGUI",
			"CS_ORG_IN_STREAM",
			"CS_ORG_LIGHT_LIMIT_FIX_SHADOWS",
			"CS_ORG_LLF_PARITY",
			"CS_ORG_LLF_SHADOWS",
			"CS_ORG_LOADING_THREAD",
			"CS_ORG_MAP",
			"CS_ORG_MENU3D",
			"CS_ORG_NATIVE",
			"CS_ORG_NATIVE_BACKEND",
			"CS_ORG_NATIVE_PRESENT_PROBE",
			"CS_ORG_NATIVE_RHI_CONTEXT",
			"CS_ORG_NATIVE_VALIDATION",
			"CS_ORG_OCCLUSION_QUERIES",
			"CS_ORG_OCCLUSION_QUERY_LOG",
			"CS_ORG_OWNED_FRAME",
			"CS_ORG_PINNED_IMPORTS",
			"CS_ORG_POST_PROCESSING",
			"CS_ORG_PRECIPITATION",
			"CS_ORG_QUEUE_UPLOADS",
			"CS_ORG_RECORDING_TIMESTAMPS",
			"CS_ORG_RESIDUE_CENSUS",
			"CS_ORG_RESIDUE_CENSUS_FRAMES",
			"CS_ORG_RESIDUE_CENSUS_TOP",
			"CS_ORG_RESTING_LAYOUTS",
			"CS_ORG_REUSE_RECORDINGS",
			"CS_ORG_SCALEFORM",
			"CS_ORG_SCALEFORM_D3D11_PARTS",
			"CS_ORG_SCREENSHOT",
			"CS_ORG_SCREEN_SPACE_GI",
			"CS_ORG_SCREEN_SPACE_SHADOWS",
			"CS_ORG_SHADOW_MASK",
			"CS_ORG_SHADOW_MASK_VIEWS",
			"CS_ORG_SHARED_CONSTANTS_CHECK",
			"CS_ORG_SKY",
			"CS_ORG_SKYLIGHTING",
			"CS_ORG_SKY_CLOUDS",
			"CS_ORG_SSGI_DEBUG",
			"CS_ORG_SUBMIT",
			"CS_ORG_SUBSURFACE_SCATTERING",
			"CS_ORG_TERRAIN_BLENDING",
			"CS_ORG_TERRAIN_SHADOWS",
			"CS_ORG_TEST_CONTRIBUTOR",
			"CS_ORG_TEST_CONTRIBUTOR_EPOCH",
			"CS_ORG_TEST_CONTRIBUTOR_FRAMES",
			"CS_ORG_TEXTURE_STREAMING",
			"CS_ORG_TICKETS_AHEAD",
			"CS_ORG_TRIPWIRE_GUEST",
			"CS_ORG_TRIPWIRE_LOG",
			"CS_ORG_TRIPWIRE_LOG_INTERVAL",
			"CS_ORG_TRIPWIRE_SELFTEST",
			"CS_ORG_TRIPWIRE_WATCH_ALL",
			"CS_ORG_UPLOAD_QUEUE",
			"CS_ORG_UPSCALING",
			"CS_ORG_VOLUMETRIC_LIGHTING",
			"CS_ORG_VOLUMETRIC_SHADOWS",
			"CS_ORG_VRAM_SOURCE",
			"CS_ORG_WATER",
			"CS_ORG_WATER_PARTS",
			"CS_PASS_ROUNDS",
			"CS_PASS_SAMPLE",
			"CS_PASS_SKIP",
			"CS_PROFILER_LOG",
			"CS_RENDERDOC_DLL",
			"CS_RENDERDOC_REF_ALL",
			"CS_SHADER_DXC",
			"CS_SKIN",
			"CS_SL_VERBOSE",
			"CS_STREAMLINE_DLSS",
			"CS_TEST_CRASH",
			"CS_TEST_HANG",
			"CS_TEST_LIST_SWITCHES",
			"CS_TEX_CENSUS",
			"CS_UPSCALE_UNFOCUSED",
			"CS_VK_CLIENT_PRIORITY",
			"CS_VK_DRIVER_TRACE",
			"CS_VK_PRIORITY",
			"CS_VK_SPILL",
			"CS_VRAM_DEBUG",
			"DXVK_CONFIG",
			"DXVK_DEBUG",
			"DXVK_DEFER_TEXTURE_ALLOCATION",
			"DXVK_HDR",
			"DXVK_HUD",
			"DXVK_LOG_LEVEL",
			"DXVK_LOG_PATH",
			"DXVK_PIPELINE_STATS",
			"DXVK_SHADER_DUMP_PATH",
			"DXVK_VIEW_TRACE",
			"ORG_BLOCK_MIB",
			"ORG_COMPILE_TRACE_PASSES",
			"ORG_GENERATE_MIPS_ARRAY",
			"ORG_GRAPH_REPLAY",
			"ORG_GRAPH_REPLAY_OUTPUT",
			"ORG_GRAPH_SEQUENCE",
			"ORG_PASS_RECORD_CONCURRENCY",
			"ORG_PERSISTENT_ALIAS_EXCLUDE",
			"ORG_PERSISTENT_ALIAS_SHARE_EPOCHS",
			"ORG_PERSISTENT_COPIES_ON_COPY_QUEUE",
			"ORG_PERSISTENT_DUMP_SLOTS",
			"ORG_PERSISTENT_FORCE_REALIAS",
			"ORG_RESOURCE_LIFETIME_TRACE",
			"ORG_RESOURCE_LIFETIME_TRACE_HOLD_RETIRED",
			"ORG_SERVICES_TEST_DXC_DIR",
			"ORG_STATEGRAPH_STALL_DUMP",
			"ORG_TEST_DEVICE",
			"ORG_TRACE",
			"ORG_TRACE_FILE",
			"ORG_TRACE_RESOURCE",
			"ORG_UPDATE_GOLDEN",
			"ORG_VALIDATE_SHARING",
			"SARP_D3D12_DRED",
			"SARP_D3D12_GPU_VALIDATION",
			"SARP_GPU_LIFETIME_TRACE",
			"VK_INSTANCE_LAYERS",
			"VK_LAYER_PATH",
			"VK_LOADER_DEBUG",
			"VK_LOADER_LAYERS_DISABLE",
			"VK_LOADER_LAYERS_ENABLE",
		};

		// Families read by a computed name: any name with the prefix is known.
		constexpr std::string_view kPrefixes[] = { "CS_GPU_COUNTERS_", "CS_NVPERF_", "DXVK_", "VK_" };

		std::string Family(std::string_view a_name)
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

		/** @brief DCLF's rows (Core's, and the features' registered so far), a_row(name, kind, description). */
		template <class Row>
		void ForEachDclfRow([[maybe_unused]] Row&& a_row)
		{
#if defined(CS_HAS_DCLF)
			DCLF::ForEachSwitchRow([&](std::string_view a_name, std::string_view a_kind, std::string_view a_description) { a_row(a_name, a_kind, a_description); });
#endif
		}

		bool DclfComplete()
		{
#if defined(CS_HAS_DCLF)
			return DCLF::FeatureSwitchesRegistered();
#else
			return true;
#endif
		}
	}

	Known Check(std::string_view a_name)
	{
		bool found = false;
		ForEachDclfRow([&](std::string_view a_row, std::string_view, std::string_view) { found = found || a_row == a_name; });
		if (found || std::ranges::find(kOther, a_name) != std::end(kOther))
			return Known::Yes;
		for (const auto prefix : kPrefixes)
			if (a_name.starts_with(prefix) && a_name.size() > prefix.size())
				return Known::Yes;
		if (a_name.starts_with("CS_DCLF_") && !DclfComplete())
			return Known::Unchecked;
		return Known::No;
	}

	std::vector<Info> All()
	{
		std::vector<Info> all;
		std::set<std::string, std::less<>> names;
		ForEachDclfRow([&](std::string_view a_name, std::string_view a_kind, std::string_view a_description) {
			if (names.emplace(a_name).second)
				all.push_back({ std::string(a_name), std::string(a_kind), std::string(a_description), "dclf" });
		});
		for (const auto name : kOther)
			if (names.emplace(name).second)
				all.push_back({ std::string(name), "other", "", Family(name) });
		std::ranges::sort(all, {}, &Info::name);
		return all;
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
		json rows = json::array();
		for (const auto& info : All())
			rows.push_back({ { "name", info.name }, { "kind", info.kind }, { "description", info.description }, { "family", info.family } });
		const json document{ { "version", 1 }, { "dclf_complete", DclfComplete() }, { "switches", std::move(rows) } };
		const auto temporary = std::filesystem::path(path).concat(".tmp");
		{
			std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
			file << document.dump(1, '\t');
		}
		std::error_code error;
		std::filesystem::rename(temporary, path, error);
		logger::info("[TEST] CS_TEST_LIST_SWITCHES: {} switches written to {}{}; exiting", document["switches"].size(), path.string(), error ? " (rename failed)" : "");
		spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& a_logger) { a_logger->flush(); });
		::TerminateProcess(::GetCurrentProcess(), 0);
	}
}
