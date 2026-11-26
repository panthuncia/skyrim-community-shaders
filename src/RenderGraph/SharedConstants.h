#pragma once

#include "RenderGraph/Frame/SegmentId.h"
#include "RenderGraph/ShaderBindings.h"

#include <RenderPasses/Base/TypedRenderGraphPass.h>

#include <memory>

namespace org
{
	class RecordingContext;
	class RenderGraph;
	class RenderPass;
}

/**
 * @brief CS's shared constant buffers for graph passes, with the bytes D3D11 passes see at the same point of the frame.
 *
 * Covers the engine's FrameBuffer (PerFrame, b12) and CS's SharedData (b5), FeatureData (b6) and Permutation (b4).
 * The anchor's epoch writes the values into a latch at its hook, and its first pass copies them into the buffers.
 * So a recording kept across frames still reads this frame's values. Unlike a LATCH entry these keep cbuffer packing.
 * A frame anchor client opts in with ClientDesc::sharedConstants. Render thread, except Declare and Index.
 */
namespace SharedConstants
{
	/** @brief The four handles of a SHARED entry, in its order. */
	struct Handles
	{
		ShaderBindings::Handle frameBuffer = 0, sharedData = 0, featureData = 0, permutation = 0;
	};

	struct Bindings
	{
		org::DeclaredViewToken frameBuffer, sharedData, featureData, permutation;
	};

	/** @brief Graph build. Call only with Ready() true. */
	Bindings Declare(org::PassBuilder& a_builder);

	/** @brief Record time. */
	Handles Index(const org::RecordingContext& a_recording, const Bindings& a_bindings);

	/**
	* @brief Setup, once (Engine::Setup), after the runtime initialized and CS sized FeatureData (State::SetupResources). Creates the
	* buffers and the latch. False and logged when they cannot be made; their clients then fail to register.
	*/
	bool Ensure();

	bool Ready();

	/** @brief Registers the four buffers with an anchor's graph. */
	void Register(org::RenderGraph& a_graph);

	/** @brief One upload point per step of a declared segment, each with its own latch block. A step past the last would read another's values. */
	inline constexpr std::uint32_t kUploadPoints = 21;

	/**
	 * @brief A pass that copies upload point a_point's latch region into the four buffers. Null for a point out of range.
	 * A declared segment has one before each step whose clients read the buffers, so each step reads the values its tap saw.
	 */
	std::shared_ptr<org::RenderPass> MakeUploadPass(std::uint32_t a_point = 0);

	/**
	* @brief Writes the values D3D11 would read now into upload point a_point. Native's frame camera stands in for the engine's
	* FrameBuffer in a segment that draws from the main camera. Inside an epoch or a held run.
	*/
	void Commit(RenderGraph::Frame::SegmentId a_segment, std::uint32_t a_point);


	/** @brief Debug check with CS_ORG_SHARED_CONSTANTS_CHECK=1. Reads the D3D11 buffers back and compares, stalling on the readback. */
	void CheckAfterRun();
}

