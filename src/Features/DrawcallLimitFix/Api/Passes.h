#pragma once

// A graph pass that the frame schedule in Schedule/Frame.cpp names by key, made at graph build time in the schedule's epoch.

#include "RenderGraph/BufferRef.h"
#include <cstdint>
#include <memory>
#include <string_view>

namespace org
{
	class RenderPass;
}

namespace DCLF::Api
{
	// Defined in Core/Extensions/PassContext.h.
	class PassContext;
	class PassOut;
	class PassSetup;

	/** @brief The kind of a pass. */
	enum class PassKind : std::uint8_t
	{
		Compute,
		Copy,
		Render,
	};

	/** @brief The queue a pass prefers. Default is the graphics queue for copy and compute, and no preference for render. */
	enum class PassQueue : std::uint8_t
	{
		Default,
		Compute,
		None,
	};

	/** @brief A pass's frame rows for this frame. A count of 0 means none. */
	struct FrameRows
	{
		std::uint64_t address = 0;
		RenderGraph::BufferRef ref;
		std::uint32_t count = 0;
	};

	/**
	 * @brief A pass the schedule names by key. Set exactly one of make, which returns one pass or null, and emit, which adds any number.
	 * "{view}" in passName becomes the view's prefix.
	 *
	 * - setup: render thread, once, when the scene's GPU tables are made. The pass is skipped while its result is null.
	 *   Only valid in a Main or View epoch.
	 * - latchWord, animatedRows, frameRows: one pass each at most, scheduled in main.depth before core.z.build-draws.
	 *   The latch has room for one stage word. Grow BuildDrawsLatch in GpuLayouts.h first if a second pass needs one.
	 * - animatedRows: the pass writes these rows whole before any of the frame's draws, and BuildDraws points those draws at them.
	 * - frameRows: lets a recording the graph keeps across frames find this frame's backing.
	 * - cpuPipeline: also record the pass in the CPU-driven pipeline, always on the graphics queue. False leaves the work to the CPU there.
	 * - recordAtExecution: CPU-driven pipeline only, recorded late when the commit opens its site.
	 * - gpuLate: with recordAtExecution, also in the GPU-driven graph, recorded at execution by its epoch.
	 */
	struct PassDef
	{
		std::string_view key;
		std::string_view passName;
		PassKind kind = PassKind::Compute;
		PassQueue queue = PassQueue::Default;
		std::shared_ptr<org::RenderPass> (*make)(const PassContext& a_context) = nullptr;
		void (*emit)(const PassContext& a_context, PassOut& a_out) = nullptr;
		std::shared_ptr<const void> (*setup)(const PassSetup& a_setup) = nullptr;
		std::uint32_t (*latchWord)() = nullptr;
		FrameRows (*animatedRows)() = nullptr;
		FrameRows (*frameRows)() = nullptr;
		bool cpuPipeline = false;
		bool recordAtExecution = false;
		bool gpuLate = false;
	};
}
