#include "DeferredShading.h"

#include "Features/Effects11/D3D11StateBackup.h"
#include "RenderGraph/DX12RenderRuntime.h"
#include "Deferred.h"
#include "Features/DeferredRendering.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <ORGModuleServices/ShaderCompiler.h>
#include <ORGModuleServices/PipelineService.h>

#include <fstream>

namespace
{
	struct InputDefinition
	{
		RE::RENDER_TARGET target;
		const char* id;
	};

	constexpr InputDefinition kInputs[]{
		{ ALBEDO, "community-shaders.deferred-shading.gbuffer.albedo" },
		{ SPECULAR, "community-shaders.deferred-shading.gbuffer.specular" },
		{ REFLECTANCE, "community-shaders.deferred-shading.gbuffer.reflectance" },
		{ NORMALROUGHNESS, "community-shaders.deferred-shading.gbuffer.normal-roughness" },
		{ MASKS, "community-shaders.deferred-shading.gbuffer.masks" },
		{ MASKS2, "community-shaders.deferred-shading.gbuffer.masks2" }
	};

	struct DeferredConstants
	{
		Matrix projectionInverse;
		Matrix viewInverse;
		uint32_t clusterGrid[3];
		uint32_t lightCount;
		float screenSize[2];
		float nearPlane;
		float farPlane;
		uint32_t contextCount;
		uint32_t pageCapacity;
		uint32_t abiVersion;
		uint32_t frameFlags;
		CS::Deferred::LightingTransform lightingTransform;
		uint32_t frameIndex;
		uint32_t cameraSignature;
		uint32_t instrumentationPadding[2]{};
	};
	static_assert(sizeof(DeferredConstants) <= 256);

	struct NativePipeline { winrt::com_ptr<ID3D12PipelineState> state; };

	std::uint32_t HashCameraMatrices(const Matrix& projectionInverse, const Matrix& viewInverse) noexcept
	{
		std::uint32_t hash = 2166136261u;
		auto append = [&](const Matrix& matrix) {
			const auto bytes = std::as_bytes(std::span{ std::addressof(matrix), std::size_t{ 1 } });
			for (const auto byte : bytes) {
				hash ^= std::to_integer<std::uint8_t>(byte);
				hash *= 16777619u;
			}
		};
		append(projectionInverse);
		append(viewInverse);
		return hash;
	}

	std::pair<std::vector<std::byte>, std::filesystem::path> LoadDeferredShader()
	{
		const std::filesystem::path paths[]{
			L"Data\\Shaders\\DeferredRendering\\DeferredCompositeCS.hlsl",
			std::filesystem::current_path() / L"package\\Shaders\\DeferredRendering\\DeferredCompositeCS.hlsl"
		};
		for (const auto& path : paths) {
			std::ifstream stream(path, std::ios::binary | std::ios::ate);
			if (!stream)
				continue;
			const auto size = stream.tellg();
			if (size <= 0)
				continue;
			std::vector<std::byte> source(static_cast<std::size_t>(size));
			stream.seekg(0);
			if (stream.read(reinterpret_cast<char*>(source.data()), size))
				return { std::move(source), path.parent_path() };
		}
		return {};
	}
}

DX12DeferredShading& DX12DeferredShading::Get()
{
	static DX12DeferredShading instance;
	return instance;
}

bool DX12DeferredShading::Initialize(DX12RenderRuntime& owner) noexcept
{
#if !defined(CS_HAS_ORG_MODULE_SERVICES) || !defined(ORG_MODULE_SERVICES_HAS_DXC)
	(void)owner;
	logger::warn("[DX12DeferredShading] ORGModuleServices DXC support is unavailable; deferred contributor is disabled");
	return true;
#else
	runtime = &owner;
	device.copy_from(owner.GetNativeDevice());
	if (!device || !CreatePipeline())
		return false;
	CSDX12ContributorDesc description{};
	description.structSize = sizeof(description);
	description.apiVersion = CS_DX12_GRAPH_API_CURRENT;
	description.id = "community-shaders.deferred-shading";
	description.kind = CS_DX12_CONTRIBUTOR_REQUIRED;
	description.userData = this;
	description.build = &Build;
	description.shutdown = &OnShutdown;
	return owner.Register(&description, &registration) == CS_DX12_OK;
#endif
}

bool DX12DeferredShading::CreatePipeline() noexcept
{
#if !defined(ORG_MODULE_SERVICES_HAS_DXC)
	return false;
#else
	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 13, 0, 0, 0 };
	ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 13 };
	D3D12_ROOT_PARAMETER parameters[2]{};
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameters[0].Descriptor.ShaderRegister = 0;
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[1].DescriptorTable = { 2, ranges };
	parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC root{ 2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
	winrt::com_ptr<ID3DBlob> serialized, errors;
	if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put())) ||
		FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(rootSignature.put()))))
		return false;
	auto* compiler = runtime->GetShaderCompiler();
	auto* pipelines = runtime->GetPipelineService();
	if (!compiler || !pipelines || !compiler->Available())
		return false;
	org::services::ShaderCompileRequest request{};
	auto [shaderSource, shaderDirectory] = LoadDeferredShader();
	if (shaderSource.empty()) {
		logger::error("[DX12DeferredShading] Packaged DeferredCompositeCS.hlsl is unavailable");
		return false;
	}
	request.sourceName = "DeferredRendering/DeferredCompositeCS.hlsl";
	request.source = shaderSource;
	request.entryPoint = L"main";
	request.target = L"cs_6_0";
	request.includeDirectories.push_back(shaderDirectory);
	request.dependencyFiles.push_back(shaderDirectory / L"DeferredLightingCommon.hlsli");
	request.dependencyFiles.push_back((shaderDirectory / L".." / L"Common" / L"LightingParity.hlsli").lexically_normal());
	auto artifact = compiler->Compile(request);
	if (!artifact) {
		logger::error("[DX12DeferredShading] DXC failed for deferred bootstrap: {}", artifact.diagnostics);
		return false;
	}
	org::services::PipelineRecipe recipe{};
	recipe.id = "community-shaders.deferred-shading.bootstrap";
	recipe.layoutKey = reinterpret_cast<uintptr_t>(rootSignature.get());
	recipe.shaderKey = artifact.key;
	recipe.deviceKey = reinterpret_cast<uintptr_t>(device.get());
	recipe.build = [nativeDevice=device, signature=rootSignature, bytecode=std::move(artifact.binary)]() mutable {
		auto payload=std::make_shared<NativePipeline>();
		D3D12_COMPUTE_PIPELINE_STATE_DESC desc{}; desc.pRootSignature=signature.get(); desc.CS={bytecode.data(),bytecode.size()};
		if(FAILED(nativeDevice->CreateComputePipelineState(&desc,IID_PPV_ARGS(payload->state.put())))) return org::services::PipelinePayload{};
		return std::static_pointer_cast<void>(payload);
	};
	auto result=pipelines->Request(std::move(recipe)).get();
	if(!result) return false;
	bootstrapPipeline=std::static_pointer_cast<NativePipeline>(result.payload)->state;
	pipelines->PublishReady(0);
	return bootstrapPipeline != nullptr;
#endif
}

bool DX12DeferredShading::EnsureComposite(uint32_t width, uint32_t height, DXGI_FORMAT format) noexcept
{
	constexpr auto transportFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	if (composite && specularComposite && composite.description.Width == width && composite.description.Height == height && composite.description.Format == transportFormat)
		return true;
	auto* interop = runtime ? runtime->GetInteropCoordinator() : nullptr;
	if (!interop || !width || !height)
		return false;
	DX12InteropCoordinator::SharedTexture candidate;
	if (!interop->CreateD3D12OwnedSharedTexture(width, height, transportFormat,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		true, candidate)) {
		logger::error("[DX12DeferredShading] Failed to create {}x{} shared composite texture", width, height);
		return false;
	}
	composite = std::move(candidate);
	DX12InteropCoordinator::SharedTexture specularCandidate;
	if (!interop->CreateD3D12OwnedSharedTexture(width, height, transportFormat,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		true, specularCandidate)) {
		composite = {};
		logger::error("[DX12DeferredShading] Failed to create shared specular composite texture");
		return false;
	}
	specularComposite = std::move(specularCandidate);
	logger::info("[DX12DeferredShading] Created D3D12-owned shared composite transport ({}x{}, transportFormat={}, destinationFormat={})",
		width, height, static_cast<unsigned>(transportFormat), static_cast<unsigned>(format));
	return true;
}

bool DX12DeferredShading::EnsureLinearDepth(uint32_t width, uint32_t height) noexcept
{
	if (linearDepth && linearDepth.uav11 && linearDepth.description.Width == width && linearDepth.description.Height == height)
		return true;
	auto* interop = runtime ? runtime->GetInteropCoordinator() : nullptr;
	if (!interop || !width || !height)
		return false;
	D3D11_TEXTURE2D_DESC description{};
	description.Width = width;
	description.Height = height;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = DXGI_FORMAT_R32_FLOAT;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	DX12InteropCoordinator::SharedTexture candidate;
	if (!interop->CreateSharedTexture(description, true, true, candidate)) {
		logger::error("[DX12DeferredShading] Failed to create shared linear-depth texture ({}x{})", width, height);
		return false;
	}
	linearDepth = std::move(candidate);
	logger::info("[DX12DeferredShading] Created directly writable shared linear-depth texture ({}x{}, R32_FLOAT)", width, height);
	return true;
}

bool DX12DeferredShading::EnsureFrameMarker() noexcept
{
	if (frameMarker)
		return true;
	auto* interop = runtime ? runtime->GetInteropCoordinator() : nullptr;
	if (!interop || !globals::d3d::device)
		return false;
	D3D11_TEXTURE2D_DESC description{};
	description.Width = 32;
	description.Height = 1;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = DXGI_FORMAT_R32_UINT;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	DX12InteropCoordinator::SharedTexture candidate;
	if (!interop->CreateD3D12OwnedSharedTexture(description.Width, description.Height,
		description.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, true, candidate)) {
		logger::error("[DX12DeferredShading] Failed to create shared frame marker");
		return false;
	}

	frameMarker = std::move(candidate);
	logger::info("[DX12DeferredShading] Created shared deferred diagnostic counters");
	return true;
}

bool DX12DeferredShading::PrepareCompatibilityInput(ID3D11Texture2D* source) noexcept
{
	if(!source)return false;
	D3D11_TEXTURE2D_DESC description{}; source->GetDesc(&description);
	if(!EnsureComposite(description.Width,description.Height,description.Format))return false;
	if (std::getenv("CS_DX12_DEFERRED_PARITY")) {
		auto* interop = runtime ? runtime->GetInteropCoordinator() : nullptr;
		if (!interop || !interop->CopyToMirror(globals::d3d::context, source, parityReference))
			return false;
	}
	return true;
}

bool DX12DeferredShading::PrepareLocalShadowMask(ID3D11Texture2D* source) noexcept
{
	auto* interop = runtime ? runtime->GetInteropCoordinator() : nullptr;
	if (!interop || !globals::d3d::context || !source)
		return false;
	D3D11_TEXTURE2D_DESC description{};
	source->GetDesc(&description);
	static std::once_flag contractLogged;
	std::call_once(contractLogged, [&] {
		logger::info("[DX12DeferredShading] Shadow-mask interop contract: format={}, bind=0x{:X}, misc=0x{:X}, samples={}",
			static_cast<unsigned>(description.Format), description.BindFlags, description.MiscFlags, description.SampleDesc.Count);
	});
	if ((description.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0)
		return interop->ImportReadOnlySharedTexture(source, false, localShadowMask);
	return interop->CopyToMirror(globals::d3d::context, source, localShadowMask);
}

bool DX12DeferredShading::PrepareScreenSpaceShadow(ID3D11Texture2D* source) noexcept
{
	auto* interop = runtime ? runtime->GetInteropCoordinator() : nullptr;
	return interop && source && interop->CopyToMirror(globals::d3d::context, source, screenSpaceShadow);
}

bool DX12DeferredShading::PreparePackedSurfaceMirror(ID3D11Texture2D* source) noexcept
{
	auto* interop = runtime ? runtime->GetInteropCoordinator() : nullptr;
	if (!interop || !source)
		return false;
	return interop->ImportReadOnlySharedTexture(source, true, packedSurfaceMirror);
}

bool DX12DeferredShading::PrepareGBufferInputs() noexcept
{
	auto* interop = runtime ? runtime->GetInteropCoordinator() : nullptr;
	auto* renderer = globals::game::renderer;
	if (!interop || !renderer || !globals::d3d::context)
		return false;

	// These CS G-buffer allocations are explicitly created with NT shared
	// handles. Import their allocation directly; the graph's first-use external
	// wait orders D3D11 rendering before D3D12 reads.
	constexpr size_t importedIndices[]{ 0, 1, 3, 4 };
	for (const auto index : importedIndices) {
		auto* source = renderer->GetRuntimeData().renderTargets[kInputs[index].target].texture;
		if (!source || !interop->ImportReadOnlySharedTexture(source, false, inputs[index].mirror)) {
			logger::error("[DX12DeferredShading] Failed to directly import G-buffer input '{}'", kInputs[index].id);
			return false;
		}
		inputs[index].source.copy_from(source);
	}
	return true;
}

bool DX12DeferredShading::EnsureCompositeBlit(ID3D11Texture2D* destination) noexcept
{
	if (!destination || !globals::d3d::device)
		return false;
	if (!compositeBlitVS) {
		compositeBlitVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(
			L"Data\\Shaders\\DeferredRendering\\CompositeHandoff.hlsl", { { "VSHADER", "" } }, "vs_5_0")));
		compositeBlitPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(
			L"Data\\Shaders\\DeferredRendering\\CompositeHandoff.hlsl", {}, "ps_5_0")));
		compositeSelectiveBlitPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(
			L"Data\\Shaders\\DeferredRendering\\CompositeHandoff.hlsl", { { "SELECTIVE_HANDOFF", "" } }, "ps_5_0")));
		compositeCoverageOverlayPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(
			L"Data\\Shaders\\DeferredRendering\\CompositeHandoff.hlsl", { { "COVERAGE_OVERLAY", "" } }, "ps_5_0")));
		if (!compositeBlitVS || !compositeBlitPS || !compositeSelectiveBlitPS || !compositeCoverageOverlayPS) {
			logger::error("[DX12DeferredShading] Failed to compile D3D11 composite handoff shaders");
			return false;
		}
	}
	if (compositeBlitDestination.get() != destination) {
		winrt::com_ptr<ID3D11RenderTargetView> view;
		if (FAILED(globals::d3d::device->CreateRenderTargetView(destination, nullptr, view.put()))) {
			logger::error("[DX12DeferredShading] Failed to create main-target RTV for composite handoff");
			return false;
		}
		compositeBlitDestination.copy_from(destination);
		compositeBlitRTV = std::move(view);
	}
	auto* specularDestination = globals::game::renderer ?
		globals::game::renderer->GetRuntimeData().renderTargets[SPECULAR].texture : nullptr;
	if (!specularDestination)
		return false;
	if (specularBlitDestination.get() != specularDestination) {
		winrt::com_ptr<ID3D11RenderTargetView> view;
		if (FAILED(globals::d3d::device->CreateRenderTargetView(specularDestination, nullptr, view.put()))) {
			logger::error("[DX12DeferredShading] Failed to create specular-target RTV for deferred handoff");
			return false;
		}
		specularBlitDestination.copy_from(specularDestination);
		specularBlitRTV = std::move(view);
	}
	return true;
}

bool DX12DeferredShading::CommitComposite(ID3D11Texture2D* destination) noexcept
{
	if(!destination||!composite.d3d11||!specularComposite.d3d11||!frameMarker.d3d11||!globals::d3d::context||!EnsureCompositeBlit(destination))return false;
	if (std::getenv("CS_DX12_DEFERRED_PARITY")) {
		if (!parityCounterReadback) {
			D3D11_TEXTURE2D_DESC description{};
			frameMarker.d3d11->GetDesc(&description);
			description.Usage = D3D11_USAGE_STAGING;
			description.BindFlags = 0;
			description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			description.MiscFlags = 0;
			if (FAILED(globals::d3d::device->CreateTexture2D(&description, nullptr, parityCounterReadback.put())))
				return false;
		}
		if (parityCounterPending) {
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(globals::d3d::context->Map(parityCounterReadback.get(), 0, D3D11_MAP_READ,
				D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
				const auto* counters = static_cast<const std::uint32_t*>(mapped.pData);
				if (counters[16]) {
					const double scale = 1.0 / (4096.0 * counters[16]);
					logger::info("[DX12DeferredParity] pixels={}, meanAbsRGB=({:.6f},{:.6f},{:.6f}), maxComponent={:.6f}",
						counters[16], counters[17] * scale, counters[18] * scale, counters[19] * scale,
						counters[20] / 4096.0);
					const auto* signedCounters = reinterpret_cast<const std::int32_t*>(counters);
					logger::info("[DX12DeferredParity] mean(candidate-reference)=({:.6f},{:.6f},{:.6f}), meanCandidate=({:.6f},{:.6f},{:.6f}), meanReference=({:.6f},{:.6f},{:.6f})",
						signedCounters[21] * scale, signedCounters[22] * scale, signedCounters[23] * scale,
						counters[24] * scale, counters[25] * scale, counters[26] * scale,
						counters[27] * scale, counters[28] * scale, counters[29] * scale);
				}
				globals::d3d::context->Unmap(parityCounterReadback.get(), 0);
				parityCounterPending = false;
			}
		}
		if (!parityCounterPending) {
			globals::d3d::context->CopyResource(parityCounterReadback.get(), frameMarker.d3d11.get());
			parityCounterPending = true;
		}
	}
	// The shared transport is guaranteed cross-API RGBA16. Skyrim's main target
	// can be R11G11B10, which cannot be opened through strict cross-API sharing,
	// so perform an ordinary D3D11 render-target conversion after the fence wait.
	Effects11Util::D3D11FullStateBackup state;
	state.Save(globals::d3d::context);
	D3D11_TEXTURE2D_DESC destinationDescription{};
	destination->GetDesc(&destinationDescription);
	D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(destinationDescription.Width),
		static_cast<float>(destinationDescription.Height), 0.0f, 1.0f };
	const D3D11_RECT scissor{ 0, 0, static_cast<LONG>(destinationDescription.Width), static_cast<LONG>(destinationDescription.Height) };
	globals::d3d::context->IASetInputLayout(nullptr);
	globals::d3d::context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	// Never inherit Skyrim's rasterizer/scissor state for a host fullscreen pass.
	// A stale empty scissor or front-cull state silently rejects every pixel while
	// Draw still succeeds, which previously made the interop handoff appear healthy.
	globals::d3d::context->RSSetState(nullptr);
	globals::d3d::context->RSSetViewports(1, &viewport);
	globals::d3d::context->RSSetScissorRects(1, &scissor);
	auto* target = compositeBlitRTV.get();
	globals::d3d::context->OMSetRenderTargets(1, &target, nullptr);
	globals::d3d::context->OMSetBlendState(nullptr, nullptr, UINT_MAX);
	globals::d3d::context->OMSetDepthStencilState(nullptr, 0);
	globals::d3d::context->VSSetShader(compositeBlitVS.get(), nullptr, 0);
	const bool visualizeCoverage = globals::features::deferredRendering.IsCoverageVisualizationEnabled();
	// The existing D3D11 deferred composite consumes SPECULAR separately from
	// main color. Replace it first so promoted pixels are not double-lit while
	// compatibility pixels retain their geometry-evaluated value.
	auto* specularTarget = specularBlitRTV.get();
	globals::d3d::context->OMSetRenderTargets(1, &specularTarget, nullptr);
	globals::d3d::context->PSSetShader(compositeBlitPS.get(), nullptr, 0);
	ID3D11ShaderResourceView* specularSource[]{ specularComposite.srv11.get() };
	globals::d3d::context->PSSetShaderResources(0, 1, specularSource);
	globals::d3d::context->Draw(3, 0);
	ID3D11ShaderResourceView* nullSpecular{};
	globals::d3d::context->PSSetShaderResources(0, 1, &nullSpecular);

	globals::d3d::context->OMSetRenderTargets(1, &target, nullptr);
	globals::d3d::context->PSSetShader(compositeSelectiveBlitPS.get(), nullptr, 0);
	ID3D11ShaderResourceView* sources[]{ composite.srv11.get(), packedSurfaceMirror.srv11.get() };
	globals::d3d::context->PSSetShaderResources(0, ARRAYSIZE(sources), sources);
	globals::d3d::context->Draw(3, 0);
	if (visualizeCoverage && packedSurfaceMirror.srv11) {
		sources[0] = nullptr;
		sources[1] = packedSurfaceMirror.srv11.get();
		globals::d3d::context->PSSetShaderResources(0, ARRAYSIZE(sources), sources);
		globals::d3d::context->PSSetShader(compositeCoverageOverlayPS.get(), nullptr, 0);
		globals::d3d::context->Draw(3, 0);
	}
	ID3D11ShaderResourceView* nullSources[2]{};
	globals::d3d::context->PSSetShaderResources(0, ARRAYSIZE(nullSources), nullSources);
	state.Restore(globals::d3d::context);
	state.Release();
	return true;
}

bool DX12DeferredShading::ShouldCommitComposite() const noexcept
{
	// Until a material class or diagnostic output is explicitly selected, the
	// DX12 epoch is culling/interop infrastructure only.  Do not replace the
	// authoritative current-frame Skyrim image with an otherwise identical
	// round-trip copy.
	static const bool enabled = [] {
		wchar_t disabled[8]{};
		if (!(GetEnvironmentVariableW(L"CS_DX12_DEFERRED_DISABLE_STATIC_OPAQUE", disabled, static_cast<DWORD>(std::size(disabled))) > 0 && disabled[0] == L'1'))
			return true;
		constexpr const wchar_t* variables[]{
			L"CS_DX12_DEFERRED_DIRECT_DIAGNOSTIC",
			L"CS_DX12_DEFERRED_COVERAGE_DIAGNOSTIC"
		};
		for (const auto* variable : variables) {
			wchar_t value[8]{};
			if (GetEnvironmentVariableW(variable, value, static_cast<DWORD>(std::size(value))) > 0 && value[0] == L'1')
				return true;
		}
		return false;
	}();
	return enabled;
}

bool DX12DeferredShading::PrepareLinearDepth(uint32_t width, uint32_t height) noexcept
{
	if (!EnsureLinearDepth(width, height))
		return false;
	if (!linearizeDepthShader) {
		linearizeDepthShader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(
			L"Data\\Shaders\\DeferredRendering\\LinearizeDepthCS.hlsl", {}, "cs_5_0")));
		if (!linearizeDepthShader) {
			logger::error("[DX12DeferredShading] Failed to compile the temporary linear-depth producer");
			return false;
		}
	}
	auto* renderer = globals::game::renderer;
	auto* context = globals::d3d::context;
	if (!renderer || !context || !globals::state || !globals::state->sharedDataCB)
		return false;
	auto* depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
	if (!depth)
		return false;
	ID3D11Buffer* constants[]{ globals::state->sharedDataCB->CB() };
	ID3D11ShaderResourceView* inputs11[]{ depth };
	ID3D11UnorderedAccessView* outputs11[]{ linearDepth.uav11.get() };
	context->CSSetConstantBuffers(5, 1, constants);
	context->CSSetShaderResources(0, 1, inputs11);
	context->CSSetUnorderedAccessViews(0, 1, outputs11, nullptr);
	context->CSSetShader(linearizeDepthShader.get(), nullptr, 0);
	context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSRV{};
	ID3D11UnorderedAccessView* nullUAV{};
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	// Keep CS b5 bound: the existing D3D11 deferred composite consumes the
	// same SharedData buffer immediately after the DX12 epoch.
	context->CSSetShader(nullptr, nullptr, 0);
	return true;
}

bool DX12DeferredShading::EnsureGBufferInputs() noexcept
{
	return inputs[0].source && inputs[0].mirror && inputs[1].source && inputs[1].mirror &&
		inputs[3].source && inputs[3].mirror;
}

CSDX12Status DX12DeferredShading::Build(void* userData, CSDX12BuildHandle build)
{
	auto* self = static_cast<DX12DeferredShading*>(userData);
	// Imported textures retain Skyrim's allocation extent; frame.width/height
	// carry the independently varying active dynamic-resolution extent.
	const auto width = self->runtime->GetAllocationWidth();
	const auto height = self->runtime->GetAllocationHeight();
	if (!self->composite || !self->specularComposite)
		return CS_DX12_E_NOT_READY;
	if (!self->EnsureGBufferInputs())
		return CS_DX12_E_NOT_READY;
	if (!self->EnsureLinearDepth(width, height))
		return CS_DX12_E_NOT_READY;
	if (!self->EnsureFrameMarker())
		return CS_DX12_E_NOT_READY;
	if (!self->packedSurfaceMirror)
		return CS_DX12_E_NOT_READY;
	if (!self->localShadowMask)
		return CS_DX12_E_NOT_READY;
	const bool parityEnabled = std::getenv("CS_DX12_DEFERRED_PARITY") != nullptr;
	if (parityEnabled && !self->parityReference)
		return CS_DX12_E_NOT_READY;

	constexpr size_t importedIndices[]{ 0, 1, 3, 4 };
	for (const auto index : importedIndices) {
		D3D11_TEXTURE2D_DESC nativeDescription{};
		self->inputs[index].source->GetDesc(&nativeDescription);
		CSDX12ResourceDesc input{};
		input.structSize = sizeof(input);
		input.apiVersion = CS_DX12_GRAPH_API_CURRENT;
		input.id = kInputs[index].id;
		input.lifetime = CS_DX12_RESOURCE_CS_IMPORTED;
		input.dimension = CS_DX12_RESOURCE_TEXTURE_2D;
		input.sizing = CS_DX12_SIZE_ABSOLUTE;
		input.format = nativeDescription.Format;
		input.width = nativeDescription.Width;
		input.height = nativeDescription.Height;
		input.depthOrArraySize = nativeDescription.ArraySize;
		input.mipLevels = nativeDescription.MipLevels;
		input.sampleCount = nativeDescription.SampleDesc.Count;
		input.allowedAccess = CS_DX12_ACCESS_SHADER_READ;
		input.initialAccess = CS_DX12_ACCESS_SHADER_READ;
		input.finalAccess = CS_DX12_ACCESS_SHADER_READ;
		input.borrowedNativeResource = self->inputs[index].mirror.d3d12.get();
		if (self->runtime->DeclareResource(build, &input, &self->inputs[index].handle) != CS_DX12_OK)
			return CS_DX12_E_INTERNAL;
	}

	CSDX12ResourceDesc depthInput{};
	depthInput.structSize = sizeof(depthInput);
	depthInput.apiVersion = CS_DX12_GRAPH_API_CURRENT;
	depthInput.id = "community-shaders.deferred-shading.linear-depth";
	depthInput.lifetime = CS_DX12_RESOURCE_CS_IMPORTED;
	depthInput.dimension = CS_DX12_RESOURCE_TEXTURE_2D;
	depthInput.sizing = CS_DX12_SIZE_ABSOLUTE;
	depthInput.format = DXGI_FORMAT_R32_FLOAT;
	depthInput.width = width;
	depthInput.height = height;
	depthInput.depthOrArraySize = 1;
	depthInput.mipLevels = 1;
	depthInput.sampleCount = 1;
	depthInput.allowedAccess = CS_DX12_ACCESS_SHADER_READ;
	depthInput.initialAccess = CS_DX12_ACCESS_SHADER_READ;
	depthInput.finalAccess = CS_DX12_ACCESS_SHADER_READ;
	depthInput.borrowedNativeResource = self->linearDepth.d3d12.get();
	if (self->runtime->DeclareResource(build, &depthInput, &self->linearDepthHandle) != CS_DX12_OK)
		return CS_DX12_E_INTERNAL;
	CSDX12ResourceDesc shadowMask{};
	shadowMask.structSize=sizeof(shadowMask); shadowMask.apiVersion=CS_DX12_GRAPH_API_CURRENT;
	shadowMask.id="community-shaders.deferred-shading.local-shadow-mask"; shadowMask.lifetime=CS_DX12_RESOURCE_CS_IMPORTED;
	shadowMask.dimension=CS_DX12_RESOURCE_TEXTURE_2D; shadowMask.sizing=CS_DX12_SIZE_ABSOLUTE;
	shadowMask.format=self->localShadowMask.description.Format; shadowMask.width=self->localShadowMask.description.Width;
	shadowMask.height=self->localShadowMask.description.Height; shadowMask.depthOrArraySize=self->localShadowMask.description.ArraySize;
	shadowMask.mipLevels=self->localShadowMask.description.MipLevels; shadowMask.sampleCount=1;
	shadowMask.allowedAccess=CS_DX12_ACCESS_SHADER_READ; shadowMask.initialAccess=CS_DX12_ACCESS_SHADER_READ;
	shadowMask.finalAccess=CS_DX12_ACCESS_SHADER_READ; shadowMask.borrowedNativeResource=self->localShadowMask.d3d12.get();
	if(self->runtime->DeclareResource(build,&shadowMask,&self->localShadowMaskHandle)!=CS_DX12_OK)return CS_DX12_E_INTERNAL;
	const auto& screenShadowResource = self->screenSpaceShadow ? self->screenSpaceShadow : self->localShadowMask;
	CSDX12ResourceDesc screenShadow = shadowMask;
	screenShadow.id = "community-shaders.deferred-shading.screen-space-shadow";
	screenShadow.format = screenShadowResource.description.Format;
	screenShadow.width = screenShadowResource.description.Width;
	screenShadow.height = screenShadowResource.description.Height;
	screenShadow.mipLevels = screenShadowResource.description.MipLevels;
	screenShadow.borrowedNativeResource = screenShadowResource.d3d12.get();
	if(self->runtime->DeclareResource(build,&screenShadow,&self->screenSpaceShadowHandle)!=CS_DX12_OK)return CS_DX12_E_INTERNAL;
	if (parityEnabled) {
		CSDX12ResourceDesc reference{};
		reference.structSize = sizeof(reference); reference.apiVersion = CS_DX12_GRAPH_API_CURRENT;
		reference.id = "community-shaders.deferred-shading.forward-reference";
		reference.lifetime = CS_DX12_RESOURCE_CS_IMPORTED; reference.dimension = CS_DX12_RESOURCE_TEXTURE_2D;
		reference.sizing = CS_DX12_SIZE_ABSOLUTE; reference.format = self->parityReference.description.Format;
		reference.width = self->parityReference.description.Width; reference.height = self->parityReference.description.Height;
		reference.depthOrArraySize = 1; reference.mipLevels = 1; reference.sampleCount = 1;
		reference.allowedAccess = CS_DX12_ACCESS_SHADER_READ; reference.initialAccess = CS_DX12_ACCESS_SHADER_READ;
		reference.finalAccess = CS_DX12_ACCESS_SHADER_READ; reference.borrowedNativeResource = self->parityReference.d3d12.get();
		if (self->runtime->DeclareResource(build, &reference, &self->parityReferenceHandle) != CS_DX12_OK)
			return CS_DX12_E_INTERNAL;
	}
	if (self->runtime->FindResource(build, "community-shaders.clustered-lighting.lights", &self->lightsHandle) != CS_DX12_OK ||
		self->runtime->FindResource(build, "community-shaders.clustered-lighting.contexts", &self->contextsHandle) != CS_DX12_OK ||
		self->runtime->FindResource(build, "community-shaders.clustered-lighting.clusters", &self->clustersHandle) != CS_DX12_OK ||
		self->runtime->FindResource(build, "community-shaders.clustered-lighting.pages", &self->pagesHandle) != CS_DX12_OK)
		return CS_DX12_E_MISSING_DEPENDENCY;

	CSDX12ResourceDesc resource{};
	resource.structSize = sizeof(resource);
	resource.apiVersion = CS_DX12_GRAPH_API_CURRENT;
	resource.id = "community-shaders.deferred-shading.composite";
	resource.lifetime = CS_DX12_RESOURCE_CS_IMPORTED;
	resource.dimension = CS_DX12_RESOURCE_TEXTURE_2D;
	resource.sizing = CS_DX12_SIZE_ABSOLUTE;
	resource.format = self->composite.description.Format;
	resource.width = width;
	resource.height = height;
	resource.depthOrArraySize = 1;
	resource.mipLevels = 1;
	resource.sampleCount = 1;
	resource.allowedAccess = CS_DX12_ACCESS_UNORDERED_WRITE | CS_DX12_ACCESS_SHADER_READ;
	resource.initialAccess = CS_DX12_ACCESS_NONE;
	resource.finalAccess = CS_DX12_ACCESS_SHADER_READ;
	resource.borrowedNativeResource = self->composite.d3d12.get();
	if (self->runtime->DeclareResource(build, &resource, &self->compositeHandle) != CS_DX12_OK)
		return CS_DX12_E_INTERNAL;
	resource.id = "community-shaders.deferred-shading.specular-composite";
	resource.format = self->specularComposite.description.Format;
	resource.borrowedNativeResource = self->specularComposite.d3d12.get();
	if (self->runtime->DeclareResource(build, &resource, &self->specularCompositeHandle) != CS_DX12_OK)
		return CS_DX12_E_INTERNAL;
	CSDX12ResourceDesc marker{};
	marker.structSize = sizeof(marker);
	marker.apiVersion = CS_DX12_GRAPH_API_CURRENT;
	marker.id = "community-shaders.deferred-shading.frame-marker";
	marker.lifetime = CS_DX12_RESOURCE_CS_IMPORTED;
	marker.dimension = CS_DX12_RESOURCE_TEXTURE_2D;
	marker.sizing = CS_DX12_SIZE_ABSOLUTE;
	marker.format = DXGI_FORMAT_R32_UINT;
	marker.width = 32;
	marker.height = 1;
	marker.depthOrArraySize = 1;
	marker.mipLevels = 1;
	marker.sampleCount = 1;
	marker.allowedAccess = CS_DX12_ACCESS_UNORDERED_WRITE | CS_DX12_ACCESS_SHADER_READ;
	marker.initialAccess = CS_DX12_ACCESS_NONE;
	marker.finalAccess = CS_DX12_ACCESS_SHADER_READ;
	marker.borrowedNativeResource = self->frameMarker.d3d12.get();
	if (self->runtime->DeclareResource(build, &marker, &self->frameMarkerHandle) != CS_DX12_OK)
		return CS_DX12_E_INTERNAL;
	CSDX12ResourceDesc packedMirror{};
	packedMirror.structSize = sizeof(packedMirror);
	packedMirror.apiVersion = CS_DX12_GRAPH_API_CURRENT;
	packedMirror.id = "community-shaders.deferred-shading.gbuffer.masks2-mirror";
	packedMirror.lifetime = CS_DX12_RESOURCE_CS_IMPORTED;
	packedMirror.dimension = CS_DX12_RESOURCE_TEXTURE_2D;
	packedMirror.sizing = CS_DX12_SIZE_ABSOLUTE;
	packedMirror.format = DXGI_FORMAT_R32_UINT;
	packedMirror.width = self->packedSurfaceMirror.description.Width;
	packedMirror.height = self->packedSurfaceMirror.description.Height;
	packedMirror.depthOrArraySize = 1;
	packedMirror.mipLevels = 1;
	packedMirror.sampleCount = 1;
	packedMirror.allowedAccess = CS_DX12_ACCESS_SHADER_READ;
	packedMirror.initialAccess = CS_DX12_ACCESS_SHADER_READ;
	packedMirror.finalAccess = CS_DX12_ACCESS_SHADER_READ;
	packedMirror.borrowedNativeResource = self->packedSurfaceMirror.d3d12.get();
	if (self->runtime->DeclareResource(build, &packedMirror, &self->packedSurfaceMirrorHandle) != CS_DX12_OK)
		return CS_DX12_E_INTERNAL;

	const char* after[]{ "community-shaders.clustered-lighting.cull" };
	const char* before[]{ CS_DX12_ANCHOR_DEFERRED_LIGHTING_BEGIN };
	std::array<CSDX12ResourceAccessDesc, 16> accesses{};
	accesses[0] = { sizeof(CSDX12ResourceAccessDesc), CS_DX12_GRAPH_API_CURRENT,
		self->compositeHandle, CS_DX12_ACCESS_UNORDERED_WRITE, {} };
	accesses[1] = { sizeof(CSDX12ResourceAccessDesc), CS_DX12_GRAPH_API_CURRENT,
		self->inputs[0].handle, CS_DX12_ACCESS_SHADER_READ, {} };
	accesses[2] = { sizeof(CSDX12ResourceAccessDesc), CS_DX12_GRAPH_API_CURRENT,
		self->inputs[3].handle, CS_DX12_ACCESS_SHADER_READ, {} };
	accesses[3] = { sizeof(CSDX12ResourceAccessDesc), CS_DX12_GRAPH_API_CURRENT,
		self->linearDepthHandle, CS_DX12_ACCESS_SHADER_READ, {} };
	const CSDX12ResourceHandle clustered[]{ self->lightsHandle, self->contextsHandle, self->clustersHandle, self->pagesHandle };
	for (size_t index=0;index<std::size(clustered);++index)
		accesses[4+index]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,clustered[index],CS_DX12_ACCESS_SHADER_READ,{}};
	accesses[8]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->localShadowMaskHandle,CS_DX12_ACCESS_SHADER_READ,{}};
	accesses[9]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->frameMarkerHandle,CS_DX12_ACCESS_UNORDERED_WRITE,{}};
	accesses[10]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->packedSurfaceMirrorHandle,CS_DX12_ACCESS_SHADER_READ,{}};
	accesses[11]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->inputs[4].handle,CS_DX12_ACCESS_SHADER_READ,{}};
	accesses[12]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->screenSpaceShadowHandle,CS_DX12_ACCESS_SHADER_READ,{}};
	accesses[13]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->specularCompositeHandle,CS_DX12_ACCESS_UNORDERED_WRITE,{}};
	accesses[14]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->inputs[1].handle,CS_DX12_ACCESS_SHADER_READ,{}};
	if (parityEnabled)
		accesses[15]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->parityReferenceHandle,CS_DX12_ACCESS_SHADER_READ,{}};
	CSDX12PassDesc pass{};
	pass.structSize = sizeof(pass);
	pass.apiVersion = CS_DX12_GRAPH_API_CURRENT;
	pass.id = "community-shaders.deferred-shading.composite";
	pass.queuePolicy = CS_DX12_QUEUE_REQUIRE_COMPUTE;
	pass.after = after;
	pass.afterCount = 1;
	pass.before = before;
	pass.beforeCount = 1;
	pass.accesses = accesses.data();
	pass.accessCount = parityEnabled ? static_cast<uint32_t>(accesses.size()) : 15u;
	pass.execute = &Execute;
	CSDX12PassHandle handle{};
	return self->runtime->DeclarePass(build, &pass, &handle);
}

CSDX12Status DX12DeferredShading::Execute(void* userData, const CSDX12ExecutionContext* context)
{
	if (!userData || !context)
		return CS_DX12_E_INVALID_ARGUMENT;
	return static_cast<DX12DeferredShading*>(userData)->Record(*context);
}

CSDX12Status DX12DeferredShading::Record(const CSDX12ExecutionContext& context) noexcept
{
	if (!context.GetResource || !context.AllocateUpload || !context.AllocateDescriptors || !context.borrowedD3D12GraphicsCommandList || !bootstrapPipeline)
		return CS_DX12_E_UNSUPPORTED_CAPABILITY;
	void* native{}; void* specularOutputNative{}; void* albedoNative{}; void* specularInputNative{}; void* masksNative{}; void* depthNative{}; void* lightsNative{}; void* contextsNative{}; void* clustersNative{}; void* pagesNative{}; void* shadowMaskNative{}; void* screenShadowNative{}; void* markerNative{}; void* packedMirrorNative{}; void* parityNative{};
	if (context.GetResource(&context, compositeHandle, &native) != CS_DX12_OK || !native ||
		context.GetResource(&context, specularCompositeHandle, &specularOutputNative) != CS_DX12_OK || !specularOutputNative ||
		context.GetResource(&context, inputs[0].handle, &albedoNative) != CS_DX12_OK || !albedoNative ||
		context.GetResource(&context, inputs[1].handle, &specularInputNative) != CS_DX12_OK || !specularInputNative ||
		context.GetResource(&context, linearDepthHandle, &depthNative) != CS_DX12_OK || !depthNative ||
		context.GetResource(&context, lightsHandle, &lightsNative) != CS_DX12_OK || !lightsNative ||
		context.GetResource(&context, contextsHandle, &contextsNative) != CS_DX12_OK || !contextsNative ||
		context.GetResource(&context, clustersHandle, &clustersNative) != CS_DX12_OK || !clustersNative ||
		context.GetResource(&context, pagesHandle, &pagesNative) != CS_DX12_OK || !pagesNative ||
		context.GetResource(&context, localShadowMaskHandle, &shadowMaskNative) != CS_DX12_OK || !shadowMaskNative ||
		context.GetResource(&context, screenSpaceShadowHandle, &screenShadowNative) != CS_DX12_OK || !screenShadowNative ||
		context.GetResource(&context, frameMarkerHandle, &markerNative) != CS_DX12_OK || !markerNative ||
		context.GetResource(&context, packedSurfaceMirrorHandle, &packedMirrorNative) != CS_DX12_OK || !packedMirrorNative ||
		context.GetResource(&context, inputs[4].handle, &masksNative) != CS_DX12_OK || !masksNative)
		return CS_DX12_E_NOT_READY;
	const bool parityEnabled = std::getenv("CS_DX12_DEFERRED_PARITY") != nullptr;
	if (parityEnabled && (context.GetResource(&context, parityReferenceHandle, &parityNative) != CS_DX12_OK || !parityNative))
		return CS_DX12_E_NOT_READY;
	auto snapshot=globals::features::deferredRendering.GetFrameSnapshot();
	if(!snapshot)return CS_DX12_E_NOT_READY;
	CSDX12UploadAllocation constantsUpload{};
	if(context.AllocateUpload(&context,256,256,&constantsUpload)!=CS_DX12_OK)return CS_DX12_E_INTERNAL;
	DeferredConstants constants{}; constants.projectionInverse=snapshot->projectionInverse; constants.viewInverse=snapshot->cameraViewInverse;
	constants.clusterGrid[0]=(context.frame->width+63)/64; constants.clusterGrid[1]=(context.frame->height+63)/64; constants.clusterGrid[2]=32;
	constants.lightCount=static_cast<uint32_t>(snapshot->lights.size()); constants.screenSize[0]=static_cast<float>(context.frame->width); constants.screenSize[1]=static_cast<float>(context.frame->height);
	constants.nearPlane=snapshot->nearPlane; constants.farPlane=snapshot->farPlane; constants.contextCount=static_cast<uint32_t>(snapshot->contexts.size());
	constants.pageCapacity=constants.clusterGrid[0]*constants.clusterGrid[1]*constants.clusterGrid[2]*10;
	constants.abiVersion=CS::Deferred::kAbiVersion;
	constants.lightingTransform=snapshot->lightingTransform;
	constants.frameIndex=static_cast<std::uint32_t>(context.frame->frameIndex);
	constants.cameraSignature=HashCameraMatrices(snapshot->projectionInverse, snapshot->cameraViewInverse);
	static const bool directDiagnostic = [] {
		wchar_t value[8]{};
		return GetEnvironmentVariableW(L"CS_DX12_DEFERRED_DIRECT_DIAGNOSTIC", value, static_cast<DWORD>(std::size(value))) > 0 && value[0] == L'1';
	}();
	static const bool staticOpaqueEnabled = [] {
		wchar_t value[8]{};
		return !(GetEnvironmentVariableW(L"CS_DX12_DEFERRED_DISABLE_STATIC_OPAQUE", value, static_cast<DWORD>(std::size(value))) > 0 && value[0] == L'1');
	}();
	const bool coverageDiagnostic = globals::features::deferredRendering.IsCoverageVisualizationEnabled();
	const bool diagnosticNoShadows = std::getenv("CS_DX12_DIAGNOSTIC_NO_SHADOWS") != nullptr;
	const bool diagnosticAlbedo = std::getenv("CS_DX12_DIAGNOSTIC_ALBEDO") != nullptr;
	const bool diagnosticCompatibility = std::getenv("CS_DX12_DIAGNOSTIC_COMPATIBILITY") != nullptr;
	const bool hasScreenSpaceShadow = static_cast<bool>(screenSpaceShadow);
	constants.frameFlags=(staticOpaqueEnabled ? 1u : 0u) | (directDiagnostic ? 2u : 0u) |
		(coverageDiagnostic ? 4u : 0u) | (parityEnabled ? 8u : 0u) |
		(diagnosticNoShadows ? 16u : 0u) | (diagnosticAlbedo ? 32u : 0u) |
		(diagnosticCompatibility ? 64u : 0u) | (hasScreenSpaceShadow ? 128u : 0u);
	if (directDiagnostic) {
		static std::once_flag diagnosticLogged;
		std::call_once(diagnosticLogged, [] { logger::warn("[DX12DeferredShading] Direct-plus-ambient diagnostic output is enabled"); });
	}
	if (staticOpaqueEnabled) {
		static std::once_flag promotionLogged;
		std::call_once(promotionLogged, [] { logger::warn("[DX12DeferredShading] Experimental generic opaque deferred promotion is enabled"); });
	}
	std::memcpy(constantsUpload.cpuAddress,&constants,sizeof(constants));
	CSDX12DescriptorAllocation descriptors{};
	if (context.AllocateDescriptors(&context, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, &descriptors) != CS_DX12_OK)
		return CS_DX12_E_INTERNAL;
	auto* resource = static_cast<ID3D12Resource*>(native);
	auto* commandList = static_cast<ID3D12GraphicsCommandList*>(context.borrowedD3D12GraphicsCommandList);
	D3D12_CPU_DESCRIPTOR_HANDLE visibleCpu{ descriptors.cpuHandle };
	auto cpuAt=[&](uint32_t index){auto value=visibleCpu;value.ptr+=uint64_t(index)*descriptors.descriptorSize;return value;};
	D3D12_SHADER_RESOURCE_VIEW_DESC albedoView{}; albedoView.Format=DXGI_FORMAT_R10G10B10A2_UNORM; albedoView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; albedoView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; albedoView.Texture2D.MipLevels=1;
	D3D12_SHADER_RESOURCE_VIEW_DESC specularView=albedoView; specularView.Format=inputs[1].mirror.description.Format;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(specularInputNative),&specularView,cpuAt(0));
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(albedoNative),&albedoView,cpuAt(1));
	D3D12_SHADER_RESOURCE_VIEW_DESC depthView{}; depthView.Format=DXGI_FORMAT_R32_FLOAT; depthView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; depthView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; depthView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(depthNative),&depthView,cpuAt(2));
	auto structured=[&](void* nativeBuffer,uint32_t slot,uint32_t stride){auto* buffer=static_cast<ID3D12Resource*>(nativeBuffer);D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Buffer.NumElements=static_cast<UINT>(buffer->GetDesc().Width/stride);srv.Buffer.StructureByteStride=stride;device->CreateShaderResourceView(buffer,&srv,cpuAt(slot));};
	structured(lightsNative,3,sizeof(DeferredRendering::LightData)); structured(contextsNative,4,sizeof(DeferredRendering::LightingContext)); structured(clustersNative,5,48); structured(pagesNative,6,56);
	D3D12_SHADER_RESOURCE_VIEW_DESC packedView{}; packedView.Format=DXGI_FORMAT_R32_UINT; packedView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; packedView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; packedView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(packedMirrorNative),&packedView,cpuAt(7));
	D3D12_SHADER_RESOURCE_VIEW_DESC shadowMaskView{}; shadowMaskView.Format=localShadowMask.description.Format; shadowMaskView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; shadowMaskView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; shadowMaskView.Texture2D.MipLevels=localShadowMask.description.MipLevels;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(shadowMaskNative),&shadowMaskView,cpuAt(8));
	void* normalNative{};
	if (context.GetResource(&context, inputs[3].handle, &normalNative) != CS_DX12_OK || !normalNative)
		return CS_DX12_E_NOT_READY;
	D3D12_SHADER_RESOURCE_VIEW_DESC normalView{}; normalView.Format=DXGI_FORMAT_R10G10B10A2_UNORM; normalView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; normalView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; normalView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(normalNative),&normalView,cpuAt(9));
	if (parityEnabled) {
		D3D12_SHADER_RESOURCE_VIEW_DESC parityView{}; parityView.Format=parityReference.description.Format; parityView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; parityView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; parityView.Texture2D.MipLevels=1;
		device->CreateShaderResourceView(static_cast<ID3D12Resource*>(parityNative),&parityView,cpuAt(10));
	} else {
		device->CreateShaderResourceView(static_cast<ID3D12Resource*>(albedoNative),&albedoView,cpuAt(10));
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC masksView{}; masksView.Format=inputs[4].mirror.description.Format; masksView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; masksView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; masksView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(masksNative),&masksView,cpuAt(11));
	const auto& screenShadowDescription = screenSpaceShadow ? screenSpaceShadow.description : localShadowMask.description;
	D3D12_SHADER_RESOURCE_VIEW_DESC screenShadowView{}; screenShadowView.Format=screenShadowDescription.Format; screenShadowView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; screenShadowView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; screenShadowView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(screenShadowNative),&screenShadowView,cpuAt(12));
	D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
	view.Format = composite.description.Format;
	view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	runtime->GetNativeDevice()->CreateUnorderedAccessView(resource, nullptr, &view, cpuAt(13));
	D3D12_UNORDERED_ACCESS_VIEW_DESC markerView{};
	markerView.Format = DXGI_FORMAT_R32_UINT;
	markerView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	runtime->GetNativeDevice()->CreateUnorderedAccessView(static_cast<ID3D12Resource*>(markerNative), nullptr, &markerView, cpuAt(14));
	D3D12_UNORDERED_ACCESS_VIEW_DESC specularOutputView{};
	specularOutputView.Format = specularComposite.description.Format;
	specularOutputView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	runtime->GetNativeDevice()->CreateUnorderedAccessView(static_cast<ID3D12Resource*>(specularOutputNative), nullptr, &specularOutputView, cpuAt(15));
	D3D12_GPU_DESCRIPTOR_HANDLE gpu{ descriptors.gpuHandle };
	auto gpuAt=[&](uint32_t index){auto value=gpu;value.ptr+=uint64_t(index)*descriptors.descriptorSize;return value;};
	ID3D12DescriptorHeap* heaps[]{ static_cast<ID3D12DescriptorHeap*>(descriptors.borrowedNativeHeap) };
	commandList->SetDescriptorHeaps(1, heaps);
	constexpr UINT clearMarker[4]{};
	commandList->ClearUnorderedAccessViewUint(gpuAt(14), cpuAt(14), static_cast<ID3D12Resource*>(markerNative), clearMarker, 0, nullptr);
	commandList->SetComputeRootSignature(rootSignature.get());
	commandList->SetComputeRootConstantBufferView(0,constantsUpload.gpuAddress);
	commandList->SetComputeRootDescriptorTable(1, gpu);
	commandList->SetPipelineState(bootstrapPipeline.get());
	commandList->Dispatch((context.frame->width+7)/8,(context.frame->height+7)/8,1);
	D3D12_RESOURCE_BARRIER compositeBarrier{};
	compositeBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	compositeBarrier.UAV.pResource = resource;
	D3D12_RESOURCE_BARRIER specularBarrier = compositeBarrier;
	specularBarrier.UAV.pResource = static_cast<ID3D12Resource*>(specularOutputNative);
	const D3D12_RESOURCE_BARRIER outputBarriers[]{ compositeBarrier, specularBarrier };
	commandList->ResourceBarrier(ARRAYSIZE(outputBarriers), outputBarriers);
	if (dispatchCount++ == 0)
		logger::info("[DX12DeferredShading] ORG traversed clustered lights/contexts using the G-buffer + linear depth");
	return CS_DX12_OK;
}

void DX12DeferredShading::OnShutdown(void* userData)
{
	static_cast<DX12DeferredShading*>(userData)->Shutdown();
}

void DX12DeferredShading::Shutdown() noexcept
{
	composite = {};
	specularComposite = {};
	linearDepth = {};
	localShadowMask = {};
	screenSpaceShadow = {};
	parityReference = {};
	frameMarker = {};
	packedSurfaceMirror = {};
	linearizeDepthShader = nullptr;
	compositeBlitVS = nullptr;
	compositeBlitPS = nullptr;
	compositeSelectiveBlitPS = nullptr;
	compositeCoverageOverlayPS = nullptr;
	compositeBlitDestination = nullptr;
	compositeBlitRTV = nullptr;
	specularBlitDestination = nullptr;
	specularBlitRTV = nullptr;
	bootstrapPipeline = nullptr;
	rootSignature = nullptr;
	device = nullptr;
	inputs = {};
	parityCounterReadback = nullptr;
	parityCounterPending = false;
	runtime = nullptr;
	registration = {};
	compositeHandle = {};
	specularCompositeHandle = {};
	linearDepthHandle = {};
	frameMarkerHandle = {};
	packedSurfaceMirrorHandle = {};
	localShadowMaskHandle = {};
	screenSpaceShadowHandle = {};
	parityReferenceHandle = {};
	lightsHandle={};contextsHandle={};clustersHandle={};pagesHandle={};
}
