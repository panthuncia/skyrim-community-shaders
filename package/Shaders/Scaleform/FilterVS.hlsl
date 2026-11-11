// Scaleform GFx 3.3's filter vertex shader (GRendererD3D1x, the exe's VS-B table slot 1), transcribed from the HLSL source the
// exe embeds (docs/development/p5/scaleform.md, "Shaders"). It draws DrawBlurRect's and DrawColorMatrixRect's rectangles. FXC
// builds it as the game's own.
//
// The ORG build (CS_ORG_BINDLESS) has no input layout: it reads the rectangle's vertices (float4 position, float2 texture
// coordinate; 24 bytes each) and the matrix from a staging buffer, at offsets in the push block (Scaleform/Filter.hlsli).

#if defined(CS_ORG_BINDLESS)

#	include "Scaleform/Filter.hlsli"

ScaleformFilterVaryings main(uint vertexID : SV_VertexID)
{
	const uint address = FilterVertexOffset + vertexID * 24;
	const float4 pos = asfloat(FilterGeometry.Load4(address));
	ScaleformFilterVaryings output;
	output.tc = asfloat(FilterGeometry.Load2(address + 16));
	// mul(pos, mvp) of a column-major float4x4: each memory row is a column.
	output.vpos = float4(dot(pos, FilterVertex.mvp[0]), dot(pos, FilterVertex.mvp[1]), dot(pos, FilterVertex.mvp[2]), dot(pos, FilterVertex.mvp[3]));
	return output;
}

#else

cbuffer VSConstants
{
	float4x4 mvp;
}
void main(float4 pos : POSITION,
	float2 atc : TEXCOORD0,
	out float2 tc : TEXCOORD0,
	out float4 vpos : SV_Position)
{
	tc = atc;
	vpos = mul(pos, mvp);
}

#endif
