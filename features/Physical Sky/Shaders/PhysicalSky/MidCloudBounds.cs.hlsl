// Dual-target (Common/OrgResources.hlsli). On the render graph (ORGVolumetricClouds) each mip of the chain is its own pass:
// main writes mip 0 from the weather map, reduceBounds writes mip n from mip n - 1.
#include "Common/OrgResources.hlsli"

#define MID_CLOUD_BOUNDS_RESOURCES(X)        	X(SRV, Texture2D<float4>, Weather, 99)  	X(SRV, Texture2D<float2>, Bounds, 105)  	X(UAV, RWTexture2D<float2>, OutputBounds, 5)
ORG_PUSH_BLOCK(MID_CLOUD_BOUNDS_RESOURCES(ORG_HANDLE))
MID_CLOUD_BOUNDS_RESOURCES(ORG_RESOURCE)

groupshared float2 SharedBounds[64];

[numthreads(8, 8, 1)] void main(uint2 group : SV_GroupID, uint2 thread : SV_GroupThreadID, uint lane : SV_GroupIndex) {
	uint2 size, tiles;
	Weather.GetDimensions(size.x, size.y);
	OutputBounds.GetDimensions(tiles.x, tiles.y);
	const int2 first = int2(group * size / tiles) - 1;
	const int2 end = int2((group + 1u) * size / tiles) + 1;
	float2 bounds = float2(1e30, -1e30);
	for (int y = first.y + int(thread.y); y < end.y; y += 8) {
		for (int x = first.x + int(thread.x); x < end.x; x += 8) {
			const uint2 address = uint2(int2(x, y) + int2(size)) % size;
			const float4 field = Weather.Load(int3(address, 0));
			if (field.x > 0.0) {
				const float3 geometry = field.yzw / field.x;
				// Extrapolated cloud types can expand the cell carrier. Keep
				// those authoring ranges valid by declining height culling.
				if (geometry.z < 0.0 || geometry.z > 1.0)
					bounds = float2(-1e30, 1e30);
				else {
					// The epsilon in weather decoding also admits a fade toward
					// zero height, even with a positive density-weighted bottom.
					bounds.x = min(bounds.x, min(min(geometry.x, geometry.y), 0.0));
					bounds.y = max(bounds.y, max(max(geometry.x, geometry.y), 0.0));
				}
			}
		}
	}
	SharedBounds[lane] = bounds;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
	{
		if (lane < stride)
			SharedBounds[lane] = float2(min(SharedBounds[lane].x, SharedBounds[lane + stride].x), max(SharedBounds[lane].y, SharedBounds[lane + stride].y));
		GroupMemoryBarrierWithGroupSync();
	}
	if (lane == 0u)
		OutputBounds[group] = SharedBounds[0];
}

	[numthreads(8, 8, 1)] void reduceBounds(uint2 pixel : SV_DispatchThreadID)
{
	uint2 size;
	OutputBounds.GetDimensions(size.x, size.y);
	if (any(pixel >= size))
		return;
	const uint2 p = pixel * 2u;
	const float2 a = Bounds.Load(int3(p, 0)), b = Bounds.Load(int3(p + uint2(1, 0), 0));
	const float2 c = Bounds.Load(int3(p + uint2(0, 1), 0)), d = Bounds.Load(int3(p + 1u, 0));
	OutputBounds[pixel] = float2(min(min(a.x, b.x), min(c.x, d.x)), max(max(a.y, b.y), max(c.y, d.y)));
}
