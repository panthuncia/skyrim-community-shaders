#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#define CHECK(value) do { if (!(value)) return __LINE__; } while (false)

int main()
{
	const std::filesystem::path root{ CS_TEST_SOURCE_ROOT };
	for (const auto& removed : {
		"src/RenderGraph/RenderGraphRegistry.cpp", "src/RenderGraph/RenderGraphHost.cpp",
		"src/RenderGraph/NativeRenderGraphRegistry.cpp", "src/RenderGraph/D3D11InteropBridge.cpp",
		"src/RenderGraph/GPUServiceAPI.cpp", "include/CommunityShaders/RenderGraphAPI.h",
		"include/CommunityShaders/GPUServiceAPI.h" })
		CHECK(!std::filesystem::exists(root / removed));

	const char* forbidden[]{ "ResourceBarrier", "CreateDescriptorHeap", "CreateShaderResourceView",
		"CreateUnorderedAccessView", "CreateConstantBufferView", "ID3D12GraphicsCommandList",
		"rhi::dx12::get_cmd_list", "RenderGraphRegistry", "NativeRenderGraphRegistry",
		"SetStructuralDefinition", "FrameUploadArena", "DescriptorViewCache" };
	for (const auto& relative : {
		"src/Features/DeferredRendering/DeferredShading.cpp",
		"src/Features/DeferredRendering/DeferredShadingExtension.cpp",
		"src/Features/DeferredRendering/LightCulling.cpp",
		"src/Features/DeferredRendering/ClusteredLightingExtension.cpp",
		"src/RenderGraph/RenderGraphRuntime.cpp" }) {
		std::ifstream stream(root / relative, std::ios::binary);
		CHECK(stream.good());
		const std::string source{ std::istreambuf_iterator<char>{ stream }, {} };
		for (const auto* token : forbidden) CHECK(source.find(token) == std::string::npos);
	}
	return 0;
}
