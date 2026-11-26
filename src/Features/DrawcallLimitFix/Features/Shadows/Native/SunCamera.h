#pragma once

// A port of BSShadowDirectionalLight::UpdateCamera (vtable slot 0x10; AE 0x141512230, SE 0x1413251c0). It sets up the sun's
// shadow cascades for a view camera: the smoothed sun direction, the full-frustum camera and its culling planes, each cascade's
// split distances, camera, texel snapping, clip planes, viewport and accumulator. It writes the same fields the engine's function
// writes, in place, and returns what it returns.



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
}
