// A static slab preview for the low-cloud layer. Several parallel density
// slices are composited along Y so the preview reads as a cloud field rather
// than one sparse section. Detail noise, local clouds, wind, evolution and
// lighting are deliberately excluded: only the authored coverage and vertical
// profile contribute.

#define CLOUD_PREVIEW_SLICES 8

// Dual-target (Common/OrgResources.hlsli). The graph reads the constants from a latch.
#include "Common/OrgResources.hlsli"

// Field: frequencyX, frequencyY, base altitude (m), top altitude (m).
// CameraSpan: cameraX (m), cameraY (m), NDF span X (m), NDF span Y (m).
// Shape: bottom spread, bottom spread height, bottom softness, bottom softness height.
// Shape2: top expansion, density scale, unused, unused.
#define CROSS_SECTION_FIELDS(M, A) \
	M(float4, Field)               \
	M(float4, CameraSpan)          \
	M(float4, Shape)               \
	M(float4, Shape2)

#define CROSS_SECTION_RESOURCES(X)                      \
	X(SRV, Texture2D<float4>, TexCloudHeight, 0)        \
	X(SRV, Texture2D<float3>, TexCloudModeling, 1)      \
	X(SRV, Texture2D<float2>, TexCloudProfileLUT, 2)    \
	X(SRV, Texture2D<float3>, TexCloudAdjustmentLUT, 3) \
	X(UAV, RWTexture2D<float4>, OutColor, 0)            \
	X(SAMPLER, SamplerState, WrapSampler, 0)            \
	X(SAMPLER, SamplerState, ClampSampler, 1)           \
	X(LATCH, CrossSectionConstants, CrossSectionCB, 0)
ORG_PUSH_BLOCK(CROSS_SECTION_RESOURCES(ORG_HANDLE))

ORG_RESOURCE(SAMPLER, SamplerState, WrapSampler, 0)
ORG_RESOURCE(SAMPLER, SamplerState, ClampSampler, 1)

ORG_RESOURCE(SRV, Texture2D<float4>, TexCloudHeight, 0)
ORG_RESOURCE(SRV, Texture2D<float3>, TexCloudModeling, 1)
ORG_RESOURCE(SRV, Texture2D<float2>, TexCloudProfileLUT, 2)
ORG_RESOURCE(SRV, Texture2D<float3>, TexCloudAdjustmentLUT, 3)

ORG_RESOURCE(UAV, RWTexture2D<float4>, OutColor, 0)

ORG_GLOBAL_LATCH_BEGIN(CrossSectionConstants, CrossSectionCB, 0)
CROSS_SECTION_FIELDS(ORG_GLOBAL_MEMBER, ORG_GLOBAL_ARRAY)
ORG_GLOBAL_LATCH_END(CrossSectionConstants, CrossSectionCB, 0)
#define ORG_GLOBAL_LATCH CrossSectionCB
CROSS_SECTION_FIELDS(ORG_GLOBAL_STATIC, ORG_GLOBAL_STATIC_ARRAY)
#undef ORG_GLOBAL_LATCH

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint width, height;
	OutColor.GetDimensions(width, height);
	if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
		return;

	const float xNorm = (dispatchThreadID.x + 0.5) / width;
	// Texture row 0 is the top of the layer.
	const float heightFraction = 1.0 - (dispatchThreadID.y + 0.5) / height;
	const float worldX = CameraSpan.x + (xNorm - 0.5) * CameraSpan.z;
	const float altitude = lerp(Field.z, Field.w, heightFraction);
	const float coverageBottomPower = 1.0 - 0.75 * saturate(Shape.x);
	const float coverageHeightRange = max(saturate(Shape.y), 0.001);
	const float bottomSoftnessPower = 2.0 + 4.0 * saturate(Shape.z);
	const float bottomWidth = 10.0 * saturate(Shape.w);
	const float topExpansion = saturate(Shape2.x);
	const float densityScale = max(Shape2.y, 0.0);

	float accumulatedAlpha = 0.0;
	[loop] for (uint slice = 0u; slice < CLOUD_PREVIEW_SLICES; ++slice)
	{
		const float slicePosition = (float(slice) + 0.5) / CLOUD_PREVIEW_SLICES;
		const float worldY = CameraSpan.y + (slicePosition - 0.5) * CameraSpan.w;
		const float2 uv = float2(worldX * Field.x + 0.5, worldY * Field.y + 0.5);

		const float3 heights = saturate(TexCloudHeight.SampleLevel(WrapSampler, uv, 0).rgb);
		const float minAltitude = lerp(Field.z, Field.w, heights.r);
		const float maxAltitude = lerp(Field.z, Field.w, heights.g);
		if (maxAltitude <= minAltitude || altitude <= minAltitude || altitude >= maxAltitude)
			continue;

		const float3 model = saturate(TexCloudModeling.SampleLevel(WrapSampler, uv, 0).rgb);
		if (model.r < 1e-6)
			continue;

		const float localHeight = saturate((altitude - minAltitude) / (maxAltitude - minAltitude));
		const float topType = model.g * saturate(model.r * 10.0);
		const float bottomType = model.b;
		const float top = TexCloudProfileLUT.SampleLevel(ClampSampler, float2(topType, localHeight), 0).g;
		const float expandedTop = TexCloudAdjustmentLUT.SampleLevel(ClampSampler, float2(topType, localHeight), 0).r;
		const float bottom = TexCloudProfileLUT.SampleLevel(ClampSampler, float2(bottomType, localHeight), 0).r;
		const float verticalProfile = max(lerp(top, max(top, expandedTop), topExpansion) * bottom, 0.0);
		const float heightFade = saturate(localHeight / coverageHeightRange);
		const float exponent = lerp(lerp(coverageBottomPower, 1.0, heightFade), 1.0, heightFade);
		const float dimensionProfile = verticalProfile * pow(model.r, exponent);
		if (dimensionProfile <= 1e-8)
			continue;

		// Smooth base shaping, without any small-scale noise.
		const float baseFade = pow(saturate(localHeight), 0.3) *
		                       pow(saturate(localHeight * (10.0 - saturate((bottomWidth - 1.0) * 0.11111111) * 9.0)), bottomSoftnessPower);
		const float density = dimensionProfile * baseFade * densityScale;
		const float sliceAlpha = saturate(density * 0.6);
		accumulatedAlpha += (1.0 - accumulatedAlpha) * sliceAlpha;
	}

	OutColor[dispatchThreadID.xy] = float4(1.0, 1.0, 1.0, accumulatedAlpha);
}
