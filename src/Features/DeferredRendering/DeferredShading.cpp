#include "DeferredShading.h"

#include "Features/Effects11/D3D11StateBackup.h"
#include "RenderGraph/DX12RenderRuntime.h"
#include "Deferred.h"
#include "Features/DeferredRendering.h"
#include "Globals.h"
#include "State.h"
#include "TruePBR.h"
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
		float grassSettings0[4]{};
		float grassSettings1[4]{};
		uint32_t iblFlags[4]{};
		float iblSettings0[4]{};
		float iblSettings1[4]{};
		float skylightingPositionOffset[3]{};
		uint32_t skylightingEnabled{};
		uint32_t skylightingArrayOrigin[3]{};
		float skylightingMinDiffuseVisibility{};
		float skylightingMinSpecularVisibility{};
		uint32_t pbrMaterialCount{};
		float lightingParameterPadding[2]{};
	};
	static_assert(sizeof(DeferredConstants) <= 512);

	struct NativePipeline { winrt::com_ptr<ID3D12PipelineState> state; };

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

	std::pair<std::vector<std::byte>, std::filesystem::path> LoadPixelBinningShader()
	{
		const std::filesystem::path paths[]{
			L"Data\\Shaders\\DeferredRendering\\DeferredPixelBinningCS.hlsl",
			std::filesystem::current_path() / L"package\\Shaders\\DeferredRendering\\DeferredPixelBinningCS.hlsl"
		};
		for (const auto& path : paths) {
			std::ifstream stream(path, std::ios::binary | std::ios::ate);
			if (!stream) continue;
			const auto size = stream.tellg();
			if (size <= 0) continue;
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

std::uint32_t DX12DeferredShading::GetEnabledEvaluatorMask() const noexcept
{
	std::uint32_t mask = 0;
	for (std::size_t index = 1; index < evaluatorPipelines.size(); ++index) {
		if (evaluatorPipelines[index])
			mask |= 1u << static_cast<std::uint32_t>(index);
	}
	if (!materialTextureSharingAvailable) {
		constexpr auto textureDependentEvaluators =
			CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBRParallax) |
			CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBRTerrainAdvanced) |
			CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBRLodBlend);
		mask &= ~textureDependentEvaluators;
	}
	if (!glintNoiseAvailable)
		mask &= ~CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBRGlint);
	return mask;
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
	if (!device || !CreatePipeline() || !CreateBinningPipelines())
		return false;
	CSDX12ContributorDesc description{};
	description.structSize = sizeof(description);
	description.apiVersion = CS_DX12_GRAPH_API_CURRENT;
	description.id = "community-shaders.deferred-shading";
	description.kind = CS_DX12_CONTRIBUTOR_REQUIRED;
	description.userData = this;
	description.build = &Build;
	description.generationActivated = &OnGenerationActivated;
	description.deviceLost = &OnDeviceLost;
	description.shutdown = &OnShutdown;
	const bool registered = owner.Register(&description, &registration) == CS_DX12_OK;
	// Promotion begins only after ORG activates a fully compiled generation.
	// The first build therefore renders one compatibility frame by design.
	globals::features::deferredRendering.SetEnabledEvaluatorMask(0u);
	return registered;
#endif
}

bool DX12DeferredShading::CreatePipeline() noexcept
{
#if !defined(ORG_MODULE_SERVICES_HAS_DXC)
	return false;
#else
	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 21, 0, 0, 0 };
	ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 7, 0, 0, 21 };
	D3D12_ROOT_PARAMETER parameters[3]{};
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameters[0].Descriptor.ShaderRegister = 0;
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[1].DescriptorTable = { 2, ranges };
	parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[2].Constants = { 1, 0, 4 };
	parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC root{ 3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
	winrt::com_ptr<ID3DBlob> serialized, errors;
	if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, serialized.put(), errors.put())) ||
		FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(rootSignature.put()))))
		return false;
	D3D12_INDIRECT_ARGUMENT_DESC indirectArguments[2]{};
	indirectArguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
	indirectArguments[0].Constant = { 2, 0, 4 };
	indirectArguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
	D3D12_COMMAND_SIGNATURE_DESC commandSignature{};
	commandSignature.ByteStride = 7 * sizeof(std::uint32_t);
	commandSignature.NumArgumentDescs = static_cast<UINT>(std::size(indirectArguments));
	commandSignature.pArgumentDescs = indirectArguments;
	if (FAILED(device->CreateCommandSignature(&commandSignature, rootSignature.get(),
		IID_PPV_ARGS(evaluatorCommandSignature.put()))))
		return false;
	auto* compiler = runtime->GetShaderCompiler();
	auto* pipelines = runtime->GetPipelineService();
	if (!compiler || !pipelines || !compiler->Available())
		return false;
	auto [shaderSource, shaderDirectory] = LoadDeferredShader();
	if (shaderSource.empty()) {
		logger::error("[DX12DeferredShading] Packaged DeferredCompositeCS.hlsl is unavailable");
		return false;
	}
	auto createEvaluator = [&](CS::Deferred::Evaluator evaluator, std::wstring_view defineValue,
		std::string_view pipelineName) {
		org::services::ShaderCompileRequest request{};
		request.sourceName = fmt::format("DeferredRendering/DeferredCompositeCS.{}.hlsl", pipelineName);
		request.source = shaderSource;
		request.entryPoint = L"main";
		request.target = L"cs_6_0";
		request.defines.push_back({ L"CS_FIXED_EVALUATOR", std::wstring(defineValue) });
		if (evaluator == CS::Deferred::Evaluator::TruePBR ||
			evaluator == CS::Deferred::Evaluator::TruePBRSubsurfaceFuzz ||
			evaluator == CS::Deferred::Evaluator::TruePBRCoat ||
			evaluator == CS::Deferred::Evaluator::TruePBRTerrain ||
			evaluator == CS::Deferred::Evaluator::TruePBRGlint) {
			request.defines.push_back({ L"TRUE_PBR", L"1" });
			request.defines.push_back({ L"CS_DEFERRED_TRUE_PBR", L"1" });
			request.defines.push_back({ L"CSHADER", L"1" });
			if (evaluator == CS::Deferred::Evaluator::TruePBRGlint)
				request.defines.push_back({ L"GLINT", L"1" });
		}
		request.includeDirectories.push_back(shaderDirectory);
		request.includeDirectories.push_back(shaderDirectory.parent_path());
		request.dependencyFiles.push_back(shaderDirectory / L"DeferredLightingCommon.hlsli");
		request.dependencyFiles.push_back(shaderDirectory / L"DeferredMaterial.hlsli");
		request.dependencyFiles.push_back(shaderDirectory / L"DeferredMaterialRegistry.def");
		request.dependencyFiles.push_back(shaderDirectory / L"DeferredTruePBR.hlsli");
		request.dependencyFiles.push_back(shaderDirectory / L"DeferredIndirectLighting.hlsli");
		request.dependencyFiles.push_back((shaderDirectory / L".." / L"Common" / L"LightingParity.hlsli").lexically_normal());
		request.dependencyFiles.push_back((shaderDirectory / L".." / L"Common" / L"PBR.hlsli").lexically_normal());
		request.dependencyFiles.push_back((shaderDirectory / L".." / L"Common" / L"LightingEval.hlsli").lexically_normal());
		auto artifact = compiler->Compile(request);
		if (!artifact) {
			logger::error("[DX12DeferredShading] DXC failed for {} evaluator: {}", pipelineName, artifact.diagnostics);
			return false;
		}
		org::services::PipelineRecipe recipe{};
		recipe.id = fmt::format("community-shaders.deferred-shading.evaluator.{}", pipelineName);
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
		evaluatorPipelines[static_cast<std::size_t>(evaluator)] =
			std::static_pointer_cast<NativePipeline>(result.payload)->state;
		return evaluatorPipelines[static_cast<std::size_t>(evaluator)] != nullptr;
	};
	if (!createEvaluator(CS::Deferred::Evaluator::Generic, L"1", "generic") ||
		!createEvaluator(CS::Deferred::Evaluator::Grass, L"3", "grass"))
		return false;
	auto createOptionalEvaluator = [&](CS::Deferred::Evaluator evaluator,
		std::wstring_view defineValue, std::string_view pipelineName) {
		if (!createEvaluator(evaluator, defineValue, pipelineName))
			logger::warn("[DX12DeferredShading] {} evaluator is unavailable; its pixels remain compatibility-lit",
				pipelineName);
	};
	createOptionalEvaluator(CS::Deferred::Evaluator::DistantTree, L"4", "distant-tree");
	createOptionalEvaluator(CS::Deferred::Evaluator::FoliageSpecial, L"8", "foliage-special");
	createOptionalEvaluator(CS::Deferred::Evaluator::TruePBR, L"2", "true-pbr-core");
	createOptionalEvaluator(CS::Deferred::Evaluator::TruePBRSubsurfaceFuzz, L"9",
		"true-pbr-subsurface-fuzz");
	createOptionalEvaluator(CS::Deferred::Evaluator::TruePBRCoat, L"10", "true-pbr-coat");
	createOptionalEvaluator(CS::Deferred::Evaluator::TruePBRGlint, L"11", "true-pbr-glint");
	createOptionalEvaluator(CS::Deferred::Evaluator::TruePBRTerrain, L"13", "true-pbr-terrain");
	pipelines->PublishReady(0);
	const auto enabledMask = GetEnabledEvaluatorMask();
	logger::info("[DX12DeferredShading] Enabled evaluator mask: 0x{:08X}", enabledMask);
	return true;
#endif
}

bool DX12DeferredShading::CreateBinningPipelines() noexcept
{
#if !defined(ORG_MODULE_SERVICES_HAS_DXC)
	return false;
#else
	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
	ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 6, 0, 0, 1 };
	D3D12_ROOT_PARAMETER parameters[2]{};
	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[0].Constants = { 0, 0, 6 };
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[1].DescriptorTable = { 2, ranges };
	parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC root{ 2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
	winrt::com_ptr<ID3DBlob> serialized, errors;
	if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
		serialized.put(), errors.put())) || FAILED(device->CreateRootSignature(0,
		serialized->GetBufferPointer(), serialized->GetBufferSize(),
		IID_PPV_ARGS(binningRootSignature.put()))))
		return false;
	auto [source, directory] = LoadPixelBinningShader();
	if (source.empty()) {
		logger::error("[DX12DeferredShading] Packaged DeferredPixelBinningCS.hlsl is unavailable");
		return false;
	}
	auto* compiler = runtime->GetShaderCompiler();
	auto* pipelines = runtime->GetPipelineService();
	if (!compiler || !pipelines || !compiler->Available()) return false;
	constexpr const wchar_t* entryPoints[]{ L"ClearBinsCS", L"HistogramCS", L"PrefixSumAndArgsCS", L"ScatterPixelsCS" };
	constexpr const char* ids[]{ "clear", "histogram", "prefix", "scatter" };
	for (std::size_t index = 0; index < std::size(entryPoints); ++index) {
		org::services::ShaderCompileRequest request{};
		request.sourceName = "DeferredRendering/DeferredPixelBinningCS.hlsl";
		request.source = source;
		request.entryPoint = entryPoints[index];
		request.target = L"cs_6_0";
		request.includeDirectories.push_back(directory);
		request.includeDirectories.push_back(directory.parent_path());
		request.dependencyFiles.push_back(directory / L"DeferredMaterial.hlsli");
		request.dependencyFiles.push_back(directory / L"DeferredMaterialRegistry.def");
		auto artifact = compiler->Compile(request);
		if (!artifact) {
			logger::error("[DX12DeferredShading] DXC failed for pixel-binning {}: {}", ids[index], artifact.diagnostics);
			return false;
		}
		org::services::PipelineRecipe recipe{};
		recipe.id = std::string("community-shaders.deferred-shading.pixel-binning.") + ids[index];
		recipe.layoutKey = reinterpret_cast<uintptr_t>(binningRootSignature.get());
		recipe.shaderKey = artifact.key;
		recipe.deviceKey = reinterpret_cast<uintptr_t>(device.get());
		recipe.build = [nativeDevice=device, signature=binningRootSignature,
			bytecode=std::move(artifact.binary)]() mutable {
			auto payload=std::make_shared<NativePipeline>();
			D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
			desc.pRootSignature=signature.get(); desc.CS={bytecode.data(),bytecode.size()};
			if(FAILED(nativeDevice->CreateComputePipelineState(&desc,IID_PPV_ARGS(payload->state.put()))))
				return org::services::PipelinePayload{};
			return std::static_pointer_cast<void>(payload);
		};
		auto result=pipelines->Request(std::move(recipe)).get();
		if(!result) return false;
		binningPipelines[index]=std::static_pointer_cast<NativePipeline>(result.payload)->state;
	}
	return true;
#endif
}

bool DX12DeferredShading::EnsureComposite(uint32_t width, uint32_t height, DXGI_FORMAT format) noexcept
{
	constexpr auto transportFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	if (composite && specularComposite && reflectanceComposite && albedoComposite &&
		normalComposite && masksComposite &&
		composite.description.Width == width && composite.description.Height == height &&
		composite.description.Format == transportFormat)
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
	DX12InteropCoordinator::SharedTexture reflectanceCandidate;
	if (!interop->CreateD3D12OwnedSharedTexture(width, height, transportFormat,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		true, reflectanceCandidate)) {
		composite = {};
		specularComposite = {};
		logger::error("[DX12DeferredShading] Failed to create shared reflectance composite texture");
		return false;
	}
	reflectanceComposite = std::move(reflectanceCandidate);
	auto createMaterialOutput = [&](DX12InteropCoordinator::SharedTexture& output,
		std::string_view name) {
		DX12InteropCoordinator::SharedTexture candidateOutput;
		if (!interop->CreateD3D12OwnedSharedTexture(width, height, transportFormat,
			D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
			true, candidateOutput)) {
			logger::error("[DX12DeferredShading] Failed to create shared {} composite texture", name);
			return false;
		}
		output = std::move(candidateOutput);
		return true;
	};
	if (!createMaterialOutput(albedoComposite, "albedo") ||
		!createMaterialOutput(normalComposite, "normal") ||
		!createMaterialOutput(masksComposite, "masks")) {
		composite = {}; specularComposite = {}; reflectanceComposite = {};
		albedoComposite = {}; normalComposite = {}; masksComposite = {};
		return false;
	}
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
	// Slots 0..143 retain parity diagnostics. Material classification uses two
	// 256-entry histograms beginning at slot 160.
	description.Width = 672;
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
	auto* interop = runtime ? runtime->GetInteropCoordinator() : nullptr;
	if (!interop)
		return false;
	// Compatibility pixels are never read from this texture: the selective
	// D3D11 handoff simply leaves Skyrim's main target untouched. Keep only a
	// tiny valid SRV in ordinary frames, and allocate/copy the full-size forward
	// reference exclusively for the opt-in same-frame parity diagnostic.
	if (!std::getenv("CS_DX12_DEFERRED_PARITY")) {
		if (compatibilityReference && compatibilityReference.description.Width == 1 &&
			compatibilityReference.description.Height == 1 &&
			compatibilityReference.description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
			return true;
		return interop->CreateD3D12OwnedSharedTexture(1, 1,
			DXGI_FORMAT_R16G16B16A16_FLOAT,
			D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, true,
			compatibilityReference);
	}
	return interop->EnsureReadOnlyMirror(source, compatibilityReference) &&
		interop->CopyToMirror(globals::d3d::context, source, compatibilityReference);
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
	constexpr size_t importedIndices[]{ 0, 1, 2, 3, 4 };
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

bool DX12DeferredShading::PrepareIndirectLightingInputs() noexcept
{
	auto* interop = runtime ? runtime->GetInteropCoordinator() : nullptr;
	if (!interop)
		return false;
	auto import = [&](ID3D11Texture2D* source, ImportedInput& input, const char* name) {
		if (!source || !interop->ImportReadOnlySharedTexture(source, false, input.mirror)) {
			logger::error("[DX12DeferredShading] Failed to import indirect-lighting input '{}'", name);
			return false;
		}
		input.source.copy_from(source);
		return true;
	};

	const auto& ibl = globals::features::ibl;
	const auto& skylighting = globals::features::skylighting;
	iblInputsAvailable = ibl.envIBLTexture && ibl.skyIBLTexture &&
		import(ibl.envIBLTexture->resource.get(), envIBLInput, "ibl.environment-sh") &&
		import(ibl.skyIBLTexture->resource.get(), skyIBLInput, "ibl.sky-sh");
	skylightingInputsAvailable = skylighting.texProbeArray && skylighting.texShadowVisibility &&
		import(skylighting.texProbeArray->resource.get(), skylightingProbeInput, "skylighting.probes") &&
		import(skylighting.texShadowVisibility->resource.get(), skylightingVisibilityInput,
			"skylighting.shadow-visibility");

	auto ensureNeutral2D = [&](ImportedInput& input, uint32_t width, uint32_t height,
		DXGI_FORMAT format, const char* name) {
		if (input.mirror && input.mirror.description.Width == width &&
			input.mirror.description.Height == height && input.mirror.description.Format == format &&
			input.mirror.description.ArraySize == 1)
			return true;
		input = {};
		if (!interop->CreateD3D12OwnedSharedTexture(width, height, format,
			D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, true, input.mirror)) {
			logger::error("[DX12DeferredShading] Failed to create neutral input '{}'", name);
			return false;
		}
		return true;
	};
	auto ensureNeutralArray = [&](ImportedInput& input, DXGI_FORMAT format, const char* name) {
		if (input.mirror && input.mirror.description.ArraySize == 1 &&
			input.mirror.description.Format == format)
			return true;
		input = {};
		if (!interop->CreateD3D12OwnedSharedTextureArray(1, 1, 1, format,
			D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, true, input.mirror)) {
			logger::error("[DX12DeferredShading] Failed to create neutral array input '{}'", name);
			return false;
		}
		return true;
	};

	if (!iblInputsAvailable &&
		(!ensureNeutral2D(envIBLInput, 3, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
			"ibl.environment-sh") ||
		 !ensureNeutral2D(skyIBLInput, 3, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
			"ibl.sky-sh")))
		return false;
	if (!skylightingInputsAvailable &&
		(!ensureNeutralArray(skylightingProbeInput, DXGI_FORMAT_R16G16B16A16_FLOAT,
			"skylighting.probes") ||
		 !ensureNeutralArray(skylightingVisibilityInput, DXGI_FORMAT_R8_UNORM,
			"skylighting.shadow-visibility")))
		return false;
	return true;
}

bool DX12DeferredShading::PrepareGlintNoiseInput() noexcept
{
	auto* interop = runtime ? runtime->GetInteropCoordinator() : nullptr;
	auto& truePBR = globals::features::truePBR;
	if (!interop)
		return false;
	auto ensureDisabledPlaceholder = [&]() {
		if (glintNoiseInput.mirror)
			return true;
		D3D11_TEXTURE2D_DESC description{};
		description.Width = 1;
		description.Height = 1;
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		return interop->CreateSharedTexture(description, true, false, glintNoiseInput.mirror);
	};
	if (!truePBR.glintsNoiseTexture || !truePBR.glintsNoiseTexture->resource) {
		glintNoiseAvailable = false;
		return ensureDisabledPlaceholder();
	}
	auto* source = truePBR.glintsNoiseTexture->resource.get();
	if (glintNoiseInput.source.get() == source && glintNoiseInput.mirror)
		return true;
	glintNoiseInput.source.copy_from(source);
	if (interop->ImportReadOnlySharedTexture(source, false, glintNoiseInput.mirror)) {
		glintNoiseAvailable = true;
		return true;
	}
	glintNoiseAvailable = false;
	glintNoiseInput.mirror = {};
	if (!ensureDisabledPlaceholder())
		return false;
	static std::once_flag warning;
	std::call_once(warning, [] {
		logger::warn("[DX12DeferredShading] Direct glint-noise sharing is unavailable; only deferred glints remain compatibility-lit");
	});
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
	if (!handoffConstants) {
		D3D11_BUFFER_DESC description{};
		description.ByteWidth = 16;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if (FAILED(globals::d3d::device->CreateBuffer(&description, nullptr, handoffConstants.put()))) {
			logger::error("[DX12DeferredShading] Failed to create evaluator handoff constants");
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
	auto* reflectanceDestination = globals::game::renderer ?
		globals::game::renderer->GetRuntimeData().renderTargets[REFLECTANCE].texture : nullptr;
	if (!reflectanceDestination)
		return false;
	if (reflectanceBlitDestination.get() != reflectanceDestination) {
		winrt::com_ptr<ID3D11RenderTargetView> view;
		if (FAILED(globals::d3d::device->CreateRenderTargetView(reflectanceDestination, nullptr, view.put()))) {
			logger::error("[DX12DeferredShading] Failed to create reflectance-target RTV for deferred handoff");
			return false;
		}
		reflectanceBlitDestination.copy_from(reflectanceDestination);
		reflectanceBlitRTV = std::move(view);
	}
	auto ensureMaterialDestination = [&](RE::RENDER_TARGET target,
		winrt::com_ptr<ID3D11Texture2D>& cachedTexture,
		winrt::com_ptr<ID3D11RenderTargetView>& cachedView) {
		auto* texture = globals::game::renderer->GetRuntimeData().renderTargets[target].texture;
		if (!texture) return false;
		if (cachedTexture.get() == texture) return true;
		winrt::com_ptr<ID3D11RenderTargetView> view;
		if (FAILED(globals::d3d::device->CreateRenderTargetView(texture, nullptr, view.put())))
			return false;
		cachedTexture.copy_from(texture); cachedView = std::move(view); return true;
	};
	if (!ensureMaterialDestination(ALBEDO, albedoBlitDestination, albedoBlitRTV) ||
		!ensureMaterialDestination(NORMALROUGHNESS, normalBlitDestination, normalBlitRTV) ||
		!ensureMaterialDestination(MASKS, masksBlitDestination, masksBlitRTV))
		return false;
	return true;
}

bool DX12DeferredShading::CommitComposite(ID3D11Texture2D* destination) noexcept
{
	if(!destination||!composite.d3d11||!specularComposite.d3d11||
		!reflectanceComposite.d3d11||!albedoComposite.d3d11||!normalComposite.d3d11||
		!masksComposite.d3d11||!frameMarker.d3d11||!globals::d3d::context||
		!EnsureCompositeBlit(destination))return false;
	const bool parityReadbackEnabled = std::getenv("CS_DX12_DEFERRED_PARITY") != nullptr;
	const bool classificationReadbackEnabled = globals::features::deferredRendering.IsMaterialClassificationEnabled();
	if (parityReadbackEnabled || classificationReadbackEnabled) {
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
				auto report = [&](std::uint32_t base, std::string_view evaluator) {
					if (!counters[base])
						return;
					const double scale = 1.0 / (4096.0 * counters[base]);
					const auto* signedCounters = reinterpret_cast<const std::int32_t*>(counters);
					logger::info("[DX12DeferredParity] evaluator={}, pixels={}, meanAbsRGB=({:.6f},{:.6f},{:.6f}), maxComponent={:.6f}",
						evaluator, counters[base], counters[base + 1] * scale,
						counters[base + 2] * scale, counters[base + 3] * scale,
						counters[base + 4] / 4096.0);
					logger::info("[DX12DeferredParity] evaluator={}, mean(candidate-reference)=({:.6f},{:.6f},{:.6f}), meanCandidate=({:.6f},{:.6f},{:.6f}), meanReference=({:.6f},{:.6f},{:.6f})",
						evaluator, signedCounters[base + 5] * scale,
						signedCounters[base + 6] * scale, signedCounters[base + 7] * scale,
						counters[base + 8] * scale, counters[base + 9] * scale,
						counters[base + 10] * scale, counters[base + 11] * scale,
						counters[base + 12] * scale, counters[base + 13] * scale);
				};
				if (parityReadbackEnabled) {
					report(0, "grass");
					report(16, "generic");
					report(32, "distant-tree");
					report(48, "foliage-special");
					report(64, "true-pbr-core");
					report(80, "true-pbr-subsurface-fuzz");
					report(96, "true-pbr-coat");
					report(112, "true-pbr-terrain");
					report(128, "true-pbr-glint");
				}
				if (classificationReadbackEnabled) {
					constexpr std::uint32_t visibleBase = 160;
					constexpr std::uint32_t deferredBase = visibleBase + 256;
					globals::features::deferredRendering.UpdateMaterialClassification(
						counters + visibleBase, counters + deferredBase, GetEnabledEvaluatorMask());
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
	const std::uint32_t handoffData[4]{ GetEnabledEvaluatorMask(), 0, 0, 0 };
	globals::d3d::context->UpdateSubresource(handoffConstants.get(), 0, nullptr, handoffData, 0, 0);
	ID3D11Buffer* handoffBuffers[]{ handoffConstants.get() };
	globals::d3d::context->PSSetConstantBuffers(0, 1, handoffBuffers);
	const bool visualizeCoverage = globals::features::deferredRendering.IsCoverageVisualizationEnabled();
	// The existing D3D11 deferred composite consumes SPECULAR separately from
	// main color. Replace it first so promoted pixels are not double-lit while
	// compatibility pixels retain their geometry-evaluated value.
	auto* specularTarget = specularBlitRTV.get();
	globals::d3d::context->OMSetRenderTargets(1, &specularTarget, nullptr);
	globals::d3d::context->PSSetShader(compositeSelectiveBlitPS.get(), nullptr, 0);
	ID3D11ShaderResourceView* specularSource[]{ specularComposite.srv11.get(), packedSurfaceMirror.srv11.get() };
	globals::d3d::context->PSSetShaderResources(0, ARRAYSIZE(specularSource), specularSource);
	globals::d3d::context->Draw(3, 0);
	ID3D11ShaderResourceView* nullSpecular[2]{};
	globals::d3d::context->PSSetShaderResources(0, ARRAYSIZE(nullSpecular), nullSpecular);

	// Special foliage and TruePBR temporarily repurpose REFLECTANCE as raster
	// payload. Restore its canonical downstream value only for those evaluators.
	const std::uint32_t reflectanceHandoffData[4]{
		GetEnabledEvaluatorMask() &
			(CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::FoliageSpecial) |
			 CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBR) |
			 CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBRSubsurfaceFuzz) |
			 CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBRCoat) |
			 CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBRGlint) |
			 CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBRTerrain)), 0, 0, 0 };
	globals::d3d::context->UpdateSubresource(handoffConstants.get(), 0, nullptr,
		reflectanceHandoffData, 0, 0);
	auto* reflectanceTarget = reflectanceBlitRTV.get();
	globals::d3d::context->OMSetRenderTargets(1, &reflectanceTarget, nullptr);
	ID3D11ShaderResourceView* reflectanceSource[]{
		reflectanceComposite.srv11.get(), packedSurfaceMirror.srv11.get() };
	globals::d3d::context->PSSetShaderResources(0, ARRAYSIZE(reflectanceSource), reflectanceSource);
	globals::d3d::context->Draw(3, 0);
	globals::d3d::context->PSSetShaderResources(0, ARRAYSIZE(nullSpecular), nullSpecular);
	globals::d3d::context->UpdateSubresource(handoffConstants.get(), 0, nullptr,
		handoffData, 0, 0);
	// TruePBR temporarily repurposes all material G-buffer fields. Restore the
	// canonical outputs only for parity-gated TruePBR evaluators.
	const std::uint32_t pbrHandoffData[4]{
		GetEnabledEvaluatorMask() &
			(CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBR) |
			 CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBRSubsurfaceFuzz) |
			 CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBRCoat) |
			 CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBRGlint) |
			 CS::Deferred::EvaluatorBit(CS::Deferred::Evaluator::TruePBRTerrain)), 0, 0, 0 };
	globals::d3d::context->UpdateSubresource(handoffConstants.get(), 0, nullptr,
		pbrHandoffData, 0, 0);
	auto restoreMaterialTarget = [&](ID3D11RenderTargetView* targetView,
		ID3D11ShaderResourceView* sourceView) {
		globals::d3d::context->OMSetRenderTargets(1, &targetView, nullptr);
		ID3D11ShaderResourceView* materialSources[]{ sourceView, packedSurfaceMirror.srv11.get() };
		globals::d3d::context->PSSetShaderResources(0, ARRAYSIZE(materialSources), materialSources);
		globals::d3d::context->Draw(3, 0);
		ID3D11ShaderResourceView* nullMaterialSources[2]{};
		globals::d3d::context->PSSetShaderResources(0, ARRAYSIZE(nullMaterialSources), nullMaterialSources);
	};
	restoreMaterialTarget(albedoBlitRTV.get(), albedoComposite.srv11.get());
	restoreMaterialTarget(normalBlitRTV.get(), normalComposite.srv11.get());
	restoreMaterialTarget(masksBlitRTV.get(), masksComposite.srv11.get());
	globals::d3d::context->UpdateSubresource(handoffConstants.get(), 0, nullptr,
		handoffData, 0, 0);

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
		inputs[2].source && inputs[2].mirror && inputs[3].source && inputs[3].mirror;
}

CSDX12Status DX12DeferredShading::Build(void* userData, CSDX12BuildHandle build)
{
	auto* self = static_cast<DX12DeferredShading*>(userData);
	// Imported textures retain Skyrim's allocation extent; frame.width/height
	// carry the independently varying active dynamic-resolution extent.
	const auto width = self->runtime->GetAllocationWidth();
	const auto height = self->runtime->GetAllocationHeight();
	if (!self->composite || !self->specularComposite || !self->reflectanceComposite ||
		!self->albedoComposite || !self->normalComposite || !self->masksComposite)
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
	if (!self->compatibilityReference)
		return CS_DX12_E_NOT_READY;
	if (!self->envIBLInput.mirror || !self->skyIBLInput.mirror ||
		!self->skylightingProbeInput.mirror || !self->skylightingVisibilityInput.mirror ||
		!self->glintNoiseInput.mirror)
		return CS_DX12_E_NOT_READY;

	constexpr size_t importedIndices[]{ 0, 1, 2, 3, 4 };
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
	auto declareLightingInput = [&](ImportedInput& imported, const char* id,
		CSDX12ResourceHandle& handle) {
		const auto& nativeDescription = imported.mirror.description;
		CSDX12ResourceDesc input{};
		input.structSize = sizeof(input); input.apiVersion = CS_DX12_GRAPH_API_CURRENT;
		input.id = id; input.lifetime = CS_DX12_RESOURCE_CS_IMPORTED;
		input.dimension = CS_DX12_RESOURCE_TEXTURE_2D; input.sizing = CS_DX12_SIZE_ABSOLUTE;
		input.format = nativeDescription.Format; input.width = nativeDescription.Width;
		input.height = nativeDescription.Height; input.depthOrArraySize = nativeDescription.ArraySize;
		input.mipLevels = nativeDescription.MipLevels; input.sampleCount = nativeDescription.SampleDesc.Count;
		input.allowedAccess = CS_DX12_ACCESS_SHADER_READ; input.initialAccess = CS_DX12_ACCESS_SHADER_READ;
		input.finalAccess = CS_DX12_ACCESS_SHADER_READ; input.borrowedNativeResource = imported.mirror.d3d12.get();
		const auto status = self->runtime->DeclareResource(build, &input, &handle);
		if (status != CS_DX12_OK)
			logger::error("[DX12DeferredShading] DeclareResource '{}' failed: status={}", id,
				static_cast<unsigned>(status));
		return status;
	};
	if (declareLightingInput(self->envIBLInput, "community-shaders.deferred-shading.ibl.environment-sh", self->envIBLHandle) != CS_DX12_OK ||
		declareLightingInput(self->skyIBLInput, "community-shaders.deferred-shading.ibl.sky-sh", self->skyIBLHandle) != CS_DX12_OK ||
		declareLightingInput(self->skylightingProbeInput, "community-shaders.deferred-shading.skylighting.probes", self->skylightingProbeHandle) != CS_DX12_OK ||
		declareLightingInput(self->skylightingVisibilityInput, "community-shaders.deferred-shading.skylighting.shadow-visibility",
			self->skylightingVisibilityHandle) != CS_DX12_OK ||
		declareLightingInput(self->glintNoiseInput, "community-shaders.deferred-shading.true-pbr.glint-noise",
			self->glintNoiseHandle) != CS_DX12_OK)
		return CS_DX12_E_INTERNAL;

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
	{
		CSDX12ResourceDesc reference{};
		reference.structSize = sizeof(reference); reference.apiVersion = CS_DX12_GRAPH_API_CURRENT;
		reference.id = "community-shaders.deferred-shading.compatibility-reference";
		reference.lifetime = CS_DX12_RESOURCE_CS_IMPORTED; reference.dimension = CS_DX12_RESOURCE_TEXTURE_2D;
		reference.sizing = CS_DX12_SIZE_ABSOLUTE; reference.format = self->compatibilityReference.description.Format;
		reference.width = self->compatibilityReference.description.Width; reference.height = self->compatibilityReference.description.Height;
		reference.depthOrArraySize = 1; reference.mipLevels = 1; reference.sampleCount = 1;
		reference.allowedAccess = CS_DX12_ACCESS_SHADER_READ; reference.initialAccess = CS_DX12_ACCESS_SHADER_READ;
		reference.finalAccess = CS_DX12_ACCESS_SHADER_READ; reference.borrowedNativeResource = self->compatibilityReference.d3d12.get();
		if (self->runtime->DeclareResource(build, &reference, &self->compatibilityReferenceHandle) != CS_DX12_OK)
			return CS_DX12_E_INTERNAL;
	}
	if (self->runtime->FindResource(build, "community-shaders.clustered-lighting.lights", &self->lightsHandle) != CS_DX12_OK ||
		self->runtime->FindResource(build, "community-shaders.clustered-lighting.contexts", &self->contextsHandle) != CS_DX12_OK ||
		self->runtime->FindResource(build, "community-shaders.clustered-lighting.pbr-materials", &self->pbrMaterialsHandle) != CS_DX12_OK ||
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
	resource.id = "community-shaders.deferred-shading.reflectance-composite";
	resource.format = self->reflectanceComposite.description.Format;
	resource.borrowedNativeResource = self->reflectanceComposite.d3d12.get();
	if (self->runtime->DeclareResource(build, &resource, &self->reflectanceCompositeHandle) != CS_DX12_OK)
		return CS_DX12_E_INTERNAL;
	auto declareMaterialOutput = [&](const char* id,
		DX12InteropCoordinator::SharedTexture& output, CSDX12ResourceHandle& handle) {
		resource.id = id; resource.format = output.description.Format;
		resource.borrowedNativeResource = output.d3d12.get();
		return self->runtime->DeclareResource(build, &resource, &handle) == CS_DX12_OK;
	};
	if (!declareMaterialOutput("community-shaders.deferred-shading.albedo-composite",
			self->albedoComposite, self->albedoCompositeHandle) ||
		!declareMaterialOutput("community-shaders.deferred-shading.normal-composite",
			self->normalComposite, self->normalCompositeHandle) ||
		!declareMaterialOutput("community-shaders.deferred-shading.masks-composite",
			self->masksComposite, self->masksCompositeHandle))
		return CS_DX12_E_INTERNAL;
	CSDX12ResourceDesc marker{};
	marker.structSize = sizeof(marker);
	marker.apiVersion = CS_DX12_GRAPH_API_CURRENT;
	marker.id = "community-shaders.deferred-shading.frame-marker";
	marker.lifetime = CS_DX12_RESOURCE_CS_IMPORTED;
	marker.dimension = CS_DX12_RESOURCE_TEXTURE_2D;
	marker.sizing = CS_DX12_SIZE_ABSOLUTE;
	marker.format = DXGI_FORMAT_R32_UINT;
	marker.width = 144;
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
	packedMirror.format = DXGI_FORMAT_R32G32B32A32_UINT;
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

	auto declareBuffer = [&](const char* id, std::uint64_t byteSize, std::uint32_t stride,
		std::uint32_t allowedAccess, CSDX12ResourceHandle& handle) {
		CSDX12ResourceDesc buffer{};
		buffer.structSize = sizeof(buffer);
		buffer.apiVersion = CS_DX12_GRAPH_API_CURRENT;
		buffer.id = id;
		buffer.lifetime = CS_DX12_RESOURCE_TRANSIENT;
		buffer.dimension = CS_DX12_RESOURCE_BUFFER;
		buffer.sizing = CS_DX12_SIZE_ABSOLUTE;
		buffer.byteSize = byteSize;
		buffer.structureByteStride = stride;
		buffer.allowedAccess = allowedAccess;
		buffer.initialAccess = CS_DX12_ACCESS_NONE;
		buffer.finalAccess = CS_DX12_ACCESS_NONE;
		return self->runtime->DeclareResource(build, &buffer, &handle);
	};
	constexpr auto evaluatorCount = CS::Deferred::kEvaluatorCount;
	constexpr std::uint32_t indirectStride = 7 * sizeof(std::uint32_t);
	if (declareBuffer("community-shaders.deferred-shading.evaluator-counts",
		evaluatorCount * sizeof(std::uint32_t), sizeof(std::uint32_t),
		CS_DX12_ACCESS_SHADER_READ | CS_DX12_ACCESS_UNORDERED_WRITE, self->evaluatorCountsHandle) != CS_DX12_OK ||
		declareBuffer("community-shaders.deferred-shading.evaluator-offsets",
			evaluatorCount * sizeof(std::uint32_t), sizeof(std::uint32_t),
			CS_DX12_ACCESS_SHADER_READ | CS_DX12_ACCESS_UNORDERED_WRITE, self->evaluatorOffsetsHandle) != CS_DX12_OK ||
		declareBuffer("community-shaders.deferred-shading.evaluator-cursors",
			evaluatorCount * sizeof(std::uint32_t), sizeof(std::uint32_t),
			CS_DX12_ACCESS_UNORDERED_WRITE, self->evaluatorCursorsHandle) != CS_DX12_OK ||
		declareBuffer("community-shaders.deferred-shading.evaluator-pixels",
			std::uint64_t(width) * height * sizeof(std::uint32_t) * 2u, sizeof(std::uint32_t) * 2u,
			CS_DX12_ACCESS_SHADER_READ | CS_DX12_ACCESS_UNORDERED_WRITE, self->evaluatorPixelListHandle) != CS_DX12_OK ||
		declareBuffer("community-shaders.deferred-shading.evaluator-indirect-args",
			evaluatorCount * indirectStride, 0,
			CS_DX12_ACCESS_UNORDERED_WRITE | CS_DX12_ACCESS_INDIRECT_ARGUMENT, self->evaluatorIndirectArgsHandle) != CS_DX12_OK)
		return CS_DX12_E_INTERNAL;

	auto access = [](CSDX12ResourceHandle handle, std::uint32_t mode) {
		return CSDX12ResourceAccessDesc{ sizeof(CSDX12ResourceAccessDesc),
			CS_DX12_GRAPH_API_CURRENT, handle, mode, {} };
	};
	auto declarePass = [&](const char* id, const char* afterID,
		std::span<const CSDX12ResourceAccessDesc> resourceAccesses, CSDX12ExecuteCallback execute) {
		const char* dependencies[]{ afterID };
		CSDX12PassDesc binPass{};
		binPass.structSize = sizeof(binPass);
		binPass.apiVersion = CS_DX12_GRAPH_API_CURRENT;
		binPass.id = id;
		binPass.queuePolicy = CS_DX12_QUEUE_REQUIRE_COMPUTE;
		binPass.after = dependencies;
		binPass.afterCount = 1;
		binPass.accesses = resourceAccesses.data();
		binPass.accessCount = static_cast<std::uint32_t>(resourceAccesses.size());
		binPass.execute = execute;
		CSDX12PassHandle handle{};
		return self->runtime->DeclarePass(build, &binPass, &handle);
	};
	const std::array clearAccesses{
		access(self->evaluatorCountsHandle, CS_DX12_ACCESS_UNORDERED_WRITE),
		access(self->evaluatorOffsetsHandle, CS_DX12_ACCESS_UNORDERED_WRITE),
		access(self->evaluatorCursorsHandle, CS_DX12_ACCESS_UNORDERED_WRITE),
		access(self->evaluatorIndirectArgsHandle, CS_DX12_ACCESS_UNORDERED_WRITE),
		access(self->frameMarkerHandle, CS_DX12_ACCESS_UNORDERED_WRITE) };
	if (declarePass("community-shaders.deferred-shading.bin-clear", "community-shaders.clustered-lighting.cull",
		clearAccesses, &ExecuteBinClear) != CS_DX12_OK) return CS_DX12_E_INTERNAL;
	const std::array histogramAccesses{
		access(self->packedSurfaceMirrorHandle, CS_DX12_ACCESS_SHADER_READ),
		access(self->evaluatorCountsHandle, CS_DX12_ACCESS_UNORDERED_WRITE),
		access(self->frameMarkerHandle, CS_DX12_ACCESS_UNORDERED_WRITE) };
	if (declarePass("community-shaders.deferred-shading.bin-histogram", "community-shaders.deferred-shading.bin-clear",
		histogramAccesses, &ExecuteBinHistogram) != CS_DX12_OK) return CS_DX12_E_INTERNAL;
	const std::array prefixAccesses{
		access(self->evaluatorCountsHandle, CS_DX12_ACCESS_UNORDERED_WRITE),
		access(self->evaluatorOffsetsHandle, CS_DX12_ACCESS_UNORDERED_WRITE),
		access(self->evaluatorIndirectArgsHandle, CS_DX12_ACCESS_UNORDERED_WRITE) };
	if (declarePass("community-shaders.deferred-shading.bin-prefix", "community-shaders.deferred-shading.bin-histogram",
		prefixAccesses, &ExecuteBinPrefix) != CS_DX12_OK) return CS_DX12_E_INTERNAL;
	const std::array scatterAccesses{
		access(self->packedSurfaceMirrorHandle, CS_DX12_ACCESS_SHADER_READ),
		access(self->evaluatorOffsetsHandle, CS_DX12_ACCESS_UNORDERED_WRITE),
		access(self->evaluatorCursorsHandle, CS_DX12_ACCESS_UNORDERED_WRITE),
		access(self->evaluatorPixelListHandle, CS_DX12_ACCESS_UNORDERED_WRITE) };
	if (declarePass("community-shaders.deferred-shading.bin-scatter", "community-shaders.deferred-shading.bin-prefix",
		scatterAccesses, &ExecuteBinScatter) != CS_DX12_OK) return CS_DX12_E_INTERNAL;

	const char* after[]{ "community-shaders.deferred-shading.bin-scatter" };
	const char* before[]{ CS_DX12_ANCHOR_DEFERRED_LIGHTING_BEGIN };
	std::array<CSDX12ResourceAccessDesc, 29> accesses{};
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
	accesses[15]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->evaluatorPixelListHandle,CS_DX12_ACCESS_SHADER_READ,{}};
	accesses[16]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->evaluatorIndirectArgsHandle,CS_DX12_ACCESS_INDIRECT_ARGUMENT,{}};
	accesses[17]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->compatibilityReferenceHandle,CS_DX12_ACCESS_SHADER_READ,{}};
	accesses[18]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->envIBLHandle,CS_DX12_ACCESS_SHADER_READ,{}};
	accesses[19]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->skyIBLHandle,CS_DX12_ACCESS_SHADER_READ,{}};
	accesses[20]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->skylightingProbeHandle,CS_DX12_ACCESS_SHADER_READ,{}};
	accesses[21]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->skylightingVisibilityHandle,CS_DX12_ACCESS_SHADER_READ,{}};
	accesses[22]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->inputs[2].handle,CS_DX12_ACCESS_SHADER_READ,{}};
	accesses[23]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->reflectanceCompositeHandle,CS_DX12_ACCESS_UNORDERED_WRITE,{}};
	accesses[24]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->albedoCompositeHandle,CS_DX12_ACCESS_UNORDERED_WRITE,{}};
	accesses[25]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->normalCompositeHandle,CS_DX12_ACCESS_UNORDERED_WRITE,{}};
	accesses[26]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->masksCompositeHandle,CS_DX12_ACCESS_UNORDERED_WRITE,{}};
	accesses[27]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->pbrMaterialsHandle,CS_DX12_ACCESS_SHADER_READ,{}};
	accesses[28]={sizeof(CSDX12ResourceAccessDesc),CS_DX12_GRAPH_API_CURRENT,self->glintNoiseHandle,CS_DX12_ACCESS_SHADER_READ,{}};
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
	pass.accessCount = static_cast<uint32_t>(accesses.size());
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

CSDX12Status DX12DeferredShading::ExecuteBinClear(void* userData, const CSDX12ExecutionContext* context)
{
	return userData && context ? static_cast<DX12DeferredShading*>(userData)->RecordBinning(*context, BinningStage::Clear) : CS_DX12_E_INVALID_ARGUMENT;
}

CSDX12Status DX12DeferredShading::ExecuteBinHistogram(void* userData, const CSDX12ExecutionContext* context)
{
	return userData && context ? static_cast<DX12DeferredShading*>(userData)->RecordBinning(*context, BinningStage::Histogram) : CS_DX12_E_INVALID_ARGUMENT;
}

CSDX12Status DX12DeferredShading::ExecuteBinPrefix(void* userData, const CSDX12ExecutionContext* context)
{
	return userData && context ? static_cast<DX12DeferredShading*>(userData)->RecordBinning(*context, BinningStage::Prefix) : CS_DX12_E_INVALID_ARGUMENT;
}

CSDX12Status DX12DeferredShading::ExecuteBinScatter(void* userData, const CSDX12ExecutionContext* context)
{
	return userData && context ? static_cast<DX12DeferredShading*>(userData)->RecordBinning(*context, BinningStage::Scatter) : CS_DX12_E_INVALID_ARGUMENT;
}

CSDX12Status DX12DeferredShading::RecordBinning(const CSDX12ExecutionContext& context, BinningStage stage) noexcept
{
	if (!context.GetResource || !context.AllocateDescriptors || !context.borrowedD3D12GraphicsCommandList ||
		!binningRootSignature || !binningPipelines[static_cast<std::size_t>(stage)])
		return CS_DX12_E_UNSUPPORTED_CAPABILITY;
	void* packedNative{}; void* countsNative{}; void* offsetsNative{}; void* cursorsNative{};
	void* pixelsNative{}; void* argumentsNative{}; void* markerNative{};
	if (context.GetResource(&context, packedSurfaceMirrorHandle, &packedNative) != CS_DX12_OK || !packedNative ||
		context.GetResource(&context, evaluatorCountsHandle, &countsNative) != CS_DX12_OK || !countsNative ||
		context.GetResource(&context, evaluatorOffsetsHandle, &offsetsNative) != CS_DX12_OK || !offsetsNative ||
		context.GetResource(&context, evaluatorCursorsHandle, &cursorsNative) != CS_DX12_OK || !cursorsNative ||
		context.GetResource(&context, evaluatorPixelListHandle, &pixelsNative) != CS_DX12_OK || !pixelsNative ||
		context.GetResource(&context, evaluatorIndirectArgsHandle, &argumentsNative) != CS_DX12_OK || !argumentsNative ||
		context.GetResource(&context, frameMarkerHandle, &markerNative) != CS_DX12_OK || !markerNative)
		return CS_DX12_E_NOT_READY;
	CSDX12DescriptorAllocation descriptors{};
	if (context.AllocateDescriptors(&context, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 7, &descriptors) != CS_DX12_OK)
		return CS_DX12_E_INTERNAL;
	auto cpuAt=[&](uint32_t index){D3D12_CPU_DESCRIPTOR_HANDLE value{descriptors.cpuHandle};value.ptr+=uint64_t(index)*descriptors.descriptorSize;return value;};
	D3D12_SHADER_RESOURCE_VIEW_DESC packedView{};
	packedView.Format=DXGI_FORMAT_R32G32B32A32_UINT; packedView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
	packedView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; packedView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(packedNative),&packedView,cpuAt(0));
	auto structuredUAV=[&](void* native,uint32_t slot,uint32_t stride,uint32_t elements){
		D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
		view.Buffer.NumElements=elements; view.Buffer.StructureByteStride=stride;
		device->CreateUnorderedAccessView(static_cast<ID3D12Resource*>(native),nullptr,&view,cpuAt(slot));
	};
	structuredUAV(countsNative,1,sizeof(std::uint32_t),CS::Deferred::kEvaluatorCount);
	structuredUAV(offsetsNative,2,sizeof(std::uint32_t),CS::Deferred::kEvaluatorCount);
	structuredUAV(cursorsNative,3,sizeof(std::uint32_t),CS::Deferred::kEvaluatorCount);
	structuredUAV(pixelsNative,4,sizeof(std::uint32_t)*2,context.frame->width*context.frame->height);
	D3D12_UNORDERED_ACCESS_VIEW_DESC argumentView{}; argumentView.Format=DXGI_FORMAT_R32_TYPELESS;
	argumentView.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
	argumentView.Buffer.NumElements=CS::Deferred::kEvaluatorCount*7;
	argumentView.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
	device->CreateUnorderedAccessView(static_cast<ID3D12Resource*>(argumentsNative),nullptr,&argumentView,cpuAt(5));
	D3D12_UNORDERED_ACCESS_VIEW_DESC classificationView{};
	classificationView.Format = DXGI_FORMAT_R32_UINT;
	classificationView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	device->CreateUnorderedAccessView(static_cast<ID3D12Resource*>(markerNative), nullptr,
		&classificationView, cpuAt(6));
	auto* commandList=static_cast<ID3D12GraphicsCommandList*>(context.borrowedD3D12GraphicsCommandList);
	ID3D12DescriptorHeap* heaps[]{static_cast<ID3D12DescriptorHeap*>(descriptors.borrowedNativeHeap)};
	commandList->SetDescriptorHeaps(1,heaps);
	commandList->SetComputeRootSignature(binningRootSignature.get());
	const std::uint32_t constants[]{context.frame->width,context.frame->height,
		CS::Deferred::kEvaluatorCount,7u*sizeof(std::uint32_t),GetEnabledEvaluatorMask(),
		globals::features::deferredRendering.IsMaterialClassificationEnabled() ? 1u : 0u};
	commandList->SetComputeRoot32BitConstants(0,std::size(constants),constants,0);
	commandList->SetComputeRootDescriptorTable(1,D3D12_GPU_DESCRIPTOR_HANDLE{descriptors.gpuHandle});
	commandList->SetPipelineState(binningPipelines[static_cast<std::size_t>(stage)].get());
	switch(stage){
	case BinningStage::Clear: commandList->Dispatch(11u,1,1); break;
	case BinningStage::Histogram:
	case BinningStage::Scatter: commandList->Dispatch((context.frame->width+7)/8,(context.frame->height+7)/8,1); break;
	case BinningStage::Prefix: commandList->Dispatch(1,1,1); break;
	}
	return CS_DX12_OK;
}

CSDX12Status DX12DeferredShading::Record(const CSDX12ExecutionContext& context) noexcept
{
	if (!context.GetResource || !context.AllocateUpload || !context.AllocateDescriptors || !context.borrowedD3D12GraphicsCommandList || GetEnabledEvaluatorMask() == 0 || !evaluatorCommandSignature)
		return CS_DX12_E_UNSUPPORTED_CAPABILITY;
	void* native{}; void* specularOutputNative{}; void* reflectanceOutputNative{}; void* albedoOutputNative{}; void* normalOutputNative{}; void* masksOutputNative{}; void* albedoNative{}; void* specularInputNative{}; void* reflectanceInputNative{}; void* masksNative{}; void* depthNative{}; void* lightsNative{}; void* contextsNative{}; void* pbrMaterialsNative{}; void* clustersNative{}; void* pagesNative{}; void* shadowMaskNative{}; void* screenShadowNative{}; void* markerNative{}; void* packedMirrorNative{}; void* parityNative{}; void* evaluatorPixelsNative{}; void* evaluatorArgsNative{}; void* envIBLNative{}; void* skyIBLNative{}; void* skylightingProbeNative{}; void* skylightingVisibilityNative{}; void* glintNoiseNative{};
	if (context.GetResource(&context, compositeHandle, &native) != CS_DX12_OK || !native ||
		context.GetResource(&context, specularCompositeHandle, &specularOutputNative) != CS_DX12_OK || !specularOutputNative ||
		context.GetResource(&context, reflectanceCompositeHandle, &reflectanceOutputNative) != CS_DX12_OK || !reflectanceOutputNative ||
		context.GetResource(&context, albedoCompositeHandle, &albedoOutputNative) != CS_DX12_OK || !albedoOutputNative ||
		context.GetResource(&context, normalCompositeHandle, &normalOutputNative) != CS_DX12_OK || !normalOutputNative ||
		context.GetResource(&context, masksCompositeHandle, &masksOutputNative) != CS_DX12_OK || !masksOutputNative ||
		context.GetResource(&context, inputs[0].handle, &albedoNative) != CS_DX12_OK || !albedoNative ||
		context.GetResource(&context, inputs[1].handle, &specularInputNative) != CS_DX12_OK || !specularInputNative ||
		context.GetResource(&context, inputs[2].handle, &reflectanceInputNative) != CS_DX12_OK || !reflectanceInputNative ||
		context.GetResource(&context, linearDepthHandle, &depthNative) != CS_DX12_OK || !depthNative ||
		context.GetResource(&context, lightsHandle, &lightsNative) != CS_DX12_OK || !lightsNative ||
		context.GetResource(&context, contextsHandle, &contextsNative) != CS_DX12_OK || !contextsNative ||
		context.GetResource(&context, pbrMaterialsHandle, &pbrMaterialsNative) != CS_DX12_OK || !pbrMaterialsNative ||
		context.GetResource(&context, clustersHandle, &clustersNative) != CS_DX12_OK || !clustersNative ||
		context.GetResource(&context, pagesHandle, &pagesNative) != CS_DX12_OK || !pagesNative ||
		context.GetResource(&context, localShadowMaskHandle, &shadowMaskNative) != CS_DX12_OK || !shadowMaskNative ||
		context.GetResource(&context, screenSpaceShadowHandle, &screenShadowNative) != CS_DX12_OK || !screenShadowNative ||
		context.GetResource(&context, frameMarkerHandle, &markerNative) != CS_DX12_OK || !markerNative ||
		context.GetResource(&context, packedSurfaceMirrorHandle, &packedMirrorNative) != CS_DX12_OK || !packedMirrorNative ||
		context.GetResource(&context, inputs[4].handle, &masksNative) != CS_DX12_OK || !masksNative ||
		context.GetResource(&context, evaluatorPixelListHandle, &evaluatorPixelsNative) != CS_DX12_OK || !evaluatorPixelsNative ||
		context.GetResource(&context, evaluatorIndirectArgsHandle, &evaluatorArgsNative) != CS_DX12_OK || !evaluatorArgsNative ||
		context.GetResource(&context, envIBLHandle, &envIBLNative) != CS_DX12_OK || !envIBLNative ||
		context.GetResource(&context, skyIBLHandle, &skyIBLNative) != CS_DX12_OK || !skyIBLNative ||
		context.GetResource(&context, skylightingProbeHandle, &skylightingProbeNative) != CS_DX12_OK || !skylightingProbeNative ||
		context.GetResource(&context, skylightingVisibilityHandle, &skylightingVisibilityNative) != CS_DX12_OK || !skylightingVisibilityNative ||
		context.GetResource(&context, glintNoiseHandle, &glintNoiseNative) != CS_DX12_OK || !glintNoiseNative)
		return CS_DX12_E_NOT_READY;
	const bool parityEnabled = std::getenv("CS_DX12_DEFERRED_PARITY") != nullptr;
	if (context.GetResource(&context, compatibilityReferenceHandle, &parityNative) != CS_DX12_OK || !parityNative)
		return CS_DX12_E_NOT_READY;
	auto snapshot=globals::features::deferredRendering.GetFrameSnapshot();
	if(!snapshot)return CS_DX12_E_NOT_READY;
	CSDX12UploadAllocation constantsUpload{};
	if(context.AllocateUpload(&context,512,256,&constantsUpload)!=CS_DX12_OK)return CS_DX12_E_INTERNAL;
	DeferredConstants constants{}; constants.projectionInverse=snapshot->projectionInverse; constants.viewInverse=snapshot->cameraViewInverse;
	constants.clusterGrid[0]=(context.frame->width+63)/64; constants.clusterGrid[1]=(context.frame->height+63)/64; constants.clusterGrid[2]=32;
	constants.lightCount=static_cast<uint32_t>(snapshot->lights.size()); constants.screenSize[0]=static_cast<float>(context.frame->width); constants.screenSize[1]=static_cast<float>(context.frame->height);
	constants.nearPlane=snapshot->nearPlane; constants.farPlane=snapshot->farPlane; constants.contextCount=static_cast<uint32_t>(snapshot->contexts.size());
	constants.pageCapacity=constants.clusterGrid[0]*constants.clusterGrid[1]*constants.clusterGrid[2]*10;
	constants.pbrMaterialCount=static_cast<uint32_t>(snapshot->pbrMaterials.size());
	constants.abiVersion=CS::Deferred::kAbiVersion;
	constants.lightingTransform=snapshot->lightingTransform;
	constants.grassSettings0[0] = snapshot->grassLighting.Glossiness;
	constants.grassSettings0[1] = snapshot->grassLighting.SpecularStrength;
	constants.grassSettings0[2] = snapshot->grassLighting.SubsurfaceScatteringAmount;
	constants.grassSettings0[3] = snapshot->grassLighting.OverrideComplexGrassSettings ? 1.0f : 0.0f;
	constants.grassSettings1[0] = snapshot->grassLighting.BasicGrassBrightness;
	constants.grassSettings1[1] = snapshot->grassLighting.ComplexGrassThreshold;
	constants.iblFlags[0] = snapshot->ibl.EnableIBL && iblInputsAvailable;
	constants.iblFlags[1] = snapshot->ibl.PreserveFogLuminance;
	constants.iblFlags[2] = snapshot->ibl.UseStaticIBL;
	constants.iblFlags[3] = snapshot->ibl.DALCMode;
	constants.iblSettings0[0] = snapshot->ibl.DALCAmount;
	constants.iblSettings0[1] = snapshot->ibl.EnvIBLScale;
	constants.iblSettings0[2] = snapshot->ibl.SkyIBLScale;
	constants.iblSettings0[3] = snapshot->ibl.EnvIBLSaturation;
	constants.iblSettings1[0] = snapshot->ibl.SkyIBLSaturation;
	constants.iblSettings1[1] = snapshot->ibl.FogAmount;
	std::copy_n(&snapshot->skylighting.PosOffset.x, 3, constants.skylightingPositionOffset);
	constants.skylightingEnabled = snapshot->skylightingEnabled && skylightingInputsAvailable ? 1u : 0u;
	std::copy_n(snapshot->skylighting.ArrayOrigin, 3, constants.skylightingArrayOrigin);
	constants.skylightingMinDiffuseVisibility = snapshot->skylighting.MinDiffuseVisibility;
	constants.skylightingMinSpecularVisibility = snapshot->skylighting.MinSpecularVisibility;
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
	if (context.AllocateDescriptors(&context, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 28, &descriptors) != CS_DX12_OK)
		return CS_DX12_E_INTERNAL;
	auto* resource = static_cast<ID3D12Resource*>(native);
	auto* commandList = static_cast<ID3D12GraphicsCommandList*>(context.borrowedD3D12GraphicsCommandList);
	D3D12_CPU_DESCRIPTOR_HANDLE visibleCpu{ descriptors.cpuHandle };
	auto cpuAt=[&](uint32_t index){auto value=visibleCpu;value.ptr+=uint64_t(index)*descriptors.descriptorSize;return value;};
	D3D12_SHADER_RESOURCE_VIEW_DESC albedoView{}; albedoView.Format=inputs[0].mirror.description.Format; albedoView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; albedoView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; albedoView.Texture2D.MipLevels=1;
	D3D12_SHADER_RESOURCE_VIEW_DESC specularView=albedoView; specularView.Format=inputs[1].mirror.description.Format;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(specularInputNative),&specularView,cpuAt(0));
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(albedoNative),&albedoView,cpuAt(1));
	D3D12_SHADER_RESOURCE_VIEW_DESC depthView{}; depthView.Format=DXGI_FORMAT_R32_FLOAT; depthView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; depthView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; depthView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(depthNative),&depthView,cpuAt(2));
	auto structured=[&](void* nativeBuffer,uint32_t slot,uint32_t stride){auto* buffer=static_cast<ID3D12Resource*>(nativeBuffer);D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Buffer.NumElements=static_cast<UINT>(buffer->GetDesc().Width/stride);srv.Buffer.StructureByteStride=stride;device->CreateShaderResourceView(buffer,&srv,cpuAt(slot));};
	structured(lightsNative,3,sizeof(DeferredRendering::LightData)); structured(contextsNative,4,sizeof(DeferredRendering::LightingContext)); structured(clustersNative,5,48); structured(pagesNative,6,56);
	D3D12_SHADER_RESOURCE_VIEW_DESC packedView{}; packedView.Format=DXGI_FORMAT_R32G32B32A32_UINT; packedView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; packedView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; packedView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(packedMirrorNative),&packedView,cpuAt(7));
	D3D12_SHADER_RESOURCE_VIEW_DESC shadowMaskView{}; shadowMaskView.Format=localShadowMask.description.Format; shadowMaskView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; shadowMaskView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; shadowMaskView.Texture2D.MipLevels=localShadowMask.description.MipLevels;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(shadowMaskNative),&shadowMaskView,cpuAt(8));
	void* normalNative{};
	if (context.GetResource(&context, inputs[3].handle, &normalNative) != CS_DX12_OK || !normalNative)
		return CS_DX12_E_NOT_READY;
	D3D12_SHADER_RESOURCE_VIEW_DESC normalView{}; normalView.Format=inputs[3].mirror.description.Format; normalView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; normalView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; normalView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(normalNative),&normalView,cpuAt(9));
	D3D12_SHADER_RESOURCE_VIEW_DESC parityView{}; parityView.Format=compatibilityReference.description.Format; parityView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; parityView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; parityView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(parityNative),&parityView,cpuAt(10));
	D3D12_SHADER_RESOURCE_VIEW_DESC masksView{}; masksView.Format=inputs[4].mirror.description.Format; masksView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; masksView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; masksView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(masksNative),&masksView,cpuAt(11));
	const auto& screenShadowDescription = screenSpaceShadow ? screenSpaceShadow.description : localShadowMask.description;
	D3D12_SHADER_RESOURCE_VIEW_DESC screenShadowView{}; screenShadowView.Format=screenShadowDescription.Format; screenShadowView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; screenShadowView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; screenShadowView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(screenShadowNative),&screenShadowView,cpuAt(12));
	D3D12_SHADER_RESOURCE_VIEW_DESC pixelListView{}; pixelListView.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
	pixelListView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	pixelListView.Buffer.NumElements=context.frame->width*context.frame->height;
	pixelListView.Buffer.StructureByteStride=sizeof(std::uint32_t)*2;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(evaluatorPixelsNative),&pixelListView,cpuAt(13));
	D3D12_SHADER_RESOURCE_VIEW_DESC iblView{}; iblView.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
	iblView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; iblView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	iblView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(envIBLNative),&iblView,cpuAt(14));
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(skyIBLNative),&iblView,cpuAt(15));
	D3D12_SHADER_RESOURCE_VIEW_DESC probeView{}; probeView.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
	probeView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY; probeView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	probeView.Texture2DArray.MipLevels=1; probeView.Texture2DArray.ArraySize=skylightingProbeInput.mirror.description.ArraySize;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(skylightingProbeNative),&probeView,cpuAt(16));
	D3D12_SHADER_RESOURCE_VIEW_DESC visibilityView=probeView; visibilityView.Format=DXGI_FORMAT_R8_UNORM;
	visibilityView.Texture2DArray.ArraySize=skylightingVisibilityInput.mirror.description.ArraySize;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(skylightingVisibilityNative),&visibilityView,cpuAt(17));
	D3D12_SHADER_RESOURCE_VIEW_DESC reflectanceView=specularView;
	reflectanceView.Format=inputs[2].mirror.description.Format;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(reflectanceInputNative),&reflectanceView,cpuAt(18));
	structured(pbrMaterialsNative,19,sizeof(CS::Deferred::PBRMaterialRecord));
	D3D12_SHADER_RESOURCE_VIEW_DESC glintNoiseView{};
	glintNoiseView.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
	glintNoiseView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
	glintNoiseView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	glintNoiseView.Texture2D.MipLevels=1;
	device->CreateShaderResourceView(static_cast<ID3D12Resource*>(glintNoiseNative),&glintNoiseView,cpuAt(20));
	D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
	view.Format = composite.description.Format;
	view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	runtime->GetNativeDevice()->CreateUnorderedAccessView(resource, nullptr, &view, cpuAt(21));
	D3D12_UNORDERED_ACCESS_VIEW_DESC markerView{};
	markerView.Format = DXGI_FORMAT_R32_UINT;
	markerView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	runtime->GetNativeDevice()->CreateUnorderedAccessView(static_cast<ID3D12Resource*>(markerNative), nullptr, &markerView, cpuAt(22));
	D3D12_UNORDERED_ACCESS_VIEW_DESC specularOutputView{};
	specularOutputView.Format = specularComposite.description.Format;
	specularOutputView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	runtime->GetNativeDevice()->CreateUnorderedAccessView(static_cast<ID3D12Resource*>(specularOutputNative), nullptr, &specularOutputView, cpuAt(23));
	D3D12_UNORDERED_ACCESS_VIEW_DESC reflectanceOutputView{};
	reflectanceOutputView.Format = reflectanceComposite.description.Format;
	reflectanceOutputView.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	runtime->GetNativeDevice()->CreateUnorderedAccessView(
		static_cast<ID3D12Resource*>(reflectanceOutputNative), nullptr,
		&reflectanceOutputView, cpuAt(24));
	runtime->GetNativeDevice()->CreateUnorderedAccessView(static_cast<ID3D12Resource*>(albedoOutputNative), nullptr, &reflectanceOutputView, cpuAt(25));
	runtime->GetNativeDevice()->CreateUnorderedAccessView(static_cast<ID3D12Resource*>(normalOutputNative), nullptr, &reflectanceOutputView, cpuAt(26));
	runtime->GetNativeDevice()->CreateUnorderedAccessView(static_cast<ID3D12Resource*>(masksOutputNative), nullptr, &reflectanceOutputView, cpuAt(27));
	D3D12_GPU_DESCRIPTOR_HANDLE gpu{ descriptors.gpuHandle };
	auto gpuAt=[&](uint32_t index){auto value=gpu;value.ptr+=uint64_t(index)*descriptors.descriptorSize;return value;};
	ID3D12DescriptorHeap* heaps[]{ static_cast<ID3D12DescriptorHeap*>(descriptors.borrowedNativeHeap) };
	commandList->SetDescriptorHeaps(1, heaps);
	commandList->SetComputeRootSignature(rootSignature.get());
	commandList->SetComputeRootConstantBufferView(0,constantsUpload.gpuAddress);
	commandList->SetComputeRootDescriptorTable(1, gpu);
	// Commands are laid out in stable Evaluator order.  Each evaluator owns its
	// PSO but consumes the same compacted pixel list and indirect-command ABI.
	// Compatibility intentionally has no PSO: those pixels retain Skyrim's
	// pre-lit result.  Future evaluators only need to populate their table slot.
	for (std::size_t evaluator = 1; evaluator < evaluatorPipelines.size(); ++evaluator) {
		if (!evaluatorPipelines[evaluator])
			continue;
		commandList->SetPipelineState(evaluatorPipelines[evaluator].get());
		commandList->ExecuteIndirect(evaluatorCommandSignature.get(), 1,
			static_cast<ID3D12Resource*>(evaluatorArgsNative),
			static_cast<UINT64>(evaluator) * 7u * sizeof(std::uint32_t), nullptr, 0);
	}
	D3D12_RESOURCE_BARRIER compositeBarrier{};
	compositeBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	compositeBarrier.UAV.pResource = resource;
	D3D12_RESOURCE_BARRIER specularBarrier = compositeBarrier;
	specularBarrier.UAV.pResource = static_cast<ID3D12Resource*>(specularOutputNative);
	D3D12_RESOURCE_BARRIER reflectanceBarrier = compositeBarrier;
	reflectanceBarrier.UAV.pResource = static_cast<ID3D12Resource*>(reflectanceOutputNative);
	D3D12_RESOURCE_BARRIER albedoBarrier = compositeBarrier;
	albedoBarrier.UAV.pResource = static_cast<ID3D12Resource*>(albedoOutputNative);
	D3D12_RESOURCE_BARRIER normalBarrier = compositeBarrier;
	normalBarrier.UAV.pResource = static_cast<ID3D12Resource*>(normalOutputNative);
	D3D12_RESOURCE_BARRIER masksBarrier = compositeBarrier;
	masksBarrier.UAV.pResource = static_cast<ID3D12Resource*>(masksOutputNative);
	const D3D12_RESOURCE_BARRIER outputBarriers[]{ compositeBarrier, specularBarrier,
		reflectanceBarrier, albedoBarrier, normalBarrier, masksBarrier };
	commandList->ResourceBarrier(ARRAYSIZE(outputBarriers), outputBarriers);
	if (dispatchCount++ == 0)
		logger::info("[DX12DeferredShading] ORG executed enabled material evaluators from histogram/prefix-sum pixel bins using ExecuteIndirect");
	return CS_DX12_OK;
}

void DX12DeferredShading::OnShutdown(void* userData)
{
	static_cast<DX12DeferredShading*>(userData)->Shutdown();
}

void DX12DeferredShading::OnGenerationActivated(void* userData,
	CSDX12GenerationHandle) noexcept
{
	auto* self = static_cast<DX12DeferredShading*>(userData);
	globals::features::deferredRendering.SetEnabledEvaluatorMask(
		self ? self->GetEnabledEvaluatorMask() : 0u);
}

void DX12DeferredShading::OnDeviceLost(void*, std::uint32_t) noexcept
{
	globals::features::deferredRendering.SetEnabledEvaluatorMask(0u);
}

void DX12DeferredShading::Shutdown() noexcept
{
	globals::features::deferredRendering.SetEnabledEvaluatorMask(0);
	composite = {};
	specularComposite = {};
	reflectanceComposite = {};
	albedoComposite = {};
	normalComposite = {};
	masksComposite = {};
	linearDepth = {};
	localShadowMask = {};
	screenSpaceShadow = {};
	compatibilityReference = {};
	frameMarker = {};
	packedSurfaceMirror = {};
	glintNoiseInput = {};
	materialTextureSharingAvailable = false;
	glintNoiseAvailable = false;
	linearizeDepthShader = nullptr;
	compositeBlitVS = nullptr;
	compositeBlitPS = nullptr;
	compositeSelectiveBlitPS = nullptr;
	compositeCoverageOverlayPS = nullptr;
	compositeBlitDestination = nullptr;
	compositeBlitRTV = nullptr;
	specularBlitDestination = nullptr;
	specularBlitRTV = nullptr;
	reflectanceBlitDestination = nullptr;
	reflectanceBlitRTV = nullptr;
	albedoBlitDestination = nullptr;
	albedoBlitRTV = nullptr;
	normalBlitDestination = nullptr;
	normalBlitRTV = nullptr;
	masksBlitDestination = nullptr;
	masksBlitRTV = nullptr;
	handoffConstants = nullptr;
	evaluatorPipelines = {};
	rootSignature = nullptr;
	binningPipelines = {};
	binningRootSignature = nullptr;
	evaluatorCommandSignature = nullptr;
	device = nullptr;
	inputs = {};
	parityCounterReadback = nullptr;
	parityCounterPending = false;
	runtime = nullptr;
	registration = {};
	compositeHandle = {};
	specularCompositeHandle = {};
	reflectanceCompositeHandle = {};
	albedoCompositeHandle = {};
	normalCompositeHandle = {};
	masksCompositeHandle = {};
	evaluatorCountsHandle = {};
	evaluatorOffsetsHandle = {};
	evaluatorCursorsHandle = {};
	evaluatorPixelListHandle = {};
	evaluatorIndirectArgsHandle = {};
	linearDepthHandle = {};
	frameMarkerHandle = {};
	packedSurfaceMirrorHandle = {};
	localShadowMaskHandle = {};
	screenSpaceShadowHandle = {};
	compatibilityReferenceHandle = {};
	glintNoiseHandle = {};
	lightsHandle={};contextsHandle={};pbrMaterialsHandle={};clustersHandle={};pagesHandle={};
}
