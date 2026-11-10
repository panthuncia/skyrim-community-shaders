#pragma once

#include "RenderGraph/FrameAnchors.h"

/**
 * @brief The frame anchors' self-test (CS_ORG_ANCHOR_SELFTEST=1): every anchor gets an epoch, and at each one a raster pass writes a
 * texture, a compute pass reads it (and one texel of the engine's kMAIN) into a buffer, and a copy pass reads that back. Each anchor
 * logs once when its value came back. Render thread, from FrameAnchors.
 */
namespace FrameAnchors::SelfTest
{
	/** @brief Whether CS_ORG_ANCHOR_SELFTEST=1 is set. */
	bool Requested();

	/** @brief Before the render graph initializes: when requested, reserves every anchor. */
	void Reserve();

	/**
	 * @brief At a frame's end with the runtime active (RenderGraphRuntime::SignalFramePace), once: when requested, makes the test's programs and resources and registers its client, an
	 * ordinary one. Its executed callback checks what each anchor's last completed run read back, and logs the first result.
	 */
	void Setup();
}
