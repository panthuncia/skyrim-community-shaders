#pragma once

#include <CommunityShaders/DX12GraphAPI.h>
#include <d3d12.h>
#include <winrt/base.h>

class DX12RenderRuntime;

class DX12LightCulling
{
public:
	static DX12LightCulling& Get();
	bool Initialize(DX12RenderRuntime& runtime) noexcept;
	void Shutdown() noexcept;

private:
	static CSDX12Status CS_DX12_GRAPH_CALL Build(void* userData, CSDX12BuildHandle build);
	static CSDX12Status CS_DX12_GRAPH_CALL Execute(void* userData, const CSDX12ExecutionContext* context);
	static void CS_DX12_GRAPH_CALL OnShutdown(void* userData);
	bool CreatePipeline() noexcept;
	bool EnsureResources(uint32_t width, uint32_t height) noexcept;
	CSDX12Status Record(const CSDX12ExecutionContext& context) noexcept;

	DX12RenderRuntime* runtime{};
	CSDX12RegistrationHandle registration{};
	winrt::com_ptr<ID3D12Device> device;
	winrt::com_ptr<ID3D12RootSignature> rootSignature;
	winrt::com_ptr<ID3D12PipelineState> clusterPipeline;
	winrt::com_ptr<ID3D12PipelineState> cullPipeline;
	winrt::com_ptr<ID3D12Resource> clusters;
	winrt::com_ptr<ID3D12Resource> pages;
	winrt::com_ptr<ID3D12Resource> pageCounter;
	uint32_t clusterCapacity{};
	uint32_t pageCapacity{};
	uint64_t dispatchCount{};
};
