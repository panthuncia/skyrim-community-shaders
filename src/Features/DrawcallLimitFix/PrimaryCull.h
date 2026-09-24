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
	 * The cut (toggle `excludePrimaryEntries`, CS_DCLF_PRIMARY_EXCLUDE): the list processes' Process1 (vtable slot 0x16,
	 * shared with every other list process, so filtered by process) stands in for the cull of every eligible entry, on
	 * the list job's own thread. For an admitted, settled entry in view (the job's own planes) it:
	 *   - runs the root's OnVisible update: the fade (ServiceFade), a leaf node's LOD step, a tree's height test and
	 *     LOD fix-up (ServiceTree), and the visibility bit (kAccumulated) the tree clock reads;
	 *   - collects its geometries that DCLF draws (the snapshot's per-geometry verdict, SunCandidates::primaryGeometry)
	 *     for synthetic main passes (SyntheticPass, built on the worker and taken by the accumulate phase);
	 *   - hands every other geometry in view to the engine's registration as the cull would (the process's
	 *     AppendVirtual, in the traversal's order), so decals, effects and blended objects register as before.
	 * Switch nodes are followed per frame (a member is drawn while every switch above it selects its path); a switch
	 * whose selected child is out of date, and a root that is fading or cross-fading LOD, leave the entry to the
	 * engine's Process1 that frame. An entry is eligible when its nodes are plain (NiNode, BSMultiBoundNode, switch
	 * nodes; a fade, leaf or tree root) and its geometries use BSGeometry's OnVisible, with at least one DCLF draws;
	 * it is admitted once the colour epoch has drawn all of those (Admit). After the jobs the render thread only
	 * gathers the jobs' output and clears the activeLightMask of what DCLF draws, as the main registration would.
	 * Frame preconditions: the sun's entry exclusion is live (its cascades are captured) and no local light cast
	 * shadows last frame (a synthetic pass has no point-light shadow).
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
		/** @brief [TEMP] CS_DCLF_PRIMARY_REASONS=1: what keeps entries in view in the engine's cull, by cause. */
		static bool ReasonsProbe();

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
		 * @brief The bits SunShadowBits gives a geometry whose bound meets a cascade: what depends on the object and
		 * the frame's globals alone. The GPU makes the cascade test (kObjectSunTest).
		 */
		static std::uint32_t SunShadowStatic(const RE::BSGeometry& a_geometry);
		/** @brief CS_DCLF_SUN_GPU (default on): synthetic passes take the static bits and the GPU's cascade test. */
		static bool SunOnGpu();
		/**
		 * @brief CS_DCLF_FEEDBACK (default on): the stood-in roots' fade, LOD and tree-clock state is updated from the
		 * GPU's visibility feedback on DCLF's worker, not in the list jobs (dclf-cull-job-elimination.md, "Phase 2").
		 */
		static bool FeedbackOn();
		/** @brief [TEMP] CS_DCLF_FEEDBACK_PROBE=1: the tree clock under the feedback. */
		static bool FeedbackProbe();
		/**
		 * @brief The colour commit, render thread (IndirectDraws::ArmFeedback): this frame's stood-in entries, carried
		 * with the frame's feedback copy to its decode. Null when the cut did not apply this frame.
		 */
		std::shared_ptr<void> TakeFeedbackTag() { return std::exchange(pendingTag, {}); }
		/**
		 * @brief The accumulate phase, render thread: gives an engine-registered pass the synthetic pass's sun bits (the
		 * static rule, and kObjectSunTest for the GPU's cascade test) when the geometry is one PrimaryCull draws
		 * synthetically, so the two sources of its pass share one pipeline key. Drawing it before admission is then
		 * the evidence that its synthetic pass can be drawn. Only on a frame the cut applies (no local shadow light).
		 */
		void UnifySunBits(const RE::BSGeometry* a_geometry, AccumulatedPass& a_pass) const;

		/**
		 * @brief The main pass GetRenderPasses would register for the geometry this frame, built without it: the
		 * derived pass descriptor with the sun's bits, the batch list, the accumulation hint and the LOD row. False for
		 * what it does not model (translucent or fading objects, an unknown cascade test, a descriptor not derived).
		 */
		static bool SyntheticPass(const RE::BSGeometry& a_geometry, std::uint32_t a_derivedPass, AccumulatedPass& a_out, bool a_sunOnGpu = false);

		/**
		 * @brief The accumulate phase, render thread: this frame's synthetic passes, one per visible geometry under a
		 * left-out entry (SyntheticPass). One the model cannot build this frame is counted, and is a hole.
		 */
		const std::vector<std::pair<const RE::BSGeometry*, AccumulatedPass>>& BuildSyntheticPasses();
		/** @brief The geometries BuildSyntheticPasses gave a pass this frame, for the hole test. */
		const std::vector<std::pair<const RE::BSGeometry*, AccumulatedPass>>& SyntheticPasses() const { return synthetic; }
		/** @brief The hole test (IndirectDraws::PublishClaims): a synthetic pass the colour epoch did not draw. */
		void CountHole() { ++cutStats.holes; }
		/**
		 * @brief After the colour epoch, render thread: the entries the cull reached in view this frame whose every geometry
		 * the epoch drew (a_drawn) are admitted, and are left out from the next frame on.
		 */
		template <class Drawn>
		void Admit(Drawn&& a_drawn)
		{
			for (const std::uint32_t e : cut.pendingAdmission) {
				if (e >= cut.admitted.size() || cut.admitted[e])
					continue;
				bool all = true;
				for (std::uint32_t m = cut.memberOffsets[e]; m < cut.memberOffsets[e + 1] && all; ++m)
					all = cut.members[m].engine || !MemberShown(cut.members[m], cut.roots[e]) || a_drawn(cut.members[m].geometry);
				if (all) {
					cut.admitted[e] = 1;
					cut.admittedRoots.insert(cut.roots[e]);
					++cutStats.admittedNow;
				}
			}
			cut.pendingAdmission.clear();
		}

		void Report(std::uint32_t a_frame, std::uint32_t a_interval);
		/** @brief At Present, render thread: the frame's feedback decode is joined (the next frame's update reads what it wrote). */
		void EndFrame() { JoinFeedback(); }

	private:
		struct Cut;
		/** @brief The switch node's selected child index, read at the engine's offset (SceneStore::ReadSwitch; CommonLib's is not AE's). */
		static std::int32_t SwitchIndex(const RE::NiSwitchNode* a_switch);
		/** @brief Whether the cull would reach a member: not app-culled up to the root, and every switch above it selects its path. */
		bool MemberShown(const auto& a_member, const RE::NiAVObject* a_root) const
		{
			for (std::uint32_t w = a_member.switchBegin; w < a_member.switchEnd; ++w)
				if (SwitchIndex(cut.switchPaths[w].first) != cut.switchPaths[w].second)
					return false;
			for (const RE::NiAVObject* object = a_member.geometry; object; object = object == a_root ? nullptr : object->parent)
				if (object->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					return false;
			return true;
		}
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
			Plain,     // no node under it needs OnVisible, the root included
			FadeRoot,  // the root is a BSFadeNode (exactly): DCLF runs its OnVisible fade update (ServiceFade); the rest plain
			LeafRoot,  // ... a BSLeafAnimNode: its LOD step, then the fade update
			TreeRoot,  // ... a BSTreeNode: its height test, the leaf node's update, its LOD fix-up (ServiceTree)
			Rejected,
		};
		/** @brief The entry's plan; its members are appended to cut.members (not for a rejected entry). */
		EntryPlan PlanOf(std::uint32_t a_entry, const RE::NiAVObject* a_root);
		std::uintptr_t geometryOnVisible = 0;  // BSGeometry's OnVisible (vtable slot 0x34): a member's must be it
		std::string lastCause;  // [TEMP] PlanOf's reason for its last rejection
		/**
		 * @brief BSFadeNode::OnVisible (AE 0x141479f50) and BSLeafAnimNode::OnVisible (0x14147c9c0) for the main camera,
		 * without the recursion: the fade and LOD state the engine's cull would have updated. Returns whether OnVisible
		 * would have gone on into the children (false once the node has faded out).
		 */
		static bool ServiceFade(RE::NiAVObject* a_node, bool a_leaf, const RE::NiCamera& a_camera);
		/** @brief BSTreeNode::OnVisible (AE 0x14147d3c0) without the recursion: false when the tree is not drawn. */
		static bool ServiceTree(RE::NiAVObject* a_node, const RE::NiCullingProcess& a_process);
		/** @brief BSTreeNode::OnVisible's height test: true when the tree is above the limit (not drawn, not updated). */
		static bool TreeAboveLimit(const RE::NiAVObject* a_node, const RE::NiCullingProcess& a_process);
		/** @brief BSTreeNode::OnVisible past its height test: the leaf update and the LOD fix-up. */
		static bool ServiceTreeState(RE::NiAVObject* a_node, const RE::NiCamera& a_camera);
		/** @brief The decode (worker): one frame's feedback, applied to the entries stood in for in that frame. */
		void ConsumeFeedback(std::uint32_t a_stamp, std::uint32_t a_objects, const std::uint32_t* a_words, const std::shared_ptr<void>& a_tag);
		/** @brief Render thread, before the list jobs: the last decode job is done (it wrote the nodes they read). */
		void JoinFeedback();
		/** @brief Render thread, after the synthetic passes' join: the decode of the completed feedback frames, on the worker. */
		void KickFeedbackDecode();
		/** @brief BuildSyntheticPasses without the decode's kick: the worker's passes joined, or built here. */
		const std::vector<std::pair<const RE::BSGeometry*, AccumulatedPass>>& JoinSyntheticPasses();
		/** @brief Render thread, after the full-frustum cull: this frame's preconditions, and a new snapshot's plans. */
		void PrepareFrame();
		/** @brief The list process's index among the scene lists', or -1 (any other process sharing the vtable). */
		int SlotOf(const RE::NiCullingProcess* a_process) const;
		/**
		 * @brief A list job's Process1 on an object, job thread: when the object is an eligible entry, what the cull would
		 * have done, done without traversing it (the class comment). False: the engine's Process1 runs as usual.
		 */
		bool StandIn(int a_slot, RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_arg);
		/** @brief Worker or render thread: frameVisible's synthetic passes into `synthetic`. */
		void BuildSyntheticInto(std::vector<std::pair<const RE::BSGeometry*, AccumulatedPass>>& a_out, std::uint64_t& a_unmodelled);

		/**
		 * @brief The snapshot's eligible entries, as the list jobs read them. Rebuilt on the render thread before the jobs
		 * (a new snapshot) and changed only between them (admission), so the jobs read it without a lock.
		 */
		struct Cut
		{
			std::shared_ptr<const SunCandidates> candidates;  // the snapshot the plans are for
			std::vector<EntryPlan> plans;                     // per candidate entry index
			/** @brief A geometry under an eligible entry: DCLF's (a synthetic pass) or the engine's (handed to its registration). */
			struct Member
			{
				const RE::BSGeometry* geometry = nullptr;
				bool engine = false;
				// The switch nodes above it, as (switch, the child index on its path) in switchPaths: drawn only while each
				// selects that child, as NiSwitchNode::OnVisible culls only its selected child.
				std::uint32_t switchBegin = 0, switchEnd = 0;
			};
			std::vector<std::pair<const RE::NiSwitchNode*, std::int32_t>> switchPaths;
			std::vector<std::uint32_t> switchOffsets;  // per candidate entry index: its switch nodes in switches
			std::vector<const RE::NiSwitchNode*> switches;
			std::vector<std::uint32_t> memberOffsets;  // per candidate entry index: its geometries in members, in the cull's order
			std::vector<Member> members;
			std::vector<std::int32_t> memberObject;    // per member: its object index in the tables (-1: none), for the feedback
			std::vector<std::uint32_t> geometryOffsets;       // per candidate entry index: its tracked geometries in geometryIndices
			std::vector<const RE::BSGeometry*> geometryIndices;
			std::vector<const RE::NiAVObject*> roots;         // per candidate entry index
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> eligible;  // root -> entry index, plan not Rejected
			std::vector<std::uint8_t> admitted;               // per entry index: DCLF has drawn all of it (see the class)
			ankerl::unordered_dense::set<const RE::NiAVObject*> admittedRoots;  // the same by node, kept across snapshots
			std::vector<std::uint32_t> pendingAdmission;       // this frame: eligible, reached, visible, not yet admitted
			// [TEMP] CS_DCLF_PRIMARY_REASONS=1: the rejected entries by root, with their cause (an index into causes).
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> rejected;
			std::vector<std::uint16_t> causeOf;  // per entry index
			std::vector<std::string> causes;
			std::array<const RE::NiCullingProcess*, 16> processes{};  // the list processes this frame
			std::uint32_t processCount = 0;
		};
		Cut cut;
		std::atomic<bool> frameLive{ false };  // set before the list jobs when the cut applies this frame, cleared after them
		bool gpuSunFrame = false;              // the cut applies this frame (PrepareFrame), until the next full-frustum cull

		/** @brief One list job's output and counters: written by its job thread only, read after Finish. */
		struct JobOut
		{
			std::vector<const RE::BSGeometry*> visible;
			std::vector<std::uint32_t> pending;
			std::vector<std::uint32_t> stoodIn;  // entries the job left to DCLF this frame, in view or not
			std::uint64_t seen = 0, skipped = 0, visibleEntries = 0, notSettled = 0, notAdmitted = 0;
			std::uint64_t fadeServiced = 0, fadedOut = 0, handedBack = 0, hidden = 0, engineMembers = 0, switchStale = 0, unselected = 0;
			std::array<std::uint64_t, 64> causeGeometries{};  // [TEMP] geometries under rejected entries in view, by cause
		};
		std::array<JobOut, 16> jobOut;

		/** @brief The cut's counters since the report (render thread). */
		struct CutStats
		{
			std::uint64_t frames = 0, appliedFrames = 0;
			std::uint64_t skippedStale = 0, skippedPreconditions = 0;
			std::uint64_t seen = 0, skipped = 0, visibleEntries = 0;
			std::uint64_t notSettled = 0, notAdmitted = 0, admittedNow = 0;
			std::uint64_t synthetic = 0, unmodelled = 0, hiddenSkipped = 0, localShadowed = 0;
			std::uint64_t holes = 0;
			std::uint64_t fadeServiced = 0, fadedOut = 0, handedBack = 0;  // ServiceFade calls; roots faded out; fading roots' geometries given to the engine
			std::uint64_t engineMembers = 0;  // the engine's members in view, handed to its registration
			std::uint64_t switchStale = 0;    // entries the engine culled this frame because a switch's selected child was out of date
			std::uint64_t unselected = 0;     // members under an unselected switch child
			std::uint64_t synthInline = 0, synthLate = 0;                   // synthetic passes built on the render thread; the worker was late
			std::int64_t prepareTicks = 0, afterTicks = 0, synthWaitTicks = 0;
			// [TEMP] why entries stay in: plan rejections by cause (per snapshot).
			std::map<std::string, std::uint64_t> planReasons;
			std::array<std::uint64_t, 64> causeGeometries{};
		};
		CutStats cutStats;
		std::vector<const RE::BSGeometry*> frameVisible;  // this frame's visible geometries under left-out entries
		std::vector<std::pair<const RE::BSGeometry*, AccumulatedPass>> synthetic;
		std::shared_ptr<void> synthJob;                    // the worker's synthetic-pass job (an AsyncWorker::JobHandle)
		std::shared_ptr<void> feedbackJob;                 // the worker's feedback decode (an AsyncWorker::JobHandle)
		std::shared_ptr<void> pendingTag;                  // this frame's stood-in entries, until the colour commit takes them
		/** @brief A feedback frame's tag: the entries stood in for, and the snapshot their indices belong to. */
		struct FeedbackTag
		{
			std::shared_ptr<const SunCandidates> candidates;
			std::vector<std::uint32_t> stoodIn;
			// The stood-in entries' roots, held: the decode touches them a frame or more later, when a cell unload may
			// have freed what the snapshot names. Taken while the list jobs had just traversed them (alive), released on
			// the render thread (retiredTags), never on the worker.
			std::vector<RE::NiPointer<RE::NiAVObject>> roots;
		};
		std::vector<std::uint32_t> stoodInScratch;
		std::vector<std::shared_ptr<void>> retiredTags;  // decoded frames' tags: the worker appends, the render thread clears after the join
		/** @brief The decode's counters (worker), read and reset by the report. */
		struct FeedbackCounters
		{
			std::atomic<std::uint64_t> frames{ 0 }, stale{ 0 }, entries{ 0 }, visible{ 0 }, serviced{ 0 }, unresolved{ 0 };
			// [TEMP] CS_DCLF_FEEDBACK_PROBE: stood-in trees in view, and those whose clock (+0x164) moved since the last decode.
			std::atomic<std::uint64_t> trees{ 0 }, treesAdvanced{ 0 };
		};
		ankerl::unordered_dense::map<const RE::NiAVObject*, float> treeClocks;  // [TEMP] worker only
		FeedbackCounters feedbackCounters;
		std::uint64_t synthJobUnmodelled = 0;
		std::atomic<bool> synthJobDone{ false };

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
