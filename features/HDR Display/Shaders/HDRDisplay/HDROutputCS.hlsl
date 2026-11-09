/**
 * @file HDROutputCS.hlsl
 * @brief HDR: gamma 2.2 decode, paper-white × (nits/203), BT.2020, PQ. SDR: passthrough + UI.
 */

// Dual-target (Common/OrgResources.hlsli). D3D11 binds the registers below. The ORG pass
// (ORGHDRDisplay.cpp) gives heap handles in the push block, the shared constant buffers' too
// (ORG_SHARED_CONSTANTS), and the per-frame values as a latch region.
#define ORG_SHARED_CONSTANTS
#include "Common/OrgResources.hlsli"

// HDRDisplay::HDRDataCB.
struct HDROutputConstants
{
	float enableHDR;
	float paperWhite;
	float peakNits;
	float hdrPad0;
	float uiBrightness;
	float isSceneLinear;
	float isMainOrLoadingMenu;
	float fgTweenMenuMidAlphaBoost;  ///< TweenMenu: soften AA band when compositing here (UIBrightnessCS skips while paused)
	float previewSDR;                ///< 1.0 = emit gamma 2.2 SDR (crop preview) instead of PQ HDR10
	float applyAutoHDR;              ///< 1.0 = expand SDR tonemap (Effects11 or PP SDR+AutoHDR) into HDR
};

#define HDR_OUTPUT_RESOURCES(X)               \
	X(SHARED, ConstantBuffer, Shared, 0)      \
	X(SRV, Texture2D<float4>, SceneTex, 0)    \
	X(SRV, Texture2D<float4>, UITex, 1)       \
	X(UAV, RWTexture2D<float4>, HDROutput, 0) \
	X(LATCH, HDROutputConstants, PerFrame, 0)
ORG_PUSH_BLOCK(HDR_OUTPUT_RESOURCES(ORG_HANDLE))

#include "Common/Color.hlsli"
#include "Common/DisplayMapping.hlsli"
#include "Common/SharedData.hlsli"
HDR_OUTPUT_RESOURCES(ORG_RESOURCE)

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	uint width, height;
	HDROutput.GetDimensions(width, height);
	if (dispatchID.x >= width || dispatchID.y >= height)
		return;

	float4 scene = SceneTex[dispatchID.xy];
	float4 ui = UITex[dispatchID.xy];

	bool hdrEnabled = PerFrame.enableHDR > 0.5;
	bool isMainLoading = PerFrame.isMainOrLoadingMenu > 0.5;
	bool postProcessOutput = SharedData::TonemapOwner == SharedData::TonemapOwnerPostProcessing && !isMainLoading;

	float3 finalColor;

	if (hdrEnabled) {
		bool sceneIsLinear = PerFrame.isSceneLinear > 0.5 || postProcessOutput;

		if (PerFrame.applyAutoHDR > 0.5) {
			float3 outputColor = sceneIsLinear ? scene.xyz : TransferFunctions::SignedGamma22ToLinear(scene.xyz);
			outputColor = DisplayMapping::PumboAutoHDR(outputColor, PerFrame.peakNits, PerFrame.paperWhite, 2.75, 1.0);
			scene.xyz = sceneIsLinear ? outputColor : TransferFunctions::LinearToSignedGamma22(outputColor);
		}

		float3 compositedColorLinear;

		if (sceneIsLinear) {
			float3 sceneLinear = max(0.0, scene.rgb);
			float3 uiLinear = TransferFunctions::Gamma22ToLinear(max(0.0, ui.rgb));
			if (!isMainLoading) {  // UI and scene can't be separated in main menu or loading screen
				// scale UI brightness (multiplier based on paperWhite)
				uiLinear *= PerFrame.uiBrightness;
			}
			if (postProcessOutput) {
				compositedColorLinear = Color::BT709ToBT2020(uiLinear) + sceneLinear * (1.0 - ui.a);
			} else {
				compositedColorLinear = uiLinear + sceneLinear * (1.0 - ui.a);
			}
		} else {
			float3 sceneGamma22 = scene.rgb;
			float3 uiGamma22 = ui.rgb;
			if (!isMainLoading) {  // UI and scene can't be separated in main menu or loading screen
				// scale UI brightness (multiplier based on paperWhite)
				float3 uiLinear = TransferFunctions::Gamma22ToLinear(max(0, uiGamma22));
				uiLinear *= PerFrame.uiBrightness;
				uiGamma22 = TransferFunctions::LinearToGamma22(uiLinear);
			}
#if 0
            if (PerFrame.fgTweenMenuMidAlphaBoost > 0.5 && ui.a > 1e-3) {
                float midBand = smoothstep(0.3, 0.35, ui.a) * (1.0 - smoothstep(0.55, 0.6, ui.a));
                const float fgMidAlphaBoost = 0.12;
                ui.a = saturate(ui.a + midBand * fgMidAlphaBoost);
            }
#endif

			float3 compositedColorGamma22 = uiGamma22 + sceneGamma22 * (1.0 - ui.a);

			// Non-LL path: ISHDR output is gamma 2.2-encoded at this stage.
			compositedColorLinear = TransferFunctions::SignedGamma22ToLinear(compositedColorGamma22);
		}

		if (PerFrame.previewSDR > 0.5) {
			// Crop preview lives in the SDR menu buffer: emit gamma 2.2 instead of PQ.
			if (postProcessOutput)
				compositedColorLinear = Color::BT2020ToBT709(compositedColorLinear);
			finalColor = saturate(TransferFunctions::LinearToGamma22(max(0.0, compositedColorLinear)));
		} else {
			if (!postProcessOutput)
				compositedColorLinear = Color::BT709ToBT2020(compositedColorLinear);
			finalColor = Color::pq::Encode(max(0.0, compositedColorLinear), PerFrame.paperWhite);

			finalColor = saturate(finalColor);
		}
	} else {
		float3 sceneGamma22 = scene.rgb;
		finalColor = ui.rgb + sceneGamma22 * (1.0 - ui.a);
		finalColor = saturate(finalColor);
	}

	HDROutput[dispatchID.xy] = float4(finalColor, 1.0);
}
