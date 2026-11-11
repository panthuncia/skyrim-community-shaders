// MenuViews: the menu scene renders as DCLF views (CS_ORG_MENU3D=1, CS_ORG_MAP=1, default off). The feature installs the probe's hooks
// and its report lines; the rest is MenuViewsProbe.cpp.

#include "Features/DrawcallLimitFix/Features/MenuViews/MenuViews.h"

#include "RenderGraph/Engine/Flush/Flush.h"
#include "RenderGraph/Switches.h"

#include <string>

namespace DCLF::Features::MenuViews
{
	namespace
	{
		namespace Flush = RenderGraph::Engine::Flush;

		void Lines(std::string& a_text)
		{
			a_text += Report();
		}

		/**
		 * @brief UI3DSceneManager::Render (SE 51855, AE 52727): a menu's model (the inventory's, the loading screen's on its serving thread),
		 * its depth, shadow maps, shadow masks and colour. No view takes it yet (menus.md, stage 2), so under the flush latch every draw in
		 * it is the engine's on purpose (p5/flush.md, "Allowed engine draws"). Installed with the latch only.
		 */
		struct Menu3DRender
		{
			static DWORD thunk(RE::UI3DSceneManager* a_manager, int a_scheme, RE::NiCamera* a_camera, bool a_arg4)
			{
				static const Flush::Reason reason{ "menu views: menu 3D left to the engine" };
				Flush::EngineDraws scope(reason);
				return func(a_manager, a_scheme, a_camera, a_arg4);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief LocalMapRenderer::Render (SE 16094, AE 16335): the local map's land, objects and water, the engine's as above. */
		struct LocalMapRender
		{
			static void thunk(void* a_renderer)
			{
				static const Flush::Reason reason{ "menu views: local map left to the engine" };
				Flush::EngineDraws scope(reason);
				func(a_renderer);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The menu scenes vouched for under the flush latch, whatever CS_ORG_MENU3D and CS_ORG_MAP say. */
		void InstallLatchHooks()
		{
			if (!Flush::LatchRequested())
				return;
			stl::detour_thunk<Menu3DRender>(REL::RelocationID(51855, 52727));
			stl::detour_thunk<LocalMapRender>(REL::RelocationID(16094, 16335));
			if (!Menu3DRender::func.address() || !LocalMapRender::func.address())
				logger::warn("[DCLF] menu views: a menu scene detour did not attach (menu 3D {}, local map {}); the flush latch skips that scene's draws",
					Menu3DRender::func.address() != 0, LocalMapRender::func.address() != 0);
			else
				logger::info("[DCLF] menu views: the menu scenes are the engine's under the flush latch (UI3DSceneManager::Render, LocalMapRenderer::Render)");
		}

		void Install(Api::InstallContext& a_context)
		{
			InstallLatchHooks();
			if (!Menu3DEnabled() && !MapEnabled())
				return;
			InstallHooks();
			a_context.RegisterReportLines({ .name = "menu views", .point = Api::ReportPoint::AsyncWork, .text = &Lines });
			logger::info("[DCLF] menu views: probe on (CS_ORG_MENU3D={}, CS_ORG_MAP={}); the engine still draws every menu scene", Menu3DEnabled() ? 1 : 0,
				MapEnabled() ? 1 : 0);
		}
	}

	bool Menu3DEnabled()
	{
		static const bool enabled = Switches::On("CS_ORG_MENU3D");
		return enabled;
	}

	bool MapEnabled()
	{
		static const bool enabled = Switches::On("CS_ORG_MAP");
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "menu views", .install = &Install, .preflight = &PreflightHooks };
	}
}
