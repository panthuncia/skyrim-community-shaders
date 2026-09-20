#include "DrawcallLimitFix.h"

#include "Deferred.h"
#include "DrawcallLimitFix/CaptureParity.h"
#include "DrawcallLimitFix/ConstantEvaluator.h"
#include "DrawcallLimitFix/DrawPipelines.h"
#include "DrawcallLimitFix/GpuResources.h"
#include "DrawcallLimitFix/GpuTextures.h"
#include "DrawcallLimitFix/IndirectDraws.h"
#include "DrawcallLimitFix/ShaderPrograms.h"
#include "DrawcallLimitFix/SceneStore.h"
#include "DrawcallLimitFix/SceneTracker.h"
#include "TerrainBlending.h"

namespace
{
	bool StatsEnabled()
	{
		static const bool enabled = [] {
			char buf[4] = {};
			return GetEnvironmentVariableA("CS_DCLF_STATS", buf, sizeof(buf)) && buf[0] == '1';
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
			char buffer[1024] = {};
			if (!GetEnvironmentVariableA("CS_DCLF_TEST_COMMANDS", buffer, sizeof(buffer)))
				return;
			std::string_view text = buffer;
			while (!text.empty()) {
				const auto end = text.find(';');
				const auto item = text.substr(0, end);
				const auto colon = item.find(':');
				if (colon != std::string_view::npos)
					commands.push_back({ static_cast<std::uint32_t>(std::strtoul(std::string(item.substr(0, colon)).c_str(), nullptr, 10)), std::string(item.substr(colon + 1)) });
				text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
			}
		}

		void OnFrame(std::uint32_t a_frame)
		{
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
	DCLF::SceneTracker::Get().Install();
	Hooks::Install();
	installed = true;
}

void DrawcallLimitFix::Reset()
{
	// Every Present, in menus too: the tracker's queue holds references to attached subtrees and
	// must not grow while the world is not rendered.
	if (!installed)
		return;
	const auto start = std::chrono::steady_clock::now();
	DCLF::SceneStore::Get().ProcessEvents();
	timing.eventsMs += MillisecondsSince(start);
}

void DrawcallLimitFix::Prepass()
{
	if (!installed)
		return;

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
		for (const auto& key : store.GetTables().pipelines) {
			if (const auto* program = programs.Find(key, *lighting))
				pipelines.Find(key, *program);
		}
		programs.Update();
		pipelines.Update();
	}

	skipStats = skipCounters;
	skipCounters = {};
	if ((store.GetFrame() % kReportInterval) == 1)
		skipSamples.clear();  // collected again for the next report

	const std::uint32_t frame = store.GetFrame();
	static TestCommands testCommands;
	testCommands.OnFrame(frame);
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().Report(frame, kReportInterval);

	if ((frame % kReportInterval) == 0 && !StatsEnabled()) {
		timing = {};
		store.ResetTimes();
	}
	if (StatsEnabled() && (frame % kReportInterval) == 0) {
		const auto& stats = store.GetStats();
		std::string reasons;
		for (std::size_t i = 1; i < stats.ineligible.size(); ++i) {
			if (stats.ineligible[i])
				reasons += fmt::format(" {}={}", DCLF::kIneligibleNames[i], stats.ineligible[i]);
		}
		const double frames = std::max(1u, timing.frames);
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
		logger::info("[DCLF] indirect draws (last frame): {} drawn, skipped:{} (missing t{} t{} t{} t{}, VS b{:04X} PS b{:04X}); {:.1f} MB uploaded, {:.3f} ms CPU; {} epochs, {} not ready; textures {} cached, rejected {}/{}/{}, {} samplers",
			draws.drawn, skipped, draws.missingTextures[0], draws.missingTextures[1], draws.missingTextures[2], draws.missingTextures[3], draws.missingVertexConstants,
			draws.missingPixelConstants, draws.uploadBytes / 1048576.0,
			draws.cpuMs, draws.epochs, draws.notReady, textures.cached, textures.rejected[1], textures.rejected[2], textures.rejected[3], textures.samplers);
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
		logger::info("[DCLF] derivation (last frame): {} objects compared, {} differ from the drawn technique (bits {:08X}), {} would stay native", stats.derivationChecked,
			stats.derivationDiffers, stats.derivationBits, stats.derivationNative);
		logger::info("[DCLF] tracked {} under {} category nodes: {} objects, {} geometries, {} pipelines, {} materials; left native:{}; events +{} -{}, validation drops {}; CPU per frame: events {:.3f} ms, tables {:.3f} ms (max {:.3f}; walk {:.3f}, classify {:.3f}, pipelines {:.3f}, materials {:.3f})",
			stats.tracked, stats.categoryNodes, stats.objects, stats.geometries, stats.pipelines, stats.materials, reasons,
			stats.attachedEvents, stats.detachedEvents, stats.validationDrops, timing.eventsMs / frames, timing.buildMs / frames, timing.buildMaxMs,
			stats.partMs[0] / frames, stats.partMs[1] / frames, stats.partMs[2] / frames, stats.partMs[3] / frames);
		timing = {};
		store.ResetTimes();
	}
}

bool DrawcallLimitFix::SkipNativePass(RE::BSRenderPass* a_pass)
{
	// CS_DCLF_HYBRID_NOSKIP=1: DCLF draws into the frame but the native loop keeps drawing everything, so
	// what DCLF fails to draw is still visible. It tells apart the two ways an object can go missing.
	static const bool noSkip = [] {
		char buf[4] = {};
		return GetEnvironmentVariableA("CS_DCLF_HYBRID_NOSKIP", buf, sizeof(buf)) && buf[0] == '1';
	}();
	// CS_DCLF_ONLY_ELIGIBLE=1: the reverse skip, for parity. The native loop draws only what the indirect
	// draws also draw, so the native depth and G-buffer hold the same object set as DCLF's own targets and
	// the two can be compared pixel for pixel.
	static const bool onlyEligible = [] {
		char buf[4] = {};
		return GetEnvironmentVariableA("CS_DCLF_ONLY_ELIGIBLE", buf, sizeof(buf)) && buf[0] == '1';
	}();
	if (noSkip && !onlyEligible)
		return false;
	if (!installed || !a_pass || !a_pass->geometry || (!DCLF::IndirectDraws::Hybrid() && !onlyEligible))
		return false;
	if (!inDepthPass && !globals::deferred->deferredPass)
		return false;  // shadows, reflections and cubemaps keep drawing everything
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
}

template <int N>
void DrawcallLimitFix::Hooks::BSBatchRenderer_RenderPassImmediately<N>::thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	auto& feature = globals::features::drawcallLimitFix;
	++feature.skipCounters.offered;
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
		// The main pass's target formats, once per frame from its first lighting draw.
		// The first lighting draw of the frame whose targets are bound: the engine binds the main pass's
		// targets while it applies a draw's state, so the first draw of the pass can still see none (which
		// is what the parity switches, where most passes are skipped, run into).
		static std::uint32_t formatsFrame = ~0u;
		if (formatsFrame != store.GetFrame()) {
			const auto formats = CurrentTargetFormats();
			if (formats.colorCount != 0 && formats.depth != DXGI_FORMAT_UNKNOWN) {
				formatsFrame = store.GetFrame();
				DCLF::DrawPipelines::Get().SetTargetFormats(formats);
				// Phase 2: what the main pass binds, for this frame's indirect draws (run before the composite).
				DCLF::IndirectDraws::Get().CaptureMainPass();
				// Hybrid path: the depth of DCLF's objects goes into the main depth buffer here, at the first
				// draw of the pass, so the native draws that follow and everything that reads depth later see
				// it. The colour pass runs before the composite.
				DCLF::IndirectDraws::Get().ExecuteZPrepass();
				RefreshDepthConsumers();
			}
		}
	}
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().OnNativeLightingDraw(a_pass, a_renderFlags);
}

void DrawcallLimitFix::RefreshDepthConsumers()
{
	// Everything the rest of the frame reads depth through is built at the end of the native depth pass,
	// before the indirect draws have written theirs: the engine's prepass copy, and - when Terrain Blending
	// is on - its blended depth, which it also points every depth SRV at for the rest of the frame. Neither
	// holds DCLF's objects, so the effects that read depth paint the background over them. Rebuild both from
	// the depth buffer now that the Z-prepass has added them.
	//
	// This is the interim placement. The durable one is to run the Z-prepass inside the native depth pass,
	// which needs a depth-only pipeline variant (the one the plan describes: position-only VS, a discard PS
	// for alpha-tested objects) so that assembling it does not depend on the main pass's pixel bindings.
	auto* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	if (!context || !renderer)
		return;

	// The pass is mid-draw: whatever it had bound has to go back before it draws.
	ID3D11RenderTargetView* targets[8] = {};
	ID3D11DepthStencilView* depthView = nullptr;
	context->OMGetRenderTargets(static_cast<UINT>(std::size(targets)), targets, &depthView);

	auto& terrainBlending = globals::features::terrainBlending;
	if (terrainBlending.loaded && terrainBlending.settings.Enabled && terrainBlending.blendedDepthTexture)
		terrainBlending.BlendPrepassDepths();

	const auto& depthStencils = renderer->GetDepthStencilData().depthStencils;
	const auto& main = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	const auto& prepassCopy = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	if (main.texture && prepassCopy.texture)
		context->CopyResource(prepassCopy.texture, main.texture);

	context->OMSetRenderTargets(static_cast<UINT>(std::size(targets)), targets, depthView);
	for (auto* target : targets)
		if (target)
			target->Release();
	if (depthView)
		depthView->Release();
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void DrawcallLimitFix::BeforeDeferredComposite()
{
	if (!installed)
		return;
	auto& draws = DCLF::IndirectDraws::Get();
	draws.Execute();       // off the hybrid path: assemble and draw into the off-screen targets
	draws.ExecuteColour();  // on it: the colour pass, against the depth written at the first draw
	draws.ShowDebugView();
}

void DrawcallLimitFix::DrawSettings()
{
	const auto& stats = DCLF::SceneStore::Get().GetStats();
	ImGui::TextUnformatted("Phase 2: tracked objects are also drawn by the render graph, off screen; the frame still comes from the native draws.");
	ImGui::Text("Tracked geometry: %u (under %u category nodes)", stats.tracked, stats.categoryNodes);
	ImGui::Text("Objects this frame: %u, geometries: %u, pipelines: %u", stats.objects, stats.geometries, stats.pipelines);
	for (std::size_t i = 1; i < stats.ineligible.size(); ++i) {
		if (stats.ineligible[i])
			ImGui::Text("Left native (%s): %u", DCLF::kIneligibleNames[i].data(), stats.ineligible[i]);
	}
}
