// The coordinator's placement work on the scene lane: move events into move keys, the light path's gate, the placement set,
// and the plan published when that set changes.

#include "Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

namespace DCLF::Scene
{
	void Coordinator::DrainMoveEvents(bool a_full)
	{
		// A reference's animation pushes several in a row.
		const void* last = nullptr;
		// Keys turn over when the pass's frame moves on, so a burst's passes add to the same frame.
		if (std::exchange(movedKeysFrame, sceneFrame) != sceneFrame) {
			std::swap(movedKeys[0], movedKeys[1]);
			movedKeys[0].clear();
			movedKeysQueued = 0;
		}
		stats.moveEvents += moveEvents.Drain([&](const void* a_key) {
			if (a_key != std::exchange(last, a_key)) {
				movedFrame[a_key] = sceneFrame;
				movedKeys[0].push_back(a_key);
			}
		});
		for (const auto& node : nodeChanged) {
			// The mirror's chain, since the scene work reads no engine memory.
			const void* key = nullptr;
			for (const void* at = node.get(); at && !key; at = mirror.Parent(at))
				if (const auto* record = mirror.Node(at))
					key = record->userData;
			if (!key)
				key = FindCategoryNode(node.get(), nullptr);
			if (key) {
				movedFrame[key] = sceneFrame;
				movedKeys[0].push_back(key);
			}
		}
		if (a_full)
			moveUngatedThrough = sceneFrame + 1;
		// The passes may skip the multiples of 256.
		if (sceneFrame - movedPruneFrame >= 256) {
			movedPruneFrame = sceneFrame;
			std::erase_if(movedFrame, [&](const auto& a_entry) { return sceneFrame - a_entry.second > 1; });
		}
		moveGating = MoveEventsLive() && sceneFrame > moveUngatedThrough;
	}

	const void* Coordinator::MoveKeyOf(const RE::BSGeometry& a_geometry, const RE::NiNode* a_categoryNode) const
	{
		++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::MoveKey)];
		auto keyOf = [&](auto&& a_parent, auto&& a_reference) -> const void* {
			const void* key = nullptr;
			for (const void* object = &a_geometry; object && object != a_categoryNode; object = a_parent(object)) {
				const void* reference = a_reference(object);
				if (!reference)
					continue;
				if (key && key != reference)
					return nullptr;
				key = reference;
			}
			return key;
		};
		const void* key = keyOf([&](const void* a_key) { return mirror.Parent(a_key); }, [&](const void* a_key) -> const void* {
			const auto* record = mirror.Node(a_key);
			return record ? record->userData : nullptr;
		});
		Diagnostics::Notify(Diagnostics::MoveKeyRead{ *this, a_geometry, a_categoryNode, key });
		return key;
	}

	std::pair<const void*, const void*> Coordinator::MoveKeysOf(const RE::BSGeometry& a_geometry, const RE::NiNode* a_categoryNode) const
	{
		if (const void* key = MoveKeyOf(a_geometry, a_categoryNode))
			return { key, nullptr };
		// An outer reference's update moves the inner's 3D, and the inner may move on its own. A cell's update keys its dynamic subtree by the node.
		const void* inner = nullptr;
		const void* outer = nullptr;
		for (const void* object = &a_geometry; object && object != a_categoryNode; object = mirror.Parent(object))
			if (const auto* record = mirror.Node(object); record && record->userData) {
				inner = inner ? inner : record->userData;
				outer = record->userData;
			}
		if (!inner)
			return { a_categoryNode, nullptr };
		return { inner, outer != inner ? outer : nullptr };
	}

	bool Coordinator::MoverDue(const Tracked& a_tracked, const RE::BSGeometry* a_geometry, std::uint32_t& a_why) const
	{
		// Without a light path there is no kept record, so it is written in full every frame.
		if (!a_tracked.lightTraits) {
			a_why = 4;
			return true;
		}
		if (a_tracked.hiddenEventOwed) {
			a_why = 2;
			return true;
		}
		// First-person chains are recaptured every frame.
		if ((a_tracked.lightTraits & kTraitOwnerClass) && ViewRootEntry(a_tracked.categoryNode)) {
			a_why = 3;
			return true;
		}
		if (!a_tracked.moveKey) {
			a_why = 5;
			return true;
		}
		a_why = 1;
		return MovedRecently(a_geometry) || MovedRecently(a_tracked.moveKey) || (a_tracked.moveKeyOuter && MovedRecently(a_tracked.moveKeyOuter));
	}


	Coordinator::PlacementPlan& Coordinator::WalkPlan()
	{
		if (!placementPlan) {
			placementPlan = std::make_shared<PlacementPlan>();
			placementPlan->walk = walkSerial;
		}
		return *placementPlan;
	}

	Coordinator::PlannedKey Coordinator::PlacedKeyOf(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		ResolveSunEntry(a_tracked, *a_geometry);
		const std::uint32_t slot = a_tracked.slot;
		PlannedKey key{ a_geometry, slot, slot < tables.layerOf.size() ? tables.layerOf[slot] : kNoObjectSlot, 0, 0, a_tracked.entryRoot,
			ViewRootEntry(a_tracked.categoryNode), Api::FrameValueItemClass::None };
		if (slot < tables.boneRows.size() && (tables.objects[slot].flags & kObjectPaletteBlock) && tables.boneRows[slot]) {
			key.boneOffset = tables.boneOffset[slot];
			key.boneRows = tables.boneRows[slot];
			// A palette rows member's block holds its class's rows, such as a particle system's quads, not a skin's palette.
			if (const auto* record = mirror.Geometry(a_geometry)) {
				key.itemClass = Extensions::PlannedItemClass(true, record->type, record->skin != nullptr);
				if (key.itemClass == Api::FrameValueItemClass::None)
					key.skinPartition = record->skinPartition;
			}
		}
		return key;
	}

	Coordinator::PlacementPlan::Item Coordinator::PlanItemOf(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		const PlannedKey key = PlacedKeyOf(a_geometry, a_tracked);
		PlacementPlan::Item item;
		item.geometry = a_tracked.geometry;
		item.listedRoot = OwnedRoot(a_tracked.entryRoot);
		item.slot = key.slot;
		item.layerSlot = key.layerSlot;
		item.firstPerson = key.firstPerson;
		item.boneOffset = key.boneOffset;
		item.boneRows = key.boneRows;
		item.itemClass = key.itemClass;
		item.skinPartition = key.skinPartition;
		return item;
	}

	bool Coordinator::IsPlacedMover(const Tracked& a_tracked) const
	{
		// An actor's skeleton moves with no controller or body on its chain, through Havok's behaviour graph.
		return a_tracked.perFrame &&
		       ((a_tracked.lightTraits & (kTraitMoves | kTraitRootMoves | kTraitSkin)) || ((a_tracked.lightTraits & kTraitOwnerClass) && a_tracked.owner.ownMotion)) &&
		       a_tracked.geometry && a_tracked.slot != kNoObjectSlot && a_tracked.objectStamp == objectStamp && a_tracked.slot < tables.objects.size();
	}

	bool Coordinator::NoteMover(RE::BSGeometry* a_geometry, const Tracked& a_tracked, bool a_written)
	{
		const bool placed = IsPlacedMover(a_tracked);
		const bool changed = placed ? moverSet.insert(a_geometry).second : moverSet.erase(a_geometry) != 0;
		if (changed || (placed && a_written))
			placementMoversTouched.push_back(a_geometry);
		return changed;
	}

	void Coordinator::QueueRoots()
	{
		// Cell update passes recompute a still root's bound once its reference had a move event, as FireSpitCooking drifts by a tenth of
		// a unit with nothing under it moving.
		auto& keys = movedKeys[0];
		for (; movedKeysQueued < keys.size(); ++movedKeysQueued) {
			const auto it = referenceRoot.find(keys[movedKeysQueued]);
			if (it == referenceRoot.end() || movingRoots.contains(it->second) || !rootDependents.contains(it->second))
				continue;
			if (stillRoots.insert(it->second).second) {
				++placementStats.stillRoots;
				placementRootsTouched.push_back(it->second);
			}
		}
	}

	void Coordinator::SyncPlacementSet()
	{
		// A planned slot's change may change the key of an item naming it. A log this reader cannot continue makes the set anew.
		auto& log = tables.changeLog;
		if (!placementCursor.Continues(log, tablesGeneration)) {
			placementCursor.Restart(tablesGeneration);
			placementDirty = true;
		} else if (!placementDirty) {
			for (const auto& change : placementCursor.Unread(log)) {
				const auto it = plannedSlots.find(change.slot);
				if (it == plannedSlots.end())
					continue;
				++placementDeltaStats.logSlots;
				for (const auto& owner : it->second) {
					if (owner.root)
						placementDependentsTouched.emplace_back(owner.root, owner.geometry);
					else
						placementMoversTouched.push_back(static_cast<RE::BSGeometry*>(const_cast<void*>(owner.geometry)));
				}
			}
		}
		placementCursor.Advance(log);
		if (objectStamp != plannedObjectStamp || viewRoots != plannedViewRoots)
			placementDirty = true;
	}

	std::optional<Coordinator::PlannedKey> Coordinator::PlannedMoverNow(RE::BSGeometry* a_geometry, bool a_prune)
	{
		if (!moverSet.contains(a_geometry))
			return std::nullopt;
		const auto it = tracked.find(a_geometry);
		if (it == tracked.end() || !IsPlacedMover(it->second)) {
			if (a_prune)
				moverSet.erase(a_geometry);
			return std::nullopt;
		}
		return PlacedKeyOf(a_geometry, it->second);
	}

	bool Coordinator::PlannedRootNow(const RE::NiAVObject* a_root, bool a_prune)
	{
		if (!movingRoots.contains(a_root) && !stillRoots.contains(a_root))
			return false;
		if (rootDependents.contains(a_root) && rootOwners.contains(a_root))
			return true;
		if (a_prune)
			stillRoots.erase(a_root);
		return false;
	}

	std::optional<Coordinator::PlannedKey> Coordinator::PlannedDependentNow(const RE::NiAVObject* a_root, const void* a_geometry) const
	{
		const auto it = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_geometry)));
		if (it == tracked.end() || it->second.listedRoot != a_root || it->second.slot == kNoObjectSlot || it->second.slot >= tables.objects.size())
			return std::nullopt;
		const std::uint32_t layer = it->second.layerSlot != kNoObjectSlot && tables.IsLayer(it->second.layerSlot) ? it->second.layerSlot : kNoObjectSlot;
		return PlannedKey{ a_geometry, it->second.slot, layer };
	}

	void Coordinator::AddPlannedSlots(const PlannedKey& a_key, const PlannedOwner& a_owner)
	{
		for (const std::uint32_t slot : { a_key.slot, a_key.layerSlot }) {
			if (slot == kNoObjectSlot)
				continue;
			auto& owners = plannedSlots[slot];
			if (std::find(owners.begin(), owners.end(), a_owner) == owners.end())
				owners.push_back(a_owner);
		}
	}

	void Coordinator::RemovePlannedSlots(const PlannedKey& a_key, const PlannedOwner& a_owner)
	{
		for (const std::uint32_t slot : { a_key.slot, a_key.layerSlot }) {
			const auto it = slot != kNoObjectSlot ? plannedSlots.find(slot) : plannedSlots.end();
			if (it == plannedSlots.end())
				continue;
			auto& owners = it->second;
			if (const auto at = std::find(owners.begin(), owners.end(), a_owner); at != owners.end()) {
				*at = owners.back();
				owners.pop_back();
			}
			if (owners.empty())
				plannedSlots.erase(it);
		}
	}

	void Coordinator::DiffPlacementSet(PlacementPlan& a_plan, bool a_reset)
	{
		auto& d = placementDeltaStats;
		if (a_reset) {
			// The consumer clears its kept set.
			a_plan.reset = true;
			++d.resets;
			plannedMovers.clear();
			plannedRoots.clear();
			plannedSlots.clear();
			placementMoversTouched.assign(moverSet.begin(), moverSet.end());
			placementRootsTouched.clear();
			for (const auto& [root, moving] : movingRoots)
				placementRootsTouched.push_back(root);
			for (const auto* root : stillRoots)
				if (!movingRoots.contains(root))
					placementRootsTouched.push_back(root);
			placementDependentsTouched.clear();
		}
		d.moversTouched += placementMoversTouched.size();
		d.rootsTouched += placementRootsTouched.size();
		d.dependentsTouched += placementDependentsTouched.size();
		for (auto* geometry : placementMoversTouched) {
			const auto now = PlannedMoverNow(geometry, true);
			const auto planned = plannedMovers.find(geometry);
			const PlannedOwner owner{ geometry, nullptr };
			if (!now) {
				if (planned != plannedMovers.end()) {
					a_plan.moversLeft.push_back(geometry);
					RemovePlannedSlots(planned->second, owner);
					plannedMovers.erase(planned);
					++d.moversLeft;
				}
				continue;
			}
			if (planned != plannedMovers.end()) {
				if (planned->second == *now)
					continue;
				RemovePlannedSlots(planned->second, owner);
				++d.moversChanged;
			} else {
				++d.moversJoined;
			}
			a_plan.movers.push_back(PlanItemOf(geometry, tracked.find(geometry)->second));
			AddPlannedSlots(*now, owner);
			plannedMovers.insert_or_assign(geometry, *now);
		}
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::size_t> rootAt;
		auto rootEntry = [&](const RE::NiAVObject* a_root) -> PlacementPlan::Root& {
			const auto [it, inserted] = rootAt.try_emplace(a_root, a_plan.roots.size());
			if (inserted)
				a_plan.roots.emplace_back().root = OwnedRoot(a_root);
			return a_plan.roots[it->second];
		};
		auto dependentItem = [&](const PlannedKey& a_key) {
			PlacementPlan::Item item;
			item.geometry = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key.object)))->second.geometry;
			item.slot = a_key.slot;
			item.layerSlot = a_key.layerSlot;
			return item;
		};
		for (const auto* root : placementRootsTouched) {
			const bool listed = PlannedRootNow(root, true);
			const auto planned = plannedRoots.find(root);
			if (!listed) {
				if (planned != plannedRoots.end()) {
					for (const auto& [geometry, key] : planned->second)
						RemovePlannedSlots(key, { geometry, root });
					plannedRoots.erase(planned);
					a_plan.rootsLeft.push_back(root);
					++d.rootsLeft;
				}
				continue;
			}
			if (planned != plannedRoots.end())
				continue;
			++d.rootsListed;
			auto& entry = rootEntry(root);
			auto& dependents = plannedRoots[root];
			for (auto* geometry : rootDependents.find(root)->second)
				if (const auto key = PlannedDependentNow(root, geometry)) {
					entry.dependents.push_back(dependentItem(*key));
					dependents.insert_or_assign(geometry, *key);
					AddPlannedSlots(*key, { geometry, root });
				}
			d.dependentsJoined += entry.dependents.size();
		}
		for (const auto& [root, geometry] : placementDependentsTouched) {
			const auto planned = plannedRoots.find(root);
			if (planned == plannedRoots.end())
				continue;
			auto& dependents = planned->second;
			const auto now = PlannedDependentNow(root, geometry);
			const auto was = dependents.find(geometry);
			const PlannedOwner owner{ geometry, root };
			if (!now) {
				if (was != dependents.end()) {
					RemovePlannedSlots(was->second, owner);
					dependents.erase(was);
					a_plan.dependentsLeft.emplace_back(root, geometry);
					++d.dependentsLeft;
				}
				continue;
			}
			if (was != dependents.end()) {
				if (was->second == *now)
					continue;
				RemovePlannedSlots(was->second, owner);
				++d.dependentsChanged;
			} else {
				++d.dependentsJoined;
			}
			rootEntry(root).dependents.push_back(dependentItem(*now));
			dependents.insert_or_assign(geometry, *now);
			AddPlannedSlots(*now, owner);
		}
		placementMoversTouched.clear();
		placementRootsTouched.clear();
		placementDependentsTouched.clear();
	}

	PlacementPlan::Check Coordinator::RebuildPlacementSet()
	{
		PlacementPlan::Check check;
		auto byObject = [](const PlannedKey& a_left, const PlannedKey& a_right) { return a_left.object < a_right.object; };
		for (auto* geometry : moverSet) {
			const auto it = tracked.find(geometry);
			if (it != tracked.end() && IsPlacedMover(it->second))
				check.movers.push_back(PlacedKeyOf(geometry, it->second));
		}
		std::sort(check.movers.begin(), check.movers.end(), byObject);
		std::vector<const RE::NiAVObject*> listed;
		listed.reserve(movingRoots.size() + stillRoots.size());
		for (const auto& [root, moving] : movingRoots)
			listed.push_back(root);
		for (const auto* root : stillRoots)
			if (!movingRoots.contains(root))
				listed.push_back(root);
		std::sort(listed.begin(), listed.end());
		for (const auto* root : listed) {
			const auto dependents = rootDependents.find(root);
			if (dependents == rootDependents.end() || !rootOwners.contains(root))
				continue;
			auto& [node, keys] = check.roots.emplace_back(root, std::vector<PlannedKey>{});
			for (auto* geometry : dependents->second) {
				const auto it = tracked.find(geometry);
				if (it == tracked.end() || it->second.slot == kNoObjectSlot || it->second.slot >= tables.objects.size())
					continue;
				const std::uint32_t layer = it->second.layerSlot != kNoObjectSlot && tables.IsLayer(it->second.layerSlot) ? it->second.layerSlot : kNoObjectSlot;
				keys.push_back({ geometry, it->second.slot, layer });
			}
			std::sort(keys.begin(), keys.end(), byObject);
		}
		return check;
	}

	void Coordinator::PublishPlacementPlan()
	{
		DCLF_SCENE_PART(Placements, "CS.DCLF.Scene.Placements");
		// Only what changed since the last plan is compared, so motion alone needs no plan. The coordinator keeps keys and slots only,
		// and copies references into a plan, so the pump never holds one across passes.
		QueueRoots();
		SyncPlacementSet();
		auto& g = moverGateStats;
		auto& d = placementDeltaStats;
		const bool written = placementPlan && !placementPlan->written.empty();
		const auto objectSlots = static_cast<std::uint32_t>(tables.objects.size());
		const std::uint32_t boneCapacity = tables.BoneCapacity();
		const bool sized = objectSlots != plannedObjectSlots || boneCapacity != plannedBoneCapacity;
		const bool reset = placementDirty;
		const bool touched = reset || !placementMoversTouched.empty() || !placementRootsTouched.empty() || !placementDependentsTouched.empty();
		auto parity = [&]() -> std::shared_ptr<const PlacementPlan::Check> {
			if (!hiddenWitness)
				return nullptr;
			auto rebuilt = std::make_shared<PlacementPlan::Check>(RebuildPlacementSet());
			auto byObject = [](const PlannedKey& a_left, const PlannedKey& a_right) { return a_left.object < a_right.object; };
			PlacementPlan::Check planned;
			planned.movers.reserve(plannedMovers.size());
			for (const auto& [geometry, key] : plannedMovers)
				planned.movers.push_back(key);
			std::sort(planned.movers.begin(), planned.movers.end(), byObject);
			for (const auto& [root, dependents] : plannedRoots) {
				auto& [node, keys] = planned.roots.emplace_back(root, std::vector<PlannedKey>{});
				for (const auto& [geometry, key] : dependents)
					keys.push_back(key);
				std::sort(keys.begin(), keys.end(), byObject);
			}
			std::sort(planned.roots.begin(), planned.roots.end(), [](const auto& a_left, const auto& a_right) { return a_left.first < a_right.first; });
			++d.parityChecks;
			if (planned.movers != rebuilt->movers || planned.roots != rebuilt->roots) {
				++d.parityDiffer;
				if (d.parityFirst.empty()) {
					std::size_t m = 0;
					while (m < planned.movers.size() && m < rebuilt->movers.size() && planned.movers[m] == rebuilt->movers[m])
						++m;
					std::size_t r = 0;
					while (r < planned.roots.size() && r < rebuilt->roots.size() && planned.roots[r] == rebuilt->roots[r])
						++r;
					d.parityFirst = fmt::format("pass {}: movers {} planned / {} rebuilt (first difference at {}: {} vs {}), roots {} planned / {} rebuilt (first difference at {}: {} vs {})",
						passSerial, planned.movers.size(), rebuilt->movers.size(), m, m < planned.movers.size() ? planned.movers[m].object : nullptr,
						m < rebuilt->movers.size() ? rebuilt->movers[m].object : nullptr, planned.roots.size(), rebuilt->roots.size(), r,
						r < planned.roots.size() ? planned.roots[r].first : nullptr, r < rebuilt->roots.size() ? rebuilt->roots[r].first : nullptr);
				}
			}
			return rebuilt;
		};
		auto none = [&](bool a_compared) {
			placementPlan.reset();
			g.candidatesSame += a_compared ? 1u : 0u;
			g.perFrameWithoutPlan += perFrameWalked ? 1u : 0u;
			(void)parity();
			ReportPlacementDeltas();
		};
		if (!written && !sized && !touched) {
			none(false);
			return;
		}
		auto& plan = WalkPlan();
		const std::size_t joinedBefore = plan.movers.size() + plan.roots.size();
		if (touched)
			DiffPlacementSet(plan, reset);
		placementDirty = false;
		plannedObjectStamp = objectStamp;
		plannedViewRoots = viewRoots;
		const bool changed = plan.reset || plan.movers.size() + plan.roots.size() != joinedBefore || !plan.moversLeft.empty() || !plan.rootsLeft.empty() ||
		                     !plan.dependentsLeft.empty();
		if (!changed && !written && !sized) {
			none(true);
			return;
		}
		plannedObjectSlots = objectSlots;
		plannedBoneCapacity = boneCapacity;
		++g.plans;
		g.plansWritten += written ? 1u : 0u;
		g.plansChanged += changed ? 1u : 0u;
		++d.plans;
		d.sized += sized ? 1u : 0u;
		placementStats.roots += plan.roots.size();
		plan.slots = objectSlots;
		plan.boneCapacity = boneCapacity;
		plan.check = parity();
		// A plan no publication took yet is kept for the next one. Each plan is a delta, so none may be replaced.
		if (placementPlanReady)
			placementPlansHeld.push_back(std::move(placementPlanReady));
		placementPlanReady = std::move(placementPlan);
		ReportPlacementDeltas();
	}

	void Coordinator::ReportPlacementDeltas()
	{
		// The coordinator's clock may skip frame numbers.
		const std::uint32_t window = sceneFrame / Reports::Interval();
		if (window == placementDeltaWindow)
			return;
		placementDeltaWindow = window;
		auto& d = placementDeltaStats;
		const bool finding = d.parityDiffer || d.rootWitnessDiffer || d.scanParityDiffer;
		if ((Diagnostics::Observe().stats || finding) && (d.plans || d.dirtyRoots || d.scans || d.parityChecks)) {
			const double plans = d.plans ? static_cast<double>(d.plans) : 1.0;
			logger::info("[DCLF] placement deltas: {} plans ({} resets, {} resized); per plan movers +{:.1f} ~{:.1f} -{:.1f}, roots +{:.1f} -{:.1f}, dependents +{:.1f} ~{:.1f} "
						 "-{:.1f}; touched {} movers, {} roots, {} dependents, {} planned slots in the change log; dirty roots {} ({} whole), dependents scheduled {} of {} "
						 "listed; geometry scans {} ({} whole), {} stale slots, {} object slots checked of {}, {} scheduled; parity: set {} checks, {} differ{}{}; "
						 "root witnesses {} checks ({} skipped), {} differ{}{}; scans {} checks, {} differ{}",
				d.plans, d.resets, d.sized, d.moversJoined / plans, d.moversChanged / plans, d.moversLeft / plans, d.rootsListed / plans, d.rootsLeft / plans,
				d.dependentsJoined / plans, d.dependentsChanged / plans, d.dependentsLeft / plans, d.moversTouched, d.rootsTouched, d.dependentsTouched, d.logSlots,
				d.dirtyRoots, d.dirtyRootsWhole, d.rootDependentsScheduled, d.rootDependentsListed, d.scans, d.scansWhole, d.staleSlots, d.scanChecked, d.scanObjects,
				d.scanScheduled, d.parityChecks, d.parityDiffer, !d.parityChecks ? "" : d.parityDiffer ? " <- PLACEMENT DELTAS DIFFER; first: " : " <- OK", d.parityFirst,
				d.rootWitnessChecks, d.rootWitnessSkipped, d.rootWitnessDiffer, !d.rootWitnessChecks ? "" : d.rootWitnessDiffer ? " <- ROOT SCHEDULE MISSED; first: " : " <- OK", d.rootWitnessFirst,
				d.scanParityChecks, d.scanParityDiffer, !d.scanParityChecks ? "" : d.scanParityDiffer ? " <- GEOMETRY SCAN MISSED" : " <- OK");
		}
		d = {};
	}
}
