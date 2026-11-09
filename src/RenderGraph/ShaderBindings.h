#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <Render/OwnedDescriptorBinding.h>
#	include <rhi.h>

#	include <cstdint>
#	include <span>
#	include <type_traits>

/**
 * @brief The C++ side of dual-target shaders (package/Shaders/Common/OrgResources.hlsli).
 *
 * Under ORG such a shader reads its resources through handles in its push block (a cbuffer at b0). A pass mirrors that block in a
 * struct: one Handle per SRV, UAV, SAMPLER, CBUFFER and EXTERN entry of the shader's list, in list order (a SHARED entry is
 * SharedConstants::Handles, four words), then each PUSH struct as an alignas(16) member. At record
 * time it fills the handles from its declared views (RecordingContext::Index) and pushes the block
 * with ComputeProgram::Dispatch or RasterProgram::DrawDesc::constants:
 *
 *     struct DiffuseIBLBlock { ShaderBindings::Handle envTexture, iblTexture, linearSampler; };
 *     program = ComputeProgram::Load(device, { .source = "IBL/DiffuseIBLCS.hlsl", .constantWords = ShaderBindings::Words<DiffuseIBLBlock>() });
 *     DiffuseIBLBlock block{ a_recording.Index(env), a_recording.Index(ibl), sampler.Index() };
 *     program->Dispatch(a_recording, ShaderBindings::Data(block), 1, 1, 1);
 *
 * Samplers are not graph resources: create one per program with CreateSampler at setup and keep the binding while the program lives.
 */
namespace ShaderBindings
{
	/**
	 * @brief The largest push block, in 32-bit words: 128 bytes, Vulkan's guaranteed maxPushConstantsSize. D3D12's root signature
	 * holds 64, so the same block fits there as root constants.
	 */
	inline constexpr std::uint32_t kMaxPushWords = 32;

	/** @brief A descriptor-heap index: what a shader's OrgHandle_<name> reads. */
	using Handle = std::uint32_t;

	/** @brief The words of a push block. A compile error when the block is not whole words or is above kMaxPushWords. */
	template <class Block>
	constexpr std::uint32_t Words()
	{
		static_assert(std::is_trivially_copyable_v<Block>, "a push block is plain data");
		static_assert(sizeof(Block) % sizeof(std::uint32_t) == 0, "a push block is whole 32-bit words");
		static_assert(sizeof(Block) <= kMaxPushWords * sizeof(std::uint32_t), "a push block is at most 128 bytes: move constants to a CBUFFER entry");
		return sizeof(Block) / sizeof(std::uint32_t);
	}

	/** @brief A push block as the words ComputeProgram::Dispatch and RasterProgram::DrawDesc::constants take. */
	template <class Block>
	std::span<const std::uint32_t> Data(const Block& a_block)
	{
		return { reinterpret_cast<const std::uint32_t*>(&a_block), Words<Block>() };
	}

	/**
	 * @brief A sampler in the graph's sampler heap, for a SAMPLER entry: its Index() is the handle. Render thread, with the runtime
	 * active. Empty, logged, when there is no descriptor service or the backend cannot create the sampler.
	 */
	org::OwnedDescriptorBinding CreateSampler(const rhi::SamplerDesc& a_desc);
}

#endif
