#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Reports.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Draws.h"
#include "Engine.h"

// IndirectDraws' report lines at the render thread's report. Async work, view epochs, snapshots and revisions.

namespace DCLF
{
	std::string IndirectDraws::AsyncReport()
	{
		std::string text;
		if (auto window = Reports::EngineReadWindowLine(); !window.empty())
			text += window + "\n";
		Extensions::ReportTextAt(Api::ReportPoint::AsyncWorkWriteBack, text);
		text += VisibilityReport();
		// View lines come before the forward epoch's.
		Extensions::ReportTextAt(Api::ReportPoint::AsyncWorkViews, text);
		text += SharedEpochReport();
		Extensions::ReportTextAt(Api::ReportPoint::AsyncWork, text);
		Extensions::ReportTextAt(Api::ReportPoint::AsyncWorkFamilies, text);
		// The builds' store lines are composed in the snapshot builder's pass and printed with its report, a report behind.
		if (auto line = Reports::FrameValuesLine(); !line.empty())
			text += line + "\n";
		if (auto line = Reports::FrameDataLine(); !line.empty())
			text += line + "\n";
		// Rows naming descriptors. Steady while the scene's bindings are, so a growing count is a range never replaced.
		if (impl->scene) {
			const auto content = impl->scene->content.TakeCounts();
			text += fmt::format("[DCLF] content owners: {} ranges naming descriptors, {} owners; {} snapshots made since the last report\n", content.ranges,
				content.owners, content.snapshotsMade);
		}
		{
			text += impl->RevisionReport();
			// The snapshots, and the builder's own lines composed in its pass and taken here.
			text += impl->SnapshotReport();
		}
		static constexpr const char* kNames[3] = { "colour", "zprepass", "shadow" };
		for (std::size_t i = 0; i < stats.async.size(); ++i) {
			auto& a = stats.async[i];
			if (!a.used)
				continue;
			text += fmt::format("[DCLF] {} epochs: {} committed the payload built ahead with their publication\n", kNames[i], a.used);
			if (i == kAsyncZPrepass)
				text += fmt::format("[DCLF] frame slots supplied from an earlier capture (6e E5): {}\n", std::exchange(impl->frameSlotsCarried, 0));
			a = {};
		}
		return text;
	}
}
