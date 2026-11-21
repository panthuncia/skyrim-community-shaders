// The tree manager's animation list without DCLF's trees, and the tree list's multibound results around its loop.
// TreeAnimation.h describes it.
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeAnimation.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeList.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWalk.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "RenderGraph/Async/EventQueue.h"
#include "RenderGraph/Fatal.h"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

namespace DCLF::Features::TreeWind::TreeAnimation
{
	namespace
	{
		using namespace Engine;

		// AE 1.6.1170. The list's writers are SWF_HOOK_TREE_ANIM_*: add FUN_1404376f0, remove FUN_140437840, clear FUN_140437ae0.
		constexpr std::size_t kAnimationList = 0x50;  // BSTArray<NiAVObject*> (a reference each, taken by hand)
		constexpr std::size_t kLock = 0x18;           // BSSpinLock (owner, count), the animation list's
		constexpr std::size_t kTreeList = 0x20;         // BSTArray<BSTreeNode*>
		constexpr std::size_t kTreeLock = 0x10;         // BSSpinLock of the tree list
		constexpr std::size_t kMultiBound = 0x128;      // BSMultiBoundNode's BSMultiBound
		constexpr std::size_t kMultiBoundShape = 0x18;  // BSMultiBound's BSMultiBoundShape
		constexpr std::size_t kShapeResult = 0x10;      // the cull result FUN_140437e50 reads: 0 stale, 1 in, 2 out

		using List = RE::BSTArray<RE::NiAVObject*>;

		std::byte* Manager() { return Global<std::byte*>(TreeWind::Address::kTreeManager); }
		List& AnimationList(std::byte* a_manager) { return *reinterpret_cast<List*>(a_manager + kAnimationList); }
		RE::BSSpinLock& Lock(std::byte* a_manager, std::size_t a_offset = kLock) { return *reinterpret_cast<RE::BSSpinLock*>(a_manager + a_offset); }

		bool installed = false;
		bool enabled = false;
		ankerl::unordered_dense::set<const RE::NiAVObject*> owned;
		// Nodes DCLF took off the list, each holding the list's reference. Changed under the manager's lock from any thread.
		// Never destroyed, because nothing may release engine objects while the process is torn down.
		auto& detached = *new ankerl::unordered_dense::map<const RE::NiAVObject*, RE::NiAVObject*>();
		// Nodes the engine added (FUN_1404376f0), for the render thread to take off again if owned. Identity only.
		auto& added = *new RenderGraph::Frame::EventQueue<const RE::NiAVObject*>();
		std::atomic<std::uint32_t> held{ 0 };

		// The stood-in tree nodes, each held while stood in, and the results written for the running loop.
		ankerl::unordered_dense::map<const RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>> ownedTrees;
		void ReleaseTree(RE::NiPointer<RE::NiAVObject>& a_tree)
		{
			if (a_tree)
				EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(a_tree)));
		}
		bool running = false;
		struct WrittenResult
		{
			RE::NiPointer<RE::NiAVObject> tree;
			std::int32_t* result = nullptr;
			std::int32_t engine = 0;
		};
		std::vector<WrittenResult> written;

		struct Stats
		{
			std::atomic<std::uint64_t> engineAdds{ 0 }, engineRemoves{ 0 }, clears{ 0 };
			std::uint64_t syncs = 0, takenOff = 0, putBack = 0, takenOffAgain = 0;
			std::uint64_t treeLoops = 0, treesIn = 0, treesOut = 0, noMultiBound = 0;
			std::array<std::uint64_t, 3> replaced{};
			std::uint64_t wideIn = 0, wideOut = 0, wideNoVerdict = 0, wideNoMultiBound = 0;
			std::array<std::uint64_t, 3> wideReplaced{};
			std::uint64_t drySame = 0, dryInOut = 0, dryOutIn = 0, dryStale = 0;
		};
		Stats stats;

		std::uint32_t TakeOff(List& a_list, const ankerl::unordered_dense::set<const RE::NiAVObject*>& a_take)
		{
			std::uint32_t kept = 0, taken = 0;
			const std::uint32_t size = a_list.size();
			for (std::uint32_t i = 0; i < size; ++i) {
				auto* node = a_list[i];
				if (node && a_take.contains(node) && detached.try_emplace(node, node).second) {
					++taken;
					continue;
				}
				a_list[kept++] = node;
			}
			if (kept != size)
				a_list.resize(kept);
			held.store(static_cast<std::uint32_t>(detached.size()), std::memory_order_relaxed);
			return taken;
		}

		struct Hooks
		{
			static void Add(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
			{
				auto* node = SceneWriter::Arg<RE::NiAVObject*>(a_event, 1);
				if (node && enabled) {
					added.Push(node);
					stats.engineAdds.fetch_add(1, std::memory_order_relaxed);
				}
			}

			// A node DCLF holds off the list is forgotten with its reference, and the engine's remove is vetoed. The caller gets 0 as it would from the engine.
			static std::int32_t Remove(const SWF_EventHeader* a_event, void*)
			{
				auto* manager = SceneWriter::Arg<std::byte*>(a_event, 0);
				auto* node = SceneWriter::Arg<RE::NiAVObject*>(a_event, 1);
				RE::NiAVObject* release = nullptr;
				if (node && manager) {
					auto& lock = Lock(manager);
					lock.Lock();
					if (const auto it = detached.find(node); it != detached.end()) {
						release = it->second;
						detached.erase(it);
						held.store(static_cast<std::uint32_t>(detached.size()), std::memory_order_relaxed);
					}
					lock.Unlock();
				}
				if (!release)
					return 0;
				stats.engineRemoves.fetch_add(1, std::memory_order_relaxed);
				release->DecRefCount();
				return 1;
			}

			static void Clear(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
			{
				auto* manager = SceneWriter::Arg<std::byte*>(a_event, 0);
				std::vector<RE::NiAVObject*> release;
				if (manager) {
					auto& lock = Lock(manager);
					lock.Lock();
					for (const auto& [node, reference] : detached)
						release.push_back(reference);
					detached.clear();
					held.store(0, std::memory_order_relaxed);
					lock.Unlock();
				}
				for (auto* node : release)
					node->DecRefCount();
				stats.clears.fetch_add(1, std::memory_order_relaxed);
			}
		};

		void DrainAdds(std::byte* a_manager)
		{
			ankerl::unordered_dense::set<const RE::NiAVObject*> take;
			added.Drain([&](const RE::NiAVObject* a_node) {
				if (owned.contains(a_node))
					take.insert(a_node);
			});
			if (take.empty() || !a_manager)
				return;
			auto& lock = Lock(a_manager);
			lock.Lock();
			stats.takenOffAgain += TakeOff(AnimationList(a_manager), take);
			lock.Unlock();
		}
	}

	void Install()
	{
		if (installed)
			return;
		// The add takes the lock at +0x18 (`lea rcx, [rdi + 0x18]`) and the array at +0x50.
		if (!SceneWriter::Has(SWF_CAP_TREE_ANIM_CALLS)) {
			logger::warn("[DCLF] tree animation list: the scene writer framework has no tree animation calls; owned trees stay on the list");
			return;
		}
		const bool subscribed = SceneWriter::SubscribeCall(SWF_HOOK_TREE_ANIM_ADD, &Hooks::Add, SWF_PHASE_POST) &&
		                        SceneWriter::AddFilter(SWF_FILTER_CALL(SWF_HOOK_TREE_ANIM_REMOVE), &Hooks::Remove) &&
		                        SceneWriter::SubscribeCall(SWF_HOOK_TREE_ANIM_CLEAR, &Hooks::Clear, SWF_PHASE_POST);
		if (!subscribed)
			::RenderGraph::Fatal("Drawcall Limit Fix", "the scene writer framework refused the tree animation calls after reporting them installed");
		installed = enabled = true;
		logger::info("[DCLF] tree animation list: owned trees are taken off the tree manager's animation list");
	}

	bool Installed() { return installed; }

	void SetOwned(const std::vector<const RE::NiAVObject*>& a_roots, const std::vector<const RE::NiAVObject*>& a_trees)
	{
		if (!installed)
			return;
		ankerl::unordered_dense::map<const RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>> trees;
		for (const auto* tree : a_trees) {
			const auto it = ownedTrees.find(tree);
			trees.try_emplace(tree, it != ownedTrees.end() ? std::move(it->second) : RE::NiPointer<RE::NiAVObject>(const_cast<RE::NiAVObject*>(tree)));
		}
		for (auto& [tree, tracked] : ownedTrees)
			ReleaseTree(tracked);
		ownedTrees = std::move(trees);
		auto* manager = Manager();
		DrainAdds(manager);
		ankerl::unordered_dense::set<const RE::NiAVObject*> next(a_roots.begin(), a_roots.end());
		++stats.syncs;
		if (!manager) {
			owned = std::move(next);
			return;
		}
		std::vector<RE::NiAVObject*> putBack;
		auto& lock = Lock(manager);
		lock.Lock();
		auto& list = AnimationList(manager);
		for (auto it = detached.begin(); it != detached.end();) {
			if (next.contains(it->first)) {
				++it;
				continue;
			}
			list.push_back(it->second);
			putBack.push_back(it->second);
			it = detached.erase(it);
		}
		held.store(static_cast<std::uint32_t>(detached.size()), std::memory_order_relaxed);
		stats.takenOff += TakeOff(list, next);
		lock.Unlock();
		stats.putBack += putBack.size();
		owned = std::move(next);
	}

	void UpdateOwned(const std::vector<const RE::NiAVObject*>& a_left, const std::vector<const RE::NiAVObject*>& a_joined)
	{
		if (!installed)
			return;
		// A leaving root is matched by key alone. A joining one is checked by its class.
		for (const auto* root : a_left)
			if (const auto it = ownedTrees.find(root); it != ownedTrees.end()) {
				ReleaseTree(it->second);
				ownedTrees.erase(it);
			}
		// A joining root is alive because the cut's snapshot holds it.
		for (const auto* root : a_joined)
			if (Engine::RttiIs(root, "BSTreeNode"))
				ownedTrees.try_emplace(root, RE::NiPointer<RE::NiAVObject>(const_cast<RE::NiAVObject*>(root)));
		for (const auto* root : a_left)
			owned.erase(root);
		for (const auto* root : a_joined)
			owned.insert(root);
		auto* manager = Manager();
		DrainAdds(manager);
		++stats.syncs;
		if (!manager)
			return;
		std::vector<RE::NiAVObject*> putBack;
		ankerl::unordered_dense::set<const RE::NiAVObject*> take(a_joined.begin(), a_joined.end());
		auto& lock = Lock(manager);
		lock.Lock();
		auto& list = AnimationList(manager);
		for (const auto* root : a_left)
			if (const auto it = owned.contains(root) ? detached.end() : detached.find(root); it != detached.end()) {
				list.push_back(it->second);
				putBack.push_back(it->second);
				detached.erase(it);
			}
		held.store(static_cast<std::uint32_t>(detached.size()), std::memory_order_relaxed);
		if (!take.empty())
			stats.takenOff += TakeOff(list, take);
		lock.Unlock();
		stats.putBack += putBack.size();
	}

	void AtUpdateEntry()
	{
		running = true;
	}

	void BeforeListLoop(std::byte* a_manager)
	{
		static const bool all = ListAll();
		if (!installed || !running || !a_manager || !written.empty() || (ownedTrees.empty() && !all))
			return;
		using FirstMultiBound = std::byte* (*)(const void*);
		static const auto firstMultiBound = reinterpret_cast<FirstMultiBound>(REL::Module::get().base() + TreeWind::Address::kTreeFirstMultiBound);
		// A stood-in tree on the list is alive while the lock is held. The reference taken keeps it alive until AfterListLoop.
		auto& lock = Lock(a_manager, kTreeLock);
		lock.Lock();
		const auto write = [&](RE::NiAVObject* a_tree, bool a_stoodIn) {
			bool visible = true;
			if (a_stoodIn) {
				visible = Extensions::StandInVerdict(a_tree);
			} else if (!Extensions::ListedVerdictOf(a_tree, visible)) {
				++stats.wideNoVerdict;
				return;
			}
			auto* multiBound = firstMultiBound(a_tree);
			auto* bound = multiBound ? At<std::byte*>(multiBound, kMultiBound) : nullptr;
			auto* shape = bound ? At<std::byte*>(bound, kMultiBoundShape) : nullptr;
			if (!shape) {
				++(a_stoodIn ? stats.noMultiBound : stats.wideNoMultiBound);
				return;
			}
			auto* result = &At<std::int32_t>(shape, kShapeResult);
			if (!a_stoodIn) {
				// The engine's result is the last scene frame's cull. DCLF's is the conservative rule's, a frame or more behind.
				const std::int32_t engine = *result;
				++(engine == 0 ? stats.dryStale : (engine == 1) == visible ? stats.drySame :
											  visible                      ? stats.dryInOut :
																			 stats.dryOutIn);
			}
			written.push_back({ RE::NiPointer<RE::NiAVObject>(a_tree), result, *result });
			++(a_stoodIn ? stats.replaced : stats.wideReplaced)[std::clamp<std::int32_t>(*result, 0, 2)];
			if (a_stoodIn)
				++(visible ? stats.treesIn : stats.treesOut);
			else
				++(visible ? stats.wideIn : stats.wideOut);
			*result = visible ? 1 : 2;
		};
		if (!all) {
			// A tree the engine took off the list meanwhile is not visited, so its write is put back unread.
			for (const auto& [tree, reference] : ownedTrees)
				if (reference)
					write(reference.get(), true);
		} else {
			const auto& list = *reinterpret_cast<const List*>(a_manager + kTreeList);
			for (std::uint32_t i = 0; i < list.size(); ++i)
				if (auto* tree = list[i])
					write(tree, ownedTrees.contains(tree));
		}
		lock.Unlock();
		stats.treeLoops += written.empty() ? 0 : 1;
	}

	void AfterListLoop()
	{
		// Put the engine's values back and release the references here, on the main thread.
		for (auto& entry : written)
			*entry.result = entry.engine;
		written.clear();
	}

	Counters Take()
	{
		Counters c;
		auto* manager = Manager();
		// Read without the lock. These are only a count and a time, for the report.
		if (manager) {
			c.updateMs = At<float>(manager, 0x7C);  // the manager's own measure of its last update (FUN_140437d40)
			c.listed = std::atomic_ref<std::uint32_t>(At<std::uint32_t>(manager, kAnimationList + 0x10)).load(std::memory_order_relaxed);
		}
		c.installed = installed;
		if (!installed)
			return c;
		auto& s = stats;
		c.held = held.load(std::memory_order_relaxed);
		c.ownedTrees = ownedTrees.size();
		c.engineAdds = s.engineAdds.exchange(0), c.engineRemoves = s.engineRemoves.exchange(0), c.clears = s.clears.exchange(0);
		c.syncs = s.syncs, c.takenOff = s.takenOff, c.putBack = s.putBack, c.takenOffAgain = s.takenOffAgain;
		c.treeLoops = s.treeLoops, c.treesIn = s.treesIn, c.treesOut = s.treesOut, c.noMultiBound = s.noMultiBound, c.replaced = s.replaced;
		c.wideIn = s.wideIn, c.wideOut = s.wideOut, c.wideNoVerdict = s.wideNoVerdict, c.wideNoMultiBound = s.wideNoMultiBound, c.wideReplaced = s.wideReplaced;
		c.drySame = s.drySame, c.dryInOut = s.dryInOut, c.dryOutIn = s.dryOutIn, c.dryStale = s.dryStale;
		s.wideIn = s.wideOut = s.wideNoVerdict = s.wideNoMultiBound = s.drySame = s.dryInOut = s.dryOutIn = s.dryStale = 0;
		s.wideReplaced = {};
		s.syncs = s.takenOff = s.putBack = s.takenOffAgain = 0;
		s.treeLoops = s.treesIn = s.treesOut = s.noMultiBound = 0;
		s.replaced = {};
		return c;
	}
}

namespace DCLF::Features::TreeWind
{
	namespace
	{
		void RootsChanged(const Api::StandInRootsChange& a_change)
		{
			TreeAnimation::UpdateOwned(a_change.left, a_change.joined);
		}

		void RootsCleared()
		{
			TreeAnimation::SetOwned({}, {});
		}
	}

	void RegisterAnimation(Api::InstallContext& a_context)
	{
		if (a_context.RegisterStandInObserver({ .changed = &RootsChanged, .cleared = &RootsCleared, .updateEntry = &TreeAnimation::AtUpdateEntry }))
			TreeAnimation::Install();
		TreeList::Install();
		TreeWalk::Install(a_context);
	}
}
