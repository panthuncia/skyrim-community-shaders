#pragma once

#include "RenderGraph/GraphResources.h"

#include <d3d11.h>

#include <memory>
#include <string_view>

/**
 * @brief The frame's own textures, one per name for the whole frame. Stage graphs import them and no graph makes them.
 *
 * A name keeps its texture while its description holds. A history or persistent texture of a new size becomes a new version of
 * the same slot at the next frame boundary, so no graph is rebuilt. Any other change makes a new texture under the name.
 * This stops each registration from leaving a copy alive in older builds and restarting histories. Render thread.
 */
namespace RenderGraph::Frame::Resources
{
	/** @brief The texture, and whether its contents are new so a history needs its clear. */
	struct Made
	{
		std::shared_ptr<GraphResources::Texture2D> texture;
		bool fresh = false;
		explicit operator bool() const { return texture && *texture; }
	};

	/** @brief The frame's texture a_name for a DEFAULT 2D desc. History and persistent textures are versioned. Empty and logged on failure. */
	Made Texture2D(std::string_view a_name, const D3D11_TEXTURE2D_DESC& a_desc, GraphResources::Lifetime a_lifetime);
}
