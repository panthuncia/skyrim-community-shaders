#pragma once

// DCLF's stages on D3D12: the pulled builds' frame rewrite before they compile, and OMS's DXIL reflection under DCLF's names.
// Uses the dxcompiler.dll the render graph's compiler loaded. Any thread.

#include <ORGModuleServices/ShaderReflection.h>

#include <cstddef>
#include <span>
#include <string>

namespace org::services
{
	struct ShaderCompileRequest;
}

namespace DCLF::DxilStages
{
	/** @brief Marks a D3D12 pulled build, whose source is preprocessed and rewritten before it compiles. */
	inline constexpr const wchar_t* kFrameDefine = L"DCLF_D3D12_FRAME";
	/** @brief Marks a D3D12 pulled Utility build, whose blocks come from the shadow view's push words. */
	inline constexpr const wchar_t* kShadowFrameDefine = L"DCLF_D3D12_SHADOW_FRAME";

	/** @brief Whether a_binary is a DXIL container. */
	using org::services::IsDxil;

	/** @brief A DXIL pixel stage's depth-related facts. A stage DXC cannot read is treated as deferring depth. */
	using FragmentFacts = org::services::DxilFragmentFacts;
	inline FragmentFacts Fragment(std::span<const std::byte> a_dxil) { return org::services::DxilFragment(a_dxil); }

	/** @brief Whether a_request is a D3D12 pulled build. */
	bool NeedsFrameSource(const org::services::ShaderCompileRequest& a_request);

	/**
	 * @brief A D3D12 pulled build's source, preprocessed with Common/DCLFFrameRecord.hlsli first, then rewritten by FrameSource.
	 * a_request's source then points into a_storage.
	 */
	bool PrepareFrameSource(org::services::ShaderCompileRequest& a_request, std::string& a_storage, std::string& a_error);
}
