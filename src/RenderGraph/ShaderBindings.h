#pragma once

#include <Render/OwnedDescriptorBinding.h>
#include <Render/ShaderBindings.h>
#include <rhi.h>

#include <cstdint>
#include <memory>

/**
 * @brief The C++ side of dual-target shaders (package/Shaders/Common/OrgResources.hlsli). The push block helpers are ORG's
 * (Render/ShaderBindings.h); CS adds the samplers and views it makes through its runtime.
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
	using org::shader_bindings::kMaxPushWords;
	using Handle = org::shader_bindings::Handle;
	using org::shader_bindings::Words;
	using Latch = org::shader_bindings::Latch;
	using org::shader_bindings::LatchOf;
	using org::shader_bindings::Data;
	using SamplerPreset = org::shader_bindings::SamplerPreset;
	using org::shader_bindings::SamplerDescOf;

	/** @brief A sampler in the graph's sampler heap. Render thread, with the runtime active. Empty and logged on failure. */
	org::OwnedDescriptorBinding CreateSampler(const rhi::SamplerDesc& a_desc);

	/** @brief Converts a D3D11 sampler description, the one converter for every port. */
	rhi::SamplerDesc SamplerFromD3D11(const D3D11_SAMPLER_DESC& a_desc);

	inline org::OwnedDescriptorBinding CreateSamplerFromD3D11(const D3D11_SAMPLER_DESC& a_desc) { return CreateSampler(SamplerFromD3D11(a_desc)); }

	/**
	 * @brief The shared sampler for a_preset, made on first use and kept for the device it was made on. Every port that asks for
	 * a preset gets the same descriptor. Render thread or a graph callback, with the runtime active. Empty and logged on failure.
	 */
	org::OwnedDescriptorBinding Sampler(SamplerPreset a_preset);

	/**
	 * @brief An SRV in the graph's heap for a resource the graph does not track. a_owner keeps the resource alive.
	 * The caller must keep the resource in its shader-read layout whenever a shader may read it. Render thread. Empty on failure.
	 */
	org::OwnedDescriptorBinding CreateShaderResourceView(rhi::Resource a_resource, std::shared_ptr<const void> a_owner, const rhi::SrvDesc& a_desc);
}

