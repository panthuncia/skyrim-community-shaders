// Main::RenderDepth and Main::RenderShadowMasks in native, driven by CS. See RenderDepth.h.
// RenderDepth's order (AE 1.6.1170 0x1414ccb30, SE 1.5.97 0x1412e3520): the main depth bound with a clear and the colour targets unbound,
// StartAccumulating of the depth accumulator (its pre-resolve, AE 106592), ClearItemsList and RenderFirstPersonView_End (DCLF's Z-prepass
// point), the focus targets' stencil marks (one StartAccumulating each), the first-person block (cull, camera and node shift, its draw),
// then the copy into kPOST_ZPREPASS_COPY. RenderShadowMasks (AE 0x1414cd600, SE 0x1412e3ac0): the clear colour, SetCameraData and the
// current camera, the world's masks when the scene's shadows are on, the clear colours restored.

#include "RenderGraph/Engine/Native/RenderDepth.h"

#include "Features/DrawcallLimitFix.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"
#include "Features/DrawcallLimitFix/Features/ShadowMask/ShadowMask.h"
#include "RenderGraph/Engine/Native/WorldPass.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "Snapshot/EngineReads.h"
#include "Utils/Logging.h"

#include "Globals.h"
#include "State.h"

#include <cstdint>

namespace RenderGraph::Engine::Native
{
	namespace
	{
		template <class T>
		T& DepthGlobal(std::uint64_t a_se, std::uint64_t a_ae)
		{
			return *reinterpret_cast<T*>(REL::RelocationID(a_se, a_ae).address());
		}

		using RE::BSGraphics::ShaderFlags;

		/**
		* @brief The shadow state RenderDepth leaves for the engine code after it, written as its setters would (observed over every native
		* world render of the third- and first-person runs; its first-person camera is set back by RenderShadowMasks): the main depth
		* bound and cleared, no colour or cube target, the viewport of the main depth at the frame's ratios, the depth mode 3, no stencil
		* (reference 255), back-face culling and the write mode 1, the changed state marked dirty.
		*/
		void LeaveDepthState()
		{
			auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			for (std::uint32_t i = 0; i < 3; ++i) {
				shadow.renderTargets[i] = RE::RENDER_TARGET::kNONE;
				shadow.setRenderTargetMode[i] = RE::BSGraphics::SetRenderTargetMode::SRTM_RESTORE;
			}
			shadow.cubeMapRenderTarget = static_cast<std::uint32_t>(RE::RENDER_TARGETS_CUBEMAP::kNONE);
			shadow.depthStencil = RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN;
			shadow.setDepthStencilMode = RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
			const auto viewport = DCLF::Family::FrameViewport(-1, static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN), 1);
			shadow.viewPort.TopLeftX = static_cast<float>(viewport.rect[0]);
			shadow.viewPort.TopLeftY = static_cast<float>(viewport.rect[1]);
			shadow.viewPort.Width = static_cast<float>(viewport.rect[2]);
			shadow.viewPort.Height = static_cast<float>(viewport.rect[3]);
			shadow.depthStencilDepthMode = RE::BSGraphics::DepthStencilDepthMode::kTestWrite;
			shadow.depthStencilStencilMode = 0;
			shadow.stencilRef = 255;
			shadow.rasterStateCullMode = 1;
			shadow.alphaBlendWriteMode = 1;
			shadow.stateUpdateFlags.set(ShaderFlags::DIRTY_RENDERTARGET, ShaderFlags::DIRTY_VIEWPORT, ShaderFlags::DIRTY_DEPTH_MODE,
				ShaderFlags::DIRTY_DEPTH_STENCILREF_MODE, ShaderFlags::DIRTY_RASTER_CULL_MODE, ShaderFlags::DIRTY_ALPHA_BLEND);
		}

		/**
		* @brief What RenderShadowMasks leaves when the scene's shadows are on (observed likewise): kSHADOW_MASK bound as target 0 and
		* cleared, no depth target, the write mode 1, and the world camera current (the engine's global the masks' SetCameraData pairs with).
		*/
		void LeaveMaskState()
		{
			auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			shadow.renderTargets[0] = RE::RENDER_TARGET::kSHADOW_MASK;
			shadow.setRenderTargetMode[0] = RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
			shadow.depthStencil = static_cast<std::uint32_t>(-1);
			shadow.setDepthStencilMode = RE::BSGraphics::SetRenderTargetMode::SRTM_RESTORE;
			shadow.alphaBlendWriteMode = 1;
			shadow.stateUpdateFlags.set(ShaderFlags::DIRTY_RENDERTARGET, ShaderFlags::DIRTY_ALPHA_BLEND);
		}

		/** @brief A clear or copy the skeleton did not take: nothing else makes it in native. Logged once per what. */
		void DepthUntaken(bool a_taken, const char* a_what)
		{
			if (a_taken)
				return;
			static Util::ReasonLog log("[ORG native] RenderDepth: the skeleton did not take the depth pass's clear or copy; it is not made");
			log.Report(a_what);
		}
	}

	void RenderDepth(bool a_firstPerson, bool a_controlDriven)
	{
		ZoneScopedN("CS.Native.RenderDepth");
		globals::state->BeginPerfEvent("Depth");
		const WorldPass::RenderDepthScope depthPass;
		// SetCurrentDepthStencilTarget(kMAIN, SRTM_CLEAR): the depth and stencil clear, which the Z-prepass epoch hosts.
		DepthUntaken(Skeleton::ClearDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL), "the main depth's clear");
		// StartAccumulating(world camera, depth accumulator, 0x21), ClearItemsList and RenderFirstPersonView_End are not called: nothing
		// registers into the depth accumulator in native, so its pre-resolve draws nothing. Then DCLF's point after the world's depth draws.
		globals::features::drawcallLimitFix.DepthPassDrawn();
		// The focus targets' stencil marks (one StartAccumulating per focus target, stencil reference index + 1) are not drawn: only the
		// masks' focus branch reads them, and neither the engine's draw under DXVK nor CS's mask program takes it.
		// The first-person block: its cull, camera and node shift and draw are not made (DCLF draws first person). Its draw point remains.
		if (a_firstPerson) {
			auto* player = CS_ENGINE_READ(Player, RecordTime, RE::PlayerCharacter::GetSingleton());
			if (auto* root = player ? player->Get3D(true) : nullptr; root && root->AsNode())
				DCLF::Features::FirstPerson::DepthDrawPoint(a_controlDriven);
		}
		// CopyResource(kPOST_ZPREPASS_COPY, the main depth).
		DepthUntaken(Skeleton::CopyDepthPassDepth(), "the depth copy");
		LeaveDepthState();
		globals::state->EndPerfEvent();
	}

	void RenderShadowMasks()
	{
		// The scene's shadows (SE 513204 / AE 390933), else a caster kept last frame (SE 528090 / AE 415035), runs the call's body.
		const bool sceneShadows = DepthGlobal<bool>(513204, 390933);
		if (!sceneShadows && DepthGlobal<std::int32_t>(528090, 415035) <= 0)
			return;
		ZoneScopedN("CS.Native.RenderShadowMasks");
		globals::state->BeginPerfEvent("Shadowmasks");
		// The masks draw only with the scene's shadows on. Renderer::SetClearColor(r, 0, 0, 0): r is 0 with the sun's shadows off
		// (SE 528095 / AE 415040), else 1.
		if (sceneShadows) {
			const float clear[4]{ DepthGlobal<bool>(528095, 415040) ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f };
			DCLF::Features::ShadowMask::DrawWorldMasks(clear);
			LeaveMaskState();
		}
		// The current camera RenderShadowMasks sets with its SetCameraData (SE 527667 / AE 414580): the world's.
		DepthGlobal<RE::NiCamera*>(527667, 414580) = CS_ENGINE_READ(Camera, RecordTime, RE::Main::WorldRootCamera());
		globals::state->EndPerfEvent();
	}
}
