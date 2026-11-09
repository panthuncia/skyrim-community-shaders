// Compile check for Common/OrgResources.hlsli: one entry of every kind and resource type the
// header covers. tools/check-dual-target-shaders.py builds it for FXC (cs_5_0, registers) and
// for DXC with CS_ORG_BINDLESS (cs_6_6, SPIR-V and DXIL). It never runs.

#include "Common/OrgResources.hlsli"

struct SampleConstants
{
	float4 Scale;
	uint4 Size;
};

struct SampleFrame
{
	float4 Tint;
	float2 Offset;
	uint Index;
	float Weight;
};

struct SampleDispatch
{
	uint Mip;
	uint Slice;
	uint2 Padding;
};

#define SAMPLE_RESOURCES(X)                              \
	X(SRV, Texture2D<float4>, Colour, 0)                 \
	X(SRV, Texture2DArray<float4>, Layers, 1)            \
	X(SRV, Texture3D<float4>, Volume, 2)                 \
	X(SRV, TextureCube<float4>, Cube, 3)                 \
	X(SRV, TextureCubeArray<float4>, Cubes, 4)           \
	X(SRV, Texture2D<float>, Depth, 5)                   \
	X(SRV, StructuredBuffer<float4>, Items, 6)           \
	X(SRV, ByteAddressBuffer, Raw, 7)                    \
	X(UAV, RWTexture2D<float4>, Output, 0)               \
	X(UAV, RWTexture2DArray<float4>, OutputLayers, 1)    \
	X(UAV, RWTexture3D<float4>, OutputVolume, 2)         \
	X(UAV, RWStructuredBuffer<uint>, Counts, 3)          \
	X(UAV, RWByteAddressBuffer, OutputRaw, 4)            \
	X(SAMPLER, SamplerState, LinearSampler, 0)           \
	X(SAMPLER, SamplerComparisonState, ShadowSampler, 1) \
	X(CBUFFER, SampleConstants, Constants, 0)            \
	X(LATCH, SampleFrame, Frame, 2)                      \
	X(PUSH, SampleDispatch, Dispatch, 1)
ORG_PUSH_BLOCK(SAMPLE_RESOURCES(ORG_HANDLE))
SAMPLE_RESOURCES(ORG_RESOURCE)

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	const float2 uv = (id.xy + 0.5) / Constants.Size.xy;
	float4 colour = Colour.SampleLevel(LinearSampler, uv, Dispatch.Mip);
	colour += Layers.SampleLevel(LinearSampler, float3(uv, Dispatch.Slice), 0);
	colour += Volume.SampleLevel(LinearSampler, float3(uv, 0.5), 0);
	colour += Cube.SampleLevel(LinearSampler, float3(uv, 1), 0);
	colour += Cubes.SampleLevel(LinearSampler, float4(uv, 1, Dispatch.Slice), 0);
	colour += Items[id.x % Constants.Size.z];
	colour += asfloat(Raw.Load4(16 * (id.y % Constants.Size.w)));
	colour *= Depth.SampleCmpLevelZero(ShadowSampler, uv, 0.5);
	colour *= Constants.Scale;
	colour = colour * Frame.Tint + float4(Frame.Offset, Frame.Index, Frame.Weight);

	Output[id.xy] = colour;
	OutputLayers[uint3(id.xy, Dispatch.Slice)] = colour;
	OutputVolume[id] = colour;
	InterlockedAdd(Counts[0], 1);
	OutputRaw.Store4(16 * id.x, asuint(colour));
}
