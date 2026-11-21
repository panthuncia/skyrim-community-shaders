#pragma once

#include "RenderGraph/Engine/Constants/BufferContents.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

struct ID3D11Buffer;
struct ID3D11Resource;
struct D3D11_BOX;

namespace DCLF
{
	/**
	 * @brief CPU copies of D3D11 buffers the graph needs but must not lock, which the game and CS rewrite with Map(WRITE_DISCARD) or
	 * UpdateSubresource. Once watched, every later write through the immediate context lands in the copy too.
	 * Immediate-context thread only.
	 */
	class ConstantMirror
	{
	public:
		static ConstantMirror& Get();

		/** @brief Starts mirroring a buffer. Its copy is invalid until the next full write. True when this call started it. */
		bool Watch(ID3D11Buffer* a_buffer);

		/**
		 * @brief The buffer's current contents, or empty. A mapped buffer is re-read from its last mapping, which DXVK keeps valid until the
		 * next Map, because the engine writes some constants after Unmap.
		 */
		std::span<const std::byte> Contents(const ID3D11Buffer* a_buffer);

		// Immediate-context hooks.
		void OnMap(ID3D11Resource* a_resource, void* a_data);
		void OnUnmap(ID3D11Resource* a_resource);
		void OnUpdateSubresource(ID3D11Resource* a_resource, const D3D11_BOX* a_box, const void* a_data);

		bool Any() const { return !entries.empty(); }

	private:
		struct Entry
		{
			ID3D11Buffer* buffer = nullptr;  // referenced while watched
			std::uint32_t byteWidth = 0;
			// The last mapping is copied by the first read after it, not at every Unmap, since buffers are unmapped more often than read.
			::RenderGraph::Engine::Constants::BufferContents contents;
		};

		Entry* Find(const ID3D11Resource* a_resource);
		const Entry* Find(const ID3D11Resource* a_resource) const;

		std::vector<Entry> entries;  // a handful of buffers, so a linear search is cheapest
	};
}
