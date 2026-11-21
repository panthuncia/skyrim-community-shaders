// DCLF's test commands ("dclf:<name>=<arg>") and waits, registered with the test runner when DCLF installs.
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "TestHarness.h"
#include "Testing/Testing.h"
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
#include "Features/ScreenshotFeature.h"
#include "Features/Upscaling.h"
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
		/** @brief Runs one "dclf:<name>=<arg>" line. Returns false with a_error set for an unknown name. A bad argument is logged and ignored. */
		bool RunDclfCommand(const std::string& a_command, DCLF::ToggleSet& a_toggles, std::string& a_error)
		{
			const std::string& command = a_command;
			// Timing runs find their phases in the Tracy capture by this zone.
			DCLF_ZONE("CS.DCLF.TestCommand");
			ZoneText(command.data(), command.size());
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
			// "dclf:shadow-min-texels=<texels>" sets the sun cascades' size cull, 0 for off.
			if (command.rfind("dclf:shadow-min-texels=", 0) == 0) {
				DCLF::Draws::shadowMinTexels.store(std::strtof(command.c_str() + 23, nullptr), std::memory_order_relaxed);
				return true;
			}
			// "dclf:cull=off|frustum|occlusion" sets GPU culling's live mode.
			if (command.rfind("dclf:cull=", 0) == 0) {
				const std::string value = command.substr(10);
				a_toggles.cullMode = value == "occlusion" ? 2 : value == "frustum" ? 1 : 0;
				return true;
			}
			// CPU pipeline toggles. "dclf:pipeline=auto|manual|gpu|cpu" applies with the revision that carries it, and pipeline-interior and
			// pipeline-exterior set Manual's choice. Also cpu-record=serial|indirect and cpu-batch, which applies from the next snapshot.
			if (command.rfind("dclf:pipeline=", 0) == 0) {
				if (!DCLF::ApplyPipeline(a_toggles, command.substr(14)))
					logger::warn("[DCLF] test command {}: not a pipeline (auto, manual, gpu, cpu)", command);
				return true;
			}
			if (command.rfind("dclf:pipeline-interior=", 0) == 0 || command.rfind("dclf:pipeline-exterior=", 0) == 0) {
				const bool cpu = command.substr(command.find('=') + 1) == "cpu";
				(command[14] == 'i' ? a_toggles.interiorCpu : a_toggles.exteriorCpu) = cpu;
				return true;
			}
			if (command.rfind("dclf:cpu-batch=", 0) == 0) {
				DCLF::Draws::SetBatchLanes(static_cast<std::uint32_t>(std::strtoul(command.c_str() + 15, nullptr, 10)));
				return true;
			}
			if (command.rfind("dclf:cpu-record=", 0) == 0) {
				const auto record = command.substr(16);
				a_toggles.cpuRecord = record == "serial" ? 0 : 2;
				return true;
			}
			if (RunTextureStreamingCommand(command))
				return true;
			// "dclf:<toggle name>=<0|1>" sets a live toggle as the menu does.
			if (command.rfind("dclf:", 0) == 0) {
				const auto equals = command.find('=');
				const auto name = command.substr(5, equals == std::string::npos ? std::string::npos : equals - 5);
				const bool on = equals != std::string::npos && command.substr(equals + 1) != "0";
				bool found = false;
				for (const auto& info : DCLF::ToggleTable())
					if (info.name == name) {
						DCLF::SetToggleValue(a_toggles, info, on);
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
			// A toggle command edits a copy of the requested set. Committing writes the toggles' setting, so it takes effect with its
			// revision (plan step 10c). Other commands leave the set as it was, which is no event.
			auto toggles = DCLF::Toggles::Get().Requested();
			const bool ran = RunDclfCommand(a_arg.empty() ? a_name : a_name + "=" + a_arg, toggles, a_error);
			DCLF::Toggles::Get().Commit(toggles, "cs-test");
			return ran;
		});
		// "settled" waits until frames draw under the requested toggles. Immediate with the feature off.
		Testing::RegisterWait("settled", [] {
			if (globals::state->IsFeatureDisabled("DrawcallLimitFix"))
				return true;
			const auto& requested = DCLF::Toggles::Get().Requested();
			return DCLF::Toggles::Get().Frame() == requested &&
			       DCLF::Draws::GraphBuiltForCpu() == DCLF::Draws::CpuPipelineForCell(requested, DCLF::Draws::AttachedGraphConditions().interior);
		});
		Testing::RegisterExitReport([] {
			const auto moves = DCLF::Extensions::TransformMoveStats();
			logger::info("[DCLF] transform moves at exit: {} spilled (allocated past the queue), {} dropped while no build drained", moves.spilled, moves.dropped);
		});
	}
}
