#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace org::runtime
{
	class IStatisticsService;
}

/**
 * @brief GPU timings from the render graph's own timestamp queries (ORG statistics, BasicRHI query pools), with no D3D11
 * query and no DXVK export.
 *
 * Two switches pick where the timers and diagnostics read the GPU from:
 * - CS_GPU_TIMERS=graph: the profiling window, CS_PROFILER_LOG and CS_GPU_EVENT_TIMERS take their GPU times from the graph
 *   alone. The profiler creates no D3D11 query: its ranges around D3D11 work keep their CPU times and have no GPU time. The
 *   graph's passes report as before ("<Feature>::<segment> / <pass>"). The default, d3d11, keeps the D3D11 timestamp queries
 *   for those ranges beside the graph's times.
 * - CS_GPU_IDLE_SOURCE=graph: CS_GPU_IDLE_TRACE reads the GPU's busy spans from the graph's ranges instead of DXVK's
 *   submission trace. The default, dxvk, keeps the trace.
 *
 * What the graph sees: every pass's timestamp pair on each queue, the barrier batches of kept recordings, and the sub-ranges a
 * pass times inside itself. Each range belongs to the Present its epoch was submitted before. A Present's ranges are handed on
 * once every epoch submitted in it has completed.
 *
 * Header free of ORG/Vulkan includes, like RenderGraphRuntime.h.
 */
namespace GpuTimingSource
{
	/** @brief Where the profiler's ranges and the event timers take GPU time from (CS_GPU_TIMERS). */
	enum class Timers : std::uint8_t
	{
		D3D11,  // D3D11 timestamp queries around D3D11 work, the graph's statistics for graph passes
		Graph,  // the graph's statistics only
	};

	/** @brief CS_GPU_TIMERS: "graph", or d3d11 by default. Read once. */
	Timers SelectedTimers();

	/** @brief CS_GPU_IDLE_SOURCE=graph: the idle trace reads the graph's ranges instead of DXVK's submission trace. Read once. */
	bool IdleFromGraph();

	/** @brief The queue a range ran on. Ticks compare only within one queue. */
	enum class Queue : std::uint8_t
	{
		Graphics,
		Compute,
		Copy,
	};

	/** @brief What a range times. */
	enum class RangeKind : std::uint8_t
	{
		Pass,
		Barrier,  // a barrier batch of a kept recording
		Sub,      // a range a pass timed inside itself, nested in its pass's range
	};

	/** @brief One timestamp pair the graph read back. */
	struct Range
	{
		std::string name;
		Queue queue = Queue::Graphics;
		RangeKind kind = RangeKind::Pass;
		std::uint64_t beginTick = 0;
		std::uint64_t endTick = 0;
		double ms = 0.0;               // end - begin at the queue's calibration
		std::int64_t submitQpc = 0;    // QueryPerformanceCounter when the render thread submitted the range's epoch
	};

	/** @brief The graph's ranges of one Present: every epoch the render thread submitted between it and the one before. */
	struct Frame
	{
		std::uint64_t present = 0;      // the Presents before it
		std::uint32_t window = 0;       // StartWindow's window it was submitted in
		std::uint32_t epochs = 0;       // epochs submitted in it
		double graphicsMsPerTick = 0.0; // the graphics queue's calibration (IStatisticsService::GetGpuTicksToMilliseconds)
		std::vector<Range> ranges;
	};

	/** @brief Whether a consumer reads the graph's frames this session (the event timers or the idle trace on graph). */
	bool Collecting();

	/** @brief Render thread, after the runtime submitted the epoch of a host frame (RenderGraphRuntime's MarkEpoch). */
	void OnEpochSubmitted(std::uint64_t a_hostFrame);

	/** @brief The host's completed-frame callback (host thread with async epochs): the frame's timestamps, read back. */
	void OnCompletedFrame(std::uint64_t a_hostFrame, const org::runtime::IStatisticsService& a_stats);

	/**
	 * @brief Render thread, at Present before the idle trace and the event timers: closes the Present and hands on every
	 * earlier Present whose epochs have all completed, oldest first (CompletedFrames). Logs the event timers' graph view.
	 */
	void OnPresent();

	/** @brief The Presents OnPresent handed on, oldest first. Valid until the next OnPresent. Render thread. */
	const std::vector<Frame>& CompletedFrames();

	/** @brief Render thread: the frames from the next one on are a window labelled a_label (GpuEventTimers::StartWindow). */
	void StartWindow(std::string_view a_label);
}
