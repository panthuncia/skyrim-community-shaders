// The LOD gates' scene side: opening gates, holding back their incoming geometries, flipping, and retiring on outcomes.
// Runs on the scene lane.

#include "Features/DrawcallLimitFix/Features/Lod/LodGateComponent.h"

#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Engine/FrameClock.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodGates.h"

#include <algorithm>

namespace DCLF::Features::Lod::Gates
{
	namespace
	{
		using Context = Api::CoordinatorComponentContext;

		// Leaves the entry's veto to the caller.
		void UntagGate(GateState& a_state, const RE::BSGeometry* a_geometry)
		{
			auto& gateTags = a_state.gateTags;
			const auto tag = gateTags.find(a_geometry);
			if (tag == gateTags.end())
				return;
			if (const auto gate = a_state.lodGates.find(tag->second.token); gate != a_state.lodGates.end())
				(tag->second.side == kGateIncoming ? gate->second.incomingGeometries : gate->second.outgoingGeometries).erase(const_cast<RE::BSGeometry*>(a_geometry));
			gateTags.erase(tag);
		}

		void TagGate(const Context& a_context, GateState& a_state, const RE::BSGeometry* a_geometry, std::uint64_t a_token, std::uint8_t a_side)
		{
			auto& gateTags = a_state.gateTags;
			auto& gateStats = a_state.gateStats;
			const auto tag = gateTags.find(a_geometry);
			if (tag != gateTags.end() && tag->second.token == a_token && tag->second.side == a_side)
				return;
			const auto gate = a_state.lodGates.find(a_token);
			if (gate == a_state.lodGates.end())
				return;
			if (tag != gateTags.end())
				UntagGate(a_state, a_geometry);
			gateTags[a_geometry] = GateTag{ a_token, a_side };
			auto* geometry = const_cast<RE::BSGeometry*>(a_geometry);
			if (a_side == kGateIncoming) {
				gate->second.incomingGeometries.insert(geometry);
				++gateStats.taggedIncoming;
				// Withheld while the gate is open. Readiness must come from an evaluation under this gate, and a verdict
				// taken while the gate's hide counted no longer stands, so classify it again.
				a_context.SetEntryVeto(a_geometry, gate->second.state == LodGate::State::Open ? Api::kVetoObserve : std::uint8_t{ 0 });
				a_context.Requeue(a_geometry, std::uint8_t(Api::kRequeueReevaluate | Api::kRequeueSlots));
			} else {
				gate->second.outgoingGeometries.insert(geometry);
				++gateStats.taggedOutgoing;
				// The flip will hold it out again.
				a_context.SetEntryVeto(a_geometry, 0);
			}
		}

		// Returns true when the mirror lacked a record under a_root.
		bool TagGateRoot(const Context& a_context, GateState& a_state, std::uint64_t a_token, const void* a_root, std::uint8_t a_side)
		{
			auto& entries = a_state.gateEntries;
			entries.clear();
			bool lacking = false;
			a_context.SubtreeEntries(a_root, entries, lacking);
			for (const void* geometry : entries)
				TagGate(a_context, a_state, static_cast<const RE::BSGeometry*>(geometry), a_token, a_side);
			return lacking;
		}

		bool GateGeometryReady(const Context& a_context, const RE::BSGeometry* a_geometry)
		{
			return GeometryReady(a_geometry, a_context.TrackedStateOf(a_geometry), [&](std::uint32_t a_slot) { return a_context.ObservedSlot(a_slot); });
		}

		void OpenGate(const Context& a_context, GateState& a_state, std::uint64_t a_token, std::span<const void* const> a_incoming, std::span<const void* const> a_outgoing,
			std::uint64_t a_stampNs, std::uint32_t a_stampFrame)
		{
			auto& lodGates = a_state.lodGates;
			auto& gateStats = a_state.gateStats;
			if (!a_token)
				return;
			const auto [it, inserted] = lodGates.try_emplace(a_token);
			if (!inserted) {
				// Tokens are unique, so a second event is a defect. The first one stands.
				static std::uint32_t logged = 0;
				if (logged++ < 4)
					logger::warn("[DCLF] LOD gate {}: a second gate event for the same token, ignored <- GATE TOKEN", a_token);
				return;
			}
			++gateStats.opened;
			{
				auto& gate = it->second;
				gate.stampNs = a_stampNs ? a_stampNs : GateNowNs();
				gate.stampFrame = a_stampFrame;
			}
			// A root another gate still names moves to this gate. Only incoming roots own their hide.
			// The commit runs live while any gate stands.
			a_context.SetCommitLive(true);
			auto take = [&](const void* a_root, std::uint8_t a_side) {
				if (const auto displaced = a_context.MarkRoot(a_root, a_token, a_side, a_side == kGateIncoming)) {
					if (displaced.component.index == component.index)
						if (const auto other = lodGates.find(displaced.token); other != lodGates.end()) {
							std::erase(other->second.incoming, a_root);
							std::erase(other->second.outgoing, a_root);
						}
					++gateStats.superseded;
				}
			};
			for (const void* root : a_incoming) {
				if (!root)
					continue;
				// The gate owns this hide, so the geometry is still classified and committed while the engine keeps it hidden.
				take(root, kGateIncoming);
				lodGates.find(a_token)->second.incoming.push_back(root);
				// A root the mirror has no record of yet holds the gate until it does.
				if (TagGateRoot(a_context, a_state, a_token, root, kGateIncoming))
					++lodGates.find(a_token)->second.unmirrored;
			}
			for (const void* root : a_outgoing) {
				if (!root)
					continue;
				take(root, kGateOutgoing);
				lodGates.find(a_token)->second.outgoing.push_back(root);
				TagGateRoot(a_context, a_state, a_token, root, kGateOutgoing);
			}
			if (lodGates.find(a_token)->second.incoming.empty())
				++gateStats.emptyIncoming;
		}

		// a_forced means the gate was released without a flip.
		void RetireGate(const Context& a_context, GateState& a_state, std::uint64_t a_token, bool a_forced)
		{
			auto& lodGates = a_state.lodGates;
			auto& gateStats = a_state.gateStats;
			const auto it = lodGates.find(a_token);
			if (it == lodGates.end())
				return;
			auto& gate = it->second;
			const bool open = gate.state == LodGate::State::Open;
			// Every root still listed is marked as this gate's. Later gates and detaches remove theirs from the list.
			for (const void* root : gate.incoming)
				a_context.UnmarkRoot(root, a_token);
			for (const void* root : gate.outgoing)
				a_context.UnmarkRoot(root, a_token);
			for (auto* geometry : gate.incomingGeometries) {
				if (!a_context.TrackedStateOf(geometry).tracked)
					continue;
				a_state.gateTags.erase(geometry);
				if (open) {
					// Forced before its flip, so the block goes back to the engine. Classify it again without the gate.
					a_context.SetEntryVeto(geometry, 0);
					a_context.Requeue(geometry, std::uint8_t(Api::kRequeueReevaluateAlways | Api::kRequeueSlots));
				}
			}
			// Flipped outgoing geometries stay held out until their detach erases them.
			for (auto* geometry : gate.outgoingGeometries)
				if (a_context.TrackedStateOf(geometry).tracked)
					a_state.gateTags.erase(geometry);
			if (a_forced) {
				++gateStats.retiredForced;
				gateStats.forcedOpen += open ? 1u : 0u;
			} else {
				++gateStats.retiredFlipped;
			}
			lodGates.erase(it);
			a_context.SetCommitLive(!lodGates.empty());
		}

		// The engine reattached a held outgoing block. No detach will ever erase its entries, so we must release the hold-out here.
		// Its gate may be open, flipped, or already retired.
		void ApplyGateKept(const Context& a_context, GateState& a_state, std::uint64_t a_token, const std::array<const void*, 2>& a_roots)
		{
			auto& gateTags = a_state.gateTags;
			auto& gateStats = a_state.gateStats;
			++gateStats.keptNotices;
			const auto gate = a_state.lodGates.find(a_token);
			for (const void* root : a_roots) {
				if (!root)
					continue;
				if (const auto mark = a_context.MarkOf(root); mark.component.index == component.index && mark.token == a_token && mark.side == kGateOutgoing) {
					a_context.UnmarkRoot(root, a_token);
					if (gate != a_state.lodGates.end())
						std::erase(gate->second.outgoing, root);
				}
				auto& entries = a_state.gateEntries;
				entries.clear();
				bool lacking = false;
				a_context.SubtreeEntries(root, entries, lacking);
				for (const void* key : entries) {
					const auto* geometry = static_cast<const RE::BSGeometry*>(key);
					const auto tag = gateTags.find(geometry);
					// Ours, or an untagged hold-out this gate's retirement left behind.
					const bool ours = tag != gateTags.end() && tag->second.token == a_token && tag->second.side == kGateOutgoing;
					const bool heldOut = a_context.TrackedStateOf(geometry).veto == Api::kVetoHold;
					if (!ours && !(tag == gateTags.end() && heldOut))
						continue;
					UntagGate(a_state, geometry);
					if (!heldOut) {
						++gateStats.keptUntagged;
						continue;
					}
					a_context.SetEntryVeto(geometry, 0);
					++gateStats.keptCleared;
					a_context.Requeue(geometry, Api::kRequeueSlots);
				}
			}
		}
	}

	std::shared_ptr<void> MakeState() { return std::make_shared<GateState>(); }

	void ApplyGateEvent(const Api::CoordinatorComponentContext& a_context, Api::EngineChannelId a_channel, const Api::Stacked& a_event)
	{
		if (a_channel == channel)
			OpenGate(a_context, a_context.State<GateState>(), a_event.token, a_event.pinned, a_event.named, a_event.stampNs, a_event.stampFrame);
	}

	void RootDetached(const Api::CoordinatorComponentContext& a_context, const void* a_key, const Api::Mark& a_mark)
	{
		// An open gate left with no incoming root never flips. The engine side's forced release retires it.
		auto& state = a_context.State<GateState>();
		if (const auto gate = state.lodGates.find(a_mark.token); gate != state.lodGates.end()) {
			std::erase(gate->second.incoming, a_key);
			std::erase(gate->second.outgoing, a_key);
		}
		++state.gateStats.rootsDetached;
	}

	void UnderMark(const Api::CoordinatorComponentContext& a_context, const void* a_geometry, const Api::Mark& a_mark)
	{
		if (a_context.TrackedStateOf(a_geometry).tracked)
			TagGate(a_context, a_context.State<GateState>(), static_cast<const RE::BSGeometry*>(a_geometry), a_mark.token, a_mark.side);
	}

	void Settle(const Api::CoordinatorComponentContext& a_context, bool a_live)
	{
		auto& state = a_context.State<GateState>();
		auto& gateStats = state.gateStats;
		for (auto& [token, gate] : state.lodGates) {
			if (gate.state != LodGate::State::Open)
				continue;
			if (gate.unmirrored) {
				std::uint32_t unmirrored = 0;
				for (const void* root : gate.incoming)
					unmirrored += TagGateRoot(a_context, state, token, root, kGateIncoming) ? 1u : 0u;
				gate.unmirrored = unmirrored;
			}
			gate.pending = 0;
			for (auto* geometry : gate.incomingGeometries)
				if (!GateGeometryReady(a_context, geometry))
					++gate.pending;
			if (!a_live || gate.pending || gate.unmirrored || gate.incoming.empty())
				continue;
			GateFlip flip;
			flip.token = token;
			flip.requestNs = gate.stampNs;
			flip.requestFrame = gate.stampFrame;
			gate.flipNs = GateNowNs();
			gate.flipFrame = FrameClock::Frame();
			flip.flipNs = gate.flipNs;
			flip.flipFrame = gate.flipFrame;
			for (auto* geometry : gate.incomingGeometries) {
				const auto entry = a_context.TrackedStateOf(geometry);
				if (!entry.tracked)
					continue;
				// Each slot takes the phases recorded at its last evaluation.
				a_context.SetEntryVeto(geometry, 0);
				// Claims are by base geometry. A layer is claimed with its base.
				if (entry.slot != kNoSlot)
					if (const auto observed = a_context.ObservedSlot(entry.slot); observed.geometry == geometry && observed.phases && !a_context.LayerSlot(entry.slot))
						flip.incoming.emplace_back(geometry, observed.phases);
			}
			for (auto* geometry : gate.outgoingGeometries) {
				const auto entry = a_context.TrackedStateOf(geometry);
				if (!entry.tracked)
					continue;
				// Held out until a LOD drain after the flip's release detaches it.
				if (entry.veto != Api::kVetoHold)
					++gateStats.heldOut;
				a_context.SetEntryVeto(geometry, Api::kVetoHold);
				flip.outgoing.push_back(geometry);
			}
			gate.state = LodGate::State::Flipped;
			++gateStats.flipped;
			gateStats.requestToFlipMs.push_back(GateMs(gate.stampNs, gate.flipNs));
			gateStats.requestToFlipFrames.push_back(gate.flipFrame >= gate.stampFrame ? gate.flipFrame - gate.stampFrame : 0u);
			a_context.PublishDelta(std::make_shared<const GateFlip>(std::move(flip)));
		}
	}

	void Take(const Api::CoordinatorComponentContext& a_context)
	{
		// Runs before the event stack is drained, so every outcome taken here was posted after its gate's events.
		auto& state = a_context.State<GateState>();
		if (!LodGates::Enabled() && state.lodGates.empty())
			return;
		auto& held = state.gateOutcomesHeld;
		LodGates::DrainOutcomes([&held](const LodGates::Outcome& a_outcome) { held.push_back(GateOutcome{ a_outcome.token, a_outcome.forced, a_outcome.kept }); });
	}

	void ApplyOutcomes(const Api::CoordinatorComponentContext& a_context, Api::EngineChannelId a_channel, std::span<const void* const>)
	{
		if (a_channel != channel)
			return;
		// Runs after the pass's batch, so gate events are applied before their outcomes.
		auto& state = a_context.State<GateState>();
		if (state.gateOutcomesHeld.empty())
			return;
		for (const auto& outcome : state.gateOutcomesHeld) {
			// A kept notice can arrive before or after its gate's flip outcome.
			if (outcome.kept[0]) {
				ApplyGateKept(a_context, state, outcome.token, outcome.kept);
				continue;
			}
			if (!state.lodGates.contains(outcome.token)) {
				++state.gateStats.unknownOutcomes;
				continue;
			}
			RetireGate(a_context, state, outcome.token, outcome.forced);
		}
		state.gateOutcomesHeld.clear();
	}

	void Forgot(const Api::CoordinatorComponentContext& a_context, const void* a_entry)
	{
		UntagGate(a_context.State<GateState>(), static_cast<const RE::BSGeometry*>(a_entry));
	}
}
