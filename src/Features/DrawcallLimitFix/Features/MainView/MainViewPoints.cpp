// The main pass's points, called by Core's FrameDriver: the capture where the opaque batches start, and the colour epoch where they end.

#include "Features/DrawcallLimitFix/Features/MainView/MainView.h"
#include "Snapshot/EngineReads.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Globals.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/Tripwire/GuestDevice.h"
#include "RenderGraph/Fatal.h"

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace DCLF::Features::MainView
{
	namespace
	{
		Api::TriggerId opaqueAfter;

#if !defined(CS_RELEASE_PROFILE)
		TargetFormats BoundTargetFormats()
		{
			CS_ENGINE_READ_MARK(BoundState, BoundState);
			TargetFormats formats;
			if (RenderGraph::Engine::GraphMode() && !RenderGraph::Engine::Tripwire::GuestContextInstalled())
				RenderGraph::Fatal("DCLF main view", "the engine's targets were read before its context handle was in place");
			ID3D11RenderTargetView* views[8] = {};
			ID3D11DepthStencilView* depth = nullptr;
			// The engine's targets are in its context handle in graph mode.
			RenderGraph::Engine::Tripwire::EngineContext()->OMGetRenderTargets(8, views, &depth);
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

		std::string DescribeFormats(const TargetFormats& a_formats)
		{
			std::string text;
			for (std::uint32_t i = 0; i < a_formats.colorCount; ++i)
				text += fmt::format("{} ", static_cast<int>(a_formats.colors[i]));
			return text + fmt::format("depth {}", static_cast<int>(a_formats.depth));
		}

		/** @brief Development builds: the bound targets must be the declared ones DCLF's pipelines were built for at setup. */
		void CheckBoundFormats()
		{
			const auto bound = BoundTargetFormats();
			const auto& declared = DrawPipelines::Get().Targets();
			if (!(bound == declared))
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the main pass binds targets in formats {}, not the declared {} its pipelines were built for",
					DescribeFormats(bound), DescribeFormats(declared)));
		}
#endif

		/** @brief The main pass's bindings for this frame's colour epoch. Its formats were fixed at setup (DeclaredTargets). */
		bool CaptureMainPass()
		{
#if !defined(CS_RELEASE_PROFILE)
			CheckBoundFormats();
#endif
			IndirectDraws::Get().CaptureMainPass();
			// Read here because inside the deferred pass the blend table holds the deferred variants.
			DrawPipelines::Get().CaptureEngineStates();
			return true;
		}

		// The engine binds the G-buffer only when a draw applies its state, so FrameDriver applied the pending state first.
		bool BeforeOpaque()
		{
			return CaptureMainPass();
		}

		void AfterOpaque()
		{
			// DCLF's objects must be in the G-buffer and depth before Terrain Blending draws its held terrain. Drawn after it, a buried object
			// would be missing from what the terrain blended onto, and the terrain's depth writes would fail DCLF's EQUAL test.
			auto& draws = IndirectDraws::Get();
			globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);  // the epoch writes the G-buffer the context has bound
			Extensions::TriggerScope trigger(opaqueAfter);
			draws.ExecuteColour();  // the colour pass, against the depth written at the first draw
			// DCLF's epoch leaves bindings to the engine's state tracking, so the targets are rebound for the rest of the pass.
			CS_ENGINE_READ(Shaders, RecordTime, globals::game::stateUpdateFlags)->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		}
	}

	void RegisterPrimaryView(Api::InstallContext& a_context)
	{
		opaqueAfter = a_context.FindTrigger("opaque.after");
		a_context.RegisterPrimaryView({ .beforeOpaque = &BeforeOpaque, .afterOpaque = &AfterOpaque });
	}
}
