// The visibility readback. VisibilityEventsCS reports main-camera visibility changes as GPU events.
// The depth commit arms and harvests them on the render thread, and a task on DCLF's executor decodes them into verdicts.
// The main thread then writes them as kAccumulated at Main::Update's entry, on the roots DCLF lists and their geometry.
// See dclf-architecture.md, "The visibility events".

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/EntrySpaceTable.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"
#include "RenderGraph/Fatal.h"
#include "Snapshot/EngineReads.h"

namespace DCLF
{
	using namespace Draws;

	namespace
	{
		using Readback = VisibilityReadback;

		// Lock-free stack of CPU batches. Any thread pushes, and the frame start takes it whole.
		std::atomic<Readback::Batch*> cpuVisibility{ nullptr };

		inline constexpr std::uint32_t kUnlisted = ~0u;
		// An object's share in its plain entry.
		inline constexpr std::uint8_t kShareListed = 1, kShareVisible = 2, kShareResidue = 4;

		/** @brief The decode task's state. A pass visits only slots that can have changed, plus the objects those changes reach.
		 * Only a reset, a camera cut or a refresh visits everything. */
		struct VisibilityDecode
		{
			struct Expiry
			{
				std::uint32_t frame = 0;  // the frame it went out of view
				std::uint32_t slot = 0;
				bool root = false;
				bool operator>(const Expiry& a_other) const { return frame > a_other.frame; }
			};
			MarkedList roots, objects, entries;
			std::vector<Expiry> expiries;        // a min-heap by frame
			LogCursor changes;                   // tables.changeLog
			std::uint64_t rootEpoch = 0;         // root states' journal epoch the root classes were judged at
			bool rootsHeld = false;
			std::uint32_t cutVisited = 0;        // the cut whose pass visited everything
			std::shared_ptr<const EntrySpace> candidates;  // the snapshot entryMembers indexes into
			// Per root slot: class as last judged (0 not written, 1 stood in, 2 wide), node, and the object slots under it.
			std::vector<std::uint8_t> rootClass;
			std::vector<const void*> rootNode;
			std::vector<std::vector<std::uint32_t>> rootMembers;
			// Per object slot: its root and plain entry with its place in each list, its share in the entry, and its last judged occupant.
			std::vector<std::uint32_t> objectRoot, objectRootAt, objectEntry, objectEntryAt;
			std::vector<std::uint8_t> objectShare;
			std::vector<const void*> objectGeometry;
			ankerl::unordered_dense::map<const void*, std::uint32_t> geometrySlot;
			// Per plain entry: its object slots, and how many count as listed, in view, or engine residue.
			std::vector<std::vector<std::uint32_t>> entryMembers;
			std::vector<std::uint32_t> entryListed, entryVisible, entryResidue;
			// CS_DCLF_STATS counters since the last line.
			std::uint64_t serial = 0, passes = 0, prepareNs = 0, visits = 0, wholePasses = 0;
		};

		/** @brief The decode task's state, made on first use. Only the decode task touches it. */
		VisibilityDecode& DecodeOf(Readback& a_readback)
		{
			if (!a_readback.decodeState)
				a_readback.decodeState = std::make_shared<VisibilityDecode>();
			return *static_cast<VisibilityDecode*>(a_readback.decodeState.get());
		}

		/** @brief Clears the verdicts on a reset frame, when the reported words get a new empty backing. */
		void StartAgain(Readback& a_readback, VisibilityDecode& a_decode)
		{
			a_readback.roots.assign(a_readback.roots.size(), {});
			a_readback.objects.assign(a_readback.objects.size(), {});
			a_readback.rootApplied.assign(a_readback.rootApplied.size(), 0u);
			a_readback.objectApplied.assign(a_readback.objectApplied.size(), 0u);
			a_readback.decodedFrame = a_readback.previousDecodedFrame = 0;
			a_decode.expiries.clear();
		}

		/** @brief Judge a_slot again once the window reaches a_frame, the frame it went out of view. */
		void Expire(VisibilityDecode& a_decode, std::uint32_t a_frame, std::uint32_t a_slot, bool a_root)
		{
			a_decode.expiries.push_back({ a_frame, a_slot, a_root });
			std::push_heap(a_decode.expiries.begin(), a_decode.expiries.end(), std::greater<>{});
		}

		/** @brief Applies one batch to the verdicts, against its frame's tables. Each slot it touches is visited. */
		void Apply(Readback& a_readback, VisibilityDecode& a_decode, const Readback::Batch& a_batch)
		{
			const auto& tables = *a_batch.tables;
			const auto rootList = Extensions::RootListOf(tables);
			if (a_batch.reset)
				StartAgain(a_readback, a_decode);
			if (a_readback.roots.size() < rootList.rows.size()) {
				a_readback.roots.resize(rootList.rows.size());
				a_readback.rootApplied.resize(rootList.rows.size(), 0u);
			}
			if (a_readback.objects.size() < tables.objects.size()) {
				a_readback.objects.resize(tables.objects.size());
				a_readback.objectApplied.resize(tables.objects.size(), 0u);
			}
			const std::uint32_t frame = a_batch.frame;
			std::uint64_t stale = 0;
			for (const VisibilityEvent& event : a_batch.events) {
				const bool visible = (event.bits & kVisibilityBitVisible) != 0;
				if (event.kind == kVisibilityKindRoot) {
					// Must match its listing in its frame's tables and be no older than the last applied event.
					if (event.slot >= rootList.rows.size() || rootList.rows[event.slot].generation != event.generation || !rootList.nodes[event.slot] ||
						frame < a_readback.rootApplied[event.slot]) {
						++stale;
						continue;
					}
					a_readback.rootApplied[event.slot] = frame;
					auto& root = a_readback.roots[event.slot];
					root.generation = event.generation;
					root.known = (event.bits & kVisibilityBitVerdict) != 0;
					if (root.visible && !visible) {
						root.outFrame = frame;
						Expire(a_decode, frame, event.slot, true);
					}
					root.visible = visible;
					if (visible)
						root.lastFrame = frame;
					a_decode.roots.Add(event.slot);
				} else {
					if (event.slot >= tables.objects.size() || !tables.objectGeometry[event.slot] || frame < a_readback.objectApplied[event.slot]) {
						++stale;
						continue;
					}
					a_readback.objectApplied[event.slot] = frame;
					auto& object = a_readback.objects[event.slot];
					if (object.visible && !visible) {
						object.outFrame = frame;
						Expire(a_decode, frame, event.slot, false);
					}
					object.visible = visible;
					if (visible)
						object.lastFrame = frame;
					a_decode.objects.Add(event.slot);
				}
			}
			if (frame > a_readback.decodedFrame) {
				a_readback.previousDecodedFrame = a_readback.decodedFrame;
				a_readback.decodedFrame = frame;
			}
			a_readback.batches.fetch_add(1, std::memory_order_relaxed);
			a_readback.events.fetch_add(a_batch.events.size(), std::memory_order_relaxed);
			a_readback.stale.fetch_add(stale, std::memory_order_relaxed);
		}

		struct Pass
		{
			Readback& readback;
			VisibilityDecode& decode;
			const SceneTables& tables;
			const Api::RootListView& rootList;
			const EntrySpace* candidates;
			std::uint32_t entries;      // the candidates' index space
			std::size_t roots, objects;
			std::uint32_t window;        // the older of the last two decoded frames
			bool cutPending, full;
			Readback::Writes& list;
		};

		/** @brief Takes a_slot out of its owner's list. a_of holds each slot's owner and a_at its place there. */
		void Unlist(std::vector<std::vector<std::uint32_t>>& a_lists, std::vector<std::uint32_t>& a_of, std::vector<std::uint32_t>& a_at, std::uint32_t a_slot)
		{
			const std::uint32_t owner = a_of[a_slot];
			if (owner == kUnlisted)
				return;
			auto& list = a_lists[owner];
			const std::uint32_t at = a_at[a_slot];
			list[at] = list.back();
			a_at[list[at]] = at;
			list.pop_back();
			a_of[a_slot] = kUnlisted;
		}

		void List(std::vector<std::vector<std::uint32_t>>& a_lists, std::vector<std::uint32_t>& a_of, std::vector<std::uint32_t>& a_at, std::uint32_t a_slot,
			std::uint32_t a_owner)
		{
			if (a_lists.size() <= a_owner)
				a_lists.resize(std::size_t(a_owner) + 1);
			a_of[a_slot] = a_owner;
			a_at[a_slot] = static_cast<std::uint32_t>(a_lists[a_owner].size());
			a_lists[a_owner].push_back(a_slot);
		}

		void SizeRoots(VisibilityDecode& a_decode, std::size_t a_count)
		{
			if (a_decode.rootClass.size() >= a_count)
				return;
			a_decode.rootClass.resize(a_count, 0u);
			a_decode.rootNode.resize(a_count, nullptr);
			if (a_decode.rootMembers.size() < a_count)
				a_decode.rootMembers.resize(a_count);
		}

		void SizeObjects(VisibilityDecode& a_decode, std::size_t a_count)
		{
			if (a_decode.objectRoot.size() >= a_count)
				return;
			a_decode.objectRoot.resize(a_count, kUnlisted);
			a_decode.objectRootAt.resize(a_count, 0u);
			a_decode.objectEntry.resize(a_count, kUnlisted);
			a_decode.objectEntryAt.resize(a_count, 0u);
			a_decode.objectShare.resize(a_count, 0u);
			a_decode.objectGeometry.resize(a_count, nullptr);
		}

		void SizeEntries(VisibilityDecode& a_decode, std::size_t a_count)
		{
			if (a_decode.entryListed.size() >= a_count)
				return;
			a_decode.entryListed.resize(a_count, 0u);
			a_decode.entryVisible.resize(a_count, 0u);
			a_decode.entryResidue.resize(a_count, 0u);
			if (a_decode.entryMembers.size() < a_count)
				a_decode.entryMembers.resize(a_count);
		}

		/** @brief Sets an object's share in its plain entry. The entry is visited when its counts move. */
		void Share(VisibilityDecode& a_decode, std::uint32_t a_object, std::uint8_t a_share)
		{
			const std::uint8_t previous = a_decode.objectShare[a_object];
			if (previous == a_share)
				return;
			a_decode.objectShare[a_object] = a_share;
			const std::uint32_t entry = a_decode.objectEntry[a_object];
			if (entry == kUnlisted)
				return;
			const auto count = [&](std::uint8_t a_bits, bool a_add) {
				const auto step = [&](std::uint32_t& a_counter) { a_counter = a_add ? a_counter + 1 : a_counter - 1; };
				if (a_bits & kShareListed)
					step(a_decode.entryListed[entry]);
				if (a_bits & kShareVisible)
					step(a_decode.entryVisible[entry]);
				if (a_bits & kShareResidue)
					step(a_decode.entryResidue[entry]);
			};
			count(previous, false);
			count(a_share, true);
			a_decode.entries.Add(entry);
		}

		/** @brief Writes a root slot when it differs from the last ask. A changed class or node visits the objects under it. */
		void JudgeRoot(Pass& a_pass, std::uint32_t a_root)
		{
			auto& readback = a_pass.readback;
			auto& decode = a_pass.decode;
			SizeRoots(decode, std::size_t(a_root) + 1);
			const auto& rootList = a_pass.rootList;
			const void* node = a_root < a_pass.roots ? rootList.nodes[a_root] : nullptr;
			std::uint8_t rootClass = 0;
			if (!node || a_root >= rootList.rows.size()) {
				// Forget the last write so the slot writes again if it returns.
				node = nullptr;
				if (a_root < readback.rootWritten.size())
					readback.rootWritten[a_root] = Readback::kWrittenNone;
			} else {
				const auto& row = rootList.rows[a_root];
				const bool stoodIn = (row.bits & kFadeRootStoodIn) != 0;
				rootClass = stoodIn ? 1 : 2;
				bool visible = a_pass.cutPending;
				const bool listed = a_root < readback.roots.size() && readback.roots[a_root].generation == row.generation && readback.roots[a_root].known;
				if (!visible)
					visible = !listed || readback.roots[a_root].visible || readback.roots[a_root].outFrame > a_pass.window;
				const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
				const std::uint8_t wideBit = stoodIn ? 0 : 1;
				if (a_pass.full || readback.rootWritten[a_root] != wanted || readback.rootWrittenGeneration[a_root] != row.generation ||
					readback.rootWrittenWide[a_root] != wideBit) {
					readback.rootWritten[a_root] = wanted;
					readback.rootWrittenGeneration[a_root] = row.generation;
					readback.rootWrittenWide[a_root] = wideBit;
					a_pass.list.writes.push_back({ const_cast<void*>(node), node, visible, Readback::kWriteRoot, !stoodIn });
				}
			}
			if (rootClass == decode.rootClass[a_root] && node == decode.rootNode[a_root])
				return;
			// Its geometry takes its class. Listing or unlisting a node as a fade root also flips its plain entry's geometry.
			for (const std::uint32_t object : decode.rootMembers[a_root])
				decode.objects.Add(object);
			if (a_pass.candidates)
				for (const void* changed : { decode.rootNode[a_root], node })
					if (const auto it = changed ? a_pass.candidates->entries.find(static_cast<const RE::NiAVObject*>(changed)) : a_pass.candidates->entries.end();
						it != a_pass.candidates->entries.end() && it->second < decode.entryMembers.size())
						for (const std::uint32_t object : decode.entryMembers[it->second])
							decode.objects.Add(object);
			decode.rootClass[a_root] = rootClass;
			decode.rootNode[a_root] = node;
		}

		/** @brief Indexes an object slot's geometry by node and records its place under its root. */
		inline void IndexObject(VisibilityDecode& a_decode, std::uint32_t a_object, RE::BSGeometry* a_geometry, bool a_drawn, std::uint32_t a_root)
		{
			auto& decode = a_decode;
			auto* geometry = a_geometry;
			const std::uint32_t root = a_root;
			if (decode.objectGeometry[a_object] != geometry) {
				if (const auto it = decode.geometrySlot.find(decode.objectGeometry[a_object]); it != decode.geometrySlot.end() && it->second == a_object)
					decode.geometrySlot.erase(it);
				if (geometry)
					decode.geometrySlot.insert_or_assign(geometry, a_object);
				decode.objectGeometry[a_object] = geometry;
			}
			if (const std::uint32_t memberOf = a_drawn && root != kNoFadeRoot ? root : kUnlisted; memberOf != decode.objectRoot[a_object]) {
				Unlist(decode.rootMembers, decode.objectRoot, decode.objectRootAt, a_object);
				if (memberOf != kUnlisted) {
					SizeRoots(decode, std::size_t(memberOf) + 1);
					List(decode.rootMembers, decode.objectRoot, decode.objectRootAt, a_object, memberOf);
				}
			}
		}

		/** @brief A drawn object's class (1 stood in, 2 wide) and the root it writes through. */
		struct ObjectClass
		{
			std::uint8_t objectClass = 0;
			const void* rootNode = nullptr;
			std::uint32_t entry = EntrySpace::kNone, entryOf = kUnlisted;
		};

		inline ObjectClass ClassifyObject(Pass& a_pass, std::uint32_t a_object, RE::BSGeometry* a_geometry, bool a_drawn, std::uint32_t a_root)
		{
			auto& readback = a_pass.readback;
			const auto& decode = a_pass.decode;
			const auto& rootList = a_pass.rootList;
			auto* geometry = a_geometry;
			const std::uint32_t root = a_root;
			ObjectClass out;
			auto& objectClass = out.objectClass;
			auto& rootNode = out.rootNode;
			auto& entry = out.entry;
			auto& entryOf = out.entryOf;
			const auto* candidates = a_pass.candidates;
			if (a_drawn) {
				if (root != kNoFadeRoot && root < a_pass.roots && root < decode.rootClass.size() && decode.rootClass[root]) {
					objectClass = decode.rootClass[root];
					rootNode = rootList.nodes[root];
				} else if (root == kNoFadeRoot && candidates) {
					if (readback.objectEntryGeometry[a_object] != geometry) {
						readback.objectEntryGeometry[a_object] = geometry;
						readback.objectEntry[a_object] = EntrySpace::kNone;
						if (const auto it = candidates->geometries.find(geometry); it != candidates->geometries.end() && it->second < candidates->geometryEntry.size())
							readback.objectEntry[a_object] = candidates->geometryEntry[it->second];
					}
					entry = readback.objectEntry[a_object];
					if (entry < a_pass.entries && candidates->entryNodes[entry]) {
						entryOf = entry;
						if (!rootList.Contains(candidates->entryNodes[entry])) {
							objectClass = 2;
							rootNode = candidates->entryNodes[entry];
						} else {
							entry = EntrySpace::kNone;
						}
					} else {
						entry = EntrySpace::kNone;
					}
				}
			}
			return out;
		}

		/** @brief Moves an object slot to its plain entry's list. A move takes its share out first. */
		inline void ListUnderEntry(Pass& a_pass, std::uint32_t a_object, std::uint32_t a_entryOf)
		{
			auto& readback = a_pass.readback;
			auto& decode = a_pass.decode;
			const std::uint32_t entryOf = a_entryOf;
			// The cached entry holds only while the object is listed under it.
			if (entryOf == kUnlisted && a_object < readback.objectEntryGeometry.size())
				readback.objectEntryGeometry[a_object] = nullptr;
			if (entryOf != decode.objectEntry[a_object]) {
				Share(decode, a_object, 0);
				Unlist(decode.entryMembers, decode.objectEntry, decode.objectEntryAt, a_object);
				if (entryOf != kUnlisted) {
					SizeEntries(decode, std::size_t(entryOf) + 1);
					List(decode.entryMembers, decode.objectEntry, decode.objectEntryAt, a_object, entryOf);
				}
			}
		}

		/** @brief Writes an object slot when it differs from the last ask, and lists it under its root and plain entry. */
		void JudgeObject(Pass& a_pass, std::uint32_t a_object)
		{
			auto& readback = a_pass.readback;
			auto& decode = a_pass.decode;
			const auto& tables = a_pass.tables;
			SizeObjects(decode, std::size_t(a_object) + 1);
			const bool held = a_object < a_pass.objects;
			auto* geometry = held ? tables.objectGeometry[a_object] : nullptr;
			const bool drawn = geometry && !tables.IsLayer(a_object);
			const auto& rootList = a_pass.rootList;
			const std::uint32_t root = held ? rootList.RootOf(a_object) : kNoFadeRoot;
			IndexObject(decode, a_object, geometry, drawn, root);
			auto [objectClass, rootNode, entry, entryOf] = ClassifyObject(a_pass, a_object, geometry, drawn, root);
			ListUnderEntry(a_pass, a_object, entryOf);
			// A wide geometry DCLF does not draw in the main view, such as engine residue or shadow-only, gets no verdict.
			// Its bit stays the engine cull's.
			std::uint8_t share = 0;
			if (objectClass == 2 && a_object < tables.objects.size() &&
				(!(tables.objects[a_object].flags & kObjectMember) || (tables.objects[a_object].flags & (kObjectNoBindings | kObjectShadowOnly | kObjectFree)))) {
				if (entry != EntrySpace::kNone)
					share = kShareResidue;
				objectClass = 0;
			}
			if (!objectClass) {
				Share(decode, a_object, share);
				if (a_object < readback.objectWrittenGeometry.size())
					readback.objectWrittenGeometry[a_object] = nullptr;
				return;
			}
			bool visible = a_pass.cutPending;
			if (!visible && a_object < readback.objects.size())
				visible = readback.objects[a_object].visible || readback.objects[a_object].outFrame > a_pass.window;
			if (entry != EntrySpace::kNone)
				share = visible ? kShareListed | kShareVisible : kShareListed;
			Share(decode, a_object, share);
			const std::uint8_t wideBit = objectClass == 2 ? 1 : 0;
			const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
			if (!a_pass.full && readback.objectWritten[a_object] == wanted && readback.objectWrittenGeometry[a_object] == geometry &&
				readback.objectWrittenWide[a_object] == wideBit)
				return;
			readback.objectWritten[a_object] = wanted;
			readback.objectWrittenGeometry[a_object] = geometry;
			readback.objectWrittenWide[a_object] = wideBit;
			a_pass.list.writes.push_back({ geometry, rootNode, visible, Readback::kWriteGeometry, wideBit != 0 });
		}

		/** @brief A plain entry root is visible when any counted geometry is, and unwritten while one is engine residue. */
		void JudgeEntry(Pass& a_pass, std::uint32_t a_entry)
		{
			if (a_entry >= a_pass.entries)
				return;
			auto& readback = a_pass.readback;
			const auto& decode = a_pass.decode;
			const bool counted = a_entry < decode.entryListed.size();
			const std::uint32_t listed = counted ? decode.entryListed[a_entry] : 0u, inView = counted ? decode.entryVisible[a_entry] : 0u;
			if (!listed || (counted && decode.entryResidue[a_entry])) {
				readback.entryWritten[a_entry] = Readback::kWrittenNone;
				return;
			}
			const bool visible = a_pass.cutPending || inView;
			const std::uint8_t wanted = visible ? Readback::kWrittenSet : Readback::kWrittenClear;
			const auto* candidates = a_pass.candidates;
			const std::uint32_t version = a_entry < candidates->entryVersion.size() ? candidates->entryVersion[a_entry] : 0u;
			if (!a_pass.full && readback.entryWritten[a_entry] == wanted && readback.entryWrittenVersion[a_entry] == version)
				return;
			readback.entryWritten[a_entry] = wanted;
			readback.entryWrittenVersion[a_entry] = version;
			const void* node = candidates->entryNodes[a_entry];
			a_pass.list.writes.push_back({ const_cast<void*>(node), node, visible, Readback::kWriteEntry, true });
		}

		/** @brief Visits the object slots changed since the last pass. Returns whether the pass must visit everything. */
		bool VisitChangedObjects(VisibilityDecode& a_decode, const SceneTables& a_tables, bool a_full, std::uint32_t a_cut, bool a_cutPending)
		{
			const auto& tables = a_tables;
			const std::uint32_t cut = a_cut;
			bool everything = a_full;
			// The log is invalidated whenever the slots are cleared, so the cursor can follow the log alone.
			if (!a_decode.changes.Continues(tables.changeLog, 0u)) {
				everything = true;
				a_decode.changes.Restart(0u);
			} else {
				for (const auto& change : a_decode.changes.Unread(tables.changeLog))
					a_decode.objects.Add(change.slot);
			}
			a_decode.changes.Advance(tables.changeLog);
			if (a_cutPending && a_decode.cutVisited != cut) {
				a_decode.cutVisited = cut;
				everything = true;
			}
			return everything;
		}

		/** @brief Visits the plain entries the candidates moved, with their members before and after. */
		void VisitMovedEntries(Readback& a_readback, VisibilityDecode& a_decode, const Readback::Batch& a_newest, std::uint32_t a_entries, bool& a_everything)
		{
			const EntrySpace* candidates = a_newest.candidates.get();
			const std::uint32_t entries = a_entries;
			bool& everything = a_everything;
			if (candidates != a_readback.entryCandidates) {
				a_readback.entryCandidates = candidates;
				if (!candidates || !a_decode.candidates) {
					everything = true;
				} else if (!everything) {
					std::vector<std::uint32_t> moved;
					ChangedEntriesByLog(a_decode.candidates.get(), *candidates, moved);
					const auto forget = [&](std::uint32_t a_object) {
						a_decode.objects.Add(a_object);
						if (a_object < a_readback.objectEntryGeometry.size())
							a_readback.objectEntryGeometry[a_object] = nullptr;
					};
					for (const std::uint32_t e : moved) {
						a_decode.entries.Add(e);
						if (e < a_decode.entryMembers.size())
							for (const std::uint32_t object : a_decode.entryMembers[e])
								forget(object);
						if (e < entries)
							for (const std::uint32_t g : candidates->entryGeometries[e]) {
								if (g < candidates->geometrySlot.size() && candidates->geometrySlot[g] >= 0)
									forget(static_cast<std::uint32_t>(candidates->geometrySlot[g]));
								if (const auto it = g < candidates->geometryNodes.size() ? a_decode.geometrySlot.find(candidates->geometryNodes[g]) : a_decode.geometrySlot.end();
									it != a_decode.geometrySlot.end())
									forget(it->second);
							}
					}
				}
				if (everything) {
					a_readback.objectEntry.assign(a_readback.objectEntry.size(), EntrySpace::kNone);
					a_readback.objectEntryGeometry.assign(a_readback.objectEntryGeometry.size(), nullptr);
				}
				a_decode.candidates = a_newest.candidates;
			}
		}

		/** @brief Visits the root rows the journal marked since the classes were judged. */
		void VisitMarkedRoots(VisibilityDecode& a_decode, const SceneTables& a_tables, const Api::RootListView& a_rootList, std::size_t a_roots, bool& a_everything)
		{
			bool& everything = a_everything;
			const std::size_t roots = a_roots;
			if (everything || !a_decode.rootsHeld) {
				everything = true;
			} else if (const auto* family = RootFamilyOf(a_tables)) {
				family->Journal().Take().ForEachRun(a_decode.rootEpoch, roots, [&](std::uint64_t a_first, std::uint64_t a_count) {
					for (std::uint64_t r = a_first; r < a_first + a_count; ++r)
						a_decode.roots.Add(static_cast<std::uint32_t>(r));
				});
			}
			a_decode.rootEpoch = a_rootList.contentEpoch;
			a_decode.rootsHeld = true;
		}

		void VisitExpired(VisibilityDecode& a_decode, std::uint32_t a_window)
		{
			const std::uint32_t window = a_window;
			while (!a_decode.expiries.empty() && a_decode.expiries.front().frame <= window) {
				std::pop_heap(a_decode.expiries.begin(), a_decode.expiries.end(), std::greater<>{});
				const auto expiry = a_decode.expiries.back();
				a_decode.expiries.pop_back();
				(expiry.root ? a_decode.roots : a_decode.objects).Add(expiry.slot);
			}
		}

		/** @brief Every root, object and plain entry slot, including those only the decoder still has state for. */
		void VisitEverything(VisibilityDecode& a_decode, std::size_t a_roots, std::size_t a_objects, std::uint32_t a_entries)
		{
			for (std::size_t r = 0, end = std::max(a_roots, a_decode.rootClass.size()); r < end; ++r)
				a_decode.roots.Add(static_cast<std::uint32_t>(r));
			for (std::size_t o = 0, end = std::max(a_objects, a_decode.objectRoot.size()); o < end; ++o)
				a_decode.objects.Add(static_cast<std::uint32_t>(o));
			for (std::size_t e = 0, end = std::max<std::size_t>(a_entries, a_decode.entryListed.size()); e < end; ++e)
				a_decode.entries.Add(static_cast<std::uint32_t>(e));
		}

		/** @brief Logs the decode line every 300 passes (CS_DCLF_STATS) and resets its counters. */
		void ReportDecode(VisibilityDecode& a_decode)
		{
			if (++a_decode.passes >= 300) {
				if (Diagnostics::Observe().stats)
					logger::info("[DCLF] accumulated writes decode (A5): {} passes, {:.1f} us preparing ({:.2f} us a pass), {} slots visited ({:.1f} a pass), {} whole",
						a_decode.passes, double(a_decode.prepareNs) / 1000.0, double(a_decode.prepareNs) / 1000.0 / double(a_decode.passes), a_decode.visits,
						double(a_decode.visits) / double(a_decode.passes), a_decode.wholePasses);
				a_decode.passes = a_decode.prepareNs = a_decode.visits = a_decode.wholePasses = 0;
			}
		}

		/** @brief The kAccumulated writes that differ from the last ask, or all of them on a full write. Null when there is nothing to write.
		 * A slot is visible if in view in either of the last two decoded frames, if it has no verdict yet, or during an unanswered camera cut. */
		Readback::Writes* PrepareWrites(Readback& a_readback, VisibilityDecode& a_decode, const Readback::Batch& a_newest, bool a_reset)
		{
			DCLF_ZONE("CS.DCLF.VisibilityPrepare");
			const auto start = std::chrono::steady_clock::now();
			const auto& tables = *a_newest.tables;
			const std::uint32_t cut = a_readback.cutFrame.load(std::memory_order_acquire);
			const bool cutPending = cut && a_readback.decodedFrame < cut;
			// The first batch at or after a cut rewrites everything. The main thread set every root at the cut and held back older clears.
			const bool cutAnswered = cut && !cutPending && a_readback.cutAnswered != cut;
			const bool full = a_reset || cutAnswered || a_readback.refreshWanted.exchange(false, std::memory_order_acq_rel);
			if (cutAnswered)
				a_readback.cutAnswered = cut;
			const auto rootList = Extensions::RootListOf(tables);
			const std::size_t roots = rootList.nodes.size(), objects = tables.objectGeometry.size();
			if (a_readback.rootWritten.size() < roots) {
				a_readback.rootWritten.resize(roots, Readback::kWrittenNone);
				a_readback.rootWrittenGeneration.resize(roots, 0u);
				a_readback.rootWrittenWide.resize(roots, 0u);
			}
			if (a_readback.objectWritten.size() < objects) {
				a_readback.objectWritten.resize(objects, Readback::kWrittenNone);
				a_readback.objectWrittenGeometry.resize(objects, nullptr);
				a_readback.objectWrittenWide.resize(objects, 0u);
			}
			const EntrySpace* candidates = a_newest.candidates.get();
			const std::uint32_t entries = candidates ? candidates->Capacity() : 0u;
			++a_decode.serial;
			bool everything = VisitChangedObjects(a_decode, tables, full, cut, cutPending);
			VisitMovedEntries(a_readback, a_decode, a_newest, entries, everything);
			if (a_readback.objectEntry.size() < objects) {
				a_readback.objectEntry.resize(objects, EntrySpace::kNone);
				a_readback.objectEntryGeometry.resize(objects, nullptr);
			}
			if (a_readback.entryWritten.size() < entries) {
				a_readback.entryWritten.resize(entries, Readback::kWrittenNone);
				a_readback.entryWrittenVersion.resize(entries, 0u);
			}
			VisitMarkedRoots(a_decode, tables, rootList, roots, everything);
			const std::uint32_t window = a_readback.previousDecodedFrame;
			VisitExpired(a_decode, window);
			if (everything)
				VisitEverything(a_decode, roots, objects, entries);
			auto* list = new Readback::Writes();
			list->frame = a_newest.frame;
			list->full = full;
			Pass pass{ a_readback, a_decode, tables, rootList, candidates, entries, roots, objects, window, cutPending, full, *list };
			// Roots first, since objects take their class. Objects next, since entries count their shares.
			for (const std::uint32_t r : a_decode.roots.list)
				JudgeRoot(pass, r);
			for (std::size_t k = 0; k < a_decode.objects.list.size(); ++k)
				JudgeObject(pass, a_decode.objects.list[k]);
			for (const std::uint32_t e : a_decode.entries.list)
				JudgeEntry(pass, e);
			a_decode.visits += a_decode.roots.Size() + a_decode.objects.Size() + a_decode.entries.Size();
			a_decode.wholePasses += everything ? 1 : 0;
			a_decode.roots.Clear();
			a_decode.objects.Clear();
			a_decode.entries.Clear();
			a_decode.prepareNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
			ReportDecode(a_decode);
			if (list->writes.empty()) {
				delete list;
				return nullptr;
			}
			a_readback.fullWrites.fetch_add(full ? 1 : 0, std::memory_order_relaxed);
			list->tables = a_newest.tables;
			list->publication = a_newest.publication;
			list->retirement = a_newest.retirement;
			if (candidates)
				list->candidates = a_newest.candidates;
			return list;
		}

		using Engine::kFlagAccumulated;
		using Engine::kFlagHidden;
		using Engine::kObjectFlags;
		using Engine::FlagsOf;

		/** @brief Main thread. Sets or clears one node's kAccumulated atomically, skipping the write when it is already right. */
		void Store(Readback& a_readback, void* a_node, bool a_set, bool a_wide = false)
		{
			std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(a_node) + kObjectFlags));
			if (((flags.load(std::memory_order_relaxed) & kFlagAccumulated) != 0) == a_set) {
				(a_wide ? a_readback.wideUnchanged : a_readback.writesUnchanged).fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (a_set) {
				flags.fetch_or(kFlagAccumulated, std::memory_order_relaxed);
				(a_wide ? a_readback.wideSet : a_readback.writesSet).fetch_add(1, std::memory_order_relaxed);
			} else {
				flags.fetch_and(~kFlagAccumulated, std::memory_order_relaxed);
				(a_wide ? a_readback.wideCleared : a_readback.writesCleared).fetch_add(1, std::memory_order_relaxed);
			}
		}

		/** @brief Main thread. Sets kAccumulated on the nodes between a visible geometry and its root, since receivers test every level.
		 * Set only, as a stale bit above an out-of-view geometry is harmless. Written only if the chain reaches the root. */
		void SetChain(Readback& a_readback, RE::NiAVObject* a_geometry, const void* a_root)
		{
			auto& chain = a_readback.chain;
			chain.clear();
			RE::NiAVObject* node = a_geometry->parent;
			for (; node && node != a_root; node = node->parent)
				chain.push_back(node);
			if (!node) {
				a_readback.wideNoChain.fetch_add(1, std::memory_order_relaxed);
				if (a_readback.wideNoChainFirst.size() < 300) {
					const auto* root = static_cast<const RE::NiAVObject*>(a_root);
					const auto* top = chain.empty() ? a_geometry : chain.back();
					a_readback.wideNoChainFirst += fmt::format("{}'{}' under '{}' (top '{}')", a_readback.wideNoChainFirst.empty() ? "" : ", ",
						a_geometry->name.c_str() ? a_geometry->name.c_str() : "?", root && root->name.c_str() ? root->name.c_str() : "?",
						top->name.c_str() ? top->name.c_str() : "?");
				}
				return;
			}
			for (auto* between : chain) {
				std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::byte*>(between) + kObjectFlags));
				if (!(flags.load(std::memory_order_relaxed) & kFlagAccumulated)) {
					flags.fetch_or(kFlagAccumulated, std::memory_order_relaxed);
					a_readback.wideNodesSet.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}

		/** @brief Main thread. Sets kAccumulated on the object root's non-hidden children, as DrawWorld_BuildSceneLists does each frame. */
		void CellRoots(Readback& a_readback)
		{
			const auto* sceneNode = CS_ENGINE_READ(ShaderManagerState, RecordTime, globals::game::smState) ? CS_ENGINE_READ(ShaderManagerState, RecordTime, globals::game::smState)->shadowSceneNode[0] : nullptr;
			if (!sceneNode)
				return;
			auto* root = Engine::ObjectRoot(*sceneNode);
			auto* objectRoot = root ? root->AsNode() : nullptr;
			if (!objectRoot || Engine::IsHidden(objectRoot))
				return;
			// Matches DrawWorld_BuildSceneLists (AE 0x14064bc20). In an exterior it sets every non-hidden child from the third on.
			// Children 0 and 1 are list entries whose bit the list cull owns. In an interior only the cell's 3D is a cell root.
			const auto* tes = CS_ENGINE_READ(TES, RecordTime, RE::TES::GetSingleton());
			auto* interior = tes ? tes->interiorCell : nullptr;
			const auto* loaded = interior ? interior->GetRuntimeData().loadedData : nullptr;
			const RE::NiAVObject* interior3D = loaded ? loaded->cell3D.get() : nullptr;
			std::uint32_t index = 0;
			for (const auto& child : Engine::Children(*objectRoot)) {
				const bool container = interior ? child.get() != interior3D : index < 2;
				++index;
				auto* node = child ? child->AsNode() : nullptr;
				if (!node || node->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					continue;
				if (!container)
					a_readback.cellRootsSeen.fetch_add(1, std::memory_order_relaxed);
				std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::byte*>(node) + kObjectFlags));
				if (flags.load(std::memory_order_relaxed) & kFlagAccumulated)
					continue;
				flags.fetch_or(kFlagAccumulated, std::memory_order_relaxed);
				a_readback.cellRootsSet.fetch_add(1, std::memory_order_relaxed);
			}
		}

		/** @brief Render thread. Frees a decoded batch and its tables. */
		void Release(Readback::Batch* a_batch)
		{
			delete a_batch;
		}

		/** @brief Render thread, at a region's depth commit. Copies the region's last events into a batch if its copy was found.
		 * That run's lease retired, and the next one is not submitted yet. */
		void Harvest(Readback& a_readback, const org::RunReadback::ListRead& a_read)
		{
			if (a_read.region >= a_readback.armed.size())
				return;
			auto& armed = a_readback.armed[a_read.region];
			if (!armed.armed)
				return;
			armed.armed = false;
			// Events past the list's end are appended again next frame, and the list grows.
			if (a_read.words) {
				auto* batch = new Readback::Batch();
				batch->frame = armed.frame;
				batch->reset = armed.reset || std::exchange(a_readback.carryReset, false);
				batch->tables = std::move(armed.tables);
				batch->publication = armed.publication;
				batch->retirement = std::move(armed.retirement);
				batch->candidates = std::move(armed.candidates);
				batch->complete = a_read.count <= a_read.capacity;
				const auto* list = reinterpret_cast<const VisibilityEvent*>(a_read.words + kVisibilityEventHeaderWords);
				batch->events.assign(list, list + std::min(a_read.count, a_read.capacity));
				a_readback.pending.push_back(batch);
				return;
			}
			// No readback ran for that frame, so the GPU reports its changes next frame. A carried reset goes with the next batch.
			a_readback.carryReset = a_readback.carryReset || armed.reset;
			armed.tables.reset();
			armed.candidates.reset();
			armed.retirement.reset();
		}
	}

	namespace
	{
		/** @brief Render thread. Releases the last task's decoded batches. */
		void ReleaseDecoded(Readback& a_readback)
		{
			auto* batch = a_readback.decoded.exchange(nullptr, std::memory_order_acquire);
			while (batch) {
				auto* following = batch->next;
				Release(batch);
				batch = following;
			}
		}

		/** @brief Render thread. Hands the harvested batches to one decode task on DCLF's executor. Only one runs at a time. */
		void Dispatch(const std::shared_ptr<Readback>& a_readback)
		{
			auto& readback = *a_readback;
			if (bool idle = false; !readback.running.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
				readback.busy.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (readback.pending.empty()) {
				readback.running.store(false, std::memory_order_release);
				readback.running.notify_all();
				return;
			}
			auto batches = std::exchange(readback.pending, {});
			std::stable_sort(batches.begin(), batches.end(), [](const auto* a_a, const auto* a_b) { return a_a->frame < a_b->frame; });
			auto decode = [readback = a_readback, batches = std::move(batches)] {
				DCLF_ZONE("CS.DCLF.VisibilityDecode");
				auto& state = DecodeOf(*readback);
				const auto start = std::chrono::steady_clock::now();
				bool reset = false;
				for (auto* batch : batches) {
					reset = reset || batch->reset;
					Apply(*readback, state, *batch);
				}
				if (auto* writes = PrepareWrites(*readback, state, *batches.back(), reset)) {
					writes->next = readback->writes.load(std::memory_order_relaxed);
					while (!readback->writes.compare_exchange_weak(writes->next, writes, std::memory_order_release, std::memory_order_relaxed)) {}
				}
				// Back to the render thread, which releases their tables.
				for (auto* batch : batches) {
					batch->next = readback->decoded.load(std::memory_order_relaxed);
					while (!readback->decoded.compare_exchange_weak(batch->next, batch, std::memory_order_release, std::memory_order_relaxed)) {}
				}
				readback->decodeNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
					std::memory_order_relaxed);
				readback->running.store(false, std::memory_order_release);
				readback->running.notify_all();
			};
			if (!::RenderGraph::Frame::SharedExecutor().Dispatch(SceneScheduler::Scope(), ::RenderGraph::Frame::Executor::Preparation, org::async::TaskDispatch::Cpu, "visibility decode",
					[decode = std::move(decode)](const auto&) { decode(); }))
				::RenderGraph::Fatal("Drawcall Limit Fix", "the visibility decode was refused by DCLF's executor");
		}
	}

	void IndirectDraws::Impl::ArmVisibilityReadback(SceneBuffers& a_scene, CommitUploads& a_uploads)
	{
		if (!a_scene.visibilityReadback || !a_scene.visibilityReadback->list || !a_scene.visibilityEvents)
			return;
		auto& readback = *a_scene.visibilityReadback;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		// Reads the run's region, whose last copy is done because the lease retired, and stamps this run.
		const std::size_t harvested = readback.pending.size();
		const auto commit = readback.list->CommitList(host->CurrentRun(), [&](const org::RunReadback::ListRead& a_read) { Harvest(readback, a_read); });
		const std::uint32_t frameSlot = commit.region;
		if (frameSlot >= readback.armed.size())
			return;
		// A zero count and the run's stamp, which the read matches.
		const std::uint32_t header[kVisibilityEventHeaderWords]{ 0u, commit.stamp, 0u, 0u };
		a_uploads(a_scene.visibilityEvents, header, sizeof(header), 0);
		// A reported-words version the decoder has not followed is zero-filled from growth, so decoding restarts with this batch.
		bool reset = false;
		auto& followed = a_scene.visibilityReportedFollowed;
		const auto &roots = Adopted().Of(a_scene.visibilityRootReported), &objects = Adopted().Of(a_scene.visibilityObjectReported);
		if (!followed[0].Holds(roots) || !followed[1].Holds(objects)) {
			followed[0].Filled(roots);
			followed[1].Filled(objects);
			reset = true;
		}
		auto& armed = readback.armed[frameSlot];
		armed.armed = true;
		armed.frame = Scene::FrameView::Get().SceneStep();
		armed.reset = reset;
		armed.tables = Scene::FrameView::Get().AcceptedTables();
		armed.publication = Scene::FrameView::Get().InstalledSequence();
		armed.retirement = Scene::FrameView::Get().InstalledRetirement();
		armed.candidates = Scene::FrameView::Get().GetSunCandidates();
		// Decode a harvested batch now so its writes reach the next Main::Update, a frame sooner.
		if (readback.pending.size() > harvested)
			Dispatch(a_scene.visibilityReadback);
	}

	void Draws::PostCpuVisibility(VisibilityReadback::Batch* a_batch)
	{
		a_batch->next = cpuVisibility.load(std::memory_order_relaxed);
		while (!cpuVisibility.compare_exchange_weak(a_batch->next, a_batch, std::memory_order_release, std::memory_order_relaxed)) {}
	}

	void IndirectDraws::KickVisibilityReadback()
	{
		// CPU batches join the harvested ones. Without a readback they are freed here.
		for (auto* batch = cpuVisibility.exchange(nullptr, std::memory_order_acquire); batch;) {
			auto* following = batch->next;
			batch->next = nullptr;
			if (impl->scene && impl->scene->visibilityReadback)
				impl->scene->visibilityReadback->pending.push_back(batch);
			else
				delete batch;
			batch = following;
		}
		if (!impl->scene || !impl->scene->visibilityReadback)
			return;
		ReleaseDecoded(*impl->scene->visibilityReadback);
		Dispatch(impl->scene->visibilityReadback);
	}

	void IndirectDraws::NoteCameraCut(std::uint32_t a_frame)
	{
		if (!impl->scene || !impl->scene->visibilityReadback)
			return;
		impl->scene->visibilityReadback->cutFrame.store(a_frame, std::memory_order_release);
		impl->scene->visibilityReadback->cuts.fetch_add(1, std::memory_order_relaxed);
	}

	void IndirectDraws::RequestAccumulatedRefresh()
	{
		if (impl->scene && impl->scene->visibilityReadback)
			impl->scene->visibilityReadback->refreshWanted.store(true, std::memory_order_release);
	}

	void IndirectDraws::WriteAccumulated(const std::function<bool(const void*)>& a_stoodIn)
	{
		if (!impl->scene || !impl->scene->visibilityReadback)
			return;
		auto& r = *impl->scene->visibilityReadback;
		const auto start = std::chrono::steady_clock::now();
		// The stack is newest first, so reverse it into frame order.
		Readback::Writes* ordered = nullptr;
		for (auto* list = r.writes.exchange(nullptr, std::memory_order_acquire); list;) {
			auto* following = list->next;
			list->next = ordered;
			ordered = list;
			list = following;
		}
		const std::uint32_t cut = r.cutFrame.load(std::memory_order_acquire);
		const bool wrote = ordered != nullptr;
		for (auto* list = ordered; list;) {
			// A batch older than the last cut applies sets only. The full write after the cut restores the clears.
			const bool beforeCut = cut && list->frame < cut;
			// Its nodes stay alive while it holds its publication's retirement node. If the publication was released anyway, which is a defect,
			// it writes nothing and asks for a full write.
			const bool alive = EngineReleases::Alive(list->publication);
			const std::uint32_t age = Scene::FrameView::Get().SceneStep() - list->frame;
			for (std::uint32_t seen = r.writeAgeMax.load(std::memory_order_relaxed); age > seen && !r.writeAgeMax.compare_exchange_weak(seen, age, std::memory_order_relaxed);) {}
			if (!alive) {
				static std::uint32_t expiredLogged = 0;
				if (expiredLogged++ < 6)
					logger::info("[DCLF] readback writes of a released publication: list publication {} (frame {}), released through {}, handed over through {}, installed {} (frame {})",
						list->publication, list->frame, EngineReleases::releasedThrough.load(), EngineReleases::pushedThrough.load(), Scene::FrameView::Get().InstalledSequence(),
						Scene::FrameView::Get().SceneStep());
				r.writesExpired.fetch_add(list->writes.size(), std::memory_order_relaxed);
				r.refreshWanted.store(true, std::memory_order_release);
			}
			static const std::vector<Readback::Writes::Write> kNoWrites;
			for (const auto& write : alive ? list->writes : kNoWrites) {
				const bool held = beforeCut && !write.set;
				if (held) {
					r.writesHeldByCut.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				Store(r, write.node, write.set, write.wide);
				if (write.kind != Readback::kWriteGeometry)
					r.rootVerdicts.insert_or_assign(write.node, write.set);
				else if (write.set)
					SetChain(r, static_cast<RE::NiAVObject*>(write.node), write.root);
			}
			r.writeBatches.fetch_add(1, std::memory_order_relaxed);
			auto* following = list->next;
			delete list;
			list = following;
		}
		// The installed publication's tables and candidates, alive while installed. Replaced every call, so nothing older is kept past a frame.
		r.writtenTables = Scene::FrameView::Get().AcceptedTables();
		r.writtenRoots = r.writtenTables ? Extensions::RootListOf(*r.writtenTables) : Api::RootListView{};
		r.writtenCandidates = Scene::FrameView::Get().GetSunCandidates();
		// On a new camera cut, set every listed root, its geometry and the plain entry roots visible now.
		if (cut && r.cutWritten != cut && r.writtenTables) {
			r.cutWritten = cut;
			const auto& tables = *r.writtenTables;
			const auto& rootList = r.writtenRoots;
			std::vector<std::uint8_t> written(rootList.nodes.size(), 0u);
			for (std::size_t root = 0; root < rootList.nodes.size(); ++root) {
				const void* node = rootList.nodes[root];
				if (!node || root >= rootList.rows.size())
					continue;
				const bool stoodIn = (rootList.rows[root].bits & kFadeRootStoodIn) && a_stoodIn(node);
				written[root] = 1;
				Store(r, const_cast<void*>(node), true, !stoodIn);
				r.rootVerdicts.insert_or_assign(node, true);
				r.cutWrites.fetch_add(1, std::memory_order_relaxed);
			}
			const auto* candidates = r.writtenCandidates.get();
			for (std::size_t o = 0; o < tables.objectGeometry.size(); ++o) {
				auto* geometry = tables.objectGeometry[o];
				if (!geometry || tables.IsLayer(static_cast<std::uint32_t>(o)))
					continue;
				const std::uint32_t root = rootList.RootOf(static_cast<std::uint32_t>(o));
				if (root < written.size() && written[root]) {
					Store(r, geometry, true, !(rootList.rows[root].bits & kFadeRootStoodIn));
					SetChain(r, geometry, rootList.nodes[root]);
					continue;
				}
				if (root != kNoFadeRoot || !candidates)
					continue;
				const auto it = candidates->geometries.find(geometry);
				const std::uint32_t entry = it != candidates->geometries.end() && it->second < candidates->geometryEntry.size() ? candidates->geometryEntry[it->second] : EntrySpace::kNone;
				const RE::NiAVObject* entryNode = entry < candidates->entryNodes.size() ? candidates->entryNodes[entry] : nullptr;
				if (!entryNode || rootList.Contains(entryNode))
					continue;
				r.rootVerdicts.insert_or_assign(entryNode, true);
				Store(r, const_cast<RE::NiAVObject*>(entryNode), true, true);
				Store(r, geometry, true, true);
				SetChain(r, geometry, entryNode);
			}
		}
		// Every 60th write pass, forget verdicts of roots no longer listed.
		if (wrote && ++r.verdictPrunes >= 60) {
			r.verdictPrunes = 0;
			for (auto it = r.rootVerdicts.begin(); it != r.rootVerdicts.end();)
				it = (r.writtenTables && r.writtenRoots.Contains(it->first)) ||
				             (r.writtenCandidates && r.writtenCandidates->entries.contains(static_cast<const RE::NiAVObject*>(it->first))) ?
				         std::next(it) :
				         r.rootVerdicts.erase(it);
		}
		CellRoots(r);
		r.writeNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
			std::memory_order_relaxed);
	}

	bool IndirectDraws::AccumulatedVerdict(const void* a_root) const
	{
		if (!impl->scene || !impl->scene->visibilityReadback)
			return true;
		const auto& verdicts = impl->scene->visibilityReadback->rootVerdicts;
		const auto it = verdicts.find(a_root);
		return it == verdicts.end() || it->second;
	}

	bool IndirectDraws::AccumulatedVerdictOf(const void* a_root, bool& a_visible) const
	{
		if (!impl->scene || !impl->scene->visibilityReadback)
			return false;
		const auto& r = *impl->scene->visibilityReadback;
		const auto it = r.rootVerdicts.find(a_root);
		if (it == r.rootVerdicts.end())
			return false;
		// Must still be listed, so a kept verdict for a freed node cannot answer for another at its address.
		if (!(r.writtenTables && r.writtenRoots.Contains(a_root)) &&
			!(r.writtenCandidates && r.writtenCandidates->entries.contains(static_cast<const RE::NiAVObject*>(a_root))))
			return false;
		a_visible = it->second;
		return true;
	}


}
