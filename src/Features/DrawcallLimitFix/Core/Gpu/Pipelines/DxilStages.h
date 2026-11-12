#pragma once

// DCLF's stages on D3D12 (native mode, CS_ORG_NATIVE_BACKEND=d3d12): what SpirvReflection and the SPIR-V scans read of a stage, read
// from DXIL instead, and the pulled builds' frame rewrite (FrameSource.h) before their compile. Through the dxcompiler.dll the render
// graph's shader compiler loaded. Any thread.

#include <cstddef>
#include <span>
#include <string>

namespace org::services
{
	struct ShaderCompileRequest;
}

namespace DCLF
{
	struct SpirvReflection;
}

namespace DCLF::DxilStages
{
	/** @brief The define a D3D12 pulled build carries (ShaderPrograms): its source is preprocessed and rewritten before it compiles. */
	inline constexpr const wchar_t* kFrameDefine = L"DCLF_D3D12_FRAME";

	/** @brief Whether a_binary is a DXIL container. */
	bool IsDxil(std::span<const std::byte> a_binary);

	/**
	 * @brief A DXIL stage's reflection, in SpirvReflection's terms: its bindings numbered as the SPIR-V builds shift them (b#, t# + 200,
	 * s# + 400, u# + 500, set 0; VS_PerFrame's space 1 is b12), the pass push block (b191) left out as push data; its constant
	 * buffers' members; its inputs; its colour outputs. False when DXC cannot read it.
	 */
	bool Reflect(std::span<const std::byte> a_dxil, SpirvReflection& a_out);

	/**
	 * @brief A DXIL pixel stage's facts, as FragmentDefersDepth and FragmentExports read SPIR-V: whether it writes depth or the sample
	 * mask, and whether it defeats early depth testing (it discards, writes either, stores to a texture or typed buffer, or uses an
	 * atomic). A stage DXC cannot read defers.
	 */
	struct FragmentFacts
	{
		bool defersDepth = true;
		bool exportsDepth = false;
		bool exportsSampleMask = false;
	};
	FragmentFacts Fragment(std::span<const std::byte> a_dxil);

	/** @brief Whether a_request is a D3D12 pulled build (it carries kFrameDefine). */
	bool NeedsFrameSource(const org::services::ShaderCompileRequest& a_request);

	/**
	 * @brief A D3D12 pulled build's source: Common/DCLFFrameRecord.hlsli first, preprocessed with the request's defines and include
	 * directories, then FrameSource's rewrite for its stage (ps_* or vs_*). a_request's source then points into a_storage. False with
	 * a_error when either step fails.
	 */
	bool PrepareFrameSource(org::services::ShaderCompileRequest& a_request, std::string& a_storage, std::string& a_error);
}
