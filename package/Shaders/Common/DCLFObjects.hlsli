#ifndef __DCLF_OBJECTS_DEPENDENCY_HLSL__
#define __DCLF_OBJECTS_DEPENDENCY_HLSL__

// Drawcall Limit Fix draws its permutations indirectly, and the handful of values that differ between the
// objects sharing one pipeline come from a GPU-resident table indexed by the draw's own object index
// rather than from per-draw constant buffers. That is what lets the PerGeometry buffer become
// per-pipeline and, once nothing else in the binding record is per-object, lets the record itself
// deduplicate to one per (material, pipeline) pair.
//
// This lives in its own header rather than in Lighting.hlsl because Color.hlsli consumes the emissive
// multiplier and is included before the point where the block used to sit.
#if defined(DCLF_BINDLESS)

struct DCLFObjectRecord
{
	float4 World[3];          // row_major float3x4, already relative to the eye
	float4 PreviousWorld[3];  // likewise, relative to the previous eye
	float4 MaterialData;
	float4 EmitColor;         // emissive in xyz, the per-object w of SSRParams in w
	// The values that used to reach the shader as constant buffers of their own: Light Limit Fix's room
	// index and shadow bit mask (b3), the alpha test reference (b11) and Linear Lighting's emissive
	// multiplier (b8). Only read when DCLF_BINDLESS_DRAW is also defined; the record carries them either
	// way so that the two builds share one layout.
	int RoomIndex;
	uint ShadowBitMask;
	float AlphaTestRef;
	float EmissiveMult;
};

// The indirect draw's push data. The first two words are the binding record's address, which the pipeline
// layout consumes to resolve this draw's buffers and descriptors; only the object index is read here.
cbuffer DCLFPushData : register(b190)
{
	uint2 DCLFRecordAddress : packoffset(c0.x);
	uint DCLFObjectIndex : packoffset(c0.z);
};

StructuredBuffer<DCLFObjectRecord> DCLFObjects : register(t127);

#endif  // DCLF_BINDLESS
#endif  // __DCLF_OBJECTS_DEPENDENCY_HLSL__
