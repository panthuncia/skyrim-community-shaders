#pragma once

// The scene side of the LOD gates, as a coordinator component. LodGates.h has the engine side.
// A gate opens when its swap event arrives. The commit holds back its incoming geometries until all of them are ready,
// then flips it. The render thread releases the flip when it adopts that publication.
// See dclf-architecture.md, "LOD gates: the scene side".

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <ankerl/unordered_dense.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace DCLF::Features::Lod::Gates
{
	/** @brief Unset while the gates are disabled. */
	inline Api::CoordinatorComponentId component;
	inline Api::EngineChannelId channel;

	/** @brief Steady clock in ns, and a span on it in ms (0 without a start). */
	inline std::uint64_t GateNowNs()
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}
	inline double GateMs(std::uint64_t a_from, std::uint64_t a_to)
	{
		return a_from && a_to > a_from ? static_cast<double>(a_to - a_from) / 1.0e6 : 0.0;
	}

	/** @brief Published as the component's delta. The frame that installs that publication releases it. */
	struct GateFlip
	{
		std::uint64_t token = 0;
		std::uint64_t requestNs = 0, flipNs = 0;
		std::uint32_t requestFrame = 0, flipFrame = 0;  // render thread frames
	};

	inline constexpr std::uint8_t kGateIncoming = 1, kGateOutgoing = 2;
	struct LodGate
	{
		enum class State : std::uint8_t
		{
			Open,     // incoming withheld, outgoing still members
			Flipped,  // incoming applied, outgoing held out until the outcome arrives
		};
		State state = State::Open;
		std::vector<const void*> incoming, outgoing;  // root keys, minus those detached since
		ankerl::unordered_dense::set<RE::BSGeometry*> incomingGeometries, outgoingGeometries;
		std::uint32_t pending = 0;     // incoming geometries neither whole members nor ineligible
		std::uint32_t unmirrored = 0;  // incoming roots or nodes under them without a mirror record
		std::uint64_t stampNs = 0;
		std::uint32_t stampFrame = 0;
		std::uint64_t flipNs = 0;
		std::uint32_t flipFrame = 0;
	};
	/** @brief Must match Core's kNoObjectSlot. */
	inline constexpr std::uint32_t kNoSlot = ~0u;
	struct GateTag
	{
		std::uint64_t token = 0;
		std::uint8_t side = 0;
	};
	struct GateOutcome
	{
		std::uint64_t token = 0;
		bool forced = false;
		std::array<const void*, 2> kept{};  // see LodGates::Outcome::kept
	};
	struct GateStats
	{
		std::uint64_t opened = 0, flipped = 0, retiredFlipped = 0, retiredForced = 0, forcedOpen = 0, unknownOutcomes = 0, superseded = 0;
		std::uint64_t rootsDetached = 0, taggedIncoming = 0, taggedOutgoing = 0, heldOut = 0, emptyIncoming = 0;
		std::uint64_t keptNotices = 0, keptCleared = 0, keptUntagged = 0;
		std::vector<double> requestToFlipMs;
		std::vector<std::uint32_t> requestToFlipFrames;
	};

	/** @brief Scene lane only. */
	struct GateState
	{
		ankerl::unordered_dense::map<std::uint64_t, LodGate> lodGates;
		// The gates' roots live in the coordinator's marks. Tags are erased with their tracked entry.
		ankerl::unordered_dense::map<const RE::BSGeometry*, GateTag> gateTags;
		std::vector<const void*> gateEntries;       // scratch, reused by each walk
		std::vector<GateOutcome> gateOutcomesHeld;  // taken, not applied yet
		GateStats gateStats;
	};

	/** @brief Ready means ineligible by a verdict since it was tagged, or every slot of it whole. */
	template <class ObservedOf>
	bool GeometryReady(const void* a_geometry, const Api::TrackedState& a_entry, ObservedOf&& a_observedOf)
	{
		if (a_entry.leafWaiting || !a_entry.classified)
			return false;
		// No slot and no reason means the verdict has not been written yet.
		if (a_entry.slot == kNoSlot && a_entry.layerSlot == kNoSlot)
			return a_entry.reason != 0;
		for (const std::uint32_t slot : { a_entry.slot, a_entry.layerSlot })
			if (slot != kNoSlot)
				if (const Api::ObservedState observed = a_observedOf(slot); observed.geometry != a_geometry || !observed.ready)
					return false;
		return true;
	}

	/** @brief Null before the state is made. */
	inline GateState* StateOf(void* a_componentState) { return static_cast<GateState*>(a_componentState); }
	inline bool IsGateMark(const Api::Mark& a_mark) { return a_mark.component.index == component.index; }
	/** @brief The component's pending deltas, each a GateFlip. */
	inline auto Flips(const std::vector<std::shared_ptr<const void>>& a_deltas)
	{
		return a_deltas | std::views::transform([](const std::shared_ptr<const void>& a_delta) -> const GateFlip& { return *static_cast<const GateFlip*>(a_delta.get()); });
	}

	/** @brief Installs the engine hooks, then registers the rest only if they installed. */
	void RegisterGates(Api::InstallContext& a_context);

	/** @brief Creates the GateState. */
	std::shared_ptr<void> MakeState();
	/** @brief A gate event opens its gate. It arrives in stack order, after its incoming blocks attached. */
	void ApplyGateEvent(const Api::CoordinatorComponentContext& a_context, Api::EngineChannelId a_channel, const Api::Stacked& a_event);
	void RootDetached(const Api::CoordinatorComponentContext& a_context, const void* a_key, const Api::Mark& a_mark);
	/** @brief Tags a newly tracked geometry found under one of the gates' roots. */
	void UnderMark(const Api::CoordinatorComponentContext& a_context, const void* a_geometry, const Api::Mark& a_mark);
	/** @brief Flips every open gate that is ready, and publishes each flip as a GateFlip delta in this commit. */
	void Settle(const Api::CoordinatorComponentContext& a_context, bool a_live);
	/** @brief Runs before the pass's events are collected. */
	void Take(const Api::CoordinatorComponentContext& a_context);
	/** @brief Retires each held outcome's gate. A forced gate's incoming geometries are classified again. */
	void ApplyOutcomes(const Api::CoordinatorComponentContext& a_context, Api::EngineChannelId a_channel, std::span<const void* const> a_keys);
	void Forgot(const Api::CoordinatorComponentContext& a_context, const void* a_entry);

	/**
	 * @brief Render thread, at the frame's start before the culls. Calls LodGates::Flip for each flip the installed
	 * publications carry, so the swap shows in the first frame whose claims cover it.
	 */
	void Adopt(const Api::AdoptionContext& a_context);

	/** @brief Render thread. Taken by ReleaseLines. */
	struct ReleaseStats
	{
		std::uint64_t released = 0, refused = 0, beforeDrawing = 0;  // beforeDrawing: before DCLF's first covered frame
		std::vector<double> flipToReleaseMs, requestToReleaseMs;
		std::vector<std::uint32_t> flipToReleaseFrames;
		bool any = false;
	};
	ReleaseStats& Releases();

	/** @brief Empty while nothing happened. */
	void GateLines(const Api::CoordinatorComponentContext& a_context, std::string& a_text);
	/** @brief Render thread, once a report window. */
	void ReleaseLines(std::uint32_t a_frame);
}
