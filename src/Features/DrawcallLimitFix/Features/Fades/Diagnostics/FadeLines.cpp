// Fades' CS_DCLF_STATS report lines: the port parity, the fade roots, the write-back and the fade writers.

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeRoots.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeWriters.h"
#include "Features/DrawcallLimitFix/Features/Fades/Diagnostics/FadeDiagnostics.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Engine.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include <algorithm>
#include <string>

namespace DCLF::Features::Fades
{
	namespace
	{
		/** @brief Prints only when the port check ran on a root since the last report. */
		void PortLines(std::uint32_t)
		{
			const auto fadePort = TakePortCheck();
			if (fadePort.checked)
				logger::info("[DCLF] fade port parity (the port against the engine's functions on node copies): {} roots, {} differ{}{}", fadePort.checked, fadePort.differ,
					fadePort.differ ? " <- FADE PORT" : " <- OK", fadePort.first.empty() ? "" : "; first: " + fadePort.first);
		}

		void RootsLines(std::uint32_t)
		{
			const auto& lodTables = Scene::FrameView::Get().GetTables();
			const auto lodSkins = std::count_if(lodTables.skinLodPartitions.begin(), lodTables.skinLodPartitions.end(), [](std::uint32_t a_word) { return a_word != 0; });
			logger::info("[DCLF] fade roots: {} stood in; {} LOD skins (partitions by their root's level on the GPU); {:.0f} entries with engine-drawn parts culled by the engine a frame",
				Scene::FadeRootsStoodIn(), lodSkins, Reports::CutMixedPerFrame());
		}

		/** @brief The write-back's readback and stores since the last report. */
		void WriteBackText(std::string& a_text)
		{
			const auto* scene = IndirectDraws::Get().Scene();
			if (const auto* fades = scene ? SceneOf(*scene) : nullptr; fades && fades->fadeWriteBack) {
				auto& writeBack = *fades->fadeWriteBack;
				const auto applied = writeBack.applied.exchange(0), stale = writeBack.stale.exchange(0), busy = writeBack.busy.exchange(0);
				const auto storeNs = writeBack.storeNs.exchange(0), notWritten = writeBack.notWritten.exchange(0), dropped = writeBack.dropped.exchange(0);
				const auto harvested = writeBack.harvested.exchange(0), withEvents = writeBack.harvestedWithEvents.exchange(0), events = writeBack.events.exchange(0);
				const auto overflowed = writeBack.overflowed.exchange(0), missed = writeBack.missed.exchange(0), setsAdded = writeBack.setsAdded.exchange(0);
				std::uint32_t sets = 0, listEvents = 0;
				for (const auto* set = writeBack.sets.load(std::memory_order_acquire); set; set = set->next) {
					++sets;
					listEvents = std::max(listEvents, set->events);
				}
				// The readback is harvested on re-arm, so replayed frames are read too.
				if (applied || stale || busy || harvested || missed)
					a_text += fmt::format("[DCLF] fade write-back (T7b4/T7b6): {} milestones written onto stood-in roots' nodes by the main thread at Main::Update's entry "
										  "({:.1f} us in all), {} left (no longer DCLF's to write), {} dropped (DCLF off), {} stale (the root listed again or no longer stood "
										  "in), {} frames found the last task still running; readback {} frames read ({} with events), {} events, {} past their list, {} arms "
										  "missed, {} staging sets (+{}), event list {}\n",
						applied, static_cast<double>(storeNs) / 1000.0, notWritten, dropped, stale, busy, harvested, withEvents, events, overflowed, missed, sets, setsAdded,
						listEvents);
			}
		}

		/** @brief Prints only when a writer was skipped. */
		void WritersText(std::string& a_text)
		{
			const auto c = TakeFadeWriterCounts();
			if (!c.gateSkipped && !c.onVisibleSkipped)
				return;
			a_text += fmt::format("[DCLF] fade writers (T7b4; T7f: every listed root): {} fade updates of the animation gate skipped on roots DCLF writes, {} OnVisible fade steps "
								  "skipped ({} roots in the set)\n",
				c.gateSkipped, c.onVisibleSkipped, c.roots);
		}
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		a_context.RegisterReportLines({ .name = "fade port", .point = Api::ReportPoint::PrimaryCullParity, .lines = &PortLines });
		a_context.RegisterReportLines({ .name = "fade roots", .point = Api::ReportPoint::PrimaryCullLast, .lines = &RootsLines });
		a_context.RegisterReportLines({ .name = "fade write-back", .point = Api::ReportPoint::AsyncWorkWriteBack, .text = &WriteBackText });
		a_context.RegisterReportLines({ .name = "fade writers", .point = Api::ReportPoint::AsyncWorkWriteBack, .text = &WritersText });
	}
}
