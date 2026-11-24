#pragma once

// The deferred ranges' draw passes and their shared epoch's submission (Api/DeferredRanges.h).

#include "Features/DrawcallLimitFix/Api/Passes.h"

#include <cstdint>
#include <span>

namespace DCLF::Draws
{
	/** @brief Core's pass definitions, one per deferred range slot: "core.deferred-range0" and "core.deferred-range1". */
	std::span<const Api::PassDef> DeferredRangePassDefs();

	/**
	 * @brief Render thread, inside a_epoch's trigger scope. Submits a_epoch, an epoch sharing main.colour's shape whose passes draw
	 * deferred ranges, at most once a frame and only after this frame's colour epoch. Skipped, boundary included, while no deferred
	 * range has a bucket, and always for a dormant epoch. Each range draws the target set its variant picks now. Returns whether it
	 * was submitted.
	 */
	bool SubmitDeferredEpoch(std::uint32_t a_epoch, std::uint32_t a_range);

	/**
	 * @brief Render thread, at a deferred range's engine batch in a CPU-driven frame. The colour commit's CPU-culled draws of range
	 * a_range, drawn into the target set a_variant picks now (Direct::RecordWaterDraws), or null: not a CPU-driven graph, the range is not
	 * registered, or the commit opened none.
	 */
	void* TakeCpuRange(std::uint32_t a_range, std::uint32_t& a_variant);

	/** @brief Any thread. The runs since the last call whose passes found no target set chosen, so drew nothing of their range. */
	std::uint64_t TakeDeferredRunsWithoutChoice();
}
