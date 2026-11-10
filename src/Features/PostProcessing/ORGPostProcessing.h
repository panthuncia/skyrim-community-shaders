#pragma once

#include <memory>

/**
 * @brief Post Processing's tonemap chain on the render graph (CS_ORG_POST_PROCESSING=1; default 0: the D3D11 chain, unchanged).
 *
 * The chain runs at the Tonemap frame anchor, where PostProcessing::PreProcess runs it on D3D11: an input pass, one or more passes
 * per sub-effect, and an output pass into kMAIN and kMAIN_COPY. It runs on the graph only while every enabled sub-effect has an ORG
 * implementation; otherwise the whole chain stays on D3D11 that frame and the log names the sub-effects that keep it there. How
 * the chain is built and the rules for porting a sub-effect are in ORGPostEffect.h.
 *
 * On any failure (a program that does not build, a target that cannot be imported, a failed graph) the chain runs on D3D11.
 *
 * Render thread. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGPostProcessing
{
public:
	/** @brief Whether CS_ORG_POST_PROCESSING=1 is set. */
	static bool Requested();

	static ORGPostProcessing& Get();

	/** @brief Before the render graph initializes (PostProcessing::PostPostLoad): reserves the Tonemap anchor when requested. */
	static void Reserve();

	/** @brief After the render graph initializes (PostProcessing::SetupResources): the copy programs and the effects. */
	void Setup();

	/**
	 * @brief At the tonemap hook, before the anchor runs: decides whether the graph runs the chain this frame, builds it again when
	 * its shape changed, and leaves the frame's values for the epoch's commit.
	 * @param a_readsMainCopy Whether the scene is in kMAIN_COPY this frame (PostProcessing::ReadsMainCopy), else kMAIN.
	 */
	void BeforeTonemap(bool a_readsMainCopy);

	/** @brief After the anchor ran: whether the graph ran the chain this frame (then PreProcess draws nothing). */
	bool Ran() const;

	/**
	 * @brief Drops the cached programs and the chain's key (PostProcessing::ClearShaderCache): the next frame builds the chain again,
	 * and a chain that failed to build is tried again.
	 */
	void ClearShaderCache();

	~ORGPostProcessing();

private:
	ORGPostProcessing() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
