#pragma once

// Coordinator components: feature state the scene coordinator keeps beside its residents, plus hooks it calls at fixed points.
// Whatever a feature lists for the scene's members, such as table rows or owned engine nodes, follows the members through these hooks.

#include "Features/DrawcallLimitFix/Api/EngineEvents.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/CoordinatorComponents.h.
	class CoordinatorComponentContext;
	class ResidentRecount;
	class AccumulatedPatch;
	class SlotRewrite;
	class WalkScheduling;
	class EntryWrite;

	/** @brief A registered component. FrameView::TakeComponentSeeds takes its seeds by this id. */
	struct CoordinatorComponentId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/** @brief A component's mark on a scene root. It applies to the entries under the root and is erased when the root detaches. */
	struct Mark
	{
		CoordinatorComponentId component;
		std::uint64_t token = 0;
		std::uint8_t side = 0;
		explicit operator bool() const { return static_cast<bool>(component); }
	};

	/**
	 * @brief Entry vetoes, which make the commit withhold the entry's slots. Hold applies nothing.
	 * Observe also records what the commit would have applied. The veto is one byte, so the two exclude each other.
	 */
	inline constexpr std::uint8_t kVetoHold = 1, kVetoObserve = 2;

	/**
	 * @brief Requeue modes. Slots recommits the entry's slots. Reevaluate retakes its verdict if it was classified.
	 * ReevaluateAlways retakes it regardless.
	 */
	inline constexpr std::uint8_t kRequeueSlots = 1, kRequeueReevaluate = 2, kRequeueReevaluateAlways = 4;

	/** @brief Mirrored node kind bits a component may test. Tree is a BSTreeNode. Captured is a node capture contribution's class. */
	inline constexpr std::uint32_t kMirroredKindTree = 1u << 8;
	inline constexpr std::uint32_t kMirroredKindCaptured = 1u << 9;

	/** @brief A node as the mirror holds it. Valid only until the hook returns. Keys are never dereferenced. */
	struct MirroredNode
	{
		bool known = false;
		const void* parent = nullptr;  // null for a root
		const void* rtti = nullptr;
		std::uint32_t kind = 0;        // kMirroredKind* bits
		std::span<const void* const> children;  // by engine slot, null slots kept, in OnVisible order
		std::span<const void* const> captured;  // the node capture contribution's list, only for kMirroredKindCaptured
	};

	/** @brief A tracked entry's state as the coordinator keeps it. */
	struct TrackedState
	{
		bool tracked = false;
		bool classified = false;   // a pass took its verdict and nothing has asked again
		bool leafWaiting = false;  // written while the mirror had no record of it
		std::uint32_t slot = ~0u, layerSlot = ~0u;
		std::uint8_t reason = 0;     // candidate verdict (Ineligible)
		std::uint8_t veto = 0;       // kVetoHold, kVetoObserve or 0
		std::uint8_t waitCause = 0;  // what the commit is waiting on, 0 if not waiting
		std::uint8_t waitWhy = 0;    // first reason of the last wait
		bool resident = false;
	};

	/** @brief What the commit recorded for an observed slot. */
	struct ObservedState
	{
		const void* geometry = nullptr;  // null if not evaluated since the Observe veto
		std::uint8_t phases = 0;
		bool ready = false;  // all phases present and not waiting
	};

	/** @brief A root the primary cull admitted. standIn means DCLF draws all of it and the engine draws none. */
	struct StandInRoot
	{
		const void* node = nullptr;
		bool standIn = false;
	};

	/**
	 * @brief What the adoption hook receives on the render thread at the frame's start.
	 * claims is null before DCLF's first covered frame. deltas are this component's, oldest first.
	 */
	struct AdoptionContext
	{
		std::uint32_t frame = 0;
		const void* claims = nullptr;
		std::span<const std::shared_ptr<const void>> deltas;
	};

	/**
	 * @brief A coordinator component: its name, its state and its hook sets.
	 * Hooks run on the scene lane in registration order, except adoption, which runs on the render thread.
	 * Each hook runs once per event, never per object per frame. Every hook is optional.
	 * make creates the state once, before the first hook.
	 */
	struct CoordinatorComponent
	{
		/**
		 * @brief Follows a slot's record, not its residency.
		 * Also called during the parity dense walk, so a component that must not touch its listing there checks DenseWalk.
		 *
		 * - written: may be called again for a slot already listed.
		 * - released: called after Residency::left, right before the slot's object is reset.
		 */
		struct Slots
		{
			void (*written)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot) = nullptr;
			void (*released)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot) = nullptr;
		};

		/**
		 * @brief Follows a slot's residency.
		 *
		 * - resident: may be called again for a slot already listed.
		 * - allLeft: called before left runs for each resident.
		 * - recount: recount the same slots the coordinator just recounted.
		 */
		struct Residency
		{
			void (*resident)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot) = nullptr;
			void (*left)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot) = nullptr;
			void (*allLeft)(const CoordinatorComponentContext& a_context) = nullptr;
			void (*recount)(const CoordinatorComponentContext& a_context, const ResidentRecount& a_recount) = nullptr;
		};

		/**
		 * @brief The accumulate phase.
		 *
		 * - patched: called after a slot's accumulated half is written from its patch.
		 * - joinsDone: bring derived state up to date and write the live tables.
		 */
		struct Accumulate
		{
			void (*patched)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot, const AccumulatedPatch& a_patch) = nullptr;
			void (*joinsDone)(const CoordinatorComponentContext& a_context) = nullptr;
		};

		/** @brief The walk. Called for both the delta walk and the parity dense walk. */
		struct Walk
		{
			void (*begun)(const CoordinatorComponentContext& a_context) = nullptr;
			void (*scheduling)(const CoordinatorComponentContext& a_context, WalkScheduling& a_scheduling) = nullptr;
			void (*rewritten)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot, const SlotRewrite& a_rewrite) = nullptr;
			void (*ended)(const CoordinatorComponentContext& a_context) = nullptr;
		};

		/**
		 * @brief Brackets the parity dense walk. The coordinator restores its own tables afterwards but not the components' state.
		 * Save in begun and restore in ended.
		 */
		struct DenseWalk
		{
			void (*begun)(const CoordinatorComponentContext& a_context) = nullptr;
			void (*ended)(const CoordinatorComponentContext& a_context) = nullptr;
		};

		/**
		 * @brief Core lets go of what the component holds.
		 *
		 * - reset: the slot tables are about to be cleared. Hand references back through the retirement chain.
		 * - recycled: a range this component retired is no longer named by any publication.
		 */
		struct Retirement
		{
			void (*reset)(const CoordinatorComponentContext& a_context) = nullptr;
			void (*forgot)(const CoordinatorComponentContext& a_context, const void* a_entry) = nullptr;
			void (*recycled)(const CoordinatorComponentContext& a_context, std::uint32_t a_first, std::uint32_t a_count) = nullptr;
		};

		/**
		 * @brief Writes of the entry class this component follows. Only the following component's hooks are called.
		 *
		 * - takes: whether the class's objects are DCLF's. The hooks run either way.
		 * - resolve: fills the entry's second vertex stream. Returning false makes the record wait for the component's next event.
		 * - streamed: point the slot at the stream that resolve supplied.
		 */
		struct EntryClassHooks
		{
			EntryClass follows = EntryClass::None;
			bool takes = false;
			void (*listed)(const CoordinatorComponentContext& a_context, const EntryWrite& a_write) = nullptr;
			bool (*resolve)(const CoordinatorComponentContext& a_context, EntryWrite& a_write) = nullptr;
			void (*streamed)(const CoordinatorComponentContext& a_context, std::uint32_t a_slot, EntryWrite& a_write) = nullptr;
		};

		/**
		 * @brief Engine channel events.
		 *
		 * - keys: called every pass, possibly with no keys. Never dereference a key unless the coordinator tracks it.
		 * - detached, underMark: only for the component that made the mark. The mark is already erased when detached runs.
		 * - admitted: the primary cull's whole admitted set, latest wins. Forces a publication.
		 */
		struct Events
		{
			void (*keys)(const CoordinatorComponentContext& a_context, EngineChannelId a_channel, std::span<const void* const> a_keys) = nullptr;
			void (*stacked)(const CoordinatorComponentContext& a_context, EngineChannelId a_channel, const Stacked& a_event) = nullptr;
			void (*detached)(const CoordinatorComponentContext& a_context, const void* a_key, const Mark& a_mark) = nullptr;
			void (*underMark)(const CoordinatorComponentContext& a_context, const void* a_geometry, const Mark& a_mark) = nullptr;
			void (*switched)(const CoordinatorComponentContext& a_context, const void* a_switch) = nullptr;
			void (*admitted)(const CoordinatorComponentContext& a_context, std::span<const StandInRoot> a_roots) = nullptr;
		};

		/**
		 * @brief The commit and the publication.
		 *
		 * - settle: veto changes made here apply in the same commit. Only called while the commit runs live.
		 *   a_live is false on a load screen, where the members are not decided.
		 * - take: take the component's own queues before the pass collects events.
		 */
		struct Commit
		{
			void (*settle)(const CoordinatorComponentContext& a_context, bool a_live) = nullptr;
			void (*take)(const CoordinatorComponentContext& a_context) = nullptr;
			void (*published)(const CoordinatorComponentContext& a_context) = nullptr;
		};

		/** @brief Runs every frame on the render thread with no coordinator context. Keep only render-thread state here. */
		struct Adoption
		{
			void (*adopt)(const AdoptionContext& a_context) = nullptr;
		};

		std::string_view name;
		std::shared_ptr<void> (*make)() = nullptr;
		Slots slots;
		Residency residency;
		Accumulate accumulate;
		Walk walk;
		DenseWalk dense;
		Retirement retirement;
		EntryClassHooks entryClass;
		Events events;
		Commit commit;
		Adoption adoption;
	};
}
