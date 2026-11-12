// The D3D12 backend's self-test (CS_ORG_D3D12_SELFTEST=1, src/RenderGraph/Engine/D3D12SelfTest.cpp). It is compiled to DXIL
// with the same dual-target resource list every ORG pass uses (Common/OrgResources.hlsli), so a pass proves three things on
// D3D12: the UAV comes from ResourceDescriptorHeap by its handle, the push block at b0 arrives as root constants, and the PUSH
// struct sits on its 16-byte boundary after the handle. Each thread writes Seed * 3 + its index into its word of the output.

#include "Common/OrgResources.hlsli"

struct SelfTestDispatch
{
	uint Seed;
	uint3 Padding;
};

#define D3D12_SELFTEST_RESOURCES(X)       \
	X(UAV, RWByteAddressBuffer, Output, 0) \
	X(PUSH, SelfTestDispatch, Dispatch, 0)

ORG_PUSH_BLOCK(D3D12_SELFTEST_RESOURCES(ORG_HANDLE))
D3D12_SELFTEST_RESOURCES(ORG_RESOURCE)

[numthreads(64, 1, 1)] void main(uint3 a_thread : SV_DispatchThreadID) {
	Output.Store(a_thread.x * 4, Dispatch.Seed * 3u + a_thread.x);
}
