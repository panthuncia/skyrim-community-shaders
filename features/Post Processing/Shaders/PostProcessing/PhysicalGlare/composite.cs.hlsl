// Physical Glare — Scene composite with energy conservation
// Community Shaders / Post Processing — Author: Jiaye, 2026
//
// Reconstructs per-channel glare from IFFT results, upsamples from FFT
// resolution to screen resolution via Catmull-Rom bicubic interpolation [4],
// and composites onto the original scene.
//
// Energy conservation: the bright threshold contribution is subtracted from
// the scene and replaced by the convolved (spread) version.  At Intensity=1.0
// total luminous energy is redistributed, not added.
//
// Only the center region [P, 1−P) of the IFFT output is read, corresponding
// to the non-padded scene area (P = PaddingRatio) [1, section 2.5].
//
// References:
//   [1] Delavennat (2021), Physically-based Real-time Glare, LiU.
//   [4] Jimenez (2012), Filmic SMAA, SIGGRAPH — bicubic via HW bilinear.

#include "PostProcessing/PhysicalGlare/glare.hlsli"

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below (PhysicalGlare::GlareCB at b1). The ORG chain
// (ORG/ORGPhysicalGlare.cpp) gives heap handles in the push block and the constants from its latch.
#define COMPOSITE_RESOURCES(X)                  \
	X(SRV, Texture2D<float4>, TexScene, 0)      \
	X(SRV, Texture2D<float4>, TexIFFT_RGB, 1)   \
	X(UAV, RWTexture2D<float4>, RWTexOutput, 0) \
	X(LATCH, GlareConstants, Glare, 1)
ORG_PUSH_BLOCK(COMPOSITE_RESOURCES(ORG_HANDLE))
COMPOSITE_RESOURCES(ORG_RESOURCE)

groupshared float3 GlareTile[256];

float4 CatmullRomWeights(float fraction)
{
	float w0 = fraction * (-0.5 + fraction * (1.0 - 0.5 * fraction));
	float w1 = 1.0 + fraction * fraction * (-2.5 + 1.5 * fraction);
	float w2 = fraction * (0.5 + fraction * (2.0 - 1.5 * fraction));
	float w3 = fraction * fraction * (-0.5 + 0.5 * fraction);
	return float4(w0, w1, w2, w3);
}

float3 LoadGlare(int2 position)
{
	return TexIFFT_RGB.Load(int3(clamp(position, 0, int(Glare.FFTResolution) - 1), 0)).rgb;
}

[numthreads(8, 8, 1)] void CS_Composite(
	uint2 tid : SV_DispatchThreadID,
	uint2 groupID : SV_GroupID,
	uint groupIndex : SV_GroupIndex) {
	float2 screenSize = float2(Glare.ScreenWidth, Glare.ScreenHeight);
	float sceneScale = 1.0 - 2.0 * Glare.PaddingRatio;
	uint2 firstPixel = groupID * 8;
	float2 firstUV = (float2(firstPixel) + 0.5) / screenSize;
	float2 lastUV = (min(float2(firstPixel + 7), screenSize - 1.0) + 0.5) / screenSize;
	int2 tileStart = int2(floor((firstUV * sceneScale + Glare.PaddingRatio) * float(Glare.FFTResolution) - 0.5)) - 1;
	int2 tileEnd = int2(floor((lastUV * sceneScale + Glare.PaddingRatio) * float(Glare.FFTResolution) - 0.5)) + 2;
	uint2 tileSize = uint2(tileEnd - tileStart + 1);
	bool cached = all(tileSize <= 16);

	[branch] if (cached)
	{
		for (uint index = groupIndex; index < tileSize.x * tileSize.y; index += 64) {
			int2 position = tileStart + int2(index % tileSize.x, index / tileSize.x);
			GlareTile[index] = LoadGlare(position);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (tid.x >= (uint)Glare.ScreenWidth || tid.y >= (uint)Glare.ScreenHeight)
		return;

	float3 scene = TexScene[tid].rgb;

	// Map screen UV [0,1] → IFFT UV [P, 1-P], reading only the centre
	// region that contained the original scene within the zero-padded
	// texture [1, section 2.5].
	float2 uv = (float2(tid) + 0.5) / screenSize;
	float2 samplePos = (uv * sceneScale + Glare.PaddingRatio) * float(Glare.FFTResolution) - 0.5;
	int2 base = int2(floor(samplePos));
	float2 fraction = samplePos - float2(base);
	float4 weightsX = CatmullRomWeights(fraction.x);
	float4 weightsY = CatmullRomWeights(fraction.y);
	int2 offset = base - 1 - tileStart;
	float3 glare = 0;

	[branch] if (cached)
	{
		[unroll] for (uint row = 0; row < 4; ++row)
		{
			float3 filteredRow = 0;
			[unroll] for (uint column = 0; column < 4; ++column)
				filteredRow += GlareTile[(offset.y + row) * tileSize.x + offset.x + column] * weightsX[column];
			glare += filteredRow * weightsY[row];
		}
	}
	else
	{
		[unroll] for (uint row = 0; row < 4; ++row)
		{
			float3 filteredRow = 0;
			[unroll] for (uint column = 0; column < 4; ++column)
				filteredRow += LoadGlare(base - 1 + int2(column, row)) * weightsX[column];
			glare += filteredRow * weightsY[row];
		}
	}
	glare = max(0, glare);

	// Sanitize extreme values
	glare = min(glare, 65000.0);

	// Energy-conserving glare contribution:
	// Subtract the thresholded bright component and output the convolved
	// (spread) result.  At Intensity = 1 total energy is redistributed,
	// not gained.  Values != 1 allow artistic attenuation or exaggeration.
	// This output is combined with the scene in the Composite pass.
	float3 bright = max(0, scene - Glare.Threshold);
	float3 output = (glare - bright) * Glare.Intensity;

	// Clamp to prevent negative values at Intensity > 1.0
	output = max(output, 0);

	RWTexOutput[tid] = float4(output, 1.0);
}
