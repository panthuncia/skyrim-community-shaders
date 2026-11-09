#ifndef __EXPONENTIAL_HEIGHT_FOG_VOLUMETRIC_CONSTANTS_HLSLI__
#define __EXPONENTIAL_HEIGHT_FOG_VOLUMETRIC_CONSTANTS_HLSLI__

// The volumetric fog passes' per-frame constants (ExponentialHeightFog::VolumetricFogCB in C++). Each pass lists
// them as X(LATCH, VolumetricFogConstants, VolumetricFog, 0): the cbuffer at b0 under D3D11, the latch under ORG.
// Packed as a cbuffer packs them, with no member across a 16-byte row. The matrix is stored as its four rows,
// so no load depends on matrix packing (DXC's SPIR-V ignores row_major in a raw buffer load).
struct VolumetricFogConstants
{
	uint4 GridSizeAndFlags;         // near volume
	float4 InvGridSizeAndNearFade;  // near volume
	float4 GridZParams;             // near volume
	float4 ClipToWorld[4];          // rows
	float4 FrameJitterOffsets[16];
	float4 HistoryParameters;
	float4 JitterParameters;
	uint4 FarGridSizeAndFlags;  // far volume (quarter lattice)
	float4 FarInvGridSizeAndNearFade;
	float4 FarGridZParams;
	float4 FarRange;  // x = far volume start depth, y = far volume end depth, zw = unused
};

// The names the shaders read.
#define VolumetricFogGridSizeAndFlags VolumetricFog.GridSizeAndFlags
#define VolumetricFogInvGridSizeAndNearFade VolumetricFog.InvGridSizeAndNearFade
#define VolumetricFogGridZParams VolumetricFog.GridZParams
#define VolumetricFogClipToWorld float4x4(VolumetricFog.ClipToWorld[0], VolumetricFog.ClipToWorld[1], VolumetricFog.ClipToWorld[2], VolumetricFog.ClipToWorld[3])
#define VolumetricFogFrameJitterOffsets VolumetricFog.FrameJitterOffsets
#define VolumetricFogHistoryParameters VolumetricFog.HistoryParameters
#define VolumetricFogJitterParameters VolumetricFog.JitterParameters
#define VolumetricFogFarGridSizeAndFlags VolumetricFog.FarGridSizeAndFlags
#define VolumetricFogFarInvGridSizeAndNearFade VolumetricFog.FarInvGridSizeAndNearFade
#define VolumetricFogFarGridZParams VolumetricFog.FarGridZParams
#define VolumetricFogFarRange VolumetricFog.FarRange

#endif
