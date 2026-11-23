// The DCLF set: who draws an object, decided by each scene pass's commit. Also the pass itself, its publication, and the lookups
// and table snapshots it carries. Scene lane.

#include "Internal.h"
#include "Features/DrawcallLimitFix/Core/Engine/SceneClock.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"


#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <algorithm>
#include <atomic>
#include <span>

namespace DCLF::Scene
{
	std::uint32_t Coordinator::SetCapability()
	{
		// Main always. Shadow and occlusion once set up. Each view's phase once it is drawable.
		std::uint32_t phases = kSetMain | IndirectDraws::Get().ShadowCapability();
		const auto& views = Extensions::Views();
		for (std::size_t v = 0; v < views.views.size(); ++v)
			if (views.views[v].phase && IndirectDraws::Get().ViewDrawable(static_cast<Api::EngineView>(v)))
				phases |= views.views[v].phase;
		return phases;
	}

	std::uint8_t Coordinator::SetParticipation(std::uint32_t a_slot, std::uint32_t a_drawn) const
	{
		const auto flags = tables.objects[a_slot].flags;
		if (flags & kObjectFree)
			return 0;
		// A first-person slot takes part in its view alone, since the engine draws the skeleton in no world view. Decals, refraction and
		// shadow-only objects stay the engine's there.
		if (const auto view = ViewRootOfSlot(a_slot); view != Api::EngineView::Count) {
			const auto p = tables.objects[a_slot].pipelineIndex;
			const bool forward = (flags & kObjectForward) && p < tables.pipelines.size() && RasterForward(tables.pipelines[p].rasterFlags);
			const bool drawable = !(flags & kObjectShadowOnly) && !ObjectDecalGroup(flags) && p < tables.pipelines.size() &&
			                      !RasterRefraction(tables.pipelines[p].rasterFlags) &&
			                      (forward || !((flags & kObjectForward) || Extensions::FamilyRules(tables.pipelines[p].rasterFlags).rootViewsForwardOnly));
			return static_cast<std::uint8_t>((drawable ? Extensions::ViewEntryOf(view).phase : 0u) & a_drawn);
		}
		std::uint32_t phases = 0;
		if (!(flags & kObjectShadowOnly))
			phases |= kSetMain;
		if (const auto p = tables.objects[a_slot].pipelineIndex; !(flags & kObjectShadowOnly) && p < tables.pipelines.size() &&
			LodLightingTechnique(tables.pipelines[p].passDescriptor))
			phases |= Extensions::Views().lodPhases;
		if (!(flags & kObjectNoShadow))
			phases |= kSetCaster | kSetCasterPoint;
		const auto& maps = Extensions::ShadowViewsSet().maps;
		if (a_slot < tables.occlusionTechnique[kOcclusionSky].size() && tables.occlusionTechnique[kOcclusionSky][a_slot])
			phases |= maps[kOcclusionSky].phase;
		if (a_slot < tables.occlusionTechnique[kOcclusionPrecipitation].size() && tables.occlusionTechnique[kOcclusionPrecipitation][a_slot])
			phases |= maps[kOcclusionPrecipitation].phase;
		return static_cast<std::uint8_t>(phases & a_drawn);
	}

	Api::EngineView Coordinator::ViewRootOfSlot(std::uint32_t a_slot) const
	{
		if (!FirstViewRoot() || a_slot >= tables.objectGeometry.size() || !tables.objectGeometry[a_slot])
			return Api::EngineView::Count;
		const auto entry = tracked.find(tables.objectGeometry[a_slot]);
		return entry != tracked.end() ? ViewRootOf(entry->second.categoryNode) : Api::EngineView::Count;
	}

	bool Coordinator::MainReady(std::uint32_t a_slot, std::uint32_t& a_why) const
	{
		return MainReadyWith(a_slot, tables.objects[a_slot].pipelineIndex, tables.objects[a_slot].materialIndex, a_why);
	}

	bool Coordinator::MainReadyWith(std::uint32_t a_slot, std::uint32_t a_pipeline, std::uint32_t a_material, std::uint32_t& a_why) const
	{
		// What a main build's pair resolution needs from this pass's lookups. A member that fails the tables' own checks is a defect set parity reports.
		const auto& object = tables.objects[a_slot];
		const std::uint32_t p = a_pipeline, m = a_material;
		if ((object.flags & kObjectNoBindings) || !PipelineDrawableIn(tables, lookups, p))
			return a_why = 0, false;
		// The engine keeps a refraction object until the catalog has the refraction pipeline.
		if (RasterRefraction(tables.pipelines[p].rasterFlags) && !(lookupsCatalog && lookupsCatalog->refraction.pipeline))
			return a_why = 11, false;
		if (m >= lookups.materials.size() || m >= tables.materialSlotKey.size() || !lookups.materials[m].resolved || lookups.materials[m].key != tables.materialSlotKey[m])
			return a_why = 1, false;
		if (!tables.PipelineConstantsCurrent(p) || !tables.TechniqueConstantsValid(p))
			return a_why = 9, false;
		if (tables.TechniqueShadowMask(p) && lookups.pipelines[p].shadowMaskIndex == Lookups::kNone)
			return a_why = 2, false;
		if (!lookups.samplersResolved || lookups.nullTexture == Lookups::kNone)
			return a_why = 3, false;
		// A ProjectedUV pipeline, except Hair (technique 6), binds the projected textures.
		if (const auto pass = tables.pipelines[p].passDescriptor; (pass & 0x8000u) && ((pass >> 24) & 0x3f) != 6)
			for (const auto index : lookups.projectedTextures)
				if (index == Lookups::kNone)
					return a_why = 3, false;
		if (object.geometryIndex >= tables.geometries.size() || !tables.geometries[object.geometryIndex].vertexAddress ||
			!tables.geometries[object.geometryIndex].indexAddress)
			return a_why = 4, false;
		if (!IndirectDraws::Get().FitsScene(&tables, a_slot, false))
			return a_why = 10, false;
		// A decal needs its ordered slots in its group's range, one per partition for blood decals.
		if (const std::uint32_t group = ObjectDecalGroup(object.flags))
			if (a_slot >= tables.rangeOrdinal.size() || tables.rangeOrdinal[a_slot] >= tables.rangeCount[group - 1])
				return a_why = 5, false;
		return true;
	}

	void Coordinator::QueueSet(std::uint32_t a_slot)
	{
		if (a_slot >= setQueueMark.size() || setQueueMark[a_slot])
			return;
		setQueueMark[a_slot] = 1;
		setQueue.push_back(a_slot);
	}

	void Coordinator::LeaveSet(std::uint32_t a_slot, std::uint8_t a_lost, bool a_partner)
	{
		// A claim is the whole object, so losing any phase loses all of them.
		if (a_slot < setPhasesNext.size() && (setPhasesNext[a_slot] & a_lost))
			a_lost = setPhasesNext[a_slot];
		if (a_slot < setPhasesNext.size() && (setPhasesNext[a_slot] & a_lost)) {
			setPhasesNext[a_slot] &= static_cast<std::uint8_t>(~a_lost);
			// The engine's copy is keyed by base geometry, and is this slot's only while it owns that geometry.
			if (const auto* geometry = a_slot < setGeometry.size() ? setGeometry[a_slot] : nullptr; geometry && setBuilding) {
				const auto owner = setMemberSlot.find(geometry);
				const bool owns = owner != setMemberSlot.end() && owner->second == a_slot;
				if (setPhasesNext[a_slot]) {
					if (owns) {
						setBuilding->phases.insert_or_assign(geometry, setPhasesNext[a_slot]);
						NoteSetPhases(geometry);
					}
				} else {
					if (owns) {
						setMemberSlot.erase(owner);
						setBuilding->phases.erase(geometry);
						NoteSetPhases(geometry);
					}
					setGeometry[a_slot] = nullptr;
				}
				setSnapshotDirty = owns || setSnapshotDirty;
			}
			++setStats.leftAfterCommit;
			QueueSet(a_slot);
			if (a_slot < tables.objects.size())
				MarkSetApply(a_slot);
		}
		if (!a_partner || !(a_lost & kSetMain) || a_slot >= tables.objects.size())
			return;
		const std::uint32_t partner = tables.IsLayer(a_slot) ? tables.layerBase[a_slot] : a_slot < tables.layerOf.size() ? tables.layerOf[a_slot] : kNoObjectSlot;
		if (partner == kNoObjectSlot || partner >= tables.objects.size())
			return;
		const auto mainPhases = static_cast<std::uint8_t>(kSetMain | Extensions::Views().lodPhases);
		if (PhasesIn(tables, partner) & mainPhases) {
			tables.setPhases[partner] = 0;
			tables.objects[partner].flags &= ~kObjectMember;
			tables.NoteChange(partner, kChangeBindings);
		}
		LeaveSet(partner, mainPhases, false);
	}

	void Coordinator::MarkSetApply(std::uint32_t a_slot)
	{
		const std::size_t objects = tables.objects.size();
		if (setApplyMark.size() < objects)
			setApplyMark.resize(objects, 0);
		if (setGeometryNext.size() < objects)
			setGeometryNext.resize(objects, nullptr);
		setGeometryNext[a_slot] = tables.objectGeometry[a_slot];
		if (const std::uint32_t at = setApplyMark[a_slot]) {
			setApply[at - 1].second = tables.objectGeometry[a_slot];
			return;
		}
		setApply.emplace_back(a_slot, tables.objectGeometry[a_slot]);
		setApplyMark[a_slot] = static_cast<std::uint32_t>(setApply.size());
	}

	std::size_t Coordinator::TakeClaimsEntry()
	{
		for (std::size_t i = 0; i < claimsPool.size(); ++i)
			if (claimsPool[i].snapshot.use_count() == 1) {
				std::atomic_thread_fence(std::memory_order_acquire);  // after the last holder's release (use_count reads relaxed)
				return i;
			}
		claimsPool.push_back({ std::make_shared<SetSnapshot>(), {}, {} });
		return claimsPool.size() - 1;
	}

	void Coordinator::RefreshPending()
	{
		// The hooks withhold waiting slots from the engine until they join. Made whole only when a waiting slot was decided.
		if (!streamWaitingSince.empty())
			setPendingDirty = true;
		if (!std::exchange(setPendingDirty, false))
			return;
		PendingMap next, nextWhy;
		const bool observe = Diagnostics::Observe().stats;
		for (auto it = streamWaitingSince.begin(); it != streamWaitingSince.end();) {
			const auto found = tracked.find(const_cast<RE::BSGeometry*>(it->first));
			if (found == tracked.end() || !found->second.streamWaiting) {
				it = streamWaitingSince.erase(it);
				continue;
			}
			if (sceneBuilt && sceneFrame - it->second <= kStreamWaitFrames)
				if (const std::uint8_t phases = static_cast<std::uint8_t>(kCoreSetPhases & setPhaseMask)) {
					next.try_emplace(it->first, phases);
					if (observe)
						nextWhy.try_emplace(it->first, kPendingComponentWhy);
				}
			++it;
		}
		if (sceneBuilt) {
			const std::uint32_t drawn = setPhaseMask;
			for (const std::uint32_t slot : setWaiting) {
				if (slot >= setWaitingMark.size() || !setWaitingMark[slot] || slot >= tables.objects.size() || tables.IsLayer(slot))
					continue;
				const auto* geometry = tables.objectGeometry[slot];
				if (const std::uint8_t phases = geometry ? SetParticipation(slot, drawn) : std::uint8_t{ 0 }) {
					next.insert_or_assign(geometry, phases);
					if (observe)
						nextWhy.insert_or_assign(geometry, slot < setWaitWhy.size() ? setWaitWhy[slot] : std::uint8_t{ 0 });
				}
			}
		}
		const auto same = [](const PendingMap& a_next, const PendingMap& a_last) {
			return a_next.size() == a_last.size() && std::ranges::all_of(a_next, [&](const auto& a_entry) {
				const auto it = a_last.find(a_entry.first);
				return it != a_last.end() && it->second == a_entry.second;
			});
		};
		if (same(next, setPending) && same(nextWhy, setPendingWhy))
			return;
		setPending = std::move(next);
		setPendingWhy = std::move(nextWhy);
		setPendingChanged = true;
	}

	void Coordinator::PublishClaims()
	{
		DCLF_ZONE("CS.DCLF.Scene.PublishClaims");
		if (!setBuilding)
			setBuilding = std::make_shared<SetSnapshot>();
		auto& pace = paceStats;
		const std::uint8_t drawn = static_cast<std::uint8_t>(setPhaseMask);
		claimsFresh = false;
		RefreshPending();
		if (claimsPublished < claimsPool.size() && publicationClaims == claimsPool[claimsPublished].snapshot && !setSnapshotDirty && !setPendingChanged) {
			const auto& last = claimsPool[claimsPublished];
			if (last.edited.empty() && last.cursor.Continues(setPhaseLog, 0) && last.cursor.position == setPhaseLog.End() && last.snapshot->drawn == drawn) {
				++pace.claimsShared;
				return;
			}
		}
		const std::size_t index = TakeClaimsEntry();
		auto& entry = claimsPool[index];
		auto& phases = entry.snapshot->phases;
		// Caught up by the keys written since its last publication, or copied whole when the log no longer reaches it.
		auto refresh = [&](const RE::BSGeometry* a_geometry) {
			if (const auto it = setBuilding->phases.find(a_geometry); it != setBuilding->phases.end())
				phases.insert_or_assign(a_geometry, it->second);
			else
				phases.erase(a_geometry);
		};
		if (entry.cursor.Continues(setPhaseLog, 0)) {
			const auto unread = entry.cursor.Unread(setPhaseLog);
			for (const auto* geometry : unread)
				refresh(geometry);
			for (const auto* geometry : entry.edited)
				refresh(geometry);
			++pace.claimsReplayed;
			pace.claimsKeys += unread.size() + entry.edited.size();
		} else {
			phases = setBuilding->phases;
			++pace.claimsWhole;
		}
		entry.edited.clear();
		entry.cursor.Restart(0);
		entry.cursor.Advance(setPhaseLog);
		entry.snapshot->frame = sceneFrame;
		entry.snapshot->drawn = drawn;
		entry.snapshot->pending = setPending;
		entry.snapshot->pendingWhy = setPendingWhy;
		setPendingChanged = false;
		publicationClaims = entry.snapshot;
		claimsPublished = index;
		claimsFresh = true;
		setSnapshotDirty = false;
		++setStats.publications;
		// One behind the bound is copied whole.
		setPhaseLog.Trim(1u << 16);
	}

	Coordinator::SlotDecision Coordinator::DecideSlot(std::uint32_t a_slot, bool a_live, std::uint32_t a_drawn) const
	{
		// Read-only. Claims are whole-object, so all participating phases if ready for all, else none.
		SlotDecision d;
		if (!a_live || a_slot >= tables.objects.size())
			return d;
		const std::uint8_t participation = SetParticipation(a_slot, a_drawn);
		std::uint8_t phases = participation;
		const auto& views = Extensions::Views();
		if (!phases) {
			return d;
		}
		auto waiting = [&](std::uint32_t a_why) {
			d.wait = true;
			if (d.whyCount < d.whys.size())
				d.whys[d.whyCount++] = static_cast<std::uint8_t>(a_why);
		};
		// The main phase needs its binding. Shadow and occlusion phases draw from the record alone.
		if (phases & kSetMain) {
			bool main = IsResidentSlot(a_slot);
			if (std::uint32_t why = 0; main && !MainReady(a_slot, why)) {
				waiting(why);
				main = false;
			}
			if (!main)
				phases &= ~kSetMain;
		}
		for (std::uint32_t i = 0; i < views.decideCount; ++i) {
			const auto view = views.decideOrder[i];
			const auto& entry = Extensions::ViewEntryOf(view);
			if (!(phases & entry.phase))
				continue;
			if (entry.membership == Api::ViewMembership::CategoryRoot) {
				// A first-person view draws with the main rows and bindings, plus its own pipelines in the catalog.
				bool member = IsResidentSlot(a_slot);
				if (std::uint32_t why = 0; member && !MainReady(a_slot, why)) {
					waiting(why);
					member = false;
				}
				std::uint32_t slotWhy = 0;
				if (member && !IndirectDraws::Get().ViewSlotReady(view, &tables, &lookups, lookupsCatalog.get(), a_slot, &slotWhy)) {
					waiting(8);
					member = false;
				}
				if (!member)
					phases &= static_cast<std::uint8_t>(~entry.phase);
				if (!member) {
					d.rootViewHeld = true;
					d.heldWhyCount = d.whyCount;
					d.rootViewSlotWhy = slotWhy;
				}
			} else if (!IndirectDraws::Get().ViewSlotReady(view, &tables, &lookups, lookupsCatalog.get(), a_slot)) {
				// The reflection's faces need the slot's forward pipeline in the catalog the revision resolves from.
				waiting(8);
				phases &= static_cast<std::uint8_t>(~entry.phase);
			}
		}
		const auto& shadowViews = Extensions::ShadowViewsSet();
		for (std::uint32_t r = 0; r < shadowViews.readinessCount; ++r) {
			const std::uint8_t phase = shadowViews.readinessOrder[r];
			if (std::uint32_t why = 7; (phases & phase) && !IndirectDraws::Get().PhaseReady(&tables, lookups, a_slot, phase, &why)) {
				waiting(why);
				phases &= ~phase;
			}
		}
		if (phases == participation) {
			d.phases = phases;
			return d;
		}
		d.partial = phases != 0;
		return d;
	}

	struct Coordinator::SetCommit
	{
		std::size_t objects = 0;
		bool live = false;          // the scene is built (else the set is empty)
		std::uint32_t drawn = 0;    // the phases DCLF can draw (SetCapability)
		bool gatesLive = false;     // a component asks for veto work (CommitLive)
		bool waitingMoved = false;  // a waiting mark moved (setWaiting gained or lost a slot): else the list stands as the last compaction left it
	};

	void Coordinator::CommitMembershipEvents(const SetCommit& a_commit)
	{
		// New phases, new tables or a log the cursor cannot continue evaluate every slot.
		auto& setPhases = setPhasesNext;
		const std::size_t objects = a_commit.objects;
		const bool live = a_commit.live;
		const std::uint32_t drawn = a_commit.drawn;
		const bool everything = drawn != setPhaseMask || !setCursor.Continues(tables.changeLog, tablesGeneration) || setPhases.size() > objects;
		if (everything) {
			++setStats.resyncs;
			// A load that never finishes can resync hundreds of times a scene frame, so the lines are capped per window.
			constexpr std::uint32_t kLinesPerWindow = 8;
			constexpr std::uint32_t kWindowFrames = 600;
			static std::uint32_t logWindow = ~0u;
			static std::uint32_t linesInWindow = 0;
			static std::uint64_t skippedLines = 0;
			if (const std::uint32_t window = sceneFrame / kWindowFrames; window != logWindow) {
				logWindow = window;
				linesInWindow = 0;
			}
			if (linesInWindow < kLinesPerWindow) {
				++linesInWindow;
				logger::info("[DCLF] set resync at frame {} ({} objects, scene {}): {}{}", sceneFrame, objects, live ? "built" : "not built",
					drawn != setPhaseMask                                 ? fmt::format("phases {:#x} -> {:#x}", setPhaseMask, drawn) :
					!setCursor.Continues(tables.changeLog, tablesGeneration) ? std::string("new tables or a change log the cursor cannot read on") :
					                                                          std::string("fewer objects"),
					skippedLines ? fmt::format(" ({} lines skipped before this one)", skippedLines) : std::string());
				skippedLines = 0;
			} else {
				++skippedLines;
			}
		}
		setPhaseMask = drawn;
		if (everything) {
			setCursor.Restart(tablesGeneration);
			setPhases.assign(objects, 0);
			setGeometry.assign(objects, nullptr);
			setLackingNext.assign(objects, 0);
			setMemberSlot.clear();
			for (std::uint32_t slot = 0; slot < objects; ++slot)
				MarkSetApply(slot);
			setBuilding->phases.clear();
			setPhaseLog.Invalidate();
			setSnapshotDirty = true;
			for (const std::uint32_t slot : setWaiting)
				if (slot < setWaitingMark.size())
					setWaitingMark[slot] = 0;
			setWaiting.clear();
			for (std::uint32_t slot = 0; slot < objects; ++slot)
				QueueSet(slot);
		} else {
			setPhases.resize(objects, 0);
			setGeometry.resize(objects, nullptr);
			setLackingNext.resize(objects, 0);
			constexpr std::uint32_t kSetCauses = kChangeBindings | kChangeMembership | kChangeGeometry | kChangeShadow | kChangeSkin;
			for (const auto& change : setCursor.Unread(tables.changeLog))
				if (change.causes & kSetCauses)
					QueueSet(change.slot);
		}
		setCursor.Advance(tables.changeLog);
	}

	void Coordinator::CommitReadinessSources()
	{
		// A waiting slot is requeued when a source it waits on moved, or any did for kWaitOther.
		const std::array<std::uint64_t, kWaitSources> readiness{ (std::uint64_t(lookups.versionCounter) << 32) ^ lookups.generation, lookups.shadowGeneration,
			tables.constantsStamp, IndirectDraws::Get().SceneFitSerial() };
		std::uint8_t moved = 0;
		for (std::uint32_t source = 0; source < kWaitSources; ++source)
			if (readiness[source] != setReadiness[source]) {
				setReadiness[source] = readiness[source];
				moved |= std::uint8_t(1u << source);
			}
		// Forward-view pipelines arrive in a new catalog without new lookups. Counted once per immutable catalog.
		{
			if (lookupsCatalog != setForwardCatalog || !setForwardCatalog) {
				std::uint64_t count = 0;
				if (lookupsCatalog)
					for (const auto& [key, entry] : lookupsCatalog->forwardEntries)
						count += entry.pipeline ? 1u : 0u;
				setForwardCatalog = lookupsCatalog;
				setForwardCount = count;
				++paceStats.forwardCounted;
			} else {
				++paceStats.forwardKept;
			}
			const std::uint64_t forwardReady = setForwardCount;
			if (forwardReady != setForwardReadiness) {
				setForwardReadiness = forwardReady;
				moved |= kWaitOther;
			}
		}
		if (moved) {
			++setStats.readinessEvents;
			for (const std::uint32_t slot : setWaiting)
				if (slot < setWaitCause.size() && (setWaitCause[slot] & (moved | kWaitOther))) {
					QueueSet(slot);
					++setStats.waitingRequeued;
				}
			for (const auto& [slot, staged] : stagedBindings)
				bindRetry.push_back(slot);
		}
		// The fit can shrink too, so a member past it is decided again.
		if (moved & kWaitFit) {
			auto& draws = IndirectDraws::Get();
			const std::size_t objects = std::min(setPhasesNext.size(), tables.objects.size());
			for (std::uint32_t slot = 0; slot < objects; ++slot) {
				const std::uint8_t phases = setPhasesNext[slot];
				if (((phases & kSetMain) && !draws.FitsScene(&tables, slot, false)) || ((phases & ~kSetMain) && !draws.FitsScene(&tables, slot, true))) {
					QueueSet(slot);
					++setStats.fitRequeued;
				}
			}
		}
	}

	void Coordinator::CommitShadowModeRequeues(std::size_t a_objects)
	{
		// New shadow view modes or rasterizer states may need pipelines a caster lacks. Read from the lane's lookups, never the render thread's.
		auto& setPhases = setPhasesNext;
		if (const std::uint64_t modes = lookups.shadowInputsSerial; modes != setShadowModes) {
			setShadowModes = modes;
			for (std::uint32_t slot = 0; slot < a_objects; ++slot)
				if (setPhases[slot] & ~kSetMain)
					QueueSet(slot);
			for (const std::uint32_t slot : setWaiting)
				QueueSet(slot);
		}
	}

	std::uint8_t Coordinator::CommitDecideSlot(std::uint32_t a_slot, const SetCommit& a_commit, bool& a_wait, bool* a_partial)
	{
		if (a_slot < setWaitCause.size())
			setWaitCause[a_slot] = 0;
		const SlotDecision decision = DecideSlot(a_slot, a_commit.live, a_commit.drawn);
		a_wait = decision.wait;
		if (a_partial && decision.partial)
			*a_partial = true;
		for (std::uint8_t w = 0; w < decision.whyCount; ++w) {
			const std::uint32_t why = decision.whys[w];
			if (!setWaitCause[a_slot])
				setWaitWhy[a_slot] = static_cast<std::uint8_t>(why);
			setWaitCause[a_slot] |= WaitCauseOf(why);
			++setStats.waitingBy[why];
			if (setStats.firstWaiting.empty())
				if (const auto* geometry = tables.objectGeometry[a_slot])
					setStats.firstWaiting = fmt::format("'{}' (slot {}, reason {})", geometry->name.c_str() ? geometry->name.c_str() : "?", a_slot, why);
		}
		return decision.phases;
	}

	void Coordinator::CommitApplySlot(std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait, SetCommit& a_commit)
	{
		auto& setPhases = setPhasesNext;
		const bool live = a_commit.live;
		const std::uint32_t drawn = a_commit.drawn;
		if (setApplyMark[a_slot])
			MarkSetApply(a_slot);
		const std::uint8_t lacking =
			live ? static_cast<std::uint8_t>(SetParticipation(a_slot, drawn) & ~a_phases & Extensions::ResolvedViewBits().lackingMask) : 0;
		if (lacking != setLackingNext[a_slot]) {
			setLackingNext[a_slot] = lacking;
			MarkSetApply(a_slot);
		}
		if (a_wait != (setWaitingMark[a_slot] != 0)) {
			setWaitingMark[a_slot] = a_wait ? 1 : 0;
			a_commit.waitingMoved = true;
			if (a_wait)
				setWaiting.push_back(a_slot);  // compacted below
		}
		if (a_wait || a_commit.waitingMoved)
			setPendingDirty = true;
		const std::uint8_t before = setPhases[a_slot];
		if (before == a_phases)
			return;
		setPhases[a_slot] = a_phases;
		MarkSetApply(a_slot);  // the record's kObjectMember and Tables::setPhases, at ApplySet
		if (!before || !a_phases)
			++(a_phases ? setStats.joined : setStats.left);
		if (before && !a_phases) {
			const std::uint8_t participation = live ? SetParticipation(a_slot, drawn) : std::uint8_t{ 0 };
			const std::size_t cause = !live                                                    ? 0 :
			                          !participation                                           ? 1 :
			                          ((participation & kSetMain) && !IsResidentSlot(a_slot))  ? 2 :
			                          (a_slot < setWaitCause.size() && setWaitCause[a_slot])   ? 3 :
			                                                                                     4;
			++setStats.leftBy[cause];
			if (cause == 3 && a_slot < setWaitWhy.size())
				++setStats.leftWaiting[(std::min<std::size_t>)(setWaitWhy[a_slot], setStats.leftWaiting.size() - 1)];
		}
		// The engine's copy is keyed by base geometry, so a layer is a member with its base.
		if (tables.IsLayer(a_slot))
			return;
		if (const auto* old = setGeometry[a_slot]; old && !a_phases) {
			if (const auto owner = setMemberSlot.find(old); owner != setMemberSlot.end() && owner->second == a_slot) {
				setMemberSlot.erase(owner);
				setBuilding->phases.erase(old);
				NoteSetPhases(old);
			}
			setGeometry[a_slot] = nullptr;
		}
		if (a_phases)
			if (const auto* geometry = tables.objectGeometry[a_slot]) {
				setMemberSlot.insert_or_assign(geometry, a_slot);
				setBuilding->phases.insert_or_assign(geometry, a_phases);
				NoteSetPhases(geometry);
				setGeometry[a_slot] = geometry;
			}
		setSnapshotDirty = true;
	}

	void Coordinator::CommitQueuedSlots(SetCommit& a_commit)
	{
		const std::size_t objects = a_commit.objects;
		const std::uint32_t drawn = a_commit.drawn;
		const bool live = a_commit.live;
		for (std::size_t q = 0; q < setQueue.size(); ++q) {
			const std::uint32_t slot = setQueue[q];
			if (slot >= objects)
				continue;
			// The engine's main passes of both are the base geometry's, so they are members together.
			const std::uint32_t partner = tables.IsLayer(slot) ? tables.layerBase[slot] : slot < tables.layerOf.size() ? tables.layerOf[slot] : kNoObjectSlot;
			bool wait = false, partial = false;
			std::uint8_t phases = CommitDecideSlot(slot, a_commit, wait, &partial);
			if (setPartialMark.size() < objects)
				setPartialMark.resize(objects, 0);
			if ((setPartialMark[slot] != 0) != partial) {
				setPartialMark[slot] = partial ? 1 : 0;
				setPartialCount += partial ? 1u : std::uint32_t(-1);
			}
			if (partner != kNoObjectSlot && partner < objects) {
				bool partnerWait = false;
				std::uint8_t partnerPhases = CommitDecideSlot(partner, a_commit, partnerWait);
				if (!(phases & kSetMain) != !(partnerPhases & kSetMain)) {
					++setStats.waitingBy[6];
					setWaitCause[slot] |= kWaitOther;
					setWaitCause[partner] |= kWaitOther;
					phases = 0;
					partnerPhases = 0;
				}
				// Gated incoming blocks hold their phases until the flip. Outgoing ones drop after it.
				if (a_commit.gatesLive)
					partnerPhases = VetoedPhases(partner, partnerPhases, partnerWait, drawn, live);
				CommitApplySlot(partner, partnerPhases, partnerWait, a_commit);
			}
			if (a_commit.gatesLive)
				phases = VetoedPhases(slot, phases, wait, drawn, live);
			CommitApplySlot(slot, phases, wait, a_commit);
		}
	}

	void Coordinator::CommitComponentsSettle(SetCommit& a_commit)
	{
		// Gates flip here and ride the publication. The vetoes they change apply before the claims are snapshotted.
		settleApply.clear();
		settling = true;
		ComponentsSettle(a_commit.live);
		settling = false;
		for (const auto& [slot, phases] : settleApply)
			if (slot < a_commit.objects)
				CommitApplySlot(slot, phases, false, a_commit);
		settleApply.clear();
	}

	void Coordinator::CommitSet()
	{
		DCLF_ZONE("CS.DCLF.Scene.CommitSet");
		++setStats.commits;
		SetCommit commit;
		const std::size_t objects = tables.objects.size();
		commit.objects = objects;
		const bool live = sceneBuilt;
		commit.live = live;
		const std::uint32_t drawn = live ? SetCapability() : 0u;
		commit.drawn = drawn;
		setQueueMark.resize(objects, 0);
		setWaitingMark.resize(objects, 0);
		setLackingNext.resize(objects, 0);
		setApplyMark.resize(objects, 0);
		setWaitCause.resize(objects, 0);
		setWaitWhy.resize(objects, 0);
		setCommitFrame = sceneFrame;
		setCommitToggles = sceneInputs.togglesGeneration;
		setCommitToggleSet = sceneInputs.toggles;
		setGeometryNext.resize(objects, nullptr);
		if (!setBuilding)
			setBuilding = std::make_shared<SetSnapshot>();
		const bool gatesLive = CommitLive();
		commit.gatesLive = gatesLive;
		for (const std::uint32_t slot : requeued)
			QueueSet(slot);
		requeued.clear();

		CommitMembershipEvents(commit);
		CommitReadinessSources();
		CommitShadowModeRequeues(objects);
		// Whatever the joins rebound or dropped earlier in this pass is queued through LeaveSet and the change log.

		CommitQueuedSlots(commit);
		if (gatesLive)
			CommitComponentsSettle(commit);
		setStats.evaluated += setQueue.size();
		for (const std::uint32_t slot : setQueue)
			if (slot < objects)
				setQueueMark[slot] = 0;
		setQueue.clear();
		if (commit.waitingMoved) {
			std::erase_if(setWaiting, [&](std::uint32_t a_slot) { return a_slot >= setWaitingMark.size() || !setWaitingMark[a_slot]; });
			std::sort(setWaiting.begin(), setWaiting.end());
			setWaiting.erase(std::unique(setWaiting.begin(), setWaiting.end()), setWaiting.end());
			++paceStats.waitingCompacted;
		} else {
			++paceStats.waitingKept;
		}
		setStats.members += setMemberSlot.size();
		setStats.partial += setPartialCount;
		setStats.waiting += setWaiting.size();

		setCommitCursor.Restart(tablesGeneration);
		setCommitCursor.Advance(tables.changeLog);
	}

	void Coordinator::ApplySet()
	{
		DCLF_ZONE("CS.DCLF.Scene.ApplySet");
		const std::size_t objects = tables.objects.size();
		tables.setPhases.resize(objects, 0);
		auto& applied = tables.setPhases;
		setLacking.resize(objects, 0);
		setPhasesNext.resize(objects, 0);
		setLackingNext.resize(objects, 0);
		setPhasesApplied.resize(objects, 0);
		setGeometryApplied.resize(objects, nullptr);
		auto& joined = publicationJoined;
		auto& left = publicationLeft;
		for (const auto& [slot, geometry] : setApply) {
			if (slot < setApplyMark.size())
				setApplyMark[slot] = 0;
			if (slot >= objects)
				continue;
			// A slot freed or holding another geometry since the commit takes nothing.
			const bool same = !(tables.objects[slot].flags & kObjectFree) && tables.objectGeometry[slot] == geometry;
			const std::uint8_t phases = same ? setPhasesNext[slot] : std::uint8_t{ 0 };
			const std::uint8_t lacking = same ? setLackingNext[slot] : std::uint8_t{ 0 };
			const auto* wasClaimed = (setPhasesApplied[slot] & kSetMain) ? setGeometryApplied[slot] : nullptr;
			setPhasesApplied[slot] = phases;
			setGeometryApplied[slot] = phases && !tables.IsLayer(slot) ? geometry : nullptr;
			const auto* claimed = (phases & kSetMain) ? setGeometryApplied[slot] : nullptr;
			if (wasClaimed != claimed) {
				if (wasClaimed)
					left.push_back(wasClaimed);
				if (claimed)
					joined.push_back(claimed);
			}
			if (lacking != setLacking[slot]) {
				for (std::uint32_t v = 0; v < 3; ++v) {
					const std::uint8_t bit = Extensions::ResolvedViewBits().lackingPhases[v];
					setLackingCount[v] += ((lacking & bit) ? 1u : 0u) - ((setLacking[slot] & bit) ? 1u : 0u);
				}
				setLacking[slot] = lacking;
			}
			if (applied[slot] == phases)
				continue;
			// kObjectMember is the main builds' GPU input. The shadow builds read the phases.
			applied[slot] = phases;
			if (!(tables.objects[slot].flags & kObjectFree)) {
				auto& object = tables.objects[slot];
				object.flags = (phases & kSetMain) ? (object.flags | kObjectMember) : (object.flags & ~kObjectMember);
				tables.NoteChange(slot, kChangeBindings);
			} else {
				tables.NotePublished(slot);
			}
		}
		setApply.clear();
		if (setCommitCursor.Continues(tables.changeLog, tablesGeneration)) {
			claimCheckCursor = setCommitCursor;
		} else {
			claimCheckCursor.Restart(tablesGeneration);
			claimCheckCursor.Advance(tables.changeLog);
		}
		publicationCommitFrame = setCommitFrame;
		publicationCommitToggles = setCommitToggles;
		publicationCommitToggleSet = setCommitToggleSet;
	}

	std::uint8_t Coordinator::VetoedPhases(std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait, std::uint32_t a_drawn, bool a_live)
	{
		const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
		const auto it = geometry ? tracked.find(const_cast<RE::BSGeometry*>(geometry)) : tracked.end();
		if (it == tracked.end())
			return a_phases;
		const std::uint8_t veto = it->second.veto;
		if (veto == Api::kVetoHold)
			return 0;
		if (veto != Api::kVetoObserve)
			return a_phases;
		if (observedSlots.size() < tables.objects.size())
			observedSlots.resize(tables.objects.size());
		auto& held = observedSlots[a_slot];
		held.geometry = geometry;
		held.phases = a_phases;
		held.ready = !a_wait && a_phases == (a_live ? SetParticipation(a_slot, a_drawn) : std::uint8_t{ 0 });
		return 0;
	}

	bool Coordinator::EntryVetoed(std::uint32_t a_slot) const
	{
		const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
		if (!geometry)
			return false;
		const auto it = tracked.find(const_cast<RE::BSGeometry*>(geometry));
		return it != tracked.end() && it->second.veto != 0;
	}

	void Coordinator::CheckClaimsDrawable()
	{
		DCLF_ZONE("CS.DCLF.Scene.CheckClaimsDrawable");
		// The commit judged every slot the log named, and nothing writes the tables before this publication. So any claimed phase the
		// tables do not draw is a defect. It is reported and the claim stands.
		auto check = [&](std::uint32_t a_slot) {
			if (a_slot >= setPhasesApplied.size() || a_slot >= setGeometryApplied.size())
				return;
			const std::uint8_t claimed = setPhasesApplied[a_slot];
			const auto* geometry = setGeometryApplied[a_slot];
			if (!claimed || !geometry)
				return;
			const bool same = a_slot < tables.objects.size() && !(tables.objects[a_slot].flags & kObjectFree) && tables.objectGeometry[a_slot] == geometry;
			std::uint8_t drawn = 0;
			if (same) {
				const auto flags = tables.objects[a_slot].flags;
				auto& draws = IndirectDraws::Get();
				const bool main = (flags & kObjectMember) && !(flags & kObjectNoBindings) && draws.FitsScene(&tables, a_slot, false);
				// A first-person member draws with main bindings but has no kObjectMember.
				const auto& views = Extensions::Views();
				const auto root = static_cast<std::uint8_t>(tables.setPhases[a_slot] & views.rootPhases);
				const bool rootMember = root && !(flags & kObjectNoBindings) && draws.FitsScene(&tables, a_slot, false);
				const std::uint8_t others = draws.FitsScene(&tables, a_slot, true) ? static_cast<std::uint8_t>(tables.setPhases[a_slot] & ~(kSetMain | views.phases)) : 0;
				drawn = static_cast<std::uint8_t>((main ? (kSetMain | views.lodPhases) : 0u) | (rootMember ? root : 0u) | others);
			}
			if (!(claimed & ~drawn))
				return;
			if (undrawnClaims++ == 0)
				undrawnFirst = fmt::format("slot {} claimed {:#x}, its record draws {:#x}{}", a_slot, claimed, drawn, same ? "" : " (slot freed or reassigned)");
		};
		if (claimCheckCursor.Continues(tables.changeLog, tablesGeneration)) {
			for (const auto& change : claimCheckCursor.Unread(tables.changeLog))
				check(change.slot);
		} else {
			for (std::uint32_t slot = 0; slot < setPhasesApplied.size(); ++slot)
				check(slot);
		}
		claimCheckCursor.Restart(tablesGeneration);
		claimCheckCursor.Advance(tables.changeLog);
	}

	namespace
	{
		std::mutex& LatestSceneMutex()
		{
			static std::mutex mutex;
			return mutex;
		}
		PublishedScene& LatestScene()
		{
			static PublishedScene scene;
			return scene;
		}
	}

	void Coordinator::PublishScene()
	{
		DCLF_ZONE("CS.DCLF.Scene.PublishScene");
		// A pass whose only changes are frame deltas publishes the last tables again. The snapshot builder then reposts the last snapshot.
		const auto& pace = paceStats;
		bool deltasOnly = publishedKeyValid && !publishForced && publishedTables && setApply.empty() && !setSnapshotDirty && !nodeSetsDirty &&
		                  publicationJoined.empty() && publicationLeft.empty() &&
		                  componentSeedRequests.empty() && switchesApplied.empty() && !switchResync && !tables.ClassMemberChangesPending() &&
		                  componentDeltaRequests.empty() && CurrentPublishKey() == publishedKey;
		const auto sharedBefore = pace.claimsShared, wholeBefore = pace.claimsWhole;
		ApplySet();
		PublishClaims();
		CheckClaimsDrawable();
		deltasOnly = deltasOnly && pace.claimsShared != sharedBefore && pace.claimsWhole == wholeBefore &&
		             CurrentPublishKey() == publishedKey;
		// A second resolve here could take a newer catalog than the commit judged.
		PublicationDeltas deltas;
		{
			DCLF_ZONE("CS.DCLF.Scene.TakeDeltas");
			TakeDeltas(deltas);
		}
		if (!deltasOnly)
			PublishTables();
		PublishLookups();
		DCLF_ZONE_NAMED(postSnapshotZone, "CS.DCLF.Scene.PostSnapshotWork", true);
		auto publication = std::make_shared<ScenePublication>();
		publication->sequence = ++publicationSequence;
		publication->commitFrame = publicationCommitFrame;
		publication->togglesGeneration = publicationCommitToggles;
		publication->toggles = publicationCommitToggleSet;
		publication->tablesGeneration = tablesGeneration;
		publication->tables = publishedTables;
		publication->retirementHold = publishedRetirement;
		publication->claims = publicationClaims ? publicationClaims : std::make_shared<const SetSnapshot>();
		publication->lackingCount = setLackingCount;
		// Read by a load's hold.
		static_assert(std::tuple_size_v<decltype(SetStats::waitingBy)> == kSetWaitReasons, "the published waiting reasons are SetStats::waitingBy's");
		publication->built = sceneBuilt;
		for (const std::uint32_t slot : setWaiting)
			if (slot < setWaitingMark.size() && setWaitingMark[slot] && slot < setWaitWhy.size())
				++publication->waiting[(std::min<std::size_t>)(setWaitWhy[slot], kSetWaitReasons - 1)];
		publication->settled = sceneBuilt && !publication->TransientWaiting();
		publication->lookups = publishedLookups;
		{
			std::scoped_lock lock(LatestSceneMutex());
			LatestScene() = { publication->tables, publication->lookups };
		}
		publication->catalog = publishedCatalog;
		// The frame reads candidates only from the publication, never the coordinator.
		publication->sunCandidates = sunCandidates;
		publication->lightCandidates = lightCandidates;
		publication->lightEntriesAppeared = lightEntriesAppeared;
		if (std::exchange(nodeSetsDirty, false) || !publishedCategoryNodes) {
			DCLF_ZONE("CS.DCLF.Scene.PublishCategoryNodes");
			publishedCategoryNodes = std::make_shared<const ankerl::unordered_dense::set<RE::NiNode*>>(categoryNodes);
		}
		publication->categoryNodes = publishedCategoryNodes;
		auto& revision = publication->revision;
		revision.pipelines = static_cast<std::uint32_t>(tables.pipelines.size());
		revision.shadowSlotKeys = lookups.shadowSlotKeys.size();
		revision.shadowKeys = tables.shadowKeysUsed.size();
		for (const auto& used : tables.occlusionKeysUsed)
			revision.shadowKeys += used.size();
		publication->eventNs = std::exchange(unpublishedNs, 0);
		publication->eventFrame = std::exchange(unpublishedFrame, 0u);
		if (publication->eventNs) {
			const auto nowNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			const std::uint32_t frameNow = DCLF::SceneClock::Step();
			pumpStats.publishMs.push_back(nowNs > publication->eventNs ? static_cast<double>(nowNs - publication->eventNs) / 1.0e6 : 0.0);
			pumpStats.publishFrames.push_back(frameNow >= publication->eventFrame ? frameNow - publication->eventFrame : 0u);
		}
		++pumpStats.published;
		// Deltas first, so whoever adopts the publication finds its node on the log.
		deltas.publication = publication->sequence;
		deltas.commitFrame = publication->commitFrame;
		deltas.eventNs = publication->eventNs;
		deltas.eventFrame = publication->eventFrame;
		channels.publicationLog.Append(std::move(deltas));
		// A skipped publication loses nothing of the log.
		IndirectDraws::Get().PostSnapshotWork(std::move(publication));
	}

	void Coordinator::TakeDeltas(PublicationDeltas& a_deltas)
	{
		a_deltas.joined = std::exchange(publicationJoined, {});
		a_deltas.left = std::exchange(publicationLeft, {});
		for (auto& held : placementPlansHeld)
			a_deltas.plans.push_back(std::move(held));
		placementPlansHeld.clear();
		if (placementPlanReady)
			a_deltas.plans.push_back(std::exchange(placementPlanReady, nullptr));
		a_deltas.shading = std::exchange(shadingNamed, {});
		a_deltas.componentSeeds = std::exchange(componentSeedRequests, {});
		// No cap, since PrimaryCull reads them all only on a full walk.
		a_deltas.switches = std::exchange(switchesApplied, {});
		a_deltas.switchResync = std::exchange(switchResync, false);
		a_deltas.retiredImports = std::exchange(retiredImports, {});
		for (std::size_t c = 0; c < a_deltas.classMembers.size(); ++c)
			a_deltas.classMembers[c] = std::exchange(tables.classMembers[c].changes, {});
		a_deltas.lightEntries = std::exchange(lightEntryChanges, {});
		// The LOD gates' observers read the pending flips before the deltas are taken.
		ComponentsPublished();
		a_deltas.componentDeltas = std::exchange(componentDeltaRequests, {});
	}

	void Coordinator::DropReturnedPublications()
	{
		channels.retiredPublications.Drop();
	}

	void Coordinator::TakeFrameInputs()
	{
		DCLF_ZONE("CS.DCLF.Scene.TakeFrameInputs");
		// Once a pass at its start, so its globals and inputs are one frame's.
		const bool taken = channels.frameInputs.TakeInto(sceneInputs);
		// Never the live toggles, which the render thread may apply mid-pass.
		if (!sceneInputs.frame)
			sceneInputs.toggles = Toggles::Get().Active();
		passSnapshot.toggles = sceneInputs.toggles;
		if (!taken)
			return;
		passTookFrameInputs = true;
		sceneFrame = sceneInputs.frame;
		if (sceneInputs.verdictsGeneration != verdictsApplied) {
			verdictsApplied = sceneInputs.verdictsGeneration;
			InvalidateVerdicts();
		}
	}

	void Coordinator::TakeCoordinatorInputs()
	{
		DCLF_ZONE("CS.DCLF.Scene.TakeCoordinatorInputs");
		// Order matters. The journaled families' sent epochs first, then PrimaryCull's admitted roots, which force a publication.
		tables.families.TakeSent();
		if (const auto roots = channels.standInRootsPosted.Take()) {
			ComponentsAdmitted(*roots);
			publishForced = true;
		}
	}

	void Coordinator::PublishIfDue()
	{
		// A publication the builder has not taken would only be replaced, so the pass defers and its changes ride the next one.
		DCLF_SCENE_PART(Publish, "CS.DCLF.Scene.Publish");
		auto& pace = paceStats;
		++pace.passes;
		if (sceneFrame != pace.lastFrame) {
			pace.lastFrame = sceneFrame;
			++pace.frames;
			pace.passesThisFrame = pace.publishedThisFrame = 0;
		}
		pace.maxPasses = (std::max)(pace.maxPasses, ++pace.passesThisFrame);
		bool needed = false;
		{
			DCLF_ZONE("CS.DCLF.Scene.PublicationNeeded");
			needed = PublicationNeeded();
		}
		if (needed) {
			if (PublishReason why{}; PublicationDue(why)) {
				const std::uint32_t folded = std::exchange(passesDeferred, 0u);
				PublishScene();
				NotePublished();
				++pace.published;
				++pace.byReason[static_cast<std::size_t>(why)];
				pace.folded += folded;
				pace.maxPublished = (std::max)(pace.maxPublished, ++pace.publishedThisFrame);
			} else {
				++passesDeferred;
				passDeferred = true;
				++pace.deferred;
			}
		}
	}

	void Coordinator::RunSceneWork()
	{
		// A record bound in this pass is committed in it and published with it or the next publishing pass.
		inSceneTask = true;
		holdPrimaryNotes = true;
		const auto workStart = std::chrono::steady_clock::now();
		{
			DCLF_ZONE("CS.DCLF.Scene.TakeAheadContext");
			IndirectDraws::Get().TakeAheadContext();
		}
		++passSerial;
		{
			DCLF_ZONE("CS.DCLF.Scene.DropReturnedPublications");
			DropReturnedPublications();
		}
		RecycleRetired();
		// While DCLF is not running a pass applies events only, so the tracked set is current when it resumes.
		const bool full = passesFull.load(std::memory_order_acquire);
		if (full) {
			TakeCoordinatorInputs();
			DCLF_ZONE("CS.DCLF.Scene.TakeAccumulateInputs");
			TakeAccumulateInputs();
		}
		{
			DCLF_ZONE("CS.DCLF.Scene.TakeCategoryCapture");
			TakeCategoryCapture();
		}
		{
			// Gate outcomes are taken before the tracker's stack, since an outcome is posted after its gate's events. A load screen's batch carries them unapplied.
			DCLF_ZONE("CS.DCLF.Scene.TakeGateOutcomes");
			ComponentsTake();
		}
		{
			auto batch = CollectEvents();
			const bool applied = batch != nullptr;
			{
				DCLF_ZONE("CS.DCLF.Scene.ApplyEvents");
				ApplyEvents(std::move(batch));
			}
			DCLF_ZONE("CS.DCLF.Scene.ApplyGateOutcomes");
			if (applied)
				ComponentsChannelKeys(Api::ChannelApply::AfterBatch);
		}
		if (!full) {
			++pumpStats.eventsOnly;
			holdPrimaryNotes = false;
			inSceneTask = false;
			return;
		}
		{
			DCLF_ZONE("CS.DCLF.Scene.ApplyConstantsPosts");
			ApplyConstantsPosts();
		}
		{
			DCLF_ZONE("CS.DCLF.Scene.RefreshTechniqueRows");
			RefreshTechniqueRows();
		}
		RefreshMaterialRecords();
		{
			// The bindings are made once, after the joins, since nothing before the commit reads them.
			DCLF_ZONE("CS.DCLF.Scene.Walk");
			BuildFrame(Phase::Scene);
		}
		// Members the joins drop from PrimaryCull are held until the publication.
		holdLostMembers = true;
		{
			DCLF_SCENE_PART(Joins, "CS.DCLF.Scene.Joins");
			{
				DCLF_ZONE("CS.DCLF.Scene.Accumulate");
				BuildFrame(Phase::Accumulate);
			}
			{
				DCLF_ZONE("CS.DCLF.Scene.MakeNewPipelineConstants");
				MakeNewPipelineConstants();
			}
			UpdateMaterialBindings();
			UpdateSharedBindings();
			// One resolve a pass. The commit judges readiness by it and the publication carries it.
			ResolveLookups();
		}
		{
			// After the joins, since their leaves and writes are on the change log it reads.
			DCLF_SCENE_PART(Commit, "CS.DCLF.Scene.Commit");
			CommitSet();
		}
		PublishIfDue();
		holdLostMembers = false;
		{
			DCLF_ZONE("CS.DCLF.Scene.EndSceneFrame");
			Diagnostics::Notify<Diagnostics::ScenePoint::MirrorCheckPoint>();
			EndSceneFrame();
		}
		[[maybe_unused]] const double workMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - workStart).count();
		TracyPlot("CS.DCLF.SceneWorkMs", workMs);
		holdPrimaryNotes = false;
		inSceneTask = false;
	}

	void Coordinator::ResolveLookups()
	{
		DCLF_ZONE("CS.DCLF.Scene.ResolveLookups");
		VersionMaterialBindings();
		if (auto catalog = DrawPipelines::Get().TakeCatalog())
			lookupsCatalog = std::move(catalog);
		IndirectDraws::Get().ResolveLookups(&tables, lookupsCatalog.get(), lookups, lookupsResolve);
	}

	void Coordinator::PublishLookups()
	{
		DCLF_ZONE("CS.DCLF.Scene.PublishLookups");
		auto& publication = tablesPublication;
		// A newer catalog of the same generations holds every index an older one did.
		publishedCatalog = lookupsCatalog;
		const auto key = lookups.ChangeKey();
		if (publishedLookups && key == publishedLookupsKey) {
			++publication.lookupsShared;
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		publishedLookups = lookups.Snapshot();
		publishedLookupsKey = key;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		++publication.lookupsCopied;
		publication.lookupsMs += ms;
		publication.lookupsMaxMs = std::max(publication.lookupsMaxMs, ms);
		const std::uint64_t copies = lookups.pipelines.Stats().chunkCopies + lookups.materials.Stats().chunkCopies;
		publication.lookupsChunks += copies >= lookupsChunkCopies ? copies - lookupsChunkCopies : copies;
		lookupsChunkCopies = copies;
	}

	void Coordinator::ResetLookups()
	{
		// Generations move past the old instance and versions continue. Publications holding the old instance keep it until released.
		const auto lookupGeneration = lookups.generation;
		const auto shadowGeneration = lookups.shadowGeneration;
		const auto lookupVersion = lookups.versionCounter;
		lookups = Lookups{};
		lookups.generation = lookupGeneration + 1;
		lookups.shadowGeneration = shadowGeneration + 1;
		lookups.versionCounter = lookupVersion;
		lookupsResolve = {};
	}

	void Coordinator::RetireIntoChain()
	{
		auto& batch = retirement.Open();
		const auto before = batch.slots.size();
		batch.slots.insert(batch.slots.end(), tables.retiring.begin(), tables.retiring.end());
		tables.retiring.clear();
		const std::uint32_t epoch = tables.slotEpoch;
		tables.geometrySlots.TakeRetiring([&](std::uint32_t a_slot, std::uint32_t a_generation) { batch.slots.push_back({ Tables::kRetiredGeometrySlot, 0, a_slot, a_generation, epoch }); });
		tables.materialSlots.TakeRetiring([&](std::uint32_t a_slot, std::uint32_t a_generation) { batch.slots.push_back({ Tables::kRetiredMaterialSlot, 0, a_slot, a_generation, epoch }); });
		tables.pipelineSlots.TakeRetiring([&](std::uint32_t a_slot, std::uint32_t a_generation) { batch.slots.push_back({ Tables::kRetiredPipelineSlot, 0, a_slot, a_generation, epoch }); });
		retirementStats.retired += batch.slots.size() - before;
	}

	void Coordinator::RecycleRetired()
	{
		DCLF_ZONE("CS.DCLF.Scene.RecycleRetired");
		retirementStats.batches += retirement.Drain([&](RetiredBatch&& a_batch) {
			for (const auto& retired : a_batch.slots) {
				const std::uint32_t slot = retired.slot;
				if (retired.epoch != tables.slotEpoch) {
					++retirementStats.dropped;
					continue;
				}
				++retirementStats.recycled;
				switch (retired.kind) {
				case Tables::kRetiredObject:
					if (tables.objectsRetiring)
						--tables.objectsRetiring;
					if (slot < tables.objects.size() && !tables.objectGeometry[slot])
						tables.objectFree.push_back(slot);
					break;
				case Tables::kRetiredFamilyRow:
					tables.families.Recycle(retired.extra, slot);
					break;
				case Tables::kRetiredExtras:
					tables.extraFree.push_back(slot);
					break;
				case Tables::kRetiredBones:
					tables.ReturnBones(slot, retired.extra);
					break;
				case Tables::kRetiredStreamRow:
					tables.streamRowFree.push_back(slot);
					break;
				case Tables::kRetiredMaterialLookup:
					RetireMaterialBinding(slot);
					break;
				case Tables::kRetiredPipelineLookup:
					RetireMaskBinding(slot);
					break;
				case Tables::kRetiredGeometrySlot:
					tables.geometrySlots.Recycle(slot, retired.extra);
					break;
				case Tables::kRetiredMaterialSlot:
					tables.materialSlots.Recycle(slot, retired.extra);
					break;
				case Tables::kRetiredPipelineSlot:
					tables.pipelineSlots.Recycle(slot, retired.extra);
					break;
				case Tables::kRetiredComponentRange:
					ComponentRecycled(retired.owner, slot, retired.extra);
					break;
				}
			}
			// Imports go to the epochs. Engine references are released at Present on the render thread.
			for (auto& owner : a_batch.imports)
				retiredImports.push_back(std::move(owner));
			EngineReleases::NoteReleased(a_batch.lastPublication);
			referencesReleased += a_batch.references.size() + a_batch.owners.size();
			for (auto& reference : a_batch.references)
				EngineReleases::Push(std::move(reference));
			for (auto& owner : a_batch.owners)
				EngineReleases::PushOwner(std::move(owner));
		});
		// Past the last slot alive or retiring nothing names a pipeline slot, so the columns and every GPU table sized from them are cut there.
		if (const auto top = tables.pipelineSlots.TrimTop(); top < tables.pipelines.size()) {
			tables.PipelineColumns([top](auto& a_column, auto&&...) { a_column.erase(a_column.begin() + static_cast<std::ptrdiff_t>(top), a_column.end()); });
			tables.TrimPipelineUsed(static_cast<std::uint32_t>(top));
			tables.NotePipelinesWrite();
			tables.NoteConstantsWrite();
			++retirementStats.pipelinesTrimmed;
		}
	}

	namespace
	{
		// A replayed slot copies a column only when a cause it was noted with covers it. A publication-log slot takes every column.
		template <class Right, class Column>
		void ForLoggedColumns(SceneTables& a_left, Right& a_right, Column&& a_column)
		{
			a_column(a_left.objects, a_right.objects, kChangeAll);
			a_column(a_left.draws, a_right.draws, kChangeAll);
			a_column(a_left.ancestorValue, a_right.ancestorValue, kChangeAncestorValue);
			a_column(a_left.listedRoot, a_right.listedRoot, kChangeAll);
			a_column(a_left.hasFadeNode, a_right.hasFadeNode, kChangeAll);
			a_column(a_left.extraOffset, a_right.extraOffset, kChangeExtras);
			a_column(a_left.objectGeometry, a_right.objectGeometry, kChangeAll);
			a_column(a_left.objectIdentity, a_right.objectIdentity, kChangeAll);
			a_column(a_left.objectGroup, a_right.objectGroup, kChangeAll);
			a_column(a_left.shadowDiffuse, a_right.shadowDiffuse, kChangeShadow);
			a_column(a_left.shadowMaterial, a_right.shadowMaterial, kChangeShadow);
			a_column(a_left.fadeDistance, a_right.fadeDistance, kChangeAll);
			a_column(a_left.boneOffset, a_right.boneOffset, kChangeSkin);
			a_column(a_left.boneRows, a_right.boneRows, kChangeSkin);
			a_column(a_left.shadowTechnique, a_right.shadowTechnique, kChangeShadow);
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				a_column(a_left.occlusionTechnique[v], a_right.occlusionTechnique[v], kChangeShadow);
			a_column(a_left.streamRow, a_right.streamRow, kChangeAll);
			a_column(a_left.sceneFlags, a_right.sceneFlags, kChangeAll);
			a_column(a_left.skinPartitions, a_right.skinPartitions, kChangeSkin);
			a_column(a_left.skinLodPartitions, a_right.skinLodPartitions, kChangeSkin);
			a_column(a_left.shadowReject, a_right.shadowReject, kChangeShadow);
			// Residency is written ahead of the accumulate phase's snapshot, so any cause.
			a_column(a_left.residentSlot, a_right.residentSlot, kChangeAll);
		}

		template <class Right, class Column>
		void ForSidedColumns(SceneTables& a_left, Right& a_right, Column&& a_column)
		{
			a_column(a_left.setPhases, a_right.setPhases, std::uint8_t{ 0 });
			a_column(a_left.rangeOrdinal, a_right.rangeOrdinal, ~0u);
			a_column(a_left.layerBase, a_right.layerBase, kNoObjectSlot);
			a_column(a_left.layerOf, a_right.layerOf, kNoObjectSlot);
		}

		// Not published. No reader of a publication reads these, so a publication's stay empty.
		template <class Column>
		void ForUnpublished(SceneTables& a_left, SceneTables& a_right, Column&& a_column)
		{
			a_column(a_left.objectSeen, a_right.objectSeen);
			a_column(a_left.geometryTemplate, a_right.geometryTemplate);
			a_column(a_left.geometrySlots, a_right.geometrySlots);
			a_column(a_left.pipelineSlots, a_right.pipelineSlots);
			a_column(a_left.materialSlots, a_right.materialSlots);
			a_column(a_left.geometryLastUsed, a_right.geometryLastUsed);
			a_column(a_left.objectFree, a_right.objectFree);
			a_column(a_left.boneFree, a_right.boneFree);
			a_column(a_left.extraFree, a_right.extraFree);
			a_column(a_left.streamRowFree, a_right.streamRowFree);
			a_column(a_left.streamRowsReleased, a_right.streamRowsReleased);
			for (std::size_t c = 0; c < a_left.classMembers.size(); ++c) {
				a_column(a_left.classMembers[c].owners, a_right.classMembers[c].owners);
				a_column(a_left.classMembers[c].changes, a_right.classMembers[c].changes);
			}
			a_column(a_left.shadowTextureSeen, a_right.shadowTextureSeen);
			a_column(a_left.shadowTextureChanges, a_right.shadowTextureChanges);
			a_column(a_left.retiring, a_right.retiring);
		}

		template <class Column>
		void ForGeometryColumns(SceneTables& a_left, SceneTables& a_right, Column&& a_column)
		{
			a_column(a_left.geometries, a_right.geometries);
			a_column(a_left.geometryImports, a_right.geometryImports);
			a_column(a_left.geometrySlotKey, a_right.geometrySlotKey);
			a_column(a_left.geometryLayerKey, a_right.geometryLayerKey);
		}

		template <class Member>
		void ForPipelineFamily(SceneTables& a_left, SceneTables& a_right, Member&& a_member)
		{
			SceneTables::PipelineFamily(a_left, a_right, a_member);
		}

		template <class Member>
		void ForConstantsFamily(SceneTables& a_left, SceneTables& a_right, Member&& a_member)
		{
			a_member(a_left.pipelineConstants, a_right.pipelineConstants);
			a_member(a_left.techniqueConstants, a_right.techniqueConstants);
		}

		void SwapHeldOut(SceneTables& a_left, SceneTables& a_right)
		{
			ForLoggedColumns(a_left, a_right, [](auto& a_x, auto& a_y, std::uint32_t) { a_x.swap(a_y); });
			ForSidedColumns(a_left, a_right, [](auto& a_x, auto& a_y, auto) { a_x.swap(a_y); });
			ForUnpublished(a_left, a_right, [](auto& a_x, auto& a_y) { std::swap(a_x, a_y); });
			ForPipelineFamily(a_left, a_right, [](auto& a_x, auto& a_y) { a_x.swap(a_y); });
			ForConstantsFamily(a_left, a_right, [](auto& a_x, auto& a_y) { std::swap(a_x, a_y); });
			std::swap(a_left.families, a_right.families);
			ForGeometryColumns(a_left, a_right, [](auto& a_x, auto& a_y) { a_x.swap(a_y); });
			a_left.extraRows.swap(a_right.extraRows);
			a_left.materials.swap(a_right.materials);
			a_left.materialVersion.swap(a_right.materialVersion);
			a_left.materialFrameVersion.swap(a_right.materialFrameVersion);
			std::swap(a_left.changeLog, a_right.changeLog);
			std::swap(a_left.geometryLog, a_right.geometryLog);
			std::swap(a_left.materialLog, a_right.materialLog);
			std::swap(a_left.extrasBlockLog, a_right.extrasBlockLog);
			std::swap(a_left.publicationLog, a_right.publicationLog);
		}

		// The free pooled snapshot with the least to replay. Returns a_pool.size() when none is free.
		std::size_t NearestPooledTables(std::vector<std::shared_ptr<SceneTables>>& a_pool, const std::vector<std::uint8_t>& a_replayable,
			const std::shared_ptr<SceneTables>& a_published, SceneTables& a_tables, CoordinatorPump::PaceStats& a_pace)
		{
			std::size_t index = a_pool.size();
			std::size_t first = a_pool.size();
			std::uint64_t nearest = 0;
			for (std::size_t i = 0; i < a_pool.size(); ++i) {
				auto& pooled = a_pool[i];
				if (pooled.use_count() != 1 || pooled == a_published)
					continue;
				first = (std::min)(first, i);
				const bool reached = a_replayable[i] && a_tables.changeLog.Readable(pooled->changeLog.End()) && a_tables.materialLog.Readable(pooled->materialLog.End());
				const std::uint64_t reach = reached ? pooled->changeLog.End() + pooled->materialLog.End() + 1 : 0;
				if (index == a_pool.size() || reach > nearest) {
					index = i;
					nearest = reach;
				}
			}
			if (index != first)
				++a_pace.replayNearer;
			return index;
		}

		struct TablesReplayPlan
		{
			std::uint64_t position = 0, publicationPosition = 0, materialPosition = 0, geometryPosition = 0, extrasPosition = 0;
			bool replayExtras = false, replayGeometry = false, replay = false, replayMaterials = false;
			bool keepConstants = false, keepPipelines = false;
		};

		TablesReplayPlan PlanTablesReplay(const SceneTables& a_snapshot, const SceneTables& a_tables, std::uint8_t a_replayable)
		{
			TablesReplayPlan plan;
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			plan.position = snapshot.changeLog.End();
			plan.publicationPosition = snapshot.publicationLog.End();
			plan.materialPosition = snapshot.materialLog.End();
			plan.geometryPosition = snapshot.geometryLog.End();
			plan.extrasPosition = snapshot.extrasBlockLog.End();
			plan.replayExtras = a_replayable && tables.extrasBlockLog.Readable(plan.extrasPosition) &&
			                    snapshot.extrasBlockLog.base <= tables.extrasBlockLog.base && snapshot.extraRows.size() <= tables.extraRows.size();
			plan.replayGeometry = a_replayable && tables.geometryLog.Readable(plan.geometryPosition) &&
			                      snapshot.geometryLog.base <= tables.geometryLog.base && snapshot.geometries.size() <= tables.geometries.size();
			plan.replay = a_replayable && tables.changeLog.Readable(plan.position) && snapshot.changeLog.base <= tables.changeLog.base &&
			              tables.publicationLog.Readable(plan.publicationPosition) && snapshot.publicationLog.base <= tables.publicationLog.base &&
			              snapshot.objects.size() <= tables.objects.size();
			plan.replayMaterials = a_replayable && tables.materialLog.Readable(plan.materialPosition) && snapshot.materialLog.base <= tables.materialLog.base &&
			                       snapshot.materials.size() <= tables.materials.size();
			// Read before the copy writes the snapshot's stamps.
			plan.keepConstants = a_replayable && snapshot.constantsStamp == tables.constantsStamp;
			plan.keepPipelines = a_replayable && snapshot.pipelinesStamp == tables.pipelinesStamp;
			return plan;
		}

		// Swap the held-out members aside on both, then copy the rest whole.
		void CopyRestWhole(SceneTables& a_snapshot, SceneTables& a_tables)
		{
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			DCLF_ZONE("CS.DCLF.Scene.PublishTables.Rest");
			SceneTables snapshotHeld, tablesHeld;
			SwapHeldOut(snapshot, snapshotHeld);
			SwapHeldOut(tables, tablesHeld);
			struct Restore
			{
				std::function<void()> undo;
				~Restore() { undo(); }
			} restore{ [&] { SwapHeldOut(tables, tablesHeld); } };
			snapshot = tables;
			SwapHeldOut(snapshot, snapshotHeld);
		}

		// A snapshot the logs no longer reach takes the logged columns and logs whole.
		void CollectReplaySlots(SceneTables& a_snapshot, SceneTables& a_tables, const TablesReplayPlan& a_plan, std::size_t a_objectSlots,
			std::vector<std::uint32_t>& a_slots, std::vector<std::uint32_t>& a_causes, Coordinator::TablesPublication& a_publication,
			CoordinatorPump::PaceStats& a_pace, bool& a_versionsKnown)
		{
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			auto& slots = a_slots;
			auto& causes = a_causes;
			const std::size_t objectSlots = a_objectSlots;
			slots.clear();
			if (a_plan.replay) {
				DCLF_ZONE("CS.DCLF.Scene.PublishTables.Collect");
				snapshot.GrowObjects(objectSlots);
				ForSidedColumns(snapshot, tables, [](auto& a_to, auto& a_from, auto a_initial) { a_to.resize(a_from.size(), a_initial); });
				if (causes.size() < objectSlots)
					causes.resize(objectSlots, 0);
				auto name = [&](std::uint32_t a_slot, std::uint32_t a_causes) {
					if (a_slot >= objectSlots)
						return;
					if (!causes[a_slot])
						slots.push_back(a_slot);
					causes[a_slot] |= a_causes;
				};
				const auto changes = tables.changeLog.From(a_plan.position);
				for (const auto& change : changes)
					name(change.slot, change.causes ? change.causes : kChangeAll);
				const auto published = tables.publicationLog.From(a_plan.publicationPosition);
				for (const std::uint32_t slot : published)
					name(slot, kChangeAll);
				std::sort(slots.begin(), slots.end());
				a_pace.replayLogEntries += changes.size() + published.size();
			} else {
				++a_publication.wholeCopies;
				ForLoggedColumns(snapshot, tables, [](auto& a_to, auto& a_from, std::uint32_t) { a_to = a_from; });
				ForSidedColumns(snapshot, tables, [](auto& a_to, auto& a_from, auto) { a_to = a_from; });
				snapshot.changeLog = tables.changeLog;
				snapshot.publicationLog = tables.publicationLog;
				snapshot.geometryLog = tables.geometryLog;
				snapshot.materialLog = tables.materialLog;
				a_versionsKnown = false;
			}
		}

		// A snapshot the log no longer reaches is compared by version, which is session-unique per write.
		void ReplayMaterialRecords(SceneTables& a_snapshot, SceneTables& a_tables, const TablesReplayPlan& a_plan, bool a_versionsKnown,
			std::vector<std::uint32_t>& a_materialSlots, std::vector<std::uint8_t>& a_marks, Coordinator::TablesPublication& a_publication,
			CoordinatorPump::PaceStats& a_pace)
		{
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			auto& materialSlots = a_materialSlots;
			const std::size_t materialCount = tables.materials.size();
			snapshot.materials.resize(materialCount);
			materialSlots.clear();
			if (a_plan.replayMaterials) {
				// A grown slot's version is 0 until its first write, which the log names.
				snapshot.materialVersion.resize(materialCount, 0);
				snapshot.materialFrameVersion.resize(materialCount, 0);
				a_marks.assign(materialCount, 0);
				const auto noted = tables.materialLog.From(a_plan.materialPosition);
				for (const std::uint32_t slot : noted)
					if (slot < materialCount && !std::exchange(a_marks[slot], std::uint8_t{ 1 }))
						materialSlots.push_back(slot);
				std::sort(materialSlots.begin(), materialSlots.end());
				a_pace.replayMaterialEntries += noted.size();
				a_publication.materialsReplayed += materialSlots.size();
				a_pace.replayMaterials += materialSlots.size();
			} else {
				const auto& recordVersions = snapshot.materialVersion;
				const auto& recordFrameVersions = snapshot.materialFrameVersion;
				for (std::size_t slot = 0; slot < materialCount; ++slot) {
					if (a_versionsKnown && slot < recordVersions.size() && recordVersions[slot] == tables.materialVersion[slot] && slot < recordFrameVersions.size() &&
						recordFrameVersions[slot] == tables.materialFrameVersion[slot])
						continue;
					snapshot.materials[slot] = tables.materials[slot];
					++a_publication.materialsReplayed;
				}
				snapshot.materialVersion = tables.materialVersion;
				snapshot.materialFrameVersion = tables.materialFrameVersion;
			}
			a_publication.materialSlots += materialCount;
		}

		void FinishObjectReplay(SceneTables& a_snapshot, SceneTables& a_tables, const std::vector<std::uint32_t>& a_slots, std::vector<std::uint32_t>& a_causes,
			std::size_t a_objectSlots, Coordinator::TablesPublication& a_publication, CoordinatorPump::PaceStats& a_pace)
		{
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			auto& slots = a_slots;
			auto& causes = a_causes;
			// The layer's write notes only the layer, so copy the other end too.
			for (const std::uint32_t slot : slots)
				for (const std::uint32_t end : { tables.layerBase[slot], tables.layerOf[slot] })
					if (end < a_objectSlots && !causes[end]) {
						snapshot.layerBase[end] = tables.layerBase[end];
						snapshot.layerOf[end] = tables.layerOf[end];
					}
			for (const std::uint32_t slot : slots)
				causes[slot] = 0;
			a_publication.objectsReplayed += slots.size();
			a_pace.replaySlots += slots.size();
			snapshot.changeLog = tables.changeLog;
			snapshot.publicationLog = tables.publicationLog;
			snapshot.geometryLog = tables.geometryLog;
			snapshot.materialLog = tables.materialLog;
		}

		void ReplayExtras(SceneTables& a_snapshot, SceneTables& a_tables, const TablesReplayPlan& a_plan)
		{
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			const std::uint64_t extrasPosition = a_plan.extrasPosition;
			if (a_plan.replayExtras) {
				snapshot.extraRows.resize(tables.extraRows.size(), 0.0f);
				for (const std::uint32_t offset : tables.extrasBlockLog.From(extrasPosition))
					if ((std::size_t(offset) + kExtraRows) * 4 <= tables.extraRows.size())
						std::copy_n(tables.extraRows.begin() + std::ptrdiff_t(offset) * 4, std::size_t(kExtraRows) * 4, snapshot.extraRows.begin() + std::ptrdiff_t(offset) * 4);
				snapshot.extrasBlockLog = tables.extrasBlockLog;  // the chunks shared
			} else {
				snapshot.extraRows = tables.extraRows;
				snapshot.extrasBlockLog = tables.extrasBlockLog;
			}
		}

		// On the lane, since an import lease the snapshot drops must never be let go on a pool worker.
		void ReplayGeometryColumns(SceneTables& a_snapshot, SceneTables& a_tables, const TablesReplayPlan& a_plan, std::vector<std::uint8_t>& a_marks,
			Coordinator::TablesPublication& a_publication)
		{
			auto& snapshot = a_snapshot;
			auto& tables = a_tables;
			const std::size_t geometrySlots = tables.geometries.size();
			a_publication.geometrySlots += geometrySlots;
			if (a_plan.replayGeometry) {
				snapshot.GeometryColumns([geometrySlots](auto& a_column, auto&&... a_initial) { a_column.resize(geometrySlots, a_initial...); });
				auto& marks = a_marks;
				marks.assign(geometrySlots, 0);
				for (const std::uint32_t slot : tables.geometryLog.From(a_plan.geometryPosition)) {
					if (slot >= geometrySlots || std::exchange(marks[slot], std::uint8_t{ 1 }))
						continue;
					ForGeometryColumns(snapshot, tables, [slot](auto& a_to, auto& a_from) { a_to[slot] = a_from[slot]; });
					++a_publication.geometriesReplayed;
				}
			} else {
				++a_publication.geometryWholeCopies;
				ForGeometryColumns(snapshot, tables, [](auto& a_to, auto& a_from) { a_to = a_from; });
			}
		}
	}

	void Coordinator::ReplayTableColumns(Tables& a_snapshot, bool a_replay)
	{
		// Each job writes only its own column or record range of a snapshot only the pool holds, and reads tables nothing writes meanwhile.
		auto& snapshot = a_snapshot;
		auto& pace = paceStats;
		auto& slots = replaySlots;
		auto& causes = replaySlotCauses;
		auto& materialSlots = replayMaterialSlots;
		std::size_t columnJobs = 0;
		if (a_replay) {
			ForLoggedColumns(snapshot, tables, [&](auto&, auto&, std::uint32_t) { ++columnJobs; });
			ForSidedColumns(snapshot, tables, [&](auto&, auto&, auto) { ++columnJobs; });
		}
		const std::size_t materialJobs = (materialSlots.size() + kReplayMaterialGrain - 1) / kReplayMaterialGrain;
		const std::size_t jobs = columnJobs + materialJobs;
		struct JobCounts
		{
			std::uint64_t columns = 0, skipped = 0, runs = 0;
		};
		std::vector<JobCounts> counts(jobs);
		auto copyRuns = [](auto& a_to, const auto& a_from, std::span<const std::uint32_t> a_list, auto&& a_keep, JobCounts& a_counts) {
			const std::size_t limit = (std::min)(a_to.size(), a_from.size());
			std::size_t runBegin = 0, runEnd = 0;
			auto flush = [&] {
				if (runEnd > runBegin) {
					std::copy(a_from.begin() + std::ptrdiff_t(runBegin), a_from.begin() + std::ptrdiff_t(runEnd), a_to.begin() + std::ptrdiff_t(runBegin));
					++a_counts.runs;
				}
			};
			for (const std::uint32_t slot : a_list) {
				if (slot >= limit)
					break;
				if (!a_keep(slot)) {
					++a_counts.skipped;
					continue;
				}
				++a_counts.columns;
				if (slot != runEnd) {
					flush();
					runBegin = slot;
				}
				runEnd = std::size_t(slot) + 1;
			}
			flush();
		};
		auto job = [&](std::size_t a_job) {
			auto& jobCounts = counts[a_job];
			if (a_job < columnJobs) {
				std::size_t at = 0;
				auto column = [&](auto& a_to, auto& a_from, std::uint32_t a_gate) {
					if (at++ == a_job)
						copyRuns(a_to, a_from, slots, [&](std::uint32_t a_slot) { return (causes[a_slot] & a_gate) != 0; }, jobCounts);
				};
				ForLoggedColumns(snapshot, tables, column);
				ForSidedColumns(snapshot, tables, [&](auto& a_to, auto& a_from, auto) { column(a_to, a_from, kChangeAll); });
				return;
			}
			const std::size_t begin = (a_job - columnJobs) * kReplayMaterialGrain;
			const std::span<const std::uint32_t> range(materialSlots.data() + begin, (std::min)(kReplayMaterialGrain, materialSlots.size() - begin));
			auto all = [](std::uint32_t) { return true; };
			copyRuns(snapshot.materials, tables.materials, range, all, jobCounts);
			JobCounts versions;
			copyRuns(snapshot.materialVersion, tables.materialVersion, range, all, versions);
			copyRuns(snapshot.materialFrameVersion, tables.materialFrameVersion, range, all, versions);
		};
		auto run = [&](std::size_t a_begin, std::size_t a_end) {
			DCLF_ZONE("CS.DCLF.Scene.PublishTables.Shards");
			for (std::size_t j = a_begin; j < a_end; ++j)
				job(j);
		};
		const auto replayStart = std::chrono::steady_clock::now();
		const bool worth = slots.size() >= kReplayMinSlots || materialSlots.size() >= kReplayMinMaterials;
		if (jobs && worth && FanoutMode(jobs, 2) == 2) {
			::RenderGraph::Frame::SharedExecutor().ParallelFor("CS.DCLF.Scene.PublishTables.Replay", jobs, 1, run);
			++pace.replayPooled;
		} else if (jobs) {
			run(0, jobs);
		}
		for (const auto& c : counts) {
			pace.replayColumns += c.columns;
			pace.replaySkipped += c.skipped;
			pace.replayRuns += c.runs;
		}
		pace.replayMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - replayStart).count();
	}

	namespace
	{
		/**
		 * @brief After each publication the free pooled snapshot nearest the logs is caught up on the preparation pool, so the next
		 * publication replays only one publication's changes on the lane. Geometry columns stay the lane's, since a dropped lease must be let go there.
		 */
		struct TablesSpare
		{
			/** @brief A new one per dispatch. Whichever of the job and the lane claims it runs it. */
			struct State
			{
				std::atomic<bool> claimed{ false };
				std::atomic<bool> busy{ false };  // released once done: the snapshot's writes with it
			};
			SceneTables* target = nullptr;              // the pool's (which never lets go of its snapshots)
			std::shared_ptr<const SceneTables> source;  // the publication it is brought up to
			std::shared_ptr<State> state;
			std::vector<std::uint32_t> slots, causes;
			std::vector<std::uint8_t> marks;
			std::uint64_t dispatched = 0, runByLane = 0, waited = 0;

			TablesSpare() = default;
			TablesSpare(const TablesSpare&) = delete;
			TablesSpare& operator=(const TablesSpare&) = delete;
			/** @brief Waits for a running catch-up. One not started is left, since nothing reads it again. */
			~TablesSpare()
			{
				if (state && state->busy.load(std::memory_order_acquire) && state->claimed.exchange(true, std::memory_order_acq_rel))
					state->busy.wait(true, std::memory_order_acquire);
			}
		};

		TablesSpare& Spare()
		{
			static TablesSpare spare;
			return spare;
		}

		/** @brief Pure on a_spare and a_scratch. */
		void CatchUpTables(SceneTables& a_spare, const SceneTables& a_source, TablesSpare& a_scratch)
		{
			DCLF_ZONE("CS.DCLF.Scene.PublishTables.Spare");
			auto& s = a_spare;
			const auto& p = a_source;
			const TablesReplayPlan plan = PlanTablesReplay(s, p, 1);
			if (plan.replay) {
				const std::size_t objectSlots = p.objects.size();
				s.GrowObjects(objectSlots);
				ForSidedColumns(s, p, [](auto& a_to, const auto& a_from, auto a_initial) { a_to.resize(a_from.size(), a_initial); });
				auto& slots = a_scratch.slots;
				auto& causes = a_scratch.causes;
				slots.clear();
				if (causes.size() < objectSlots)
					causes.resize(objectSlots, 0);
				auto name = [&](std::uint32_t a_slot, std::uint32_t a_causes) {
					if (a_slot >= objectSlots)
						return;
					if (!causes[a_slot])
						slots.push_back(a_slot);
					causes[a_slot] |= a_causes;
				};
				for (const auto& change : p.changeLog.From(plan.position))
					name(change.slot, change.causes ? change.causes : kChangeAll);
				for (const std::uint32_t slot : p.publicationLog.From(plan.publicationPosition))
					name(slot, kChangeAll);
				ForLoggedColumns(s, p, [&](auto& a_to, const auto& a_from, std::uint32_t a_gate) {
					const std::size_t limit = (std::min)(a_to.size(), a_from.size());
					for (const std::uint32_t slot : slots)
						if (slot < limit && (causes[slot] & a_gate))
							a_to[slot] = a_from[slot];
				});
				ForSidedColumns(s, p, [&](auto& a_to, const auto& a_from, auto) {
					const std::size_t limit = (std::min)(a_to.size(), a_from.size());
					for (const std::uint32_t slot : slots)
						if (slot < limit)
							a_to[slot] = a_from[slot];
				});
				// The layer's write notes only the layer, so copy the other end too.
				for (const std::uint32_t slot : slots)
					for (const std::uint32_t end : { p.layerBase[slot], p.layerOf[slot] })
						if (end < objectSlots && !causes[end]) {
							s.layerBase[end] = p.layerBase[end];
							s.layerOf[end] = p.layerOf[end];
						}
				for (const std::uint32_t slot : slots)
					causes[slot] = 0;
				s.changeLog = p.changeLog;
				s.publicationLog = p.publicationLog;
			}
			if (plan.replayMaterials) {
				const std::size_t materialCount = p.materials.size();
				s.materials.resize(materialCount);
				s.materialVersion.resize(materialCount, 0);
				s.materialFrameVersion.resize(materialCount, 0);
				auto& marks = a_scratch.marks;
				marks.assign(materialCount, 0);
				for (const std::uint32_t slot : p.materialLog.From(plan.materialPosition)) {
					if (slot >= materialCount || std::exchange(marks[slot], std::uint8_t{ 1 }))
						continue;
					s.materials[slot] = p.materials[slot];
					if (slot < p.materialVersion.size())
						s.materialVersion[slot] = p.materialVersion[slot];
					if (slot < p.materialFrameVersion.size())
						s.materialFrameVersion[slot] = p.materialFrameVersion[slot];
				}
				s.materialLog = p.materialLog;
			}
			if (plan.replayExtras) {
				s.extraRows.resize(p.extraRows.size(), 0.0f);
				for (const std::uint32_t offset : p.extrasBlockLog.From(plan.extrasPosition))
					if ((std::size_t(offset) + kExtraRows) * 4 <= p.extraRows.size())
						std::copy_n(p.extraRows.begin() + std::ptrdiff_t(offset) * 4, std::size_t(kExtraRows) * 4, s.extraRows.begin() + std::ptrdiff_t(offset) * 4);
				s.extrasBlockLog = p.extrasBlockLog;
			}
		}

		/** @brief Runs the spare's catch-up here if its job has not started, else waits for it. */
		void FinishTablesSpare()
		{
			auto& spare = Spare();
			if (spare.state && spare.state->busy.load(std::memory_order_acquire)) {
				if (!spare.state->claimed.exchange(true, std::memory_order_acq_rel)) {
					// Not started yet, so the lane runs it and the job finds it claimed.
					CatchUpTables(*spare.target, *spare.source, spare);
					spare.state->busy.store(false, std::memory_order_relaxed);
					++spare.runByLane;
				} else {
					spare.state->busy.wait(true, std::memory_order_acquire);
					++spare.waited;
				}
			}
			spare.source.reset();
			spare.target = nullptr;
		}

		void DispatchTablesSpare(const std::vector<std::shared_ptr<SceneTables>>& a_pool, const std::vector<std::uint8_t>& a_replayable,
			const std::shared_ptr<SceneTables>& a_published)
		{
			std::size_t index = a_pool.size();
			std::uint64_t nearest = 0;
			for (std::size_t i = 0; i < a_pool.size(); ++i) {
				const auto& pooled = a_pool[i];
				if (pooled.use_count() != 1 || pooled == a_published || !a_replayable[i])
					continue;
				const TablesReplayPlan plan = PlanTablesReplay(*pooled, *a_published, 1);
				if (!plan.replay && !plan.replayMaterials && !plan.replayExtras)
					continue;
				const std::uint64_t reach = pooled->changeLog.End() + pooled->materialLog.End() + 1;
				if (index == a_pool.size() || reach > nearest) {
					index = i;
					nearest = reach;
				}
			}
			if (index == a_pool.size())
				return;
			auto& spare = Spare();
			spare.target = a_pool[index].get();
			spare.source = a_published;
			auto state = std::make_shared<TablesSpare::State>();
			state->busy.store(true, std::memory_order_relaxed);
			spare.state = state;
			++spare.dispatched;
			// The job reads the snapshots by address. The pool and the spare's source hold them until the lane finishes it.
			auto job = [target = spare.target, source = spare.source.get(), state = std::move(state)]() {
				if (state->claimed.exchange(true, std::memory_order_acq_rel))
					return;
				CatchUpTables(*target, *source, Spare());
				state->busy.store(false, std::memory_order_release);
				state->busy.notify_all();
			};
			if (!::RenderGraph::Frame::SharedExecutor().Dispatch(SceneScheduler::Scope(), ::RenderGraph::Frame::Executor::Preparation, org::async::TaskDispatch::Cpu,
					"DCLF tables spare", [job](const auto&) { job(); }))
				job();
		}
	}

	namespace
	{
		/** @brief A free snapshot holds its geometry slots' import leases until replayed, so idle extras would keep buffers the scene let go of. */
		void TrimTablesPool(std::vector<std::shared_ptr<SceneTables>>& a_pool, std::vector<std::uint8_t>& a_replayable, const std::shared_ptr<SceneTables>& a_published,
			std::uint64_t& a_trimmed)
		{
			constexpr std::size_t kFreeTables = 2;
			const auto* spareTarget = Spare().target;
			std::vector<std::size_t> idle;
			for (std::size_t i = 0; i < a_pool.size(); ++i)
				if (a_pool[i].use_count() == 1 && a_pool[i] != a_published && a_pool[i].get() != spareTarget)
					idle.push_back(i);
			if (idle.size() <= kFreeTables)
				return;
			const auto reach = [&](std::size_t a_i) { return a_replayable[a_i] ? a_pool[a_i]->changeLog.End() + a_pool[a_i]->materialLog.End() + 1 : 0; };
			std::sort(idle.begin(), idle.end(), [&](std::size_t a_left, std::size_t a_right) { return reach(a_left) > reach(a_right); });
			std::vector<std::uint8_t> drop(a_pool.size(), 0);
			for (std::size_t k = kFreeTables; k < idle.size(); ++k)
				drop[idle[k]] = 1;
			std::size_t kept = 0;
			for (std::size_t i = 0; i < a_pool.size(); ++i) {
				if (drop[i])
					continue;
				a_pool[kept] = std::move(a_pool[i]);
				a_replayable[kept] = a_replayable[i];
				++kept;
			}
			a_pool.resize(kept);
			a_replayable.resize(kept);
			a_trimmed += idle.size() - kFreeTables;
		}
	}

	void Coordinator::PublishTables()
	{
		DCLF_ZONE("CS.DCLF.Scene.PublishTables");
		const auto start = std::chrono::steady_clock::now();
		auto& publication = tablesPublication;
		auto& pace = paceStats;
		std::size_t index = tablesPool.size();
		{
			DCLF_ZONE("CS.DCLF.Scene.PublishTables.Pool");
			RetireIntoChain();
			FinishTablesSpare();
			index = NearestPooledTables(tablesPool, tablesPoolReplayable, publishedTables, tables, pace);
		}
		if (index < tablesPool.size()) {
			++publication.reused;
		} else {
			tablesPool.emplace_back(std::make_shared<Tables>());
			tablesPoolReplayable.push_back(0);
			++publication.made;
		}
		const std::shared_ptr<Tables> target = tablesPool[index];
		Tables& snapshot = *target;

		// The snapshot equals the tables at its change log's end. Replay what the logs name since then, keep families whose stamps stood,
		// and copy everything else whole.
		const TablesReplayPlan plan = PlanTablesReplay(snapshot, tables, tablesPoolReplayable[index]);
		// No material version is trusted after a whole copy of the objects.
		bool versionsKnown = tablesPoolReplayable[index] != 0;
		CopyRestWhole(snapshot, tables);
		const auto restEnd = std::chrono::steady_clock::now();
		const double restMs = std::chrono::duration<double, std::milli>(restEnd - start).count();
		publication.restMs += restMs;
		pace.restMs += restMs;

		// The copies run as jobs over disjoint columns and record ranges.
		const std::size_t objectSlots = tables.objects.size();
		publication.objectSlots += objectSlots;
		CollectReplaySlots(snapshot, tables, plan, objectSlots, replaySlots, replaySlotCauses, publication, pace, versionsKnown);
		ReplayMaterialRecords(snapshot, tables, plan, versionsKnown, replayMaterialSlots, replayMaterialMarks, publication, pace);
		ReplayTableColumns(snapshot, plan.replay);
		if (plan.replay)
			FinishObjectReplay(snapshot, tables, replaySlots, replaySlotCauses, objectSlots, publication, pace);
		DCLF_ZONE_NAMED(familiesZone, "CS.DCLF.Scene.PublishTables.Families", true);
		{
			DCLF_ZONE("CS.DCLF.Scene.PublishTables.Extras");
			ReplayExtras(snapshot, tables, plan);
		}
		auto assign = [](auto& a_to, auto& a_from) { a_to = a_from; };
		{
			DCLF_ZONE("CS.DCLF.Scene.PublishTables.TableFamilies");
			(void)snapshot.families.Publish(tables.families, tablesPoolReplayable[index] != 0, publication.familiesKept);
		}
		if (!plan.keepConstants) {
			DCLF_ZONE("CS.DCLF.Scene.PublishTables.Constants");
			ForConstantsFamily(snapshot, tables, assign);
		}
		if (!plan.keepPipelines) {
			DCLF_ZONE("CS.DCLF.Scene.PublishTables.PipelineFamily");
			ForPipelineFamily(snapshot, tables, assign);
		}
		{
			DCLF_ZONE("CS.DCLF.Scene.PublishTables.Geometry");
			ReplayGeometryColumns(snapshot, tables, plan, replayGeometryMarks, publication);
		}
		tablesPoolReplayable[index] = 1;

		// The snapshots are its only readers. One behind the bound is copied whole.
		tables.publicationLog.Trim(1u << 16);
		{
			DCLF_ZONE("CS.DCLF.Scene.PublishTables.Retirement");
			retirement.Open().lastPublication = publicationSequence;
			publishedRetirement = retirement.Publish();
			snapshot.publishedSequence = publicationSequence;
		}
		publishedTables = target;
		++publication.published;
		{
			DCLF_ZONE("CS.DCLF.Scene.PublishTables.SpareDispatch");
			DispatchTablesSpare(tablesPool, tablesPoolReplayable, publishedTables);
			TrimTablesPool(tablesPool, tablesPoolReplayable, publishedTables, publication.trimmed);
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		publication.ms += ms;
		publication.maxMs = std::max(publication.maxMs, ms);
	}
}

namespace DCLF
{
	PublishedScene LatestPublishedScene()
	{
		std::scoped_lock lock(Scene::LatestSceneMutex());
		return Scene::LatestScene();
	}
}
