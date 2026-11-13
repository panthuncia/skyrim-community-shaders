#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/FrameAnchors.h"

#	include <array>
#	include <cstdint>
#	include <memory>

namespace org
{
	class RenderPass;
	class Resource;
}

namespace GraphPasses
{
	/**
	 * @brief A pass that clears a graph texture with a UAV to a float value (UnorderedAccessClear, ClearUavFloat), for a frame
	 * anchor client's make. The clear is the texture's first use in the frame, so a transient texture stays aliasable. a_texture is an
	 * import's slot or any graph resource.
	 */
	std::shared_ptr<org::RenderPass> MakeUavClear(FrameAnchors::Texture a_texture, std::array<float, 4> a_value);

	/** @brief MakeUavClear to a uint value (ClearUavUint), for integer formats. */
	std::shared_ptr<org::RenderPass> MakeUavClearUint(FrameAnchors::Texture a_texture, std::array<std::uint32_t, 4> a_value);
}

#endif
