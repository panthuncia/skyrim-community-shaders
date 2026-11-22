// Buffer resolution and leases. Each instance runs on its owner's thread.

#include "Features/DrawcallLimitFix/Core/Gpu/GpuResources.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"

#include <chrono>
#include <exception>
#include <Tracy/Tracy.hpp>

#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "RenderGraph/DxvkImageImport.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/Engine/Buffers/EngineBuffers.h"
#include "RenderGraph/Relocation/DxvkSource.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/UndeclaredAccess.h"

#include <OpenRenderGraph/GraphHost.h>
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF
{
	namespace
	{
		// Shared by the instances, so a generation names one import whichever instance made it.
		std::atomic<std::uint64_t> nextGeneration{ 1 };

		// Only the owner writes these counters, so a relaxed load and store is enough.
		template <class T, class U>
		void Bump(std::atomic<T>& a_counter, U a_amount)
		{
			a_counter.store(a_counter.load(std::memory_order_relaxed) + static_cast<T>(a_amount), std::memory_order_relaxed);
		}
	}

	struct GpuResources::Batch
	{
		std::vector<winrt::com_ptr<ID3D11Buffer>> references;  // moved into the leases once done
		std::vector<IUnknown*> objects;
		std::vector<DxvkOrgInteropResourceInfo> infos;
		std::vector<DxvkOrgInteropTrackedResource> tracked;
		std::vector<HRESULT> results;
		bool pinless = false;
		// Begun before the description. Moves taken until the batch goes wait for its registrations.
		RenderGraph::Relocation::DxvkSource::Ticket ticket;
		bool described = false;  // otherwise Acquire describes each one alone
		double ms = 0.0;
		std::atomic<bool> claimed{ false };  // the job or the owner, whichever describes first
		std::atomic<bool> done{ false };
		bool noted = false;

		/** @brief Run by whoever claimed the batch. Touches only the batch. */
		void Describe()
		{
			const auto start = std::chrono::steady_clock::now();
			described = pinless ? RenderGraphRuntime::Get().TrackResources(objects, tracked, results) : RenderGraphRuntime::Get().DescribeResources(objects, infos, results);
			ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		}

		const DxvkOrgInteropResourceInfo& Info(std::uint32_t a_index) const { return pinless ? tracked[a_index].info : infos[a_index]; }

		~Batch()
		{
			// Tracked imports no lease took end with the batch, on whichever thread drops it last.
			if (!pinless || !described)
				return;
			std::vector<IUnknown*> untaken;
			for (std::size_t i = 0; i < references.size(); ++i)
				if (SUCCEEDED(results[i]) && references[i])
					untaken.push_back(references[i].get());
			RenderGraphRuntime::Get().UntrackResources(untaken);
		}
	};

	GpuResources::Held::~Held()
	{
		// Leases are retired behind the GPU's use. End the import first, then release the storages it read.
		if (tracked) {
			IUnknown* object = reference.get();
			RenderGraphRuntime::Get().UntrackResources({ &object, 1 });
		}
		registration.reset();
	}

	GpuResources& GpuResources::Scene()
	{
		static GpuResources resources;
		return resources;
	}

	GpuResources& GpuResources::Frame()
	{
		static GpuResources resources;
		return resources;
	}

	bool GpuResources::Enabled() const
	{
		return RenderGraphRuntime::Get().IsActive();
	}

	std::optional<GpuResources::LeasedBuffer> GpuResources::Acquire(ID3D11Buffer* a_buffer, std::source_location a_where)
	{
		if (!a_buffer)
			return std::nullopt;
		// ORG-owned engine buffers are checked where their ref is made. Noting them here too would report ring chunks as undeclared.
		if (RenderGraph::UndeclaredAccess::On() && !RenderGraph::Engine::Buffers::Owns(a_buffer))
			RenderGraph::UndeclaredAccess::NoteBuffer(a_buffer, "GpuResources::Acquire", a_where);
		if (const auto it = entries.find(a_buffer); it != entries.end()) {
			if (auto owner = it->second.owner.lock())
				return LeasedBuffer{ it->second.buffer, it->second.generation, std::shared_ptr<const void>(std::move(owner)) };
			// Its last lease went, so a buffer at this address now is a new one.
			entries.erase(it);
			Bump(counters.cached, -1);
		}

		// An ORG-owned engine buffer is read directly and never moves. Never pass it to DXVK, since it may be a tripwire handle.
		if (const auto direct = RenderGraph::Engine::Buffers::Resolve(a_buffer)) {
			if (!direct->address) {
				Bump(counters.rejected, 1);
				return std::nullopt;
			}
			winrt::com_ptr<ID3D11Buffer> reference;
			reference.copy_from(a_buffer);
			return Insert(a_buffer, std::move(reference), Buffer{ direct->vkBuffer, 0, direct->size, direct->address, direct->ref }, nullptr);
		}

		// A prefetch from this pass or the last was taken under its own reference, so the description still matches this buffer.
		for (auto* map : { &prefetched, &prefetchedBefore }) {
			const auto it = map->find(a_buffer);
			if (it == map->end())
				continue;
			const auto batch = std::move(it->second.batch);
			const std::uint32_t i = it->second.index;
			map->erase(it);
			Finish(*batch);
			if (!batch->described)
				break;  // no batched export, described alone below
			auto& reference = batch->references[i];
			if (Diagnostics::Observe().parity) {
				Bump(counters.prefetchTaken, 1);
				reference->AddRef();
				Bump(counters.prefetchReleased, reference->Release() == 1 ? 1u : 0u);
			}
			const auto& info = batch->Info(i);
			if (SUCCEEDED(batch->results[i]) && info.kind == DXVK_ORG_INTEROP_RESOURCE_BUFFER && info.buffer.address != 0) {
				// The batch's ticket covers a move since the description: the registration takes its old storage.
				return Insert(a_buffer, std::move(reference),
					Buffer{ reinterpret_cast<std::uint64_t>(info.buffer.buffer), info.buffer.offset, info.buffer.size, info.buffer.address },
					batch->pinless ? &batch->tracked[i] : nullptr);
			}
			Bump(counters.rejected, 1);
			return std::nullopt;
		}

		const auto start = std::chrono::steady_clock::now();
		winrt::com_ptr<ID3D11Buffer> reference;
		reference.copy_from(a_buffer);
		auto& runtime = RenderGraphRuntime::Get();
		std::optional<LeasedBuffer> lease;
		auto insert = [&](const DxvkOrgInteropResourceInfo& a_info, const DxvkOrgInteropTrackedResource* a_tracked) {
			lease = Insert(a_buffer, std::move(reference),
				Buffer{ reinterpret_cast<std::uint64_t>(a_info.buffer.buffer), a_info.buffer.offset, a_info.buffer.size, a_info.buffer.address }, a_tracked);
		};
		if (runtime.PinlessImports()) {
			// Covers a move between the description and the registration in Insert.
			const auto ticket = RenderGraph::Relocation::DxvkSource::Begin();
			IUnknown* object = a_buffer;
			DxvkOrgInteropTrackedResource tracked{};
			HRESULT result = E_FAIL;
			const bool imported = runtime.TrackResources({ &object, 1 }, { &tracked, 1 }, { &result, 1 }) && SUCCEEDED(result);
			if (imported && tracked.info.kind == DXVK_ORG_INTEROP_RESOURCE_BUFFER && tracked.info.buffer.address != 0)
				insert(tracked.info, &tracked);
			else if (imported)
				runtime.UntrackResources({ &object, 1 });
		} else {
			DxvkOrgInteropResourceInfo info{};
			if (runtime.DescribeResource(a_buffer, info) && info.kind == DXVK_ORG_INTEROP_RESOURCE_BUFFER && info.buffer.address != 0)
				insert(info, nullptr);
		}
		if (!lease)
			Bump(counters.rejected, 1);
		NoteResolve(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), 1);
		return lease;
	}

	GpuResources::LeasedBuffer GpuResources::Insert(ID3D11Buffer* a_buffer, winrt::com_ptr<ID3D11Buffer> a_reference, const Buffer& a_resolved,
		const DxvkOrgInteropTrackedResource* a_tracked)
	{
		Entry entry;
		entry.buffer = a_resolved;
		entry.generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);
		if (!entry.generation)
			std::terminate();
		// The lease holds the reference the description was taken under, so the address cannot be reused while it lives.
		auto* held = new Held;
		held->reference = std::move(a_reference);
		held->tracked = a_tracked != nullptr;
		if (!entry.buffer.ref)
			if (auto* host = RenderGraphRuntime::Get().Host())
				entry.buffer.ref = RenderGraph::ImportBufferRef(host->Device(), entry.buffer.vkBuffer, entry.buffer.offset, entry.buffer.size, entry.buffer.address, held->refKeep);
		if (a_tracked) {
			// The registration holds the old storage of each later move, and the entry goes at the next BeginFrame.
			const auto moveOf = [moved = moved, key = a_buffer, generation = entry.generation] { moved->Push({ key, generation }); };
			auto registration = RenderGraph::Relocation::DxvkSource::Hold(a_tracked->cookie, a_tracked->storageSerial, moveOf);
			// A move since the description: this lease reads the old storage, which the registration holds.
			if (registration && registration->Superseded()) {
				Bump(counters.supersededAtImport, 1);
				moveOf();
			}
			held->registration = std::move(registration);
		}
		std::shared_ptr<Held> owner(held, [released = released, key = a_buffer](Held* a_held) {
			delete a_held;
			released->Push(key);
		});
		entry.owner = owner;
		LeasedBuffer lease{ entry.buffer, entry.generation, std::shared_ptr<const void>(std::move(owner)) };
		entries.insert_or_assign(a_buffer, std::move(entry));
		Bump(counters.cached, 1);
		return lease;
	}

	void GpuResources::TakeMoved()
	{
		relocatedBuffers.clear();  // Frame()'s holders never re-acquire
		moved->Drain([this](MovedImport&& a_moved) {
			// An entry made since at the same key is another import.
			const auto e = entries.find(a_moved.buffer);
			if (e == entries.end() || e->second.generation != a_moved.generation)
				return;
			relocatedBuffers.push_back(a_moved.buffer);
			entries.erase(e);
			Bump(counters.cached, -1);
			Bump(counters.relocated, 1);
		});
	}

	void GpuResources::NoteResolve(double a_ms, std::uint64_t a_buffers)
	{
		Bump(counters.resolvedTotal, a_buffers);
		Bump(counters.resolveMsTotal, a_ms);
		if (a_ms > counters.resolveMsMax.load(std::memory_order_relaxed))
			counters.resolveMsMax.store(a_ms, std::memory_order_relaxed);
	}

	void GpuResources::Prefetch(std::span<ID3D11Buffer* const> a_buffers)
	{
		std::shared_ptr<Batch> batch;
		for (auto* buffer : a_buffers) {
			if (!buffer || prefetched.contains(buffer) || prefetchedBefore.contains(buffer))
				continue;
			if (const auto it = entries.find(buffer); it != entries.end() && !it->second.owner.expired())
				continue;
			// ORG-owned buffers need no description.
			if (RenderGraph::Engine::Buffers::Owns(buffer))
				continue;
			if (!batch)
				batch = std::make_shared<Batch>();
			// Described on the pool under this reference, taken here on the owner's thread.
			prefetched.emplace(buffer, Prefetched{ batch, static_cast<std::uint32_t>(batch->objects.size()) });
			batch->references.emplace_back().copy_from(buffer);
			batch->objects.push_back(buffer);
		}
		if (!batch)
			return;
		batch->pinless = RenderGraphRuntime::Get().PinlessImports();
		if (batch->pinless) {
			batch->ticket = RenderGraph::Relocation::DxvkSource::Begin();
			batch->tracked.resize(batch->objects.size());
		}
		else
			batch->infos.resize(batch->objects.size());
		batch->results.assign(batch->objects.size(), E_FAIL);
		Bump(counters.prefetchBatches, 1);
		Bump(counters.prefetched, batch->objects.size());
		// The job holds the batch, so its answers outlive whatever the owner drops. A refused job is left to Finish.
		::RenderGraph::Frame::SharedExecutor().Dispatch(SceneScheduler::Scope(), ::RenderGraph::Frame::Executor::Preparation, org::async::TaskDispatch::Cpu, "DCLF buffer prefetch",
			[batch](const auto&) {
				DCLF_ZONE("CS.DCLF.Scene.PrefetchBuffers.Describe");
				if (batch->claimed.exchange(true, std::memory_order_acq_rel))
					return;
				batch->Describe();
				batch->done.store(true, std::memory_order_release);
				batch->done.notify_all();
			});
	}

	void GpuResources::Finish(Batch& a_batch)
	{
		if (!a_batch.done.load(std::memory_order_acquire)) {
			const auto start = std::chrono::steady_clock::now();
			if (!a_batch.claimed.exchange(true, std::memory_order_acq_rel)) {
				// Not started because the pool is busy, so describe it here.
				a_batch.Describe();
				a_batch.done.store(true, std::memory_order_relaxed);
				Bump(counters.prefetchRunByOwner, 1);
			} else {
				// Already running on a worker, so wait for it.
				DCLF_ZONE("CS.DCLF.Scene.PrefetchBuffers.Wait");
				a_batch.done.wait(false, std::memory_order_acquire);
				Bump(counters.prefetchWaited, 1);
				Bump(counters.prefetchWaitMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
			}
		}
		if (!std::exchange(a_batch.noted, true) && a_batch.described)
			NoteResolve(a_batch.ms, a_batch.objects.size());
	}

	void GpuResources::BeginFrame()
	{
		// The pass before's batches go now. This pass's stay one more.
		prefetchedBefore = std::move(prefetched);
		prefetched.clear();
		released->Drain([this](ID3D11Buffer*&& a_key) {
			// A key resolved again since has a live lease, so that entry stays.
			if (const auto it = entries.find(a_key); it != entries.end() && it->second.owner.expired()) {
				entries.erase(it);
				Bump(counters.cached, -1);
			}
		});
		TakeMoved();
	}

	GpuResources::Stats GpuResources::GetStats() const
	{
		Stats stats;
		stats.cached = counters.cached.load(std::memory_order_relaxed);
		stats.rejected = counters.rejected.load(std::memory_order_relaxed);
		stats.resolvedTotal = counters.resolvedTotal.load(std::memory_order_relaxed);
		stats.prefetchBatches = counters.prefetchBatches.load(std::memory_order_relaxed);
		stats.prefetched = counters.prefetched.load(std::memory_order_relaxed);
		stats.resolveMsTotal = counters.resolveMsTotal.load(std::memory_order_relaxed);
		stats.resolveMsMax = counters.resolveMsMax.load(std::memory_order_relaxed);
		stats.prefetchTaken = counters.prefetchTaken.load(std::memory_order_relaxed);
		stats.prefetchReleased = counters.prefetchReleased.load(std::memory_order_relaxed);
		stats.prefetchRunByOwner = counters.prefetchRunByOwner.load(std::memory_order_relaxed);
		stats.prefetchWaited = counters.prefetchWaited.load(std::memory_order_relaxed);
		stats.prefetchWaitMs = counters.prefetchWaitMs.load(std::memory_order_relaxed);
		stats.relocated = counters.relocated.load(std::memory_order_relaxed);
		stats.supersededAtImport = counters.supersededAtImport.load(std::memory_order_relaxed);
		return stats;
	}

	void GpuResources::Clear()
	{
		entries.clear();
		prefetched.clear();
		prefetchedBefore.clear();
		released->Discard();
		moved->Discard();
		relocatedBuffers.clear();
		for (auto* counter : { &counters.cached, &counters.rejected })
			counter->store(0, std::memory_order_relaxed);
		for (auto* counter : { &counters.resolvedTotal, &counters.prefetchBatches, &counters.prefetched, &counters.prefetchTaken, &counters.prefetchReleased,
				 &counters.prefetchRunByOwner, &counters.prefetchWaited, &counters.relocated, &counters.supersededAtImport })
			counter->store(0, std::memory_order_relaxed);
		counters.resolveMsTotal.store(0.0, std::memory_order_relaxed);
		counters.prefetchWaitMs.store(0.0, std::memory_order_relaxed);
		counters.resolveMsMax.store(0.0, std::memory_order_relaxed);
	}
}
