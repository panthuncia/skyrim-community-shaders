#pragma once

#include <memory>
#include <rhi.h>
#include "RenderGraphRegistry.h"

namespace org::services { class ShaderCompiler; class PipelineService; }

class RenderGraphHost
{
public:
	static std::unique_ptr<RenderGraphHost> Create(rhi::Device device);
	~RenderGraphHost();

	struct FrameInfo
	{
		uint64_t frameIndex{};
		uint64_t generation{};
		uint64_t completionValue{};
		uint32_t frameSlot{};
		uint32_t framesInFlight{};
		uint32_t width{};
		uint32_t height{};
	};
	void SetStructuralDefinition(const RenderGraphRegistry::Candidate& candidate);
	void Execute(
		uint32_t frameIndex,
		uint64_t frameFenceValue,
		rhi::Timeline readyTimeline,
		uint64_t readyValue,
		rhi::Timeline completeTimeline,
		uint64_t completeValue,
		const FrameInfo& frame);
	void Retire(uint64_t completedValue) noexcept;
	org::services::ShaderCompiler* GetShaderCompiler() noexcept;
	org::services::PipelineService* GetPipelineService() noexcept;

	RenderGraphHost(const RenderGraphHost&) = delete;
	RenderGraphHost& operator=(const RenderGraphHost&) = delete;

private:
	class Impl;
	explicit RenderGraphHost(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl;
};
