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
	 * @brief ORG's GenerateMips.hlsl built as a CS ComputeProgram.
	 *
	 * CS's graphs have no org::PipelineCompiler, so org::GenerateMipsPass cannot declare its pipeline here.
	 * Each mip is the area-weighted average of the one above. Formats must allow typed UAV loads.
	 */
	struct GenerateMipsProgram
	{
		std::shared_ptr<const ComputeProgram> compute;  // its dispatch signature serves the gated steps
		bool array = false;                             // ORG_GENERATE_MIPS_ARRAY, for 2D arrays and cube maps
	};

	/** @brief Render thread, at setup, since it blocks on an uncached compile. Null and logged on failure. */
	std::shared_ptr<const GenerateMipsProgram> LoadGenerateMips(rhi::Device a_device, bool a_array);

	/** @brief Group counts, then a pad to 16 bytes. */
	struct DispatchGroups
	{
		std::uint32_t x = 0, y = 0, z = 0, pad = 0;
	};

	/** @brief The group counts of each mip step from mip 1, for a gate's latch region. */
	std::vector<DispatchGroups> GenerateMipsGroups(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mips, std::uint32_t a_slices);

	/**
	 * @brief Makes the mip dispatches conditional while the recording is kept. Step i reads its counts at a_offset + i * 16.
	 * The commit writes GenerateMipsGroups there, or zeros to skip the chain.
	 */
	struct GenerateMipsGate
	{
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t offset = 0;
	};

	/** @brief Builds mips 1 to N-1 from mip 0. Every mip needs unordered access. Without a gate it dispatches every execution. */
	std::shared_ptr<org::RenderPass> MakeGenerateMips(std::shared_ptr<const GenerateMipsProgram> a_program, std::shared_ptr<org::Resource> a_texture,
		GenerateMipsGate a_gate = {});
}

#endif
