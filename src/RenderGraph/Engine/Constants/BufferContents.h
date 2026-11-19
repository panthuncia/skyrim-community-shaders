#pragma once

#include <d3d11.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace RenderGraph::Engine::Constants
{
	/**
	* @brief What the last writer left in one D3D11 buffer, as the Map, Unmap and UpdateSubresource hooks see it.
	*
	* Mapped buffers are read from their last mapping with no copy. DXVK keeps it valid until the next Map, and the engine writes
	* some constants after Unmap, such as BSLightingShader's VPOSOffset. UpdateSubresource's data is only valid during the call,
	* so it is copied. Immediate context thread only.
	*/
	class BufferContents
	{
	public:
		void OnMap(void* a_data)
		{
			mapped = a_data;
			++version;
		}

		/** @brief WRITE_DISCARD and NO_OVERWRITE alike. */
		void OnUnmap()
		{
			if (!mapped)
				return;
			unread = static_cast<const std::byte*>(std::exchange(mapped, nullptr));
			valid = true;
			// Bump again, since a read between Map and Unmap saw the old contents under the Map's version.
			++version;
		}

		/** @brief A box update only counts once the buffer was fully written before. */
		void OnUpdate(const D3D11_BOX* a_box, const void* a_data, std::uint32_t a_byteWidth)
		{
			if (!a_data)
				return;
			if (copy.size() < a_byteWidth)
				copy.resize(a_byteWidth);
			Settle();
			const std::size_t begin = a_box ? a_box->left : 0;
			const std::size_t end = a_box ? (std::min<std::size_t>)(a_box->right, a_byteWidth) : a_byteWidth;
			if (begin < end)
				std::memcpy(copy.data() + begin, a_data, end - begin);
			valid |= !a_box;
			++version;
		}

		/** @brief Null when never fully written. Valid until the next Map or update. */
		const std::byte* Data() const
		{
			if (!valid)
				return nullptr;
			return unread ? unread : copy.data();
		}

		/** @brief A copy that outlives the next Map. a_byteWidth sizes it the first time. */
		std::span<const std::byte> Settled(std::uint32_t a_byteWidth)
		{
			if (!valid)
				return {};
			if (copy.size() < a_byteWidth)
				copy.resize(a_byteWidth);
			Settle();
			return { copy.data(), a_byteWidth };
		}

		/** @brief Two reads with the same version see the same bytes. */
		std::uint32_t Version() const { return version; }

		bool Valid() const { return valid; }

	private:
		/** @brief Once, so a partial update or a settled read applies over the mapping. */
		void Settle()
		{
			if (unread && !copy.empty())
				std::memcpy(copy.data(), std::exchange(unread, nullptr), copy.size());
		}

		void* mapped = nullptr;
		const std::byte* unread = nullptr;   // the last finished mapping, not copied yet
		std::vector<std::byte> copy;         // UpdateSubresource's data, or a settled mapping
		std::uint32_t version = 0;
		bool valid = false;
	};
}
