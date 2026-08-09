// Community Shaders D3D12 deferred-lighting entry point.
// Existing CS lighting behavior is authoritative; material classes remain on
// Skyrim's main target remains authoritative for unsupported pixels. This
// shader writes only promoted material classes; the D3D11 handoff discards all
// other pixels and therefore preserves their existing geometry-lit color.

Texture2D<float4> AlbedoTexture : register(t1);
Texture2D<float> LinearDepthTexture : register(t2);

struct Light
{
	float3 color; float fade;
	float radius; float invRadius; float fadeZone; float sizeBias;
	float3 positionWS; uint positionPadding;
	uint4 roomFlags;
	uint lightFlags; uint shadowMaskIndex; uint2 padding;
};

struct LightingContext
{
	float4 directionalLightDirection;
	float4 directionalLightColor;
	float4 directionalAmbient[3];
	float4 ambientSpecularTintAndFresnelPower;
	int roomIndex;
	uint shadowMembership;
	uint featureFlags;
	uint abiVersion;
};

#include "DeferredLightingCommon.hlsli"

struct Cluster
{
	float4 minPoint;
	float4 maxPoint;
	uint numLights;
	uint firstPage;
	uint2 padding;
};

struct LightPage
{
	uint nextPage;
	uint numLights;
	uint lightIndices[12];
};

StructuredBuffer<Light> Lights : register(t3);
StructuredBuffer<LightingContext> Contexts : register(t4);
StructuredBuffer<Cluster> Clusters : register(t5);
StructuredBuffer<LightPage> Pages : register(t6);
Texture2D<uint> PackedSurfaceTexture : register(t7);
Texture2D<float4> LocalShadowMaskTexture : register(t8);
Texture2D<float4> NormalRoughnessTexture : register(t9);
RWTexture2D<float4> CompositeTexture : register(u0);
RWTexture2D<uint> FrameMarker : register(u1);

cbuffer DeferredFrame : register(b0)
{
	row_major float4x4 projectionInverse;
	row_major float4x4 viewInverse;
	uint3 clusterGrid; uint lightCount;
	float2 screenSize; float nearPlane; float farPlane;
	uint contextCount; uint pageCapacity; uint abiVersion; uint frameFlags;
	uint enableLinearLighting; uint isDirectionalLightLinear; float directionalLightScale; float lightGamma;
	float directionalLightMultiplier; float pointLightMultiplier; float vanillaNormalization; float ambientGamma;
	float ambientMultiplier; float3 ambientPadding;
	uint frameIndex; uint cameraSignature; uint2 instrumentationPadding;
};

bool RoomAcceptsLight(Light light, int roomIndex)
{
	if ((light.lightFlags & CS_LIGHT_FLAG_PORTAL_STRICT) == 0 || roomIndex < 0)
		return true;
	uint room = uint(roomIndex);
	if (room >= 128)
		return false;
	return ((light.roomFlags[room >> 5] >> (room & 31)) & 1u) != 0;
}

bool ContextAcceptsLight(Light light, LightingContext context)
{
	if ((light.lightFlags & CS_LIGHT_FLAG_SHADOW) != 0) {
		if (light.shadowMaskIndex >= 32)
			return false;
		if ((context.shadowMembership & (1u << light.shadowMaskIndex)) == 0)
			return false;
	}
	return RoomAcceptsLight(light, context.roomIndex);
}

float3 DecodeCSNormal(float2 encoded)
{
	float2 f = encoded * 2.0f - 1.0f;
	float3 normal = float3(f, 1.0f - abs(f.x) - abs(f.y));
	float t = saturate(-normal.z);
	normal.xy += float2(normal.x >= 0.0f ? -t : t, normal.y >= 0.0f ? -t : t);
	return -normalize(normal);
}

float3 ReconstructWorldPosition(uint2 pixel, float linearDepth)
{
	float2 uv = (float2(pixel) + 0.5f) / screenSize;
	float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
	float4 ray = mul(float4(ndc, 1.0f, 1.0f), projectionInverse);
	float3 positionVS = ray.xyz / ray.w;
	positionVS *= linearDepth / max(positionVS.z, 1e-6f);
	return mul(float4(positionVS, 1.0f), viewInverse).xyz;
}

[numthreads(8, 8, 1)]
void main(uint3 pixel : SV_DispatchThreadID)
{
	uint width, height;
	CompositeTexture.GetDimensions(width, height);
	if (pixel.x >= width || pixel.y >= height)
		return;
	if (all(pixel.xy == uint2(0, 0))) {
		const uint2 samplePixel = uint2(width / 2, height / 2);
		FrameMarker[uint2(0, 0)] = frameIndex;
		FrameMarker[uint2(1, 0)] = cameraSignature;
		FrameMarker[uint2(2, 0)] = asuint(AlbedoTexture.Load(int3(samplePixel, 0)).x);
		FrameMarker[uint2(3, 0)] = PackedSurfaceTexture.Load(int3(samplePixel, 0));
	}
	// Material promotion is explicit. Until the host advertises at least one
	// parity-tested class, no packed value (including output from an older or
	// non-lighting permutation) is allowed to select deferred evaluation.
	if (frameFlags == 0) {
		return;
	}
	uint packed = PackedSurfaceTexture.Load(int3(pixel.xy, 0));
	uint contextIndex = CSDeferredContextIndex(packed);
	uint materialClass = CSDeferredMaterialClass(packed);
	if ((frameFlags & 8u) != 0 && materialClass == 1u) {
		uint ignored;
		InterlockedAdd(FrameMarker[uint2(4, 0)], 1u, ignored);
		InterlockedAdd(FrameMarker[uint2(8, 0)], pixel.x, ignored);
		InterlockedAdd(FrameMarker[uint2(9, 0)], pixel.y, ignored);
	}

	// Coverage is deliberately independent of lighting evaluation.  It proves
	// the per-pixel object-class contract before a class is promoted: legacy is
	// dim compatibility, static opaque is green, alpha-tested is blue, and any
	// unknown class is magenta.  An invalid context on a promoted class is red.
	if ((frameFlags & 4u) != 0) {
		uint ignored;
		if (materialClass == 1u) {
			if ((frameFlags & 8u) == 0)
				InterlockedAdd(FrameMarker[uint2(4, 0)], 1u, ignored);
			InterlockedAdd(FrameMarker[uint2(8, 0)], pixel.x, ignored);
			InterlockedAdd(FrameMarker[uint2(9, 0)], pixel.y, ignored);
		}
		else if (materialClass == CS_LEGACY_MATERIAL)
			InterlockedAdd(FrameMarker[uint2(5, 0)], 1u, ignored);
		else if (materialClass == 2u)
			InterlockedAdd(FrameMarker[uint2(6, 0)], 1u, ignored);
		else
			InterlockedAdd(FrameMarker[uint2(7, 0)], 1u, ignored);
		if (materialClass == CS_LEGACY_MATERIAL) {
			return;
		} else if (contextIndex == CS_INVALID_CONTEXT) {
			CompositeTexture[pixel.xy] = float4(1, 0, 0, 1);
		} else if (materialClass == 1u) {
			CompositeTexture[pixel.xy] = float4(0, 1, 0, 1);
		} else if (materialClass == 2u) {
			CompositeTexture[pixel.xy] = float4(0, 0.35, 1, 1);
		} else {
			CompositeTexture[pixel.xy] = float4(1, 0, 1, 1);
		}
		return;
	}

	// Bit zero promotes only the first independently validated object class.
	// Bit one is the developer lighting diagnostic and may evaluate that same
	// class before promotion.  No future/nonzero class is admitted implicitly.
	const bool staticOpaqueSelected = materialClass == 1u && (frameFlags & 3u) != 0;
	if (!staticOpaqueSelected) {
		return;
	}
	// Motion-isolation modes deliberately branch only after classification so
	// they exercise the identical packed-surface and interop path as lighting.
	if ((frameFlags & 64u) != 0) {
		return;
	}
	if ((frameFlags & 32u) != 0) {
		CompositeTexture[pixel.xy] = float4(AlbedoTexture.Load(int3(pixel.xy, 0)).rgb, 1.0f);
		return;
	}

	// Compatibility is authoritative until a material class has a complete,
	// parity-tested resolved surface contract.
	if (materialClass == CS_LEGACY_MATERIAL || contextIndex == CS_INVALID_CONTEXT) {
		return;
	}
	if (contextIndex >= contextCount) {
		CompositeTexture[pixel.xy] = float4(1, 0, 1, 1);
		return;
	}

	LightingContext lightingContext = Contexts[contextIndex];
	if (lightingContext.abiVersion != abiVersion) {
		CompositeTexture[pixel.xy] = float4(1, 0, 1, 1);
		return;
	}

	float depth = LinearDepthTexture.Load(int3(pixel.xy, 0));
	if (!(depth > 0.0) || !isfinite(depth) || depth >= farPlane * 0.999f) {
		if ((frameFlags & 8u) != 0) {
			uint ignored;
			InterlockedAdd(FrameMarker[uint2(10, 0)], 1u, ignored);
			InterlockedCompareExchange(FrameMarker[uint2(11, 0)], 0u,
				pixel.y * width + pixel.x + 1u, ignored);
		}
		return;
	}

	float logarithmicRange = max(1e-6, log(max(farPlane, nearPlane + 1e-3) / max(nearPlane, 1e-3)));
	uint slice = (uint)clamp(log(max(depth, nearPlane) / max(nearPlane, 1e-3)) /
		logarithmicRange * clusterGrid.z, 0.0, clusterGrid.z - 1.0);
	uint2 tile = min(pixel.xy / 64, clusterGrid.xy - 1);
	uint clusterIndex = tile.x + tile.y * clusterGrid.x + slice * clusterGrid.x * clusterGrid.y;
	Cluster cluster = Clusters[clusterIndex];
	float3 positionWS = ReconstructWorldPosition(pixel.xy, depth);
	float4 normalRoughnessSample = NormalRoughnessTexture.Load(int3(pixel.xy, 0));
	float3 normalVS = DecodeCSNormal(normalRoughnessSample.xy);
	float3 normalWS = normalize(mul(float4(normalVS, 0.0f), viewInverse).xyz);
	float3 directIrradiance = 0.0f;
	float3 directionalColor = CSDeferredTransformLight(
		lightingContext.directionalLightColor.xyz / max(directionalLightScale, 1e-5f),
		isDirectionalLightLinear != 0, lightGamma, directionalLightMultiplier,
		enableLinearLighting != 0) * directionalLightScale;
	directIrradiance += CSDeferredVanillaDiffuse(normalWS,
		normalize(lightingContext.directionalLightDirection.xyz), directionalColor,
		(frameFlags & 16u) != 0 ? 1.0f : LocalShadowMaskTexture.Load(int3(pixel.xy, 0)).x, vanillaNormalization);

	uint page = cluster.firstPage;
	uint traversedPages = 0;
	uint visitedLights = 0;
	while (page != CS_NULL_LIGHT_PAGE && page < pageCapacity && traversedPages++ < 64 && visitedLights < cluster.numLights) {
		LightPage lightPage = Pages[page];
		uint count = min(lightPage.numLights, 12);
		[unroll] for (uint i = 0; i < 12; ++i) {
			if (i >= count || visitedLights >= cluster.numLights)
				break;
			uint lightIndex = lightPage.lightIndices[i];
			if (lightIndex >= lightCount) {
				CompositeTexture[pixel.xy] = float4(1, 0, 1, 1);
				return;
			}
			// Keep rejection behavior live even before the first material class is
			// promoted; the evaluated light is consumed by the class-specific path.
			Light light = Lights[lightIndex];
			if (ContextAcceptsLight(light, lightingContext)) {
				float3 toLight = light.positionWS - positionWS;
				float distanceToLight = length(toLight);
				float attenuation = CSDeferredAttenuation(distanceToLight, light);
				if (attenuation >= 1e-5f) {
					float shadow = 1.0f;
					if ((frameFlags & 16u) == 0 && (light.lightFlags & CS_LIGHT_FLAG_SHADOW) != 0 && light.shadowMaskIndex < 4)
						shadow = LocalShadowMaskTexture.Load(int3(pixel.xy, 0))[light.shadowMaskIndex];
					float3 lightColor = CSDeferredTransformLight(light.color,
						(light.lightFlags & (1u << 11)) != 0, lightGamma, pointLightMultiplier,
						enableLinearLighting != 0) * attenuation * light.fade;
					directIrradiance += CSDeferredVanillaDiffuse(normalWS, toLight / max(distanceToLight, 1e-6f),
						lightColor, shadow, vanillaNormalization);
				}
			}
			++visitedLights;
		}
		page = lightPage.nextPage;
	}

	if ((page != CS_NULL_LIGHT_PAGE && (page >= pageCapacity || traversedPages >= 64)) || visitedLights != cluster.numLights) {
		CompositeTexture[pixel.xy] = float4(1, 0, 1, 1);
		return;
	}

	// Standard opaque has no feature lobes or material specular. Its resolved
	// albedo already contains the geometry path's base color and vertex color.
	// This pass replaces geometry-direct lighting only; CS's existing D3D11
	// deferred composite subsequently applies ambient, SSGI, skylighting and IBL.
	if ((frameFlags & 3u) != 0) {
		float3 albedo = AlbedoTexture.Load(int3(pixel.xy, 0)).rgb;
		if ((frameFlags & 8u) != 0) {
			uint ignored;
			if (all(albedo == 0.0f)) {
				InterlockedAdd(FrameMarker[uint2(12, 0)], 1u, ignored);
				InterlockedCompareExchange(FrameMarker[uint2(13, 0)], 0u,
					pixel.y * width + pixel.x + 1u, ignored);
			}
			if (all(normalRoughnessSample == 0.0f)) {
				InterlockedAdd(FrameMarker[uint2(14, 0)], 1u, ignored);
				InterlockedCompareExchange(FrameMarker[uint2(15, 0)], 0u,
					pixel.y * width + pixel.x + 1u, ignored);
			}
		}
		float3 candidate = CSDeferredIrradianceToGamma(directIrradiance * albedo,
			enableLinearLighting != 0);
		CompositeTexture[pixel.xy] = float4(candidate, 1.0f);
	}
}
