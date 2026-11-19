#pragma once

// The render thread's mirror of the engine's grass shapes. The capture sink pushes events from any thread, the frame drains them,
// and the depth commit uploads what changed into the GPU tables (GrassTables.cpp).
// Shapes live in slots, their instances in ranges of one flat table.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <winrt/base.h>

#include "Features/DrawcallLimitFix/Features/Grass/GrassLayout.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

struct ID3D11Buffer;

namespace DCLF::Features::Grass
{
	/** @brief A grass shape's renderer mesh, or a Grass Optimizations LOD mesh. */
	struct MeshSource
	{
		const void* key = nullptr;  // the BSGraphics::TriShape, or the LOD mesh entry
		winrt::com_ptr<ID3D11Buffer> vertexBuffer, indexBuffer;  // held from the sink until the render thread leases them
		std::uint64_t vertexDesc = 0;
		std::uint32_t indexCount = 0;
		float canopyHeight = 1.0f;
		float occlusionRadius = 0.0f;
	};

	/** @brief Everything that must match for shapes to share one bucket. Wind direction matches Grass Optimizations' draw key. */
	struct BucketKey
	{
		const void* mesh = nullptr;
		const void* material = nullptr;
		const void* diffuse = nullptr;
		std::uint64_t vertexDesc = 0;
		std::uint32_t clampMode = 0;
		float alphaRef = 0.0f;
		std::array<float, 2> windDirection{ 0.0f, 1.0f };
		bool pbr = false, sphereNormal = false, uniformScale = false;
		bool operator==(const BucketKey&) const = default;
	};

	struct BucketKeyHash
	{
		using is_avalanching = void;
		std::uint64_t operator()(const BucketKey& a_key) const noexcept;
	};

	/** @brief A bucket's material. The textures stay referenced while the bucket lives. */
	struct BucketSource
	{
		RE::NiPointer<RE::NiSourceTexture> diffuse, normal, rmaos, features0, features1, emissive;
		float boundCenter[3]{};
		float modelRadius = 0.0f;
		float wavePeriod = 1.0f;
		std::uint32_t triangleCount = 0;
		std::uint32_t pbrFlags = 0;
		float pbrParams1[3]{}, pbrParams2[4]{}, pbrParams3[4]{}, pbrParams4[4]{};
		float fuzzRoughness = -1.0f;
	};

	/**
	 * @brief One capture event. Remove fires for every BSMultiStreamInstanceTriShape destroyed, grass or not.
	 * Rejected means the engine still draws the group, so the family must stay the engine's while it lives.
	 */
	struct Event
	{
		enum class Kind : std::uint8_t
		{
			Add,
			RemoveGroup,
			Remove,
			Rejected
		};
		Kind kind = Kind::Add;
		std::uint64_t seq = 0;        // orders a shape's events across the capture's threads
		const void* shape = nullptr;  // a key only, never dereferenced. The shape may be dead.
		std::uint32_t group = 0;      // unused by Remove
		void* lodMesh = nullptr;      // GrassMeshLibrary::RegisteredMesh, null until LoadGrassType names it
		std::vector<GrassInstances::Instance> instances;
		std::array<float, 3> origin{};
		BucketKey key;
		BucketSource source;
		MeshSource mesh;
	};

	/** @brief Pushed from any thread by GrassShared's capture sink, drained on the render thread. */
	inline EventQueue<Event> events;
	inline bool installed = false;

	/** @brief Safe from any thread. */
	void Push(Event&& a_event);

	/** @brief One group of a shape, which takes one shape slot. The engine adds and removes groups per cell. */
	struct Group
	{
		std::vector<GrassInstances::Instance> instances;
		std::uint32_t bucket = 0;  // bucket slot
		std::uint32_t first = 0;   // start of its range in the instance table
		GrassInstances::ShapeRow row;
	};

	/** @brief A mirrored shape's groups by engine index, with the seq that last set each. */
	struct Shape
	{
		static constexpr std::uint32_t kRejected = ~0u;
		struct GroupSlot
		{
			std::uint32_t slot = kRejected;
			std::uint64_t seq = 0;
		};
		ankerl::unordered_dense::map<std::uint32_t, GroupSlot> slots;
		std::uint32_t records = 0;
	};

	struct GroupKey
	{
		const void* shape = nullptr;
		std::uint32_t group = 0;
		bool operator==(const GroupKey&) const = default;
	};

	struct GroupKeyHash
	{
		using is_avalanching = void;
		std::uint64_t operator()(const GroupKey& a_key) const noexcept;
	};

	/** @brief A mesh shared by buckets. */
	struct Mesh
	{
		std::uint32_t slot = 0;
		std::uint32_t users = 0;
		MeshSource source;
		GrassInstances::MeshRow row;
		std::shared_ptr<const void> vertexOwner, indexOwner;  // GpuResources leases
		bool resolved = false, failed = false;
	};

	/** @brief A bucket of shapes drawn alike. The depth commit resolves its texture bindings into the row. */
	struct Bucket
	{
		BucketKey key;
		BucketSource source;
		std::uint32_t slot = 0;
		std::uint32_t shapes = 0;  // shape groups drawn in it
		std::uint32_t total = 0;
		std::array<const void*, GrassInstances::kTiers> meshes{};
		// Shared with Grass Optimizations. The bucket holds the record's activeBuckets while it lives.
		void* lodMesh = nullptr;
		bool lodPending = false;
		OwnedRow<GrassInstances::BucketRow> row;
		std::uint32_t bindingGeneration = ~0u;           // GpuTextures::Generation the bindings were resolved in
		bool bound = false;
		bool live = false;
	};

	struct Stats
	{
		std::uint64_t adds = 0, removes = 0, groupRemoves = 0, stale = 0, rejected = 0;
	};

	/**
	 * @brief The render thread's mirror of every captured grass shape, laid out in slots for the GPU tables.
	 *
	 * Events can arrive out of order, so Drain applies them by seq per group. An event older than the group's last one, or than a
	 * recent removal of the group or shape, is dropped as stale. A shape removal only takes groups older than it, because a later one
	 * belongs to a new shape at the same address.
	 *
	 * When any bucket's instance total changes, every bucket's visible bases are laid out again and every bucket row is resent.
	 */
	class Mirror
	{
	public:
		using Shapes = ankerl::unordered_dense::map<const void*, Shape>;
		using Buckets = std::vector<Bucket>;  // by bucket slot
		// The queue's reorder window is a drain or two, so removals are remembered for longer than that.
		static constexpr std::uint32_t kTombstoneDrains = 16;

		void Drain(std::uint32_t a_frame);
		std::size_t Size() const { return shapes.size(); }
		std::uint64_t InstanceCount() const { return instanceCount; }
		std::size_t BucketCount() const { return bucketIndex.size(); }
		std::size_t MeshCount() const { return meshes.size(); }
		/** @brief Live groups the engine still draws, which keep the family the engine's. */
		std::size_t RejectedLive() const { return rejectedGroups; }
		/** @brief Summed engine frame time for the spawn fade, like Grass Optimizations' timeAccum. */
		float FadeClock() const { return fadeClock; }
		Stats TakeStats() { return std::exchange(stats, {}); }
		struct Readiness
		{
			std::uint32_t meshesResolved = 0, meshesFailed = 0, meshesPending = 0, bucketsDrawable = 0;
		};
		Readiness ReadinessOf() const;
		const Shapes& MirroredShapes() const { return shapes; }
		const Buckets& MirroredBuckets() const { return buckets; }

		/** @brief High-water marks, which the GPU tables must hold. */
		std::uint32_t InstanceSlots() const { return instanceSlots; }
		std::uint32_t ShapeSlots() const { return static_cast<std::uint32_t>(slotShapes.size()); }
		std::uint32_t BucketSlots() const { return static_cast<std::uint32_t>(buckets.size()); }
		std::uint32_t MeshSlots() const { return static_cast<std::uint32_t>(meshSlots.size()); }
		/** @brief Resend everything, for a new table backing. */
		void MarkAllChanged();

		struct Changes
		{
			std::vector<std::uint32_t> shapes, buckets, meshes;
			std::vector<std::pair<std::uint32_t, std::uint32_t>> freedRanges;  // instance ranges whose owners become ~0u
		};
		/** @brief Leases new meshes first. A mesh that cannot be leased draws nothing, and its tier names kNoMesh. */
		void TakeChanges(Changes& a_changes);
		const GrassInstances::ShapeRow& SlotRow(std::uint32_t a_slot) const;
		const std::vector<GrassInstances::Instance>* SlotInstances(std::uint32_t a_slot) const;
		const GrassInstances::MeshRow& MeshSlotRow(std::uint32_t a_slot) const;
		/** @brief Null when the slot is free. */
		Bucket* SlotBucket(std::uint32_t a_slot);
		/** @brief Resend a bucket row at the next commit. */
		void MarkBucket(std::uint32_t a_slot);
		/** @brief Leases no table names any more. The next execution holds them until it retires. */
		std::vector<std::shared_ptr<const void>> TakeRetired() { return std::exchange(retired, {}); }

	private:
		bool Stale(const Event& a_event) const;
		void Apply(Event&& a_event);
		void RemoveShape(const void* a_shape, std::uint64_t a_seq);
		void RemoveGroup(Shape& a_shape, std::uint32_t a_group);
		void ReleaseGroup(Group& a_group);
		std::uint32_t AcquireSlot();
		std::uint32_t AllocateRange(std::uint32_t a_count);
		void FreeRange(std::uint32_t a_first, std::uint32_t a_count);
		void MarkSlot(std::uint32_t a_slot);
		Bucket& AddBucketUser(Event& a_event);
		void RemoveBucketUser(std::uint32_t a_slot, std::uint32_t a_instances);
		void AdoptLodMesh(Bucket& a_bucket, void* a_lodMesh);
		void ResolveLods();
		const void* AddMeshUser(MeshSource&& a_source);
		void RemoveMeshUser(const void* a_key);
		void LayOutVisible();
		void PruneTombstones();

		Shapes shapes;
		Buckets buckets;
		ankerl::unordered_dense::map<BucketKey, std::uint32_t, BucketKeyHash> bucketIndex;
		std::vector<const void*> slotShapes;  // null when free
		std::vector<Group> groups;  // by shape slot
		std::vector<std::uint32_t> freeSlots, changedSlots;
		std::vector<std::uint8_t> slotChanged;
		std::vector<std::uint32_t> freeBuckets, changedBuckets;
		std::vector<std::uint8_t> bucketChanged;
		bool totalsChanged = false;
		ankerl::unordered_dense::map<const void*, Mesh> meshes;
		std::vector<const void*> meshSlots;
		std::vector<std::uint32_t> freeMeshSlots, changedMeshes;
		std::uint32_t instanceSlots = 0;
		std::uint64_t instanceCount = 0;
		std::map<std::uint32_t, std::uint32_t> freeRanges;  // first to count, coalesced
		std::vector<std::pair<std::uint32_t, std::uint32_t>> freedRanges;
		// Removal seq and the drain it happened in, so stale events after a removal are dropped.
		ankerl::unordered_dense::map<const void*, std::pair<std::uint64_t, std::uint32_t>> tombstones;
		ankerl::unordered_dense::map<GroupKey, std::pair<std::uint64_t, std::uint32_t>, GroupKeyHash> groupTombstones;
		std::size_t rejectedGroups = 0;
		std::uint32_t lodPending = 0;
		std::vector<std::shared_ptr<const void>> retired;
		std::uint32_t drains = 0;
		float fadeClock = 0.0f;
		Stats stats;
	};

	/** @brief See dclf-architecture.md, "Grass: the draws". Drained every frame, whether grass is DCLF's or not. */
	Mirror& RenderMirror();
}
