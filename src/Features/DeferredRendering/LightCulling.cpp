#include "LightCulling.h"
#include "ClusteredLightingExtension.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/NativeRenderGraphRegistry.h"
#include "Features/DeferredRendering.h"
#include "Globals.h"

#include <fstream>

namespace
{
	struct Constants
	{
		Matrix projectionInverse;
		Matrix view;
		uint32_t grid[4];
		float screen[2];
		float nearPlane;
		float farPlane;
		uint32_t lightCount;
		uint32_t contextCount;
		uint32_t pageCapacity;
		uint32_t materialCount;
		uint32_t lightsIndex;
		uint32_t contextsIndex;
		uint32_t materialsIndex;
		uint32_t clustersIndex;
		uint32_t pagesIndex;
		uint32_t pageCounterIndex;
		uint32_t diagnosticsIndex;
	};

}

DX12LightCulling& DX12LightCulling::Get() { static DX12LightCulling value; return value; }

bool DX12LightCulling::Initialize(RenderGraphRuntime& owner) noexcept
{
	runtime = &owner;
	if (!owner.GetRHIDevice() || !CreatePipeline()) return false;
	try {
		NativeRenderGraphRegistry::Descriptor desc{};
		desc.id = "community-shaders.clustered-lighting";
		desc.kind = NativeRenderGraphRegistry::Kind::Required;
		desc.exportedResources = { "community-shaders.clustered-lighting.lights",
			"community-shaders.clustered-lighting.contexts", "community-shaders.clustered-lighting.pbr-materials",
			"community-shaders.clustered-lighting.clusters", "community-shaders.clustered-lighting.pages",
			"community-shaders.clustered-lighting.page-counter", "community-shaders.clustered-lighting.diagnostics" };
		desc.factory = [this] { return std::make_unique<ClusteredLightingExtension>(*this); };
		desc.diagnostic = [](std::string_view message) { logger::error("[DX12LightCulling] Native graph contributor: {}", message); };
		registration = NativeRenderGraphRegistry::Get().Register(std::move(desc));
		owner.RequestGraphRebuild();
		return true;
	} catch (const std::exception& error) {
		logger::error("[DX12LightCulling] Native registration failed: {}", error.what());
		return false;
	}
}

bool DX12LightCulling::CreatePipeline() noexcept
{
	auto device = runtime ? runtime->GetRHIDevice() : rhi::Device{};
	rhi::PushConstantRangeDesc constants[]{
		{ rhi::ShaderStage::Compute, sizeof(Constants) / 4, 0, 0, rhi::PushConstantRangeType::RootConstants32 }
	};
	if (!device || !rhi::IsOk(device.CreatePipelineLayout({ {}, constants, {}, rhi::PF_None }, layout)))
		return false;
	auto loadBinary = [](const wchar_t* name) {
		const std::filesystem::path paths[]{ std::filesystem::path(L"Data\\Shaders\\DeferredRendering") / name,
			std::filesystem::current_path() / L"build/ALL/DeferredNativeShaders" / name };
		for (const auto& path : paths) {
			std::ifstream stream(path, std::ios::binary | std::ios::ate);
			if (!stream) continue;
			const auto length = stream.tellg(); if (length <= 0) continue;
			std::vector<std::byte> bytes(static_cast<size_t>(length)); stream.seekg(0);
			if (stream.read(reinterpret_cast<char*>(bytes.data()), length)) return bytes;
		}
		return std::vector<std::byte>{};
	};
	auto create = [&](const wchar_t* file, rhi::PipelinePtr& output) {
		auto bytes = loadBinary(file); if (bytes.empty()) return false;
		const rhi::SubobjLayout pipelineLayout{ layout->GetHandle() };
		const rhi::SubobjShader shader{ rhi::ShaderStage::Compute,
			rhi::ShaderBinary{ bytes.data(), static_cast<std::uint32_t>(bytes.size()) }, "main" };
		const rhi::PipelineStreamItem stream[]{ rhi::Make(pipelineLayout), rhi::Make(shader) };
		return rhi::IsOk(device.CreatePipeline(stream, static_cast<std::uint32_t>(std::size(stream)), output)) && output;
	};
	if (create(L"ClusteredLighting.ClearCounters.dxil", clearPipeline) &&
		create(L"ClusteredLighting.BuildClusters.dxil", clusterPipeline) &&
		create(L"ClusteredLighting.CullLights.dxil", cullPipeline)) return true;
	clearPipeline.Reset(); clusterPipeline.Reset(); cullPipeline.Reset();
	logger::error("[DX12LightCulling] Required precompiled native shaders are missing");
	return false;
}

void DX12LightCulling::Shutdown() noexcept
{
	if (registration) { NativeRenderGraphRegistry::Get().BeginUnregister(registration); registration = 0; }
	clearPipeline.Reset(); cullPipeline.Reset(); clusterPipeline.Reset(); layout.Reset();
}
