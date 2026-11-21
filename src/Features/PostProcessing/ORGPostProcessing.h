#pragma once

#include "RenderGraph/Port.h"

#include <memory>

namespace RenderGraph::Engine
{
	struct TonemapTargets;
}

/**
* @brief Post Processing's tonemap chain on the render graph at the Tonemap anchor, in graph mode.
* Every sub-effect has an ORG port. D3D11 never runs the chain in graph mode: a frame without a build that holds it draws no
* chain, and an effect that cannot read its inputs is fatal. See ORGPostEffect.h.
* Render thread. Free of ORG/Vulkan includes.
*/
class ORGPostProcessing : public RenderGraph::Port<ORGPostProcessing>
{
public:
	static constexpr std::string_view kName = "Post Processing";

	/** @brief PostProcessing::SetupResources. Outside graph mode it does nothing; in graph mode a failure is fatal. */
	void Setup();

	/**
	* @brief Before the anchor runs. Decides whether the graph runs the chain, rebuilds when its shape changed, and leaves the values for the commit.
	* @param a_refraction Guest: refraction routed the scene through kMAIN_COPY although the input does not say so.
	*/
	void BeforeTonemap(const RenderGraph::Engine::TonemapTargets& a_targets, bool a_refraction);

	/** @brief After the anchor ran: the graph ran the chain this frame. In graph mode PreProcess draws nothing either way. */
	bool Ran() const;

	/** @brief A new shader generation: the next revision builds the chain's programs and client again. */
	void ClearShaderCache();
	~ORGPostProcessing();

private:
	friend class RenderGraph::Port<ORGPostProcessing>;
	ORGPostProcessing();

	struct Impl;
	std::unique_ptr<Impl> impl;
};
