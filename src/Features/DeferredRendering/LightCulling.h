#pragma once

#include <rhi.h>

class RenderGraphRuntime;

class DX12LightCulling
{
public:
	static DX12LightCulling& Get();
	bool Initialize(RenderGraphRuntime& runtime) noexcept;
	void Shutdown() noexcept;
	rhi::PipelineLayoutHandle GetLayout() const noexcept { return layout ? layout->GetHandle() : rhi::PipelineLayoutHandle{}; }
	rhi::PipelineHandle GetClearPipeline() const noexcept { return clearPipeline ? clearPipeline->GetHandle() : rhi::PipelineHandle{}; }
	rhi::PipelineHandle GetClusterPipeline() const noexcept { return clusterPipeline ? clusterPipeline->GetHandle() : rhi::PipelineHandle{}; }
	rhi::PipelineHandle GetCullPipeline() const noexcept { return cullPipeline ? cullPipeline->GetHandle() : rhi::PipelineHandle{}; }

private:
	bool CreatePipeline() noexcept;

	RenderGraphRuntime* runtime{};
	uint64_t registration{};
	rhi::PipelineLayoutPtr layout;
	rhi::PipelinePtr clearPipeline;
	rhi::PipelinePtr clusterPipeline;
	rhi::PipelinePtr cullPipeline;
};
