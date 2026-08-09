#pragma once

#include <memory>
#include <functional>
#include <span>
#include <string>
#include <vector>
#include <d3d12.h>
#include <rhi.h>
#include <CommunityShaders/DX12GraphAPI.h>

namespace org::services { class ShaderCompiler; class PipelineService; }

class DX12GraphHost
{
public:
	static std::unique_ptr<DX12GraphHost> Create(rhi::Device device);
	~DX12GraphHost();

	using ExecutionCallback = std::function<CSDX12Status(const CSDX12ExecutionContext&)>;
	struct ResourceDefinition
	{
		CSDX12ResourceHandle handle{};
		std::string name;
		CSDX12ResourceDesc desc{};
	};
	struct WorkItem
	{
		std::string name;
		CSDX12QueuePolicy queuePolicy{ CS_DX12_QUEUE_AUTOMATIC };
		uint32_t flags{};
		std::vector<std::string> after;
		std::vector<std::string> before;
		std::vector<CSDX12ResourceAccessDesc> accesses;
		ExecutionCallback execute;
	};
	void SetStructuralDefinition(std::vector<ResourceDefinition> resources, std::vector<WorkItem> workItems);
	void Execute(
		uint32_t frameIndex,
		uint64_t frameFenceValue,
		rhi::Timeline readyTimeline,
		uint64_t readyValue,
		rhi::Timeline completeTimeline,
		uint64_t completeValue,
		const CSDX12FrameInfo& frame);
	void Retire(uint64_t completedValue) noexcept;
	uint64_t UploadBytesInFlight() const noexcept;
	org::services::ShaderCompiler* GetShaderCompiler() noexcept;
	org::services::PipelineService* GetPipelineService() noexcept;

	DX12GraphHost(const DX12GraphHost&) = delete;
	DX12GraphHost& operator=(const DX12GraphHost&) = delete;

private:
	class Impl;
	explicit DX12GraphHost(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl;
};
