#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/OrgResources.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

// D3D11: t0, u0, s0. ORG: three handles in the push block (Common/OrgResources.hlsli).
#define DIFFUSE_IBL_RESOURCES(X)               \
	X(SRV, TextureCube<float4>, EnvTexture, 0) \
	X(UAV, RWTexture2D<sh2>, IBLTexture, 0)    \
	X(SAMPLER, SamplerState, LinearSampler, 0)
ORG_PUSH_BLOCK(DIFFUSE_IBL_RESOURCES(ORG_HANDLE))
DIFFUSE_IBL_RESOURCES(ORG_RESOURCE)

// 16x16 threads, each integrating a 4x4 patch: single point samples alias small bright
// features (sunlit cloud edges) into a biased, flickering SH.
#define AXIS_SAMPLE_COUNT 16
#define TOTAL_SAMPLES (AXIS_SAMPLE_COUNT * AXIS_SAMPLE_COUNT)
#define AXIS_SUBSAMPLE_COUNT 4

// Shared memory for parallel reduction - each thread gets its own slot
groupshared sh2 sharedR[TOTAL_SAMPLES];
groupshared sh2 sharedG[TOTAL_SAMPLES];
groupshared sh2 sharedB[TOTAL_SAMPLES];

// Parallelize computation: 16x16 = 256 threads, one patch each
[numthreads(AXIS_SAMPLE_COUNT, AXIS_SAMPLE_COUNT, 1)] void main(uint3 dispatchID : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
	// Note: No bounds check needed since we dispatch exactly AXIS_SAMPLE_COUNT x AXIS_SAMPLE_COUNT threads
	uint az = dispatchID.x;
	uint ze = dispatchID.y;

	const float rcpAxisSampleCount = rcp((float)AXIS_SAMPLE_COUNT);
	const float rcpAxisSubsampleCount = rcp((float)AXIS_SUBSAMPLE_COUNT);
	const float shFactor = 4.0 * Math::PI * rcpAxisSampleCount * rcpAxisSampleCount * rcpAxisSubsampleCount * rcpAxisSubsampleCount;

	sh2 contributionR = SphericalHarmonics::Zero();
	sh2 contributionG = SphericalHarmonics::Zero();
	sh2 contributionB = SphericalHarmonics::Zero();
	[unroll] for (uint sy = 0; sy < AXIS_SUBSAMPLE_COUNT; ++sy)
	{
		[unroll] for (uint sx = 0; sx < AXIS_SUBSAMPLE_COUNT; ++sx)
		{
			float3 rayDir = SphericalHarmonics::GetUniformSphereCellSample(uint2(az, ze), uint2(sx, sy), AXIS_SAMPLE_COUNT, AXIS_SUBSAMPLE_COUNT);
			float3 color = EnvTexture.SampleLevel(LinearSampler, -rayDir, 0).xyz;
			sh2 sh = SphericalHarmonics::Evaluate(rayDir);
			contributionR = SphericalHarmonics::Add(contributionR, SphericalHarmonics::Scale(sh, color.r * shFactor));
			contributionG = SphericalHarmonics::Add(contributionG, SphericalHarmonics::Scale(sh, color.g * shFactor));
			contributionB = SphericalHarmonics::Add(contributionB, SphericalHarmonics::Scale(sh, color.b * shFactor));
		}
	}

	// Store each thread's contribution in shared memory
	sharedR[groupIndex] = contributionR;
	sharedG[groupIndex] = contributionG;
	sharedB[groupIndex] = contributionB;

	GroupMemoryBarrierWithGroupSync();

	// Parallel reduction using tree-based approach (compatible with DirectX 11)
	// Reduce 256 values down to 1 using logarithmic steps
	[unroll] for (uint stride = TOTAL_SAMPLES / 2; stride > 0; stride >>= 1)
	{
		if (groupIndex < stride) {
			sharedR[groupIndex] = SphericalHarmonics::Add(sharedR[groupIndex], sharedR[groupIndex + stride]);
			sharedG[groupIndex] = SphericalHarmonics::Add(sharedG[groupIndex], sharedG[groupIndex + stride]);
			sharedB[groupIndex] = SphericalHarmonics::Add(sharedB[groupIndex], sharedB[groupIndex + stride]);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	// Only first thread writes the final accumulated result
	if (groupIndex == 0) {
		IBLTexture[int2(0, 0)] = sharedR[0];
		IBLTexture[int2(1, 0)] = sharedG[0];
		IBLTexture[int2(2, 0)] = sharedB[0];
	}
}