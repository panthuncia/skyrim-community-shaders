#include "ScreenSpaceShadows.h"

#include "Features/ScreenSpaceShadows/ORGScreenSpaceShadows.h"
#include "Features/TerrainBlending.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.screen_space_shadows."

#pragma warning(push)
#pragma warning(disable: 4838 4244)
#include "ScreenSpaceShadows/bend_sss_cpu.h"
#pragma warning(pop)

using RE::RENDER_TARGETS;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceShadows::BendSettings,
	Enable,
	SampleCount,
	SurfaceThickness,
	BilinearThreshold,
	ShadowContrast)

void ScreenSpaceShadows::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("general"), "General"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable"), "Enable"), (bool*)&bendSettings.Enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("enable_tooltip"), "Enable screen-space contact shadows from the sun/moon direction."));

		ImGui::SliderInt(T(TKEY("sample_count"), "Sample Count Multiplier"), (int*)&bendSettings.SampleCount, 1, 4, "%d", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("sample_count_tooltip"), "Multiplier for shadow ray sample count. Higher values increase shadow reach at the cost of performance. Adapts to render resolution."));

		ImGui::SliderFloat(T(TKEY("surface_thickness"), "Surface Thickness"), &bendSettings.SurfaceThickness, 0.005f, 0.05f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("surface_thickness_tooltip"), "Assumed thickness of surfaces for shadow detection. Lower values produce thinner, more precise shadows."));

		ImGui::SliderFloat(T(TKEY("bilinear_threshold"), "Bilinear Threshold"), &bendSettings.BilinearThreshold, 0.02f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("bilinear_threshold_tooltip"), "Depth threshold for edge detection during bilinear interpolation. Higher values smooth more aggressively across edges."));

		ImGui::SliderFloat(T(TKEY("shadow_contrast"), "Shadow Contrast"), &bendSettings.ShadowContrast, 0.0f, 4.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shadow_contrast_tooltip"), "Contrast boost for the shadow transition. Higher values produce harder shadow edges."));

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

void ScreenSpaceShadows::InvalidateRaymarchShaders()
{
	if (raymarchCS) {
		raymarchCS->Release();
		raymarchCS = nullptr;
	}
}

void ScreenSpaceShadows::ClearShaderCache()
{
	InvalidateRaymarchShaders();
}

uint ScreenSpaceShadows::GetScaledSampleCount()
{
	float2 renderSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });

	// Scale sample count based on both dimensions relative to 1920x1080 reference
	float2 referenceRes = { 1920.0f, 1080.0f };
	float referenceArea = referenceRes.x * referenceRes.y;
	float currentArea = renderSize.x * renderSize.y;
	float areaScale = std::sqrt(currentArea / referenceArea);
	uint scaledSampleCount = static_cast<uint>(std::round(std::clamp(bendSettings.SampleCount, 1u, 4u) * 60 * areaScale));

	// Quantize to steps of 8 to prevent frequent recompilation from small DRS oscillations
	scaledSampleCount = ((scaledSampleCount + 7u) / 8u) * 8u;
	scaledSampleCount = std::max(scaledSampleCount, 8u);

	return scaledSampleCount;
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeRaymarch()
{
	uint scaledSampleCount = GetScaledSampleCount();

	if (scaledSampleCount != lastCompiledSampleCount) {
		lastCompiledSampleCount = scaledSampleCount;
		InvalidateRaymarchShaders();
	}

	if (!raymarchCS) {
		auto sampleCount = std::format("{}", scaledSampleCount);
		std::vector<std::pair<const char*, const char*>> defines{ { "SAMPLE_COUNT", sampleCount.c_str() } };
		// TERRAIN_BLENDING flips DepthTexture's HLSL type from `Texture2D<unorm float>`
		// (R24_UNORM_X8_TYPELESS game depth) to `Texture2D<float>` (R32_FLOAT blendedDepth).
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ "TERRAIN_BLENDING", "" });
		raymarchCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", defines, "cs_5_0");
	}
	return raymarchCS;
}

bool ScreenSpaceShadows::ShouldDraw() const
{
	auto sky = globals::game::sky;
	return sky && bendSettings.Enable && sky->mode.get() == RE::Sky::Mode::kFull;
}

ScreenSpaceShadows::Dispatches ScreenSpaceShadows::BuildDispatches() const
{
	auto accumulator = *globals::game::currentAccumulator.get();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	auto& directionNi = dirLight->GetWorldDirection();
	float3 light = { directionNi.x, directionNi.y, directionNi.z };
	light.Normalize();
	float4 lightProjection = float4(-light.x, -light.y, -light.z, 0.0f);

	auto viewProjMat = globals::game::frameBufferCached.GetCameraViewProj().Transpose();
	auto projectedLight = DirectX::SimpleMath::Vector4::Transform(lightProjection, viewProjMat);
	float lightProjectionF[4] = { projectedLight.x, projectedLight.y, projectedLight.z, projectedLight.w };

	float2 renderSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });
	int viewportSize[2] = { (int)renderSize.x, (int)renderSize.y };

	int minRenderBounds[2] = { 0, 0 };
	int maxRenderBounds[2] = { viewportSize[0], viewportSize[1] };

	auto viewport = globals::game::graphicsState;
	float2 dynamicRes = { viewport->GetRuntimeData().dynamicResolutionWidthRatio, viewport->GetRuntimeData().dynamicResolutionHeightRatio };

	auto dispatchList = Bend::BuildDispatchList(lightProjectionF, viewportSize, minRenderBounds, maxRenderBounds);

	Dispatches dispatches;
	dispatches.count = static_cast<uint32_t>(std::clamp(dispatchList.DispatchCount, 0, static_cast<int>(Dispatches::kMax)));
	for (uint32_t i = 0; i < dispatches.count; i++) {
		auto& dispatchData = dispatchList.Dispatch[i];
		auto& data = dispatches.constants[i];
		data.LightCoordinate[0] = dispatchList.LightCoordinate_Shader[0];
		data.LightCoordinate[1] = dispatchList.LightCoordinate_Shader[1];
		data.LightCoordinate[2] = dispatchList.LightCoordinate_Shader[2];
		data.LightCoordinate[3] = dispatchList.LightCoordinate_Shader[3];

		data.WaveOffset[0] = dispatchData.WaveOffset_Shader[0];
		data.WaveOffset[1] = dispatchData.WaveOffset_Shader[1];

		data.FarDepthValue = 1.0f;
		data.NearDepthValue = 0.0f;

		data.DynamicRes = dynamicRes;

		data.InvDepthTextureSize[0] = 1.0f / (float)viewportSize[0];
		data.InvDepthTextureSize[1] = 1.0f / (float)viewportSize[1];

		data.settings = bendSettings;

		for (int axis = 0; axis < 3; axis++)
			dispatches.groups[i][axis] = static_cast<uint32_t>(dispatchData.WaveCount[axis]);
	}
	return dispatches;
}

void ScreenSpaceShadows::DrawShadows()
{
	ZoneScopedS(8);
	TracyD3D11Zone(globals::state->tracyCtx, "Screen Space Shadows");

	auto context = globals::d3d::context;

	// Setup common render state.
	// SSS always uses 24/32-bit depth, never the R16_UNORM half-precision path.
	// With TerrainBlending loaded the SRV is R32_FLOAT (blendedDepthTexture);
	// without it, the game's kPOST_ZPREPASS_COPY (R24_UNORM_X8_TYPELESS).
	// The shader's DepthTexture declaration is conditional on TERRAIN_BLENDING:
	// `<float>` for the R32_FLOAT path, `<unorm float>` for the R24_UNORM path.
	auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
	context->CSSetShaderResources(0, 1, &depthSRV);

	auto uav = screenSpaceShadowsTexture->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	context->CSSetSamplers(0, 1, &pointBorderSampler);

	auto buffer = raymarchCB->CB();
	context->CSSetConstantBuffers(1, 1, &buffer);

	globals::profiler->BeginPass("ScreenSpaceShadows::RayMarch");

	if (globals::state->frameAnnotations) {
		globals::state->BeginPerfEvent("SSS - Ray March");
	}

	context->CSSetShader(GetComputeRaymarch(), nullptr, 0);

	const auto dispatches = BuildDispatches();
	for (uint32_t i = 0; i < dispatches.count; i++) {
		{
			TracyD3D11Zone(globals::state->tracyCtx, "SSS - Dispatch CB");
			raymarchCB->Update(dispatches.constants[i]);
		}

		{
			TracyD3D11Zone(globals::state->tracyCtx, "SSS - Dispatch Sweep");
			context->Dispatch(dispatches.groups[i][0], dispatches.groups[i][1], dispatches.groups[i][2]);
		}
	}

	if (globals::state->frameAnnotations) {
		globals::state->EndPerfEvent();
	}

	globals::profiler->EndPass();

	ID3D11ShaderResourceView* views[1]{ nullptr };
	context->CSSetShaderResources(0, 1, views);

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11SamplerState* sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);

	buffer = nullptr;
	context->CSSetConstantBuffers(1, 1, &buffer);
}

void ScreenSpaceShadows::PostPostLoad()
{
	ORGScreenSpaceShadows::Reserve();
}

void ScreenSpaceShadows::Prepass()
{
	auto context = globals::d3d::context;

	// The ORG path: its passes run in the Prepass anchor's epoch, right after the features' Prepass.
	if (auto* view = ORGScreenSpaceShadows::Get().Prepass()) {
		context->PSSetShaderResources(45, 1, &view);
		return;
	}

	EnsureShadowTexture();

	float white[4] = { 1, 1, 1, 1 };
	context->ClearUnorderedAccessViewFloat(screenSpaceShadowsTexture->uav.get(), white);

	if (ShouldDraw())
		DrawShadows();

	auto view = screenSpaceShadowsTexture->srv.get();
	context->PSSetShaderResources(45, 1, &view);
}

void ScreenSpaceShadows::LoadSettings(json& o_json)
{
	bendSettings = o_json;
}

void ScreenSpaceShadows::SaveSettings(json& o_json)
{
	o_json = bendSettings;
}

void ScreenSpaceShadows::RestoreDefaultSettings()
{
	bendSettings = {};
}

bool ScreenSpaceShadows::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}

void ScreenSpaceShadows::SetupResources()
{
	raymarchCB = new ConstantBuffer(ConstantBufferDesc<RaymarchCB>(), "ScreenSpaceShadows::RaymarchCB");

	{
		auto device = globals::d3d::device;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		samplerDesc.BorderColor[0] = 1.0f;
		samplerDesc.BorderColor[1] = 1.0f;
		samplerDesc.BorderColor[2] = 1.0f;
		samplerDesc.BorderColor[3] = 1.0f;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointBorderSampler));
		Util::SetResourceName(pointBorderSampler, "ScreenSpaceShadows::PointBorderSampler");
	}

	{
		auto renderer = globals::game::renderer;
		auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];

		shadowMask.texture->GetDesc(&shadowTextureDesc);
		shadowMask.SRV->GetDesc(&shadowTextureSrvDesc);

		// Store normal front-facing visibility and the reversed depth-offset
		// visibility used by back-facing SSS/transmission receivers.
		shadowTextureDesc.Format = DXGI_FORMAT_R8G8_UNORM;
		shadowTextureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		shadowTextureSrvDesc.Format = shadowTextureDesc.Format;

		// The ORG path's texture has the same description. The D3D11 one is made only when the D3D11 path runs.
		ORGScreenSpaceShadows::Get().Setup(shadowTextureDesc, {});
	}
}

void ScreenSpaceShadows::EnsureShadowTexture()
{
	if (screenSpaceShadowsTexture)
		return;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
		.Format = shadowTextureDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};
	screenSpaceShadowsTexture = new Texture2D(shadowTextureDesc, "ScreenSpaceShadows::ShadowTexture");
	screenSpaceShadowsTexture->CreateSRV(shadowTextureSrvDesc);
	screenSpaceShadowsTexture->CreateUAV(uavDesc);
}
#undef I18N_KEY_PREFIX
