// The CPU-driven pipeline's draw sites. Culls are dispatched at each trigger, and one callback per site records its uploads,
// pre-work and direct draws into DXVK's command buffer on its CS thread.

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "RenderGraph/BufferRefs.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/SiteFrame.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/SiteStats.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/ViewSites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatches.h"
#include "RenderGraph/Async/Retirement.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "RenderGraph/Backend.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Fatal.h"

#include <ORGModuleServices/BackingPool.h>
#include <Render/RenderGraph/ExternalEpochExecution.h>
#include <Render/Runtime/ScopedActiveGraphServices.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>
#include <Tracy/Tracy.hpp>
#include <Tracy/TracyD3D11.hpp>

#include <TlHelp32.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <string_view>
#include <thread>
#include <vector>

namespace DCLF::Direct::detail
{
	namespace
	{
		// Lanes per cull job. Smaller batches are coalesced into a job.
		constexpr std::size_t kJobLanes = 2048;

	}

	org::services::ResetPools<ListPool>& ListPools()
	{
		// Never destroyed. A reset the executor refuses at shutdown releases its pool there.
		static auto* pools = new org::services::ResetPools<ListPool>([](std::function<void()> a_reset) {
			(void)::RenderGraph::Frame::SharedExecutor().Dispatch(SceneScheduler::Scope(), ::RenderGraph::Frame::Executor::Preparation, org::async::TaskDispatch::Cpu,
				"dclf cpu list pool reset", [reset = std::move(a_reset)](const auto&) {
					DCLF_ZONE("CS.DCLF.Cpu.ListPoolReset");
					reset();
				});
		});
		return *pools;
	}

	State& S()
	{
		static State state;
		return state;
	}

	/** @brief The calling pool worker's index, assigned at its first cull. The pool's threads are fixed. */
	std::uint32_t WorkerIndex()
	{
		thread_local std::uint32_t index = ~0u;
		if (index == ~0u)
			index = S().nextWorker.fetch_add(1, std::memory_order_relaxed);
		return index;
	}

	CullArena& WorkerOf(FrameSlot& a_slot)
	{
		const std::uint32_t index = WorkerIndex();
		if (index >= a_slot.recording.Workers())
			::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("a CPU-pipeline cull ran on thread {} of a pool of {} (not a preparation worker)", index,
				a_slot.recording.Workers()));
		return a_slot.recording.Arena(index);
	}

	/** @brief Render thread. The main resources' RTVs and DSV, made once per resources. Older ones stay alive while frames in flight use them. */
	std::shared_ptr<const TargetViews> TargetsOf(std::vector<std::shared_ptr<const void>>& a_holds, const Draws::Resources& a_resources)
	{
		auto& s = S();
		// A new version of an engine target's slot is a new image, so it needs new views.
		std::array<std::shared_ptr<org::GloballyIndexedResource>, Draws::kColorTargets> native{};
		for (std::size_t i = 0; i < native.size(); ++i)
			native[i] = a_resources.native[i].Current();
		const auto nativeDepth = a_resources.nativeDepth.Current();
		const auto refractionNormals = a_resources.refractionNormals.Current();
		// The same resources can have new images: frame placement places them again, and a defrag moves them.
		const auto same = [](rhi::ResourceHandle a_left, rhi::ResourceHandle a_right) { return a_left.index == a_right.index && a_left.generation == a_right.generation; };
		const auto sameImages = [&](const TargetViews& a_views) {
			for (std::uint32_t i = 0; i < a_views.count; ++i)
				if (native[i] && !same(native[i]->GetAPIResource().GetHandle(), a_views.colourResources[i]))
					return false;
			if (nativeDepth && !same(nativeDepth->GetAPIResource().GetHandle(), a_views.depthResource))
				return false;
			return !refractionNormals || !a_views.refractionResource.valid() || same(refractionNormals->GetAPIResource().GetHandle(), a_views.refractionResource);
		};
		if (s.targets && s.targets->count == a_resources.targetCount && s.targets->native == native && s.targets->nativeDepth == nativeDepth &&
			s.targets->refractionNormals == refractionNormals && sameImages(*s.targets))
			return s.targets;
		if (s.targets)
			a_holds.push_back(std::move(s.targets));
		auto views = std::make_shared<TargetViews>();
		views->native = native;
		views->nativeDepth = nativeDepth;
		views->refractionNormals = refractionNormals;
		views->count = a_resources.targetCount;
		const std::uint32_t rtvs = a_resources.targetCount + 1;
		bool ok = s.device.CreateDescriptorHeap({ rhi::DescriptorHeapType::RTV, rtvs, false, "cs.dclf.direct.rtv" }, views->rtvHeap) == rhi::Result::Ok &&
		          s.device.CreateDescriptorHeap({ rhi::DescriptorHeapType::DSV, 1, false, "cs.dclf.direct.dsv" }, views->dsvHeap) == rhi::Result::Ok;
		for (std::uint32_t i = 0; ok && i < a_resources.targetCount; ++i) {
			const auto& target = native[i];
			ok = target != nullptr;
			if (!ok)
				break;
			views->colourResources[i] = target->GetAPIResource().GetHandle();
			views->formats[i] = a_resources.native[i].Format();
			views->colours[i] = rhi::DescriptorSlot(views->rtvHeap->GetHandle(), i);
			ok = s.device.CreateRenderTargetView(views->colours[i], views->colourResources[i], rhi::RtvDesc{}) == rhi::Result::Ok;
		}
		if (ok && nativeDepth) {
			views->depthFormat = a_resources.nativeDepth.Format();
			views->depth = rhi::DescriptorSlot(views->dsvHeap->GetHandle(), 0);
			views->depthResource = nativeDepth->GetAPIResource().GetHandle();
			ok = s.device.CreateDepthStencilView(views->depth, views->depthResource, rhi::DsvDesc{}) == rhi::Result::Ok;
		} else {
			ok = false;
		}
		if (ok && refractionNormals) {
			views->refractionResource = refractionNormals->GetAPIResource().GetHandle();
			views->refractionFormat = a_resources.refractionNormals.Format();
			views->refraction = rhi::DescriptorSlot(views->rtvHeap->GetHandle(), a_resources.targetCount);
			if (s.device.CreateRenderTargetView(views->refraction, views->refractionResource, rhi::RtvDesc{}) != rhi::Result::Ok)
				views->refractionResource = {};
		}
		views->valid = ok;
		if (!ok)
			logger::error("[DCLF] CPU pipeline: the main targets' views could not be made; its sites draw nothing");
		s.targets = views;
		return s.targets;
	}

	CulledQueue* QueueOf(FrameSlot& a_slot)
	{
		if (a_slot.queuesUsed == a_slot.queues.size())
			a_slot.queues.push_back(std::make_unique<CulledQueue>());
		return a_slot.queues[a_slot.queuesUsed++].get();
	}

	SiteView& NewView(FrameSlot& a_slot, SiteRun& a_run, ViewType a_type, const CullView& a_view, bool a_depthPass)
	{
		DCLF_ZONE("CS.DCLF.Cpu.NewView");
		auto& view = a_slot.views.emplace_back();
		view.type = a_type;
		view.view = a_view;
		view.depthPass = a_depthPass;
		view.list = &a_slot.set->List(a_type);
		view.queue = QueueOf(a_slot);
		view.arrived.assign(view.list->size(), nullptr);
		a_run.views.push_back(&view);
		Add(a_slot.counters.batches[static_cast<std::size_t>(a_type)], static_cast<std::uint32_t>(view.list->size()));
		return view;
	}

	namespace
	{
		/**
		 * @brief A slot's first index, only while its range still holds the geometry the set was resolved against. Once streaming
		 * reassigns a range, the old sequence would read past its vertex buffer and fault. Such draws wait for a newer set.
		 */
		void PoolFirstsOf(const ViewBatchSet& a_set, const Draws::IndexPool& a_pool, std::vector<std::uint32_t>& a_firsts, std::uint32_t& a_stale)
		{
			const auto& resolved = a_set.geometryIndexAddress;
			a_firsts.assign(a_pool.slotFirst.size(), Draws::IndexPool::kNoRange);
			for (std::size_t g = 0; g < a_pool.slotFirst.size(); ++g) {
				if (a_pool.slotFirst[g] == Draws::IndexPool::kNoRange || g >= resolved.size() || !resolved[g])
					continue;
				if (g < a_pool.slots.size() && a_pool.slots[g].address == resolved[g])
					a_firsts[g] = a_pool.slotFirst[g];
				else
					++a_stale;
			}
		}
	}

	/** @brief Render thread. Copies the index pool's first indices into the slot, once a frame. */
	void TakePoolFirsts(FrameSlot& a_slot, const Draws::IndexPool* a_pool)
	{
		if (a_slot.poolFirstsTaken || !a_pool || !a_slot.set)
			return;
		DCLF_ZONE("CS.DCLF.Cpu.TakePoolFirsts");
		ZoneValue(a_pool->slotFirst.size());
		PoolFirstsOf(*a_slot.set, *a_pool, a_slot.poolFirsts, a_slot.counters.staleGeometry);
		a_slot.poolFirstsTaken = true;
	}

	/** @brief Render thread. What a main site's recordings bind. Immutable while they read it. */
	void MainBindings(std::vector<std::shared_ptr<const void>>& a_holds, std::span<const std::uint32_t> a_poolFirsts, SiteRun& a_run, const MainCommitInputs& a_inputs)
	{
		auto& b = a_run.bindings;
		const auto& shape = *a_inputs.shape;
		b.resourceHeap = shape.resourceHeap;
		b.samplerHeap = shape.samplerHeap;
		b.main = &shape.indirect;
		if (a_inputs.adopted && a_inputs.resources->pool)
			if (auto version = a_inputs.adopted->Find(*a_inputs.resources->pool->indices); version && version->buffer) {
				b.indexPool = version->buffer->GetAPIResource().GetHandle();
				a_holds.push_back(std::move(version));
			}
		b.poolFirsts = a_poolFirsts;
		b.framePush = Draws::MainFramePushWords(a_inputs.resources->frameConstantsAddress, a_inputs.resources->frameConstantsRef);
		// The animated rows as the commit latched them, which BuildDrawsCS's MaterialRowAddress also reads.
		if (const auto* latch = a_inputs.latch) {
			b.animatedRows = std::uint64_t(latch->animatedRowsLo) | (std::uint64_t(latch->animatedRowsHi) << 32);
			b.animatedRowsRef = latch->animatedRowsRef;
		}
		a_run.refractionBindings = b;
		std::uint32_t push[kShadowPushWords]{};
		Draws::MainShadowLayoutPush(a_inputs.resources->frameConstantsAddress, a_inputs.resources->frameConstantsRef, push);
		a_run.refractionBindings.lodFadeFrame = Draws::LodFadeFrameAddress(a_inputs.resources->frameConstantsAddress);
		// D3D12's refraction stage reads its blocks and LodFadeFrame through heap indices, so device addresses there fault on AMD.
		if (::RenderGraph::Backend::IsD3D12()) {
			if (!a_inputs.resources->frameConstantsRef || !a_inputs.resources->frameBlocks)
				logger::critical("[DCLF] D3D12 refraction draws: the frame constants have no buffer view ref or block views");
			else {
				Draws::MainShadowLayoutViews(a_inputs.resources->frameConstantsRef, *a_inputs.resources->frameBlocks, push);
				a_run.refractionBindings.lodFadeFrameRef = a_inputs.resources->frameConstantsRef + Draws::LodFadeFrameAddress(0);
			}
		}
		std::copy(std::begin(push), std::end(push), a_run.refractionBindings.shadowPush.begin());
	}

	SiteRun& NewBareRun(FrameSlot& a_slot, Site a_site)
	{
		auto& run = a_slot.runs.emplace_back();
		run.site = a_site;
		run.record = FrameMode().record;
		run.slot = &a_slot;
		run.device = S().device;
		run.frameValue = S().signalled + 1;
		return run;
	}

	SiteRun& NewRun(FrameSlot& a_slot, Site a_site, const MainCommitInputs& a_inputs)
	{
		auto& run = NewBareRun(a_slot, a_site);
		run.shape = a_inputs.shape;
		run.resources = a_inputs.resources;
		run.targets = TargetsOf(a_slot.holds, *a_inputs.resources);
		run.valid = run.targets && run.targets->valid;
		MainBindings(a_slot.holds, a_slot.poolFirsts, run, a_inputs);
		if (a_slot.ownsEpochs && (a_site == Site::Depth || a_site == Site::Colour))
			PrepareFamilies(a_slot, run, a_inputs);
		return run;
	}

	void SetPending(SiteRun& a_run)
	{
		S().pending = &a_run;
	}


	void ArriveColour(FrameSlot& a_slot);

	/** @brief Marks a culled batch's list end and writes its indirect draws in Indirect mode. */
	CulledBatch* FinishCulled(FrameSlot& a_slot, SiteRun& a_run, SiteView& a_view, CullArena& a_arena, std::size_t a_index, const CulledBatch& a_culled,
		const RecordBindings& a_bindings)
	{
		auto* out = a_arena.Make<CulledBatch>(1);
		*out = a_culled;
		out->last = a_index + 1 == a_view.list->size();
		out->hasRuns = false;
		if (DrawsIndirect(a_run, a_view))
			if (const auto indices = WriteIndirectDraws(*out, *a_slot.set, a_view.type, a_view.depthPass, a_bindings, a_slot.indirect, a_arena)) {
				std::atomic_ref<std::uint64_t>(a_slot.counters.indices[static_cast<std::size_t>(a_run.site)]).fetch_add(indices, std::memory_order_relaxed);
				std::atomic_ref<std::uint64_t>(a_slot.counters.viewIndices[static_cast<std::size_t>(a_view.type)]).fetch_add(indices, std::memory_order_relaxed);
			}
		return out;
	}

	void CullJob(FrameSlot& a_slot, SiteRun& a_run, SiteView& a_view, const CullFrame& a_frame, std::size_t a_first, std::size_t a_end)
	{
		DCLF_ZONE("CS.DCLF.Cpu.Cull");
		const auto viewName = CpuViewName(static_cast<std::size_t>(a_view.type));
		ZoneText(viewName.data(), viewName.size());
		const auto start = Clock::now();
		auto& arena = WorkerOf(a_slot);
		const auto& list = *a_view.list;
		const std::uint32_t parity = static_cast<std::uint32_t>(LiveParity(a_frame.frameValuesSeq));
		const bool depthMain = a_run.site == Site::Depth && a_view.type == ViewType::Main;
		const auto& bindings = a_view.bindings ? *a_view.bindings : a_run.bindings;
		for (std::size_t i = a_first; i < a_end; ++i) {
			const auto& batch = list[i];
			const CulledBatch* culled = nullptr;
			if (a_view.colourWords) {
				const CulledBatch* depth = i < a_slot.mainDepthCount ? a_slot.mainDepth[i].load(std::memory_order_acquire) : nullptr;
				if (!depth)
					depth = &CullBatch(batch, parity, a_view.mainView, a_frame, arena);
				culled = &ColourObjectWords(*depth, a_view.view, a_frame, arena);
			} else {
				culled = &CullBatch(batch, parity, a_view.view, a_frame, arena);
			}
			auto* out = FinishCulled(a_slot, a_run, a_view, arena, i, *culled, bindings);
			if (depthMain && i < a_slot.mainDepthCount)
				a_slot.mainDepth[i].store(out, std::memory_order_release);
			if (!a_view.view.stampOnly)
				a_view.queue->Push(out);
		}
		Add(a_slot.counters.cullUs[static_cast<std::size_t>(a_view.type)], MicrosecondsSince(start));
		if (depthMain && a_slot.mainDepthPending.fetch_sub(static_cast<std::uint32_t>(a_end - a_first), std::memory_order_acq_rel) == a_end - a_first)
			ArriveColour(a_slot);
	}


	/**
	 * @brief Any thread. Dispatches a view's culls on DCLF's pool as frame-critical work. Never runs them inline, since the caller
	 * may be the render thread.
	 */
	void DispatchCulls(FrameSlot& a_slot, SiteRun& a_run, SiteView& a_view, const std::shared_ptr<const CullFrame>& a_frame)
	{
		DCLF_ZONE("CS.DCLF.Cpu.DispatchCulls");
		const std::size_t count = a_view.list->size();
		const std::size_t jobBatches = std::max<std::size_t>(1, kJobLanes / a_slot.set->lanes);
		for (std::size_t first = 0; first < count; first += jobBatches) {
			const std::size_t end = std::min(count, first + jobBatches);
			a_slot.outstanding.fetch_add(1, std::memory_order_acq_rel);
			if (!::RenderGraph::Frame::SharedExecutor().DispatchFrameCritical(SceneScheduler::Scope(), "dclf cpu cull",
					[slot = &a_slot, run = &a_run, view = &a_view, frame = a_frame, first, end](const auto&) {
						CullJob(*slot, *run, *view, *frame, first, end);
						slot->outstanding.fetch_sub(1, std::memory_order_acq_rel);
					}))
				::RenderGraph::Fatal("Drawcall Limit Fix", "the preparation pool refused a CPU-pipeline cull");
		}
	}

	void DispatchSiteCulls(FrameSlot& a_slot, std::function<void()> a_dispatch)
	{
		a_slot.outstanding.fetch_add(1, std::memory_order_acq_rel);
		if (!::RenderGraph::Frame::SharedExecutor().DispatchFrameCritical(SceneScheduler::Scope(), "dclf cpu site culls", [slot = &a_slot, dispatch = std::move(a_dispatch)](const auto&) {
				dispatch();
				slot->outstanding.fetch_sub(1, std::memory_order_acq_rel);
			}))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the preparation pool refused a CPU-pipeline site's culls");
	}

	/** @brief The colour gate. The last arrival dispatches main colour. */
	void ArriveColour(FrameSlot& a_slot)
	{
		if (a_slot.colourGate.fetch_sub(1, std::memory_order_acq_rel) == 1 && a_slot.colourDispatch)
			a_slot.colourDispatch();
	}


	/** @brief Moves the view's queued batches into its arrival slots. Returns how many arrived. */
	std::size_t DrainView(SiteView& a_view)
	{
		return a_view.queue->Drain([&](const CulledBatch*&& a_batch) {
			a_view.arrived[a_batch->batch->index] = a_batch;
			++a_view.received;
		});
	}

	/** @brief Waits for batch a_index. Later batches are kept as they arrive. */
	const CulledBatch& Await(SiteRun& a_run, SiteView& a_view, std::size_t a_index)
	{
		bool waited = false;
		while (!a_view.arrived[a_index])
			if (!DrainView(a_view)) {
				if (!std::exchange(waited, true))
					Add(a_run.slot->waits, 1u);
				// One zone per stretch the CS thread waits on the workers.
				DCLF_ZONE("CS.DCLF.Cpu.AwaitBatch");
				do
					std::this_thread::yield();
				while (!DrainView(a_view));
			}
		return *a_view.arrived[a_index];
	}

	std::vector<const CulledBatch*> AwaitAll(SiteRun& a_run, SiteView& a_view)
	{
		std::vector<const CulledBatch*> all;
		all.reserve(a_view.list->size());
		for (std::size_t i = 0; i < a_view.list->size(); ++i)
			all.push_back(&Await(a_run, a_view, i));
		return all;
	}

	/** @brief A rendering of the main targets, depth only when a_colours is empty. Not copyable, because begin points into it. */
	struct MainPass
	{
		std::array<rhi::ColorAttachment, Draws::kColorTargets> colours{};
		rhi::DepthAttachment depth{};
		rhi::PassBeginInfo begin{};

		MainPass(const SiteRun& a_run, bool a_colours, const char* a_name)
		{
			const auto& targets = *a_run.targets;
			const auto& shape = *a_run.shape;
			for (std::uint32_t i = 0; a_colours && i < targets.count; ++i) {
				colours[i].rtv = targets.colours[i];
				colours[i].loadOp = rhi::LoadOp::Load;
				colours[i].storeOp = rhi::StoreOp::Store;
				colours[i].resource = targets.colourResources[i];
			}
			depth.dsv = targets.depth;
			depth.depthLoad = rhi::LoadOp::Load;
			depth.depthStore = rhi::StoreOp::Store;
			depth.stencilLoad = rhi::LoadOp::Load;
			depth.stencilStore = rhi::StoreOp::Store;
			begin.colors = { colours.data(), a_colours ? targets.count : 0u };
			begin.depth = &depth;
			begin.width = shape.width;
			begin.height = shape.height;
			begin.minDepth = shape.minDepth;
			begin.maxDepth = shape.maxDepth;
			begin.debugName = a_name;
		}
		MainPass(const MainPass&) = delete;
		MainPass& operator=(const MainPass&) = delete;
	};

	void BeginMain(rhi::CommandList& a_list, const SiteRun& a_run, bool a_colours, const char* a_name)
	{
		MainPass pass(a_run, a_colours, a_name);
		a_list.BeginPass(pass.begin);
	}

	rhi::CommandList& SiteOut::Inline()
	{
		if (direct)
			return primary;
		if (open)
			return *open;
		open = pool.Next(device);
		rhi::SecondaryInheritance outside{};
		outside.insideRendering = false;
		outside.debugName = "DCLF direct site";
		if (!open || open->BeginSecondary(outside) != rhi::Result::Ok)
			::RenderGraph::Fatal("Drawcall Limit Fix", "the CPU pipeline could not begin a secondary list on DXVK's CS thread");
		return *open;
	}

	void SiteOut::Flush()
	{
		if (!open)
			return;
		open->End();
		rhi::CommandList* list = std::exchange(open, nullptr);
		BindHeaps();
		primary.ExecuteCommands({ &list, 1u });
	}

	void SiteOut::BindHeaps()
	{
		if (!direct && resourceHeap.valid())
			primary.SetDescriptorHeaps(resourceHeap, samplerHeap);
	}

	std::vector<const CulledBatch*> RecordViewPass(SiteOut& a_out, SiteRun& a_run, SiteView& a_view, rhi::PassBeginInfo a_begin, const RecordBindings& a_bindings)
	{
		auto& slot = *a_run.slot;
		std::vector<const CulledBatch*> batches;
		batches.reserve(a_view.list->size());
		auto& list = a_out.Inline();
		a_begin.contents = rhi::PassContents::Inline;
		list.BeginPass(a_begin);
		RecordContext context{ list, *slot.set, a_view.type, a_view.view.viewIndex, a_run.site, a_view.depthPass, a_bindings, &slot.counters };
		if (DrawsIndirect(a_run, a_view))
			context.indirect = &slot.indirect;
		const bool valid = BeginList(context);
		// Draw casters nearest the light first. Fewer depth writes survive and HiZ rejects more tiles.
		if (a_run.site == Site::Shadow && !context.indirect) {
			batches = AwaitAll(a_run, a_view);
			if (valid)
				RecordSortedBatches(context, batches, a_view.view.clipPlanes[4]);
		} else {
			for (std::size_t i = 0; i < a_view.list->size(); ++i) {
				const auto& batch = Await(a_run, a_view, i);
				batches.push_back(&batch);
				if (valid && batch.count)
					RecordBatch(context, batch);
			}
		}
		list.EndPass();
		return batches;
	}

	/** @brief A plain list's pass in the main targets' rendering. */
	std::vector<const CulledBatch*> RecordPlainList(SiteOut& a_out, SiteRun& a_run, SiteView& a_view, bool a_colours, const char* a_name)
	{
		MainPass pass(a_run, a_colours, a_name);
		return RecordViewPass(a_out, a_run, a_view, pass.begin, a_run.bindings);
	}

	void RecordDepthSite(SiteOut& a_out, SiteRun& a_run)
	{
		if (a_run.main)
			RecordPlainList(a_out, a_run, *a_run.main, false, "DCLF direct depth");
		RecordFamilies(a_out, a_run, true);
		RecordHostedDepthViews(a_out, a_run);
	}

	void RecordColourSite(SiteOut& a_out, SiteRun& a_run)
	{
		auto& slot = *a_run.slot;
		const auto& ranges = Extensions::MainRanges().drawn;
		// Decals are drawn in the decal depth pass first, then in the second pass's groups.
		std::vector<const CulledBatch*> decals;
		if (a_run.decal)
			decals = AwaitAll(a_run, *a_run.decal);
		if (!decals.empty() && Extensions::MainRanges().anyDepthPrepass) {
			auto& list = a_out.Inline();
			BeginMain(list, a_run, false, "DCLF direct decal depth");
			RecordContext context{ list, *slot.set, ViewType::Decal, 0, Site::Colour, true, a_run.bindings, &slot.counters };
			if (BeginList(context))
				for (const auto& range : ranges)
					if (range.depthPrepass && range.slot < Draws::kDecalGroups)
						RecordDecalGroup(context, range.slot, decals);
			list.EndPass();
		}
		// Main colour tests depth EQUAL against the depth site's output.
		std::vector<const CulledBatch*> main;
		if (a_run.main)
			main = RecordPlainList(a_out, a_run, *a_run.main, true, "DCLF direct colour");
		RecordFamilies(a_out, a_run, false);
		if (ranges.empty() || (decals.empty() && main.empty()))
			return;
		auto& list = a_out.Inline();
		BeginMain(list, a_run, true, "DCLF direct second pass");
		RecordContext decal{ list, *slot.set, ViewType::Decal, 0, Site::Colour, false, a_run.bindings, &slot.counters };
		RecordContext terrain{ list, *slot.set, ViewType::Main, 0, Site::Colour, false, a_run.bindings, &slot.counters };
		if (a_run.main && DrawsIndirect(a_run, *a_run.main))
			terrain.indirect = &slot.indirect;
		for (const auto& range : ranges) {
			if (range.slot < Draws::kDecalGroups) {
				if (!decals.empty() && BeginList(decal))
					RecordDecalGroup(decal, range.slot, decals);
			} else if (!main.empty() && BeginList(terrain)) {
				RecordTerrainGroup(terrain, range.slot - Draws::kDecalGroups + 1, main);
			}
		}
		list.EndPass();
	}

	void RecordForwardSite(SiteOut& a_out, SiteRun& a_run)
	{
		auto& slot = *a_run.slot;
		if (a_run.forward) {
			const auto batches = AwaitAll(a_run, *a_run.forward);
			auto& list = a_out.Inline();
			BeginMain(list, a_run, true, "DCLF direct forward");
			RecordContext context{ list, *slot.set, ViewType::Forward, 0, Site::Forward, false, a_run.bindings, &slot.counters };
			if (BeginList(context))
				RecordForward(context, batches);
			list.EndPass();
		}
		if (a_run.refractionView) {
			const auto batches = AwaitAll(a_run, *a_run.refractionView);
			const auto& targets = *a_run.targets;
			if (!a_run.refraction || !targets.refractionResource.valid())
				return;
			const auto& shape = *a_run.shape;
			// Clear refraction normals as the engine's RENDER_NORMAL_CLEAR does.
			rhi::ColorAttachment colour{};
			colour.rtv = targets.refraction;
			colour.loadOp = rhi::LoadOp::Clear;
			colour.storeOp = rhi::StoreOp::Store;
			colour.clear.type = rhi::ClearValueType::Color;
			colour.clear.rgba[0] = colour.clear.rgba[1] = 0.5f;
			colour.clear.rgba[2] = colour.clear.rgba[3] = 0.0f;
			colour.resource = targets.refractionResource;
			rhi::DepthAttachment depth{};
			depth.dsv = targets.depth;
			depth.depthLoad = rhi::LoadOp::Load;
			depth.depthStore = rhi::StoreOp::Store;
			depth.stencilLoad = rhi::LoadOp::Load;
			depth.stencilStore = rhi::StoreOp::Store;
			rhi::PassBeginInfo begin{};
			begin.colors = { &colour, 1 };
			begin.depth = &depth;
			begin.width = shape.width;
			begin.height = shape.height;
			begin.minDepth = shape.minDepth;
			begin.maxDepth = shape.maxDepth;
			begin.debugName = "DCLF direct refraction normals";
			auto& list = a_out.Inline();
			list.BeginPass(begin);
			RecordContext context{ list, *slot.set, ViewType::Refraction, 0, Site::Forward, false, a_run.refractionBindings, &slot.counters };
			if (BeginList(context))
				RecordForward(context, batches);
			list.EndPass();
		}
	}

	namespace
	{
		// CS thread. The run being recorded, for its epoch's late pass.
		thread_local SiteRun* executing = nullptr;
	}

	bool SitesInline()
	{
		const auto* host = RenderGraphRuntime::Get().Host();
		return host && host->InlineAtExecution();
	}

	namespace
	{
		/** @brief Takes the run AdoptSiteRun kept. a_found says whether the commit kept any entry, null or not. */
		SiteRun* TakeSiteRun(const org::RunRef& a_run, bool& a_found)
		{
			auto& s = S();
			std::scoped_lock lock(s.siteRunsLock);
			a_found = a_run.lease < s.siteRuns.size() && s.siteRuns[a_run.lease].first == a_run;
			if (!a_found)
				return nullptr;
			return std::exchange(s.siteRuns[a_run.lease].second, nullptr);
		}
	}

	namespace
	{
		bool SameImage(rhi::ResourceHandle a_left, rhi::ResourceHandle a_right)
		{
			return a_left.index == a_right.index && a_left.generation == a_right.generation;
		}

		/** @brief Points a run's views at the images its pass's recording binds, when they differ from the ones made at commit. */
		void UseSiteImages(SiteRun& a_run, const SiteImages& a_images)
		{
			if (a_run.site == Site::FirstPerson) {
				UseFirstPersonImages(a_run, a_images);
				return;
			}
			if (!a_run.targets)
				return;
			const auto& made = *a_run.targets;
			const std::uint32_t colours = std::min<std::uint32_t>(made.count, static_cast<std::uint32_t>(a_images.colours.size()));
			// Forward declares refraction normals after the main colours.
			const bool refraction = a_run.site == Site::Forward && a_images.colours.size() > made.count && made.refractionResource.valid();
			bool differs = refraction && !SameImage(made.refractionResource, a_images.colours[made.count].resource);
			for (std::uint32_t i = 0; i < colours; ++i)
				differs = differs || !SameImage(made.colourResources[i], a_images.colours[i].resource);
			if (!a_images.depths.empty())
				differs = differs || !SameImage(made.depthResource, a_images.depths.front().resource);
			if (!differs)
				return;
			auto views = TargetViews::RebindOf(a_run.targets);
			for (std::uint32_t i = 0; i < colours; ++i) {
				views->colours[i] = a_images.colours[i].view;
				views->colourResources[i] = a_images.colours[i].resource;
			}
			if (refraction) {
				views->refraction = a_images.colours[made.count].view;
				views->refractionResource = a_images.colours[made.count].resource;
			}
			if (!a_images.depths.empty()) {
				views->depth = a_images.depths.front().view;
				views->depthResource = a_images.depths.front().resource;
			}
			// The run lives until its frame retires, and the copy keeps the commit's views.
			a_run.targets = std::move(views);
		}
	}

	bool RecordExecutingSite(Site a_site, rhi::CommandList& a_list, const org::RunRef* a_run, const SiteImages* a_images)
	{
		auto& s = S();
		auto* run = executing;
		// Outside a site callback, a native frame records the run its epoch's commit opened or was handed, on the thread that submits the epoch.
		const bool callback = run != nullptr;
		bool adopted = false;
		if (!callback && a_run) {
			run = TakeSiteRun(*a_run, adopted);
			if (run && run->site != a_site)
				run = nullptr;
		}
		if (!callback && !adopted && SitesInline() && s.pending && s.pending->site == a_site && s.pending->slot->ownsEpochs)
			run = s.pending;
		if (!run && !adopted) {
			run = s.handedOff.load(std::memory_order_acquire);
			if (run && run->site == a_site)
				s.handedOff.store(nullptr, std::memory_order_release);
		}
		if (!run || run->site != a_site || !run->valid)
			return false;
		if (a_images)
			UseSiteImages(*run, *a_images);
		const auto start = Clock::now();
		const auto drawnNow = [&] {
			std::uint32_t drawn = 0;
			for (const auto* view : run->views)
				drawn += std::atomic_ref<std::uint32_t>(run->slot->counters.drawn[static_cast<std::size_t>(view->type)]).load(std::memory_order_relaxed);
			return drawn;
		};
		const std::uint32_t drawnBefore = callback ? 0u : drawnNow();
		auto full = rhi::FullMemoryBarrier();
		rhi::BarrierBatch barrier{};
		barrier.globals = { &full, 1u };
		a_list.Barriers(barrier);
		// Native frames count site stats around the draws. The guest callback wraps the whole execution.
		const std::size_t siteIndex = static_cast<std::size_t>(run->site);
		const std::uint32_t statsSlot = !callback && run->slot->ownsEpochs ? SiteStats::Begin(run->device, a_list, siteIndex, run->frameValue) : ~0u;
		// Guest records into secondaries. Native records straight into the epoch's primary list.
		SiteOut out{ a_list, run->slot->recording.Lists(), run->device, run->bindings.resourceHeap, run->bindings.samplerHeap };
		out.direct = SitesInline();
		switch (run->site) {
		case Site::Shadow:
			RecordShadowSite(out, *run);
			break;
		case Site::Depth:
			RecordDepthSite(out, *run);
			break;
		case Site::Colour:
			RecordColourSite(out, *run);
			break;
		case Site::Forward:
			RecordForwardSite(out, *run);
			break;
		case Site::FirstPerson:
			RecordFirstPersonSite(out, *run);
			break;
		default:
			break;
		}
		out.Flush();
		SiteStats::End(a_list, statsSlot);
		run->recordedDraws = true;
		if (!callback) {
			auto& slot = *run->slot;
			Add(slot.siteRuns[siteIndex], 1u);
			if (drawnNow() == drawnBefore)
				Add(slot.siteEmpty[siteIndex], 1u);
			Add(slot.counters.recordUs[siteIndex], MicrosecondsSince(start));
		}
		return true;
	}

	/** @brief Guest, CS thread. Records a site's commands into DXVK's command buffer, one call per segment of its epoch's execution. */
	void SiteCallback(void* a_user, rhi::CommandList& a_list)
	{
		auto& run = *static_cast<SiteRun*>(a_user);
		auto& slot = *run.slot;
		const auto start = Clock::now();
		const std::size_t site = static_cast<std::size_t>(run.site);
		static constexpr std::string_view kSiteNames[kSites] = { "shadow", "depth", "colour", "forward", "first person" };
		DCLF_ZONE("CS.DCLF.Cpu.Site");
		const auto siteName = kSiteNames[site];
		ZoneText(siteName.data(), siteName.size());
		// One call per segment, in order. Draws are counted from the first and the views drained after the last.
		const std::size_t segment = run.execution ? run.execution->ExecutedSegments() : 0;
		const std::size_t segments = run.execution ? (std::max<std::size_t>)(run.execution->segments.size(), 1) : 1;
		const bool lastSegment = segment + 1 >= segments;
		if (segment == 0) {
			run.drawnBefore = 0;
			for (const auto* view : run.views)
				run.drawnBefore += std::atomic_ref<std::uint32_t>(slot.counters.drawn[static_cast<std::size_t>(view->type)]).load(std::memory_order_relaxed);
		}
		if (run.valid && run.execution) {
			// The epoch's ORG execution runs uploads, its passes, then the late pass that records the draws. Entry and exit barriers are full.
			executing = &run;
			const std::uint32_t statsSlot = SiteStats::Begin(run.device, a_list, site, run.frameValue);
			if (segment < run.execution->segments.size())
				run.execution->Execute(segment, a_list);
			SiteStats::End(a_list, statsSlot);
			executing = nullptr;
			if (lastSegment && !run.recordedDraws && !std::exchange(S().loggedNoLatePass, true))
				logger::error("[DCLF] CPU pipeline: the {} site's epoch was executed without its late pass (no draws pass in its recording): it draws nothing", site);
		} else if (!std::exchange(S().loggedNotAdopted, true)) {
			logger::error("[DCLF] CPU pipeline: the site's targets have no views, or its epoch gave no execution: the {} site draws nothing", site);
		}
		if (!lastSegment) {
			Add(slot.counters.recordUs[site], MicrosecondsSince(start));
			return;
		}
		// Empty every view's queue for the slot's next frame, whatever was recorded.
		for (auto* view : run.views)
			if (!view->view.stampOnly)
				while (view->received < view->list->size())
					if (!DrainView(*view))
						std::this_thread::yield();
		std::uint32_t drawnAfter = 0;
		for (const auto* view : run.views)
			drawnAfter += std::atomic_ref<std::uint32_t>(slot.counters.drawn[static_cast<std::size_t>(view->type)]).load(std::memory_order_relaxed);
		Add(slot.siteRuns[site], 1u);
		if (drawnAfter == run.drawnBefore)
			Add(slot.siteEmpty[site], 1u);
		Add(slot.counters.recordUs[site], MicrosecondsSince(start));
	}

	/** @brief Render thread. Queues the site's callback with the frame values' wait and the execution's waits and signals. */
	void Emit(SiteRun& a_run)
	{
		DCLF_ZONE("CS.DCLF.Cpu.Emit");
		auto& s = S();
		const rhi::TimelinePoint wait = FrameValues::Get().FrameWaitPoint();
		auto& waits = s.emitWaits;
		auto& signals = s.emitSignals;
		static constexpr const char* kLabels[kSites] = { "DCLF direct shadow", "DCLF direct depth", "DCLF direct colour", "DCLF direct forward",
			"DCLF direct first person" };
		// One callback per segment, each with its waits and its signals on the graphics timeline.
		// DXVK's list is ended first for a segment marked split.
		const auto& execution = a_run.execution;
		const std::size_t count = execution ? (std::max<std::size_t>)(execution->segments.size(), 1) : 1;
		for (std::size_t index = 0; index < count; ++index) {
			waits.clear();
			signals.clear();
			if (index == 0 && wait.value)
				waits.push_back(wait);
			if (execution && index < execution->segments.size()) {
				const auto& segment = execution->segments[index];
				if (segment.split && index)
					RenderGraphRuntime::Get().SplitStream();
				waits.insert(waits.end(), segment.waits.begin(), segment.waits.end());
				signals.insert(signals.end(), segment.signals.begin(), segment.signals.end());
			}
			// DXVK rejected the commands, or a wait or signal is not on the host's timelines.
			if (!RenderGraphRuntime::Get().EmitCommands(&SiteCallback, &a_run, waits.data(), static_cast<std::uint32_t>(waits.size()), signals.data(),
					static_cast<std::uint32_t>(signals.size()), true, kLabels[static_cast<std::size_t>(a_run.site)], true))
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the CPU pipeline's {} site could not emit its commands", kLabels[static_cast<std::size_t>(a_run.site)]));
		}
	}

	namespace
	{
		/**
		 * @brief A released slot's frame, destroyed together on the pool. Its signal has completed, so nothing reads it.
		 * It leaves the render thread because BasicRHI destroys take device locks, and ORG and DCLF deletions take their own locks.
		 * Engine references never die here. They go back lock-free and are released at Present.
		 * Members are destroyed in reverse order, runs before their views.
		 */
		struct ReleasedFrame
		{
			std::shared_ptr<void> familyState, viewState, shadowState;
			std::function<void()> colourDispatch;
			std::unique_ptr<std::atomic<const CulledBatch*>[]> mainDepth;
			std::shared_ptr<const ViewBatchSet> set;
			std::vector<std::shared_ptr<const void>> holds;
			std::deque<SiteView> views;
			std::deque<SiteRun> runs;
		};

		/** @brief Render thread. Destroys a_frame on DCLF's pool, or here if refused at shutdown. */
		void ReleaseLater(std::unique_ptr<ReleasedFrame> a_frame)
		{
			// Shared because a task must be copyable. The body destroys the frame, so no task copy left here ends it on this thread.
			auto frame = std::make_shared<std::unique_ptr<ReleasedFrame>>(std::move(a_frame));
			(void)::RenderGraph::Frame::SharedExecutor().Dispatch(SceneScheduler::Scope(), ::RenderGraph::Frame::Executor::Preparation, org::async::TaskDispatch::Cpu,
				"dclf cpu frame release", [frame](const auto&) {
					DCLF_ZONE("CS.DCLF.Cpu.ReleaseFrame");
					frame->reset();
				});
		}
	}

	/** @brief Render thread. Releases a slot's last frame after its signal completed. */
	void Release(FrameSlot& a_slot)
	{
		DCLF_ZONE("CS.DCLF.Cpu.Release");
		if (const auto& timeline = S().timeline; timeline && *timeline)
			SiteStats::Collect((*timeline)->GetCompletedValue());
		// Nothing of the frame still runs on the pool, and a continuation not started yet will do nothing.
		a_slot.generation.fetch_add(1, std::memory_order_seq_cst);
		if (a_slot.outstanding.load(std::memory_order_seq_cst)) {
			DCLF_ZONE("CS.DCLF.Cpu.ReleaseWait");
			while (a_slot.outstanding.load(std::memory_order_seq_cst))
				std::this_thread::yield();
		}
		// What no callback drained, such as a site whose trigger was not reached.
		for (std::size_t q = 0; q < a_slot.queuesUsed; ++q)
			a_slot.queues[q]->Discard();
		auto& window = S().window;
		const auto& c = a_slot.counters;
		for (std::size_t v = 0; v < kViewTypes; ++v) {
			window.sum.batches[v] += c.batches[v];
			window.sum.drawn[v] += c.drawn[v];
			window.sum.cullUs[v] += c.cullUs[v];
			window.sum.viewIndices[v] += c.viewIndices[v];
		}
		for (std::size_t site = 0; site < kSites; ++site) {
			window.sum.recordUs[site] += c.recordUs[site];
			window.sum.indices[site] += c.indices[site];
			window.siteRuns[site] += a_slot.siteRuns[site];
			window.siteEmpty[site] += a_slot.siteEmpty[site];
		}
		window.sum.waits += a_slot.waits;
		window.sum.staleGeometry += c.staleGeometry;
		window.sum.binds += c.binds;
		window.sum.sameBinds += c.sameBinds;
		window.sum.pushes += c.pushes;
		window.sum.indirects += c.indirects;
		// The arenas let go, and the CS thread's command pool is reset on the pool, since each reset is a driver call. The slot takes an
		// already-reset pool instead.
		a_slot.recording.Retire(ListPools());
		a_slot.queuesUsed = 0;
		// The frame's objects leave the slot whole and are destroyed on the pool.
		auto released = std::make_unique<ReleasedFrame>();
		std::swap(released->runs, a_slot.runs);
		std::swap(released->views, a_slot.views);
		std::swap(released->holds, a_slot.holds);
		released->set = std::move(a_slot.set);
		released->mainDepth = std::move(a_slot.mainDepth);
		released->colourDispatch = std::exchange(a_slot.colourDispatch, nullptr);
		released->shadowState = std::move(a_slot.shadowState);
		released->viewState = std::move(a_slot.viewState);
		released->familyState = std::move(a_slot.familyState);
		ReleaseLater(std::move(released));
		a_slot.poolFirstsTaken = false;
		a_slot.mainDepthCount = 0;
		a_slot.depthRan = false;
		a_slot.forwardRun = nullptr;
		a_slot.counters = {};
		a_slot.siteRuns = {};
		a_slot.siteEmpty = {};
		a_slot.waits = 0;
		a_slot.signal = 0;
		a_slot.retirePoint = 0;
		a_slot.retireNoted = false;
		a_slot.used = false;
	}

	/** @brief Render thread. Waits for a slot's last frame on the GPU, then releases it. */
	void Retire(FrameSlot& a_slot)
	{
		auto& s = S();
		if (a_slot.signal && s.timeline && *s.timeline && (*s.timeline)->GetCompletedValue() < a_slot.signal) {
			DCLF_ZONE("CS.DCLF.Cpu.SlotWait");
			const auto start = Clock::now();
			(void)(*s.timeline)->HostWait(a_slot.signal);
			++s.window.slotWaits;
			s.window.slotWaitMs += MicrosecondsSince(start) / 1000.0;
		}
		Release(a_slot);
	}

	namespace
	{
		std::unique_ptr<FrameSlot> NewSlot(std::uint32_t a_index)
		{
			auto slot = std::make_unique<FrameSlot>();
			slot->index = a_index;
			slot->recording = SlotRecording(::RenderGraph::Frame::PreparationWorkers() + ::RenderGraph::Frame::FrameCriticalWorkers());  // threads a cull runs on
			return slot;
		}

		/** @brief Render thread. Ends a forward slot's frame. What it drew retires with everything submitted so far. */
		void CloseForwardSlot(FrameSlot& a_slot)
		{
			a_slot.retirePoint = ::RenderGraph::Frame::Retirement::SubmittedPoint();
			a_slot.retireNoted = true;
			if (a_slot.set)
				Draws::BindViewBatches(*a_slot.set, a_slot.retirePoint);
		}

		/** @brief Render thread. Releases forward slots whose GPU work is done, without waiting. */
		void ReleaseForwardSlots()
		{
			auto& s = S();
			const std::uint64_t completed = ::RenderGraph::Frame::Retirement::Clock()->Completed();
			for (auto& slot : s.forwardSlots)
				if (slot->used && slot->retireNoted && completed >= slot->retirePoint)
					Release(*slot);
		}

		/**
		 * @brief Render thread. A forward slot for a GPU-driven frame, reused or new, never a wait. Their indices come after the CPU frames' slots.
		 */
		FrameSlot& TakeForwardSlot(const org::GraphHost& a_host)
		{
			auto& s = S();
			for (auto& slot : s.forwardSlots)
				if (!slot->used)
					return *slot;
			auto slot = NewSlot(a_host.FramesInFlight() + static_cast<std::uint32_t>(s.forwardSlots.size()));
			slot->ownsEpochs = false;
			s.forwardSlots.push_back(std::move(slot));
			return *s.forwardSlots.back();
		}
	}

	bool HandOffRun(SiteRun& a_run, bool a_rowsDone)
	{
		auto& s = S();
		const auto frame = LatestCullFrame();
		if (!a_rowsDone || !frame || frame->sceneFrame != a_run.slot->sceneFrame) {
			++s.window.forwardSkipped;
			return false;
		}
		// Native records the late pass inline during submission on this thread, so the run is handed over directly.
		if (SitesInline()) {
			s.handedOff.store(&a_run, std::memory_order_release);
			return true;
		}
		// Guest hands off in stream order, ahead of the epoch's execution.
		const auto handOff = [](void* a_user, rhi::CommandList&) { S().handedOff.store(static_cast<SiteRun*>(a_user), std::memory_order_release); };
		if (!RenderGraphRuntime::Get().EmitCommands(handOff, &a_run, nullptr, 0, nullptr, 0, false, "DCLF forward draws"))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the forward draws' hand-off could not be emitted");
		return true;
	}

	namespace
	{
		constexpr std::uint32_t kMinimumIndirectDraws = 1u << 15;

		/**
		 * @brief D3D12's instance word signature for a layout (below Shader Model 6.8), made once per layout. A root constant argument names its root signature,
		 * so each layout needs its own. Null when it cannot be made.
		 */
		rhi::CommandSignatureHandle IndirectSignatureFor(rhi::PipelineLayoutHandle a_layout)
		{
			auto& s = S();
			const auto same = [&](const auto& a_entry) { return a_entry.first.index == a_layout.index && a_entry.first.generation == a_layout.generation; };
			if (const auto found = std::find_if(s.layoutSignatures.begin(), s.layoutSignatures.end(), same); found != s.layoutSignatures.end())
				return found->second ? found->second->GetHandle() : rhi::CommandSignatureHandle{};
			rhi::IndirectArg args[2]{};
			args[0].kind = rhi::IndirectArgKind::Constant;
			args[0].u.rootConstants = { 0, kDrawPushInstance, 1 };
			args[1].kind = rhi::IndirectArgKind::DrawIndexed;
			rhi::CommandSignatureDesc desc{};
			desc.args = { args, 2 };
			desc.byteStride = sizeof(IndirectDraw);
			auto& entry = s.layoutSignatures.emplace_back(a_layout, rhi::CommandSignaturePtr{});
			if (s.device.CreateCommandSignature(desc, a_layout, entry.second) != rhi::Result::Ok || !entry.second) {
				entry.second = {};
				logger::error("[DCLF] CPU pipeline: a D3D12 indirect draws' signature could not be made; those runs draw nothing");
				return {};
			}
			return entry.second->GetHandle();
		}

		/** @brief Render thread. Grows the slot's draw buffer to what its last frame used plus slack. None outside Indirect mode. */
		void EnsureDrawBuffer(FrameSlot& a_slot)
		{
			auto& s = S();
			const std::uint32_t reserved = a_slot.drawCursor.exchange(0, std::memory_order_relaxed);
			if (FrameMode().record != RecordMode::Indirect) {
				a_slot.indirect = {};
				a_slot.drawBuffer.reset();
				return;
			}
			if (!s.indirectSignature) {
				rhi::IndirectArg draw{};
				draw.kind = rhi::IndirectArgKind::DrawIndexed;
				rhi::CommandSignatureDesc desc{};
				desc.args = { &draw, 1 };
				desc.byteStride = sizeof(IndirectDraw);
				if (s.device.CreateCommandSignature(desc, {}, s.indirectSignature) != rhi::Result::Ok) {
					logger::error("[DCLF] CPU pipeline: the indirect draws' signature could not be made; its views draw directly");
					return;
				}
			}
			if (a_slot.drawBuffer && reserved <= a_slot.indirect.capacity)
				return;
			const std::uint32_t capacity = std::max(kMinimumIndirectDraws, reserved + reserved / 4);
			a_slot.indirect = {};
			// D3D12 refuses unordered access on an upload-heap buffer. The GPU only reads these records, so make it without.
			const bool unordered = !::RenderGraph::Backend::IsD3D12();
			a_slot.drawBuffer = org::Buffer::CreateShared(rhi::HeapType::HostVisibleCoherent, std::uint64_t(capacity) * sizeof(IndirectDraw), unordered);
			a_slot.drawBuffer->SetName("cs.dclf.cpu.indirect-draws");
			auto resource = a_slot.drawBuffer->GetAPIResource();
			void* mapped = nullptr;
			resource.Map(&mapped);
			const RenderGraph::BufferRef ref = RenderGraph::BufferRefOf(s.device, resource.GetHandle());
			if (!mapped || !ref) {
				logger::error("[DCLF] CPU pipeline: the indirect draws' buffer has no {}; its views draw directly", mapped ? "buffer view ref" : "mapping");
				return;
			}
			// D3D12 below Shader Model 6.8 writes the instance word through a per-layout signature. Else the draws start past it.
			const bool instanceWord = ::RenderGraph::Backend::D3D12InstanceWord();
			a_slot.indirect = { s.indirectSignature->GetHandle(), instanceWord ? &IndirectSignatureFor : nullptr, instanceWord ? 0u : offsetof(IndirectDraw, indexCount), resource.GetHandle(),
				ref, static_cast<IndirectDraw*>(mapped), capacity, &a_slot.drawCursor };
		}
	}

	bool EnsureDevice()
	{
		auto& s = S();
		if (s.deviceSet)
			return s.timeline != nullptr;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return false;
		s.deviceSet = true;
		s.device = host->Device();
		auto timeline = std::make_shared<rhi::TimelinePtr>();
		if (s.device.CreateTimeline(*timeline, 0, "cs.dclf.cpu-frames") != rhi::Result::Ok || !*timeline) {
			logger::error("[DCLF] CPU pipeline: its frame timeline could not be made; CPU frames draw nothing");
			return false;
		}
		s.timeline = timeline;
		::RenderGraph::Frame::Retirement::SetCpuFrameTimeline(std::move(timeline));
		return true;
	}
}

namespace DCLF::Direct
{
	using namespace detail;

	void AdoptSiteRun()
	{
		auto& s = S();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !SitesInline())
			return;
		// A commit with no run clears its lease's entry, so its late pass never finds an older run of the same lease index.
		SiteRun* run = s.pending && s.pending->slot->ownsEpochs ? s.pending : s.handedOff.exchange(nullptr, std::memory_order_acq_rel);
		const org::RunRef key = host->CurrentRun();
		std::scoped_lock lock(s.siteRunsLock);
		if (s.siteRuns.size() <= key.lease)
			s.siteRuns.resize(static_cast<std::size_t>(key.lease) + 1);
		s.siteRuns[key.lease] = { key, run };
	}


	bool CpuFrame()
	{
		return Draws::GraphBuiltForCpu();
	}

	const ViewBatchSet* ForwardBatches()
	{
		const auto* slot = S().current;
		return slot ? slot->set.get() : nullptr;
	}

	void ForwardHandOffCancel()
	{
		if (SitesInline()) {
			S().handedOff.store(nullptr, std::memory_order_release);
			return;
		}
		const auto cancel = [](void*, rhi::CommandList&) { S().handedOff.store(nullptr, std::memory_order_release); };
		if (!RenderGraphRuntime::Get().EmitCommands(cancel, nullptr, nullptr, 0, nullptr, 0, false, "DCLF forward hand-off cancelled"))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the forward draws' hand-off could not be cancelled");
	}

	Mode FrameMode()
	{
		Mode mode = ModeOf(FrameToggles());
		mode.cpu = CpuFrame();
		return mode;
	}

	namespace
	{
		/**
		 * @brief CPU time of the render thread and DXVK's CS thread since the last frame start, for Tracy and the [CPU] report line.
		 * Finding the "dxvk-cs" thread walks every thread and costs tens of ms, so it runs on a helper thread and never in native mode.
		 */
		struct ThreadBusy
		{
			HANDLE render = nullptr, cs = nullptr;
			std::uint64_t renderCycles = 0, csCycles = 0;
			std::uint64_t tsc0 = 0;
			Clock::time_point t0{}, last{};
			// The helper thread owns the handle until the render thread takes it.
			inline static std::atomic<HANDLE> searchResult{ nullptr };
			bool searchStarted = false;

			static HANDLE FindCsThread()
			{
				const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
				if (snapshot == INVALID_HANDLE_VALUE)
					return nullptr;
				HANDLE found = nullptr;
				THREADENTRY32 entry{ sizeof(entry) };
				const DWORD pid = GetCurrentProcessId();
				for (BOOL more = Thread32First(snapshot, &entry); more && !found; more = Thread32Next(snapshot, &entry)) {
					if (entry.th32OwnerProcessID != pid)
						continue;
					const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
					if (!thread)
						continue;
					PWSTR name = nullptr;
					if (SUCCEEDED(GetThreadDescription(thread, &name)) && name && std::wstring_view(name) == L"dxvk-cs")
						found = thread;
					if (name)
						LocalFree(name);
					if (found != thread)
						CloseHandle(thread);
				}
				CloseHandle(snapshot);
				return found;
			}

			/** @brief The frame's sample in microseconds. Zero on the first frame or without a CS thread. */
			void Sample(double& a_renderUs, double& a_csUs, double& a_wallUs)
			{
				a_renderUs = a_csUs = a_wallUs = 0.0;
				const auto now = Clock::now();
				if (!render) {
					render = GetCurrentThread();
					tsc0 = __rdtsc();
					t0 = last = now;
				}
				if (!cs && !std::exchange(searchStarted, true) && !RenderGraph::Engine::Native::Active()) {
					// DXVK's device exists before the first frame, so the first try normally finds it.
					std::thread([] {
						for (int i = 0; i < 30; ++i) {
							if (const HANDLE h = FindCsThread()) {
								searchResult.store(h, std::memory_order_release);
								return;
							}
							std::this_thread::sleep_for(std::chrono::seconds(2));
						}
					}).detach();
				}
				if (!cs)
					cs = searchResult.load(std::memory_order_acquire);
				const double us = std::chrono::duration<double, std::micro>(now - t0).count();
				const double perUs = us > 1e5 ? static_cast<double>(__rdtsc() - tsc0) / us : 0.0;
				ULONG64 r = 0, c = 0;
				QueryThreadCycleTime(render, &r);
				if (cs)
					QueryThreadCycleTime(cs, &c);
				if (perUs > 0.0) {
					a_renderUs = static_cast<double>(r - renderCycles) / perUs;
					a_csUs = cs && csCycles ? static_cast<double>(c - csCycles) / perUs : 0.0;
					a_wallUs = std::chrono::duration<double, std::micro>(now - last).count();
				}
				renderCycles = r;
				csCycles = c;
				last = now;
			}
		};
	}

	void FrameStart(std::shared_ptr<const ViewBatchSet> a_set)
	{
		DCLF_ZONE("CS.DCLF.Cpu.FrameStart");
		auto& s = S();
		static ThreadBusy busy;
		double renderUs = 0.0, csUs = 0.0, wallUs = 0.0;
		busy.Sample(renderUs, csUs, wallUs);
		TracyPlot("CS.DCLF.Busy.Render", renderUs);
		TracyPlot("CS.DCLF.Busy.Cs", csUs);
		if (s.wasCpu && wallUs > 0.0) {
			s.window.renderBusyUs += renderUs;
			s.window.csBusyUs += csUs;
			s.window.wallUs += wallUs;
		}
		{
			DCLF_ZONE("CS.DCLF.Cpu.FrameStart.PollRetirement");
			::RenderGraph::Frame::Retirement::Poll();
		}
		// Release forward slots every frame. One left behind during CPU frames would hold the tables and their retirement node.
		{
			DCLF_ZONE("CS.DCLF.Cpu.FrameStart.ForwardSlots");
			if (s.current && !s.current->ownsEpochs) {
				CloseForwardSlot(*s.current);
				s.current = nullptr;
			}
			ReleaseForwardSlots();
		}
		auto* host = RenderGraphRuntime::Get().Host();
		bool cpu = false;
		{
			DCLF_ZONE("CS.DCLF.Cpu.FrameStart.EnsureDevice");
			cpu = CpuFrame() && host && EnsureDevice();
		}
		if (!cpu) {
			// After CPU frames, the retirement points and latch regions are the host's own again.
			if (std::exchange(s.wasCpu, false)) {
				::RenderGraph::Frame::Retirement::NoteCpuFrameOpened(s.signalled);
				SubmitQueued();
			}
			{
				DCLF_ZONE("CS.DCLF.Cpu.FrameStart.ReleaseCompleted");
				ReleaseCompletedSlots();
			}
			// Read-only fade states. FadeStateCS stays the authority on fades.
			std::shared_ptr<const ViewBatchSet> forward;
			if (host && EnsureDevice() && a_set && a_set->forwardOnly) {
				forward = std::move(a_set);
				auto& slot = TakeForwardSlot(*host);
				slot.used = true;
				slot.set = forward;
				slot.sceneFrame = Scene::FrameView::Get().SceneStep();
				s.current = &slot;
			}
			{
				DCLF_ZONE("CS.DCLF.Cpu.FrameStart.SetViewBatches");
				FrameValues::Get().SetViewBatches(forward);
			}
			{
				DCLF_ZONE("CS.DCLF.Cpu.FrameStart.PublishViewBatches");
				Draws::PublishViewBatches(forward.get());
			}
			{
				DCLF_ZONE("CS.DCLF.Cpu.FrameStart.KickCpuFrameState");
				KickCpuFrameState(std::move(forward), true);
			}
			return;
		}
		s.wasCpu = true;
		// From here, what this frame binds retires after its own signal. A frame whose signal was not queued leaves the value to the next.
		::RenderGraph::Frame::Retirement::NoteCpuFrameOpened(s.signalled + 1);
		// One slot per frame in flight. Reusing a slot waits for its last frame's signal.
		const std::uint32_t slots = host->FramesInFlight();
		while (s.slots.size() < slots)
			s.slots.push_back(NewSlot(static_cast<std::uint32_t>(s.slots.size())));
		auto& slot = *s.slots[s.frames++ % slots];
		Retire(slot);
		EnsureDrawBuffer(slot);
		slot.used = true;
		slot.set = a_set;
		slot.sceneFrame = Scene::FrameView::Get().SceneStep();
		slot.colourGate.store(2, std::memory_order_relaxed);
		s.signalQueuedFor = nullptr;
		s.current = &slot;
		// Latch regions go in an external run of the host's, retired after this frame's signal.
		(void)host->BeginExternalRun(s.timeline, s.signalled + 1);
		// These must run in this order, before FrameValues::Kick.
		FrameValues::Get().SetViewBatches(a_set);
		Draws::PublishViewBatches(a_set.get());
		KickCpuFrameState(std::move(a_set));
	}

	void ReleaseCompletedSlots()
	{
		auto& s = S();
		const std::uint64_t completed = s.timeline && *s.timeline ? (*s.timeline)->GetCompletedValue() : ~std::uint64_t{ 0 };
		for (auto& slot : s.slots)
			if (slot->used && slot.get() != s.current && slot->signal <= completed)
				Release(*slot);
		ReleaseForwardSlots();
	}

	void SubmitQueued(bool a_always)
	{
		auto& s = S();
		const auto* host = RenderGraphRuntime::Get().Host();
		const std::uint64_t made = host ? host->SubmissionsMade() : 0;
		if (!a_always && s.submitted >= s.signalled && s.hostSubmitted >= made)
			return;
		DCLF_ZONE("CS.DCLF.SubmitQueued");
		globals::d3d::context->Flush();
		s.submitted = s.signalled;
		s.hostSubmitted = made;
	}

	const ViewBatchSet* FrameBatches()
	{
		const auto* slot = S().current;
		return slot && slot->ownsEpochs ? slot->set.get() : nullptr;
	}

	void QueueFrameSignal()
	{
		auto& s = S();
		auto* slot = s.current;
		if (!slot || !slot->ownsEpochs || std::exchange(s.signalQueuedFor, slot) == slot)
			return;
		// Queue the frame signal before Present so it reaches the device with this frame's work. Queued later it would ride the next
		// frame's list, and a wait for it would also wait for that frame's work.
		s.submitted = s.signalled;
		const std::uint64_t value = s.signalled + 1;
		// Guest uses a signal-only external command. Native submits its own list after the frame's site epochs.
		const rhi::TimelinePoint signal{ (*s.timeline)->GetHandle(), value };
		if (!RenderGraphRuntime::Get().EmitCommands(nullptr, nullptr, nullptr, 0, &signal, 1, false, "DCLF CPU frame"))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the CPU pipeline's frame signal could not be emitted");
		s.signalled = value;
		slot->signal = value;
		::RenderGraph::Frame::Retirement::NoteCpuFrameSignalled(value);
	}

	void EndFrame()
	{
		DCLF_ZONE("CS.DCLF.Cpu.EndFrame");
		auto& s = S();
		QueueFrameSignal();
		s.signalQueuedFor = nullptr;
		auto* slot = std::exchange(s.current, nullptr);
		if (!slot)
			return;
		if (!slot->ownsEpochs) {
			CloseForwardSlot(*slot);
			++s.window.frames;
			return;
		}
		if (slot->set)
			Draws::BindViewBatches(*slot->set, ::RenderGraph::Frame::Retirement::SubmittedPoint());
		if (auto* host = RenderGraphRuntime::Get().Host())
			host->EndExternalRun();
		++s.window.frames;
	}

	namespace
	{
		/**
		 * @brief Native CPU frame site epoch. The graph submits it, and its late pass records the run's draws inline on this thread
		 * during the same submission. ORG holds a_owner until the execution retires.
		 */
		bool ExecuteInlineEpoch(RenderGraphRuntime::Segment a_segment, const std::function<void()>& a_body, std::shared_ptr<const void> a_owner)
		{
			auto& s = S();
			bool executed = true;
			if (Draws::CpuPipelineSegmentRecords(a_segment)) {
				try {
					executed = RenderGraphRuntime::Get().ExecuteEpoch(a_segment, [&](org::RenderGraph&) {
						a_body();
						AdoptSiteRun();
					}, std::move(a_owner));
				} catch (const std::exception& e) {
					executed = false;
					if (!std::exchange(s.loggedExternal, true))
						logger::error("[DCLF] CPU pipeline: the {} site's ORG epoch was not submitted ({}): its draws are left out", static_cast<std::uint32_t>(a_segment), e.what());
				}
			} else {
				// No CPU pipeline passes, so nothing to submit and no late pass.
				auto* graph = RenderGraphRuntime::Get().Host()->Graph();
				org::runtime::ScopedActiveGraphServices services(graph->GetUploadService(), graph->GetDescriptorService());
				a_body();
				s.current->holds.push_back(std::move(a_owner));
			}
			auto* run = std::exchange(s.pending, nullptr);
			if (!run)
				return executed;
			// With a submission thread the late pass may not have recorded yet. Only a synchronous submission can tell.
			const auto* host = RenderGraphRuntime::Get().Host();
			const bool recordedBySubmission = !host || !host->SubmissionThreadOn();
			if (recordedBySubmission && executed && run->valid && !run->recordedDraws && !std::exchange(s.loggedNoLatePass, true))
				logger::error("[DCLF] CPU pipeline: the {} site's epoch was submitted without recording its late pass (no draws pass in its graph): it draws nothing",
					static_cast<std::uint32_t>(run->site));
			// A submitted run's late pass may still be reading it on the submission thread.
			if (!executed)
				run->valid = false;
			return executed;
		}
	}

	bool ExecuteEpoch(RenderGraphRuntime::Segment a_segment, const std::function<void()>& a_body, std::shared_ptr<const void> a_owner)
	{
		auto& s = S();
		if (!CpuFrame() || !s.current || !s.current->ownsEpochs)
			return RenderGraphRuntime::Get().ExecuteEpoch(a_segment, [&](org::RenderGraph&) {
				a_body();
				AdoptSiteRun();
			}, std::move(a_owner));
		s.pending = nullptr;
		auto* host = RenderGraphRuntime::Get().Host();
		auto* graph = host ? host->Graph() : nullptr;
		if (!graph)
			return false;
		const auto siteZoneName = fmt::format("DCLF site {}", static_cast<std::uint32_t>(a_segment));
		TracyD3D11ZoneTransientS(globals::state->tracyCtx, siteZone, siteZoneName.c_str(), 0, globals::state->tracyCtx != nullptr);
		// The colour commit writes the main latch in its own external run. The depth commit's run is kept for the depth site's readers.
		if (a_segment == RenderGraphRuntime::Segment::MainOpaque)
			(void)host->BeginExternalRun(S().timeline, S().signalled + 1);
		if (SitesInline())
			return ExecuteInlineEpoch(a_segment, a_body, std::move(a_owner));
		std::shared_ptr<org::ExternalEpochExecution> execution;
		bool executed = true;
		if (Draws::CpuPipelineSegmentRecords(a_segment)) {
			// External epoch. The ticket is submitted with the body as its commit, and the site's callback runs the execution ahead of its draws.
			try {
				execution = RenderGraphRuntime::Get().ExecuteExternalEpoch(a_segment, [&](org::RenderGraph&) { a_body(); }, a_owner);
			} catch (const std::exception& e) {
				executed = false;
				if (!std::exchange(s.loggedExternal, true))
					logger::error("[DCLF] CPU pipeline: the {} site's ORG epoch was not submitted ({}): its draws are left out", static_cast<std::uint32_t>(a_segment), e.what());
			}
		} else {
			// Commits stage uploads through the graph's services, so they must be active here as in ORG's epochs.
			org::runtime::ScopedActiveGraphServices services(graph->GetUploadService(), graph->GetDescriptorService());
			a_body();
		}
		s.current->holds.push_back(std::move(a_owner));
		if (auto* run = std::exchange(s.pending, nullptr)) {
			const bool hasExecution = execution != nullptr;
			run->execution = std::move(execution);
			// The epoch's passes failed, so its draws would read stale data. Drain the queues and draw nothing.
			run->valid = run->valid && executed;
			// Emit the stream's wait for the epoch's compute work now. Left to the next epoch, it landed in the list holding those segments,
			// and the GPU waited for itself.
			Emit(*run);
			RenderGraphRuntime::Get().CloseExternalEpoch(hasExecution);
			return true;
		}
		if (execution)
			RenderGraphRuntime::Get().SignalExecutionUnexecuted(*execution, execution->ExecutedSegments(), "DCLF site epoch without a run");
		RenderGraphRuntime::Get().CloseExternalEpoch(false);
		return executed;
	}

	void DepthCommitted(const MainCommitInputs& a_inputs)
	{
		DCLF_ZONE("CS.DCLF.Cpu.DepthCommitted");
		auto& s = S();
		auto* slot = s.current;
		if (!slot || !slot->set || !a_inputs.shape || !a_inputs.resources || !a_inputs.latch)
			return;
		TakePoolFirsts(*slot, a_inputs.resources->pool.get());
		// A GPU-driven frame has no depth site. Only first person's forward part is culled with this commit.
		if (!slot->ownsEpochs) {
			if (!slot->viewState)
				return;
			auto& run = NewRun(*slot, Site::Depth, a_inputs);
			Draws::RevisionShapes shapes;
			AttachHostedViews(*slot, run, shapes);
			OnCullFrame(*slot, [slot, run = &run](const std::shared_ptr<const CullFrame>& a_frame) {
				run->cullFrame = a_frame;
				DispatchSiteCulls(*slot, [slot, run, a_frame] {
					for (auto* view : run->views)
						DispatchCulls(*slot, *run, *view, a_frame);
				});
			});
			return;
		}
		auto& run = NewRun(*slot, Site::Depth, a_inputs);
		Draws::RevisionShapes shapes;
		shapes.main[Draws::kDepthShape] = a_inputs.shape;
		shapes.depthWithoutIndexPool = a_inputs.poolCopies == 0;
		AttachHostedViews(*slot, run, shapes);
		auto& main = NewView(*slot, run, ViewType::Main, MainCullView(*a_inputs.latch), true);
		run.main = &main;
		slot->mainDepthCount = main.list->size();
		slot->mainDepth = std::make_unique<std::atomic<const CulledBatch*>[]>(slot->mainDepthCount);
		slot->mainDepthPending.store(static_cast<std::uint32_t>(slot->mainDepthCount), std::memory_order_relaxed);
		slot->depthRan = true;
		if (!slot->mainDepthCount)
			ArriveColour(*slot);
		OnCullFrame(*slot, [slot, run = &run](const std::shared_ptr<const CullFrame>& a_frame) {
			run->cullFrame = a_frame;
			DispatchSiteCulls(*slot, [slot, run, a_frame] {
				for (auto* view : run->views)
					DispatchCulls(*slot, *run, *view, a_frame);
			});
		});
		SetPending(run);
	}

	void ColourCommitted(const MainCommitInputs& a_inputs)
	{
		DCLF_ZONE("CS.DCLF.Cpu.ColourCommitted");
		auto& s = S();
		auto* slot = s.current;
		if (!slot || !slot->set || !a_inputs.shape || !a_inputs.resources || !a_inputs.latch)
			return;
		TakePoolFirsts(*slot, a_inputs.resources->pool.get());
		SiteRun* run = slot->ownsEpochs ? &NewRun(*slot, Site::Colour, a_inputs) : nullptr;
		const auto& latch = *a_inputs.latch;
		// Copied, because the slot must hold the latched cascades and volumes for as long as the culls run.
		auto cascades = std::make_shared<const std::vector<Draws::GpuCascade>>(a_inputs.cascades.begin(), a_inputs.cascades.end());
		auto volumes = std::make_shared<const std::vector<Draws::GpuShadowVolume>>(a_inputs.volumes.begin(), a_inputs.volumes.end());
		auto strict = a_inputs.resources->scene ? FrameStrictGates(*a_inputs.resources->scene, slot->sceneFrame) : nullptr;
		const auto colourView = [&](ViewType a_type) {
			auto view = ColourCullView(a_type, latch, *cascades, *volumes);
			view.strict = strict.get();
			return view;
		};
		if (strict)
			slot->holds.push_back(strict);
		slot->holds.push_back(cascades);
		slot->holds.push_back(volumes);
		if (run) {
			auto& main = NewView(*slot, *run, ViewType::Main, colourView(ViewType::Main), false);
			main.colourWords = true;
			main.mainView = MainCullView(latch);
			run->main = &main;
			if (!slot->set->List(ViewType::Decal).empty())
				run->decal = &NewView(*slot, *run, ViewType::Decal, colourView(ViewType::Decal), false);
		}
		// The forward epoch shares the colour shape. Its callback is queued at the forward trigger.
		auto& forward = NewRun(*slot, Site::Forward, a_inputs);
		forward.forward = &NewView(*slot, forward, ViewType::Forward, colourView(ViewType::Forward), false);
		forward.refractionView = &NewView(*slot, forward, ViewType::Refraction, colourView(ViewType::Refraction), false);
		slot->forwardRun = &forward;
		// Main colour culls wait for the depth site's batches and this frame's cull frame.
		if (run)
			slot->colourDispatch = [slot, run] { DispatchCulls(*slot, *run, *run->main, run->cullFrame); };
		OnCullFrame(*slot, [slot, run, forward = &forward](const std::shared_ptr<const CullFrame>& a_frame) {
			forward->cullFrame = a_frame;
			if (run)
				run->cullFrame = a_frame;
			DispatchSiteCulls(*slot, [slot, run, forward, a_frame] {
				if (run && run->decal)
					DispatchCulls(*slot, *run, *run->decal, a_frame);
				DispatchCulls(*slot, *forward, *forward->forward, a_frame);
				DispatchCulls(*slot, *forward, *forward->refractionView, a_frame);
				if (run)
					ArriveColour(*slot);
			});
		});
		if (!run)
			return;
		// No depth site this frame, so the colour culls cull the main list themselves.
		if (!slot->depthRan)
			ArriveColour(*slot);
		s.pending = run;
	}

	bool ForwardReached(bool a_refraction)
	{
		auto& s = S();
		auto* slot = s.current;
		auto* run = slot ? std::exchange(slot->forwardRun, nullptr) : nullptr;
		if (!run)
			return false;
		// Rows for the forward sort on the CS thread. The producer changes them at the next Kick. A frame whose producer is not done waits here.
		const auto* rows = FrameValues::Get().RowsIfDone();
		if (!rows) {
			DCLF_ZONE("CS.DCLF.Cpu.RowsWait");
			++s.window.rowWaits;
			rows = FrameValues::Get().RowsIfDone(true);
		}
		if (rows) {
			if (slot->forwardRows.size() < rows->size())
				slot->forwardRows.resize(rows->size());
			for (const ViewType type : { ViewType::Forward, ViewType::Refraction })
				for (const auto& batch : slot->set->List(type))
					for (std::uint32_t lane = 0; lane < batch.count; ++lane)
						if (const std::uint32_t object = batch.object[lane]; object < rows->size())
							slot->forwardRows[object] = (*rows)[object];
			run->bindings.placements = slot->forwardRows;
			run->refractionBindings.placements = slot->forwardRows;
		}
		run->bindings.viewProj = run->forward->view.viewProj;
		run->refractionBindings.viewProj = run->refractionView->view.viewProj;
		run->refraction = a_refraction;
		if (slot->ownsEpochs) {
			s.pending = run;
			return true;
		}
		return HandOffRun(*run, rows != nullptr);
	}

	WindowCounters TakeWindowCounters()
	{
		return std::exchange(S().window, {});
	}
}
