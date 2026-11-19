// MenuViews: the menu scene renders as DCLF views (CS_ORG_MENU3D, CS_ORG_MAP, default off). The rest is MenuViewsProbe.cpp.

#include "Features/DrawcallLimitFix/Features/MenuViews/MenuViews.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "Features/DrawcallLimitFix/Features/MenuViews/MenuScenes.h"

#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"

#include "RenderGraph/Switches.h"

#include <string>

namespace DCLF::Features::MenuViews
{
	namespace
	{
		void Lines(std::string& a_text)
		{
			a_text += Report();
		}

		/**
		 * @brief UI3DSceneManager::Render (SE 51855, AE 52727). Its registrations are no world view's, so a load's hold leaves them to the engine.
		 */
		struct Menu3DRender
		{
			static DWORD thunk(RE::UI3DSceneManager* a_manager, int a_scheme, RE::NiCamera* a_camera, bool a_arg4)
			{
				PassCapture::MenuSceneScope menuScene;
				return func(a_manager, a_scheme, a_camera, a_arg4);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief LocalMapRenderer::Render (SE 16094, AE 16335): the local map's land, objects and fog of war. */
		struct LocalMapRender
		{
			static void thunk(void* a_renderer)
			{
				PassCapture::MenuSceneScope menuScene;
				func(a_renderer);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The menu scenes' scopes for a load's hold, whatever CS_ORG_MENU3D and CS_ORG_MAP say. */
		void InstallSceneHooks()
		{
			stl::detour_thunk<Menu3DRender>(REL::RelocationID(51855, 52727));
			stl::detour_thunk<LocalMapRender>(REL::RelocationID(16094, 16335));
			if (!Menu3DRender::func.address() || !LocalMapRender::func.address())
				logger::warn(
					"[DCLF] menu views: a menu scene detour did not attach (menu 3D {}, local map {}); a load's hold withholds that scene's "
					"world-class passes, and the flush latch skips its draws",
					Menu3DRender::func.address() != 0, LocalMapRender::func.address() != 0);
		}

		void Install(Api::InstallContext& a_context)
		{
			InstallSceneHooks();
			// Before the render graph initializes, since the anchors' epochs are fixed then.
			if (Scenes::AnyEnabled())
				Scenes::SceneDraws::Reserve();
			if (!Menu3DEnabled() && !MapEnabled()) {
				Scenes::InstallHooks();
				return;
			}
			InstallHooks();
			// After the probe's, since the family's thunks chain onto the probe's at the same calls.
			Scenes::InstallHooks();
			a_context.RegisterReportLines({ .name = "menu views", .point = Api::ReportPoint::AsyncWork, .text = &Lines });
			logger::info("[DCLF] menu views: probe on (CS_ORG_MENU3D={}, CS_ORG_MAP={}); the switched-on scenes' batches are the menu scenes family's", Menu3DEnabled() ? 1 : 0,
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
			return std::string("menu scenes: ") + Scenes::SceneDraws::Status();
		}
	}

	bool Menu3DEnabled()
	{
		static const bool enabled = ::RenderGraph::Engine::Native::GraphMode();
		return enabled;
	}

	bool MapEnabled()
	{
		static const bool enabled = ::RenderGraph::Engine::Native::GraphMode();
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "menu views", .install = &Install, .preflight = &Preflight, .status = &Status };
	}
}
