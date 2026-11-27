// The coordinator's event intake on the scene lane. CollectEvents drains every queue into the pass's batch, ApplyEvents applies
// it with the mirror first, and the retired batch goes back through the retirement chain.

#include "Internal.h"
#include "Features/DrawcallLimitFix/Core/Engine/SceneClock.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Render/LoadHold.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include "TextureStreaming/Streamer.h"

#include "RenderGraph/Frame/FrameDriver.h"

namespace DCLF::Scene
{
	namespace
	{
		using ChannelKeys = std::array<std::vector<const void*>, Extensions::kEngineChannelSlots>;

		// A SceneStack channel has no queue, since its events are on the scene's stack.
		void DrainChannels(Api::ChannelApply a_apply, ChannelKeys& a_keys)
		{
			const auto engineChannels = Extensions::RegisteredEngineChannels();
			for (std::size_t k = 0; k < engineChannels.size(); ++k)
				if (engineChannels[k].apply == a_apply && engineChannels[k].kind != Api::ChannelKind::SceneStack) {
					Api::KeySink sink(&a_keys[k]);
					engineChannels[k].drain(sink);
				}
		}


		std::size_t ChannelKeyCount(const ChannelKeys& a_keys)
		{
			std::size_t count = 0;
			for (const auto& keys : a_keys)
				count += keys.size();
			return count;
		}
	}

	struct Coordinator::BatchIndex
	{
		std::vector<SceneTracker::Event*> structural;      // head's attach and detach events, not mirror-only, in order
		std::vector<const SceneCapture::Records*> leaves;  // both heads' captures and leaf updates' records, in order
		bool sawDetach = false;                            // one of structural is a detach
	};

	std::shared_ptr<Coordinator::EventBatch> Coordinator::CollectEvents()
	{
		DCLF_ZONE("CS.DCLF.Scene.CollectEvents");
		// Each queue is drained only to where it stands now, so a pass always ends. The render thread's posts were taken before, so every
		// event older than its capture is in this batch or an earlier one.
		auto& tracker = SceneTracker::Get();
		// During a load the batch applies as usual. Only the pacing differs, one pass a Present and frame start.
		if (channels.loadingSeen.load(std::memory_order_acquire))
			++pumpStats.loading;
		Diagnostics::Notify(Diagnostics::EventsCollecting{ *this });
		auto batch = std::make_shared<EventBatch>();
		// A throw hands the batch and its engine references to the render thread, never destroys it here.
		BatchUnwindGuard unwindGuard(*this, batch);
		// Their writers push the mirror update first, so it is always in this batch or an earlier one.
		DrainChannels(Api::ChannelApply::BeforeStructural, batch->channelKeys);
		// Only pointer moves here, in push order.
		auto* drained = tracker.Drain();
		batch->Append(drained);
		DrainPropertyEvents(batch->properties);
		DrainNodeEvents(batch->nodes);
		switchEvents.Drain([&](SwitchEvent&& a_event) { batch->switches.push_back(std::move(a_event)); });
		DrainChannels(Api::ChannelApply::AfterSwitches, batch->channelKeys);
		// A detach forces the next category capture, since it can take a category node before the signature sees its cell go.
		// Switch catch-ups are engine code that writes, so the render thread runs them at frame start.
		std::shared_ptr<SwitchCatchUp> catchUp;
		// On a throw its reference copies go to the render thread, never dropped here.
		struct CatchUpUnwind
		{
			std::shared_ptr<SwitchCatchUp>& held;
			int exceptions = std::uncaught_exceptions();
			~CatchUpUnwind()
			{
				if (!held || std::uncaught_exceptions() <= exceptions)
					return;
				for (auto& node : held->attached)
					EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(node)));
				for (auto& event : held->switches)
					EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(event.node)));
			}
		} catchUpUnwind{ catchUp };
		std::uint32_t events = 0;
		for (const auto* event = drained; event; event = event->next) {
			++events;
			if (event->stampNs && (!batch->oldestNs || event->stampNs < batch->oldestNs)) {
				batch->oldestNs = event->stampNs;
				batch->oldestFrame = event->stampFrame;
			}
			if (event->mirrorOnly)
				continue;
			if (event->type == SceneTracker::EventType::Detached && !event->unheldExit)
				channels.categoryDetachSeen.store(true, std::memory_order_relaxed);
			else if (event->type == SceneTracker::EventType::Attached && event->node) {
				if (!catchUp)
					catchUp = std::make_shared<SwitchCatchUp>();
				catchUp->attached.push_back(event->node);
			}
		}
		if (!batch->switches.empty()) {
			if (!catchUp)
				catchUp = std::make_shared<SwitchCatchUp>();
			catchUp->switches.reserve(batch->switches.size());
			for (const auto& event : batch->switches)
				catchUp->switches.push_back(SwitchEvent{ event.node, event.before, event.structural });
		}
		if (catchUp)
			channels.switchCatchUps.Push(std::move(catchUp));
		events += static_cast<std::uint32_t>(batch->properties.size() + batch->nodes.size() + batch->switches.size() + ChannelKeyCount(batch->channelKeys));
		pumpStats.passEvents.push_back(events);
		return batch;
	}

	void Coordinator::HandBackHeld(std::unique_ptr<MaterialPort::HeldSnapshot>&& a_held)
	{
		if (!a_held || !a_held->reference)
			return;
		MaterialReference reference;
		reference.Adopt(std::exchange(a_held->reference, nullptr));
		EngineReleases::PushOwner(std::make_shared<MaterialReference>(std::move(reference)));
	}

	void Coordinator::DrainMaterialCaptures()
	{
		auto& s = materialSnapshotStats;
		struct AlphaWritten
		{
			const RE::BSShaderMaterial* material;
			float alpha;
			std::uint64_t sequence;
		};
		std::vector<AlphaWritten> alphaWritten;
		constexpr std::size_t kMaterialAlpha = offsetof(RE::BSLightingShaderMaterialBase, materialAlpha);
		MaterialPort::captures.Drain([&](std::unique_ptr<MaterialPort::HeldSnapshot>&& a_held) {
			++s.captured;
			const RE::BSShaderMaterial* material = a_held->reference;
			auto& entry = materialSnapshots[a_held->reference];
			if (entry.held && entry.held->sequence > a_held->sequence) {
				++s.stale;
				HandBackHeld(std::move(a_held));
				return;
			}
			if (a_held->snapshot.port == 0 && a_held->snapshot.size >= kMaterialAlpha + sizeof(float))
				alphaWritten.push_back({ material, a_held->snapshot.At<float>(kMaterialAlpha), a_held->sequence });
			// A capture equal to the held one changes no record. One whose frame-owned bytes alone moved is only counted while a feature
			// patches them every frame. Unpatched, those bytes belong to the record, so it is rewritten.
			auto change = MaterialPort::SnapshotChange::Fields;
			if (entry.held)
				change = MaterialPort::SnapshotChangeOf(entry.held->snapshot, a_held->snapshot);
			if (change == MaterialPort::SnapshotChange::Transforms && !Extensions::FrameVSVariablePatched())
				change = MaterialPort::SnapshotChange::Fields;
			if (entry.held && change == MaterialPort::SnapshotChange::Fields && entry.held->snapshot.size == a_held->snapshot.size) {
				++s.fieldsMoved;
				if (!s.fieldsMovedVtable)
					s.fieldsMovedVtable = a_held->snapshot.vtable;
				for (auto& [vtable, count] : s.fieldsMovedBy)
					if (!vtable || vtable == a_held->snapshot.vtable) {
						vtable = a_held->snapshot.vtable;
						++count;
						break;
					}
				const auto& was = entry.held->snapshot.bytes;
				const auto& now = a_held->snapshot.bytes;
				for (std::size_t w = 0; w < s.wordMoved.size() && w * 4 + 4 <= a_held->snapshot.size; ++w)
					s.wordMoved[w] += std::memcmp(was.data() + w * 4, now.data() + w * 4, 4) != 0;
			}
			if (entry.held) {
				++s.replaced;
				HandBackHeld(std::move(entry.held));
			}
			entry.held = std::move(a_held);
			entry.frame = sceneFrame;
			if (change != MaterialPort::SnapshotChange::Fields) {
				++s.deduped;
				if (change == MaterialPort::SnapshotChange::Transforms) {
					++s.transformsOnly;
					// The frame values read the material's frame buffer, so there is nothing to follow.
					if (materialDependents.contains(material))
						++materialRecordStats.transformsWatched;
				}
				return;
			}
			if (entry.listed) {
				++s.relisted;
				return;
			}
			entry.listed = true;
			materialsCaptured.push_back(material);
		});
		// RaceMenu stores an overlay's alpha straight into its material with no engine setter. Its properties cache the alpha and
		// have no event of their own, so the record takes the capture's alpha and the row is sampled again.
		for (const auto& written : alphaWritten)
			for (const void* key : mirror.PropertiesOfMaterial(written.material)) {
				const auto* record = mirror.Property(key);
				// A newer record read the alpha itself.
				if (!record || record->sequence > written.sequence ||
					std::bit_cast<std::uint32_t>(record->materialAlpha) == std::bit_cast<std::uint32_t>(written.alpha))
					continue;
				SceneCapture::PropertyRecord update = *record;
				update.materialAlpha = written.alpha;
				SceneCapture::Update apply{ SceneCapture::PropertyRecord::kMaterialAlpha, std::move(update) };
				apply.sequence = written.sequence;
				mirror.Update(apply);
				propertyChanged.push_back(key);
				MaterialWrites::NoteShading(static_cast<const RE::BSShaderProperty*>(key));
			}
		// A served capture was pushed before its answer, so the join may ask again.
		channels.materialRequestsAnswered.Drain([&](MaterialAnswer&& a_answer) {
			materialRequested.erase(a_answer.material);
			++(a_answer.captured ? residentStats.materialsServed : residentStats.materialsStale);
		});
		// Once a frame, since a later pass of the same frame would find nothing older.
		if (std::exchange(materialSnapshotsAgedFrame, sceneFrame) == sceneFrame)
			return;
		for (auto it = materialSnapshots.begin(); it != materialSnapshots.end();) {
			if (sceneFrame - it->second.frame > kMaterialSnapshotFrames) {
				++s.aged;
				HandBackHeld(std::move(it->second.held));
				it = materialSnapshots.erase(it);
			} else {
				++it;
			}
		}
	}

	void Coordinator::ApplyFadeNodeAnswers()
	{
		auto& r = referenceStats;
		channels.fadeNodeAnswers.Drain([&](FadeNodeAnswer&& a_answer) {
			auto* geometry = const_cast<RE::BSGeometry*>(a_answer.geometry);
			const auto it = tracked.find(geometry);
			auto* entry = it != tracked.end() ? &it->second : nullptr;
			if (entry && entry->fadeNodeRequested == a_answer.fadeNode)
				entry->fadeNodeRequested = nullptr;
			const auto* record = entry ? mirror.Geometry(geometry) : nullptr;
			const auto* property = record && record->property ? mirror.Property(record->property) : nullptr;
			if (!entry || !a_answer.node || !property || property->fadeNode != a_answer.fadeNode || entry->fadeNodeRef.get() == a_answer.fadeNode) {
				r.fadeNodesAnsweredNone += a_answer.node ? 0 : 1;
				HandBack(std::move(a_answer.node));
				return;
			}
			++r.fadeNodesAnswered;
			HandBack(std::move(entry->fadeNodeRef));
			entry->fadeNodeRef = std::move(a_answer.node);
			for (const std::uint32_t slot : { entry->slot, entry->layerSlot }) {
				if (slot == kNoObjectSlot || slot >= tables.objects.size() || tables.objectGeometry[slot] != geometry)
					continue;
				ComponentsSlotWritten(slot);
				if (IsResidentSlot(slot))
					ComponentsResident(slot);
			}
		});
	}

	void Coordinator::ApplyEvents(std::shared_ptr<EventBatch> a_batch)
	{
		// Material captures first, since an attach names materials whose captures were pushed before it.
		{
			DCLF_ZONE("CS.DCLF.Scene.MaterialCaptures");
			DrainMaterialCaptures();
		}
		auto batch = std::move(a_batch);
		BatchUnwindGuard unwindGuard(*this, batch);

		BatchIndex index;
		ApplyMirrorEvents(*batch, index);

		// A detach forces the category refresh, since it can take a category node the signature cannot see go.
		RefreshBatchCategories(index);
		ApplyAttachDetach(index);

		{
			// Parity only. Never a live walk while a load screen is up.
			DCLF_SCENE_PART(Validate, "CS.DCLF.Scene.Validate");
			if (Policy::ValidateTrackedSlice() && !channels.loadingSeen.load(std::memory_order_acquire) &&
				std::exchange(validatedFrame, PassStamp()) != PassStamp())
				ValidateSlice();
		}
		DCLF_SCENE_PART(StructuralEvents, "CS.DCLF.Scene.StructuralEvents");
		ComponentsChannelKeys(Api::ChannelApply::BeforeStructural, *batch);
		propertyChanged.insert(propertyChanged.end(), batch->properties.begin(), batch->properties.end());
		for (auto& node : batch->nodes)
			nodeChanged.push_back(std::move(node));
		batch->nodes.clear();
		// Its index before the oldest event decides whether the selection changed.
		for (auto& event : batch->switches) {
			++delta.switchEvents;
			const auto [at, inserted] = switchPendingIndex.try_emplace(event.node.get(), static_cast<std::uint32_t>(switchPending.size()));
			if (inserted)
				switchPending.push_back({ std::move(event.node), event.before, event.structural });
			else
				switchPending[at->second].structural |= event.structural;
		}
		ComponentsChannelKeys(Api::ChannelApply::AfterSwitches, *batch);

		stats.tracked = static_cast<std::uint32_t>(tracked.size());
		stats.categoryNodes = static_cast<std::uint32_t>(categoryNodes.size());
		HoldLeafProperties(index);
		PrewarmLoadTextures(index);
		if (batch->oldestNs && (!unpublishedNs || batch->oldestNs < unpublishedNs)) {
			unpublishedNs = batch->oldestNs;
			unpublishedFrame = batch->oldestFrame;
		}
		// Its events hold detached subtrees a published table may still name, so it is retired through the chain.
		if (batch->head || !batch->switches.empty())
			retirement.Open().owners.push_back(std::move(batch));
		// No reference is made from the pins after the apply. Let go rather than cleared, or every pass of a burst would clear the buckets again.
		batchPins = {};
		channels.categoryPinsReleased.store(categoryPinsGeneration, std::memory_order_release);
	}

	void Coordinator::RefreshBatchCategories(const BatchIndex& a_index)
	{
		DCLF_SCENE_PART(CategoryNodes, "CS.DCLF.Scene.CategoryNodes");
		RefreshCategoryNodes(a_index.sawDetach);
		// Hand the references back first, since the list may be a root's last holder and must never release on the pump.
		auto retry = std::exchange(pendingSubtrees, {});
		std::vector<std::pair<RE::NiAVObject*, SubtreeSource>> retryRoots;
		retryRoots.reserve(retry.size());
		for (const auto& root : retry)
			retryRoots.emplace_back(root.get(), SubtreeSource::Retry);
		HandBack(retry);
		AddSubtrees(retryRoots);
		ApplyFadeNodeAnswers();
	}

	void Coordinator::ApplyAttachDetach(const BatchIndex& a_index)
	{
		DCLF_SCENE_PART(AttachDetach, "CS.DCLF.Scene.AttachDetach");
		// The engine moves objects between containers by detach plus attach, so a detach leaves only if nothing reattached the geometry by the batch's end.
		std::vector<RE::BSGeometry*> detached;
		// Prepare the attaches' subtrees first, then merge each at its event. Event order matters only to the merges.
		auto& a = attachStats;
		a.structuralEvents += a_index.structural.size();
		std::vector<std::pair<RE::NiAVObject*, SubtreeSource>> attached;
		if (!categoryNodes.empty())
			for (const auto* event : a_index.structural)
				if (event->type == SceneTracker::EventType::Attached)
					attached.emplace_back(event->node.get(), SubtreeSource::AttachEvent);
		const std::uint32_t subtreeMode = attached.empty() ? 0 : FanoutMode(attached.size(), kSubtreeFanoutMin);
		if (!attached.empty()) {
			++fanoutStats.subtreeRounds;
			fanoutStats.subtreeRoots += attached.size();
		}
		[[maybe_unused]] const std::uint64_t mirrorWrites = mirror.Writes();
		if (subtreeMode != 0)
			PrepareSubtrees(attached, subtreeMode, subtreePreps);
		std::size_t nextPrep = 0;
		double mergeMs = 0.0;
		double ancestorMs = 0.0, serialMs = 0.0;
		for (auto* event : a_index.structural) {
			// Its subtree changed, which its primary-cut plan reads.
			if (!event->ancestors.empty() && !sunTable.index.empty()) {
				const auto ancestorStart = std::chrono::steady_clock::now();
				for (const void* ancestor : event->ancestors)
					if (const auto it = sunTable.index.find(static_cast<const RE::NiAVObject*>(ancestor)); it != sunTable.index.end()) {
						sunEntriesForced.insert(it->first);
						sunEntriesDirty.push_back(it->first);
					}
				ancestorMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ancestorStart).count();
			}
			if (event->type == SceneTracker::EventType::Attached) {
				++stats.attachedEvents;
				if (!categoryNodes.empty()) {
					DCLF_ZONE("CS.DCLF.Scene.SubtreesMerge");
					const auto mergeStart = std::chrono::steady_clock::now();
					if (subtreeMode == 0)
						AddSubtree(event->node.get());
					else
						MergeSubtree(subtreePreps[nextPrep++]);
					const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mergeStart).count();
					if (subtreeMode == 0)
						serialMs += ms;
					else
						mergeMs += ms;
				}
			} else if (event->type == SceneTracker::EventType::Detached) {
				++stats.detachedEvents;
				detached.insert(detached.end(), event->removed.begin(), event->removed.end());
			}
		}
		if (subtreeMode != 0) {
			assert(nextPrep == subtreePreps.size() && mirror.Writes() == mirrorWrites);
			AddScenePartMs(ScenePart::SubtreesMerge, mergeMs);
			fanoutStats.subtreeMergeMs += mergeMs;
		}
		a.ancestorMs += ancestorMs;
		a.mergeMs += mergeMs + serialMs;
		ApplyDetaches(detached);
	}

	void Coordinator::ApplyDetaches(const std::vector<RE::BSGeometry*>& a_detached)
	{
		DCLF_ZONE("CS.DCLF.Scene.Detaches");
		auto& a = attachStats;
		const auto detachStart = std::chrono::steady_clock::now();
		a.detachedGeometries += a_detached.size();
		// Tracked again within two frames of its detach means a detach and attach split across passes. A load's moves are not counted.
		for (auto it = erasedByDetach.begin(); it != erasedByDetach.end();)
			it = sceneFrame - it->second > 2 ? erasedByDetach.erase(it) : std::next(it);
		const bool noteSplits = !channels.loadingSeen.load(std::memory_order_acquire) && !LoadHold::Active() && !LoadHold::MenuHeld();
		for (auto* geometry : a_detached) {
			// Only a tracked geometry is known alive, so it is safe to walk up from.
			if (!tracked.contains(geometry))
				continue;
			if (FindCategoryNode(geometry, nullptr)) {
				++stats.detachMoves;
				continue;
			}
			++a.detachedErased;
			EraseTracked(geometry);
			if (noteSplits)
				erasedByDetach.insert_or_assign(geometry, sceneFrame);
		}
		a.detachMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - detachStart).count();
	}

	void Coordinator::HoldLeafProperties(const BatchIndex& a_index)
	{
		DCLF_ZONE("CS.DCLF.Scene.LeafHolds");
		const auto leafStart = std::chrono::steady_clock::now();
		for (const auto* records : a_index.leaves) {
			attachStats.leafKeys += records->geometries.size();
			for (const auto& geometry : records->geometries)
				if (const auto it = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(geometry.key))); it != tracked.end()) {
					++attachStats.leafHolds;
					HoldProperties(it->second);
					if (std::exchange(it->second.leafWaiting, false)) {
						it->second.candidateFrame = 0;
						pendingEvaluation.push_back(it->first);
						++leafStats.rescheduled;
					}
				}
		}
		attachStats.leafMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - leafStart).count();
	}

	void Coordinator::PrewarmLoadTextures(const BatchIndex& a_index)
	{
		DCLF_ZONE("CS.DCLF.Scene.LoadPrewarm");
		auto& w = loadPrewarm;
		// Each import is kept while the load and its hold last, so the registry still has it when the bindings ask.
		w.replies.Drain([&](GpuTextures::Reply&& a_reply) {
			if (a_reply.cookie != w.cookie)
				return;
			if (a_reply.binding.owner) {
				w.held.push_back(std::move(a_reply.binding.owner));
				++w.imported;
			} else {
				++w.rejected;
			}
		});
		if (!channels.loadingSeen.load(std::memory_order_acquire)) {
			if ((!w.held.empty() || !w.asked.empty()) && !LoadHold::Active() && !LoadHold::MenuHeld()) {
				logger::info("[DCLF] load prewarm: {} diffuse textures asked for during the load, {} imported, {} rejected; let go after the hold",
					w.requested, w.imported, w.rejected);
				w.held.clear();
				w.asked.clear();
				w.requested = w.imported = w.rejected = 0;
				++w.cookie;
			}
			return;
		}
		for (const auto* records : a_index.leaves)
			for (const auto& property : records->properties) {
				auto* view = property.diffuseHeld.get();
				if (!view || !w.asked.insert(view).second)
					continue;
				// The record holds a reference on the view, so it is alive here. The request takes its own.
				TextureStreaming::RequestBase(view);
				GpuTextures::Get().Request(view, GpuTextures::Tag(GpuTextures::Source::Material), w.replies, w.cookie);
				++w.requested;
			}
	}

	void Coordinator::PrepareMirrorCaptures(const std::vector<const SceneCapture::Records*>& a_captures, std::size_t a_records)
	{
		// Copied and stamped here so the serial apply only moves records into the maps. Pure over the events' own records.
		const auto& captures = a_captures;
		const std::size_t records = a_records;
		mirrorPreps.clear();
		if (captures.empty())
			return;
		auto& a = attachStats;
		++a.mirrorBatches;
		a.mirrorCaptures += captures.size();
		a.mirrorRecords += records;
		mirrorPreps.resize(captures.size());
		const std::uint32_t frame = DCLF::SceneClock::Step();
		const std::size_t jobs = captures.size() * SceneMirror::kTypes;
		auto body = [&](std::size_t a_begin, std::size_t a_end) {
			DCLF_ZONE("CS.DCLF.Scene.Mirror.PrepareShards");
			for (std::size_t j = a_begin; j < a_end; ++j)
				SceneMirror::Prepare(*captures[j / SceneMirror::kTypes], static_cast<std::uint32_t>(j % SceneMirror::kTypes), frame, mirrorPreps[j / SceneMirror::kTypes]);
		};
		DCLF_ZONE("CS.DCLF.Scene.Mirror.Prepare");
		const auto start = std::chrono::steady_clock::now();
		if (FanoutMode(records, kMirrorFanoutRecords) == 2) {
			++a.mirrorPooled;
			::RenderGraph::Frame::SharedExecutor().ParallelFor("CS.DCLF.Scene.Mirror.Prepare", jobs, 2, body);
		} else {
			body(0, jobs);
		}
		a.mirrorPrepareMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	namespace
	{
		void ApplyMirrorAttach(SceneMirror& a_mirror, std::vector<SceneMirror::Prepared>& a_mirrorPreps, SceneTracker::Event& a_event, std::size_t& a_nextPrep)
		{
			if (a_event.captured && a_event.mirrorFill) {
				const auto& captured = *a_event.captured;
				SceneCapture::Records fill;
				fill.sequence = captured.sequence;
				for (const auto& n : captured.nodes)
					if (!a_mirror.Node(n.key))
						fill.nodes.push_back(n);
				for (const auto& g : captured.geometries)
					if (!a_mirror.Geometry(g.key))
						fill.geometries.push_back(g);
				for (const auto& p : captured.properties)
					if (!a_mirror.Property(p.key))
						fill.properties.push_back(p);
				for (const auto& a : captured.alphas)
					if (!a_mirror.Alpha(a.key))
						fill.alphas.push_back(a);
				if (!fill.nodes.empty() || !fill.geometries.empty() || !fill.properties.empty() || !fill.alphas.empty())
					a_mirror.Apply(fill);
			} else if (a_event.captured) {
				assert(a_nextPrep < a_mirrorPreps.size());
				a_mirror.Apply(*a_event.captured, &a_mirrorPreps[a_nextPrep++]);
			}
			else if (a_event.node && a_mirror.Node(a_event.node.get()))
				// Its record is stale. The event holds the object, so its name is safe to read.
				a_mirror.Evict(a_event.node.get(), fmt::format("'{}' {}", a_event.node->name.c_str() ? a_event.node->name.c_str() : "",
													 const_cast<RE::NiAVObject*>(a_event.node.get())->GetRTTI() ? const_cast<RE::NiAVObject*>(a_event.node.get())->GetRTTI()->name : "?"));
		}

		void ApplyMirrorUpdate(SceneMirror& a_mirror, SceneTracker::Event& a_event, ankerl::unordered_dense::set<const void*>& a_mirrorMotionNodes,
			ankerl::unordered_dense::set<const void*>& a_mirrorSkinGeometries, std::vector<CoordinatorHidden::BatchHidden>& a_batchHidden,
			std::vector<const void*>& a_fadeChanged)
		{
			// A fade node crossing 1 or changing its door byte changes its members' synthetic pass hint, so it goes in as a property event.
			using Node = SceneCapture::NodeRecord;
			const void* fadeKey = nullptr;
			bool fadingBefore = false;
			std::uint8_t doorBefore = 0;
			if (a_event.update.fields & (Node::kFadeCurrent | Node::kFadeDoor))
				if (const auto* record = std::get_if<Node>(&a_event.update.record))
					if (const auto* before = a_mirror.Node(record->key)) {
						fadeKey = record->key;
						fadingBefore = before->currentFade < 1.0f;
						doorBefore = before->fadeDoor;
					}
			const auto* hidden = a_event.update.hiddenSite != ~0u ? std::get_if<SceneCapture::NodeRecord>(&a_event.update.record) : nullptr;
			const CoordinatorHidden::BatchHidden hiddenEvent = hidden ? CoordinatorHidden::BatchHidden{ hidden->key, a_event.update.hiddenSite, (hidden->flags & 1u) != 0, a_event.update.sequence } : CoordinatorHidden::BatchHidden{};
			// A skin update applies to every geometry on the skin. RaceMenu's overlays share their base's skin instance, so armour over a
			// painted body hides the paint's partitions too.
			if (auto* skinUpdate = std::get_if<SceneCapture::GeometryRecord>(&a_event.update.record); skinUpdate && !skinUpdate->key && skinUpdate->skin) {
				const auto sharers = a_mirror.GeometriesOfSkin(skinUpdate->skin);
				for (std::size_t i = 1; i < sharers.size(); ++i) {
					SceneCapture::Update copy = a_event.update;
					std::get<SceneCapture::GeometryRecord>(copy.record).key = sharers[i];
					const auto [sharerType, sharer] = a_mirror.UpdateTaking(copy);
					Diagnostics::Notify(Diagnostics::MirrorUpdateApplied{ sharerType, sharer, a_event.update.fields });
					if (sharer)
						a_mirrorSkinGeometries.insert(sharer);
				}
				if (!sharers.empty())
					skinUpdate->key = sharers.front();
			}
			const auto [type, key] = a_mirror.UpdateTaking(a_event.update);
			Diagnostics::Notify(Diagnostics::MirrorUpdateApplied{ type, key, a_event.update.fields });
			// Entries named by a trait or skin input are classified again at the next delta walk, not polled every frame.
			if (key && type == 0 && (a_event.update.fields & (SceneCapture::NodeRecord::kControllers | SceneCapture::NodeRecord::kBody)))
				a_mirrorMotionNodes.insert(key);
			else if (key && type == 1 && (a_event.update.fields & (SceneCapture::GeometryRecord::kDismember | SceneCapture::GeometryRecord::kSkin)))
				a_mirrorSkinGeometries.insert(key);
			if (hidden)
				a_batchHidden.push_back(hiddenEvent);
			if (fadeKey)
				if (const auto* after = a_mirror.Node(fadeKey); after && ((after->currentFade < 1.0f) != fadingBefore || after->fadeDoor != doorBefore))
					a_fadeChanged.push_back(fadeKey);
		}
	}

	void Coordinator::ApplyMirrorEvents(const EventBatch& a_batch, BatchIndex& a_index)
	{
		DCLF_ZONE("CS.DCLF.Scene.Mirror");
		mirror.BeginBatch();
		std::vector<const SceneCapture::Records*> captures;
		std::size_t records = 0;
		PinMirrorBatch(a_batch, captures, records);
		PrepareMirrorCaptures(captures, records);
		DCLF_ZONE_NAMED(mirrorApplyZone, "CS.DCLF.Scene.Mirror.Apply", true);
		const auto applyStart = std::chrono::steady_clock::now();
		std::size_t nextPrep = 0;
		// Taken where the kind changes, so a run of one kind costs one clock read.
		auto& attach = attachStats;
		std::size_t kind = AttachStats::kMirrorKinds;
		auto runStart = applyStart;
		auto enter = [&](std::size_t a_kind) {
			++attach.mirrorKindEvents[a_kind];
			if (a_kind == kind)
				return;
			const auto now = std::chrono::steady_clock::now();
			if (kind != AttachStats::kMirrorKinds)
				attach.mirrorKindMs[kind] += std::chrono::duration<double, std::milli>(now - runStart).count();
			kind = a_kind;
			runStart = now;
		};
		auto apply = [&](SceneTracker::Event* a_head) {
			for (auto* event = a_head; event; event = event->next) {
				if (event->type == SceneTracker::EventType::Attached || event->type == SceneTracker::EventType::Detached) {
					if (event->type == SceneTracker::EventType::Attached && event->captured)
						a_index.leaves.push_back(event->captured.get());
					if (!event->mirrorOnly) {
						a_index.structural.push_back(event);
						a_index.sawDetach |= event->type == SceneTracker::EventType::Detached;
					}
				} else if (event->type == SceneTracker::EventType::Updated && event->update.leaf) {
					a_index.leaves.push_back(event->update.leaf.get());
				}
				enter(static_cast<std::size_t>(event->type == SceneTracker::EventType::Attached ? (event->captured ? (event->mirrorFill ? 1 : 0) : 2) :
											   event->type == SceneTracker::EventType::Detached ? 3 :
											   event->type == SceneTracker::EventType::Stacked  ? 4 :
											                                                      5));
				if (event->type == SceneTracker::EventType::Attached) {
					ApplyMirrorAttach(mirror, mirrorPreps, *event, nextPrep);
				} else if (event->type == SceneTracker::EventType::Detached) {
					// Must run before the records go.
					if (event->detachedRoot && !rootMarks.empty())
						UnmarkDetached(event->detachedRoot, event->removedNodes);
					if (event->detachedRoot)
						mirror.Detach(event->detachedRoot, event->removed, event->removedNodes);
				} else if (event->type == SceneTracker::EventType::Stacked) {
					// Ordered after the attaches pushed before it, such as a LOD gate's incoming blocks.
					ComponentsStacked(Api::EngineChannelId{ event->stackedChannel },
						Api::Stacked{ event->stackedToken, event->stackedPinned, event->stackedNamed, event->stampNs, event->stampFrame });
				} else {
					ApplyMirrorUpdate(mirror, *event, mirrorMotionNodes, mirrorSkinGeometries, batchHidden, propertyChanged);
				}
			}
		};
		apply(a_batch.head);
		if (kind != AttachStats::kMirrorKinds)
			attach.mirrorKindMs[kind] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - runStart).count();
		CountMirrorWork();
		assert(nextPrep == mirrorPreps.size());
		mirrorPreps.clear();
		attachStats.mirrorApplyMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - applyStart).count();
		Diagnostics::Notify(Diagnostics::MirrorEventsApplied{ *this, std::span(a_batch.channelKeys).first(Extensions::RegisteredEngineChannels().size()) });
	}

	void Coordinator::PinMirrorBatch(const EventBatch& a_batch, std::vector<const SceneCapture::Records*>& a_captures, std::size_t& a_records)
	{
		DCLF_ZONE("CS.DCLF.Scene.Mirror.Pins");
		// The batch's pins by key are the only source of references while it applies.
		std::vector<const std::vector<RE::NiPointer<RE::NiRefObject>>*> pinned;
		std::size_t pins = 0;
		for (const auto* event = a_batch.head; event; event = event->next) {
			if (event->type == SceneTracker::EventType::Attached && event->captured) {
				const auto& captured = *event->captured;
				if (!captured.pins.empty()) {
					pinned.push_back(&captured.pins);
					pins += captured.pins.size();
				}
				if (!event->mirrorFill) {
					a_captures.push_back(&captured);
					a_records += captured.nodes.size() + captured.geometries.size() + captured.properties.size() + captured.alphas.size();
				}
			}
			if (!event->pins.empty()) {
				pinned.push_back(&event->pins);
				pins += event->pins.size();
			}
		}
		batchPins.clear();
		batchPins.reserve(pins);
		for (const auto* list : pinned)
			PinBatch(*list);
	}

	void Coordinator::CountMirrorWork()
	{
		auto& attach = attachStats;
		const auto work = mirror.TakeWork();
		attach.replaysSkipped += work.replaysSkipped;
		attach.replaysWalked += work.replaysWalked;
		attach.updatesKept += work.remembered;
		attach.updatesForgotten += work.forgotten;
	}

	void Coordinator::ApplySwitchEvents(bool a_full)
	{
		if (a_full)
			switchResync = true;
		for (auto& pending : switchPending) {
			auto* node = pending.node.get();
			const auto* record = node ? mirror.Node(node) : nullptr;
			// Only the covered scene. A switch still loading is caught up by its attach.
			if (!record || !(record->kind & SceneCapture::kKindSwitch) || !FindCategoryNode(node, nullptr))
				continue;
			if (!pending.structural && record->switchIndex == pending.before)
				continue;
			++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::SwitchEvent)];
			++delta.switchChanges;
			switchesApplied.push_back(node);
			ComponentsSwitched(node);
			if (a_full)
				continue;
			ForEachMirror(mirror, node, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
				if (a_record.kind & SceneCapture::kKindGeometry)
					if (const auto entry = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key))); entry != tracked.end()) {
						Reclassify(entry->first, entry->second);
						++delta.switchReclassified;
					}
				return true;
			}, kMaxEventNodes);
		}
		// The switches' references are the render thread's to drop.
		for (auto& pending : switchPending)
			HandBack(std::move(pending.node));
		switchPending.clear();
		switchPendingIndex.clear();
	}

	void Coordinator::ApplyNodeEvent(RE::NiAVObject* a_node)
	{
		if (!a_node || !FindCategoryNode(a_node, nullptr))
			return;
		const auto* record = mirror.Node(a_node);
		// An actor's entries are per frame and its sun entry is never tested.
		if (Extensions::OwnerClassOf(*record).ownMotion)
			return;
		++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::NodeEvent)];
		auto placedEveryFrame = [](const Tracked& a_tracked) {
			return a_tracked.perFrame && (!a_tracked.lightTraits || (a_tracked.lightTraits & (kTraitMoves | kTraitRootMoves)));
		};
		// A root known to move is already handled by QueueRoots.
		for (const void* key = a_node; key;) {
			const auto* object = static_cast<const RE::NiAVObject*>(key);
			const auto* at = mirror.Node(key);
			key = at ? at->parent : nullptr;
			if (!rootDependents.contains(object))
				continue;
			if (const auto motion = rootMotion.find(object); motion != rootMotion.end() && motion->second)
				continue;
			ScheduleRoot(object);
		}
		ForEachMirror(mirror, a_node, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
			if (a_record.kind & SceneCapture::kKindGeometry)
				if (const auto entry = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key)));
					entry != tracked.end() && !placedEveryFrame(entry->second) && PlacementMatters(entry->second))
					Reclassify(entry->first, entry->second);
			return true;
		}, kMaxEventNodes);
	}

	void Coordinator::ApplyMotionUpdate(const void* a_node)
	{
		// Keys only. A node the mirror no longer holds was detached with its entries.
		const auto* record = mirror.Node(a_node);
		Ineligible reason = Ineligible::None;
		bool unmirrored = false;
		if (!record || !MirrorCategoryNode(a_node, reason, unmirrored))
			return;
		if (Extensions::OwnerClassOf(*record).ownMotion)
			return;
		for (const void* key = a_node; key;) {
			const auto* at = mirror.Node(key);
			const auto* object = static_cast<const RE::NiAVObject*>(key);
			key = at ? at->parent : nullptr;
			if (rootDependents.contains(object))
				ScheduleRoot(object);
		}
		ForEachMirror(mirror, a_node, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
			if (a_record.kind & SceneCapture::kKindGeometry)
				if (const auto entry = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key))); entry != tracked.end())
					Reclassify(entry->first, entry->second);
			return true;
		}, kMaxEventNodes);
	}

	void Coordinator::ValidateSlice()
	{
		// Missed-detach net. A slice per frame keeps the cost flat.
		if (tracked.empty())
			return;
		std::vector<RE::BSGeometry*> stale;
		// A recent detach differs until the next batch erases it, so only a suspect still differing a batch later counts.
		for (auto* geometry : std::exchange(validationSuspects, {}))
			if (const auto it = tracked.find(geometry); it != tracked.end() && FindCategoryNodeLive(geometry, nullptr) != it->second.categoryNode)
				stale.push_back(geometry);
		const std::size_t size = tracked.size();
		const std::size_t count = std::min(kValidationSlice, size);
		for (std::size_t i = 0; i < count; ++i) {
			validationCursor = (validationCursor + 1) % size;
			auto it = tracked.begin() + static_cast<std::ptrdiff_t>(validationCursor);
			Ineligible reason = Ineligible::None;
			if (FindCategoryNodeLive(it->first, &reason) != it->second.categoryNode) {
				validationSuspects.push_back(it->first);
			} else if (it->second.parentReason != reason) {
				it->second.parentReason = reason;
				it->second.candidateFrame = 0;
				pendingEvaluation.push_back(it->first);
			}
		}
		// What the engine shows now, so the log names the engine change that had no event. A node is named only while it is a category node.
		const auto describe = [&](RE::BSGeometry* a_geometry) {
			const auto nameOf = [](const RE::NiAVObject* a_object) { return a_object && a_object->name.c_str() ? a_object->name.c_str() : ""; };
			RE::NiNode* const was = [&]() -> RE::NiNode* { const auto it = tracked.find(a_geometry); return it != tracked.end() ? it->second.categoryNode : nullptr; }();
			std::string chain;
			const RE::NiAVObject* top = a_geometry;
			for (const auto* node = a_geometry->parent; node && chain.size() < 1024; node = node->parent) {
				chain += fmt::format(" <- '{}' {}{}", nameOf(node), fmt::ptr(node), categoryNodes.contains(const_cast<RE::NiNode*>(node)) ? " (category)" : "");
				top = node;
			}
			const auto* owner = a_geometry->GetUserData();
			for (const RE::NiAVObject* at = a_geometry; at && !owner; at = at->parent)
				owner = at->GetUserData();
			const auto* live = FindCategoryNodeLive(a_geometry, nullptr);
			return fmt::format("'{}' {} of {:08X} '{}': tracked under {}{}, now under {} '{}'; chain{}{}", nameOf(a_geometry), fmt::ptr(a_geometry),
				owner ? owner->GetFormID() : 0u, owner && owner->GetBaseObject() && owner->GetBaseObject()->GetName() ? owner->GetBaseObject()->GetName() : "",
				fmt::ptr(was), was && categoryNodes.contains(was) ? fmt::format(" '{}' (still a category node)", nameOf(was)) : std::string(" (no longer a category node)"),
				fmt::ptr(live), nameOf(live), chain, top && !top->parent ? " (top has no parent)" : "");
		};
		// A missed detach is a defect: fatal in development builds. Release counts it and drops the entry.
		for (auto* geometry : stale) {
			::RenderGraph::Frame::Driver::NoteTapError(::RenderGraph::Frame::Driver::TapError::SceneMismatch, ::RenderGraph::Frame::SegmentId::Count,
				fmt::format("tracked-set check: geometry {} is tracked under a category node it no longer has (a missed detach)", describe(geometry)));
			EraseTracked(geometry);
			++stats.validationDrops;
		}
	}


	void Coordinator::ListHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		// The chain above a tracked geometry does not change while tracked.
		if (!a_tracked.hiddenChain.empty())
			return;
		// Up to but not including the category node, which thousands share.
		++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::HiddenChain)];
		for (const void* key = a_geometry; key && key != a_tracked.categoryNode; key = mirror.Parent(key)) {
			const auto* object = static_cast<const RE::NiAVObject*>(key);
			a_tracked.hiddenChain.push_back(object);
			List(hiddenDependents, hiddenDependentsAt, object, a_geometry);
		}
	}

	void Coordinator::UnlistHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		for (const auto* object : a_tracked.hiddenChain)
			Unlist(hiddenDependents, hiddenDependentsAt, object, a_geometry);
		a_tracked.hiddenChain.clear();
	}

	void Coordinator::DrainHiddenEvents()
	{
		// Keys only. The addresses are never read.
		auto& primary = PrimaryCull::Get();
		ankerl::unordered_dense::set<RE::BSGeometry*> retake;
		ankerl::unordered_dense::set<const void*> categoryEvents;
		auto announce = [&](RE::BSGeometry* a_geometry, Tracked& a_entry) {
			a_entry.hiddenEventOwed = true;
			if (!(a_entry.lightTraits & kTraitOwnerClass))
				retake.insert(a_geometry);
		};
		auto announceKey = [&](const void* a_key) {
			// The first-person root is toggled within every frame, so its events are skipped.
			if (categoryNodes.contains(static_cast<RE::NiNode*>(const_cast<void*>(a_key))) && !ViewRoot(a_key))
				categoryEvents.insert(a_key);
			const auto dependents = hiddenDependents.find(a_key);
			if (dependents == hiddenDependents.end())
				return;
			for (auto* geometry : dependents->second)
				if (const auto entry = tracked.find(geometry); entry != tracked.end())
					announce(entry->first, entry->second);
		};
		// Nodes in the frame's capture are judged by the capture's bit. An event after the capture may be newer, as when first person hides
		// the third-person skeleton (FUN_140736150). Such a key is announced again at the first pass with a newer capture.
		const auto& globals = FrameGlobals::Current();
		for (auto it = hiddenAhead.begin(); it != hiddenAhead.end();) {
			if (it->second < globals.cullHiddenSequence) {
				announceKey(it->first);
				++stats.hiddenAheadAnnounced;
				it = hiddenAhead.erase(it);
			} else
				++it;
		}
		auto aheadOfCapture = [&](const BatchHidden& a_event) {
			if (a_event.sequence < globals.cullHiddenSequence)
				return false;
			const auto& listed = globals.cullHidden;
			const auto* object = static_cast<const RE::NiAVObject*>(a_event.key);
			const auto it = std::lower_bound(listed.begin(), listed.end(), object, [](const auto& a_entry, const RE::NiAVObject* a_object) { return a_entry.first < a_object; });
			return it != listed.end() && it->first == object && it->second != a_event.hidden;
		};
		stats.hiddenEvents += batchHidden.size();
		for (const auto& a_event : std::exchange(batchHidden, {})) {
			const void* a_key = a_event.key;
			if (aheadOfCapture(a_event)) {
				auto& sequence = hiddenAhead[a_key];
				sequence = std::max(sequence, a_event.sequence);
			}
			// Held while the scene list job may run.
			if (holdPrimaryNotes)
				channels.primaryNotes.Push({ PrimaryNote::Kind::HiddenKey, a_key });
			else
				primary.NoteHiddenKey(a_key);
			announceKey(a_key);
		}
		for (const void* key : categoryEvents)
			if (const auto members = categoryMembers.find(static_cast<const RE::NiNode*>(key)); members != categoryMembers.end())
				for (auto* geometry : members->second)
					if (const auto entry = tracked.find(geometry); entry != tracked.end() && entry->second.categoryNode == key)
						announce(entry->first, entry->second);
		for (auto* geometry : retake)
			if (const auto entry = tracked.find(geometry); entry != tracked.end()) {
				entry->second.candidateFrame = 0;  // Reclassify, scheduled with the pending ones (DeltaWalk)
				pendingEvaluation.push_back(geometry);
			}
		stats.hiddenRetaken += retake.size();
		hiddenGating = hiddenEventsInstalled && sceneFrame > moveUngatedThrough;
	}
}
