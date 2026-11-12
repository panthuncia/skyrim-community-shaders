#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/**
 * @brief The D3D12 builds' second source rewrite: a pulled build's frame bindings read by the stage itself. Standard library only,
 * so tools/shader-compare's dclf-shadertool runs it as the game does.
 *
 * On Vulkan, DCLF's pipeline layouts map every register a pulled build still declares (the pass's blocks, textures and samplers) to
 * its entry in the pass's frame record (DrawPipelines.h, DrawBindings) through VK_EXT_descriptor_heap. D3D12's layouts hold only root
 * constants (b190, the draw's; b191, the pass's). So the D3D12 build preprocesses its source (every include and macro resolved) and
 * this rewrite turns each register declaration but the push blocks' into reads through Common/DCLFFrameRecord.hlsli:
 * - a texture or buffer (t#): a static from the heap at the record's index for its register;
 * - a sampler (s#): likewise from the sampler heap;
 * - a constant buffer (b#): the pass's view of its block (the record's frameBlocks for a pass-wide block, else its address), each
 *   member a static of the same name. A block whose members are placed with packoffset is read register by register; one without
 *   is the same members in a struct, read as a ConstantBuffer (cbuffer packing either way).
 */
namespace DCLF::FrameSource
{
	struct Options
	{
		bool pixel = false;  // the stage: its blocks are the pixel stage's registers
		// The pass-wide blocks (DrawPipelines.h, kFramePushVS and kFramePushPS): a view each, in that order.
		std::uint32_t vertexBlocks = 0;
		std::uint32_t pixelBlocks = 0;
		// The constant buffer registers left as they are: the push blocks.
		std::uint32_t keepFirst = 190, keepLast = 191;
	};

	struct Result
	{
		std::string source;  // empty with an error
		std::string error;
		std::uint32_t textures = 0, samplers = 0, blocks = 0, members = 0;
	};

	/** @brief a_preprocessed (DXC -P output) rewritten for a D3D12 pulled build. */
	Result Rewrite(std::string_view a_preprocessed, const Options& a_options);
}
