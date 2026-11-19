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

namespace GraphResources
{
	class Resource;
}

namespace RenderGraph
{
	class RunGate;
}

namespace GraphPasses
{
	/** @brief A gate entry the clear waits for. The pass is recorded at execution and skips runs that did not open it. */
	struct ClearGate
	{
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t entry = 0;
	};

	/**
	 * @brief A pass that clears a graph texture with a UAV to a float value.
	 * The clear is the texture's first use in the frame, so a transient stays aliasable.
	 */
	std::shared_ptr<org::RenderPass> MakeUavClear(FrameAnchors::Texture a_texture, std::array<float, 4> a_value, ClearGate a_gate = {});

	/** @brief MakeUavClear to a uint value, for integer formats. */
	std::shared_ptr<org::RenderPass> MakeUavClearUint(FrameAnchors::Texture a_texture, std::array<std::uint32_t, 4> a_value, ClearGate a_gate = {});

	/** @brief Clears a twin through its slot when it is versioned. A versioned twin's clears are made this way, never with Graph(). */
	std::shared_ptr<org::RenderPass> MakeUavClear(const GraphResources::Resource& a_twin, std::array<float, 4> a_value, ClearGate a_gate = {});
}

#endif
