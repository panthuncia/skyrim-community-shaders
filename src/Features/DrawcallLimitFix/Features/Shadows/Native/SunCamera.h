#pragma once

// A port of BSShadowDirectionalLight::UpdateCamera (vtable slot 0x10; AE 0x141512230, SE 0x1413251c0). It sets up the sun's
// shadow cascades for a view camera: the smoothed sun direction, the full-frustum camera and its culling planes, each cascade's
// split distances, camera, texel snapping, clip planes, viewport and accumulator. It writes the same fields the engine's function
// writes, in place, and returns what it returns.

#include "Features/DrawcallLimitFix/Features/Shadows/Native/WrittenRange.h"

#include <vector>

namespace RE
{
	class BSShadowDirectionalLight;
	class NiCamera;
}

namespace DCLF::Features::Shadows::Native
{
	/**
	 * @brief The port. Same contract as the engine's UpdateCamera(this, viewCamera): same inputs, same writes, same return value.
	 * @param a_light The sun's shadow light.
	 * @param a_viewCamera The camera the cascades cover.
	 * @return Always true, like the engine.
	 */
	bool UpdateSunCamera(RE::BSShadowDirectionalLight& a_light, const RE::NiCamera& a_viewCamera);

	/**
	 * @brief Every memory range UpdateSunCamera (and so the engine's UpdateCamera) may write for this light, for a parity harness that
	 * saves, runs one, restores and runs the other.
	 * @details The objects a first call creates (a cascade's camera or accumulator) are listed only once they exist, so list after
	 * one call has run. The function writes no engine global: it sets the thread's memory context id while it allocates and
	 * restores it after.
	 * @param a_light The sun's shadow light.
	 * @param a_out The ranges are appended here.
	 */
	void SunCameraWrites(RE::BSShadowDirectionalLight& a_light, std::vector<WrittenRange>& a_out);
}
