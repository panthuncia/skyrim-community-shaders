#pragma once

#include <memory>

namespace RenderGraph::Engine
{
	struct TonemapTargets;
}

/**
* @brief Post Processing's tonemap chain on the render graph at the Tonemap anchor, with CS_ORG_POST_PROCESSING=1.
* It runs on the graph only while every enabled sub-effect has an ORG port, otherwise on D3D11 that frame. See ORGPostEffect.h.
* Render thread. Free of ORG/Vulkan includes.
*/
class ORGPostProcessing
{
public:
	static bool Requested();

	/** @brief Render thread. */
	static ORGPostProcessing& Get();

	/** @brief PostProcessing::PostPostLoad, before the render graph initializes. */
	static void Reserve();

	/** @brief PostProcessing::SetupResources. */
	void Setup();

	/**
	* @brief Before the anchor runs. Decides whether the graph runs the chain, rebuilds when its shape changed, and leaves the values for the commit.
	* @param a_refraction Guest: refraction routed the scene through kMAIN_COPY although the input does not say so.
	*/
	void BeforeTonemap(const RenderGraph::Engine::TonemapTargets& a_targets, bool a_refraction);

	/** @brief After the anchor ran. When true PreProcess draws nothing. */
	bool Ran() const;

	/** @brief The next frame rebuilds the chain, and a failed one is tried again. */
	void ClearShaderCache();

	~ORGPostProcessing();

private:
	ORGPostProcessing() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
