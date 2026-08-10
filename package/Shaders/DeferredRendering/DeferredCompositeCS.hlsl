// Community Shaders D3D12 deferred-lighting entry point.
// Existing CS lighting behavior is authoritative; material classes remain on
// Skyrim's main target remains authoritative for unsupported pixels. This
// shader writes only promoted material classes; the D3D11 handoff discards all
// other pixels and therefore preserves their existing geometry-lit color.

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
	float4 emissiveColor;
	float4 specularColorAndShininess;
	float4 lightingEffectParams;
	int roomIndex;
	uint shadowMembership;
	uint featureFlags;
	uint abiVersion;
};

#include "DeferredLightingCommon.hlsli"
#include "DeferredTruePBR.hlsli"

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

struct DeferredFrameData
{
	row_major float4x4 projectionInverse;
	row_major float4x4 viewInverse;
	uint3 clusterGrid; uint lightCount;
	float2 screenSize; float nearPlane; float farPlane;
	uint contextCount; uint pageCapacity; uint abiVersion; uint frameFlags;
	uint enableLinearLighting; uint isDirectionalLightLinear; float directionalLightScale; float lightGamma;
	float directionalLightMultiplier; float pointLightMultiplier; float vanillaNormalization; float ambientGamma;
	float ambientMultiplier; float3 ambientPadding;
	float4 grassFrameSettings0;
	float4 grassFrameSettings1;
	uint4 iblFlags;
	float4 iblSettings0;
	float4 iblSettings1;
	float3 skylightingPositionOffset; uint skylightingEnabled;
	uint3 skylightingArrayOrigin; float skylightingMinDiffuseVisibility;
	float skylightingMinSpecularVisibility; uint pbrMaterialCount; float2 lightingParameterPadding;
};

#define projectionInverse frame.projectionInverse
#define viewInverse frame.viewInverse
#define clusterGrid frame.clusterGrid
#define lightCount frame.lightCount
#define screenSize frame.screenSize
#define nearPlane frame.nearPlane
#define farPlane frame.farPlane
#define contextCount frame.contextCount
#define pageCapacity frame.pageCapacity
#define frameFlags frame.frameFlags
#define enableLinearLighting frame.enableLinearLighting
#define isDirectionalLightLinear frame.isDirectionalLightLinear
#define directionalLightScale frame.directionalLightScale
#define lightGamma frame.lightGamma
#define directionalLightMultiplier frame.directionalLightMultiplier
#define pointLightMultiplier frame.pointLightMultiplier
#define vanillaNormalization frame.vanillaNormalization
#define ambientGamma frame.ambientGamma
#define ambientMultiplier frame.ambientMultiplier
#define grassFrameSettings0 frame.grassFrameSettings0
#define grassFrameSettings1 frame.grassFrameSettings1
#define iblFlags frame.iblFlags
#define iblSettings0 frame.iblSettings0
#define iblSettings1 frame.iblSettings1
#define skylightingPositionOffset frame.skylightingPositionOffset
#define skylightingEnabled frame.skylightingEnabled
#define skylightingArrayOrigin frame.skylightingArrayOrigin
#define skylightingMinDiffuseVisibility frame.skylightingMinDiffuseVisibility
#define skylightingMinSpecularVisibility frame.skylightingMinSpecularVisibility
#define pbrMaterialCount frame.pbrMaterialCount

#include "DeferredIndirectLighting.hlsli"

cbuffer EvaluatorDispatch : register(b1)
{
	uint evaluatorID;
	uint evaluatorBaseOffset;
	uint evaluatorPixelCount;
	uint evaluatorDispatchWidth;
	uint frameConstantsIndex;
	uint4 descriptorIndices0;
	uint4 descriptorIndices1;
	uint4 descriptorIndices2;
	uint4 descriptorIndices3;
	uint4 descriptorIndices4;
	uint4 descriptorIndices5;
	uint4 descriptorIndices6;
};

#if defined(CS_DEFERRED_TRUE_PBR)
#ifndef CSHADER
#define CSHADER 1
#endif
#define CS_GLINT_BINDLESS 1
#include "Common/PBR.hlsli"
#include "Common/LightingEval.hlsli"

DirectLightingOutput EvaluateDeferredTruePBRDirect(MaterialProperties material,
	float3 normal, float3 coatNormal, float3 viewDirection, float3 lightDirection,
	float3 lightColor, float detailedShadow, float softShadow,
	float3 wetnessNormal, float wetnessRoughness, float3x3 tbnTr, DeferredFrameData frame)
{
	DirectContext context = CreateDirectLightingContext(normal, coatNormal, normal,
		viewDirection, viewDirection, lightDirection, lightDirection, lightColor,
		detailedShadow, softShadow, material.Flags);
	DirectLightingOutput output;
	EvaluateLighting(context, material, tbnTr, 0.0f, 0.0f, 0.0f, output);
	if (wetnessRoughness < 1.0f)
		EvaluateWetnessLighting(wetnessNormal, context, wetnessRoughness, output);
	return output;
}
#endif

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

bool IsGenericMaterial(uint materialClass)
{
	return CSDeferredEvaluatorForMaterial(materialClass) == CS_EVALUATOR_GENERIC;
}

bool HasGenericSpecular(uint materialClass)
{
	return materialClass == CS_MATERIAL_StandardSpecular ||
		materialClass == CS_MATERIAL_AlphaTestedSpecular ||
		materialClass == CS_MATERIAL_FoliageSpecular ||
		materialClass == CS_MATERIAL_TerrainSpecular;
}

CSGenericDirectLighting EvaluateDeferredDirect(uint materialClass,
	LightingContext context, float3 normal, float3 viewDirection,
	float3 lightDirection, float3 lightColor, float detailedShadow,
	float softShadow, float glossiness, float3 rimSoftColor,
	float3 backLightColor, DeferredFrameData frame)
{
	const bool hasSpecular = HasGenericSpecular(materialClass);
	CSGenericDirectLighting result = CSLightingEvaluateGenericDirect(normal,
		viewDirection, lightDirection, lightColor, detailedShadow,
		context.specularColorAndShininess.w, glossiness,
		hasSpecular ? context.specularColorAndShininess.xyz : 0.0f,
		vanillaNormalization, hasSpecular);
	if (materialClass == CS_MATERIAL_FoliageSpecial) {
		const float angle = dot(normal, lightDirection);
		const float3 softLightColor = lightColor * softShadow;
		if ((context.featureFlags & CS_CONTEXT_USES_SOFT_LIGHTING) != 0u)
			result.diffuse += softLightColor *
				CSLightingSoftMultiplier(angle, context.lightingEffectParams.x) *
				rimSoftColor * vanillaNormalization;
		if ((context.featureFlags & CS_CONTEXT_USES_RIM_LIGHTING) != 0u)
			result.diffuse += softLightColor * CSLightingRimMultiplier(
				lightDirection, viewDirection, normal, context.lightingEffectParams.y) *
				rimSoftColor * vanillaNormalization;
		if ((context.featureFlags & CS_CONTEXT_USES_BACK_LIGHTING) != 0u)
			result.diffuse += softLightColor * saturate(-angle) * backLightColor *
				vanillaNormalization;
	}
	return result;
}

float3 DecodeCSNormal(float2 encoded)
{
	float2 f = encoded * 2.0f - 1.0f;
	float3 normal = float3(f, 1.0f - abs(f.x) - abs(f.y));
	float t = saturate(-normal.z);
	normal.xy += float2(normal.x >= 0.0f ? -t : t, normal.y >= 0.0f ? -t : t);
	return -normalize(normal);
}

float3 ReconstructWorldPosition(uint2 pixel, float linearDepth, DeferredFrameData frame)
{
	float2 uv = (float2(pixel) + 0.5f) / screenSize;
	float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
	// Skyrim/CS camera constants use the matrix-first convention (see
	// FrameBuffer::ViewToWorld). Keeping row_major storage does not reverse the
	// mathematical multiplication order.
	float4 ray = mul(projectionInverse, float4(ndc, 1.0f, 1.0f));
	float3 positionVS = ray.xyz / ray.w;
	positionVS *= linearDepth / max(positionVS.z, 1e-6f);
	return mul(viewInverse, float4(positionVS, 1.0f)).xyz;
}

float3 ReconstructWorldViewDirection(uint2 pixel, DeferredFrameData frame)
{
	float2 uv = (float2(pixel) + 0.5f) / screenSize;
	float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
	float4 positionVS = mul(projectionInverse, float4(ndc, 1.0f, 1.0f));
	positionVS.xyz /= max(abs(positionVS.w), 1e-6f);
	return normalize(mul(viewInverse, float4(-positionVS.xyz, 0.0f)).xyz);
}

void AccumulateParity(uint baseSlot, uint2 pixel, float3 candidate,
	Texture2D<float4> CompatibilityReferenceTexture, RWTexture2D<uint> FrameMarker)
{
	float3 reference = CompatibilityReferenceTexture.Load(int3(pixel, 0)).rgb;
	float3 delta = candidate - reference;
	uint3 absoluteError = uint3(min(abs(delta), 16.0f) * 4096.0f + 0.5f);
	int3 signedError = int3(clamp(delta, -16.0f, 16.0f) * 4096.0f);
	uint3 candidateValue = uint3(min(max(candidate, 0.0f), 16.0f) * 4096.0f + 0.5f);
	uint3 referenceValue = uint3(min(max(reference, 0.0f), 16.0f) * 4096.0f + 0.5f);
	uint ignored;
	InterlockedAdd(FrameMarker[uint2(baseSlot + 0u, 0)], 1u, ignored);
	InterlockedAdd(FrameMarker[uint2(baseSlot + 1u, 0)], absoluteError.x, ignored);
	InterlockedAdd(FrameMarker[uint2(baseSlot + 2u, 0)], absoluteError.y, ignored);
	InterlockedAdd(FrameMarker[uint2(baseSlot + 3u, 0)], absoluteError.z, ignored);
	InterlockedMax(FrameMarker[uint2(baseSlot + 4u, 0)],
		max(absoluteError.x, max(absoluteError.y, absoluteError.z)), ignored);
	InterlockedAdd(FrameMarker[uint2(baseSlot + 5u, 0)], asuint(signedError.x), ignored);
	InterlockedAdd(FrameMarker[uint2(baseSlot + 6u, 0)], asuint(signedError.y), ignored);
	InterlockedAdd(FrameMarker[uint2(baseSlot + 7u, 0)], asuint(signedError.z), ignored);
	InterlockedAdd(FrameMarker[uint2(baseSlot + 8u, 0)], candidateValue.x, ignored);
	InterlockedAdd(FrameMarker[uint2(baseSlot + 9u, 0)], candidateValue.y, ignored);
	InterlockedAdd(FrameMarker[uint2(baseSlot + 10u, 0)], candidateValue.z, ignored);
	InterlockedAdd(FrameMarker[uint2(baseSlot + 11u, 0)], referenceValue.x, ignored);
	InterlockedAdd(FrameMarker[uint2(baseSlot + 12u, 0)], referenceValue.y, ignored);
	InterlockedAdd(FrameMarker[uint2(baseSlot + 13u, 0)], referenceValue.z, ignored);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThread : SV_DispatchThreadID)
{
	ConstantBuffer<DeferredFrameData> frameBuffer = ResourceDescriptorHeap[frameConstantsIndex];
	DeferredFrameData frame = frameBuffer;
	Texture2D<float4> SpecularTexture = ResourceDescriptorHeap[descriptorIndices0.x];
	Texture2D<float4> AlbedoTexture = ResourceDescriptorHeap[descriptorIndices0.y];
	Texture2D<float> LinearDepthTexture = ResourceDescriptorHeap[descriptorIndices0.z];
	StructuredBuffer<Light> Lights = ResourceDescriptorHeap[descriptorIndices0.w];
	StructuredBuffer<LightingContext> Contexts = ResourceDescriptorHeap[descriptorIndices1.x];
	StructuredBuffer<Cluster> Clusters = ResourceDescriptorHeap[descriptorIndices1.y];
	StructuredBuffer<LightPage> Pages = ResourceDescriptorHeap[descriptorIndices1.z];
	Texture2D<uint4> PackedSurfaceTexture = ResourceDescriptorHeap[descriptorIndices1.w];
	Texture2D<float4> LocalShadowMaskTexture = ResourceDescriptorHeap[descriptorIndices2.x];
	Texture2D<float4> NormalRoughnessTexture = ResourceDescriptorHeap[descriptorIndices2.y];
	Texture2D<float4> CompatibilityReferenceTexture = ResourceDescriptorHeap[descriptorIndices2.z];
	Texture2D<float4> MasksTexture = ResourceDescriptorHeap[descriptorIndices2.w];
	Texture2D<float> ScreenSpaceShadowTexture = ResourceDescriptorHeap[descriptorIndices3.x];
	StructuredBuffer<uint2> EvaluatorPixelList = ResourceDescriptorHeap[descriptorIndices3.y];
	Texture2D<float4> EnvironmentIBLTexture = ResourceDescriptorHeap[descriptorIndices3.z];
	Texture2D<float4> SkyIBLTexture = ResourceDescriptorHeap[descriptorIndices3.w];
	Texture2DArray<float4> SkylightingProbeTexture = ResourceDescriptorHeap[descriptorIndices4.x];
	Texture2DArray<float> SkylightingVisibilityTexture = ResourceDescriptorHeap[descriptorIndices4.y];
	Texture2D<float4> ReflectanceTexture = ResourceDescriptorHeap[descriptorIndices4.z];
	StructuredBuffer<PBRMaterialRecord> PBRMaterials = ResourceDescriptorHeap[descriptorIndices4.w];
	RWTexture2D<float4> CompositeTexture = ResourceDescriptorHeap[descriptorIndices5.y];
	RWTexture2D<uint> FrameMarker = ResourceDescriptorHeap[descriptorIndices5.z];
	RWTexture2D<float4> SpecularCompositeTexture = ResourceDescriptorHeap[descriptorIndices5.w];
	RWTexture2D<float4> ReflectanceCompositeTexture = ResourceDescriptorHeap[descriptorIndices6.x];
	RWTexture2D<float4> AlbedoCompositeTexture = ResourceDescriptorHeap[descriptorIndices6.y];
	RWTexture2D<float4> NormalCompositeTexture = ResourceDescriptorHeap[descriptorIndices6.z];
	RWTexture2D<float4> MasksCompositeTexture = ResourceDescriptorHeap[descriptorIndices6.w];
	uint evaluatorPixelIndex = dispatchThread.y * evaluatorDispatchWidth + dispatchThread.x;
	if (evaluatorID != CS_FIXED_EVALUATOR ||
		evaluatorPixelIndex >= evaluatorPixelCount)
		return;
	uint2 pixel = EvaluatorPixelList[evaluatorBaseOffset + evaluatorPixelIndex];
	uint width, height;
	CompositeTexture.GetDimensions(width, height);
	if (pixel.x >= width || pixel.y >= height)
		return;
	// Material promotion is explicit. Until the host advertises at least one
	// parity-tested class, no packed value (including output from an older or
	// non-lighting permutation) is allowed to select deferred evaluation.
	if (frameFlags == 0) {
		return;
	}
	uint4 packedPayload = PackedSurfaceTexture.Load(int3(pixel.xy, 0));
	uint packed = packedPayload.x;
	uint contextIndex = CSDeferredContextIndex(packed);
	uint materialClass = CSDeferredMaterialClass(packed);
	if (CSDeferredEvaluatorForMaterial(materialClass) != evaluatorID)
		return;
	// Pixel binning already applied the host's enabled-evaluator mask, so every
	// invocation here represents an evaluator that actually recorded work.
	// TruePBR repurposes several raster payload targets and must still execute
	// while the coverage overlay is active so its canonical downstream G-buffer
	// outputs are restored. The D3D11 overlay replaces Main after handoff.
	if ((frameFlags & 4u) != 0 && evaluatorID != CS_EVALUATOR_TRUE_PBR &&
		evaluatorID != CS_EVALUATOR_TRUE_PBR_SUBSURFACE_FUZZ &&
		evaluatorID != CS_EVALUATOR_TRUE_PBR_COAT &&
		evaluatorID != CS_EVALUATOR_TRUE_PBR_GLINT &&
		evaluatorID != CS_EVALUATOR_TRUE_PBR_TERRAIN) {
		if (contextIndex == CS_INVALID_CONTEXT) {
			CompositeTexture[pixel.xy] = float4(1, 0, 0, 1);
		} else {
			CompositeTexture[pixel.xy] = float4(CSDeferredEvaluatorDebugColor(evaluatorID), 1);
		}
		return;
	}

	// Bit zero promotes the independently validated generic classes. Bit one is
	// the developer lighting diagnostic. No future/nonzero class is admitted
	// implicitly; IsGenericMaterial is the authoritative allow-list.
	const bool staticOpaqueSelected = (frameFlags & 3u) != 0;
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
	if (lightingContext.abiVersion != frame.abiVersion) {
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
	float3 positionWS = ReconstructWorldPosition(pixel.xy, depth, frame);
	float4 normalRoughnessSample = NormalRoughnessTexture.Load(int3(pixel.xy, 0));
	float3 normalVS = DecodeCSNormal(normalRoughnessSample.xy);
	float3 normalWS = normalize(mul(viewInverse, float4(normalVS, 0.0f)).xyz);
	float3 viewDirection = ReconstructWorldViewDirection(pixel.xy, frame);

#if defined(CS_DEFERRED_TRUE_PBR)
	if (evaluatorID == CS_EVALUATOR_TRUE_PBR ||
		evaluatorID == CS_EVALUATOR_TRUE_PBR_SUBSURFACE_FUZZ ||
		evaluatorID == CS_EVALUATOR_TRUE_PBR_COAT ||
		evaluatorID == CS_EVALUATOR_TRUE_PBR_GLINT ||
		evaluatorID == CS_EVALUATOR_TRUE_PBR_TERRAIN) {
		uint materialRecordIndex = CSDeferredPBRMaterialRecord(packedPayload);
		// Raster-resolved profiles do not require a record, but when one is
		// supplied it must refer to this visual ABI. Texture-backed profiles use
		// this same check as a hard precondition before promotion.
		if (materialRecordIndex != 0xFFFFu &&
			(materialRecordIndex >= pbrMaterialCount ||
			 PBRMaterials[materialRecordIndex].abiVersion != frame.abiVersion))
			return;
		if (evaluatorID == CS_EVALUATOR_TRUE_PBR_GLINT &&
			materialRecordIndex == 0xFFFFu)
			return;
		PBRMaterialRecord pbrMaterialRecord = (PBRMaterialRecord)0;
		if (materialRecordIndex != 0xFFFFu)
			pbrMaterialRecord = PBRMaterials[materialRecordIndex];
		float4 resolvedBase = AlbedoTexture.Load(int3(pixel.xy, 0));
		float4 resolvedF0AO = SpecularTexture.Load(int3(pixel.xy, 0));
		float4 resolvedEmissive = ReflectanceTexture.Load(int3(pixel.xy, 0));
		float4 resolvedMasks = MasksTexture.Load(int3(pixel.xy, 0));
		MaterialProperties material = (MaterialProperties)0;
		material.Flags = any(resolvedEmissive.rgb != 0.0f) ? PBR::Flags::HasEmissive : 0u;
		material.BaseColor = resolvedBase.rgb;
		material.Roughness = clamp(1.0f - normalRoughnessSample.z,
			PBR::Constants::MinRoughness, PBR::Constants::MaxRoughness);
		material.AO = resolvedF0AO.a;
		material.F0 = resolvedF0AO.rgb;
		float resolvedCoverage = resolvedBase.a;
		float3x3 pbrTbnTr = (float3x3)0;
		uint payloadProfile = CSDeferredPBRPayloadProfile(packedPayload);
		uint commonPayloadAux = CSDeferredPBRPayloadAux(packedPayload);
		float3 wetnessNormalWS = normalWS;
		float3 coatNormalWS = normalWS;
		float wetnessRoughness = 1.0f;
		bool wetnessHasIndirect = false;
		float resolvedDirectionalSoftShadow = -1.0f;
		if (payloadProfile == 0u && (commonPayloadAux & 1u) != 0u) {
			wetnessNormalWS = normalWS;
			normalWS = normalize(float3(
				f16tof32(packedPayload.z & 0xFFFFu),
				f16tof32(packedPayload.z >> 16u),
				f16tof32(packedPayload.w & 0xFFFFu)));
			wetnessRoughness = f16tof32(packedPayload.w >> 16u);
			wetnessHasIndirect = (commonPayloadAux & 2u) != 0u;
		}
		if (evaluatorID == CS_EVALUATOR_TRUE_PBR_SUBSURFACE_FUZZ) {
			uint payloadAux = CSDeferredPBRPayloadAux(packedPayload);
			if ((payloadAux & 1u) != 0u) material.Flags |= PBR::Flags::Subsurface;
			if ((payloadAux & 2u) != 0u) material.Flags |= PBR::Flags::Fuzz;
			if ((payloadAux & 4u) != 0u) material.Flags |= PBR::Flags::DeferredTreeAnim;
			material.SubsurfaceColor = float3(
				f16tof32(packedPayload.z & 0xFFFFu),
				f16tof32(packedPayload.z >> 16u),
				f16tof32(packedPayload.w & 0xFFFFu));
			material.Thickness = f16tof32(packedPayload.w >> 16u);
			material.FuzzColor = resolvedMasks.yzw;
			material.FuzzWeight = resolvedEmissive.a;
			resolvedDirectionalSoftShadow = float(payloadAux >> 3u) / 31.0f;
		}
		if (evaluatorID == CS_EVALUATOR_TRUE_PBR_COAT) {
			uint payloadAux = CSDeferredPBRPayloadAux(packedPayload);
			material.Flags |= PBR::Flags::TwoLayer;
			if ((payloadAux & 1u) != 0u) material.Flags |= PBR::Flags::ColoredCoat;
			if ((payloadAux & 2u) != 0u) {
				material.Flags |= PBR::Flags::CoatNormal;
				coatNormalWS = normalize(mul(viewInverse,
					float4(DecodeCSNormal(resolvedMasks.zw), 0.0f)).xyz);
			}
			material.CoatColor = float3(
				f16tof32(packedPayload.z & 0xFFFFu),
				f16tof32(packedPayload.z >> 16u),
				f16tof32(packedPayload.w & 0xFFFFu));
			material.CoatRoughness = f16tof32(packedPayload.w >> 16u);
			material.CoatStrength = resolvedEmissive.a;
			material.CoatF0 = resolvedMasks.yyy;
		}
#if defined(GLINT)
		if (evaluatorID == CS_EVALUATOR_TRUE_PBR_GLINT) {
			material.Flags |= PBR::Flags::Glint;
			material.Metallic = resolvedBase.a;
			resolvedCoverage = normalRoughnessSample.a;
			material.Noise = resolvedMasks.y;
			material.GlintCache.uv = float2(
				f16tof32(packedPayload.z & 0xFFFFu),
				f16tof32(packedPayload.z >> 16u));
			material.GlintCache.gridSeed = packedPayload.w;
			material.GlintCache.footprintArea = resolvedEmissive.a;
			float4 glintParameters = pbrMaterialRecord.materialParameters[5];
			material.GlintScreenSpaceScale = max(1.0f, glintParameters.x);
			// Material records store the logical JSON value. The forward constant
			// buffer stores (Max-density) and subtracts it again in Lighting.hlsl;
			// using the logical value here produces the identical final parameter.
			material.GlintLogMicrofacetDensity = clamp(glintParameters.y,
				PBR::Constants::MinGlintDensity, PBR::Constants::MaxGlintDensity);
			material.GlintMicrofacetRoughness = clamp(glintParameters.z,
				PBR::Constants::MinGlintRoughness, PBR::Constants::MaxGlintRoughness);
			material.GlintDensityRandomization = clamp(glintParameters.w,
				PBR::Constants::MinGlintDensityRandomization,
				PBR::Constants::MaxGlintDensityRandomization);
			float3 tangentWS = normalize(mul(viewInverse,
				float4(DecodeCSNormal(resolvedMasks.zw), 0.0f)).xyz);
			float3 bitangentWS = normalize(cross(normalWS, tangentWS));
			if ((CSDeferredPBRPayloadAux(packedPayload) & 1u) != 0u)
				bitangentWS = -bitangentWS;
			pbrTbnTr = float3x3(tangentWS, bitangentWS, normalWS);
		}
#endif

		uint surfaceFlags = (packed >> 24u) & 7u;
		float3 directionalDirection = normalize(lightingContext.directionalLightDirection.xyz);
		float3 directionalColor = CSDeferredTransformLight(
			lightingContext.directionalLightColor.xyz / max(directionalLightScale, 1e-5f),
			isDirectionalLightLinear != 0u, lightGamma, directionalLightMultiplier,
			enableLinearLighting != 0u) * directionalLightScale * resolvedMasks.x;
		float directionalShadow =
			(frameFlags & 16u) != 0u ||
			(lightingContext.featureFlags & (CS_CONTEXT_RECEIVES_DEFERRED_SHADOW |
				CS_CONTEXT_RECEIVES_DIRECTIONAL_SHADOW)) !=
			(CS_CONTEXT_RECEIVES_DEFERRED_SHADOW | CS_CONTEXT_RECEIVES_DIRECTIONAL_SHADOW) ?
			1.0f : LocalShadowMaskTexture.Load(int3(pixel.xy, 0)).x;
		if ((frameFlags & 128u) != 0u && (lightingContext.featureFlags & 1u) != 0u &&
			dot(normalWS, directionalDirection) >= 0.0f)
			directionalShadow *= ScreenSpaceShadowTexture.Load(int3(pixel.xy, 0));

		DirectLightingOutput direct = (DirectLightingOutput)0;
		if ((surfaceFlags & 1u) != 0u) {
			float directionalSoftShadow = resolvedDirectionalSoftShadow >= 0.0f ?
				resolvedDirectionalSoftShadow : directionalShadow;
			DirectLightingOutput directional = EvaluateDeferredTruePBRDirect(material,
				normalWS, coatNormalWS, viewDirection, directionalDirection, directionalColor,
				directionalShadow, directionalSoftShadow, wetnessNormalWS, wetnessRoughness,
				pbrTbnTr, frame);
			direct.diffuse += directional.diffuse;
			direct.specular += directional.specular;
			direct.transmission += directional.transmission;
			direct.coatDiffuse += directional.coatDiffuse;
		}
		uint pbrPage = cluster.firstPage;
		uint pbrTraversedPages = 0u;
		uint pbrVisitedLights = 0u;
		while ((surfaceFlags & 2u) != 0u && pbrPage != CS_NULL_LIGHT_PAGE &&
			pbrPage < pageCapacity && pbrTraversedPages++ < 64u &&
			pbrVisitedLights < cluster.numLights) {
			LightPage lightPage = Pages[pbrPage];
			uint count = min(lightPage.numLights, 12u);
			[unroll] for (uint i = 0u; i < 12u; ++i) {
				if (i >= count || pbrVisitedLights >= cluster.numLights) break;
				uint lightIndex = lightPage.lightIndices[i];
				if (lightIndex >= lightCount) return;
				Light light = Lights[lightIndex];
				if (ContextAcceptsLight(light, lightingContext)) {
					float3 toLight = light.positionWS - positionWS;
					float distanceToLight = length(toLight);
					float attenuation = CSDeferredAttenuation(distanceToLight, light);
					if (attenuation >= 1e-5f) {
						float shadow = 1.0f;
						if ((frameFlags & 16u) == 0u &&
							(lightingContext.featureFlags & CS_CONTEXT_RECEIVES_DEFERRED_SHADOW) != 0u &&
							(light.lightFlags & CS_LIGHT_FLAG_SHADOW) != 0u && light.shadowMaskIndex < 4u)
							shadow = LocalShadowMaskTexture.Load(int3(pixel.xy, 0))[light.shadowMaskIndex];
						float3 lightColor = CSDeferredTransformLight(light.color,
							(light.lightFlags & (1u << 11)) != 0u, lightGamma,
							pointLightMultiplier, enableLinearLighting != 0u) * attenuation * light.fade;
						DirectLightingOutput local = EvaluateDeferredTruePBRDirect(material,
							normalWS, coatNormalWS, viewDirection, toLight / max(distanceToLight, 1e-6f),
							lightColor, shadow, shadow, wetnessNormalWS, wetnessRoughness,
							pbrTbnTr, frame);
						direct.diffuse += local.diffuse;
						direct.specular += local.specular;
						direct.transmission += local.transmission;
						direct.coatDiffuse += local.coatDiffuse;
					}
				}
				++pbrVisitedLights;
			}
			pbrPage = lightPage.nextPage;
		}
		if ((surfaceFlags & 2u) != 0u && ((pbrPage != CS_NULL_LIGHT_PAGE &&
			(pbrPage >= pageCapacity || pbrTraversedPages >= 64u)) ||
			pbrVisitedLights != cluster.numLights)) return;

		IndirectContext indirectContext = CreateIndirectLightingContext(normalWS, normalWS, viewDirection);
		IndirectLobeWeights indirect;
		GetIndirectLobeWeights(indirect, indirectContext, material, 0.0f);
		float3 wetnessReflectance = 0.0f;
		if (wetnessRoughness < 1.0f && wetnessHasIndirect)
			wetnessReflectance = GetWetnessIndirectLobeWeights(
				indirect, wetnessNormalWS, wetnessRoughness, indirectContext);
		float3 ambient = CSDeferredAmbient(lightingContext, normalWS,
			enableLinearLighting != 0u, ambientGamma, ambientMultiplier);
		float3 ambientAtZero = CSDeferredAmbient(lightingContext, 0.0f,
			enableLinearLighting != 0u, ambientGamma, ambientMultiplier);
		const bool interior = (lightingContext.featureFlags & 1u) == 0u;
		ambient = CSDeferredDiffuseIBL(EnvironmentIBLTexture, SkyIBLTexture, ambient,
			ambientAtZero, -normalWS, iblFlags, iblSettings0, iblSettings1, interior);
		float pbrScale = CSLightingPBRScale(enableLinearLighting != 0u);
		float3 directDiffuse = direct.diffuse * material.BaseColor;
		if ((material.Flags & PBR::Flags::ColoredCoat) != 0u)
			directDiffuse = lerp(directDiffuse,
				material.CoatColor * direct.coatDiffuse, material.CoatStrength);
		float3 candidate = directDiffuse +
			indirect.diffuse * ambient + direct.transmission + resolvedEmissive.rgb;
		candidate *= pbrScale;
		direct.specular *= pbrScale;
		float3 outputAlbedo = indirect.diffuse * pbrScale;
		float3 ambientLit = ambient * outputAlbedo;
		float vertexAO = float((packed >> 27u) & 31u) / 31.0f;
		float skylightingDiffuse = 1.0f;
		if (skylightingEnabled != 0u && !interior) {
			float4 probe = CSDeferredSampleSkylighting(SkylightingProbeTexture,
				SkylightingVisibilityTexture, positionWS, normalWS, skylightingPositionOffset,
				skylightingArrayOrigin, directionalShadow);
			skylightingDiffuse = CSDeferredSkylightDiffuse(probe, positionWS, normalWS,
				vertexAO, skylightingMinDiffuseVisibility);
			CSDeferredApplySkylighting(candidate, ambientLit, outputAlbedo,
				skylightingDiffuse, enableLinearLighting != 0u);
		}
		if ((frameFlags & 8u) != 0u) {
			uint parityBase = evaluatorID == CS_EVALUATOR_TRUE_PBR ? 64u :
				(evaluatorID == CS_EVALUATOR_TRUE_PBR_SUBSURFACE_FUZZ ? 80u :
				(evaluatorID == CS_EVALUATOR_TRUE_PBR_COAT ? 96u :
				(evaluatorID == CS_EVALUATOR_TRUE_PBR_TERRAIN ? 112u : 128u)));
			AccumulateParity(parityBase, pixel.xy, candidate, CompatibilityReferenceTexture, FrameMarker);
		}
		CompositeTexture[pixel.xy] = float4(candidate, resolvedCoverage);
		SpecularCompositeTexture[pixel.xy] = float4(direct.specular, resolvedCoverage);
		ReflectanceCompositeTexture[pixel.xy] = float4(indirect.specular + wetnessReflectance,
			resolvedCoverage);
		AlbedoCompositeTexture[pixel.xy] = float4(outputAlbedo, resolvedCoverage);
		NormalCompositeTexture[pixel.xy] = normalRoughnessSample;
		MasksCompositeTexture[pixel.xy] = float4(0.0f, 0.0f,
			CSLightingRGBToYCoCg(ambientLit).x, resolvedCoverage);
		return;
	}
#endif

	if (evaluatorID == CS_EVALUATOR_DISTANT_TREE) {
		float3 albedo = AlbedoTexture.Load(int3(pixel.xy, 0)).rgb;
		float directionalEnvironmentVisibility =
			MasksTexture.Load(int3(pixel.xy, 0)).x;
		float screenSpaceVisibility = 1.0f;
		if ((frameFlags & 128u) != 0u)
			screenSpaceVisibility = lerp(1.0f,
				ScreenSpaceShadowTexture.Load(int3(pixel.xy, 0)), 0.8f);
		float3 directional = CSDeferredTransformLight(
			lightingContext.directionalLightColor.xyz /
				max(directionalLightScale, 1e-5f),
			isDirectionalLightLinear != 0u, lightGamma,
			directionalLightMultiplier, enableLinearLighting != 0u) *
			directionalLightScale * directionalEnvironmentVisibility *
			screenSpaceVisibility * 0.5f * vanillaNormalization;
		float3 ambient = CSDeferredAmbient(lightingContext, normalWS,
			enableLinearLighting != 0u, ambientGamma, ambientMultiplier);
		float3 ambientAtZero = CSDeferredAmbient(lightingContext, 0.0f,
			enableLinearLighting != 0u, ambientGamma, ambientMultiplier);
		const bool interior = (lightingContext.featureFlags & 1u) == 0u;
		ambient = CSDeferredDiffuseIBL(EnvironmentIBLTexture, SkyIBLTexture,
			ambient, ambientAtZero, -normalWS, iblFlags, iblSettings0,
			iblSettings1, interior);
		float3 candidate = (directional + ambient) * albedo;
		CompositeTexture[pixel.xy] = float4(candidate, 1.0f);
		SpecularCompositeTexture[pixel.xy] = 0.0f;
		ReflectanceCompositeTexture[pixel.xy] = 0.0f;
		if ((frameFlags & 8u) != 0u)
			AccumulateParity(32u, pixel.xy, candidate, CompatibilityReferenceTexture, FrameMarker);
		return;
	}

	if (evaluatorID == CS_EVALUATOR_GRASS) {
		float4 grassVisibility = SpecularTexture.Load(int3(pixel.xy, 0));
		float4 grassParameters = MasksTexture.Load(int3(pixel.xy, 0));
		if (!(grassParameters.w > 0.0f)) return;
		float3 albedo = AlbedoTexture.Load(int3(pixel.xy, 0)).rgb;
		float3 directionalLightDirection = normalize(lightingContext.directionalLightDirection.xyz);
		float3 grassDirectionalColor = CSDeferredTransformLight(
			lightingContext.directionalLightColor.xyz / max(directionalLightScale, 1e-5f),
			isDirectionalLightLinear != 0u, lightGamma, directionalLightMultiplier,
			enableLinearLighting != 0u) * directionalLightScale * grassVisibility.x;
		float directionalAngle = dot(normalWS, directionalLightDirection);
		float3 diffuseIrradiance = grassDirectionalColor * grassVisibility.y *
			saturate(directionalAngle) * vanillaNormalization;
		float3 subsurfaceIrradiance = grassDirectionalColor * grassParameters.y *
			CSLightingGrassSoftMultiplier(directionalAngle, grassParameters.x) * vanillaNormalization;
		const bool complexGrass = (((packed >> 24u) & 4u) != 0u);
		float3 specular = complexGrass ?
			grassVisibility.y * CSLightingGrassSpecular(directionalLightDirection,
				viewDirection, normalWS, grassDirectionalColor, grassParameters.w) * vanillaNormalization : 0.0f;

		uint grassPage = cluster.firstPage;
		uint grassTraversedPages = 0u;
		uint grassVisitedLights = 0u;
		while (grassPage != CS_NULL_LIGHT_PAGE && grassPage < pageCapacity &&
			grassTraversedPages++ < 64u && grassVisitedLights < cluster.numLights) {
			LightPage lightPage = Pages[grassPage];
			uint count = min(lightPage.numLights, 12u);
			[unroll] for (uint i = 0u; i < 12u; ++i) {
				if (i >= count || grassVisitedLights >= cluster.numLights)
					break;
				uint lightIndex = lightPage.lightIndices[i];
				if (lightIndex >= lightCount)
					return;
				Light light = Lights[lightIndex];
				float3 toLight = light.positionWS - positionWS;
				float distanceToLight = length(toLight);
				float attenuation = CSDeferredAttenuation(distanceToLight, light);
				if (attenuation >= 1e-5f) {
					float shadow = 1.0f;
					if ((frameFlags & 16u) == 0 && (light.lightFlags & CS_LIGHT_FLAG_SHADOW) != 0u &&
						light.shadowMaskIndex < 4u)
						shadow = LocalShadowMaskTexture.Load(int3(pixel.xy, 0))[light.shadowMaskIndex];
					float3 lightColor = CSDeferredTransformLight(light.color,
						(light.lightFlags & (1u << 11)) != 0u, lightGamma, pointLightMultiplier,
						enableLinearLighting != 0u) * attenuation * light.fade * shadow;
					float3 lightDirection = toLight / max(distanceToLight, 1e-6f);
					float angle = dot(normalWS, lightDirection);
					diffuseIrradiance += lightColor * saturate(angle) * vanillaNormalization;
					subsurfaceIrradiance += lightColor *
						CSLightingGrassSoftMultiplier(angle, grassParameters.x) * vanillaNormalization;
					if (complexGrass)
						specular += CSLightingGrassSpecular(lightDirection, viewDirection, normalWS,
							lightColor, grassParameters.w) * vanillaNormalization;
				}
				++grassVisitedLights;
			}
			grassPage = lightPage.nextPage;
		}
		if ((grassPage != CS_NULL_LIGHT_PAGE &&
			(grassPage >= pageCapacity || grassTraversedPages >= 64u)) ||
			grassVisitedLights != cluster.numLights)
			return;

		float vertexAO = float((packed >> 27u) & 31u) / 31.0f;
		float3 ambient = CSDeferredAmbient(lightingContext, normalWS,
			enableLinearLighting != 0, ambientGamma, ambientMultiplier);
		float3 ambientAtZero = CSDeferredAmbient(lightingContext, 0.0f,
			enableLinearLighting != 0, ambientGamma, ambientMultiplier);
		const bool interior = (lightingContext.featureFlags & 1u) == 0u;
		ambient = CSDeferredDiffuseIBL(EnvironmentIBLTexture, SkyIBLTexture, ambient, ambientAtZero,
			-normalWS, iblFlags, iblSettings0, iblSettings1, interior);
		float skylightingShadowVisibility = grassParameters.y;
		float skylightingDiffuse = 1.0f;
		if (skylightingEnabled != 0u && !interior) {
			float4 probe = CSDeferredSampleSkylighting(SkylightingProbeTexture,
				SkylightingVisibilityTexture, positionWS, normalWS, skylightingPositionOffset,
				skylightingArrayOrigin, skylightingShadowVisibility);
			skylightingDiffuse = CSDeferredSkylightDiffuse(probe, positionWS, normalWS,
				vertexAO, skylightingMinDiffuseVisibility);
		}
		float3 candidate = diffuseIrradiance + ambient + subsurfaceIrradiance * albedo;
		candidate *= albedo;
		float3 ambientLit = ambient * albedo;
		if (skylightingEnabled != 0u && !interior)
			CSDeferredApplySkylighting(candidate, ambientLit, albedo, skylightingDiffuse,
				enableLinearLighting != 0u);
		if ((frameFlags & 8u) != 0)
			AccumulateParity(0u, pixel.xy, candidate, CompatibilityReferenceTexture, FrameMarker);
		CompositeTexture[pixel.xy] = float4(candidate, 1.0f);
		SpecularCompositeTexture[pixel.xy] = float4(specular * normalRoughnessSample.z, 1.0f);
		ReflectanceCompositeTexture[pixel.xy] = 0.0f;
		return;
	}
	float3 directIrradiance = 0.0f;
	float3 directSpecular = 0.0f;
	float glossiness = normalRoughnessSample.z;
	float3 foliageRimSoftColor = materialClass == CS_MATERIAL_FoliageSpecial ?
		SpecularTexture.Load(int3(pixel.xy, 0)).rgb : 0.0f;
	float3 foliageBackLightColor = materialClass == CS_MATERIAL_FoliageSpecial ?
		ReflectanceTexture.Load(int3(pixel.xy, 0)).rgb : 0.0f;
	uint surfaceFlags = (packed >> 24u) & 7u;
	float3 directionalColor = CSDeferredTransformLight(
		lightingContext.directionalLightColor.xyz / max(directionalLightScale, 1e-5f),
		isDirectionalLightLinear != 0, lightGamma, directionalLightMultiplier,
		enableLinearLighting != 0) * directionalLightScale * MasksTexture.Load(int3(pixel.xy, 0)).x;
	float3 directionalLightDirection = normalize(lightingContext.directionalLightDirection.xyz);
	float directionalShadow =
		(frameFlags & 16u) != 0 ||
			(lightingContext.featureFlags & (CS_CONTEXT_RECEIVES_DEFERRED_SHADOW |
				CS_CONTEXT_RECEIVES_DIRECTIONAL_SHADOW)) !=
			(CS_CONTEXT_RECEIVES_DEFERRED_SHADOW | CS_CONTEXT_RECEIVES_DIRECTIONAL_SHADOW) ?
			1.0f : LocalShadowMaskTexture.Load(int3(pixel.xy, 0)).x;
	if ((frameFlags & 128u) != 0 && (lightingContext.featureFlags & 1u) != 0 &&
		dot(normalWS, directionalLightDirection) >= 0.0f)
		directionalShadow *= ScreenSpaceShadowTexture.Load(int3(pixel.xy, 0));
	if ((surfaceFlags & 1u) != 0) {
		float directionalSoftShadow = materialClass == CS_MATERIAL_FoliageSpecial ?
			MasksTexture.Load(int3(pixel.xy, 0)).y : directionalShadow;
		CSGenericDirectLighting directionalLighting = EvaluateDeferredDirect(
			materialClass, lightingContext, normalWS, viewDirection,
			directionalLightDirection, directionalColor, directionalShadow,
			directionalSoftShadow, glossiness, foliageRimSoftColor,
			foliageBackLightColor, frame);
		directIrradiance += directionalLighting.diffuse;
		directSpecular += directionalLighting.specular;
	}

	uint page = cluster.firstPage;
	uint traversedPages = 0;
	uint visitedLights = 0;
	while ((surfaceFlags & 2u) != 0 && page != CS_NULL_LIGHT_PAGE && page < pageCapacity && traversedPages++ < 64 && visitedLights < cluster.numLights) {
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
					if ((frameFlags & 16u) == 0 &&
						(lightingContext.featureFlags & CS_CONTEXT_RECEIVES_DEFERRED_SHADOW) != 0 &&
						(light.lightFlags & CS_LIGHT_FLAG_SHADOW) != 0 && light.shadowMaskIndex < 4)
						shadow = LocalShadowMaskTexture.Load(int3(pixel.xy, 0))[light.shadowMaskIndex];
					float3 lightColor = CSDeferredTransformLight(light.color,
						(light.lightFlags & (1u << 11)) != 0, lightGamma, pointLightMultiplier,
						enableLinearLighting != 0) * attenuation * light.fade;
					CSGenericDirectLighting localLighting = EvaluateDeferredDirect(
						materialClass, lightingContext, normalWS, viewDirection,
						toLight / max(distanceToLight, 1e-6f), lightColor, shadow,
						shadow, glossiness, foliageRimSoftColor,
						foliageBackLightColor, frame);
					directIrradiance += localLighting.diffuse;
					directSpecular += localLighting.specular;
				}
			}
			++visitedLights;
		}
		page = lightPage.nextPage;
	}

	if ((surfaceFlags & 2u) != 0 && ((page != CS_NULL_LIGHT_PAGE && (page >= pageCapacity || traversedPages >= 64)) || visitedLights != cluster.numLights)) {
		CompositeTexture[pixel.xy] = float4(1, 0, 1, 1);
		return;
	}

	// Standard opaque has no feature lobes or material specular. Its resolved
	// albedo already contains the geometry path's base color and vertex color.
	// Match Lighting.hlsl's DEFERRED geometry output: Main contains direct plus
	// directional ambient diffuse in irradiance space. The later D3D11 composite
	// owns SSGI adjustment, reflections, specular composition, and final gamma.
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
		float3 ambientIrradiance = CSDeferredAmbient(lightingContext, normalWS,
			enableLinearLighting != 0, ambientGamma, ambientMultiplier);
		float3 ambientAtZero = CSDeferredAmbient(lightingContext, 0.0f,
			enableLinearLighting != 0, ambientGamma, ambientMultiplier);
		const bool interior = (lightingContext.featureFlags & 1u) == 0u;
		ambientIrradiance = CSDeferredDiffuseIBL(EnvironmentIBLTexture, SkyIBLTexture,
			ambientIrradiance, ambientAtZero, -normalWS, iblFlags, iblSettings0,
			iblSettings1, interior);
		float vertexAO = float((packed >> 27u) & 31u) / 31.0f;
		float skylightingShadowVisibility = 1.0f;
		float skylightingDiffuse = 1.0f;
		if (skylightingEnabled != 0u && !interior) {
			float4 probe = CSDeferredSampleSkylighting(SkylightingProbeTexture,
				SkylightingVisibilityTexture, positionWS, normalWS,
				skylightingPositionOffset, skylightingArrayOrigin,
				skylightingShadowVisibility);
			skylightingDiffuse = CSDeferredSkylightDiffuse(probe, positionWS,
				normalWS, vertexAO, skylightingMinDiffuseVisibility);
		}
		float3 ambientLit = ambientIrradiance * albedo;
		float3 candidate = (directIrradiance + lightingContext.emissiveColor.xyz) * albedo + ambientLit;
		if (skylightingEnabled != 0u && !interior)
			CSDeferredApplySkylighting(candidate, ambientLit, albedo,
				skylightingDiffuse, enableLinearLighting != 0u);
		SpecularCompositeTexture[pixel.xy] = float4(directSpecular, 1.0f);
		if (materialClass == CS_MATERIAL_FoliageSpecial)
			ReflectanceCompositeTexture[pixel.xy] = 0.0f;
		if ((frameFlags & 8u) != 0)
			AccumulateParity(materialClass == CS_MATERIAL_FoliageSpecial ? 48u : 16u,
				pixel.xy, candidate, CompatibilityReferenceTexture, FrameMarker);
		CompositeTexture[pixel.xy] = float4(candidate, 1.0f);
	}
}
