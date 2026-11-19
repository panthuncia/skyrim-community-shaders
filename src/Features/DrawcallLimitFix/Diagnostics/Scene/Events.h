#pragma once

#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/MirrorEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/RootViewEvents.h"

#include <vector>

#include <cstdint>
#include <span>

namespace DCLF::Diagnostics
{
	/** @brief Scene points that carry no data. Coordinator thread unless marked render thread. */
	enum class ScenePoint : std::uint8_t
	{
		AccumulateInputsTaken,   // first in the pass, the frame inputs posted since the last pass taken
		ScenePhaseBegin,         // the change log trimmed, before anything of the pass writes
		ShadowSetsKept,          // the shadow sets updated by changes, about to be made whole again
		ShadowSetsRebuilt,       // ... made whole again from the same tables
		WalkDone,                // before the joins
		JoinsBegin,              // nothing bound yet
		ResidentsKept,           // before the slot check
		JoinsEnd,                // the joins done (the verdicts and the residency this pass leaves)
		MirrorCheckPoint,        // at a pass's end (its events all drained)
		MirrorProbePoint,        // render thread, frame start, the live scene captured for the mirror's observers
		AccumulatePrepared,      // render thread, EarlyPrepass, the material captures served
		PrepassConstantsBegin,   // render thread, Prepass, the character light's view taken
		PipelineConstantsBegin,  // ... the pipelines' blocks are about to be refreshed
		PrepassConstantsEnd,     // ... the frame constants refreshed
	};

	// Scene hook points. Each runs on its Notify's thread, which is the coordinator's pass unless noted, and reads the
	// coordinator's state through SceneView.

	/** @brief The frame's registrations, drained from the pass capture. Render thread, EarlyPrepass. */
	struct CaptureDrained
	{
		Scene::FrameView& view;
		std::span<const PassCapture::Entry> entries;
	};

	/** @brief A material slot lost its last reference, before its contents are dropped. */
	struct MaterialSlotDrained
	{
		Scene::Coordinator& store;
		std::uint32_t slot;
	};

	/** @brief A candidate table was updated and its snapshot published. `sun` picks the sun's, else the point lights'. */
	struct CandidatesUpdated
	{
		Scene::Coordinator& store;
		bool sun;
	};

	/** @brief A publication's snapshot was brought up to the tables. */
	struct TablesReplayed
	{
		Scene::Coordinator& store;
		const SceneTables& snapshot;
		std::uint64_t published;
		std::uint64_t position, geometryPosition;  // change and geometry log ends before the replay
		std::uint32_t keptFamilies;  // families the replay kept, a bit per registration
		bool keepConstants;  // the other family the replay kept
	};

	// Mirror reads, announced for observers that check them against the live scene. Policy::SerialFanout keeps them on the pass's
	// thread while observed.

	/** @brief FindCategoryNode by the mirror. */
	struct CategoryNodeRead
	{
		const Scene::Coordinator& store;
		RE::NiAVObject* object;
		RE::NiNode* found;
		Ineligible reason;
		bool unmirrored;  // a record was missing on the way
	};

	/** @brief ResolveSunEntry by the mirror, and the live chain's entry when `liveKnown`. */
	struct SunEntryRead
	{
		const Scene::Coordinator& store;
		const RE::BSGeometry& geometry;
		const void* entry;
		const RE::NiAVObject* live;
		bool liveKnown;
	};

	/** @brief LightEntryOf by the mirror, and the live chain's root when `liveKnown`. */
	struct LightEntryRead
	{
		const Scene::Coordinator& store;
		const RE::BSGeometry& geometry;
		const RE::NiNode* category;
		const void* root;
		bool actorRoot;
		const RE::NiAVObject* live;
		bool liveActor;
		bool liveKnown;
	};

	/** @brief PrepareSubtree by the mirror, with each geometry's parent reasons. */
	struct SubtreeRead
	{
		const Scene::Coordinator& store;
		RE::NiAVObject* root;
		Ineligible reasonAbove;
		std::span<const std::pair<RE::BSGeometry*, Ineligible>> found;
		bool lacking;
	};

	/** @brief A switch or node event's walk below `root` by the mirror. */
	struct TrackedBelowRead
	{
		const Scene::Coordinator& store;
		SceneView::MirrorRead read;
		RE::NiAVObject* root;
	};

	/** @brief ListHiddenChain by the mirror. */
	struct HiddenChainRead
	{
		const Scene::Coordinator& store;
		const RE::BSGeometry* geometry;
		const RE::NiNode* category;
		std::span<const RE::NiAVObject* const> chain;
	};

	/** @brief MoveKeyOf by the mirror. `key` is null for none or several. */
	struct MoveKeyRead
	{
		const Scene::Coordinator& store;
		const RE::BSGeometry& geometry;
		const RE::NiNode* category;
		const void* key;
	};

	// Mirror writes and probe points.

	/** @brief A hook's update was applied to the mirror. `key` is null when the mirror holds no record. */
	struct MirrorUpdateApplied
	{
		std::uint8_t type;
		const void* key;
		std::uint32_t fields;
	};

	/** @brief First in the coordinator's event drains. */
	struct EventsCollecting
	{
		Scene::Coordinator& store;
	};

	/** @brief A batch was applied to the mirror, with the objects named by its value-less events, by engine channel. */
	struct MirrorEventsApplied
	{
		Scene::Coordinator& store;
		std::span<const std::vector<const void*>> channelKeys;
	};

	/** @brief A local shadow descriptor's volume as the Lights board has it, and its culling process after the cull. */
	struct ShadowVolumeSampled
	{
		const ShadowCameras::Camera& view;
		const RE::BSCullingProcess& process;
	};

	/** @brief An entry whose mirror chain reaches a CategoryRoot view's root while filed under another category node. Any pass thread. */
	struct ViewMisfiled
	{
		Api::EngineView view;
		const void* geometry;
		const void* category;
	};

	/** @brief An actor's frame verdict changed with no hidden event, with its chain up to its category node. */
	struct ActorVerdictUnannounced
	{
		std::span<const RE::NiAVObject* const> chain;
	};

	/** @brief CommitSet left a CategoryRoot view's slot out of its view. `waitWhy` is the waiting reason it shows. */
	struct SetSlotDecided
	{
		Scene::Coordinator& store;
		std::uint32_t slot;
		const SceneView::SlotDecision& decision;
		std::uint8_t waitWhy;
	};

	/** @brief The set's commit applied its queue and flips. */
	struct SetCommitted
	{
		Scene::Coordinator& store;
		bool live;
		std::uint32_t drawn;
		bool gatesLive;
	};

}
