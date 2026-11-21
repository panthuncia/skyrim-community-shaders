#include "RenderGraph/Engine/UiTargets.h"

#include "Features/HDRDisplay/ORGHDRDisplay.h"
#include "Globals.h"
#include "RenderGraph/Engine/Targets/EngineTargetSet.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Variables.h"

namespace RenderGraph::Engine::UiTargets
{
	namespace
	{
		/** @brief The targets, with HDR on or off. The slots and formats are the session's. */
		Targets Make(bool a_hdrEnabled)
		{
			Targets targets;
			// kFRAMEBUFFER is a declared slot of the engine target set, in the swap chain's format without sRGB.
			targets.backBuffer = Engine::Targets::FramebufferSlot();
			targets.backBufferFormat = Engine::Targets::FramebufferFormat();
			targets.hdrUi = ORGHDRDisplay::Get().Slot(ORGHDRDisplay::Texture::Ui);
			targets.hdr = a_hdrEnabled && static_cast<bool>(targets.hdrUi);
			targets.uiTarget = targets.hdr ? targets.hdrUi : targets.backBuffer;
			// HDR Display's UI texture is R8G8B8A8_UNORM, as its RTV (HDRDisplay::SetupResources).
			targets.uiFormat = targets.hdr ? DXGI_FORMAT_R8G8B8A8_UNORM : targets.backBufferFormat;
			return targets;
		}
	}

	Targets Current(const Frame::Inputs& a_in)
	{
		return Make(a_in.Get(Frame::Vars::HdrEnabled()));
	}

	Targets Current()
	{
		return Make(Frame::Vars::Adopted(Frame::Vars::HdrEnabled()));
	}

	bool DrawsIntoHdrUi()
	{
		return Frame::Vars::Adopted(Frame::Vars::HdrEnabled()) && static_cast<bool>(ORGHDRDisplay::Get().Slot(ORGHDRDisplay::Texture::Ui));
	}

	ID3D11Texture2D* BackBufferTexture()
	{
		// Null before DeclareSet, and outside graph mode.
		return Engine::Targets::FramebufferTexture();
	}
}

