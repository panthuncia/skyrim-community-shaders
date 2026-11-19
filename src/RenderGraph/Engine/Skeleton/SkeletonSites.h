#pragma once

#include <d3d11.h>

namespace RenderGraph::Engine::Skeleton
{
	/** @brief Setup turns the skeleton on only after the call sites are patched. */
	void MarkInstalled();
}

// What the engine call sites ask of the skeleton. Main thread, with the engine's context.
namespace RenderGraph::Engine::Skeleton::Sites
{
	/** @brief True when the skeleton takes the clear for the next Flush. False means the caller makes the D3D11 call now. */
	bool TakeColourClear(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_view, const FLOAT a_colour[4]);

	/** @brief As TakeColourClear. */
	bool TakeDepthClear(ID3D11DeviceContext* a_context, ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil);

	/** @brief A run that does not draw them is an error. */
	void Flush();

	/** @brief False means the caller must copy. */
	bool TakeDepthCopy(ID3D11Resource* a_destination, ID3D11Resource* a_source);

	// Native mode: clears and copies that reach the immediate context outside the patched sites. Each runs on the graph now.
	// False lets the context record it. Render owner, never inside an anchor's run.

	bool TakeContextColourClear(ID3D11RenderTargetView* a_view, const FLOAT a_colour[4]);

	/** @brief As TakeContextColourClear. */
	bool TakeContextDepthClear(ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil);

	/** @brief The main depth into kPOST_ZPREPASS_COPY, or in native mode kMAIN into its copy texture. */
	bool TakeContextCopy(ID3D11Resource* a_destination, ID3D11Resource* a_source);
}
