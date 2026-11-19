#include "Common/OrgResources.hlsli"

struct ColorCorrectionConstants
{
	float Brightness;
	float GammaCurve;
	uint FrameCount;
	uint Pad;
};

// D3D11: u0 and b0. ORG: handles in the push block (Common/OrgResources.hlsli), the values a latch region.
#define COLOR_CORRECTION_RESOURCES(X)             \
	X(UAV, RWTexture2D<float4>, OutputTexture, 0) \
	X(LATCH, ColorCorrectionConstants, ColorCorrection, 0)
ORG_PUSH_BLOCK(COLOR_CORRECTION_RESOURCES(ORG_HANDLE))
COLOR_CORRECTION_RESOURCES(ORG_RESOURCE)

uint3 pcg3d(uint3 v)
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	return v;
}

float3 TriDither(float2 screenPos, uint frameCount)
{
	uint3 seed1 = uint3(screenPos, frameCount);
	uint3 seed2 = uint3(screenPos, frameCount + 4729u);
	return (float3(pcg3d(seed1)) - float3(pcg3d(seed2))) / float(0xFFFFFFFFu);
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint width, height;
	OutputTexture.GetDimensions(width, height);
	if (id.x >= width || id.y >= height) {
		return;
	}

	float4 color = OutputTexture[id.xy];
	color.rgb = pow(abs(color.rgb), ColorCorrection.GammaCurve);
	color.rgb *= ColorCorrection.Brightness;
	color.rgb += TriDither(float2(id.xy), ColorCorrection.FrameCount) / 1023.0;
	OutputTexture[id.xy] = color;
}
