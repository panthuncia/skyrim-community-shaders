#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/ShaderBindings.h"

#	include <RenderPasses/Base/TypedRenderGraphPass.h>

#	include <memory>

namespace org
{
	class RecordingContext;
	class RenderGraph;
	class RenderPass;
}

/**
 * @brief CS's shared constant buffers for render-graph passes: the engine's FrameBuffer (PerFrame, b12), and CS's SharedData (b5),
 * FeatureData (b6) and Permutation (b4), as graph constant buffers with the bytes the D3D11 passes see at the same point of the frame.
 *
 * Each is a small device buffer with a CBV. The anchor's epoch writes the values into a LatchBlock region at its hook (Commit), and
 * its first pass copies them into the four buffers, so a recording kept across frames still reads this frame's values. The sources
 * are the ones D3D11 uploads from: FrameBuffer is CS's copy of the engine's mapping (globals::game::frameBufferCached), the others
 * are what State last uploaded (lastSharedData, lastFeatureData, permutationDataPrevious).
 *
 * Use: a frame anchor client sets ClientDesc::sharedConstants (FrameAnchors.h); FrameAnchors then adds the upload to the anchor's
 * epoch. The client's passes declare the buffers with Declare and push the four handles (Index) as the shader's SHARED entry
 * (package/Shaders/Common/OrgResources.hlsli, with ORG_SHARED_CONSTANTS). They are not the same thing as a LATCH entry: those are a
 * pass's own per-frame values, loaded from the latch as packed data, whereas these keep cbuffer packing (ConstantBuffer<T>).
 *
 * Render thread, but Declare and Index (graph build and record).
 */
namespace SharedConstants
{
	/** @brief The four handles of a SHARED entry, in its order. */
	struct Handles
	{
		ShaderBindings::Handle frameBuffer = 0, sharedData = 0, featureData = 0, permutation = 0;
	};

	/** @brief A pass's declared reads of the four buffers. */
	struct Bindings
	{
		org::DeclaredViewToken frameBuffer, sharedData, featureData, permutation;
	};

	/** @brief Graph build: declares the four buffers as constant buffers the pass reads. Call only with Ready() true. */
	Bindings Declare(org::PassBuilder& a_builder);

	/** @brief Record time: the handles of a pass's declared buffers. */
	Handles Index(const org::RecordingContext& a_recording, const Bindings& a_bindings);

	/**
	 * @brief Creates the buffers and the latch, once. False, logged, when the runtime is not active or CS has not uploaded the
	 * buffers yet (FeatureData's size is not known).
	 */
	bool Ensure();

	/** @brief Whether Ensure succeeded. */
	bool Ready();

	/** @brief Whether Ensure failed for good (the buffers could not be created): clients waiting for them never get them. */
	bool Failed();

	// What FrameAnchors asks of them.

	/** @brief Registers the four buffers with an anchor's graph ("cs.shared.frame-buffer" and so on). */
	void Register(org::RenderGraph& a_graph);

	/** @brief A new upload pass: copies the execution's latch region into the four buffers. */
	std::shared_ptr<org::RenderPass> MakeUploadPass();

	/** @brief Inside an epoch (beforePrepare): this execution's values into its latch region. */
	void Commit();

	/**
	 * @brief After an epoch that committed, on the render thread: CS_ORG_SHARED_CONSTANTS_CHECK=1 reads the D3D11 buffers back and
	 * compares them with what Commit wrote, for the first frames and on every mismatch. A debug switch: it stalls on the readback.
	 */
	void CheckAfterRun();
}

#endif
