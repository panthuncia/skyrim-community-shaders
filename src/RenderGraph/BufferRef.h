#pragma once

#include <cstdint>

namespace RenderGraph
{
	/**
	 * @brief A buffer view ref as a shader reads it (Common/DeviceRef.hlsli): the heap slot of the buffer's raw SRV (its raw UAV is the
	 * slot after it), then a byte offset into the buffer. The same two words as BasicRHI's rhi::BufferViewRef (BufferRefs.h makes them),
	 * so it takes a 64-bit device address's place in a GPU record. View slot 0 names no buffer: a default ref is null.
	 */
	struct BufferRef
	{
		std::uint32_t viewSlot = 0;
		std::uint32_t byteOffset = 0;

		explicit operator bool() const noexcept { return viewSlot != 0; }
		bool operator==(const BufferRef&) const noexcept = default;
		/** @brief The ref a_bytes further on; a null ref stays null. A buffer is under 4 GiB, so the offset stays 32-bit. */
		BufferRef operator+(std::uint64_t a_bytes) const noexcept
		{
			return viewSlot ? BufferRef{ viewSlot, static_cast<std::uint32_t>(byteOffset + a_bytes) } : *this;
		}
		BufferRef& operator+=(std::uint64_t a_bytes) noexcept { return *this = *this + a_bytes; }
		/** @brief The two words as one, the view slot low: what a record's 64-bit field holds. */
		std::uint64_t Bits() const noexcept { return std::uint64_t(viewSlot) | (std::uint64_t(byteOffset) << 32); }
	};
	static_assert(sizeof(BufferRef) == 8);
}
