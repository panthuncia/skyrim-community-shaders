// The fixed GPU load of CS_PASS_SKIP runs (CS_GPU_LOAD=<groups>), on the render graph at the Present anchor
// (src/RenderGraph/GpuLoadPass.cpp). The same arithmetic as the D3D11 load in GpuEventTimers.cpp, so both cost the same.
// The output comes from the descriptor heap by index (BasicRHI's ABI); the push block must match LoadConstants there.

cbuffer LoadConstants : register(b0)
{
	uint OutputIndex;  // RWTexture2D<float>, 256x256
	uint3 Padding;
}

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
	RWTexture2D<float> output = ResourceDescriptorHeap[OutputIndex];
	float v = id.x * 0.001 + id.y * 0.37;
	[loop] for (int i = 0; i < 1024; ++i)
		v = frac(sin(v * 12.9898 + i) * 43758.5453);
	output[uint2(id.x % 256, id.y % 256)] = v;
}
