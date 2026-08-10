#include "DeferredShadingExtension.h"

#include "DeferredShading.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <Render/PassBuilders.h>
#include <RenderPasses/Base/ComputePass.h>
#include <Render/Runtime/DescriptorServiceAccess.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/ExternalTextureResource.h>
#include <rhi_helpers.h>

namespace {
using ID = org::ResourceIdentifier;
constexpr org::ExternalTimelineBinding kD3D11Ready = 1;

const ID kPacked{ "community-shaders.deferred-shading.gbuffer.masks2-mirror" };
const ID kDepth{ "community-shaders.deferred-shading.linear-depth" };
const ID kCounts{ "community-shaders.deferred-shading.evaluator-counts" };
const ID kOffsets{ "community-shaders.deferred-shading.evaluator-offsets" };
const ID kCursors{ "community-shaders.deferred-shading.evaluator-cursors" };
const ID kPixels{ "community-shaders.deferred-shading.evaluator-pixels" };
const ID kArgs{ "community-shaders.deferred-shading.evaluator-indirect-args" };
const ID kMarker{ "community-shaders.deferred-shading.frame-marker" };
const ID kFrameConstants{ "community-shaders.deferred-shading.frame-constants" };
const ID kGBufferInputs[]{
	{ "community-shaders.deferred-shading.gbuffer.albedo" },
	{ "community-shaders.deferred-shading.gbuffer.specular" },
	{ "community-shaders.deferred-shading.gbuffer.reflectance" },
	{ "community-shaders.deferred-shading.gbuffer.normal-roughness" },
	{ "community-shaders.deferred-shading.gbuffer.masks" }
};

const std::vector<ID> kAllResources{
	{ "community-shaders.deferred-shading.gbuffer.albedo" }, { "community-shaders.deferred-shading.gbuffer.specular" },
	{ "community-shaders.deferred-shading.gbuffer.reflectance" }, { "community-shaders.deferred-shading.gbuffer.normal-roughness" },
	{ "community-shaders.deferred-shading.gbuffer.masks" }, kPacked, kDepth,
	{ "community-shaders.deferred-shading.local-shadow-mask" }, { "community-shaders.deferred-shading.screen-space-shadow" },
	{ "community-shaders.deferred-shading.compatibility-reference" },
	{ "community-shaders.deferred-shading.ibl.environment-sh" }, { "community-shaders.deferred-shading.ibl.sky-sh" },
	{ "community-shaders.deferred-shading.skylighting.probes" }, { "community-shaders.deferred-shading.skylighting.shadow-visibility" },
	{ "community-shaders.deferred-shading.true-pbr.glint-noise" },
	{ "community-shaders.clustered-lighting.lights" }, { "community-shaders.clustered-lighting.contexts" },
	{ "community-shaders.clustered-lighting.pbr-materials" }, { "community-shaders.clustered-lighting.clusters" },
	{ "community-shaders.clustered-lighting.pages" },
	{ "community-shaders.deferred-shading.composite" }, { "community-shaders.deferred-shading.specular-composite" },
	{ "community-shaders.deferred-shading.reflectance-composite" }, { "community-shaders.deferred-shading.albedo-composite" },
	{ "community-shaders.deferred-shading.normal-composite" }, { "community-shaders.deferred-shading.masks-composite" },
	kCounts, kOffsets, kCursors, kPixels, kArgs, kMarker, kFrameConstants
};

enum class Usage { SRV, UAV, CBV, Indirect };
struct Access { ID id; Usage usage; };

class DeferredPass final : public org::ComputePass
{
public:
	enum class Operation { BinClear, BinHistogram, BinPrefix, BinScatter, Evaluator, Seed };
	DeferredPass(DeferredShadingExtension& extension, DX12DeferredShading& owner,
		Operation operation, std::size_t evaluator, std::vector<Access> accesses, bool waitsForD3D11)
		: extension_(extension), owner_(owner), operation_(operation), evaluator_(evaluator),
		  accesses_(std::move(accesses)), waits_(waitsForD3D11) {}
	void Setup() override {
		for (const auto& access : accesses_) {
			if (access.usage == Usage::SRV) RegisterSRV(access.id);
			else if (access.usage == Usage::UAV) RegisterUAV(access.id);
			else if (access.usage == Usage::CBV) RegisterCBV(access.id);
		}
	}
	void Update(const org::UpdateExecutionContext&) override {
		if (operation_ == Operation::Seed)
			owner_.UpdateNativeFrame(extension_.Resource(kFrameConstants));
	}
	void Cleanup() override {}
	void DeclareResourceUsages(org::ComputePassBuilder* builder) override {
		for (const auto& access : accesses_) {
			if (!extension_.Resource(access.id))
				throw std::runtime_error("Deferred native pass is missing resource " + access.id.name);
			// Declare by symbolic ID so ORG includes the identifier in this pass's
			// registry view. Descriptor registration during Setup intentionally uses
			// those same IDs and must not depend on a pointer-only declaration.
			switch (access.usage) {
			case Usage::SRV: builder->WithShaderResource(access.id); break;
			case Usage::UAV: builder->WithUnorderedAccess(access.id); break;
			case Usage::CBV: builder->WithConstantBuffer(access.id); break;
			case Usage::Indirect: builder->WithIndirectArguments(access.id); break;
			}
		}
		if (waits_) builder->WithExternalWaitBindingBeforeTransitions(kD3D11Ready);
	}
	org::PassReturn Execute(org::PassExecutionContext& context) override {
		bool succeeded{};
		if (operation_ == Operation::Seed) {
			DX12DeferredShading::NativeSeedBindings bindings{};
			const ID descriptorResources[]{
				{ "community-shaders.deferred-shading.compatibility-reference" },
				{ "community-shaders.deferred-shading.gbuffer.specular" },
				{ "community-shaders.deferred-shading.gbuffer.reflectance" },
				{ "community-shaders.deferred-shading.gbuffer.albedo" },
				{ "community-shaders.deferred-shading.gbuffer.normal-roughness" },
				{ "community-shaders.deferred-shading.gbuffer.masks" },
				kPacked, kDepth,
				{ "community-shaders.deferred-shading.composite" },
				{ "community-shaders.deferred-shading.specular-composite" },
				{ "community-shaders.deferred-shading.reflectance-composite" },
				{ "community-shaders.deferred-shading.albedo-composite" },
				{ "community-shaders.deferred-shading.normal-composite" },
				{ "community-shaders.deferred-shading.masks-composite" }
			};
			for (std::size_t index = 0; index < std::size(descriptorResources); ++index)
				bindings.descriptors[index] = Index(descriptorResources[index]);
			succeeded = owner_.RecordNativeSeed(context, bindings);
		} else if (operation_ != Operation::Evaluator) {
			DX12DeferredShading::NativeBinningBindings bindings{};
			bindings.packedSurface = Index(kPacked); bindings.linearDepth = Index(kDepth);
			bindings.counts = Index(kCounts); bindings.offsets = Index(kOffsets);
			bindings.cursors = Index(kCursors); bindings.pixels = Index(kPixels);
			bindings.indirectArguments = Index(kArgs); bindings.marker = Index(kMarker);
			const auto stage = static_cast<DX12DeferredShading::NativeBinningStage>(
				static_cast<unsigned>(operation_));
			succeeded = owner_.RecordNativeBinning(context, stage, bindings);
		} else {
			DX12DeferredShading::NativeEvaluatorBindings bindings{};
			const ID descriptorResources[]{
				{ "community-shaders.deferred-shading.gbuffer.specular" },
				{ "community-shaders.deferred-shading.gbuffer.albedo" }, kDepth,
				{ "community-shaders.clustered-lighting.lights" }, { "community-shaders.clustered-lighting.contexts" },
				{ "community-shaders.clustered-lighting.clusters" }, { "community-shaders.clustered-lighting.pages" }, kPacked,
				{ "community-shaders.deferred-shading.local-shadow-mask" },
				{ "community-shaders.deferred-shading.gbuffer.normal-roughness" },
				{ "community-shaders.deferred-shading.compatibility-reference" },
				{ "community-shaders.deferred-shading.gbuffer.masks" },
				{ "community-shaders.deferred-shading.screen-space-shadow" }, kPixels,
				{ "community-shaders.deferred-shading.ibl.environment-sh" }, { "community-shaders.deferred-shading.ibl.sky-sh" },
				{ "community-shaders.deferred-shading.skylighting.probes" },
				{ "community-shaders.deferred-shading.skylighting.shadow-visibility" },
				{ "community-shaders.deferred-shading.gbuffer.reflectance" },
				{ "community-shaders.clustered-lighting.pbr-materials" },
				{ "community-shaders.deferred-shading.true-pbr.glint-noise" },
				{ "community-shaders.deferred-shading.composite" }, kMarker,
				{ "community-shaders.deferred-shading.specular-composite" },
				{ "community-shaders.deferred-shading.reflectance-composite" },
				{ "community-shaders.deferred-shading.albedo-composite" },
				{ "community-shaders.deferred-shading.normal-composite" },
				{ "community-shaders.deferred-shading.masks-composite" }
			};
			for (std::size_t index = 0; index < std::size(descriptorResources); ++index)
				bindings.descriptors[index] = Index(descriptorResources[index]);
			bindings.frameConstants = Index(kFrameConstants);
			auto arguments = extension_.Resource(kArgs);
			bindings.indirectArguments = arguments ? arguments->GetAPIResource().GetHandle() : rhi::ResourceHandle{};
			succeeded = owner_.RecordNativeEvaluator(context, evaluator_, bindings);
		}
		if (!succeeded) throw std::runtime_error("Deferred native pass recording failed");
		return {};
	}
private:
	std::uint32_t Index(const ID& id) const {
		return m_resourceDescriptorIndexHelper->GetResourceDescriptorIndex(id, true);
	}
	DeferredShadingExtension& extension_; DX12DeferredShading& owner_;
	Operation operation_{}; std::size_t evaluator_{}; std::vector<Access> accesses_; bool waits_{};
};

std::vector<Access> EvaluatorAccesses() {
	std::vector<Access> result;
	for (const auto& id : kAllResources) {
		const bool output = id.name == "community-shaders.deferred-shading.composite" ||
			id.name == "community-shaders.deferred-shading.specular-composite" ||
			id.name == "community-shaders.deferred-shading.reflectance-composite" ||
			id.name == "community-shaders.deferred-shading.albedo-composite" ||
			id.name == "community-shaders.deferred-shading.normal-composite" ||
			id.name == "community-shaders.deferred-shading.masks-composite" || id == kMarker;
		if (id == kCounts || id == kOffsets || id == kCursors) continue;
		if (id == kFrameConstants) { result.push_back({ id, Usage::CBV }); continue; }
		result.push_back({ id, output ? Usage::UAV : (id == kArgs ? Usage::Indirect : Usage::SRV) });
	}
	return result;
}
}

DeferredShadingExtension::DeferredShadingExtension(DX12DeferredShading& owner) : owner_(owner)
{
	const auto width = owner_.runtime->GetAllocationWidth();
	const auto height = owner_.runtime->GetAllocationHeight();
	if (!owner_.composite || !owner_.specularComposite || !owner_.reflectanceComposite ||
		!owner_.albedoComposite || !owner_.normalComposite || !owner_.masksComposite ||
		!owner_.EnsureGBufferInputs() || !owner_.EnsureLinearDepth(width, height) ||
		!owner_.EnsureFrameMarker() || !owner_.packedSurfaceMirror || !owner_.localShadowMask ||
		!owner_.compatibilityReference || !owner_.envIBLInput.mirror || !owner_.skyIBLInput.mirror ||
		!owner_.skylightingProbeInput.mirror || !owner_.skylightingVisibilityInput.mirror ||
		!owner_.glintNoiseInput.mirror)
		throw std::runtime_error("Deferred native resources are not ready");

	auto importTexture = [&](const ID& id, const org::interop::D3D11Interop::SharedTexture& source,
		bool srv, bool uav) {
		rhi::ResourcePtr imported;
		if (!owner_.runtime->GetInteropCoordinator()->ImportGraphResource(source, imported))
			throw std::runtime_error("Deferred import has no RHI backing: " + id.name);
		org::TextureDescription description{};
		const auto mipCount = (std::max)(1u, source.description.MipLevels);
		description.imageDimensions.resize(mipCount);
		auto mipWidth = source.description.Width;
		auto mipHeight = source.description.Height;
		for (auto& dimensions : description.imageDimensions) {
			dimensions.width = mipWidth; dimensions.height = mipHeight;
			mipWidth = (std::max)(1u, mipWidth >> 1); mipHeight = (std::max)(1u, mipHeight >> 1);
		}
		description.channels = 4;
		description.format = rhi::helpers::ToRHI(source.description.Format);
		description.arraySize = (std::max)(1u, source.description.ArraySize);
		description.isArray = description.arraySize > 1;
		description.hasSRV = srv; description.srvFormat = description.format;
		description.hasUAV = uav; description.uavFormat = description.format;
		description.initialLayout = rhi::ResourceLayout::Common;
		auto resource = org::ExternalTextureResource::CreateShared(std::move(imported), description, true);
		resource->SetName(id.name);
		resources_.emplace(id, std::move(resource));
	};

	for (std::size_t index = 0; index < std::size(kGBufferInputs); ++index)
		importTexture(kGBufferInputs[index], owner_.inputs[index].mirror, true, false);
	importTexture(kPacked, owner_.packedSurfaceMirror, true, false);
	importTexture(kDepth, owner_.linearDepth, true, false);
	importTexture(ID{ "community-shaders.deferred-shading.local-shadow-mask" }, owner_.localShadowMask, true, false);
	const auto& screenShadow = owner_.screenSpaceShadow ? owner_.screenSpaceShadow : owner_.localShadowMask;
	importTexture(ID{ "community-shaders.deferred-shading.screen-space-shadow" }, screenShadow, true, false);
	importTexture(ID{ "community-shaders.deferred-shading.compatibility-reference" }, owner_.compatibilityReference, true, false);
	importTexture(ID{ "community-shaders.deferred-shading.ibl.environment-sh" }, owner_.envIBLInput.mirror, true, false);
	importTexture(ID{ "community-shaders.deferred-shading.ibl.sky-sh" }, owner_.skyIBLInput.mirror, true, false);
	importTexture(ID{ "community-shaders.deferred-shading.skylighting.probes" }, owner_.skylightingProbeInput.mirror, true, false);
	importTexture(ID{ "community-shaders.deferred-shading.skylighting.shadow-visibility" }, owner_.skylightingVisibilityInput.mirror, true, false);
	importTexture(ID{ "community-shaders.deferred-shading.true-pbr.glint-noise" }, owner_.glintNoiseInput.mirror, true, false);
	importTexture(ID{ "community-shaders.deferred-shading.composite" }, owner_.composite, true, true);
	importTexture(ID{ "community-shaders.deferred-shading.specular-composite" }, owner_.specularComposite, true, true);
	importTexture(ID{ "community-shaders.deferred-shading.reflectance-composite" }, owner_.reflectanceComposite, true, true);
	importTexture(ID{ "community-shaders.deferred-shading.albedo-composite" }, owner_.albedoComposite, true, true);
	importTexture(ID{ "community-shaders.deferred-shading.normal-composite" }, owner_.normalComposite, true, true);
	importTexture(ID{ "community-shaders.deferred-shading.masks-composite" }, owner_.masksComposite, true, true);
	importTexture(kMarker, owner_.frameMarker, true, true);

	auto structured = [&](const ID& id, std::uint32_t elements, std::uint32_t stride) {
		auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(elements, stride, true);
		buffer->SetName(id.name); resources_.emplace(id, std::move(buffer));
	};
	structured(kCounts, CS::Deferred::kEvaluatorCount, sizeof(std::uint32_t));
	structured(kOffsets, CS::Deferred::kEvaluatorCount, sizeof(std::uint32_t));
	structured(kCursors, CS::Deferred::kEvaluatorCount, sizeof(std::uint32_t));
	structured(kPixels, width * height, sizeof(std::uint32_t) * 2u);
	constexpr std::uint32_t indirectStride = 7u * sizeof(std::uint32_t);
	auto arguments = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal,
		CS::Deferred::kEvaluatorCount * indirectStride, true);
	org::BufferBase::DescriptorRequirements argumentViews{};
	argumentViews.createUAV = true;
	argumentViews.uavDesc = { .dimension = rhi::UavDim::Buffer, .formatOverride = rhi::Format::R32_Typeless,
		.buffer = { .kind = rhi::BufferViewKind::Raw, .firstElement = 0,
			.numElements = CS::Deferred::kEvaluatorCount * indirectStride / 4u } };
	arguments->SetDescriptorRequirements(argumentViews);
	arguments->SetName(kArgs.name); resources_.emplace(kArgs, std::move(arguments));

	auto constants = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, 512, false);
	constants->SetName(kFrameConstants.name);
	org::BufferBase::DescriptorRequirements requirements{};
	requirements.createCBV = true;
	requirements.cbvDesc = { .byteOffset = 0, .byteSize = 512 };
	constants->SetDescriptorRequirements(requirements);
	resources_.emplace(kFrameConstants, std::move(constants));
}

void DeferredShadingExtension::PrepareForBuild(org::RenderGraph& graph)
{
	graph.RegisterProvider(this);
	for (const auto& id : kAllResources) resources_[id] = graph.RequestResourcePtr(id);
}

std::shared_ptr<org::Resource> DeferredShadingExtension::Resource(const org::ResourceIdentifier& id) const
{
	if (const auto found = resources_.find(id); found != resources_.end()) return found->second;
	return {};
}

std::shared_ptr<org::Resource> DeferredShadingExtension::ProvideResource(const org::ResourceIdentifier& id)
{
	return Resource(id);
}

std::vector<org::ResourceIdentifier> DeferredShadingExtension::GetSupportedKeys()
{
	std::vector<org::ResourceIdentifier> result;
	result.reserve(resources_.size());
	for (const auto& [id, resource] : resources_) if (resource) result.push_back(id);
	return result;
}

void DeferredShadingExtension::GatherStructuralPasses(org::RenderGraph&,
	std::vector<org::RenderGraph::ExternalPassDesc>& out)
{
	auto add = [&](const char* id, const char* after, DeferredPass::Operation operation,
		std::size_t evaluator, std::vector<Access> accesses, bool waits) {
		auto point = org::RenderGraph::ExternalInsertPoint::After(after);
		point.keepExtensionOrder = false; point.AlsoBefore("cs.deferred-lighting.begin");
		out.push_back(org::RenderGraph::ExternalPassDesc::Compute(id,
			std::make_shared<DeferredPass>(*this, owner_, operation, evaluator, std::move(accesses), waits))
			.At(std::move(point)).PreferQueue(org::QueueKind::Compute));
	};
	add("community-shaders.deferred-shading.seed-outputs", "community-shaders.clustered-lighting.cull-lights",
		DeferredPass::Operation::Seed, 0, {
			{ ID{ "community-shaders.deferred-shading.compatibility-reference" }, Usage::SRV },
			{ ID{ "community-shaders.deferred-shading.gbuffer.specular" }, Usage::SRV },
			{ ID{ "community-shaders.deferred-shading.gbuffer.reflectance" }, Usage::SRV },
			{ ID{ "community-shaders.deferred-shading.gbuffer.albedo" }, Usage::SRV },
			{ ID{ "community-shaders.deferred-shading.gbuffer.normal-roughness" }, Usage::SRV },
			{ ID{ "community-shaders.deferred-shading.gbuffer.masks" }, Usage::SRV },
			{ kPacked, Usage::SRV }, { kDepth, Usage::SRV },
			{ ID{ "community-shaders.deferred-shading.composite" }, Usage::UAV },
			{ ID{ "community-shaders.deferred-shading.specular-composite" }, Usage::UAV },
			{ ID{ "community-shaders.deferred-shading.reflectance-composite" }, Usage::UAV },
			{ ID{ "community-shaders.deferred-shading.albedo-composite" }, Usage::UAV },
			{ ID{ "community-shaders.deferred-shading.normal-composite" }, Usage::UAV },
			{ ID{ "community-shaders.deferred-shading.masks-composite" }, Usage::UAV }
		}, true);
	add("community-shaders.deferred-shading.bin-clear", "community-shaders.deferred-shading.seed-outputs",
		DeferredPass::Operation::BinClear, 0, { { kCounts, Usage::UAV }, { kOffsets, Usage::UAV },
			{ kCursors, Usage::UAV }, { kArgs, Usage::UAV }, { kMarker, Usage::UAV } }, false);
	add("community-shaders.deferred-shading.bin-histogram", "community-shaders.deferred-shading.bin-clear",
		DeferredPass::Operation::BinHistogram, 0, { { kPacked, Usage::SRV }, { kDepth, Usage::SRV },
			{ kCounts, Usage::UAV }, { kMarker, Usage::UAV } }, false);
	add("community-shaders.deferred-shading.bin-prefix", "community-shaders.deferred-shading.bin-histogram",
		DeferredPass::Operation::BinPrefix, 0, { { kCounts, Usage::UAV }, { kOffsets, Usage::UAV }, { kArgs, Usage::UAV } }, false);
	add("community-shaders.deferred-shading.bin-scatter", "community-shaders.deferred-shading.bin-prefix",
		DeferredPass::Operation::BinScatter, 0, { { kPacked, Usage::SRV }, { kOffsets, Usage::UAV },
			{ kCursors, Usage::UAV }, { kPixels, Usage::UAV } }, false);

	struct Evaluator { size_t index; const char* id; };
	constexpr Evaluator evaluators[]{ { 1, "generic" }, { 2, "true-pbr" }, { 3, "grass" },
		{ 4, "distant-tree" }, { 8, "foliage-special" }, { 9, "true-pbr-subsurface-fuzz" },
		{ 10, "true-pbr-coat" }, { 11, "true-pbr-glint" }, { 13, "true-pbr-terrain" } };
	std::string previous = "community-shaders.deferred-shading.bin-scatter";
	const auto mask = owner_.GetEnabledEvaluatorMask();
	for (const auto& evaluator : evaluators) {
		if ((mask & (1u << evaluator.index)) == 0) continue;
		const auto id = "community-shaders.deferred-shading.evaluate." + std::string(evaluator.id);
		add(id.c_str(), previous.c_str(), DeferredPass::Operation::Evaluator,
			evaluator.index, EvaluatorAccesses(), false);
		previous = id;
	}
}
