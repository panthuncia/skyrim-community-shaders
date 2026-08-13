#include "DeferredShading.h"
#include <OpenRenderGraph/ContributorRuntime.h>

#include "Features/Effects11/D3D11StateBackup.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "Deferred.h"
#include "Features/DeferredRendering.h"
#include "DeferredShadingExtension.h"
#include "Globals.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/D3D.h"

#include <Render/Runtime/DescriptorServiceAccess.h>
#include <Render/Runtime/UploadServiceAccess.h>
#include <rhi_helpers.h>

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
		Matrix viewInverse;
		Matrix viewProjectionInverse;
		float cameraData[4];
		float dynamicResolutionScale[2];
		float reconstructionPadding[2];
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
		uint32_t debugView{};
		float lightingParameterPadding{};
	};
	static_assert(sizeof(DeferredConstants) <= 512);

	bool CreateComputePipeline(rhi::Device device, const rhi::PipelineLayoutPtr& layout,
		const std::vector<std::byte>& bytecode, std::string_view entryPoint,
		rhi::PipelinePtr& result) noexcept
	{
		if (!device || !layout || bytecode.empty()) return false;
		const rhi::SubobjLayout pipelineLayout{ layout->GetHandle() };
		const rhi::SubobjShader shader{ rhi::ShaderStage::Compute,
			rhi::ShaderBinary{ bytecode.data(), static_cast<std::uint32_t>(bytecode.size()) },
			std::string(entryPoint) };
		const rhi::PipelineStreamItem stream[]{ rhi::Make(pipelineLayout), rhi::Make(shader) };
		return rhi::IsOk(device.CreatePipeline(stream, static_cast<std::uint32_t>(std::size(stream)), result)) && result;
	}

	std::vector<std::byte> LoadPrecompiledShader(const wchar_t* name)
	{
		const std::filesystem::path paths[]{
			std::filesystem::path(L"Data\\Shaders\\DeferredRendering") / name,
			std::filesystem::current_path() / L"build/ALL/DeferredNativeShaders" / name
		};
		for (const auto& path : paths) {
			std::ifstream stream(path, std::ios::binary | std::ios::ate);
			if (!stream) continue;
			const auto length = stream.tellg();
			if (length <= 0) continue;
			std::vector<std::byte> bytes(static_cast<size_t>(length));
			stream.seekg(0);
			if (stream.read(reinterpret_cast<char*>(bytes.data()), length)) return bytes;
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
	return mask;
}

bool DX12DeferredShading::Initialize(RenderGraphRuntime& owner)
{
	runtime = &owner;
	if (!owner.GetRHIDevice() || !CreatePipeline() || !CreateBinningPipelines())
		throw std::runtime_error("Required deferred shading pipelines could not be created");
	try {
		org::contributor::ExtensionRegistry::Descriptor native{};
		native.id = "community-shaders.deferred-shading.native";
		native.kind = org::contributor::ExtensionRegistry::Kind::Required;
		native.exportedResources = {
			"community-shaders.deferred-shading.gbuffer.albedo", "community-shaders.deferred-shading.gbuffer.specular",
			"community-shaders.deferred-shading.gbuffer.reflectance", "community-shaders.deferred-shading.gbuffer.normal-roughness",
			"community-shaders.deferred-shading.gbuffer.masks", "community-shaders.deferred-shading.gbuffer.masks2-mirror",
			"community-shaders.deferred-shading.linear-depth", "community-shaders.deferred-shading.local-shadow-mask",
			"community-shaders.deferred-shading.screen-space-shadow", "community-shaders.deferred-shading.compatibility-reference",
			"community-shaders.deferred-shading.ibl.environment-sh", "community-shaders.deferred-shading.ibl.sky-sh",
			"community-shaders.deferred-shading.skylighting.probes", "community-shaders.deferred-shading.skylighting.shadow-visibility",
			"community-shaders.deferred-shading.true-pbr.glint-noise", "community-shaders.deferred-shading.composite",
			"community-shaders.deferred-shading.specular-composite", "community-shaders.deferred-shading.reflectance-composite",
			"community-shaders.deferred-shading.albedo-composite", "community-shaders.deferred-shading.normal-composite",
			"community-shaders.deferred-shading.masks-composite", "community-shaders.deferred-shading.evaluator-counts",
			"community-shaders.deferred-shading.evaluator-offsets", "community-shaders.deferred-shading.evaluator-cursors",
			"community-shaders.deferred-shading.evaluator-pixels", "community-shaders.deferred-shading.evaluator-indirect-args",
			"community-shaders.deferred-shading.frame-marker", "community-shaders.deferred-shading.frame-constants"
		};
		native.requiredImports = { "community-shaders.clustered-lighting.lights",
			"community-shaders.clustered-lighting.contexts", "community-shaders.clustered-lighting.pbr-materials",
			"community-shaders.clustered-lighting.clusters", "community-shaders.clustered-lighting.pages" };
		native.factory = [this] { return std::make_unique<DeferredShadingExtension>(*this); };
		native.activated = [this](std::uint64_t) {
			globals::features::deferredRendering.SetEnabledEvaluatorMask(GetEnabledEvaluatorMask());
		};
		native.retired = [](std::uint64_t) {};
		native.diagnostic = [](std::string_view message) { logger::error("[DX12DeferredShading] Native graph contributor: {}", message); };
		nativeRegistration = owner.GetContributorRuntime()->RegisterExtension(std::move(native));
		owner.RequestGraphRebuild();
	} catch (const std::exception& error) {
		throw std::runtime_error(std::format(
			"Deferred native graph registration failed: {}", error.what()));
	}
	// Promotion begins only after ORG activates a fully compiled generation.
	// The first build therefore renders one compatibility frame by design.
	globals::features::deferredRendering.SetEnabledEvaluatorMask(0u);
	return true;
}

bool DX12DeferredShading::CreatePipeline()
{
	auto rhiDevice = runtime ? runtime->GetRHIDevice() : rhi::Device{};
	rhi::PushConstantRangeDesc evaluatorConstants[]{
		// EvaluatorDispatch contains five scalar uints followed by uint4 values.
		// HLSL aligns the first uint4 to the next 16-byte register, so the cbuffer
		// occupies 36 DWORDs (including three padding DWORDs), not 33.
		{ rhi::ShaderStage::Compute, 36, 0, 1, rhi::PushConstantRangeType::RootConstants32 }
	};
	if (!rhiDevice || !rhi::IsOk(rhiDevice.CreatePipelineLayout(
		{ {}, evaluatorConstants, {}, rhi::PF_None }, evaluatorLayout))) return false;
	rhi::IndirectArg indirectArguments[]{
		{ .kind = rhi::IndirectArgKind::Constant, .u = { .rootConstants = { 0, 0, 4 } } },
		{ .kind = rhi::IndirectArgKind::Dispatch }
	};
	const rhi::CommandSignatureDesc evaluatorSignature{
		rhi::Span<rhi::IndirectArg>{ indirectArguments,
			static_cast<std::uint32_t>(std::size(indirectArguments)) },
		7u * sizeof(std::uint32_t)
	};
	if (!rhi::IsOk(rhiDevice.CreateCommandSignature(
		evaluatorSignature, evaluatorLayout->GetHandle(),
		evaluatorCommandSignature))) return false;
	auto createPrecompiled = [&](CS::Deferred::Evaluator evaluator, const wchar_t* file) {
		auto bytecode = LoadPrecompiledShader(file);
		return CreateComputePipeline(rhiDevice, evaluatorLayout, bytecode, "main",
			evaluatorPipelines[static_cast<size_t>(evaluator)]);
	};
	const bool allActiveEvaluatorsLoaded =
		createPrecompiled(CS::Deferred::Evaluator::Generic, L"DeferredEvaluator.generic.dxil") &&
		createPrecompiled(CS::Deferred::Evaluator::Grass, L"DeferredEvaluator.grass.dxil") &&
		createPrecompiled(CS::Deferred::Evaluator::DistantTree, L"DeferredEvaluator.distant-tree.dxil") &&
		createPrecompiled(CS::Deferred::Evaluator::FoliageSpecial, L"DeferredEvaluator.foliage-special.dxil") &&
		createPrecompiled(CS::Deferred::Evaluator::TruePBR, L"DeferredEvaluator.true-pbr-core.dxil") &&
		createPrecompiled(CS::Deferred::Evaluator::TruePBRSubsurfaceFuzz, L"DeferredEvaluator.true-pbr-subsurface-fuzz.dxil") &&
		createPrecompiled(CS::Deferred::Evaluator::TruePBRCoat, L"DeferredEvaluator.true-pbr-coat.dxil") &&
		createPrecompiled(CS::Deferred::Evaluator::TruePBRGlint, L"DeferredEvaluator.true-pbr-glint.dxil") &&
		createPrecompiled(CS::Deferred::Evaluator::TruePBRTerrain, L"DeferredEvaluator.true-pbr-terrain.dxil");
	if (!allActiveEvaluatorsLoaded)
		throw std::runtime_error(
			"One or more active deferred material evaluator pipelines are unavailable");
	logger::info("[DX12DeferredShading] Loaded precompiled evaluator mask: 0x{:08X}", GetEnabledEvaluatorMask());
	return true;
}

bool DX12DeferredShading::CreateBinningPipelines() noexcept
{
	auto rhiDevice = runtime ? runtime->GetRHIDevice() : rhi::Device{};
	rhi::PushConstantRangeDesc binningConstants[]{
		{ rhi::ShaderStage::Compute, 17, 0, 0, rhi::PushConstantRangeType::RootConstants32 }
	};
	if (!rhiDevice || !rhi::IsOk(rhiDevice.CreatePipelineLayout(
		{ {}, binningConstants, {}, rhi::PF_None }, binningLayout))) return false;
	if (!CreateComputePipeline(rhiDevice, binningLayout,
		LoadPrecompiledShader(L"DeferredSeed.dxil"), "main", seedPipeline)) {
		logger::error("[DX12DeferredShading] Required precompiled seed DXIL is unavailable");
		return false;
	}
	constexpr const wchar_t* precompiled[]{ L"DeferredBinning.clear.dxil",
		L"DeferredBinning.histogram.dxil", L"DeferredBinning.prefix.dxil",
		L"DeferredBinning.scatter.dxil" };
	bool loaded = true;
	for (size_t index = 0; index < std::size(precompiled); ++index) {
		auto bytecode = LoadPrecompiledShader(precompiled[index]);
		if (bytecode.empty()) { loaded = false; break; }
		if (!CreateComputePipeline(rhiDevice, binningLayout, bytecode, "main",
			binningPipelines[index])) { loaded = false; break; }
	}
	if (loaded) return true;
	seedPipeline.Reset();
	for (auto& pipeline : binningPipelines) pipeline.Reset();
	logger::error("[DX12DeferredShading] Required precompiled binning DXIL is unavailable");
	return false;
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
	org::interop::D3D11Interop::SharedTexture candidate;
	if (!interop->CreateGraphOwnedSharedTexture(width, height, rhi::helpers::ToRHI(transportFormat),
		rhi::RF_AllowRenderTarget | rhi::RF_AllowUnorderedAccess,
		true, candidate)) {
		logger::error("[DX12DeferredShading] Failed to create {}x{} shared composite texture", width, height);
		return false;
	}
	composite = std::move(candidate);
	org::interop::D3D11Interop::SharedTexture specularCandidate;
	if (!interop->CreateGraphOwnedSharedTexture(width, height, rhi::helpers::ToRHI(transportFormat),
		rhi::RF_AllowRenderTarget | rhi::RF_AllowUnorderedAccess,
		true, specularCandidate)) {
		composite = {};
		logger::error("[DX12DeferredShading] Failed to create shared specular composite texture");
		return false;
	}
	specularComposite = std::move(specularCandidate);
	org::interop::D3D11Interop::SharedTexture reflectanceCandidate;
	if (!interop->CreateGraphOwnedSharedTexture(width, height, rhi::helpers::ToRHI(transportFormat),
		rhi::RF_AllowRenderTarget | rhi::RF_AllowUnorderedAccess,
		true, reflectanceCandidate)) {
		composite = {};
		specularComposite = {};
		logger::error("[DX12DeferredShading] Failed to create shared reflectance composite texture");
		return false;
	}
	reflectanceComposite = std::move(reflectanceCandidate);
	auto createMaterialOutput = [&](org::interop::D3D11Interop::SharedTexture& output,
		std::string_view name) {
		org::interop::D3D11Interop::SharedTexture candidateOutput;
		if (!interop->CreateGraphOwnedSharedTexture(width, height, rhi::helpers::ToRHI(transportFormat),
			rhi::RF_AllowRenderTarget | rhi::RF_AllowUnorderedAccess,
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
	org::interop::D3D11Interop::SharedTexture candidate;
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
	// 0..673 are parity/classification counters. 674+ are reserved for
	// frame-control validation written by native evaluator passes.
	description.Width = 678;
	description.Height = 1;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = DXGI_FORMAT_R32_UINT;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	org::interop::D3D11Interop::SharedTexture candidate;
	if (!interop->CreateGraphOwnedSharedTexture(description.Width, description.Height,
		rhi::helpers::ToRHI(description.Format), rhi::RF_AllowRenderTarget |
			rhi::RF_AllowUnorderedAccess, true, candidate)) {
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
	// Every persistent graph output is seeded from this current-frame image
	// before selective evaluators run. Without a full-size refresh, pixels that
	// leave a material bin retain arbitrary history from the previous camera
	// position (most visibly coverage-debug colors).
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
		if (!interop->CreateGraphOwnedSharedTexture(width, height, rhi::helpers::ToRHI(format),
			rhi::RF_AllowRenderTarget, true, input.mirror)) {
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
		if (!interop->CreateGraphOwnedSharedTextureArray(1, 1, 1, rhi::helpers::ToRHI(format),
			rhi::RF_AllowRenderTarget, true, input.mirror)) {
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

bool DX12DeferredShading::PrepareGlintNoiseInput()
{
	auto* interop = runtime ? runtime->GetInteropCoordinator() : nullptr;
	auto& truePBR = globals::features::truePBR;
	if (!interop)
		throw std::runtime_error("Deferred glint-noise import has no interop coordinator");
	if (!truePBR.glintsNoiseTexture || !truePBR.glintsNoiseTexture->resource) {
		throw std::runtime_error(
			"Deferred TruePBR glint evaluator is active but its noise texture is unavailable");
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
	throw std::runtime_error(
		"Deferred TruePBR glint-noise sharing failed; refusing to disable the material evaluator");
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
	const bool debugViewReadbackEnabled = globals::features::deferredRendering.IsDebugViewEnabled();
	const bool classificationReadbackEnabled = globals::features::deferredRendering.IsMaterialClassificationEnabled();
	if (parityReadbackEnabled || classificationReadbackEnabled || debugViewReadbackEnabled) {
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
						counters + visibleBase, counters + deferredBase, GetEnabledEvaluatorMask(),
						counters[deferredBase + 256], counters[deferredBase + 257]);
				}
				if (debugViewReadbackEnabled) {
					static std::uint32_t lastReportedView = UINT32_MAX;
					if (lastReportedView != counters[674]) {
						lastReportedView = counters[674];
						logger::info("[DeferredRendering] GPU consumed component view {} in {} evaluator dispatches",
							counters[674], counters[675]);
					}
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
	globals::d3d::context->UpdateSubresource(handoffConstants.get(), 0, nullptr,
		handoffData, 0, 0);
	// Every evaluator now produces the canonical downstream mask values. Restore
	// them for all promoted pixels so raster variants do not retain forward
	// ambient-lighting work merely for the later D3D11 composite.
	restoreMaterialTarget(masksBlitRTV.get(), masksComposite.srv11.get());

	globals::d3d::context->OMSetRenderTargets(1, &target, nullptr);
	globals::d3d::context->PSSetShader(compositeSelectiveBlitPS.get(), nullptr, 0);
	ID3D11ShaderResourceView* sources[]{ composite.srv11.get(), packedSurfaceMirror.srv11.get() };
	globals::d3d::context->PSSetShaderResources(0, ARRAYSIZE(sources), sources);
	globals::d3d::context->Draw(3, 0);
	ID3D11ShaderResourceView* nullSources[2]{};
	globals::d3d::context->PSSetShaderResources(0, ARRAYSIZE(nullSources), nullSources);
	state.Restore(globals::d3d::context);
	state.Release();
	// Deterministic developer capture of the exact post-handoff image. This is
	// intentionally opt-in and one-shot: synchronous staging readback is too
	// expensive for normal gameplay, but is more reliable than injected input
	// while validating component views under MO2.
	static bool diagnosticCaptureComplete = false;
	static std::uint32_t diagnosticCaptureDelay = 120;
	if (!diagnosticCaptureComplete && diagnosticCaptureDelay > 0)
		--diagnosticCaptureDelay;
	if (!diagnosticCaptureComplete && diagnosticCaptureDelay == 0) {
		wchar_t bundlePath[32768]{};
		const auto bundlePathLength = GetEnvironmentVariableW(
			L"CS_DX12_DEFERRED_CAPTURE_BUNDLE_PATH", bundlePath,
			static_cast<DWORD>(std::size(bundlePath)));
		if (bundlePathLength > 0 && bundlePathLength < std::size(bundlePath)) {
			diagnosticCaptureComplete = true;
			const std::filesystem::path directory(bundlePath);
			std::error_code error;
			std::filesystem::create_directories(directory, error);
			if (error) {
				logger::error("Could not create deferred diagnostic directory '{}': {}",
					directory.string(), error.message());
				return false;
			}
			auto save = [&](std::wstring_view name, ID3D11Texture2D* texture) {
				if (!texture)
					return false;
				const auto result = Util::SaveTextureToFile(globals::d3d::device,
					globals::d3d::context, directory / name, texture);
				if (FAILED(result))
					logger::error("Deferred diagnostic capture '{}' failed: HRESULT=0x{:08X}",
						std::filesystem::path(name).string(), static_cast<std::uint32_t>(result));
				return SUCCEEDED(result);
			};
			const bool captured =
				save(L"00-final.dds", destination) &&
				save(L"01-compatibility.dds", compatibilityReference.d3d11.get()) &&
				save(L"02-deferred-candidate.dds", composite.d3d11.get()) &&
				save(L"03-packed-surface.dds", packedSurfaceMirror.d3d11.get()) &&
				save(L"04-albedo.dds", inputs[0].source.get()) &&
				save(L"05-specular.dds", inputs[1].source.get()) &&
				save(L"06-reflectance.dds", inputs[2].source.get()) &&
				save(L"07-normal-roughness.dds", inputs[3].source.get()) &&
				save(L"08-masks.dds", inputs[4].source.get()) &&
				save(L"09-linear-depth.dds", linearDepth.d3d11.get());
			if (!captured)
				return false;
			logger::info("[DeferredRendering] Captured diagnostic bundle to {}", directory.string());
		}
		wchar_t capturePath[32768]{};
		const auto capturePathLength = GetEnvironmentVariableW(
			L"CS_DX12_DEFERRED_CAPTURE_PATH", capturePath,
			static_cast<DWORD>(std::size(capturePath)));
		if (!diagnosticCaptureComplete && capturePathLength > 0 && capturePathLength < std::size(capturePath)) {
			diagnosticCaptureComplete = true;
			const auto result = Util::SaveTextureToFile(globals::d3d::device,
				globals::d3d::context, capturePath, destination);
			if (SUCCEEDED(result))
				logger::info("[DeferredRendering] Captured post-composite debug image to {}",
					std::filesystem::path(capturePath).string());
			else
				logger::error("[DeferredRendering] Failed post-composite debug capture: HRESULT=0x{:08X}",
					static_cast<std::uint32_t>(result));
		}
	}
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

bool DX12DeferredShading::RecordNativeSeed(org::PassExecutionContext& context,
	const NativeSeedBindings& bindings) noexcept
{
	if (!seedPipeline || !binningLayout)
		return false;
	const auto width = runtime ? runtime->GetAllocationWidth() : 0u;
	const auto height = runtime ? runtime->GetAllocationHeight() : 0u;
	if (!width || !height)
		return false;
	std::array<std::uint32_t, 17> constants{};
	constants[0] = width;
	constants[1] = height;
	constants[2] = static_cast<std::uint32_t>(
		globals::features::deferredRendering.GetDebugView());
	std::copy(bindings.descriptors.begin(), bindings.descriptors.end(), constants.begin() + 3);
	auto heap = org::runtime::GetActiveSRVDescriptorHeap();
	if (!heap)
		return false;
	context.commandList.SetDescriptorHeaps(heap.GetHandle(), std::nullopt);
	context.commandList.BindLayout(binningLayout->GetHandle());
	context.commandList.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0,
		static_cast<std::uint32_t>(constants.size()), constants.data());
	context.commandList.BindPipeline(seedPipeline->GetHandle());
	context.commandList.Dispatch((width + 7) / 8, (height + 7) / 8, 1);
	return true;
}

bool DX12DeferredShading::RecordNativeBinning(org::PassExecutionContext& context,
	NativeBinningStage stage, const NativeBinningBindings& bindings) noexcept
{
	const auto pipelineIndex = static_cast<std::size_t>(stage);
	if (!binningLayout || pipelineIndex >= binningPipelines.size() || !binningPipelines[pipelineIndex])
		return false;
	const auto snapshot = nativeFrameSnapshot;
	if (!snapshot) return false;
	const std::uint32_t constants[]{ snapshot->renderWidth, snapshot->renderHeight,
		CS::Deferred::kEvaluatorCount, 7u * sizeof(std::uint32_t), GetEnabledEvaluatorMask(),
		globals::features::deferredRendering.IsMaterialClassificationEnabled() ? 1u : 0u,
		std::bit_cast<std::uint32_t>(snapshot->farPlane),
		bindings.packedSurface, bindings.linearDepth, bindings.counts, bindings.offsets,
		bindings.cursors, bindings.pixels, bindings.indirectArguments, bindings.marker };
	auto heap = org::runtime::GetActiveSRVDescriptorHeap();
	if (!heap) return false;
	context.commandList.SetDescriptorHeaps(heap.GetHandle(), std::nullopt);
	context.commandList.BindLayout(binningLayout->GetHandle());
	context.commandList.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0,
		static_cast<std::uint32_t>(std::size(constants)), constants);
	context.commandList.BindPipeline(binningPipelines[pipelineIndex]->GetHandle());
	switch (stage) {
	case NativeBinningStage::Clear: context.commandList.Dispatch(11u, 1, 1); break;
	case NativeBinningStage::Histogram:
	case NativeBinningStage::Scatter:
		context.commandList.Dispatch((snapshot->renderWidth + 7) / 8, (snapshot->renderHeight + 7) / 8, 1); break;
	case NativeBinningStage::Prefix: context.commandList.Dispatch(1, 1, 1); break;
	}
	return true;
}

bool DX12DeferredShading::RecordNativeEvaluator(org::PassExecutionContext& context,
	std::size_t evaluator, const NativeEvaluatorBindings& bindings) noexcept
{
	if (evaluator >= evaluatorPipelines.size() || !evaluatorPipelines[evaluator] ||
		!bindings.indirectArguments.valid() || !evaluatorLayout || !evaluatorCommandSignature)
		return false;
	auto heap = org::runtime::GetActiveSRVDescriptorHeap();
	if (!heap) return false;
	// Constants 0..3 are supplied by ExecuteIndirect. The directly-bound tail
	// starts at DWORD 4. Pass frame-local control values as immediate constants;
	// they must not depend on the scheduled-upload path used for bulk frame data.
	// Descriptors start at DWORD 8 after HLSL's uint4 alignment padding.
	std::array<std::uint32_t, 32> constants{};
	constants[0] = bindings.frameConstants;
	constants[1] = static_cast<std::uint32_t>(
		globals::features::deferredRendering.GetDebugView());
	std::copy(bindings.descriptors.begin(), bindings.descriptors.end(), constants.begin() + 4);
	context.commandList.SetDescriptorHeaps(heap.GetHandle(), std::nullopt);
	context.commandList.BindLayout(evaluatorLayout->GetHandle());
	context.commandList.PushConstants(rhi::ShaderStage::Compute, 0, 1, 4,
		static_cast<std::uint32_t>(constants.size()), constants.data());
	context.commandList.BindPipeline(evaluatorPipelines[evaluator]->GetHandle());
	context.commandList.ExecuteIndirect(evaluatorCommandSignature->GetHandle(),
		bindings.indirectArguments, static_cast<std::uint64_t>(evaluator) * 7u * sizeof(std::uint32_t),
		{}, 0, 1);
	if (dispatchCount++ == 0)
		logger::info("[DX12DeferredShading] Native ORG evaluator passes use BasicRHI commands and graph-owned descriptors");
	return true;
}

void DX12DeferredShading::UpdateNativeFrame(const std::shared_ptr<org::Resource>& constantsResource)
{
	nativeFrameSnapshot = globals::features::deferredRendering.GetFrameSnapshot();
	const auto& snapshot = nativeFrameSnapshot;
	if (!snapshot || !constantsResource) return;
	DeferredConstants constants{};
	constants.viewInverse = snapshot->cameraViewInverse;
	constants.viewProjectionInverse = snapshot->viewProjectionInverse;
	std::copy_n(&snapshot->cameraData.x, 4, constants.cameraData);
	constants.dynamicResolutionScale[0] = snapshot->dynamicResolutionParams2.x;
	constants.dynamicResolutionScale[1] = snapshot->dynamicResolutionParams2.y;
	constants.clusterGrid[0] = (snapshot->renderWidth + 63) / 64;
	constants.clusterGrid[1] = (snapshot->renderHeight + 63) / 64;
	constants.clusterGrid[2] = 32;
	constants.lightCount = static_cast<uint32_t>(snapshot->lights.size());
	constants.screenSize[0] = static_cast<float>(snapshot->renderWidth);
	constants.screenSize[1] = static_cast<float>(snapshot->renderHeight);
	constants.nearPlane = snapshot->nearPlane;
	constants.farPlane = snapshot->farPlane;
	constants.contextCount = static_cast<uint32_t>(snapshot->contexts.size());
	constants.pageCapacity = constants.clusterGrid[0] * constants.clusterGrid[1] * constants.clusterGrid[2] * 10;
	constants.pbrMaterialCount = static_cast<uint32_t>(snapshot->pbrMaterials.size());
	constants.abiVersion = CS::Deferred::kAbiVersion;
	constants.lightingTransform = snapshot->lightingTransform;
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
	constants.skylightingEnabled = snapshot->skylightingEnabled && skylightingInputsAvailable &&
		std::getenv("CS_DX12_DIAGNOSTIC_NO_SKYLIGHTING") == nullptr ? 1u : 0u;
	std::copy_n(snapshot->skylighting.ArrayOrigin, 3, constants.skylightingArrayOrigin);
	constants.skylightingMinDiffuseVisibility = snapshot->skylighting.MinDiffuseVisibility;
	constants.skylightingMinSpecularVisibility = snapshot->skylighting.MinSpecularVisibility;
	constants.debugView = static_cast<std::uint32_t>(
		globals::features::deferredRendering.GetDebugView());
	constants.frameFlags = (std::getenv("CS_DX12_DEFERRED_DISABLE_STATIC_OPAQUE") == nullptr ? 1u : 0u) |
		(std::getenv("CS_DX12_DEFERRED_DIRECT_DIAGNOSTIC") ? 2u : 0u) |
		(globals::features::deferredRendering.IsCoverageVisualizationEnabled() ? 4u : 0u) |
		(std::getenv("CS_DX12_DEFERRED_PARITY") ? 8u : 0u) |
		(std::getenv("CS_DX12_DIAGNOSTIC_NO_SHADOWS") ? 16u : 0u) |
		(std::getenv("CS_DX12_DIAGNOSTIC_ALBEDO") ? 32u : 0u) |
		(std::getenv("CS_DX12_DIAGNOSTIC_COMPATIBILITY") ? 64u : 0u) |
		(screenSpaceShadow ? 128u : 0u);
	BUFFER_UPLOAD(&constants, sizeof(constants),
		org::runtime::UploadTarget::FromShared(constantsResource), 0);
}
void DX12DeferredShading::Shutdown() noexcept
{
	if (nativeRegistration) {
		if (runtime && runtime->GetContributorRuntime())
			runtime->GetContributorRuntime()->BeginUnregisterExtension(nativeRegistration);
		nativeRegistration = 0;
	}
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
	nativeFrameSnapshot.reset();
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
	for (auto& pipeline : evaluatorPipelines) pipeline.Reset();
	evaluatorLayout.Reset();
	seedPipeline.Reset();
	for (auto& pipeline : binningPipelines) pipeline.Reset();
	binningLayout.Reset();
	evaluatorCommandSignature.Reset();
	inputs = {};
	parityCounterReadback = nullptr;
	parityCounterPending = false;
	runtime = nullptr;
}
