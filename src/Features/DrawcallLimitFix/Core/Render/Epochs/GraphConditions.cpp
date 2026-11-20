#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/BuildState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "RenderGraph/BuildManager.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace DCLF::Draws
{
	namespace
	{
		// Render thread: the defaults until the first build point.
		std::shared_ptr<const GraphConditions> requested = std::make_shared<const GraphConditions>();
		// Every attached value is kept, so a reference CurrentGraphConditions handed out stays valid while another thread swaps it.
		std::mutex attachedMutex;
		std::vector<std::shared_ptr<const GraphConditions>> attachedKept;  // attachedMutex
		std::atomic<const GraphConditions*> attachedConditions{ nullptr };
		std::mutex cutsMutex;
		std::string requestedCuts;  // cutsMutex
		std::atomic<bool> latchedRecordCut{ false };
		std::atomic<bool> builtForCpu{ false };

		const GraphConditions& AttachedConditions()
		{
			static const GraphConditions kDefaults;
			const auto* conditions = attachedConditions.load(std::memory_order_acquire);
			return conditions ? *conditions : kDefaults;
		}
	}

	bool GraphBuiltForCpu()
	{
		return builtForCpu.load(std::memory_order_acquire);
	}

	bool LatchedRecordCut()
	{
		return latchedRecordCut.load(std::memory_order_relaxed);
	}

	void SetPassCuts(std::string a_cuts)
	{
		std::lock_guard lock(cutsMutex);
		requestedCuts = std::move(a_cuts);
	}

	bool PassCut(std::string_view a_key)
	{
		std::string_view cuts = CurrentGraphConditions().cuts;
		while (!cuts.empty()) {
			const auto comma = cuts.find(',');
			const auto prefix = cuts.substr(0, comma);
			if (prefix == "*" || (!prefix.empty() && a_key.starts_with(prefix)))
				return true;
			cuts = comma == std::string_view::npos ? std::string_view{} : cuts.substr(comma + 1);
		}
		return false;
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

	void RefreshGraphConditions(org::GraphHost& a_host)
	{
		GraphConditions next;
		const auto* tes = RE::TES::GetSingleton();
		next.interior = tes && tes->interiorCell;
		next.twoPassOcclusion = Toggles::Get().Active().cullMode >= 2;
		// A pipeline change rebuilds DCLF's graphs. Frames keep the old ones until a Begin adopts a snapshot recorded on the new
		// ones. Entering or leaving an interior rebuilds the graph anyway.
		next.cpuPipeline = CpuPipelineForCell(Toggles::Get().Active(), next.interior);
		// The direct forward families need their epochs' late passes recorded at execution, after the commit.
		const auto& runtime = RenderGraphRuntime::Get();
		next.forwardDirect = !next.cpuPipeline && SwitchOnUnlessZero(Switch::ForwardDirect) &&
		                     runtime.RecordsAtExecution(RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::Forward));
		next.firstPersonForwardDirect = next.forwardDirect && runtime.RecordsAtExecution(RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::FirstPerson));
		// The first-person forward part has no plain buckets. Without device-generated commands, the CPU pipeline or the direct
		// forward family must draw it, or the engine draws the view.
		next.firstPerson = next.cpuPipeline || next.firstPersonForwardDirect || DrawPipelines::Get().DeviceGenerated();
		if (!next.firstPerson) {
			static std::atomic<bool> logged{ false };
			if (!logged.exchange(true))
				logger::warn(
					"[DCLF] GPU-driven without device-generated commands and without the direct forward family (CS_DCLF_FORWARD_DIRECT): "
					"first person stays the engine's (its epochs and pipeline slots are left out)");
		}
		{
			std::lock_guard lock(cutsMutex);
			next.cuts = requestedCuts;
		}
		if (next == *requested)
			return;
		logger::info("[DCLF] graph conditions: {} cell, {}, {} pipeline{}{}{}{}; the graph is rebuilt", next.interior ? "interior" : "exterior",
				next.twoPassOcclusion ? "two-pass occlusion" : "no occlusion pass (portal and frustum culling)", next.cpuPipeline ? "CPU" : "GPU",
				next.forwardDirect ? ", forward draws on the CPU" : "", next.firstPerson ? "" : ", first person the engine's",
				next.cuts.empty() ? "" : ", passes cut: ", next.cuts);
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
		for (std::uint32_t segment = 0; segment < static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count); ++segment)
			if (RenderGraphRuntime::IsDclfSegment(static_cast<RenderGraphRuntime::Segment>(segment)))
				RenderGraph::Builds::Request(RenderGraphRuntime::EpochOf(static_cast<RenderGraphRuntime::Segment>(segment)), "Drawcall Limit Fix's graph conditions changed");
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
		latchedRecordCut.store(("," + raw->cuts + ",").find(",~latched-record,") != std::string::npos, std::memory_order_relaxed);
	}

	const GraphConditions& CurrentGraphConditions()
	{
		if (const auto* building = ScopedBuildState(); building && building->conditions)
			return *building->conditions;
		return AttachedConditions();
	}

	bool OcclusionTested(const Resources& a_resources, const PassFrame& a_frame)
	{
		const auto& current = CurrentGraphConditions();
		return a_resources.hzb && current.twoPassOcclusion && !current.cpuPipeline && a_frame.cullMode >= 2 && a_frame.width && a_frame.height;
	}
}
