// The device address step of the D3D12 self-test (CS_ORG_D3D12_SELFTEST=1, src/RenderGraph/Engine/D3D12SelfTest.cpp). Each thread
// reads a word through the source buffer's device address and writes Value * 7 + 1 through the destination's
// (Common/DeviceAddress.hlsli). On D3D12 both go through BasicRHI's device address table, which this checks end to end.

#include "Common/DeviceAddress.hlsli"

cbuffer DeviceAddressTest : register(b0)
{
	uint2 Source;       // the source buffer's device address (low, high)
	uint2 Destination;  // the destination buffer's
	uint Count;
	uint3 Padding;
}

[numthreads(64, 1, 1)] void main(uint3 a_thread : SV_DispatchThreadID) {
	if (a_thread.x >= Count)
		return;
	const uint64_t source = ((uint64_t(Source.y) << 32) | uint64_t(Source.x)) + uint64_t(a_thread.x) * 4;
	const uint64_t destination = ((uint64_t(Destination.y) << 32) | uint64_t(Destination.x)) + uint64_t(a_thread.x) * 4;
	const uint value = DeviceLoad(uint, source, 4);
	DeviceStore(uint, destination, value * 7u + 1u, 4);
}
