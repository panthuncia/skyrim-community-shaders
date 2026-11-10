/// By ProfJack/五脚猫, 2024-2-28 UTC
/// ref:
/// http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
///
/// Fullscreen-triangle bloom chain. Each mip pass renders into its target mip
/// with a viewport sized to that mip. The upsample accumulation is done by
/// fixed-function blending: the PS emits the upsample contribution scaled by
/// UpsampleMult and the blend stage adds it to the destination mip
/// pre-multiplied by CurrentMipMult (SrcBlend = ONE, DestBlend = BLEND_FACTOR,
/// blend factor = CurrentMipMult).

#include "Common/OrgResources.hlsli"
#include "PostProcessing/fullscreen.hlsli"

#include "PostProcessing/common.hlsli"

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below (CODBloom::BloomCB at b1). The ORG chain
// (ORG/ORGCODBloom.cpp) gives heap handles in the push block and each pass's constants from its latch.
struct BloomConstants
{
	// threshold
	float Threshold;
	// upsample
	float UpsampleRadius;
	float UpsampleMult;  // in composite: bloom mult
	float CurrentMipMult;
};

#define BLOOM_RESOURCES(X)                   	X(SRV, Texture2D<float4>, TexColor, 0)   	X(SRV, Texture2D<float4>, TexBloomIn, 1) 	X(SAMPLER, SamplerState, SampColor, 0)   	X(LATCH, BloomConstants, Bloom, 1)
ORG_PUSH_BLOCK(BLOOM_RESOURCES(ORG_HANDLE))
BLOOM_RESOURCES(ORG_RESOURCE)

// A per-component or of bool vectors: HLSL 2021 (DXC) takes one only through or, FXC's || is per component.
#if defined(__HLSL_VERSION) && __HLSL_VERSION >= 2021
#	define BLOOM_OR(a, b) or(a, b)
#else
#	define BLOOM_OR(a, b) ((a) || (b))
#endif

bool3 IsNaN(float3 x)
{
	return !BLOOM_OR(BLOOM_OR(x < 0.f, x > 0.f), x == 0.f);
}

float3 Sanitise(float3 v)
{
	bool3 err = BLOOM_OR(IsNaN(v), (v < 0));
	v.x = err.x ? 0 : v.x;
	v.y = err.y ? 0 : v.y;
	v.z = err.z ? 0 : v.z;
	return v;
}

float3 ThresholdColor(float3 col, float threshold)
{
	float luma = Color::RGBToLuminance(col);
	if (luma < 1e-3)
		return 0;
	return col * (max(0, luma - threshold) / luma);
}

float4 UpsampleCOD(Texture2D tex, float2 uv, float2 radius)
{
	float4 retval = 0;
	for (int x = -1; x <= 1; ++x)
		for (int y = -1; y <= 1; ++y)
			retval += (1 << (!x + !y)) * 0.0625 * tex.SampleLevel(SampColor, uv + float2(x, y) * radius, 0);
	return retval;
}

float4 PS_Threshold(FullscreenTriangleVSOutput input) : SV_Target
{
	uint2 tid = uint2(input.Position.xy);

	float3 col_input = TexColor[tid].rgb;

	float3 col = col_input;
	col = Sanitise(col);
	col = ThresholdColor(col, Bloom.Threshold);
	return float4(col, 1);
};

float4 PS_Downsample(FullscreenTriangleVSOutput input) : SV_Target
{
	float2 px_size = fwidth(input.TexCoord);
	float2 uv = input.TexCoord;

#ifdef FIRST_MIP
	float3 col = DownsampleCODFirstMip(TexBloomIn, SampColor, uv, px_size).rgb;
#else
	float3 col = DownsampleCOD(TexBloomIn, SampColor, uv, px_size).rgb;
#endif
	return float4(col, 1);
};

/// Upsample-accumulate pass:
///     out = dst * CurrentMipMult + UpsampleCOD(TexBloomIn) * UpsampleMult
/// The PS returns the scaled upsample contribution; the destination multiply
/// happens in the blend stage (see file header).
float4 PS_Upsample(FullscreenTriangleVSOutput input) : SV_Target
{
	float2 px_size = fwidth(input.TexCoord);
	float2 uv = input.TexCoord;

	float3 col = UpsampleCOD(TexBloomIn, uv, px_size * Bloom.UpsampleRadius).rgb * Bloom.UpsampleMult;
	return float4(col, 1);
};

float4 PS_Composite(FullscreenTriangleVSOutput input) : SV_Target
{
	float2 px_size = fwidth(input.TexCoord);
	float2 uv = input.TexCoord;

	float3 col = TexColor[input.Position.xy].rgb + UpsampleCOD(TexBloomIn, uv, px_size * Bloom.UpsampleRadius).rgb * Bloom.UpsampleMult;

	return float4(col, 1);
};
