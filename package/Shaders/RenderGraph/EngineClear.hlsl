// The engine skeleton's clears (src/RenderGraph/Engine/Skeleton): a clear drawn as a fullscreen triangle, so a run gate can
// switch it on or off per run while the recording is kept. The engine clears depth to 1.0 and stencil to 0 only; the depth
// program writes depth 1.0 from the vertex stage with an Always test, and its stencil ops write zero. The colour program
// writes the colour the run's commit latched for its view: a recording is kept across runs, so the colour cannot be in the
// push block. ClearBlock in Skeleton.cpp mirrors the push block.

#include "Common/OrgResources.hlsli"

struct ClearColour
{
	float4 Colour;
};

#define ENGINE_CLEAR_RESOURCES(X) X(LATCH, ClearColour, Clear, 0)

ORG_PUSH_BLOCK(ENGINE_CLEAR_RESOURCES(ORG_HANDLE))
ENGINE_CLEAR_RESOURCES(ORG_RESOURCE)

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
	return Clear.Colour;
}

void PSDepth(ClearVSOutput input)
{
}
