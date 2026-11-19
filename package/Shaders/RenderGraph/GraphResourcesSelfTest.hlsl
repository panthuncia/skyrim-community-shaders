// The graph resources' self-test (CS_ORG_GRAPHRES_SELFTEST=1, src/RenderGraph/GraphResourcesSelfTest.cpp). A compute pass at the
// DeferredPasses anchor writes Value into every texel of an exported graph texture; D3D11 then copies the texture into a staging
// texture and reads one texel back. The texture comes from the descriptor heap by index (BasicRHI's ABI); the push block must match
// SelfTestConstants in GraphResourcesSelfTest.cpp.

cbuffer SelfTestConstants : register(b0)
{
	uint Value;
	uint TextureIndex;  // RWTexture2D<uint>
	uint Width;
	uint Height;
}

[numthreads(8, 8, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	RWTexture2D<uint> texture = ResourceDescriptorHeap[TextureIndex];
	texture[id.xy] = Value;
}
