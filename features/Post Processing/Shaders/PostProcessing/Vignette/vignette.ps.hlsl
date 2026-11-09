// Vignette: purely per-pixel multiply. SV_Position at pixel centers is
// tid + 0.5, so uvs derive straight from it.

#include "Common/OrgResources.hlsli"
#include "PostProcessing/fullscreen.hlsli"

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below (Vignette::VignetteCB at b1). The ORG chain
// (ORG/ORGVignette.cpp) gives heap handles in the push block and the constants from its latch.
struct VignetteConstants
{
	float4 Params0;  // focal, anamorphism (included in aspect ratio), power, aspect ratio
	float4 RcpDynRes;
};

#define VIGNETTE_RESOURCES(X)              \
	X(SRV, Texture2D<float4>, TexColor, 0) \
	X(LATCH, VignetteConstants, Vignette, 1)
ORG_PUSH_BLOCK(VIGNETTE_RESOURCES(ORG_HANDLE))
VIGNETTE_RESOURCES(ORG_RESOURCE)

float4 main(FullscreenTriangleVSOutput input) : SV_Target
{
	uint2 tid = uint2(input.Position.xy);
	float3 color = TexColor[tid].rgb;

	float2 uv = (tid + .5) * Vignette.RcpDynRes.xy;

	float cos_view = length((uv - .5) * float2(1, Vignette.Params0.w));
	cos_view = Vignette.Params0.x * rsqrt(cos_view * cos_view + Vignette.Params0.x * Vignette.Params0.x);
	float vignette = pow(abs(cos_view), Vignette.Params0.z);

	color *= vignette;

	return float4(color, 1);
}
