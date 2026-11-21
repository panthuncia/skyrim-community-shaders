#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Reports.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"
#include "Engine.h"

// View epoch report lines. The shared epochs' point, the accumulated visibility and the shadow capability.

namespace DCLF
{
	std::string IndirectDraws::SharedEpochReport()
	{
		if (!impl)
			return {};
		// Report while an epoch shares a main shape, as the forward epoch does.
		bool due = false;
		for (std::uint32_t e = 0; e < EpochCount() && !due; ++e) {
			const auto& traits = EpochTraitsOf(e);
			due = traits.sharesWith != kNoEpoch && EpochTraitsOf(traits.sharesWith).shape == ShapeKind::Main;
		}
		if (!due)
			return {};
		// The forward and refraction lines, then the forward epoch's features' lines.
		std::string text;
		Extensions::ReportTextAt(Api::ReportPoint::AsyncWorkForward, text);
		return text;
	}

	SharedEpochStats IndirectDraws::TakeSharedEpochStats(std::uint32_t a_epoch)
	{
		if (!impl || a_epoch >= EpochCount())
			return {};
		auto& f = impl->sharedEpochs[a_epoch];
		const SharedEpochStats taken = f;
		f = {};
		f.submittedFrame = taken.submittedFrame;
		return taken;
	}

	std::string IndirectDraws::VisibilityReport()
	{
		using Readback = VisibilityReadback;
		if (!impl->scene || !impl->scene->visibilityReadback)
			return {};
		auto& r = *impl->scene->visibilityReadback;
		const auto take = [](std::atomic<std::uint64_t>& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); };
		// The writes, on the normal path.
		std::string text = fmt::format(
			"[DCLF] accumulated writes (T7b2): {} write lists made at Main::Update's entry ({} full), {} set, {} cleared, {} already so, "
			"{} clears held by a camera cut ({} cuts, {} roots set by them), {} of a released publication (oldest list {} frames); {:.1f} us on the main thread\n",
			take(r.writeBatches), take(r.fullWrites), take(r.writesSet), take(r.writesCleared), take(r.writesUnchanged), take(r.writesHeldByCut),
			take(r.cuts), take(r.cutWrites), take(r.writesExpired), r.writeAgeMax.exchange(0, std::memory_order_relaxed), double(take(r.writeNs)) / 1000.0);
		{
			text += fmt::format(
				"[DCLF] accumulated, every listed root (T7f checklist item 1): wide writes {} set, {} cleared, {} already so; {} nodes between "
				"set ({} geometries whose walk missed their root); cell roots {} seen, {} set\n",
				take(r.wideSet), take(r.wideCleared), take(r.wideUnchanged), take(r.wideNodesSet), take(r.wideNoChain), take(r.cellRootsSeen), take(r.cellRootsSet));
			if (!r.wideNoChainFirst.empty())
				text += fmt::format("[DCLF] accumulated, geometries whose walk missed their root (T7f): {}\n", r.wideNoChainFirst);
			r.wideNoChainFirst.clear();
		}
		Extensions::ReportTextAt(Api::ReportPoint::AccumulatedWrites, text);
		return text;
	}

	std::string IndirectDraws::ShadowCapabilityReport() const
	{
		const auto& c = impl->shadowEngine->capabilityStats;
		return fmt::format("[DCLF] shadow capability: phases {:#x}, {} rasterizer states in the catalog; {} changes since startup, {} of them views it did not foresee{}{}",
			ShadowCapability(), DrawPipelines::Get().ShadowRasterStateCount(), c.changes, c.defects, c.defects ? " <- PHASES" : " <- OK",
			c.lastCause.empty() ? "" : " (last: " + c.lastCause + ")");
	}
}
