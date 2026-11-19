#pragma once

// Buffer view refs from BasicRHI buffers. Device::GetBufferViewRef makes a buffer's raw view pair on first use and frees it with the buffer.

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

	// Ref checks, CS_DCLF_REF_CHECK, off by default. Made refs and refs given to CheckBufferRef are checked against BasicRHI's view ref region.
	inline std::atomic<bool> g_bufferRefChecks{ false };
	inline bool BufferRefChecksOn() { return g_bufferRefChecks.load(std::memory_order_relaxed); }
	/** @brief Turns the ref checks on or off. Set once, at install. */
	void SetBufferRefChecks(bool a_on);

	/** @brief Checks a ref against BasicRHI's view ref region on Vulkan, logging the first mismatches with a_what. Null refs pass.
	*  Any thread. Takes BasicRHI's views mutex. */
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

	/** @brief a_buffer's ref at a_offset, or null. A raw ref by handle, so the undeclared-access validator logs it with a_where. Any thread. */
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

	/** @brief A ref for a buffer every reading pass declares, so the undeclared-access validator skips it. Name those passes at the call. */
	BufferRef DeclaredRefOf(const org::Buffer& a_buffer, std::uint64_t a_offset = 0, std::source_location a_where = std::source_location::current());
	/** @brief DeclaredRefOf on a_device, for a buffer made before the graph host is reachable. The same rule applies. Any thread. */
	BufferRef DeclaredRefOf(rhi::Device a_device, const org::Buffer& a_buffer, std::uint64_t a_offset = 0,
		std::source_location a_where = std::source_location::current());

	/** @brief CheckBufferRef on the graph host's device; nothing without a host or with the checks off. */
	void CheckBufferRef(BufferRef a_ref, std::uint64_t a_address, std::uint64_t a_bytes, std::string_view a_what,
		std::source_location a_where = std::source_location::current());

	/** @brief The render thread's frame for latch checks, advanced at each frame's end. A latch keeps its frame so checks can tell frames apart. */
	inline std::atomic<std::uint64_t> g_refFrame{ 1 };
	inline std::uint64_t RefFrame() { return g_refFrame.load(std::memory_order_acquire); }
	/** @brief The frame's end. Render thread. */
	inline void AdvanceRefFrame() { g_refFrame.fetch_add(1, std::memory_order_acq_rel); }

	/** @brief The ref a site last latched for a buffer at commit, with its frame and the last few before it. Prepare compares it with the declaration.
	*  Set it to the buffer's ref at byte 0, since the check compares view slots. Any thread. */
	class RefLatch
	{
	public:
		/** @brief The refs kept, for the frames in flight a preparation may be for. */
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

	/** @brief Buffers a pass uses through refs latched at commit, each with its declaration's binding and latch. Cheap to copy.
	*  An interim form. Prepare should write these refs from the declaration and drop the latch. */
	struct DeclaredRefs
	{
		struct Entry;
		std::shared_ptr<std::vector<Entry>> entries;
	};

	/** @brief How a pass uses a buffer through its ref. Read declares a shader resource, Write an unordered access, without views. */
	enum class RefAccess
	{
		Read,
		Write
	};

	/** @brief Graph build. Declares a_buffer without views, since a ref is BasicRHI's raw pair, and records it with a_latch.
	*  Returns the binding for DeclaredBufferRef. */
	org::ResourceBindingToken DeclareRef(org::RenderPassBuilder& a_builder, const std::shared_ptr<org::Buffer>& a_buffer, RefAccess a_access, std::string_view a_what,
		std::shared_ptr<const RefLatch> a_latch, DeclaredRefs& a_out);
	/** @brief DeclareRef for a versioned buffer: the check uses the version bound to the preparation. Not for a late-bound one. */
	org::ResourceBindingToken DeclareRef(org::RenderPassBuilder& a_builder, org::VersionedBuffer& a_buffer, RefAccess a_access, std::string_view a_what,
		std::shared_ptr<const RefLatch> a_latch, DeclaredRefs& a_out);
	/** @brief DeclareRef for a late-bound buffer. Its declaration resolves to a stand-in, so the check uses the published version at Prepare. */
	org::ResourceBindingToken DeclareLateRef(org::RenderPassBuilder& a_builder, const std::shared_ptr<org::VersionedBuffer>& a_buffer, RefAccess a_access,
		std::string_view a_what, std::shared_ptr<const RefLatch> a_latch, DeclaredRefs& a_out);

	/** @brief A per-frame buffer outside the graph made declarable as a late-bound VersionedBuffer. Publishing costs no graph build. */
	class LateRefBuffer
	{
	public:
		/** @brief Any thread: the late-bound buffer, its stand-in named a_name. */
		explicit LateRefBuffer(std::string_view a_name);
		/** @brief Render thread. Makes a_backing the current version and latches a_used, or a_backing's own ref when a_used is null.
		*  A null backing clears the latch. Returns a_backing's ref at byte 0. */
		BufferRef Publish(const std::shared_ptr<org::Buffer>& a_backing, std::shared_ptr<const void> a_owner, BufferRef a_used = {});
		/** @brief Graph build. Declares the buffer for a_access without views, with the latch. */
		void Declare(org::RenderPassBuilder& a_builder, RefAccess a_access, DeclaredRefs& a_out) const;

	private:
		std::string name;
		std::shared_ptr<org::VersionedBuffer> buffer;
		std::shared_ptr<RefLatch> latch = std::make_shared<RefLatch>();
	};

	/** @brief Prepare, on the host's thread. The ref of the declared buffer's bound backing at a_offset, or null.
	*  A late-bound buffer resolves to its stand-in, so its readers take the published version's ref instead. */
	BufferRef DeclaredBufferRef(const org::FramePreparationContext& a_preparation, const org::ResourceBindingToken& a_binding, std::uint64_t a_offset = 0);

	/**
	* @brief Prepare, on the host's thread. Checks each latched ref against the declared buffer's backing, keyed to the frame.
	* Logs the first mismatch per site and a periodic summary. Late-bound checks during another frame's writes are skipped and counted.
	*/
	void CheckDeclaredRefs(const DeclaredRefs& a_refs, const org::FramePreparationContext& a_preparation, std::string_view a_site);
}
