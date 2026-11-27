// The feature shell: install, the engine hook thunks and their installation, the menu toggle and the settings UI.
// Lifecycle points forward to Draws/FrameDriver on the render thread.
#include "DrawcallLimitFix.h"
#include "DrawcallLimitFix/Diagnostics/Capture/Capture.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/PrecipitationOcclusion.h"
#include "Snapshot/Publication.h"

#include "Deferred.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "RenderGraph/Backend.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/BufferRefs.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Native/RenderDepth.h"
#include "RenderGraph/Engine/Native/RenderScene.h"
#include "RenderGraph/Engine/Native/WaterEffects.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "Features/DrawcallLimitFix/Api/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Engine/WindowWriters.h"
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameTicket.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Features/FeatureList.h"
#include "Features/DrawcallLimitFix/Features/Water/Port/WaterPort.h"
#include "Features/DrawcallLimitFix/Integrations/IntegrationList.h"
#include "Features/DrawcallLimitFix/Schedule/Schedule.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"
#include "DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "DrawcallLimitFix/Diagnostics/Hooks.h"
#include "DrawcallLimitFix/Diagnostics/PlayerParity.h"
#include "DrawcallLimitFix/Diagnostics/Reports/FrameSummary.h"
#include "DrawcallLimitFix/Core/Shading/MaterialWrites.h"
#include "DrawcallLimitFix/Core/Shading/MaterialSources.h"
#include "DrawcallLimitFix/Diagnostics/TelemetryProfile.h"
#include "DrawcallLimitFix/Diagnostics/TestHarness.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DeclaredTargets.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/SetupPrograms.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/Skylighting.h"
#include "ShaderCache.h"
#include "TextureStreaming/Streamer.h"

#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"
#include "DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatches.h"
#include "DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "DrawcallLimitFix/Diagnostics/ReplayCapture/ReplayCapture.h"
#include "DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Render/LoadHold.h"
#include "DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "RenderGraph/MemoryWindow.h"
#include "RenderGraph/ScheduleWindow.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/GraphHost.h>
#include "State.h"
#include "DrawcallLimitFix/Core/Engine/Capture/ConstantMirror.h"
#include "DrawcallLimitFix/Diagnostics/Scene/CaptureParity.h"

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"

#include <ORGModuleServices/BackingPool.h>

namespace
{
	// Every engine hook and offset is an Engine::Offset. Site positions inside functions are only checked against SE 1.5.97, AE 1.6.1170
	// and AE 1.7.104, and another build may move them. SceneWriterFramework has tables for the same three.
	bool SupportedGame()
	{
		return DCLF::Engine::Supported();
	}
}

void DrawcallLimitFix::PostLoad()
{
	// SceneWriterFramework's election, only when PostPostLoad will install. Without CS's, the engine's stores stay unpatched unless another plugin's SWF elects.
	if (SupportedGame() && RenderGraph::Engine::GraphMode() && DCLF::SwitchValue(DCLF::Switch::Dclf) != "0")
		DCLF::SceneWriter::Elect();
}

void DrawcallLimitFix::DataLoaded()
{
	// Patches another plugin rewrote after SWF's install, each logged with its module, kept for the menu.
	DCLF::SceneWriter::CheckIntegrity();
	DCLF::DeclaredTargets::InitShadows();
}

void DrawcallLimitFix::StartPipelinePrecompile()
{
	if (!installed || !RenderGraphRuntime::Get().IsActive())
		return;
	// The shadow keys need the shadow map format and the Utility shader, which exist once the data has loaded. DataLoaded's
	// later call then does nothing.
	DCLF::DeclaredTargets::InitShadows();
	DCLF::DrawPipelines::Get().StartPrecompile();
}

DrawcallLimitFix::PipelinePrecompileProgress DrawcallLimitFix::GetPipelinePrecompileProgress() const
{
	if (!installed)
		return {};
	const auto progress = DCLF::DrawPipelines::Get().GetPrecompileProgress();
	return { progress.pending, progress.total, progress.finished, progress.failed };
}

namespace
{
	// The scene writer framework's provider, its families and the integrity check's findings (the menu).
	void DrawSceneWriterPanel()
	{
		if (!ImGui::TreeNodeEx(T("feature.dclf.scene_writer.title", "Scene writer framework")))
			return;
		ImGui::TextWrapped(T("feature.dclf.scene_writer.provider", "Provider: %s"), DCLF::SceneWriter::ProviderLine().c_str());
		ImGui::TextWrapped(T("feature.dclf.scene_writer.families", "Hook families: %s"), DCLF::SceneWriter::CapsLine().c_str());
		const auto& changes = DCLF::SceneWriter::IntegrityChanges();
		if (!DCLF::SceneWriter::IntegrityChecked())
			ImGui::TextUnformatted(T("feature.dclf.scene_writer.integrity_pending", "Integrity: not checked yet (it runs once the game's data has loaded)."));
		else if (changes.empty())
			ImGui::TextUnformatted(T("feature.dclf.scene_writer.integrity_ok", "Integrity: every patch is as installed."));
		else {
			ImGui::TextWrapped(T("feature.dclf.scene_writer.integrity_changed",
				"Integrity: %zu patch(es) rewritten by another plugin after install. A later hook that calls on still reaches SWF's; "
				"one that does not hides those writes from DCLF."),
				changes.size());
			for (const auto& change : changes)
				ImGui::BulletText(T("feature.dclf.scene_writer.integrity_change", "%s +0x%x: now leads to %s"), change.family.c_str(), change.offset, change.module.empty() ? T("feature.dclf.scene_writer.no_module", "no module") : change.module.c_str());
		}
		ImGui::TreePop();
	}
}

void DrawcallLimitFix::PostPostLoad()
{
	if (!SupportedGame()) {
		const auto game = REL::Module::get().version();
		unavailableReason = fmt::format("DCLF supports Skyrim SE 1.5.97, AE 1.6.1170 and AE 1.7.104 only (this game is {})", game.string("."));
		logger::info("[DCLF] {}; the game renders natively", unavailableReason);
		RenderGraph::Engine::Native::RequireDclf(unavailableReason);
		return;
	}
	// DCLF draws only through the render graph. In plain D3D11 mode (CS_ORG=0) it is not installed.
	if (!RenderGraph::Engine::GraphMode()) {
		unavailableReason = "DCLF draws only through the render graph, and this session runs without it";
		logger::info("[DCLF] not installed: the session runs without the render graph; the game renders natively");
		return;
	}
	// CS_DCLF=0 is the baseline, with nothing installed. Unset means on. Graph mode fails at its switch check before this.
	if (DCLF::SwitchValue(DCLF::Switch::Dclf) == "0") {
		logger::info("[DCLF] CS_DCLF=0; the feature stays off and the game renders natively");
		return;
	}
	// The patch sites DCLF cannot run without, checked before any hook is written. Another plugin's patch at one leaves the game
	// native rather than half hooked. Optional sites warn and skip at their own install.
	if (const auto missing = DCLF::SceneWriter::MissingReason(); !missing.empty()) {
		unavailableReason = missing;
		logger::warn("[DCLF] {}; the game renders natively", unavailableReason);
		RenderGraph::Engine::Native::RequireDclf(unavailableReason);
		return;
	}
	for (auto missing : { DCLF::PassCapture::SitesMissing() })
		if (!missing.empty()) {
			unavailableReason = fmt::format("{} DCLF hooks are not the engine's code (another plugin patched them first)", missing);
			logger::warn("[DCLF] {}; the game renders natively", unavailableReason);
			RenderGraph::Engine::Native::RequireDclf(unavailableReason);
			return;
		}
	// Every other site DCLF's hooks write. A chained hook needs a CALL there, and one that skips or replaces the engine's code needs the engine's own bytes.
	// A conflict leaves the game native rather than half hooked.
	Hooks::Preflight();
	DCLF::Extensions::PreflightFeatures(DCLF::Features::List());
	if (const auto conflicts = DCLF::HookSites::Conflicts(); !conflicts.empty()) {
		unavailableReason = fmt::format("its hooks conflict with another plugin's ({})", conflicts);
		logger::error("[DCLF] {}; DCLF stays off and the game renders natively", unavailableReason);
		RenderGraph::Engine::Native::RequireDclf(unavailableReason);
		return;
	}
	logger::info("[DCLF] hook preflight: {} sites checked, no conflicts", DCLF::HookSites::Checked());
	DCLF::SceneTracker::Get().Install();
	DCLF::Scene::InstallSceneEvents();
	// Where the set's members' passes are withheld from the views DCLF draws.
	DCLF::PassCapture::Get().Install();
	// After every load, the loading menu stays up until DCLF draws the load's objects.
	DCLF::LoadHold::Install();
	// The features' early engine hooks, which must go in before DCLF's own engine side reads what they change,
	// then the observers this run's switches ask for. All before DCLF's threads start.
	DCLF::Extensions::InstallEarlyHooks(DCLF::Features::List());
	DCLF::Diagnostics::Install();
	DCLF::MaterialWrites::Install();
	// Built now, so the render thread's first frame does not pay for it.
	(void)DCLF::MaterialSources::FramePSFloats();
	(void)DCLF::MaterialSources::FrameVSFloats();
	// The shadow sources' hooks, source by source. The point culls write NiNode's OnVisible slot before the primary cull reads it.
	DCLF::Extensions::InstallSourceHooks(DCLF::Features::List());
	DCLF::PrimaryCull::Get().Install();
	// The features' extension points and engine hooks, then the integrations'. Registered before DCLF's threads start and read-only after.
	DCLF::Extensions::InstallFeatures(DCLF::Features::List(), DCLF::Integrations::List(), DCLF::Schedule::Frame());
	// DCLF's scene is a fragment source of the frame revision, like any other client. Its segments become lane-only build epochs
	// when the render graph initializes.
	DCLF::RegisterSceneSource();
	// The schedule's epoch orders, handed to the render graph before it initializes.
	{
		const auto& orders = DCLF::Extensions::ScheduledFrameOrders();
		RenderGraphRuntime::ConfigureFrame({ orders.epochOrder, orders.revisionEpochs });
	}
	// DCLF's threads and state graph, before the first frame dispatches a job.
	(void)DCLF::SceneScheduler::Graph();
	Hooks::Install();
	installed = true;
	// DCLF's test commands and waits exist only when it installs.
	DCLF::RegisterTestCommands();
	// The menu's saved toggles, now that every toggle is seeded. Set switches win.
	ApplySavedSettings();
	// The coordinator's pump, from now on woken by its producers. Its passes only apply events until the first frame runs DCLF.
	DCLF::Scene::Coordinator::Get().SetScenePassMode(false);
	DCLF::Scene::Coordinator::Get().StartScenePump();
	// Log the switches this process actually sees, once, since a silent log looks the same as a switch that never reached the game.
	logger::info("[DCLF] switches: {}", DCLF::SwitchSummary());
	if (const auto reduced = DCLF::ReducedFeatures(); reduced.empty())
		logger::info("[DCLF] featureset: full (every feature on)");
	else
		logger::warn("[DCLF] featureset: REDUCED by {}; this run does not exercise DCLF's full featureset", reduced);
	// A hook that passed preflight but failed to attach. The others are already written, and graph mode needs DCLF, so this is fatal.
	if (const auto failed = DCLF::HookSites::Conflicts(); !failed.empty()) {
		unavailableReason = fmt::format("its hooks did not all install ({})", failed);
		logger::error("[DCLF] {}", unavailableReason);
		RenderGraph::Engine::Native::RequireDclf(unavailableReason);
	}
}

void DrawcallLimitFix::SetupResources()
{
	// On D3D12 native mode DCLF draws with pulled builds and plain indirect draws only. Anything without a D3D12 port is turned off
	// by the switches native mode sets, each named in its log line.
	const bool d3d12 = ::RenderGraph::Backend::IsD3D12();
	if (installed && RenderGraphRuntime::Get().IsActive())
		DCLF::ShaderPrograms::Get().StartPrecompile();
	// Runs right after the render graph tried to adopt DXVK's device. The hooks went in before any device existed.
	// Without the graph or indirect pipelines there is nothing to draw with, and graph mode needs DCLF, so that is fatal.
	if (!installed)
		return;
	auto& graph = RenderGraphRuntime::Get();
	if (!graph.IsActive())
		unavailableReason = graph.GetDisabledReason();
	else if (!graph.Host())
		unavailableReason = "the render graph has no host";
	else if (!DCLF::DrawPipelines::Get().Enabled())
		unavailableReason = "the GPU or driver lacks the indirect pipelines DCLF draws with (see the [DCLF] Indirect pipelines line)";
	if (!unavailableReason.empty()) {
		logger::error("[DCLF] {}", unavailableReason);
		RenderGraph::Engine::Native::RequireDclf(unavailableReason);
		stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}", unavailableReason));
	}
	// The device's indirect-command features, read once at DrawPipelines' setup, decide plain draws on D3D12. No switch is set for it.
	if (d3d12)
		logger::info("[DCLF] D3D12: on, the GPU pipeline; colour draws with {}",
			DCLF::DrawPipelines::Get().DeviceGenerated() ? "device-generated commands" : "plain indirect draws (the device has no pipeline sets)");
	// The frame-time setups read these, so CS's setup join builds them before the first frame.
	DCLF::SetupPrograms::Queue();
}

void DrawcallLimitFix::BeforePresent()
{
	if (installed)
		DCLF::FrameDriver::Get().BeforePresent();
}

void DrawcallLimitFix::Reset()
{
	// The telemetry profile's frame (with DCLF on or off).
	DCLF::Diagnostics::TelemetryFrame();
	// The diagnostic capture's steps (with DCLF on or off: a capture of a session it could not start is the most useful one).
	DCLF::Capture::OnPresent();
	// Every Present, menus included. The coordinator's queues hold references to attached subtrees and must not grow while the world is not rendered.
	if (!installed)
		return;
	// A pass capture's dump, before the frame's end retires what it drew with (CS_DCLF_CAPTURE).
	DCLF::ReplayCapture::OnFrameEnd();
	{
		DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::Present);
		DCLF::FrameDriver::Get().EndFrame();
	}
	DCLF::RenderThreadBudget::Get().EndFrame();
}

Feature::BootToggleLock DrawcallLimitFix::GetBootToggleLock() const
{
	if (!RenderGraph::Engine::GraphMode())
		return {};
	return { T("feature.dclf.graph_mode_locked", "Locked by the render graph"),
		T("feature.dclf.graph_mode_locked_tooltip",
			"The render graph draws the world through Drawcall Limit Fix, so it cannot be turned off while the game runs. "
			"A change saved in the settings applies at the next launch.") };
}

bool DrawcallLimitFix::ToggleAtBootSetting()
{
	if (GetBootToggleLock().note) {
		logger::info("[DCLF] the on/off toggle is locked in graph mode; nothing changed");
		return globals::state->IsFeatureDisabled(GetShortName());
	}
	return Feature::ToggleAtBootSetting();
}

void DrawcallLimitFix::Hooks::Main_Update_Entry::thunk(void* a_stages)
{
	func(a_stages);
	if (globals::features::drawcallLimitFix.installed)
		DCLF::PrimaryCull::Get().AtUpdateEntry();
}

std::int64_t DrawcallLimitFix::Hooks::Main_Draw_Early::thunk(void* a_main)
{
	// Native runs Main::SetupSunAndSky's game half in the capture (Native/WaterEffects.h), and draws the water work the capture queued
	// here, where Main::Draw would have called RenderWaterEffects: nothing in between draws in native.
	// A routed update's game half (Native::Scene::PrepareFrame) makes DCLF's ticket only: its render half and the water work run at the
	// render's start (Native::Scene::RecordFrame).
	namespace Water = RenderGraph::Engine::Native::WaterEffects;
	if (Water::Owns()) {
		if (RenderGraph::Engine::Native::Scene::InGameHalf()) {
			if (globals::features::drawcallLimitFix.installed)
				DCLF::FrameDriver::Get().SceneGameStart();
		} else {
			globals::features::drawcallLimitFix.BeginSceneFrame();
			Water::Render();
		}
		return static_cast<std::int64_t>(Water::SunAndSkyResult());
	}
	const auto result = func(a_main);
	globals::features::drawcallLimitFix.BeginSceneFrame();
	return result;
}

void DrawcallLimitFix::BeginSceneFrame()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::SceneFrame);
	WatchScenePump();
	DCLF::FrameDriver::Get().SceneFrameStart();
}

void DrawcallLimitFix::OnGraphEpochFailed(std::string_view a_epoch, const std::string& a_error)
{
	// A failed epoch draws nothing, and DCLF would wait for it forever. Graph mode draws the world through DCLF, so it cannot hand drawing back.
	const auto failure = fmt::format("its render graph epoch '{}' failed: {}", a_epoch, a_error);
	logger::error("[DCLF] {}", failure);
	stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}. Graph mode draws the world through DCLF, so it cannot switch off", failure));
}

void DrawcallLimitFix::WatchScenePump()
{
	// Every frame wakes a scene pass, and a pass ends a frame or two behind. Frames going on with no pass ending for long means the pump
	// stopped, so the builder would never publish again. That is fatal. The bound is time, not frames: the render rate varies (repeated
	// snapshots render faster than the game updates), and it leaves room for a slow pass at a load.
	constexpr auto kLagTime = std::chrono::seconds(30);
	const auto frame = DCLF::Scene::FrameView::Get().SceneStep();
	const auto done = DCLF::IndirectDraws::Get().ScenePassDoneFrame();
	std::string reason;
	if (DCLF::Scene::Coordinator::Get().ScenePumpFailed())
		reason = "the scene pump stopped (see the [DCLF] error above)";
	else if (frame > done) {
		const auto now = std::chrono::steady_clock::now();
		if (!std::exchange(pumpLagging, true) || std::exchange(pumpLagDone, done) != done)
			pumpLagSince = now;
		else if (now - pumpLagSince > kLagTime)
			reason = fmt::format("no scene pass has ended for {:.0f} s (the last for frame {}, now {})", std::chrono::duration<double>(now - pumpLagSince).count(), done, frame);
	} else
		pumpLagging = false;
	if (reason.empty())
		return;
	// Graph mode draws the world through DCLF, so it cannot hand drawing back (D-3).
	logger::error("[DCLF] {}", reason);
	stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}. Graph mode draws the world through DCLF, so it cannot switch off", reason));
}

void DrawcallLimitFix::BeforeShadowMaps()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::BeforeShadowMaps);
	if (installed)
		DCLF::FrameDriver::Get().BeforeShadowMaps();
}

void DrawcallLimitFix::AfterShadowMaps()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::AfterShadowMaps);
	if (installed)
		DCLF::FrameDriver::Get().AfterShadowMaps();
}

void DrawcallLimitFix::EarlyPrepass()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::EarlyPrepass);
	if (installed)
		DCLF::FrameDriver::Get().EarlyPrepass();
}

bool DrawcallLimitFix::OcclusionReady(OcclusionMap a_map)
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::Occlusion);
	static_assert(kSkyOcclusion == static_cast<std::uint32_t>(DCLF::Api::OcclusionMap::Sky) &&
				  kPrecipitationOcclusion == static_cast<std::uint32_t>(DCLF::Api::OcclusionMap::Precipitation));
	return installed && DCLF::FrameDriver::Get().OcclusionReady(a_map);
}

bool DrawcallLimitFix::OcclusionHeld(OcclusionMap a_map) const
{
	return installed && DCLF::FrameDriver::Get().OcclusionHeld(a_map);
}

bool DrawcallLimitFix::LoadHoldsPresents() const
{
	return installed && DCLF::LoadHold::PresentsHeld();
}

void DrawcallLimitFix::OcclusionView(OcclusionMap a_map)
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::Occlusion);
	// The map's camera, from its entry.
	const auto& map = DCLF::Extensions::OcclusionMapOf(a_map);
	if (!installed || !map.camera)
		return;
	DCLF::Api::OcclusionCamera camera;
	map.camera(camera);
	if (!camera.valid)
		return;
	DCLF::IndirectDraws::Get().OcclusionView(a_map, camera.world, camera.frustum, camera.port);
}

bool DrawcallLimitFix::OcclusionNeedsEngine(OcclusionMap a_map)
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::Occlusion);
	return !installed || DCLF::FrameDriver::Get().OcclusionNeedsEngine(a_map);
}

namespace
{
	/**
	 * @brief Guest: the precipitation object Main::Precipitation::RenderOcclusion (AE FUN_1404076c0, SE FUN_1403ae920) renders the mask for:
	 * the current rain or snow, else the last, while the occlusion map is on. Null when the engine renders none. Native reads the capture's
	 * test (Snapshot::PrecipitationOcclusion::Block::engineMask).
	 */
	RE::BSGeometry* EngineMaskObject()
	{
		static REL::Relocation<const bool*> maskOn{ REL::RelocationID(502538, 370126) };
		auto* sky = CS_ENGINE_READ(Sky, RecordTime, globals::game::sky);
		auto* precip = sky ? sky->precip : nullptr;
		if (!precip || !*maskOn)
			return nullptr;
		const auto falls = [](RE::TESWeather* a_weather) {
			using Data = RE::BGSShaderParticleGeometryData;
			auto* data = a_weather ? a_weather->precipitationData : nullptr;
			if (!data)
				return false;
			const auto type = data->GetSettingValue(Data::DataID::kParticleType).i;
			return type == static_cast<std::uint32_t>(Data::ParticleType::kRain) || type == static_cast<std::uint32_t>(Data::ParticleType::kSnow);
		};
		if (!falls(sky->currentWeather) && !falls(sky->lastWeather))
			return nullptr;
		return precip->currentPrecip ? precip->currentPrecip.get() : precip->lastPrecip.get();
	}

	/** @brief Native: whether the engine would render the mask this update, from the capture. */
	bool NativeEngineMask()
	{
		const auto* block = Snapshot::PrecipitationOcclusion::Current();
		return block && block->engineMask && block->mask.valid;
	}
}

void DrawcallLimitFix::RenderPrecipitationMask(RE::Precipitation* a_precip, RE::BSGeometry* a_precipObject, bool a_dclf)
{
	// When DCLF draws the mask it draws every occluder from its own tables in the shadow epoch, so SetupMask is skipped
	// and RenderMask only sets the camera and clears the mask. Otherwise the engine does it all.
	// Held by a load, so the engine's point renders it without its cull, as DCLF's variant does.
	const bool held = !a_dclf && OcclusionHeld(kPrecipitationOcclusion);
	if (!a_dclf && !held) {
		a_precip->SetupMask();
	} else {
		// What SetupMask sets: the projection and the accumulator's camera.
		static REL::Relocation<void(RE::Precipitation*, RE::NiPointer<RE::NiCamera>)> computeProjection{ REL::RelocationID(25643, 26185) };
		computeProjection(a_precip, a_precip->occlusionData.camera);
		if (auto* accumulator = a_precip->occlusionData.accumulator.get())
			accumulator->camera = a_precip->occlusionData.camera.get();
	}
	OcclusionView(kPrecipitationOcclusion);
	auto* particleShaderProperty = netimmerse_cast<RE::BSParticleShaderProperty*>(a_precipObject->GetGeometryRuntimeData().shaderProperty.get());
	auto* rain = (RE::BSParticleShaderRainEmitter*)(particleShaderProperty->particleEmitter);

	globals::profiler->BeginPass("Skylighting::PrecipMask");
	a_precip->RenderMask(rain);
	globals::profiler->EndPass();
}

void DrawcallLimitFix::RenderPrecipitationAtShadowMaps()
{
	// Skylighting draws the maps when loaded. With the shader cache off, the engine renders the mask whole.
	if (globals::features::skylighting.loaded || !globals::shaderCache->IsEnabled())
		return;
	// Native: the capture placed the mask's camera and set the rain emitter's projection, so only DCLF's view is left to set up.
	const bool native = RenderGraph::Engine::Native::Active();
	auto* object = native ? nullptr : EngineMaskObject();
	if (!(native ? NativeEngineMask() : object != nullptr) || !OcclusionReady(kPrecipitationOcclusion))
		return;
	auto state = globals::state;
	TracyD3D11Zone(state->tracyCtx, "Precipitation Mask (Drawcall Limit Fix)");
	state->BeginPerfEvent("Precipitation Mask (Drawcall Limit Fix)");
	if (native) {
		OcclusionView(kPrecipitationOcclusion);
		state->EndPerfEvent();
		return;
	}
	// As Skylighting does, before the engine's shadow maps reset the renderer's state. Restore the camera they begin with,
	// since EarlyPrepasses reads its eye after them. The mask's viewport is not scaled by dynamic resolution.
	auto& shadowState = CS_ENGINE_READ(ShadowState, RecordTime, globals::game::shadowState)->GetRuntimeData();
	const auto posAdjust = shadowState.posAdjust;
	const auto previousPosAdjust = shadowState.previousPosAdjust;
	const auto cameraData = shadowState.cameraData;
	auto& graphicsState = CS_ENGINE_READ(GraphicsState, RecordTime, globals::game::graphicsState)->GetRuntimeData();
	const auto dynamicResolutionLock = graphicsState.dynamicResolutionLock;
	graphicsState.dynamicResolutionLock = 1;
	RenderPrecipitationMask(CS_ENGINE_READ(Sky, RecordTime, globals::game::sky)->precip, object, true);
	graphicsState.dynamicResolutionLock = dynamicResolutionLock;
	shadowState.posAdjust = posAdjust;
	shadowState.previousPosAdjust = previousPosAdjust;
	shadowState.cameraData = cameraData;
	state->EndPerfEvent();
}

void DrawcallLimitFix::Hooks::Main_Precipitation_RenderOcclusion::thunk()
{
	// DCLF drew the mask in this frame's shadow epoch, so it is done. Otherwise the engine renders it whole.
	auto& dclf = globals::features::drawcallLimitFix;
	if (RenderGraph::Engine::Native::Active()) {
		// Native makes no engine render call: a map DCLF did not draw this frame has no occluders, as a held map has. The capture placed
		// its camera.
		if (NativeEngineMask() && dclf.OcclusionNeedsEngine(kPrecipitationOcclusion))
			dclf.OcclusionView(kPrecipitationOcclusion);
		return;
	}
	if (!globals::shaderCache->IsEnabled() || !EngineMaskObject() || dclf.OcclusionNeedsEngine(kPrecipitationOcclusion))
		func();
}

void DrawcallLimitFix::Prepass()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::Prepass);
	if (installed)
		DCLF::FrameDriver::Get().Prepass();
}

void DrawcallLimitFix::Hooks::Main_RenderDepth::thunk(bool a_firstPerson, bool a_a2)
{
	auto& passes = DCLF::ShellState::Passes();
	passes.inDepthPass = true;
	passes.depthControlDriven = a_a2;
	// Native drives Main::RenderDepth's work itself; the engine's function is not entered.
	if (RenderGraph::Engine::Native::Active())
		RenderGraph::Engine::Native::RenderDepth(a_firstPerson, a_a2);
	else
		func(a_firstPerson, a_a2);
	passes.inDepthPass = false;
}

void DrawcallLimitFix::Hooks::Main_RenderDepth_WorldDrawn::thunk(void* a_accumulator, bool a_a2)
{
	func(a_accumulator, a_a2);
	if (!DCLF::ShellState::Passes().inDepthPass)
		return;
	globals::features::drawcallLimitFix.DepthPassDrawn();
	// DCLF's epoch leaves the context's bindings to the engine's state tracking, so rebind its targets for the rest of the pass.
	CS_ENGINE_READ(Shaders, CapturePoint, globals::game::stateUpdateFlags)->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void DrawcallLimitFix::DepthPassDrawn()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::ZPrepass);
	// The world's depth draws are done and its camera is still current. The kPOST_ZPREPASS_COPY at the end of the pass will include
	// DCLF's objects, and first-person depth goes in after the world's as it does natively, so nothing needs refreshing.
	DCLF::FrameDriver::Get().DepthCommitted();
}

template <int N>
void DrawcallLimitFix::Hooks::BSBatchRenderer_RenderPassImmediately<N>::thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	DCLF::Diagnostics::Notify(DCLF::Diagnostics::NativePassOffered{ a_pass, a_technique, DCLF::ShellState::Passes().inDepthPass });
	func(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

void DrawcallLimitFix::Hooks::BSShaderAccumulator_FinishAccumulating::thunk(RE::BSGraphics::BSShaderAccumulator* a_accumulator, std::uint32_t a_renderFlags)
{
	func(a_accumulator, a_renderFlags);
	const auto mode = static_cast<std::uint32_t>(a_accumulator->GetRuntimeData().renderMode);
	if (mode < 0xD || mode > 0xF)
		return;  // shadow-map modes only: plain, clamped, paraboloid
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::ShadowViewCapture);
	const auto view = DCLF::ShadowViews::Get().ViewOfAccumulator(a_accumulator);
	if (view != ~0u)
		DCLF::IndirectDraws::Get().CaptureShadowView(view, mode);
}

namespace
{
	// Hooks::Install's sites. Preflight checks the same ones.
	std::uintptr_t DepthPassSite() { return REL::RelocationID(35560, 36559).address() + Util::VersionedRelocation::Select(0x395, 0x395, 0x3B3); }
	std::uintptr_t WorldDrawnSite() { return REL::RelocationID(100421, 107139).address() + REL::Relocate(0x18E, 0x1AA); }
	std::uintptr_t ScenePhaseSite() { return REL::RelocationID(35560, 36559).address() + 0xD3; }
	std::uintptr_t PrecipitationMaskSite() { return REL::RelocationID(35560, 36559).address() + Util::VersionedRelocation::Select(0x3A1, 0x3A1, 0x3BF); }
}

void DrawcallLimitFix::Hooks::Preflight()
{
	// Chained, so another plugin's target, such as Terrain Blending's or ShadowBoost's, runs before or around DCLF's thunk.
	// DCLF cannot run without any of these: the depth pass and its commit point, the frame's start, and Main::Update's entry.
	DCLF::HookSites::RequireCall("Main::Draw's depth pass call", DepthPassSite());
	DCLF::HookSites::RequireCall("Main::RenderDepth's world-drawn call", WorldDrawnSite());
	DCLF::HookSites::RequireCall("Main::Draw's first call", ScenePhaseSite());
	DCLF::HookSites::RequireCall("Main::Update's first call", REL::Offset(DCLF::Engine::Address::kUpdateEntryCall).address());
	// Not chained. On frames DCLF drew the mask the engine's call is skipped, and any hook another plugin put there with it.
	// Skylighting, which drives the mask itself, takes the call instead.
	if (!globals::features::skylighting.loaded)
		DCLF::HookSites::RequireCallTo("Main::Draw's precipitation mask call", PrecipitationMaskSite());
	DCLF::WindowWriters::Preflight();
	DCLF::PassCapture::Preflight();
	DCLF::PrimaryCull::Preflight();
}

void DrawcallLimitFix::Hooks::Install()
{
	stl::write_vfunc<0x2A, BSShaderAccumulator_FinishAccumulating>(RE::VTABLE_BSShaderAccumulator[0]);
	logger::info("[DCLF] shadow view hook installed on BSShaderAccumulator::FinishAccumulatingPreResolveDepth");
	// The diagnostics' sites may already be redirected by another plugin, so only check for a CALL rel32, as write_thunk_call needs.
	const auto isCall = [](std::uintptr_t a_site) { return *reinterpret_cast<const std::uint8_t*>(a_site) == 0xE8; };
	// The main camera's depth pass, hooked where Terrain Blending hooks it.
	stl::write_thunk_call<Main_RenderDepth>(DepthPassSite());
	// Main::RenderDepth's call of RenderFirstPersonView_End, SE +0x18E and AE +0x1AA.
	stl::write_thunk_call<Main_RenderDepth_WorldDrawn>(WorldDrawnSite());
	logger::info("[DCLF] Z-prepass hook installed inside Main::RenderDepth, after the world's depth draws");
	// Light Limit Fix's RenderPass3 site, the plain pass list loop: SE FUN_1413090c0, AE FUN_1414f4f20.
	if (const auto site = REL::RelocationID(100871, 107667).address() + REL::Relocate(0xEE, 0xED); isCall(site))
		stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<1>>(site);
	if (const auto site = REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F); isCall(site))
		stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<2>>(site);
	// A geometry group's pass list (AE FUN_1414f19d0), where terrain LOD's colour passes are drawn. AE inlines the plain-list loop there.
	// SE calls FUN_1413090c0 instead, which the RenderPassImmediately<1> hook already covers, so there is no SE address. A diagnostics observer.
	if (DCLF::Engine::Address::kGeometryGroupDraw.Known()) {
		if (const auto site = REL::Offset(DCLF::Engine::Address::kGeometryGroupDraw).address(); isCall(site))
			stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<3>>(site);
		else
			logger::warn("[DCLF] the geometry groups' pass draw is not a call; the diagnostics do not see their passes");
	}
	stl::write_thunk_call<Main_Draw_Early>(ScenePhaseSite());
	logger::info("[DCLF] scene phase hook installed on Main::Draw");
	// The stood-in roots' kAccumulated, written at Main::Update's entry, chained so ShadowBoost's hook and the engine's run first.
	// Without this hook nothing takes the decoder's write lists, which stalls the retirement chain and keeps every released engine reference.
	if (const auto site = REL::Offset(DCLF::Engine::Address::kUpdateEntryCall).address(); !DCLF::Engine::CallsTo(site, REL::Offset(DCLF::Engine::Address::kStagesReset).address()))
		logger::info("[DCLF] Main::Update's first call is hooked by {}; DCLF's entry hook chains onto it", DCLF::HookSites::BranchTargetModule(site));
	stl::write_thunk_call<Main_Update_Entry>(REL::Offset(DCLF::Engine::Address::kUpdateEntryCall).address());
	// The engine's precipitation mask, without Skylighting, which hooks the same call and drives the mask itself.
	if (!globals::features::skylighting.loaded) {
		stl::write_thunk_call<Main_Precipitation_RenderOcclusion>(PrecipitationMaskSite());
		logger::info("[DCLF] precipitation mask hook installed on Main::Draw (without Skylighting)");
	}
	// Keeps the engine's writers inside the engine-read window out of DCLF's reads there.
	DCLF::WindowWriters::Install();
	// The game update is over at its capture, so a detach still waiting for a re-attach is a removal. Pushed on the game thread.
	Snapshot::RegisterCapture([](Snapshot::Builder&) { DCLF::SceneTracker::Get().PushHeldDetaches(); });
	// Native: the water port's game values, after the water update the capture ran.
	Snapshot::RegisterCapture(&DCLF::Features::Water::Port::CaptureGame);
	// A world update the cadence renders no frame for still makes its frame-start ticket, which then retires unused.
	Snapshot::RegisterUnrenderedUpdate([](const Snapshot::FrameSnapshot& a_snapshot) {
		if (globals::features::drawcallLimitFix.installed && a_snapshot.kind == Snapshot::FrameKind::World)
			DCLF::FrameDriver::Get().UnrenderedUpdate(a_snapshot.id);
	});
	// The update's game-side end, after its renders.
	Snapshot::RegisterUpdateEnd([](const Snapshot::FrameSnapshot&) {
		if (globals::features::drawcallLimitFix.installed)
			DCLF::FrameDriver::Get().UpdateEnd();
	});
	logger::info("[DCLF] Native pass hooks installed");
}

void DrawcallLimitFix::OnNativeGameHalfStart()
{
	if (installed)
		DCLF::FrameDriver::Get().GameHalfStart();
}

void DrawcallLimitFix::ExpectNativeTicket(std::shared_ptr<const void> a_ticket)
{
	if (installed)
		DCLF::FrameDriver::Get().ExpectTicket(std::static_pointer_cast<const DCLF::FrameTicket>(std::move(a_ticket)));
}

void DrawcallLimitFix::BeginNativeRenderHalf()
{
	if (!installed)
		return;
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::SceneFrame);
	WatchScenePump();
	DCLF::FrameDriver::Get().SceneRenderStart();
}

std::shared_ptr<const void> DrawcallLimitFix::NativeGameTicket() const
{
	return installed ? DCLF::FrameDriver::Get().GameTicket() : nullptr;
}

void DrawcallLimitFix::OnNativeGameHalfEnd()
{
	if (installed)
		DCLF::FrameDriver::Get().GameHalfEnd();
}

void DrawcallLimitFix::OnNativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
{
	if (installed)
		DCLF::FrameDriver::Get().NativeLightingDraw(a_pass, a_renderFlags);
}

void DrawcallLimitFix::BeforeOpaquePass()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::BeforeOpaque);
	if (installed)
		DCLF::FrameDriver::Get().BeforeOpaquePass();
}

void DrawcallLimitFix::AfterOpaquePass()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::AfterOpaque);
	if (installed)
		DCLF::FrameDriver::Get().AfterOpaquePass();
}

#define I18N_KEY_PREFIX "feature.drawcall_limit_fix."
namespace
{
	/** @brief The rule violations a release build kept running past (Frame::Driver::ViolationWarnings), until the game exits. */
	void DrawViolationWarning()
	{
		const auto kinds = RenderGraph::Frame::Driver::ViolationWarnings();
		if (kinds.empty())
			return;
		std::string list;
		for (const auto& kind : kinds)
			list += fmt::format("{}{}", list.empty() ? "" : ", ", kind);
		const auto folder = logger::log_directory();
		const auto& theme = Menu::GetSingleton()->GetTheme();
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(theme.StatusPalette.Warning, "%s",
			T(TKEY("violation_title"), "Community Shaders found a rendering error and kept the game running. Some effects may be missing until you restart."));
		ImGui::TextColored(theme.StatusPalette.Warning, "%s",
			I18n::GetSingleton()->Format(TKEY("violation_kinds"), { { "kinds", list } }, "Error: {kinds}").c_str());
		ImGui::TextColored(theme.StatusPalette.Warning, "%s",
			I18n::GetSingleton()
				->Format(TKEY("violation_logs"), { { "folder", folder ? folder->string() : std::string() } },
					"Please send your logs to the Community Shaders developers: CommunityShaders.log, which also holds the DCLF log, from the SKSE "
					"log folder ({folder}). This warning stays until you exit the game.")
				.c_str());
		ImGui::PopTextWrapPos();
		ImGui::Separator();
	}
}
#undef I18N_KEY_PREFIX

void DrawcallLimitFix::DrawSettings()
{
	DrawViolationWarning();
	if (!unavailableReason.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.3f, 1.0f));
		ImGui::TextUnformatted(T("feature.dclf.not_installed_session", "Not installed this session: everything renders natively."));
		ImGui::PopStyleColor();
		ImGui::TextWrapped(T("feature.dclf.not_installed_reason", "Reason: %s"), unavailableReason.c_str());
		DCLF::Capture::DrawControls();
		DrawSceneWriterPanel();
		return;
	}
	if (!installed) {
		ImGui::TextUnformatted(T("feature.dclf.not_installed_switch", "Not installed (CS_DCLF=0)."));
		DCLF::Capture::DrawControls();
		return;
	}
	// Developer tools, under a header a player can leave closed.
	const auto drawTools = [] {
		if (ImGui::Button(T("feature.dclf.tools.reload_shaders", "Reload shaders")))
			RenderGraphRuntime::RequestShaderReload();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T("feature.dclf.tools.reload_shaders_tooltip",
				"Builds every live render graph pipeline (DCLF's graphics pipelines, every compute program) again from Data/Shaders as it is now. The "
				"current pipelines draw until every graph is built again with the new ones; one that fails to compile keeps its previous "
				"pipeline (the log names the error)."));
		ImGui::SameLine();
		if (ImGui::Button(T("feature.dclf.tools.schedule", "Render graph schedule")))
			RenderGraphSchedule::OpenWindow();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T("feature.dclf.tools.schedule_tooltip",
				"Opens the persistent schedule inspector: each executable's batches per queue, their passes, the barriers before each pass and "
				"every cross-queue wait with its cause. The graph traces its schedule while the window is open."));
		ImGui::SameLine();
		if (ImGui::Button(T("feature.dclf.tools.vram", "VRAM attribution")))
			RenderGraphMemory::OpenWindow();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T("feature.dclf.tools.vram_tooltip",
				"Opens VRAM attribution: every D3D11 resource (DXVK's) and render graph resource (DCLF's tables, pools and targets among them) "
				"with its owner and the size of its allocation, reconciled against the driver's heap usage."));
	};
	// The player's capture to send back comes first, because a problem report needs it.
	DCLF::Capture::DrawControls();
	DCLF::Diagnostics::PlayerParity::DrawPanel();
	DrawSceneWriterPanel();
	// The live toggles, for A/B comparisons without a restart. A copy of the requested set. An edit is committed below to the
	// toggles' setting, and takes effect with the revision that carries it.
	auto toggles = DCLF::Toggles::Get().Requested();
	if (ImGui::TreeNodeEx(T("feature.dclf.draws.title", "How DCLF draws"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextWrapped("%s", T("feature.dclf.draws.intro", "Each takes effect at the next frame, and Save Settings keeps it."));
		int cull = toggles.cullMode;
		if (ImGui::Combo(T("feature.dclf.draws.gpu_culling", "GPU culling"), &cull, "off\0frustum\0frustum + occlusion\0"))
			toggles.cullMode = static_cast<std::uint8_t>(cull);
		// The pipeline choice and the CPU-driven pipeline's options. Save Settings keeps the pipeline choice. The CPU options are seeded by their switches only.
		int pipelineMode = toggles.pipelineMode == DCLF::PipelineMode::Manual ? 1 : 0;
		if (ImGui::Combo(T("feature.dclf.draws.pipeline", "Pipeline"), &pipelineMode, "Auto\0Manual\0"))
			toggles.pipelineMode = pipelineMode == 1 ? DCLF::PipelineMode::Manual : DCLF::PipelineMode::Auto;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T("feature.dclf.draws.pipeline_tooltip",
				"GPU-driven: the draws culled and recorded on the GPU. CPU-driven: culled on DCLF's worker threads and drawn with direct draws "
				"recorded into DXVK's command buffer. Auto draws exteriors GPU-driven and interiors CPU-driven; Manual chooses for each. Takes "
				"effect at the next graph build (entering or leaving an interior builds it); Save Settings keeps it (CS_DCLF_PIPELINE seeds it)."));
		if (toggles.pipelineMode == DCLF::PipelineMode::Manual) {
			ImGui::Indent();
			int interior = toggles.interiorCpu ? 1 : 0, exterior = toggles.exteriorCpu ? 1 : 0;
			if (ImGui::Combo(T("feature.dclf.draws.interiors", "Interiors"), &interior, "GPU-driven\0CPU-driven\0"))
				toggles.interiorCpu = interior == 1;
			if (ImGui::Combo(T("feature.dclf.draws.exteriors", "Exteriors"), &exterior, "GPU-driven\0CPU-driven\0"))
				toggles.exteriorCpu = exterior == 1;
			ImGui::Unindent();
		}
		ImGui::Text(T("feature.dclf.draws.now", "Now: %s-driven (%s)"), DCLF::Draws::AttachedGraphConditions().cpuPipeline ? "CPU" : "GPU",
			DCLF::Draws::AttachedGraphConditions().interior ? T("feature.dclf.draws.interior", "interior") : T("feature.dclf.draws.exterior", "exterior"));
		if (DCLF::Draws::CpuPipelineForced())
			ImGui::TextWrapped("%s", T("feature.dclf.draws.cpu_forced", "This GPU or driver has no counted indirect draws: every cell draws CPU-driven."));
		// The CPU-driven pipeline's options, while it draws some kind of cell.
		if (toggles.pipelineMode == DCLF::PipelineMode::Auto || toggles.interiorCpu || toggles.exteriorCpu) {
			ImGui::Indent();
			// The combo lists serial then indirect. The toggle stores 0 and 2.
			int record = toggles.cpuRecord == 0 ? 0 : 1;
			if (ImGui::Combo(T("feature.dclf.draws.cpu_recording", "CPU recording"), &record, "serial\0indirect\0"))
				toggles.cpuRecord = static_cast<std::uint8_t>(record == 0 ? 0 : 2);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T("feature.dclf.draws.cpu_recording_tooltip",
					"Indirect (default): the workers cull and write each batch's draws into a buffer, and DXVK's CS thread records one indirect draw per pipeline run. "
					"Serial: the workers cull, and DXVK's CS thread records every batch's draws. CS_DCLF_CPU_RECORD seeds it."));
			// Applied when changed, from the next snapshot. Each change rebuilds the view batches.
			int lanes = static_cast<int>(DCLF::Draws::BatchLanes());
			if (ImGui::InputInt(T("feature.dclf.draws.batch_lanes", "Batch lanes"), &lanes, 256, 1024, ImGuiInputTextFlags_EnterReturnsTrue))
				DCLF::Draws::SetBatchLanes(static_cast<std::uint32_t>(std::max(lanes, 8)));
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T("feature.dclf.draws.batch_lanes_tooltip",
					"The draws a culled batch holds (a multiple of 8, 8 to 65528; default 4096). More coalesce more draws into each batch; "
					"fewer spread a view's culls over more jobs. Takes effect from the next "
					"scene snapshot; CS_DCLF_CPU_BATCH seeds it."));
			ImGui::Unindent();
		}
		for (const auto& toggle : DCLF::ToggleTable()) {
			if (!toggle.section.empty())
				ImGui::SeparatorText(toggle.section.data());
			if (bool value = DCLF::ToggleValue(toggles, toggle); ImGui::Checkbox(toggle.label, &value))
				DCLF::SetToggleValue(toggles, toggle, value);
			// The tooltip names the toggle's switch, which overrides the saved setting at startup when set.
			if (auto _tt = Util::HoverTooltipWrapper()) {
				if (toggle.tooltip)
					ImGui::TextUnformatted(toggle.tooltip);
				const auto name = DCLF::SwitchName(toggle.seed);
				const auto& seed = DCLF::SwitchValue(toggle.seed);
				ImGui::TextDisabled("%.*s%s", static_cast<int>(name.size()), name.data(), seed.empty() ? "" : fmt::format(" = {} (set: it seeds this toggle, not the saved setting)", seed).c_str());
			}
		}
		// Writing the same set is no event, so this commits only an edit.
		DCLF::Toggles::Get().Commit(toggles, "menu");
		ImGui::TreePop();
	}
	if (!ImGui::TreeNodeEx(T("feature.dclf.advanced", "Advanced")))
		return;
	drawTools();
	if (ImGui::TreeNodeEx(T("feature.dclf.this_frame", "This frame"), ImGuiTreeNodeFlags_DefaultOpen)) {
		// The first line, the active toggles, is long so it wraps. The rest are short.
		const auto lines = DCLF::Reports::FrameSummary();
		for (std::size_t i = 0; i < lines.size(); ++i) {
			if (i == 0)
				ImGui::TextWrapped("%s", lines[i].c_str());
			else
				ImGui::TextUnformatted(lines[i].c_str());
		}
		ImGui::TreePop();
	}
	ImGui::TreePop();
}

void DrawcallLimitFix::SaveSettings(json& o_json)
{
	// Not installed, so nothing was seeded and what was loaded is kept as is.
	if (!installed) {
		o_json = savedSettings;
		return;
	}
	const auto& requested = DCLF::Toggles::Get().Requested();
	json toggles = json::object();
	for (const auto& toggle : DCLF::ToggleTable())
		toggles[std::string(toggle.name)] = DCLF::ToggleValue(requested, toggle);
	o_json["Toggles"] = std::move(toggles);
	// Save the cull mode only when not the default, so a later default reaches settings saved before it.
	if (requested.cullMode != DCLF::DefaultCullMode())
		o_json["CullMode"] = requested.cullMode;
	else
		o_json.erase("CullMode");
	o_json["Pipeline"] = requested.pipelineMode == DCLF::PipelineMode::Manual ? "manual" : "auto";
	o_json["InteriorPipeline"] = requested.interiorCpu ? "cpu" : "gpu";
	o_json["ExteriorPipeline"] = requested.exteriorCpu ? "cpu" : "gpu";
	o_json["CompatibilityMonitor"] = DCLF::Diagnostics::PlayerParity::Requested();
	savedSettings = o_json;
}

void DrawcallLimitFix::LoadSettings(json& o_json)
{
	savedSettings = o_json;
	if (installed)
		ApplySavedSettings();
}

void DrawcallLimitFix::ApplySavedSettings()
{
	if (!savedSettings.is_object())
		return;
	auto requested = DCLF::Toggles::Get().Requested();
	std::string applied;
	if (const auto toggles = savedSettings.find("Toggles"); toggles != savedSettings.end() && toggles->is_object())
		for (const auto& toggle : DCLF::ToggleTable()) {
			const auto& seed = DCLF::SwitchValue(toggle.seed);
			const auto saved = toggles->find(std::string(toggle.name));
			if (!seed.empty() || saved == toggles->end() || !saved->is_boolean() || saved->get<bool>() == DCLF::ToggleValue(requested, toggle))
				continue;
			DCLF::SetToggleValue(requested, toggle, saved->get<bool>());
			applied += fmt::format("{}{} {}", applied.empty() ? "" : ", ", toggle.name, saved->get<bool>() ? 1 : 0);
		}
	if (const auto cull = savedSettings.find("CullMode"); cull != savedSettings.end() && cull->is_number_unsigned() && DCLF::SwitchValue(DCLF::Switch::Cull).empty() &&
														  cull->get<std::uint32_t>() <= 2 && cull->get<std::uint8_t>() != requested.cullMode) {
		requested.cullMode = cull->get<std::uint8_t>();
		applied += fmt::format("{}cull {}", applied.empty() ? "" : ", ", requested.cullMode);
	}
	// The pipeline choice, unless CS_DCLF_PIPELINE set it.
	if (DCLF::SwitchValue(DCLF::Switch::CpuPipeline).empty()) {
		const auto stringOf = [&](const char* a_key) {
			const auto found = savedSettings.find(a_key);
			return found != savedSettings.end() && found->is_string() ? found->get<std::string>() : std::string();
		};
		const auto mode = stringOf("Pipeline"), interior = stringOf("InteriorPipeline"), exterior = stringOf("ExteriorPipeline");
		const auto before = std::tuple(requested.pipelineMode, requested.interiorCpu, requested.exteriorCpu);
		if (mode == "auto" || mode == "manual")
			requested.pipelineMode = mode == "manual" ? DCLF::PipelineMode::Manual : DCLF::PipelineMode::Auto;
		if (interior == "cpu" || interior == "gpu")
			requested.interiorCpu = interior == "cpu";
		if (exterior == "cpu" || exterior == "gpu")
			requested.exteriorCpu = exterior == "cpu";
		if (std::tuple(requested.pipelineMode, requested.interiorCpu, requested.exteriorCpu) != before)
			applied += fmt::format("{}pipeline {} (interiors {}, exteriors {})", applied.empty() ? "" : ", ", requested.pipelineMode == DCLF::PipelineMode::Manual ? "manual" : "auto",
				requested.interiorCpu ? "cpu" : "gpu", requested.exteriorCpu ? "cpu" : "gpu");
	}
	if (const auto monitor = savedSettings.find("CompatibilityMonitor"); monitor != savedSettings.end() && monitor->is_boolean() &&
																		 DCLF::SwitchValue(DCLF::Switch::PlayerParity).empty() && monitor->get<bool>() != DCLF::Diagnostics::PlayerParity::Requested()) {
		DCLF::Diagnostics::PlayerParity::Request(monitor->get<bool>());
		applied += fmt::format("{}compatibility monitor {}", applied.empty() ? "" : ", ", monitor->get<bool>() ? 1 : 0);
	}
	if (!applied.empty())
		logger::info("[DCLF] from the saved settings (where no switch is set): {}", applied);
	DCLF::Toggles::Get().Commit(requested, "saved settings");
}

void DrawcallLimitFix::ClearShaderCache()
{
	// Every live render graph pipeline, whether DCLF is installed or not.
	RenderGraphRuntime::RequestShaderReload();
}

void DrawcallLimitFix::NoteMaterialWritten(const RE::BSShaderMaterial* a_material)
{
	// Not installed, so nothing drains the captures. Each holds a reference, so the materials and textures would never be freed.
	if (!globals::features::drawcallLimitFix.installed)
		return;
	// CS's PBR materials reach Core through the material-write sink, like the features' write hooks.
	DCLF::Api::NoteMaterialWritten(a_material);
}

namespace
{
	/** @brief SceneWriterFramework's Write, when the provider serves it at ABI minor 2. False means the caller stores. */
	bool SceneWrite(std::uint32_t a_kind, void* a_object, std::uint64_t a_value)
	{
		const auto* api = DCLF::SceneWriter::Api();
		return api && api->abiMinor >= 2 && api->size > offsetof(SWF_Api, Write) && api->Write && api->Write(a_kind, a_object, a_value) == SWF_OK;
	}
}

void DrawcallLimitFix::WriteHidden(RE::NiAVObject* a_object, bool a_hidden)
{
	if (a_object && !SceneWrite(SWF_WRITE_HIDDEN, a_object, a_hidden ? 1 : 0))
		a_object->SetAppCulled(a_hidden);
}

void DrawcallLimitFix::WriteShaderProperty(RE::BSGeometry* a_geometry, RE::BSShaderProperty* a_property)
{
	if (a_geometry && !SceneWrite(SWF_WRITE_SHADER_PROPERTY, a_geometry, reinterpret_cast<std::uintptr_t>(a_property)))
		a_geometry->GetGeometryRuntimeData().shaderProperty = RE::NiPointer(a_property);
}

void DrawcallLimitFix::WriteAlphaFlags(RE::NiAlphaProperty* a_alpha, std::uint16_t a_flags)
{
	if (a_alpha && !SceneWrite(SWF_WRITE_ALPHA_FLAGS, a_alpha, a_flags))
		a_alpha->alphaFlags = a_flags;
}

void DrawcallLimitFix::PrecompileShader(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor)
{
	DCLF::ShaderPrograms::Get().Precompile(a_shader, a_pixel, a_descriptor);
	// The engine draw families' ORG stages for the descriptor: the sky, the water, the screen blood and the shadow masks.
	DCLF::Family::PrecompileStages(a_shader, a_pixel, a_descriptor);
}

void DrawcallLimitFix::PrecompileFamilyStages(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor)
{
	DCLF::Family::PrecompileStages(a_shader, a_pixel, a_descriptor);
}

void DrawcallLimitFix::QueueProgramStages(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor)
{
	const auto type = a_shader.shaderType.get();
	if (type == RE::BSShader::Type::Lighting || type == RE::BSShader::Type::Utility)
		DCLF::ShaderPrograms::Get().QueuePrecompile(a_shader, a_pixel, a_descriptor);
}

void DrawcallLimitFix::PrecompileDerivedStages(const RE::BSShader& a_utility)
{
	DCLF::ShaderPrograms::Get().PrecompileDerivedStages(a_utility);
}

void DrawcallLimitFix::PostSetupResources()
{
	if (!installed)
		return;
	// After Deferred made its targets and the engine target set is declared: the pipelines are built for these formats from here on.
	DCLF::DeclaredTargets::Init();
	// Their clients register before the setup's graph build, so they are in their anchors' graphs before the first world frame.
	DCLF::Family::RunSetups();
}

void DrawcallLimitFix::OnMap(ID3D11Resource* a_resource, void* a_data)
{
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().OnMap(a_resource, a_data);
	if (auto& mirror = DCLF::ConstantMirror::Get(); mirror.Any())
		mirror.OnMap(a_resource, a_data);
}

void DrawcallLimitFix::OnUnmap(ID3D11Resource* a_resource)
{
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().OnUnmap(a_resource);
	if (auto& mirror = DCLF::ConstantMirror::Get(); mirror.Any())
		mirror.OnUnmap(a_resource);
}

void DrawcallLimitFix::OnUpdateSubresource(ID3D11Resource* a_resource, const D3D11_BOX* a_box, const void* a_data)
{
	if (auto& mirror = DCLF::ConstantMirror::Get(); mirror.Any())
		mirror.OnUpdateSubresource(a_resource, a_box, a_data);
}

