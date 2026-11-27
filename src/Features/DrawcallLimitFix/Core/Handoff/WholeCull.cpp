// The whole engine culls of DCLF views: counted per reason, warned about once each.

#include "Features/DrawcallLimitFix/Core/Handoff/WholeCull.h"

#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include <array>
#include <atomic>

namespace DCLF::WholeCull
{
	namespace
	{
		constexpr std::array<const char*, static_cast<std::size_t>(Reason::Count)> kReasonNames{ "sun cascade not covered",
			"sun cascade not covered, no exclusion", "point light not covered", "point light past the mask's 32", "spot light not covered",
			"spot light past the mask's 32", "main camera without a snapshot", "occlusion map not taken" };
		std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Reason::Count)> wholeCulls{};
		std::array<std::atomic<bool>, static_cast<std::size_t>(Reason::Count)> wholeWarned{};
	}

	void Note(Reason a_reason)
	{
		const auto i = static_cast<std::size_t>(a_reason);
		if (i >= wholeCulls.size())
			return;
		wholeCulls[i].fetch_add(1, std::memory_order_relaxed);
		if (!wholeWarned[i].exchange(true, std::memory_order_relaxed))
			logger::warn("[DCLF] the engine culled a view whole ({}) at scene frame {}, loading screen {}: DCLF does not cover it yet (counted in the primary cull's report lines)",
				kReasonNames[i], Scene::FrameView::Get().SceneStep(), Scene::FrameView::LoadingScreenInFrame() ? "up" : "down");
	}

	std::string Line()
	{
		std::string reasons;
		for (std::size_t i = 0; i < wholeCulls.size(); ++i)
			if (const auto n = wholeCulls[i].exchange(0, std::memory_order_relaxed))
				reasons += fmt::format("{}{} {}", reasons.empty() ? "" : ", ", n, kReasonNames[i]);
		return reasons.empty() ? std::string{} : "[DCLF] whole engine culls (views DCLF does not cover yet): " + reasons + " <- WHOLE ENGINE CULL";
	}
}
