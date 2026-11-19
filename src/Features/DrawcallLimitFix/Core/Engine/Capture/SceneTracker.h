#pragma once

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

#include <atomic>
#include <memory>
#include <span>
#include <vector>

namespace DCLF
{
	/**
	 * @brief The scene's event stack: attaches, detaches, hook value updates and channel events, from any thread.
	 * The NiNode child functions are detoured once. A detach collects the subtree's leaves on the detaching thread while it is still
	 * intact. Every event goes onto one lock-free stack so they apply in push order. References are released with their batch on the render thread.
	 */
	class SceneTracker
	{
	public:
		enum class EventType : std::uint8_t
		{
			Attached,
			Detached,
			Updated,  // in the same order as the attaches and detaches
			Stacked,  // after what its thread pushed before it
		};

		struct Event
		{
			Event* next = nullptr;
			EventType type = EventType::Attached;
			RE::NiPointer<RE::NiAVObject> node;
			std::vector<RE::BSGeometry*> removed;  // identity only, never dereferenced
			std::unique_ptr<SceneCapture::Records> captured;
			const void* detachedRoot = nullptr;
			std::vector<const void*> removedNodes;
			// The node's ancestors at the hook, identities only. Sun candidates among them are planned again.
			std::vector<const void*> ancestors;
			SceneCapture::Update update;
			// The event's own references, released with the batch on the render thread. They belong to the event, not the update, because
			// the mirror keeps copies of updates whose last release would otherwise land on the scene work.
			std::vector<RE::NiPointer<RE::NiRefObject>> pins;
			// The render thread's own captures at the frame's start, for the mirror alone. Ordered with the hooks' events so a capture never overwrites a newer write.
			bool mirrorOnly = false;
			// A mirror request's capture fills only missing records. The frame's start can catch an ancestor in a state the engine undoes within the frame, with no event.
			bool mirrorFill = false;
			// An unheld root leaving its owner's array. The category capture follows it with a diff, so it forces no full capture.
			bool unheldExit = false;
			// Push time (steady clock, ns) and frame, for latency reports.
			std::uint64_t stampNs = 0;
			std::uint32_t stampFrame = 0;
			std::uint32_t thread = 0;  // for the cell trace
			// A Stacked event's channel, wake, token, pinned roots (held in `pins`) and named roots (keys only, the pusher holds them).
			std::uint32_t stackedChannel = ~0u;
			bool stackedWakes = false;
			std::uint64_t stackedToken = 0;
			std::vector<const void*> stackedPinned, stackedNamed;
		};

		static SceneTracker& Get();

		void Install();
		bool IsInstalled() const { return installed; }

		/** @brief Stops queueing and frees pending events for good. The detours pass straight through. Used when DCLF is forced off after install. */
		void Stop();

		/** @brief Takes every pending event, oldest first. The caller owns the list. */
		Event* Drain();
		static void FreeEvents(Event* a_head);

		/**
		 * @brief An attach, after the engine's. a_parent is the node whose array took the child, which differs for a parentless root an
		 * array owns, such as the particle manager's master systems.
		 */
		void PushAttached(RE::NiAVObject* a_child, const RE::NiAVObject* a_parent = nullptr);
		static void CollectAncestors(const RE::NiAVObject* a_node, std::vector<const void*>& a_out);
		/** @brief A detach, before the engine's, while the child is still in its array. */
		void PushDetached(RE::NiAVObject* a_child, const RE::NiAVObject* a_parent = nullptr);
		/** @brief Render thread, at the frame's start. Pushes detaches held back for an attach of the same child. */
		void PushHeldDetaches();
		/**
		 * @brief A hook's values, after its write. On the same stack as attaches and detaches, so a reused address never gets an earlier object's values.
		 */
		void PushUpdate(SceneCapture::Update&& a_update, std::vector<RE::NiPointer<RE::NiRefObject>>&& a_pins = {});
		/** @brief Render thread. Pushes its own capture, marked mirror-only. The stack owns a_event from here. */
		void PushCaptured(Event* a_event, bool a_fillOnly = false);
		/** @brief A channel's event, on the pushing thread, applied after the attaches it pushed before. */
		void PushStacked(std::uint32_t a_channel, std::uint64_t a_token, std::span<RE::NiAVObject* const> a_pinned, std::span<RE::NiAVObject* const> a_named,
			bool a_wakes);

		static void CollectGeometry(RE::NiAVObject* a_root, std::vector<RE::BSGeometry*>& a_out, std::vector<const void*>* a_nodes = nullptr);

		/** @brief An edit under the watched subtree: a hidden store, a detach or an attach. The node is an identity, never dereferenced. */
		struct SubtreeEvent
		{
			enum class Kind : std::uint8_t
			{
				Hidden,
				Shown,
				Removed,
				Attached,
			};
			const RE::NiAVObject* node = nullptr;
			Kind kind = Kind::Hidden;
		};
		/** @brief Render thread. The root whose subtree edits are pushed onto SubtreeEvents from now on. The root's own stores are not pushed. */
		static void WatchSubtree(const RE::NiAVObject* a_root);
		/**
		 * @brief Render thread. The watched subtree's members, as keys. Hidden stores are pushed only for members, because the stored
		 * object may be under construction, so the hook looks it up instead of walking its parents.
		 */
		static void WatchSubtreeMembers(std::vector<const void*> a_members);
		/** @brief The watched subtree's edits, for their one consumer on the render thread. */
		static EventQueue<SubtreeEvent>& SubtreeEvents();

		/** @brief Render thread. The nodes the category capture finds category nodes through. Their edits count CategoryEdits. */
		static void WatchCategoryNodes(std::vector<const void*> a_nodes);
		/** @brief Any thread. Edits of watched category nodes since the start. */
		static std::uint64_t CategoryEdits();
		/** @brief Render thread, once at the frame's start. Frees replaced sets no reader holds. */
		static void RetireCategoryNodes();

		/**
		 * @brief Any thread. Marks a read of a lock-free published set from before its load to after its last use, so a replaced set
		 * is freed only once no such read is in.
		 */
		class SetRead
		{
		public:
			SetRead();
			~SetRead();
			SetRead(const SetRead&) = delete;
			SetRead& operator=(const SetRead&) = delete;
		};
		/** @brief Render thread, after storing the replacement. Retires a replaced set until no SetRead is in. */
		static void RetireSet(std::unique_ptr<const std::vector<const void*>> a_set);

		/** @brief Render thread. The unheld roots' owner, whose child edits count UnheldEdits instead of CategoryEdits. */
		static void WatchUnheldOwner(const RE::NiAVObject* a_owner);
		/** @brief Any thread. The unheld roots owner's child edits since the start. */
		static std::uint64_t UnheldEdits();

		/**
		 * @brief Checks the frame start's premise that nothing attaches or detaches in the world while EngineReadWindow is open.
		 */
		struct WindowEdits
		{
			std::uint64_t renderThread = 0, otherThreads = 0;
			std::uint32_t firstThread = 0;
		};
		static WindowEdits TakeWindowEdits();

	private:
		void Push(Event* a_event);
		/** @brief Links a chain onto the stack at once, then wakes the pump. */
		void PushChain(Event* a_oldest, Event* a_newest, std::uint32_t a_wakes);
		/** @brief Counts a child edit. Returns whether a_parent is the unheld roots' owner. */
		static bool NoteEdit(const RE::NiAVObject* a_child, const RE::NiAVObject* a_parent, bool a_inWorld);

		std::atomic<Event*> head{ nullptr };
		std::atomic<bool> stopped{ false };
		bool installed = false;
	};
}
