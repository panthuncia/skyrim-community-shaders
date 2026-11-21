#pragma once

#include "RenderGraph/Engine/Targets/TargetRef.h"
#include "RenderGraph/FrameAnchors.h"

#include <d3d11.h>

#include <string>
#include <string_view>

/**
* @brief CS owns the engine's render targets (cleanup plan A1/B1, phase 3 design section 3).
*
* Each engine target is a declared texture on Vars::EngineTargetShapes() (and Vars::DisplaySize() for kFRAMEBUFFER). The engine's
* fields hold tripwire handles that live for the session. When the engine makes its set again (a window resize, a display
* change), the create thunks only record the new shapes. The revision lane makes the new versions, and at adoption the set points
* the field handles at them and writes the engine's size state. So a slot, a field handle and an Identify answer never change.
*
* Kinds of target:
* - backed: a declared, versioned texture. Its slot is published under its name ("Engine::kMAIN", "Engine::Depth::kMAIN").
* - unbacked: a handle with nothing behind it (Backing::None), in both modes. Any GPU use of one is fatal in development builds.
* - engine-owned: mip-generating targets and the mip views of them stay the engine's own D3D11 objects. Slot is empty for them.
* - mip views of a backed target: views of the source's version. They are named by the source (TargetRef::mip).
*
* Render thread unless a function says otherwise. Graph modes only (Engine::GraphMode); elsewhere every query is empty.
*/
namespace RenderGraph::Engine::Targets
{
	/**
	* @brief Once, inside Engine::Setup, after the first BSShaderRenderTargets::Create and before any feature reads a target.
	* Declares every target from the recorded shapes and the plans, makes the field handles over version 0, releases the engine's
	* first D3D11 objects, fills Identify's map, and writes EngineTargetShapes and DisplaySize. Render owner. Fatal on any failure.
	*/
	void DeclareSet();

	/** @brief Whether DeclareSet ran. Any thread. */
	bool SetDeclared();

	/**
	* @brief The declared texture's slot, stable for the session, so callers compare nothing. Empty for an engine-owned target.
	* Empty for an unbacked one, which is a recipe mismatch (Frame::Driver::NoteUnbackedUse, fatal in development builds).
	* A mip view target gives its source's slot. The view and slice of a_target are ignored.
	*/
	FrameAnchors::Texture Slot(TargetRef a_target);

	/** @brief Slot(Colour(kFRAMEBUFFER)): the scene back buffer, which present copies into the swap chain. For Engine::UiTargets. */
	FrameAnchors::Texture FramebufferSlot();

	/**
	* @brief kFRAMEBUFFER's field texture handle (the engine's renderTargets[kFRAMEBUFFER].texture). Stable for the session. The
	* guest present unwraps it (Tripwire::Unwrap) for the current version's D3D11 export. Null before DeclareSet.
	*/
	ID3D11Texture2D* FramebufferTexture();

	/** @brief The format kFRAMEBUFFER is declared in: the swap chain's, without sRGB. UNKNOWN before DeclareSet. Any thread. */
	DXGI_FORMAT FramebufferFormat();

	/**
	* @brief The adopted description of a 2D target (colour, copy, depth, cubemap), for keys and pipeline setup. Reads what the
	* field handles point at now, so it changes only at adoption. Zeroed for an unknown target.
	*/
	D3D11_TEXTURE2D_DESC Desc(TargetRef a_target);

	/** @brief The adopted shape of any target, a volume included. Zeroed for an unknown target. */
	Shape AdoptedShape(TargetRef a_target);

	/**
	* @brief The format of the field view a_target.view names: an RTV, DSV or SRV format, or the texture's format for View::Texture.
	* UNKNOWN for an unknown target.
	*/
	DXGI_FORMAT ViewFormat(TargetRef a_target);

	/** @brief Whether the target has nothing behind it this session (Backing::None). Any thread after DeclareSet. */
	bool Unbacked(TargetRef a_target);

	/** @brief Why an unbacked target has no memory, "" for a backed one. Any thread after DeclareSet. */
	std::string_view UnbackedReason(TargetRef a_target);

	/** @brief Whether the target stays the engine's own D3D11 object (a mip-generating target or a mip view of one). Any thread. */
	bool EngineOwned(TargetRef a_target);

	/** @brief The declaration's name: "Engine::kMAIN", "Engine::kMAIN Copy", "Engine::Depth::kMAIN", "Engine::Cubemap::kREFLECTIONS". */
	std::string Name(TargetRef a_target);
}
