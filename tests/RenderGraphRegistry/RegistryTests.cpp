#include "RenderGraphRegistry.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (false)

namespace
{
	RenderGraphRegistry* registry{};

	CSRGStatus CS_RG_CALL Execute(void*, const CSRGExecutionContext*) { return CS_RG_OK; }

	CSRGStatus AddBuffer(CSRGBuildHandle build, const char* id)
	{
		CSRGResourceDesc desc{};
		desc.structSize = sizeof(desc); desc.apiVersion = CS_RENDER_GRAPH_API_CURRENT;
		desc.id = id; desc.lifetime = CS_RG_RESOURCE_TRANSIENT; desc.dimension = CS_RG_RESOURCE_BUFFER;
		desc.heapClass = CS_RG_HEAP_DEVICE_LOCAL; desc.sizing = CS_RG_SIZE_ABSOLUTE;
		desc.format = CS_RG_FORMAT_UNKNOWN; desc.byteSize = 1024; desc.mipLevels = 1; desc.sampleCount = 1;
		desc.allowedUsages = CS_RG_USAGE_UNORDERED_ACCESS | CS_RG_USAGE_SHADER_RESOURCE;
		return registry->DeclareResource(build, &desc);
	}

	CSRGStatus AddPass(CSRGBuildHandle build, const char* id, const char* resource,
		CSRGAccessKind kind, uint32_t binding, const char* after = nullptr)
	{
		CSRGResourceAccessDesc access{};
		access.structSize = sizeof(access); access.apiVersion = CS_RENDER_GRAPH_API_CURRENT;
		access.resourceId = resource; access.binding = binding; access.access = kind;
		access.range = { 0, UINT32_MAX, 0, UINT32_MAX };
		access.viewFlags = CS_RG_VIEW_FLAG_RAW_BUFFER;
		access.elementCount = UINT32_MAX;
		const char* dependencies[]{ after };
		CSRGPassDesc pass{};
		pass.structSize = sizeof(pass); pass.apiVersion = CS_RENDER_GRAPH_API_CURRENT;
		pass.id = id; pass.kind = CS_RG_PASS_COMPUTE; pass.queue = CS_RG_QUEUE_AUTOMATIC;
		pass.after = after ? dependencies : nullptr; pass.afterCount = after ? 1u : 0u;
		pass.accesses = &access; pass.accessCount = 1; pass.execute = &Execute;
		return registry->DeclarePass(build, &pass);
	}

	CSRGStatus CS_RG_CALL Producer(void*, CSRGBuildHandle build)
	{
		auto status = AddBuffer(build, "test.producer.buffer");
		return status == CS_RG_OK ? AddPass(build, "test.producer.write", "test.producer.buffer",
			CS_RG_ACCESS_UNORDERED_ACCESS, 3) : status;
	}

	CSRGStatus CS_RG_CALL Consumer(void*, CSRGBuildHandle build)
	{
		return AddPass(build, "test.consumer.read", "test.producer.buffer",
			CS_RG_ACCESS_SHADER_RESOURCE, 99, "test.producer.write");
	}

	CSRGStatus CS_RG_CALL OptionalMissing(void*, CSRGBuildHandle build)
	{
		return AddPass(build, "test.optional.read", "test.missing.resource", CS_RG_ACCESS_SHADER_RESOURCE, 1);
	}

	CSRGStatus CS_RG_CALL DuplicateBindings(void*, CSRGBuildHandle build)
	{
		auto status = AddBuffer(build, "test.duplicate.buffer");
		if (status != CS_RG_OK) return status;
		CSRGResourceAccessDesc accesses[2]{};
		for (auto& access : accesses) {
			access.structSize = sizeof(access); access.apiVersion = CS_RENDER_GRAPH_API_CURRENT;
			access.resourceId = "test.duplicate.buffer"; access.binding = 4;
			access.access = CS_RG_ACCESS_SHADER_RESOURCE; access.range = { 0, UINT32_MAX, 0, UINT32_MAX };
			access.viewFlags = CS_RG_VIEW_FLAG_RAW_BUFFER;
			access.elementCount = UINT32_MAX;
		}
		CSRGPassDesc pass{ sizeof(pass), CS_RENDER_GRAPH_API_CURRENT };
		pass.id = "test.duplicate.pass"; pass.kind = CS_RG_PASS_COMPUTE; pass.queue = CS_RG_QUEUE_AUTOMATIC;
		pass.accesses = accesses; pass.accessCount = 2; pass.execute = &Execute;
		return registry->DeclarePass(build, &pass);
	}

	CSRGRegistrationHandle Register(const char* id, CSRGContributorKind kind, CSRGBuildCallback build)
	{
		CSRGContributorDesc desc{};
		desc.structSize = sizeof(desc); desc.apiVersion = CS_RENDER_GRAPH_API_CURRENT;
		desc.id = id; desc.kind = kind; desc.build = build;
		CSRGRegistrationHandle handle{};
		if (registry->Register(&desc, &handle) != CS_RG_OK) return 0;
		return handle;
	}
}

int main()
{
	registry = &RenderGraphRegistry::Get();
	const auto consumer = Register("test.consumer", CS_RG_CONTRIBUTOR_REQUIRED, &Consumer);
	const auto optional = Register("test.optional", CS_RG_CONTRIBUTOR_OPTIONAL, &OptionalMissing);
	const auto producer = Register("test.producer", CS_RG_CONTRIBUTOR_REQUIRED, &Producer);
	CHECK(consumer && optional && producer);

	RenderGraphRegistry::Candidate first;
	CHECK(registry->Compile(1, 1920, 1080, 2560, 1440, first) == CS_RG_OK);
	CHECK(first.resources.size() == 1);
	CHECK(first.passes.size() == 2);
	CHECK(first.passes[0].id == "test.producer.write");
	CHECK(first.passes[1].id == "test.consumer.read");
	CHECK(first.passes[1].accesses[0].resource == first.resources[0].handle);
	CHECK(first.passes[1].accesses[0].binding == 99);
	CHECK(first.contributors.size() == 2); // optional missing input was pruned
	registry->Activate(first);

	CHECK(registry->BeginUnregister(consumer) == CS_RG_OK);
	CHECK(registry->BeginUnregister(producer) == CS_RG_OK);
	CHECK(registry->BeginUnregister(optional) == CS_RG_OK);
	RenderGraphRegistry::Candidate empty;
	CHECK(registry->Compile(2, 1, 1, 1, 1, empty) == CS_RG_OK);
	registry->Activate(empty);
	registry->Retire(1);
	CSRGRegistrationState state{};
	CHECK(registry->GetRegistrationState(consumer, &state) == CS_RG_OK && state == CS_RG_REGISTRATION_RETIRED);

	const auto duplicate = Register("test.duplicate", CS_RG_CONTRIBUTOR_REQUIRED, &DuplicateBindings);
	CHECK(duplicate);
	RenderGraphRegistry::Candidate rejected;
	CHECK(registry->Compile(3, 1, 1, 1, 1, rejected) == CS_RG_E_DUPLICATE_BINDING);
	CHECK(registry->BeginUnregister(duplicate) == CS_RG_OK);

	// Feature passes must remain declarative: ORG owns barriers, queues and all
	// descriptor/view creation used by DeferredRendering.
	const std::filesystem::path root{ CS_TEST_SOURCE_ROOT };
	const std::filesystem::path featureSources[]{
		root / "src/Features/DeferredRendering/DeferredShading.cpp",
		root / "src/Features/DeferredRendering/DeferredShadingExtension.cpp",
		root / "src/Features/DeferredRendering/LightCulling.cpp",
		root / "src/Features/DeferredRendering/ClusteredLightingExtension.cpp"
	};
	const char* forbidden[]{ "ResourceBarrier", "CreateDescriptorHeap", "CreateShaderResourceView",
		"CreateUnorderedAccessView", "CreateConstantBufferView", "SetComputeRootDescriptorTable",
		"ID3D12GraphicsCommandList", "rhi::dx12::get_cmd_list", "SetComputeRoot",
		"CreateComputePipelineState", "CreateRootSignature", "CSDX12ContributorDesc",
		"DeclareResource(", "GetGraphicsQueue", "->Signal(", "->Wait(" };
	for (const auto& path : featureSources) {
		std::ifstream stream(path, std::ios::binary);
		CHECK(stream.good());
		const std::string source{ std::istreambuf_iterator<char>{ stream }, {} };
		for (const auto* token : forbidden) CHECK(source.find(token) == std::string::npos);
	}
	// The graph frontend itself is backend-neutral. Native API access belongs only
	// to the host's explicit D3D11 interoperability layer and native providers.
	for (const auto& path : { root / "src/RenderGraph/RenderGraphHost.h", root / "src/RenderGraph/RenderGraphHost.cpp",
		root / "src/RenderGraph/RenderGraphRuntime.h", root / "src/RenderGraph/D3D11InteropBridge.h" }) {
		std::ifstream stream(path, std::ios::binary);
		CHECK(stream.good());
		const std::string source{ std::istreambuf_iterator<char>{ stream }, {} };
		for (const auto* token : { "ID3D12", "D3D12_", "rhi::dx12", "RenderGraphDX12" })
			CHECK(source.find(token) == std::string::npos);
	}
	return 0;
}
