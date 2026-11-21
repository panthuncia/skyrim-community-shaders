#pragma once

// What ComputeProgram and RasterProgram share to build their stages.
// Only for translation units that include volk first (rhi_interop_vulkan.h).

#if defined(ORG_MODULE_SERVICES_HAS_DXC)

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

	/** @brief a_source is relative to RenderGraphRuntime::kShaderDirectory. */
	std::filesystem::path SourcePath(std::string_view a_source);

	/** @brief The file's bytes, empty when it is missing. */
	std::vector<std::byte> ReadSource(const std::filesystem::path& a_path);

	/** @brief FNV-1a of a binary, used as a live pipeline's fingerprint. */
	std::uint64_t HashBinary(std::span<const std::byte> a_bytes);

	/**
	 * @brief a_own plus the defines every D3D11 build of the stage gets, so both paths build one permutation.
	 * Reads State's defines, so call it at setup, not from a compile thread.
	 */
	Defines WithCommonDefines(const Defines& a_own, const wchar_t* a_target);

	/** @brief The DXC request for one stage, SPIR-V on Vulkan and DXIL on D3D12. a_source must outlive the request. */
	org::services::ShaderCompileRequest MakeRequest(const std::filesystem::path& a_path, std::span<const std::byte> a_source, const std::wstring& a_entry,
		const wchar_t* a_target, const Defines& a_defines);
}

#endif
