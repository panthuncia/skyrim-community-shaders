#if defined(ORG_MODULE_SERVICES_HAS_DXC)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/ProgramBuild.h"

#	include "Utils/D3D.h"

#	include <algorithm>
#	include <string>
#	include <string_view>
#	include <vector>

namespace RenderGraph::ProgramBuild
{
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
}

#endif
