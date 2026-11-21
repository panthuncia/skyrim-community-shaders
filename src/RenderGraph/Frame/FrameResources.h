#pragma once

#include "RenderGraph/GraphResources.h"

#include <d3d11.h>

#include <memory>
#include <string_view>

/**
 * @brief The frame's own transient textures, one per name for the whole frame. Stage graphs import them and no graph makes them.
 *
 * A name keeps its texture while its description holds, so a registration does not leave a copy alive in older builds.
 * Any change makes a new texture under the name. Textures kept across frames are declared instead (Frame/Declare.h), so a new
 * size is a new version of their slot. Render thread.
 */
namespace RenderGraph::Frame::Resources
{
	/** @brief The frame's transient texture a_name for a DEFAULT 2D desc. Null and logged on failure. */
	std::shared_ptr<GraphResources::Texture2D> Texture2D(std::string_view a_name, const D3D11_TEXTURE2D_DESC& a_desc);
}
