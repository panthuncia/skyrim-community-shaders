#include "CloudShadows.h"

#include "../I18n/I18n.h"
#include "Effects11.h"
#include "Effects11/SettingManager.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Globals.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "State.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.cloud_shadows."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudShadows::Settings,
	Opacity)

void CloudShadows::DrawSettings()
{
	const bool managedByEffects11 = globals::features::effects11.OwnsFeature(E11Handoff::Feature::CloudShadows);
	const Util::LockedSection enbLock(managedByEffects11,
		T("common.settings_managed_by_enb", "This setting is managed by Effects 11."));
	const Util::DefaultsWhileActive<Settings> shownDefaults(settings, managedByEffects11);

	ImGui::SliderFloat(T(TKEY("opacity"), "Opacity"), &settings.Opacity, 0.0f, 4.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("opacity_tooltip"),
							  "Higher values make cloud shadows darker."));
	}
}

#undef I18N_KEY_PREFIX

void CloudShadows::LoadSettings(json& o_json)
{
	settings = o_json;
}

void CloudShadows::SaveSettings(json& o_json)
{
	o_json = settings;
}

void CloudShadows::RestoreDefaultSettings()
{
	settings = {};
}

CloudShadows::Settings CloudShadows::GetCommonBufferData()
{
	if (!loaded)
		return settings;

	const bool managedByEffects11 = globals::features::effects11.OwnsFeature(E11Handoff::Feature::CloudShadows);
	Settings data = managedByEffects11 ? Settings{} : settings;

	if (managedByEffects11) {
		auto& settingManager = SettingManager::GetSingleton();
		if (settingManager.GetValue<bool>("EnableCloudShadows", "EFFECT")) {
			data.Opacity = settingManager.GetInterpolatedTimeOfDayValue("Amount", "CLOUDSHADOWS");
		} else {
			data.Opacity = 0.0f;
		}
	}

	return data;
}

void CloudShadows::FlushQueuedLayerOps()
{
	if (auto ops = ORGCloudShadows::Get().TakeLayerOps(); !ops.empty())
		ApplyLayerOps(ops);
}

void CloudShadows::ApplyLayerOps(const std::vector<ORGCloudShadows::LayerOp>& a_ops)
{
	// In graph mode only an engine-drawn face's ops get here (QueueLayerOp is fatal for the rest).
	if (!a_ops.empty())
		ORGCloudShadows::NoteD3D11Work();
	auto context = globals::d3d::context;
	float black[4] = { 0, 0, 0, 0 };
	for (const auto& op : a_ops) {
		if (op.src == ORGCloudShadows::kClearOp) {
			context->ClearRenderTargetView(cloudShadowLayerRTVs[op.dst][op.face], black);
			continue;
		}
		UINT subresource = D3D11CalcSubresource(0, op.face, cubemapMipLevels);
		context->CopySubresourceRegion(texCloudShadowLayers[op.dst]->resource.get(), subresource, 0, 0, 0,
			texCloudShadowLayers[op.src]->resource.get(), subresource, nullptr);
	}
}

void CloudShadows::ClearLayerFace(int a_layer, int a_face, LayerOpPoint a_point)
{
	const ORGCloudShadows::LayerOp op{ static_cast<std::uint8_t>(a_layer), ORGCloudShadows::kClearOp, static_cast<std::uint8_t>(a_face) };
	if (ORGCloudShadows::Get().QueueLayerOp(a_point, op))
		return;
	FlushQueuedLayerOps();
	ApplyLayerOps({ op });
}

void CloudShadows::CopyLayerFace(int a_to, int a_from, int a_face, LayerOpPoint a_point)
{
	const ORGCloudShadows::LayerOp op{ static_cast<std::uint8_t>(a_to), static_cast<std::uint8_t>(a_from), static_cast<std::uint8_t>(a_face) };
	if (ORGCloudShadows::Get().QueueLayerOp(a_point, op))
		return;
	FlushQueuedLayerOps();
	ApplyLayerOps({ op });
}

void CloudShadows::CheckResourcesSide(int side, LayerOpPoint a_point)
{
	if (!frame_checker[side].IsNewFrame())
		return;

	if (previouslyRenderedSide >= 0 && previouslyRenderedSide != side)
		PropagateToCompletion(previouslyRenderedSide, a_point);
	previouslyRenderedSide = side;

	globals::profiler->BeginPass("CloudShadows::ClearLayer");
	ClearLayerFace(0, side, a_point);
	globals::profiler->EndPass();
	renderedLayersMask[side] = 0;
}

void CloudShadows::PropagateToCompletion(int side, LayerOpPoint a_point)
{
	uint32_t mask = renderedLayersMask[side];
	unsigned long highBit;
	int fromLayer = _BitScanReverse(&highBit, mask) ? static_cast<int>(highBit) : 0;

	globals::profiler->BeginPass("CloudShadows::Propagate");
	uint32_t newLayers = mask & ~globalRenderedMask;
	if (newLayers) {
		unsigned long bit;
		uint32_t remaining = newLayers;
		while (_BitScanForward(&bit, remaining)) {
			int newLayer = static_cast<int>(bit);
			for (int otherSide = 0; otherSide < 6; otherSide++) {
				if (otherSide == side)
					continue;
				if (renderedLayersMask[otherSide] & (1u << newLayer))
					continue;
				uint32_t belowMask = renderedLayersMask[otherSide] & ((1u << newLayer) - 1);
				unsigned long nearestBit;
				int srcLayer = _BitScanReverse(&nearestBit, belowMask) ? static_cast<int>(nearestBit) : 0;
				CopyLayerFace(newLayer, srcLayer, otherSide, a_point);
				renderedLayersMask[otherSide] |= (1u << newLayer);
			}
			remaining &= ~(1u << bit);
		}
		globalRenderedMask |= mask;
	}

	if (fromLayer < kMaxCloudLayers - 1)
		CopyLayerFace(kMaxCloudLayers - 1, fromLayer, side, a_point);
	globals::profiler->EndPass();
}

void CloudShadows::NoteGraphCloudLayer(int side, int layer)
{
	if (side < 0 || side >= 6 || layer < 0 || layer >= kMaxCloudLayers)
		return;
	// As SkyShaderHacks for the engine's draw, but the layer starts empty: the graph draws the face's earlier layers into it again
	// instead of the copy from the layer below, which the graph has not drawn yet at this point of the D3D11 stream. The clears and
	// the previous face's propagation are layer operations of the SkyReflections run that draws this face, before its draws.
	CheckResourcesSide(side, LayerOpPoint::SkyReflections);
	ClearLayerFace(layer, side, LayerOpPoint::SkyReflections);
	renderedLayersMask[side] |= (1u << layer);
}

void CloudShadows::SkyShaderHacks()
{
	auto context = globals::d3d::context;

	// Outside the capture, a cloud layer reads the occlusion of the layers up to and including itself. In graph mode this
	// runs only for a cloud draw the engine makes on D3D11 (DCLF clears the flags of the draws it takes).
	if (bindLayerSelfShadow) {
		bindLayerSelfShadow = false;
		ORGCloudShadows::NoteD3D11Work();
		FlushQueuedLayerOps();
		ID3D11ShaderResourceView* selfShadowSRV = texCloudShadowLayers[currentLayerForDraw]->srv.get();
		context->PSSetShaderResources(26, 1, &selfShadowSRV);
	}

	if (!overrideSky)
		return;
	overrideSky = false;

	auto renderer = globals::game::renderer;

	auto reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

	// The draw's targets, from the engine's shadow state (the flush has just applied it; in graph mode its binds stay in the
	// engine's context handle). A reflection face is the cube target with its face as the view, and SetDirtyStates then binds that face alone.
	// The faces can be tripwire handles: DXVK's context takes what they wrap.
	using RenderGraph::Engine::Tripwire::Unwrap;
	const auto& shadowState = globals::game::shadowState->GetRuntimeData();
	ID3D11RenderTargetView* rtvs[3] = {};
	int side = -1;
	if (shadowState.cubeMapRenderTarget == static_cast<std::uint32_t>(RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS) && shadowState.cubeMapRenderTargetView < 6) {
		rtvs[0] = Unwrap(reflections.cubeSideRTV[shadowState.cubeMapRenderTargetView]);
		if (rtvs[0])
			side = static_cast<int>(shadowState.cubeMapRenderTargetView);
	}

	if (side >= 0) {
		// The engine draws this face's cloud layer on D3D11: what the graph has queued for the layers comes first.
		ORGCloudShadows::NoteD3D11Work();
		FlushQueuedLayerOps();
		CheckResourcesSide(side);

		int layer = currentLayerForDraw;

		unsigned long highBit;
		int prevLayer = _BitScanReverse(&highBit, renderedLayersMask[side]) ? static_cast<int>(highBit) : -1;

		UINT subresource = D3D11CalcSubresource(0, side, cubemapMipLevels);

		int fromLayer = std::max(prevLayer, 0);

		if (layer > 0) {
			globals::profiler->BeginPass("CloudShadows::LayerSeed");
			context->CopySubresourceRegion(
				texCloudShadowLayers[layer]->resource.get(), subresource, 0, 0, 0,
				texCloudShadowLayers[fromLayer]->resource.get(), subresource, nullptr);
			globals::profiler->EndPass();
		}

		// The layer chain is being written by this draw, so self shadowing reads last frame's composite
		ID3D11ShaderResourceView* selfShadowSRV = OcclusionSRV();
		context->PSSetShaderResources(26, 1, &selfShadowSRV);

		ID3D11RenderTargetView* captureRTVs[4] = { rtvs[0], rtvs[1], rtvs[2], cloudShadowLayerRTVs[layer][side] };
		context->OMSetRenderTargets(4, captureRTVs, nullptr);

		float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		UINT sampleMask = 0xffffffff;

		context->OMSetBlendState(cloudShadowBlendState, blendFactor, sampleMask);
		// Make the engine re-apply its own targets and blend state on the next draw
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

		auto cubemapDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS];
		ID3D11ShaderResourceView* cubemapDepthSRV = Unwrap(cubemapDepth.depthSRV);
		context->PSSetShaderResources(17, 1, &cubemapDepthSRV);

		renderedLayersMask[side] |= (1u << layer);
	}
}

int CloudShadows::FindCloudLayer(RE::BSRenderPass* Pass)
{
	auto sky = globals::game::sky;
	if (!sky || !sky->clouds)
		return -1;

	// Slots past numLayers can still hold a previous weather's cloud geometry
	const int layerCount = std::min(static_cast<int>(sky->clouds->numLayers), kMaxCloudLayers);
	for (int i = 0; i < layerCount; i++) {
		if (sky->clouds->clouds[i].get() == Pass->geometry)
			return i;
	}
	return -1;
}

void CloudShadows::ModifySky(RE::BSRenderPass* Pass)
{
	const auto cubeMapRenderTarget = globals::game::shadowState->GetRuntimeData().cubeMapRenderTarget;
	ModifySky(Pass, cubeMapRenderTarget == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS);
}

void CloudShadows::ModifySky(RE::BSRenderPass* Pass, bool a_reflectionFace)
{
	auto skyProperty = static_cast<const RE::BSSkyShaderProperty*>(Pass->shaderProperty);

	if (skyProperty->uiSkyObjectType != RE::BSSkyShaderProperty::SkyObject::SO_CLOUDS)
		return;

	int layer = FindCloudLayer(Pass);
	if (layer < 0)
		return;

	currentLayerForDraw = layer;
	occlusionClearedWithoutClouds = false;
	if (a_reflectionFace)
		overrideSky = true;
	else
		bindLayerSelfShadow = true;
}

void CloudShadows::ReflectionsPrepass()
{
	if (frameChecker.IsNewFrame()) {
		if ((globals::game::sky->mode.get() != RE::Sky::Mode::kFull) ||
			!globals::game::sky->currentClimate)
			return;

		auto context = globals::d3d::context;

		globals::profiler->BeginPass("CloudShadows::OcclusionCopy");
		// No cloud layer draws, so nothing would overwrite the last weather's occlusion
		auto clouds = globals::game::sky->clouds;
		if (!clouds || clouds->numLayers == 0) {
			if (!occlusionClearedWithoutClouds) {
				// Layer operations of the ReflectionsPrepass run that follows, before its occlusion copy.
				for (int face = 0; face < 6; ++face)
					ClearLayerFace(kMaxCloudLayers - 1, face, LayerOpPoint::ReflectionsPrepass);
				occlusionClearedWithoutClouds = true;
			}
		} else {
			occlusionClearedWithoutClouds = false;
		}

		// On the ORG path the ReflectionsPrepass anchor, which follows, copies into the graph's cubemap. It refuses only
		// outside graph mode.
		if (!ORGCloudShadows::Get().QueueOcclusionCopy()) {
			FlushQueuedLayerOps();
			context->CopyResource(OcclusionCopy()->resource.get(), texCloudShadowLayers[kMaxCloudLayers - 1]->resource.get());
		}
		globals::profiler->EndPass();

		ID3D11ShaderResourceView* srv = OcclusionSRV();
		context->PSSetShaderResources(25, 1, &srv);
		context->CSSetShaderResources(25, 1, &srv);
	}
}

Texture2D* CloudShadows::OcclusionCopy()
{
	if (texCubemapCloudOccCopy)
		return texCubemapCloudOccCopy;
	// The layers' shape and views.
	auto* firstLayer = texCloudShadowLayers[0];
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	firstLayer->srv->GetDesc(&srvDesc);
	texCubemapCloudOccCopy = new Texture2D(firstLayer->desc, "CloudShadows::CubemapCloudOccCopy");
	texCubemapCloudOccCopy->CreateSRV(srvDesc);
	// Bound for self shadowing before the first reflections prepass fills it
	FlushQueuedLayerOps();
	globals::d3d::context->CopyResource(texCubemapCloudOccCopy->resource.get(), firstLayer->resource.get());
	return texCubemapCloudOccCopy;
}

ID3D11ShaderResourceView* CloudShadows::OcclusionSRV() const
{
	if (auto* srv = ORGCloudShadows::Get().OcclusionSRV())
		return srv;
	return texCubemapCloudOccCopy ? texCubemapCloudOccCopy->srv.get() : nullptr;
}

void CloudShadows::PostPostLoad()
{
	Hooks::Install();
}

void CloudShadows::EarlyPrepass()
{
	if (previouslyRenderedSide >= 0) {
		// Layer operations of the EarlyPrepass run that follows the features' EarlyPrepass.
		PropagateToCompletion(previouslyRenderedSide, LayerOpPoint::EarlyPrepass);
		previouslyRenderedSide = -1;
	}
	globalRenderedMask = 0;
}

void CloudShadows::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	{
		auto reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};

		reflections.texture->GetDesc(&texDesc);
		reflections.SRV->GetDesc(&srvDesc);

		texDesc.Format = srvDesc.Format = DXGI_FORMAT_R8_UNORM;
		cubemapMipLevels = texDesc.MipLevels;

		for (int layer = 0; layer < kMaxCloudLayers; ++layer) {
			char name[64];
			snprintf(name, sizeof(name), "CloudShadows::Layer[%d]", layer);
			// In graph mode a graph-owned layer exported to D3D11, else a D3D11 texture.
			texCloudShadowLayers[layer] = ORGCloudShadows::MakeLayer(layer, texDesc, srvDesc, name);
			if (!texCloudShadowLayers[layer]) {
				texCloudShadowLayers[layer] = new Texture2D(texDesc, name);
				texCloudShadowLayers[layer]->CreateSRV(srvDesc);
			}

			for (int face = 0; face < 6; ++face) {
				reflections.cubeSideRTV[face]->GetDesc(&rtvDesc);
				rtvDesc.Format = texDesc.Format;
				DX::ThrowIfFailed(device->CreateRenderTargetView(texCloudShadowLayers[layer]->resource.get(), &rtvDesc, &cloudShadowLayerRTVs[layer][face]));
				Util::SetResourceName(cloudShadowLayerRTVs[layer][face], "CloudShadows::Layer[%d] RTV[%d]", layer, face);
				// Faces are only written once the engine renders them; self shadowing can read them before that
				float black[4] = { 0, 0, 0, 0 };
				context->ClearRenderTargetView(cloudShadowLayerRTVs[layer][face], black);
			}
		}

		// The render graph copies into its own occlusion cubemap when requested. The D3D11 copy is made only for the D3D11 path.
		if (!ORGCloudShadows::Get().Setup())
			OcclusionCopy();
	}
	{
		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.AlphaToCoverageEnable = false;
		blendDesc.IndependentBlendEnable = false;

		blendDesc.RenderTarget[0].BlendEnable = true;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &cloudShadowBlendState));
		Util::SetResourceName(cloudShadowBlendState, "CloudShadows::BlendState");
	}
}

void CloudShadows::Hooks::BSSkyShader_SetupMaterial::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::state->UpdateSkyShaderPermutation(Pass);
	globals::features::cloudShadows.ModifySky(Pass);
	func(This, Pass, RenderFlags);
}
