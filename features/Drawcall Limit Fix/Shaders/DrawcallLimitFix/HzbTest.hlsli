// Drawcall Limit Fix. The occlusion test against the HZB, shared by BuildDrawsCS's objects and TreeLodCullCS's instances.
// Phase 1 tests against last frame's HZB. Phase 2 retests phase 1's rejections against the one rebuilt from this frame's depth.
#ifndef DCLF_HZB_TEST_HLSLI
#define DCLF_HZB_TEST_HLSLI

struct HzbView
{
	// The main pass's camera-relative view-projection and the eye world positions are made relative to.
	float4x4 ViewProj;
	float3 Eye;
	uint Index;       // Texture2D<float>, 0 when there is none
	uint2 BaseSize;   // mip 0's
	uint Mips;
	// The HZB covers a larger power-of-two area than the image, so image UVs must be scaled into HZB UVs. 16-bit fixed point per axis.
	float2 UvScale;
};

uint2 HzbUnpackSize(uint a_packed) { return uint2(a_packed & 0xFFFF, a_packed >> 16); }
float2 HzbUnpackUvScale(uint a_packed) { return float2(a_packed & 0xFFFF, a_packed >> 16) / 65535.0; }

// The bound's screen extent and nearest depth, in the draws' clip space. False when a corner is behind the near plane,
// so the object is kept.
bool HzbScreenExtent(float4x4 a_viewProj, float3 a_eye, float3 boundCentre, float boundRadius, out float2 uvMin, out float2 uvMax, out float nearestZ)
{
	uvMin = float2(1, 1);
	uvMax = float2(0, 0);
	nearestZ = 1;
	const precise float3 centre = boundCentre - a_eye;
	float2 ndcMin = float2(1e30, 1e30);
	float2 ndcMax = float2(-1e30, -1e30);
	float minZ = 1e30;
	[unroll] for (uint corner = 0; corner < 8; ++corner) {
		const float3 offset = float3((corner & 1) ? boundRadius : -boundRadius,
			(corner & 2) ? boundRadius : -boundRadius,
			(corner & 4) ? boundRadius : -boundRadius);
		const float4 clip = mul(a_viewProj, float4(centre + offset, 1.0));
		if (clip.w <= 1e-4)
			return false;
		const float3 ndc = clip.xyz / clip.w;
		ndcMin = min(ndcMin, ndc.xy);
		ndcMax = max(ndcMax, ndc.xy);
		minZ = min(minZ, ndc.z);
	}
	// Clip to texture space. The y axis flips because the draws use a y-flipped viewport. The other orientation gives far more false negatives.
	uvMin = saturate(float2(ndcMin.x, -ndcMax.y) * 0.5 + 0.5);
	uvMax = saturate(float2(ndcMax.x, -ndcMin.y) * 0.5 + 0.5);
	nearestZ = minZ;
	return true;
}

// Whether the HZB was sampled, and what it held under the bound against the bound's nearest depth.
struct HzbResult
{
	bool Sampled;
	bool Occluded;
	float Farthest;
	float NearestZ;
	float2 UvMin, UvMax;  // in HZB texture space
	int Mip;
};

// Occluded when every depth under the extent is nearer than the object's nearest point. Four corner taps at a mip coarse
// enough to cover the extent, against the nearest point of a box holding the sphere, are conservative twice over.
HzbResult HzbTest(HzbView a_view, float3 boundCentre, float boundRadius)
{
	HzbResult result = (HzbResult)0;
	// One unsigned relational compare, not `Index == 0 || Mips == 0`. AMD's RDNA2 compiler folds Index to 0 past such a guard
	// (AGENTS.md), and the taps then read heap slot 0.
	if (min(a_view.Index, a_view.Mips) < 1u)
		return result;
	float2 uvMin, uvMax;
	float nearestZ;
	if (!HzbScreenExtent(a_view.ViewProj, a_view.Eye, boundCentre, boundRadius, uvMin, uvMax, nearestZ))
		return result;
	if (nearestZ <= 0.0)
		return result;  // in front of the near plane, nothing can occlude it

	// Image UVs to HZB UVs, before anything is measured in HZB texels.
	uvMin *= a_view.UvScale;
	uvMax *= a_view.UvScale;

	const uint2 baseSize = a_view.BaseSize;
	const float2 extent = (uvMax - uvMin) * float2(baseSize);
	// A mip whose texels are at least half the extent, so four corner taps cover it.
	int mip = (int)ceil(log2(max(max(extent.x, extent.y), 1.0)));
	mip = clamp(mip, 0, (int)a_view.Mips - 1);
	const int2 mipSize = max(int2(baseSize) >> mip, int2(1, 1));
	const int2 texelMin = clamp(int2(uvMin * float2(mipSize)), int2(0, 0), mipSize - 1);
	const int2 texelMax = clamp(int2(uvMax * float2(mipSize)), int2(0, 0), mipSize - 1);

	Texture2D<float> hzb = ResourceDescriptorHeap[a_view.Index];
	const float farthest = max(
		max(hzb.Load(int3(texelMin.x, texelMin.y, mip)), hzb.Load(int3(texelMax.x, texelMin.y, mip))),
		max(hzb.Load(int3(texelMin.x, texelMax.y, mip)), hzb.Load(int3(texelMax.x, texelMax.y, mip))));

	// nearestZ is raw clip space, but the HZB holds stored depth, scaled by viewport depth ranges that differ between the native pass
	// and DCLF's draws. The error leans toward culling and hurts flat objects on surfaces, so err toward drawing.
	const float kDepthMargin = 1e-4;
	result.Sampled = true;
	result.Occluded = farthest < nearestZ - kDepthMargin;
	result.Farthest = farthest;
	result.NearestZ = nearestZ;
	result.UvMin = uvMin;
	result.UvMax = uvMax;
	result.Mip = mip;
	return result;
}

#endif  // DCLF_HZB_TEST_HLSLI
