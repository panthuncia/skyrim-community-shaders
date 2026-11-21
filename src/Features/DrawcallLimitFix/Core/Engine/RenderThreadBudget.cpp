// Per-frame render-thread time by bucket and hook site, and its report line. Render thread only. BasicTelemetry's ThreadBudget counts.
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"

#include "RenderGraph/RenderGraphRuntime.h"

#include <fmt/format.h>

namespace DCLF
{
	namespace
	{
		constexpr const char* kBucketNames[] = { "scene exchange (snapshot taken and adopted)", "scene captures (globals, categories, mirror, switch catch-ups)",
			"capture", "submit", "ORG overhead", "engine boundary", "other" };
		constexpr const char* kSiteNames[] = { "Present", "scene frame", "BeforeShadowMaps", "AfterShadowMaps (shadow epoch)", "shadow view capture",
			"occlusion", "EarlyPrepass", "Prepass", "Z-prepass", "BeforeOpaquePass", "AfterOpaquePass (colour epoch)", "reflection face" };
		static_assert(std::size(kBucketNames) == static_cast<std::size_t>(RenderThreadBudget::Bucket::Count));
		static_assert(std::size(kSiteNames) == static_cast<std::size_t>(RenderThreadBudget::Site::Count));

		RenderThreadBudget::Budget::SplitTotals EpochSplits()
		{
			const auto& epochs = RenderGraphRuntime::Get().RenderThreadEpochTotals();
			return { epochs.submitUs, epochs.overheadUs };
		}
	}

	RenderThreadBudget& RenderThreadBudget::Get()
	{
		static RenderThreadBudget budget;
		return budget;
	}

	// A load screen's Present runs on another thread, which is not the frame's, so the scene frame's hook claims the thread.
	// The epochs' submission and other ORG work inside a hook go to their buckets.
	RenderThreadBudget::RenderThreadBudget() :
		budget(Budget::Desc{ Site::SceneFrame, Bucket::Other, { Bucket::Submit, Bucket::OrgOverhead }, &EpochSplits })
	{}

	const char* RenderThreadBudget::BucketName(Bucket a_bucket)
	{
		const auto i = static_cast<std::size_t>(a_bucket);
		return i < std::size(kBucketNames) ? kBucketNames[i] : "?";
	}

	std::string RenderThreadBudget::Report()
	{
		const auto totals = budget.Take();
		if (!totals)
			return {};
		const double n = totals->frames;
		std::string buckets, sites;
		for (std::size_t i = 0; i < totals->bucketUs.size(); ++i)
			buckets += fmt::format("{}{} {:.3f}", i ? ", " : "", kBucketNames[i], totals->bucketUs[i] / n / 1000.0);
		for (std::size_t i = 0; i < totals->siteUs.size(); ++i)
			if (totals->siteCalls[i])
				sites += fmt::format("{}{} {:.3f} ({:.1f} calls)", sites.empty() ? "" : ", ", kSiteNames[i], totals->siteUs[i] / n / 1000.0, totals->siteCalls[i] / n);
		return fmt::format("[DCLF] render thread, ms per frame over {} frames: {:.3f} (max {:.3f}); by bucket: {}; by hook: {}", totals->frames,
			totals->totalUs / n / 1000.0, totals->frameMaxUs / 1000.0, buckets, sites);
	}
}
