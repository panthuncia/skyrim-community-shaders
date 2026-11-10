#ifndef __LENSFLARE_CONSTANTS_HLSLI__
#define __LENSFLARE_CONSTANTS_HLSLI__

// LensFlareConstants (LensFlare::LensFlareCB), for lensflare.cs.hlsl and lensflare_fft.cs.hlsl. The including shader defines
// NUM_GHOSTS and lists the LATCH entry LensFlareFrame in its push block, after its own resources.
//
// D3D11 reads the cbuffer at b1, declared exactly as before. Under ORG (ORG/ORGLensFlare.cpp) the same fields are the LATCH
// entry LensFlareFrame, and each field is copied into a static of the same name, so the shader code reads them as it does
// under D3D11. The fields are listed once: M(type, name) for a value, A(type, name, count) for an array.
#define LENS_FLARE_CB_FIELDS(M, A)    \
	M(float, OutputWidth)             \
	M(float, OutputHeight)            \
	M(float, InputWidth)              \
	M(float, InputHeight)             \
	M(float, ThresholdLevel)          \
	M(float, ThresholdRange)          \
	M(float, GhostStrength)           \
	M(float, GhostChromaShift)        \
	M(float, HaloStrength)            \
	M(float, HaloRadius)              \
	M(float, HaloWidth)               \
	M(float, HaloCompression)         \
	M(float, HaloChromaShift)         \
	M(float, Intensity)               \
	M(uint, FFTResolution)            \
	M(int, GLocalMask)                \
	M(float3, Tint)                   \
	M(float, KernelScale)             \
	M(float, AspectRatio)             \
	M(int, ApertureBlades)            \
	M(float, ApertureRotation)        \
	M(float, PadScale)                \
	M(uint, ActiveGhostMask)          \
	M(float, ApertureSize)            \
	M(float2, _pad0)                  \
	A(float4, GhostColors, NUM_GHOSTS) \
	A(float4, GhostScalesPacked, 2)   \
	A(float4, GhostKernelScalesPacked, 2)

#define LENS_FLARE_CB_MEMBER(T, N) T N;
#define LENS_FLARE_CB_ARRAY(T, N, C) T N[C];

#if defined(CS_ORG_BINDLESS)
struct LensFlareConstants
{
	LENS_FLARE_CB_FIELDS(LENS_FLARE_CB_MEMBER, LENS_FLARE_CB_ARRAY)
};
ORG_RESOURCE(LATCH, LensFlareConstants, LensFlareFrame, 1)
#	define LENS_FLARE_CB_STATIC(T, N) static T N = LensFlareFrame.N;
#	define LENS_FLARE_CB_STATIC_ARRAY(T, N, C) static T N[C] = LensFlareFrame.N;
LENS_FLARE_CB_FIELDS(LENS_FLARE_CB_STATIC, LENS_FLARE_CB_STATIC_ARRAY)
#else
cbuffer LensFlareConstants : register(b1){ LENS_FLARE_CB_FIELDS(LENS_FLARE_CB_MEMBER, LENS_FLARE_CB_ARRAY) };
#endif

#endif  // __LENSFLARE_CONSTANTS_HLSLI__
