#pragma once

// The shader binary the render graph's backend takes, for every runtime DXC request CS makes for it (ProgramBuild, Drawcall
// Limit Fix's stages). Vulkan takes SPIR-V with BasicRHI's descriptor-heap bindings; D3D12 takes DXIL, where the same HLSL
// indexes ResourceDescriptorHeap and SamplerDescriptorHeap (Shader Model 6.6) and its push block is root constants.
// Only for translation units built with the render graph and ORGModuleServices' compiler.

#include "RenderGraph/Backend.h"

#include <ORGModuleServices/ShaderCompiler.h>

#include <string>
#include <utility>

namespace RenderGraph::ShaderTarget
{
	/** @brief The binary format of the active backend. */
	inline org::services::ShaderBinaryFormat Format()
	{
		return Backend::IsD3D12() ? org::services::ShaderBinaryFormat::Dxil : org::services::ShaderBinaryFormat::Spirv;
	}

	/** @brief Whether the active backend takes SPIR-V. */
	inline bool IsSpirv() { return Format() == org::services::ShaderBinaryFormat::Spirv; }

	/** @brief "SPIR-V" or "DXIL", for log lines. */
	inline const char* FormatName() { return IsSpirv() ? "SPIR-V" : "DXIL"; }

	/** @brief The define that names the shader API to BasicRHI's shared HLSL (BASICRHI_SHADER_API_VULKAN or _D3D12). */
	inline const wchar_t* ApiDefine() { return IsSpirv() ? L"BASICRHI_SHADER_API_VULKAN" : L"BASICRHI_SHADER_API_D3D12"; }

	/**
	 * @brief Sets the request's format to the active backend's. A request's SPIR-V arguments go in only when IsSpirv()
	 * (AddSpirvArgument). It adds no define, so a Vulkan request keeps the cache key it had.
	 */
	inline void Apply(org::services::ShaderCompileRequest& a_request) { a_request.format = Format(); }

	/** @brief Adds a DXC argument that only SPIR-V code generation knows (-fvk-*, -fspv-*). Nothing on D3D12. */
	inline void AddSpirvArgument(org::services::ShaderCompileRequest& a_request, std::wstring a_argument)
	{
		if (IsSpirv())
			a_request.arguments.push_back(std::move(a_argument));
	}
}
