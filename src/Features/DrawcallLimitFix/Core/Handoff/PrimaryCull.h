#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace DCLF::Diagnostics
{
	struct PrimaryCullView;
}

namespace DCLF::SceneCapture
{
	struct LeafView;
	struct NodeRecord;
	struct PropertyRecord;
}

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Scene/Channels.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "RenderGraph/Async/EventQueue.h"

namespace DCLF
{
	struct EntrySpace;
	struct FrameGlobals;

	/**
	 * @brief Takes DCLF's objects out of the main camera's cull, and builds the scene lists without them.
	 *
	 * The engine's scene lists (DAT_14338c870, DAT_14338c868) hold reference roots. The sun's full-frustum cull reads them, then one
	 * list job per list culls them. DCLF hooks the list processes' Process1 on the job thread. For an admitted entry in view it skips
	 * the geometry DCLF draws on the GPU, hands every other geometry to the engine's registration, and leaves the root's OnVisible work
	 * to FadeStateCS. An entry is eligible only when DCLF stands in for every OnVisible under it. It is admitted once every member it
	 * shows is in the frame's set. An entry with engine-drawn parts runs the engine's Process1 whole.
	 */
	class PrimaryCull
	{
	public:
		static PrimaryCull& Get();
		/** @brief Checks every site Install writes, before any hook is written. */
		static void Preflight();

		/** @brief AE: the call sites in CalculateAndDrawShadowCasterLights, verified before patching. */
		void Install();
		bool Installed() const { return installed; }

		/** @brief The ShadowDir and DefShadow bits (13, 14) a geometry takes if its bound meets a cascade. The GPU does the cascade test. */
		static std::uint32_t SunShadowStatic(const SceneCapture::LeafView& a_leaf, const SceneCapture::PropertyRecord* a_property = nullptr);
		/** @brief CS_DCLF_RESIDENT_PARITY: the synthetic pass built from scratch, without the cache. */
		static bool FreshSyntheticPass(const SceneCapture::LeafView& a_leaf, AccumulatedPass& a_out);
		/** @brief A membership pass, or why it failed, plus the derivation cache entry to store. */
		struct Membership
		{
			bool ok = false;
			std::uint8_t fail = 0;
			MembershipDerived derived;
			AccumulatedPass pass;
		};
		/** @brief The synthetic pass an object is bound with, from the mirror's records. Pure. Reuses a_cached when its witnesses match. */
		static Membership MembershipPass(const SceneCapture::LeafView& a_leaf, const MembershipDerived& a_cached);
		/** @brief MembershipPass for a_leaf's layer, its property's pass with hint 12. */
		static Membership MembershipLayerPass(const SceneCapture::LeafView& a_leaf, const MembershipDerived& a_cached);
		/** @brief The frame globals a membership pass reads. A change rebinds every object. */
		static std::uint32_t MembershipWitness(const FrameGlobals& a_globals);
		/** @brief Render thread, at BeginSceneFrame. Takes the camera cut from the main camera so it does not depend on the list processes running. */
		void BeginFrameCamera();
		/**
		 * @brief Native, render thread, where the list jobs were queued and awaited: no engine list cull runs, so the cut's frame ends here.
		 * The observers' beforeListJobsWait and listJobsDone run as if the jobs had.
		 */
		void NoListJobs();
		/**
		 * @brief Main thread at Main::Update's entry, where no cull or animation job writes the flags. Writes the stood-in roots'
		 * kAccumulated from DCLF's visibility, then calls the stand-in observers.
		 */
		void AtUpdateEntry();
		/** @brief Whether the cut stands in for a_root. Render thread. */
		bool StoodIn(const void* a_root) const { return standInRoots.contains(a_root); }
		/** @brief Render thread. Whether a_geometry is under an entry the cut has whole. */
		bool DrawsWhole(const RE::NiAVObject* a_geometry) const
		{
			if (!standInLive)
				return false;
			for (const RE::NiAVObject* object = a_geometry; object; object = object->parent)
				if (const auto it = cut.eligible.find(object); it != cut.eligible.end())
					return it->second < cut.admitted.size() && cut.admitted[it->second] && !cut.mixed[it->second] && !cut.walk[it->second];
			return false;
		}
		/** @brief Whether a_node is an eligible entry's root in the cut's snapshot. */
		bool IsEntry(const RE::NiAVObject* a_node) const { return cut.eligible.contains(a_node); }
		/** @brief Why this thread's last SyntheticPass failed, or 0 if it succeeded. */
		static std::uint8_t LastSyntheticFail();
		static constexpr std::array<const char*, 6> kSyntheticFailNames{ "-", "not derived", "not lighting", "-", "translucent layer", "translucent" };
		/** @brief The main pass GetRenderPasses would register. False for cases not modelled, such as a translucent layer. */
		static bool SyntheticPass(const SceneCapture::LeafView& a_leaf, std::uint32_t a_derivedPass, AccumulatedPass& a_out,
			const SceneCapture::PropertyRecord* a_layer = nullptr);

		/** @brief Render thread. The geometry's entry is walked until the next frame. */
		void NoteMemberLost(const RE::BSGeometry* a_geometry);
		/** @brief Render thread. Every entry is walked until the next snapshot. */
		void NoteAllMembersLost();
		/** @brief Render thread. Samples MembershipWitness once so binding and the frame agree. */
		std::uint32_t SampleMembershipWitness();
		/** @brief Render thread, on install. A joined member's entry is checked for admission, a left member's entry is walked. */
		void NoteSetChanges(const std::vector<const RE::BSGeometry*>& a_joined, const std::vector<const RE::BSGeometry*>& a_left);
		/**
		 * @brief Brings the cut up to a_candidates. Changed entries are let go and planned again on DCLF's pool, or here when a_inline.
		 * The engine culls them meanwhile.
		 */
		void SyncCut(const std::shared_ptr<const EntrySpace>& a_candidates, std::span<const std::uint32_t> a_changed, bool a_inline = false);
		/** @brief Repacks the member and switch pools without the let-go ranges. */
		void CompactCut();
		std::size_t cutGarbage = 0;  // members of let-go ranges still in the pool
		/**
		 /**
		  * @brief Render thread, before the list jobs. Admits an entry once every DCLF member it shows is in the frame's set.
		  * An entry with an untracked member waits a frame, since its geometry may be released.
		  */
		void RunAdmission();

		/**
		 * @brief Any thread. In the frame's set with the main phase, so the engine's cull skips it.
		 * Only an optimisation, since PassCapture withholds a member's passes anyway.
		 */
		bool Owned(const RE::BSGeometry& a_geometry) const;

		/** @brief Render thread. The cut's counters since the report. */
		struct CutCounters
		{
			std::uint64_t frames = 0, appliedFrames = 0;
			std::uint64_t skippedStale = 0, skippedPreconditions = 0;
			std::uint64_t entriesPlanned = 0, compactions = 0;
			std::uint64_t seen = 0, skipped = 0, visibleEntries = 0;
			std::uint64_t notSettled = 0, notAdmitted = 0, admittedNow = 0;
			std::uint64_t members = 0, unbound = 0, hiddenSkipped = 0;
			std::uint64_t undrawable = 0;  // bound members in view not in the set, handed to the engine
			std::uint64_t walked = 0;
			std::uint64_t excluded = 0;  // owned geometries the engine's cull reached and did not register
			std::uint64_t mixed = 0;  // admitted entries with engine-drawn parts
			std::uint64_t engineMembers = 0;
			std::uint64_t switchStale = 0;  // culled by the engine because a switch's selected child was stale
			std::uint64_t unselected = 0;
			std::uint64_t liveAll = 0;
			std::uint64_t liveEntries = 0;
			// Pool plans kicked, spliced, stale and refused, plus entries waiting summed over frames.
			std::uint64_t plansKicked = 0, plansSpliced = 0, plansStale = 0, plansRefused = 0, awaiting = 0;
			std::int64_t prepareTicks = 0, afterTicks = 0;
		};
		CutCounters TakeCutCounters() { return std::exchange(cutStats, {}); }
		/** @brief Render thread. The scene lists' counters since the last call. */
		struct ListCounters
		{
			std::uint64_t frames = 0, published = 0, built = 0;
			std::uint64_t notToggled = 0, notCurrent = 0, noExclusion = 0, noOcclusion = 0;
			std::uint64_t restored = 0, restoreDetached = 0, unexcluded = 0, lostWhileOut = 0, engineFrames = 0, notKeepable = 0;
			std::array<std::uint64_t, 5> notKeepableBy{};
			std::uint64_t filtered = 0, entries = 0, removed = 0, kept = 0, rebuilt = 0, lateRebuilds = 0, rebuildTicks = 0, rebuildEntries = 0;
			std::uint64_t rebuildStructure = 0, rebuildFilter = 0, rebuildWitness = 0, rebuildDirty = 0;
			std::array<std::uint64_t, 4> structureBy{};
			std::uint64_t eventsApplied = 0, eventsAdded = 0, eventsRemoved = 0, eventsRebuilt = 0;
			std::uint64_t filterJudged = 0, filterInserted = 0, filterErased = 0;
			std::uint64_t deltaKept = 0, deltaOut = 0, deltaIn = 0, deltaRebuilt = 0;
			std::uint64_t reasonFrames = 0;
			std::array<std::uint64_t, 6> keptBy{};
		};
		ListCounters TakeListCounters();
		/** @brief Why an entry stays in the scene lists, in the order judged. kRemovable means left out. */
		enum ListReason : std::uint8_t
		{
			kRemovable,
			kNoEntry,  // no root, or not eligible
			kNotAdmitted,
			kMixed,  // engine-drawn parts
			kWalked,  // a member not in the set
			kNotExcluded,  // the sun's exclusion keeps it in the cascades
		};
		static constexpr std::array<const char*, 6> kListReasonNames{ "left out", "not eligible", "not admitted", "mixed", "walked", "not sun-excluded" };
		/** @brief Render thread, at Present. */
		void EndFrame()
		{
			Extensions::ShadowSourcesFrameEnd();
			listFilter.store(nullptr, std::memory_order_release);
			listMode.store(ListMode::Engine, std::memory_order_release);
			// One may hold a detached cell's last reference, so they are let go on the main thread after the update's frame jobs.
			if (!listGraveyard.empty())
				EngineReleases::PushOwner(std::make_shared<std::vector<RE::NiPointer<RE::NiAVObject>>>(std::exchange(listGraveyard, {})));
		}

		/**
		 * @brief Render thread, at the scene phase's end. Publishes which roots this frame's scene lists leave out.
		 * Admitted, unwalked entries leave the lists when the full-frustum exclusion removes them from the cascades, since DCLF draws
		 * both occlusion maps. Depending on the mode, the lists are kept, rebuilt by DCLF, or engine-built and filtered.
		 */
		void PublishListFilter();
		/** @brief Render thread. Whether this frame's scene lists leave DCLF's roots out. */
		bool ListsFiltered() const { return listsFiltered.load(std::memory_order_acquire); }
		/**
		 * @brief Render thread. The engine is drawing an occlusion map whose Precipitation::SetupMask culls the lists, so put the left-out
		 * roots back first.
		 */
		void RestoreSceneLists();
		/** @brief Any thread. A root attached or detached under a category node is queued. Anything higher rebuilds the kept lists. */
		void NoteListStructure(const RE::NiNode* a_parent, RE::NiAVObject* a_child, bool a_attached);
		/** @brief Render thread. A hidden bit on a structure node rebuilds the kept lists. */
		void NoteHiddenKey(const void* a_key)
		{
			if (listStructural.contains(a_key)) {
				listStructure.fetch_add(1, std::memory_order_relaxed);
				listStats.structureBy[3].fetch_add(1, std::memory_order_relaxed);
			}
		}

	private:
		struct Cut;
		/** @brief The selected child index at AE's offset. CommonLib's offset is wrong for AE. */
		static std::int32_t SwitchIndex(const RE::NiSwitchNode* a_switch);
		/** @brief Whether every switch above the member selects its path. */
		bool PathSelected(const auto& a_member) const
		{
			for (std::uint32_t w = a_member.switchBegin; w < a_member.switchEnd; ++w)
				if (SwitchIndex(cut.switchPaths[w].first) != cut.switchPaths[w].second)
					return false;
			return true;
		}
		/** @brief Whether the cull would reach the member: not hidden up to the root and selected by every switch above it. */
		bool MemberShown(std::uint32_t a_m, const RE::NiAVObject* a_root) const
		{
			const auto& member = cut.members[a_m];
			if (!(cut.liveEvents ? cut.memberLive[a_m] != 0 : PathSelected(member)))
				return false;
			for (const RE::NiAVObject* object = member.geometry; object; object = object == a_root ? nullptr : object->parent)
				if (object->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					return false;
			return true;
		}
		/** @brief A set hash of an entry's DCLF members in the current snapshot. */
		std::uint64_t MemberSignature(std::uint32_t a_e) const;
		/** @brief Render thread, before the list jobs. Refreshes an entry's memberLive from the switches. */
		void RefreshLive(std::uint32_t a_e);
		PrimaryCull() = default;

		struct Hooks;
		friend struct Hooks;
		friend struct Diagnostics::PrimaryCullView;

		/** @brief Render thread, after the full-frustum cull. */
		void AfterFullFrustum();
		struct ListHooks;
		friend struct ListHooks;
		void InstallSceneLists();
		static void PreflightSceneLists();
		enum class ListMode : std::uint8_t
		{
			Engine,  // DCLF's roots filtered out after the engine's build
			Rebuild,  // DCLF builds them
			Keep,  // as the last frame left them
		};
		/** @brief The roots left out for one snapshot, immutable once published. Each version is a delta from its base. */
		struct ListFilter
		{
			std::shared_ptr<const EntrySpace> candidates;
			ankerl::unordered_dense::set<const RE::NiAVObject*> roots;
			std::uint64_t version = 0, base = 0;  // base is the version the delta is from, 0 for none
			std::vector<const RE::NiAVObject*> inserted, erased;
		};
		/** @brief Render thread. The current filter, or a new version when an entry's verdict moved. */
		std::shared_ptr<const ListFilter> CurrentListFilter();
		/** @brief CurrentListFilter's preconditions. False means no filter. */
		bool ListFilterReady(bool& a_sunOff, Api::ShadowListExclusion& a_exclusion);
		bool MarkFilterDirty(std::uint32_t a_entries, bool a_sunOff, Api::ShadowListExclusion& a_exclusion);
		std::shared_ptr<ListFilter> JudgeFilterEntries(std::uint32_t a_entries, bool a_sunOff, bool a_all);
		/** @brief Render thread. An entry's verdict may have moved, so judge it again in the next filter. */
		void NoteFilterEntry(std::uint32_t a_e) { filterDirty.push_back(a_e); }
		/** @brief The build's job, Keep mode. Applies a filter's delta to the kept lists. False when they need a rebuild. */
		bool ApplyFilterDelta(const ListFilter& a_filter);
		/** @brief Whether the exterior lists can be kept, and the globals' witness. */
		bool ListsKeepable(std::uint64_t& a_witness);
		/** @brief The build's job. DCLF's build of the object root's part, less the filter's roots. */
		void RebuildSceneLists(const ListFilter* a_filter);
		/** @brief Visits the object root's roots, hidden included, and the structural nodes that decide them. */
		template <class List, class Structural>
		static void EnumerateSceneLists(const RE::NiAVObject* a_objectRoot, bool a_skipCategory2, List&& a_list, Structural&& a_structural);
		/** @brief The build's job. The engine build's object-root work besides the lists, such as kAccumulated. */
		void ListUpkeep();
		/** @brief The build's job. The object root and its roots. False without a scene or TES. */
		static bool ListRoots(const RE::NiAVObject*& a_objectRoot, std::vector<const RE::NiAVObject*>& a_roots);
		/** @brief DrawWorld_BuildSceneLists' job, after the engine's build. Removes the published roots. */
		void FilterSceneLists();
		/** @brief Whether a_list is one of the scene lists, or with a_extra the extra list. */
		static bool IsSceneList(const void* a_list, bool a_extra);

		std::atomic<ListMode> listMode{ ListMode::Engine };  // this frame's, read by the list jobs' hooks
		std::atomic<std::shared_ptr<const ListFilter>> listFilter;  // this frame's, read by the build's job
		std::shared_ptr<const ListFilter> listFilterBuilt;  // republished while nothing moved
		std::shared_ptr<const ListFilter> listFilterKept;  // written by the build's job
		std::vector<std::uint8_t> listReason;
		std::vector<const RE::NiAVObject*> listReasonRoot;
		std::array<std::uint64_t, 6> listReasonCount{};
		std::vector<std::uint32_t> filterDirty;
		bool filterDirtyAll = true;
		Api::ShadowListExclusion filterExclusion;
		bool filterSunOff = false;
		std::uint64_t listFilterVersion = 0;
		std::vector<std::uint8_t> filterScratch;  // the build's job
		std::atomic<bool> listsFiltered{ false };
		std::atomic<bool> listsDirty{ true };  // the lists are not DCLF's build
		std::atomic<std::uint32_t> listStructure{ 0 };
		std::uint32_t listStructureBuilt = ~0u;  // written by the build's job, read by the render thread after Finish
		std::uint64_t listWitness = 0, listWitnessBuilt = ~0ull;
		// Written by the build's job, read by the render thread between frames.
		ankerl::unordered_dense::set<const void*> listStructural;
		std::atomic<const RE::NiAVObject*> listObjectRoot{ nullptr };
		std::atomic<const RE::NiAVObject*> listWholeEntries[2]{ nullptr, nullptr };
		// Each list starts with an empty, never-hidden node so each cull's Process2 sets its frustum up.
		std::array<RE::NiPointer<RE::NiNode>, 16> listSentinels;
		std::array<std::uint32_t, 16> listKeptSize{};  // the build's job's part. The engine's per-frame entries follow
		bool skipObjectRoot = false;  // the build's job: ObjectRootAsNode returns null
		/** @brief A root attached under or detached from a category node, in event order. */
		struct ListEvent
		{
			RE::NiPointer<RE::NiAVObject> held;  // kept alive until applied, then buried in the graveyard
			const RE::NiAVObject* child = nullptr;
			const RE::NiAVObject* parent = nullptr;
			bool attached = false;
		};
		RenderGraph::Frame::EventQueue<ListEvent> listEvents;
		/** @brief The build's job, Keep mode. Applies the queued roots. False when the lists need a rebuild. */
		bool ApplyListEvents(const ListFilter* a_filter);
		/** @brief The build's job. Drops the queued events, since the build reads the current structure. */
		void DropListEvents();
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint16_t> listCategories;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::pair<std::uint32_t, std::uint32_t>> listPositions;
		std::uint32_t listNext = 0;
		std::vector<RE::NiPointer<RE::NiAVObject>> listGraveyard;  // the build's job fills, Present hands to the retire queue
		/** @brief The build's job. Empties the scene lists, keeping their references for the graveyard. */
		void BuryLists();
		bool listsKeepInstalled = false;
		struct ListStats
		{
			std::uint64_t frames = 0, published = 0, built = 0;
			std::uint64_t notToggled = 0, notCurrent = 0, noExclusion = 0, noOcclusion = 0;
			std::uint64_t restored = 0;
			std::uint64_t restoreDetached = 0;  // roots detached since the snapshot, not restored
			std::uint64_t unexcluded = 0;  // must be 0: filtered lists without a full-frustum exclusion
			std::uint64_t lostWhileOut = 0;
			std::uint64_t engineFrames = 0, notKeepable = 0;
			std::array<std::uint64_t, 5> notKeepableBy{};
			std::atomic<std::uint64_t> filtered{ 0 }, entries{ 0 }, removed{ 0 };  // the build's job
			std::atomic<std::uint64_t> kept{ 0 }, rebuilt{ 0 }, lateRebuilds{ 0 }, rebuildTicks{ 0 }, rebuildEntries{ 0 };
			std::uint64_t rebuildStructure = 0, rebuildFilter = 0, rebuildWitness = 0, rebuildDirty = 0;
			std::array<std::atomic<std::uint64_t>, 4> structureBy{};
			std::atomic<std::uint64_t> eventsApplied{ 0 }, eventsAdded{ 0 }, eventsRemoved{ 0 }, eventsRebuilt{ 0 };  // the build's job
			std::uint64_t filterJudged = 0, filterInserted = 0, filterErased = 0;
			std::atomic<std::uint64_t> deltaKept{ 0 }, deltaOut{ 0 }, deltaIn{ 0 }, deltaRebuilt{ 0 };  // the build's job
			std::uint64_t reasonFrames = 0;
			std::array<std::uint64_t, 6> keptBy{};
		};
		ListStats listStats;
		/** @brief Render thread, after the list jobs' Finish. */
		void AfterListJobs();

		bool installed = false;
		// Native: no engine cull runs and the engine-drawn parts are not drawn, so a mixed entry is stood in like a whole one.
		bool mixedStoodIn = false;

		/** @brief Per candidate entry, from the snapshot and a walk of its subtree. */
		enum class EntryPlan : std::uint8_t
		{
			Plain,  // no node under it needs OnVisible
			FadeRoot,  // the root is exactly a BSFadeNode, so DCLF runs its fade update
			LeafRoot,  // a BSLeafAnimNode: its LOD step, then the fade update
			TreeRoot,  // a BSTreeNode: height test, leaf node update and LOD fix-up
			Rejected,
		};
		struct PlannedEntry;
		/** @brief Plans an entry from a walk of its subtree. Any thread. Reads nothing of the cut. */
		EntryPlan PlanEntry(const EntrySpace& a_candidates, const RE::NiAVObject* a_root, PlannedEntry& a_out) const;
		/** @brief PlanEntry's verdict from its walk's tallies. */
		static EntryPlan ConcludePlan(const RE::NiAVObject* a_root, const char* a_rejected, const RE::BSGeometry* a_mixedBy, std::uint32_t a_ours,
			std::uint32_t a_hidden, PlannedEntry& a_out);
		/** @brief Render thread. Puts a pool-made plan into the cut. */
		void SplicePlan(std::uint32_t a_e, PlannedEntry& a_planned);
		/** @brief Render thread, before the cut is read. Splices in the pool's plans that are still current. */
		void TakePlans();
		/** @brief Render thread. Sends the queued entries to the pool as one task, a lease per entry. */
		void KickPlans();
		std::uintptr_t geometryOnVisible = 0;  // vtable slot 0x34, read from BSTriShape's vtable
		/**
		 * @brief OnVisibles (slot 0x34) whose cull side effects DCLF makes itself or does not need, read from vtables at Install.
		 * See skyrim-engine-notes.md, "the OnVisibles a skipped entry no longer runs".
		 */
		struct WideOnVisible
		{
			std::uintptr_t node = 0, engineNode = 0, multiBound = 0, switchNode = 0, billboard = 0, ordered = 0, dynamic = 0, subIndex = 0, segmented = 0;
		};
		WideOnVisible wideOnVisible;
		/** @brief BSTreeNode::OnVisible's height test. True when the tree is above the limit and neither drawn nor updated. */
		static bool TreeAboveLimit(const RE::NiAVObject* a_node, const RE::NiCullingProcess& a_process);
		/** @brief Render thread, after the full-frustum cull. */
		void PrepareFrame();
		/** @brief The list process's index among the scene lists', or -1 for another process sharing the vtable. */
		int SlotOf(const RE::NiCullingProcess* a_process) const;
		/** @brief Job thread, in a list job's Process1. Does what the cull would for an eligible entry without traversing it. False runs the engine's. */
		bool StandIn(int a_slot, RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_arg);

		/** @brief The snapshot's eligible entries. Changed only between the jobs, so the jobs read it without a lock. */
		struct Cut
		{
			std::shared_ptr<const EntrySpace> candidates;
			std::vector<EntryPlan> plans;
			/** @brief A geometry under an eligible entry, either DCLF's or the engine's. */
			struct Member
			{
				const RE::BSGeometry* geometry = nullptr;
				bool engine = false;
				// The switches above it. Drawn only while each selects this path, as NiSwitchNode::OnVisible culls only its selected child.
				std::uint32_t switchBegin = 0, switchEnd = 0;
			};
			std::vector<std::pair<const RE::NiSwitchNode*, std::int32_t>> switchPaths;
			// Per entry index, its ranges in switches and members, in cull order.
			std::vector<std::uint32_t> switchBegin, switchEnd;
			std::vector<const RE::NiSwitchNode*> switches;
			std::vector<std::uint32_t> memberBegin, memberEnd;
			std::vector<Member> members;
			std::vector<std::int32_t> memberObject;  // -1 for none
			// Per member, whether every switch above it selects its path. Kept current by switch events.
			std::vector<std::uint8_t> memberLive;
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> switchEntry;
			bool liveEvents = false;  // otherwise the jobs read the switches
			std::vector<const RE::NiAVObject*> roots;
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> eligible;  // plan not Rejected
			std::vector<std::uint8_t> admitted;  // DCLF draws all of it
			std::vector<std::uint8_t> mixed;  // some member is the engine's
			// Per entry index, whether the stand-in walks it: it has engine-drawn members, or a tracked member not in the set.
			// The engine can show such a member before any event reaches the decision.
			std::vector<std::uint8_t> walk;
			// Per entry, for the census: why it was rejected, or a mixed entry's first engine-drawn geometry.
			std::vector<const char*> rejectReason;
			std::vector<std::string> mixedBy;
			// A root whose members changed is admitted again only once the new ones are drawn.
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> admittedRoots;
			std::vector<std::uint32_t> pendingAdmission;
			// Per entry index, whether it waits for a pool plan. It counts as Rejected meanwhile, so the engine culls it.
			std::vector<std::uint8_t> awaiting;
			std::vector<std::uint64_t> ticket;
			std::vector<std::uint32_t> planQueue;
			std::array<const RE::NiCullingProcess*, 16> processes{};
			std::uint32_t processCount = 0;
		};
		Cut cut;
		enum : std::uint8_t
		{
			kPlanNone,
			kPlanQueued,
			kPlanInFlight,
		};
		/** @brief One entry's plan as the pool made it. */
		struct PlannedEntry
		{
			std::uint32_t entry = 0;
			std::uint64_t ticket = 0;
			const RE::NiAVObject* root = nullptr;
			bool planned = false;  // false if the lease was refused, so queued again
			EntryPlan plan = EntryPlan::Rejected;
			std::vector<Cut::Member> members;
			std::vector<std::pair<const RE::NiSwitchNode*, std::int32_t>> switchPaths;
			std::vector<const RE::NiSwitchNode*> switches;
			std::vector<std::int32_t> memberObject;
			const char* rejectReason = nullptr;
			std::string mixedBy;
		};
		/** @brief A kick's plans and the snapshot they were made against, which keeps their roots alive. */
		struct PlanBatch
		{
			std::shared_ptr<const EntrySpace> candidates;
			std::shared_ptr<const void> retirement;  // keeps the roots alive until taken
			std::vector<PlannedEntry> entries;
		};
		RenderGraph::Frame::EventQueue<PlanBatch*, 64> plansReady;
		std::uint64_t planTickets = 0;  // never reused
		std::size_t plansOutstanding = 0;
		std::atomic<bool> frameLive{ false };  // set before the list jobs when the cut applies, cleared after them
		bool gpuSunFrame = false;

		/** @brief One list job's output and counters. Written by its job thread, read after Finish. */
		struct JobOut
		{
			std::uint64_t inView = 0;
			std::uint64_t seen = 0, skipped = 0, visibleEntries = 0, notSettled = 0, notAdmitted = 0, walked = 0;
			std::uint64_t hidden = 0, engineMembers = 0, switchStale = 0, unselected = 0;
			std::uint64_t unbound = 0;  // not bound yet, handed to the engine
			std::uint64_t undrawable = 0;
			std::uint64_t excluded = 0;
			std::uint64_t mixed = 0;
		};
		std::array<JobOut, 16> jobOut;

		CutCounters cutStats;
		std::vector<const RE::NiAVObject*> switchChanges;  // scratch
		const void* frameCamera = nullptr;
		std::shared_ptr<const SetSnapshot> frameSet;
		bool standInLive = false;  // this frame's snapshot is current, so the stand-in runs
		bool liveStale = true;  // a frame skipped the switch changes, so memberLive is read again
		enum : std::uint8_t
		{
			kOwnNone,
			kOwnPlain,
			kOwnMixedFade,  // not stood in
			kOwnFade,
			kOwnTree,  // TreeWind's tree list takes it from the stand-in's roots
		};
		std::vector<std::uint8_t> ownState;
		std::vector<const RE::NiAVObject*> ownRoot;
		std::vector<std::uint32_t> ownDirty;
		std::uint8_t OwnStateOf(std::uint32_t a_e) const;
		/** @brief Empties every set below, and judges every entry again when the stand-in comes back. */
		void ClearRootOwnership();
		static const void* KeyOf(const void* a_root) { return a_root; }
		static const void* KeyOf(const Api::StandInRoot& a_root) { return a_root.node; }
		std::vector<Api::StandInRoot> ownedRoots;
		ankerl::unordered_dense::map<const void*, std::uint32_t> ownedRootPosition;
		// The roots the cut stands in for. No engine cull writes their kAccumulated, so AtUpdateEntry does.
		ankerl::unordered_dense::set<const void*> standInRoots;
		std::vector<const void*> standInList;
		ankerl::unordered_dense::map<const void*, std::uint32_t> standInPosition;
		std::array<float, 12> cutCamera{};
		bool cutCameraKnown = false;
		/** @brief Updates the stand-in roots and observers from the entries that changed. */
		void SyncRootOwnership();
		/** @brief Render thread, between the jobs. Recomputes an entry's walk flag. */
		void RefreshWalk(std::uint32_t a_e);
		std::vector<std::uint32_t> walkRefresh;  // refreshed at the next PrepareFrame
		std::uint32_t sampledWitness = 0;
		std::uint32_t sampledWitnessFrame = ~0u;
		/** @brief List jobs and render thread. Whether a member is in the frame's set with the main phase. */
		bool MemberDrawable(std::int32_t a_object) const;
	};
}
