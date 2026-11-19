// CS_DCLF_PERSISTENT_PARITY checks for the LOD gates. After each commit while gates live, the gates' state is checked
// against the data it was derived from. Every so many passes a sweep also checks the whole tracked set.
// Gate roots are used only as keys and are never dereferenced.

#include "Features/DrawcallLimitFix/Features/Lod/LodGateComponent.h"

#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneView.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace DCLF::Features::Lod
{
	namespace
	{
		using V = Diagnostics::SceneView;
		namespace G = Gates;
		using Parity = G::ObserverState::Parity;

		struct Commit
		{
			const Scene::Coordinator& store;
			bool live;
			std::uint32_t drawn;
			const G::GateState& gates;
			Parity& parity;
		};

		G::GateTag GateTagOf(const G::GateState& a_gates, const RE::BSGeometry* a_geometry)
		{
			const auto at = a_gates.gateTags.find(a_geometry);
			return at != a_gates.gateTags.end() ? at->second : G::GateTag{};
		}

		// Counts a difference and keeps the text of the first one.
		template <class Describe>
		void Differ(Parity& a_parity, Describe&& a_describe)
		{
			if (a_parity.differ++ == 0)
				a_parity.first = a_describe();
		}

		const char* SideName(std::uint8_t a_side)
		{
			return a_side == G::kGateIncoming ? "incoming" : a_side == G::kGateOutgoing ? "outgoing" : "no side";
		}

		// Decides the slot again the way the commit records an observed slot, pairing layers the same way.
		bool SlotWholeNow(const Commit& a_commit, std::uint32_t a_slot)
		{
			const auto& tables = V::tables(a_commit.store);
			const std::size_t objects = tables.objects.size();
			// The commit evaluates no slot past the tables.
			if (a_slot >= objects)
				return false;
			const auto decision = V::DecideSlot(a_commit.store, a_slot, a_commit.live, a_commit.drawn);
			std::uint8_t phases = decision.phases;
			const std::uint32_t partner = tables.IsLayer(a_slot) ? tables.layerBase[a_slot] : a_slot < tables.layerOf.size() ? tables.layerOf[a_slot] : kNoObjectSlot;
			if (partner != kNoObjectSlot && partner < objects && !(phases & kSetMain) != !(V::DecideSlot(a_commit.store, partner, a_commit.live, a_commit.drawn).phases & kSetMain))
				phases = 0;
			return !decision.wait && phases == (a_commit.live ? V::SetParticipation(a_commit.store, a_slot, a_commit.drawn) : std::uint8_t{ 0 });
		}

		// GeometryReady with each slot decided now instead of read from the commit's record.
		bool ReadyNow(const Commit& a_commit, const RE::BSGeometry* a_geometry, const V::Tracked& a_entry)
		{
			if (a_entry.leafWaiting || a_entry.candidateFrame == 0)
				return false;
			if (a_entry.slot == kNoObjectSlot && a_entry.layerSlot == kNoObjectSlot)
				return a_entry.candidateReason != Ineligible::None;
			for (const std::uint32_t slot : { a_entry.slot, a_entry.layerSlot })
				if (slot != kNoObjectSlot && (V::ObservedSlot(a_commit.store, slot).geometry != a_geometry || !SlotWholeNow(a_commit, slot)))
					return false;
			return true;
		}

		// G3. A withheld geometry must have no phases in the commit and no claim in the next publication.
		// a_open means an open gate's incoming, otherwise a held-out outgoing.
		void CheckWithheld(const Commit& a_commit, const RE::BSGeometry* a_geometry, const V::Tracked& a_entry, std::uint64_t a_token, bool a_open)
		{
			const auto& setPhases = V::setPhasesNext(a_commit.store);
			bool base = false;
			for (const std::uint32_t slot : { a_entry.slot, a_entry.layerSlot }) {
				if (slot == kNoObjectSlot || (a_open && V::ObservedSlot(a_commit.store, slot).geometry != a_geometry))
					continue;
				base = base || slot == a_entry.slot;
				if (slot < setPhases.size() && setPhases[slot])
					Differ(a_commit.parity, [&] {
						return fmt::format("withheld: geometry {} ({} of gate {}) slot {} holds {:#x} in the commit", static_cast<const void*>(a_geometry),
							a_open ? "incoming" : "held out", a_token, slot, setPhases[slot]);
					});
			}
			if (const auto* building = V::setBuilding(a_commit.store); base && building)
				if (const auto it = building->phases.find(a_geometry); it != building->phases.end())
					Differ(a_commit.parity, [&] {
						return fmt::format("withheld: geometry {} ({} of gate {}) claimed {:#x} by the next publication", static_cast<const void*>(a_geometry),
							a_open ? "incoming" : "held out", a_token, it->second);
					});
		}

		// G1. The gates' root lists, the root map and the mirror's gated roots agree.
		void CheckRoots(const Commit& a_commit)
		{
			const auto& lodGates = a_commit.gates.lodGates;
			const auto& rootMarks = V::rootMarks(a_commit.store);
			const auto& mirror = V::mirror(a_commit.store);
			for (const auto& [token, gate] : lodGates)
				for (const std::uint8_t side : { G::kGateIncoming, G::kGateOutgoing })
					for (const void* root : side == G::kGateIncoming ? gate.incoming : gate.outgoing)
						if (const auto at = rootMarks.find(root);
							at == rootMarks.end() || !G::IsGateMark(at->second.mark) || at->second.mark.token != token || at->second.mark.side != side)
							Differ(a_commit.parity, [&] {
								return at == rootMarks.end() || !G::IsGateMark(at->second.mark) ?
								           fmt::format("roots: gate {} lists {} root {}, the root map has none", token, SideName(side), root) :
								           fmt::format("roots: gate {} lists {} root {}, the root map names gate {} ({})", token, SideName(side), root,
											   at->second.mark.token, SideName(at->second.mark.side));
							});
			std::size_t incoming = 0;
			for (const auto& [root, marked] : rootMarks) {
				if (!G::IsGateMark(marked.mark))
					continue;
				const auto& owner = marked.mark;
				const auto gate = lodGates.find(owner.token);
				const auto* roots = gate == lodGates.end()          ? nullptr :
				                    owner.side == G::kGateIncoming ? &gate->second.incoming :
				                    owner.side == G::kGateOutgoing ? &gate->second.outgoing :
				                                                     nullptr;
				if (!roots || std::ranges::find(*roots, root) == roots->end())
					Differ(a_commit.parity, [&] {
						return fmt::format("roots: root {} mapped to gate {} ({}), {}", root, owner.token, SideName(owner.side), gate == lodGates.end() ? "no such gate" : "not listed by it");
					});
				incoming += owner.side == G::kGateIncoming ? 1u : 0u;
				if (mirror.Gated(root) != (owner.side == G::kGateIncoming))
					Differ(a_commit.parity, [&] { return fmt::format("roots: root {} of gate {} ({}) gated {} in the mirror", root, owner.token, SideName(owner.side), mirror.Gated(root)); });
			}
			if (mirror.GatedCount() != incoming)
				Differ(a_commit.parity, [&] { return fmt::format("roots: the mirror gates {} roots, the root map has {} incoming", mirror.GatedCount(), incoming); });
		}

		// G2. Each geometry in a gate's sets is tracked and tagged with that gate and side.
		void CheckTags(const Commit& a_commit, std::uint64_t a_token, const G::LodGate& a_gate)
		{
			const auto& tracked = V::tracked(a_commit.store);
			for (const std::uint8_t side : { G::kGateIncoming, G::kGateOutgoing })
				for (auto* geometry : side == G::kGateIncoming ? a_gate.incomingGeometries : a_gate.outgoingGeometries) {
					const auto it = tracked.find(geometry);
					const auto tag = GateTagOf(a_commit.gates, geometry);
					if (it == tracked.end() || tag.token != a_token || tag.side != side) {
						Differ(a_commit.parity, [&] {
							return it == tracked.end() ? fmt::format("tags: gate {} {} geometry {} not tracked", a_token, SideName(side), static_cast<const void*>(geometry)) :
							                             fmt::format("tags: gate {} {} geometry {} tagged gate {} ({})", a_token, SideName(side), static_cast<const void*>(geometry),
															 tag.token, SideName(tag.side));
						});
						continue;
					}
					if (it->second.veto == Api::kVetoHold)
						CheckWithheld(a_commit, geometry, it->second, a_token, false);
				}
		}

		// G3 and G4 for an open gate. Recorded readiness must match a fresh decision, and a ready gate must have flipped.
		void CheckOpen(const Commit& a_commit, std::uint64_t a_token, const G::LodGate& a_gate, bool a_retagged)
		{
			const auto& store = a_commit.store;
			const auto& tracked = V::tracked(store);
			std::uint32_t recorded = 0, fresh = 0;
			const RE::BSGeometry* differs = nullptr;
			bool differsRecorded = false;
			for (auto* geometry : a_gate.incomingGeometries) {
				const auto it = tracked.find(geometry);
				if (it == tracked.end()) {
					++recorded;
					++fresh;
					continue;
				}
				CheckWithheld(a_commit, geometry, it->second, a_token, true);
				const bool wasReady = G::GeometryReady(geometry, V::TrackedStateOf(store, geometry), [&](std::uint32_t a_slot) { return V::ObservedSlot(store, a_slot); });
				const bool readyNow = ReadyNow(a_commit, geometry, it->second);
				recorded += wasReady ? 0u : 1u;
				fresh += readyNow ? 0u : 1u;
				if (wasReady != readyNow && !differs) {
					differs = geometry;
					differsRecorded = wasReady;
				}
			}
			const bool flippable = a_commit.live && !a_gate.unmirrored && !a_gate.incoming.empty();
			if (flippable && !a_gate.pending)
				Differ(a_commit.parity, [&] { return fmt::format("stuck: gate {} open with nothing pending ({} incoming geometries, {} roots)", a_token, a_gate.incomingGeometries.size(), a_gate.incoming.size()); });
			// Skip the count when the settle deferred the gate or retagged anything, since pending can legitimately differ then.
			const bool counted = !(flippable && !recorded) && !a_retagged;
			if (differs || (counted && fresh != a_gate.pending))
				Differ(a_commit.parity, [&] {
					auto text = fmt::format("ready: gate {}: {} incoming geometries not ready by a fresh decision, {} by the commit's records, {} pending{}", a_token, fresh, recorded,
						a_gate.pending, counted ? "" : " (not compared)");
					if (differs)
						text += fmt::format("; geometry {} {} as recorded, {} now", static_cast<const void*>(differs), differsRecorded ? "ready" : "not ready", differsRecorded ? "not ready" : "ready");
					return text;
				});
		}

		// G4 and G5 for this commit's flips. Each incoming geometry must be ready and match the commit's phases.
		// Skipped after a retag, since a later gate's walk may take a flipped gate's geometry.
		void CheckFlips(const Commit& a_commit, bool a_retagged)
		{
			const auto& store = a_commit.store;
			const auto& lodGates = a_commit.gates.lodGates;
			const auto& tracked = V::tracked(store);
			const auto& tables = V::tables(store);
			const auto& setPhases = V::setPhasesNext(store);
			const auto& flips = G::Flips(V::componentDeltas(store, G::component));
			auto& p = a_commit.parity;
			// Only the flips after the newest one an earlier commit checked.
			std::size_t from = 0;
			if (p.lastFlip)
				for (std::size_t i = flips.size(); i > 0; --i)
					if (flips[i - 1].token == p.lastFlip) {
						from = i;
						break;
					}
			if (!flips.empty())
				p.lastFlip = flips.back().token;
			for (std::size_t f = from; f < flips.size(); ++f) {
				const auto& flip = flips[f];
				const auto found = lodGates.find(flip.token);
				if (found == lodGates.end() || found->second.state != G::LodGate::State::Flipped) {
					Differ(p, [&] { return fmt::format("flip: gate {} flipped by this commit is {}", flip.token, found == lodGates.end() ? "gone" : "open"); });
					continue;
				}
				const auto& gate = found->second;
				ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint8_t> held;
				for (auto* geometry : gate.incomingGeometries) {
					const auto it = tracked.find(geometry);
					if (it == tracked.end())
						continue;
					if (!ReadyNow(a_commit, geometry, it->second))
						Differ(p, [&] {
							return fmt::format("early: gate {} flipped with incoming geometry {} not ready by a fresh decision (slots {}, {})", flip.token, static_cast<const void*>(geometry),
								it->second.slot, it->second.layerSlot);
						});
					if (const std::uint32_t slot = it->second.slot; slot != kNoObjectSlot && slot < setPhases.size() && !tables.IsLayer(slot) && setPhases[slot])
						held.emplace(geometry, setPhases[slot]);
				}
				if (a_retagged)
					continue;
				if (flip.incoming.size() != held.size())
					Differ(p, [&] { return fmt::format("flip: gate {}: {} incoming geometries flipped, the commit holds {} with phases", flip.token, flip.incoming.size(), held.size()); });
				for (const auto& [geometry, phases] : flip.incoming)
					if (const auto it = held.find(geometry); it == held.end() || it->second != phases)
						Differ(p, [&] {
							return fmt::format("flip: gate {}: incoming geometry {} flipped with {:#x}, the commit holds {:#x}", flip.token, static_cast<const void*>(geometry), phases,
								it == held.end() ? 0u : it->second);
						});
				std::size_t outgoing = 0;
				for (auto* geometry : gate.outgoingGeometries)
					outgoing += tracked.contains(geometry) ? 1u : 0u;
				if (flip.outgoing.size() != outgoing)
					Differ(p, [&] { return fmt::format("flip: gate {}: {} outgoing geometries flipped, {} tracked in its outgoing", flip.token, flip.outgoing.size(), outgoing); });
				for (const auto* geometry : flip.outgoing)
					if (auto* key = const_cast<RE::BSGeometry*>(geometry); !gate.outgoingGeometries.contains(key) || !tracked.contains(key))
						Differ(p, [&] { return fmt::format("flip: gate {}: outgoing geometry {} flipped, not a tracked outgoing of it", flip.token, static_cast<const void*>(geometry)); });
			}
		}

		// The nearest marked root on the entry's mirror chain, as the prepare's mark walk finds it.
		// a_resolved is false when the chain hits a node the mirror has no record of.
		const Api::Mark* GateAbove(const Scene::Coordinator& a_store, const RE::BSGeometry* a_geometry, const V::Tracked& a_entry, bool& a_resolved)
		{
			const auto& mirror = V::mirror(a_store);
			const auto& rootMarks = V::rootMarks(a_store);
			const auto* record = mirror.Node(a_geometry);
			if (!record) {
				a_resolved = false;
				return nullptr;
			}
			std::uint32_t depth = 0;
			for (const void* key = record->parent; key;) {
				// The nearest mark decides, even when it belongs to another component.
				if (const auto at = rootMarks.find(key); at != rootMarks.end())
					return G::IsGateMark(at->second.mark) ? &at->second.mark : nullptr;
				if (key == a_entry.categoryNode)
					return nullptr;
				const auto* node = mirror.Node(key);
				if (!node || ++depth > 256) {
					a_resolved = false;
					return nullptr;
				}
				key = node->parent;
			}
			return nullptr;
		}

		// G7. Every open gate's incoming roots are hidden in the mirror. A forced release can show a root before its
		// outcome is applied, so a shown root is flagged only when it is still shown at the next sweep.
		void CheckVetoes(const Commit& a_commit)
		{
			const auto& mirror = V::mirror(a_commit.store);
			auto& p = a_commit.parity;
			std::vector<std::pair<std::uint64_t, const void*>> shown;
			for (const auto& [token, gate] : a_commit.gates.lodGates) {
				if (gate.state != G::LodGate::State::Open)
					continue;
				for (const void* root : gate.incoming) {
					const auto* record = mirror.Node(root);
					if (!record || (record->flags & static_cast<std::uint32_t>(RE::NiAVObject::Flag::kHidden)))
						continue;
					shown.emplace_back(token, root);
					if (std::ranges::find(p.shown, shown.back()) != p.shown.end())
						Differ(p, [&] { return fmt::format("veto: gate {} open with incoming root {} shown in the mirror at two sweeps", token, root); });
				}
			}
			p.shown = std::move(shown);
		}

		// The sweep runs G2, G3, G6 and G7 over the whole tracked set.
		void Sweep(const Commit& a_commit)
		{
			const auto& store = a_commit.store;
			const auto& lodGates = a_commit.gates.lodGates;
			auto& p = a_commit.parity;
			++p.sweeps;
			std::uint32_t heldOut = 0, observed = 0;
			std::vector<std::pair<std::uint64_t, const void*>> unmarkedOutgoing;
			for (const auto& [geometry, entry] : V::tracked(store)) {
				++p.entries;
				bool covered = false;
				const auto tag = GateTagOf(a_commit.gates, geometry);
				// Observe must be set exactly while the entry is an open gate's incoming.
				bool open = false;
				if (tag.token) {
					const auto gate = lodGates.find(tag.token);
					const auto* set = gate == lodGates.end()          ? nullptr :
					                  tag.side == G::kGateIncoming ? &gate->second.incomingGeometries :
					                  tag.side == G::kGateOutgoing ? &gate->second.outgoingGeometries :
					                                                 nullptr;
					covered = set && set->contains(geometry);
					open = tag.side == G::kGateIncoming && gate != lodGates.end() && gate->second.state == G::LodGate::State::Open;
					if (!covered)
						Differ(p, [&] {
							return fmt::format("tags: geometry {} tagged gate {} ({}), {}", static_cast<const void*>(geometry), tag.token, SideName(tag.side),
								gate == lodGates.end() ? "no such gate" : "not in its set");
						});
				}
				observed += entry.veto == Api::kVetoObserve ? 1u : 0u;
				if ((entry.veto == Api::kVetoObserve) != open)
					Differ(p, [&] {
						return fmt::format("tags: geometry {} tagged gate {} ({}{}), veto {:#x}", static_cast<const void*>(geometry), tag.token, SideName(tag.side),
							open ? ", open" : "", entry.veto);
					});
				if (entry.veto == Api::kVetoHold) {
					++heldOut;
					if (!covered)
						CheckWithheld(a_commit, geometry, entry, tag.token, false);
					// A retired flip's outgoing still waiting for its detach.
					if (!tag.token)
						continue;
				}
				bool resolved = true;
				const auto* above = GateAbove(store, geometry, entry, resolved);
				if (!resolved)
					continue;
				const std::uint64_t token = above ? above->token : 0;
				const std::uint8_t side = above ? above->side : 0;
				if (!above && tag.token && tag.side == G::kGateOutgoing) {
					// The root can detach before its entries are erased, so flag only when it is still so at the next sweep.
					++p.unmarked;
					unmarkedOutgoing.emplace_back(tag.token, geometry);
					if (std::ranges::find(p.unmarkedOutgoing, unmarkedOutgoing.back()) != p.unmarkedOutgoing.end())
						Differ(p, [&] {
							const auto gate = lodGates.find(tag.token);
							return fmt::format("marks: geometry {} tagged gate {} (outgoing) under no gate root at two sweeps; gate {}, {} outgoing roots left, {}held out",
								static_cast<const void*>(geometry), tag.token,
								gate == lodGates.end() ? "retired" : gate->second.state == G::LodGate::State::Open ? "open" : "flipped",
								gate == lodGates.end() ? 0 : gate->second.outgoing.size(), entry.veto == Api::kVetoHold ? "" : "not ");
						});
					continue;
				}
				if (token != tag.token || side != tag.side)
					Differ(p, [&] {
						return fmt::format("marks: geometry {} tagged gate {} ({}), its mirror chain's nearest root is gate {} ({})", static_cast<const void*>(geometry), tag.token,
							SideName(tag.side), token, SideName(side));
					});
			}
			p.unmarkedOutgoing = std::move(unmarkedOutgoing);
			if (heldOut != V::vetoHeldCount(store))
				Differ(p, [&] { return fmt::format("tags: {} entries held out, {} counted", heldOut, V::vetoHeldCount(store)); });
			if (observed != V::vetoObservedCount(store))
				Differ(p, [&] { return fmt::format("tags: {} entries observed, {} counted", observed, V::vetoObservedCount(store)); });
			CheckVetoes(a_commit);
		}

		void OnCommitted(const Diagnostics::SetCommitted& a_event)
		{
			if (!a_event.gatesLive)
				return;
			const auto& store = a_event.store;
			auto* gates = G::StateOf(V::componentState(store, G::component));
			if (!gates)
				return;
			Commit commit{ store, a_event.live, a_event.drawn, *gates, gates->observed.parity };
			auto& p = commit.parity;
			++p.checks;
			CheckRoots(commit);
			const bool retagged = !V::requeued(store).empty();
			for (const auto& [token, gate] : commit.gates.lodGates) {
				++p.gates;
				CheckTags(commit, token, gate);
				if (gate.state == G::LodGate::State::Open)
					CheckOpen(commit, token, gate, retagged);
			}
			CheckFlips(commit, retagged);
			if (Diagnostics::ParityDue(V::passSerial(store), 37) && std::exchange(p.pass, store.PassStamp()) != store.PassStamp())
				Sweep(commit);
		}
	}

	void Gates::SubscribeGateParity()
	{
		Diagnostics::Subscribe<Diagnostics::SetCommitted>(OnCommitted);
	}
}
