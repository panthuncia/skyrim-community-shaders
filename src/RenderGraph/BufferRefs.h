#pragma once

// Making buffer view refs (BufferRef.h) from BasicRHI buffers: Device::GetBufferViewRef gives a buffer its raw view pair the first
// time and frees it with the buffer.

#include "RenderGraph/BufferRef.h"

#include <rhi.h>

namespace org
{
	class Buffer;
}

namespace RenderGraph
{
	inline BufferRef ToBufferRef(rhi::BufferViewRef a_ref) { return { a_ref.viewSlot, a_ref.byteOffset }; }

	/** @brief a_buffer's ref at a_offset on a_device; null when it has none. Any thread. */
	inline BufferRef BufferRefOf(rhi::Device a_device, rhi::ResourceHandle a_buffer, std::uint64_t a_offset = 0)
	{
		return a_device ? ToBufferRef(a_device.GetBufferViewRef({ a_buffer, a_offset })) : BufferRef{};
	}

	/** @brief a_buffer's ref at a_offset on the graph host's device; null without a host. Any thread. */
	BufferRef BufferRefOf(rhi::ResourceHandle a_buffer, std::uint64_t a_offset = 0);
	BufferRef BufferRefOf(const org::Buffer& a_buffer, std::uint64_t a_offset = 0);
}
