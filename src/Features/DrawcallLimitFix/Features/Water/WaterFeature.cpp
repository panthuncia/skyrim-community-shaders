// Water: the engine's BSWaterShader draws drawn by the render graph (CS_ORG_WATER=1, default off). The feature reserves the EngineDraws
// anchor and installs the engine hooks; the rest is WaterCapture.cpp (the engine side) and WaterDraws.cpp (the graph side).

#include "Features/DrawcallLimitFix/Features/Water/Water.h"

#include "RenderGraph/Switches.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace DCLF::Features::Water
{
	namespace
	{
		void Install(Api::InstallContext&)
		{
			if (!Enabled())
				return;
			// At PostPostLoad, before the render graph initializes: the anchors' epochs are fixed then.
			WaterDraws::Reserve();
			InstallHooks();
			logger::info("[DCLF] water: on (CS_ORG_WATER=1): the engine's water batches are drawn at the EngineDraws anchor");
		}

		std::string Status()
		{
			return WaterDraws::Status();
		}
	}

	const Diagnostics& GetDiagnostics()
	{
		static const Diagnostics diagnostics = [] {
			Diagnostics out;
			const auto split = [](const std::string& a_list, auto&& a_each) {
				std::size_t start = 0;
				while (start <= a_list.size()) {
					const auto end = (std::min)(a_list.find(',', start), a_list.size());
					if (end > start)
						a_each(a_list.substr(start, end - start));
					start = end + 1;
				}
			};
			auto parts = Switches::Value("CS_ORG_WATER_PARTS");
			if (parts.empty())
				parts = Switches::Value("CS_ORG_WATER_VIEWS");
			if (!parts.empty()) {
				out.mask = out.surface = false;
				split(parts, [&](const std::string& a_part) {
					out.mask = out.mask || a_part == "mask";
					out.surface = out.surface || a_part == "surface";
				});
			}
			if (const auto techniques = Switches::Value("CS_ORG_WATER_TECHNIQUES"); !techniques.empty()) {
				out.techniques = 0;
				split(techniques, [&](const std::string& a_technique) {
					const auto value = std::strtoul(a_technique.c_str(), nullptr, 10);
					if (value < 32)
						out.techniques |= 1u << value;
				});
			}
			out.safe = static_cast<std::uint32_t>(std::strtoul(Switches::Value("CS_ORG_WATER_SAFE").c_str(), nullptr, 10));
			if (!parts.empty() || out.techniques != ~0u || out.safe)
				logger::info("[DCLF] water: diagnostics: parts {}{}, techniques {:#x}, safe {}", out.mask ? "mask " : "", out.surface ? "surface" : "",
					out.techniques, out.safe);
			return out;
		}();
		return diagnostics;
	}

	bool Enabled()
	{
		static const bool enabled = Switches::On("CS_ORG_WATER");
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "water", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
