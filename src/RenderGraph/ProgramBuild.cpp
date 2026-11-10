#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/ProgramBuild.h"

#	include "RenderGraph/RenderGraphRuntime.h"
#	include "Utils/D3D.h"

#	include <algorithm>
#	include <cstring>
#	include <fstream>
#	include <iterator>

namespace RenderGraph::ProgramBuild
{
	std::filesystem::path SourcePath(std::string_view a_source)
	{
		return std::filesystem::path(RenderGraphRuntime::kShaderDirectory) / a_source;
	}

	std::vector<std::byte> ReadSource(const std::filesystem::path& a_path)
	{
		std::ifstream file(a_path, std::ios::binary);
		const std::vector<char> chars((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		std::vector<std::byte> bytes(chars.size());
		if (!chars.empty())
			std::memcpy(bytes.data(), chars.data(), chars.size());
		return bytes;
	}

	std::uint64_t HashBinary(std::span<const std::byte> a_bytes)
	{
		std::uint64_t hash = 1469598103934665603ull;
		for (const auto byte : a_bytes) {
			hash ^= static_cast<std::uint8_t>(byte);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	Defines WithCommonDefines(const Defines& a_own, const wchar_t* a_target)
	{
		Defines defines = a_own;
		const auto has = [&](const std::wstring& a_name) {
			return std::ranges::any_of(defines, [&](const auto& a_define) { return a_define.first == a_name; });
		};
		// Defines are ASCII.
		const auto wide = [](std::string_view a_text) { return std::wstring(a_text.begin(), a_text.end()); };
		const auto add = [&](std::wstring a_name, std::wstring a_value) {
			if (!has(a_name))
				defines.emplace_back(std::move(a_name), std::move(a_value));
		};
		std::vector<std::string> ownNames;
		for (const auto& [name, value] : a_own) {
			std::string& narrow = ownNames.emplace_back();
			for (const wchar_t c : name)
				narrow.push_back(static_cast<char>(c));
		}
		const std::vector<std::string_view> explicitNames(ownNames.begin(), ownNames.end());
		for (const auto& [name, value] : Util::GetCommonShaderDefines(explicitNames))
			add(wide(name), wide(value));
		const std::wstring_view target = a_target ? a_target : L"";
		if (target.starts_with(L"cs_"))
			add(L"COMPUTESHADER", L"");
		else if (target.starts_with(L"vs_"))
			add(L"VSHADER", L"");
		else if (target.starts_with(L"ps_"))
			add(L"PSHADER", L"");
		add(L"WINPC", L"");
		add(L"DX11", L"");
		return defines;
	}

	org::services::ShaderCompileRequest MakeRequest(const std::filesystem::path& a_path, std::span<const std::byte> a_source, const std::wstring& a_entry,
		const wchar_t* a_target, const Defines& a_defines)
	{
		org::services::ShaderCompileRequest request{};
		request.sourceName = a_path.generic_string();
		request.source = a_source;
		request.entryPoint = a_entry;
		request.target = a_target;
		request.format = org::services::ShaderBinaryFormat::Spirv;
		request.warningsAsErrors = false;  // as the build-time compilation these replaced
		// Source-level debug info for Nsight and RenderDoc; drivers ignore it, and optimization is unchanged.
		request.debugInfo = true;
		request.includeDirectories = { RenderGraphRuntime::kShaderDirectory };
		// Storage images take the format of the view they are bound through, as in D3D11 (and the DXC game shaders, DxcShaderStore).
		// Without it DXC types every float4 RWTexture as Rgba32f, which a view of another format (RGBA16F) must not be written through.
		request.arguments.emplace_back(L"-fspv-use-unknown-image-format");
		request.defines.push_back({ L"BASICRHI_SHADER_API_VULKAN", L"1" });
		// Dual-target shaders (Common/OrgResources.hlsli) take their bindless declarations.
		request.defines.push_back({ L"CS_ORG_BINDLESS", L"1" });
		// The vendor, for workarounds of its shader compiler (CS_GPU_AMD: PortalPrograms.hlsli's interpreter).
		if (RenderGraphRuntime::VendorId() == 0x1002)
			request.defines.push_back({ L"CS_GPU_AMD", L"1" });
		for (const auto& [name, value] : a_defines)
			request.defines.push_back({ name, value });
		return request;
	}
}

#endif
