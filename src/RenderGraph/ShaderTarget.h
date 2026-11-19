#pragma once

// The shader binary format the graph's backend takes. Vulkan takes SPIR-V, D3D12 takes DXIL where the same HLSL
// indexes the SM 6.6 descriptor heaps. Only for translation units built with the render graph and ORGModuleServices' compiler.

#include "RenderGraph/Backend.h"

#include <ORGModuleServices/ShaderCompiler.h>

#include <string>
#include <utility>

namespace RenderGraph::ShaderTarget
{
	inline org::services::ShaderBinaryFormat Format()
	{
		return Backend::IsD3D12() ? org::services::ShaderBinaryFormat::Dxil : org::services::ShaderBinaryFormat::Spirv;
	}

	inline bool IsSpirv() { return Format() == org::services::ShaderBinaryFormat::Spirv; }

	inline const char* FormatName() { return IsSpirv() ? "SPIR-V" : "DXIL"; }

	/** @brief The define that names the shader API to BasicRHI's shared HLSL. */
	inline const wchar_t* ApiDefine() { return IsSpirv() ? L"BASICRHI_SHADER_API_VULKAN" : L"BASICRHI_SHADER_API_D3D12"; }

	/** @brief Sets the request's format. It adds no define, so a Vulkan request keeps its cache key. */
	inline void Apply(org::services::ShaderCompileRequest& a_request) { a_request.format = Format(); }

	/** @brief Adds a SPIR-V-only DXC argument (-fvk-*, -fspv-*). Nothing on D3D12. */
	inline void AddSpirvArgument(org::services::ShaderCompileRequest& a_request, std::wstring a_argument)
	{
		if (IsSpirv())
			a_request.arguments.push_back(std::move(a_argument));
	}
}
