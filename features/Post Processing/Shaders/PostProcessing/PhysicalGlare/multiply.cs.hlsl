// Physical Glare — Frequency-domain complex multiplication
// Community Shaders / Post Processing — Author: Jiaye, 2026
//
// Element-wise complex multiplication of the scene FFT with the PSF FFT
// for one colour channel: (a+bi)(c+di) = (ac−bd) + (ad+bc)i.
// Implements the convolution theorem: IFFT(F·G) = f∗g [1, section 2.1].
//
// References:
//   [1] Delavennat (2021), Physically-based Real-time Glare, LiU.

#include "PostProcessing/PhysicalGlare/glare.hlsli"

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below (PhysicalGlare::GlareCB at b1). The ORG chain
// (ORG/ORGPhysicalGlare.cpp) gives heap handles in the push block and the constants from its latch.
#define MULTIPLY_RESOURCES(X)                     \
	X(SRV, Texture2D<float2>, TexSceneFFT_R, 0)   \
	X(SRV, Texture2D<float2>, TexPSF_FFT_R, 1)    \
	X(SRV, Texture2D<float2>, TexSceneFFT_G, 2)   \
	X(SRV, Texture2D<float2>, TexPSF_FFT_G, 3)    \
	X(SRV, Texture2D<float2>, TexSceneFFT_B, 4)   \
	X(SRV, Texture2D<float2>, TexPSF_FFT_B, 5)    \
	X(UAV, RWTexture2D<float2>, RWTexResult_R, 0) \
	X(UAV, RWTexture2D<float2>, RWTexResult_G, 1) \
	X(UAV, RWTexture2D<float2>, RWTexResult_B, 2) \
	X(LATCH, GlareConstants, Glare, 1)
ORG_PUSH_BLOCK(MULTIPLY_RESOURCES(ORG_HANDLE))
MULTIPLY_RESOURCES(ORG_RESOURCE)

float2 MultiplyAndNormalise(float2 scene, float2 psf, float psfDC)
{
	// DC component F[0,0] of PSF FFT equals its spatial-domain sum.
	// Dividing by it normalises the PSF to unit energy so the convolution
	// preserves the thresholded scene's brightness level.
	float2 result;
	result.x = scene.x * psf.x - scene.y * psf.y;
	result.y = scene.x * psf.y + scene.y * psf.x;
	return result / max(psfDC, 1e-6);
}

[numthreads(8, 8, 1)] void CS_Multiply(uint2 tid : SV_DispatchThreadID) {
	if (tid.x >= Glare.FFTResolution || tid.y >= Glare.FFTResolution)
		return;

	RWTexResult_R[tid] = MultiplyAndNormalise(TexSceneFFT_R[tid], TexPSF_FFT_R[tid], TexPSF_FFT_R[uint2(0, 0)].x);
	RWTexResult_G[tid] = MultiplyAndNormalise(TexSceneFFT_G[tid], TexPSF_FFT_G[tid], TexPSF_FFT_G[uint2(0, 0)].x);
	RWTexResult_B[tid] = MultiplyAndNormalise(TexSceneFFT_B[tid], TexPSF_FFT_B[tid], TexPSF_FFT_B[uint2(0, 0)].x);
}
