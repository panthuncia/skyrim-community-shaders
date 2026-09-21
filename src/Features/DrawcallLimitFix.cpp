#include "DrawcallLimitFix.h"

#include "Deferred.h"
#include "DrawcallLimitFix/CaptureParity.h"
#include "DrawcallLimitFix/ConstantEvaluator.h"
#include "DrawcallLimitFix/DecalProbe.h"
#include "DrawcallLimitFix/SkinProbe.h"
#include "DrawcallLimitFix/DrawPipelines.h"
#include "DrawcallLimitFix/GpuResources.h"
#include "DrawcallLimitFix/GpuTextures.h"
#include "DrawcallLimitFix/IndirectDraws.h"
#include "DrawcallLimitFix/ShaderPrograms.h"
#include "DrawcallLimitFix/PassCapture.h"
#include "DrawcallLimitFix/SceneStore.h"
#include "DrawcallLimitFix/SceneTracker.h"
#include "DrawcallLimitFix/Switches.h"
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

	// Render-thread CPU spent on scene capture, averaged over a report interval.
	struct CaptureTiming
	{
		double eventsMs = 0.0;
		double buildMs = 0.0;
		double buildMaxMs = 0.0;
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
	Hooks::Install();
	installed = true;
	// The switches this process actually sees, once. Several reports below are gated on them, so without
	// this a silent log is indistinguishable from a switch that never reached the game - which is exactly
	// what happened when they came from the environment alone (see Switches.h).
	logger::info("[DCLF] switches: {}", DCLF::SwitchSummary());
}

void DrawcallLimitFix::Reset()
{
	// The test commands run before the installed check, so a CS_DCLF=0 baseline reaches the same place at
	// the same in-game hour as the run it is compared against.
	static TestCommands testCommands;
	testCommands.OnFrame();

	// Every Present, in menus too: the tracker's queue holds references to attached subtrees and
	// must not grow while the world is not rendered.
	if (!installed)
		return;
	const auto start = std::chrono::steady_clock::now();
	DCLF::SceneStore::Get().ProcessEvents();
	timing.eventsMs += MillisecondsSince(start);
}

void DrawcallLimitFix::EarlyPrepass()
{
	if (!installed)
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
	const auto start = std::chrono::steady_clock::now();
	store.BuildFrame();
	const double buildMs = MillisecondsSince(start);
	timing.buildMs += buildMs;
	timing.buildMaxMs = std::max(timing.buildMaxMs, buildMs);
	++timing.frames;

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
	}
}

void DrawcallLimitFix::Prepass()
{
	if (!installed)
		return;

	auto& store = DCLF::SceneStore::Get();
	// The one point in the frame where the main camera's accumulator is identifiable: EarlyPrepass, where
	// the tables are now built, is too early for `currentAccumulator` to be set.
	store.LatchAccumulator();
	// The camera-dependent half of the per-frame constants, now that the main camera's shadow state is
	// current (BuildFrame ran at EarlyPrepass, where it still belonged to the shadow-map camera).
	store.RefreshFrameConstants();
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
		logger::info("[DCLF] SPIR-V programs: {} requested, {} ready ({} stages from cache), {} failed", shaders.requested, shaders.ready, shaders.fromCache, shaders.failed);
		const auto& indirect = DCLF::DrawPipelines::Get().GetStats();
		logger::info("[DCLF] indirect pipelines: {} requested, {} in the set, {} failed; target changes {}", indirect.requested, indirect.ready, indirect.failed,
			indirect.targetChanges);
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
		logger::info("[DCLF] indirect epoch CPU by part: {}", epochParts);
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
		logger::info("[DCLF] tracked {} under {} category nodes: {} objects ({} the engine also kept), {} geometries, {} pipelines, {} materials; left native:{}; events +{} -{}, validation drops {}; CPU per frame: events {:.3f} ms, tables {:.3f} ms (max {:.3f}){}",
			stats.tracked, stats.categoryNodes, stats.objects, stats.nativeVisible, stats.geometries, stats.pipelines, stats.materials, reasons,
			stats.attachedEvents, stats.detachedEvents, stats.validationDrops, timing.eventsMs / frames, timing.buildMs / frames, timing.buildMaxMs,
			parts);
		timing = {};
		store.ResetTimes();
	}
}

bool DrawcallLimitFix::SkipNativePass(RE::BSRenderPass* a_pass)
{
	// CS_DCLF_HYBRID_NOSKIP=1: DCLF draws into the frame but the native loop keeps drawing everything, so
	// what DCLF fails to draw is still visible. It tells apart the two ways an object can go missing.
	static const bool noSkip = [] {
		return DCLF::SwitchEnabled("CS_DCLF_HYBRID_NOSKIP");
	}();
	// CS_DCLF_ONLY_ELIGIBLE=1: the reverse skip, for parity. The native loop draws only what the indirect
	// draws also draw, so the native depth and G-buffer hold the same object set as DCLF's own targets and
	// the two can be compared pixel for pixel.
	static const bool onlyEligible = [] {
		return DCLF::SwitchEnabled("CS_DCLF_ONLY_ELIGIBLE");
	}();
	if (noSkip && !onlyEligible)
		return false;
	if (!installed || !a_pass || !a_pass->geometry || (!DCLF::IndirectDraws::Hybrid() && !onlyEligible))
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

void DrawcallLimitFix::Hooks::Install()
{
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
	if (!installed || DCLF::ConstantEvaluator::Evaluating())
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
	if (main.texture && prepassCopy.texture)
		context->CopyResource(prepassCopy.texture, main.texture);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void DrawcallLimitFix::BeforeDeferredComposite()
{
	if (!installed)
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
	ImGui::TextUnformatted("Phase 2: tracked objects are also drawn by the render graph, off screen; the frame still comes from the native draws.");
	ImGui::Text("Tracked geometry: %u (under %u category nodes)", stats.tracked, stats.categoryNodes);
	ImGui::Text("Objects this frame: %u (%u the engine's culling also kept), geometries: %u, pipelines: %u", stats.objects, stats.nativeVisible, stats.geometries, stats.pipelines);
	for (std::size_t i = 1; i < stats.ineligible.size(); ++i) {
		if (stats.ineligible[i])
			ImGui::Text("Left native (%s): %u", DCLF::kIneligibleNames[i].data(), stats.ineligible[i]);
	}
}
