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
 * @brief GPU timings from the render graph's own timestamp queries, with no D3D11 query and no DXVK export.
 *
 * CS_GPU_TIMERS=graph makes the profiler and event timers read GPU time from the graph only. D3D11 ranges then have no GPU time.
 * Native mode always reads the graph. Kept recordings are timed per list by default, or per pass with
 * CS_ORG_RECORDING_TIMESTAMPS=1. A Present's ranges are handed on once every epoch submitted in it has completed.
 * This header stays free of ORG and Vulkan includes.
 */
namespace GpuTimingSource
{
	/** @brief Where the profiler's ranges and the event timers take GPU time from. */
	enum class Timers : std::uint8_t
	{
		D3D11,  // D3D11 queries for D3D11 work, the graph's statistics for graph passes
		Graph,
	};

	/** @brief CS_GPU_TIMERS, d3d11 by default and always graph in native mode. Read once. */
	Timers SelectedTimers();

	/** @brief The queue a range ran on. Ticks compare only within one queue. */
	enum class Queue : std::uint8_t
	{
		Graphics,
		Compute,
		Copy,
	};

	enum class RangeKind : std::uint8_t
	{
		Pass,
		Barrier,  // a barrier batch of a kept recording
		Sub,      // nested in its pass's range
	};

	struct Range
	{
		std::string name;
		Queue queue = Queue::Graphics;
		RangeKind kind = RangeKind::Pass;
		std::uint64_t beginTick = 0;
		std::uint64_t endTick = 0;
		double ms = 0.0;
		std::int64_t submitQpc = 0;    // QPC when the render thread submitted the range's epoch
		const char* stage = nullptr;   // the epoch's label, for the per-stage rows
	};

	/** @brief The graph's ranges of one Present, from every epoch submitted since the one before. */
	struct Frame
	{
		std::uint64_t present = 0;      // the Presents before it
		std::uint32_t window = 0;
		std::uint32_t epochs = 0;
		double graphicsMsPerTick = 0.0;
		std::vector<Range> ranges;
	};

	/** @brief Whether a consumer reads the graph's frames this session. */
	bool Collecting();

	/** @brief Render thread, after the runtime submitted the epoch of a host frame. */
	void OnEpochSubmitted(std::uint64_t a_hostFrame, const char* a_stage);

	/** @brief The host's completed-frame callback, on the host thread with async epochs. */
	void OnCompletedFrame(std::uint64_t a_hostFrame, const org::runtime::IStatisticsService& a_stats);

	/**
	 * @brief Any thread. A range timed outside the graph's own queries, such as the native present copy. It joins the newest Present
	 * still waiting for its epochs, or the next one to submit an epoch, so it adds no frame of its own. Use RangeKind::Sub to keep it
	 * out of the queue walk.
	 */
	void AddExternalRange(Range a_range);

	/** @brief Render thread, at Present. Hands on every earlier Present whose epochs have all completed, oldest first. */
	void OnPresent();

	/** @brief Valid until the next OnPresent. Render thread. */
	const std::vector<Frame>& CompletedFrames();

	/** @brief Render thread. Frames from the next one on form a window labelled a_label. */
	void StartWindow(std::string_view a_label);
}
