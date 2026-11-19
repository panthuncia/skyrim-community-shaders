#pragma once

// A scene graph whose parts a plugin hides after the shadow maps by shrinking nodes, so shadows keep them but later views do not.
// Improved Camera hides the first-person head and arms this way.
//
// The plugin sets a tiny local scale and restores it by the next frame's start, so a hidden node's world scale ends up far below its parent's.
// DCLF looks for that at the depth commit. The next frame's producer applies the same shrink to a second copy of the placement and palette rows.
// Epochs from the depth commit on read that copy when the frame's own check confirms the hide. A hide that starts shows one frame late.
//
// This stands in for an API that sets view masks on geometry, and will be replaced by it.

#include <string_view>

namespace DCLF::Api
{
	/**
	 * @brief A late pose contribution.
	 * root runs on the render thread at the frame's start and at the depth commit. Return null unless the plugin may hide parts now.
	 */
	struct LatePoseContribution
	{
		std::string_view name;
		const void* (*root)() = nullptr;
	};
}
