// The game's separable compute blur, transcribed from the vanilla shader so it can build with DXC. The defines are the
// game's (BSImagespaceShader::GetShaderMacros), one set per blur:
//
//   Reflection     TEXTAP=9  BLUR_SCALE=2 SIGMA=2  OUTPUT_CHANNEL_COUNT=4
//   Volumetric     TEXTAP=5  BLUR_SCALE=6 SIGMA=3  OUTPUT_CHANNEL_COUNT=1 DEPTH_THRESHOLD=0.002f EARLY_DEPTH_REJECT
//   Parallax mask  TEXTAP=11 BLUR_SCALE=8 SIGMA=18 OUTPUT_CHANNEL_COUNT=1 (INPUT_CHANNEL_ALPHA on the horizontal pass)
//   Depth of field TEXTAP=5  BLUR_SCALE=1 SIGMA=1  DEPTH_THRESHOLD=0.04f (three channels)
//
// plus GRIDSIZE_X (horizontal) or GRIDSIZE_Y (vertical). The thread group sizes are vanilla's, because the game sizes the
// dispatch for them. The weights are vanilla's numbers.

#if defined(CS_ORG_BINDLESS)
// The ORG build (RenderGraph/Engine/ImageSpace, CS_ORG_IMAGESPACE): t0-t1, s0-s1 and u0 from the heap (t1 and s1 are read only
// with DEPTH_THRESHOLD), and the constants the engine uploaded for this dispatch (CS b0) from the engine-constants capture through
// the pass's latch.
#	define ORG_SHARED_CONSTANTS
#	include "Common/OrgEngineConstants.hlsli"
#	include "Common/OrgResources.hlsli"
#	define ISBLURCS_ORG_RESOURCES(X) X(SHARED, ConstantBuffer, Shared, 0) X(SRV, Texture2D<float4>, InputTexture, 0) X(SRV, Texture2D<float4>, DepthTexture, 1) X(SAMPLER, SamplerState, LinearSampler, 0) X(SAMPLER, SamplerState, DepthSampler, 1) X(UAV, RWTexture2D<float4>, OutputTexture, 0) X(LATCH, OrgEngineConstantsAt, PerGeometryAt, 0)
ORG_PUSH_BLOCK(ISBLURCS_ORG_RESOURCES(ORG_HANDLE))
ISBLURCS_ORG_RESOURCES(ORG_RESOURCE)
#endif

#if !defined(OUTPUT_CHANNEL_COUNT)
#	define OUTPUT_CHANNEL_COUNT 3
#endif

#if TEXTAP == 9 && SIGMA == 2
static const float TapWeights[9] = { 0.028532, 0.067234, 0.124009, 0.179044, 0.202360, 0.179044, 0.124009, 0.067234, 0.028532 };
#elif TEXTAP == 5 && SIGMA == 3
static const float TapWeights[5] = { 0.178400, 0.210431, 0.222338, 0.210431, 0.178400 };
#elif TEXTAP == 11 && SIGMA == 18
static const float TapWeights[11] = { 0.088821, 0.090063, 0.091041, 0.091746, 0.092172, 0.092314, 0.092172, 0.091746, 0.091041, 0.090063, 0.088821 };
#elif TEXTAP == 5 && SIGMA == 1
static const float TapWeights[5] = { 0.061360, 0.244770, 0.387740, 0.244770, 0.061360 };
#else
#	error ISBlurCS.hlsl has no weights for this TEXTAP and SIGMA
#endif

#if OUTPUT_CHANNEL_COUNT == 1
typedef float Texel;
#elif OUTPUT_CHANNEL_COUNT == 3
typedef float3 Texel;
#else
typedef float4 Texel;
#endif

#if defined(CS_ORG_BINDLESS)
static float4 InvSize = OrgEngineRow(PerGeometryAt, 0);  // xy: one over the input size
static float4 MaxUV = OrgEngineRow(PerGeometryAt, 1);    // xy: the largest UV to sample
#else
Texture2D<float4> InputTexture : register(t0);
SamplerState LinearSampler : register(s0);
#if defined(DEPTH_THRESHOLD)
Texture2D<float4> DepthTexture : register(t1);
SamplerState DepthSampler : register(s1);
#endif
RWTexture2D<float4> OutputTexture : register(u0);

// The engine uploads this block whole (SetComputeShaderConstants), so only its layout matters.
cbuffer PerGeometry : register(b0)
{
	float4 InvSize;  // xy: one over the input size
	float4 MaxUV;    // xy: the largest UV to sample
}
#endif

#if defined(GRIDSIZE_X)
#	define SPAN GRIDSIZE_X
#else
#	define SPAN GRIDSIZE_Y
#endif
#define HALO (TEXTAP * BLUR_SCALE / 2)
// Vanilla's group is SPAN + TEXTAP * BLUR_SCALE lanes: one more than the two halos when that product is odd.
#define GROUP (SPAN + TEXTAP * BLUR_SCALE)

groupshared Texel Line[GROUP];
#if defined(DEPTH_THRESHOLD)
groupshared float LineDepth[GROUP];
#endif

Texel ReadInput(float2 uv)
{
	const float4 value = InputTexture.SampleLevel(LinearSampler, uv, 0);
#if defined(INPUT_CHANNEL_ALPHA)
	return value.w;
#elif OUTPUT_CHANNEL_COUNT == 1
	return value.x;
#elif OUTPUT_CHANNEL_COUNT == 3
	return value.xyz;
#else
	return value;
#endif
}

float4 ToOutput(Texel value)
{
#if OUTPUT_CHANNEL_COUNT == 1
	return value.xxxx;
#elif OUTPUT_CHANNEL_COUNT == 3
	return value.xyzx;
#else
	return value;
#endif
}

#if defined(GRIDSIZE_X)
[numthreads(GROUP, 1, 1)]
#else
[numthreads(1, GROUP, 1)]
#endif
	void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID) {
#if defined(GRIDSIZE_X)
		const int lane = groupThreadId.x;
		const int along = lane - HALO;
		const int2 pixel = int2(groupId.x * SPAN + along, groupId.y);
#else
		const int lane = groupThreadId.y;
		const int along = lane - HALO;
		const int2 pixel = int2(groupId.x, groupId.y * SPAN + along);
#endif

		// Every lane loads one texel, the halo lanes included. The clamp keeps reads inside the rendered area.
		const float2 uv = min((float2(pixel) + 0.5) * InvSize.xy, MaxUV.xy);
		const Texel center = ReadInput(uv);
		Line[lane] = center;
#if defined(DEPTH_THRESHOLD)
		const float centerDepth = DepthTexture.SampleLevel(DepthSampler, uv, 0).x;
		LineDepth[lane] = centerDepth;
#endif

		GroupMemoryBarrierWithGroupSync();

		if (along < 0 || along >= SPAN)
			return;

		// D3D drops stores outside the texture. Vulkan does not promise to, so test here.
		uint2 size;
		OutputTexture.GetDimensions(size.x, size.y);
		if (any(uint2(pixel) >= size))
			return;

#if defined(DEPTH_THRESHOLD)
		// How far the center's depth is from the taps' depths: TEXTAP times the center, less every tap.
		float depthDifference = TEXTAP * centerDepth;
		[unroll] for (int d = 0; d < TEXTAP; ++d)
			depthDifference -= LineDepth[lane + (d - TEXTAP / 2) * BLUR_SCALE];
#endif

		Texel sum = 0;
#if defined(EARLY_DEPTH_REJECT)
		// Across a depth edge the pixel keeps its own value.
		[branch] if (abs(depthDifference) > DEPTH_THRESHOLD)
		{
			sum = center;
		}
		else
#endif
		{
			[unroll] for (int i = 0; i < TEXTAP; ++i)
				sum += Line[lane + (i - TEXTAP / 2) * BLUR_SCALE] * TapWeights[i];
		}

#if defined(DEPTH_THRESHOLD) && !defined(EARLY_DEPTH_REJECT)
		// Near a depth edge, blend halfway back toward the unblurred pixel (a smoothstep of the difference).
		const float edge = min(abs(depthDifference) * (1.0 / DEPTH_THRESHOLD), 1.0);
		const float keep = edge * edge * (3.0 - 2.0 * edge) * 0.5;
		sum = sum + keep * (center - sum);
#endif

		OutputTexture[pixel] = ToOutput(sum);
	}
