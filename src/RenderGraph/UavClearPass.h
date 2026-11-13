#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <array>
#	include <cstdint>
#	include <memory>

namespace org
{
	class RenderPass;
	class Resource;
	class VersionedTexture;
}

namespace GraphResources
{
	class Resource;
}

namespace GraphPasses
{
	/**
	 * @brief A pass that clears a graph texture with a UAV to a float value (UnorderedAccessClear, ClearUavFloat), for a frame
	 * anchor client's make. The clear is the texture's first use in the frame, so a transient texture stays aliasable.
	 */
	std::shared_ptr<org::RenderPass> MakeUavClear(std::shared_ptr<org::Resource> a_texture, std::array<float, 4> a_value);

	/** @brief MakeUavClear to a uint value (ClearUavUint), for integer formats. */
	std::shared_ptr<org::RenderPass> MakeUavClearUint(std::shared_ptr<org::Resource> a_texture, std::array<std::uint32_t, 4> a_value);

	/** @brief MakeUavClear of a versioned texture's slot: each run clears the slot's current version. */
	std::shared_ptr<org::RenderPass> MakeUavClear(std::shared_ptr<org::VersionedTexture> a_slot, std::array<float, 4> a_value);
	std::shared_ptr<org::RenderPass> MakeUavClearUint(std::shared_ptr<org::VersionedTexture> a_slot, std::array<std::uint32_t, 4> a_value);

	/**
	 * @brief MakeUavClear of a graph twin: its slot when it is versioned (GraphResources::Options::versioned), else its graph
	 * resource. A versioned twin's clears are made this way, never with Graph().
	 */
	std::shared_ptr<org::RenderPass> MakeUavClear(const GraphResources::Resource& a_twin, std::array<float, 4> a_value);
}

#endif
