// Shadow mask: the engine's shadow mask passes drawn by the render graph (CS_ORG_SHADOW_MASK=1, default off). The feature reserves the
// EngineDraws anchor and installs the engine hooks; the rest is ShadowMaskCapture.cpp (the engine side) and ShadowMaskDraws.cpp (the
// graph side).

#include "Features/DrawcallLimitFix/Features/ShadowMask/ShadowMask.h"

#include "RenderGraph/Switches.h"

#include <algorithm>
#include <array>
#include <string>

namespace DCLF::Features::ShadowMask
{
	namespace
	{
		void Install(Api::InstallContext&)
		{
			if (!Enabled())
				return;
			// At PostPostLoad, before the render graph initializes: the anchors' epochs are fixed then.
			ShadowMaskDraws::Reserve();
			InstallHooks();
			logger::info("[DCLF] shadow mask: on (CS_ORG_SHADOW_MASK=1): the shadow masks are drawn at the EngineDraws anchor (world {}, menu scenes {})",
				TakesView(View::World) ? "the graph's" : "the engine's", TakesView(View::Menu) ? "the graph's" : "the engine's");
		}

		std::string Status()
		{
			return ShadowMaskDraws::Status();
		}
	}

	const char* NameOf(View a_view)
	{
		return a_view == View::Menu ? "menu" : "world";
	}

	bool TakesView(View a_view)
	{
		static const std::array<bool, static_cast<std::size_t>(View::Count)> taken = [] {
			const auto views = Switches::List("CS_ORG_SHADOW_MASK_VIEWS");
			if (views.empty())
				return std::array<bool, static_cast<std::size_t>(View::Count)>{ true, true };
			const auto named = [&](const char* a_view) { return std::ranges::find(views, a_view) != views.end(); };
			return std::array<bool, static_cast<std::size_t>(View::Count)>{ named("world"), named("menu") };
		}();
		return a_view < View::Count && taken[static_cast<std::size_t>(a_view)];
	}

	bool Enabled()
	{
		static const bool enabled = Switches::On("CS_ORG_SHADOW_MASK");
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "shadow mask", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
