#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

/**
 * @brief The window a frame belongs to for the native-mode censuses, so a loading screen's work does not hide in the world's.
 *
 * A census reports a window after CS_ORG_CENSUS_FRAMES frames in it, default 600, and also when the game leaves it.
 * CS_ORG_CENSUS_TOP, default 25, is how many entries a report lists.
 */
namespace RenderGraph::CensusWindow
{
	enum class Kind : std::uint8_t
	{
		World,
		Menu,
		Loading,
		Map,
		Count
	};

	/** @brief The window now, from State's menu flags. Any thread. */
	Kind Current();

	const char* Name(Kind a_kind);

	/** @brief CS_ORG_CENSUS_FRAMES, default 600, at least 1. */
	std::uint64_t ReportFrames();

	/** @brief CS_ORG_CENSUS_TOP, default 25. */
	std::size_t ReportTop();

	/** @brief One tally per window. Not thread-safe, so the census guards it. */
	template <class Data>
	class PerWindow
	{
	public:
		/** @brief The tally of the window the current frame counts in. */
		Data& Active() { return data[static_cast<std::size_t>(active)]; }

		Kind ActiveKind() const { return active; }

		/** @brief Closes a frame and reports the active window after a_every frames or when the game left it. The tally then restarts. */
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
