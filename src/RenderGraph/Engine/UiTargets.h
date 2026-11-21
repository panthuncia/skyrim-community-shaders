#pragma once

#include "RenderGraph/FrameAnchors.h"

#include <d3d11.h>

namespace RenderGraph::Frame
{
	class Inputs;
}

/**
* @brief Where the interface draws, in graph mode (cleanup plan B17, phase 3 design 3.7).
*
* Scaleform, Bink and the ImGui overlay draw into uiTarget by its slot. That is HDR Display's UI texture while HDR is on, which
* the HDR composite reads, and else the back buffer. The back buffer is kFRAMEBUFFER's slot, which the present copies into the
* swap chain. HDR Display no longer points kFRAMEBUFFER's fields at its UI texture in graph mode, so nobody undoes that swap.
*
* The slots are stable for the session, so the only variable here is HdrEnabled. A declaration that reads Current(a_in) depends
* on Vars::HdrEnabled(). Free of ORG and Vulkan includes.
*/
namespace RenderGraph::Engine::UiTargets
{
	/** @brief The interface's targets for one frame. */
	struct Targets
	{
		FrameAnchors::Texture backBuffer;  // kFRAMEBUFFER's slot
		FrameAnchors::Texture hdrUi;       // HDR Display's UI texture; empty without HDR Display
		FrameAnchors::Texture uiTarget;    // hdrUi while HDR is on, else backBuffer
		DXGI_FORMAT backBufferFormat = DXGI_FORMAT_UNKNOWN;  // the format of kFRAMEBUFFER's RTV
		DXGI_FORMAT uiFormat = DXGI_FORMAT_UNKNOWN;          // the format uiTarget is drawn in
		bool hdr = false;                                    // uiTarget is hdrUi
		bool operator==(const Targets&) const = default;
	};

	/** @brief From a revision's inputs, for a key or a description. The declaration must depend on Vars::HdrEnabled(). */
	Targets Current(const Frame::Inputs& a_in);

	/** @brief From the adopted revision. Render thread. */
	Targets Current();

	/** @brief Whether this frame's uiTarget is hdrUi, from the adopted revision. Cheap: for draw-time choices. Render thread. */
	bool DrawsIntoHdrUi();

	/**
	* @brief kFRAMEBUFFER's texture field once it holds a CS slot: a tripwire handle that follows the slot's adopted version.
	* Null before the engine target set took the back buffer, and outside graph mode. For D3D11 copies into the back buffer.
	*/
	ID3D11Texture2D* BackBufferTexture();
}
