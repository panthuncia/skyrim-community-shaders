#include "GpuEventTimers.h"

#include "Globals.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace GpuEventTimers
{
	namespace
	{
		// Frames in flight before a frame's queries are read back; a frame whose slot comes round again unread is dropped.
		constexpr std::uint32_t kFrames = 4;
		// Queries per frame (two an event, two for the frame); events past it are not timed, and counted.
		constexpr std::uint32_t kMaxQueries = 4096;
		constexpr std::uint32_t kNone = ~0u;
		// Paths below this average inclusive time a frame are left out of the log.
		constexpr double kMinLoggedMs = 0.02;

		std::uint32_t ReadInterval()
		{
			char value[32] = {};
			const DWORD length = GetEnvironmentVariableA("CS_GPU_EVENT_TIMERS", value, sizeof(value));
			return length && length < sizeof(value) ? static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
		}

		/** @brief CS_GPU_EVENT_STATS="<path>;<path>...": the event paths that also get a pipeline statistics query. */
		const std::vector<std::string>& StatsPaths()
		{
			static const std::vector<std::string> paths = [] {
				std::vector<std::string> out;
				char value[1024] = {};
				const DWORD length = GetEnvironmentVariableA("CS_GPU_EVENT_STATS", value, sizeof(value));
				if (!length || length >= sizeof(value))
					return out;
				std::string text(value, length);
				std::size_t start = 0;
				while (start <= text.size()) {
					const auto end = text.find(';', start);
					auto part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
					if (!part.empty())
						out.push_back(std::move(part));
					if (end == std::string::npos)
						break;
					start = end + 1;
				}
				return out;
			}();
			return paths;
		}

		struct Event
		{
			std::uint32_t path;
			std::uint32_t parent;  // the enclosing event's index in the frame, or kNone
			std::uint32_t begin, end = kNone;  // query indices
			std::uint32_t stats = kNone;       // its pipeline statistics query, when its path is one of StatsPaths
		};

		struct Frame
		{
			winrt::com_ptr<ID3D11Query> disjoint;
			std::vector<winrt::com_ptr<ID3D11Query>> queries;
			std::uint32_t used = 0;
			std::uint32_t frameBegin = kNone, frameEnd = kNone;
			std::vector<Event> events;
			std::vector<winrt::com_ptr<ID3D11Query>> statsQueries;
			std::uint32_t statsUsed = 0;
			bool open = false, pending = false, overflow = false;
		};

		struct Accumulated
		{
			double inclusive = 0.0, self = 0.0;
			std::uint64_t count = 0;
			D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
			std::uint64_t statsCount = 0;
		};

		struct State
		{
			std::uint32_t interval = 0;
			bool initialized = false, failed = false;
			DWORD thread = 0;
			Frame frames[kFrames];
			std::uint32_t write = 0;
			// The open events, innermost last: an index into the recording frame's events, or kNone for one begun while
			// nothing was recorded (before the first frame, on the far side of a Present), whose end is then ignored.
			std::vector<std::uint32_t> stack;
			std::vector<std::string> paths;  // in the order they were first seen: the frame's order, near enough
			std::vector<std::uint32_t> depth;
			std::unordered_map<std::string, std::uint32_t> pathIndex;
			std::vector<Accumulated> accumulated;
			double frameMs = 0.0;
			std::uint32_t collected = 0, dropped = 0, disjoint = 0, overflowed = 0;
			// Each collected frame of the interval: its GPU frame time and every path's inclusive time in it, for the
			// frame-time spread and for what the slow frames spend their extra time on.
			struct Sample
			{
				double frameMs;
				std::vector<float> paths;
			};
			std::vector<Sample> samples;
		};

		State& Get()
		{
			static State state;
			return state;
		}

		bool Initialize(State& a_state)
		{
			auto* device = globals::d3d::device;
			if (!device || !globals::d3d::context)
				return false;
			D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			for (auto& frame : a_state.frames) {
				if (FAILED(device->CreateQuery(&desc, frame.disjoint.put())))
					return false;
			}
			a_state.initialized = true;
			logger::info("[GpuEvents] GPU event timers on: every perf event's GPU time, logged every {} frames", a_state.interval);
			return true;
		}

		/** @brief A timestamp in the recording frame, or kNone when the frame has used all its queries. */
		std::uint32_t Timestamp(Frame& a_frame)
		{
			if (a_frame.used >= kMaxQueries) {
				a_frame.overflow = true;
				return kNone;
			}
			if (a_frame.used == a_frame.queries.size()) {
				D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP, 0 };
				winrt::com_ptr<ID3D11Query> query;
				if (FAILED(globals::d3d::device->CreateQuery(&desc, query.put()))) {
					a_frame.overflow = true;
					return kNone;
				}
				a_frame.queries.push_back(std::move(query));
			}
			const std::uint32_t index = a_frame.used++;
			globals::d3d::context->End(a_frame.queries[index].get());
			return index;
		}

		std::uint32_t PathOf(State& a_state, std::uint32_t a_parentPath, std::string_view a_name)
		{
			std::string path = a_parentPath == kNone ? std::string(a_name) : a_state.paths[a_parentPath] + " / " + std::string(a_name);
			const auto [at, inserted] = a_state.pathIndex.try_emplace(path, static_cast<std::uint32_t>(a_state.paths.size()));
			if (inserted) {
				a_state.paths.push_back(std::move(path));
				a_state.depth.push_back(a_parentPath == kNone ? 0u : a_state.depth[a_parentPath] + 1);
				a_state.accumulated.emplace_back();
			}
			return at->second;
		}

		/** @brief Reads a finished frame's queries into the totals; false while the GPU has not reached its end. */
		bool Collect(State& a_state, Frame& a_frame, bool a_flush)
		{
			auto* context = globals::d3d::context;
			const UINT flags = a_flush ? 0u : UINT(D3D11_ASYNC_GETDATA_DONOTFLUSH);
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			if (context->GetData(a_frame.disjoint.get(), &disjoint, sizeof(disjoint), flags) != S_OK)
				return false;
			std::vector<std::uint64_t> ticks(a_frame.used);
			for (std::uint32_t i = 0; i < a_frame.used; ++i)
				if (context->GetData(a_frame.queries[i].get(), &ticks[i], sizeof(std::uint64_t), flags) != S_OK)
					return false;
			std::vector<D3D11_QUERY_DATA_PIPELINE_STATISTICS> stats(a_frame.statsUsed);
			for (std::uint32_t i = 0; i < a_frame.statsUsed; ++i)
				if (context->GetData(a_frame.statsQueries[i].get(), &stats[i], sizeof(stats[i]), flags) != S_OK)
					return false;
			a_frame.pending = false;
			if (disjoint.Disjoint || !disjoint.Frequency || a_frame.frameBegin == kNone || a_frame.frameEnd == kNone) {
				++a_state.disjoint;
				return true;
			}
			const double toMs = 1000.0 / static_cast<double>(disjoint.Frequency);
			auto span = [&](std::uint32_t a_begin, std::uint32_t a_end) {
				return ticks[a_end] >= ticks[a_begin] ? static_cast<double>(ticks[a_end] - ticks[a_begin]) * toMs : 0.0;
			};
			std::vector<double> inclusive(a_frame.events.size(), -1.0), children(a_frame.events.size(), 0.0);
			for (std::size_t e = 0; e < a_frame.events.size(); ++e) {
				const auto& event = a_frame.events[e];
				if (event.begin == kNone || event.end == kNone)
					continue;
				inclusive[e] = span(event.begin, event.end);
				if (event.parent != kNone)
					children[event.parent] += inclusive[e];
			}
			auto& sample = a_state.samples.emplace_back();
			sample.frameMs = span(a_frame.frameBegin, a_frame.frameEnd);
			sample.paths.assign(a_state.paths.size(), 0.0f);
			for (std::size_t e = 0; e < a_frame.events.size(); ++e) {
				if (inclusive[e] < 0.0)
					continue;
				sample.paths[a_frame.events[e].path] += static_cast<float>(inclusive[e]);
				auto& total = a_state.accumulated[a_frame.events[e].path];
				total.inclusive += inclusive[e];
				total.self += std::max(0.0, inclusive[e] - children[e]);
				++total.count;
				if (const auto q = a_frame.events[e].stats; q != kNone && q < stats.size()) {
					total.stats.IAVertices += stats[q].IAVertices;
					total.stats.IAPrimitives += stats[q].IAPrimitives;
					total.stats.VSInvocations += stats[q].VSInvocations;
					total.stats.CInvocations += stats[q].CInvocations;
					total.stats.CPrimitives += stats[q].CPrimitives;
					total.stats.PSInvocations += stats[q].PSInvocations;
					++total.statsCount;
				}
			}
			a_state.frameMs += span(a_frame.frameBegin, a_frame.frameEnd);
			a_state.overflowed += a_frame.overflow ? 1u : 0u;
			++a_state.collected;
			return true;
		}

		/**
		 * @brief The interval's GPU frame times by percentile, and the paths the slowest tenth of its frames spend their extra
		 * time on: each path's mean inclusive time in the frames at or above the 90th percentile, against the frames at or
		 * below the median. Top-level paths and leaves both appear, so a nested path's excess is also in its parents'.
		 */
		void LogSpread(State& a_state)
		{
			auto& samples = a_state.samples;
			if (samples.size() < 10) {
				samples.clear();
				return;
			}
			std::vector<double> sorted;
			for (const auto& sample : samples)
				sorted.push_back(sample.frameMs);
			std::sort(sorted.begin(), sorted.end());
			auto at = [&](double a_fraction) { return sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(a_fraction * sorted.size()))]; };
			const double median = at(0.5), p90 = at(0.9);
			std::vector<double> slow(a_state.paths.size(), 0.0), typical(a_state.paths.size(), 0.0);
			std::uint32_t slowCount = 0, typicalCount = 0;
			for (const auto& sample : samples) {
				const bool isSlow = sample.frameMs >= p90, isTypical = sample.frameMs <= median;
				if (!isSlow && !isTypical)
					continue;
				auto& into = isSlow ? slow : typical;
				(isSlow ? slowCount : typicalCount)++;
				for (std::size_t p = 0; p < sample.paths.size(); ++p)
					into[p] += sample.paths[p];
			}
			std::vector<std::pair<double, std::size_t>> excess;
			for (std::size_t p = 0; p < a_state.paths.size(); ++p) {
				const double delta = slow[p] / std::max(1u, slowCount) - typical[p] / std::max(1u, typicalCount);
				if (delta >= 0.05)
					excess.emplace_back(delta, p);
			}
			std::sort(excess.begin(), excess.end(), std::greater<>());
			std::string text = fmt::format("[GpuEvents] GPU frame spread over {} frames: min {:.2f}, median {:.2f}, p90 {:.2f}, p99 {:.2f}, max {:.2f} ms; the slowest tenth's excess over the median half, ms:",
				samples.size(), sorted.front(), median, p90, at(0.99), sorted.back());
			for (std::size_t i = 0; i < std::min<std::size_t>(excess.size(), 8); ++i)
				text += fmt::format("\n    {:>6.2f}  {}", excess[i].first, a_state.paths[excess[i].second]);
			logger::info("{}", text);
			samples.clear();
		}

		void Log(State& a_state)
		{
			const double frames = a_state.collected;
			double annotated = 0.0;
			for (std::size_t p = 0; p < a_state.paths.size(); ++p)
				if (a_state.depth[p] == 0)
					annotated += a_state.accumulated[p].inclusive;
			std::string text = fmt::format("[GpuEvents] {} frames ({} dropped unread, {} disjoint, {} out of queries): GPU frame {:.2f} ms, {:.2f} ms outside any event; per frame, inclusive / self ms, calls:",
				a_state.collected, a_state.dropped, a_state.disjoint, a_state.overflowed, a_state.frameMs / frames, (a_state.frameMs - annotated) / frames);
			for (std::size_t p = 0; p < a_state.paths.size(); ++p) {
				const auto& total = a_state.accumulated[p];
				if (total.inclusive / frames < kMinLoggedMs)
					continue;
				const auto& path = a_state.paths[p];
				const auto leaf = path.rfind(" / ");
				text += fmt::format("\n    {:>6.2f} {:>6.2f} {:>5.1f}  {}{}", total.inclusive / frames, total.self / frames, static_cast<double>(total.count) / frames,
					std::string(std::size_t(a_state.depth[p]) * 2, ' '), leaf == std::string::npos ? path : path.substr(leaf + 3));
				if (total.statsCount) {
					const double n = static_cast<double>(total.statsCount);
					text += fmt::format("  [per call: {:.0f} vertices, {:.0f} primitives, {:.0f} VS, {:.0f} rasterized primitives, {:.0f} PS invocations]",
						static_cast<double>(total.stats.IAVertices) / n, static_cast<double>(total.stats.IAPrimitives) / n, static_cast<double>(total.stats.VSInvocations) / n,
						static_cast<double>(total.stats.CPrimitives) / n, static_cast<double>(total.stats.PSInvocations) / n);
				}
			}
			logger::info("{}", text);
			LogSpread(a_state);
			for (auto& total : a_state.accumulated)
				total = {};
			a_state.frameMs = 0.0;
			a_state.collected = a_state.dropped = a_state.disjoint = a_state.overflowed = 0;
		}
	}

	bool Requested()
	{
		static const bool requested = ReadInterval() != 0;
		return requested;
	}

	void BeginEvent(std::string_view a_name)
	{
		if (!Requested())
			return;
		auto& state = Get();
		auto& frame = state.frames[state.write];
		if (!frame.open || GetCurrentThreadId() != state.thread) {
			if (GetCurrentThreadId() == state.thread || !state.initialized)
				state.stack.push_back(kNone);
			return;
		}
		const std::uint32_t parent = state.stack.empty() ? kNone : state.stack.back();
		const std::uint32_t parentPath = parent == kNone ? kNone : frame.events[parent].path;
		// An event under one begun outside the frame has no place in the tree: it is not timed.
		if (!state.stack.empty() && parent == kNone) {
			state.stack.push_back(kNone);
			return;
		}
		const auto index = static_cast<std::uint32_t>(frame.events.size());
		const std::uint32_t path = PathOf(state, parentPath, a_name);
		frame.events.push_back({ path, parent, Timestamp(frame) });
		const auto& statsPaths = StatsPaths();
		if (std::find(statsPaths.begin(), statsPaths.end(), state.paths[path]) != statsPaths.end()) {
			if (frame.statsUsed == frame.statsQueries.size()) {
				D3D11_QUERY_DESC desc{ D3D11_QUERY_PIPELINE_STATISTICS, 0 };
				winrt::com_ptr<ID3D11Query> query;
				if (SUCCEEDED(globals::d3d::device->CreateQuery(&desc, query.put())))
					frame.statsQueries.push_back(std::move(query));
			}
			if (frame.statsUsed < frame.statsQueries.size()) {
				frame.events.back().stats = frame.statsUsed++;
				globals::d3d::context->Begin(frame.statsQueries[frame.events.back().stats].get());
			}
		}
		state.stack.push_back(index);
	}

	void EndEvent()
	{
		if (!Requested())
			return;
		auto& state = Get();
		if (state.initialized && GetCurrentThreadId() != state.thread)
			return;
		if (state.stack.empty())
			return;
		const std::uint32_t index = state.stack.back();
		state.stack.pop_back();
		auto& frame = state.frames[state.write];
		if (index != kNone && frame.open && index < frame.events.size() && frame.events[index].begin != kNone) {
			if (frame.events[index].stats != kNone)
				globals::d3d::context->End(frame.statsQueries[frame.events[index].stats].get());
			frame.events[index].end = Timestamp(frame);
		}
	}

	void OnPresent()
	{
		if (!Requested())
			return;
		auto& state = Get();
		if (state.failed)
			return;
		if (!state.initialized) {
			state.interval = ReadInterval();
			if (!Initialize(state)) {
				state.failed = true;
				return;
			}
		}
		state.thread = GetCurrentThreadId();
		auto* context = globals::d3d::context;

		// Close the recording frame. An event still open at Present is not timed, and its end is ignored.
		auto& current = state.frames[state.write];
		if (current.open) {
			for (auto& open : state.stack)
				open = kNone;
			current.frameEnd = Timestamp(current);
			context->End(current.disjoint.get());
			current.open = false;
			current.pending = true;
		}

		// Read back every finished frame, oldest first. The oldest is the next to be reused, so its read may flush: without
		// it, a frame whose query ends DXVK has not submitted yet (nothing else flushed the context) is never readable, and
		// with DCLF off every frame was dropped unread.
		for (std::uint32_t i = 1; i <= kFrames; ++i) {
			auto& frame = state.frames[(state.write + i) % kFrames];
			if (frame.pending && !Collect(state, frame, i == 1))
				break;
		}
		if (state.collected >= state.interval)
			Log(state);

		// Open the next frame in the ring; one the GPU has still not finished is given up.
		state.write = (state.write + 1) % kFrames;
		auto& next = state.frames[state.write];
		if (next.pending) {
			next.pending = false;
			++state.dropped;
		}
		next.used = 0;
		next.statsUsed = 0;
		next.events.clear();
		next.overflow = false;
		next.frameEnd = kNone;
		context->Begin(next.disjoint.get());
		next.open = true;
		next.frameBegin = Timestamp(next);
	}
}
