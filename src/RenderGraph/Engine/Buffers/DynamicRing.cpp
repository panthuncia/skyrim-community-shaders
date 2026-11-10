// The engine's dynamic ring (75484 MapDynamicBuffer) as an ORG upload ring (EngineBuffers.h, docs/development/p4/p4.5-buffers.md).

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/Engine/Buffers/BufferBacking.h"
#	include "RenderGraph/Engine/Buffers/EngineBuffers.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <algorithm>
#	include <bit>
#	include <memory>
#	include <mutex>
#	include <utility>
#	include <vector>

namespace RenderGraph::Engine::Buffers::detail
{
	namespace
	{
		// The engine's chunk size: no map is ever larger.
		constexpr std::uint32_t kChunkBytes = 0x400000;
		// The engine's ring has three slots; the users bind ring[cur].
		constexpr std::uint32_t kEngineSlots = 3;

		/** @brief The engine's ring globals (R1). */
		ID3D11Buffer** EngineRing()
		{
			static REL::Relocation<ID3D11Buffer**> ring{ REL::RelocationID(524731, 411350) };
			return ring.get();
		}
		std::uint32_t& EngineCurrent()
		{
			static REL::Relocation<std::uint32_t*> current{ REL::RelocationID(524732, 411351) };
			return *current;
		}
		std::uint32_t& EngineOffset()
		{
			static REL::Relocation<std::uint32_t*> offset{ REL::RelocationID(524733, 411352) };
			return *offset;
		}

		/** @brief One piece of the ring: an upload backing and what the engine's slot holds for it. */
		struct Chunk
		{
			Backing backing;
			winrt::com_ptr<ID3D11Buffer> engine;
			std::uint32_t size = 0;
		};

		/**
		 * @brief The ring. Map runs on the thread that drives the immediate context under Renderer::Lock (the render thread, or the
		 * loading screen's), so the engine's globals are only touched there. A retired chunk comes back from ReleaseAfterD3D11's
		 * service, also on that thread; the free list has its own lock because the service can run inside Map.
		 */
		class Ring
		{
		public:
			static Ring& Get()
			{
				// Never destroyed: chunks may still be bound by DXVK, and retirements may come back during shutdown.
				static Ring* ring = new Ring;
				return *ring;
			}

			bool Active() const { return active.load(std::memory_order_acquire); }

			/** @brief The engine's map: a pointer to a_size bytes and their offset in ring[cur]; null when the engine's own map runs. */
			void* Map(std::uint32_t a_size, std::uint32_t* a_outOffset)
			{
				std::scoped_lock lock(mutex);
				if (abandoned || (!Active() && !Activate()))
					return nullptr;
				if (!current || a_size > current->size - offset) {
					if (!Advance(a_size)) {
						FallBack();
						return nullptr;
					}
				}
				const std::uint32_t at = offset;
				offset += a_size;
				EngineOffset() = offset;
				*a_outOffset = at;
				return current->backing.data + at;
			}

		private:
			Ring() = default;

			/** @brief The first map with the render graph active: the engine's three buffers are put aside, a chunk takes every slot. */
			bool Activate()
			{
				if (!RenderGraphRuntime::Get().IsActive())
					return false;
				Chunk* first = Take(kChunkBytes);
				if (!first) {
					abandoned = true;
					logger::warn("[ORG] engine buffers: the dynamic ring's first chunk could not be made; the engine's ring stays");
					return false;
				}
				auto* ring = EngineRing();
				for (std::uint32_t slot = 0; slot < kEngineSlots; ++slot) {
					originals[slot].attach(std::exchange(ring[slot], nullptr));
					Publish(slot, *first);
				}
				current = first;
				offset = 0;
				active.store(true, std::memory_order_release);
				GetCounters().ringActive.store(true, std::memory_order_relaxed);
				logger::info("[ORG] engine buffers: the dynamic ring runs on ORG upload chunks of {} KiB", kChunkBytes / 1024);
				return true;
			}

			/**
			 * @brief The current chunk is full: it is retired behind the D3D11 work issued so far, and a free chunk (or a new one) takes
			 * the engine's next slot. Nothing waits for the GPU.
			 */
			bool Advance(std::uint32_t a_size)
			{
				if (current)
					Retire(*std::exchange(current, nullptr));
				Chunk* next = Take(a_size);
				if (!next)
					return false;
				auto& slot = EngineCurrent();
				slot = (slot + 1) % kEngineSlots;
				Publish(slot, *next);
				current = next;
				offset = 0;
				return true;
			}

			/** @brief Hands a_chunk back to the free list once the GPU is past every D3D11 command issued until now. */
			void Retire(Chunk& a_chunk)
			{
				std::shared_ptr<const void> token(static_cast<const void*>(&a_chunk), [this](const void* a_retired) {
					std::scoped_lock lock(freeMutex);
					freeChunks.push_back(const_cast<Chunk*>(static_cast<const Chunk*>(a_retired)));
				});
				RenderGraphRuntime::Get().ReleaseAfterD3D11(std::move(token));
			}

			/** @brief A free chunk of at least a_size bytes, or a new one; null when none can be made. */
			Chunk* Take(std::uint32_t a_size)
			{
				{
					std::scoped_lock lock(freeMutex);
					const auto it = std::ranges::find_if(freeChunks, [&](const Chunk* a_chunk) { return a_chunk->size >= a_size; });
					if (it != freeChunks.end()) {
						Chunk* chunk = *it;
						freeChunks.erase(it);
						return chunk;
					}
				}
				const std::uint32_t size = (std::max)(kChunkBytes, a_size);
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = size;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
				auto backing = MakeBacking(desc, "Engine::DynamicRing");
				if (!backing)
					return nullptr;
				auto engine = EngineView(desc, *backing, "Engine::DynamicRing");
				if (!engine)
					return nullptr;
				auto chunk = std::make_unique<Chunk>();
				chunk->backing = std::move(*backing);
				chunk->engine = std::move(engine);
				chunk->size = size;
				chunks.push_back(std::move(chunk));
				const auto made = GetCounters().ringChunks.fetch_add(1, std::memory_order_relaxed) + 1;
				if (made > 8 && std::has_single_bit(made))
					logger::info("[ORG] engine buffers: the dynamic ring has {} chunks ({} MiB)", made, (std::uint64_t(made) * kChunkBytes) >> 20);
				return chunks.back().get();
			}

			/** @brief Puts a_chunk in the engine's slot, with a reference of the slot's own, and drops the slot's old one. */
			void Publish(std::uint32_t a_slot, Chunk& a_chunk)
			{
				auto* ring = EngineRing();
				a_chunk.engine->AddRef();
				if (auto* old = std::exchange(ring[a_slot], a_chunk.engine.get()))
					old->Release();
			}

			/**
			 * @brief No chunk could be made: the engine's own buffers go back in its slots and its map runs from now on. The offset is
			 * set to the end, so the engine's next map fences its current buffer and moves on, as it does on a wrap.
			 */
			void FallBack()
			{
				auto* ring = EngineRing();
				for (std::uint32_t slot = 0; slot < kEngineSlots; ++slot) {
					if (auto* old = std::exchange(ring[slot], originals[slot].detach()))
						old->Release();
				}
				EngineOffset() = kChunkBytes;
				current = nullptr;
				abandoned = true;
				active.store(false, std::memory_order_release);
				GetCounters().ringActive.store(false, std::memory_order_relaxed);
				logger::error("[ORG] engine buffers: a dynamic ring chunk could not be made; the engine's own ring is back");
			}

			std::mutex mutex;  // Map's (the engine's globals and the current chunk)
			std::mutex freeMutex;
			std::vector<std::unique_ptr<Chunk>> chunks;  // every chunk made, kept for the process
			std::vector<Chunk*> freeChunks;
			Chunk* current = nullptr;
			std::uint32_t offset = 0;
			std::atomic<bool> active{ false };
			bool abandoned = false;
			winrt::com_ptr<ID3D11Buffer> originals[kEngineSlots];  // the engine's buffers while the ORG ring runs
		};

		/** @brief MapDynamicBuffer (75484): the ORG ring's map once it is active. */
		struct MapDynamicBuffer
		{
			static void* thunk(void* a_renderer, std::uint32_t a_size, std::uint32_t* a_outOffset)
			{
				if (void* data = Ring::Get().Map(a_size, a_outOffset))
					return data;
				return func(a_renderer, a_size, a_outOffset);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The ring's unmaps (75485, 75487, 75498): nothing to unmap while the ORG ring runs (its chunks stay mapped). */
		template <int Unmapper>
		struct UnmapDynamicBuffer
		{
			static void thunk(void* a_renderer, void* a_second, void* a_third, void* a_fourth)
			{
				if (!Ring::Get().Active())
					func(a_renderer, a_second, a_third, a_fourth);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallRingHooks()
	{
		stl::detour_thunk<MapDynamicBuffer>(REL::RelocationID(75484, 77271));
		stl::detour_thunk<UnmapDynamicBuffer<0>>(REL::RelocationID(75485, 77272));  // UnmapDynamicBuffer
		stl::detour_thunk<UnmapDynamicBuffer<1>>(REL::RelocationID(75487, 77274));  // UnmapDynamicTriShapeDynamicData
		stl::detour_thunk<UnmapDynamicBuffer<2>>(REL::RelocationID(75498, 77288));  // UnmapDynamicLineShapeDynamicData
	}
}

#endif
