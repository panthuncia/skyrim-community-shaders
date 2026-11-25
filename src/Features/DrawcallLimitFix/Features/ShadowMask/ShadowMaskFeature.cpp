// Shadow mask: the engine's shadow mask passes drawn by the render graph in graph mode.
// The engine side is ShadowMaskHooks.cpp and ShadowMaskNative.cpp, the graph side ShadowMaskDraws.cpp.

#include "Features/DrawcallLimitFix/Features/ShadowMask/ShadowMask.h"
#include "RenderGraph/Engine/Native/Native.h"


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
			InstallHooks();
			logger::info("[DCLF] shadow mask: on (graph mode): the shadow masks are drawn at the EngineDraws anchor (world and menu scenes)");
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

	bool Enabled()
	{
		static const bool enabled = ::RenderGraph::Engine::GraphMode();
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "shadow mask", .install = &Install, .preflight = &PreflightHooks, .status = &Status };
	}
}
