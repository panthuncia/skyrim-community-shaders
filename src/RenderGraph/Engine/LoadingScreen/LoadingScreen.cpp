#include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"

#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/Switches.h"

// The loading screen's handoff (docs/development/p5/loading-screen.md). SE 1.5.97 and AE 1.6.1170 are the same function.
namespace RenderGraph::Engine::LoadingScreen
{
	namespace
	{
		/**
		 * @brief ServingThread::SetLoadingScreenActive (SE 34552, AE 35407), called by the load coordinator (SE 13214, AE 13363) on
		 * the main thread. true starts the loading loop on the serving thread (mode 2) and returns. false stops it and returns only
		 * once the loop's last iteration ended (it waits on the thread's done event, pumping window messages meanwhile).
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
				// The loop has stopped: no other thread renders until the main thread's frames resume. Taking ownership here means
				// the main thread's first frame after the load does not have to wait for its lock to claim it.
				FrameAnchors::TakeRenderOwnership();
				static std::uint32_t logged = 0;
				if (logged < 16) {
					++logged;
					logger::info("[ORG] loading screen: the load ended ({} ownership changes since it started); the main thread renders again",
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
		logger::info("[ORG] render ownership: the CS_ORG_LOADING_THREAD rule is on; hooking ServingThread::SetLoadingScreenActive");
		stl::detour_thunk<SetLoadingScreenActive>(REL::RelocationID(34552, 35407));
	}
}
