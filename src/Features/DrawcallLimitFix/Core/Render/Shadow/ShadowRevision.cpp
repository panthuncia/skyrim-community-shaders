// The shadow engine's revision participant steps. Most run on the snapshot builder: buffer requirements, the shadow shapes,
// and the build ahead's shadow payload with its exclusions. PostInputs and PrepareAhead run on the render thread.

#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowExclusionBuild.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Snapshot/EngineReads.h"

namespace DCLF
{
	namespace
	{
		/** @brief Builds a mode's exclusion through the exclusion contribution. Null when there is no contribution. */
		std::shared_ptr<void> BuildExclusion(Api::ShadowSource a_source, const std::shared_ptr<const EntrySpace>& a_candidates,
			const ShadowPayload& a_payload, std::uint32_t a_mode, const SceneTables& a_tables)
		{
			Api::ShadowExclusionBuild build(a_source, a_candidates, a_tables, a_payload.regionInputs, a_payload.inputList);
			build.modeBits = a_payload.modeBits[a_mode];
			build.membership = a_payload.kept ? a_payload.membership[a_mode] : 0;
			build.frameNumber = a_payload.inputs.frameNumber;
			build.tablesGeneration = a_payload.inputs.tablesGeneration;
			build.keptBuild = a_payload.keptBuild;
			build.movesValid = a_payload.movesValid[a_mode];
			build.joined = a_payload.joined[a_mode];
			build.left = a_payload.left[a_mode];
			Extensions::BuildShadowExclusion(build);
			return std::move(build.result);
		}
	}

	void ShadowEngine::RequireSequences(RevisionMake& a_make, GrowthBuild& a_build)
	{
		auto& producer = a_make.producer.producer;
		const auto& request = a_make.publication.revision;
		const auto& inputs = producer.inputs;
		const auto& latches = producer.latches;
		auto& scratch = revisionScratch;
		scratch.latched.clear();
		scratch.indirect = a_make.catalog ? GetShadowIndirectState(*a_make.catalog, a_make.catalog->shadowFormat) : ShadowIndirectState{};
		// Keep only the occlusion maps DCLF draws.
		{
			auto& layouts = producer.occlusionLayouts;
			layouts = inputs.occlusionLayouts;
			std::erase_if(layouts, [](const ShadowViewLayout& a_layout) { return !Extensions::OcclusionOn(OcclusionOfMode(a_layout.modeIndex)); });
		}
		// Remember the most recent sets of drawn views. Each gets its own variant.
		if (const auto active = inputs.shadowActiveViews; active != 0) {
			auto& recent = producer.recentActiveViews;
			if (recent.empty() || recent.front() != active) {
				std::erase(recent, active);
				recent.insert(recent.begin(), active);
				if (recent.size() > kRecentActiveViews)
					recent.resize(kRecentActiveViews);
			}
		}
		const auto& shadowState = inputs.shadow;
		if (shadowState && scratch.indirect.valid) {
			RequireShadowSequences(a_make.producer, a_build);
			// Size the latch for every view, key slot, registered state and sun process the epoch could name.
			const std::size_t slotKeys = std::max(a_make.lookups.shadowSlotKeys.size(), request.shadowSlotKeys);
			const std::size_t keys = slotKeys + request.shadowKeys;
			const auto views = static_cast<std::uint32_t>(inputs.shadowPlacements.size());
			ReserveShadowLatch(a_make.producer, a_build, *shadowState, views, static_cast<std::uint32_t>(keys), inputs.shadowRasterStates,
				std::max(views, latches.shadow.sunProcesses));
		}
	}

	void ShadowEngine::RequireLatched(RevisionMake& a_make, GrowthBuild& a_build)
	{
		const auto& producer = a_make.producer.producer;
		auto& scratch = revisionScratch;
		if (const auto& shadowState = producer.inputs.shadow; shadowState && scratch.indirect.valid) {
			scratch.latched = ShadowLatchedLayout(*shadowState);
			RequireLatchedBlock(a_build, producer.latchHandles.shadowLatched, scratch.latched);
		}
	}

	void ShadowEngine::NoteSealed(RevisionMake& a_make)
	{
		// Remake the shapes only when the casting bound outgrows them, not on every bound change.
		auto& owner = a_make.producer;
		const auto& producer = owner.producer;
		if (producer.drawBound.shadowVersion != owner.shadowBoundChecked) {
			owner.shadowBoundChecked = producer.drawBound.shadowVersion;
			if (!ShadowShapesHold(owner, ShadowBoundsOf(producer.drawBound, a_make.lookups)))
				++owner.shadowBoundOutgrown;
		}
	}

	void ShadowEngine::MixShapesKey(const RevisionMake& a_make, Draws::ShapesKeyMixer& a_mixer) const
	{
		auto& group = a_mixer.group;
		auto mix = [&a_mixer](std::uint64_t a_value) { a_mixer.Mix(a_value); };
		auto ptr = [&a_mixer](const void* a_pointer) { a_mixer.Ptr(a_pointer); };
		auto raw = [&a_mixer]<class T>(const T& a_value) { a_mixer.Raw(a_value); };
		const auto& owner = a_make.producer;
		const auto& producer = owner.producer;
		const auto& inputs = producer.inputs;
		const auto& latches = producer.latches;
		group = ImplProducer::kKeyPipelines;
		ptr(revisionScratch.indirect.version.get());
		mix(revisionScratch.indirect.valid);
		group = ImplProducer::kKeyShadow;
		if (const auto& s = inputs.shadow) {
			ptr(s.get());
			raw(latches.shadow);
			ptr(s->latchedTargets.load(std::memory_order_acquire).get());
			mix(inputs.shadowSlots ? inputs.shadowSlots->drawArgs.size() : 0);
		}
		for (const auto& view : inputs.shadowPlacements)
			raw(view);
		mix(producer.occlusionLayouts.size());
		for (const auto& view : producer.occlusionLayouts)
			raw(view);
		mix(producer.recentActiveViews.size());
		for (const auto active : producer.recentActiveViews)
			mix(active);
		group = ImplProducer::kKeyBound;
		mix(owner.shadowBoundOutgrown);
	}

	void ShadowEngine::MakeShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout&)
	{
		MakeShadowShapes(a_make.producer, a_sealed, revisionScratch.indirect, a_make.lookups, revisionScratch.latched);
	}

	void ShadowEngine::MakeShadowShapes(ImplProducer& a_producer, const NamedVersions& a_sealed, const ShadowIndirectState& a_indirect, const Lookups& a_lookups,
		const std::vector<LatchedCopy>& a_latched)
	{
		auto& producer = a_producer.producer;
		const auto& inputs = producer.inputs;
		auto& made = producer.made;
		const auto& handles = producer.latchHandles;
		const auto& latches = producer.latches;
		const auto& shadowState = inputs.shadow;
		const auto& shadowIndirect = a_indirect;
		const auto& lookups = a_lookups;
		const rhi::DescriptorHeapHandle resourceHeap = inputs.resourceHeap, samplerHeap = inputs.samplerHeap;
		// Capacities grow from the last make's shapes.
		auto previousShadow = std::exchange(made.shadow, {});
		// A layout past the posted slots waits for them.
		const std::size_t shadowSlotCount = inputs.shadowSlots ? inputs.shadowSlots->drawArgs.size() : 0;
		if ((inputs.shadowPlacements.empty() && producer.occlusionLayouts.empty()) || !shadowState || !shadowIndirect.valid)
			return;
		// Sized from the scene's casting bound, not a payload, so shapes change with the scene's objects and not every frame.
		const auto bounds = std::make_shared<const ShadowBounds>(ShadowBoundsOf(producer.drawBound, lookups));
		ShadowCapacity capacity;
		{
			UnitGroup sequences, counts;
			if (const auto& slotBuffers = inputs.shadowSlots) {
				for (const auto& buffer : slotBuffers->drawArgs)
					sequences.Add(buffer);
				for (const auto& buffer : slotBuffers->bucketCounts)
					counts.Add(buffer);
			}
			capacity.sequenceDraws = static_cast<std::uint32_t>(std::min<std::uint64_t>(sequences.Units(a_sealed), UINT32_MAX));
			capacity.bucketCountWords = static_cast<std::uint32_t>(std::min<std::uint64_t>(counts.Units(a_sealed), UINT32_MAX));
			capacity.viewSlots = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_sealed.Of(shadowState->viewBlocks.buffer).units, UINT32_MAX));
		}
		producer.shadowBucketWords = capacity.bucketCountWords;
		const std::uint64_t viewBlocks = a_sealed.Of(shadowState->viewBlocks.buffer).deviceAddress;
		// Draws look up their vertex layout and technique by key slot, not by bucket.
		auto slotLayouts = std::make_shared<std::vector<ShadowSlotEntry>>();
		slotLayouts->reserve(lookups.shadowSlotKeys.size());
		for (const auto& key : lookups.shadowSlotKeys)
			slotLayouts->push_back({ key.vertexLayout, key.technique | kShadowSlotTechniqueKnown, 0 });
		// Every placement is required. An occlusion map whose slot does not exist yet is left out, and the engine draws it.
		std::vector<ShadowViewLayout> layouts = inputs.shadowPlacements;
		if (std::any_of(layouts.begin(), layouts.end(), [&](const auto& a_view) { return a_view.slot >= shadowSlotCount || a_view.slot >= capacity.viewSlots; }))
			return;
		for (const auto& map : producer.occlusionLayouts)
			if (map.slot < shadowSlotCount && map.slot < capacity.viewSlots)
				layouts.push_back(map);
		ShadowShapeInputs in;
		in.resourceHeap = resourceHeap;
		in.samplerHeap = samplerHeap;
		in.indirect = shadowIndirect;
		for (const auto& layout : layouts)
			in.rows.push_back(ShadowRowBuckets(producer.rowBuckets, layout.modeIndex, layout.rasterState, lookups, shadowIndirect));
		in.views = layouts;
		in.bounds = bounds.get();
		in.capacity = capacity;
		in.viewBlocks = viewBlocks;
		in.latch = a_sealed.Latch(handles.shadow);
		in.latchLayout = latches.shadow;
		in.slotLayouts = slotLayouts;
		in.zeros = a_sealed.Latch(handles.shadowZeros);
		// Slot capacities only grow from the last shape with this layout.
		for (const auto& candidate : previousShadow)
			if (candidate && LayoutOf(*candidate) == layouts)
				in.previous = candidate;
		in.latched.copies = a_latched;
		if (!in.latched.copies.empty())
			in.latched.latch = a_sealed.Latch(handles.shadowLatched);
		made.shadow.push_back(MakeShadowShape(*shadowState, in));
		// Variants share the full shape but record only a recent set of views, such as an interior frame without the maps.
		const auto full = made.shadow.back();
		const std::uint64_t all = full->views.size() >= 64 ? ~0ull : ((1ull << full->views.size()) - 1);
		for (const auto active : producer.recentActiveViews) {
			if ((active & all) == all || (active & all) == 0)
				continue;
			auto variant = std::make_shared<ShadowFrame>(*full);
			variant->activeViews = active & all;
			made.shadow.push_back(std::move(variant));
		}
	}

	bool ShadowEngine::ShadowShapesHold(const ImplProducer& a_producer, const ShadowBounds& a_bounds)
	{
		const auto& producer = a_producer.producer;
		const auto& shadowState = producer.inputs.shadow;
		if (!shadowState)
			return true;
		const std::uint32_t words = producer.shadowBucketWords;
		std::vector<std::uint64_t> need;
		for (const auto& shape : producer.made.shadow) {
			if (!shape || !shape->rows)
				continue;
			for (std::size_t v = 0; v < shape->views.size() && v < shape->rows->size(); ++v) {
				const auto& view = shape->views[v];
				const auto& row = (*shape->rows)[v];
				if (row.pipelines.size() >= words || view.modeIndex >= kShadowModeCount || a_bounds.modeDraws[view.modeIndex] > view.capacity)
					return false;
				const auto& draws = a_bounds.keySlotDraws[view.modeIndex];
				need.assign(view.buckets.size(), 0);
				for (std::size_t k = 0; k < row.bucketOfSlot.size() && k < draws.size(); ++k)
					if (const auto bucket = row.bucketOfSlot[k]; bucket != Lookups::kNone && bucket < need.size())
						need[bucket] += draws[k];
				for (std::size_t b = 0; b < need.size(); ++b)
					if (need[b] > view.buckets[b].capacity)
						return false;
			}
		}
		return true;
	}

	void ShadowEngine::PostInputs(RevisionInputs& a_next)
	{
		a_next.shadow = shadow;
		auto& drawPipelines = DrawPipelines::Get();
		a_next.shadowRasterStates = drawPipelines.ShadowRasterStateCount();
		a_next.occlusionLayouts = PredictedOcclusion(false);
		a_next.shadowPlacements = shadowPlacements;
		a_next.shadowActiveViews = lastActiveViews;
		a_next.shadowSlots = SlotBuffers();
	}

	std::shared_ptr<const ShadowSlotBuffers> ShadowEngine::SlotBuffers()
	{
		// Remade only when something changed, so the revision code and the shadow build see the same value.
		if (!shadow)
			return nullptr;
		if (!postedShadowSlots || !SameShadowSlotBuffers(*postedShadowSlots, *shadow))
			postedShadowSlots = std::make_shared<const ShadowSlotBuffers>(MakeShadowSlotBuffers(*shadow));
		return postedShadowSlots;
	}

	void ShadowEngine::PrepareAhead(ImplSnapshotBuilder::AheadContext& a_context)
	{
		auto& c = a_context;
		const auto& tables = Scene::FrameView::Get().GetTables();
		if (shadow && lastShadow.known && TicketUtilityShader() && !tables.objects.empty() &&
			tables.shadowTechnique.size() == tables.objects.size()) {
			c.shadowInputs = PrepareShadowInputs(*shadow, lastShadow.modes, lastShadow.rasterStates);
			c.shadowTarget = shadow;
			c.shadow = true;
		}
	}

	void ShadowEngine::BuildPayloadShared(const AheadBuild& a_build)
	{
		const auto& c = a_build.context;
		if (!c.shadow || !c.shadowTarget || !c.shadowTarget->scene)
			return;
		const auto oldest = [&a_build](PayloadPart a_part) { return a_build.payloads.OldestContentEpoch(a_part).value_or(~std::uint64_t{ 0 }); };
		auto& a = aheadShadow;
		auto& in = a.in;
		in = c.shadowInputs;
		in.frameNumber = a_build.frame;
		in.tablesGeneration = a_build.generation;
		in.lookupGeneration = a_build.lookups.shadowGeneration;
		in.sunCandidates = a_build.sunCandidates;
		in.lightCandidates = a_build.lightCandidates;
		in.tablesOldestContentEpochs = a_build.oldestTables;
		in.materialRowsOldestContentEpoch = oldest(PayloadPart::ShadowRows);
		in.inputsOldestContentEpoch = oldest(PayloadPart::Inputs);
		a.payload = AcquireShadowPayload();
		a.publication = &a_build.out;
		a.frame = a_build.frame;
		const auto shadowStart = std::chrono::steady_clock::now();
		BuildShadowPayloadShared(in, a_build.tables, a_build.lookups, *a.payload, a_build.out.streams, ShadowKeptState(), a.split);
		a_build.stats.shadowMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shadowStart).count();
	}

	void ShadowEngine::BuildPayload(const AheadBuild& a_build)
	{
		const auto& c = a_build.context;
		if (!c.shadow || !c.shadowTarget || !c.shadowTarget->scene)
			return;
		auto& pairStats = a_build.stats;
		const auto msSince = [](std::chrono::steady_clock::time_point a_start) {
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
		};
		const auto& tables = a_build.tables;
		auto& a = aheadShadow;
		{
			DCLF_ZONE("CS.DCLF.BuildShadowPayload");
			// Run the shared half unless it already ran for this build. The region half writes into the finished main region.
			if (!a.split.pending || a.publication != &a_build.out || a.frame != a_build.frame)
				BuildPayloadShared(a_build);
			const auto shadowStart = std::chrono::steady_clock::now();
			BuildShadowPayloadRegion(a.in, tables, a_build.lookups, *a.payload, ShadowKeptState(), &a_build.mainRegion, a.split);
			pairStats.shadowMs += msSince(shadowStart);
		}
		a.publication = nullptr;
		const auto& in = a.in;
		auto payload = std::move(a.payload);
		// Epochs may keep the payload longer than the publication, so it must not hold the stream views.
		payload->streams.reset();
		if (PassCapture::ShadowWithholdingEnabled()) {
			DCLF_ZONE("CS.DCLF.BuildShadow.Exclusions");
			const auto exclusionsStart = std::chrono::steady_clock::now();
			// The two exclusions use separate caches, so they can run in parallel.
			const bool sun = in.modeUsed[kSunShadowMode], parabolic = in.modeUsed[kParabolicShadowMode];
			const auto exclusion = [&](std::size_t a_which) {
				if (a_which == 0 && sun)
					payload->sunExclusion = BuildExclusion(Api::ShadowSource::Sun, in.sunCandidates, *payload, kSunShadowMode, tables);
				else if (a_which == 1 && parabolic)
					payload->parabolicExclusion = BuildExclusion(Api::ShadowSource::Point, in.lightCandidates, *payload, kParabolicShadowMode, tables);
			};
			if (sun && parabolic) {
				::RenderGraph::Frame::SharedExecutor().ParallelFor("CS.DCLF.BuildShadow.Exclusions", 2, 1, [&](std::size_t a_begin, std::size_t a_end) {
					DCLF_ZONE("CS.DCLF.BuildShadow.ExclusionsShards");
					for (std::size_t x = a_begin; x < a_end; ++x)
						exclusion(x);
				});
			} else {
				exclusion(0);
				exclusion(1);
			}
			pairStats.exclusionsMs += msSince(exclusionsStart);
		}
		a_build.out.shadow = std::move(payload);
	}
}
