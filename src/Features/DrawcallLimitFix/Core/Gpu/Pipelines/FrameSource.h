#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/**
 * @brief The D3D12 builds' second source rewrite, so a pulled build reads its frame bindings itself.
 *
 * On Vulkan, the layouts map each remaining register to the pass's frame record. D3D12's layouts hold only root constants, so the
 * preprocessed source has every register declaration except the push blocks turned into reads through Common/DCLFFrameRecord.hlsli.
 * Standard library only, so tools/shader-compare runs it as the game does.
 */
namespace DCLF::FrameSource
{
	// The register space of the bound pass-wide blocks (Options::boundBlocks): block ordinal n is register(bn, space9).
	inline constexpr std::uint32_t kBoundBlockSpace = 9;
	// Spaces kBoundBlockSpace.. for a second block at one register; the layout binds the same table at each.
	inline constexpr std::uint32_t kBoundBlockSpaces = 2;

	struct Options
	{
		bool pixel = false;  // the stage, whose blocks are the pixel stage's registers
		// The pass-wide blocks, a view each, in kFramePushVS and kFramePushPS order.
		std::uint32_t vertexBlocks = 0;
		std::uint32_t pixelBlocks = 0;
		// Registers left as they are, the push blocks.
		std::uint32_t keepFirst = 190, keepLast = 191;
		// A shadow view's build reads its blocks through the view's push words instead of the frame record. The masks above are not read.
		bool shadow = false;
		// The pass-wide blocks as bound constant buffers, register(b<ordinal>, space9), which the layout's PushIndex ranges bind
		// (DrawPipelines.h, kFrameBlockSpace). False: views read by heap index from the frame record. Not for shadow builds.
		bool boundBlocks = false;
	};

	struct Result
	{
		std::string source;  // empty with an error
		std::string error;
		std::uint32_t textures = 0, samplers = 0, blocks = 0, members = 0;
	};

	/** @brief a_preprocessed rewritten for a D3D12 pulled build. */
	Result Rewrite(std::string_view a_preprocessed, const Options& a_options);
}
