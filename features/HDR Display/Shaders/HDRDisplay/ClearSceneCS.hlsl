#include "Common/OrgResources.hlsli"

// HDR Display's scene cleared on the render graph (src/Features/HDRDisplay/ORGHDRDisplay.cpp): the clear a Bink movie frame asks
// for (HDRDisplay::PrepareMovieDraw), so the composite reads a black scene under the movie. Only the ORG path builds it: D3D11
// clears through the scene's RTV. A dispatch through the client's run gate, so a run without a movie dispatches no groups.
#define CLEAR_SCENE_RESOURCES(X) X(UAV, RWTexture2D<float4>, Scene, 0)
ORG_PUSH_BLOCK(CLEAR_SCENE_RESOURCES(ORG_HANDLE))
CLEAR_SCENE_RESOURCES(ORG_RESOURCE)

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint width, height;
	Scene.GetDimensions(width, height);
	if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
		return;
	Scene[dispatchThreadID.xy] = 0.0f;
}
