#pragma once

#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"

#include <ankerl/unordered_dense.h>

#include <array>
#include <deque>
#include <span>
#include <string>
#include <utility>

namespace DCLF
{
	namespace Diagnostics
	{
		struct SceneView;
	}

	/**
	 * @brief The coordinator's copy of the engine's scene, built from the records the hooks captured and applied in event order.
	 * Only ApplyEvents writes it. Pool evaluations read it while it is unchanged. CS_DCLF_PARITY checks it against live captures.
	 */
	class SceneMirror
	{
		friend struct Diagnostics::SceneView;

	public:
		using KeySet = ankerl::unordered_dense::set<const void*>;
		using FieldMap = ankerl::unordered_dense::map<std::uintptr_t, std::uint32_t>;
		static std::uintptr_t Key(const void* a_object, std::uint8_t a_type) { return reinterpret_cast<std::uintptr_t>(a_object) | a_type; }
		/** @brief The record type (node 0, geometry 1, property 2, alpha 3) and key of an update. */
		static std::pair<std::uint8_t, const void*> TypeOf(const SceneCapture::Update& a_update);

		/** @brief A capture's records copied off the lane, so Apply can move them in instead of copying. */
		struct Prepared
		{
			std::vector<SceneCapture::NodeRecord> nodes;
			std::vector<SceneCapture::GeometryRecord> geometries;
			std::vector<SceneCapture::PropertyRecord> properties;
			std::vector<SceneCapture::AlphaRecord> alphas;
		};
		/** @brief Pure. Reads the capture alone, so it runs on any thread in any order. a_kind is node 0, geometry 1, property 2, alpha 3. */
		static void Prepare(const SceneCapture::Records& a_records, std::uint32_t a_kind, std::uint32_t a_frame, Prepared& a_out);
		/** @brief Newest wins. With a_prepared the records are moved in. */
		void Apply(const SceneCapture::Records& a_records, Prepared* a_prepared = nullptr);
		/** @brief Parity only. None should be kept. */
		std::size_t UnusedRecords() const;
		void Detach(const void* a_root, std::span<RE::BSGeometry* const> a_geometries, std::span<const void* const> a_nodes);
		/**
		 * @brief Drops a record whose object an event names attached out of the world. It left by no detach a hook saw, or its memory now
		 * holds another object, so it and every record under it go.
		 */
		void Evict(const void* a_key, std::string_view a_what);
		/** @brief Returns the record's type and key, with a null key when the mirror holds no record. An object out of the world has none. */
		std::pair<std::uint8_t, const void*> Update(const SceneCapture::Update& a_update);
		/** @brief Update that moves a_update's record into the replay log. Its fields, leaf, sequence and hidden site stay for other readers. */
		std::pair<std::uint8_t, const void*> UpdateTaking(SceneCapture::Update& a_update);
		/** @brief Returns a copy, since an update may change the set. */
		std::vector<const void*> PropertiesOfMaterial(const void* a_material) const;
		/** @brief Returns a copy, since an update may change the set. */
		std::vector<const void*> GeometriesOfSkin(const void* a_skin) const;
		struct Work
		{
			std::uint64_t replaysSkipped = 0;  // captures numbered at or after every kept update: no key looked up
			std::uint64_t replaysWalked = 0;   // captures whose keys were looked up in the kept updates
			std::uint64_t remembered = 0;      // updates kept for the replays
			std::uint64_t forgotten = 0;       // updates two batches old let go
		};
		Work TakeWork() { return std::exchange(work, {}); }
		/** @brief Updates older than two batches go. */
		void BeginBatch();
		void Clear();

		const SceneCapture::NodeRecord* Node(const void* a_key) const;
		const void* Parent(const void* a_key) const
		{
			const auto* record = Node(a_key);
			return record ? record->parent : nullptr;
		}
		const SceneCapture::GeometryRecord* Geometry(const void* a_key) const;
		const SceneCapture::PropertyRecord* Property(const void* a_key) const;
		const SceneCapture::AlphaRecord* Alpha(const void* a_key) const;
		/** @brief Null where the mirror holds no record. */
		SceneCapture::LeafView Leaf(const void* a_key) const;
		/** @brief Never reset. Pool evaluations assert it unchanged across them. */
		std::uint64_t Writes() const { return writes; }

		/**
		 * @brief A LOD gate's incoming root keeps its kHidden bit set by the gate until the flip, so the hidden test ignores that bit.
		 * Kept beside the records, since a capture replaces a record whole and the parity compares records alone.
		 */
		void SetGated(const void* a_key, bool a_gated)
		{
			++writes;
			if (a_gated)
				gated.insert(a_key);
			else
				gated.erase(a_key);
		}
		bool Gated(const void* a_key) const { return !gated.empty() && gated.contains(a_key); }
		std::size_t GatedCount() const { return gated.size(); }

		static constexpr std::array<const char*, 4> kTypeNames{ "node", "geometry", "property", "alpha" };
		template <class Record>
		static std::string Fields(std::uint32_t a_fields)
		{
			std::string text;
			for (std::uint32_t f = 0; f < Record::kFieldCount; ++f)
				if (a_fields & (1u << f))
					text += fmt::format("{}{}", text.empty() ? "" : "+", Record::kFieldNames[f]);
			return text;
		}
		static constexpr std::size_t kTypes = 4;
		static constexpr std::uint32_t kMaxFields = 32;  // a record's fields at most (their bits)

	private:
		template <class T>
		struct Counted
		{
			T record;
			std::uint32_t uses = 0;
		};
		void Use(const void* a_property, const void* a_layer, const void* a_alpha, int a_delta);
		void ApplyCapture(const SceneCapture::Records& a_records, Prepared* a_prepared);

		ankerl::unordered_dense::map<const void*, SceneCapture::NodeRecord> nodes;
		ankerl::unordered_dense::map<const void*, SceneCapture::GeometryRecord> geometries;
		ankerl::unordered_dense::map<const void*, Counted<SceneCapture::PropertyRecord>> properties;
		ankerl::unordered_dense::map<const void*, Counted<SceneCapture::AlphaRecord>> alphas;
		// RaceMenu's overlays share their base's skin instance, so a skin's update applies to every geometry on it.
		ankerl::unordered_dense::map<const void*, std::vector<const void*>> geometriesBySkin;
		void LinkSkin(const void* a_skin, const void* a_key);
		void UnlinkSkin(const void* a_skin, const void* a_key);
		// A material's capture finds the properties that cache its alpha through this.
		ankerl::unordered_dense::map<const void*, std::vector<const void*>> propertiesByMaterial;
		void IndexMaterial(const void* a_property, const void* a_before, const void* a_after);
		// The last two batches' updates, replayed onto captures numbered before them. Each entry links to the previous one of its key.
		// A capture numbered at or after recentNewest replays nothing.
		struct Recent
		{
			std::uint64_t batch = 0;
			std::uintptr_t key = 0;
			std::uint64_t previous = ~0ull;
			SceneCapture::Update update;
		};
		std::deque<Recent> recent;
		std::uint64_t recentBase = 0;
		std::uint64_t recentNewest = 0;
		ankerl::unordered_dense::map<std::uintptr_t, std::uint64_t> recentLast;
		void Remember(std::uintptr_t a_key, SceneCapture::Update&& a_update);
		/** @brief Newest first. */
		void Later(std::uintptr_t a_key, std::uint64_t a_after, std::vector<const SceneCapture::Update*>& a_out) const;
		Work work;
		std::uint64_t batch = 0;
		std::uint64_t writes = 0;  // Writes()
		KeySet gated;              // SetGated
		bool replaying = false;
		std::uint64_t replayed = 0, stale = 0, superseded = 0;
		std::pair<std::uint8_t, const void*> UpdateRecord(const SceneCapture::Update& a_update);
		void Replay(std::uintptr_t a_key, std::uint64_t a_after);
		std::uint64_t applied = 0, detached = 0, updates = 0, updatesUnheld = 0;
		std::uint64_t evictions = 0, evictedRecords = 0, lightEvictions = 0;  // lights: ShadowSceneNode's queued removal, no detach
		std::string firstEviction;
		// The updates applied, by record type and field.
		std::array<std::array<std::uint64_t, kMaxFields>, kTypes> updated{};
	};

	/**
	 * @brief Visits the mirror's records under a_root depth first, a_root included, as ForEachLive does.
	 * True when a key on the way had no record. That key is passed over.
	 */
	template <class State, class Visit>
	bool ForEachMirror(const SceneMirror& a_mirror, const void* a_root, State a_state, Visit&& a_visit, std::size_t a_limit = ~std::size_t{ 0 })
	{
		bool missing = false;
		std::vector<std::pair<const void*, State>> stack{ { a_root, std::move(a_state) } };
		for (std::size_t visited = 0; !stack.empty() && visited < a_limit; ++visited) {
			auto [key, state] = std::move(stack.back());
			stack.pop_back();
			const auto* record = a_mirror.Node(key);
			if (!record) {
				missing = true;
				continue;
			}
			State below = state;
			const Engine::Walk walk = Engine::Detail::Answer(a_visit, key, *record, std::as_const(state), below);
			if (walk == Engine::Walk::Stop)
				return missing;
			if (walk == Engine::Walk::Skip)
				continue;
			for (const void* child : record->children)
				if (child)
					stack.emplace_back(child, below);
		}
		return missing;
	}

	template <class Visit>
	bool ForEachMirror(const SceneMirror& a_mirror, const void* a_root, Visit&& a_visit, std::size_t a_limit = ~std::size_t{ 0 })
	{
		return ForEachMirror(
			a_mirror, a_root, std::uint8_t{ 0 },
			[&](const void* a_key, const SceneCapture::NodeRecord& a_record, std::uint8_t, std::uint8_t&) { return Engine::Detail::Answer(a_visit, a_key, a_record); },
			a_limit);
	}
}
