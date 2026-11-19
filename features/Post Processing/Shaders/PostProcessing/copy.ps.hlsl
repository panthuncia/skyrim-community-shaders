#include "Common/OrgResources.hlsli"
#include "PostProcessing/ConvertColor.hlsli"
#include "PostProcessing/fullscreen.hlsli"

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below (PostProcessing::CopyCB at b1). The ORG chain
// (ORGPostProcessing.cpp) gives heap handles in the push block and the constants from its latch.
struct CopyConstants
{
	uint inputGamut;
	uint outputGamut;
	float gamma;
	float scale;  // divides out the scene pre-exposure when Composite did not run
#if defined(CS_ORG_BINDLESS)
	uint source;  // ORG only: 1 reads texSrcAlt instead of texSrc. D3D11 binds texSrc alone.
#endif
};

// Under ORG the chain's input pass declares both main targets and picks one per frame through Copy.source, so its
// recording stays the same whichever target the tonemap hands over.
#if defined(CS_ORG_BINDLESS)
#	define COPY_RESOURCES(X)                   \
		X(SRV, Texture2D<float4>, texSrc, 0)    \
		X(SRV, Texture2D<float4>, texSrcAlt, 1) \
		X(LATCH, CopyConstants, Copy, 1)
#else
#	define COPY_RESOURCES(X)                \
		X(SRV, Texture2D<float4>, texSrc, 0) \
		X(LATCH, CopyConstants, Copy, 1)
#endif
ORG_PUSH_BLOCK(COPY_RESOURCES(ORG_HANDLE))
COPY_RESOURCES(ORG_RESOURCE)

float4 main(FullscreenTriangleVSOutput input) : SV_Target
{
	const int3 pixel = int3(input.Position.xy, 0);
#if defined(CS_ORG_BINDLESS)
	float4 color;
	if (Copy.source != 0)
		color = texSrcAlt.Load(pixel);
	else
		color = texSrc.Load(pixel);
#else
	float4 color = texSrc.Load(pixel);
#endif
	return ConvertColor(color, Copy.inputGamut, Copy.outputGamut, Copy.gamma, Copy.scale);
}
