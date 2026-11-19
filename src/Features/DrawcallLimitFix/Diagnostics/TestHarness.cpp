// DCLF's test commands ("dclf:<name>=<arg>") and waits, registered with the test runner when DCLF installs.
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "TestHarness.h"
#include "Testing/Testing.h"
#include "RenderGraph/BuildManager.h"
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
		// Texture streaming commands. tex-stats, tex-force=<top or -1>, tex-distance and tex-feedback=<0|1>, tex-budget=<MiB, 0 for driver>,
		// tex-offset=<mips>, tex-dump=<name> to texture-<name>.tsv, and tex-census, which needs CS_TEX_CENSUS=1.
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
		/** @brief The last reload asked for and the last NvPerf capture started, for DCLF's waits. Render thread. */
		struct HarnessState
		{
			bool reloadAsked = false;
			std::uint64_t reloadSerial = 0, reloadGeneration = 0;  // the registry's before the reload was asked
			std::uint64_t nvperfCapture = 0;                         // 0 for none started
		};
		HarnessState& Harness()
		{
			static HarnessState state;
			return state;
		}

		/** @brief Runs one "dclf:<name>=<arg>" line. Returns false with a_error set for an unknown name. A bad argument is logged and ignored. */
		bool RunDclfCommand(const std::string& a_command, std::string& a_error)
		{
			const std::string& command = a_command;
			// Timing runs find their phases in the Tracy capture by this zone.
			DCLF_ZONE("CS.DCLF.TestCommand");
			ZoneText(command.data(), command.size());
			// "dclf:actor-probe=<label>" makes the next scene pass log how DCLF binds each actor's geometry.
			if (command.rfind("dclf:actor-probe", 0) == 0) {
				Scene::Coordinator::Get().RequestActorProbe(command.size() > 17 ? command.substr(17) : std::string{});
				return true;
			}
			// "dclf:kill-scene-pump" stops the scene pump for good, as an escaped exception would, to test the watchdog.
			if (command == "dclf:kill-scene-pump") {
				Scene::Coordinator::Get().ThrowNextScenePass();
				return true;
			}
			// "dclf:compat=0|1" toggles the compatibility monitor. "dclf:compat-report" writes its report.
			if (command == "dclf:compat=0" || command == "dclf:compat=1") {
				Diagnostics::PlayerParity::Request(command == "dclf:compat=1");
				return true;
			}
			if (command == "dclf:compat-report") {
				logger::info("[DCLF] test command: compatibility report {}", Diagnostics::PlayerParity::WriteReport());
				return true;
			}
			// "dclf:foreign-write=<n>" fakes a foreign plugin's write for the monitor to find. It stores the alpha threshold of up to n
			// nearby alpha-tested geometries straight into the property on the main thread, where no hook sees it.
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
			// "dclf:rebuild" requests a graph rebuild at the next build point. That frame waits at its start for a snapshot on the new build.
			// "dclf:refresh=<client id>" refreshes one anchor client's epochs only, for stress runs.
			if (command.rfind("dclf:refresh=", 0) == 0) {
				FrameAnchors::Refresh(command.substr(13));
				return true;
			}
			if (command == "dclf:rebuild") {
				if (RenderGraphRuntime::Get().Host())
					RenderGraph::Builds::RequestAll("test harness command");
				else
					logger::warn("[DCLF] test command: no render graph host to rebuild");
				return true;
			}
			// Shader hot reload, served at the next present. Plain reloads every live pipeline and keeps the unchanged ones.
			// "=all" rebuilds and swaps every one even when unchanged. "=<file>" reloads those built from that file, or all for an include.
			if (command == "dclf:reload-shaders" || command.rfind("dclf:reload-shaders=", 0) == 0) {
				const std::string value = command.size() > 20 ? command.substr(20) : std::string{};
				// Baseline for dclf:wait=reload.
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
			// "dclf:define=<NAME>[=<value>],..." adds defines for DCLF stages compiled from now on, used by the next reload.
			// A shader A/B is then an #if in the source, compared in one launch.
			if (command.rfind("dclf:define=", 0) == 0) {
				DCLF::ShaderPrograms::SetTestDefines(command.substr(12));
				return true;
			}
			// "dclf:gpu-window=<label>" logs the GPU event timers from the next frame on as one window. "-" is not logged.
			// Needs CS_GPU_EVENT_TIMERS.
			if (command.rfind("dclf:gpu-window=", 0) == 0) {
				if (!GpuEventTimers::Requested())
					logger::warn("[DCLF] test command {}: the GPU event timers are off (CS_GPU_EVENT_TIMERS)", command);
				GpuEventTimers::StartWindow(command.substr(16));
				return true;
			}
			// "dclf:nvperf=<range>|<range>...[#<samples>]" captures those ranges with NvPerf from the next frame, 3 samples by default.
			// A trailing '*' makes a range a prefix. dclf:wait=nvperf waits for it.
			if (command.rfind("dclf:nvperf=", 0) == 0) {
				std::string value = command.substr(12);
				GpuCounters::CaptureConfiguration configuration;
				configuration.samples = 3;
				// "@<metric>[:<kind>],..." replaces the default metrics.
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
			// "dclf:ts-dump=<epochs>" logs the next completed epochs' timestamp ranges in GPU order.
			if (command.rfind("dclf:ts-dump=", 0) == 0) {
				RenderGraphRuntime::RequestTimestampDump(static_cast<std::uint32_t>(std::max(0, std::atoi(command.c_str() + 13))));
				return true;
			}
			// "dclf:async-compute=0|1|default" runs async-compute passes on the graphics queue, the compute queue, or as CS_DCLF_ASYNC_COMPUTE
			// says. Takes effect from a rebuild.
			if (command.rfind("dclf:async-compute=", 0) == 0) {
				const std::string value = command.substr(19);
				DCLF::Draws::asyncComputeOverride.store(value == "default" ? -1 : value != "0", std::memory_order_relaxed);
				RenderGraph::Builds::RequestAll("test harness command");
				return true;
			}
			// Cost attribution. Use a frozen camera, since a cut build leaves draws reading an older frame's outputs. "dclf:cut=<prefixes>" drops
			// pass key prefixes from the next rebuild. "dclf:epochs=0|1" submits DCLF's epochs or not. "dclf:early-flush" sets the pre-epoch flush.
			if (command.rfind("dclf:cut=", 0) == 0) {
				DCLF::Draws::SetPassCuts(command.substr(9));
				return true;
			}
			if (command.rfind("dclf:epochs=", 0) == 0) {
				// "dclf:epochs=flush|enqueue|signal" skips submission but still closes DXVK's command list, enqueues an empty submission, or signals.
				const std::string value = command.substr(12);
				const std::uint32_t boundary = value == "flush" ? 1u : value == "enqueue" ? 2u : value == "signal" ? 3u : value == "cmd" ? 4u :
											   value == "cmd3" ? 5u : 0u;
				RenderGraphRuntime::SkipDclfEpochs(value == "1" ? 0u : ~0u, boundary);
				return true;
			}
			// "dclf:submit-dump=<epochs>" logs the next epochs' submission waits and signals.
			if (command.rfind("dclf:submit-dump=", 0) == 0) {
				RenderGraphRuntime::RequestSubmissionDump(static_cast<std::uint32_t>(std::strtoul(command.c_str() + 17, nullptr, 10)));
				return true;
			}
			// "dclf:worker-spin=<us>" loads DXVK's worker for that long per skipped epoch. "dclf:worker-sync=0|1" waits for it there.
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
			// "dclf:epoch-spin=<us>" busy-waits on the render thread per skipped epoch.
			if (command.rfind("dclf:epoch-spin=", 0) == 0) {
				RenderGraphRuntime::SetSkippedEpochSpin(static_cast<std::uint32_t>(std::strtoul(command.c_str() + 16, nullptr, 10)));
				return true;
			}
			// "dclf:pass-queries=0|1" records the lists with or without pass timestamps and Tracy GPU zones.
			if (command.rfind("dclf:pass-queries=", 0) == 0) {
				org::diagnostics::DiagnosticSkipPassQueries().store(command.substr(18) == "0", std::memory_order_relaxed);
				RenderGraph::Builds::RequestAll("test harness command");
				return true;
			}
			// "dclf:drop-lists=0|1" makes DCLF's epochs submit only their signals.
			if (command.rfind("dclf:drop-lists=", 0) == 0) {
				RenderGraphRuntime::DropDclfLists(command.substr(16) == "1");
				return true;
			}
			// "dclf:coalesce=0|1" merges each epoch's submissions. Unsafe except with empty epochs.
			if (command.rfind("dclf:coalesce=", 0) == 0) {
				RenderGraphRuntime::CoalesceEpochSubmits(command.substr(14) == "1");
				return true;
			}
			// "dclf:split-enqueue=0|1" enqueues each epoch submission alone, so DXVK's trace times each.
			if (command.rfind("dclf:split-enqueue=", 0) == 0) {
				RenderGraphRuntime::CoalesceEpochSubmits(false, command.substr(19) == "1");
				return true;
			}
			// "dclf:submitted-spin=<us>" busy-waits on the render thread after each submitted epoch.
			if (command.rfind("dclf:submitted-spin=", 0) == 0) {
				RenderGraphRuntime::SetSkippedEpochSpin(static_cast<std::uint32_t>(std::strtoul(command.c_str() + 20, nullptr, 10)), true);
				return true;
			}
			// "dclf:hold=<shadow,z,colour,forward,fp>" sends those epochs' submissions with the next epoch's enqueue.
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
			// "dclf:heap-dedupe=0|1" makes BasicRHI skip rebinding heaps a list already bound, from a rebuild.
			if (command.rfind("dclf:heap-dedupe=", 0) == 0) {
				rhi::DiagnosticDedupeHeapBinds().store(command.substr(17) == "1", std::memory_order_relaxed);
				RenderGraph::Builds::RequestAll("test harness command");
				return true;
			}
			// "dclf:skip-epochs=<shadow,z,colour,forward,fp>" skips submitting those epochs only.
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
			// "dclf:list-heaps=0|1" binds descriptor heaps at each list's start or not, from a rebuild. Off is unsafe, for empty-epoch timing only.
			if (command.rfind("dclf:list-heaps=", 0) == 0) {
				org::diagnostics::DiagnosticSkipListHeapBinds().store(command.substr(16) == "0", std::memory_order_relaxed);
				RenderGraph::Builds::RequestAll("test harness command");
				return true;
			}
			// "dclf:list-barriers=external|all|default" drops the entry and exit barriers or every barrier, from a rebuild. Unsafe, timing only.
			if (command.rfind("dclf:list-barriers=", 0) == 0) {
				const std::string value = command.substr(19);
				org::diagnostics::DiagnosticSkipListBarriers().store(value == "external" ? 1 : value == "all" ? 2 : 0, std::memory_order_relaxed);
				RenderGraph::Builds::RequestAll("test harness command");
				return true;
			}
			// "dclf:rhi-globals=0|1" makes BasicRHI record global barriers or none, from a rebuild. Off is unsafe, timing only.
			if (command.rfind("dclf:rhi-globals=", 0) == 0) {
				rhi::DiagnosticSkipGlobalBarriers().store(command.substr(17) == "0", std::memory_order_relaxed);
				RenderGraph::Builds::RequestAll("test harness command");
				return true;
			}
			// "dclf:queue-boundary=0|1" toggles the graph's external entry and exit barriers, from a rebuild. Off is unsafe, timing only.
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
			// "dclf:shadow-min-texels=<texels>" sets the sun cascades' size cull, 0 for off.
			if (command.rfind("dclf:shadow-min-texels=", 0) == 0) {
				DCLF::Draws::shadowMinTexels.store(std::strtof(command.c_str() + 23, nullptr), std::memory_order_relaxed);
				return true;
			}
			// "dclf:shadow-probe=<label>" pins the sun size probe's readback to that shadow view label, -1 for off.
			if (command.rfind("dclf:shadow-probe=", 0) == 0) {
				DCLF::Draws::shadowSizeProbe.store(std::atoi(command.c_str() + 18), std::memory_order_relaxed);
				return true;
			}
			// "dclf:cull=off|frustum|occlusion" sets GPU culling's live mode.
			if (command.rfind("dclf:cull=", 0) == 0) {
				const std::string value = command.substr(10);
				DCLF::Toggles::Get().Requested().cullMode = value == "occlusion" ? 2 : value == "frustum" ? 1 : 0;
				return true;
			}
			// CPU pipeline toggles. "dclf:pipeline=auto|manual|gpu|cpu" applies at the next build point, and pipeline-interior and
			// pipeline-exterior set Manual's choice. Also cpu-record, cpu-cadence[-<view>] and cpu-batch, which applies from the next snapshot.
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
			// "dclf:cpu-exp=<bits>" sets the CPU pipeline's A/B bits, decimal or 0x hex.
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
			// "dclf:<toggle name>=<0|1>" sets a live toggle as the menu does, to compare DCLF and the engine from the same camera.
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
		// "reload" waits until the last reload finished and its pipelines are swapped in.
		Testing::RegisterWait("reload", [] {
			auto& harness = Harness();
			const auto* registry = RenderGraphRuntime::PipelineRegistry();
			if (!registry || !harness.reloadAsked)
				return true;
			const auto reload = registry->LastReload();
			return reload.serial > harness.reloadSerial && (reload.rebuilt == 0 || registry->Generation() > harness.reloadGeneration);
		});
		// "settled" waits until frames draw under the requested toggles. Immediate with the feature off.
		Testing::RegisterWait("settled", [] {
			if (globals::state->IsFeatureDisabled("DrawcallLimitFix"))
				return true;
			const auto& requested = DCLF::Toggles::Get().Requested();
			return DCLF::Toggles::Get().Frame() == requested &&
			       DCLF::Draws::GraphBuiltForCpu() == DCLF::Draws::CpuPipelineForCell(requested, DCLF::Draws::CurrentGraphConditions().interior);
		});
		// "nvperf" waits until the last capture is done or failed.
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
