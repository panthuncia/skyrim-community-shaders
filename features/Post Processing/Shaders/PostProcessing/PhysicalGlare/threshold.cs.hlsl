// Physical Glare — Threshold extraction and zero-padded downsampling
// Community Shaders / Post Processing — Author: Jiaye, 2026
//
// Per-channel luminance thresholding: pixels below the threshold are
// discarded; the remainder is placed into the center [P, 1−P) region of
// the N×N FFT texture.  The surrounding border remains zero, providing
// spatial padding to suppress FFT circular convolution wrap-around
// artefacts [1, section 2.5].  P is controlled by PaddingRatio.
//
// References:
//   [1] Delavennat (2021), Physically-based Real-time Glare, LiU.

#include "PostProcessing/PhysicalGlare/glare.hlsli"

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below (PhysicalGlare::GlareCB at b1). The ORG chain
// (ORG/ORGPhysicalGlare.cpp) gives heap handles in the push block and the constants from its latch.
#define THRESHOLD_RESOURCES(X)                 \
	X(SRV, Texture2D<float4>, TexColor, 0)     \
	X(UAV, RWTexture2D<float2>, RWTexFFT_R, 0) \
	X(UAV, RWTexture2D<float2>, RWTexFFT_G, 1) \
	X(UAV, RWTexture2D<float2>, RWTexFFT_B, 2) \
	X(LATCH, GlareConstants, Glare, 1)
ORG_PUSH_BLOCK(THRESHOLD_RESOURCES(ORG_HANDLE))
THRESHOLD_RESOURCES(ORG_RESOURCE)

[numthreads(8, 8, 1)] void CS_Threshold(uint2 tid : SV_DispatchThreadID) {
	if (tid.x >= Glare.FFTResolution || tid.y >= Glare.FFTResolution)
		return;

	// Zero-padding [1, section 2.5]:
	// Scene is placed in the centre region of the N×N FFT texture.
	// PaddingRatio controls border width per side that absorbs convolution
	// overflow:  0.25 = 50% effective (default in [1]), 0.1 = 80%, 0 = none.
	uint padding = uint(float(Glare.FFTResolution) * Glare.PaddingRatio);
	uint sceneSize = Glare.FFTResolution - 2 * padding;

	if (tid.x >= padding && tid.x < padding + sceneSize &&
		tid.y >= padding && tid.y < padding + sceneSize) {
		// Map center region to screen UV [0,1]
		float2 localPos = float2(tid.x - padding, tid.y - padding);
		float2 uv = (localPos + 0.5) / float(sceneSize);

		// Sample scene at corresponding screen position
		uint2 screenPos = uint2(uv * float2(Glare.ScreenWidth, Glare.ScreenHeight));
		screenPos = min(screenPos, uint2(uint(Glare.ScreenWidth) - 1, uint(Glare.ScreenHeight) - 1));

		float3 color = TexColor[screenPos].rgb;

		// Per-channel thresholding [1, section 3.2], adapted for HDR:
		// Values exceeding Threshold pass through at original HDR magnitude.
		// DC-normalised convolution preserves energy, so the empirical
		// scaling factor from [1] is unnecessary.
		float3 extracted = max(0, color.rgb - Glare.Threshold);

		RWTexFFT_R[tid] = float2(extracted.r, 0);
		RWTexFFT_G[tid] = float2(extracted.g, 0);
		RWTexFFT_B[tid] = float2(extracted.b, 0);
	} else {
		// Zero-padded border — absorbs convolution overflow
		RWTexFFT_R[tid] = float2(0, 0);
		RWTexFFT_G[tid] = float2(0, 0);
		RWTexFFT_B[tid] = float2(0, 0);
	}
}
