#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <array>
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
	 * anchor client's make. The clear is the texture's first use in the frame, so a transient texture stays aliasable.
	 */
	std::shared_ptr<org::RenderPass> MakeUavClear(std::shared_ptr<org::Resource> a_texture, std::array<float, 4> a_value);
}

#endif
