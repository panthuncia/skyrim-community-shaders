#pragma once

// Deferred ranges: objects the colour segment culls once and a later epoch draws, bucket by bucket, each bucket with its own
// stencil reference. Water is the first user: its mask and its surface. The slots are Core's, and a registration names them.
//
// - Which objects: every object whose pipeline key belongs to a program family built as ProgramBuild::DeferredRanges. Such an object
//   gets view bit kViewWater alone, so it is never a depth segment input and never writes the HZB.
// - The cull: the colour segment's BuildDraws, frustum then the HZB the depth epoch built, once per object.
// - The draws: plain indexed indirect draws from the index pool, one per bucket of (set index, stencil reference), in the Z-prepass
//   layout and the plain draw signature. Not device-generated commands, since the stencil reference cannot change per sequence.
// - The epoch: an epoch of the schedule that shares main.colour's shape and lists the range's pass key ("core.deferred-range0" or
//   "core.deferred-range1"). Its owner submits it at its trigger with Draws::SubmitDeferredEpoch.

#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"
#include "RenderGraph/FrameAnchors.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	/** @brief Core's fixed slot per deferred range. Its pass key, count words and sequence range are Core's. */
	enum class DeferredRangeSlot : std::uint8_t
	{
		Range0,  // pass key "core.deferred-range0"
		Range1,  // pass key "core.deferred-range1"
		Count,
	};
	inline constexpr std::size_t kDeferredRanges = static_cast<std::size_t>(DeferredRangeSlot::Count);
	/** @brief The most colour targets a deferred range's pass binds. */
	inline constexpr std::uint32_t kDeferredRangeTargets = 4;

	/** @brief A pipeline key as a deferred range sees it. The same fields as Core's PipelineKey. */
	struct DeferredKey
	{
		std::uint32_t vertexDescriptor = 0;
		std::uint32_t pixelDescriptor = 0;
		std::uint32_t rasterFlags = 0;
		std::uint32_t passDescriptor = 0;
		std::uint64_t vertexLayout = 0;
	};

	/**
	 * @brief A deferred range.
	 *
	 * - family: the program family whose keys the range draws. Its build must be ProgramBuild::DeferredRanges.
	 * - name: the nvperf range "cs.dclf.deferred.<name>" and the report name.
	 * - draws: snapshot builder, once per key slot per plan. Whether a key draws in this range, and its stencil reference there.
	 *   A pure function of the key. Core groups the range's draws into buckets by set index, ordered by stencil reference.
	 * - target: render thread, at the graph's build. The pass's colour target a_index, or a null texture past the last one.
	 *   The pass loads and stores them. Depth and stencil are the main depth, loaded and stored.
	 */
	struct DeferredRangeContribution
	{
		DeferredRangeSlot slot = DeferredRangeSlot::Count;
		std::string_view name;
		ProgramFamily family = ProgramFamily::Count;
		bool (*draws)(const DeferredKey& a_key, std::uint8_t& a_stencilRef) = nullptr;
		FrameAnchors::Texture (*target)(std::uint32_t a_index) = nullptr;
	};
}
