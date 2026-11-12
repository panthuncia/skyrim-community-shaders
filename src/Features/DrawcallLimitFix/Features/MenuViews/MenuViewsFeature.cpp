// MenuViews: the menu scene renders as DCLF views (CS_ORG_MENU3D=1, CS_ORG_MAP=1, default off). The feature installs the probe's hooks
// and its report lines; the rest is MenuViewsProbe.cpp.

#include "Features/DrawcallLimitFix/Features/MenuViews/MenuViews.h"
#include "Features/DrawcallLimitFix/Features/MenuViews/MenuScenes.h"

#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"

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
		 * its depth, shadow maps, shadow masks and colour. Its registrations are no world view's, so a load's hold leaves them to the engine
		 * (PassCapture::MenuSceneScope: the loading menu's model under the hold). No view takes it yet (menus.md, stage 2), so under the flush
		 * latch every draw in it is the engine's on purpose (p5/flush.md, "Allowed engine draws").
		 */
		struct Menu3DRender
		{
			static DWORD thunk(RE::UI3DSceneManager* a_manager, int a_scheme, RE::NiCamera* a_camera, bool a_arg4)
			{
				static const Flush::Reason reason{ "menu views: menu 3D left to the engine" };
				PassCapture::MenuSceneScope menuScene;
				if (!Flush::LatchRequested())
					return func(a_manager, a_scheme, a_camera, a_arg4);
				Flush::EngineDraws scope(reason);
				return func(a_manager, a_scheme, a_camera, a_arg4);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief LocalMapRenderer::Render (SE 16094, AE 16335): the local map's land and objects and its fog of war. Its two batches are the
		 * menu scenes family's (MenuScenes.h: drawn by the graph with CS_ORG_MAP=1, the engine's and vouched for by the family otherwise);
		 * its ImageSpace effect (0x62, kLOCAL_MAP_SWAP into kLOCAL_MAP) is the ImageSpace port's.
		 */
		struct LocalMapRender
		{
			static void thunk(void* a_renderer)
			{
				PassCapture::MenuSceneScope menuScene;
				func(a_renderer);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The menu scenes' scopes: a load's hold's (always) and the flush latch's vouches, whatever CS_ORG_MENU3D and CS_ORG_MAP say. */
		void InstallSceneHooks()
		{
			stl::detour_thunk<Menu3DRender>(REL::RelocationID(51855, 52727));
			stl::detour_thunk<LocalMapRender>(REL::RelocationID(16094, 16335));
			if (!Menu3DRender::func.address() || !LocalMapRender::func.address())
				logger::warn(
					"[DCLF] menu views: a menu scene detour did not attach (menu 3D {}, local map {}); a load's hold withholds that scene's "
					"world-class passes, and the flush latch skips its draws",
					Menu3DRender::func.address() != 0, LocalMapRender::func.address() != 0);
			else if (Flush::LatchRequested())
				logger::info("[DCLF] menu views: the menu 3D models are the engine's under the flush latch (UI3DSceneManager::Render)");
		}

		void Install(Api::InstallContext& a_context)
		{
			InstallSceneHooks();
			// At PostPostLoad, before the render graph initializes: the anchors' epochs are fixed then.
			if (Scenes::Enabled())
				Scenes::SceneDraws::Reserve();
			if (!Menu3DEnabled() && !MapEnabled()) {
				Scenes::InstallHooks();
				return;
			}
			InstallHooks();
			// After the probe's: the family's thunks chain onto the probe's at the same calls.
			Scenes::InstallHooks();
			a_context.RegisterReportLines({ .name = "menu views", .point = Api::ReportPoint::AsyncWork, .text = &Lines });
			logger::info("[DCLF] menu views: probe on (CS_ORG_MENU3D={}, CS_ORG_MAP={}); the local map's batches are the menu scenes family's with CS_ORG_MAP", Menu3DEnabled() ? 1 : 0,
				MapEnabled() ? 1 : 0);
		}
	}

	namespace
	{
		void Preflight()
		{
			PreflightHooks();
			Scenes::PreflightHooks();
		}

		std::string Status()
		{
			return std::string("local map: ") + Scenes::SceneDraws::Status();
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
		return { .name = "menu views", .install = &Install, .preflight = &Preflight, .status = &Status };
	}
}
