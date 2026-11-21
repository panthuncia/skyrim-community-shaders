// The engine's dynamic ring (75484 MapDynamicBuffer) as an ORG upload ring. See EngineBuffers.h.

#include "RenderGraph/BufferRefs.h"
#include "RenderGraph/Engine/Buffers/BufferBacking.h"
#include "RenderGraph/Engine/Buffers/EngineBuffers.h"
#include "RenderGraph/Engine/Buffers/UploadRing.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <Resources/Buffers/Buffer.h>
#include <Resources/Buffers/VersionedBuffer.h>

#include <algorithm>
#include <bit>
#include <exception>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <utility>
#include <vector>

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

		/** @brief An upload ring chunk the engine binds: the ORG buffer, its DXVK buffer and the engine's view of it. */
		struct DynamicChunk final : UploadRing::Chunk
		{
			Backing backing;
			winrt::com_ptr<ID3D11Buffer> engine;
		};

		/** @brief Null when the graph is inactive or the chunk cannot be made. */
		std::unique_ptr<UploadRing::Chunk> MakeDynamicChunk(std::uint32_t a_bytes)
		{
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = a_bytes;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
			auto backing = MakeBacking(desc, "Engine::DynamicRing");
			if (!backing)
				return nullptr;
			auto engine = EngineView(desc, *backing, "Engine::DynamicRing");
			if (!engine)
				return nullptr;
			auto chunk = std::make_unique<DynamicChunk>();
			chunk->backing = std::move(*backing);
			chunk->engine = std::move(engine);
			chunk->data = reinterpret_cast<std::byte*>(chunk->backing.data);
			chunk->size = a_bytes;
			{
				auto& set = Chunks();
				std::unique_lock lock(set.lock);
				set.buffers.insert(chunk->backing.buffer.get());
			}
			const auto made = GetCounters().ringChunks.fetch_add(1, std::memory_order_relaxed) + 1;
			if (made > 8 && std::has_single_bit(made))
				logger::info("[ORG] engine buffers: the dynamic ring has {} chunks ({} MiB)", made, (std::uint64_t(made) * kChunkBytes) >> 20);
			return chunk;
		}

		/**
		* @brief Map runs on the immediate context's thread under Renderer::Lock, so the engine's globals are only touched there.
		* The memory comes from an upload ring, which retires a full chunk behind the D3D11 work issued so far. Nothing waits for the GPU.
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
				if (!Active() && !RenderGraphRuntime::Get().IsActive())
					return nullptr;
				const auto at = upload.Allocate(a_size);
				if (!at)
					RenderGraph::Fatal("engine buffers", Active() ? "a dynamic ring chunk could not be made" : "the dynamic ring's first chunk could not be made");
				auto& chunk = static_cast<DynamicChunk&>(*at->chunk);
				if (!Active())
					Activate(chunk);
				else if (at->fresh)
					Advance(chunk);
				EngineOffset() = at->offset + a_size;
				*a_outOffset = at->offset;
				return at->data;
			}

			/**
			* @brief After Present, releases the full chunks of ended frames, since every execution of that frame has been submitted.
			* Not done when the next chunk fills, because that is inside a stage's window and in guest mode the release's query would reach DXVK's stream there.
			*/
			void ReleaseEndedFrames()
			{
				std::scoped_lock lock(mutex);
				upload.ReleaseParked(RenderGraph::RefFrame());
			}

		private:
			Ring() :
				upload({ .name = "engine buffers", .chunkBytes = kChunkBytes, .alignment = 1, .make = &MakeDynamicChunk })
			{}

			/** @brief On the first map with the graph active, puts the engine's three buffers aside. */
			void Activate(DynamicChunk& a_first)
			{
				auto* ring = EngineRing();
				for (std::uint32_t slot = 0; slot < kEngineSlots; ++slot) {
					originals[slot].attach(std::exchange(ring[slot], nullptr));
					Publish(slot, a_first);
				}
				PublishVersion(a_first);
				active.store(true, std::memory_order_release);
				GetCounters().ringActive.store(true, std::memory_order_relaxed);
				logger::info("[ORG] engine buffers: the dynamic ring runs on ORG upload chunks of {} KiB", kChunkBytes / 1024);
			}

			/** @brief The upload ring retired the full chunk. The new one goes in the engine's next slot. */
			void Advance(DynamicChunk& a_next)
			{
				auto& slot = EngineCurrent();
				slot = (slot + 1) % kEngineSlots;
				Publish(slot, a_next);
				PublishVersion(a_next);
			}

			/** @brief Readers declare the ring and the chunk lives as long as the ring, so the version needs no owner. */
			void PublishVersion(DynamicChunk& a_chunk)
			{
				if (const auto& versions = RingVersions())
					versions->PublishLate(a_chunk.backing.buffer);
			}

			/** @brief The slot takes its own reference and drops the old one. */
			void Publish(std::uint32_t a_slot, DynamicChunk& a_chunk)
			{
				auto* ring = EngineRing();
				a_chunk.engine->AddRef();
				if (auto* old = std::exchange(ring[a_slot], a_chunk.engine.get()))
					old->Release();
			}

			std::mutex mutex;  // guards the engine's globals
			UploadRing upload;
			std::atomic<bool> active{ false };
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
				RenderGraph::Fatal("engine buffers", fmt::format("the dynamic ring's late-bound buffer could not be made ({})", e.what()));
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

