#pragma once

// The hierarchical depth (HZB) Core's cull tests against, made and filled by a feature.
// Core owns the test in HzbTest.hlsli, the latch words and the phase 2 block. The producer owns the texture and the pass that fills it.

#include <cstdint>
#include <memory>
#include <string_view>

namespace org
{
	class Buffer;
	class PixelBuffer;
}

namespace DCLF::Api
{
	// Defined in Core/Extensions/DepthPyramid.h.
	class DepthPyramidSetup;

	/**
	 * @brief What a pyramid's setup makes. Mip 0 is half the depth's next power of two on each side.
	 * That lets a mip be picked from a screen extent by log2 alone.
	 */
	struct DepthPyramid
	{
		std::shared_ptr<org::PixelBuffer> texture;  // null means frustum culling only, with no phase 2 passes
		std::uint32_t width = 0, height = 0, mips = 0;
		std::shared_ptr<org::Buffer> scratch;  // optional, zeroed once at the first depth commit after setup
	};

	/**
	 * @brief The pyramid's producer. One contribution only.
	 * setup runs on the render thread when Core makes the main view's resources, including after a target change.
	 */
	struct DepthPyramidContribution
	{
		std::string_view resource;         // the texture's graph name
		std::string_view scratchResource;  // the scratch buffer's graph name
		std::shared_ptr<const void> (*setup)(const DepthPyramidSetup& a_setup, DepthPyramid& a_out) = nullptr;
	};
}
