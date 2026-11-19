// The main pass's points, called by Core's FrameDriver: the capture where the opaque batches start, and the colour epoch where they end.

#include "Features/DrawcallLimitFix/Features/MainView/MainView.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Globals.h"

#include <d3d11.h>

#include <cstdint>

namespace DCLF::Features::MainView
{
	namespace
	{
		Api::TriggerId opaqueAfter;
		bool loggedCaptureFailure = false;

		TargetFormats CurrentTargetFormats()
		{
			TargetFormats formats;
			ID3D11RenderTargetView* views[8] = {};
			ID3D11DepthStencilView* depth = nullptr;
			globals::d3d::context->OMGetRenderTargets(8, views, &depth);
			for (std::uint32_t i = 0; i < 8; ++i) {
				if (!views[i])
					continue;
				D3D11_RENDER_TARGET_VIEW_DESC desc;
				views[i]->GetDesc(&desc);
				formats.colors[i] = desc.Format;
				formats.colorCount = i + 1;
				views[i]->Release();
			}
			if (depth) {
				D3D11_DEPTH_STENCIL_VIEW_DESC desc;
				depth->GetDesc(&desc);
				formats.depth = desc.Format;
				depth->Release();
			}
			return formats;
		}

		/** @brief The main pass's targets and bindings for this frame's colour epoch; false when its targets are not bound. */
		bool CaptureMainPass()
		{
			const auto formats = CurrentTargetFormats();
			if (formats.colorCount == 0 || formats.depth == DXGI_FORMAT_UNKNOWN)
				return false;
			DrawPipelines::Get().SetTargetFormats(formats);
			IndirectDraws::Get().CaptureMainPass();
			// Read here because inside the deferred pass the blend table holds the deferred variants.
			DrawPipelines::Get().CaptureEngineStates();
			return true;
		}

		// The engine binds the G-buffer only when a draw applies its state, so FrameDriver applied the pending state first.
		bool BeforeOpaque()
		{
			const bool captured = CaptureMainPass();
			if (!captured && !loggedCaptureFailure) {
				loggedCaptureFailure = true;
				logger::warn("[DCLF] the main pass's targets are not bound where its opaque batches start; the colour epoch is skipped");
			}
			return captured;
		}

		void AfterOpaque()
		{
			// DCLF's objects must be in the G-buffer and depth before Terrain Blending draws its held terrain. Drawn after it, a buried object
			// would be missing from what the terrain blended onto, and the terrain's depth writes would fail DCLF's EQUAL test.
			auto& draws = IndirectDraws::Get();
			globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);  // the epoch writes the G-buffer the context has bound
			draws.ProbeTargets("before colour");
			Extensions::TriggerScope trigger(opaqueAfter);
			draws.ExecuteColour();  // the colour pass, against the depth written at the first draw
			draws.ProbeTargets("after colour");
			// DCLF's epoch leaves bindings to the engine's state tracking, so the targets are rebound for the rest of the pass.
			globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		}
	}

	void RegisterPrimaryView(Api::InstallContext& a_context)
	{
		opaqueAfter = a_context.FindTrigger("opaque.after");
		a_context.RegisterPrimaryView({ .beforeOpaque = &BeforeOpaque, .afterOpaque = &AfterOpaque });
	}
}
