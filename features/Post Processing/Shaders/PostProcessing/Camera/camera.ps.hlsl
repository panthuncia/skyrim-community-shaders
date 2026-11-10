// Camera effects for Community Shaders
// Film grain based on https://www.shadertoy.com/view/3sGSWV (MIT License)
//
// Purely per-pixel (fisheye + chromatic aberration + grain). SV_Position at
// pixel centers is DTid + 0.5.

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below (Camera::CameraCB at b1, SharedData at b5).
// The ORG chain (ORG/ORGCamera.cpp) gives heap handles in the push block, the constants from its latch and the shared
// constants from the heap (ORG_SHARED_CONSTANTS).
#define ORG_SHARED_CONSTANTS
#include "Common/OrgResources.hlsli"

struct CameraConstants
{
	// Fisheye
	float FEFoV;
	float FECrop;

	// Chromatic aberration
	float CAStrength;

	// Noise
	float NoiseStrength;
	int NoiseType;
	float2 ScreenSize;

	bool UseFE;
};

#define CAMERA_RESOURCES(X)                    \
	X(SHARED, ConstantBuffer, Shared, 0)       \
	X(SRV, Texture2D<float4>, InputTexture, 0) \
	X(SAMPLER, SamplerState, ColorSampler, 0)  \
	X(LATCH, CameraConstants, Camera, 1)
ORG_PUSH_BLOCK(CAMERA_RESOURCES(ORG_HANDLE))

#include "PostProcessing/fullscreen.hlsli"

#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"
#include "PostProcessing/common.hlsli"

CAMERA_RESOURCES(ORG_RESOURCE)

#define BUFFER_ASPECT_RATIO Camera.ScreenSize.x / Camera.ScreenSize.y
#define ASPECT_RATIO float2(BUFFER_ASPECT_RATIO, 1.0)

// Film grain helper functions
// From Dave Hoskins: https://www.shadertoy.com/view/4djSRW
float GrainHash(float3 p3)
{
	p3 = frac(p3 * 0.1031);
	p3 += dot(p3, p3.yzx + 19.19);
	return frac((p3.x + p3.y) * p3.z);
}

// From iq: https://www.shadertoy.com/view/4sfGzS
float GrainNoise(float3 x)
{
	float3 i = floor(x);
	float3 f = frac(x);
	f = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(lerp(GrainHash(i + float3(0, 0, 0)),
						 GrainHash(i + float3(1, 0, 0)), f.x),
					lerp(GrainHash(i + float3(0, 1, 0)),
						GrainHash(i + float3(1, 1, 0)), f.x),
					f.y),
		lerp(lerp(GrainHash(i + float3(0, 0, 1)),
				 GrainHash(i + float3(1, 0, 1)), f.x),
			lerp(GrainHash(i + float3(0, 1, 1)),
				GrainHash(i + float3(1, 1, 1)), f.x),
			f.y),
		f.z);
}

// Slightly high-passed continuous value-noise
float GrainSource(float3 x, float strength, float pitch)
{
	float center = GrainNoise(x);
	float v1 = center - GrainNoise(float3(1, 0, 0) / pitch + x) + 0.5;
	float v2 = center - GrainNoise(float3(0, 1, 0) / pitch + x) + 0.5;
	float v3 = center - GrainNoise(float3(-1, 0, 0) / pitch + x) + 0.5;
	float v4 = center - GrainNoise(float3(0, -1, 0) / pitch + x) + 0.5;

	float total = (v1 + v2 + v3 + v4) * 0.25;
	return lerp(1.0, 0.5 + total, strength);
}

float2 FishEye(float2 texcoord, float FEFoV, float FECrop)
{
	float2 radiant_vector = texcoord - 0.5;
	float diagonal_length = length(ASPECT_RATIO);

	float fov_factor = Math::PI * float(FEFoV) / 360.0;

	float fit_fov = sin(atan(tan(fov_factor) * diagonal_length));
	float crop_value = lerp(1.0 + (diagonal_length - 1.0) * cos(fov_factor), diagonal_length, FECrop * pow(abs(sin(fov_factor)), 6.0));

	// Circularize radiant vector and apply cropping
	float2 cn_radiant_vector = 2.0 * radiant_vector * ASPECT_RATIO / crop_value * fit_fov;

	if (length(cn_radiant_vector) < 1.0) {
		float z = sqrt(1.0 - cn_radiant_vector.x * cn_radiant_vector.x - cn_radiant_vector.y * cn_radiant_vector.y);
		float theta = acos(z) / fov_factor;

		float2 d = normalize(cn_radiant_vector);
		texcoord = (theta * d) / (2.0 * ASPECT_RATIO) + 0.5;
	}

	return texcoord;
}

float4 main(FullscreenTriangleVSOutput input) : SV_Target
{
	uint2 DTid = uint2(input.Position.xy);
	static const float2 TEXEL_SIZE = float2(1.0f / Camera.ScreenSize.x, 1.0f / Camera.ScreenSize.y);
	float2 texcoord = (DTid.xy + 0.5f) * TEXEL_SIZE;
	float2 texcoord_clean = texcoord.xy;

	// Fisheye
	if (Camera.UseFE) {
		texcoord.xy = FishEye(texcoord_clean, Camera.FEFoV, Camera.FECrop);
	}

	float3 color = InputTexture.SampleLevel(ColorSampler, texcoord, 0).rgb;

	// Chromatic aberration
	[branch] if (Camera.CAStrength != 0.0)
	{
		color = SampleCA(InputTexture, ColorSampler, texcoord, Camera.CAStrength, 0).rgb;
	}

	// Film grain
	[branch] if (Camera.NoiseStrength != 0.0)
	{
		float2 pixelCoord = DTid.xy;
		float t = float(SharedData::FrameCount);

		static const float GRAIN_RATE = 1.0;
		static const float GRAIN_PITCH = 1.0;
		static const float GRAIN_LIFT_RATIO = 0.5;

		float3 grain;
		if (Camera.NoiseType == 1) {
			// Color grain
			float rg = GrainSource(float3(pixelCoord, floor(GRAIN_RATE * t)), Camera.NoiseStrength, GRAIN_PITCH);
			float gg = GrainSource(float3(pixelCoord, floor(GRAIN_RATE * (t + 9.0))), Camera.NoiseStrength, GRAIN_PITCH);
			float bg = GrainSource(float3(pixelCoord, floor(GRAIN_RATE * (t - 9.0))), Camera.NoiseStrength, GRAIN_PITCH);

			static const float COLOR_LEVEL = 1.0;
			float3 color_grain = float3(rg, gg, bg);
			grain = lerp(dot(color_grain, float3(0.2126, 0.7152, 0.0722)).xxx, color_grain, COLOR_LEVEL);
		} else {
			// Monochrome film grain
			static const float NEUTRAL_GRAIN_FACTOR = 1.4142135;  // sqrt(2)
			grain = GrainSource(float3(pixelCoord, floor(GRAIN_RATE * t)), Camera.NoiseStrength / NEUTRAL_GRAIN_FACTOR, GRAIN_PITCH).xxx;
		}

		color = max(lerp(color * grain, color + (grain - 1.0), GRAIN_LIFT_RATIO), 0.0);
	}

	return float4(color, 1.0f);
}
