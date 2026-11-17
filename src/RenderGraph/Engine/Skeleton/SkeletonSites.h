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
	 * @brief A colour clear the engine issues: true when the skeleton takes it (the graph clears it at the next Flush), false when
	 * the caller must make the D3D11 call now.
	 */
	bool TakeColourClear(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_view, const FLOAT a_colour[4]);

	/** @brief A depth-stencil clear, as TakeColourClear. */
	bool TakeDepthClear(ID3D11DeviceContext* a_context, ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil);

	/** @brief The clears taken so far run on the graph. A run that does not draw them is an error (the frame driver's mismatch policy). */
	void Flush();

	/** @brief CopyResource(kPOST_ZPREPASS_COPY, kMAIN) on the graph: true when it ran, false when the caller must copy. */
	bool TakeDepthCopy(ID3D11Resource* a_destination, ID3D11Resource* a_source);

	// What native mode's immediate context asks of the skeleton (org-native-frame-graph.md, Phase 3): the engine's clears and copies
	// that reach the context outside the patched sites. Each runs on the graph now, at its place in the stream. False when the
	// skeleton does not take the operation: the context records it itself. Render owner; never inside an anchor's run
	// (FrameAnchors::InRun).

	/**
	 * @brief A colour clear of a view the skeleton has a pass for (the RE list, and in native mode the TAA mask and the water
	 * targets). True when the skeleton took it.
	 */
	bool TakeContextColourClear(ID3D11RenderTargetView* a_view, const FLOAT a_colour[4]);

	/** @brief A depth-stencil clear, as TakeContextColourClear. */
	bool TakeContextDepthClear(ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil);

	/**
	 * @brief A CopyResource the skeleton has a pass for: the main depth into kPOST_ZPREPASS_COPY, or (native mode) kMAIN into its copy
	 * texture at the EngineCopy anchor. True when the graph copied.
	 */
	bool TakeContextCopy(ID3D11Resource* a_destination, ID3D11Resource* a_source);
}
