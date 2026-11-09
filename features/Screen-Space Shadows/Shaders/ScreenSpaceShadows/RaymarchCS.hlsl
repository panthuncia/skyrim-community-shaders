
#include "Common/OrgResources.hlsli"
#include "Common/SharedData.hlsli"

#include "ScreenSpaceShadows/bend_sss_gpu.hlsli"

// One dispatch's values. Must match ScreenSpaceShadows::RaymarchCB.
struct RaymarchConstants
{
	// Runtime data returned from BuildDispatchList():
	float4 LightCoordinate;  // Values stored in DispatchList::LightCoordinate_Shader by BuildDispatchList()
	int2 WaveOffset;         // Values stored in DispatchData::WaveOffset_Shader by BuildDispatchList()

	// Renderer Specific Values:
	float FarDepthValue;   // Set to the Depth Buffer Value for the far clip plane, as determined by renderer projection matrix setup (typically 0).
	float NearDepthValue;  // Set to the Depth Buffer Value for the near clip plane, as determined by renderer projection matrix setup (typically 1).

	// Sampling data:
	float2 InvDepthTextureSize;  // Inverse of the texture dimensions for 'DepthTexture' (used to convert from pixel coordinates to UVs)
								 // If 'PointBorderSampler' is an Unnormalized sampler, then this value can be hard-coded to 1.
								 // The 'USE_HALF_PIXEL_OFFSET' macro might need to be defined if sampling at exact pixel coordinates isn't precise (e.g., if odd patterns appear in the shadow).

	float2 DynamicRes;

	float SurfaceThickness;
	float BilinearThreshold;
	float ShadowContrast;
};

// Match bend_sss_gpu.hlsli's struct field types for assignment compatibility.
// TERRAIN_BLENDING ON  -> R32_FLOAT (no unorm). OFF -> R24_UNORM_X8_TYPELESS (unorm).
#if defined(TERRAIN_BLENDING)
#	define RAYMARCH_DEPTH_TEXTURE Texture2D<float>
#else
#	define RAYMARCH_DEPTH_TEXTURE Texture2D<unorm float>
#endif

// DepthTexture: the depth buffer (R32_FLOAT with Terrain Blending, else R24_UNORM_X8_TYPELESS).
// OutputTexture: front- and back-facing screen-space shadow visibility (R8G8_UNORM).
// PointBorderSampler: a point sampler, with Wrap Mode set to Clamp-To-Border-Color (D3D12_TEXTURE_ADDRESS_MODE_BORDER), and Border Color set to "FarDepthValue" (typically zero), or some other far-depth value out of DepthBounds.
// If you have issues where invalid shadows are appearing from off-screen, it is likely that this sampler is not correctly setup.
// PerFrame: the dispatch's values. D3D11 updates the constant buffer before each dispatch; ORG
// reads them from the execution's latch region (ORGScreenSpaceShadows.cpp).
#define RAYMARCH_RESOURCES(X)                           \
	X(SRV, RAYMARCH_DEPTH_TEXTURE, DepthTexture, 0)     \
	X(UAV, RWTexture2D<unorm float2>, OutputTexture, 0) \
	X(SAMPLER, SamplerState, PointBorderSampler, 0)     \
	X(LATCH, RaymarchConstants, PerFrame, 1)
ORG_PUSH_BLOCK(RAYMARCH_RESOURCES(ORG_HANDLE))
RAYMARCH_RESOURCES(ORG_RESOURCE)

[numthreads(WAVE_SIZE, 1, 1)] void main(
	int3 groupID : SV_GroupID,
	int groupThreadID : SV_GroupThreadID) {
	DispatchParameters parameters;
	parameters.SetDefaults();

	parameters.LightCoordinate = PerFrame.LightCoordinate;
	parameters.WaveOffset = PerFrame.WaveOffset;
	parameters.FarDepthValue = 1;
	parameters.NearDepthValue = 0;
	parameters.InvDepthTextureSize = PerFrame.InvDepthTextureSize;
	parameters.DepthTexture = DepthTexture;
	parameters.OutputTexture = OutputTexture;
	parameters.PointBorderSampler = PointBorderSampler;

	parameters.SurfaceThickness = PerFrame.SurfaceThickness;
	parameters.BilinearThreshold = PerFrame.BilinearThreshold;
	parameters.ShadowContrast = PerFrame.ShadowContrast;

	parameters.DynamicRes = PerFrame.DynamicRes;

	parameters.UsePrecisionOffset = true;

	WriteScreenSpaceShadow(parameters, groupID, groupThreadID);
}
