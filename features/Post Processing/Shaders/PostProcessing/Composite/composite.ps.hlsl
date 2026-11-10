/// Bloom/Flare/Glare/Exposure Composite pass
/// Combines bloom, lens flare, physical glare, and auto exposure results with the main color texture.
/// Formula: SceneColor * GlobalExposure * LocalExposure + Bloom * GlobalExposure
/// Scene and Bloom paths are kept separate so LocalExposure is applied on scene only.
/// Purkinje effect is applied after compositing on the final perceived image.
/// Uses #ifdef HAS_BLOOM / HAS_LENS_FLARE / HAS_GLARE / HAS_EXPOSURE / HAS_LOCAL_EXPOSURE to control behavior.

#include "Common/OrgResources.hlsli"
#include "PostProcessing/fullscreen.hlsli"

#include "Common/Color.hlsli"

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below: Composite::Draw binds Histogram Auto
// Exposure's and Local Exposure's constant buffers at b1 and b2 and Composite::CompositeCB at b3. The ORG chain
// (ORG/ORGComposite.cpp) gives heap handles in the push block; the exposure constants are the regions those effects
// publish in the chain's latch, and the composite constants Composite's own region. Each permutation reads only the
// entries its HAS_* defines name; the others are never read.

// HistogramAutoExposure::AutoExposureCB (its padding is never read).
struct AutoExposureConstants
{
	float2 AdaptArea;
	float2 AdaptationRange;
	float AdaptLerp;
	float ExposureCompensation;
	float PurkinjeStartEV;
	float PurkinjeMaxEV;
	float PurkinjeStrength;
};

// LocalExposure::LocalExposureCB, up to the last member read.
struct LocalExposureConstants
{
	float ManualExposure;
	float Strength;
	float HighlightContrast;
	float ShadowContrast;
	float DetailStrength;
	float BaseBlend;
	float BlurRadius;
	float MiddleGreyBias;
	float HighlightThreshold;
	float ShadowThreshold;
	float HighlightThresholdStrength;
	float ShadowThresholdStrength;
	uint InputWidth;
	uint InputHeight;
	uint BlurredWidth;
	uint BlurredHeight;
	float LogLuminanceMin;
	float LogLuminanceMax;
};

// Composite::CompositeCB: the final bloom/flare sampling parameters.
struct CompositeConstants
{
	float2 OutputSize;
	float2 FlareSize;
	float3 FlareTint;
	float FlareIntensity;
	float BloomRadius;
	float BloomIntensity;
	float2 CompositePadding;
};

// TexBloom: the accumulated bloom mip 1. TexFlare: the half-resolution ghosts and halo. TexAdaptation: the adapted
// luminance. TexLocalExposureLog: the original scene log luminance. TexLuminanceGrid: the edge-aware luminance grid.
// TexBlurredLuminance: the broad base. LinearSampler: linear clamp (local exposure and flare). BloomSampler: linear
// border (bloom).
#define COMPOSITE_RESOURCES(X)                             \
	X(SRV, Texture2D<float4>, TexColor, 0)                 \
	X(SRV, Texture2D<float4>, TexBloom, 1)                 \
	X(SRV, Texture2D<float4>, TexFlare, 2)                 \
	X(SRV, Texture2D<float4>, TexGlare, 3)                 \
	X(SRV, StructuredBuffer<float>, TexAdaptation, 4)      \
	X(SRV, Texture2D<float>, TexLocalExposureLog, 5)       \
	X(SRV, Texture3D<float2>, TexLuminanceGrid, 6)         \
	X(SRV, Texture2D<float>, TexBlurredLuminance, 7)       \
	X(SAMPLER, SamplerState, LinearSampler, 0)             \
	X(SAMPLER, SamplerState, BloomSampler, 1)              \
	X(LATCH, AutoExposureConstants, AutoExposure, 1)       \
	X(LATCH, LocalExposureConstants, LocalExposure, 2)     \
	X(LATCH, CompositeConstants, Composite, 3)
ORG_PUSH_BLOCK(COMPOSITE_RESOURCES(ORG_HANDLE))
COMPOSITE_RESOURCES(ORG_RESOURCE)

#ifdef HAS_LOCAL_EXPOSURE

float RemapBaseContrast(float centeredBase)
{
	bool isHighlight = centeredBase >= 0.0;
	float magnitude = abs(centeredBase);
	float contrast = isHighlight ? LocalExposure.HighlightContrast : LocalExposure.ShadowContrast;
	float threshold = isHighlight ? LocalExposure.HighlightThreshold : LocalExposure.ShadowThreshold;
	float thresholdStrength = isHighlight ? LocalExposure.HighlightThresholdStrength : LocalExposure.ShadowThresholdStrength;

	// Preserve tones near middle grey, then release smoothly into the selected
	// base contrast. At full threshold strength the protected shoulder persists.
	float transitionWidth = rcp(max(1.0 - thresholdStrength, 1e-3));
	float release = smoothstep(threshold, threshold + transitionWidth, magnitude);
	float protectedOffset = min(magnitude, threshold) * (1.0 - release);
	float remappedMagnitude = magnitude * contrast + protectedOffset * (1.0 - contrast);

	if (isHighlight && magnitude > threshold)
		remappedMagnitude = max(remappedMagnitude, threshold);

	return isHighlight ? remappedMagnitude : -remappedMagnitude;
}

float ComputeLocalExposure(float3 sceneColor, float baseLogLuminance, float globalExposure, float middleGreyCompensation)
{
	float logGlobalExposure = log2(max(globalExposure, 1e-5));
	float exposedBase = baseLogLuminance + logGlobalExposure;
	float middleGrey = log2(0.18 * middleGreyCompensation) + LocalExposure.MiddleGreyBias;
	float centeredBase = exposedBase - middleGrey;
	float logAdjustment = RemapBaseContrast(centeredBase) - centeredBase;

	[branch] if (LocalExposure.DetailStrength != 1.0)
	{
		float sceneLuminance = Color::RGBToLuminance(max(sceneColor, 0.0));
		float sceneLogLuminance = clamp(log2(max(sceneLuminance, exp2(LocalExposure.LogLuminanceMin))), LocalExposure.LogLuminanceMin, LocalExposure.LogLuminanceMax);
		logAdjustment += (sceneLogLuminance - baseLogLuminance) * (LocalExposure.DetailStrength - 1.0);
	}

	return exp2(clamp(logAdjustment * LocalExposure.Strength, -8.0, 8.0));
}
#endif

#ifdef HAS_EXPOSURE
// ==================== Purkinje Effect ====================
// https://www.ncbi.nlm.nih.gov/pmc/articles/PMC2630540/pdf/nihms80286.pdf
// Simulates the blue shift of human vision under low light conditions.
// Applied AFTER compositing, on the final perceived (exposed) image.

static const float4x3 RGB2LMSR_MATRIX = float4x3(
											0.31670331, 0.70299344, 0.08120592,
											0.10129085, 0.72118661, 0.12041039,
											0.01451538, 0.05643031, 0.53416779,
											0.01724063, 0.60147464, 0.40056206) *
                                        24.303;

static const float3x3 LMS2RGB_MATRIX = float3x3(
										   4.57829597, -4.48749114, 0.31554848,
										   -0.63342362, 2.03236026, -0.36183302,
										   -0.05749394, -0.09275939, 1.90172089) /
                                       24.303;

float4 RGB2LMSR(float3 c)
{
	return mul(RGB2LMSR_MATRIX, c);
}

float3 LMS2RGB(float3 c)
{
	return mul(LMS2RGB_MATRIX, c);
}

static const float3 m = float3(0.63721, 0.39242, 1.6064);
static const float K = 45.0;
static const float S = 10.0;
static const float k3 = 0.6;
static const float k5 = 0.2;
static const float k6 = 0.29;
static const float rw = 0.139;
static const float p = 0.6189;
static const float logExposure = 380.0f;
static const float K_S = K / S;

float3 PurkinjeShift(float3 c, float nightAdaptation)
{
	if (nightAdaptation < 1e-5)
		return c;

	float4 lmsr = RGB2LMSR(c * logExposure);

	float3 lmsr_w_terms = float3(k5, k5, k6) * lmsr.w;
	float3 denominator = 1 + (.33 / m) * (lmsr.xyz + lmsr_w_terms);
	float3 g = rsqrt(denominator);

	float g_x_over_m_x = g.x / m.x;
	float g_y_over_m_y = g.y / m.y;
	float g_z_over_m_z = g.z / m.z;
	float k5_lmsr_w = k5 * lmsr.w;

	float rc_gr = K_S * ((1.0 + rw * k3) * g_y_over_m_y - (k3 + rw) * g_x_over_m_x) * k5_lmsr_w;
	float rc_by = K_S * (k6 * g_z_over_m_z - k3 * (p * k5 * g_x_over_m_x + (1.0 - p) * k5 * g_y_over_m_y)) * lmsr.w;
	float rc_lm = K * (p * g_x_over_m_x + (1.0 - p) * g_y_over_m_y) * k5_lmsr_w;

	float half_rc_gr = 0.5 * rc_gr;
	float3 lms_gain = float3(-half_rc_gr + 0.5 * rc_lm, half_rc_gr + 0.5 * rc_lm, rc_by + rc_lm) * nightAdaptation;

	return LMS2RGB(lmsr.rgb + lms_gain) / logExposure;
}
#endif  // HAS_EXPOSURE

// The final upsample/mix/resolve run here to avoid full-resolution intermediates.
#ifdef HAS_BLOOM
float3 SampleBloom(float2 uv, float2 pixelSize)
{
	float3 result = 0;
	for (int x = -1; x <= 1; ++x)
		for (int y = -1; y <= 1; ++y)
			result += (1 << (!x + !y)) * 0.0625 * TexBloom.SampleLevel(BloomSampler, uv + float2(x, y) * pixelSize * Composite.BloomRadius, 0).rgb;
	return f16tof32(f32tof16(result * Composite.BloomIntensity));
}
#endif

#ifdef HAS_LENS_FLARE
float3 SampleBicubicCatmullRom(Texture2D<float4> tex, SamplerState samp, float2 uv, float2 texSize)
{
	float2 samplePos = uv * texSize;
	float2 tc = floor(samplePos - 0.5) + 0.5;
	float2 f = samplePos - tc;
	float2 f2 = f * f;
	float2 f3 = f2 * f;

	// Catmull-Rom weights
	float2 w0 = f2 - 0.5 * (f3 + f);
	float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
	float2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
	float2 w3 = 0.5 * (f3 - f2);

	// Collapse the center weights; retain the existing five-tap approximation.
	float2 w12 = w1 + w2;
	float2 tc0 = (tc - 1.0) / texSize;
	float2 tc12 = (tc + w2 / w12) / texSize;
	float2 tc3 = (tc + 2.0) / texSize;

	float3 result =
		tex.SampleLevel(samp, float2(tc12.x, tc0.y), 0).rgb * (w12.x * w0.y) +
		tex.SampleLevel(samp, float2(tc0.x, tc12.y), 0).rgb * (w0.x * w12.y) +
		tex.SampleLevel(samp, float2(tc12.x, tc12.y), 0).rgb * (w12.x * w12.y) +
		tex.SampleLevel(samp, float2(tc3.x, tc12.y), 0).rgb * (w3.x * w12.y) +
		tex.SampleLevel(samp, float2(tc12.x, tc3.y), 0).rgb * (w12.x * w3.y);

	// The 4 corner taps have very small weights, skip for performance
	return max(result, 0.0);
}

float3 SampleFlare(uint2 tid)
{
	float2 uv = (float2(tid) + 0.5) / Composite.OutputSize;
	float3 flares = SampleBicubicCatmullRom(TexFlare, LinearSampler, uv, Composite.FlareSize);
	float gradientT = saturate(distance(uv, 0.5) * 2.0);
	float3 gradient = lerp(1.0, Composite.FlareTint, gradientT);
	// Preserve the former RGBA16F mix output.
	return f16tof32(f32tof16(flares * gradient * Composite.FlareIntensity));
}
#endif

#ifdef HAS_LOCAL_EXPOSURE
float ResolveBaseLuminance(uint2 tid)
{
	float logLuminance = TexLocalExposureLog.Load(int3(tid, 0));
	float normalizedLog = saturate((logLuminance - LocalExposure.LogLuminanceMin) / (LocalExposure.LogLuminanceMax - LocalExposure.LogLuminanceMin));
	float2 uv = (float2(tid) + 0.5) / float2(LocalExposure.InputWidth, LocalExposure.InputHeight);

	uint gridWidth, gridHeight, gridDepth;
	TexLuminanceGrid.GetDimensions(gridWidth, gridHeight, gridDepth);
	float3 gridUV;
	gridUV.xy = (float2(tid) + 0.5) / (64 * float2(gridWidth, gridHeight));
	gridUV.z = (normalizedLog * (gridDepth - 1) + 0.5) / gridDepth;

	float2 gridMoments = TexLuminanceGrid.SampleLevel(LinearSampler, gridUV, 0);
	float broadBase = TexBlurredLuminance.SampleLevel(LinearSampler, uv, 0);
	float bilateralBase = broadBase;
	if (gridMoments.y >= 0.001)
		bilateralBase = lerp(LocalExposure.LogLuminanceMin, LocalExposure.LogLuminanceMax, gridMoments.x / gridMoments.y);

	// Preserve the former R16F base-luminance store.
	return f16tof32(f32tof16(lerp(bilateralBase, broadBase, LocalExposure.BaseBlend)));
}

#endif

float4 PSComposite(FullscreenTriangleVSOutput input) : SV_Target
{
	uint2 tid = uint2(input.Position.xy);

	float3 sceneColor = TexColor[tid].rgb;

	// Accumulate bloom/flare/glare contributions (separate from scene)
	float3 bloomContrib = 0;

#ifdef HAS_BLOOM
	bloomContrib += SampleBloom(input.TexCoord, fwidth(input.TexCoord));
#endif

#ifdef HAS_LENS_FLARE
	bloomContrib += SampleFlare(tid);
#endif

#ifdef HAS_GLARE
	bloomContrib += TexGlare[tid].rgb;
#endif

#ifdef HAS_EXPOSURE
	// Compute global exposure value
	float avgLuma = TexAdaptation[0];
	float globalExposure = 0.18 * AutoExposure.ExposureCompensation / clamp(avgLuma, AutoExposure.AdaptationRange.x, AutoExposure.AdaptationRange.y);

	// Formula: SceneColor * GlobalExposure * LocalExposure + Bloom * GlobalExposure
	// LocalExposure multiplier derived from the base luminance pass (1.0 if not enabled)
#	ifdef HAS_LOCAL_EXPOSURE
	float baseLogLuminance = ResolveBaseLuminance(tid);
	float localExposure = ComputeLocalExposure(sceneColor, baseLogLuminance, globalExposure, AutoExposure.ExposureCompensation);
#	else
	float localExposure = 1.0;
#	endif

	float3 result = sceneColor * globalExposure * localExposure + bloomContrib * globalExposure;

	// Purkinje effect: applied on the final perceived (exposed + composited) image
	if (AutoExposure.PurkinjeStrength > 1e-3) {
		float avgEV100 = log2(avgLuma) + 3.0;
		float mix_term = (avgEV100 - AutoExposure.PurkinjeMaxEV) / (AutoExposure.PurkinjeStartEV - AutoExposure.PurkinjeMaxEV);
		float purkinjeMix = lerp(AutoExposure.PurkinjeStrength, 0.0, saturate(mix_term));

		if (purkinjeMix > 1e-3)
			result = PurkinjeShift(result, purkinjeMix);
	}
#else
	// No global exposure
#	ifdef HAS_LOCAL_EXPOSURE
	float baseLogLuminance = ResolveBaseLuminance(tid);
	float localExposure = ComputeLocalExposure(sceneColor, baseLogLuminance, LocalExposure.ManualExposure, 1.0);
	float3 result = (sceneColor * localExposure + bloomContrib) * LocalExposure.ManualExposure;
#	else
	// No exposure at all: simple additive composite
	float3 result = sceneColor + bloomContrib;
#	endif
#endif

	return float4(result, 1);
}
