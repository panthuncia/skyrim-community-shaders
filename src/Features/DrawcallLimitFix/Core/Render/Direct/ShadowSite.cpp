// The CPU-driven pipeline's shadow site. It draws the shadow views and occlusion maps at shadow-maps.after, as the GPU
// path's shadows.view pass does. Each view's casters go into its map's slice, which is loaded and stored because the
// engine's casters are already in it. Bias and cull state come from each lane's pipeline.

// Main/Resources.h goes first because volk must come before every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/ShadowSite.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/SiteFrame.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "RenderGraph/Backend.h"
#include "RenderGraph/BufferRefs.h"

#include <Resources/Buffers/Buffer.h>
#include <Resources/ExternalTextureResource.h>
#include <rhi.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <deque>
#include <memory>
#include <source_location>
#include <vector>
#include <Tracy/Tracy.hpp>
#include <Tracy/TracyC.h>

namespace DCLF::Direct::detail
{
	namespace
	{
		/**
		 * @brief A DSV per slice of each imported shadow map, in DCLF's own heap. Made once per import set, and the old ones
		 * are kept alive by the frame slot that replaced them.
		 */
		struct ShadowTargets
		{
			// Held as long as the views and used as their key, since an import's address can be reused by the next one.
			std::array<std::shared_ptr<org::GloballyIndexedResource>, Draws::kShadowDepthTargets> depth{};
			std::array<std::uint32_t, Draws::kShadowDepthTargets> layers{};
			std::array<std::uint32_t, Draws::kShadowDepthTargets> first{};  // its slice 0's DSV in the heap
			std::array<rhi::Format, Draws::kShadowDepthTargets> formats{};
			std::array<rhi::ResourceHandle, Draws::kShadowDepthTargets> resources{};
			std::array<bool, Draws::kShadowDepthTargets> valid{};
			rhi::DescriptorHeapPtr dsvHeap;
		};

		/** @brief One view's rendering: its map's slice and its viewport there. */
		struct ShadowPass
		{
			SiteView* view = nullptr;
			rhi::DescriptorSlot dsv{};
			rhi::ResourceHandle resource{};
			std::uint32_t x = 0, y = 0, width = 0, height = 0;
			float minDepth = 0.0f, maxDepth = 1.0f;
			bool occlusion = false;
		};

		/** @brief A shadow run's own state, released with the run. */
		struct ShadowState
		{
			std::shared_ptr<const Draws::ShadowFrame> shape;  // the bindings point into it
			std::shared_ptr<const Draws::ShadowResources> resources;
			std::shared_ptr<const ShadowTargets> targets;
			std::vector<Draws::SunEntryProcess> sunEntry;  // the sun views' full-frustum processes
			std::shared_ptr<const StrictGates> strict;  // null for none
			// Per view, never moved, because the bindings and site views point into them.
			std::deque<std::vector<std::uint32_t>> mapRows;
			std::deque<RecordBindings> bindings;
			std::vector<ShadowPass> passes;  // in the latch's order, placement views then occlusion maps
		};

		/** @brief The site's own state across frames. Render thread. */
		struct ShadowSiteState
		{
			std::shared_ptr<const ShadowTargets> targets;
			bool loggedTargets = false;
		};

		ShadowSiteState& Own()
		{
			static ShadowSiteState state;
			return state;
		}

		/** @brief Render thread: the shadow maps' DSVs, made once per import set. */
		std::shared_ptr<const ShadowTargets> ShadowTargetsOf(FrameSlot& a_slot, const Draws::ShadowResources& a_resources)
		{
			auto& own = Own();
			// A new version of an engine target's slot is a new image, so it gets new views.
			std::array<std::shared_ptr<org::GloballyIndexedResource>, Draws::kShadowDepthTargets> depths{};
			for (std::size_t t = 0; t < depths.size(); ++t)
				depths[t] = a_resources.depth[t].Current();
			if (own.targets && own.targets->depth == depths && own.targets->layers == a_resources.depthLayers)
				return own.targets;
			if (own.targets)
				a_slot.holds.push_back(std::move(own.targets));
			auto& device = S().device;
			auto targets = std::make_shared<ShadowTargets>();
			targets->depth = depths;
			targets->layers = a_resources.depthLayers;
			std::uint32_t dsvs = 0;
			for (std::uint32_t t = 0; t < Draws::kShadowDepthTargets; ++t)
				if (targets->depth[t]) {
					targets->first[t] = dsvs;
					dsvs += std::max(1u, targets->layers[t]);
				}
			bool made = dsvs == 0 ||
			            device.CreateDescriptorHeap({ rhi::DescriptorHeapType::DSV, dsvs, false, "cs.dclf.direct.shadow-dsv" }, targets->dsvHeap) == rhi::Result::Ok;
			for (std::uint32_t t = 0; made && t < Draws::kShadowDepthTargets; ++t) {
				const auto& depth = targets->depth[t];
				if (!depth)
					continue;
				targets->formats[t] = a_resources.depth[t].Format();
				targets->resources[t] = depth->GetAPIResource().GetHandle();
				// A DSV per array layer, matching the graph's declared views.
				const std::uint32_t layers = std::max(1u, targets->layers[t]);
				bool valid = true;
				for (std::uint32_t slice = 0; valid && slice < layers; ++slice) {
					rhi::DsvDesc desc{};
					desc.dimension = targets->layers[t] > 1 ? rhi::DsvDim::Texture2DArray : rhi::DsvDim::Texture2D;
					desc.range = { 0, 1, targets->layers[t] > 1 ? slice : 0u, 1 };
					desc.formatOverride = targets->formats[t];
					valid = device.CreateDepthStencilView(rhi::DescriptorSlot(targets->dsvHeap->GetHandle(), targets->first[t] + slice), targets->resources[t], desc) ==
					        rhi::Result::Ok;
				}
				targets->valid[t] = valid;
				made = made && valid;
			}
			if (!made && !std::exchange(own.loggedTargets, true))
				logger::error("[DCLF] CPU pipeline: the shadow maps' views could not be made; the shadow site draws no view into a map without one");
			own.targets = targets;
			return own.targets;
		}

		/**
		 * @brief A view's map row: each key slot's pipeline under the view's mode and rasterizer state. ~0u for a pipeline past the
		 * published set, whose draws are skipped as the GPU path's buckets leave them out.
		 */
		void MapRowOf(const Draws::RowBuckets& a_row, std::vector<std::uint32_t>& a_out)
		{
			a_out.assign(a_row.bucketOfSlot.size(), ~0u);
			for (std::size_t k = 0; k < a_row.bucketOfSlot.size(); ++k)
				if (const std::uint32_t bucket = a_row.bucketOfSlot[k]; bucket < a_row.pipelines.size())
					a_out[k] = a_row.pipelines[bucket];
		}

		bool IsOcclusionView(ViewType a_type) { return a_type == ViewType::SkyOccluder || a_type == ViewType::PrecipOccluder; }
	}

	void RecordShadowSite(SiteOut& a_out, SiteRun& a_run)
	{
		const auto* state = static_cast<const ShadowState*>(a_run.shadowState.get());
		if (!state)
			return;
		// Each view gets its own rendering in the latch's order, its slice loaded and stored.
		for (const auto& pass : state->passes) {
			rhi::DepthAttachment depth{};
			depth.dsv = pass.dsv;
			depth.depthLoad = rhi::LoadOp::Load;
			depth.depthStore = rhi::StoreOp::Store;
			depth.stencilLoad = rhi::LoadOp::Load;
			depth.stencilStore = rhi::StoreOp::Store;
			depth.resource = pass.resource;
			rhi::PassBeginInfo begin{};
			begin.depth = &depth;
			begin.x = pass.x;
			begin.y = pass.y;
			begin.width = pass.width;
			begin.height = pass.height;
			begin.minDepth = pass.minDepth;
			begin.maxDepth = pass.maxDepth;
			begin.debugName = pass.occlusion ? "DCLF direct occlusion map" : "DCLF direct shadow view";
			RecordViewPass(a_out, a_run, *pass.view, begin, *pass.view->bindings);
		}
	}
}

namespace DCLF::Direct
{
	using namespace detail;

	void ShadowCommitted(const ShadowCommitInputs& a_inputs)
	{
		DCLF_ZONE("CS.DCLF.Cpu.ShadowCommitted");
		auto& s = S();
		auto* slot = s.current;
		if (!slot || !slot->ownsEpochs || !slot->set || !a_inputs.shape || !a_inputs.resources || !a_inputs.adopted)
			return;
		const auto& shape = *a_inputs.shape;
		const auto& resources = *a_inputs.resources;
		// Every site of the frame draws with the pool's first indices from the frame's first drawing commit.
		TakePoolFirsts(*slot, resources.pool.get());
		TracyCZoneN(sState, "CS.DCLF.Cpu.Shadow.State", true);
		auto& run = NewBareRun(*slot, Site::Shadow);
		auto state = std::make_shared<ShadowState>();
		state->shape = a_inputs.shape;
		state->resources = a_inputs.resources;
		state->targets = ShadowTargetsOf(*slot, resources);
		state->sunEntry.assign(a_inputs.sunEntry.begin(), a_inputs.sunEntry.end());
		if (resources.scene)
			state->strict = FrameStrictGates(*resources.scene, slot->sceneFrame);
		run.shadowState = state;
		// What every view's recordings bind: the shape's heaps and shadow set, the index pool and the slot's first indices.
		RecordBindings common{};
		common.resourceHeap = shape.resourceHeap;
		common.samplerHeap = shape.samplerHeap;
		common.shadow = &shape.indirect;
		// The callback binds the run's own heaps in DXVK's buffer.
		run.bindings.resourceHeap = shape.resourceHeap;
		run.bindings.samplerHeap = shape.samplerHeap;
		if (resources.pool)
			if (auto version = a_inputs.adopted->Find(*resources.pool->indices); version && version->buffer) {
				common.indexPool = version->buffer->GetAPIResource().GetHandle();
				slot->holds.push_back(std::move(version));
			}
		common.poolFirsts = slot->poolFirsts;
		const auto& targets = *state->targets;
		bool drawable = shape.indirect.valid && common.indexPool.valid() && shape.rows;
		// D3D12's layout holds root constants only, so a view's push words name its blocks by ref and by view.
		// Vulkan's layout reads the shape's addresses.
		const bool d3d12 = ::RenderGraph::Backend::IsD3D12();
		RenderGraph::BufferRef viewBlocksRef{};
		if (d3d12 && drawable) {
			if (resources.viewBlocks.buffer)
				if (auto version = a_inputs.adopted->Find(*resources.viewBlocks.buffer); version && version->buffer) {
					viewBlocksRef = RenderGraph::MakeBufferRef(s.device, version->buffer->GetAPIResource().GetHandle(), 0, std::source_location::current());
					slot->holds.push_back(std::move(version));
				}
			if (!viewBlocksRef || !resources.constantsRef || !resources.blockViews) {
				drawable = false;
				static std::atomic<bool> warned{ false };
				if (!warned.exchange(true))
					logger::error("[DCLF] D3D12 CPU shadow site: no buffer view ref of the view blocks or the constants, or no SharedData view; its views draw nothing");
			}
		}
		TracyCZoneEnd(sState);
		for (const auto& latched : a_inputs.views) {
			if (!drawable || latched.index >= shape.views.size() || latched.index >= shape.rows->size())
				continue;
			const auto& view = shape.views[latched.index];
			// A target grown since the shape was made has slices it cannot name.
			if (view.target >= Draws::kShadowDepthTargets || !targets.depth[view.target] || !targets.valid[view.target] ||
				view.slice >= std::max(1u, targets.layers[view.target]))
				continue;
			if (slot->set->List(latched.type).empty())
				continue;
			auto& mapRow = state->mapRows.emplace_back();
			MapRowOf((*shape.rows)[latched.index], mapRow);
			auto& bindings = state->bindings.emplace_back(common);
			// The view's own push words, so its recording stays valid as long as the shape.
			static_assert(std::tuple_size_v<decltype(view.push)> == std::tuple_size_v<decltype(bindings.shadowPush)>);
			std::copy(view.push.begin(), view.push.end(), bindings.shadowPush.begin());
			if (d3d12) {
				auto& push = bindings.shadowPush;
				const auto ref = [&](std::uint32_t a_word, RenderGraph::BufferRef a_ref) {
					push[a_word] = a_ref.viewSlot;
					push[a_word + 1] = a_ref.byteOffset;
				};
				const RenderGraph::BufferRef viewBlock = viewBlocksRef + std::uint64_t(view.slot) * Draws::kShadowViewSlotBytes;
				ref(kShadowPushViewBlock, viewBlock);
				ref(kShadowPushPerFrame, viewBlock + Draws::kShadowPerFrameOffset);
				ref(kShadowPushZeros, resources.constantsRef + Draws::kShadowArenaBlocksOffset);  // the zero block heads the arena
				push[kShadowPushSharedData] = resources.blockViewIndices[0];
				push[kShadowPushSharedData + 1] = 0;
				push[kShadowPushFeatureData] = resources.blockViewIndices[1];
				push[kShadowPushFeatureData + 1] = 0;
			}
			bindings.shadowMapRow = mapRow;
			const bool occlusion = IsOcclusionView(latched.type);
			CullView cull = occlusion ? OcclusionCullView(latched.type, latched.slot, latched.latch) :
			                            ShadowCullView(latched.type, latched.slot, latched.latch, state->sunEntry);
			cull.strict = state->strict.get();
			auto& site = NewView(*slot, run, latched.type, cull, false);
			site.bindings = &bindings;
			ShadowPass pass{};
			pass.view = &site;
			pass.dsv = rhi::DescriptorSlot(targets.dsvHeap->GetHandle(), targets.first[view.target] + (targets.layers[view.target] > 1 ? view.slice : 0u));
			pass.resource = targets.resources[view.target];
			pass.x = view.x;
			pass.y = view.y;
			pass.width = view.width;
			pass.height = view.height;
			pass.minDepth = view.minDepth;
			pass.maxDepth = view.maxDepth;
			pass.occlusion = occlusion;
			state->passes.push_back(pass);
		}
		DCLF_ZONE_NAMED(sCull, "CS.DCLF.Cpu.Shadow.OnCullFrame", true);
		OnCullFrame(*slot, [slot, run = &run](const std::shared_ptr<const CullFrame>& a_frame) {
			run->cullFrame = a_frame;
			DispatchSiteCulls(*slot, [slot, run, a_frame] {
				for (auto* view : run->views)
					DispatchCulls(*slot, *run, *view, a_frame);
			});
		});
		SetPending(run);
	}
}
