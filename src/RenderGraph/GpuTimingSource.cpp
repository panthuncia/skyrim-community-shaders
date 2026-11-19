#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "RenderGraph/GpuTimingSource.h"

#include "GpuEventTimers.h"
#include "GpuIdleTrace.h"
#include "RenderGraph/Engine/Native/Native.h"
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
		// The host completes a frame a few game frames after it ran, and a game frame runs at most one host frame per epoch.
		constexpr std::size_t kEpochRing = 512;
		// A Present whose epochs have not all completed this many Presents later is given up, such as an epoch of a failed graph.
		constexpr std::uint64_t kPatiencePresents = 32;
		// Ranges below this average time a frame are left out of the event log.
		constexpr double kMinLoggedMs = 0.003;

		struct Submitted
		{
			std::uint64_t hostFrame = ~0ull;
			std::uint64_t present = 0;
			std::int64_t qpc = 0;
			const char* stage = nullptr;
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

		// The event timers' graph view. Render thread only.
		struct Events
		{
			std::uint32_t interval = 0;
			std::vector<std::string> names;  // in the order first seen
			std::unordered_map<std::string, std::size_t> index;
			std::vector<Totals> totals;
			// Per stage and queue, the self time along the queue and the summed range time.
			std::vector<std::string> stageNames;
			std::unordered_map<std::string, std::size_t> stageIndex;
			std::vector<Totals> stageTotals;
			std::array<double, 3> busyMs{};
			double graphicsSpanMs = 0.0;
			std::uint32_t frames = 0;
			std::uint64_t dropped = 0;
			// Window 0 is the interval's, as in GpuEventTimers.
			std::vector<std::string> labels{ std::string{} };
			std::uint32_t collecting = 0;
		};

		struct State
		{
			std::mutex mutex;  // the host thread reads these at completion
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

		/** @brief From a range with a length, else a_fallback. */
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
			for (auto& total : a_events.stageTotals) {
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
			double stageSelf = 0.0;
			for (std::size_t i = 0; i < a_events.stageNames.size(); ++i) {
				const auto& total = a_events.stageTotals[i];
				if (!total.count)
					continue;
				stageSelf += total.queue == Queue::Graphics ? total.self : 0.0;
				text += fmt::format("\n    stage {:>6.3f} {:>6.3f} {:>5.1f}  [{}] {}", total.self / frames, total.inclusive / frames,
					static_cast<double>(total.count) / frames, QueueName(total.queue), a_events.stageNames[i]);
			}
			text += fmt::format("\n    stages on the graphics queue, self: {:.3f} ms a frame", stageSelf / frames);
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
				// Sub-ranges sit inside their pass's range, so they get their own row and stay out of the queue's walk.
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
					{
						const std::string stage = fmt::format("{}{}", range->stage ? range->stage : "(no stage)", queue == Queue::Graphics ? "" : " (async)");
						const auto [at, inserted] = a_events.stageIndex.try_emplace(stage, a_events.stageNames.size());
						if (inserted) {
							a_events.stageNames.push_back(stage);
							a_events.stageTotals.push_back({ queue, RangeKind::Pass });
						}
						auto& stageTotal = a_events.stageTotals[at->second];
						stageTotal.inclusive += static_cast<double>(range->endTick - range->beginTick) * msPerTick;
						stageTotal.self += self;
						++stageTotal.count;
					}
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
				// Frames come in order, so a later window's first frame means the one before is complete.
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
		// Native mode's D3D11 queries answer 0, so the graph is the only source there.
		static const Timers timers = [] {
			if (!RenderGraph::Engine::Native::Requested())
				return Switches::EqualsNoCase("CS_GPU_TIMERS", "graph") ? Timers::Graph : Timers::D3D11;
			if (Switches::IsSet("CS_GPU_TIMERS") && !Switches::EqualsNoCase("CS_GPU_TIMERS", "graph"))
				logger::info("[GpuEvents] native mode: GPU timers come from the render graph (CS_GPU_TIMERS={} has no D3D11 queries to read here)",
					Switches::Value("CS_GPU_TIMERS"));
			return Timers::Graph;
		}();
		return timers;
	}

	bool IdleFromGraph()
	{
		// Native mode has no DXVK submission trace.
		static const bool graph = RenderGraph::Engine::Native::Requested() || Switches::EqualsNoCase("CS_GPU_IDLE_SOURCE", "graph");
		return graph;
	}

	bool Collecting()
	{
		static const bool collecting = EventsOnGraph() || (IdleFromGraph() && GpuIdleTrace::Requested());
		return collecting;
	}

	void OnEpochSubmitted(std::uint64_t a_hostFrame, const char* a_stage)
	{
		if (!Collecting())
			return;
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		auto& state = Get();
		std::lock_guard lock(state.mutex);
		state.epochs[a_hostFrame % kEpochRing] = { a_hostFrame, state.present, now.QuadPart, a_stage };
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
		// Not one of the runtime's epochs, or its Present was given up.
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
			range.stage = slot.stage;
			frame.ranges.push_back(std::move(range));
		}
		++pending->second.completed;
#endif
	}

	namespace
	{
		// CS_RHI_CENSUS=<frames> logs BasicRHI's command census by list label every that many Presents. Render thread.
		void CensusOnPresent()
		{
#if defined(CS_HAS_RENDER_GRAPH)
			static const std::uint32_t interval = [] {
				const auto frames = static_cast<std::uint32_t>(Switches::Number("CS_RHI_CENSUS"));
				if (frames)
					rhi::census::Enable(true);
				return frames;
			}();
			if (!interval)
				return;
			static std::uint32_t presents = 0;
			if (++presents % interval)
				return;
			const auto rows = rhi::census::Take();
			std::vector<std::pair<std::string, rhi::census::Counts>> sorted(rows.begin(), rows.end());
			std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.first < b.first; });
			rhi::census::Counts total;
			const double frames = interval;
			std::string text = fmt::format("[RhiCensus] {} frames, per frame by list label: lists, pass begins, same attachments (no barrier between), "
										   "colour / depth attachments, load L/C/D, store S/D, GENERAL attachments, barrier commands, image barriers "
										   "(layout changes, same state, onto the previous pass's attachments), buffer, memory, dispatches, copies, barrier commands "
										   "before the list's first work / right after another barrier command",
				interval);
			const auto line = [&](const rhi::census::Counts& c, std::string_view name) {
				text += fmt::format("\n    {:5.1f} {:6.1f} {:5.1f} ({:5.1f}) {:6.1f}/{:5.1f} {:6.1f}/{:5.1f}/{:5.1f} {:6.1f}/{:5.1f} {:6.1f} {:6.1f} {:6.1f} ({:6.1f} {:5.1f} {:5.1f}) {:5.1f} {:5.1f} {:6.1f} {:5.1f} {:5.1f}/{:5.1f}  {}",
					c.lists / frames, c.passBegins / frames, c.sameAttachmentPasses / frames, c.sameAttachmentNoBarrier / frames, c.colorAttachments / frames,
					c.depthAttachments / frames, c.loadOpLoad / frames, c.loadOpClear / frames, c.loadOpDontCare / frames, c.storeOpStore / frames,
					c.storeOpDontCare / frames, c.generalAttachments / frames, c.barrierCalls / frames, c.imageBarriers / frames, c.imageLayoutChanges / frames,
					c.imageSameState / frames, c.imageOntoPreviousPass / frames, c.bufferBarriers / frames, c.memoryBarriers / frames, c.dispatches / frames, c.copies / frames,
					c.leadingBarrierCalls / frames, c.adjacentBarrierCalls / frames, name);
			};
			for (const auto& [label, counts] : sorted) {
				total.Add(counts);
				line(counts, label.empty() ? std::string_view("(unlabelled)") : std::string_view(label));
			}
			line(total, "TOTAL");
			logger::info("{}", text);
#endif
		}
	}

	void OnPresent()
	{
		CensusOnPresent();
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
