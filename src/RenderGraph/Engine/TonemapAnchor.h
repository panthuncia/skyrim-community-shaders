#pragma once

#include "RenderGraph/FrameAnchors.h"

#include <cstdint>

namespace RenderGraph::Engine
{
	/** @brief In native mode with CS's post chain these are CS's fixed images. Otherwise they are the engine's targets of the call. */
	struct TonemapTargets
	{
		FrameAnchors::Texture input;
		FrameAnchors::Texture output;
		rhi::Format inputFormat{};     // rhi::Format::Unknown when not known
		rhi::Format outputFormat{};
		bool chain = false;  // the input is also written by a client that changes the scene
	};

	/**
	* @brief The seam at the HDR tonemap, before Effects11 and Post Processing replace or follow it. Runs the Tonemap anchor. Render thread.
	* a_input and a_output are the engine's targets, used only when CS's chain does not choose them.
	* @return Whether the Tonemap anchor submitted an epoch.
	*/
	bool RunTonemapAnchor(std::uint32_t a_input, std::uint32_t a_output);
}
