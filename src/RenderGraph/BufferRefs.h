#pragma once

// Making buffer view refs (BufferRef.h) from BasicRHI buffers: Device::GetBufferViewRef gives a buffer its raw view pair the first
// time and frees it with the buffer.

#include "RenderGraph/BufferRef.h"
#include "RenderGraph/UndeclaredAccess.h"

#include <rhi.h>

#include <atomic>
#include <memory>
#include <source_location>
#include <string_view>
#include <vector>

namespace org
{
	class Buffer;
	class VersionedBuffer;
	class RenderPassBuilder;
	struct FramePreparationContext;
	struct ResourceBindingToken;
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

	/** @brief BufferRefOf's work, without the undeclared-access note. Any thread. */
	inline BufferRef MakeBufferRef(rhi::Device a_device, rhi::ResourceHandle a_buffer, std::uint64_t a_offset, std::source_location a_where)
	{
		const BufferRef ref = a_device ? ToBufferRef(a_device.GetBufferViewRef({ a_buffer, a_offset })) : BufferRef{};
		if (ref && BufferRefChecksOn())
			CheckBufferRef(a_device, ref, a_buffer, 0, 0, "BufferRefOf", a_where);
		return ref;
	}

	/**
	 * @brief a_buffer's ref at a_offset on a_device; null when it has none. A raw ref by handle: the undeclared-access validator
	 * (CS_ORG_VALIDATE_UNDECLARED=1) logs it with a_where. Any thread.
	 */
	inline BufferRef BufferRefOf(rhi::Device a_device, rhi::ResourceHandle a_buffer, std::uint64_t a_offset = 0,
		std::source_location a_where = std::source_location::current())
	{
		if (UndeclaredAccess::On())
			UndeclaredAccess::NoteRhiBuffer(a_buffer.index, a_buffer.generation, a_where);
		return MakeBufferRef(a_device, a_buffer, a_offset, a_where);
	}

	/** @brief a_buffer's ref at a_offset on the graph host's device; null without a host. Any thread. */
	BufferRef BufferRefOf(rhi::ResourceHandle a_buffer, std::uint64_t a_offset = 0, std::source_location a_where = std::source_location::current());
	BufferRef BufferRefOf(const org::Buffer& a_buffer, std::uint64_t a_offset = 0, std::source_location a_where = std::source_location::current());

	/** @brief CheckBufferRef on the graph host's device; nothing without a host or with the checks off. */
	void CheckBufferRef(BufferRef a_ref, std::uint64_t a_address, std::uint64_t a_bytes, std::string_view a_what,
		std::source_location a_where = std::source_location::current());

	/**
	 * @brief The ref a site last wrote into a latch or a table for a buffer, at commit (render thread). A declaring pass holds the same
	 * latch and compares it with its declaration in Prepare (CheckDeclaredRefs). Set it to the buffer's ref at byte 0: the check compares
	 * view slots, since sites add their own offsets. Any thread.
	 */
	class RefLatch
	{
	public:
		void Set(BufferRef a_ref) noexcept { bits.store(a_ref.Bits(), std::memory_order_relaxed); }
		BufferRef Get() const noexcept
		{
			const auto value = bits.load(std::memory_order_relaxed);
			return { static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32) };
		}

	private:
		std::atomic<std::uint64_t> bits{ 0 };
	};

	/**
	 * @brief The buffers a pass reads or writes through buffer view refs that a site latched at commit, before the graph prepares. Each
	 * entry keeps the declaration's binding and the site's RefLatch; CheckDeclaredRefs compares the two in the pass's Prepare. This is the
	 * interim form, like DeclaredReads for texture indices: Phase 4 of the native frame graph writes these refs in Prepare from the
	 * declaration (DeclaredBufferRef) and drops the latch. Cheap to copy.
	 */
	struct DeclaredRefs
	{
		struct Entry;
		std::shared_ptr<std::vector<Entry>> entries;
	};

	/** @brief How a pass uses a buffer through its ref: Read declares a shader resource, Write an unordered access (no views either way). */
	enum class RefAccess
	{
		Read,
		Write
	};

	/**
	 * @brief Graph build: declares a_buffer for a_access without views (a buffer view ref is BasicRHI's own raw pair, not an ORG view),
	 * and adds it to a_out with a_latch when one is given. a_what names the buffer in the mismatch log.
	 * @return The declaration's binding, for DeclaredBufferRef.
	 */
	org::ResourceBindingToken DeclareRef(org::RenderPassBuilder& a_builder, const std::shared_ptr<org::Buffer>& a_buffer, RefAccess a_access, std::string_view a_what,
		std::shared_ptr<const RefLatch> a_latch, DeclaredRefs& a_out);
	/** @brief DeclareRef for a versioned buffer: the check uses the version bound to the preparation. Not for a late-bound one. */
	org::ResourceBindingToken DeclareRef(org::RenderPassBuilder& a_builder, org::VersionedBuffer& a_buffer, RefAccess a_access, std::string_view a_what,
		std::shared_ptr<const RefLatch> a_latch, DeclaredRefs& a_out);
	/**
	 * @brief DeclareRef for a late-bound buffer (org::VersionedBuffer::CreateLateBound). Its declaration always resolves to its stand-in,
	 * and its readers read the version the host published (PublishLate), so the check uses that current version at Prepare.
	 */
	org::ResourceBindingToken DeclareLateRef(org::RenderPassBuilder& a_builder, const std::shared_ptr<org::VersionedBuffer>& a_buffer, RefAccess a_access,
		std::string_view a_what, std::shared_ptr<const RefLatch> a_latch, DeclaredRefs& a_out);

	/**
	 * @brief A pass's Prepare (the host's thread): the ref of the declared buffer's backing bound to this preparation (a versioned
	 * buffer's version for this frame), at a_offset; null when it cannot be resolved. Not a raw ref: the pass declared the buffer. A
	 * late-bound buffer's declaration resolves to its stand-in: its readers take the published version's ref instead.
	 */
	BufferRef DeclaredBufferRef(const org::FramePreparationContext& a_preparation, const org::ResourceBindingToken& a_binding, std::uint64_t a_offset = 0);

	/**
	 * @brief A pass's Prepare (the host's thread): logs "[ORG] declared view mismatch" once per entry whose latched ref (when the site set
	 * one) is not a ref of the declared buffer's backing bound to this preparation (a late-bound buffer's current version). a_site names
	 * the pass.
	 */
	void CheckDeclaredRefs(const DeclaredRefs& a_refs, const org::FramePreparationContext& a_preparation, std::string_view a_site);
}
