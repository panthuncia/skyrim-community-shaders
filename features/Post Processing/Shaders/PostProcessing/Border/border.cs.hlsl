#include "Common/OrgResources.hlsli"

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below (Border::BorderCB at b1). The ORG chain
// (ORG/ORGBorder.cpp) gives heap handles in the push block and the constants from its latch.
struct BorderConstants
{
	float4 BorderColor;  // color in xyz, depth threshold in w
	float4 Scale;        // xyzw: up, down, left, right
	float4 BufferDim;
};

#define BORDER_RESOURCES(X)                       \
	X(SRV, Texture2D<float4>, InputTexture, 0)    \
	X(SRV, Texture2D<float>, DepthTexture, 1)     \
	X(UAV, RWTexture2D<float4>, OutputTexture, 0) \
	X(UAV, RWTexture2D<float4>, MotionTexture, 1) \
	X(LATCH, BorderConstants, Border, 1)
ORG_PUSH_BLOCK(BORDER_RESOURCES(ORG_HANDLE))
BORDER_RESOURCES(ORG_RESOURCE)

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	if (any(DTid.xy >= uint2(Border.BufferDim.xy)))
		return;

	float depth = DepthTexture[DTid.xy];
	float3 borderColor = Border.BorderColor.xyz;
	float depthThreshold = Border.BorderColor.w;
	if (depth > depthThreshold || depthThreshold == 0.0f) {
		float2 uv = (DTid.xy + 0.5f) * Border.BufferDim.zw;
		if (uv.y < Border.Scale.x || uv.y > (1 - Border.Scale.y) || uv.x < Border.Scale.z || uv.x > (1 - Border.Scale.w)) {
			OutputTexture[DTid.xy] = float4(borderColor, 1.0);
			MotionTexture[DTid.xy] = float4(0.0, 0.0, 0.0, 1.0);
		} else {
			OutputTexture[DTid.xy] = InputTexture[DTid.xy];
		}
	} else {
		OutputTexture[DTid.xy] = InputTexture[DTid.xy];
	}
}
