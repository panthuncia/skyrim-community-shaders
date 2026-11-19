#pragma once

// Atmosphere and Fog's medium (Common/ViewAtmosphere.hlsli) as frame textures, in the graph modes.
// DCLF's own buffers hold the medium's D3D11 registers t125-t127, so its draws read it at t104, t107 and t108.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::ViewAtmosphere
{
	// t104: the integrated luminance atlas.
	inline constexpr std::uint32_t kLuminanceRegister = 104;
	// t107: the integrated transmittance atlas.
	inline constexpr std::uint32_t kTransmittanceRegister = 107;
	// t108: the parameters buffer.
	inline constexpr std::uint32_t kParametersRegister = 108;

	Api::Feature Integration();
}
