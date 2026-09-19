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

void DrawcallLimitFix::OnNativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
{
	if (!installed || DCLF::ConstantEvaluator::Evaluating())
		return;
	if (globals::deferred->deferredPass) {
		auto& store = DCLF::SceneStore::Get();
		// The main pass's target formats, once per frame from its first lighting draw.
		static std::uint32_t formatsFrame = ~0u;
		if (formatsFrame != store.GetFrame()) {
			formatsFrame = store.GetFrame();
			DCLF::DrawPipelines::Get().SetTargetFormats(CurrentTargetFormats());
			// Phase 2: what the main pass binds, for this frame's indirect draws (run before the composite).
			DCLF::IndirectDraws::Get().CaptureMainPass();
		}
	}
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().OnNativeLightingDraw(a_pass, a_renderFlags);
}

void DrawcallLimitFix::BeforeDeferredComposite()
{
	if (!installed)
		return;
	// Phase 2: this frame's objects, drawn by the graph into off-screen targets.
	auto& draws = DCLF::IndirectDraws::Get();
	draws.Execute();
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
