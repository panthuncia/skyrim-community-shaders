#pragma once

#include <cstdint>

namespace RenderGraph::Engine
{
	/**
	 * @brief The seam at the ImageSpace HDR tonemap (Hooks.cpp, Main_HDRTonemapBlendCinematic_Render), before Effects11 and Post
	 * Processing replace or follow the vanilla pass: the ORG FX runtime settles its passes for the frame, Post Processing decides
	 * whether its chain runs on the graph, then the Tonemap anchor runs its epoch. Render thread.
	 * @param a_input The tonemap's input (RE::RENDER_TARGET).
	 * @param a_output Its output (RE::RENDER_TARGET).
	 * @return Whether the Tonemap anchor submitted an epoch.
	 */
	bool RunTonemapAnchor(std::uint32_t a_input, std::uint32_t a_output);
}
