#pragma once

// What ComputeProgram and RasterProgram share to build their stages: the shader source read from Data/Shaders, the DXC request with
// BasicRHI's ABI, and a hash of the binary. Only for translation units that include volk first (rhi_interop_vulkan.h).

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)

#	include <ORGModuleServices/ShaderCompiler.h>

#	include <cstddef>
#	include <cstdint>
#	include <filesystem>
#	include <span>
#	include <string>
#	include <string_view>
#	include <utility>
#	include <vector>

namespace RenderGraph::ProgramBuild
{
	using Defines = std::vector<std::pair<std::wstring, std::wstring>>;

	/** @brief A shader file's path: a_source is relative to RenderGraphRuntime::kShaderDirectory. */
	std::filesystem::path SourcePath(std::string_view a_source);

	/** @brief The file's bytes; empty when it is missing. */
	std::vector<std::byte> ReadSource(const std::filesystem::path& a_path);

	/** @brief FNV-1a of a binary: a live pipeline's fingerprint (PipelineRegistry). */
	std::uint64_t HashBinary(std::span<const std::byte> a_bytes);

	/**
	 * @brief a_own, then what Util::CompileShader gives every D3D11 build of the stage, so both paths build one permutation:
	 * CS's common shader defines (Util::GetCommonShaderDefines: the user's shader defines and the loaded features'), the stage's
	 * define for a_target (COMPUTESHADER for "cs_*", VSHADER for "vs_*", PSHADER for "ps_*"), WINPC and DX11. A name a_own already
	 * has is not added again, so a caller that adds them itself compiles the same permutation. Reads State's defines: call it at
	 * setup (a program's Load), not from a compile thread.
	 */
	Defines WithCommonDefines(const Defines& a_own, const wchar_t* a_target);

	/**
	 * @brief The DXC request for one stage: SPIR-V on Vulkan and DXIL on D3D12 (ShaderTarget) for a_target ("cs_6_6", "vs_6_6",
	 * "ps_6_6"), with debug info, Data/Shaders as the include directory, BASICRHI_SHADER_API_VULKAN or _D3D12, CS_ORG_BINDLESS (Common/OrgResources.hlsli), CS_GPU_AMD on AMD, then a_defines. a_source must outlive the request.
	 */
	org::services::ShaderCompileRequest MakeRequest(const std::filesystem::path& a_path, std::span<const std::byte> a_source, const std::wstring& a_entry,
		const wchar_t* a_target, const Defines& a_defines);
}

#endif
