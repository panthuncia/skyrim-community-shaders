#pragma once

#include <optional>
#include <span>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <source_location>
#include <vector>

#include <winrt/base.h>

#include "RenderGraph/BufferRef.h"
#include "RenderGraph/Async/EventQueue.h"

struct DxvkOrgInteropTrackedResource;

namespace DCLF
{
	/**
	 * @brief The game's D3D11 buffers as the render graph sees them.
	 *
	 * A buffer is described once through DXVK, which makes it stable, and handed out as a lease that holds a reference on it.
	 * An entry lives exactly as long as some lease does. Buffers the game can map cannot be made stable and are not remembered.
	 *
	 * There is one instance per thread domain, and each is its maps' only user, so there is no lock. Scene() is the scene pass's
	 * thread and Frame() is the render thread. Leases may be released on any thread.
	 *
	 * Each description is taken under a reference on the buffer, because the key is the game's pointer and a successor may reuse it.
	 * With pinless imports DXVK may move a buffer. Each import is registered with Relocation::DxvkSource, which hands the lease's owner
	 * the old storage and tells the instance. The entry goes at the next BeginFrame, and the next Acquire imports the new storage.
	 */
	class GpuResources
	{
	public:
		struct Buffer
		{
			std::uint64_t vkBuffer = 0;  // VkBuffer
			std::uint64_t offset = 0;  // offset of the D3D11 buffer within vkBuffer
			std::uint64_t size = 0;
			std::uint64_t address = 0;   // device address of the first byte
			RenderGraph::BufferRef ref;  // null if none
		};
		struct LeasedBuffer
		{
			Buffer buffer;
			std::uint64_t generation = 0;  // unique on successful import, including pointer reuse
			std::shared_ptr<const void> owner;  // pins the D3D11 buffer and keeps the entry
		};

		/** @brief Totals since the instance started. */
		struct Stats
		{
			std::uint32_t cached = 0;
			std::uint32_t rejected = 0;
			std::uint64_t resolvedTotal = 0;
			std::uint64_t prefetchBatches = 0, prefetched = 0;
			double resolveMsTotal = 0.0;
			double resolveMsMax = 0.0;
			// Prefetched descriptions taken by Acquire, and those whose buffer the game had released by then.
			std::uint64_t prefetchTaken = 0, prefetchReleased = 0;
			// Prefetch batches the owner ran itself or waited for, and the time it waited.
			std::uint64_t prefetchRunByOwner = 0, prefetchWaited = 0;
			double prefetchWaitMs = 0.0;
			// Entries a move superseded, and of those, imports a move superseded before they registered.
			std::uint64_t relocated = 0, supersededAtImport = 0;
		};

		/** @brief The scene work's instance, used on the pass's thread. */
		static GpuResources& Scene();
		/** @brief The render thread's instance, for tree LOD meshes. */
		static GpuResources& Frame();

		/** @brief Any thread. Whether resolution is possible, which needs the render graph on DXVK's device. */
		bool Enabled() const;

		/** @brief A lease on the buffer's Vulkan view, resolving it if needed. nullopt if it cannot be made stable. */
		std::optional<LeasedBuffer> Acquire(ID3D11Buffer* a_buffer, std::source_location a_where = std::source_location::current());
		/**
		 * @brief Describes many buffers in one sync with DXVK's worker, on the preparation pool. Returns at once and Acquire finishes it.
		 * Results are kept through the next BeginFrame. Repeats and buffers already held cost nothing.
		 */
		void Prefetch(std::span<ID3D11Buffer* const> a_buffers);

		/** @brief Call before leases are taken. Drops prefetched descriptions and removes entries whose last lease went. */
		void BeginFrame();

		/** @brief The owner's thread, after BeginFrame. Buffers moved by the last relocations, for their holders to acquire again. */
		std::vector<ID3D11Buffer*> TakeRelocated() { return std::exchange(relocatedBuffers, {}); }

		/** @brief Any thread. The counters as the owner last wrote them. */
		Stats GetStats() const;

		/** @brief The owner's thread. Releases everything. */
		void Clear();

	private:
		// A lease's owner. Its last release ends the import, then lets the old storages go (its registration holds them).
		struct Held
		{
			winrt::com_ptr<ID3D11Buffer> reference;
			bool tracked = false;
			std::shared_ptr<const void> refKeep;
			std::shared_ptr<const void> registration;
			~Held();
		};
		struct Entry
		{
			Buffer buffer;
			std::uint64_t generation = 0;
			std::weak_ptr<Held> owner;
		};
		// Keys whose last lease went, pushed by whichever thread released it and drained by BeginFrame.
		using Released = RenderGraph::Frame::EventQueue<ID3D11Buffer*, 1024>;
		// Imports a move superseded, pushed on the committing thread and drained by BeginFrame. The generation tells a new import at
		// the same key apart.
		struct MovedImport
		{
			ID3D11Buffer* buffer = nullptr;
			std::uint64_t generation = 0;
		};
		using Moved = RenderGraph::Frame::EventQueue<MovedImport, 1024>;
		// Written by the owner alone with relaxed load and store, read by any thread.
		struct Counters
		{
			std::atomic<std::uint32_t> cached{ 0 }, rejected{ 0 };
			std::atomic<std::uint64_t> resolvedTotal{ 0 }, prefetchBatches{ 0 }, prefetched{ 0 }, prefetchTaken{ 0 }, prefetchReleased{ 0 };
			std::atomic<std::uint64_t> prefetchRunByOwner{ 0 }, prefetchWaited{ 0 };
			std::atomic<std::uint64_t> relocated{ 0 }, supersededAtImport{ 0 };
			std::atomic<double> resolveMsTotal{ 0.0 }, resolveMsMax{ 0.0 }, prefetchWaitMs{ 0.0 };
		};

		/** @brief A new entry for a resolved buffer, held by a_reference, and its first lease. */
		LeasedBuffer Insert(ID3D11Buffer* a_buffer, winrt::com_ptr<ID3D11Buffer> a_reference, const Buffer& a_resolved,
			const DxvkOrgInteropTrackedResource* a_tracked);
		/** @brief Called by BeginFrame. Each superseded entry goes. Its leases still read the old storage, which their owner holds. */
		void TakeMoved();
		void NoteResolve(double a_ms, std::uint64_t a_buffers);
		ankerl::unordered_dense::map<ID3D11Buffer*, Entry> entries;
		struct Batch;
		/** @brief The owner's thread. Runs the batch itself if no worker took it, otherwise waits for it. */
		void Finish(Batch& a_batch);
		// Kept until the BeginFrame after next, so the current pass and the one before can use them.
		struct Prefetched
		{
			std::shared_ptr<Batch> batch;
			std::uint32_t index = 0;
		};
		ankerl::unordered_dense::map<ID3D11Buffer*, Prefetched> prefetched, prefetchedBefore;
		std::shared_ptr<Released> released = std::make_shared<Released>();
		std::shared_ptr<Moved> moved = std::make_shared<Moved>();
		std::vector<ID3D11Buffer*> relocatedBuffers;
		Counters counters;
	};
}
