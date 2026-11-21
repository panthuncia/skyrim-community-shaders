#pragma once

#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"

namespace DCLF::Diagnostics
{
	/**
	 * @brief The kept scene lists brought up to this frame, before the engine's build adds its own entries. In the build's job.
	 * `kept[l]` entries of list l are DCLF's and `filter` holds the roots they leave out.
	 */
	struct KeptSceneListsBuilt
	{
		const ankerl::unordered_dense::set<const RE::NiAVObject*>* filter;
		const std::uint32_t* kept;
		std::uint32_t count;
	};

	/**
	 * @brief Read-only view of PrimaryCull's private state for the engine observers. Render thread unless noted.
	 * A list job may read the cut only between PrepareFrame and the jobs' Finish.
	 */
	struct PrimaryCullView
	{
		static const PrimaryCull& Self() { return PrimaryCull::Get(); }
		/** @brief The cut. */
		static const auto& Cut() { return Self().cut; }
		/** @brief Whether a plan is a fade root's. */
		static bool FadePlan(std::uint32_t a_entry)
		{
			const auto plan = Self().cut.plans[a_entry];
			return plan == PrimaryCull::EntryPlan::FadeRoot || plan == PrimaryCull::EntryPlan::LeafRoot || plan == PrimaryCull::EntryPlan::TreeRoot;
		}
		static bool Rejected(std::uint32_t a_entry) { return Self().cut.plans[a_entry] == PrimaryCull::EntryPlan::Rejected; }
		/** @brief Whether a plan is Rejected. */
		template <class Plan>
		static bool RejectedPlan(Plan a_plan)
		{
			return a_plan == PrimaryCull::EntryPlan::Rejected;
		}
		/** @brief DCLF's enumeration of the object root's roots. The build's job. */
		static bool ListRoots(const RE::NiAVObject*& a_objectRoot, std::vector<const RE::NiAVObject*>& a_roots) { return PrimaryCull::ListRoots(a_objectRoot, a_roots); }
		/** @brief The cut's frames since its counters were last taken. */
		static std::uint64_t CutFrames() { return Self().cutStats.frames; }
		/** @brief Whether this frame's snapshot is current, so the stand-in runs. */
		static bool StandInLive() { return Self().standInLive; }
		/** @brief The frame's set, or null. */
		static const auto& FrameSet() { return Self().frameSet; }
		/** @brief Whether the cut stands in for a_node whole this frame, so the engine's cull does not write its bit. */
		static bool StoodInWhole(const void* a_node);
		/** @brief The stood-in fade roots. */
		static const std::vector<const void*>& StandInList() { return Self().standInList; }
		/** @brief The main camera BeginFrameCamera read this frame. */
		static const void* FrameCamera() { return Self().frameCamera; }
	};
}
