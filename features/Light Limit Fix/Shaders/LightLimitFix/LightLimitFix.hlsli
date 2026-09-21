
#include "Common/NamespacedCBuffer.hlsli"
#include "Common/DCLFObjects.hlsli"

namespace LightLimitFix
{

#include "LightLimitFix/Common.hlsli"

	cbuffer StrictLightData : register(b3)
	{
		uint NSCB(LightLimitFix, NumStrictLights);
		int NSCB(LightLimitFix, RoomIndex);
		uint NSCB(LightLimitFix, ShadowBitMask);
		uint NSCB(LightLimitFix, pad0);
		Light NSCB(LightLimitFix, StrictLights)[15];
	};
	NSCB_ALIAS(LightLimitFix, uint, NumStrictLights)
#if defined(DCLF_BINDLESS_DRAW)
	// Drawcall Limit Fix supplies these two per object from its own record, so that the StrictLightData
	// buffer stops being what makes a draw's binding record unique - DCLF writes nothing else into it, and
	// with NumStrictLights at 0 the StrictLights tail is never read. The cbuffer above keeps its
	// declaration and its layout: only the names change source, and NSCB_ALIAS is already exactly this
	// shape of static under DXC. Every other includer of this header compiles the aliases unchanged.
	static const int RoomIndex = DCLFObjects[DCLFObjectIndex].RoomIndex;
	static const uint ShadowBitMask = DCLFObjects[DCLFObjectIndex].ShadowBitMask;
#else
	NSCB_ALIAS(LightLimitFix, int, RoomIndex)
	NSCB_ALIAS(LightLimitFix, uint, ShadowBitMask)
#endif  // DCLF_BINDLESS_DRAW
	NSCB_ALIAS(LightLimitFix, uint, pad0)
	NSCB_ARRAY_ALIAS(LightLimitFix, Light, StrictLights, 15)

	StructuredBuffer<Light> lights : register(t35);
	StructuredBuffer<uint> lightList : register(t36);       //MAX_CLUSTER_LIGHTS * 16^3
	StructuredBuffer<LightGrid> lightGrid : register(t37);  //16^3

	bool GetClusterIndex(in float2 uv, in float z, inout uint clusterIndex)
	{
		const uint3 clusterSize = SharedData::lightLimitFixSettings.ClusterSize.xyz;

		if (!FrameBuffer::FrameParams.y)  // Fix first person lights
			uv = 0.5;

		z = max(z, SharedData::CameraData.y);

		uint clusterZ = log(z / SharedData::CameraData.y) * clusterSize.z / log(SharedData::CameraData.x / SharedData::CameraData.y);
		uint3 cluster = uint3(uint2(uv * clusterSize.xy), clusterZ);

		// Bounds validation to prevent out-of-range cluster indices
		if (any(cluster >= clusterSize))
			return false;

		clusterIndex = cluster.x + (clusterSize.x * cluster.y) + (clusterSize.x * clusterSize.y * cluster.z);
		return true;
	}

	bool IsLightIgnored(Light light)
	{
		if (light.lightFlags & LightLimitFix::LightFlags::Shadow) {
			return !(ShadowBitMask & (1 << light.shadowLightIndex));
		}

		bool lightIgnored = false;
		if ((light.lightFlags & LightFlags::PortalStrict) && RoomIndex >= 0) {
			lightIgnored = true;
			int roomIndex = RoomIndex;
			[unroll] for (int flagsIndex = 0; flagsIndex < 4; ++flagsIndex)
			{
				if (roomIndex < 32) {
					if (((light.roomFlags[flagsIndex] >> roomIndex) & 1) == 1) {
						lightIgnored = false;
					}
					break;
				}
				roomIndex -= 32;
			}
		}
		return lightIgnored;
	}
}
