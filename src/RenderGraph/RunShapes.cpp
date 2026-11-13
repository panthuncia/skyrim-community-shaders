#include "RenderGraph/RunShapes.h"

#include "RenderGraph/CensusWindow.h"
#include "RenderGraph/Switches.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace RenderGraph::RunShapes
{
	namespace
	{
		/** @brief A 64-bit mix (splitmix64's finalizer): sums of mixed values make an order-free set hash. */
		std::uint64_t Mix(std::uint64_t a_value)
		{
			a_value += 0x9E3779B97F4A7C15ull;
			a_value = (a_value ^ (a_value >> 30)) * 0xBF58476D1CE4E5B9ull;
			a_value = (a_value ^ (a_value >> 27)) * 0x94D049BB133111EBull;
			return a_value ^ (a_value >> 31);
		}

		constexpr std::size_t kMaxWindowShapes = 4096;

		/** @brief One anchor in one window. */
		struct AnchorWindow
		{
			const char* name = "?";
			std::uint64_t framesRan = 0;
			std::map<std::uint32_t, std::uint64_t> runsPerFrame;  // runs a frame -> frames
			std::uint32_t peak = 0;
			std::uint64_t framesVaried = 0;            // frames whose runs had more than one shape
			std::size_t mostShapesInFrame = 0;         // the most distinct shapes in one frame
			std::unordered_set<std::uint64_t> shapes;  // distinct shapes in the window (the first kMaxWindowShapes)
		};

		struct Window
		{
			std::map<std::uint32_t, AnchorWindow> anchors;  // by anchor index
		};

		/** @brief One anchor's runs in the frame so far. */
		struct AnchorFrame
		{
			const char* name = "?";
			std::uint32_t runs = 0;
			std::vector<std::uint64_t> shapes;  // distinct
		};

		struct State
		{
			std::mutex lock;
			std::uint64_t open = 0;  // the open run's gate entries, summed
			std::map<std::uint32_t, AnchorFrame> frame;
			CensusWindow::PerWindow<Window> windows;
		};

		State& Get()
		{
			static State state;
			return state;
		}

		std::uint32_t Median(const std::map<std::uint32_t, std::uint64_t>& a_histogram, std::uint64_t a_count)
		{
			std::uint64_t seen = 0;
			for (const auto& [runs, frames] : a_histogram) {
				seen += frames;
				if (seen * 2 >= a_count)
					return runs;
			}
			return 0;
		}

		void Report(CensusWindow::Kind a_window, const Window& a_data, std::uint64_t a_frames)
		{
			const char* window = CensusWindow::Name(a_window);
			logger::info(
				"[ORG] run shapes ({}, {} frames): {} anchors ran; a shape is the gate entries a run's commits opened, its engine targets and its active "
				"clients",
				window, a_frames, a_data.anchors.size());
			for (const auto& [index, anchor] : a_data.anchors) {
				const bool varies = anchor.framesVaried > 0;
				logger::info(
					"[ORG] run shapes ({}): {}: ran in {} frames, runs a frame peak {} median {}; shapes within a frame {}: {} frames varied, up to {} "
					"distinct in one frame, {}{} distinct in the window",
					window, anchor.name, anchor.framesRan, anchor.peak, Median(anchor.runsPerFrame, anchor.framesRan), varies ? "varies" : "same",
					anchor.framesVaried, anchor.mostShapesInFrame, anchor.shapes.size() >= kMaxWindowShapes ? "at least " : "", anchor.shapes.size());
			}
		}
	}

	bool On()
	{
		static const bool on = Switches::On("CS_ORG_EPOCH_STATS");
		return on;
	}

	void BeginRun()
	{
		if (!On())
			return;
		auto& state = Get();
		std::lock_guard guard(state.lock);
		state.open = 0;
	}

	void NoteOpen(const void* a_gate, std::uint32_t a_entry)
	{
		if (!On())
			return;
		auto& state = Get();
		std::lock_guard guard(state.lock);
		state.open += Mix(reinterpret_cast<std::uintptr_t>(a_gate) ^ (std::uint64_t{ a_entry } << 48));
	}

	void EndRun(std::uint32_t a_anchor, const char* a_name, std::int32_t a_input, std::int32_t a_output, std::uint64_t a_clients)
	{
		if (!On())
			return;
		auto& state = Get();
		std::lock_guard guard(state.lock);
		const std::uint64_t targets = Mix((std::uint64_t{ static_cast<std::uint32_t>(a_input) } << 32) | static_cast<std::uint32_t>(a_output));
		const std::uint64_t shape = Mix(state.open + targets * 3 + Mix(a_clients) * 5);
		state.open = 0;
		auto& frame = state.frame[a_anchor];
		frame.name = a_name;
		++frame.runs;
		if (std::ranges::find(frame.shapes, shape) == frame.shapes.end())
			frame.shapes.push_back(shape);
	}

	void EndFrame()
	{
		if (!On())
			return;
		auto& state = Get();
		std::lock_guard guard(state.lock);
		auto& window = state.windows.Active();
		for (const auto& [index, frame] : state.frame) {
			if (!frame.runs)
				continue;
			auto& anchor = window.anchors[index];
			anchor.name = frame.name;
			++anchor.framesRan;
			++anchor.runsPerFrame[frame.runs];
			anchor.peak = (std::max)(anchor.peak, frame.runs);
			anchor.framesVaried += frame.shapes.size() > 1 ? 1 : 0;
			anchor.mostShapesInFrame = (std::max)(anchor.mostShapesInFrame, frame.shapes.size());
			for (const auto shape : frame.shapes)
				if (anchor.shapes.size() < kMaxWindowShapes)
					anchor.shapes.insert(shape);
		}
		state.frame.clear();
		state.windows.EndFrame(CensusWindow::ReportFrames(), [](CensusWindow::Kind a_window, const Window& a_data, std::uint64_t a_frames) { Report(a_window, a_data, a_frames); });
	}
}
