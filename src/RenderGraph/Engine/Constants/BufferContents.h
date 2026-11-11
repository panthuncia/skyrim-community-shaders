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
	 * @brief What the last writer of one D3D11 buffer left in it, as the immediate context's Map, Unmap and UpdateSubresource hooks
	 * see it. Shared by DCLF's ConstantMirror and the per-use capture (EngineConstants.h).
	 *
	 * A mapped buffer is read from its last mapping, with no copy. DXVK keeps that memory valid until the buffer's next Map, and the
	 * engine writes some constants after Unmap (BSLightingShader's SetupTechnique writes VPOSOffset), so the mapping holds what the
	 * GPU will read. UpdateSubresource's data is only valid during the call, so it is copied.
	 *
	 * The immediate context's thread only, like the hooks that feed it.
	 */
	class BufferContents
	{
	public:
		/** @brief A Map: the writer starts a new version. */
		void OnMap(void* a_data)
		{
			mapped = a_data;
			++version;
		}

		/** @brief An Unmap: whatever the writer left in the mapping is the buffer's contents (WRITE_DISCARD or NO_OVERWRITE alike). */
		void OnUnmap()
		{
			if (!mapped)
				return;
			unread = static_cast<const std::byte*>(std::exchange(mapped, nullptr));
			valid = true;
		}

		/**
		 * @brief An UpdateSubresource of a_byteWidth bytes in all. A box updates part of the contents, over what was there; it only
		 * counts once the buffer was fully written before. The copy is sized once per buffer.
		 */
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

		/**
		 * @brief The contents without a copy: the last mapping while it is newer than the copy, else the copy. Null when the buffer
		 * was never fully written. Valid until the buffer's next Map or update.
		 */
		const std::byte* Data() const
		{
			if (!valid)
				return nullptr;
			return unread ? unread : copy.data();
		}

		/**
		 * @brief The contents as a copy that outlives the next Map: the last mapping is copied into it first (once per mapping). Empty
		 * when never fully written. a_byteWidth sizes the copy the first time.
		 */
		std::span<const std::byte> Settled(std::uint32_t a_byteWidth)
		{
			if (!valid)
				return {};
			if (copy.size() < a_byteWidth)
				copy.resize(a_byteWidth);
			Settle();
			return { copy.data(), a_byteWidth };
		}

		/** @brief Bumped by every Map and update: two reads with the same version see the same bytes. */
		std::uint32_t Version() const { return version; }

		/** @brief Whether the buffer has been fully written since it was first seen. */
		bool Valid() const { return valid; }

	private:
		/** @brief Copies the last mapping into the copy, once, so a partial update or a settled read applies over it. */
		void Settle()
		{
			if (unread && !copy.empty())
				std::memcpy(copy.data(), std::exchange(unread, nullptr), copy.size());
		}

		void* mapped = nullptr;              // between Map and Unmap
		const std::byte* unread = nullptr;   // the last finished mapping, not copied yet
		std::vector<std::byte> copy;         // UpdateSubresource's data, or a settled mapping
		std::uint32_t version = 0;
		bool valid = false;
	};
}
