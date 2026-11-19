#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <Render/LatchBlock.h>
#	include <Render/OwnedDescriptorBinding.h>
#	include <rhi.h>

#	include <cstdint>
#	include <memory>
#	include <span>
#	include <type_traits>

/**
 * @brief The C++ side of dual-target shaders (package/Shaders/Common/OrgResources.hlsli).
 *
 * Under ORG a shader reads its resources through handles in its push block. A pass mirrors that block in a struct, in the
 * shader's list order, and pushes it at record time:
 *
 *     struct DiffuseIBLBlock { ShaderBindings::Handle envTexture, iblTexture, linearSampler; };
 *     program = ComputeProgram::Load(device, { .source = "IBL/DiffuseIBLCS.hlsl", .constantWords = ShaderBindings::Words<DiffuseIBLBlock>() });
 *     DiffuseIBLBlock block{ a_recording.Index(env), a_recording.Index(ibl), sampler.Index() };
 *     program->Dispatch(a_recording, ShaderBindings::Data(block), 1, 1, 1);
 *
 * Per-frame values go in a LATCH entry, so the recording does not depend on them and can be prepared ahead.
 */
namespace ShaderBindings
{
	/** @brief The largest push block in 32-bit words. 128 bytes is Vulkan's guaranteed minimum, and D3D12 root constants hold 64. */
	inline constexpr std::uint32_t kMaxPushWords = 32;

	/** @brief A descriptor-heap index, read by a shader's OrgHandle_<name>. */
	using Handle = std::uint32_t;

	/** @brief A compile error when the block is not whole words or is above kMaxPushWords. */
	template <class Block>
	constexpr std::uint32_t Words()
	{
		static_assert(std::is_trivially_copyable_v<Block>, "a push block is plain data");
		static_assert(sizeof(Block) % sizeof(std::uint32_t) == 0, "a push block is whole 32-bit words");
		static_assert(sizeof(Block) <= kMaxPushWords * sizeof(std::uint32_t), "a push block is at most 128 bytes: move constants to a CBUFFER entry");
		return sizeof(Block) / sizeof(std::uint32_t);
	}

	/** @brief The latch block's raw view, then the byte offset of the struct in it. */
	struct Latch
	{
		Handle handle = 0;
		std::uint32_t offset = 0;
	};

	/** @brief A LATCH entry for a latch block's region of a run, a_offset bytes in. The offset must be a multiple of 4. */
	inline Latch LatchOf(const org::LatchBlock& a_block, const org::RunRef& a_run, std::uint32_t a_offset = 0)
	{
		const auto region = a_block.At(a_run);
		return { region.srvIndex, static_cast<std::uint32_t>(region.offset) + a_offset };
	}

	template <class Block>
	std::span<const std::uint32_t> Data(const Block& a_block)
	{
		return { reinterpret_cast<const std::uint32_t*>(&a_block), Words<Block>() };
	}

	/** @brief A sampler in the graph's sampler heap. Render thread, with the runtime active. Empty and logged on failure. */
	org::OwnedDescriptorBinding CreateSampler(const rhi::SamplerDesc& a_desc);

	/** @brief Converts a D3D11 sampler description, the one converter for every port. */
	rhi::SamplerDesc SamplerFromD3D11(const D3D11_SAMPLER_DESC& a_desc);

	inline org::OwnedDescriptorBinding CreateSamplerFromD3D11(const D3D11_SAMPLER_DESC& a_desc) { return CreateSampler(SamplerFromD3D11(a_desc)); }

	/**
	 * @brief An SRV in the graph's heap for a resource the graph does not track. a_owner keeps the resource alive.
	 * The caller must keep the resource in its shader-read layout whenever a shader may read it. Render thread. Empty on failure.
	 */
	org::OwnedDescriptorBinding CreateShaderResourceView(rhi::Resource a_resource, std::shared_ptr<const void> a_owner, const rhi::SrvDesc& a_desc);
}

#endif
