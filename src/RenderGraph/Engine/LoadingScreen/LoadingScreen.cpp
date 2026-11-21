#include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/Switches.h"

// The loading screen's log and the frame driver's loading flag. The render owner's claim point is Renderer::Begin's detour
// in Frame/FrameDriver.cpp. SE 1.5.97 and AE 1.6.1170 are the same functions.
namespace RenderGraph::Engine::LoadingScreen
{
	namespace
	{
		/**
		* @brief ServingThread::SetLoadingScreenActive (SE 34552, AE 35407), on the main thread. False returns only after the loop's last iteration.
		* Logged only, since the frames claim rendering at their own Begin.
		*/
		struct SetLoadingScreenActive
		{
			static void thunk(bool a_active)
			{
				const std::uint64_t before = FrameAnchors::RenderOwnerChanges();
				// The loop's frames count as a loading screen from before the loop starts until after it ends.
				if (a_active)
					Frame::Driver::OnLoadingScreen(true);
				func(a_active);
				if (!a_active)
					Frame::Driver::OnLoadingScreen(false);
				if (!GraphMode())
					return;
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

	void Install()
	{
		// Hooked for the frame driver in every mode.
		stl::detour_thunk<SetLoadingScreenActive>(REL::RelocationID(34552, 35407));
	}
}
