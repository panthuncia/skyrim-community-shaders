#include "DrawcallLimitFix.h"

#include "Deferred.h"
#include "DrawcallLimitFix/CaptureParity.h"
#include "DrawcallLimitFix/ConstantEvaluator.h"
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

	// Render-thread CPU spent on scene capture, averaged over a report interval.
	struct CaptureTiming
	{
		double eventsMs = 0.0;
		double buildMs = 0.0;
		double buildMaxMs = 0.0;
		std::uint32_t frames = 0;
	} timing;

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

	const std::uint32_t frame = store.GetFrame();
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().Report(frame, kReportInterval);

	if ((frame % kReportInterval) == 0 && !StatsEnabled())
		timing = {};
	if (StatsEnabled() && (frame % kReportInterval) == 0) {
		const auto& stats = store.GetStats();
		std::string reasons;
		for (std::size_t i = 1; i < stats.ineligible.size(); ++i) {
			if (stats.ineligible[i])
				reasons += fmt::format(" {}={}", DCLF::kIneligibleNames[i], stats.ineligible[i]);
		}
		logger::info("[DCLF] tracked {} under {} category nodes: {} objects, {} geometries, {} pipelines, {} materials; left native:{}; events +{} -{}, validation drops {}; CPU per frame: events {:.3f} ms, tables {:.3f} ms (max {:.3f})",
			stats.tracked, stats.categoryNodes, stats.objects, stats.geometries, stats.pipelines, stats.materials, reasons,
			stats.attachedEvents, stats.detachedEvents, stats.validationDrops,
			timing.eventsMs / std::max(1u, timing.frames), timing.buildMs / std::max(1u, timing.frames), timing.buildMaxMs);
		timing = {};
	}
}

void DrawcallLimitFix::OnNativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
{
	if (!installed || DCLF::ConstantEvaluator::Evaluating())
		return;
	if (globals::deferred->deferredPass)
		DCLF::SceneStore::Get().SetMainPassRenderFlags(a_renderFlags);
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().OnNativeLightingDraw(a_pass, a_renderFlags);
}

void DrawcallLimitFix::DrawSettings()
{
	const auto& stats = DCLF::SceneStore::Get().GetStats();
	ImGui::TextUnformatted("Phase 1: scene capture only; nothing is drawn by the render graph yet.");
	ImGui::Text("Tracked geometry: %u (under %u category nodes)", stats.tracked, stats.categoryNodes);
	ImGui::Text("Objects this frame: %u, geometries: %u, pipelines: %u", stats.objects, stats.geometries, stats.pipelines);
	for (std::size_t i = 1; i < stats.ineligible.size(); ++i) {
		if (stats.ineligible[i])
			ImGui::Text("Left native (%s): %u", DCLF::kIneligibleNames[i].data(), stats.ineligible[i]);
	}
}
