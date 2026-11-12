// DCLF's test commands ("dclf:<name>=<arg>") and waits, registered with the test runner (src/Testing) when DCLF installs.
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "TestHarness.h"
#include "Testing/Testing.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/Engine/Bink/Bink.h"

#include "Vram/MemProbe.h"
#include "Vram/SpillLab.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatches.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Capture/Capture.h"
#include "Features/DrawcallLimitFix/Diagnostics/PlayerParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/TelemetryProfile.h"
#include "CSEditor/EditorWindow.h"
#include "Features/HDRDisplay.h"
#include "Features/PostProcessing.h"
#include "Features/RemoteControl/GpuCounters.h"
#include "Features/ScreenshotFeature.h"
#include "Features/Upscaling.h"
#include "GpuEventTimers.h"
#include "Globals.h"
#include "Menu.h"

#include <Tracy/Tracy.hpp>
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "ShaderCache.h"
#include "State.h"
#include "Vram/VramLedger.h"
#include "Features/DrawcallLimitFix/Features/TextureStreaming/TextureStreaming.h"
#include "TextureStreaming/Census.h"
#include "TextureStreaming/Streamer.h"

#include <ORGModuleServices/PipelineRegistry.h>
#include <OpenRenderGraph/GraphHost.h>
#include <Render/RenderGraph/RhiExecution.h>

#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace DCLF
{
	namespace
	{
		// "dclf:tex-stats": texture streaming's totals, in the log. "dclf:tex-force=<top>": every streamed window at this top, or -1 for the policy's
		// choice. "dclf:tex-distance=<0|1>", "dclf:tex-feedback=<0|1>": the demand's two sources. "dclf:tex-budget=<MiB>": the budget, 0 for the driver's. "dclf:tex-offset=<mips>": how far what is on screen may step down over it. "dclf:tex-dump=<name>": every texture's state to texture-<name>.tsv, and what the materials draw, in the log. "dclf:tex-census": the texture census's rows written to the log directory (CS_TEX_CENSUS=1).
		bool RunTextureStreamingCommand(const std::string& a_command)
		{
			if (a_command == "dclf:tex-stats") {
				const auto stats = TextureStreaming::GetStats();
				logger::info("[TextureStreaming] {} textures ({} with windows, {} pinned, {} full for native draws), {} MiB resident, {} MiB wanted, of {} MiB; {} windows built ({} upgrades, {} downgrades, {} under pressure asked), {} MiB read, {} notes, {} feedback reports ({} finer than distance), {} failed, {} bound before their base, {} needed their storage, {} left to the engine",
					stats.textures, stats.withWindow, stats.pinned, stats.nativeFull, stats.residentBytes >> 20, stats.wantedBytes >> 20, stats.fullBytes >> 20, stats.windowsBuilt, stats.upgrades, stats.downgrades, stats.pressureDowngrades,
					stats.bytesRead >> 20, stats.noted, stats.feedbackReported, stats.feedbackFiner, stats.loadFailures, stats.safetyNetLoads, stats.storageFills, stats.passedThrough);
				logger::info("[TextureStreaming] budget {} MiB (card {}, driver {}): {} used, {} of it in system memory, {} not windows ({} outside DXVK), {} allowed for windows, {} wanted, {} of that on screen; pulled back {:.2f} mips by distance, {:.2f} on screen",
					stats.budgetBytes >> 20, stats.cardBytes >> 20, stats.driverBudgetBytes >> 20, stats.usedBytes >> 20, stats.overflowBytes >> 20, stats.otherBytes >> 20, stats.outsideBytes >> 20, stats.allowanceBytes >> 20, stats.wantedBytes >> 20, stats.visibleBytes >> 20, stats.distancePullback, stats.visiblePullback);
				logger::info("[TextureStreaming] left to the engine: {} loads, {} of them failed", stats.passedThrough, stats.engineFailures);
				logger::info("[TextureStreaming] builds: {} first windows (read {} ms, create {} ms in all), {} later (read {} ms, create {} ms in all)", stats.baseBuilds, stats.baseReadMicroseconds / 1000,
					stats.baseCreateMicroseconds / 1000, stats.otherBuilds, stats.otherReadMicroseconds / 1000, stats.otherCreateMicroseconds / 1000);
				logger::info("[TextureStreaming] render graph windows: {} made, installed in {} frames, {} built on D3D11 instead", stats.graphWindows, stats.graphInstallFrames, stats.graphRetried);
				return true;
			}
			if (a_command.rfind("dclf:tex-force=", 0) == 0) {
				TextureStreaming::ForceTop(std::atoi(a_command.c_str() + 15));
				return true;
			}
			if (a_command == "dclf:tex-dump" || a_command.rfind("dclf:tex-dump=", 0) == 0) {
				const auto name = a_command.size() > 14 ? a_command.substr(14) : std::string("dump");
				if (const auto directory = SKSE::log::log_directory())
					TextureStreaming::RequestDump((*directory / std::format("texture-{}.tsv", name)).string());
				Features::Streaming::ReportBindings();
				return true;
			}
			if (a_command.rfind("dclf:tex-offset=", 0) == 0) {
				auto settings = TextureStreaming::GetSettings();
				settings.visibleOffset = static_cast<float>(std::atof(a_command.c_str() + 16));
				TextureStreaming::SetSettings(settings);
				return true;
			}
			if (a_command.rfind("dclf:tex-budget=", 0) == 0) {
				auto settings = TextureStreaming::GetSettings();
				settings.budgetMB = static_cast<std::uint32_t>(std::atoi(a_command.c_str() + 16));
				TextureStreaming::SetSettings(settings);
				return true;
			}
			if (a_command.rfind("dclf:tex-distance=", 0) == 0 || a_command.rfind("dclf:tex-feedback=", 0) == 0) {
				auto settings = TextureStreaming::GetSettings();
				(a_command[9] == 'd' ? settings.distance : settings.feedback) = a_command.back() != '0';
				TextureStreaming::SetSettings(settings);
				return true;
			}
			if (a_command == "dclf:tex-census") {
				const auto path = TextureStreaming::Census::WriteJson(logger::log_directory().value_or(std::filesystem::path{}));
				logger::info("[DCLF] Texture census written to {}", path.string());
				return true;
			}
			return false;
		}
	}
}

namespace DCLF
{
	namespace
	{
		/**

namespace DCLF
{
	namespace
	{
		/** @brief What DCLF's waits test: the last reload asked for and the last NvPerf capture started. Render thread. */
		struct HarnessState
		{
			bool reloadAsked = false;
			std::uint64_t reloadSerial = 0, reloadGeneration = 0;  // the registry's before the reload was asked
			std::uint64_t nvperfCapture = 0;                         // 0: none started
		};
		HarnessState& Harness()
		{
			static HarnessState state;
			return state;
		}

		/**
		 * @brief Runs one "dclf:" command (a_command is the whole line, "dclf:<name>=<arg>"). Returns false with a_error set for a
		 * name DCLF does not know. A bad argument is logged and the command does nothing.
		 */
		bool RunDclfCommand(const std::string& a_command, std::string& a_error)
		{
			const std::string& command = a_command;
			// The command in a capture too: a timing run's phases are found in its Tracy capture by these zones.
			DCLF_ZONE("CS.DCLF.TestCommand");
			ZoneText(command.data(), command.size());
			// "dclf:actor-probe=<label>": the next scene pass logs how DCLF binds each actor's geometry (Coordinator::RequestActorProbe).
			if (command.rfind("dclf:actor-probe", 0) == 0) {
				Scene::Coordinator::Get().RequestActorProbe(command.size() > 17 ? command.substr(17) : std::string{});
				return true;
			}
			// "dclf:kill-scene-pump": the scene pump stopped for good, as a pass throwing past its handler stops it, for the watchdog
			// (DrawcallLimitFix::WatchScenePump) to hand the game back.
			if (command == "dclf:kill-scene-pump") {
				Scene::Coordinator::Get().ThrowNextScenePass();
				return true;
			}
			// "dclf:compat=0|1": the compatibility monitor off or on, as its menu checkbox; "dclf:compat-report": its report written.
			if (command == "dclf:compat=0" || command == "dclf:compat=1") {
				Diagnostics::PlayerParity::Request(command == "dclf:compat=1");
				return true;
			}
			if (command == "dclf:compat-report") {
				logger::info("[DCLF] test command: compatibility report {}", Diagnostics::PlayerParity::WriteReport());
				return true;
			}
			// "dclf:foreign-write=<n>": a foreign plugin's write, for the monitor to find: on the main thread, the alpha threshold of
			// the first alpha-tested geometry of up to n references near the player, stored straight into the property (no hook sees it).
			if (command.rfind("dclf:foreign-write=", 0) == 0) {
				const auto wanted = static_cast<std::uint32_t>(std::strtoul(command.c_str() + 19, nullptr, 10));
				if (auto* tasks = SKSE::GetTaskInterface())
					tasks->AddTask([wanted] {
						auto* tes = RE::TES::GetSingleton();
						auto* player = RE::PlayerCharacter::GetSingleton();
						if (!tes || !player)
							return;
						std::uint32_t written = 0;
						tes->ForEachReferenceInRange(player, 4096.0f, [&](RE::TESObjectREFR* a_reference) {
							auto* root = a_reference && a_reference != player ? a_reference->Get3D() : nullptr;
							if (!root)
								return RE::BSContainer::ForEachResult::kContinue;
							RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
								auto* alpha = a_geometry->GetGeometryRuntimeData().alphaProperty.get();
								if (!alpha)
									return RE::BSVisit::BSVisitControl::kContinue;
								alpha->alphaThreshold = static_cast<std::uint8_t>(alpha->alphaThreshold ^ 1);
								const auto* file = a_reference->GetFile(0);
								logger::info("[DCLF] test command: foreign write, alpha threshold of '{}' under reference {:08X} ({}) now {}", a_geometry->name.c_str(),
									a_reference->GetFormID(), file ? file->GetFilename() : "created in game", alpha->alphaThreshold);
								++written;
								return RE::BSVisit::BSVisitControl::kStop;
							});
							return written < wanted ? RE::BSContainer::ForEachResult::kContinue : RE::BSContainer::ForEachResult::kStop;
						});
					});
				return true;
			}
			// "dclf:rebuild": a graph rebuild requested (GraphHost::RequestRebuild, here at Present on the graph's owner
			// thread), which the next frame's build point performs: every recording invalidated, so that frame waits at its start for
			// a snapshot recorded on the new build (IndirectDraws::AdoptCurrentSnapshot; the strict epochs line counts the waits).
			// "dclf:refresh=<client id>": FrameAnchors::Refresh of that anchor client, which rebuilds its anchors' epochs alone (the
			// path a client changing its passes or resources takes), for stress runs.
			if (command.rfind("dclf:refresh=", 0) == 0) {
				FrameAnchors::Refresh(command.substr(13));
				return true;
			}
			if (command == "dclf:rebuild") {
				if (auto* host = RenderGraphRuntime::Get().Host())
					host->RequestRebuild("test harness command");
				else
					logger::warn("[DCLF] test command: no render graph host to rebuild");
				return true;
			}
			// Shader hot reload (dclf-hot-reload.md), served at the next present: "[ORG] shader reload: N of M pipelines rebuilt (K failed,
			// U unchanged) in S s" marks the rebuilds done, "[ORG] shader reload: N pipelines swapped in at graph build B" the swap.
			// "dclf:reload-shaders": every live pipeline, as the menu's button asks it (the unchanged ones stay). "=all": every one built
			// and swapped even when unchanged (the swap exercised without an edit). "=<file>": those built from that shader file, as the
			// shader watcher asks for an edit (an include: every one).
			if (command == "dclf:reload-shaders" || command.rfind("dclf:reload-shaders=", 0) == 0) {
				const std::string value = command.size() > 20 ? command.substr(20) : std::string{};
				// dclf:wait=reload's baseline: the registry's before this reload.
				if (const auto* registry = RenderGraphRuntime::PipelineRegistry()) {
					auto& harness = Harness();
					harness.reloadAsked = true;
					harness.reloadSerial = registry->LastReload().serial;
					harness.reloadGeneration = registry->Generation();
				}
				if (value == "all")
					RenderGraphRuntime::RequestShaderReload({}, true);
				else
					RenderGraphRuntime::RequestShaderReload(value);
				return true;
			}
			// "dclf:define=<NAME>[=<value>],...": extra defines for every DCLF stage compiled from now on (empty: none), taken up by the
			// next dclf:reload-shaders, so a shader A/B is an #if in the source, compared in one launch.
			if (command.rfind("dclf:define=", 0) == 0) {
				DCLF::ShaderPrograms::SetTestDefines(command.substr(12));
				return true;
			}
			// "dclf:gpu-window=<label>": the GPU event timers' frames from the next on are one window, logged as one block once read
			// back ("[GpuEvents] window '<label>'"); "-" is not logged. Needs CS_GPU_EVENT_TIMERS.
			if (command.rfind("dclf:gpu-window=", 0) == 0) {
				if (!GpuEventTimers::Requested())
					logger::warn("[DCLF] test command {}: the GPU event timers are off (CS_GPU_EVENT_TIMERS)", command);
				GpuEventTimers::StartWindow(command.substr(16));
				return true;
			}
			// "dclf:nvperf=<range>|<range>...[#<samples>]": an NvPerf capture of those ranges (a trailing '*' a prefix) from the next
			// frame, 3 samples by default; its rows are logged and appended to the CSV, and dclf:wait=nvperf waits for it.
			if (command.rfind("dclf:nvperf=", 0) == 0) {
				std::string value = command.substr(12);
				GpuCounters::CaptureConfiguration configuration;
				configuration.samples = 3;
				// "@<metric>[:<kind>],...": those metrics instead of the defaults (GpuCounters::ParseMetric's form).
				if (const auto at = value.rfind('@'); at != std::string::npos) {
					std::stringstream list(value.substr(at + 1));
					for (std::string metric; std::getline(list, metric, ',');) {
						GpuCounters::MetricRequest request;
						std::string error;
						if (GpuCounters::ParseMetric(metric, request, error))
							configuration.metrics.push_back(std::move(request));
						else
							logger::warn("[DCLF] test command: NvPerf metric '{}': {}", metric, error);
					}
					value.resize(at);
				}
				if (const auto hash = value.rfind('#'); hash != std::string::npos) {
					configuration.samples = std::max(1u, static_cast<std::uint32_t>(std::strtoul(value.c_str() + hash + 1, nullptr, 10)));
					value.resize(hash);
				}
				for (std::size_t start = 0; start <= value.size();) {
					const auto bar = value.find('|', start);
					auto range = value.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
					if (!range.empty())
						configuration.ranges.push_back(std::move(range));
					if (bar == std::string::npos)
						break;
					start = bar + 1;
				}
				std::uint64_t id = 0;
				std::string error;
				if (GpuCounters::RequestCapture(configuration, id, error)) {
					Harness().nvperfCapture = id;
					logger::info("[DCLF] test command: NvPerf capture {} of {} ranges, {} samples", id, configuration.ranges.size(), configuration.samples);
				} else
					logger::warn("[DCLF] test command {}: no capture: {}", command, error);
				return true;
			}
			// "dclf:ts-dump=<epochs>": the next <epochs> completed epochs log their graphics timestamp ranges in GPU order ("[TSDUMP]").
			if (command.rfind("dclf:ts-dump=", 0) == 0) {
				RenderGraphRuntime::RequestTimestampDump(static_cast<std::uint32_t>(std::max(0, std::atoi(command.c_str() + 13))));
				return true;
			}
			// "dclf:async-compute=0|1|default": the passes preferring async compute on the graphics queue (0), the compute queue (1)
			// or as CS_DCLF_ASYNC_COMPUTE says, from a graph rebuild on (dclf:rebuild's), so one run compares the queues.
			if (command.rfind("dclf:async-compute=", 0) == 0) {
				const std::string value = command.substr(19);
				DCLF::Draws::asyncComputeOverride.store(value == "default" ? -1 : value != "0", std::memory_order_relaxed);
				if (auto* host = RenderGraphRuntime::Get().Host())
					host->RequestRebuild("test harness command");
				return true;
			}
			// Cost attribution (diagnostics; a frozen camera, since a cut build leaves the draws reading an older frame's outputs):
			// "dclf:cut=<prefixes>": the pass key prefixes the graph leaves out, comma-separated ("*" every pass, empty none), from a
			// rebuild at the next build point (Draws::GraphConditions); "dclf:epochs=0|1": DCLF's epochs submitted or not (nothing they
			// claim is drawn while not); "dclf:early-flush=0|1|default": the D3D11 flush before each epoch.
			if (command.rfind("dclf:cut=", 0) == 0) {
				DCLF::Draws::SetPassCuts(command.substr(9));
				return true;
			}
			if (command.rfind("dclf:epochs=", 0) == 0) {
				// "dclf:epochs=flush|enqueue|signal": not submitted, each still closing DXVK's command list at its point, then
				// enqueuing an empty submission, or one signalling a timeline (RenderGraphRuntime::SkipDclfEpochs).
				const std::string value = command.substr(12);
				const std::uint32_t boundary = value == "flush" ? 1u : value == "enqueue" ? 2u : value == "signal" ? 3u : value == "cmd" ? 4u :
											   value == "cmd3" ? 5u : 0u;
				RenderGraphRuntime::SkipDclfEpochs(value == "1" ? 0u : ~0u, boundary);
				return true;
			}
			// "dclf:submit-dump=<epochs>": the next epochs log their enqueued submissions' waits and signals ("[ORG] submission dump").
			if (command.rfind("dclf:submit-dump=", 0) == 0) {
				RenderGraphRuntime::RequestSubmissionDump(static_cast<std::uint32_t>(std::strtoul(command.c_str() + 17, nullptr, 10)));
				return true;
			}
			// "dclf:worker-spin=<us>" / "dclf:worker-sync=0|1": each skipped epoch loads DXVK's worker thread that long, and/or
			// waits for it there (RenderGraphRuntime::SetSkippedEpochWorkerLoad).
			if (command.rfind("dclf:worker-spin=", 0) == 0 || command.rfind("dclf:worker-sync=", 0) == 0) {
				static std::uint32_t spinUs = 0;
				static bool sync = false;
				if (command[13] == 'p')
					spinUs = static_cast<std::uint32_t>(std::strtoul(command.c_str() + 17, nullptr, 10));
				else
					sync = command.substr(17) == "1";
				RenderGraphRuntime::SetSkippedEpochWorkerLoad(spinUs, sync);
				return true;
			}
			// "dclf:epoch-spin=<us>": each skipped epoch busy-waits that long on the render thread (0: none).
			if (command.rfind("dclf:epoch-spin=", 0) == 0) {
				RenderGraphRuntime::SetSkippedEpochSpin(static_cast<std::uint32_t>(std::strtoul(command.c_str() + 16, nullptr, 10)));
				return true;
			}
			// "dclf:pass-queries=0|1": the graph's lists recorded with their pass timestamp queries and Tracy GPU zones, or without.
			if (command.rfind("dclf:pass-queries=", 0) == 0) {
				org::diagnostics::DiagnosticSkipPassQueries().store(command.substr(18) == "0", std::memory_order_relaxed);
				if (auto* host = RenderGraphRuntime::Get().Host())
					host->RequestRebuild("test harness command");
				return true;
			}
			// "dclf:drop-lists=0|1": DCLF's epochs submit their signals alone (RenderGraphRuntime::DropDclfLists).
			if (command.rfind("dclf:drop-lists=", 0) == 0) {
				RenderGraphRuntime::DropDclfLists(command.substr(16) == "1");
				return true;
			}
			// "dclf:coalesce=0|1": each epoch's submissions as one (RenderGraphRuntime::CoalesceEpochSubmits; unsafe: empty epochs only).
			if (command.rfind("dclf:coalesce=", 0) == 0) {
				RenderGraphRuntime::CoalesceEpochSubmits(command.substr(14) == "1");
				return true;
			}
			// "dclf:split-enqueue=0|1": each epoch submission its own enqueue (DXVK's submission trace times each).
			if (command.rfind("dclf:split-enqueue=", 0) == 0) {
				RenderGraphRuntime::CoalesceEpochSubmits(false, command.substr(19) == "1");
				return true;
			}
			// "dclf:submitted-spin=<us>": each submitted DCLF epoch busy-waits that long on the render thread after its enqueue.
			if (command.rfind("dclf:submitted-spin=", 0) == 0) {
				RenderGraphRuntime::SetSkippedEpochSpin(static_cast<std::uint32_t>(std::strtoul(command.c_str() + 20, nullptr, 10)), true);
				return true;
			}
			// "dclf:hold=<shadow,z,colour,forward,fp>": those epochs' submissions go with the next epoch's enqueue (empty: none held).
			if (command.rfind("dclf:hold=", 0) == 0) {
				using Segment = RenderGraphRuntime::Segment;
				const std::string list = "," + command.substr(10) + ",";
				std::uint32_t mask = 0;
				for (const auto& [name, segment] : { std::pair{ "shadow", Segment::ShadowView }, std::pair{ "z", Segment::ZPrepass },
						 std::pair{ "colour", Segment::MainOpaque }, std::pair{ "forward", Segment::Forward }, std::pair{ "fp", Segment::FirstPerson } })
					if (list.find(std::string(",") + name + ",") != std::string::npos)
						mask |= 1u << static_cast<std::uint32_t>(segment);
				RenderGraphRuntime::HoldDclfSubmissions(mask);
				return true;
			}
			// "dclf:heap-dedupe=0|1": BasicRHI's descriptor heap binds of the heaps a list already bound emit nothing, from a rebuild.
			if (command.rfind("dclf:heap-dedupe=", 0) == 0) {
				rhi::DiagnosticDedupeHeapBinds().store(command.substr(17) == "1", std::memory_order_relaxed);
				if (auto* host = RenderGraphRuntime::Get().Host())
					host->RequestRebuild("test harness command");
				return true;
			}
			// "dclf:skip-epochs=<shadow,z,colour,forward,fp>": those epochs alone not submitted (empty: every one submitted).
			if (command.rfind("dclf:skip-epochs=", 0) == 0) {
				using Segment = RenderGraphRuntime::Segment;
				const std::string list = "," + command.substr(17) + ",";
				std::uint32_t mask = 0;
				for (const auto& [name, segment] : { std::pair{ "shadow", Segment::ShadowView }, std::pair{ "z", Segment::ZPrepass },
						 std::pair{ "colour", Segment::MainOpaque }, std::pair{ "forward", Segment::Forward }, std::pair{ "fp", Segment::FirstPerson } })
					if (list.find(std::string(",") + name + ",") != std::string::npos)
						mask |= 1u << static_cast<std::uint32_t>(segment);
				RenderGraphRuntime::SkipDclfEpochs(mask);
				return true;
			}
			// "dclf:list-heaps=0|1": recordings bind the descriptor heaps at each command list's start, or not (unsafe off: only
			// passes binding their own heaps then work; timing an empty-epoch run only), from a rebuild.
			if (command.rfind("dclf:list-heaps=", 0) == 0) {
				org::diagnostics::DiagnosticSkipListHeapBinds().store(command.substr(16) == "0", std::memory_order_relaxed);
				if (auto* host = RenderGraphRuntime::Get().Host())
					host->RequestRebuild("test harness command");
				return true;
			}
			// "dclf:list-barriers=external|all|default": recordings leave out their full entry and exit barriers (the closed
			// execution's too) or every barrier (unsafe: empty-epoch timing only), from a rebuild.
			if (command.rfind("dclf:list-barriers=", 0) == 0) {
				const std::string value = command.substr(19);
				org::diagnostics::DiagnosticSkipListBarriers().store(value == "external" ? 1 : value == "all" ? 2 : 0, std::memory_order_relaxed);
				if (auto* host = RenderGraphRuntime::Get().Host())
					host->RequestRebuild("test harness command");
				return true;
			}
			// "dclf:rhi-globals=0|1": BasicRHI records global barriers or none (the upload lists' too; unsafe off: empty-epoch
			// timing only), recordings from a rebuild.
			if (command.rfind("dclf:rhi-globals=", 0) == 0) {
				rhi::DiagnosticSkipGlobalBarriers().store(command.substr(17) == "0", std::memory_order_relaxed);
				if (auto* host = RenderGraphRuntime::Get().Host())
					host->RequestRebuild("test harness command");
				return true;
			}
			// "dclf:queue-boundary=0|1": the graph's external entry and exit barriers (unsafe off: timing only), from a rebuild.
			if (command.rfind("dclf:queue-boundary=", 0) == 0) {
				const bool on = command.substr(20) != "0";
				if (auto* host = RenderGraphRuntime::Get().Host())
					host->SetQueueBoundary({ .entry = on, .exit = on });
				return true;
			}
			if (command.rfind("dclf:early-flush=", 0) == 0) {
				const std::string value = command.substr(17);
				RenderGraphRuntime::SetEarlyFlush(value == "default" ? -1 : value != "0");
				return true;
			}
			// "dclf:shadow-min-texels=<texels>": the sun cascades' size cull (0: off), from the next shadow commit.
			if (command.rfind("dclf:shadow-min-texels=", 0) == 0) {
				DCLF::Draws::shadowMinTexels.store(std::strtof(command.c_str() + 23, nullptr), std::memory_order_relaxed);
				return true;
			}
			// "dclf:shadow-probe=<label>": the sun views' size probe, the readback pinned to that shadow view label; -1 off.
			if (command.rfind("dclf:shadow-probe=", 0) == 0) {
				DCLF::Draws::shadowSizeProbe.store(std::atoi(command.c_str() + 18), std::memory_order_relaxed);
				return true;
			}
			// "dclf:cull=off|frustum|occlusion": the GPU culling's mode (CS_DCLF_CULL's live value).
			if (command.rfind("dclf:cull=", 0) == 0) {
				const std::string value = command.substr(10);
				DCLF::Toggles::Get().Requested().cullMode = value == "occlusion" ? 2 : value == "frustum" ? 1 : 0;
				return true;
			}
			// The CPU-driven pipeline's toggles (CS_DCLF_PIPELINE's, CS_DCLF_CPU_RECORD's and CS_DCLF_CPU_CADENCE's live values):
			// "dclf:pipeline=auto|manual|gpu|cpu" (ApplyPipeline; from the next build point: it changes the graph's conditions),
			// "dclf:pipeline-interior=gpu|cpu" and "dclf:pipeline-exterior=gpu|cpu" (Manual's), "dclf:cpu-record=indirect|serial|parallel",
			// "dclf:cpu-cadence-<view>=frame|snapshot" ("dclf:cpu-cadence=frame|snapshot": every view but main), "dclf:cpu-batch=<lanes>"
			// (CS_DCLF_CPU_BATCH's: from the next snapshot).
			if (command.rfind("dclf:pipeline=", 0) == 0) {
				if (!DCLF::ApplyPipeline(DCLF::Toggles::Get().Requested(), command.substr(14)))
					logger::warn("[DCLF] test command {}: not a pipeline (auto, manual, gpu, cpu)", command);
				return true;
			}
			if (command.rfind("dclf:pipeline-interior=", 0) == 0 || command.rfind("dclf:pipeline-exterior=", 0) == 0) {
				auto& requested = DCLF::Toggles::Get().Requested();
				const bool cpu = command.substr(command.find('=') + 1) == "cpu";
				(command[14] == 'i' ? requested.interiorCpu : requested.exteriorCpu) = cpu;
				return true;
			}
			if (command.rfind("dclf:cpu-batch=", 0) == 0) {
				DCLF::Draws::SetBatchLanes(static_cast<std::uint32_t>(std::strtoul(command.c_str() + 15, nullptr, 10)));
				return true;
			}
			// "dclf:cpu-exp=<bits>": the CPU pipeline's performance A/B bits (Direct::Experiment), decimal or 0x hex.
			if (command.rfind("dclf:cpu-exp=", 0) == 0) {
				DCLF::Direct::ExperimentBits().store(static_cast<std::uint32_t>(std::strtoul(command.c_str() + 13, nullptr, 0)), std::memory_order_relaxed);
				return true;
			}
			if (command.rfind("dclf:cpu-record=", 0) == 0) {
				const auto record = command.substr(16);
				DCLF::Toggles::Get().Requested().cpuRecord = record == "parallel" ? 1 : record == "serial" ? 0 : 2;
				return true;
			}
			if (command.rfind("dclf:cpu-cadence", 0) == 0) {
				const auto equals = command.find('=');
				const std::string view = command.size() > 16 && command[16] == '-' ? command.substr(17, equals == std::string::npos ? std::string::npos : equals - 17) : std::string();
				const std::string value = equals == std::string::npos ? std::string() : command.substr(equals + 1);
				if (!DCLF::ApplyCpuCadence(DCLF::Toggles::Get().Requested(), view.empty() ? value : view + "=" + value))
					logger::warn("[DCLF] test command: '{}' names an unknown view or cadence", command);
				return true;
			}
			if (RunTextureStreamingCommand(command))
				return true;
			// "dclf:<toggle name>=<0|1>": a live toggle (Toggles.h, ToggleInfo::name), as the menu sets it, so one run can
			// compare a class drawn by DCLF and by the engine from the same camera.
			if (command.rfind("dclf:", 0) == 0) {
				const auto equals = command.find('=');
				const auto name = command.substr(5, equals == std::string::npos ? std::string::npos : equals - 5);
				const bool on = equals != std::string::npos && command.substr(equals + 1) != "0";
				bool found = false;
				for (const auto& info : DCLF::ToggleTable())
					if (info.name == name) {
						DCLF::SetToggleValue(DCLF::Toggles::Get().Requested(), info, on);
						found = true;
					}
				if (!found)
					a_error = fmt::format("no DCLF command or toggle named '{}'", name);
				return found;
			}
			a_error = fmt::format("'{}' is not a DCLF command", command);
			return false;
		}
	}

	void RegisterTestCommands()
	{
		Testing::RegisterCommandPrefix("dclf:", [](const std::string& a_name, const std::string& a_arg, std::string& a_error) {
			return RunDclfCommand(a_arg.empty() ? a_name : a_name + "=" + a_arg, a_error);
		});
		// "reload": the last dclf:reload-shaders finished, and its rebuilt pipelines (if any) are swapped in.
		Testing::RegisterWait("reload", [] {
			auto& harness = Harness();
			const auto* registry = RenderGraphRuntime::PipelineRegistry();
			if (!registry || !harness.reloadAsked)
				return true;
			const auto reload = registry->LastReload();
			return reload.serial > harness.reloadSerial && (reload.rebuilt == 0 || registry->Generation() > harness.reloadGeneration);
		});
		// "settled": the frames draw under the requested toggles on the pipeline they choose; at once with the feature off.
		Testing::RegisterWait("settled", [] {
			if (globals::state->IsFeatureDisabled("DrawcallLimitFix"))
				return true;
			const auto& requested = DCLF::Toggles::Get().Requested();
			return DCLF::Toggles::Get().Frame() == requested &&
			       DCLF::Draws::GraphBuiltForCpu() == DCLF::Draws::CpuPipelineForCell(requested, DCLF::Draws::CurrentGraphConditions().interior);
		});
		// "nvperf": the last dclf:nvperf capture done or failed.
		Testing::RegisterWait("nvperf", [] {
			auto& harness = Harness();
			if (!harness.nvperfCapture)
				return true;
			const auto latest = GpuCounters::LatestResult();
			return latest && latest->id >= harness.nvperfCapture;
		});
		Testing::RegisterExitReport([] {
			const auto moves = DCLF::Extensions::TransformMoveStats();
			logger::info("[DCLF] transform moves at exit: {} spilled (allocated past the queue), {} dropped while no build drained", moves.spilled, moves.dropped);
		});
	}
}
