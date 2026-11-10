#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "RenderGraph/GpuTimingSource.h"

#include "GpuEventTimers.h"
#include "GpuIdleTrace.h"
#include "RenderGraph/Switches.h"

#if defined(CS_HAS_RENDER_GRAPH)
#	include <Render/Runtime/IStatisticsService.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <mutex>
#include <unordered_map>

namespace GpuTimingSource
{
	namespace
	{
		// Host frames whose submission is remembered until their timestamps come back. The host completes a frame a few game
		// frames after it ran, and a game frame runs at most one host frame per epoch.
		constexpr std::size_t kEpochRing = 512;
		// A Present whose epochs have not all completed this many Presents later is given up (an epoch of a failed graph).
		constexpr std::uint64_t kPatiencePresents = 32;
		// Ranges below this average inclusive time a frame are left out of the event log.
		constexpr double kMinLoggedMs = 0.003;

		struct Submitted
		{
			std::uint64_t hostFrame = ~0ull;
			std::uint64_t present = 0;
			std::int64_t qpc = 0;
		};

		struct Pending
		{
			Frame frame;
			std::uint32_t completed = 0;
		};

		struct Totals
		{
			Queue queue = Queue::Graphics;
			RangeKind kind = RangeKind::Pass;
			double inclusive = 0.0;
			double self = 0.0;
			std::uint64_t count = 0;
		};

		// The event timers' graph view (CS_GPU_EVENT_TIMERS with CS_GPU_TIMERS=graph). Render thread only.
		struct Events
		{
			std::uint32_t interval = 0;
			std::vector<std::string> names;  // in the order first seen
			std::unordered_map<std::string, std::size_t> index;
			std::vector<Totals> totals;
			std::array<double, 3> busyMs{};
			double graphicsSpanMs = 0.0;
			std::uint32_t frames = 0;
			std::uint64_t dropped = 0;
			// Windows (StartWindow), as GpuEventTimers keeps them: window 0 is the interval's.
			std::vector<std::string> labels{ std::string{} };
			std::uint32_t collecting = 0;
		};

		struct State
		{
			std::mutex mutex;  // epochs, pending, present, window: the host thread reads them at completion
			std::array<Submitted, kEpochRing> epochs{};
			std::map<std::uint64_t, Pending> pending;
			std::uint64_t present = 0;
			std::uint32_t window = 0;
			std::uint64_t dropped = 0;
			std::vector<Frame> completed;  // render thread
			Events events;                 // render thread
		};

		State& Get()
		{
			static State state;
			return state;
		}

		bool EventsOnGraph()
		{
			static const bool on = SelectedTimers() == Timers::Graph && GpuEventTimers::Requested();
			return on;
		}

		const char* QueueName(Queue a_queue)
		{
			switch (a_queue) {
			case Queue::Compute:
				return "compute";
			case Queue::Copy:
				return "copy";
			default:
				return "graphics";
			}
		}

		/** @brief The ms per tick of one queue's ranges, from a range with a length, or a_fallback. */
		double MsPerTick(const std::vector<const Range*>& a_ranges, double a_fallback)
		{
			for (const auto* range : a_ranges)
				if (range->endTick > range->beginTick && range->ms > 0.0)
					return range->ms / static_cast<double>(range->endTick - range->beginTick);
			return a_fallback;
		}

		void ResetEvents(Events& a_events)
		{
			for (auto& total : a_events.totals) {
				total.inclusive = total.self = 0.0;
				total.count = 0;
			}
			a_events.busyMs = {};
			a_events.graphicsSpanMs = 0.0;
			a_events.frames = 0;
			a_events.dropped = 0;
		}

		void LogEvents(Events& a_events)
		{
			const double frames = std::max(1u, a_events.frames);
			const auto& label = a_events.labels[a_events.collecting];
			std::string text = fmt::format(
				"[GpuEvents] graph {}{} frames ({} given up incomplete): graphics queue {:.2f} ms from the first graph range to the last, {:.2f} ms in graph ranges; "
				"compute queue {:.2f} ms, copy queue {:.2f} ms in graph ranges; per frame, inclusive / self ms, calls:",
				label.empty() ? std::string{} : fmt::format("window '{}': ", label), a_events.frames, a_events.dropped, a_events.graphicsSpanMs / frames,
				a_events.busyMs[0] / frames, a_events.busyMs[1] / frames, a_events.busyMs[2] / frames);
			for (std::size_t i = 0; i < a_events.names.size(); ++i) {
				const auto& total = a_events.totals[i];
				if (total.inclusive / frames < kMinLoggedMs)
					continue;
				text += fmt::format("\n    {:>6.2f} {:>6.2f} {:>5.1f}  {}[{}{}] {}", total.inclusive / frames, total.self / frames, static_cast<double>(total.count) / frames,
					total.kind == RangeKind::Sub ? "  " : "", QueueName(total.queue), total.kind == RangeKind::Barrier ? " barriers" : "", a_events.names[i]);
			}
			logger::info("{}", text);
			ResetEvents(a_events);
		}

		/** @brief Adds one Present's ranges: per range its time and its self time along its queue, and each queue's busy time. */
		void AddEventFrame(Events& a_events, const Frame& a_frame)
		{
			auto totalOf = [&](const Range& a_range) -> Totals& {
				const auto [at, inserted] = a_events.index.try_emplace(a_range.name, a_events.names.size());
				if (inserted) {
					a_events.names.push_back(a_range.name);
					a_events.totals.push_back({ a_range.queue, a_range.kind });
				}
				return a_events.totals[at->second];
			};
			for (std::uint32_t q = 0; q < 3; ++q) {
				const auto queue = static_cast<Queue>(q);
				std::vector<const Range*> ranges;
				for (const auto& range : a_frame.ranges)
					if (range.queue == queue && range.endTick >= range.beginTick && range.beginTick)
						ranges.push_back(&range);
				if (ranges.empty())
					continue;
				const double msPerTick = MsPerTick(ranges, a_frame.graphicsMsPerTick);
				// Sub-ranges sit inside their pass's range: they have their own row, and stay out of the queue's walk.
				std::vector<const Range*> top;
				for (const auto* range : ranges) {
					if (range->kind == RangeKind::Sub) {
						auto& total = totalOf(*range);
						total.inclusive += range->ms;
						total.self += range->ms;
						++total.count;
					} else {
						top.push_back(range);
					}
				}
				if (top.empty())
					continue;
				std::ranges::sort(top, [](const Range* a, const Range* b) { return a->beginTick < b->beginTick; });
				std::uint64_t previousEnd = top.front()->beginTick;
				double busy = 0.0;
				for (const auto* range : top) {
					auto& total = totalOf(*range);
					const std::uint64_t from = std::max(previousEnd, range->beginTick);
					const double self = range->endTick > from ? static_cast<double>(range->endTick - from) * msPerTick : 0.0;
					total.inclusive += static_cast<double>(range->endTick - range->beginTick) * msPerTick;
					total.self += self;
					++total.count;
					busy += self;
					previousEnd = std::max(previousEnd, range->endTick);
				}
				a_events.busyMs[q] += busy;
				if (queue == Queue::Graphics)
					a_events.graphicsSpanMs += static_cast<double>(previousEnd - top.front()->beginTick) * msPerTick;
			}
			++a_events.frames;
		}

		void OnEventFrames(State& a_state, std::uint64_t a_dropped)
		{
			auto& events = a_state.events;
			if (!events.interval) {
				events.interval = static_cast<std::uint32_t>(std::max<std::uint64_t>(1u, Switches::Number("CS_GPU_EVENT_TIMERS")));
				logger::info("[GpuEvents] GPU event timers on the render graph (CS_GPU_TIMERS=graph): every graph range's GPU time, logged every {} frames. "
							 "D3D11 work has no timer here.",
					events.interval);
			}
			events.dropped += a_dropped;
			for (const auto& frame : a_state.completed) {
				// The first frame of a later window: the window before is complete (frames come in order), so it is logged.
				if (frame.window != events.collecting) {
					if (events.labels[events.collecting] != "-")
						LogEvents(events);
					else
						ResetEvents(events);
					events.collecting = frame.window;
				}
				AddEventFrame(events, frame);
				if (a_state.window == 0 && events.frames >= events.interval)
					LogEvents(events);
			}
		}
	}

	Timers SelectedTimers()
	{
		static const Timers timers = Switches::EqualsNoCase("CS_GPU_TIMERS", "graph") ? Timers::Graph : Timers::D3D11;
		return timers;
	}

	bool IdleFromGraph()
	{
		static const bool graph = Switches::EqualsNoCase("CS_GPU_IDLE_SOURCE", "graph");
		return graph;
	}

	bool Collecting()
	{
		static const bool collecting = EventsOnGraph() || (IdleFromGraph() && GpuIdleTrace::Requested());
		return collecting;
	}

	void OnEpochSubmitted(std::uint64_t a_hostFrame)
	{
		if (!Collecting())
			return;
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		auto& state = Get();
		std::lock_guard lock(state.mutex);
		state.epochs[a_hostFrame % kEpochRing] = { a_hostFrame, state.present, now.QuadPart };
		auto& pending = state.pending[state.present];
		pending.frame.present = state.present;
		pending.frame.window = state.window;
		++pending.frame.epochs;
	}

	void OnCompletedFrame([[maybe_unused]] std::uint64_t a_hostFrame, [[maybe_unused]] const org::runtime::IStatisticsService& a_stats)
	{
#if defined(CS_HAS_RENDER_GRAPH)
		if (!Collecting())
			return;
		std::vector<Range> ranges;
		const auto& timestamps = a_stats.GetCompletedTimestamps();
		ranges.reserve(timestamps.size());
		for (const auto& timestamp : timestamps) {
			Range range;
			range.name = std::string(timestamp.name);
			range.queue = timestamp.queueKind == rhi::QueueKind::Graphics ? Queue::Graphics :
			              timestamp.queueKind == rhi::QueueKind::Compute  ? Queue::Compute :
			                                                                Queue::Copy;
			range.kind = timestamp.kind == org::runtime::TimestampRangeKind::Barrier ? RangeKind::Barrier :
			             timestamp.kind == org::runtime::TimestampRangeKind::Sub     ? RangeKind::Sub :
			                                                                           RangeKind::Pass;
			range.beginTick = timestamp.beginTick;
			range.endTick = timestamp.endTick;
			range.ms = timestamp.milliseconds;
			ranges.push_back(std::move(range));
		}
		auto& state = Get();
		std::lock_guard lock(state.mutex);
		auto& slot = state.epochs[a_hostFrame % kEpochRing];
		// A host frame no epoch submission named (not one of the runtime's epochs), or one whose Present was given up.
		if (slot.hostFrame != a_hostFrame)
			return;
		slot.hostFrame = ~0ull;
		const auto pending = state.pending.find(slot.present);
		if (pending == state.pending.end())
			return;
		auto& frame = pending->second.frame;
		frame.graphicsMsPerTick = a_stats.GetGpuTicksToMilliseconds();
		for (auto& range : ranges) {
			range.submitQpc = slot.qpc;
			frame.ranges.push_back(std::move(range));
		}
		++pending->second.completed;
#endif
	}

	void OnPresent()
	{
		if (!Collecting())
			return;
		auto& state = Get();
		state.completed.clear();
		std::uint64_t dropped = 0;
		{
			std::lock_guard lock(state.mutex);
			++state.present;
			for (auto it = state.pending.begin(); it != state.pending.end() && it->first < state.present;) {
				if (it->second.completed >= it->second.frame.epochs) {
					state.completed.push_back(std::move(it->second.frame));
				} else if (it->first + kPatiencePresents < state.present) {
					++dropped;
				} else {
					break;
				}
				it = state.pending.erase(it);
			}
			state.dropped += dropped;
		}
		if (EventsOnGraph())
			OnEventFrames(state, dropped);
	}

	const std::vector<Frame>& CompletedFrames()
	{
		return Get().completed;
	}

	void StartWindow(std::string_view a_label)
	{
		if (!EventsOnGraph())
			return;
		auto& state = Get();
		state.events.labels.emplace_back(a_label);
		std::lock_guard lock(state.mutex);
		state.window = static_cast<std::uint32_t>(state.events.labels.size() - 1);
	}
}
