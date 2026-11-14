#include "RenderGraph/FrameSequence.h"

#include "RenderGraph/CensusWindow.h"
#include "RenderGraph/Switches.h"

#include <mutex>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace RenderGraph::FrameSequence
{
	namespace
	{
		/** @brief CS_ORG_FRAME_SEQUENCE: frames between logged frames, 0 when off. */
		std::uint64_t Every()
		{
			static const std::uint64_t every = [] {
				const std::uint64_t value = Switches::Number("CS_ORG_FRAME_SEQUENCE", 0);
				return value == 1 ? std::uint64_t{ 600 } : value;
			}();
			return every;
		}

		std::uint64_t Mix(std::uint64_t a_value)
		{
			a_value += 0x9E3779B97F4A7C15ull;
			a_value = (a_value ^ (a_value >> 30)) * 0xBF58476D1CE4E5B9ull;
			a_value = (a_value ^ (a_value >> 27)) * 0x94D049BB133111EBull;
			return a_value ^ (a_value >> 31);
		}

		std::uint64_t HashText(std::string_view a_text)
		{
			std::uint64_t hash = 1469598103934665603ull;
			for (const char c : a_text)
				hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
			return hash;
		}

		struct Window
		{
			std::unordered_set<std::uint64_t> sequences;
		};

		struct State
		{
			std::mutex lock;
			std::vector<std::string> events;  // this frame's, in order
			std::uint64_t hash = 0;           // this frame's anchors and epochs in order (not clients or targets)
			std::uint64_t frame = 0;
			CensusWindow::PerWindow<Window> windows;
		};

		State& Get()
		{
			static State state;
			return state;
		}

		void Add(std::string a_event, std::string_view a_key)
		{
			auto& state = Get();
			std::scoped_lock lock(state.lock);
			state.hash = Mix(state.hash ^ HashText(a_key));
			if (state.events.size() < 4096)
				state.events.push_back(std::move(a_event));
		}
	}

	bool On() { return Every() != 0; }

	void NoteRun(const char* a_anchor, bool a_held, const std::string& a_clients, std::int32_t a_input, std::int32_t a_output)
	{
		if (!On())
			return;
		Add(fmt::format("run {} ({}) clients [{}] in {} out {}", a_anchor, a_held ? "held" : "own epoch", a_clients, a_input, a_output), a_anchor);
	}

	void NoteEpoch(const char* a_label)
	{
		if (!On())
			return;
		Add(fmt::format("submit {}", a_label), a_label);
	}

	void NoteBoundary(std::string_view a_what)
	{
		if (!On())
			return;
		Add(fmt::format("boundary: {}", a_what), a_what);
	}

	void EndFrame()
	{
		if (!On())
			return;
		auto& state = Get();
		std::scoped_lock lock(state.lock);
		++state.frame;
		auto& window = state.windows.Active();
		window.sequences.insert(state.hash);
		// One frame's list every Every() frames of the world, the menu, the map or a loading screen.
		const auto kind = state.windows.ActiveKind();
		state.windows.EndFrame(Every(), [&](CensusWindow::Kind a_kind, Window& a_window, std::uint64_t a_frames) {
			if (a_kind != kind)
				return;
			logger::info("[ORG] frame sequence ({}, frame {}, {} distinct sequences in {} frames): {} events", CensusWindow::Name(a_kind), state.frame,
				a_window.sequences.size(), a_frames, state.events.size());
			for (std::size_t i = 0; i < state.events.size(); ++i)
				logger::info("[ORG] frame sequence {:3}: {}", i, state.events[i]);
		});
		state.events.clear();
		state.hash = 0;
	}
}
