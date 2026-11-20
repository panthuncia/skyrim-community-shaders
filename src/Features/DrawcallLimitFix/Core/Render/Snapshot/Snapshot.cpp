// The snapshot builder. It runs one pass at a time on DCLF's preparation pool. For each publication it builds the draws ahead,
// makes and seals the revision, and posts a snapshot as the frame revision's scene fragment (plan step 10b). The frame revision
// adopts it at a Begin, with the staged builds it was recorded on, alone or with the revision whose versions it recorded against.
// A Begin waits only after a shader reload's pipeline swap (plan step 10a).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Gpu/NamedRefs.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "RenderGraph/BuildManager.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Frame/BoundaryCheck.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/Revision.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatches.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix.h"

#include <algorithm>
#include <thread>

#include <Tracy/TracyC.h>

namespace DCLF
{
	using org::async::RevisionFragment;

	void IndirectDraws::PostAheadContext()
	{
		auto post = std::make_unique<Impl::AheadPost>();
		TracyCZoneN(make, "CS.DCLF.Ahead.Make", true);
		const bool fit = impl->MakeAheadContext(post->context, failed);
		TracyCZoneEnd(make);
		DCLF_ZONE("CS.DCLF.Ahead.Post");
		impl->PostAhead(std::move(post), fit);
	}

	bool IndirectDraws::Impl::MakeAheadContext(AheadContext& a_out, bool a_failed)
	{
		auto& c = a_out;
		c = {};
		// Use the newest toggles, not the frame's, because this is for the coordinator's next commit.
		c.toggles = SceneToggles();
		if (a_failed || !resources || !resources->scene)
			return false;
		auto* lighting = EngineShaders::Lighting();
		if (!lighting || !DrawPipelines::Get().Enabled() || !FrameIndirectStateValid())
			return true;
		for (const std::size_t j : { kAsyncZPrepass, kAsyncColour }) {
			const auto& masks = epochMasks[j];
			// The colour build replays the Z-prepass's vertex inputs, so it needs them first.
			if (!masks.known || (j == kAsyncColour && !prepassInputs))
				continue;
			DCLF_ZONE("CS.DCLF.Ahead.MainInputs");
			c.inputs[j] = PrepareMainInputs(j == kAsyncZPrepass, *resources, masks.vsMask, masks.psMask);
			c.build[j] = true;
		}
		c.target = resources;
		DCLF_ZONE("CS.DCLF.Ahead.Participants");
		for (auto* participant : participants)
			participant->PrepareAhead(c);
		c.valid = c.build[kAsyncZPrepass] || c.build[kAsyncColour] || c.shadow;
		return true;
	}

	void IndirectDraws::Impl::PostResourcesAhead(bool a_failed)
	{
		// Post to the builder only. A second post to the coordinator in one frame would look like the next frame's input.
		// A context without both main builds cannot build complete draws, so it is not posted.
		auto context = std::make_unique<AheadContext>();
		if (!MakeAheadContext(*context, a_failed) || !context->target || !context->build[kAsyncZPrepass] || !context->build[kAsyncColour])
			return;
		resourcesAheadSlot.Post(std::move(context));
		(void)SnapshotPump();
		snapshotInputsMoved.store(true, std::memory_order_release);
		WakeSnapshotBuilder();
	}

	bool IndirectDraws::Impl::RefreshWorkContext()
	{
		resourcesAheadSlot.TakeInto(resourcesAhead);
		auto& context = snapshotBuilder.work->context;
		const auto& main = producer.inputs.main;
		// Nothing to do if the draws already target these main resources, or no context for them was posted yet.
		if (!main || context.target == main || resourcesAhead.target != main)
			return false;
		context = resourcesAhead;
		++snapshotBuilder.contextsReplaced;
		return true;
	}

	void IndirectDraws::Impl::PostAhead(std::unique_ptr<AheadPost> a_post, bool a_fit)
	{
		// Immutable once posted, and the latest wins. The coordinator takes it at the start of its pass.
		if (a_fit) {
			DCLF_ZONE("CS.DCLF.Ahead.PostFit");
			PostFit();
		}
		auto& post = a_post;
		post->fit = postedFit;
		post->fitSerial = postedFitSerial;
		aheadSlot.Post(std::move(post));
		WakeScenePass(SceneWake::FrameInput);
	}

	void IndirectDraws::TakeAheadContext()
	{
		if (impl)
			impl->aheadSlot.TakeInto(impl->aheadTaken);
	}

	void IndirectDraws::Impl::PostFit()
	{
		// What a commit may claim is bounded by the buffer versions the adopted set names.
		const auto& adopted = Adopted();
		const auto& shadow = shadowEngine->shadow;
		const auto same = [](const std::weak_ptr<const void>& a_kept, const std::shared_ptr<const void>& a_now) {
			return !a_kept.owner_before(a_now) && !a_now.owner_before(a_kept);
		};
		if (adopted.Serial() == fitSetSerial && same(fitMain, resources) && same(fitShadow, shadow))
			return;
		fitSetSerial = adopted.Serial();
		fitMain = resources;
		fitShadow = shadow;
		const auto units = [&](const Versioned& a_buffer) { return static_cast<std::uint32_t>(std::min<std::uint64_t>(NamedUnits(adopted, a_buffer), UINT32_MAX)); };
		const std::array<SceneFit, 2> fit{ SceneFitOf(adopted, *resources->scene, std::min(units(resources->visibility), units(resources->frustum))),
			shadow && shadow->scene ? SceneFitOf(adopted, *shadow->scene, units(shadow->visibility)) : SceneFit{} };
		if (fit != postedFit) {
			postedFit = fit;
			++postedFitSerial;
		}
	}

	bool IndirectDraws::FitsScene(const void* a_tables, std::uint32_t a_slot, bool a_shadow) const
	{
		if (!impl)
			return true;
		// Read the coordinator's copy, never the render thread's.
		const auto& posted = impl->aheadTaken;
		const auto& tables = *static_cast<const SceneTables*>(a_tables);
		// Rows never wait for capacity, because the payload parts grow with it.
		return a_slot < tables.objects.size() && ObjectFits(tables, a_slot, posted.fit[a_shadow ? 1 : 0]);
	}

	std::uint64_t IndirectDraws::SceneFitSerial() const
	{
		return impl ? impl->aheadTaken.fitSerial : 0;
	}

	void IndirectDraws::PostSnapshotWork(std::shared_ptr<const void> a_publication)
	{
		if (!impl || !a_publication)
			return;
		auto& s = *impl;
		auto publication = std::static_pointer_cast<const ScenePublication>(std::move(a_publication));
		if (failed) {
			// Failed. Post the publication alone, with no draws or revision, and mark it taken at once. It names no frame versions.
			auto snapshot = std::make_shared<Impl::SceneSnapshot>();
			s.snapshotWorkTaken.store(publication->sequence, std::memory_order_release);
			snapshot->publication = std::move(publication);
			snapshot->drawsComplete = true;
			RenderGraph::Frame::Revision::Scene scene;
			scene.value = std::move(snapshot);
			RenderGraph::Frame::Revision::PublishScene(std::move(scene));
			s.snapshotsPosted.fetch_add(1, std::memory_order_release);
			return;
		}
		auto& store = Scene::Coordinator::Get();
		// Copy now anything the coordinator or frame will overwrite later. The tables and lookups are immutable.
		auto work = std::make_unique<Impl::SnapshotWork>();
		work->publication = std::move(publication);
		work->context = s.aheadTaken.context;
		work->frame = store.GetFrame();
		work->sun = store.CoordinatorSunCandidates();
		work->light = store.CoordinatorLightCandidates();
		work->aheadNumber = s.aheadKicked.fetch_add(1, std::memory_order_acq_rel) + 1;
		// Latest wins, so publications made while the builder is busy coalesce here. The replaced one is dropped on this thread.
		s.snapshotWorkSlot.Post(std::move(work));
		(void)s.SnapshotPump();
		WakeSnapshotBuilder();
	}

	namespace
	{
		// Up to this many unread change-log entries, RunAhead runs its two stages inline instead of on two pool tasks.
		constexpr std::uint64_t kInlineStageChanges = 128;

		template <class Resources>
		void NamedAddressesOf(const NamedVersions& a_sealed, const Resources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out)
		{
			const auto& scene = *a_resources.scene;
			std::uint64_t objects = a_sealed.Of(a_resources.visibility).units;
			if constexpr (requires { a_resources.frustum; })
				objects = std::min(objects, a_sealed.Of(a_resources.frustum).units);
			Extensions::FrameBindingValuesOf(a_sealed, scene, a_frame, a_out.frameBindings, false);
			a_out.streamBuffer = Extensions::VertexStreamTaken() ? RefOf(a_sealed.Of(scene.vertexStream)) : RenderGraph::BufferRef{};
			a_out.fit = SceneFitOf(a_sealed, scene, objects);
		}

		/**
		 * @brief Whether a_work would build the same snapshot as the last one posted. The coordinator republishes shared objects only
		 * when they change, so pointer equality plus generations means the same build.
		 */
		bool PassesThrough(const ImplSnapshotBuilder::SnapshotBuilder::Posted& a_posted, const ImplSnapshotBuilder::SnapshotWork& a_work)
		{
			if (!a_posted.publication || !a_posted.revision || !a_work.publication)
				return false;
			const auto& next = *a_work.publication;
			const auto& prior = *a_posted.publication;
			if (next.tables != prior.tables || next.lookups != prior.lookups || next.catalog != prior.catalog || next.claims != prior.claims ||
				next.sunCandidates != prior.sunCandidates || next.lightCandidates != prior.lightCandidates || next.categoryNodes != prior.categoryNodes)
				return false;
			if (next.tablesGeneration != prior.tablesGeneration || next.togglesGeneration != prior.togglesGeneration || next.lackingCount != prior.lackingCount ||
				next.TransientWaiting() != prior.TransientWaiting() || next.built != prior.built ||
				next.lightEntriesAppeared != prior.lightEntriesAppeared || next.revision.pipelines != prior.revision.pipelines ||
				next.revision.shadowSlotKeys != prior.revision.shadowSlotKeys || next.revision.shadowKeys != prior.revision.shadowKeys)
				return false;
			if (a_work.sun != a_posted.sun || a_work.light != a_posted.light)
				return false;
			const auto& c = a_work.context;
			if (c.valid != a_posted.valid || c.shadow != a_posted.shadow || c.build != a_posted.build || c.target.get() != a_posted.target ||
				c.shadowTarget.get() != a_posted.shadowTarget)
				return false;
			for (const std::size_t j : { kAsyncZPrepass, kAsyncColour })
				if (c.inputs[j].vsFrameMask != a_posted.vsMask[j] || c.inputs[j].psFrameMask != a_posted.psMask[j])
					return false;
			return true;
		}

		/** @brief Give every payload of a_out the scene list region as both builds left it. */
		void ShareAheadRegion(ImplSnapshotBuilder::DrawPublication& a_out, BuildCache& a_mainCache)
		{
			KeptView<DrawInput> region;
			if (a_out.shadow)
				region = a_out.shadow->regionInputs;
			else if (Policy::ReuseResidentRegion() && a_mainCache.region.begun)
				region = a_mainCache.region.inputs.View();
			for (const std::size_t j : { kAsyncZPrepass, kAsyncColour })
				if (const auto& payload = a_out.payloads[j])
					payload->resident = region;
			if (a_out.shadow) {
				const auto* main = ImplSnapshotBuilder::NewestMainPayload(a_out);
				a_out.shadow->mainTail = main ? main->body->inputList.size() : 0;
			}
		}
	}

	std::unique_ptr<ImplSnapshotBuilder::SnapshotWork> ImplSnapshotBuilder::WorkToBuild()
	{
		auto& b = snapshotBuilder;
		if (auto next = snapshotWorkSlot.Take())
			b.held = std::move(next);
		if (!b.held)
			return nullptr;
		bool closed = PublicationGateClosed();
		if (closed) {
			// The scene pass's end wakes the builder only while it is flagged as waiting. Re-read the gate after setting the flag.
			publicationGated.store(true, std::memory_order_seq_cst);
			closed = PublicationGateClosed();
		}
		// A publication that moved only deltas passes through at once with nothing built.
		if (!closed || (b.stage == SnapshotBuilder::Stage::Idle && !snapshotInputsMoved.load(std::memory_order_acquire) && PassesThrough(b.posted, *b.held)))
			return std::move(b.held);
		++b.heldPasses;
		return nullptr;
	}

	void IndirectDraws::Impl::AddressesOf(const NamedVersions& a_sealed, const MainLayout& a_layout, const Resources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out)
	{
		NamedAddressesOf(a_sealed, a_resources, a_frame, a_out);
		a_out.sequenceDecals = a_layout.sequenceDecals;
	}

	void IndirectDraws::Impl::AddressesOf(const NamedVersions& a_sealed, const ShadowResources& a_resources, std::uint32_t a_frame, ResourceAddresses& a_out)
	{
		NamedAddressesOf(a_sealed, a_resources, a_frame, a_out);
	}

	void IndirectDraws::Impl::BuildSnapshotDraws(const NamedVersions* a_sealed, const MainLayout* a_layout)
	{
		auto& b = snapshotBuilder;
		auto& work = *b.work;
		const auto start = std::chrono::steady_clock::now();
		const auto& publication = *work.publication;
		// Draws embed buffer addresses and SRV indices. They must come from the sealed set, never from the context, or a draw may
		// read a version that was released after a growth, which loses the device.
		AheadContext context = work.context;
		// A context made under other toggles has a stale shadow part, so drop it and leave the shadow views to the engine.
		if (context.toggles != publication.toggles) {
			context.shadow = false;
			context.shadowTarget.reset();
			context.valid = context.build[kAsyncZPrepass] || context.build[kAsyncColour];
		}
		if (a_sealed && a_layout) {
			for (const std::size_t j : { kAsyncZPrepass, kAsyncColour })
				if (context.build[j] && context.target && context.target->scene)
					AddressesOf(*a_sealed, *a_layout, *context.target, context.inputs[j].frameNumber, context.inputs[j].addresses);
			if (context.shadow && context.shadowTarget && context.shadowTarget->scene)
				AddressesOf(*a_sealed, *context.shadowTarget, context.shadowInputs.frameNumber, context.shadowInputs.addresses);
		}
		b.drawsSerial = a_sealed ? a_sealed->Serial() : 0;
		std::shared_ptr<const DrawPublication> built;
		aheadRunning.store(true, std::memory_order_release);
		if (publication.tables && publication.lookups) {
			try {
				built = RunAhead(publication.tables, context, *publication.lookups, publication.tablesGeneration, work.frame, work.sun, work.light);
			} catch (const std::exception& e) {
				static std::atomic<std::uint32_t> logged{ 0 };
				if (logged++ < 4)
					logger::error("[DCLF] builds ahead of publication {} failed: {}; its snapshot draws nothing", publication.sequence, e.what());
			}
		}
		aheadRunning.store(false, std::memory_order_release);
		aheadRunning.notify_all();
		aheadDone.store(work.aheadNumber, std::memory_order_release);
		aheadDone.notify_all();
		b.draws = std::move(built);
		b.drawsPosted = false;
		++b.buildsStarted;
		b.payload.reset();
		b.payloadFailed = false;
		b.publicationWaited = false;
		b.timing.ahead = true;
		b.timing.aheadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	void IndirectDraws::Impl::WaitAhead()
	{
		while (aheadRunning.load(std::memory_order_acquire))
			aheadRunning.wait(true, std::memory_order_acquire);
	}

	std::shared_ptr<const IndirectDraws::Impl::DrawPublication> IndirectDraws::Impl::RunAhead(std::shared_ptr<const SceneTables> a_tables,
		const AheadContext& a_context, const Lookups& a_lookups, std::uint32_t a_generation, std::uint32_t a_frame,
		std::shared_ptr<const EntrySpace> a_sunCandidates, std::shared_ptr<const EntrySpace> a_lightCandidates)
	{
		DCLF_ZONE("CS.DCLF.BuildAhead");
		auto out = std::make_shared<DrawPublication>();
		out->tables = a_tables;
		// Journals keep changes back to the oldest position any backing still holds. With no backings, everything may go.
		const auto oldest = [](PayloadPart a_part) { return PayloadPools::OldestContentEpoch(a_part).value_or(~std::uint64_t{ 0 }); };
		const TablesContentEpochs oldestTables{ oldest(PayloadPart::Objects), oldest(PayloadPart::Extras), oldest(PayloadPart::Geometries) };
		const auto& c = a_context;
		const bool buildMain = c.valid && c.target && c.target->scene;
		// The shadow build writes into the main build's region. Its journal begins once per publication, by whichever writes first.
		if (buildMain)
			mainCache.region.begun = false;
		// Both segments share one list and read it through their view bits.
		std::array<MainInputs, 2> inputs;
		std::array<const MainInputs*, 2> segmentIn{};
		std::array<MainPayload*, 2> segmentOut{};
		for (const std::size_t j : { kAsyncZPrepass, kAsyncColour }) {
			if (!buildMain || !c.build[j])
				continue;
			MainInputs& in = inputs[j];
			in = c.inputs[j];
			in.frameNumber = a_frame;
			in.tablesGeneration = a_generation;
			in.lookupGeneration = a_lookups.generation;
			in.materialRowsOldestContentEpoch = oldest(PayloadPart::MaterialRows);
			in.pipelineRowsOldestContentEpoch = oldest(PayloadPart::PipelineRows);
			in.inputsOldestContentEpoch = oldest(PayloadPart::Inputs);
			in.tablesOldestContentEpochs = oldestTables;
			out->payloads[j] = AcquirePayload();
			segmentIn[j] = &in;
			segmentOut[j] = out->payloads[j].get();
		}
		// Stage 0 makes the stream views and runs the participants' shared halves. Stage 1 is the main build, which reads the
		// stream views only in Finish, so the two stages can run in parallel.
		auto& pairStats = mainRows.pairStats;
		const auto msSince = [](std::chrono::steady_clock::time_point a_start) {
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
		};
		const auto stagesStart = std::chrono::steady_clock::now();
		Draws::MainBuildStages mainBuild(segmentIn, *a_tables, Scene::FrameView::Get().GetFrameTables(), a_lookups, segmentOut, mainRows, &mainCache);
		static constexpr PayloadFloors kPayloadFloors{};
		const AheadBuild build{ *out, c, *a_tables, a_lookups, a_generation, a_frame, a_sunCandidates, a_lightCandidates, oldestTables, kPayloadFloors, mainCache.region,
			pairStats };
		const auto stage = [&](std::size_t a_stage) {
			const auto start = std::chrono::steady_clock::now();
			if (a_stage == 0) {
				out->streams = MakeStreamViews(objectStore, extrasStore, geometryStore, oldestTables, a_tables, *a_tables, a_generation, a_frame);
				pairStats.streamsMs += msSince(start);
				// Only when BuildPayload will follow, because every shared half must be followed by it.
				if (buildMain)
					for (auto* participant : participants)
						participant->BuildPayloadShared(build);
			} else {
				mainBuild.Run();
				pairStats.mainMs += msSince(start);
			}
		};
		// For a small change, overlapping the stages saves less than waiting on the helper task costs.
		const auto unread = [a_generation](const LogCursor& a_cursor, const auto& a_log) -> std::uint64_t {
			return a_cursor.Continues(a_log, a_generation) ? a_cursor.Unread(a_log).size() : kInlineStageChanges + 1;
		};
		const bool smallChange = unread(objectStore.cursor, a_tables->changeLog) + unread(geometryStore.cursor, a_tables->geometryLog) <= kInlineStageChanges;
		if (buildMain && !smallChange && !Policy::SerialFanout()) {
			SceneScheduler::Executor().ParallelFor("CS.DCLF.BuildAhead.Stages", 2, 1, [&](std::size_t a_begin, std::size_t a_end) {
				DCLF_ZONE("CS.DCLF.BuildAhead.StagesShards");
				for (std::size_t s = a_begin; s < a_end; ++s)
					stage(s);
			});
			++pairStats.overlapped;
			++snapshotBuilder.stagesPooled;
		} else {
			stage(0);
			if (buildMain)
				stage(1);
			snapshotBuilder.stagesInline += buildMain && smallChange ? 1u : 0u;
		}
		++pairStats.aheads;
		if (!buildMain) {
			pairStats.stagesMs += msSince(stagesStart);
			return out;
		}
		mainBuild.Finish(out->streams);
		pairStats.stagesMs += msSince(stagesStart);
		for (auto* participant : participants)
			participant->BuildPayload(build);
		ShareAheadRegion(*out, mainCache);
		Diagnostics::Notify(Diagnostics::DrawsBuiltAhead{ *out, *a_tables, a_frame });
		return out;
	}

	namespace
	{
		std::atomic<org::async::SerializedTaskPump*> builderPump{ nullptr };
	}

	void Draws::WakeSnapshotBuilder()
	{
		if (auto* pump = builderPump.load(std::memory_order_acquire))
			(void)pump->Notify();
	}

	org::async::SerializedTaskPump& IndirectDraws::Impl::SnapshotPump()
	{
		// Never destroyed, because a revision's completion may still wake it during process teardown.
		static auto* pump = [this] {
			auto* created = SceneScheduler::MakeLanePump(
				SceneScheduler::Executor(), SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "DCLF snapshot builder",
				[this] { SnapshotPass(); },
				[] { logger::error("[DCLF] the snapshot builder was refused by DCLF's executor; no scene snapshot is built from here on"); });
			builderPump.store(created, std::memory_order_release);
			return created;
		}();
		return *pump;
	}

	// The CPU pipeline's view batches must be GPU ready before the snapshot is posted. Returns true when there is nothing to wait for.
	bool IndirectDraws::Impl::ViewBatchesReady()
	{
		auto& b = snapshotBuilder;
		if ((!producer.inputs.cpuPipeline && !producer.inputs.forwardDirect) || !b.payload || !b.draws) {
			if (b.viewBatches)
				b.viewBatches->Reset();
			return true;
		}
		if (!b.viewBatches)
			b.viewBatches = std::make_shared<Draws::ViewBatchBuilder>();
		const auto& publication = *b.work->publication;
		Draws::ViewBatchInputs in;
		in.keep = std::make_shared<std::pair<std::shared_ptr<const DrawPublication>, std::shared_ptr<const ScenePayload>>>(b.draws, b.payload);
		in.tables = b.draws->tables.get();
		in.lookups = publication.lookups.get();
		in.catalog = publication.catalog.get();
		in.targetsGeneration = producer.inputs.targetsGeneration;
		in.payload = b.payload.get();
		in.main = NewestMainPayload(*b.draws);
		in.shadow = b.draws->shadow.get();
		in.uploads = producer.inputs.uploads.lock();
		in.forwardOnly = !producer.inputs.cpuPipeline;
		return b.viewBatches->Ready(in);
	}

	bool IndirectDraws::Impl::PayloadSettled()
	{
		const auto& b = snapshotBuilder;
		return (b.payload || b.payloadFailed || !PayloadWanted()) && ViewBatchesReady();
	}

	void IndirectDraws::Impl::SnapshotPass()
	{
		DCLF_ZONE("CS.DCLF.SnapshotBuilder");
		// Drop retired snapshots here, never on the render thread.
		retiredSnapshots.Drop();
		if (producerReportWanted.exchange(false, std::memory_order_acq_rel))
			producerReportSlot.Post(std::make_unique<std::string>(ProducerReport()));
		auto& b = snapshotBuilder;
		auto& rv = revisions;
		using Stage = SnapshotBuilder::Stage;
		auto msSince = [](std::chrono::steady_clock::time_point a_start) {
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
		};
		const auto takeWork = [&](std::unique_ptr<SnapshotWork> a_next) {
			const std::uint64_t sequence = a_next->publication ? a_next->publication->sequence : 0;
			if (b.lastPublication && sequence > b.lastPublication + 1)
				snapshotsCoalesced.fetch_add(sequence - b.lastPublication - 1, std::memory_order_relaxed);
			b.lastPublication = sequence;
			// Publication pacing. The coordinator publishes again once this one is taken.
			snapshotWorkTaken.store(sequence, std::memory_order_release);
			b.work = std::move(a_next);
			b.buildsDiscarded += b.draws && !b.drawsPosted ? 1u : 0u;
			b.draws.reset();
			b.timing = {};
			b.stage = Stage::Make;
			b.fresh = true;
			publicationGated.store(false, std::memory_order_relaxed);
		};
		++b.passes;
		try {
			for (;;) {
				if (b.stage == Stage::Recordings) {
					// Wait for the sealed revision's recordings. Their completion wakes the builder.
					const auto collected = rv.assembler.Collect();
					const bool failedHere = std::any_of(collected.failures.begin(), collected.failures.end(),
						[&](const auto& a_failure) { return a_failure.sequence == b.sealedSequence; });
					// The payload is published only once the frame will adopt it. A failed payload posts the snapshot without one.
					if (!b.payload && PayloadWanted())
						PublishPayload();
					(void)rv.assembler.TrySelect();
					auto lease = rv.assembler.AcquireActive();
					if (lease && lease->Sequence() >= b.sealedSequence && PayloadSettled()) {
						b.timing.recordingsMs = msSince(b.sealedAt);
						PostSnapshot(std::move(lease));
						b.stage = Stage::Idle;
					} else if (failedHere) {
						// A recording failed or was dropped by a graph build. Make the revision again rather than keep it.
						revisionReuse.valid = false;
						b.stage = Stage::Make;
					} else {
						return;
					}
					continue;
				}
				// One build per adoption. New work is taken only while the publication gate is open, and the coordinator does not
				// publish again until it is taken. The gate opening wakes the builder.
				if (auto next = WorkToBuild())
					takeWork(std::move(next));
				if (!b.work || !b.work->publication) {
					++b.idlePasses;
					return;
				}
				// Only deltas moved, so post the last snapshot again for this publication. The deltas still reach the frame through
				// the change log. Not while the inputs name a build state whose builds are pending: the snapshot would carry that state
				// and its conditions with recordings of the old builds, and the frame would run them together (plan step 10c).
				const bool inputsBuilt = !producer.inputs.build || producer.inputs.build->serial <= Draws::BuiltBuildSerial();
				if (b.stage == Stage::Make && std::exchange(b.fresh, false) && !snapshotInputsMoved.load(std::memory_order_acquire) && inputsBuilt &&
					PassesThrough(b.posted, *b.work)) {
					b.draws = b.posted.draws;
					b.payload = b.posted.payload;
					b.payloadFailed = false;
					b.drawsSerial = b.posted.drawsSerial;
					b.publicationWaited = false;
					// The frame's fallback streams wait on this, as in BuildSnapshotDraws.
					aheadDone.store(b.work->aheadNumber, std::memory_order_release);
					aheadDone.notify_all();
					++b.passedThrough;
					PostSnapshot(b.posted.revision);
					b.stage = Stage::Idle;
					continue;
				}
				if (b.stage == Stage::Idle) {
					// Remake the snapshot only when the frame's inputs moved.
					if (!snapshotInputsMoved.exchange(false, std::memory_order_acq_rel)) {
						++b.idlePasses;
						return;
					}
					b.timing = {};
				}
				snapshotInputsMoved.store(false, std::memory_order_release);
				const auto start = std::chrono::steady_clock::now();
				const std::uint64_t before = rv.assembler.LatestSequence();
				const auto& publication = *b.work->publication;
				MakeRevision(publication, publication.commitFrame);
				b.timing.shapesMs += msSince(start);
				// Either a new revision was sealed or the last one was kept.
				if (rv.assembler.LatestSequence() != before || revisionReuse.reusedNow) {
					b.sealedSequence = rv.assembler.LatestSequence();
					b.sealedAt = std::chrono::steady_clock::now();
					// Rebuild the draws when the sealed set names other versions than they embed. A snapshot must never pair draws
					// and a revision of different versions.
					const auto& versions = rv.revisionVersions;
					const bool contextReplaced = RefreshWorkContext();
					if (!b.draws || contextReplaced || b.drawsSerial != versions->Serial())
						BuildSnapshotDraws(versions.get(), rv.revisionLayout.get());
					if (b.draws)
						Diagnostics::Notify(Diagnostics::SnapshotSealed{ *versions, *b.draws, b.work->context });
					b.payloadFailed = false;
					PublishPayload();
					b.stage = Stage::Recordings;
					continue;
				}
				// Nothing sealed. Without claims or main resources, post the publication alone so the frame still gets its tables.
				// Otherwise a growth or build is pending and will wake the builder.
				if (!producer.inputs.claims || !producer.inputs.main) {
					if (!b.draws)
						BuildSnapshotDraws(nullptr, nullptr);
					PostSnapshot(nullptr);
					b.stage = Stage::Idle;
					return;
				}
				b.stage = Stage::Waiting;
				return;
			}
		} catch (const std::exception& e) {
			static std::atomic<std::uint32_t> logged{ 0 };
			if (logged++ < 4)
				logger::error("[DCLF] the snapshot builder's pass failed: {}; it makes the snapshot again when woken", e.what());
			b.stage = b.work ? Stage::Waiting : Stage::Idle;
			// The wake this pass needs may already have been used by the pass that just failed. Wake again so the new inputs
			// are not missed. The next pass clears the flag first, so this cannot loop.
			if (b.work && snapshotInputsMoved.load(std::memory_order_acquire))
				WakeSnapshotBuilder();
		}
	}

	bool IndirectDraws::Impl::PayloadWanted() const
	{
		const auto& b = snapshotBuilder;
		return b.draws && b.draws->streams && (NewestMainPayload(*b.draws) || b.draws->shadow);
	}

	void IndirectDraws::Impl::PublishPayload()
	{
		auto& b = snapshotBuilder;
		if (b.payload || b.payloadFailed || !PayloadWanted())
			return;
		// Each part has one source in flight at a time. Wait for it to settle first.
		if (!PayloadPools::Settle())
			return;
		if (auto source = std::move(b.payloadSource); source && source->keep == b.draws) {
			b.payload = PayloadPools::Finish(*source);
			b.payloadFailed = !b.payload;
			return;
		}
		// Publish only a payload the frame will adopt, so none is built for nothing. Wait until the frame has taken the last
		// snapshot and its scene pass has ended.
		if (PublicationGateClosed()) {
			// The scene pass's end wakes the builder only while it is flagged as waiting. Re-read the gate after setting the flag.
			publicationGated.store(true, std::memory_order_seq_cst);
			if (PublicationGateClosed()) {
				b.publicationsWaited += std::exchange(b.publicationWaited, true) ? 0u : 1u;
				return;
			}
		}
		publicationGated.store(false, std::memory_order_relaxed);
		auto source = PayloadPools::MakeSource(b.draws, *b.draws->streams, NewestMainPayload(*b.draws), b.draws->shadow.get());
		if (!source || !PayloadPools::Post(source, producer.inputs.uploads.lock())) {
			b.payloadFailed = true;
			return;
		}
		if (PayloadPools::Settle()) {
			b.payload = PayloadPools::Finish(*source);
			b.payloadFailed = !b.payload;
			return;
		}
		b.payloadSource = std::move(source);
	}

	void IndirectDraws::Impl::PostSnapshot(org::async::RevisionAssembler::Lease a_revision)
	{
		auto& b = snapshotBuilder;
		const auto& inputs = producer.inputs;
		auto snapshot = std::make_shared<SceneSnapshot>();
		snapshot->serial = ++b.serial;
		snapshot->publication = b.work->publication;
		snapshot->draws = b.draws;
		b.drawsPosted = true;
		snapshot->payload = b.payload;
		snapshot->revision = std::move(a_revision);
		// Complete means both main payloads exist for the current main resources. Without that, frames make no claims.
		bool complete = b.draws != nullptr && inputs.main != nullptr;
		for (const std::size_t j : { kAsyncZPrepass, kAsyncColour })
			complete = complete && b.draws->payloads[j] && b.draws->payloads[j]->inputs.addresses.identity == inputs.main.get();
		snapshot->drawsComplete = complete && snapshot->revision != nullptr;
		if (!snapshot->drawsComplete)
			snapshotsWithoutDraws.fetch_add(1, std::memory_order_relaxed);
		if (!snapshot->drawsComplete && RenderGraph::Engine::Native::Active()) {
			static std::atomic_flag logged;
			if (!logged.test_and_set()) {
				const auto payloadState = [&](std::size_t a_index) -> std::string {
					if (!b.draws)
						return "no draws";
					const auto& payload = b.draws->payloads[a_index];
					if (!payload)
						return "none";
					return payload->inputs.addresses.identity == inputs.main.get() ? "built" : "built for other main resources";
				};
				logger::warn("[DCLF] native: snapshot {} has no complete draws: draws {}, main resources {}, revision {}, Z-prepass payload {}, colour payload {}",
					snapshot->serial, b.draws != nullptr, inputs.main != nullptr, snapshot->revision != nullptr, payloadState(kAsyncZPrepass), payloadState(kAsyncColour));
			}
		}
		snapshot->buildGeneration = inputs.buildGeneration;
		snapshot->epochBuilds = inputs.epochBuilds;
		snapshot->build = inputs.build;
		snapshot->mainGeneration = inputs.mainGeneration;
		// The frame versions its recordings named: those its versions fragment was made for. A revision of another fragment (one
		// posted again) names unknown ones, so the frame drops it and a new one is made.
		if (snapshot->revision) {
			const auto& fragment = snapshot->revision->Fragment(SceneRevisions::kVersionsSlot);
			snapshot->frameVersions = fragment && fragment == revisions.versionsFragment ? revisions.frameVersions : kUnknownFrameVersions;
		}
		// A forward-only batch set is only valid for the GPU-driven graph. The CPU pipeline's sites draw every list.
		if ((inputs.cpuPipeline || inputs.forwardDirect) && b.payload) {
			if (const auto& set = b.viewBatches ? b.viewBatches->Set() : nullptr; set && b.viewBatches->For() == b.payload.get()) {
				if (!set->serial)
					set->serial = snapshot->serial;
				snapshot->viewBatches = set;
			} else if (b.posted.payload == b.payload && b.posted.viewBatches && b.posted.viewBatches->forwardOnly == !inputs.cpuPipeline) {
				snapshot->viewBatches = b.posted.viewBatches;
			}
		}
		// Remember what this snapshot was built from, so a deltas-only publication can pass through to it.
		auto& posted = b.posted;
		if (snapshot->revision && snapshot->drawsComplete && snapshot->payload && b.work->context.toggles == b.work->publication->toggles) {
			const auto& context = b.work->context;
			posted.publication = b.work->publication;
			posted.sun = b.work->sun;
			posted.light = b.work->light;
			posted.draws = b.draws;
			posted.payload = b.payload;
			posted.viewBatches = snapshot->viewBatches;
			posted.revision = snapshot->revision;
			posted.drawsSerial = b.drawsSerial;
			posted.valid = context.valid;
			posted.shadow = context.shadow;
			posted.build = context.build;
			posted.target = context.target.get();
			posted.shadowTarget = context.shadowTarget.get();
			for (const std::size_t j : { kAsyncZPrepass, kAsyncColour }) {
				posted.vsMask[j] = context.inputs[j].vsFrameMask;
				posted.psMask[j] = context.inputs[j].psFrameMask;
			}
		} else {
			posted = {};
		}
		// The frame revision adopts it as its scene fragment (plan step 10b). Latest wins, and a replaced one is dropped here on the
		// builder. Count after the post, because a waiting frame polls the count.
		RenderGraph::Frame::Revision::Scene posting;
		posting.serial = snapshot->serial;
		if (snapshot->revision) {
			if (const auto versions = snapshot->revision->Get<NamedVersions>(SceneRevisions::kVersionsSlot))
				posting.buffersSerial = versions->Serial();
			posting.frameVersions = snapshot->frameVersions;
			for (std::uint32_t segment = 0; segment < static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count); ++segment)
				if (const auto kind = static_cast<RenderGraphRuntime::Segment>(segment); RenderGraphRuntime::IsDclfSegment(kind))
					posting.builds.emplace_back(RenderGraphRuntime::EpochOf(kind), snapshot->epochBuilds[segment]);
		}
		posting.value = std::move(snapshot);
		RenderGraph::Frame::Revision::PublishScene(std::move(posting));
		snapshotsPosted.fetch_add(1, std::memory_order_release);
		snapshotsBuilt.fetch_add(1, std::memory_order_relaxed);
		snapshotTimings.Push(b.timing);
	}

	std::shared_ptr<const void> IndirectDraws::AdoptedPublication() const
	{
		return impl && impl->adoptedSnapshot ? impl->adoptedSnapshot->publication : nullptr;
	}

	void IndirectDraws::SceneTaken(const std::shared_ptr<const void>& a_scene)
	{
		if (!impl)
			return;
		auto& s = *impl;
		s.sceneTakenThisBegin = true;
		const auto* snapshot = static_cast<const Impl::SceneSnapshot*>(a_scene.get());
		// At Begin the frame view still counts the last frame. The scene is for the next one (plan Q7).
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame() + 1;
		// The post is free, so the builder may publish the next payload after this frame's scene pass. A failed feature's posts have
		// no serial.
		if (snapshot && snapshot->serial) {
			s.snapshotTakenFrame.store(frame, std::memory_order_release);
			s.snapshotTaken.store(snapshot->serial, std::memory_order_release);
		}
	}

	void IndirectDraws::InstallScene(const std::shared_ptr<const void>& a_scene)
	{
		if (!impl || !a_scene)
			return;
		auto& s = *impl;
		DCLF_ZONE("CS.DCLF.Snapshot.Install");
		auto snapshot = std::static_pointer_cast<const Impl::SceneSnapshot>(a_scene);
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame() + 1;
		// A scene recorded on other builds than the adopted one's took them with it (its staged builds attached just before).
		if (snapshot->revision && s.adoptedSnapshot && s.adoptedSnapshot->revision && snapshot->epochBuilds != s.adoptedSnapshot->epochBuilds)
			++s.stagedAdoptions;
		s.restagesWithoutInput = 0;
		// The frame takes the conditions and build state of the graphs it now runs.
		if (snapshot->build && snapshot->build->serial > Draws::AttachedBuildSerial())
			Draws::PublishAttachedBuildState(snapshot->build);
		TracyCZoneN(adoptVersions, "CS.DCLF.Snapshot.AdoptVersions", true);
		const auto versions = snapshot->revision ? snapshot->revision->Get<NamedVersions>(Impl::SceneRevisions::kVersionsSlot) : nullptr;
		if (versions && versions->AdoptAll()) {
			if (auto* host = RenderGraphRuntime::Get().Host())
				host->NoteNewVersions();
			if (s.growthAdoptedFrame == ~0u)
				s.growthAdoptedFrame = frame;
		}
		TracyCZoneEnd(adoptVersions);
		s.revisions.active = snapshot->revision;
		s.adoptedSet = versions.get();
		s.installedDraws = snapshot->draws;
		if (s.installedPayload != snapshot->payload) {
			DCLF_ZONE("CS.DCLF.Snapshot.InstallPayload");
			// Keep the replaced versions from reuse until submitted work retires, because a frame without EndFrame may have read them.
			PayloadPools::BindInstalled(SubmittedRetirementPoint());
			s.installedPayload = snapshot->payload;  // the snapshot holds it too, so the builder does the final release
			if (s.installedPayload)
				s.payloadsInstalled.fetch_add(1, std::memory_order_relaxed);
			PayloadPools::Install(s.installedPayload);
			Extensions::InstallPayloadProducts(s.installedPayload ? &s.installedPayload->products : nullptr);
		}
		s.snapshotStale = false;
		++s.snapshotsAdopted;
		s.CheckSceneVersions(*snapshot);
		if (Diagnostics::Armed<Diagnostics::SnapshotAdopted>())
			Diagnostics::Notify(Diagnostics::SnapshotAdopted{ frame, snapshot->serial, versions.get(), snapshot->payload.get(), s.growthAdoptedFrame == frame });
		if (const auto& publication = snapshot->publication)
			s.adoptionLatency.push_back(frame >= publication->commitFrame ? frame - publication->commitFrame : 0u);
		// Hand the replaced snapshot back so it is released on the builder, not here.
		{
			DCLF_ZONE("CS.DCLF.Snapshot.RetireReplaced");
			if (auto replaced = std::exchange(s.adoptedSnapshot, std::move(snapshot)))
				s.retiredSnapshots.Push(std::move(replaced));
		}
		WakeSnapshotBuilder();
	}

	void IndirectDraws::DropScene(const std::shared_ptr<const void>& a_scene, RenderGraph::Frame::Revision::SceneDrop a_why, std::string_view a_text,
		std::span<const std::uint32_t> a_epochs)
	{
		if (!impl || !a_scene)
			return;
		using Drop = RenderGraph::Frame::Revision::SceneDrop;
		auto& s = *impl;
		auto snapshot = std::static_pointer_cast<const Impl::SceneSnapshot>(a_scene);
		const auto kind = static_cast<std::size_t>(a_why);
		++s.scenesDropped[kind];
		s.snapshotStale = true;
		static constexpr std::array<const char*, 4> kWhy{ "recorded against other frame versions", "recorded on a replaced build", "its staged builds were refused",
			"superseded" };
		if (++s.scenesDroppedLogged <= 16 || s.scenesDroppedLogged % 100 == 0)
			logger::info("[DCLF] scene {} not adopted ({}): {} ({} not adopted)", snapshot->serial, kWhy[kind], a_text, s.scenesDroppedLogged);
		if (a_why == Drop::Refused) {
			// The host's configuration changed since the builds started. Build them again, and a scene is made on the new ones.
			++s.stagedRefusals;
			if (++s.stagedRefusalsLogged <= 8 || s.stagedRefusalsLogged % 100 == 0)
				logger::warn("[DCLF] snapshot {}'s staged builds were refused at its adoption ({}); they are built again ({} refusals)", snapshot->serial, a_text,
					s.stagedRefusalsLogged);
			for (const auto epoch : a_epochs)
				RenderGraph::Builds::Request(epoch, "Drawcall Limit Fix: its staged build was refused at a snapshot's adoption");
		}
		// A build that is staged again and again and never switches in (the open item of plan step 10a, like the anchors' Starved).
		// A new build state is a new input. Without one, scenes dropped for their builds this often are a schedule bug.
		if (a_why == Drop::BuildReplaced || a_why == Drop::Refused) {
			const std::uint64_t stateSerial = s.buildState ? s.buildState->serial : 0;
			if (stateSerial != s.restageInputSerial) {
				s.restageInputSerial = stateSerial;
				s.restagesWithoutInput = 0;
			}
			if (++s.restagesWithoutInput > Impl::kRestageStarvedAfter) {
				RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::Starved, RenderGraph::Frame::SegmentId::Count,
					fmt::format("DCLF's staged builds have not switched in: {} scenes in a row were dropped for their builds with no new build state "
								"(serial {}); last: scene {}, {}",
						s.restagesWithoutInput, stateSerial, snapshot->serial, a_text));
				s.restagesWithoutInput = 0;
			}
		}
		s.retiredSnapshots.Push(std::move(snapshot));
		// Ask the builder for a new one.
		s.snapshotInputsMoved.store(true, std::memory_order_release);
		WakeSnapshotBuilder();
	}

	std::uint64_t IndirectDraws::SceneRecordedFrames() const
	{
		return impl ? impl->sceneRecordedFrames.load(std::memory_order_acquire) : 0;
	}

	void IndirectDraws::Impl::CheckSceneVersions(const SceneSnapshot& a_snapshot)
	{
		// Each epoch's recordings bind the versions the frame now reads, DCLF's buffers and the frame's textures (plan step 10b).
		// Checked once per scene, on each epoch's first recording and its first copy.
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !a_snapshot.revision)
			return;
		using R = SceneRevisions;
		for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e) {
			const auto recordings = a_snapshot.revision->Get<RevisionRecordings>(R::kRecordingSlot + e);
			if (!recordings || recordings->recordings.empty() || !recordings->recordings.front())
				continue;
			const auto& recording = *recordings->recordings.front();
			if (recording.copies.empty() || host->EpochRecordingVersionsCurrent(recording, 0))
				continue;
			++staleSceneVersions;
			if (staleSceneVersions <= 8 || staleSceneVersions % 100 == 0)
				logger::warn("[DCLF] scene {}: the {} epoch's recording binds a version the frame does not read: {} ({} in all)", a_snapshot.serial,
					EpochTraitsOf(e).name, host->EpochRecordingStaleVersion(recording, 0), staleSceneVersions);
		}
	}

	bool IndirectDraws::Impl::AdoptedCurrent() const
	{
		const auto& adopted = adoptedSnapshot;
		if (!adopted || !adopted->revision || !adopted->drawsComplete || !installedPayload)
			return false;
		// Its main payloads are for the main resources the frame runs. Those change only before DCLF first draws (plan step 10a-0).
		for (const std::size_t j : { kAsyncZPrepass, kAsyncColour })
			if (!adopted->draws || !adopted->draws->payloads[j] || adopted->draws->payloads[j]->inputs.addresses.identity != resources.get())
				return false;
		// Each epoch's recordings are of the build attached now: the scene's staged builds attached with it.
		if (auto* host = RenderGraphRuntime::Get().Host())
			for (std::uint32_t segment = 0; segment < static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count); ++segment) {
				const auto kind = static_cast<RenderGraphRuntime::Segment>(segment);
				if (RenderGraphRuntime::IsDclfSegment(kind) && adopted->epochBuilds[segment] != host->BuildGeneration(RenderGraphRuntime::EpochOf(kind)))
					return false;
			}
		return true;
	}

	void IndirectDraws::AfterSceneStep()
	{
		lastAdoptWaitUs = 0;
		if (!impl)
			return;
		auto& s = *impl;
		// The graph conditions follow the variables this Begin's revision adopted, so their builds are asked for inside the adoption
		// point, never in the frame (plan step 10c).
		if (auto* host = RenderGraphRuntime::Get().Host(); host && RenderGraphRuntime::Get().IsActive()) {
			s.RegisterGraphBuilds();
			Draws::UpdateGraphConditions(*host);
		}
		if (!std::exchange(s.sceneTakenThisBegin, false))
			++s.snapshotFramesKept;
		// A shader reload's pipeline swap rebuilt every graph in a Begin, so the adopted snapshot's recordings are of the old builds.
		// Until recordings name their pipelines, this is the one case a Begin waits (director, item 57). A swap made while DCLF does
		// not draw is waited for at the first Begin that draws, unless a current snapshot came first.
		const std::uint64_t swaps = RenderGraphRuntime::Get().PipelineSwaps();
		if (swaps != std::exchange(s.pipelineSwapsSeen, swaps))
			s.pipelineSwapPending = true;
		if (!s.pipelineSwapPending)
			return;
		if (failed || !RenderGraphRuntime::Get().Host() || s.AdoptedCurrent()) {
			s.pipelineSwapPending = false;
			return;
		}
		if (Scene::FrameView::Get().NotDrawingYet() || Scene::FrameView::IsLoadingScreenUp())
			return;
		s.pipelineSwapPending = false;
		// UINT64_MAX: a scene posted before the wait started is taken in its first pass too.
		const double ms = s.WaitAfterPipelineSwap(UINT64_MAX);
		lastAdoptWaitUs = static_cast<std::uint32_t>((std::min)(ms * 1000.0, 4.0e9));
	}

	double IndirectDraws::Impl::WaitAfterPipelineSwap(std::uint64_t a_postedBefore)
	{
		DCLF_ZONE("CS.DCLF.WaitAfterPipelineSwap");
		auto& s = *this;
		auto* host = RenderGraphRuntime::Get().Host();
		using Clock = std::chrono::steady_clock;
		// A snapshot normally takes milliseconds, so five idle seconds means the builder is stuck. Compiles share the builder's
		// threads, so the five seconds count from the last time no compile ran. No wait is longer than kCatalogTimeout.
		constexpr auto kTimeout = std::chrono::seconds(5);
		constexpr auto kCatalogTimeout = std::chrono::seconds(120);
		auto progressAt = std::chrono::seconds(5);
		constexpr auto kSpin = std::chrono::milliseconds(2);
		constexpr auto kTicketPeriod = std::chrono::milliseconds(16);
		const auto start = Clock::now();
		auto ticketsAt = start;
		auto busyAt = start;     // last poll that saw a shader compile or pipeline build running
		std::size_t busyBuilds = 0;
		bool busyCompiling = false;
		const auto droppedBefore = s.scenesDropped;
		const std::uint64_t wait = ++s.shaderReloadWaits;
		// The frame this Begin starts.
		const std::uint32_t frame = Scene::FrameView::Get().GetFrame() + 1;
		// A frame revision with DCLF epochs in preparation holds DCLF's scenes until it adopts at a later Begin, so this wait would
		// get none (plan step 10b). It is superseded: its inputs are prepared again from the next Begin, on the rebuilt graphs, and
		// the scenes this wait takes name the adopted versions. PostRevisionInputs below carries them to the builder.
		const std::uint64_t superseded = RenderGraph::Frame::Revision::SupersedeForSceneWait(
			fmt::format("shader-reload wait {} needs DCLF's scene on the adopted frame versions", wait));
		s.shaderReloadSupersessions += superseded ? 1 : 0;
		// Open the scene pass half of the publication gate, because this frame's pass only runs after its start.
		s.frameWaiting.store(true, std::memory_order_seq_cst);
		// The payload may wait on a backing the last frame read, so submit queued work now.
		Direct::SubmitQueued();
		(void)s.SnapshotPump();
		s.snapshotInputsMoved.store(true, std::memory_order_release);
		WakeSnapshotBuilder();
		std::uint64_t seen = a_postedBefore;
		for (;;) {
			// Serve the owner thread's build point work while waiting, since nothing else will run it during this wait.
			// A poll here stages a build, which attaches with the snapshot recorded on it.
			s.ServeRevisionRequests();
			if (s.revisions.explicitBuilds && (host->RebuildRequested() || !RenderGraphRuntime::Get().DclfBuildsSettled()))
				s.BuildIfAsked();
			s.PostRevisionInputs();
			// The graph stopped after a host failure, so no snapshot will come.
			if (!RenderGraphRuntime::Get().IsActive()) {
				s.frameWaiting.store(false, std::memory_order_release);
				return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
			}
			if (const auto now = Clock::now(); now - ticketsAt >= kTicketPeriod) {
				// A failed epoch means no snapshot will ever be current. Fail clearly now instead of after a timeout.
				RenderGraphRuntime::Get().PollEpochFailures();
				if (const auto& failure = DrawcallLimitFix::GraphEpochFailure(); !failure.empty())
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: frame {} waited at its Begin after a shader reload's pipeline swap for a scene snapshot, and "
													 "{}. DCLF does not hand drawing back to the engine within a frame it draws",
						frame, failure));
				s.ReleaseTicketsForRecordings(*host, 0);
				// Flush DXVK's stream. A recording waits for each slot's last run, and a run still in this thread's stream never
				// completes until flushed, so the wait would never end.
				Direct::SubmitQueued(true);
				// Guards against a lost wake on ORG's host thread.
				(void)host->NudgeHostThread();
				Direct::ReleaseCompletedSlots();
				PollHostRetirement();
				busyBuilds = DrawPipelines::Get().BuildsInFlight();
				busyCompiling = SIE::ShaderCache::Instance().IsCompiling();
				if (busyBuilds != 0 || busyCompiling)
					busyAt = now;
				ticketsAt = now;
			}
			if (const std::uint64_t posted = s.snapshotsPosted.load(std::memory_order_acquire); posted != seen) {
				seen = posted;
				// The frame revision takes the scene, inside this Begin's adoption point (plan step 10b).
				(void)RenderGraph::Frame::Revision::AdoptScene();
				if (s.AdoptedCurrent())
					break;
			}
			const auto checkedAt = Clock::now();
			const auto waited = checkedAt - start;
			const auto idle = checkedAt - busyAt;
			if (waited > progressAt && waited <= kCatalogTimeout) {
				if (idle < kTimeout)
					logger::info("[DCLF] shader-reload wait {}: frame {} has waited {} s at its Begin for a scene snapshot while shaders compile ({} pipeline "
								 "builds in flight, Community Shaders' compile workers {})",
						wait, frame, std::chrono::duration_cast<std::chrono::seconds>(waited).count(), busyBuilds, busyCompiling ? "busy" : "idle");
				progressAt += std::chrono::seconds(5);
			}
			if (idle > kTimeout || waited > kCatalogTimeout) {
				const auto& adopted = s.adoptedSnapshot;
				const auto pumpStats = s.SnapshotPump().GetStats();
				const auto assembler = s.revisions.assembler.GetStats();
				const auto active = s.revisions.assembler.AcquireActive();
				std::string stale;
				for (std::size_t k = 0; k < s.scenesDropped.size(); ++k)
					stale += fmt::format("{}{}", k ? "/" : "", s.scenesDropped[k] - droppedBefore[k]);
				stl::report_and_fail(fmt::format(
					"Drawcall Limit Fix: frame {} waited {} s at its Begin after a shader reload's pipeline swap for a scene snapshot recorded on the rebuilt graphs and "
					"never got one, and DCLF does not hand drawing back to the engine (no shader compile or pipeline build ran for the last {} s). Waiting for: build "
					"generation {} (rebuild requested: {}), main resources generation {}, targets generation {}. Scenes not adopted meanwhile (stale/replaced "
					"build/refused/superseded): {}; snapshots posted {}, recordings outstanding {}, inputs moved {}, publication gated {}, build ahead running {}. Adopted snapshot: "
					"serial {}, revision {}, draws complete {}, build generation {}, main generation {}, payload {}. The builder (read racily, for this message): stage "
					"{}, passes {} ({} idle), work {}, draws {}, payload {} (failed {}, source in flight {}, parts in flight: {}), sealed sequence {}, assembler latest "
					"{} pending {} (sealed {}, published {}, failed {}, superseded {}, abandoned {}), active {}; its pump: runner active {}, requested epoch {}, drained "
					"{}, notifications {}, passes {}, rejected {}; retirement: {}. ORG's {}",
					frame, std::chrono::duration_cast<std::chrono::seconds>(waited).count(), std::chrono::duration_cast<std::chrono::seconds>(idle).count(),
					Impl::GraphBuildGeneration(*host), host->RebuildRequested(), s.postedInputs.mainGeneration, DrawPipelines::Get().Generation(), stale, seen,
					s.recordingsOutstanding.load(std::memory_order_acquire), s.snapshotInputsMoved.load(std::memory_order_acquire),
					s.publicationGated.load(std::memory_order_acquire), s.aheadRunning.load(std::memory_order_acquire), adopted ? adopted->serial : 0,
					adopted && adopted->revision, adopted && adopted->drawsComplete, adopted ? adopted->buildGeneration : 0, adopted ? adopted->mainGeneration : 0,
					!s.installedPayload ? "none" : "installed", static_cast<int>(s.snapshotBuilder.stage), s.snapshotBuilder.passes, s.snapshotBuilder.idlePasses,
					s.snapshotBuilder.work != nullptr, s.snapshotBuilder.draws != nullptr, s.snapshotBuilder.payload != nullptr, s.snapshotBuilder.payloadFailed,
					s.snapshotBuilder.payloadSource != nullptr, PayloadPools::PendingParts(), s.snapshotBuilder.sealedSequence, s.revisions.assembler.LatestSequence(),
					s.revisions.assembler.Pending(), assembler.sealed, assembler.published, assembler.failed, assembler.superseded, assembler.abandoned,
					active ? active->Sequence() : 0, pumpStats.runnerActive, pumpStats.requestedEpoch, pumpStats.drainedEpoch, pumpStats.notifications,
					pumpStats.drainPasses, pumpStats.rejected, RetirementState(), host->DescribeAsyncState()));
			}
			if (waited < kSpin)
				std::this_thread::yield();
			else
				std::this_thread::sleep_for(std::chrono::microseconds(250));
		}
		s.frameWaiting.store(false, std::memory_order_release);
		const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		++s.snapshotWaits;
		s.snapshotWaitMs += ms;
		s.snapshotWaitMaxMs = std::max(s.snapshotWaitMaxMs, ms);
		logger::info("[DCLF] shader-reload wait {}: frame {} waited {:.2f} ms at its Begin after the pipeline swap for a snapshot recorded on the rebuilt "
					 "graphs (build generation {}, main resources generation {}): adopted serial {}; frame revision superseded for it: {} ({} in the "
					 "session)",
			wait, frame, ms, Impl::GraphBuildGeneration(*host), s.postedInputs.mainGeneration, s.adoptedSnapshot->serial,
			superseded ? fmt::format("{}", superseded) : std::string("none"), s.shaderReloadSupersessions);
		return ms;
	}

	void IndirectDraws::NoteScenePassDone(std::uint32_t a_frame)
	{
		if (!impl)
			return;
		// Wake the builder only if it is waiting on this gate.
		impl->scenePassDoneFrame.store(a_frame, std::memory_order_seq_cst);
		if (impl->publicationGated.load(std::memory_order_seq_cst))
			WakeSnapshotBuilder();
		else
			impl->passDoneWakesSkipped.fetch_add(1, std::memory_order_relaxed);
	}

	std::uint32_t IndirectDraws::ScenePassDoneFrame() const
	{
		return impl ? impl->scenePassDoneFrame.load(std::memory_order_acquire) : 0;
	}

	std::uint64_t IndirectDraws::SnapshotWorkTaken() const
	{
		return impl ? impl->snapshotWorkTaken.load(std::memory_order_acquire) : ~0ull;
	}

	void IndirectDraws::BindAdopted()
	{
		// Call after the frame's last submission, so the point covers the frame's own GPU work. Bind even when stale.
		if (!impl)
			return;
		const std::uint64_t point = SubmittedRetirementPoint();
		if (Diagnostics::Armed<Diagnostics::AdoptedBound>())
			Diagnostics::Notify(Diagnostics::AdoptedBound{ Scene::FrameView::Get().GetFrame(), point, FrameRetirement()->Completed() });
		if (impl->adoptedSet)
			impl->adoptedSet->Bind(point);
		PayloadPools::BindInstalled(point);
		impl->framePayloadMade = false;
	}

	bool IndirectDraws::MakeFramePayload()
	{
		if (!impl)
			return false;
		impl->framePayloadMade = PayloadPools::MakeFramePayload(impl->framePayload);
		return impl->framePayloadMade;
	}

	void IndirectDraws::NoteEventAdoption(double a_ms, std::uint32_t a_frames)
	{
		if (!impl)
			return;
		impl->eventAdoptionMs.push_back(a_ms);
		impl->eventAdoptionFrames.push_back(a_frames);
	}
}
