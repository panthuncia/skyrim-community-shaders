// Main::RenderDepth, Main::RenderShadowMasks and Main::RenderFirstPersonView in native, driven by CS. See RenderDepth.h.
// RenderDepth's order (AE 1.6.1170 0x1414ccb30, SE 1.5.97 0x1412e3520): the main depth bound with a clear and the colour targets unbound,
// StartAccumulating of the depth accumulator (its pre-resolve, AE 106592), ClearItemsList and RenderFirstPersonView_End (DCLF's Z-prepass
// point), the focus targets' stencil marks (one StartAccumulating each), the first-person block (cull, camera and node shift, its draw),
// then the copy into kPOST_ZPREPASS_COPY. RenderShadowMasks (AE 0x1414cd600, SE 0x1412e3ac0): the clear colour, SetCameraData and the
// current camera, the world's masks when the scene's shadows are on, the clear colours restored. RenderFirstPersonView (AE 0x1414cb6d0,
// SE 0x1412e21a0): the depth range and targets set, the first-person accumulator rendered (DCLF's colour point) and finished, the sun
// glare culled and drawn over first person, the sun's second occlusion query.

#include "RenderGraph/Engine/Native/RenderDepth.h"

#include "Features/DrawcallLimitFix.h"
#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"
#include "Features/DrawcallLimitFix/Features/ShadowMask/ShadowMask.h"
#include "RenderGraph/Engine/Native/EngineAccess.h"
#include "RenderGraph/Engine/Native/WorldPass.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "Snapshot/EngineReads.h"
#include "Utils/Logging.h"

#include "Globals.h"
#include "State.h"

#include <cstdint>

namespace RenderGraph::Engine::Native
{
	void RenderDepth(bool a_firstPerson, bool a_controlDriven)
	{
		ZoneScopedN("CS.Native.RenderDepth");
		globals::state->BeginPerfEvent("Depth");
		const WorldPass::RenderDepthScope depthPass;
		static Access::UntakenLog untaken("[ORG native] RenderDepth: the skeleton did not take the depth pass's clear or copy; it is not made");
		// SetCurrentDepthStencilTarget(kMAIN, SRTM_CLEAR): the depth and stencil clear, which the Z-prepass epoch hosts.
		untaken(Skeleton::ClearDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL), "the main depth's clear");
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
		untaken(Skeleton::CopyDepthPassDepth(), "the depth copy");
		// The shadow state RenderDepth leaves (the main depth bound, its viewport, no colour target, the depth and raster modes) is not
		// written: nothing after reads it in native (the masks, the precipitation and RenderWorld set their own or read CS's frame).
		globals::state->EndPerfEvent();
	}

	void RenderShadowMasks()
	{
		// The scene's shadows (SE 513204 / AE 390933), else a caster kept last frame (SE 528090 / AE 415035), runs the call's body.
		const bool sceneShadows = Access::Global<bool>(513204, 390933);
		if (!sceneShadows && Access::Global<std::int32_t>(528090, 415035) <= 0)
			return;
		ZoneScopedN("CS.Native.RenderShadowMasks");
		globals::state->BeginPerfEvent("Shadowmasks");
		// The masks draw only with the scene's shadows on. Renderer::SetClearColor(r, 0, 0, 0): r is 0 with the sun's shadows off
		// (SE 528095 / AE 415040), else 1.
		if (sceneShadows) {
			const float clear[4]{ Access::Global<bool>(528095, 415040) ? 0.0f : 1.0f, 0.0f, 0.0f, 0.0f };
			DCLF::Features::ShadowMask::DrawWorldMasks(clear);
		}
		// Not written: the shadow state the masks leave (kSHADOW_MASK bound, no depth target) and the current camera, which nothing
		// after reads in native. The game reads the current camera; the game side writes it (Snapshot/SideEffects.cpp).
		globals::state->EndPerfEvent();
	}

	void RenderFirstPersonView()
	{
		ZoneScopedN("CS.Native.RenderFirstPersonView");
		globals::state->BeginPerfEvent("First Person View");
		DCLF::Features::FirstPerson::ColourDrawPoint();
		globals::state->EndPerfEvent();
	}
}
