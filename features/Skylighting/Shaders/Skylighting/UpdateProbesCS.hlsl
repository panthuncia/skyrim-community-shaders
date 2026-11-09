#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/OrgResources.hlsli"
#include "Skylighting/Skylighting.hlsli"

struct DirectionalShadowLightData
{
	column_major float4x4 ShadowProj[2];
	column_major float4x4 InvShadowProj[2];
	float2 EndSplitDistances;
	float2 StartSplitDistances;
};

// The ORG build cannot read the shared constant buffers yet, so the frame's values come from a latch.
// FXC drops the latch's cbuffer: only the ORG build reads it.
// Matrices are stored as float4 rows (or columns), so no load depends on matrix packing.
struct UpdateProbesFrame
{
	float4 OcclusionViewProj[4];  // rows, as SkylightingSettings' row_major matrix
	float4 OcclusionSHBasis4Pi;
	float4 PosOffset;
	uint4 ArrayOrigin;
	int4 ValidMargin;
	float4 CameraViewProj[4];  // rows, as FrameBuffer's row_major matrix
	float4 CameraPosAdjust;
	float4 CameraData;      // SharedData::CameraData
	float4 ShadowProj[8];   // the two cascades' columns, as DirectionalShadowLightData's column_major matrices
	float4 SplitDistances;  // xy: end, zw: start
	uint4 FrameCount;       // x: SharedData::FrameCountAlwaysActive, y: whether this frame updates the probes
};

#define UPDATE_PROBES_RESOURCES(X)                           \
	X(SRV, Texture2D<unorm float>, srcOcclusionDepth, 0)     \
	X(SRV, Texture2DArray<float4>, ShadowCascadeMap, 1)      \
	X(SRV, Texture2DArray<float4>, ESRAMShadow, 3)           \
	X(UAV, RWTexture3D<sh2>, outProbeArray, 0)               \
	X(UAV, RWTexture3D<uint>, outAccumFramesArray, 1)        \
	X(UAV, RWTexture3D<uint>, outShadowBitmask, 2)           \
	X(UAV, RWTexture3D<float>, outShadowVisibility, 3)       \
	X(SAMPLER, SamplerComparisonState, comparisonSampler, 0) \
	X(LATCH, UpdateProbesFrame, Frame, 0)
ORG_PUSH_BLOCK(UPDATE_PROBES_RESOURCES(ORG_HANDLE))
UPDATE_PROBES_RESOURCES(ORG_RESOURCE)

#if !defined(CS_ORG_BINDLESS)
StructuredBuffer<DirectionalShadowLightData> DirectionalShadowLights : register(t2);
#endif

// The frame's values: the shared constant buffers under D3D11, the latch under ORG.
namespace UpdateProbes
{
#if defined(CS_ORG_BINDLESS)
	SharedData::SkylightingSettings Settings()
	{
		SharedData::SkylightingSettings settings = (SharedData::SkylightingSettings)0;
		settings.OcclusionViewProj = float4x4(Frame.OcclusionViewProj[0], Frame.OcclusionViewProj[1], Frame.OcclusionViewProj[2], Frame.OcclusionViewProj[3]);
		settings.OcclusionSHBasis4Pi = Frame.OcclusionSHBasis4Pi;
		settings.PosOffset = Frame.PosOffset;
		settings.ArrayOrigin = Frame.ArrayOrigin;
		settings.ValidMargin = Frame.ValidMargin;
		return settings;
	}

	float4x4 CameraViewProj() { return float4x4(Frame.CameraViewProj[0], Frame.CameraViewProj[1], Frame.CameraViewProj[2], Frame.CameraViewProj[3]); }
	float4 CameraPosAdjust() { return Frame.CameraPosAdjust; }
	uint FrameCountAlwaysActive() { return Frame.FrameCount.x; }
	// A frame without an update (an interior, the map menu) still runs the recorded dispatch when the epoch runs.
	bool Enabled() { return Frame.FrameCount.y != 0; }
	float GetScreenDepth(float depth) { return Frame.CameraData.w / (-depth * Frame.CameraData.z + Frame.CameraData.x); }

	DirectionalShadowLightData ShadowLight()
	{
		DirectionalShadowLightData light = (DirectionalShadowLightData)0;
		light.ShadowProj[0] = transpose(float4x4(Frame.ShadowProj[0], Frame.ShadowProj[1], Frame.ShadowProj[2], Frame.ShadowProj[3]));
		light.ShadowProj[1] = transpose(float4x4(Frame.ShadowProj[4], Frame.ShadowProj[5], Frame.ShadowProj[6], Frame.ShadowProj[7]));
		light.EndSplitDistances = Frame.SplitDistances.xy;
		light.StartSplitDistances = Frame.SplitDistances.zw;
		return light;
	}
#else
	SharedData::SkylightingSettings Settings() { return SharedData::skylightingSettings; }
	float4x4 CameraViewProj() { return FrameBuffer::CameraViewProj; }
	float4 CameraPosAdjust() { return FrameBuffer::CameraPosAdjust; }
	uint FrameCountAlwaysActive() { return SharedData::FrameCountAlwaysActive; }
	bool Enabled() { return true; }
	float GetScreenDepth(float depth) { return SharedData::GetScreenDepth(depth); }
	DirectionalShadowLightData ShadowLight() { return DirectionalShadowLights[0]; }
#endif
}

static const float3 noise3D[32] = {
	float3(0.247, -0.583, 0.891),
	float3(-0.672, 0.315, -0.428),
	float3(0.934, 0.762, -0.153),
	float3(-0.391, -0.847, 0.526),
	float3(0.618, 0.094, 0.739),
	float3(-0.825, -0.271, -0.683),
	float3(0.152, 0.968, 0.347),
	float3(0.503, -0.714, -0.592),
	float3(-0.436, 0.629, 0.814),
	float3(0.887, -0.198, 0.461),
	float3(-0.759, 0.852, -0.305),
	float3(0.321, -0.476, -0.921),
	float3(-0.094, 0.543, -0.768),
	float3(0.776, 0.418, 0.632),
	float3(-0.538, -0.695, 0.279),
	float3(0.649, -0.921, 0.186),
	float3(-0.913, 0.127, 0.574),
	float3(0.285, 0.806, -0.447),
	float3(0.471, -0.352, 0.698),
	float3(-0.627, -0.194, -0.856),
	float3(0.834, 0.591, -0.712),
	float3(-0.173, -0.968, -0.421),
	float3(0.562, 0.239, -0.785),
	float3(-0.745, 0.487, 0.316),
	float3(0.108, -0.631, 0.894),
	float3(0.926, -0.845, -0.267),
	float3(-0.384, 0.712, -0.539),
	float3(0.697, 0.163, 0.825),
	float3(-0.851, -0.429, 0.641),
	float3(0.214, 0.934, 0.372),
	float3(0.578, -0.762, -0.614),
	float3(-0.469, 0.381, 0.947)
};

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (!UpdateProbes::Enabled())
		return;

	const float fadeInThreshold = 15;
	const static sh2 unitSH = Skylighting::UNIT_SH;
	const SharedData::SkylightingSettings settings = UpdateProbes::Settings();
	uint3 cellID = ((uint3)dtid - settings.ArrayOrigin.xyz) % Skylighting::ARRAY_DIM;
	uint3 validMin = (uint3)max(0, settings.ValidMargin.xyz);
	uint3 validMax = Skylighting::ARRAY_DIM - 1 + (uint3)min(0, settings.ValidMargin.xyz);
	bool isValid = all(cellID >= validMin) && all(cellID <= validMax);  // check if the cell is newly added
	float3 cellCentreMS = cellID + 0.5 - Skylighting::ARRAY_DIM / 2;
	cellCentreMS = cellCentreMS / Skylighting::ARRAY_DIM * Skylighting::ARRAY_SIZE + settings.PosOffset.xyz;

	float3 cellCentreOS = mul(settings.OcclusionViewProj, float4(cellCentreMS, 1)).xyz;
	cellCentreOS.y = -cellCentreOS.y;
	float2 occlusionUV = cellCentreOS.xy * 0.5 + 0.5;

	if (all(occlusionUV > 0) && all(occlusionUV < 1)) {
		uint accumFrames = isValid ? (outAccumFramesArray[dtid] + 1) : 1;
		float visibility = srcOcclusionDepth.SampleCmpLevelZero(comparisonSampler, occlusionUV, cellCentreOS.z);

		sh2 occlusionSH = settings.OcclusionSHBasis4Pi * visibility;
		if (isValid) {
			float lerpFactor = rcp(accumFrames);
			sh2 prevProbeSH = unitSH;
			if (accumFrames > 1)
				prevProbeSH += (outProbeArray[dtid] - unitSH) * fadeInThreshold / min(fadeInThreshold, accumFrames - 1);  // inverse confidence
			occlusionSH = lerp(prevProbeSH, occlusionSH, lerpFactor);
		}
		occlusionSH = lerp(unitSH, occlusionSH, min(fadeInThreshold, accumFrames) / fadeInThreshold);  // confidence fade in

		outProbeArray[dtid] = occlusionSH;
		outAccumFramesArray[dtid] = accumFrames;
	} else if (!isValid) {
		outProbeArray[dtid] = unitSH;
		outAccumFramesArray[dtid] = 0;
	}

	// Shadow cascade sampling with bitmask accumulation
	float4 cellCentreCS = mul(UpdateProbes::CameraViewProj(), float4(cellCentreMS, 1));
	bool onScreen = false;
	if (cellCentreCS.w > 0) {
		float2 screenUV = (cellCentreCS.xy / cellCentreCS.w) * float2(0.5, -0.5) + 0.5;
		onScreen = all(screenUV > 0) && all(screenUV < 1);
	}

	// Only a valid projection advances the history. Clamping an out-of-range one would sample the
	// shadow map border into the history, and the jitter can cross the camera near plane.
	// The history is a shift register of the last 32 accepted samples, so a rejected sample leaves no stale bit.
	uint jitterIndex = UpdateProbes::FrameCountAlwaysActive() % 32;
	bool advanceShadowHistory = false;
	float shadowSample = 1.0;
	if (onScreen) {
		DirectionalShadowLightData shadowData = UpdateProbes::ShadowLight();

		float3 jitteredMS = cellCentreMS + noise3D[jitterIndex] * 128;
		float4 jitteredCS = mul(UpdateProbes::CameraViewProj(), float4(jitteredMS, 1));

		if (jitteredCS.w > 0) {
			float ndcDepth = jitteredCS.z / jitteredCS.w;
			float linearDepth = UpdateProbes::GetScreenDepth(ndcDepth);

			if (ndcDepth > 0 && ndcDepth < 1 && linearDepth > 0) {
				if (linearDepth >= shadowData.EndSplitDistances.y) {
					// Past the directional shadow range: fully lit
					advanceShadowHistory = true;
				} else {
					float3 positionWS = jitteredMS + UpdateProbes::CameraPosAdjust().xyz;

					uint cascadeIndex = (linearDepth > shadowData.EndSplitDistances.x) ? 1u : 0u;
					float3 positionLS = mul(shadowData.ShadowProj[cascadeIndex], float4(positionWS, 1)).xyz;
					bool inCascade = all(positionLS.xy > 0) && all(positionLS.xy < 1) && positionLS.z > 0 && positionLS.z < 1;
					// The jitter can leave cascade 0's footprint; cascade 1 covers a wider area, so try it next.
					if (!inCascade && cascadeIndex == 0) {
						cascadeIndex = 1;
						positionLS = mul(shadowData.ShadowProj[1], float4(positionWS, 1)).xyz;
						inCascade = all(positionLS.xy > 0) && all(positionLS.xy < 1) && positionLS.z > 0 && positionLS.z < 1;
					}

					if (inCascade) {
						float cascadeShadow = ShadowCascadeMap.SampleCmpLevelZero(comparisonSampler, float3(positionLS.xy, cascadeIndex), positionLS.z);
						float esramShadow = ESRAMShadow.SampleCmpLevelZero(comparisonSampler, float3(positionLS.xy, cascadeIndex), positionLS.z);
						shadowSample = min(cascadeShadow, esramShadow);

						float fade = saturate(linearDepth / shadowData.EndSplitDistances.y);
						float fadeFactor = 1.0 - pow(fade * fade, 8);
						shadowSample = lerp(1.0, shadowSample, fadeFactor);
						advanceShadowHistory = true;
					}
				}
			}
		}
	}

	if (advanceShadowHistory) {
		// Unseeded history starts fully lit to match the cleared visibility of 1.0
		uint bitmask = isValid ? outShadowBitmask[dtid] : 0xFFFFFFFFu;
		bitmask = (bitmask << 1) | (shadowSample > 0.5 ? 1u : 0u);

		outShadowBitmask[dtid] = bitmask;

		float shadow = float(countbits(bitmask)) / 32.0;
		outShadowVisibility[dtid] = shadow;
	} else if (!isValid) {
		outShadowBitmask[dtid] = 0xFFFFFFFFu;
		outShadowVisibility[dtid] = 1.0;
	}
}
