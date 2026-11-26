#include "VanillaBinding.h"
#include "Snapshot/EngineReads.h"

#include "Globals.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"

#include <winrt/base.h>

namespace VanillaBinding
{
	void BindSharedConstantBuffers()
	{
		auto* state = globals::state;
		auto* context = globals::d3d::context;
		ID3D11Buffer* buffers[] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
		context->PSSetConstantBuffers(4, static_cast<UINT>(std::size(buffers)), buffers);
		context->CSSetConstantBuffers(5, 2, buffers + 1);
	}

	namespace
	{
		// Held like the context's slot holds it, so the view lives while a graph pass may still look it up.
		winrt::com_ptr<ID3D11ShaderResourceView> lastSceneDepth;
	}

	void BindSceneDepth()
	{
		auto* srv = Util::GetCurrentSceneDepthSRV(true);
		lastSceneDepth.copy_from(srv);
		globals::d3d::context->PSSetShaderResources(17, 1, &srv);
	}

	ID3D11ShaderResourceView* LastSceneDepth()
	{
		return lastSceneDepth.get();
	}

	void BindSkyResources(const RE::BSRenderPass* a_pass)
	{
		if (!a_pass || !a_pass->shaderProperty)
			return;

		auto* state = globals::state;
		auto* context = globals::d3d::context;
		// The Sky vertex shader uses the per-draw Sun, Moon and Secunda flags for billboard sizing.
		auto* permutationBuffer = state->permutationCB->CB();
		context->VSSetConstantBuffers(4, 1, &permutationBuffer);

		const auto* skyProperty = static_cast<const RE::BSSkyShaderProperty*>(a_pass->shaderProperty);
		if (skyProperty->uiSkyObjectType != RE::BSSkyShaderProperty::SkyObject::SO_SUN_GLARE)
			return;

		auto* sharedBuffer = state->sharedDataCB->CB();
		context->VSSetConstantBuffers(5, 1, &sharedBuffer);
		// The sky draws with the z-prepass copy as its DSV, which would null an SRV of it; kMAIN holds the same depth
		// Engine target fields can be tripwire handles: DXVK's context takes the wrapped view.
		auto* depthSRV = RenderGraph::Engine::Tripwire::Unwrap(CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV);
		context->VSSetShaderResources(17, 1, &depthSRV);
	}

	void BindDrawResources()
	{
		if (!globals::shaderCache->IsEnabled())
			return;

		const auto* state = globals::state;
		const auto* shader = state->currentShader;
		if (!state->customPixelShader || !shader || shader->shaderType.get() != RE::BSShader::Type::ImageSpace)
			return;

		if (std::string_view(static_cast<const RE::BSImagespaceShader*>(shader)->name) == "BSImagespaceShaderReflectionsRayTracing") {
			auto* reflections = RenderGraph::Engine::Tripwire::Unwrap(CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS].SRV);
			globals::d3d::context->PSSetShaderResources(4, 1, &reflections);
			CS_ENGINE_READ(ShadowState, RecordTime, globals::game::shadowState)->GetRuntimeData().PSResourceModifiedBits |= (1u << 4);
		}
	}
}
