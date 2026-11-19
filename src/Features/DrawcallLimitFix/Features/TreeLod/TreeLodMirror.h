#pragma once

// Tree LOD's event channel and the render thread's mirror of the engine's tree LOD groups, in slots for the TreeInstances tables.
// Included by the feature's sources only.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <winrt/base.h>

#include "Features/DrawcallLimitFix/Features/TreeLod/TreeInstancesLayout.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

struct ID3D11Buffer;

namespace DCLF::Features::TreeLod
{
	/*
	 * Tree LOD (AE 1.6.1170; dclf-architecture.md, "Tree LOD: what the engine does").
	 * Each distant tree group is one BSMultiStreamInstanceTriShape under the LOD trees root (0x14315b880). Its records exist only
	 * at AddGroup (vfunc 0x1E0) and RemoveGroup (vfunc 0x1E8), called from the refill FUN_140504a40 and the detach FUN_1405033d0.
	 * So the mirror takes them there as events and never reads the blocks per frame.
	 */

	/** @brief A shape's mesh, as AddGroup's shape holds it. */
	struct MeshSource
	{
		const void* key = nullptr;  // the BSGraphics::TriShape
		winrt::com_ptr<ID3D11Buffer> vertexBuffer, indexBuffer;  // held from the hook until the render thread leases them
		std::uint64_t vertexDesc = 0;
		std::uint32_t indexCount = 0;
	};

	/** @brief A group's records as AddGroup took them or as the rule packs them after a block update, or a removal. */
	struct Event
	{
		enum class Kind : std::uint8_t
		{
			Add,
			Remove
		};
		const void* shape = nullptr;  // a key: never dereferenced by the drain
		const void* block = nullptr;  // BGSDistantTreeBlock and TreeGroup, for the parity's repack (Add)
		const void* group = nullptr;
		std::vector<TreeInstances::Instance> instances;
		Kind kind = Kind::Add;
		bool intended = false;  // packed by the rule after a block update, not given to AddGroup (TreeLodMirror.cpp, BlockUpdate)
		std::array<float, 3> translate{};
		float extent = 0.0f;
		MeshSource mesh;
	};

	/** @brief The engine's tree LOD groups as events, pushed by the hooks on any thread and drained by the render thread. */
	inline EventQueue<Event> events;
	inline bool installed = false;

	/** @brief Pushes a_event from a hook on any thread. Pushes off the drain's thread are counted. */
	void Push(Event&& a_event);

	/** @brief Packs a group's records by the refill's rule (FUN_140504a40), with the engine's own conversions. */
	void PackGroup(bool a_allVisible, const RE::BGSDistantTreeBlock::TreeGroup& a_group, std::vector<TreeInstances::Instance>& a_out);
	/** @brief The LOD trees root, the shapes' parent (0x14315b880). */
	RE::NiNode* LodTreesRoot();
	/** @brief Events pushed off the drain's thread since the last call. */
	std::uint64_t TakeOffThread();

	/**
	 * @brief A mirrored group. Its records are the engine's, except after a block update, where the engine keeps hidden
	 * records that the rule shows (BlockUpdate).
	 */
	struct Group
	{
		const void* block = nullptr;
		const void* group = nullptr;
		std::vector<TreeInstances::Instance> instances;
		std::uint32_t slot = 0;
		const void* mesh = nullptr;
		TreeInstances::ShapeRow row;
	};

	struct Mesh
	{
		std::uint32_t slot = 0;
		std::uint32_t users = 0;
		MeshSource source;
		TreeInstances::MeshRow row;
		std::shared_ptr<const void> vertexOwner, indexOwner;  // GpuResources leases
		bool resolved = false, failed = false;
	};

	struct Stats
	{
		std::uint64_t adds = 0, removes = 0, intended = 0;
	};

	/**
	 * @brief Every tree LOD shape the engine has, with the records it draws, in slots for the GPU's tables. Render thread.
	 * Drain applies events in push order. Changed slots and new meshes wait for the depth commit (TakeChanges).
	 */
	class Mirror
	{
	public:
		using Groups = ankerl::unordered_dense::map<const void*, Group>;

		void Drain(std::uint32_t a_frame);
		std::size_t Size() const { return groups.size(); }
		std::size_t InstanceCount() const;
		Stats TakeStats() { return std::exchange(stats, {}); }
		/** @brief The mirrored groups by shape, for parity. */
		const Groups& MirroredGroups() const { return groups; }

		/** @brief The slot high-water marks, which the GPU's tables must hold. */
		std::uint32_t ShapeSlots() const { return static_cast<std::uint32_t>(slotGroups.size()); }
		std::uint32_t MeshSlots() const { return static_cast<std::uint32_t>(meshSlots.size()); }
		/** @brief Marks every slot and mesh changed, for a new table backing. */
		void MarkAllChanged();
		/**
		 * @brief The changed shape and mesh slots since the last call, each once.
		 * A new mesh that cannot be leased draws nothing, and its shapes' rows name mesh slot ~0u.
		 */
		void TakeChanges(std::vector<std::uint32_t>& a_slots, std::vector<std::uint32_t>& a_meshes);
		/** @brief A shape slot's row (count 0 when free) and records. */
		const TreeInstances::ShapeRow& SlotRow(std::uint32_t a_slot) const;
		const std::vector<TreeInstances::Instance>* SlotInstances(std::uint32_t a_slot) const;
		const TreeInstances::MeshRow& MeshSlotRow(std::uint32_t a_slot) const;
		/** @brief The largest index count of the meshes, the draw's vertices per instance. */
		std::uint32_t MaxIndices() const { return maxIndices; }
		/** @brief Leases of meshes no shape draws any more. The next execution holds them until it retires. */
		std::vector<std::shared_ptr<const void>> TakeRetired() { return std::exchange(retired, {}); }

	private:
		std::uint32_t AcquireSlot();
		void ReleaseSlot(std::uint32_t a_slot);
		void MarkSlot(std::uint32_t a_slot);
		const void* AddMeshUser(MeshSource&& a_source);
		void RemoveMeshUser(const void* a_key);

		Groups groups;
		std::vector<const void*> slotGroups;
		std::vector<std::uint32_t> freeSlots;
		std::vector<std::uint32_t> changedSlots;
		std::vector<std::uint8_t> slotChanged;
		ankerl::unordered_dense::map<const void*, Mesh> meshes;
		std::vector<const void*> meshSlots;  // each mesh slot's key (null when free)
		std::vector<std::uint32_t> freeMeshSlots;
		std::vector<std::uint32_t> changedMeshes;
		std::vector<std::shared_ptr<const void>> retired;
		std::uint32_t maxIndices = 0;
		Stats stats;
	};

	/** @brief The render thread's mirror. Drained at the frame requests even when not running, and uploaded by the depth commit. */
	Mirror& RenderMirror();
}
