#pragma once

#include "RenderGraph/FrameAnchors.h"

/**
 * @brief The frame anchors' self-test (CS_ORG_ANCHOR_SELFTEST=1). At each anchor a raster pass writes a texture,
 * a compute pass reads it into a buffer and a copy reads that back. Each anchor logs once. Render thread.
 */
namespace FrameAnchors::SelfTest
{
	bool Requested();

	/** @brief Reserves every anchor before the render graph initializes, when requested. */
	void Reserve();

	/** @brief Makes the test's resources and registers its client once, at a frame's end with the runtime active. */
	void Setup();
}
