#pragma once

// The water batches without draw capture (docs/development/plans/dclf-water-indirect.md, section 13): each batch's draws are
// computed from the passes the engine registered, with the water port's constants (Features/Water/Port), instead of latched from
// the engine's draws. This covers what DCLF's scene path does not claim: the wading displacement mesh (its mask pass in the mask
// batch, its surface in geometry group 10), and any water the scene path refuses.

#include "Features/DrawcallLimitFix/Features/Water/Water.h"

#include <string>
#include <vector>

namespace RE
{
	class BSShaderAccumulator;
}

namespace DCLF::Features::Water::Native
{
	/** @brief What a batch's draws share, read at the batch's call. */
	struct BatchInputs
	{
		View view = View::Surface;
		RE::BSShaderAccumulator* accumulator = nullptr;
		std::uint32_t first = 0, last = 0;  // RenderBatches' technique range
		std::int32_t group = -1;            // its geometry group, or -1 for the accumulator's batch renderer
		std::uint32_t depthFormat = 0;
		const void* directionalShadow = nullptr;  // kDirectionalShadowBytes, the batch's t98 record
		Integrations::PhysicalSky::OrgHandles physSky{};
	};

	/**
	 * @brief Render thread, at a water batch's call, before it. Computes the draws of every water pass the batch renderer holds in
	 * the batch's range, in the engine's order. False, with a_why, when a pass cannot be computed: the batch's water then reaches the engine.
	 */
	bool BuildBatch(const BatchInputs& a_inputs, std::vector<Draw>& a_draws, Family::Owners& a_owners, std::string& a_why);

	/** @brief The water pass groups taken out of the batch renderer, so the engine's call finds nothing of the water to draw. */
	struct Detached
	{
		struct Entry
		{
			void* group = nullptr;  // the PassGroup
			std::array<RE::BSRenderPass*, 5> heads{};
			std::uint32_t bits = 0;
		};
		std::vector<Entry> entries;
		bool restore = false;  // the batch renderer keeps its passes (autoClearPasses off)
	};

	/** @brief Takes the batch's water pass groups out, as the engine's auto clear would after drawing them. */
	Detached DetachBatch(const BatchInputs& a_inputs);
	/** @brief Puts them back when the engine would have kept them. After the engine's call. */
	void Reattach(Detached& a_detached);
}
