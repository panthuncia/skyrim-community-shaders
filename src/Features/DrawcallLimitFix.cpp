#include "DrawcallLimitFix.h"

#include "Deferred.h"
#include "DrawcallLimitFix/AsyncWorker.h"
#include "DrawcallLimitFix/CaptureParity.h"
#include "DrawcallLimitFix/ConstantEvaluator.h"
#include "DrawcallLimitFix/DecalProbe.h"
#include "DrawcallLimitFix/SkinProbe.h"
#include "DrawcallLimitFix/DrawPipelines.h"
#include "DrawcallLimitFix/GpuResources.h"
#include "DrawcallLimitFix/GpuTextures.h"
#include "DrawcallLimitFix/IndirectDraws.h"
#include "DrawcallLimitFix/ShaderPrograms.h"
#include "DrawcallLimitFix/ShadowProbe.h"
#include "DrawcallLimitFix/ShadowViews.h"
#include "DrawcallLimitFix/Toggles.h"
#include "DrawcallLimitFix/PassCapture.h"
#include "DrawcallLimitFix/SceneStore.h"
#include "DrawcallLimitFix/SceneTracker.h"
#include "DrawcallLimitFix/Switches.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "ShaderCache.h"
#include "State.h"
#include "TerrainBlending.h"

namespace
{
	bool StatsEnabled()
	{
		static const bool enabled = [] {
			return DCLF::SwitchEnabled("CS_DCLF_STATS");
		}();
		return enabled;
	}

	constexpr std::uint32_t kReportInterval = 300;

	/**
	 * @brief Test switch CS_DCLF_TEST_COMMANDS="<frame>:<console command>;...": runs each console command
	 * once the main pass has run that many frames (loading screens do not count), on the main thread.
	 * Used to take coverage runs to other cells from the auto-loaded save, e.g.
	 * "600:coc WhiterunDragonsreach;1500:coc BleakFallsBarrow01".
	 */
	class TestCommands
	{
	public:
		TestCommands()
		{
			const std::string commandList = DCLF::SwitchValue("CS_DCLF_TEST_COMMANDS");
			std::string_view text = commandList;
			if (text.empty())
				return;
			while (!text.empty()) {
				const auto end = text.find(';');
				const auto item = text.substr(0, end);
				const auto colon = item.find(':');
				if (colon != std::string_view::npos)
					commands.push_back({ static_cast<std::uint32_t>(std::strtoul(std::string(item.substr(0, colon)).c_str(), nullptr, 10)), std::string(item.substr(colon + 1)) });
				text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
			}
		}

		/**
		 * @brief Advances the counter and runs whatever is due.
		 *
		 * The counter lives here rather than being SceneStore's frame number, because the commands have to
		 * fire at the same point in a run whether or not the feature is installed. Driven off SceneStore's
		 * frame, a CS_DCLF=0 run never ran them at all, so a "baseline" pinned to a given hour was in fact
		 * whatever hour the save happened to be at - which is precisely the drift that makes screenshot
		 * comparisons worthless. Loading screens do not count, so the two runs stay in step.
		 */
		void OnFrame()
		{
			if (DCLF::SceneStore::IsLoadingScreenUp())
				return;
			const std::uint32_t a_frame = ++frame;
			while (next < commands.size() && a_frame >= commands[next].frame) {
				auto command = commands[next++].command;
				logger::info("[DCLF] test command at frame {}: {}", a_frame, command);
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([command] {
						const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
						if (auto* script = factory ? static_cast<RE::Script*>(factory->Create()) : nullptr) {
							script->SetCommand(command);
							script->CompileAndRun(nullptr);
							delete script;
						}
					});
				}
			}
		}

	private:
		struct Command
		{
			std::uint32_t frame;
			std::string command;
		};
		std::vector<Command> commands;
		std::size_t next = 0;
		std::uint32_t frame = 0;
	};

	/**
	 * @brief Test switch CS_DCLF_TEST_TOGGLE="<off frame>:<on frame>": flips the feature's menu toggle (the
	 * same disabled flag the feature list writes) at those frames, loading screens not counted, to exercise
	 * switching DCLF off and back on in a running session. The flag is restored to on at the second frame.
	 */
	class TestToggle
	{
	public:
		TestToggle()
		{
			const std::string value = DCLF::SwitchValue("CS_DCLF_TEST_TOGGLE");
			if (const auto colon = value.find(':'); colon != std::string::npos) {
				offFrame = static_cast<std::uint32_t>(std::strtoul(value.substr(0, colon).c_str(), nullptr, 10));
				onFrame = static_cast<std::uint32_t>(std::strtoul(value.substr(colon + 1).c_str(), nullptr, 10));
			}
		}

		void OnFrame(const std::string& a_feature)
		{
			if (!offFrame || DCLF::SceneStore::IsLoadingScreenUp())
				return;
			++frame;
			if (frame == offFrame || frame == onFrame) {
				logger::info("[DCLF] test toggle at frame {}: {}", frame, frame == offFrame ? "off" : "on");
				globals::state->SetFeatureDisabled(a_feature, frame == offFrame);
			}
		}

	private:
		std::uint32_t offFrame = 0;
		std::uint32_t onFrame = 0;
		std::uint32_t frame = 0;
	};

	// Render-thread CPU spent on scene capture, averaged over a report interval.
	struct CaptureTiming
	{
		double eventsMs = 0.0;
		double buildMs = 0.0;  // the accumulate phase, at EarlyPrepass
		double buildMaxMs = 0.0;
		double sceneMs = 0.0;  // the scene phase, before the shadow maps
		double sceneMaxMs = 0.0;
		std::uint32_t frames = 0;
	} timing;

	DCLF::TargetFormats CurrentTargetFormats()
	{
		DCLF::TargetFormats formats;
		ID3D11RenderTargetView* views[8] = {};
		ID3D11DepthStencilView* depth = nullptr;
		globals::d3d::context->OMGetRenderTargets(8, views, &depth);
		for (std::uint32_t i = 0; i < 8; ++i) {
			if (!views[i])
				continue;
			D3D11_RENDER_TARGET_VIEW_DESC desc;
			views[i]->GetDesc(&desc);
			formats.colors[i] = desc.Format;
			formats.colorCount = i + 1;
			views[i]->Release();
		}
		if (depth) {
			D3D11_DEPTH_STENCIL_VIEW_DESC desc;
			depth->GetDesc(&desc);
			formats.depth = desc.Format;
			depth->Release();
		}
		return formats;
	}

	double MillisecondsSince(std::chrono::steady_clock::time_point a_start)
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
	}
}

void DrawcallLimitFix::PostPostLoad()
{
	if (REL::Module::IsVR()) {
		logger::info("[DCLF] Not supported on VR; scene tracking stays off");
		return;
	}
	// CS_DCLF=0 is the off switch every gate in this work compares against, and until now nothing read it:
	// the switch appeared in the summary line below and nowhere else, so runs labelled "CS_DCLF=0 baseline"
	// had the feature fully installed and were not baselines at all. Unset still means on, which is the
	// behaviour everything else here was built against.
	if (DCLF::SwitchValue("CS_DCLF") == "0") {
		logger::info("[DCLF] CS_DCLF=0; the feature stays off and the game renders natively");
		return;
	}
	DCLF::SceneTracker::Get().Install();
	// Capture at registration, the foundation for static ownership: withholding a pass from the batch
	// renderer removes the very data the tables are built from today, so the capture has to prove itself
	// first (it claims nothing and withholds nothing yet).
	DCLF::PassCapture::Get().Install();
	DCLF::ShadowProbe::Get().Install();
	Hooks::Install();
	installed = true;
	// The switches this process actually sees, once. Several reports below are gated on them, so without
	// this a silent log is indistinguishable from a switch that never reached the game - which is exactly
	// what happened when they came from the environment alone (see Switches.h).
	logger::info("[DCLF] switches: {}", DCLF::SwitchSummary());
}

void DrawcallLimitFix::SetupResources()
{
	// Runs right after the render graph tried to adopt DXVK's device (State::SetupResources). The hooks went in
	// at PostPostLoad, before any device existed; without the graph there is nothing to draw with, and left
	// on, DCLF would track, classify and skip for nothing while the frame silently stays native. So it is
	// forced off here, with the reason in the log and in the menu instead of only a line of zeroes in the stats.
	if (!installed || RenderGraphRuntime::Get().IsActive())
		return;
	unavailableReason = RenderGraphRuntime::Get().GetDisabledReason();
	installed = false;
	DCLF::SceneTracker::Get().Stop();
	DCLF::PassCapture::Get().SetBypassed(true);
	logger::warn("[DCLF] Forced off: the render graph is unavailable ({}); the game renders natively", unavailableReason);
}

void DrawcallLimitFix::Reset()
{
	// The test commands run before the installed check, so a CS_DCLF=0 baseline reaches the same place at
	// the same in-game hour as the run it is compared against.
	static TestCommands testCommands;
	testCommands.OnFrame();
	static TestToggle testToggle;
	testToggle.OnFrame(GetShortName());

	// Every Present, in menus too: the tracker's queue holds references to attached subtrees and
	// must not grow while the world is not rendered.
	if (!installed)
		return;
	const auto start = std::chrono::steady_clock::now();
	// A worker job the frame never joined must not cross into the next frame's tables.
	DCLF::SceneStore::Get().JoinScenePhase();
	DCLF::IndirectDraws::Get().EndFrame();
	DCLF::SceneStore::Get().ProcessEvents();
	timing.eventsMs += MillisecondsSince(start);
	// The menu's toggle, between frames. Scene events keep flowing above while off, so the tracked set is
	// current the moment it comes back on.
	UpdateActive();
}

void DrawcallLimitFix::UpdateActive()
{
	if (!installed)
		return;
	if (const bool wanted = loaded && !globals::state->IsFeatureDisabled(GetShortName()); wanted != switchedOn)
		SetActive(wanted);
}

void DrawcallLimitFix::SetActive(bool a_active)
{
	switchedOn = a_active;
	if (!a_active) {
		DCLF::SceneStore::Get().AbandonSceneJob();
		DCLF::IndirectDraws::Get().DrainAsync();
	}
	auto& capture = DCLF::PassCapture::Get();
	// Off: every pass reaches the batch renderers again, and no claim outlives the switch. Back on, the claims
	// are empty until the first frame republishes them, so nothing is withheld that DCLF has not drawn.
	capture.SetBypassed(!a_active);
	capture.PublishClaims(nullptr);
	for (std::uint32_t mode = 0; mode < DCLF::PassCapture::kShadowModes; ++mode)
		capture.PublishShadowClaims(mode, nullptr);
	if (a_active)
		DCLF::SceneStore::Get().InvalidateVerdicts();
	skipCounters = {};
	logger::info("[DCLF] {} from the menu", a_active ? "Switched on" : "Switched off; the game renders natively");
}

void DrawcallLimitFix::BeforeShadowMaps()
{
	// Called directly by Deferred every frame, loaded or not: the one place an unload (which stops Reset
	// from being called) is noticed.
	UpdateActive();
	if (!Running())
		return;
	// The scene half of the tables, before the engine draws the shadow maps: the scene graph and the
	// main camera's culling are final here, and the records a shadow view needs exist from this point.
	// The accumulator's half follows at EarlyPrepass, once the registration jobs have finished.
	auto& store = DCLF::SceneStore::Get();
	// The frame's toggles, before anything reads them. A change that enters the classification drops the
	// cached verdicts, so the next frame classifies every object under the new switches.
	if (DCLF::Toggles::Get().BeginFrame())
		store.InvalidateVerdicts();
	ScopedPerfEvent event("CS DCLF: scene tables and shadow views");
	const auto start = std::chrono::steady_clock::now();
	store.BuildFrame(DCLF::SceneStore::Phase::Scene);
	// The frame's shadow views, in the order the engine is about to render them. Everything downstream -
	// the capture's attribution, the claims, the epochs - identifies a view by this list.
	DCLF::ShadowViews::Get().Rebuild();
	if (DCLF::IndirectDraws::ShadowsEnabled()) {
		DCLF::IndirectDraws::Get().BeginShadowFrame(globals::game::shadowState->GetRuntimeData().posAdjust.getEye());
		// The shadow epoch's build, on the worker, while the engine draws the shadow maps (CS_DCLF_ASYNC).
		DCLF::IndirectDraws::Get().KickShadowBuild();
	}
	const double sceneMs = MillisecondsSince(start);
	timing.sceneMs += sceneMs;
	timing.sceneMaxMs = std::max(timing.sceneMaxMs, sceneMs);
	if (DCLF::ShadowProbe::Enabled())
		DCLF::ShadowProbe::Get().OnBeforeShadowMaps();
}

void DrawcallLimitFix::AfterShadowMaps()
{
	RenderGraphRuntime::EpochBodyScope body(RenderGraphRuntime::Segment::ShadowView);
	// The scene walk (CS_DCLF_ASYNC), before anything reads the tables it writes.
	DCLF::SceneStore::Get().JoinScenePhase();
	if (!Running())
		return;
	// The frame's shadow epoch: every view the 0x2A hook captured, drawn in one graph execution.
	DCLF::IndirectDraws::Get().ExecuteShadowFrame();
	if (DCLF::ShadowProbe::Enabled())
		DCLF::ShadowProbe::Get().OnAfterShadowMaps();
}

void DrawcallLimitFix::EarlyPrepass()
{
	DCLF::SceneStore::Get().JoinScenePhase();  // normally joined at AfterShadowMaps already
	if (!Running())
		return;

	// The tables are built here, from Main_RenderShadowMaps, rather than in Prepass from StartDeferred.
	// Both DCLF epochs then read the same generation: the Z-prepass runs at the end of Main_RenderDepth,
	// which is after this and before Prepass, so it used to see the *previous* frame's tables and the
	// per-object visibility verdicts it wrote could not be applied by index in the colour epoch.
	//
	// This is only possible because the accumulator is already complete here - the cull job finishes
	// before the shadow maps (Main::Draw) - and because the tables read the latched accumulator rather
	// than `currentAccumulator`, which is not set this early.
	auto& store = DCLF::SceneStore::Get();
	ScopedPerfEvent event("CS DCLF: accumulator tables and pipelines");
	const auto start = std::chrono::steady_clock::now();
	store.BuildFrame(DCLF::SceneStore::Phase::Accumulate);
	const double buildMs = MillisecondsSince(start);
	timing.buildMs += buildMs;
	timing.buildMaxMs = std::max(timing.buildMaxMs, buildMs);
	++timing.frames;
	// The shadow probe reads this frame's tables (the main pass's kept objects) and the Utility
	// registrations, which are complete now that the thunk has returned.
	if (DCLF::ShadowProbe::Enabled())
		DCLF::ShadowProbe::Get().OnFrameBuilt();

	// Phase 2: SPIR-V programs and indirect pipelines for the pipelines drawn this frame (built once, asynchronously).
	auto& programs = DCLF::ShaderPrograms::Get();
	auto& pipelines = DCLF::DrawPipelines::Get();
	if (auto* lighting = DCLF::ConstantEvaluator::Get().GetLightingShader(); lighting && programs.Enabled()) {
		const auto& tables = store.GetTables();
		for (std::size_t p = 0; p < tables.pipelines.size(); ++p) {
			if (!tables.PipelineUsed(p, store.GetFrame()))
				continue;
			if (const auto* program = programs.Find(tables.pipelines[p], *lighting))
				pipelines.Find(tables.pipelines[p], *program);
		}
		programs.Update();
		pipelines.Update();
		// The pipeline lookups an epoch's build reads (Lookups.h): the set index of every pipeline used this
		// frame, its shaders' constant tables and its register usage, after Update has admitted this frame's
		// finished builds. Resolved here, where the GPU is busy with the shadow maps, rather than in the epoch.
		auto& lookups = store.MutableLookups();
		if (lookups.pipelineSetGeneration != pipelines.Generation()) {
			// The set was recreated (a target change): every index a build may hold is stale.
			lookups.pipelineSetGeneration = pipelines.Generation();
			++lookups.generation;
		}
		lookups.pipelines.resize(tables.pipelines.size());
		auto& cache = SIE::ShaderCache::Instance();
		for (std::size_t p = 0; p < tables.pipelines.size(); ++p) {
			auto& entry = lookups.pipelines[p];
			if (!tables.PipelineUsed(p, store.GetFrame())) {
				entry.setIndex = DCLF::Lookups::kNone;
				continue;
			}
			const auto& key = tables.pipelines[p];
			const auto* program = programs.Find(key, *lighting);
			const std::uint32_t setIndex = program ? pipelines.Find(key, *program) : DCLF::DrawPipelines::kNotReady;
			auto* vs = cache.GetVertexShader(*lighting, key.vertexDescriptor);
			auto* ps = cache.GetPixelShader(*lighting, key.pixelDescriptor);
			const std::uint32_t resolved = (setIndex != DCLF::DrawPipelines::kNotReady && vs && ps) ? setIndex : DCLF::Lookups::kNone;
			if (!(entry.key == key)) {
				entry.key = key;
				entry.setIndex = DCLF::Lookups::kNone;
				entry.shadowMaskIndex = DCLF::Lookups::kNone;
			}
			if (entry.setIndex != DCLF::Lookups::kNone && entry.setIndex != resolved)
				++lookups.generation;  // a build may hold the old index
			entry.setIndex = resolved;
			if (resolved == DCLF::Lookups::kNone)
				continue;
			entry.vsTable.assign(vs->constantTable.begin(), vs->constantTable.end());
			entry.psTable.assign(ps->constantTable.begin(), ps->constantTable.end());
			for (std::uint32_t variant = 0; variant < 2; ++variant) {
				const auto& usage = pipelines.Usage(resolved, variant);
				auto& bits = entry.usage[variant];
				bits.vertexConstants = usage.vertexConstants;
				bits.pixelConstants = usage.pixelConstants;
				bits.textures = usage.textures;
				bits.samplers = usage.samplers;
			}
		}
	}

	// The shadow views' programs: one Utility build per technique of the frame's casters, per render mode
	// among the views the engine drew. Requested here, beside the Lighting builds, so they are compiled
	// long before a shadow epoch would draw with them.
	if (auto* utility = globals::game::utilityShader; utility && programs.Enabled()) {
		std::uint32_t modeBits = 0;
		for (const auto& view : DCLF::ShadowViews::Get().All()) {
			// Clamped for cascades and spot lights, the paraboloid warp for point lights; the engine
			// picks the render mode from the light, not from the descriptor (engine notes: shadow maps).
			modeBits |= DCLF::ShadowModeBits(view.kind == DCLF::ShadowViews::Kind::Parabolic ? 0xFu : 0xEu);
		}
		const auto& tables = store.GetTables();
		// The shadow map array the views draw into; its format is what a shadow pipeline is built for.
		DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;
		if (auto* renderer = globals::game::renderer) {
			const auto& depthStencils = renderer->GetDepthStencilData().depthStencils;
			// The depth-stencil view's format, not the texture's: the texture is typeless (R16_TYPELESS)
			// and a pipeline is built against the view it renders through (D16_UNORM), as the epoch does.
			if (auto* view = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].views[0]) {
				D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
				view->GetDesc(&desc);
				shadowFormat = desc.Format;
			}
		}
		std::vector<DCLF::ShadowPipelineKey> shadowKeys;
		shadowKeys.reserve(tables.shadowKeysUsed.size() * 2);
		for (const auto& key : tables.shadowKeysUsed) {
			for (std::uint32_t mode : { 0xEu, 0xFu }) {
				const std::uint32_t bits = DCLF::ShadowModeBits(mode);
				if (!(modeBits & bits))
					continue;
				const DCLF::ShadowPipelineKey viewKey{ key.technique | bits, key.rasterFlags, key.vertexLayout };
				shadowKeys.push_back(viewKey);
				if (const auto* program = programs.FindShadow(viewKey.technique, *utility))
					pipelines.FindShadow(viewKey, *program, shadowFormat);
			}
		}
		pipelines.CaptureShadowStates(shadowKeys);
	}

	// The Z-prepass epoch's build, on the worker, from here to the end of Main_RenderDepth (CS_DCLF_ASYNC).
	DCLF::IndirectDraws::Get().KickZPrepassBuild();
}

void DrawcallLimitFix::Prepass()
{
	if (!Running())
		return;

	auto& store = DCLF::SceneStore::Get();
	// The one point in the frame where the main camera's accumulator is identifiable: EarlyPrepass, where
	// the tables are now built, is too early for `currentAccumulator` to be set.
	store.LatchAccumulator();
	// The camera-dependent half of the per-frame constants, now that the main camera's shadow state is
	// current (BuildFrame ran at EarlyPrepass, where it still belonged to the shadow-map camera).
	store.RefreshFrameConstants();
	// The colour epoch's build, on the worker, from here to the epoch (CS_DCLF_ASYNC).
	DCLF::IndirectDraws::Get().KickColourBuild();
	skipStats = skipCounters;
	skipCounters = {};
	if ((store.GetFrame() % kReportInterval) == 1)
		skipSamples.clear();  // collected again for the next report

	const std::uint32_t frame = store.GetFrame();
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().Report(frame, kReportInterval);
	if (DCLF::DecalProbe::Enabled())
		DCLF::DecalProbe::Get().Report(frame, kReportInterval);
	if (DCLF::SkinProbe::Enabled())
		DCLF::SkinProbe::Get().Report(frame, kReportInterval);
	if (DCLF::ShadowProbe::Enabled())
		DCLF::ShadowProbe::Get().Report(frame, kReportInterval);

	// The culling's counters, reported whether or not the full statistics are on: they are what says
	// whether GPU culling is running and how much it rejects.
	if ((frame % kReportInterval) == 0) {
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		if (draws.cullDrawn || draws.cullRejected || draws.cullOccluded) {
			const std::uint32_t rejected = draws.cullRejected + draws.cullOccluded;
			logger::info("[DCLF] culling: {} draws written, {} of {} tested were rejected ({:.1f}%: {} outside the frustum, {} occluded, {} of those the engine had kept); against the engine: {} it culled were gated out, {} it culled were kept, {} it kept the frustum test rejected{}",
				draws.cullDrawn, rejected, draws.cullTested,
				draws.cullTested ? 100.0 * rejected / draws.cullTested : 0.0,
				draws.cullRejected, draws.cullOccluded, draws.cullOccludedVisible,
				draws.cullEngineCulled, draws.cullRescued, draws.cullFalseNegatives,
				draws.cullFalseNegatives ? " <- FALSE NEGATIVES" : "");
			if (draws.cullRescuedByPhaseTwo || draws.cullDrawnPhaseTwo)
				logger::info("[DCLF] two-phase culling: phase 2 brought back {} objects the stale HZB had rejected, and drew depth for {} of them",
					draws.cullRescuedByPhaseTwo, draws.cullDrawnPhaseTwo);
			if (draws.hzbSampled)
				logger::info("[DCLF] HZB: {} footprints sampled, {} came back all-near (~0), {} all-far (~1)",
					draws.hzbSampled, draws.hzbNear, draws.hzbFar);
			if (draws.hzbSample.valid) {
				const auto& sample = draws.hzbSample;
				logger::info("[DCLF] HZB rejection sample: farthest {:.6f} against nearest {:.6f}, uv ({:.4f} {:.4f})-({:.4f} {:.4f}), mip {}, engine {}",
					sample.farthest, sample.nearestZ, sample.uvMin[0], sample.uvMin[1], sample.uvMax[0], sample.uvMax[1],
					sample.mip, sample.nativeVisible ? "kept it" : "culled it");
			}
		}
	}
	// The bindless record's own gate, reported whenever it is on: it says whether the table the shaders
	// read holds what the constant buffer path would have given them, which no screenshot can say.
	if ((frame % kReportInterval) == 0) {
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		if (draws.recordParityChecks) {
			if (draws.recordParityMismatches)
				logger::warn("[DCLF] record dedup parity MISMATCH: {} of {} rebuilt records differ from their pair's", draws.recordParityMismatches, draws.recordParityChecks);
			else
				logger::info("[DCLF] record dedup parity OK: {} rebuilt records match their pair's byte for byte", draws.recordParityChecks);
		}
		if (draws.bindlessParityChecks) {
			if (draws.bindlessParityMismatches)
				logger::warn("[DCLF] bindless record parity MISMATCH: {} of {} components differ", draws.bindlessParityMismatches, draws.bindlessParityChecks);
			else
				logger::info("[DCLF] bindless record parity OK: {} components match the constant groups", draws.bindlessParityChecks);
		}
	}
	// What the GPU spent on each segment, measured by the graph itself; the menu shows the latest window.
	if ((frame % kReportInterval) == 0)
		RenderGraphRuntime::Get().ReportGpuTimings(kReportInterval, StatsEnabled());
	if ((frame % kReportInterval) == 0 && !StatsEnabled()) {
		timing = {};
		store.ResetTimes();
	}
	if (StatsEnabled() && (frame % kReportInterval) == 0) {
		const auto& stats = store.GetStats();
		std::string reasons;
		for (std::size_t i = 1; i < stats.ineligible.size(); ++i) {
			if (stats.ineligible[i])
				reasons += fmt::format(" {}={}({} drawn)", DCLF::kIneligibleNames[i], stats.ineligible[i], stats.ineligibleDrawn[i]);
		}
		const double frames = std::max(1u, timing.frames);
		// The per-part breakdown appears only under CS_DCLF_PROFILE=1, because that is the only time it is
		// measured. Timing from inside the loop perturbs it, so compare a profiled run's parts against an
		// unprofiled run's total rather than treating the parts as free.
		std::string parts;
		if (DCLF::SceneStore::ProfileEnabled()) {
			for (std::size_t i = 0; i < stats.partMs.size(); ++i)
				parts += fmt::format("{}{} {:.3f}", parts.empty() ? "; by part: " : ", ", DCLF::kBuildPartNames[i], stats.partMs[i] / frames);
		}
		const auto& shaders = DCLF::ShaderPrograms::Get().GetStats();
		logger::info("[DCLF] SPIR-V programs: {} requested, {} ready ({} stages from cache), {} failed; shadow (Utility): {} requested, {} ready, {} failed",
			shaders.requested, shaders.ready, shaders.fromCache, shaders.failed, shaders.shadowRequested, shaders.shadowReady, shaders.shadowFailed);
		const auto& indirect = DCLF::DrawPipelines::Get().GetStats();
		logger::info("[DCLF] indirect pipelines: {} requested, {} in the set, {} failed; target changes {}; shadow: {} requested, {} in the set, {} failed",
			indirect.requested, indirect.ready, indirect.failed, indirect.targetChanges, indirect.shadowRequested, indirect.shadowReady, indirect.shadowFailed);
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		std::string skipped;
		for (std::size_t i = 0; i < draws.skipped.size(); ++i) {
			if (draws.skipped[i])
				skipped += fmt::format(" {}={}", DCLF::kSkipNames[i], draws.skipped[i]);
		}
		const auto& textures = DCLF::GpuTextures::Get().GetStats();
		logger::info("[DCLF] indirect draws (last frame): {} candidates built from {} binding records, skipped:{} (missing t{} t{} t{} t{}, VS b{:04X} PS b{:04X}); {:.1f} MB uploaded, {:.3f} ms CPU; {} epochs, {} not ready; textures {} cached, rejected {}/{}/{}, {} samplers",
			draws.drawn, draws.records, skipped, draws.missingTextures[0], draws.missingTextures[1], draws.missingTextures[2], draws.missingTextures[3], draws.missingVertexConstants,
			draws.missingPixelConstants, draws.uploadBytes / 1048576.0,
			draws.cpuMs, draws.epochs, draws.notReady, textures.cached, textures.rejected[1], textures.rejected[2], textures.rejected[3], textures.samplers);
		std::string epochParts;
		for (std::size_t i = 0; i < draws.partMs.size(); ++i)
			epochParts += fmt::format("{}{} {:.3f} ms", epochParts.empty() ? "" : ", ", DCLF::kEpochPartNames[i], draws.partMs[i]);
		// Under CS_DCLF_ASYNC the build's parts are measured on the worker when it built the payload, and
		// "graph execute" is then everything the render thread spent on the epoch.
		logger::info("[DCLF] indirect epoch CPU by part{}: {}", DCLF::AsyncModeSetting() != DCLF::AsyncMode::Off ? " (build parts on the worker when it built)" : "", epochParts);
		if (draws.commitEpochs) {
			static constexpr std::array<const char*, 7> kCommitParts{ "join", "lookups", "frame textures and patches", "frame blocks",
				"payload uploads", "drawn set", "rest" };
			std::string commitParts;
			for (std::size_t i = 0; i < kCommitParts.size(); ++i)
				commitParts += fmt::format("{}{} {:.1f}", commitParts.empty() ? "" : ", ", kCommitParts[i], draws.commitUs[i] / draws.commitEpochs);
			logger::info("[DCLF] main epoch commit on the render thread, us per epoch over {} epochs: {}", draws.commitEpochs, commitParts);
			DCLF::IndirectDraws::Get().ResetCommitTimings();
		}
		if (DCLF::AsyncModeSetting() != DCLF::AsyncMode::Off) {
			std::istringstream asyncLines(DCLF::IndirectDraws::Get().AsyncReport() + DCLF::SceneStore::Get().SceneAsyncReport());
			for (std::string line; std::getline(asyncLines, line);)
				logger::info("{}", line);
		}
		if (DCLF::IndirectDraws::ShadowsEnabled()) {
			const auto& shadow = DCLF::IndirectDraws::Get().GetShadowStats();
			std::string notReadyReasons;
			for (std::size_t r = 0; r < shadow.notReadyReasons.size(); ++r) {
				if (shadow.notReadyReasons[r])
					notReadyReasons += fmt::format(" {}={}", DCLF::kShadowNotReadyNames[r], shadow.notReadyReasons[r]);
			}
			logger::info("[DCLF] shadow views: {} offered, {} drawn in {} epochs, {} not ready ({}), {} focus views left native; last mode {} inputs ({} without a pipeline, {} without a texture), {} records; CPU {:.3f} ms per frame ({:.3f} capturing, {:.3f} preparing, {:.3f} inputs, {:.3f} blocks, {:.3f} graph, {:.3f} claiming)",
				shadow.views, shadow.viewsDrawn, shadow.epochs, shadow.notReady, notReadyReasons.empty() ? "-" : notReadyReasons.c_str() + 1, shadow.focusSkipped, shadow.inputs, shadow.skippedPipeline,
				shadow.skippedTexture, shadow.records, (shadow.cpuMs + shadow.captureMs) / frames, shadow.captureMs / frames, shadow.prepareMs / frames, shadow.inputsMs / frames, shadow.blocksMs / frames,
				shadow.executeMs / frames, shadow.claimMs / frames);
			if (shadow.cullTested)
				logger::info("[DCLF] shadow culling (view {} mode {:#x}, sampled): {} tested, {} drawn, {} rejected by the frustum",
					shadow.cullSampledView, shadow.cullSampledMode, shadow.cullTested, shadow.cullDrawn, shadow.cullRejected);
			if (DCLF::PassCapture::ShadowWithholdingEnabled()) {
				const auto& captured = DCLF::PassCapture::Get().GetStats();
				logger::info("[DCLF] shadow ownership: withheld plain {} / clamped {} / paraboloid {} passes (last frame); claimed {} / {} / {} casters; {} views not ready under ownership{}",
					captured.shadowWithheld[0], captured.shadowWithheld[1], captured.shadowWithheld[2], shadow.claimed[0], shadow.claimed[1], shadow.claimed[2],
					shadow.notReady, shadow.notReady ? " <- HOLES" : "");
			}
			DCLF::IndirectDraws::Get().ResetShadowStats();
		}
		if (stats.projectedUV || stats.landBlend)
			logger::info("[DCLF] projected UV / terrain (last frame): {} projected candidates, {} terrain candidates, projected textures {}",
				stats.projectedUV, stats.landBlend, store.GetProjectedTextures().valid ? "captured" : "not seen yet");
		if (stats.skinned || draws.boneRows)
			logger::info("[DCLF] skinned (last frame): {} candidates, {} palette rows in the tables, {} rows uploaded by the epoch", stats.skinned, stats.boneRows, draws.boneRows);
		if (stats.decals[0] || stats.decals[1] || draws.decalsDrawn)
			logger::info("[DCLF] decals (last frame): {} candidates ({} in the opaque group, {} in the blended group), {} submitted to the second pass, {} of {} tested were culled",
				stats.decals[0] + stats.decals[1], stats.decals[0], stats.decals[1], draws.decalsDrawn, draws.decalsCulled, draws.decalsTested);
		if (DCLF::IndirectDraws::Hybrid())
			logger::info("[DCLF] hybrid (last frame): {} of {} native passes left to the indirect draws ({} in the depth pass, {} in the opaque pass)",
				skipStats.skipped, skipStats.offered, skipStats.skippedInDepth, skipStats.skippedInOpaque);
		if (DCLF::IndirectDraws::Hybrid() && !skipSamples.empty()) {
			std::string names;
			for (const auto& name : skipSamples)
				names += fmt::format("{}'{}'", names.empty() ? "" : ", ", name);
			logger::info("[DCLF] hybrid: passes left to the indirect draws include {}", names);
		}
		logger::info("[DCLF] parity: {} native passes kept, {} skipped", skipStats.kept, skipStats.skipped);
		if (DCLF::IndirectDraws::Hybrid() && skipStats.notInTables)
			logger::warn("[DCLF] hybrid (last frame): {} native passes were kept because their geometry left the tables", skipStats.notInTables);
		if (draws.shortBuffers)
			logger::warn("[DCLF] {} draws of the last epoch reach past their vertex or index buffer slice", draws.shortBuffers);
		const auto& gpu = DCLF::GpuResources::Get().GetStats();
		logger::info("[DCLF] game buffers for the render graph: {} stable, {} rejected; {} resolved so far in {:.1f} ms (slowest {:.3f} ms)", gpu.cached, gpu.rejected,
			gpu.resolvedTotal, gpu.resolveMsTotal, gpu.resolveMsMax);
		// Two halves, because they mean different things: outside kRuntimePassBits the derivation is meant
		// to be exact and any difference is a defect; inside it the derivation is guessing at what
		// GetRenderPasses computes per frame, and that number is what decides whether GetRenderPasses can
		// be skipped outright rather than merely withheld from the batch renderer.
		std::string bitBreakdown;
		// Only when CS_DCLF_DERIVE_PROBE measured it. Printed unconditionally this line read
		// "0 objects compared, 0 differ" with the probe off, which scans as a passing check rather
		// than as one that never ran.
		if (DCLF::SwitchEnabled("CS_DCLF_DERIVE_PROBE")) {
			for (std::uint32_t bit = 0; bit < 32; ++bit) {
				if (!stats.derivationBitCounts[bit])
					continue;
				bitBreakdown += std::format("{} bit {}{}={}", bitBreakdown.empty() ? "" : ",", bit,
					((1u << bit) & DCLF::kRuntimePassBits) ? "*" : "", stats.derivationBitCounts[bit]);
			}
			logger::info("[DCLF] derivation (last frame): {} objects compared, {} would stay native; property bits: {} differ ({:08X}); runtime bits: {} differ ({:08X}); per bit (* = runtime):{}",
				stats.derivationChecked, stats.derivationNative, stats.derivationDiffers, stats.derivationBits,
				stats.derivationRuntimeDiffers, stats.derivationRuntimeBits, bitBreakdown.empty() ? std::string(" none") : bitBreakdown);
		}
		const auto& capture = DCLF::PassCapture::Get().GetStats();
		logger::info("[DCLF] pass capture: {} registrations from {} threads ({} overflowed); against the accumulator: {} compared, {} missing, {} extra, {} technique differs, {} subPass differs{}",
			capture.captured, capture.threads, capture.overflowed, capture.compared, capture.missing, capture.extra,
			capture.techniqueDiffers, capture.subPassDiffers,
			(capture.missing || capture.techniqueDiffers || capture.subPassDiffers) ? "" : " <- OK");
		// CS_DCLF_REGISTER_PROBE=1: what reaches RegisterPass besides the lighting passes, and how much of
		// it lands in one of the main camera's batch renderers. This is what says whether the depth pass
		// can be owned the same way the opaque pass is.
		if (DCLF::SwitchEnabled("CS_DCLF_REGISTER_PROBE")) {
			auto& probeCapture = DCLF::PassCapture::Get();
			std::string byType;
			for (std::size_t i = 0; i < probeCapture.probeCounts.size(); ++i) {
				const auto total = probeCapture.probeCounts[i].exchange(0);
				const auto main = probeCapture.probeMain[i].exchange(0);
				if (total)
					byType += fmt::format(" type{}={}/{}", i, main, total);
			}
			logger::info("[DCLF] RegisterPass by shader type (into a main renderer / total):{}", byType.empty() ? " none" : byType);
		}
		if (DCLF::PassCapture::WithholdingEnabled())
			logger::info("[DCLF] static ownership: {} passes withheld from the batch renderer, {} objects claimed, {} claimed but not drawn{}",
				capture.withheld, capture.claimed, capture.holes, capture.holes ? " <- HOLES" : "");
		if (DCLF::PassCapture::WithholdingEnabled())
			logger::info("[DCLF] claim churn: +{} -{} ({} of the drops still had an engine pass, so the native loop takes them back)",
				capture.claimsAdded, capture.claimsDropped, capture.droppedAfterCull);
		logger::info("[DCLF] derived cache (last frame): {} served, {} recomputed and compared, {} differ{}; slots alive {} geometries / {} pipelines / {} materials, {} swept, {} geometries refreshed in place, {} slot violations{}",
			stats.derivedHits, stats.derivedChecked, stats.derivedDiffers, stats.derivedDiffers ? " <- STALE" : "",
			stats.geometriesAlive, stats.pipelinesAlive, stats.materialsAlive, stats.slotsSwept, stats.geometriesRefreshed,
			stats.slotViolations, stats.slotViolations ? " <- SLOT VIOLATION" : "");
		if (stats.classifyHits || stats.classifyChecked)
			logger::info("[DCLF] classification cache (last frame): {} served from the cache, {} recomputed and compared, {} differ{}; RTTI casts walked {}",
				stats.classifyHits, stats.classifyChecked, stats.classifyDiffers, stats.classifyDiffers ? " <- STALE" : "", stats.castResolved);
		// The Stage 4c gate. A pipeline's per-frame lighting template must come from an object the engine
		// itself kept; anything else hands a culled object's scene light list to the visible objects drawn
		// on that pipeline, which is the blown-out interior lighting defect.
		{
			static constexpr const char* kTechniqueNames[20] = { "none", "envmap", "glowmap", "parallax", "facegen",
				"facegenRGBTint", "hair", "parallaxOcc", "MTLand", "LODLand", "snow", "multilayerParallax", "treeAnim",
				"LODObjects", "multiIndexSparkle", "LODObjectHD", "eye", "cloud", "LODLandNoise", "MTLandLODBlend" };
			std::string techniques;
			for (std::size_t t = 0; t < stats.techniqueRejects.size(); ++t) {
				if (!stats.techniqueRejects[t])
					continue;
				const char* name = t == 63 ? "refraction" : (t < 20 ? kTechniqueNames[t] : "?");
				techniques += fmt::format("{}{}({})={}", techniques.empty() ? "" : " ", name, t, stats.techniqueRejects[t]);
			}
			if (!techniques.empty())
				logger::info("[DCLF] left native by technique: {}", techniques);
		}
		if (!stats.propertyRejects.empty()) {
			std::vector<std::pair<const RE::NiRTTI*, std::uint32_t>> sorted(stats.propertyRejects.begin(), stats.propertyRejects.end());
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			std::string byProperty;
			for (const auto& [rtti, count] : sorted)
				byProperty += fmt::format(" {}={}", rtti && rtti->name ? rtti->name : "?", count);
			logger::info("[DCLF] left native by property type:{}; {} alpha blended, {} opaque ({} alpha tested)",
				byProperty, stats.rejectedBlended, stats.rejectedOpaque, stats.rejectedOpaqueAlphaTest);
		}
		logger::info("[DCLF] pipeline templates: {} of {} drawn only by culled candidates, {} taken over this frame; {} visible objects on a culled template{}",
			stats.pipelinesCulledOnly, stats.pipelines, stats.templateUpgrades, stats.templateDefects,
			stats.templateDefects ? " <- CULLED TEMPLATE" : "");
		// The material cache and its standing alarm. materialCacheStale must be 0: it is the count of
		// entries that were re-evaluated live and disagreed with what the cache would have served.
		logger::info("[DCLF] materials: {} evaluated, {} served from the cache, {} skipped as undrawable; {} floats patched per record; cache {} entries (+{} evicted); validated {}, stale {}{}",
			stats.materialsEvaluated, stats.materialsFromCache, stats.materialsSkipped, stats.materialDriftFloats,
			stats.materialCacheEntries, stats.materialCacheEvicted, stats.materialsValidated, stats.materialCacheStale,
			stats.materialCacheStale ? " <- STALE MATERIAL" : "");
		if (stats.materialCacheStale)
			logger::warn("[DCLF] material cache staleness is in: {}{}{}{}{}{}",
				(stats.materialDiffMask & 1) ? "vs " : "", (stats.materialDiffMask & 2) ? "ps " : "",
				(stats.materialDiffMask & 4) ? "textures " : "", (stats.materialDiffMask & 8) ? "address " : "",
				(stats.materialDiffMask & 16) ? "filter " : "", (stats.materialDiffMask & 32) ? "written" : "");
		logger::info("[DCLF] tracked {} under {} category nodes: {} objects ({} the engine also kept), {} geometries, {} pipelines, {} materials; left native:{}; events +{} -{}, validation drops {}; CPU per frame: events {:.3f} ms, tables {:.3f} ms (scene {:.3f}, max {:.3f}; accumulate {:.3f}, max {:.3f}){}",
			stats.tracked, stats.categoryNodes, stats.objects, stats.nativeVisible, stats.geometries, stats.pipelines, stats.materials, reasons,
			stats.attachedEvents, stats.detachedEvents, stats.validationDrops, timing.eventsMs / frames,
			(timing.sceneMs + timing.buildMs) / frames, timing.sceneMs / frames, timing.sceneMaxMs, timing.buildMs / frames, timing.buildMaxMs,
			parts);
		{
			std::string rejects;
			for (std::size_t r = 1; r < stats.shadowRejects.size(); ++r) {
				if (stats.shadowRejects[r])
					rejects += fmt::format(" {}={}", DCLF::ShadowRejectName(static_cast<DCLF::ShadowReject>(r)), stats.shadowRejects[r]);
			}
			logger::info("[DCLF] shadow casters (last frame): {} of {} records would be drawn into a shadow map; not casters:{}; {} views this frame",
				stats.shadowCasters, stats.objects, rejects.empty() ? " none" : rejects, DCLF::ShadowViews::Get().All().size());
		}
		if (stats.accumulatedWithoutRecord)
			logger::warn("[DCLF] two-phase frame: {} objects the engine accumulated had no record from the scene phase, so they stayed native",
				stats.accumulatedWithoutRecord);
		timing = {};
		store.ResetTimes();
	}
}

bool DrawcallLimitFix::SkipNativePass(RE::BSRenderPass* a_pass)
{
	// CS_DCLF_HYBRID_NOSKIP=1: DCLF draws into the frame but the native loop keeps drawing everything, so
	// what DCLF fails to draw is still visible. It tells apart the two ways an object can go missing.
	const auto toggles = DCLF::Toggles::Get().Active();
	const bool noSkip = toggles.hybridNoSkip;
	// CS_DCLF_ONLY_ELIGIBLE=1: the reverse skip, for parity. The native loop draws only what the indirect
	// draws also draw, so the native depth and G-buffer hold the same object set as DCLF's own targets and
	// the two can be compared pixel for pixel.
	const bool onlyEligible = toggles.onlyEligible;
	if (noSkip && !onlyEligible)
		return false;
	if (!Running() || !a_pass || !a_pass->geometry || (!DCLF::IndirectDraws::Hybrid() && !onlyEligible))
		return false;
	if (!inDepthPass && !globals::deferred->deferredPass)
		return false;  // shadows, reflections and cubemaps keep drawing everything
	// A decal's depth-pass draw stays native. DCLF draws decals in a second colour pass that writes no
	// depth (they are not occluders), so a decal with kZBufferWrite that the native depth pass would
	// have written must still get that write from the native pass; skipping it here would leave the
	// decal's depth out of the frame entirely.
	if (inDepthPass && a_pass->shaderProperty &&
		a_pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kDecal, RE::BSShaderProperty::EShaderPropertyFlag::kDynamicDecal))
		return false;
	if (onlyEligible) {
		// Membership of this frame's tables, not what the epoch drew: the epoch only runs once the main pass
		// has drawn, so a rule based on the frame before would skip everything and never start.
		if (DCLF::SceneStore::Get().FindObject(a_pass->geometry) >= 0) {
			++skipCounters.kept;
			if (skipCounters.kept == 1)
				logger::info("[DCLF] parity: keeping '{}' (depth pass {}, deferred {})", a_pass->geometry->name.c_str() ? a_pass->geometry->name.c_str() : "?",
					inDepthPass, globals::deferred->deferredPass);
			return false;
		}
		++skipCounters.skipped;
		return true;
	}
	auto& store = DCLF::SceneStore::Get();
	if (!DCLF::IndirectDraws::Get().DrewLastFrame(a_pass->geometry, store.GetFrame()))
		return false;
	// A geometry the epoch drew in the frame before but that is not in this frame's tables would be left
	// out of the frame entirely: the pass stays native and the mismatch is reported.
	if (skipSamples.size() < 12 && a_pass->geometry->name.c_str())
		skipSamples.emplace_back(a_pass->geometry->name.c_str());
	if (store.FindObject(a_pass->geometry) < 0) {
		++skipCounters.notInTables;
		if (skipCounters.notInTables == 1 && a_pass->geometry->name.c_str())
			logger::warn("[DCLF] hybrid: '{}' was drawn last frame but is not in this frame's tables; it stays native", a_pass->geometry->name.c_str());
		return false;
	}
	return true;
}

void DrawcallLimitFix::Hooks::Main_RenderDepth::thunk(bool a_a1, bool a_a2)
{
	auto& feature = globals::features::drawcallLimitFix;
	feature.inDepthPass = true;
	func(a_a1, a_a2);
	feature.inDepthPass = false;
	if (!feature.Running())
		return;
	// The Z-prepass, at the end of the native depth pass: DCLF's objects go into the depth buffer before
	// anything is derived from it. This thunk is the inner one of the chain on this call site (DCLF installs
	// before Terrain Blending, so Terrain Blending wraps it), which is what puts the prepass ahead of the
	// blended depth Terrain Blending builds.
	DCLF::IndirectDraws::Get().CaptureDepthPass();
	feature.RefreshDepthConsumers();
}

template <int N>
void DrawcallLimitFix::Hooks::BSBatchRenderer_RenderPassImmediately<N>::thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	auto& feature = globals::features::drawcallLimitFix;
	++feature.skipCounters.offered;
	if (DCLF::DecalProbe::Enabled())
		DCLF::DecalProbe::Get().OnPassOffered(a_pass, feature.inDepthPass);
	if (feature.SkipNativePass(a_pass)) {
		++(feature.inDepthPass ? feature.skipCounters.skippedInDepth : feature.skipCounters.skippedInOpaque);
		++feature.skipCounters.skipped;
		return;
	}
	func(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

void DrawcallLimitFix::Hooks::BSShaderAccumulator_FinishAccumulating::thunk(RE::BSGraphics::BSShaderAccumulator* a_accumulator, std::uint32_t a_renderFlags)
{
	func(a_accumulator, a_renderFlags);
	if (!globals::features::drawcallLimitFix.Running())
		return;
	const auto mode = static_cast<std::uint32_t>(a_accumulator->GetRuntimeData().renderMode);
	if (mode < 0xD || mode > 0xF)
		return;  // shadow-map modes only: plain, clamped, paraboloid (engine notes: shadow maps)
	const auto view = DCLF::ShadowViews::Get().ViewOfAccumulator(a_accumulator);
	if (view != ~0u)
		DCLF::IndirectDraws::Get().ExecuteShadowView(view, mode);
}

void DrawcallLimitFix::Hooks::Install()
{
	// Always installed: the shadow views are a live toggle (Toggles.h), and the thunk costs one compare per
	// accumulator finish when they are off.
	stl::write_vfunc<0x2A, BSShaderAccumulator_FinishAccumulating>(RE::VTABLE_BSShaderAccumulator[0]);
	logger::info("[DCLF] shadow view hook installed on BSShaderAccumulator::FinishAccumulatingPreResolveDepth");
	// The main camera's depth pass, hooked where Terrain Blending hooks it.
	stl::write_thunk_call<Main_RenderDepth>(REL::RelocationID(35560, 36559).address() + Util::VersionedRelocation::Select(0x395, 0x395, 0x3B3));
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<1>>(REL::RelocationID(100877, 107667).address() + REL::Relocate(0x1E5, 0xED));
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<2>>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));
	if (REL::Module::IsSE())  // this call site only exists in SE, as Light Limit Fix's hooks show
		stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<3>>(REL::RelocationID(100871, 107661).address() + 0xEE);
	logger::info("[DCLF] Native pass hooks installed");
}

void DrawcallLimitFix::OnNativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
{
	if (!Running() || DCLF::ConstantEvaluator::Evaluating())
		return;
	if (globals::deferred->deferredPass) {
		auto& store = DCLF::SceneStore::Get();
		// A ProjectedUV draw has SetupGeometry's four projected textures bound now: keep them for the
		// pipelines DCLF builds with that bit (the Hair technique binds none).
		if ((DCLF::PassDescriptorOf(a_pass->passEnum) & 0x8000u) && ((DCLF::PassDescriptorOf(a_pass->passEnum) >> 24) & 0x3f) != 6)
			store.NoteProjectedTextures();
		// The main pass's target formats, once per frame from its first lighting draw.
		// The first lighting draw of the frame whose targets are bound: the engine binds the main pass's
		// targets while it applies a draw's state, so the first draw of the pass can still see none (which
		// is what the parity switches, where most passes are skipped, run into).
		if (captureFrame != store.GetFrame()) {
			const auto formats = CurrentTargetFormats();
			if (formats.colorCount != 0 && formats.depth != DXGI_FORMAT_UNKNOWN) {
				captureFrame = store.GetFrame();
				DCLF::DrawPipelines::Get().SetTargetFormats(formats);
				// Phase 2: what the main pass binds, for this frame's indirect draws (run before the composite).
				DCLF::IndirectDraws::Get().CaptureMainPass();
				// The engine's state objects behind any key that carries state bits (decals), read here
				// because this is inside the deferred pass, where the blend table holds the deferred variants.
				DCLF::DrawPipelines::Get().CaptureEngineStates(store.GetTables().pipelines);
			}
		}
	}
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().OnNativeLightingDraw(a_pass, a_renderFlags);
	if (DCLF::DecalProbe::Enabled())
		DCLF::DecalProbe::Get().OnNativeLightingDraw(a_pass, a_renderFlags);
	if (DCLF::SkinProbe::Enabled())
		DCLF::SkinProbe::Get().OnNativeLightingDraw(a_pass, a_renderFlags);
}

void DrawcallLimitFix::RefreshDepthConsumers()
{
	// The Z-prepass has just run, inside the native depth pass, so whatever is derived from the depth buffer
	// after this point already sees DCLF's objects. Terrain Blending's blended depth is one of those: it is
	// built further up this same call site, after this thunk returns.
	//
	// The engine's prepass copy is not, unless Terrain Blending is on to redirect its SRV: the copy is taken
	// inside the depth pass, before this runs. Refresh it so that the effects reaching it through
	// Util::GetCurrentSceneDepthSRV see the same scene the depth buffer holds.
	auto& terrainBlending = globals::features::terrainBlending;
	if (terrainBlending.loaded && terrainBlending.settings.Enabled)
		return;
	auto* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	if (!context || !renderer)
		return;
	const auto& depthStencils = renderer->GetDepthStencilData().depthStencils;
	const auto& main = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	const auto& prepassCopy = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	if (main.texture && prepassCopy.texture) {
		ScopedPerfEvent event("CS DCLF: post-Z-prepass depth copy");
		context->CopyResource(prepassCopy.texture, main.texture);
	}
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void DrawcallLimitFix::BeforeDeferredComposite()
{
	if (!Running())
		return;
	auto& draws = DCLF::IndirectDraws::Get();
	draws.ProbeTargets("before colour");
	draws.Execute();       // off the hybrid path: assemble and draw into the off-screen targets
	draws.ExecuteColour();  // on it: the colour pass, against the depth written at the first draw
	draws.ProbeTargets("after colour");
	// Publish what DCLF owns now that the colour epoch has said what it actually drew. The registration
	// hook reads this on the next frame, before BuildFrame - which is the point: a claim is a standing
	// statement of ownership, not a per-frame decision.
	draws.PublishClaims();
	draws.ShowDebugView();
}

void DrawcallLimitFix::DrawSettings()
{
	const auto& stats = DCLF::SceneStore::Get().GetStats();
	if (!unavailableReason.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.3f, 1.0f));
		ImGui::TextUnformatted("Unavailable this session: the render graph could not start, so everything renders natively.");
		ImGui::PopStyleColor();
		ImGui::TextWrapped("Reason: %s", unavailableReason.c_str());
		return;
	}
	if (!installed) {
		ImGui::TextUnformatted("Not installed (CS_DCLF=0 or VR).");
		return;
	}
	if (!switchedOn) {
		ImGui::TextWrapped("Switched off with the feature's toggle: the game renders natively and DCLF does no frame work. Switch it back on to resume; no restart needed.");
		return;
	}
	// The live toggles: every non-default feature, for A/B comparisons without a restart. They are the
	// REQUESTED set; the render thread applies them at the start of the next frame (Toggles.h).
	auto& toggles = DCLF::Toggles::Get().Requested();
	const auto active = DCLF::Toggles::Get().Active();
	if (ImGui::TreeNodeEx("Live toggles (A/B)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextWrapped("Each is seeded from its CS_DCLF_* switch and applied at the next frame. A change to an object class drops the classification caches, so the frame after it re-classifies everything.");
		ImGui::SeparatorText("Main pass");
		ImGui::Checkbox("Hybrid: draw into the frame, skip natively (CS_DCLF_HYBRID)", &toggles.hybrid);
		ImGui::BeginDisabled(!toggles.hybrid);
		ImGui::Checkbox("Static ownership: withhold claimed passes (CS_DCLF_OWNERSHIP=static)", &toggles.ownership);
		ImGui::EndDisabled();
		int cull = toggles.cullMode;
		if (ImGui::Combo("GPU culling (CS_DCLF_CULL)", &cull, "off\0frustum\0frustum + occlusion\0"))
			toggles.cullMode = static_cast<std::uint8_t>(cull);
		ImGui::Checkbox("Cull the tracked set, not only what the engine kept (CS_DCLF_CULL_INPUT=tracked)", &toggles.cullTracked);
		ImGui::SeparatorText("Object classes");
		ImGui::Checkbox("Skinned (CS_DCLF_SKINNED)", &toggles.skinned);
		ImGui::Checkbox("Trees (CS_DCLF_TREES)", &toggles.trees);
		ImGui::Checkbox("Decals (CS_DCLF_DECALS)", &toggles.decals);
		ImGui::Checkbox("Projected UV (CS_DCLF_PROJECTED_UV)", &toggles.projectedUv);
		ImGui::Checkbox("Terrain (CS_DCLF_MTLAND)", &toggles.mtLand);
		ImGui::SeparatorText("Shadow views");
		ImGui::Checkbox("Draw the shadow views (CS_DCLF_SHADOWS)", &toggles.shadows);
		ImGui::BeginDisabled(!toggles.shadows);
		ImGui::Checkbox("Static shadow ownership: withhold claimed casters (CS_DCLF_SHADOW_OWNERSHIP=static)", &toggles.shadowOwnership);
		ImGui::EndDisabled();
		ImGui::SeparatorText("Diagnostics");
		ImGui::Checkbox("Debug view: show DCLF's targets (CS_DCLF_DEBUG_VIEW)", &toggles.debugView);
		ImGui::Checkbox("Hybrid without skipping: the native loop draws everything too (CS_DCLF_HYBRID_NOSKIP)", &toggles.hybridNoSkip);
		ImGui::Checkbox("Only eligible: the native loop draws only DCLF's set (CS_DCLF_ONLY_ELIGIBLE)", &toggles.onlyEligible);
		ImGui::Checkbox("No Z-prepass (CS_DCLF_NO_ZPREPASS)", &toggles.noZPrepass);
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx("This frame", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text("Active: hybrid %d, ownership %d, cull %u%s, skinned %d, trees %d, decals %d, projected %d, terrain %d, shadows %d, shadow ownership %d",
			active.hybrid, active.ownership, active.cullMode, active.cullTracked ? " (tracked)" : "", active.skinned, active.trees, active.decals,
			active.projectedUv, active.mtLand, active.shadows, active.shadowOwnership);
		ImGui::Text("Tracked geometry: %u (under %u category nodes)", stats.tracked, stats.categoryNodes);
		ImGui::Text("Objects this frame: %u (%u the engine's culling also kept), geometries: %u, pipelines: %u", stats.objects, stats.nativeVisible, stats.geometries, stats.pipelines);
		for (std::size_t i = 1; i < stats.ineligible.size(); ++i) {
			if (stats.ineligible[i])
				ImGui::Text("Left native (%s): %u", DCLF::kIneligibleNames[i].data(), stats.ineligible[i]);
		}
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		ImGui::Text("Indirect draws: %u drawn from %u records, %u epochs; culling tested %u, rejected %u, false negatives %u",
			draws.drawn, draws.records, draws.epochs, draws.cullTested, draws.cullRejected, draws.cullFalseNegatives);
		if (const auto& gpu = RenderGraphRuntime::Get().GpuTimingSummary(); !gpu.empty())
			ImGui::TextUnformatted(("GPU (ORG pass timestamps, last report):\n" + gpu).c_str());
		const auto& capture = DCLF::PassCapture::Get().GetStats();
		if (active.ownership)
			ImGui::Text("Ownership: %u withheld, %u claimed, %u holes", capture.withheld, capture.claimed, capture.holes);
		if (active.shadows) {
			const auto& shadow = DCLF::IndirectDraws::Get().GetShadowStats();
			ImGui::Text("Shadow views (since the last report): %u offered, %u drawn, %u not ready; last view %u inputs, %u records; sampled culling: %u tested, %u rejected",
				shadow.views, shadow.viewsDrawn, shadow.notReady, shadow.inputs, shadow.records, shadow.cullTested, shadow.cullRejected);
			if (active.shadowOwnership)
				ImGui::Text("Shadow ownership: withheld %u / %u / %u (plain / clamped / paraboloid), claimed %u / %u / %u",
					capture.shadowWithheld[0], capture.shadowWithheld[1], capture.shadowWithheld[2], shadow.claimed[0], shadow.claimed[1], shadow.claimed[2]);
		}
		ImGui::TreePop();
	}
}
