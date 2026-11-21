#pragma once

#include <cstdint>

/** @brief The window the game shows now: the world, a menu, a loading screen or the map. */
namespace RenderGraph::CensusWindow
{
	enum class Kind : std::uint8_t
	{
		World,
		Menu,
		Loading,
		Map,
	};

	/** @brief The window now, from State's menu flags. Any thread. */
	Kind Current();
}
