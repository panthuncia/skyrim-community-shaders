#include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"

#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/Switches.h"

// The render owner's claim point and the loading screen's log (docs/development/p5/loading-screen.md). SE 1.5.97 and AE 1.6.1170
// are the same functions.
namespace RenderGraph::Engine::LoadingScreen
{
	namespace
	{
		/**
		 * @brief Renderer::Begin (SE 75460 / 0x140d6a0c0, AE 77245): the start of every engine frame, inside Renderer::Lock, on the thread
		 * that renders it. Its callers (Ghidra, SE) are the main frame (0x1405b1020), the loading loop (ServingThread::RunLoadingScreen,
		 * 38085) and BSMoviePlayer::Tick when no frame is open (the startup logo, on the MoviePlayer thread). That thread owns rendering
		 * from here until another thread begins a frame.
		 */
		struct RendererBegin
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_flags)
			{
				FrameAnchors::ClaimRenderOwnership();
				func(a_renderer, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief ServingThread::SetLoadingScreenActive (SE 34552, AE 35407), called by the load coordinator (SE 13214, AE 13363) on
		 * the main thread. true starts the loading loop on the serving thread (mode 2) and returns. false stops it and returns only
		 * once the loop's last iteration ended. Logged only: the loop's frames claim rendering at their Begin, and the main thread's
		 * next frame claims it back the same way.
		 */
		struct SetLoadingScreenActive
		{
			static void thunk(bool a_active)
			{
				const std::uint64_t before = FrameAnchors::RenderOwnerChanges();
				func(a_active);
				if (a_active) {
					logger::info("[ORG] loading screen: the serving thread renders until the load ends");
					return;
				}
				static std::uint32_t logged = 0;
				if (logged < 16) {
					++logged;
					logger::info("[ORG] loading screen: the load ended ({} ownership changes since it started); the main thread's next frame takes it back",
						FrameAnchors::RenderOwnerChanges() - before);
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool Requested()
	{
		static const bool requested = !REL::Module::IsVR() && Switches::On("CS_ORG_LOADING_THREAD");
		return requested;
	}

	void Install()
	{
		if (!Requested())
			return;
		logger::info("[ORG] render ownership: the CS_ORG_LOADING_THREAD rule is on; hooking Renderer::Begin and ServingThread::SetLoadingScreenActive");
		stl::detour_thunk<RendererBegin>(REL::RelocationID(75460, 77245));
		stl::detour_thunk<SetLoadingScreenActive>(REL::RelocationID(34552, 35407));
	}
}
