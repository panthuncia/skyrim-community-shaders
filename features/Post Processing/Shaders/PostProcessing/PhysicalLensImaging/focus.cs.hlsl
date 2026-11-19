// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers. The ORG chain (ORG/ORGPhysicalLens.cpp) gives
// heap handles and latches the parameters. On ORG the previous focus is the output texel itself, so no copy follows.
#define ORG_SHARED_CONSTANTS
#include "Common/OrgResources.hlsli"

struct FocusConstants
{
	float2 FocusUV;
	float ManualKM;
	float Speed;
	uint AutoFocus;
	uint ResetFocus;
	float2 Padding;
};

#define FOCUS_RESOURCES(X)                       \
	X(SHARED, ConstantBuffer, Shared, 0)         \
	X(LATCH, FocusConstants, FocusParameters, 0) \
	X(SRV, Texture2D<float>, Depth, 0)           \
	X(SRV, Texture2D<float>, Previous, 1)        \
	X(SAMPLER, SamplerState, LinearSampler, 0)   \
	X(UAV, RWTexture2D<float>, Focus, 0)
ORG_PUSH_BLOCK(FOCUS_RESOURCES(ORG_HANDLE))

#include "Common/Game.hlsli"
#include "Common/SharedData.hlsli"

FOCUS_RESOURCES(ORG_RESOURCE)

[numthreads(1, 1, 1)] void UpdateFocus(uint3 id : SV_DispatchThreadID) {
	float depth = FocusParameters.ManualKM;
	if (FocusParameters.AutoFocus)
		depth = max(SharedData::GetScreenDepth(Depth.SampleLevel(LinearSampler, FocusParameters.FocusUV, 0)) * GAME_UNIT_TO_M * .001, 1e-6);
#if defined(CS_ORG_BINDLESS)
	float previous = Focus[uint2(0, 0)];
#else
	float previous = Previous[uint2(0, 0)];
#endif
	Focus[uint2(0, 0)] = FocusParameters.ResetFocus ? depth : lerp(previous, depth, FocusParameters.Speed);
}
