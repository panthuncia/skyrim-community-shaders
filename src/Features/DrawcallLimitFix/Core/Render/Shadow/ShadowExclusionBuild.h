#pragma once

// The input handed to a shadow exclusion contribution for one shadow mode, and the slot for its answer.
// The answer's type belongs to the contributing feature and is opaque to Core.

#include "Features/DrawcallLimitFix/Api/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/EntrySpace.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF
{
	struct SceneTables;
}

namespace DCLF::Api
{
	/**
	 * @brief One exclusion build for the sun cascades or the point light paraboloids.
	 * Runs on the snapshot builder's thread, or as a pool task each when both modes build.
	 */
	class ShadowExclusionBuild
	{
	public:
		ShadowExclusionBuild(ShadowSource a_source, const std::shared_ptr<const EntrySpace>& a_candidates, const SceneTables& a_tables,
			const KeptView<Draws::DrawInput>& a_region, const std::vector<Draws::DrawInput>& a_list) :
			source(a_source),
			candidates(a_candidates),
			tables(a_tables),
			region(a_region),
			list(a_list)
		{}

		ShadowSource source;
		const std::shared_ptr<const EntrySpace>& candidates;
		const SceneTables& tables;
		std::uint32_t modeBits = 0;             // view bits that select this mode's inputs
		std::uint64_t membership = 0;           // 0 without the kept state
		std::uint32_t frameNumber = 0;
		std::uint32_t tablesGeneration = 0;
		// Objects that joined and left the mode's region inputs since the previous kept build, for incremental caches.
		// movesValid is false when the build retook the whole mode, so a cache must rebuild.
		std::uint64_t keptBuild = 0;
		bool movesValid = false;
		std::span<const std::uint32_t> joined, left;
		std::shared_ptr<void> result;           // null means no answer

		/** @brief The point light mode, whose culls use the reach words. */
		bool Paraboloid() const { return source == ShadowSource::Point; }
		/** @brief Visits the region's inputs, then the frame's own, in the same order as ShadowPayload::ForEachInput. */
		template <class F>
		void ForEachInput(F&& a_visit) const
		{
			if (region.elements)
				for (const auto& input : *region.elements)
					if (input.view.mask & modeBits)
						a_visit(input);
			for (const auto& input : list)
				if (input.view.mask & modeBits)
					a_visit(input);
		}
		/** @brief The region part of ForEachInput. */
		template <class F>
		void ForEachRegionInput(F&& a_visit) const
		{
			if (region.elements)
				for (const auto& input : *region.elements)
					if (input.view.mask & modeBits)
						a_visit(input);
		}
		/** @brief The frame list part of ForEachInput. It holds only a few face shapes. */
		template <class F>
		void ForEachListInput(F&& a_visit) const
		{
			for (const auto& input : list)
				if (input.view.mask & modeBits)
					a_visit(input);
		}

	private:
		const KeptView<Draws::DrawInput>& region;
		const std::vector<Draws::DrawInput>& list;
	};
}
