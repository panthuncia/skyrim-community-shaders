#ifndef __SCALEFORM_FILTER_DEPENDENCY_HLSL__
#define __SCALEFORM_FILTER_DEPENDENCY_HLSL__

// The ORG side of Scaleform's filter shaders (FilterVS.hlsl, FilterPS.hlsl). Only the ORG build (CS_ORG_BINDLESS) includes
// this file. A filter draw reads its rectangle's vertices, the vertex stage's matrix and the pixel stage's constants from one
// staging buffer, at the byte offsets in the push block; the constants are the bytes the D3D11 renderer writes into its
// PSConstants buffer, in cbuffer packing.

#	include "Common/OrgResources.hlsli"

ORG_PUSH_BLOCK(
	uint FilterGeometryIndex;  // ByteAddressBuffer: the vertices, the matrix and the constants
	uint FilterVertexOffset;   // bytes: the rectangle's first vertex
	uint FilterMatrixOffset;   // bytes: the vertex stage's float4x4 (VSConstants)
	uint FilterConstantsOffset;  // bytes: the pixel stage's PSConstants
	uint FilterSourceIndex;    // Texture2D<float4>: t0
	uint FilterSecondIndex;    // t1
	uint FilterSampler0Index;  // SamplerState: s0
	uint FilterSampler1Index;) // s1

static ByteAddressBuffer FilterGeometry = ResourceDescriptorHeap[FilterGeometryIndex];

struct ScaleformFilterVertex
{
	float4 mvp[4];  // the column-major float4x4's memory rows
};
static ScaleformFilterVertex FilterVertex = FilterGeometry.Load<ScaleformFilterVertex>(FilterMatrixOffset);

/** @brief What the filter vertex stage passes on (TEXCOORD0 at location 0, as the pixel stages read it). */
struct ScaleformFilterVaryings
{
	[[vk::location(0)]] float2 tc : TEXCOORD0;
	float4 vpos : SV_Position;
};

#endif
