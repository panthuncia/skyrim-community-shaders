#pragma once

#include <OpenRenderGraph/D3D11Interop.h>

#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#include <ORGModuleServices/ShaderCompiler.h>
#include <ORGModuleServices/ShaderServiceAdapter.h>
#endif

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace org::contributor { class Runtime; }

class RenderGraphRuntime
{
public:
	static RenderGraphRuntime& Get();

	bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
	bool ExecuteGraph(uint32_t width, uint32_t height, uint32_t allocationWidth, uint32_t allocationHeight) noexcept;
	void Shutdown() noexcept;
	bool IsAvailable() const noexcept { return available.load(std::memory_order_acquire); }
	rhi::Device GetRHIDevice() const noexcept { return deviceBundle.graphDevice ? deviceBundle.graphDevice.Get() : rhi::Device{}; }
	org::interop::D3D11Interop* GetInteropCoordinator() const noexcept { return interop.get(); }
	org::contributor::Runtime* GetContributorRuntime() const noexcept { return runtime.get(); }
	uint32_t GetRenderWidth() const noexcept { return renderWidth; }
	uint32_t GetRenderHeight() const noexcept { return renderHeight; }
	uint32_t GetAllocationWidth() const noexcept { return allocationWidth; }
	uint32_t GetAllocationHeight() const noexcept { return allocationHeight; }
	void RequestGraphRebuild() noexcept;
	uint64_t GetActiveGeneration() const noexcept;
	uint64_t GetLastSubmittedCompletion() const noexcept;
	uint64_t GetCompletedGraphValue() noexcept { return completeTimeline ? completeTimeline->GetCompletedValue() : 0; }
	static constexpr uint32_t GetFramesInFlight() noexcept { return kCommandFrameCount; }

private:
	RenderGraphRuntime() = default;
	~RenderGraphRuntime();
	RenderGraphRuntime(const RenderGraphRuntime&) = delete;
	RenderGraphRuntime& operator=(const RenderGraphRuntime&) = delete;

	bool CreateInterop() noexcept;
	void SetDiagnostic(int32_t status, std::string message) const noexcept;

	org::interop::D3D11Interop::DeviceBundle deviceBundle;
	std::unique_ptr<org::interop::D3D11Interop> interop;
	std::unique_ptr<org::contributor::Runtime> runtime;
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	std::unique_ptr<org::services::ShaderCompiler> shaderCompiler;
	std::unique_ptr<org::services::ShaderServiceAdapter> shaderService;
#endif
	rhi::Queue graphicsQueue{};
	rhi::TimelinePtr readyTimeline;
	rhi::TimelinePtr completeTimeline;
	winrt::com_ptr<ID3D11Fence> readyFence11;
	winrt::com_ptr<ID3D11Fence> completeFence11;
	static constexpr uint32_t kCommandFrameCount = 3;

	std::atomic_bool available{ false };
	std::atomic_uint64_t nextReadyFence{ 1 };
	std::atomic_uint64_t nextCompleteFence{ 1 };
	uint64_t frameIndex{};
	uint32_t renderWidth{}, renderHeight{}, allocationWidth{}, allocationHeight{};
	std::thread::id renderThread;
	mutable std::mutex diagnosticMutex;
	mutable int32_t diagnosticStatus{};
	mutable std::string diagnosticMessage;
};
