#pragma once

#include "RenderGraph/ComputeProgram.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace org
{
	class Buffer;
	class RenderPass;
	struct FramePreparationContext;
	using PassPrepareContext = FramePreparationContext;
}

/**
 * @brief The exclusive prefix sum of a uint buffer on the GPU, as one render-graph pass (PrefixSumCS.hlsl).
 *
 * A two-level block scan, as in BasicRenderer's binning. The usual pairing is a counting sort. A pass counts keys with atomics
 * and keeps each item's rank, this pass turns counts into each key's first slot, and a scatter writes item i to offsets[key] + rank.
 * The owner creates and registers the uint buffers with unordered access, and the graph orders the three passes.
 */
namespace PrefixSum
{
	inline constexpr std::uint32_t kBlockSize = 1024;  // PrefixSumCS.hlsl's
	inline constexpr std::uint32_t kMaxElements = kBlockSize * kBlockSize;

	constexpr std::uint32_t Blocks(std::uint32_t a_elements) { return (a_elements + kBlockSize - 1) / kBlockSize; }

	struct Programs
	{
		std::shared_ptr<const ComputeProgram> blockScan, blockOffsets;
	};

	/** @brief The two programs' descriptions. The owner queues them at setup (Programs::Queue) and shares them between passes. */
	ComputeProgram::Desc BlockScanDesc();
	ComputeProgram::Desc BlockOffsetsDesc();

	struct Desc
	{
		std::shared_ptr<const Programs> programs;
		std::shared_ptr<org::Buffer> counts;     // uint[elements]
		std::shared_ptr<org::Buffer> offsets;    // uint[elements], counts' exclusive prefix sum
		std::shared_ptr<org::Buffer> blockSums;  // uint[Blocks(elements)], scratch
		std::shared_ptr<org::Buffer> total;      // optional uint, the sum of every count
		std::uint32_t elements = 0;              // at most kMaxElements
		// Zero the counts as they are read so the next accumulation needs no clear. The counts must start at zero,
		// and every accumulation must be followed by this pass.
		bool clearCounts = false;
		// Whether an execution scans, null for every one. It may run on a worker, so it must read state published for preparation.
		std::function<bool(const org::PassPrepareContext&)> active;
		// Added to the pass's revision. Anything `active` depends on beyond the preparation's host data.
		std::function<void(std::vector<std::uint64_t>&)> revision;
	};

	std::shared_ptr<org::RenderPass> CreatePass(Desc a_desc);
}

