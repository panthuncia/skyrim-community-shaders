// The coordinator's classification on the scene lane: category nodes refreshed from the render thread's capture, subtrees tracked
// from the mirror by prepare then merge in event order, and the per-entry verdicts.

#include "Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/AncestorValues.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include <cassert>

namespace DCLF::Scene
{
	RE::NiNode* Coordinator::FindCategoryNode(RE::NiAVObject* a_object, Ineligible* a_parentReason, bool* a_unmirrored) const
	{
		constexpr auto kRead = static_cast<std::size_t>(MirrorRead::CategoryNode);
		++MirrorReads().reads[kRead];
		Ineligible reason = Ineligible::None;
		bool unmirrored = false;
		RE::NiNode* found = MirrorCategoryNode(a_object, reason, unmirrored);
		if (unmirrored)
			++MirrorReads().unmirrored[kRead];
		if (a_unmirrored)
			*a_unmirrored = unmirrored;
		if (found && a_parentReason)
			*a_parentReason = reason;
		return found;
	}

	RE::NiNode* Coordinator::MirrorCategoryNode(const void* a_object, Ineligible& a_reason, bool& a_unmirrored) const
	{
		Ineligible reason = Ineligible::None;
		RE::NiNode* found = nullptr;
		bool unmirrored = false;
		const void* top = a_object;
		// A CategoryRoot view's root is its own category node, since members of a node above it take no part in the view's phase.
		if (ViewRoot(a_object)) {
			a_reason = Ineligible::None;
			a_unmirrored = !mirror.Node(a_object);
			return static_cast<RE::NiNode*>(const_cast<void*>(a_object));
		}
		const SceneCapture::NodeRecord* record = a_object ? mirror.Node(a_object) : nullptr;
		unmirrored = a_object && !record;
		const void* key = record ? record->parent : nullptr;
		for (std::uint32_t depth = 0; key && depth < kMaxParentDepth; ++depth) {
			auto* node = static_cast<RE::NiNode*>(const_cast<void*>(key));
			if (categoryNodes.contains(node)) {
				found = node;
				break;
			}
			const auto* parent = mirror.Node(key);
			if (!parent) {
				unmirrored = true;
				break;
			}
			reason = Eligibility::CombineParentReasons(reason, Eligibility::ParentReasonOf(*parent));
			top = key;
			key = parent->parent;
		}
		if (!found && !key && !unmirrored && top && !alwaysRenderRoots.empty())
			if (const auto it = alwaysRenderRoots.find(static_cast<const RE::NiAVObject*>(top)); it != alwaysRenderRoots.end())
				found = it->second.category;
		a_reason = reason;
		a_unmirrored = unmirrored;
		return found;
	}

	RE::NiNode* Coordinator::FindCategoryNodeLive(RE::NiAVObject* a_object, Ineligible* a_parentReason) const
	{
		EngineReadWindow::Touch("Coordinator::FindCategoryNodeLive");
		if (ViewRoot(a_object)) {
			if (a_parentReason)
				*a_parentReason = Ineligible::None;
			return static_cast<RE::NiNode*>(a_object);
		}
		Ineligible reason = Ineligible::None;
		const RE::NiAVObject* top = a_object;
		RE::NiNode* node = a_object ? a_object->parent : nullptr;
		for (std::uint32_t depth = 0; node && depth < kMaxParentDepth; ++depth, node = node->parent) {
			if (categoryNodes.contains(node)) {
				if (a_parentReason)
					*a_parentReason = reason;
				return node;
			}
			reason = Eligibility::CombineParentReasons(reason, Eligibility::ParentReason(node));
			top = node;
		}
		if (!node && top && !top->parent && !alwaysRenderRoots.empty()) {
			if (const auto it = alwaysRenderRoots.find(top); it != alwaysRenderRoots.end()) {
				if (a_parentReason)
					*a_parentReason = reason;
				return it->second.category;
			}
		}
		return nullptr;
	}

	void Coordinator::TakeCategoryCapture()
	{
		// Taken before the drains so the portal roots it pushed are in this pass's batch.
		if (auto taken = channels.categoryPosted.Take())
			categoryTaken = std::move(*taken);
	}

	void Coordinator::RefreshCategoryNodes(bool a_force)
	{
		// Diffed when new, or forced after a detach.
		const auto* capture = categoryTaken.get();
		if (!capture || (!a_force && capture->generation == channels.categoryAppliedGeneration.load(std::memory_order_relaxed)))
			return;
		const std::uint8_t cause = a_force ? 1 : 0;
		channels.categoryAppliedGeneration.store(capture->generation, std::memory_order_release);
		// Pins are taken only at a capture's first application. A forced diff of an applied capture finds them let go of.
		if (capture->generation > categoryPinsGeneration) {
			categoryPinsGeneration = capture->generation;
			for (const auto& pin : capture->pins) {
				batchPins.insert_or_assign(pin.get(), pin.get());
				++referenceStats.pins;
			}
		}
		categorySignature = capture->signature;
		viewRoots = capture->viewRoots;
		const auto& current = capture->nodes;
		const auto& currentRoots = capture->roots;

		bool removedAny = false;
		for (auto* node : categoryNodes) {
			if (!current.contains(node)) {
				removedAny = true;
				break;
			}
		}
		bool removedRoot = false;
		for (const auto& [root, entry] : alwaysRenderRoots) {
			if (const auto it = currentRoots.find(root); it == currentRoots.end() || it->second.category != entry.category) {
				removedRoot = true;
				break;
			}
		}
		if (removedAny || removedRoot)
			DropRemovedCategories(*capture, removedRoot);
		std::vector<RE::NiAVObject*> addedRoots;
		for (const auto& [root, entry] : currentRoots) {
			if (!alwaysRenderRoots.contains(root) && categoryNodes.contains(entry.category))
				addedRoots.push_back(entry.object);
		}

		// Their content was attached before the cell was, so scan it now.
		std::vector<RE::NiNode*> added;
		for (auto* node : current) {
			if (!categoryNodes.contains(node))
				added.push_back(node);
		}
		categoryNodes = current;
		nodeSetsDirty = true;  // the publication's copy made again (IsCategoryNode)
		for (auto& [root, entry] : alwaysRenderRoots)
			HandBack(std::move(entry.root));
		// Copies of the capture's references, never made from a key.
		alwaysRenderRoots.clear();
		for (const auto& [root, entry] : currentRoots)
			alwaysRenderRoots.emplace(root, AlwaysRenderRoot{ entry.root, entry.category, entry.object });
		std::erase_if(categoryFound, [&](const auto& a_entry) { return !categoryNodes.contains(const_cast<RE::NiNode*>(a_entry.first)); });
		TrackAppearedCategories(*capture, added, addedRoots, cause);
	}

	void Coordinator::DropRemovedCategories(const CategoryCapture& a_capture, bool a_removedRoot)
	{
		const auto& current = a_capture.nodes;
		const auto& currentRoots = a_capture.roots;
		// Uses the index rather than a walk over every tracked entry.
		auto& a = attachStats;
		std::vector<RE::BSGeometry*> stale;
		auto portalStale = [&](RE::BSGeometry* a_geometry, const RE::NiNode* a_category) {
			const void* top = a_geometry;
			const void* parent = mirror.Parent(top);
			for (; parent && parent != a_category; parent = mirror.Parent(top))
				top = parent;
			if (parent || !alwaysRenderRoots.contains(static_cast<const RE::NiAVObject*>(top)))
				return false;
			const auto it = currentRoots.find(static_cast<const RE::NiAVObject*>(top));
			return it == currentRoots.end() || it->second.category != a_category;
		};
		std::vector<const RE::NiNode*> scanned;
		for (const auto& [node, members] : categoryMembers)
			if (!current.contains(const_cast<RE::NiNode*>(node))) {
				++a.categoriesLeft;
				scanned.push_back(node);
			}
		const std::size_t gone = scanned.size();
		if (a_removedRoot)
			for (const auto& [root, entry] : alwaysRenderRoots)
				if (const auto it = currentRoots.find(root); (it == currentRoots.end() || it->second.category != entry.category) && current.contains(const_cast<RE::NiNode*>(entry.category)) &&
															 std::find(scanned.begin(), scanned.end(), entry.category) == scanned.end())
					scanned.push_back(entry.category);
		for (std::size_t i = 0; i < scanned.size(); ++i) {
			const auto members = categoryMembers.find(scanned[i]);
			if (members == categoryMembers.end())
				continue;
			for (auto* geometry : members->second) {
				const auto entry = tracked.find(geometry);
				if (entry == tracked.end() || entry->second.categoryNode != scanned[i])
					continue;
				a.rootsLeftScanned += i >= gone ? 1 : 0;
				if (i < gone || portalStale(geometry, scanned[i]))
					stale.push_back(geometry);
			}
		}
		a.categoryMembersDropped += stale.size();
		for (auto* geometry : stale)
			EraseTracked(geometry);
	}

	void Coordinator::TrackAppearedCategories(const CategoryCapture& a_capture, const std::vector<RE::NiNode*>& a_added,
		const std::vector<RE::NiAVObject*>& a_addedRoots, std::uint8_t a_cause)
	{
		const TrackSource previousSource = addSource;
		addSource = TrackSource::CategoryAppeared;
		std::vector<std::pair<RE::NiAVObject*, SubtreeSource>> walked;
		for (auto* node : a_added) {
			categoryFound[node] = { sceneFrame, a_cause };
			const auto keys = a_capture.children.find(node);
			if (keys == a_capture.children.end()) {
				++categoryChildrenMissing;
				continue;
			}
			for (const auto* child : keys->second)
				walked.emplace_back(const_cast<RE::NiAVObject*>(child), SubtreeSource::CategoryWalk);
		}
		for (const auto& [root, entry] : alwaysRenderRoots) {
			if (std::find(a_added.begin(), a_added.end(), entry.category) != a_added.end())
				walked.emplace_back(entry.object, SubtreeSource::CategoryWalk);
		}
		for (auto* root : a_addedRoots)
			walked.emplace_back(root, SubtreeSource::CategoryWalk);
		AddSubtrees(walked);
		addSource = previousSource;
	}

	void Coordinator::UnlistCategoryMember(RE::BSGeometry* a_geometry, Tracked& a_tracked)
	{
		if (a_tracked.categoryNode)
			Unlist(categoryMembers, categoryMembersAt, static_cast<const RE::NiNode*>(a_tracked.categoryNode), a_geometry);
	}

	std::string Coordinator::TakeAttachLine()
	{
		auto& a = attachStats;
		if (!a.mirrorBatches && !a.geometryRounds && !a.categoriesLeft && !a.mirrorKindEvents[5] && !a.structuralEvents)
			return {};
		std::string line = fmt::format(
			"[DCLF] attach burst (W8): mirror {} batches, {} captures prepared ({} batches on the pool), {} records, prepare {:.2f} ms, apply {:.2f} ms; subtree geometries "
			"{} rounds ({} on the pool), {} prepared ({} on the pool); category nodes left {}, members dropped {}, portal-category members scanned {}",
			a.mirrorBatches, a.mirrorCaptures, a.mirrorPooled, a.mirrorRecords, a.mirrorPrepareMs, a.mirrorApplyMs, a.geometryRounds, a.geometryPooled, a.geometries,
			a.geometriesPooled, a.categoriesLeft, a.categoryMembersDropped, a.rootsLeftScanned);
		std::string kinds;
		static constexpr std::array<const char*, AttachStats::kMirrorKinds> kKindNames{ "captures", "fills", "evictions", "detaches", "gates", "updates" };
		for (std::size_t k = 0; k < AttachStats::kMirrorKinds; ++k)
			if (a.mirrorKindEvents[k])
				kinds += fmt::format("{}{} {} {:.2f} ms", kinds.empty() ? "" : ", ", kKindNames[k], a.mirrorKindEvents[k], a.mirrorKindMs[k]);
		line += fmt::format("; mirror apply by kind: {}; replays skipped {}, walked {}; updates kept {}, let go {}", kinds.empty() ? "none" : kinds, a.replaysSkipped,
			a.replaysWalked, a.updatesKept, a.updatesForgotten);
		line += fmt::format("; attach/detach: {} events, sun ancestors {:.2f} ms, merges {:.2f} ms, detached geometries {} ({} erased) {:.2f} ms; leaf holds {} of {} keys {:.2f} ms",
			a.structuralEvents, a.ancestorMs, a.mergeMs, a.detachedGeometries, a.detachedErased, a.detachMs, a.leafHolds, a.leafKeys, a.leafMs);
		a = {};
		return line;
	}

	const void* Coordinator::MirrorSunEntry(const void* a_geometry) const
	{
		const auto* record = mirror.Node(a_geometry);
		const void* entry = nullptr;
		if (const void* reference = record ? record->userData : nullptr) {
			// An actor's entry is its cell's container, which the full-frustum cull never tests.
			if (record->formType != static_cast<std::uint8_t>(RE::FormType::ActorCharacter)) {
				const void* root = a_geometry;
				for (const void* node = record->parent; node;) {
					const auto* at = mirror.Node(node);
					if (!at || at->userData != reference)
						break;
					root = node;
					node = at->parent;
				}
				entry = root;
			}
		} else if (record) {
			static const REL::Relocation<const RE::NiRTTI*> multiBound{ RE::BSMultiBoundNode::Ni_RTTI };
			for (const void* node = record->parent; node;) {
				const auto* at = mirror.Node(node);
				if (!at)
					break;
				if (at->rtti == multiBound.get()) {
					entry = node;
					break;
				}
				node = at->parent;
			}
		}
		return entry;
	}

	void Coordinator::ResolveSunEntry(Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live)
	{
		if (!a_tracked.sunEntryResolved) {
			a_tracked.sunEntryResolved = true;
			++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::SunEntry)];
			const void* entry = MirrorSunEntry(&a_geometry);
			// The mirror's, or on a live walk the live chain's.
			if (!a_live) {
				a_tracked.entryRoot = static_cast<const RE::NiAVObject*>(entry);
				return;
			}
			const EngineReadWindow::Lease lease;
			a_tracked.entryRoot = LiveSunEntry(a_geometry);
		}
	}

	const RE::NiAVObject* Coordinator::LiveSunEntry(const RE::BSGeometry& a_geometry)
	{
		const RE::NiAVObject* live = nullptr;
		auto* geometry = const_cast<RE::BSGeometry*>(&a_geometry);
		if (auto* reference = geometry->GetUserData()) {
			// An actor's entry is its cell's container, which the full-frustum cull never tests.
			if (reference->GetFormType() != RE::FormType::ActorCharacter) {
				const RE::NiAVObject* root = geometry;
				for (auto* node = geometry->parent; node && node->GetUserData() == reference; node = node->parent)
					root = node;
				live = root;
			}
		} else {
			static const REL::Relocation<const RE::NiRTTI*> multiBound{ RE::BSMultiBoundNode::Ni_RTTI };
			for (auto* node = geometry->parent; node; node = node->parent)
				if (node->GetRTTI() == multiBound.get()) {
					live = node;
					break;
				}
		}
		return live;
	}

	std::int32_t Coordinator::NearestFadeNodeBsx(const SceneCapture::LeafView& a_leaf) const
	{
		const SceneCapture::NodeRecord* fadeNode = nullptr;
		for (const auto* record = a_leaf.parent; record && !fadeNode; record = record->parent ? mirror.Node(record->parent) : nullptr)
			fadeNode = (record->kind & SceneCapture::kKindFadeNode) ? record : nullptr;
		return fadeNode ? fadeNode->bsx : -1;
	}

	const void* Coordinator::AncestorKeyOf(const void* a_object) const
	{
		// The nearest node of the contribution's classes, as Light Limit Fix's GetParentRoomNode finds it. Its classes were resolved at install.
		const auto* value = Extensions::AncestorValue();
		if (!value)
			return nullptr;
		for (const void* key = a_object; key;) {
			const auto* record = mirror.Node(key);
			if (!record)
				return nullptr;
			if (value->IsKey(record->rtti))
				return key;
			key = record->parent;
		}
		return nullptr;
	}

	Coordinator::HeldKeys Coordinator::HeldKeysOf(const void* a_geometry) const
	{
		HeldKeys keys;
		const auto* record = mirror.Geometry(a_geometry);
		if (!record)
			return keys;
		keys.property = record->property;
		keys.layer = record->Layer();
		// An attach's capture pins every ancestor.
		const auto* property = record->property ? mirror.Property(record->property) : nullptr;
		keys.fadeNode = property ? property->fadeNode : nullptr;
		// The engine's empty fade node sentinel is no root, unless the switch says otherwise.
		static const bool emptyIsNone = RegisteredSwitchValue("CS_DCLF_EMPTY_FADE_NODE") != "0";
		if (emptyIsNone && keys.fadeNode == Engine::EmptyFadeNode())
			keys.fadeNode = nullptr;
		keys.propertyPin = PinOf(keys.property);
		keys.layerPin = PinOf(keys.layer);
		keys.fadeNodePin = PinOf(keys.fadeNode);
		return keys;
	}

	void Coordinator::HoldProperties(Tracked& a_entry, const HeldKeys& a_keys)
	{
		auto hold = [&](RE::NiPointer<RE::BSShaderProperty>& a_held, const void* a_key, RE::NiRefObject* a_pin, std::uint32_t a_slot) {
			if (a_held.get() == a_key)
				return;
			// A resident's pipeline template is its first member's held property, so recount it.
			if (a_slot != kNoObjectSlot)
				NoteResidentTouched(a_slot);
			auto next = FromPin<RE::BSShaderProperty>(a_pin);
			if (a_key && !next) {
				++referenceStats.propertiesRefused;
				if (referenceStats.firstPropertyRefused.empty())
					referenceStats.firstPropertyRefused = fmt::format("property {} of geometry {}", a_key, static_cast<const void*>(a_entry.geometry.get()));
			} else if (next) {
				++referenceStats.properties;
			}
			HandBack(std::move(a_held));
			a_held = std::move(next);
		};
		hold(a_entry.property, a_keys.property, a_keys.propertyPin, a_entry.slot);
		hold(a_entry.layerProperty, a_keys.layer, a_keys.layerPin, a_entry.layerSlot);
		const void* fadeNode = a_keys.fadeNode;
		if (a_entry.fadeNodeRef.get() != fadeNode) {
			HandBack(std::move(a_entry.fadeNodeRef));
			if (fadeNode && (a_entry.fadeNodeRef = FromPin<RE::NiAVObject>(a_keys.fadeNodePin)))
				++referenceStats.fadeNodes;
			else if (fadeNode)
				AcquireFadeNode(a_entry, fadeNode, "a capture");
		}
	}

	bool Coordinator::AcquireFadeNode(Tracked& a_entry, const void* a_node, const char* a_path)
	{
		auto& r = referenceStats;
		const auto* key = static_cast<const RE::NiAVObject*>(a_node);
		if ((a_entry.fadeNodeRef = Pinned(key))) {
			++r.fadeNodes;
			return true;
		}
		++r.fadeNodesRefused;
		// A shared model's property names the fade node of the instance cloned last, so use that root's owner.
		if (const auto owner = rootOwners.find(key); owner != rootOwners.end() && owner->second) {
			a_entry.fadeNodeRef = owner->second;
			++r.fadeNodesFromRoots;
			return true;
		}
		if (r.fadeNodeNamed < 4) {
			++r.fadeNodeNamed;
			const auto* geometry = a_entry.geometry.get();
			const auto* leafRecord = geometry ? mirror.Node(geometry) : nullptr;
			const auto* nodeRecord = mirror.Node(a_node);
			bool ancestor = false;
			for (const void* at = geometry ? mirror.Parent(geometry) : nullptr; at && !ancestor; at = mirror.Parent(at))
				ancestor = at == a_node;
			r.fadeNodeFirst += fmt::format("{}'{}' {}: fade node '{}' {} ({}{}), {} the geometry's mirror chain, wanted at {}, the batch {} pins", r.fadeNodeFirst.empty() ? "" : "; ",
				leafRecord && leafRecord->name ? leafRecord->name : "", static_cast<const void*>(geometry), nodeRecord && nodeRecord->name ? nodeRecord->name : "", a_node,
				!nodeRecord ? "no record" : (nodeRecord->kind & SceneCapture::kKindTree) ? "a tree node" : (nodeRecord->kind & SceneCapture::kKindFadeNode) ? "a fade node" : "another node",
				nodeRecord && nodeRecord->userData ? ", a reference's" : "", ancestor ? "on" : "off", a_path, batchPins.empty() ? "had no" : fmt::format("had {}", batchPins.size()));
		}
		// Asked once per key.
		if (a_entry.geometry && a_entry.fadeNodeRequested != a_node) {
			a_entry.fadeNodeRequested = a_node;
			channels.fadeNodeRequests.Push(FadeNodeRequest{ a_entry.geometry, a_node });
			++r.fadeNodesRequested;
		}
		return false;
	}

	void Coordinator::AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason, bool a_live, GeometryPrep& a_prep)
	{
		// The reads were the prepare's. Here are the stores, in event order.
		RE::NiPointer<RE::BSGeometry> reference;
		auto it = tracked.find(a_geometry);
		if (it != tracked.end() && it->second.geometry.get() == a_geometry)
			reference = it->second.geometry;
		else if (!(reference = FromPin<RE::BSGeometry>(a_prep.geometryPin)) && a_live) {
			++referenceStats.live;
			reference.reset(a_geometry);
		}
		if (!reference) {
			// Nothing holds it, because it left or a pin is missing.
			++referenceStats.refused;
			++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::Reference)];
			return;
		}
		const bool inserted = it == tracked.end();
		if (inserted)
			it = tracked.try_emplace(a_geometry).first;
		trackedLayout += inserted ? 1 : 0;
		auto& entry = it->second;
		const auto* ownerReference = a_prep.ownerReference;
		const auto identity = sceneIdentity.Attach(a_geometry, ownerReference);
		if (identity.replaced) {
			ReleaseObjectSlot(entry);
			entry.ownerResolved = false;
			entry.entryClassResolved = false;
		}
		if (inserted) {
			entry.trackedFrame = sceneFrame;
			entry.trackedBy = addSource;
			if (const auto erased = erasedByDetach.find(a_geometry); erased != erasedByDetach.end()) {
				// A load moves many at once, so the lines are capped.
				constexpr std::uint32_t kSplitMoveLines = 2, kSplitMoveWindow = 600;
				static std::uint32_t splitWindow = ~0u;
				static ankerl::unordered_dense::map<const void*, std::uint32_t> splitLines;
				if (sceneFrame / kSplitMoveWindow != splitWindow) {
					splitWindow = sceneFrame / kSplitMoveWindow;
					splitLines.clear();
				}
				++splitMoves;
				if (splitLines[a_prep.ownerReference]++ < kSplitMoveLines)
					logger::warn("[DCLF] split move: '{}' {} (reference {}) erased by a detach at frame {} and tracked again at frame {} ({} so far)",
						a_geometry->name.c_str() ? a_geometry->name.c_str() : "", static_cast<const void*>(a_geometry), static_cast<const void*>(a_prep.ownerReference),
						erased->second, sceneFrame, splitMoves);
				erasedByDetach.erase(erased);
			}
		}
		entry.identity = identity.member;
		entry.groupIdentity = identity.group;
		entry.ownerReference = ownerReference;
		// The engine moved the object to another room. Look it up now, since the joins write it only when they evaluate the object again.
		if (entry.ancestorKey != a_prep.ancestorKey) {
			entry.ancestorKey = a_prep.ancestorKey;
			if (Extensions::AncestorValueOn())
				RefreshAncestorValue(entry);
			else
				entry.ancestorMapGeneration = 0;
		}
		entry.geometry = std::move(reference);
		HoldProperties(entry, a_prep.held);
		{
			const auto* head = a_prep.source;
			if (entry.sourceRef.get() != head) {
				HandBack(std::move(entry.sourceRef));
				if (head && !(entry.sourceRef = FromPin<RE::NiNode>(a_prep.sourcePin)))
					++referenceStats.refused;
			}
		}
		if (entry.categoryNode != a_categoryNode) {
			if (entry.categoryNode)
				Unlist(categoryMembers, categoryMembersAt, static_cast<const RE::NiNode*>(entry.categoryNode), a_geometry);
			List(categoryMembers, categoryMembersAt, static_cast<const RE::NiNode*>(a_categoryNode), a_geometry);
		}
		entry.categoryNode = a_categoryNode;
		// No mirror record, so the render thread captures it at the next frame start.
		if (!a_prep.mirrored)
			channels.mirrorCaptureRequests.Push(RE::NiPointer<RE::NiAVObject>(a_geometry));
		entry.parentReason = a_parentReason;
		// The sun entry's bound now includes this one, so its other dependents are evaluated again.
		entry.candidateFrame = 0;
		{
			UnlistDependents(a_geometry, entry, true);
			entry.sunEntryResolved = false;
			entry.entryRoot = nullptr;
			const bool prepared = a_prep.resolved && !a_live;
			if (prepared) {
				++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::SunEntry)];
				entry.sunEntryResolved = true;
				entry.entryRoot = static_cast<const RE::NiAVObject*>(a_prep.sunEntry);
			} else {
				ResolveSunEntry(entry, *a_geometry, a_live);
			}
			if (entry.entryRoot && !OwnRoot(entry.entryRoot, a_live))
				entry.entryRoot = nullptr;
			if (entry.entryRoot) {
				List(rootDependents, rootDependentsAt, entry.entryRoot, a_geometry);
				const auto* rootRecord = prepared ? nullptr : mirror.Node(entry.entryRoot);
				if (const auto* rootRef = prepared ? a_prep.sunEntryReference : rootRecord ? static_cast<const RE::TESObjectREFR*>(rootRecord->userData) : nullptr;
					rootRef && rootReference.try_emplace(entry.entryRoot, rootRef).second)
					referenceRoot[rootRef] = entry.entryRoot;
				entry.listedRoot = entry.entryRoot;
				dirtyRoots.push_back(entry.entryRoot);
				MarkListedRootDirty(entry.entryRoot);
			}
			const RE::NiAVObject* lightEntry = nullptr;
			if (!prepared) {
				lightEntry = LightEntryOf(entry, *a_geometry, a_live);
			} else {
				if (entry.categoryNode)
					++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::LightEntry)];
				lightEntry = a_prep.lightActorRoot ? a_prep.lightActorRoot : entry.entryRoot && !a_prep.sunEntryCategory ? entry.entryRoot : nullptr;
			}
			if (lightEntry && OwnRoot(lightEntry, a_live)) {
				auto& dependents = lightDependents[lightEntry];
				lightEntriesAppeared += dependents.empty() ? 1 : 0;
				if (dependents.empty())
					lightEntryChanges.push_back({ lightEntry, 1 });  // a light entry new to the frame's set (IsLightEntry, through the log)
				List(lightDependents, lightDependentsAt, lightEntry, a_geometry);
				entry.lightRoot = lightEntry;
				MarkLightEntryDirty(lightEntry);
			}
		}
		if (a_prep.ranged)
			entryRanges[a_geometry] = std::move(a_prep.ranges);
		pendingEvaluation.push_back(a_geometry);
		if (a_prep.mark)
			ComponentsUnderMark(a_geometry, a_prep.mark);
	}

	const RE::NiAVObject* Coordinator::LightEntryOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live) const
	{
		// An actor's root is the light entry for everything under it, carried items too.
		if (const auto* category = a_tracked.categoryNode) {
			++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::LightEntry)];
			const void* root = &a_geometry;
			for (const void* node = mirror.Parent(&a_geometry); node && node != category; node = mirror.Parent(node))
				root = node;
			const auto* record = root != &a_geometry ? mirror.Node(root) : nullptr;
			const bool actorRoot = record && record->parent == category && Extensions::OwnerClassOf(*record).ownerClass != Api::EntryClass::None;
			// The mirror's, or on a live walk the live chain's. Observers compare the two.
			if (!a_live) {
				if (actorRoot)
					return static_cast<const RE::NiAVObject*>(root);
				return a_tracked.entryRoot && !CategoryNodeOwn(a_tracked.entryRoot) ? a_tracked.entryRoot : nullptr;
			}
			const EngineReadWindow::Lease lease;
			bool liveActor = false;
			const RE::NiAVObject* live = LiveLightRoot(a_geometry, category, liveActor);
			if (liveActor)
				return live;
		}
		return a_tracked.entryRoot && !CategoryNodeOwn(a_tracked.entryRoot) ? a_tracked.entryRoot : nullptr;
	}

	const RE::NiAVObject* Coordinator::LiveLightRoot(const RE::BSGeometry& a_geometry, const RE::NiNode* a_category, bool& a_actor)
	{
		const RE::NiAVObject* live = &a_geometry;
		for (const auto* node = a_geometry.parent; node && node != a_category; node = node->parent)
			live = node;
		const auto* reference = live != &a_geometry && live->parent == a_category ? live->GetUserData() : nullptr;
		a_actor = reference && Extensions::OwnerClassOfForm(static_cast<std::uint8_t>(reference->GetFormType())).ownerClass != Api::EntryClass::None;
		return live;
	}

	void Coordinator::AddSubtree(RE::NiAVObject* a_root, SubtreeSource a_source)
	{
		SubtreePrep prep;
		PrepareSubtree(a_root, a_source, prep);
		fanoutStats.subtreeGeometries += prep.geometries.size();
		MergeSubtree(prep);
	}

	void Coordinator::PrepareSubtree(RE::NiAVObject* a_root, SubtreeSource a_source, SubtreePrep& a_out) const
	{
		PrepareSubtreeWalk(a_root, a_source, a_out);
		for (auto& geometry : a_out.geometries)
			PrepareGeometry(geometry.geometry, geometry.reason, a_out.category, geometry);
	}

	void Coordinator::PrepareSubtreeWalk(RE::NiAVObject* a_root, SubtreeSource a_source, SubtreePrep& a_out) const
	{
		// Pure over the mirror, category nodes and portal roots, so any thread under a ShardScope. The root is a key only, except for
		// the mirror-read parity's live check, which runs on the pass's thread.
		a_out.root = a_root;
		a_out.source = a_source;
		a_out.category = nullptr;
		a_out.unmirrored = false;
		a_out.geometries.clear();
		// The attach is drained a frame or more after it was queued, and a cell transition may have released the subtree.
		if (!a_root)
			return;
		Ineligible reasonAbove = Ineligible::None;
		a_out.category = FindCategoryNode(a_root, &reasonAbove, &a_out.unmirrored);
		if (!a_out.category)
			return;

		// The mirror may name a geometry the engine has since freed, so each reference is its capture's pin, taken at the merge.
		std::vector<std::pair<RE::BSGeometry*, Ineligible>> found;
		bool lacking = false;
		++MirrorReads().reads[static_cast<std::size_t>(MirrorRead::Subtree)];
		MirrorSubtree(a_root, reasonAbove, found, lacking);
		if (lacking)
			++MirrorReads().unmirrored[static_cast<std::size_t>(MirrorRead::Subtree)];
		a_out.geometries.resize(found.size());
		for (std::size_t i = 0; i < found.size(); ++i) {
			a_out.geometries[i].geometry = found[i].first;
			a_out.geometries[i].reason = found[i].second;
		}
	}

	void Coordinator::PrepareGeometry(RE::BSGeometry* a_geometry, Ineligible a_reason, const RE::NiNode* a_category, GeometryPrep& a_out) const
	{
		// Keys only. Sun and light entry reads are counted at the merge.
		a_out = GeometryPrep{};
		a_out.geometry = a_geometry;
		a_out.reason = a_reason;
		const auto* record = mirror.Node(a_geometry);
		a_out.mirrored = record != nullptr;
		a_out.ownerReference = record && Extensions::OwnerClassOf(*record).ownerClass != Api::EntryClass::None ?
		                           static_cast<const RE::TESObjectREFR*>(record->userData) :
		                           nullptr;
		a_out.ancestorKey = AncestorKeyOf(a_geometry);
		const auto* g = mirror.Geometry(a_geometry);
		const auto* parent = record && record->parent ? mirror.Node(record->parent) : nullptr;
		const auto entryClass = parent && g ? Extensions::EntryClassOf(g->type, parent->kind) : Api::EntryClass::None;
		const bool held = entryClass != Api::EntryClass::None && Extensions::EntryClassRuleOf(entryClass).holdsParent;
		a_out.source = held ? static_cast<const RE::NiNode*>(record->parent) : nullptr;
		a_out.ranged = g && Extensions::GeometryTypeRanged(g->type);
		a_out.geometryPin = PinOf(a_geometry);
		a_out.sourcePin = a_out.source ? PinOf(a_out.source) : nullptr;
		a_out.held = HeldKeysOf(a_geometry);
		if (a_out.ranged && g)
			a_out.ranges = g->ranges;
		// Marks change only with the mirror's events, before the adds, so the parallel prepares read them as data.
		if (!rootMarks.empty())
			for (const void* key = record ? record->parent : nullptr; key;) {
				if (const auto at = rootMarks.find(key); at != rootMarks.end()) {
					a_out.mark = at->second.mark;
					break;
				}
				if (key == a_category)
					break;
				const auto* node = mirror.Node(key);
				key = node ? node->parent : nullptr;
			}
		a_out.resolved = true;
		a_out.sunEntry = MirrorSunEntry(a_geometry);
		if (a_out.sunEntry) {
			const auto* rootRecord = mirror.Node(a_out.sunEntry);
			a_out.sunEntryReference = rootRecord ? static_cast<const RE::TESObjectREFR*>(rootRecord->userData) : nullptr;
			a_out.sunEntryCategory = CategoryNodeOwn(static_cast<const RE::NiAVObject*>(a_out.sunEntry));
		}
		if (a_category) {
			const void* root = a_geometry;
			for (const void* node = mirror.Parent(a_geometry); node && node != a_category; node = mirror.Parent(node))
				root = node;
			const auto* rootRecord = root != static_cast<const void*>(a_geometry) ? mirror.Node(root) : nullptr;
			if (rootRecord && rootRecord->parent == a_category && Extensions::OwnerClassOf(*rootRecord).ownerClass != Api::EntryClass::None)
				a_out.lightActorRoot = static_cast<const RE::NiAVObject*>(root);
		}
	}

	void Coordinator::MergeSubtree(SubtreePrep& a_prep)
	{
		if (!a_prep.root)
			return;
		if (!a_prep.category) {
			if (a_prep.unmirrored && a_prep.source == SubtreeSource::CategoryWalk) {
				// A pin's copy, never a reference made from a key.
				if (auto root = Pinned(static_cast<const RE::NiAVObject*>(a_prep.root))) {
					++subtreesPended;
					channels.mirrorCaptureRequests.Push(root);
					pendingSubtrees.push_back(std::move(root));
				} else {
					++subtreesDropped;
				}
			} else if (a_prep.unmirrored && a_prep.source == SubtreeSource::Retry) {
				++subtreesDropped;
			}
			return;
		}
		for (auto& geometry : a_prep.geometries)
			AddGeometry(geometry.geometry, a_prep.category, geometry.reason, false, geometry);
	}

	void Coordinator::PrepareSubtrees(const std::vector<std::pair<RE::NiAVObject*, SubtreeSource>>& a_roots, std::uint32_t a_mode, std::vector<SubtreePrep>& a_out)
	{
		// Mirror reads are counted per chunk and merged in chunk order.
		const std::size_t count = a_roots.size();
		const bool parallel = a_mode == 2;
		const std::size_t chunks = (count + kSubtreeGrain - 1) / kSubtreeGrain;
		auto& f = fanoutStats;
		f.subtreeParallelRounds += parallel ? 1 : 0;
		f.subtreeParallelRoots += parallel ? count : 0;
		a_out.resize(count);
		// Geometry prepares get their own chunks, so a few large attaches reach the pool though their roots are few.
		auto walk = [&](std::vector<SubtreePrep>& a_into, std::vector<MirrorReadStats>& a_counters, bool a_pool) {
			auto body = [&](std::size_t a_begin, std::size_t a_end) {
				ShardScope scope(*this, a_pool, &a_counters[a_begin / kSubtreeGrain]);
				for (std::size_t i = a_begin; i < a_end; ++i)
					PrepareSubtreeWalk(a_roots[i].first, a_roots[i].second, a_into[i]);
			};
			if (a_pool) {
				::RenderGraph::Frame::SharedExecutor().ParallelFor("CS.DCLF.Scene.Subtrees", count, kSubtreeGrain, body);
			} else {
				for (std::size_t begin = 0; begin < count; begin += kSubtreeGrain)
					body(begin, std::min(count, begin + kSubtreeGrain));
			}
		};
		std::vector<MirrorReadStats> counters(chunks);
		std::vector<MirrorReadStats> geometryCounters;
		{
			DCLF_ZONE("CS.DCLF.Scene.SubtreesFanout");
			const auto start = std::chrono::steady_clock::now();
			walk(a_out, counters, parallel);
			auto& index = subtreeGeometryIndex;
			index.clear();
			for (std::uint32_t p = 0; p < a_out.size(); ++p)
				if (a_out[p].category)
					for (std::uint32_t g = 0; g < a_out[p].geometries.size(); ++g)
						index.emplace_back(p, g);
			const std::size_t geometries = index.size();
			const bool pool = geometries && FanoutMode(geometries, kGeometryFanoutMin) == 2;
			auto& a = attachStats;
			a.geometryRounds += geometries ? 1 : 0;
			a.geometryPooled += pool ? 1 : 0;
			a.geometries += geometries;
			a.geometriesPooled += pool ? geometries : 0;
			geometryCounters.resize((geometries + kGeometryGrain - 1) / kGeometryGrain);
			auto body = [&](std::size_t a_begin, std::size_t a_end) {
				DCLF_ZONE("CS.DCLF.Scene.SubtreeGeometriesShards");
				ShardScope scope(*this, pool, &geometryCounters[a_begin / kGeometryGrain]);
				for (std::size_t i = a_begin; i < a_end; ++i) {
					auto& prep = a_out[index[i].first];
					auto& geometry = prep.geometries[index[i].second];
					PrepareGeometry(geometry.geometry, geometry.reason, prep.category, geometry);
				}
			};
			if (pool) {
				::RenderGraph::Frame::SharedExecutor().ParallelFor("CS.DCLF.Scene.SubtreeGeometries", geometries, kGeometryGrain, body);
			} else {
				for (std::size_t begin = 0; begin < geometries; begin += kGeometryGrain)
					body(begin, std::min(geometries, begin + kGeometryGrain));
			}
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			AddScenePartMs(ScenePart::SubtreesFanout, ms);
			f.subtreePrepareMs += ms;
		}
		for (auto& chunk : counters)
			MergeMirrorReads(chunk);
		for (auto& chunk : geometryCounters)
			MergeMirrorReads(chunk);
		for (const auto& prep : a_out)
			f.subtreeGeometries += prep.geometries.size();
	}

	void Coordinator::AddSubtrees(const std::vector<std::pair<RE::NiAVObject*, SubtreeSource>>& a_roots)
	{
		if (a_roots.empty())
			return;
		auto& f = fanoutStats;
		++f.subtreeRounds;
		f.subtreeRoots += a_roots.size();
		const std::uint32_t mode = FanoutMode(a_roots.size(), kSubtreeFanoutMin);
		if (mode == 0) {
			for (const auto& [root, source] : a_roots)
				AddSubtree(root, source);
			return;
		}
		// Merges never write what a prepare reads.
		[[maybe_unused]] const std::uint64_t mirrorWrites = mirror.Writes();
		[[maybe_unused]] const std::size_t categories = categoryNodes.size();
		[[maybe_unused]] const std::size_t portalRoots = alwaysRenderRoots.size();
		PrepareSubtrees(a_roots, mode, subtreePreps);
		{
			DCLF_ZONE("CS.DCLF.Scene.SubtreesMerge");
			const auto start = std::chrono::steady_clock::now();
			for (auto& prep : subtreePreps)
				MergeSubtree(prep);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			AddScenePartMs(ScenePart::SubtreesMerge, ms);
			f.subtreeMergeMs += ms;
		}
		assert(mirror.Writes() == mirrorWrites && categoryNodes.size() == categories && alwaysRenderRoots.size() == portalRoots);
	}

	void Coordinator::MirrorSubtree(const void* a_root, Ineligible a_reason, std::vector<std::pair<RE::BSGeometry*, Ineligible>>& a_out, bool& a_unmirrored) const
	{
		// Switch selections were caught up at ingestion.
		if (ForEachMirror(mirror, a_root, a_reason, [&](const void* a_key, const SceneCapture::NodeRecord& a_record, Ineligible a_above, Ineligible& a_below) {
				if (a_record.kind & SceneCapture::kKindGeometry)
					a_out.emplace_back(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key)), a_above);
				else if (a_record.kind & SceneCapture::kKindNode)
					a_below = Eligibility::CombineParentReasons(a_above, Eligibility::ParentReasonOf(a_record));
			}))
			a_unmirrored = true;
	}




	bool Coordinator::HiddenForWalk(const RE::NiAVObject* a_object) const
	{
		const auto& cullHidden = FrameGlobals::Current().cullHidden;
		const auto it = std::lower_bound(cullHidden.begin(), cullHidden.end(), a_object, [](const auto& a_entry, const RE::NiAVObject* a_key) { return a_entry.first < a_key; });
		if (it != cullHidden.end() && it->first == a_object)
			return it->second;
		return IsHidden(a_object);
	}

	bool Coordinator::HiddenForWalk(const void* a_object, const SceneCapture::NodeRecord& a_record) const
	{
		const auto& cullHidden = FrameGlobals::Current().cullHidden;
		const auto* object = static_cast<const RE::NiAVObject*>(a_object);
		const auto it = std::lower_bound(cullHidden.begin(), cullHidden.end(), object, [](const auto& a_entry, const RE::NiAVObject* a_key) { return a_entry.first < a_key; });
		if (it != cullHidden.end() && it->first == object)
			return it->second;
		// A gated LOD block's own hide belongs to the gate, not the scene.
		return (a_record.flags & static_cast<std::uint32_t>(RE::NiAVObject::Flag::kHidden)) != 0 && !mirror.Gated(a_object);
	}

	std::int32_t Coordinator::BillboardModeOf(const Tracked& a_tracked) const
	{
		if (a_tracked.parentReason != Ineligible::Billboard)
			return -1;
		// First-person entries such as magic hand glows too, since the first-person forward block is world space throughout.
		const auto leaf = mirror.Leaf(a_tracked.geometry.get());
		const auto* port = leaf ? SceneCapture::LeafView::Port(leaf.property) : nullptr;
		const std::uint8_t kind = port ? port->port : std::uint8_t{ 0 };
		const auto* lighting = leaf ? SceneCapture::LeafView::Lighting(leaf.property) : nullptr;
		if (!leaf || (!Extensions::BillboardTurnedLeaf(kind, leaf.Type(), leaf.Skinned()) &&
						 !Extensions::BillboardTurnedRefraction(kind, lighting ? lighting->flags : 0u, leaf.Type(), leaf.Skinned())))
			return -1;
		std::int32_t mode = -1;
		std::uint32_t billboards = 0;
		const auto* record = mirror.Node(a_tracked.geometry.get());
		for (const void* key = record ? record->parent : nullptr; key && key != a_tracked.categoryNode; key = record->parent) {
			record = mirror.Node(key);
			if (!record)
				return -1;
			if (record->kind & SceneCapture::kKindBillboard) {
				++billboards;
				mode = static_cast<std::int32_t>(SceneCapture::BillboardModeOf(record->kind));
			}
		}
		// Exactly one, since a nested billboard turns from a parent world the outer node's cull wrote.
		return billboards == 1 && mode <= 5 ? mode : -1;
	}

	void Coordinator::ViewDecalForward(const Tracked& a_tracked, LightingDescriptors& a_descriptors) const
	{
		// A view root never has group 3, which is a multi-index layer's.
		const std::uint32_t group = a_descriptors.decalGroup;
		if (group != 1 && group != 2)
			return;
		const auto view = ViewRootOf(a_tracked.categoryNode);
		if (view == Api::EngineView::Count || !Extensions::ViewForwardDecals(view))
			return;
		// The forward pixel stage drops its kind's Deferred bit, as LightingShaderDescriptors does for a forward key.
		a_descriptors.pixel &= ~Extensions::PortRules(a_descriptors.port).deferredPixelBit;
		a_descriptors.forward = true;
		a_descriptors.forwardBlendMode = group == 2 ? a_descriptors.decalBlendMode : 0u;
		a_descriptors.forwardDepthTest = true;
		a_descriptors.forwardDepthWrite = group == 1;
		a_descriptors.forwardDecalGroup = group;
		a_descriptors.decalGroup = 0;
		a_descriptors.decalBlendMode = 0;
		a_descriptors.decalWriteMode = 0;
	}

	bool Coordinator::HiddenFocusCaster(const Tracked& a_tracked, Ineligible a_reason, const SceneCapture::LeafView& a_leaf) const
	{
		if (a_reason != Ineligible::Hidden || !a_leaf || !Extensions::ParentAdmits(Api::EngineParent::Actor, Api::ParentLeaves::AllLeaves))
			return false;
		// A caster the focus registration builds passes for, not a volumetric-only one.
		if (ShadowCasterReject(a_leaf) != ShadowReject::None)
			return false;
		const void* key = a_tracked.geometry.get();
		std::uint32_t depth = 0;
		for (const auto* record = mirror.Node(key); record && depth < 256; key = record->parent, record = key ? mirror.Node(key) : nullptr, ++depth) {
			if (record->userData && record->actor)
				return true;
			if (key == a_tracked.categoryNode)
				break;
		}
		return false;
	}

	Ineligible Coordinator::ClassifyFrame(const Tracked& a_tracked) const
	{
		const auto switchLeaves = [] { return Extensions::ParentAdmits(Api::EngineParent::Switch, Api::ParentLeaves::SelectedLeaves); };
		const bool underSwitch = a_tracked.parentReason == Ineligible::Switch;
		if (underSwitch && !switchLeaves())
			return Ineligible::Switch;
		// Billboard outranks switch and ordered in parentReason, so the chain walk below applies their tests too.
		const bool underBillboard = a_tracked.parentReason == Ineligible::Billboard;
		// A particle system under billboards needs no turn. The engine updates it in 0x140d3e650 and its UpdateWorldData (0x140dd4c90)
		// is empty, so the engine draws the last update's world, which the snapshot copies.
		const auto particlesUnderBillboard = [&] {
			if (!Extensions::ParentAdmits(Api::EngineParent::Billboard, Api::ParentLeaves::ParticleSystems))
				return false;
			const auto leaf = mirror.Leaf(a_tracked.geometry.get());
			return leaf && ParticleGeometryType(leaf.Type());
		};
		if (underBillboard && BillboardModeOf(a_tracked) < 0 && !particlesUnderBillboard())
			return Ineligible::Billboard;
		const bool chainTests = underSwitch || underBillboard;
		// Under an ordered node only a blended leaf is rejected, unless the forward epoch takes it.
		const bool ordered = a_tracked.parentReason == Ineligible::UnsupportedParent;
		if (a_tracked.parentReason != Ineligible::None && !underSwitch && !ordered && !underBillboard)
			return a_tracked.parentReason;
		const auto orderedLeaf = [&] {
			return !Extensions::ObjectClassOn(Api::ObjectClass::Forward) && Eligibility::OrderedLeaf(mirror.Leaf(a_tracked.geometry.get()));
		};
		if (ordered && orderedLeaf())
			return Ineligible::UnsupportedParent;

		const bool actors = Extensions::ParentAdmits(Api::EngineParent::Actor, Api::ParentLeaves::AllLeaves);
		const void* child = nullptr;
		const void* key = a_tracked.geometry.get();
		std::uint32_t depth = 0;
		for (const auto* record = mirror.Node(key); record; child = key, key = record->parent, record = key ? mirror.Node(key) : nullptr, ++depth) {
			// The first-person root's kHidden belongs to the view, since the engine toggles it within the frame (FUN_140736150 at the camera
			// switch). The chain stops before it.
			if (ViewRootOf(key) != Api::EngineView::Count)
				break;
			if (HiddenForWalk(key, *record)) {
				// Diagnostic only. Pointers are never dereferenced.
				if (Diagnostics::Armed<Diagnostics::RootViewHidden>() && FirstViewRoot()) {
					const auto& globals = FrameGlobals::Current();
					const auto& skeleton = globals.firstPersonGeometry;
					const bool sameRoot = ViewRootEntry(a_tracked.categoryNode);
					if (sameRoot ||
						std::binary_search(skeleton.begin(), skeleton.end(), static_cast<const RE::NiAVObject*>(static_cast<const void*>(a_tracked.geometry.get()))))
						Diagnostics::Notify(Diagnostics::RootViewHidden{ *this, a_tracked.geometry.get(), a_tracked.categoryNode, FirstViewRoot(), sameRoot, key, depth,
							(record->flags & static_cast<std::uint32_t>(RE::NiAVObject::Flag::kHidden)) != 0, globals.cullHidden });
				}
				return Ineligible::Hidden;
			}
			if (key == a_tracked.categoryNode)
				break;
			if (underBillboard && (record->kind & SceneCapture::kKindSwitch) && !switchLeaves())
				return Ineligible::Switch;
			if (chainTests && child && (record->kind & SceneCapture::kKindSwitch) && !SwitchSelects(*record, child))
				return Ineligible::Switch;
			if (chainTests && (record->kind & SceneCapture::kKindOrdered) && orderedLeaf())
				return Ineligible::UnsupportedParent;
			if (!actors && record->userData && record->actor)
				return Ineligible::Actor;
		}


		return Ineligible::None;
	}

}
