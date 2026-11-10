#include "Common/OrgResources.hlsli"

// Cloud Shadows' occlusion copy on the render graph (src/Features/CloudShadows/ORGCloudShadows.cpp): one mip of the last cloud
// layer's six faces into the occlusion cubemap. A group's z is its face. Only the ORG path builds it: D3D11 copies the whole
// cubemap with CopyResource. A dispatch through the client's run gate, so a run that does not copy dispatches no groups.
#define OCCLUSION_COPY_RESOURCES(X)               \
	X(SRV, Texture2DArray<float>, Face0, 0)       \
	X(SRV, Texture2DArray<float>, Face1, 1)       \
	X(SRV, Texture2DArray<float>, Face2, 2)       \
	X(SRV, Texture2DArray<float>, Face3, 3)       \
	X(SRV, Texture2DArray<float>, Face4, 4)       \
	X(SRV, Texture2DArray<float>, Face5, 5)       \
	X(UAV, RWTexture2DArray<float>, Occlusion, 0)
ORG_PUSH_BLOCK(OCCLUSION_COPY_RESOURCES(ORG_HANDLE))
OCCLUSION_COPY_RESOURCES(ORG_RESOURCE)

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint width, height, faces;
	Occlusion.GetDimensions(width, height, faces);
	if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
		return;

	// Each face view holds this mip of one face, as its first slice and mip.
	const int4 texel = int4(dispatchThreadID.xy, 0, 0);
	float value;
	switch (dispatchThreadID.z) {
	case 0:
		value = Face0.Load(texel);
		break;
	case 1:
		value = Face1.Load(texel);
		break;
	case 2:
		value = Face2.Load(texel);
		break;
	case 3:
		value = Face3.Load(texel);
		break;
	case 4:
		value = Face4.Load(texel);
		break;
	default:
		value = Face5.Load(texel);
		break;
	}
	Occlusion[dispatchThreadID] = value;
}
