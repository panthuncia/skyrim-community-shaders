#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "LightingDescriptors.h"

namespace DCLF
{
	struct SunCandidates;

	/**
	 * @brief The primary view's cull, and taking DCLF's objects out of it (docs/development/dclf-gpu-driven-frame.md;
	 * the plan: the main camera's list jobs and registration stop visiting what DCLF draws). AE only.
	 *
	 * The main camera culls the scene lists (`DAT_14338c870`, `DAT_14338c868` of them): BSTArray<NiPointer<NiAVObject>>
	 * that DrawWorld_BuildSceneLists fills with reference roots, round robin. NiCamera::CalculateAndDrawShadowCasterLights
	 * hands them to the sun's full-frustum cull first, then queues one list job per list and waits for them
	 * (JobList::Finish); the registration jobs walk the list processes' output afterwards. Between the full-frustum
	 * cull and that Finish, nothing but the list jobs reads the lists.
	 *
	 * Step 1, the census (CS_DCLF_PRIMARY_EXCLUDE=probe, nothing removed): what the lists hold, against the sun's entry
	 * candidates, and what the main camera registers under them.
	 *
	 * The cut (toggle `excludePrimaryEntries`, CS_DCLF_PRIMARY_EXCLUDE=1): right after the full-frustum cull, every list
	 * entry DCLF can stand in for (below) is taken out of its list, and the lists are put back as they were once the
	 * list jobs have finished. Such an entry is never traversed, culled or registered for the main camera or its depth
	 * pass. In its place, after the jobs:
	 *   - the entry's bound is tested against the main camera's planes (the list process's, as the job set them up);
	 *   - every geometry under a visible entry that is not hidden gets a synthetic main pass (SyntheticPass), which the
	 *     accumulate phase takes as if the engine had registered it (BuildSyntheticPasses);
	 *   - its activeLightMask is cleared, as the main registration (+0x160 = 0xFFFF) would have.
	 * An entry is left out when the candidates' snapshot says every geometry under it is a main-pass table object the
	 * synthetic pass reproduces (SunCandidates::primary), its nodes need nothing from OnVisible (a plain node, or a
	 * fade node the fade update skips: settled and fully faded in), every geometry under it derives, and DCLF drew
	 * all of them in the last frame the entry was in the lists (admission: having drawn it is the evidence that it can
	 * be drawn). Frame preconditions: the sun's entry exclusion is live (its cascades are captured) and no local light
	 * cast shadows last frame (a synthetic pass has no point-light shadow).
	 */
	class PrimaryCull
	{
	public:
		static PrimaryCull& Get();

		/** @brief AE: the call sites in CalculateAndDrawShadowCasterLights, verified before patching. */
		void Install();
		bool Installed() const { return installed; }

		/** @brief CS_DCLF_PRIMARY_EXCLUDE=probe. */
		static bool Probe();

		/**
		 * @brief A registration through FUN_140e28af0 outside the sun's Accumulate, any thread: counted by accumulator
		 * (the main one, render mode 0; the depth one, 0xC) and by whether the geometry is under a listed candidate entry.
		 */
		void NoteRegistration(const void* a_accumulator, const RE::BSGeometry* a_geometry);
		bool Counting() const { return counting.load(std::memory_order_relaxed); }

		/** @brief Render thread, after the list jobs: whether the geometry is under a candidate entry the lists hold. */
		bool UnderListedCandidate(const RE::BSGeometry* a_geometry) const;
		/**
		 * @brief The accumulate phase, render thread, for an object under a listed candidate entry: what its
		 * registration gave it (the technique, the light assignment, the LOD mode, the fade), against what DCLF
		 * derives without it.
		 */
		void NoteDerived(const RE::BSGeometry& a_geometry, const LightingDescriptors& a_descriptors, const AccumulatedPass& a_accumulated,
			Ineligible a_reason, std::uint32_t a_derivedLodRow);

		/**
		 * @brief Render thread, after the sun's Accumulate: the pass descriptor's ShadowDir and DefShadow bits (13, 14)
		 * GetRenderPasses would give the geometry's main pass this frame, for a geometry no shadowed point light reaches;
		 * ~0u when the cascades are not known this frame. The engine's rule (dclf-gpu-driven-frame.md, "Phase 1, step 2"),
		 * with the sun's mask replaced by the cascade test.
		 */
		static std::uint32_t SunShadowBits(const RE::BSGeometry& a_geometry);

		/**
		 * @brief The main pass GetRenderPasses would register for the geometry this frame, built without it: the
		 * derived pass descriptor with the sun's bits, the batch list, the accumulation hint and the LOD row. False for
		 * what it does not model (translucent or fading objects, an unknown cascade test, a descriptor not derived).
		 */
		static bool SyntheticPass(const RE::BSGeometry& a_geometry, std::uint32_t a_derivedPass, AccumulatedPass& a_out);

		/**
		 * @brief The accumulate phase, render thread: this frame's synthetic passes, one per visible geometry under a
		 * left-out entry (SyntheticPass). One the model cannot build this frame is counted, and is a hole.
		 */
		const std::vector<std::pair<const RE::BSGeometry*, AccumulatedPass>>& BuildSyntheticPasses();
		/** @brief The geometries BuildSyntheticPasses gave a pass this frame, for the hole test. */
		const std::vector<std::pair<const RE::BSGeometry*, AccumulatedPass>>& SyntheticPasses() const { return synthetic; }
		/** @brief The hole test (IndirectDraws::PublishClaims): a synthetic pass the colour epoch did not draw. */
		void CountHole() { ++cutStats.holes; }

		void Report(std::uint32_t a_frame, std::uint32_t a_interval);

	private:
		PrimaryCull() = default;

		struct Hooks;
		friend struct Hooks;

		/** @brief After the full-frustum cull, render thread: the census of the lists. */
		void AfterFullFrustum();
		/** @brief After the list jobs' Finish, render thread. */
		void AfterListJobs();

		bool installed = false;
		std::atomic<bool> counting{ false };

		struct Census
		{
			std::uint64_t frames = 0;
			std::uint64_t lists = 0;
			std::uint64_t entries = 0;          // all lists' entries
			std::uint64_t firstEntries = 0;     // entry 0 of each list (never removable)
			std::uint64_t candidates = 0;       // entries that are sun candidates
			std::uint64_t candidateGeometries = 0;  // tracked geometries under those
			std::uint64_t actorEntries = 0;     // entries whose reference is an actor
			std::uint64_t extraEntries = 0;     // the first job's extra list (DAT_14338c888)
			std::uint64_t sunCandidates = 0;    // the sun candidates' size, for the share found in the lists
			std::map<std::string, std::uint64_t> others;  // not a candidate: "RTTI / parent" -> count
		};
		Census census;

		// This frame's listed candidate entries (by candidate entry index), for the registration counts.
		std::shared_ptr<const SunCandidates> frameCandidates;
		std::vector<std::uint8_t> listed;  // per candidate entry index

		struct Registrations
		{
			std::atomic<std::uint64_t> main{ 0 }, mainUnder{ 0 };
			std::atomic<std::uint64_t> depth{ 0 }, depthUnder{ 0 };
			std::atomic<std::uint64_t> other{ 0 };
		};
		Registrations registrations;

		/** @brief NoteDerived's counters (render thread). */
		struct Derived
		{
			std::uint64_t objects = 0;
			std::uint64_t notDerived = 0;               // the derivation leaves it native
			std::uint64_t ineligible = 0;               // this frame's verdict is not None
			std::array<std::uint64_t, 40> byReason{};   // ... by Ineligible
			std::uint64_t differ = 0;                   // derived pass descriptor != registered, outside bits 13 and 14
			std::uint64_t sunOnly = 0;                  // ... only in bits 13 and 14
			std::array<std::uint64_t, 32> bits{};       // per differing bit
			std::uint64_t shadowLights = 0;             // the pass has shadowed point lights (bits 6-8, LLF's mask)
			std::uint64_t lodRowDiffer = 0;             // the fade node's LOD row != the pass's LODMode (skinned LOD)
			std::uint64_t fading = 0;                   // fading at registration
			std::uint64_t alphaMask = 0;                // AdditionalAlphaMask (screen-door fade)
			std::uint64_t specularFadeDiffer = 0;       // derived LOD fade != the property's, beyond 1e-3
			std::uint64_t envmapFadeDiffer = 0;
			std::array<std::uint64_t, 32> hints{};      // accumulation hint
			std::uint64_t sunAgree = 0;                 // SunShadowBits against the registered bits 13 and 14
			std::uint64_t sunEngineOnly = 0;
			std::uint64_t sunDclfOnly = 0;
			std::uint64_t sunOther = 0;                 // the two bits disagree in another way (one of them)
			std::uint64_t sunUnknown = 0;               // the cascades were not known
			std::map<std::string, std::uint64_t> sunSamples;
			std::uint64_t synthAgree = 0;               // SyntheticPass against the captured pass, every field
			std::uint64_t synthUnmodeled = 0;
			std::uint64_t synthTechnique = 0, synthSubPass = 0, synthHint = 0, synthLodRow = 0;
			std::map<std::string, std::uint64_t> synthSamples;
			std::map<std::string, std::uint64_t> samples;  // a few differing objects
		};
		Derived derived;
		std::string playerChain;

		/** @brief Per candidate entry, what the snapshot and a walk of its subtree say (once per generation). */
		enum class EntryPlan : std::uint8_t
		{
			Unknown,
			Plain,     // no node under it needs OnVisible, the root included
			FadeRoot,  // the root is a BSFadeNode (exactly): DCLF runs its OnVisible fade update (ServiceFade); the rest plain
			LeafRoot,  // ... a BSLeafAnimNode: its LOD step, then the fade update
			Rejected,
		};
		EntryPlan PlanOf(std::uint32_t a_entry, const RE::NiAVObject* a_root);
		void FilterLists();
		/**
		 * @brief BSFadeNode::OnVisible (AE 0x141479f50) and BSLeafAnimNode::OnVisible (0x14147c9c0) for the main camera,
		 * without the recursion: the fade and LOD state the engine's cull would have updated. Returns whether OnVisible
		 * would have gone on into the children (false once the node has faded out).
		 */
		static bool ServiceFade(RE::NiAVObject* a_node, bool a_leaf, const RE::NiCamera& a_camera);
		void RestoreLists();

		struct Cut
		{
			std::shared_ptr<const SunCandidates> candidates;  // the snapshot the plans and the frame's exclusion are for
			std::vector<EntryPlan> plans;                     // per candidate entry index
			std::vector<std::uint32_t> geometryOffsets;       // per candidate entry index: its geometries in geometryIndices
			std::vector<const RE::BSGeometry*> geometryIndices;
			std::vector<const RE::NiAVObject*> roots;         // per candidate entry index
			ankerl::unordered_dense::set<const RE::NiAVObject*> admitted;  // entries DCLF has drawn in full (see the class)

			// This frame's filter: each list's entries as they were, and the removed ones (kept alive until restored).
			// Per list, its size before the filter and where its removed entries start in `removed` (by position, ascending).
			std::vector<std::uint32_t> originalSize, removedBegin;
			std::vector<std::pair<std::uint32_t, RE::NiPointer<RE::NiAVObject>>> removed;
			std::vector<std::uint32_t> excluded;  // candidate entry indices left out this frame
			bool filtered = false;

		};
		Cut cut;
		/** @brief The cut's counters since the report. */
		struct CutStats
		{
			std::uint64_t frames = 0, appliedFrames = 0;
			std::uint64_t skippedStale = 0, skippedPreconditions = 0;
			std::uint64_t entries = 0, removed = 0, visibleEntries = 0;
			std::uint64_t rejectedPlan = 0, notSettled = 0, notAdmitted = 0;
			std::uint64_t synthetic = 0, unmodelled = 0, hiddenSkipped = 0, localShadowed = 0;
			std::uint64_t holes = 0;
			std::uint64_t fadeServiced = 0, fadedOut = 0, handedBack = 0;  // ServiceFade calls; roots faded out; geometries given to the engine
			std::int64_t filterTicks = 0, afterTicks = 0, synthTicks = 0;
			// [TEMP] why entries stay in: plan rejections by cause, and the fade roots' state.
			std::map<std::string, std::uint64_t> planReasons;
			std::map<std::string, std::uint64_t> fadeStates;
		};
		CutStats cutStats;
		std::vector<const RE::BSGeometry*> frameVisible;  // this frame's visible geometries under left-out entries
		std::vector<std::pair<const RE::BSGeometry*, AccumulatedPass>> synthetic;

		/** @brief The derived pass descriptor per geometry, recomputed when what it reads changes. */
		struct DerivedEntry
		{
			const void* property = nullptr;
			const void* material = nullptr;
			std::uint64_t flags = 0;
			std::uint8_t fadeState = 0;
			std::uint32_t derivedPass = kNotDerived;
		};
		ankerl::unordered_dense::map<const RE::BSGeometry*, DerivedEntry> derivedCache;
	};
}
