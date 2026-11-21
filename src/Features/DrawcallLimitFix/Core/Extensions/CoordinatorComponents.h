#pragma once

// Core's side of Api/CoordinatorComponents.h: what a component's hooks receive.
// Hooks get the live tables, their own state and a few coordinator services, never the coordinator itself.

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Api/EntryRanges.h"
#include "Features/DrawcallLimitFix/Api/MainRanges.h"

#include <ankerl/unordered_dense.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF
{
	struct SceneTables;
	struct FadeSeedItem;
	class SceneMirror;
	namespace Scene
	{
		class Coordinator;
	}
}

namespace DCLF::Api
{
	/** @brief What a component's hook gets on the scene lane: the live tables, its state and the coordinator's services. */
	class CoordinatorComponentContext
	{
	public:
		CoordinatorComponentContext(SceneTables& a_tables, const std::shared_ptr<void>& a_state, Scene::Coordinator& a_coordinator, std::uint32_t a_component) :
			tables(a_tables), state(a_state), coordinator(a_coordinator), component(a_component) {}

		SceneTables& tables;
		const std::shared_ptr<void>& state;

		template <class T>
		T& State() const
		{
			return *static_cast<T*>(state.get());
		}

		/** @brief The fade node of a_slot's property as the mirror holds it, and its capture kind. Reads no engine memory. */
		const void* MirroredFadeNode(std::uint32_t a_slot, std::uint32_t& a_kind) const;
		/**
		 * @brief A reference to a_node held by a_slot's entry, for a newly listed owner.
		 * Returns null when the entry holds a different node. The render thread is then asked, and the component is called again as resident.
		 */
		RE::NiPointer<RE::NiAVObject> HoldFadeNode(std::uint32_t a_slot, const void* a_node) const;
		/** @brief Recounts a_slot at the next KeepResidentsAlive, because its keys may have changed. */
		void NoteTouched(std::uint32_t a_slot) const;
		/** @brief Releases an engine reference at Present through the retirement chain. */
		void HandBack(RE::NiPointer<RE::NiAVObject>&& a_reference) const;
		void HandBack(std::vector<RE::NiPointer<RE::NiAVObject>>& a_references) const;
		void HandBack(RE::NiPointer<RE::NiRefObject>&& a_reference) const;
		/** @brief A tracked entry's write state. */
		struct TrackedWrite
		{
			bool tracked = false;
			std::uint32_t slot = ~0u;
			bool streamWaiting = false;  // its last record write waited for its second vertex stream
		};
		TrackedWrite TrackedWriteOf(const RE::BSGeometry* a_geometry) const;
		/** @brief Writes a_geometry's tracked entry in full this walk. Returns false if it is not tracked. */
		bool ScheduleWrite(RE::BSGeometry* a_geometry) const;
		/** @brief The tracked entry's own reference to the geometry keyed a_key, or null when not tracked. */
		RE::BSGeometry* TrackedGeometry(const void* a_key) const;
		/** @brief Frees a range of the component's. It retires until no publication names it, then comes back through Retirement::recycled. */
		void RetireRange(std::uint32_t a_first, std::uint32_t a_count) const;
		/** @brief Adds a seed for the next frame's start to hand to this component. */
		void RequestSeed(FadeSeedItem&& a_seed) const;
		/** @brief Whether KeepResidentsAlive counted a_slot, meaning a resident with a pipeline. */
		bool Counted(std::uint32_t a_slot) const;
		/** @brief The scene mirror as this pass's events left it. No engine memory. */
		const SceneMirror& Mirror() const;
		MirroredNode MirroredNodeOf(const void* a_key) const;

		// Object slot columns a component reads, and the fixed main range ordinals it writes. Read per event, never per object per frame.
		std::uint32_t ObjectSlots() const;
		/** @brief a_slot's geometry, or null. */
		const RE::BSGeometry* ObjectGeometry(std::uint32_t a_slot) const;
		/** @brief The slots a_slot takes in its fixed range, one per skin partition. */
		std::uint32_t RangeSpan(std::uint32_t a_slot) const;
		/** @brief The slots a range member with a_partitions skin partitions takes. */
		static std::uint32_t RangeSpanOf(std::uint32_t a_partitions);
		/** @brief Each object slot's ordinal in its fixed range, or ~0u. Resized to the object slots first. */
		std::span<std::uint32_t> SizedRangeOrdinals() const;
		std::span<std::uint32_t, kFixedMainRanges> RangeCounts() const;
		/** @brief Reports that a_slot's ordinal moved or it left its range, so its draw's inputs changed. */
		void NoteRangeOrdinalChanged(std::uint32_t a_slot) const;
		std::uint64_t PassSerial() const;
		/** @brief The pass's stamp, never 0, for a check run once per pass. */
		std::uint32_t PassStamp() const;

		// Drawn index ranges kept per tracked entry, such as object LOD's sub-index shapes. Erased with the entry.
		using EntryRangesMap = ankerl::unordered_dense::map<const RE::BSGeometry*, std::vector<EntryRange>>;
		const EntryRangesMap& EntryRanges() const;
		/** @brief a_entry's kept ranges, or null. Only a tracked entry with ranges is known alive. */
		const std::vector<EntryRange>* EntryRangesOf(const void* a_entry) const;
		/** @brief The ranges in the mirror's record of a_entry, or null. */
		const std::vector<EntryRange>* MirroredRanges(const void* a_entry) const;
		/** @brief Replaces a_entry's kept ranges. a_rewrite rewrites its range slot chain this walk. */
		void SetEntryRanges(const void* a_entry, std::vector<EntryRange>&& a_ranges, bool a_rewrite) const;

		// Marks on scene roots. Written only while the mirror's events are applied, before the adds.
		/**
		 * @brief Marks a_key for this component. a_ownHide makes the mirror count the subtree's hide as the component's.
		 * Returns the mark it displaced.
		 */
		Mark MarkRoot(const void* a_key, std::uint64_t a_token, std::uint8_t a_side, bool a_ownHide) const;
		/** @brief Erases a_key's mark if it is this component's with a_token. Returns whether it did. */
		bool UnmarkRoot(const void* a_key, std::uint64_t a_token) const;
		Mark MarkOf(const void* a_key) const;

		// The commit's vetoes, one byte per entry.
		/** @brief Sets a_entry's veto. Observe resets its observed slots. Set during settle, it applies in the same commit. */
		void SetEntryVeto(const void* a_entry, std::uint8_t a_veto) const;
		/** @brief What the commit recorded of a_slot while Observe withheld it. */
		ObservedState ObservedSlot(std::uint32_t a_slot) const;
		/** @brief Whether a_slot is a layer's. A publication claims a layer with its base geometry. */
		bool LayerSlot(std::uint32_t a_slot) const;
		/** @brief How many entries hold a_veto, across every component. */
		std::uint32_t VetoCount(std::uint8_t a_veto) const;
		/** @brief Asks for the commit to run live, so settle is called, whatever the vetoes. */
		void SetCommitLive(bool a_live) const;
		/** @brief Requeues a_entry. Does nothing when it is not tracked. */
		void Requeue(const void* a_entry, std::uint8_t a_flags) const;
		TrackedState TrackedStateOf(const void* a_entry) const;
		/** @brief Adds a delta for the next publication, which this forces. The delta reaches the component's adoption. */
		void PublishDelta(std::shared_ptr<const void> a_delta) const;
		/** @brief Appends the tracked entries under a_root as the mirror holds the subtree. a_lacking is set if the mirror lacks any record. */
		void SubtreeEntries(const void* a_root, std::vector<const void*>& a_out, bool& a_lacking) const;

	private:
		Scene::Coordinator& coordinator;
		std::uint32_t component;
	};

	/** @brief The accumulated patch values a component lists by: the decal draw chain key for an object drawn in a kept order. */
	class AccumulatedPatch
	{
	public:
		std::uint64_t chain;
	};

	/** @brief A walk's rewrite of a slot: whether its scene half was rewritten, and its skin partitions before and after. */
	class SlotRewrite
	{
	public:
		bool written;
		std::uint32_t partitionsBefore, partitionsNow;
	};

	/**
	 * @brief A recount. With rebuild, every resident is counted from nothing.
	 * Otherwise the touched slots, sorted and unique, are all uncounted first and then counted as they are now.
	 */
	class ResidentRecount
	{
	public:
		bool rebuild;
		std::span<const std::uint32_t> touched;    // without rebuild
		std::span<const std::uint32_t> residents;  // with rebuild, in residents' order
	};

	/** @brief The delta walk's scheduling, and the component's own counts for its report lines. */
	class WalkScheduling
	{
	public:
		bool full;
		std::uint64_t events = 0;
		std::uint64_t updated = 0;  // slots updated in place
		std::uint64_t written = 0;  // writes scheduled through ScheduleWrite
	};

	/** @brief A walk's write of an entry of the followed class, and the second vertex stream resolve gives it. Lives on the merge's stack. */
	class EntryWrite
	{
	public:
		RE::BSGeometry& geometry;
		// The entry's held source node, such as a face shape's BSFaceGenNiNode, or null.
		RE::NiNode* source = nullptr;
		// The stream's float4 positions, leased by owner, and their region of the scene's positions buffer.
		const float* positions = nullptr;
		std::uint32_t vertexCount = 0;
		std::uint64_t generation = 0;
		std::shared_ptr<const std::vector<float>> owner;
		std::uint32_t region = ~0u;  // kNoFirst means none
	};
}

namespace DCLF::Extensions
{
	/** @brief Any thread, after InstallFeatures. Whether the component that follows a_class takes its objects. */
	bool EntryClassTaken(Api::EntryClass a_class);
	/**
	 * @brief Render thread, at the frame's start after coverage is decided. Calls every component's adoption hook in registration order.
	 * a_claims is null until claims are installed.
	 */
	void ComponentsAdopt(std::uint32_t a_frame, const void* a_claims, std::span<const std::vector<std::shared_ptr<const void>>> a_deltas);
}
