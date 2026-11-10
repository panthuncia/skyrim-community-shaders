// Physical Glare — animated tear-film phase applied to a cached aperture

#include "PostProcessing/PhysicalGlare/glare.hlsli"

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below (PhysicalGlare::GlareCB at b1). The ORG chain
// (ORG/ORGPhysicalGlare.cpp) gives heap handles in the push block and the constants from its latch.
#define TEAR_FILM_RESOURCES(X)                    \
	X(SRV, Texture2D<float2>, TexApertureBase, 0) \
	X(UAV, RWTexture2D<float2>, RWTexAperture, 0) \
	X(LATCH, GlareConstants, Glare, 1)
ORG_PUSH_BLOCK(TEAR_FILM_RESOURCES(ORG_HANDLE))
TEAR_FILM_RESOURCES(ORG_RESOURCE)

static const float PI = 3.14159265358979323846;

float HashToFloat(uint seed)
{
	seed = (seed ^ 61u) ^ (seed >> 16u);
	seed *= 9u;
	seed ^= seed >> 4u;
	seed *= 0x27d4eb2du;
	seed ^= seed >> 15u;
	return float(seed) / 4294967295.0;
}

[numthreads(8, 8, 1)] void CS_TearFilm(uint2 tid : SV_DispatchThreadID) {
	if (tid.x >= Glare.FFTResolution || tid.y >= Glare.FFTResolution)
		return;

	float2 center = float2(Glare.FFTResolution, Glare.FFTResolution) * 0.5;
	float aspect = Glare.ScreenWidth / max(Glare.ScreenHeight, 1.0);
	float radius = float(Glare.FFTResolution) * Glare.ApertureSize;
	float2 pos = float2(tid) + 0.5 - center;
	pos.y *= aspect;

	float r = length(pos);
	float theta = atan2(pos.y, pos.x);
	float edgeFactor = smoothstep(radius * 0.55, radius * 0.95, r);
	float phaseOffset = 0.0;
	uint complexity = min(Glare.TearFilmComplexity, 16u);
	for (uint h = 0; h < complexity; h++) {
		float amp = HashToFloat(h * 3u + 7000u) * 0.6 + 0.4;
		float angularFreq = float(h + 2);
		float timeSpeed = (HashToFloat(h * 3u + 7001u) * 1.6 + 0.2) * Glare.TearFilmSpeed;
		float timePhase = HashToFloat(h * 3u + 7002u) * 2.0 * PI;
		phaseOffset += amp * sin(angularFreq * theta + timeSpeed * Glare.TearFilmTime + timePhase);
	}
	phaseOffset *= Glare.TearFilmStrength * edgeFactor * 2.5;

	float s, c;
	sincos(phaseOffset, s, c);
	float2 value = TexApertureBase[tid];
	RWTexAperture[tid] = float2(value.x * c - value.y * s, value.x * s + value.y * c);
}
