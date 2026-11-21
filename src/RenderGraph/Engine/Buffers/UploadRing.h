#pragma once

// Only built with the render graph.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace RenderGraph::Engine::Buffers
{
	/**
	* @brief Host-visible upload memory handed out front to back, one current chunk at a time.
	*
	* The engine's dynamic ring (vertices and indices) and the engine constants' capture both write through one. A chunk is
	* retired when the next request does not fit, or by Retire. A retired chunk is parked with the frame it was filled in.
	* ReleaseParked hands the parked chunks of ended frames to ReleaseAfterD3D11, so a chunk comes back for reuse once the work
	* issued by then is done. Nothing waits for the GPU. Chunks are kept for the process.
	* Allocate and Retire may run on any thread; the ring locks. ReleaseParked runs after Present. A ring must never be destroyed,
	* since released chunks come back to it from ReleaseAfterD3D11's service, also during shutdown.
	*/
	class UploadRing
	{
	public:
		/** @brief One chunk. A client derives from it to keep its own views of the memory. */
		struct Chunk
		{
			virtual ~Chunk() = default;

			std::byte* data = nullptr;  // mapped for the chunk's life
			std::uint32_t size = 0;
		};

		/** @brief Makes a chunk of at least a_bytes. Null when it cannot. Called with the ring's lock, so it must not use the ring. */
		using MakeChunk = std::function<std::unique_ptr<Chunk>(std::uint32_t a_bytes)>;

		struct Desc
		{
			std::string name;
			std::uint32_t chunkBytes = 1u << 20;  // the smallest chunk made
			std::uint32_t alignment = 1;          // a power of two
			MakeChunk make;
		};

		struct Allocation
		{
			Chunk* chunk = nullptr;
			std::uint32_t offset = 0;
			std::byte* data = nullptr;  // chunk->data + offset
			bool fresh = false;         // the first allocation from this chunk since it was taken
		};

		explicit UploadRing(Desc a_desc);
		UploadRing(const UploadRing&) = delete;
		UploadRing& operator=(const UploadRing&) = delete;

		/** @brief a_bytes from the current chunk, or from the next one when it does not fit. Nullopt when no chunk can be made. */
		std::optional<Allocation> Allocate(std::uint32_t a_bytes);

		/** @brief Parks the current chunk, so the next Allocate takes another. */
		void Retire();

		/** @brief Releases the parked chunks filled before frame a_before (RefFrame) behind the D3D11 work issued so far. */
		void ReleaseParked(std::uint64_t a_before);

		/** @brief Chunks made so far. */
		std::uint32_t Made() const;

	private:
		struct Parked
		{
			Chunk* chunk = nullptr;
			std::uint64_t frame = 0;
		};

		/** @brief A free chunk of at least a_bytes, or a new one. Null when none can be made. Call with the ring's lock. */
		Chunk* Take(std::uint32_t a_bytes);

		void RetireLocked();

		Desc desc;
		mutable std::mutex mutex;  // the current chunk, the parked ones and the made ones
		std::mutex freeMutex;      // the free list, also taken by ReleaseAfterD3D11's service
		std::vector<std::unique_ptr<Chunk>> chunks;
		std::vector<Chunk*> freeChunks;
		std::vector<Parked> parked;
		Chunk* current = nullptr;
		std::uint32_t offset = 0;
		bool fresh = false;
	};
}
