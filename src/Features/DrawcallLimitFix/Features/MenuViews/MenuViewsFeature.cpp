// MenuViews: the menu scene renders as DCLF views (on in graph mode).

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
		/**
		 * @brief UI3DSceneManager::Render (SE 51855, AE 52727). Its registrations are no world view's, so a load's hold leaves them to the engine.
		 */
		struct Menu3DRender
		{
			static DWORD thunk(RE::UI3DSceneManager* a_manager, int a_scheme, RE::NiCamera* a_camera, bool a_arg4)
			{
				// A scheme CS draws natively: the engine's function is not entered.
				if (a_manager) {
					const auto current = static_cast<std::int32_t>(a_manager->currentlightScheme);
					if (Scenes::Native::Owns(current) && (a_scheme == 0 || a_scheme == current))
						return Scenes::Native::Render(a_manager, a_scheme, a_camera, a_arg4);
				}
				PassCapture::MenuSceneScope menuScene;
				return func(a_manager, a_scheme, a_camera, a_arg4);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief LocalMapRenderer::Render (SE 16094, AE 16335): the local map's land, objects and fog of war. Native draws it from the scene
		 * (Scenes::Native::RenderLocalMap) and the engine's function is not entered; guest keeps the engine's render with its batches latched.
		 */
		struct LocalMapRender
		{
			static void thunk(void* a_renderer)
			{
				if (::RenderGraph::Engine::Native::Active()) {
					Scenes::Native::RenderLocalMap(a_renderer);
					return;
				}
				PassCapture::MenuSceneScope menuScene;
				func(a_renderer);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The menu scenes' scopes for a load's hold, whether or not the menu scenes are on. */
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

		void Install(Api::InstallContext&)
		{
			InstallSceneHooks();
			Scenes::InstallHooks();
		}
	}

	namespace
	{
		void Preflight()
		{
			Scenes::PreflightHooks();
		}

		std::string Status()
		{
			return std::string("menu scenes: ") + Scenes::SceneDraws::Status();
		}
	}

	bool Menu3DEnabled()
	{
		static const bool enabled = ::RenderGraph::Engine::GraphMode();
		return enabled;
	}

	bool MapEnabled()
	{
		static const bool enabled = ::RenderGraph::Engine::GraphMode();
		return enabled;
	}

	Api::Feature Feature()
	{
		return { .name = "menu views", .install = &Install, .preflight = &Preflight, .status = &Status };
	}
}
