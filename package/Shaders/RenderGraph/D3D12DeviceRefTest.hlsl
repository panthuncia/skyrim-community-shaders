// The buffer view ref step of the D3D12 self-test (CS_ORG_D3D12_SELFTEST=1, src/RenderGraph/Engine/D3D12SelfTest.cpp). Each thread
// reads a word through the source buffer's view ref and writes Value * 7 + 1 through the destination's (Common/DeviceRef.hlsli),
// which checks BasicRHI's raw view pairs end to end: the SRV at the ref's slot, the UAV at the slot after it.

#include "Common/DeviceRef.hlsli"

cbuffer DeviceRefTest : register(b0)
{
	uint2 Source;       // the source buffer's view ref
	uint2 Destination;  // the destination buffer's
	uint Count;
	uint3 Padding;
}

[numthreads(64, 1, 1)] void main(uint3 a_thread : SV_DispatchThreadID) {
	if (a_thread.x >= Count)
		return;
	const uint value = DeviceLoad(uint, DeviceRefAdd(Source, a_thread.x * 4), 4);
	DeviceStore(uint, DeviceRefAdd(Destination, a_thread.x * 4), value * 7u + 1u, 4);
}
