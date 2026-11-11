#pragma once

#include <d3d11.h>

namespace RenderGraph::Engine::Skeleton
{
	/** @brief Install's: the engine call sites are patched (Setup turns the skeleton on only then). */
	void MarkInstalled();
}

// What the engine call sites (SkeletonHooks.cpp) ask of the skeleton (Skeleton.cpp). Main thread; the engine's context in each.
namespace RenderGraph::Engine::Skeleton::Sites
{
	/**
	 * @brief A colour clear the engine issues: true when the skeleton takes it (the graph clears it at the next Flush, or the
	 * engine's call is replayed there), false when the caller must make the D3D11 call now.
	 */
	bool TakeColourClear(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_view, const FLOAT a_colour[4]);

	/** @brief A depth-stencil clear, as TakeColourClear. */
	bool TakeDepthClear(ID3D11DeviceContext* a_context, ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil);

	/** @brief The clears taken so far run on the graph; any whose pass did not run get the engine's D3D11 call, in order. */
	void Flush(ID3D11DeviceContext* a_context);

	/** @brief CopyResource(kPOST_ZPREPASS_COPY, kMAIN) on the graph: true when it ran, false when the caller must copy. */
	bool TakeDepthCopy(ID3D11Resource* a_destination, ID3D11Resource* a_source);
}
