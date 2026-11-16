#pragma once

#include "RenderGraph/FrameAnchors.h"

#include <cstdint>

namespace RenderGraph::Engine
{
	/**
	 * @brief The targets the Tonemap anchor's clients read and write, as graph textures. In native mode with CS's post chain these are
	 * CS's: the chain's image before the tonemap (kMAIN, where a client that changes the scene writes its result back) and the image
	 * after it (the final target). They do not change from frame to frame. Otherwise (guest) they are the engine's targets of its call.
	 */
	struct TonemapTargets
	{
		FrameAnchors::Texture input;   // the HDR image the tonemap reads
		FrameAnchors::Texture output;  // the image the tonemap writes
		rhi::Format inputFormat{};     // their view formats (rhi::Format::Unknown when not known)
		rhi::Format outputFormat{};
		bool chain = false;  // CS's native post chain chose them: the input is also the output of a client that changes the scene
	};

	/**
	 * @brief The seam at the ImageSpace HDR tonemap (Hooks.cpp, Main_HDRTonemapBlendCinematic_Render), before Effects11 and Post
	 * Processing replace or follow the vanilla pass: the ORG FX runtime settles its passes for the frame, Post Processing decides
	 * whether its chain runs on the graph, then the Tonemap anchor runs its epoch. Render thread.
	 * @param a_input The engine's tonemap input (RE::RENDER_TARGET), used when CS's chain does not choose the targets (guest).
	 * @param a_output The engine's tonemap output (RE::RENDER_TARGET), the same.
	 * @return Whether the Tonemap anchor submitted an epoch.
	 */
	bool RunTonemapAnchor(std::uint32_t a_input, std::uint32_t a_output);
}
