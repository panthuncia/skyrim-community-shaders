#include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/PostChain.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/Switches.h"

// The render owner's claim point and the loading screen's log. SE 1.5.97 and AE 1.6.1170 are the same functions.
namespace RenderGraph::Engine::LoadingScreen
{
	namespace
	{
		/**
		* @brief Renderer::Begin (SE 75460, AE 77245), on whichever thread renders the frame, inside Renderer::Lock.
		* With CS_ORG_LOADING_THREAD=1 that thread owns rendering until another one begins a frame. The frame driver describes the frame after the claim.
		*/
		struct RendererBegin
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_flags)
			{
				{
					ZoneScopedN("CS.Frame.RendererBegin");
					if (Requested())
						FrameAnchors::ClaimRenderOwnership();
					Frame::Driver::OnBegin();
				}
				func(a_renderer, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

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
				if (!Requested())
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

	bool Requested()
	{
		static const bool requested = !REL::Module::IsVR() && RenderGraph::Engine::Native::GraphMode();
		return requested;
	}

	void Install()
	{
		// Hooked for the frame driver in every mode. The render-owner rule acts only when requested.
		if (Requested())
			logger::info("[ORG] render ownership: the CS_ORG_LOADING_THREAD rule is on; Renderer::Begin claims rendering for its thread");
		stl::detour_thunk<RendererBegin>(REL::RelocationID(75460, 77245));
		stl::detour_thunk<SetLoadingScreenActive>(REL::RelocationID(34552, 35407));
		Frame::Driver::Install();
		Frame::PostChain::Install();
	}
}
