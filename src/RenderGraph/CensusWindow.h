#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

/**
 * @brief The window a frame belongs to, for the native-mode censuses (OpCensus, RunShapes, UndeclaredAccess): the world, the main
 * menu, a loading screen or the map. Each census keeps one tally per window and reports it on its own, so a loading screen's work
 * does not hide in the world's.
 *
 * Every census reports a window after CS_ORG_CENSUS_FRAMES frames in it (default 600), and also when the game leaves it, so a short
 * loading screen still gets its line. CS_ORG_CENSUS_TOP (default 25) is how many entries a report lists.
 */
namespace RenderGraph::CensusWindow
{
	enum class Kind : std::uint8_t
	{
		World,
		Menu,     // the main menu
		Loading,  // a loading screen
		Map,      // the world map
		Count
	};

	/** @brief The window now, from State's menu flags. Any thread: it reads plain flags. */
	Kind Current();

	/** @brief "world", "menu", "loading screen" or "map". */
	const char* Name(Kind a_kind);

	/** @brief CS_ORG_CENSUS_FRAMES: the frames a window collects before its report (default 600, at least 1). */
	std::uint64_t ReportFrames();

	/** @brief CS_ORG_CENSUS_TOP: how many entries a report lists (default 25). */
	std::size_t ReportTop();

	/**
	 * @brief One tally per window. The census adds to Active() during a frame and calls EndFrame once a frame. Not thread-safe: the
	 * census guards it.
	 */
	template <class Data>
	class PerWindow
	{
	public:
		/** @brief The tally of the window the current frame counts in. */
		Data& Active() { return data[static_cast<std::size_t>(active)]; }

		/** @brief The window the current frame counts in. */
		Kind ActiveKind() const { return active; }

		/**
		 * @brief Closes a frame. a_report(kind, data, frames) runs for the active window when it has a_every frames, or when the game
		 * left it with at least one frame counted. The reported tally starts again empty.
		 */
		template <class Report>
		void EndFrame(std::uint64_t a_every, Report&& a_report)
		{
			auto index = static_cast<std::size_t>(active);
			++frames[index];
			if (frames[index] >= a_every)
				Flush(index, a_report);
			const Kind now = Current();
			if (now != active) {
				if (frames[index])
					Flush(index, a_report);
				active = now;
			}
		}

	private:
		template <class Report>
		void Flush(std::size_t a_index, Report& a_report)
		{
			a_report(static_cast<Kind>(a_index), data[a_index], frames[a_index]);
			data[a_index] = Data{};
			frames[a_index] = 0;
		}

		std::array<Data, static_cast<std::size_t>(Kind::Count)> data{};
		std::array<std::uint64_t, static_cast<std::size_t>(Kind::Count)> frames{};
		Kind active = Kind::Menu;
	};
}
