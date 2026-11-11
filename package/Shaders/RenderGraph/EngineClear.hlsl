// The engine skeleton's clears (src/RenderGraph/Engine/Skeleton): a clear drawn as a fullscreen triangle, so a run gate can
// switch it on or off per run while the recording is kept. The engine clears depth to 1.0 and stencil to 0 only; the depth
// program writes depth 1.0 from the vertex stage with an Always test, and its stencil ops write zero. The colour program
// writes the push block's colour. The push block must match ClearConstants in Skeleton.cpp.

cbuffer ClearConstants : register(b0)
{
	float4 Colour;
}

struct ClearVSOutput
{
	float4 Position : SV_POSITION;
};

ClearVSOutput VSMain(uint vertexID : SV_VertexID)
{
	ClearVSOutput output;
	const float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	// z = w = 1: the far plane, which is the engine's depth clear value.
	output.Position = float4(uv * 2.0f - 1.0f, 1.0f, 1.0f);
	return output;
}

float4 PSColour(ClearVSOutput input) : SV_Target0
{
	return Colour;
}

void PSDepth(ClearVSOutput input)
{
}
