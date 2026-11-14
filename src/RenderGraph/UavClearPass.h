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
	/**
	 * @brief A run gate entry a clear waits for: the clear happens only on runs whose commit opened it (any Open call with work). Such a
	 * pass is recorded at execution (FrameAnchors::PassDesc::atExecution), where it skips the other runs, and is gated (PassDesc::gated).
	 */
	struct ClearGate
	{
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t entry = 0;
	};

	/**
	 * @brief A pass that clears a graph texture with a UAV to a float value (UnorderedAccessClear, ClearUavFloat), for a frame
	 * anchor client's make. The clear is the texture's first use in the frame, so a transient texture stays aliasable. a_texture is an
	 * import's slot or any graph resource.
	 */
	std::shared_ptr<org::RenderPass> MakeUavClear(FrameAnchors::Texture a_texture, std::array<float, 4> a_value, ClearGate a_gate = {});

	/** @brief MakeUavClear to a uint value (ClearUavUint), for integer formats. */
	std::shared_ptr<org::RenderPass> MakeUavClearUint(FrameAnchors::Texture a_texture, std::array<std::uint32_t, 4> a_value, ClearGate a_gate = {});

	/**
	 * @brief MakeUavClear of a graph twin: its slot when it is versioned (GraphResources::Options::versioned), else its graph
	 * resource. A versioned twin's clears are made this way, never with Graph().
	 */
	std::shared_ptr<org::RenderPass> MakeUavClear(const GraphResources::Resource& a_twin, std::array<float, 4> a_value, ClearGate a_gate = {});
}

#endif
