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

	/** @brief Once, at the first anchor's run with the runtime active: makes the test's programs and resources and registers its client. */
	void Ensure();

	/** @brief After an anchor's epoch was submitted: checks what the anchor's last completed run read back, and logs the first result. */
	void AfterRun(Anchor a_anchor);
}
