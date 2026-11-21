#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/BuildState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "RenderGraph/BuildManager.h"
#include "RenderGraph/Frame/BoundaryCheck.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/Fatal.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace DCLF::Draws
{
	namespace
	{
		// Main thread: the defaults until the first UpdateGraphConditions.
		std::shared_ptr<const GraphConditions> requested = std::make_shared<const GraphConditions>();
		// Every attached value is kept, so a reference AttachedGraphConditions handed out stays valid while another thread swaps it.
		std::mutex attachedMutex;
		std::vector<std::shared_ptr<const GraphConditions>> attachedKept;  // attachedMutex
		std::atomic<const GraphConditions*> attachedConditions{ nullptr };
		std::atomic<bool> builtForCpu{ false };
		// Main thread: whether the first conditions were made. Until DCLF's first build point a Begin leaves them alone.
		bool firstMade = false;
		// Main thread: DCLF's first build point ran, so the next adoption point makes the first conditions.
		bool firstWanted = false;

	}

	bool GraphBuiltForCpu()
	{
		return builtForCpu.load(std::memory_order_acquire);
	}

	bool CpuPipelineForced()
	{
		const auto& pipelines = DrawPipelines::Get();
		return pipelines.Enabled() && !pipelines.GpuDriven();
	}

	bool CpuPipelineForCell(const ToggleSet& a_set, bool a_interior)
	{
		return CpuPipelineFor(a_set, a_interior) || CpuPipelineForced();
	}

	void FirstGraphConditions(org::GraphHost& a_host)
	{
		if (firstMade)
			return;
		firstWanted = true;
		// Native mode's build point is inside the adoption point, so the first conditions are made now. Guest mode's is in the
		// frame (Main::Draw), so they wait for the next Begin's UpdateGraphConditions: builds are asked for only at a boundary.
		if (RenderGraph::Frame::Boundary::InAdoptionPoint())
			UpdateGraphConditions(a_host);
	}

	void UpdateGraphConditions(org::GraphHost& a_host)
	{
		// Before DCLF's first build point nothing of DCLF runs, so its builds are not asked for (a main menu Begin).
		if (!firstMade) {
			if (!firstWanted)
				return;
			firstMade = true;
		}
		// Made from the adopted values at every call. Only a change asks for builds.
		GraphConditions next;
		next.interior = RenderGraph::Frame::Vars::Adopted(RenderGraph::Frame::Vars::Interior());
		const auto toggles = Toggles::Get().Active();
		next.twoPassOcclusion = toggles.cullMode >= 2;
		// A pipeline change rebuilds DCLF's graphs. Frames keep the old ones until a Begin adopts a scene recorded on the new ones,
		// with these conditions (its build state's). Entering or leaving an interior rebuilds the graph anyway.
		next.cpuPipeline = CpuPipelineForCell(toggles, next.interior);
		// The GPU-driven graph draws the forward families on the CPU pipeline's forward sites, recorded at execution after the commit.
		if (!next.cpuPipeline) {
			const auto& runtime = RenderGraphRuntime::Get();
			for (const auto segment : { RenderGraphRuntime::Segment::Forward, RenderGraphRuntime::Segment::FirstPerson })
				if (!runtime.RecordsAtExecution(RenderGraphRuntime::EpochOf(segment)))
					::RenderGraph::Fatal("Drawcall Limit Fix",
						fmt::format("epoch {} does not record at execution, so its forward draws have no site", RenderGraphRuntime::EpochOf(segment)));
		}
		if (next == *requested)
			return;
		logger::info("[DCLF] graph conditions: {} cell, {}, {} pipeline; the graph is rebuilt", next.interior ? "interior" : "exterior",
				next.twoPassOcclusion ? "two-pass occlusion" : "no occlusion pass (portal and frustum culling)", next.cpuPipeline ? "CPU" : "GPU");
		// Set the external epochs before the build the change requests. Running graphs keep the list they were built with.
		// A native host submits the sites' epochs itself, so none is external there.
		if (next.cpuPipeline != requested->cpuPipeline) {
			auto external = RenderGraphRuntime::Get().InStreamEpochs();
			if (next.cpuPipeline && !a_host.InlineAtExecution())
				for (const auto epoch : CpuPipelineEpochs())
					if (std::ranges::find(external, epoch) == external.end())
						external.push_back(epoch);
			std::ranges::sort(external);
			a_host.SetExternalEpochs(std::move(external));
		}
		requested = std::make_shared<const GraphConditions>(std::move(next));
		// The frame takes the new conditions with the snapshot recorded on the new graphs, at a Begin (PublishAttachedBuildState).
		// Only DCLF's epochs are rebuilt. A hosted segment has no graph of its own.
		RenderGraph::Builds::Wanted rebuild;
		for (std::uint32_t segment = 0; segment < static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count); ++segment)
			if (DCLF::IsDclfSegment(static_cast<RenderGraphRuntime::Segment>(segment)))
				rebuild.epochs.push_back(RenderGraphRuntime::EpochOf(static_cast<RenderGraphRuntime::Segment>(segment)));
		RenderGraph::Builds::Ask(std::move(rebuild), "Drawcall Limit Fix's graph conditions changed");
	}

	std::shared_ptr<const GraphConditions> RequestedGraphConditions()
	{
		return requested;
	}

	void AttachGraphConditions(std::shared_ptr<const GraphConditions> a_conditions)
	{
		if (!a_conditions)
			return;
		const auto* raw = a_conditions.get();
		{
			std::scoped_lock lock(attachedMutex);
			if (attachedConditions.load(std::memory_order_relaxed) == raw)
				return;
			attachedKept.push_back(std::move(a_conditions));
		}
		attachedConditions.store(raw, std::memory_order_release);
		builtForCpu.store(raw->cpuPipeline, std::memory_order_release);
	}

	const GraphConditions& AttachedGraphConditions()
	{
		static const GraphConditions kDefaults;
		const auto* conditions = attachedConditions.load(std::memory_order_acquire);
		return conditions ? *conditions : kDefaults;
	}

	bool OcclusionTested(const GraphConditions& a_conditions, const Resources& a_resources, const PassFrame& a_frame)
	{
		return a_resources.hzb && a_conditions.twoPassOcclusion && !a_conditions.cpuPipeline && a_frame.cullMode >= 2 && a_frame.width && a_frame.height;
	}
}
