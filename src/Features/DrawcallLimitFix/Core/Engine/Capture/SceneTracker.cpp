// The NiNode child edits and the event stack they and the hooks push onto.

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Core/Engine/SceneClock.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "RenderGraph/Fatal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace DCLF
{
	namespace
	{
		// SWF reports every child edit, including subclass overrides and the remove-all (FUN_140d1d630). Detaches arrive before the engine's,
		// attaches after. A move is pushed whole at its attach, so no scene pass takes its detach alone and erases what the attach keeps.
		struct MoveGroup
		{
			const RE::NiAVObject* child = nullptr;
			SceneTracker::Event* oldest = nullptr;
			SceneTracker::Event* newest = nullptr;
			std::uint32_t wakes = 0;
		};
		thread_local MoveGroup t_move;

		// Plain detaches by child. Each waits for an attach of the same child or the frame's start.
		struct HeldDetach
		{
			const RE::NiAVObject* child = nullptr;
			SceneTracker::Event* oldest = nullptr;
			SceneTracker::Event* newest = nullptr;
		};
		std::mutex heldDetachLock;
		std::vector<HeldDetach> heldDetaches;
		std::atomic<std::uint32_t> heldDetachCount{ 0 };

		// A load's detaches are not held, since no frame starts until it ends.
		bool LoadingMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		}

		void StampWoken(SceneTracker::Event* a_event)
		{
			a_event->stampNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			a_event->stampFrame = DCLF::SceneClock::Step();
		}

		void OnChildAttached(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto& event = *reinterpret_cast<const SWF_ChildEvent*>(a_event);
			auto* child = static_cast<RE::NiAVObject*>(event.child);
			// A SetAt of the child the slot already held is no structure change.
			if (!event.unchanged)
				PrimaryCull::Get().NoteListStructure(static_cast<const RE::NiNode*>(event.parent), child, true);
			SceneTracker::Get().PushAttached(child, static_cast<const RE::NiAVObject*>(event.parent));
		}

		void OnChildDetaching(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto& event = *reinterpret_cast<const SWF_ChildEvent*>(a_event);
			auto* child = static_cast<RE::NiAVObject*>(event.child);
			if (event.flags & SWF_CHILD_REPARENT) {
				// The engine's attach of this child is in progress on this thread, so no other move can be.
				if (t_move.child && t_move.child != child)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("a move of {} began on thread {} while the move of {} was open", static_cast<const void*>(child),
						::GetCurrentThreadId(), static_cast<const void*>(t_move.child)));
				t_move.child = child;
			}
			PrimaryCull::Get().NoteListStructure(static_cast<const RE::NiNode*>(event.parent), child, false);
			SceneTracker::Get().PushDetached(child, static_cast<const RE::NiAVObject*>(event.parent));
		}

		// Compared by the hooks, never dereferenced except for descendants' parent chains.
		std::atomic<const RE::NiAVObject*> watchedRoot{ nullptr };

		/** @brief Whether a_object is a_root or under it. Call on the hook's thread, where the chain is alive. */
		bool Under(const RE::NiAVObject* a_object, const RE::NiAVObject* a_root)
		{
			for (std::uint32_t depth = 0; a_object && depth <= Engine::kMaxParentDepth; ++depth, a_object = a_object->parent)
				if (a_object == a_root)
					return true;
			return false;
		}

		// An immutable sorted set the hooks read lock-free, counting themselves in. The render thread frees a replaced set once no reader is in.
		std::atomic<const std::vector<const void*>*> categoryNodes{ nullptr };
		std::atomic<std::uint32_t> categoryReaders{ 0 };  // every SetRead, not only category node reads
		// Read and retired like categoryNodes.
		std::atomic<const std::vector<const void*>*> subtreeMembers{ nullptr };
		std::unique_ptr<const std::vector<const void*>> subtreeCurrent;
		std::atomic<std::uint64_t> categoryEdits{ 0 };
		std::unique_ptr<const std::vector<const void*>> categoryCurrent;  // render thread
		std::vector<std::unique_ptr<const std::vector<const void*>>> categoryRetired;  // render thread: replaced, maybe still read
		std::atomic<std::uint32_t> renderThread{ 0 };
		// Compared by the hooks, never dereferenced.
		std::atomic<const RE::NiAVObject*> unheldOwner{ nullptr };
		std::atomic<std::uint64_t> unheldEdits{ 0 };

		std::atomic<std::uint64_t> windowEditsRender{ 0 }, windowEditsOther{ 0 };
		std::atomic<std::uint32_t> windowEditFirst{ 0 };
	}

	SceneTracker& SceneTracker::Get()
	{
		static SceneTracker tracker;
		return tracker;
	}

	void SceneTracker::Install()
	{
		if (installed)
			return;
		// PostPostLoad required SWF's structure capability before any hook was written.
		if (!SceneWriter::Subscribe(SWF_EVENT_CHILD_ATTACHED, &OnChildAttached) || !SceneWriter::Subscribe(SWF_EVENT_CHILD_DETACHING, &OnChildDetaching))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the scene writer framework refused the child-edit subscriptions after reporting them installed");
		installed = true;
		logger::info("[DCLF] Scene tracking subscribed to SceneWriterFramework's child edits");
	}

	void SceneTracker::Push(Event* a_event)
	{
		// Only edits that should reach the frame at once wake the pump and are stamped for latency: world attaches, detaches, hidden bits,
		// leaf swaps, property and alpha values. Per-frame node and geometry values wake nothing.
		SceneWake source = SceneWake::Count;
		switch (a_event->type) {
		case EventType::Attached:
			// A loader's subtree attached outside the world wakes nothing. Its world attach does.
			if (a_event->captured)
				source = SceneWake::Attach;
			break;
		case EventType::Detached:
			source = SceneWake::Detach;
			break;
		case EventType::Updated:
			if (a_event->update.hiddenSite != ~0u)
				source = SceneWake::Hidden;
			else if (a_event->update.leaf)
				source = SceneWake::Leaf;
			else if (std::holds_alternative<SceneCapture::PropertyRecord>(a_event->update.record))
				source = SceneWake::PropertyUpdate;
			else if (std::holds_alternative<SceneCapture::AlphaRecord>(a_event->update.record))
				source = SceneWake::AlphaUpdate;
			break;
		case EventType::Stacked:
			if (a_event->stackedWakes)
				source = SceneWake::Channel;
			break;
		}
		if (source != SceneWake::Count)
			StampWoken(a_event);
		if (!t_move.child) {
			PushChain(a_event, a_event, source != SceneWake::Count ? 1u << static_cast<std::uint32_t>(source) : 0u);
			return;
		}
		// A move open on this thread holds its events until its attach pushes them all.
		a_event->next = t_move.newest;
		t_move.newest = a_event;
		if (!t_move.oldest)
			t_move.oldest = a_event;
		if (source != SceneWake::Count)
			t_move.wakes |= 1u << static_cast<std::uint32_t>(source);
		if (a_event->type != EventType::Attached || a_event->node.get() != t_move.child)
			return;
		const MoveGroup move = std::exchange(t_move, {});
		PushChain(move.oldest, move.newest, move.wakes);
	}

	void SceneTracker::PushChain(Event* a_oldest, Event* a_newest, std::uint32_t a_wakes)
	{
		a_oldest->next = head.load(std::memory_order_relaxed);
		while (!head.compare_exchange_weak(a_oldest->next, a_newest, std::memory_order_release, std::memory_order_relaxed)) {
		}
		for (std::uint32_t wake = 0; wake < static_cast<std::uint32_t>(SceneWake::Count); ++wake)
			if (a_wakes & (1u << wake))
				WakeScenePass(static_cast<SceneWake>(wake));
	}

	void SceneTracker::PushHeldDetaches()
	{
		if (!heldDetachCount.load(std::memory_order_acquire))
			return;
		std::vector<HeldDetach> taken;
		{
			std::scoped_lock lock(heldDetachLock);
			taken.swap(heldDetaches);
			heldDetachCount.store(0, std::memory_order_release);
		}
		for (const auto& held : taken)
			PushChain(held.oldest, held.newest, 1u << static_cast<std::uint32_t>(SceneWake::Detach));
	}

	void SceneTracker::PushCaptured(Event* a_event, bool a_fillOnly)
	{
		if (!a_event)
			return;
		a_event->mirrorOnly = true;
		a_event->mirrorFill = a_fillOnly;
		Push(a_event);
	}

	void SceneTracker::CollectAncestors(const RE::NiAVObject* a_node, std::vector<const void*>& a_out)
	{
		// On the hook's thread, while the engine holds the chain.
		for (const RE::NiAVObject* parent = a_node ? a_node->parent : nullptr; parent && a_out.size() < 32; parent = parent->parent)
			a_out.push_back(parent);
	}

	void SceneTracker::PushAttached(RE::NiAVObject* a_child, const RE::NiAVObject* a_parent)
	{
		if (!a_child)
			return;
		auto* event = new Event{};
		event->type = EventType::Attached;
		event->thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
		event->node.reset(a_child);
		CollectAncestors(a_child, event->ancestors);
		event->captured = SceneCapture::CaptureAttached(*a_child);
		// The child's parent now, or for a parentless root the node whose array took it.
		NoteEdit(a_child, a_parent ? a_parent : a_child->parent, event->captured != nullptr);
		if (const auto* root = watchedRoot.load(std::memory_order_acquire); root && a_child != root && Under(a_child, root))
			SubtreeEvents().Push(SubtreeEvent{ a_child, SubtreeEvent::Kind::Attached });
		// The held detach of this child goes with its attach, so the scene pass applies them together as a move.
		HeldDetach detach;
		if (heldDetachCount.load(std::memory_order_acquire)) {
			std::scoped_lock lock(heldDetachLock);
			if (const auto held = std::ranges::find(heldDetaches, static_cast<const RE::NiAVObject*>(a_child), &HeldDetach::child); held != heldDetaches.end()) {
				detach = *held;
				heldDetaches.erase(held);
				heldDetachCount.fetch_sub(1, std::memory_order_release);
			}
		}
		if (detach.oldest) {
			if (!t_move.child) {
				std::uint32_t wakes = 1u << static_cast<std::uint32_t>(SceneWake::Detach);
				if (event->captured) {
					StampWoken(event);
					wakes |= 1u << static_cast<std::uint32_t>(SceneWake::Attach);
				}
				event->next = detach.newest;
				PushChain(detach.oldest, event, wakes);
				return;
			}
			PushChain(detach.oldest, detach.newest, 1u << static_cast<std::uint32_t>(SceneWake::Detach));
		}
		Push(event);
	}

	void SceneTracker::PushDetached(RE::NiAVObject* a_child, const RE::NiAVObject* a_parent)
	{
		if (!a_child)
			return;
		// Before the engine's detach, so the child's parent is still the node it leaves.
		const bool inWorld = SceneCapture::InWorld(a_child);
		const bool unheldExit = NoteEdit(a_child, a_parent ? a_parent : a_child->parent, inWorld);
		// Name each watched key it takes away before the engine can free any of them.
		if (const auto* root = watchedRoot.load(std::memory_order_acquire); root && a_child != root && Under(a_child, root))
			Engine::ForEachLive(a_child, [](RE::NiAVObject& a_object, std::uint32_t) { SubtreeEvents().Push(SubtreeEvent{ &a_object, SubtreeEvent::Kind::Removed }); });
		auto* event = new Event{};
		event->type = EventType::Detached;
		event->thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
		event->unheldExit = unheldExit;
		CollectAncestors(a_child, event->ancestors);
		CollectGeometry(a_child, event->removed, inWorld ? &event->removedNodes : nullptr);
		if (inWorld)
			event->detachedRoot = a_child;
		if (event->removed.empty() && !inWorld) {
			delete event;
			return;
		}
		// The engine re-equips with a detach and attach of the same child, which no scene pass may split. So a plain detach waits for
		// that attach or the frame's start, pinned so no new object takes its address.
		if (!t_move.child && !LoadingMenuOpen()) {
			event->pins.emplace_back(a_child);
			StampWoken(event);
			std::scoped_lock lock(heldDetachLock);
			auto held = std::ranges::find(heldDetaches, static_cast<const RE::NiAVObject*>(a_child), &HeldDetach::child);
			if (held == heldDetaches.end()) {
				held = heldDetaches.insert(held, HeldDetach{ a_child });
				heldDetachCount.fetch_add(1, std::memory_order_release);
			}
			event->next = held->newest;
			held->newest = event;
			if (!held->oldest)
				held->oldest = event;
			return;
		}
		Push(event);
	}

	void SceneTracker::PushStacked(std::uint32_t a_channel, std::uint64_t a_token, std::span<RE::NiAVObject* const> a_pinned, std::span<RE::NiAVObject* const> a_named,
		bool a_wakes)
	{
		if (!a_token)
			return;
		auto* event = new Event{};
		event->type = EventType::Stacked;
		event->stackedChannel = a_channel;
		event->stackedWakes = a_wakes;
		event->stackedToken = a_token;
		// Pinned roots are pinned here while the pusher holds them, and released on the render thread. Named roots are keys only.
		event->stackedPinned.reserve(a_pinned.size());
		event->pins.reserve(a_pinned.size());
		for (auto* root : a_pinned)
			if (root) {
				event->stackedPinned.push_back(root);
				event->pins.emplace_back(root);
			}
		event->stackedNamed.reserve(a_named.size());
		for (auto* root : a_named)
			if (root)
				event->stackedNamed.push_back(root);
		Push(event);
	}

	void SceneTracker::PushUpdate(SceneCapture::Update&& a_update, std::vector<RE::NiPointer<RE::NiRefObject>>&& a_pins)
	{
		if (!installed)
			return;
		if (a_update.hiddenSite != ~0u)
			if (watchedRoot.load(std::memory_order_acquire))
				if (const auto* record = std::get_if<SceneCapture::NodeRecord>(&a_update.record)) {
					// Look the member up, because the stored object may be under construction with no parent yet.
					const auto* node = static_cast<const RE::NiAVObject*>(record->key);
					bool member = false;
					{
						const SetRead read;
						const auto* members = subtreeMembers.load(std::memory_order_seq_cst);
						member = node && members && std::ranges::binary_search(*members, static_cast<const void*>(node));
					}
					if (member)
						SubtreeEvents().Push(SubtreeEvent{ node, (record->flags & 1u) ? SubtreeEvent::Kind::Hidden : SubtreeEvent::Kind::Shown });
				}
		auto* event = new Event{};
		event->type = EventType::Updated;
		event->update = std::move(a_update);
		event->pins = std::move(a_pins);
		if (!event->update.sequence)
			event->update.sequence = SceneCapture::NextSequence();
		Push(event);
	}

	SceneTracker::Event* SceneTracker::Drain()
	{
		// The stack holds the newest event first. Reverse it to replay in order.
		Event* newestFirst = head.exchange(nullptr, std::memory_order_acquire);
		Event* oldestFirst = nullptr;
		while (newestFirst) {
			Event* next = newestFirst->next;
			newestFirst->next = oldestFirst;
			oldestFirst = newestFirst;
			newestFirst = next;
		}
		return oldestFirst;
	}

	void SceneTracker::FreeEvents(Event* a_head)
	{
		while (a_head) {
			Event* next = a_head->next;
			delete a_head;
			a_head = next;
		}
	}

	void SceneTracker::CollectGeometry(RE::NiAVObject* a_root, std::vector<RE::BSGeometry*>& a_out, std::vector<const void*>* a_nodes)
	{
		if (!a_root)
			return;

		// Iterative, because scene subtrees can be deep enough to risk recursion on loader threads.
		Engine::ForEachLive(a_root, [&](RE::NiAVObject& a_object, std::uint32_t) {
			if (auto* geometry = a_object.AsGeometry())
				a_out.push_back(geometry);
			else if (auto* node = a_object.AsNode(); node && a_nodes)
				a_nodes->push_back(node);
		});
	}

	bool SceneTracker::NoteEdit(const RE::NiAVObject* a_child, const RE::NiAVObject* a_parent, bool a_inWorld)
	{
		// The particle manager holds its masters without being their parent, so its edits are counted apart.
		const auto* owner = unheldOwner.load(std::memory_order_acquire);
		const bool unheld = owner && a_parent == owner;
		if (unheld)
			unheldEdits.fetch_add(1, std::memory_order_release);
		{
			const SetRead read;
			if (const auto* set = categoryNodes.load(std::memory_order_seq_cst))
				if (std::ranges::binary_search(*set, static_cast<const void*>(a_child)) ||
					(!unheld && a_parent && std::ranges::binary_search(*set, static_cast<const void*>(a_parent))))
					categoryEdits.fetch_add(1, std::memory_order_release);
		}
		// The frame start's leased captures assume nothing in the world is attached or detached inside the window. Count what breaks that.
		if (a_inWorld && EngineReadWindow::IsOpen()) {
			const auto thread = static_cast<std::uint32_t>(::GetCurrentThreadId());
			(thread == renderThread.load(std::memory_order_relaxed) ? windowEditsRender : windowEditsOther).fetch_add(1, std::memory_order_relaxed);
			std::uint32_t none = 0;
			windowEditFirst.compare_exchange_strong(none, thread, std::memory_order_relaxed);
		}
		return unheld;
	}

	void SceneTracker::WatchSubtree(const RE::NiAVObject* a_root)
	{
		watchedRoot.store(a_root, std::memory_order_release);
	}

	void SceneTracker::WatchSubtreeMembers(std::vector<const void*> a_members)
	{
		std::erase(a_members, nullptr);
		std::ranges::sort(a_members);
		if (subtreeCurrent && *subtreeCurrent == a_members)
			return;
		auto set = std::make_unique<const std::vector<const void*>>(std::move(a_members));
		subtreeMembers.store(set.get(), std::memory_order_seq_cst);
		RetireSet(std::move(subtreeCurrent));
		subtreeCurrent = std::move(set);
	}

	RenderGraph::Frame::EventQueue<SceneTracker::SubtreeEvent>& SceneTracker::SubtreeEvents()
	{
		static RenderGraph::Frame::EventQueue<SubtreeEvent> queue;
		return queue;
	}

	void SceneTracker::WatchCategoryNodes(std::vector<const void*> a_nodes)
	{
		if (const auto thread = static_cast<std::uint32_t>(::GetCurrentThreadId()); renderThread.exchange(thread, std::memory_order_relaxed) != thread)
			logger::info("[DCLF] scene tracker: the render thread is {}", thread);
		std::erase(a_nodes, nullptr);
		std::ranges::sort(a_nodes);
		a_nodes.erase(std::unique(a_nodes.begin(), a_nodes.end()), a_nodes.end());
		if (categoryCurrent && *categoryCurrent == a_nodes)
			return;
		auto set = std::make_unique<const std::vector<const void*>>(std::move(a_nodes));
		categoryNodes.store(set.get(), std::memory_order_seq_cst);
		RetireSet(std::move(categoryCurrent));
		categoryCurrent = std::move(set);
	}

	std::uint64_t SceneTracker::CategoryEdits()
	{
		return categoryEdits.load(std::memory_order_acquire);
	}

	void SceneTracker::RetireCategoryNodes()
	{
		// A reader that loaded a retired set counted itself in first, so once none is in, every retired set is unreachable.
		if (!categoryRetired.empty() && categoryReaders.load(std::memory_order_seq_cst) == 0)
			categoryRetired.clear();
	}

	SceneTracker::SetRead::SetRead()
	{
		categoryReaders.fetch_add(1, std::memory_order_seq_cst);
	}

	SceneTracker::SetRead::~SetRead()
	{
		categoryReaders.fetch_sub(1, std::memory_order_seq_cst);
	}

	void SceneTracker::RetireSet(std::unique_ptr<const std::vector<const void*>> a_set)
	{
		if (a_set)
			categoryRetired.push_back(std::move(a_set));
	}

	void SceneTracker::WatchUnheldOwner(const RE::NiAVObject* a_owner)
	{
		unheldOwner.store(a_owner, std::memory_order_release);
	}

	std::uint64_t SceneTracker::UnheldEdits()
	{
		return unheldEdits.load(std::memory_order_acquire);
	}

	SceneTracker::WindowEdits SceneTracker::TakeWindowEdits()
	{
		WindowEdits edits;
		edits.renderThread = windowEditsRender.exchange(0, std::memory_order_relaxed);
		edits.otherThreads = windowEditsOther.exchange(0, std::memory_order_relaxed);
		edits.firstThread = windowEditFirst.exchange(0, std::memory_order_relaxed);
		return edits;
	}
}
