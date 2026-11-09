#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <rhi.h>

#	include <cstdint>
#	include <memory>
#	include <vector>

namespace org
{
	class LatchBlock;
	class RenderPass;
	class Resource;
}

struct ComputeProgram;

namespace GraphPasses
{
	/**
	 * @brief ORG's GenerateMips.hlsl (org::GenerateMipsPass's shader, docs/pipelines.md "Built-in passes with shaders") as a CS program.
	 *
	 * CS's graphs have no org::PipelineCompiler, so org::GenerateMipsPass cannot declare its pipeline here. This builds the same shader
	 * with ComputeProgram from Data/Shaders/OpenRenderGraph (cmake/RenderGraph.cmake ships it), with the same 7-word push block. Each mip
	 * is the area-weighted average of the mip above it; float, UNORM and SNORM formats whose UAVs take typed loads.
	 */
	struct GenerateMipsProgram
	{
		std::shared_ptr<const ComputeProgram> compute;
		rhi::CommandSignaturePtr signature;  // one indirect dispatch, against compute's layout
		bool array = false;                  // built for 2D arrays and cube maps (ORG_GENERATE_MIPS_ARRAY)
	};

	/**
	 * @brief Builds the program: a_array for texture arrays and cube maps, else for 2D textures. Render thread, at setup (it blocks on a
	 * compile the shader store does not have). Null, logged, when it cannot be built.
	 */
	std::shared_ptr<const GenerateMipsProgram> LoadGenerateMips(rhi::Device a_device, bool a_array);

	/** @brief One indirect dispatch's arguments, as the gate's latch region holds them: group counts, then a pad to 16 bytes. */
	struct DispatchGroups
	{
		std::uint32_t x = 0, y = 0, z = 0, pad = 0;
	};

	/**
	 * @brief The group counts of each mip step of a a_width x a_height texture with a_mips mips and a_slices slices (or faces): one
	 * entry per mip from 1, for a gate's latch region.
	 */
	std::vector<DispatchGroups> GenerateMipsGroups(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mips, std::uint32_t a_slices);

	/**
	 * @brief Makes the mip chain's dispatches conditional for a client that builds the mips only on some frames, while its recording is
	 * kept: step i (mip i + 1) reads its group counts from a_latch's region of the execution's frame slot, at a_offset + i * 16 bytes
	 * (DispatchGroups). The client's commit writes GenerateMipsGroups there, or zeros to skip the chain.
	 */
	struct GenerateMipsGate
	{
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t offset = 0;
	};

	/**
	 * @brief A pass that builds mips 1 to N-1 of a_texture from mip 0, for a frame anchor client's make. The texture is a 2D texture, an
	 * array or a cube map (a_program built to match), with unordered access on every mip. Without a gate it dispatches every execution.
	 */
	std::shared_ptr<org::RenderPass> MakeGenerateMips(std::shared_ptr<const GenerateMipsProgram> a_program, std::shared_ptr<org::Resource> a_texture,
		GenerateMipsGate a_gate = {});
}

#endif
