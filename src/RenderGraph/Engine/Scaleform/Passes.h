#pragma once

#include <cstdint>

namespace RenderGraph::Engine::Scaleform
{
	struct Display;
}

/**
 * @brief The render graph side of Scaleform's ORG path. It owns the Interface anchor's client, its surfaces and pipelines,
 * and the per-display commit. Render thread throughout. A pass's Record reads only the frame its run's commit stored.
 */
namespace RenderGraph::Engine::Scaleform::Passes
{
	/** @brief Makes the samplers, run gate, surfaces and pipelines, and declares the client. Graph mode only. A failure is fatal. */
	void Setup();

	/** @brief Whether displays are captured for the graph. */
	bool Ready();

	/** @brief The size of the scratch images for GFx's pushed render targets. A larger target cannot be drawn. */
	bool ScratchSize(std::uint32_t& a_width, std::uint32_t& a_height);

	std::uint32_t ScratchCount();

	/**
	 * @brief Draws a_display on the graph, one Interface anchor run per run gate's worth of commands. False when it did not,
	 * with a_reason. a_lagged means the refusal is schedule lag after a surface rebuild, not a mismatch. Render thread.
	 */
	bool Submit(Display& a_display, const char*& a_reason, bool& a_lagged);
}
