#include "RenderGraph/CensusWindow.h"

#include "Globals.h"
#include "RenderGraph/Switches.h"
#include "State.h"

#include <algorithm>

namespace RenderGraph::CensusWindow
{
	Kind Current()
	{
		const auto* state = globals::state;
		if (!state)
			return Kind::Menu;
		if (state->isLoadingMenuOpen)
			return Kind::Loading;
		if (state->isMapMenuOpen)
			return Kind::Map;
		if (state->isMainMenuOpen)
			return Kind::Menu;
		return Kind::World;
	}

	const char* Name(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::World:
			return "world";
		case Kind::Menu:
			return "menu";
		case Kind::Loading:
			return "loading screen";
		case Kind::Map:
			return "map";
		default:
			return "?";
		}
	}

	std::uint64_t ReportFrames()
	{
		static const std::uint64_t frames = (std::max<std::uint64_t>)(Switches::Number("CS_ORG_CENSUS_FRAMES", 600), 1);
		return frames;
	}

	std::size_t ReportTop()
	{
		static const auto top = static_cast<std::size_t>(Switches::Number("CS_ORG_CENSUS_TOP", 25));
		return top;
	}
}
