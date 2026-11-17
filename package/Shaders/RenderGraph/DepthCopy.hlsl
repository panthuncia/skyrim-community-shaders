// A depth-stencil copy drawn as a fullscreen triangle (src/RenderGraph/AnchorPasses.cpp, DepthCopyPass).
// A transfer copy into or out of a depth image makes AMD's driver decompress it and rebuild its compression afterwards.
// A draw keeps both images in depth layouts. The target is the depth attachment, written with an Always test.
// Each pixel reads its own texel, so the copy is exact. The stencil is written through SV_StencilRef with a REPLACE op.

#include "Common/OrgResources.hlsli"

#if defined(DEPTH_COPY_STENCIL)
#	define DEPTH_COPY_RESOURCES(X)          \
		X(SRV, Texture2D<float>, Depth, 0) \
		X(SRV, Texture2D<uint2>, Stencil, 1)
#else
#	define DEPTH_COPY_RESOURCES(X) X(SRV, Texture2D<float>, Depth, 0)
#endif

ORG_PUSH_BLOCK(DEPTH_COPY_RESOURCES(ORG_HANDLE))
DEPTH_COPY_RESOURCES(ORG_RESOURCE)

struct DepthCopyVSOutput
{
	float4 Position : SV_POSITION;
};

DepthCopyVSOutput VSMain(uint vertexID : SV_VertexID)
{
	DepthCopyVSOutput output;
	const float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	output.Position = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
	return output;
}

struct DepthCopyPSOutput
{
	float Depth : SV_Depth;
#if defined(DEPTH_COPY_STENCIL)
	uint Stencil : SV_StencilRef;
#endif
};

DepthCopyPSOutput PSMain(DepthCopyVSOutput input)
{
	const int3 texel = int3(input.Position.xy, 0);
	DepthCopyPSOutput output;
	output.Depth = Depth.Load(texel);
#if defined(DEPTH_COPY_STENCIL)
	// The stencil plane's view holds the stencil in its first channel on Vulkan and in its second on D3D12.
#	if defined(__spirv__)
	output.Stencil = Stencil.Load(texel).x;
#	else
	output.Stencil = Stencil.Load(texel).y;
#	endif
#endif
	return output;
}
