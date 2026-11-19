// Dual-target (Common/OrgResources.hlsli). The graph reads the parameters from a latch.
#include "Common/OrgResources.hlsli"

#define LOCAL_NDF_FIELDS(M, A) \
	M(float4, fieldRect)       \
	M(float4, centerSize)      \
	M(float4, rotationWeights) \
	M(float4, heightFeather)   \
	M(float4, altitude)        \
	M(uint, hasMask)           \
	M(uint, modelingAlpha)     \
	M(uint, dimension)         \
	M(uint, modelingBlend)

#define LOCAL_NDF_RESOURCES(X)                          \
	X(SRV, Texture2D<float2>, HeightInput, 0)           \
	X(SRV, Texture2D<float4>, ModelingInput, 1)         \
	X(SRV, Texture2D<float>, MaskInput, 2)              \
	X(UAV, RWTexture2DArray<float4>, HeightOutput, 0)   \
	X(UAV, RWTexture2DArray<float4>, ModelingOutput, 1) \
	X(UAV, RWTexture2DArray<float4>, MaximumOutput, 2)  \
	X(SAMPLER, SamplerState, ClampSampler, 0)           \
	X(LATCH, LocalNdfConstants, LocalNdfParameters, 1)  \
	X(EXTERN, uint, OutputSlice, 0)
ORG_PUSH_BLOCK(LOCAL_NDF_RESOURCES(ORG_HANDLE))

ORG_GLOBAL_LATCH_BEGIN(LocalNdfConstants, LocalNdfParameters, 1)
LOCAL_NDF_FIELDS(ORG_GLOBAL_MEMBER, ORG_GLOBAL_ARRAY)
ORG_GLOBAL_LATCH_END(LocalNdfConstants, LocalNdfParameters, 1)
#define ORG_GLOBAL_LATCH LocalNdfParameters
LOCAL_NDF_FIELDS(ORG_GLOBAL_STATIC, ORG_GLOBAL_STATIC_ARRAY)
#undef ORG_GLOBAL_LATCH

ORG_RESOURCE(SRV, Texture2D<float2>, HeightInput, 0)
ORG_RESOURCE(SRV, Texture2D<float4>, ModelingInput, 1)
ORG_RESOURCE(SRV, Texture2D<float>, MaskInput, 2)
ORG_RESOURCE(SAMPLER, SamplerState, ClampSampler, 0)
ORG_RESOURCE(UAV, RWTexture2DArray<float4>, HeightOutput, 0)
ORG_RESOURCE(UAV, RWTexture2DArray<float4>, ModelingOutput, 1)
ORG_RESOURCE(UAV, RWTexture2DArray<float4>, MaximumOutput, 2)

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	if (any(pixel >= dimension))
		return;
#if defined(CS_ORG_BINDLESS)
	// The graph writes a whole-array view, so the push block names the layer's slice.
	const uint3 target = uint3(pixel, OrgHandle_OutputSlice);
#else
	const uint3 target = uint3(pixel, 0);
#endif
	HeightOutput[target] = 0.0;
	ModelingOutput[target] = 0.0;
	MaximumOutput[target] = 0.0;
	const float2 world = fieldRect.xy + ((pixel + 0.5) / dimension) / fieldRect.zw;
	const float2 delta = world - centerSize.xy;
	const float2 local = float2(dot(delta, rotationWeights.xy), dot(delta, float2(-rotationWeights.y, rotationWeights.x)));
	const float2 uv = local / centerSize.zw + 0.5;
	if (any(uv <= 0.0) || any(uv >= 1.0))
		return;
	uint width, height;
	ModelingInput.GetDimensions(width, height);
	const float2 footprint = float2(width, height) / centerSize.zw / (dimension * fieldRect.zw);
	const float mip = max(log2(max(footprint.x, footprint.y)), 0.0);
	const float4 model = saturate(ModelingInput.SampleLevel(ClampSampler, uv, mip));
	float mask = 1.0;
	if (hasMask != 0u) {
		MaskInput.GetDimensions(width, height);
		const float2 maskFootprint = float2(width, height) / centerSize.zw / (dimension * fieldRect.zw);
		mask *= saturate(MaskInput.SampleLevel(ClampSampler, uv, max(log2(max(maskFootprint.x, maskFootprint.y)), 0.0)));
	}
	const float2 edge = min(uv, 1.0 - uv) * centerSize.zw;
	mask *= heightFeather.z > 0.0 ? smoothstep(0.0, heightFeather.z, min(edge.x, edge.y)) : 1.0;
	const float alpha = modelingAlpha != 0u ? model.a : 1.0;
	const float heightWeight = mask * alpha * rotationWeights.w;
	if (heightWeight > 0.0) {
		const float2 heights = saturate(HeightInput.SampleLevel(ClampSampler, uv, mip));
		const float2 normalizedHeight = (heightFeather.x + heights * heightFeather.y - altitude.x) / max(altitude.y, 1.0);
		HeightOutput[target] = float4(normalizedHeight * heightWeight, heightWeight, 0.0);
	}
	if (modelingBlend != 0u) {
		const float weight = mask * rotationWeights.z;
		MaximumOutput[target] = float4(model.rgb * weight, 0.0);
	} else {
		const float weight = mask * alpha * rotationWeights.z;
		ModelingOutput[target] = float4(model.rgb * weight, weight);
	}
}
