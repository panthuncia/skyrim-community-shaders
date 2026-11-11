#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
SamplerState ImageSampler : register(s0);
SamplerState DepthSampler : register(s1);

Texture2D<float4> ImageTex : register(t0);
Texture2D<float4> DepthTex : register(t1);

cbuffer PerGeometry : register(b2)
{
	// The engine fills these by name. The names are vanilla's, from the BSImagespaceShaderWorldMap constructor.
	float4 TexelSize : packoffset(c0);     // xy: the blur step
	float4 CameraParams : packoffset(c1);  // yzw: depth linearization
	float4 DepthParams : packoffset(c2);   // x: focus distance, y: largest step, z: step per distance, w: scale nearer than focus
};

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float2 adjustedTexCoord = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);

	float depth = DepthTex.Sample(DepthSampler, adjustedTexCoord).x;

	float depthFactor = CameraParams.w / ((1 - depth) * CameraParams.z + CameraParams.y);
	float offsetDelta = min(DepthParams.y, DepthParams.z * abs(depthFactor - DepthParams.x));
#	if defined(NO_SKY)
	if (1 - depth <= 1e-4) {
		offsetDelta = 0;
	}
#	endif
	if (depthFactor < DepthParams.x) {
		offsetDelta *= DepthParams.w;
	}
	float2 startOffset = input.TexCoord - 3 * (TexelSize.xy * offsetDelta);

	float4 color = 0;
	for (int i = 0; i < 7; ++i) {
		for (int j = 0; j < 7; ++j) {
			float2 currentTexCoord = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(
				startOffset + TexelSize.xy * offsetDelta * float2(i, j));
			float4 currentColor = ImageTex.Sample(ImageSampler, currentTexCoord);
			color += currentColor;
		}
	}

	psout.Color = 0.0204081628 * color;

	return psout;
}
#endif
