#ifndef __VOLUMETRIC_LIGHTING_REPARTITION_DEPENDENCY_HLSL__
#define __VOLUMETRIC_LIGHTING_REPARTITION_DEPENDENCY_HLSL__

// The volumetric lighting's depth repartition on ORG (Features/ORGVolumetricLighting). The engine keeps it in two 1D textures
// of 4096 texels that it rewrites on the CPU when the weather's range factor changes (AE 0x1414c6330): texel i holds
// pow(i / 4096, k) for the repartition, and pow(i / 4096, 1 / k) for its inverse. The D3D11 shaders sample them linearly with
// clamp. The ORG builds compute the same samples from the exponent, which the pass's latch holds, so no 1D texture is
// imported.

namespace VolumetricLightingRepartition
{
	static const float TexelCount = 4096.0;

	float Texel(float a_index, float a_exponent)
	{
		return pow(clamp(a_index, 0.0, TexelCount - 1.0) / TexelCount, a_exponent);
	}

	/** The texture's linear, clamped sample at a_u, with texels pow(i / 4096, a_exponent). */
	float Sample(float a_u, float a_exponent)
	{
		const float t = a_u * TexelCount - 0.5;
		const float first = floor(t);
		return lerp(Texel(first, a_exponent), Texel(first + 1.0, a_exponent), t - first);
	}
}

#endif  // __VOLUMETRIC_LIGHTING_REPARTITION_DEPENDENCY_HLSL__
