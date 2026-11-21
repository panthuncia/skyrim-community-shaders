#include "RenderGraph/CensusWindow.h"

#include "Globals.h"
#include "State.h"

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
}
