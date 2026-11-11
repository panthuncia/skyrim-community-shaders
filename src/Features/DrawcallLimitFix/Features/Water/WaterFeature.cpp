// Water: the engine's BSWaterShader draws drawn by the render graph (CS_ORG_WATER=1, default off). The feature reserves the EngineDraws
// anchor and installs the engine hooks; the rest is WaterCapture.cpp (the engine side) and WaterDraws.cpp (the graph side).

#include "Features/DrawcallLimitFix/Features/Water/Water.h"

#include "RenderGraph/Switches.h"

#include <algorithm>
#include <array>
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
			logger::info("[DCLF] water: on (CS_ORG_WATER=1): the engine's water batches are drawn at the EngineDraws anchor (mask {}, surface {})",
				TakesView(View::Mask) ? "the graph's" : "the engine's", TakesView(View::Surface) ? "the graph's" : "the engine's");
		}

		std::string Status()
		{
			return WaterDraws::Status();
		}
	}

	bool TakesView(View a_view)
	{
		static const std::array<bool, static_cast<std::size_t>(View::Count)> taken = [] {
			const auto parts = Switches::List("CS_ORG_WATER_PARTS");
			if (parts.empty())
				return std::array<bool, static_cast<std::size_t>(View::Count)>{ true, true };
			const auto named = [&](const char* a_part) { return std::ranges::find(parts, a_part) != parts.end(); };
			return std::array<bool, static_cast<std::size_t>(View::Count)>{ named("mask"), named("surface") };
		}();
		return a_view < View::Count && taken[static_cast<std::size_t>(a_view)];
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
