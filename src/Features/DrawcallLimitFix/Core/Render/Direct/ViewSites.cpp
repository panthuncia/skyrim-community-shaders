// The CPU-driven pipeline's view sites: the depth site's hosted views (the reflection faces and first-person depth) and
// the first-person colour site.
//
// Reflection faces draw into the cube's slice, which is loaded because the engine's sky is there, with a depth of the
// site's own that each face clears. The face's tree LOD is a rendering of its own after the face's draws.
// First-person depth draws into the main depth after main depth. First-person colour draws its buckets into kMAIN and
// the motion vectors against kPOST_ZPREPASS_COPY, then the forward range back to front onto every main target.
// Lanes bind pipelines as the view's shape has them, so a lane the GPU map sends to no bucket draws nothing here either.

// Main/Resources.h goes first because volk must come before every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/SiteFrame.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/ViewSites.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"

#include <Resources/ExternalTextureResource.h>
#include <Resources/PixelBuffer.h>
#include <rhi.h>

#include <algorithm>
#include <array>
#include <deque>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>
#include <Tracy/Tracy.hpp>

namespace DCLF::Direct::detail
{
	namespace
	{
		using RA = rhi::ResourceAccessType;
		using RS = rhi::ResourceSyncState;

		constexpr auto Access(std::initializer_list<rhi::ResourceAccessType> a_types)
		{
			std::uint64_t bits = 0;
			for (const auto type : a_types)
				bits |= static_cast<std::uint64_t>(type);
			return static_cast<rhi::ResourceAccessType>(bits);
		}
		constexpr auto Sync(std::initializer_list<rhi::ResourceSyncState> a_states)
		{
			std::uint32_t bits = 0;
			for (const auto state : a_states)
				bits |= static_cast<std::uint32_t>(state);
			return static_cast<rhi::ResourceSyncState>(bits);
		}

		// Between two renderings of the same attachments, the first's colour and depth writes against the second's tests and writes.
		constexpr rhi::GlobalBarrier kAttachments{ Sync({ RS::RenderTarget, RS::DepthStencil }), Sync({ RS::RenderTarget, RS::DepthStencil }),
			Access({ RA::RenderTarget, RA::DepthReadWrite }), Access({ RA::RenderTarget, RA::DepthReadWrite }) };

		void Barrier(rhi::CommandList& a_list, const rhi::GlobalBarrier& a_barrier)
		{
			const rhi::BarrierBatch batch{ {}, {}, { &a_barrier, 1 } };
			a_list.Barriers(batch);
		}

		/**
		 * @brief A hosted view's own targets. Made once per target, and the old ones are kept alive by the frame slot that
		 * replaced them.
		 */
		struct ViewTargets
		{
			std::shared_ptr<org::GloballyIndexedResource> target;  // held because the views are of it
			rhi::ResourcePtr depth;  // the reflection faces' own depth
			rhi::DescriptorHeapPtr rtvHeap, dsvHeap;
			std::array<rhi::DescriptorSlot, Draws::kMaxViewFaces> colours{};
			rhi::DescriptorSlot dsv{};
			rhi::ResourceHandle targetResource{}, depthResource{};
			rhi::Format colourFormat = rhi::Format::Unknown, depthFormat = rhi::Format::Unknown;
			std::uint32_t faces = 0, width = 0, height = 0;
			bool valid = false;
			std::shared_ptr<const void> keep;  // the targets a rebound copy replaced, with their heaps
		};

		/** @brief One face of a hosted view: its cull's view, its push words and its view of the depth run. */
		struct HostedFace
		{
			std::uint32_t face = 0;
			CullView view;
			RecordBindings bindings;
			SiteView* site = nullptr;
		};

		/** @brief A hosted view of a frame: its commit's inputs, then its pipelines, targets and face views once attached. */
		struct HostedView
		{
			ViewType type = ViewType::Reflection;
			std::shared_ptr<const Draws::ViewFrame> shape;
			std::shared_ptr<Draws::ViewResources> resources;
			std::vector<HostedFaceDraw> families;
			std::shared_ptr<const void> keep;
			std::deque<HostedFace> faces;  // never moved, since a SiteView names its bindings by address
			std::shared_ptr<const IndirectState> pipelines;
			std::shared_ptr<const ViewTargets> targets;
			std::shared_ptr<const TargetViews> main;
			bool valid = false;
		};

		/** @brief Render thread: the frame's hosted views not attached yet, and first person's once it is. */
		struct ViewFrameState
		{
			std::vector<std::shared_ptr<HostedView>> pending;
			std::shared_ptr<HostedView> firstPerson;
		};

		/** @brief The depth run's hosted views, read by its callback. */
		struct HostedRun
		{
			std::vector<std::shared_ptr<HostedView>> views;
		};

		/** @brief The first-person colour run's view and what its two renderings bind. */
		struct FirstPersonRun
		{
			std::shared_ptr<HostedView> view;
			RecordBindings colour, forward;
		};

		ViewFrameState& FrameStateOf(FrameSlot& a_slot)
		{
			if (!a_slot.viewState)
				a_slot.viewState = std::make_shared<ViewFrameState>();
			return *static_cast<ViewFrameState*>(a_slot.viewState.get());
		}

		/**
		 * @brief First person's forward sort rows per frame slot, for its sort on the CS thread. Grown, never shrunk,
		 * and rewritten only once the slot's last frame retired.
		 */
		std::vector<ForwardSortRow>& FirstPersonRowsOf(const FrameSlot& a_slot)
		{
			static std::vector<std::vector<ForwardSortRow>> rows;
			if (rows.size() <= a_slot.index)
				rows.resize(a_slot.index + 1);
			return rows[a_slot.index];
		}

		/**
		 * @brief A view's lane pipelines as its shape has them, so the recorder can bind them as for the main list. A slot the
		 * map sends to no bucket, or to one without capacity, gets none.
		 */
		std::shared_ptr<const IndirectState> ViewPipelines(const Draws::ViewFrame& a_shape, const ViewBuckets& a_buckets)
		{
			auto state = std::make_shared<IndirectState>(a_shape.indirect);
			std::uint32_t sets = 0;
			for (const std::uint32_t set : a_buckets.pipeline)
				sets = std::max(sets, set + 1);
			state->zGroups.assign(sets, ~0u);
			state->zPipelines.clear();
			state->plainColour.assign(sets, rhi::PipelineHandle{});
			const std::size_t mapped = a_shape.map ? a_shape.map->size() : 0;
			for (std::size_t b = 0; b < a_buckets.pipeline.size(); ++b) {
				const std::uint32_t set = a_buckets.pipeline[b];
				const std::uint32_t slot = b < a_buckets.pipelineSlot.size() ? a_buckets.pipelineSlot[b] : ~0u;
				const std::uint32_t bucket = slot < mapped ? (*a_shape.map)[slot] : ~0u;
				if (bucket >= a_shape.buckets.size() || !a_shape.buckets[bucket].capacity)
					continue;
				state->zGroups[set] = static_cast<std::uint32_t>(state->zPipelines.size());
				state->zPipelines.push_back(a_shape.buckets[bucket].depth);
				state->plainColour[set] = a_shape.buckets[bucket].colour;
			}
			return state;
		}

		/** @brief Render thread: the reflection's cube slice RTVs and the faces' own depth. */
		std::shared_ptr<const ViewTargets> ReflectionTargets(FrameSlot& a_slot, const Draws::ViewResources& a_view)
		{
			static std::shared_ptr<const ViewTargets> current;
			const rhi::Format depthFormat = a_view.depth ? a_view.depth->GetFormat() : rhi::Format::Unknown;
			// A new image of the same resource (a placement or a defrag move) needs new views too.
			if (current && current->target == a_view.target.Current() && current->width == a_view.width && current->height == a_view.height &&
				current->depthFormat == depthFormat && current->target->GetAPIResource().GetHandle().index == current->targetResource.index &&
				current->target->GetAPIResource().GetHandle().generation == current->targetResource.generation)
				return current;
			if (current)
				a_slot.holds.push_back(std::move(current));
			auto& device = S().device;
			auto targets = std::make_shared<ViewTargets>();
			targets->target = a_view.target.Current();
			targets->depthFormat = depthFormat;
			targets->width = a_view.width;
			targets->height = a_view.height;
			targets->faces = std::min(a_view.traits ? a_view.traits->faces : 0u, Draws::kMaxViewFaces);
			bool ok = targets->target && depthFormat != rhi::Format::Unknown && targets->width && targets->height && targets->faces &&
			          device.CreateDescriptorHeap({ rhi::DescriptorHeapType::RTV, targets->faces, false, "cs.dclf.direct.reflection.rtv" }, targets->rtvHeap) == rhi::Result::Ok &&
			          device.CreateDescriptorHeap({ rhi::DescriptorHeapType::DSV, 1, false, "cs.dclf.direct.reflection.dsv" }, targets->dsvHeap) == rhi::Result::Ok;
			if (ok) {
				targets->targetResource = targets->target->GetAPIResource().GetHandle();
				targets->colourFormat = a_view.target.Format();
			}
			for (std::uint32_t f = 0; ok && f < targets->faces; ++f) {
				targets->colours[f] = rhi::DescriptorSlot(targets->rtvHeap->GetHandle(), f);
				rhi::RtvDesc desc{};
				desc.dimension = rhi::RtvDim::Texture2DArray;
				desc.range.baseLayer = f;
				desc.range.layerCount = 1;
				ok = device.CreateRenderTargetView(targets->colours[f], targets->targetResource, desc) == rhi::Result::Ok;
			}
			if (ok) {
				// The faces' depth uses the view depth's format. Only the faces ever clear and draw it.
				rhi::ClearValue clear{};
				clear.type = rhi::ClearValueType::DepthStencil;
				clear.format = depthFormat;
				clear.depthStencil = { 1.0f, 0 };
				rhi::ResourceDesc desc{};
				desc.type = rhi::ResourceType::Texture2D;
				desc.resourceFlags = rhi::RF_AllowDepthStencil;
				desc.debugName = "cs.dclf.direct.reflection.depth";
				desc.texture.format = depthFormat;
				desc.texture.width = targets->width;
				desc.texture.height = targets->height;
				desc.texture.optimizedClear = &clear;
				ok = device.CreateCommittedResource(desc, targets->depth) == rhi::Result::Ok && targets->depth;
			}
			if (ok) {
				targets->depthResource = targets->depth->GetHandle();
				targets->dsv = rhi::DescriptorSlot(targets->dsvHeap->GetHandle(), 0);
				ok = device.CreateDepthStencilView(targets->dsv, targets->depthResource, rhi::DsvDesc{}) == rhi::Result::Ok;
			}
			targets->valid = ok;
			if (!ok)
				logger::error("[DCLF] CPU pipeline: the reflection's targets could not be made; its faces draw nothing");
			current = std::move(targets);
			return current;
		}

		/** @brief Render thread: first person's colour depth target, kPOST_ZPREPASS_COPY, as a DSV. */
		std::shared_ptr<const ViewTargets> FirstPersonTargets(FrameSlot& a_slot, const Draws::ViewResources& a_view)
		{
			static std::shared_ptr<const ViewTargets> current;
			if (current && current->target == a_view.target.Current())
				return current;
			if (current)
				a_slot.holds.push_back(std::move(current));
			auto& device = S().device;
			auto targets = std::make_shared<ViewTargets>();
			targets->target = a_view.target.Current();
			bool ok = targets->target && device.CreateDescriptorHeap({ rhi::DescriptorHeapType::DSV, 1, false, "cs.dclf.direct.first-person.dsv" }, targets->dsvHeap) == rhi::Result::Ok;
			if (ok) {
				targets->depthResource = targets->target->GetAPIResource().GetHandle();
				targets->depthFormat = a_view.target.Format();
				targets->dsv = rhi::DescriptorSlot(targets->dsvHeap->GetHandle(), 0);
				ok = device.CreateDepthStencilView(targets->dsv, targets->depthResource, rhi::DsvDesc{}) == rhi::Result::Ok;
			}
			targets->valid = ok;
			if (!ok)
				logger::error("[DCLF] CPU pipeline: first person's depth target view could not be made; its colour draws nothing");
			current = std::move(targets);
			return current;
		}

		/** @brief Render thread: sets up a_view's pipelines, targets and face views in the depth run. */
		void PrepareHostedView(FrameSlot& a_slot, SiteRun& a_depth, HostedView& a_view)
		{
			const auto& shape = *a_view.shape;
			const bool reflection = a_view.type == ViewType::Reflection;
			a_view.pipelines = ViewPipelines(shape, a_slot.set->buckets[static_cast<std::size_t>(a_view.type)]);
			a_view.targets = reflection ? ReflectionTargets(a_slot, *a_view.resources) : FirstPersonTargets(a_slot, *a_view.resources);
			a_view.main = a_depth.targets;
			a_view.valid = a_depth.valid && a_view.targets->valid && a_view.main && a_view.main->valid && shape.indirect.valid && shape.width && shape.height &&
			               (reflection || a_view.main->count >= 2);
			for (auto& face : a_view.faces) {
				auto& b = face.bindings;
				b = a_depth.bindings;
				b.resourceHeap = shape.resourceHeap;
				b.samplerHeap = shape.samplerHeap;
				b.main = a_view.pipelines.get();
				b.families = {};
				b.sortRows = {};
				b.offsetDepth = false;
				if (face.face < shape.push.size())
					b.framePush = shape.push[face.face];
				// Reflection lanes draw the faces' forward pipelines, first-person depth its Z-prepass ones.
				auto& site = NewView(a_slot, a_depth, a_view.type, face.view, !reflection);
				site.bindings = &b;
				face.site = &site;
			}
		}

		/** @brief A face's tree LOD, drawn in its own rendering after the face's draws with colour and depth loaded. */
		void RecordFaceFamilies(SiteOut& a_out, SiteRun& a_run, const HostedView& a_view, const HostedFace& a_face, rhi::PassBeginInfo a_begin)
		{
			static const std::shared_ptr<const void> kNothing;  // the face's pipeline is the shape's
			const auto drawn = [&](const HostedFaceDraw& a_draw) { return a_draw.face == a_face.face && a_draw.list.valid() && a_draw.pipeline.valid(); };
			if (std::none_of(a_view.families.begin(), a_view.families.end(), drawn))
				return;
			auto& slot = *a_run.slot;
			auto& list = a_out.Inline();
			Barrier(list, kAttachments);
			list.BeginPass(a_begin);
			RecordContext context{ list, *slot.set, a_view.type, a_face.face, a_run.site, false, a_face.bindings, &slot.counters };
			if (BeginList(context))
				for (const auto& draw : a_view.families)
					if (drawn(draw) && draw.family < Extensions::kDrawFamilies)
						if (const auto& faces = Extensions::DrawFamilies()[draw.family].contribution.faces; faces.record)
							faces.record({ list, Api::DrawFamilyPass::Face, kNothing, draw.row, draw.list, draw.pipeline, draw.signature });
			list.EndPass();
		}

		/** @brief The reflection faces. Each draws into its loaded cube slice with the faces' depth cleared, then its tree LOD. */
		void RecordReflection(SiteOut& a_out, SiteRun& a_run, HostedView& a_view)
		{
			const auto& shape = *a_view.shape;
			const auto& targets = *a_view.targets;
			for (auto& face : a_view.faces) {
				if (face.face >= targets.faces || !face.site)
					continue;
				// Every face clears the shared depth, so the previous face's writes must finish first.
				rhi::TextureBarrier depthBarrier{};
				depthBarrier.texture = targets.depthResource;
				depthBarrier.beforeSync = RS::All;
				depthBarrier.afterSync = RS::DepthStencil;
				depthBarrier.beforeAccess = RA::DepthReadWrite;
				depthBarrier.afterAccess = RA::DepthReadWrite;
				depthBarrier.beforeLayout = rhi::ResourceLayout::Undefined;
				depthBarrier.afterLayout = rhi::ResourceLayout::DepthReadWrite;
				depthBarrier.discard = true;
				a_out.Inline().Barriers(rhi::BarrierBatch{ { &depthBarrier, 1 }, {}, {} });
				rhi::ColorAttachment colour{};
				colour.rtv = targets.colours[face.face];
				colour.loadOp = rhi::LoadOp::Load;
				colour.storeOp = rhi::StoreOp::Store;
				colour.resource = targets.targetResource;
				rhi::DepthAttachment depth{};
				depth.dsv = targets.dsv;
				depth.depthLoad = rhi::LoadOp::Clear;
				depth.depthStore = rhi::StoreOp::Store;
				depth.clear.type = rhi::ClearValueType::DepthStencil;
				depth.clear.format = targets.depthFormat;
				depth.clear.depthStencil = { 1.0f, 0 };
				rhi::PassBeginInfo begin{};
				begin.colors = { &colour, 1 };
				begin.depth = &depth;
				begin.width = shape.width;
				begin.height = shape.height;
				begin.minDepth = 0.0f;
				begin.maxDepth = shape.depthRange;
				begin.debugName = "DCLF direct reflection face";
				RecordViewPass(a_out, a_run, *face.site, begin, face.bindings);
				depth.depthLoad = rhi::LoadOp::Load;
				begin.debugName = "DCLF direct reflection face trees";
				RecordFaceFamilies(a_out, a_run, a_view, face, begin);
			}
		}

		/** @brief First-person depth: Z-prepass depth pipelines into the main depth, after main depth and the families. */
		void RecordFirstPersonDepth(SiteOut& a_out, SiteRun& a_run, HostedView& a_view)
		{
			if (a_view.faces.empty() || !a_view.faces.front().site)
				return;
			auto& face = a_view.faces.front();
			const auto& shape = *a_view.shape;
			Barrier(a_out.Inline(), kAttachments);
			rhi::DepthAttachment depth{};
			depth.dsv = a_view.main->depth;
			depth.depthLoad = rhi::LoadOp::Load;
			depth.depthStore = rhi::StoreOp::Store;
			depth.stencilLoad = rhi::LoadOp::Load;
			depth.stencilStore = rhi::StoreOp::Store;
			rhi::PassBeginInfo begin{};
			begin.depth = &depth;
			begin.width = shape.width;
			begin.height = shape.height;
			begin.minDepth = 0.0f;
			begin.maxDepth = shape.depthRange;
			begin.debugName = "DCLF direct first-person depth";
			RecordViewPass(a_out, a_run, *face.site, begin, face.bindings);
		}
	}

	void AttachHostedViews(FrameSlot& a_slot, SiteRun& a_depth, Draws::RevisionShapes& a_shapes)
	{
		if (!a_slot.viewState || !a_slot.set)
			return;
		auto& frame = FrameStateOf(a_slot);
		if (frame.pending.empty())
			return;
		auto run = std::make_shared<HostedRun>();
		for (auto& view : std::exchange(frame.pending, {})) {
			const auto kind = view->type == ViewType::Reflection ? Api::EngineView::Reflection : Api::EngineView::FirstPerson;
			a_shapes.views[Draws::ViewIndex(kind)] = view->shape;
			PrepareHostedView(a_slot, a_depth, *view);
			if (view->type == ViewType::FirstPerson)
				frame.firstPerson = view;
			run->views.push_back(std::move(view));
		}
		a_depth.viewState = std::move(run);
	}

	void RecordHostedDepthViews(SiteOut& a_out, SiteRun& a_run)
	{
		const auto* run = static_cast<const HostedRun*>(a_run.viewState.get());
		if (!run)
			return;
		// An invalid view records nothing. Its queues are drained with the run's.
		for (const auto& view : run->views) {
			if (!view->valid)
				continue;
			if (view->type == ViewType::Reflection)
				RecordReflection(a_out, a_run, *view);
			else
				RecordFirstPersonDepth(a_out, a_run, *view);
		}
	}

	void UseFirstPersonImages(SiteRun& a_run, const SiteImages& a_images)
	{
		const auto* run = static_cast<const FirstPersonRun*>(a_run.viewState.get());
		if (!run || !run->view || !run->view->main || !run->view->targets)
			return;
		const auto same = [](rhi::ResourceHandle a_left, rhi::ResourceHandle a_right) {
			return a_left.index == a_right.index && a_left.generation == a_right.generation;
		};
		auto& view = *run->view;
		const auto& main = *view.main;
		const std::uint32_t colours = std::min<std::uint32_t>(main.count, static_cast<std::uint32_t>(a_images.colours.size()));
		bool coloursDiffer = false;
		for (std::uint32_t i = 0; i < colours; ++i)
			coloursDiffer = coloursDiffer || !same(main.colourResources[i], a_images.colours[i].resource);
		if (coloursDiffer) {
			auto views = TargetViews::RebindOf(view.main);
			for (std::uint32_t i = 0; i < colours; ++i) {
				views->colours[i] = a_images.colours[i].view;
				views->colourResources[i] = a_images.colours[i].resource;
			}
			view.main = std::move(views);
		}
		if (!a_images.depths.empty() && !same(view.targets->depthResource, a_images.depths.front().resource)) {
			// Only first person's depth target: no colours of its own and no faces' depth.
			auto targets = std::make_shared<ViewTargets>();
			const auto& made = *view.targets;
			targets->target = made.target;
			targets->targetResource = made.targetResource;
			targets->colourFormat = made.colourFormat;
			targets->depthFormat = made.depthFormat;
			targets->faces = made.faces;
			targets->width = made.width;
			targets->height = made.height;
			targets->valid = made.valid;
			targets->keep = view.targets;
			targets->dsv = a_images.depths.front().view;
			targets->depthResource = a_images.depths.front().resource;
			view.targets = std::move(targets);
		}
	}

	void RecordFirstPersonSite(SiteOut& a_out, SiteRun& a_run)
	{
		const auto* run = static_cast<const FirstPersonRun*>(a_run.viewState.get());
		if (!run || !run->view || !run->view->valid)
			return;
		auto& slot = *a_run.slot;
		const auto& view = *run->view;
		const auto& shape = *view.shape;
		const auto& main = *view.main;
		const auto& targets = *view.targets;
		// The depth site's callback already drained every first-person batch on this thread.
		const auto batches = AwaitAll(a_run, *view.faces.front().site);
		auto& list = a_out.Inline();
		// Each lane's first-person forward pipeline into kMAIN and the motion vectors, tested against kPOST_ZPREPASS_COPY.
		Barrier(list, kAttachments);
		std::array<rhi::ColorAttachment, Draws::kColorTargets> colours{};
		for (std::uint32_t i = 0; i < main.count; ++i) {
			colours[i].rtv = main.colours[i];
			colours[i].loadOp = rhi::LoadOp::Load;
			colours[i].storeOp = rhi::StoreOp::Store;
			colours[i].resource = main.colourResources[i];
		}
		rhi::DepthAttachment depth{};
		depth.dsv = targets.dsv;
		depth.depthLoad = rhi::LoadOp::Load;
		depth.depthStore = rhi::StoreOp::Store;
		depth.stencilLoad = rhi::LoadOp::Load;
		depth.stencilStore = rhi::StoreOp::Store;
		rhi::PassBeginInfo begin{};
		begin.colors = { colours.data(), 2u };
		begin.depth = &depth;
		begin.width = shape.width;
		begin.height = shape.height;
		begin.minDepth = 0.0f;
		begin.maxDepth = shape.depthRange;
		begin.debugName = "DCLF direct first person";
		// In a GPU-driven frame the graph draws the buckets, and the lanes are only the forward part.
		if (slot.ownsEpochs) {
			list.BeginPass(begin);
			RecordContext colour{ list, *slot.set, ViewType::FirstPerson, 0, Site::FirstPerson, false, run->colour, &slot.counters };
			colour.mainTargets = false;  // kMAIN and the motion vectors only
			if (BeginList(colour))
				for (const auto* batch : batches)
					if (batch->count)
						RecordBatch(colour, *batch);
			list.EndPass();
		}
		// The forward part: forward lanes with the main set's pipelines, farthest first, onto every main target.
		if (slot.ownsEpochs && shape.forwardRanges.empty())
			return;
		Barrier(list, kAttachments);
		begin.colors = { colours.data(), main.count };
		begin.debugName = "DCLF direct first-person forward";
		list.BeginPass(begin);
		RecordContext forward{ list, *slot.set, ViewType::FirstPerson, 0, Site::FirstPerson, false, run->forward, &slot.counters };
		if (BeginList(forward))
			RecordForward(forward, batches);
		list.EndPass();
	}
}

namespace DCLF::Direct
{
	using namespace detail;

	void HostedViewCommitted(const HostedViewCommit& a_commit)
	{
		DCLF_ZONE("CS.DCLF.Cpu.HostedViewCommitted");
		auto* slot = S().current;
		if (!slot || !slot->set || !a_commit.shape || !a_commit.resources || a_commit.faces.empty())
			return;
		auto view = std::make_shared<HostedView>();
		view->type = a_commit.type;
		view->shape = a_commit.shape;
		view->resources = a_commit.resources;
		view->families.assign(a_commit.families.begin(), a_commit.families.end());
		view->keep = a_commit.keep;
		for (const auto& cull : a_commit.faces) {
			auto& face = view->faces.emplace_back();
			face.face = cull.viewIndex;
			face.view = cull;
		}
		FrameStateOf(*slot).pending.push_back(std::move(view));
	}

	bool FirstPersonColourReached()
	{
		auto& s = S();
		auto* slot = s.current;
		if (!slot || !slot->viewState || !slot->set)
			return false;
		auto view = std::exchange(FrameStateOf(*slot).firstPerson, nullptr);
		if (!view || !view->valid || view->faces.empty() || !view->faces.front().site)
			return false;
		const auto& face = view->faces.front();
		auto& run = NewBareRun(*slot, Site::FirstPerson);
		// The callback binds the run's own heaps in DXVK's buffer.
		run.bindings.resourceHeap = face.bindings.resourceHeap;
		run.bindings.samplerHeap = face.bindings.samplerHeap;
		auto state = std::make_shared<FirstPersonRun>();
		state->view = view;
		state->colour = face.bindings;
		// The forward part uses the main set's pipelines, sorted by the first-person camera.
		state->forward = face.bindings;
		state->forward.main = &view->shape->indirect;
		state->forward.viewProj = face.view.viewProj;
		bool rowsDone = true;
		if (!slot->ownsEpochs || !view->shape->forwardRanges.empty()) {
			// A frame whose producer is not done waits here, and the wait is counted.
			const auto* rows = FrameValues::Get().RowsIfDone();
			if (!rows) {
				++s.window.rowWaits;
				rows = FrameValues::Get().RowsIfDone(true);
			}
			rowsDone = rows != nullptr;
			if (rows) {
				auto& kept = FirstPersonRowsOf(*slot);
				if (kept.size() < rows->size())
					kept.resize(rows->size());
				for (const auto& batch : slot->set->List(ViewType::FirstPerson))
					for (std::uint32_t lane = 0; lane < batch.count; ++lane)
						if (const std::uint32_t object = batch.object[lane]; (!batch.flags || (batch.flags[lane] & kLaneForward)) && object < rows->size())
							kept[object] = ForwardSortRowOf((*rows)[object]);
				state->forward.sortRows = kept;
			}
		}
		// Its view is the depth site's, for the drawn counts and the callback's drain.
		run.views.push_back(face.site);
		run.viewState = std::move(state);
		if (!slot->ownsEpochs)
			return HandOffRun(run, rowsDone);
		SetPending(run);
		return true;
	}

	bool ViewSiteLive(std::uint32_t a_view)
	{
		if (a_view != Draws::ViewIndex(Api::EngineView::Reflection) && a_view != Draws::ViewIndex(Api::EngineView::FirstPerson))
			return false;
		const auto kind = static_cast<Api::EngineView>(a_view);
		return Extensions::ViewRegistered(kind);
	}
}
