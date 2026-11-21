// One upload ring for the engine's dynamic buffers and constants. See UploadRing.h.

#include "RenderGraph/Engine/Buffers/UploadRing.h"

#include "RenderGraph/BufferRefs.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace RenderGraph::Engine::Buffers
{
	UploadRing::UploadRing(Desc a_desc) :
		desc(std::move(a_desc))
	{
		desc.alignment = std::bit_ceil((std::max)(desc.alignment, 1u));
	}

	std::optional<UploadRing::Allocation> UploadRing::Allocate(std::uint32_t a_bytes)
	{
		std::scoped_lock lock(mutex);
		std::uint32_t at = (offset + desc.alignment - 1) & ~(desc.alignment - 1);
		if (!current || at > current->size || a_bytes > current->size - at) {
			if (current)
				RetireLocked();
			current = Take(a_bytes);
			if (!current)
				return std::nullopt;
			offset = 0;
			at = 0;
			fresh = true;
		}
		offset = at + a_bytes;
		return Allocation{ current, at, current->data + at, std::exchange(fresh, false) };
	}

	void UploadRing::Retire()
	{
		std::scoped_lock lock(mutex);
		if (current)
			RetireLocked();
	}

	void UploadRing::RetireLocked()
	{
		// Work later in the same frame may still read a full chunk. In guest mode a stage's block reaches DXVK only at its commit.
		// So the chunk waits for the end of its frame before it is released.
		parked.push_back({ std::exchange(current, nullptr), RenderGraph::RefFrame() });
		offset = 0;
	}

	void UploadRing::ReleaseParked(std::uint64_t a_before)
	{
		std::scoped_lock lock(mutex);
		if (parked.empty())
			return;
		auto& runtime = RenderGraphRuntime::Get();
		std::erase_if(parked, [&](const Parked& a_parked) {
			if (a_parked.frame >= a_before)
				return false;
			std::shared_ptr<const void> token(static_cast<const void*>(a_parked.chunk), [this](const void* a_retired) {
				std::scoped_lock free(freeMutex);
				freeChunks.push_back(const_cast<Chunk*>(static_cast<const Chunk*>(a_retired)));
			});
			runtime.ReleaseAfterD3D11(std::move(token));
			return true;
		});
	}

	std::uint32_t UploadRing::Made() const
	{
		std::scoped_lock lock(mutex);
		return static_cast<std::uint32_t>(chunks.size());
	}

	UploadRing::Chunk* UploadRing::Take(std::uint32_t a_bytes)
	{
		{
			std::scoped_lock free(freeMutex);
			const auto it = std::ranges::find_if(freeChunks, [&](const Chunk* a_chunk) { return a_chunk->size >= a_bytes; });
			if (it != freeChunks.end()) {
				Chunk* chunk = *it;
				freeChunks.erase(it);
				return chunk;
			}
		}
		auto made = desc.make ? desc.make((std::max)(desc.chunkBytes, a_bytes)) : nullptr;
		if (!made || !made->data || made->size < a_bytes)
			return nullptr;
		chunks.push_back(std::move(made));
		const auto count = static_cast<std::uint32_t>(chunks.size());
		if (count > 4 && std::has_single_bit(count))
			logger::info("[ORG] {}: the upload ring has {} chunks", desc.name, count);
		return chunks.back().get();
	}
}

