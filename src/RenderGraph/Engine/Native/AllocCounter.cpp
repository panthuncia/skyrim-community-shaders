#include "RenderGraph/Engine/Native/AllocCounter.h"

#include <atomic>
#include <cstdlib>
#include <malloc.h>
#include <new>

// The global operator new and delete of CommunityShaders.dll, with a per-thread count while counting is on. They allocate as
// the CRT's do: malloc and free, _aligned_malloc and _aligned_free for the aligned forms.

namespace RenderGraph::Engine::Native::AllocCounter
{
	namespace
	{
		constexpr std::uint32_t kSlots = 1024;

		struct Slot
		{
			std::atomic<std::uint32_t> threadId{ 0 };
			std::atomic<std::uint64_t> allocs{ 0 };
			std::atomic<std::uint64_t> bytes{ 0 };
			std::atomic<std::uint64_t> frees{ 0 };
		};

		struct Ring
		{
			std::atomic<std::uint32_t> threadId{ 0 };
			std::atomic<std::uint32_t> written{ 0 };
			Site sites[kSiteRing];
		};
		Ring* g_rings[kSiteRings]{};

		std::atomic<bool> g_on{ false };
		std::atomic<std::uint32_t> g_used{ 0 };
		Slot g_slots[kSlots];
		// No slot yet (nullptr), or no slot left (the sentinel).
		thread_local Slot* t_slot = nullptr;
		Slot g_none;

		Slot* SlotOfThread()
		{
			Slot* slot = t_slot;
			if (slot)
				return slot == &g_none ? nullptr : slot;
			const std::uint32_t index = g_used.fetch_add(1, std::memory_order_relaxed);
			if (index >= kSlots) {
				t_slot = &g_none;
				return nullptr;
			}
			slot = &g_slots[index];
			slot->threadId.store(GetCurrentThreadId(), std::memory_order_release);
			t_slot = slot;
			return slot;
		}

		// Only the owning thread writes its slot, so a load and a store are enough (no locked add).
		inline void Add(std::atomic<std::uint64_t>& a_value, std::uint64_t a_amount)
		{
			a_value.store(a_value.load(std::memory_order_relaxed) + a_amount, std::memory_order_relaxed);
		}

		inline void CountAlloc(std::size_t a_size)
		{
			if (!g_on.load(std::memory_order_relaxed))
				return;
			if (Slot* slot = SlotOfThread()) {
				const std::uint64_t count = slot->allocs.load(std::memory_order_relaxed) + 1;
				slot->allocs.store(count, std::memory_order_relaxed);
				Add(slot->bytes, a_size);
				if (count % kSiteEvery == 0) {
					const std::uint32_t id = slot->threadId.load(std::memory_order_relaxed);
					for (Ring* ring : g_rings) {
						if (!ring || ring->threadId.load(std::memory_order_relaxed) != id)
							continue;
						const std::uint32_t index = ring->written.load(std::memory_order_relaxed);
						Site& site = ring->sites[index % kSiteRing];
						site.bytes = static_cast<std::uint32_t>(a_size);
						site.count = RtlCaptureStackBackTrace(2, kSiteFrames, site.frames, nullptr);
						ring->written.store(index + 1, std::memory_order_release);
					}
				}
			}
		}

		inline void CountFree(void* a_pointer)
		{
			if (!a_pointer || !g_on.load(std::memory_order_relaxed))
				return;
			if (Slot* slot = SlotOfThread())
				Add(slot->frees, 1);
		}
	}

	void Enable() { g_on.store(true, std::memory_order_relaxed); }

	void TrackSites(std::uint32_t a_ring, std::uint32_t a_threadId)
	{
		if (a_ring >= kSiteRings)
			return;
		if (!g_rings[a_ring])
			g_rings[a_ring] = new Ring;
		g_rings[a_ring]->written.store(0, std::memory_order_relaxed);
		g_rings[a_ring]->threadId.store(a_threadId, std::memory_order_release);
	}

	void StopSites()
	{
		for (Ring* ring : g_rings)
			if (ring)
				ring->threadId.store(0, std::memory_order_release);
	}

	std::uint32_t SiteCount(std::uint32_t a_ring)
	{
		if (a_ring >= kSiteRings || !g_rings[a_ring])
			return 0;
		const std::uint32_t written = g_rings[a_ring]->written.load(std::memory_order_acquire);
		return written < kSiteRing ? written : kSiteRing;
	}

	std::uint32_t SiteWritten(std::uint32_t a_ring)
	{
		return a_ring < kSiteRings && g_rings[a_ring] ? g_rings[a_ring]->written.load(std::memory_order_acquire) : 0;
	}

	const Site& SiteAt(std::uint32_t a_ring, std::uint32_t a_index) { return g_rings[a_ring]->sites[a_index]; }

	std::uint32_t SlotCount()
	{
		const std::uint32_t used = g_used.load(std::memory_order_relaxed);
		return used < kSlots ? used : kSlots;
	}

	void SlotAt(std::uint32_t a_index, std::uint32_t& a_threadId, Totals& a_totals)
	{
		const Slot& slot = g_slots[a_index];
		a_threadId = slot.threadId.load(std::memory_order_acquire);
		a_totals.allocs = slot.allocs.load(std::memory_order_relaxed);
		a_totals.bytes = slot.bytes.load(std::memory_order_relaxed);
		a_totals.frees = slot.frees.load(std::memory_order_relaxed);
	}

	Totals Of(std::uint32_t a_threadId)
	{
		Totals out;
		ForEach([&](std::uint32_t a_id, const Totals& a_totals) {
			if (a_id == a_threadId)
				out = a_totals;
		});
		return out;
	}

	namespace
	{
		void* Allocate(std::size_t a_size)
		{
			CountAlloc(a_size);
			for (;;) {
				if (void* p = std::malloc(a_size ? a_size : 1))
					return p;
				const std::new_handler handler = std::get_new_handler();
				if (!handler)
					return nullptr;
				handler();
			}
		}

		void* AllocateAligned(std::size_t a_size, std::align_val_t a_align)
		{
			CountAlloc(a_size);
			for (;;) {
				if (void* p = _aligned_malloc(a_size ? a_size : 1, static_cast<std::size_t>(a_align)))
					return p;
				const std::new_handler handler = std::get_new_handler();
				if (!handler)
					return nullptr;
				handler();
			}
		}
	}
}

using RenderGraph::Engine::Native::AllocCounter::Allocate;
using RenderGraph::Engine::Native::AllocCounter::AllocateAligned;
using RenderGraph::Engine::Native::AllocCounter::CountFree;

void* operator new(std::size_t a_size)
{
	if (void* p = Allocate(a_size))
		return p;
	throw std::bad_alloc();
}
void* operator new[](std::size_t a_size)
{
	if (void* p = Allocate(a_size))
		return p;
	throw std::bad_alloc();
}
void* operator new(std::size_t a_size, const std::nothrow_t&) noexcept { return Allocate(a_size); }
void* operator new[](std::size_t a_size, const std::nothrow_t&) noexcept { return Allocate(a_size); }
void* operator new(std::size_t a_size, std::align_val_t a_align)
{
	if (void* p = AllocateAligned(a_size, a_align))
		return p;
	throw std::bad_alloc();
}
void* operator new[](std::size_t a_size, std::align_val_t a_align)
{
	if (void* p = AllocateAligned(a_size, a_align))
		return p;
	throw std::bad_alloc();
}
void* operator new(std::size_t a_size, std::align_val_t a_align, const std::nothrow_t&) noexcept { return AllocateAligned(a_size, a_align); }
void* operator new[](std::size_t a_size, std::align_val_t a_align, const std::nothrow_t&) noexcept { return AllocateAligned(a_size, a_align); }

void operator delete(void* a_pointer) noexcept
{
	CountFree(a_pointer);
	std::free(a_pointer);
}
void operator delete[](void* a_pointer) noexcept
{
	CountFree(a_pointer);
	std::free(a_pointer);
}
void operator delete(void* a_pointer, std::size_t) noexcept
{
	CountFree(a_pointer);
	std::free(a_pointer);
}
void operator delete[](void* a_pointer, std::size_t) noexcept
{
	CountFree(a_pointer);
	std::free(a_pointer);
}
void operator delete(void* a_pointer, const std::nothrow_t&) noexcept
{
	CountFree(a_pointer);
	std::free(a_pointer);
}
void operator delete[](void* a_pointer, const std::nothrow_t&) noexcept
{
	CountFree(a_pointer);
	std::free(a_pointer);
}
void operator delete(void* a_pointer, std::align_val_t) noexcept
{
	CountFree(a_pointer);
	_aligned_free(a_pointer);
}
void operator delete[](void* a_pointer, std::align_val_t) noexcept
{
	CountFree(a_pointer);
	_aligned_free(a_pointer);
}
void operator delete(void* a_pointer, std::size_t, std::align_val_t) noexcept
{
	CountFree(a_pointer);
	_aligned_free(a_pointer);
}
void operator delete[](void* a_pointer, std::size_t, std::align_val_t) noexcept
{
	CountFree(a_pointer);
	_aligned_free(a_pointer);
}
void operator delete(void* a_pointer, std::align_val_t, const std::nothrow_t&) noexcept
{
	CountFree(a_pointer);
	_aligned_free(a_pointer);
}
void operator delete[](void* a_pointer, std::align_val_t, const std::nothrow_t&) noexcept
{
	CountFree(a_pointer);
	_aligned_free(a_pointer);
}
