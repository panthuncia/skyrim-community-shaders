#pragma once

// Making buffer view refs (BufferRef.h) from BasicRHI buffers: Device::GetBufferViewRef gives a buffer its raw view pair the first
// time and frees it with the buffer.

#include "RenderGraph/BufferRef.h"
#include "RenderGraph/UndeclaredAccess.h"

#include <rhi.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
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

	/**
	 * @brief a_buffer's ref at a_offset on the graph host's device, for a buffer every pass reading the ref declares (no views): the
	 * undeclared-access validator does not note it. The caller names those passes beside the call. Null without a host. Any thread.
	 */
	BufferRef DeclaredRefOf(const org::Buffer& a_buffer, std::uint64_t a_offset = 0, std::source_location a_where = std::source_location::current());
	/**
	 * @brief DeclaredRefOf on a_device, for a buffer made before the graph host is reachable (a scene table's setup, which has the
	 * device). The same rule: every pass reading the ref declares the buffer, and the caller names those passes. Any thread.
	 */
	BufferRef DeclaredRefOf(rhi::Device a_device, const org::Buffer& a_buffer, std::uint64_t a_offset = 0,
		std::source_location a_where = std::source_location::current());

	/** @brief CheckBufferRef on the graph host's device; nothing without a host or with the checks off. */
	void CheckBufferRef(BufferRef a_ref, std::uint64_t a_address, std::uint64_t a_bytes, std::string_view a_what,
		std::source_location a_where = std::source_location::current());

	/**
	 * @brief The render thread's frame for the latch checks: advanced once a frame, at its end (RenderGraphRuntime::SealPresented). A
	 * latch remembers the frame it was set in, so a check made while another frame's values are being written can tell. Any thread.
	 */
	inline std::atomic<std::uint64_t> g_refFrame{ 1 };
	inline std::uint64_t RefFrame() { return g_refFrame.load(std::memory_order_acquire); }
	/** @brief The frame's end (render thread). */
	inline void AdvanceRefFrame() { g_refFrame.fetch_add(1, std::memory_order_acq_rel); }

	/**
	 * @brief The ref a site last wrote into a latch or a table for a buffer, at commit (render thread), with the frame it was written in
	 * (RefFrame) and the last few before it. A declaring pass holds the same latch and compares it with its declaration in Prepare
	 * (CheckDeclaredRefs). Set it to the buffer's ref at byte 0: the check compares view slots, since sites add their own offsets. Any
	 * thread.
	 */
	class RefLatch
	{
	public:
		/** @brief The refs kept: the latest and the ones before it, for the frames in flight a preparation may be for. */
		static constexpr std::size_t kHistory = 4;

		/** @brief What the latch holds at one moment: the latest ref and its frame, and the refs before it (newest first). */
		struct Snapshot
		{
			BufferRef ref;
			std::uint64_t frame = 0;
			std::array<BufferRef, kHistory> history{};
		};

		void Set(BufferRef a_ref) noexcept
		{
			const auto frame = RefFrame();
			std::lock_guard guard(lock);
			if (latest.ref.Bits() != a_ref.Bits()) {
				for (std::size_t i = kHistory - 1; i > 0; --i)
					latest.history[i] = latest.history[i - 1];
				latest.history[0] = latest.ref;
			}
			latest.ref = a_ref;
			latest.frame = frame;
		}
		BufferRef Get() const noexcept
		{
			std::lock_guard guard(lock);
			return latest.ref;
		}
		Snapshot Take() const noexcept
		{
			std::lock_guard guard(lock);
			return latest;
		}

	private:
		mutable std::mutex lock;
		Snapshot latest;
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
	 * @brief A buffer outside the graph that a host replaces per frame or per snapshot (a pooled version built on the upload queue), made
	 * declarable: a late-bound org::VersionedBuffer whose version the host publishes when the frame adopts it, and the ref it latched
	 * then. Readers declare it (Declare) and check the ref in Prepare (CheckDeclaredRefs). Publish costs no graph build.
	 */
	class LateRefBuffer
	{
	public:
		/** @brief Any thread: the late-bound buffer, its stand-in named a_name. */
		explicit LateRefBuffer(std::string_view a_name);
		/**
		 * @brief The frame's adoption (render thread): a_backing becomes the current version, kept alive with a_owner. The latch takes
		 * a_used, the ref the readers' tables carry (made earlier, from the backing they were built for), or a_backing's own ref when
		 * a_used is null. The current backing again keeps its version. A null backing publishes nothing and clears the latch.
		 * @return a_backing's ref at byte 0 (null without a backing or a graph host). Not a raw ref: readers declare the buffer.
		 */
		BufferRef Publish(const std::shared_ptr<org::Buffer>& a_backing, std::shared_ptr<const void> a_owner, BufferRef a_used = {});
		/** @brief Graph build: declares the buffer for a_access (no views) with the latch, into a_out. */
		void Declare(org::RenderPassBuilder& a_builder, RefAccess a_access, DeclaredRefs& a_out) const;

	private:
		std::string name;
		std::shared_ptr<org::VersionedBuffer> buffer;
		std::shared_ptr<RefLatch> latch = std::make_shared<RefLatch>();
	};

	/**
	 * @brief A pass's Prepare (the host's thread): the ref of the declared buffer's backing bound to this preparation (a versioned
	 * buffer's version for this frame), at a_offset; null when it cannot be resolved. Not a raw ref: the pass declared the buffer. A
	 * late-bound buffer's declaration resolves to its stand-in: its readers take the published version's ref instead.
	 */
	BufferRef DeclaredBufferRef(const org::FramePreparationContext& a_preparation, const org::ResourceBindingToken& a_binding, std::uint64_t a_offset = 0);

	/**
	 * @brief A pass's Prepare (the host's thread): checks each entry's latched ref (when the site set one) against the declared buffer's
	 * backing, keyed to the frame, so a preparation made while another frame's values are written is not taken for a mismatch:
	 * - a late-bound buffer: its current version, only while the latch and that version are the same frame's (RefFrame unchanged
	 *   across the check, and the latch set in it); otherwise the check is skipped and counted as such;
	 * - a versioned buffer: the version bound to this preparation, which may be an earlier frame's: a mismatch only when none of the
	 *   latch's recent refs (RefLatch::kHistory) is that version's.
	 * Each site and buffer counts its checks, mismatches and skips: the first mismatch is logged as "[ORG] declared view mismatch", and
	 * a summary of every site with mismatches or skips every 10 seconds while they grow. a_site names the pass.
	 */
	void CheckDeclaredRefs(const DeclaredRefs& a_refs, const org::FramePreparationContext& a_preparation, std::string_view a_site);
}
