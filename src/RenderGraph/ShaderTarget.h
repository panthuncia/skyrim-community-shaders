#pragma once

// The shader binary format the graph's backend takes, from OMS's (ORGModuleServices/Programs/ShaderTarget.h). Vulkan takes
// SPIR-V, D3D12 takes DXIL. Only for translation units built with the render graph and ORGModuleServices' compiler.

#include "RenderGraph/Backend.h"

#include <ORGModuleServices/Programs/ShaderTarget.h>
#include <ORGModuleServices/ShaderCompiler.h>

namespace RenderGraph::ShaderTarget
{
	inline org::services::ShaderBinaryFormat Format() { return org::services::ShaderFormatFor(Backend::Active()); }

	inline bool IsSpirv() { return Format() == org::services::ShaderBinaryFormat::Spirv; }

	inline const char* FormatName() { return org::services::ShaderFormatName(Format()); }

	/** @brief Sets the request's format. It adds no define, so a Vulkan request keeps its cache key. */
	inline void Apply(org::services::ShaderCompileRequest& a_request) { a_request.format = Format(); }
}
