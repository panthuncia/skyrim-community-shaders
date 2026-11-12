#pragma once

// Making buffer view refs (BufferRef.h) from BasicRHI buffers: Device::GetBufferViewRef gives a buffer its raw view pair the first
// time and frees it with the buffer.

#include "RenderGraph/BufferRef.h"

#include <rhi.h>

#include <atomic>
#include <source_location>
#include <string_view>

namespace org
{
	class Buffer;
}

namespace RenderGraph
{
	inline BufferRef ToBufferRef(rhi::BufferViewRef a_ref) { return { a_ref.viewSlot, a_ref.byteOffset }; }

	// Ref checks (CS_DCLF_REF_CHECK): every ref made, and the refs given to CheckBufferRef, are checked against BasicRHI's view ref
	// region. Off by default.
	inline std::atomic<bool> g_bufferRefChecks{ false };
	inline bool BufferRefChecksOn() { return g_bufferRefChecks.load(std::memory_order_relaxed); }
	/** @brief Turns the ref checks on or off. Set once, at install. */
	void SetBufferRefChecks(bool a_on);

	/**
	 * @brief Checks a ref on Vulkan (CS_DCLF_REF_CHECK): its slot is a pair of the region, the pair is live, it is a_buffer's when given
	 * (and a_buffer is alive), the pair's descriptors start at a_address minus the ref's offset when a_address is given, a_bytes from
	 * the offset are inside the view, and every heap holds the pair's bytes. The first mismatches are logged with a_what and the caller.
	 * A null ref is not checked. Any thread; takes BasicRHI's views mutex.
	 */
	void CheckBufferRef(rhi::Device a_device, BufferRef a_ref, rhi::ResourceHandle a_buffer, std::uint64_t a_address, std::uint64_t a_bytes, std::string_view a_what,
		std::source_location a_where = std::source_location::current());

	/** @brief a_buffer's ref at a_offset on a_device; null when it has none. Any thread. */
	inline BufferRef BufferRefOf(rhi::Device a_device, rhi::ResourceHandle a_buffer, std::uint64_t a_offset = 0,
		std::source_location a_where = std::source_location::current())
	{
		const BufferRef ref = a_device ? ToBufferRef(a_device.GetBufferViewRef({ a_buffer, a_offset })) : BufferRef{};
		if (ref && BufferRefChecksOn())
			CheckBufferRef(a_device, ref, a_buffer, 0, 0, "BufferRefOf", a_where);
		return ref;
	}

	/** @brief a_buffer's ref at a_offset on the graph host's device; null without a host. Any thread. */
	BufferRef BufferRefOf(rhi::ResourceHandle a_buffer, std::uint64_t a_offset = 0, std::source_location a_where = std::source_location::current());
	BufferRef BufferRefOf(const org::Buffer& a_buffer, std::uint64_t a_offset = 0, std::source_location a_where = std::source_location::current());

	/** @brief CheckBufferRef on the graph host's device; nothing without a host or with the checks off. */
	void CheckBufferRef(BufferRef a_ref, std::uint64_t a_address, std::uint64_t a_bytes, std::string_view a_what,
		std::source_location a_where = std::source_location::current());
}
