// The engine's dynamic ring (75484 MapDynamicBuffer) as an ORG upload ring. See EngineBuffers.h.

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/BufferRefs.h"
#	include "RenderGraph/Engine/Buffers/BufferBacking.h"
#	include "RenderGraph/Engine/Buffers/EngineBuffers.h"
#	include "RenderGraph/Engine/Native/Native.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <Resources/Buffers/Buffer.h>
#	include <Resources/Buffers/VersionedBuffer.h>

#	include <algorithm>
#	include <bit>
#	include <exception>
#	include <memory>
#	include <mutex>
#	include <shared_mutex>
#	include <unordered_set>
#	include <utility>
#	include <vector>

namespace RenderGraph::Engine::Buffers::detail
{
	namespace
	{
		// The engine's chunk size. No map is ever larger.
		constexpr std::uint32_t kChunkBytes = 0x400000;
		// The engine's ring has three slots, and users bind ring[cur].
		constexpr std::uint32_t kEngineSlots = 3;

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

		/** @brief Chunks are kept for the process, so an entry never goes stale. */
		struct ChunkSet
		{
			std::shared_mutex lock;
			std::unordered_set<const org::Buffer*> buffers;
		};
		ChunkSet& Chunks()
		{
			static ChunkSet* set = new ChunkSet;  // never destroyed, as the ring
			return *set;
		}

		struct Chunk
		{
			Backing backing;
			winrt::com_ptr<ID3D11Buffer> engine;
			std::uint32_t size = 0;
		};

		/**
		* @brief Map runs on the immediate context's thread under Renderer::Lock, so the engine's globals are only touched there.
		* The free list has its own lock because the release service can run inside Map.
		*/
		class Ring
		{
		public:
			static Ring& Get()
			{
				// Never destroyed, since DXVK may still bind chunks and retirements may come back during shutdown.
				static Ring* ring = new Ring;
				return *ring;
			}

			bool Active() const { return active.load(std::memory_order_acquire); }

			/** @brief Null when the engine's own map runs. */
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

			/** @brief On the first map with the graph active, puts the engine's three buffers aside. */
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
				PublishVersion(*first);
				active.store(true, std::memory_order_release);
				GetCounters().ringActive.store(true, std::memory_order_relaxed);
				logger::info("[ORG] engine buffers: the dynamic ring runs on ORG upload chunks of {} KiB", kChunkBytes / 1024);
				return true;
			}

			/** @brief Retires the full chunk behind the D3D11 work issued so far and puts another in the next slot. Nothing waits for the GPU. */
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
				PublishVersion(*next);
				return true;
			}

			/**
			* @brief Graph executions later in the same frame still read a full chunk. In guest mode a stage's block reaches DXVK only at
			* its commit. So the chunk is parked until the frame ends, then waits for the work submitted by then.
			*/
			void Retire(Chunk& a_chunk)
			{
				parked.push_back({ &a_chunk, RenderGraph::RefFrame() });
			}

			void ReleaseAfterWork(Chunk& a_chunk)
			{
				std::shared_ptr<const void> token(static_cast<const void*>(&a_chunk), [this](const void* a_retired) {
					std::scoped_lock lock(freeMutex);
					freeChunks.push_back(const_cast<Chunk*>(static_cast<const Chunk*>(a_retired)));
				});
				RenderGraphRuntime::Get().ReleaseAfterD3D11(std::move(token));
			}

		public:
			/**
			* @brief After Present, releases the parked chunks of ended frames, since every execution of that frame has been submitted.
			* Not done when the next chunk fills, because that is inside a stage's window and in guest mode the release's query would reach DXVK's stream there.
			*/
			void ReleaseEndedFrames()
			{
				std::scoped_lock lock(mutex);
				ReleaseParked();
			}

		private:
			void ReleaseParked()
			{
				if (parked.empty())
					return;
				const std::uint64_t frame = RenderGraph::RefFrame();
				std::erase_if(parked, [&](const Parked& a_parked) {
					if (a_parked.frame >= frame)
						return false;
					ReleaseAfterWork(*a_parked.chunk);
					return true;
				});
			}

			/** @brief Readers declare the ring and the chunk lives as long as the ring, so the version needs no owner. */
			void PublishVersion(Chunk& a_chunk)
			{
				if (const auto& versions = RingVersions())
					versions->PublishLate(a_chunk.backing.buffer);
			}

			/** @brief Null when none can be made. */
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
				{
					auto& set = Chunks();
					std::unique_lock lock(set.lock);
					set.buffers.insert(chunk->backing.buffer.get());
				}
				chunks.push_back(std::move(chunk));
				const auto made = GetCounters().ringChunks.fetch_add(1, std::memory_order_relaxed) + 1;
				if (made > 8 && std::has_single_bit(made))
					logger::info("[ORG] engine buffers: the dynamic ring has {} chunks ({} MiB)", made, (std::uint64_t(made) * kChunkBytes) >> 20);
				return chunks.back().get();
			}

			/** @brief The slot takes its own reference and drops the old one. */
			void Publish(std::uint32_t a_slot, Chunk& a_chunk)
			{
				auto* ring = EngineRing();
				a_chunk.engine->AddRef();
				if (auto* old = std::exchange(ring[a_slot], a_chunk.engine.get()))
					old->Release();
			}

			/**
			* @brief No chunk could be made, so the engine's buffers go back and its map runs from now on.
			* The offset is set to the end so its next map fences and moves on, as on a wrap.
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

			/** @brief Native mode: a full chunk waiting for the end of the frame it filled in. */
			struct Parked
			{
				Chunk* chunk;
				std::uint64_t frame;
			};

			std::mutex mutex;  // guards the engine's globals, the current chunk and the parked ones
			std::mutex freeMutex;
			std::vector<std::unique_ptr<Chunk>> chunks;  // kept for the process
			std::vector<Chunk*> freeChunks;
			std::vector<Parked> parked;
			Chunk* current = nullptr;
			std::uint32_t offset = 0;
			std::atomic<bool> active{ false };
			bool abandoned = false;
			winrt::com_ptr<ID3D11Buffer> originals[kEngineSlots];  // set aside while the ORG ring runs
		};

		/** @brief MapDynamicBuffer (75484). */
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

		/** @brief The ring's unmaps (75485, 75487, 75498). The ORG ring's chunks stay mapped, so there is nothing to do. */
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

	const std::shared_ptr<org::VersionedBuffer>& RingVersions()
	{
		static std::once_flag once;
		// Never destroyed, as chunks may still be read during shutdown.
		static auto* versions = new std::shared_ptr<org::VersionedBuffer>;
		std::call_once(once, [] {
			try {
				// Readers' declarations resolve to this fixed stand-in, so a new chunk costs no graph edit.
				auto standIn = org::Buffer::CreateUnmaterializedStructuredBuffer(1, 4, false);
				standIn->SetName("Engine::DynamicRing.stand-in");
				standIn->Materialize();
				*versions = org::VersionedBuffer::CreateLateBound(std::move(standIn));
			} catch (const std::exception& e) {
				logger::warn("[ORG] engine buffers: the dynamic ring's late-bound buffer could not be made ({}); its readers declare nothing", e.what());
			}
		});
		return *versions;
	}

	void ReleaseEndedFrames()
	{
		Ring::Get().ReleaseEndedFrames();
	}

	bool IsRingChunk(const org::Buffer* a_buffer)
	{
		auto& set = Chunks();
		std::shared_lock lock(set.lock);
		return set.buffers.contains(a_buffer);
	}

	void InstallRingHooks()
	{
		stl::detour_thunk<MapDynamicBuffer>(REL::RelocationID(75484, 77271));
		stl::detour_thunk<UnmapDynamicBuffer<0>>(REL::RelocationID(75485, 77272));
		stl::detour_thunk<UnmapDynamicBuffer<1>>(REL::RelocationID(75487, 77274));
		stl::detour_thunk<UnmapDynamicBuffer<2>>(REL::RelocationID(75498, 77288));
	}
}

#endif
