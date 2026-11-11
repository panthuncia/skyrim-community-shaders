// ISCopySubRegionCS: copies a rectangle of one texture into another, transcribed from the vanilla DXBC. The snow and
// water passes use it. The engine uploads the constants whole (SetComputeShaderConstants), so only their layout matters.

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

cbuffer PerTechnique : register(b0)
{
	// The names are the engine's (the constructor's constant list).
	uint4 srcRect;     // xy: the rectangle's corner in the input, zw: its size
	uint2 destOffset;  // where the corner lands in the output
	uint2 padding;
};

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (all(dispatchID.xy < srcRect.zw)) {
		OutputTexture[dispatchID.xy + destOffset] = InputTexture.Load(int3(dispatchID.xy + srcRect.xy, 0));
	}
}
