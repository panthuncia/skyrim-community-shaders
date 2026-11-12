// Buffer view refs from the graph host's device (BufferRefs.h). Built with OpenRenderGraph only: nothing else includes the header.

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/BufferRefs.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Resources/Buffers/Buffer.h>

#	include <chrono>
#	include <filesystem>
#	include <format>
#	include <string>

namespace RenderGraph
{
	namespace logger = SKSE::log;

	BufferRef BufferRefOf(rhi::ResourceHandle a_buffer, std::uint64_t a_offset, std::source_location a_where)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		return host ? BufferRefOf(host->Device(), a_buffer, a_offset, a_where) : BufferRef{};
	}

	BufferRef BufferRefOf(const org::Buffer& a_buffer, std::uint64_t a_offset, std::source_location a_where)
	{
		return BufferRefOf(const_cast<org::Buffer&>(a_buffer).GetAPIResource().GetHandle(), a_offset, a_where);
	}

	void SetBufferRefChecks(bool a_on)
	{
		g_bufferRefChecks.store(a_on, std::memory_order_relaxed);
		if (a_on)
			logger::info("[ORG] ref check (CS_DCLF_REF_CHECK=1): buffer view refs are checked against BasicRHI's region as they are made "
						 "and recorded; the first mismatches are logged as \"ref check:\" lines, with a summary every 10 seconds");
	}

	namespace
	{
		struct Counts
		{
			std::atomic<std::uint64_t> checked{ 0 };
			std::atomic<std::uint64_t> mismatched{ 0 };
			std::atomic<std::uint32_t> logged{ 0 };
			std::atomic<std::int64_t> lastSummary{ 0 };
		};
		Counts& CheckCounts()
		{
			static Counts counts;
			return counts;
		}
		constexpr std::uint32_t kLoggedMismatches = 32;

		std::string Caller(const std::source_location& a_where)
		{
			return std::format("{}:{}", std::filesystem::path(a_where.file_name()).filename().string(), a_where.line());
		}

		// A summary line every 10 seconds, so a clean run says so.
		void Summarize(Counts& a_counts)
		{
			const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
			auto last = a_counts.lastSummary.load(std::memory_order_relaxed);
			if (now - last < 10 || !a_counts.lastSummary.compare_exchange_strong(last, now))
				return;
			logger::info("[ORG] ref check: {} refs checked, {} mismatched", a_counts.checked.load(std::memory_order_relaxed),
				a_counts.mismatched.load(std::memory_order_relaxed));
		}
	}

	void CheckBufferRef(rhi::Device a_device, BufferRef a_ref, rhi::ResourceHandle a_buffer, std::uint64_t a_address, std::uint64_t a_bytes, std::string_view a_what,
		std::source_location a_where)
	{
		if (!a_ref || !a_device || !BufferRefChecksOn())
			return;
		rhi::vulkan::BufferViewRefCheck check{};
		if (!rhi::vulkan::check_buffer_view_ref(a_device, a_ref.viewSlot, a_buffer, check) || check.slotCount == 0)
			return;  // not Vulkan, or no region
		auto& counts = CheckCounts();
		if (counts.checked.fetch_add(1, std::memory_order_relaxed) == 0)
			logger::info("[ORG] ref check: the region is slots [{}, {}) in {} heaps; heap slot 0 (what a load through a null ref reads) holds {} in {} of them",
				check.firstSlot, check.firstSlot + check.slotCount, check.heaps, check.slotZeroNonzero ? "a DESCRIPTOR" : "zeros",
				check.slotZeroNonzero);
		std::string problems;
		auto problem = [&](std::string_view a_text) {
			if (!problems.empty())
				problems += "; ";
			problems += a_text;
		};
		if (!check.inRegion) {
			problem(std::format("slot {} is not a pair's SRV slot in the region [{}, {})", a_ref.viewSlot, check.firstSlot, check.firstSlot + check.slotCount));
		} else {
			if (!check.pairLive)
				problem("its pair is freed (no buffer holds it)");
			if (check.resourceGiven && !check.resourceLive)
				problem("its buffer is not alive");
			if (check.resourceGiven && check.resourceLive && !check.pairIsResource)
				problem(std::format("its pair is buffer {}:{}'s, not this buffer's {}:{}", check.pairKey & 0xFFFFFFFFu, check.pairKey >> 32, a_buffer.index,
					a_buffer.generation));
			if (check.resourceGiven && check.resourceLive && check.resourceAddress != check.descriptorAddress)
				problem(std::format("the buffer is at 0x{:x}, its pair's descriptors at 0x{:x}", check.resourceAddress, check.descriptorAddress));
			if (a_address && check.descriptorAddress + a_ref.byteOffset != a_address)
				problem(std::format("it reads at 0x{:x} (pair 0x{:x} + offset {}), not at the address 0x{:x}", check.descriptorAddress + a_ref.byteOffset,
					check.descriptorAddress, a_ref.byteOffset, a_address));
			if (std::uint64_t(a_ref.byteOffset) + (std::max<std::uint64_t>)(a_bytes, 1) > check.descriptorRange)
				problem(std::format("offset {} + {} bytes is past the view's {} bytes", a_ref.byteOffset, a_bytes, check.descriptorRange));
			if (check.heapsMismatched)
				problem(std::format("{} of {} heaps hold other bytes for the pair", check.heapsMismatched, check.heaps));
		}
		if (!problems.empty()) {
			counts.mismatched.fetch_add(1, std::memory_order_relaxed);
			if (counts.logged.fetch_add(1, std::memory_order_relaxed) < kLoggedMismatches)
				logger::error("[ORG] ref check: {} ref {{slot {}, offset {}}} (pair {}, view 0x{:x} + {} bytes; address 0x{:x}, {} bytes) from {}: {}", a_what,
					a_ref.viewSlot, a_ref.byteOffset, check.pair, check.descriptorAddress, check.descriptorRange, a_address, a_bytes, Caller(a_where), problems);
		}
		Summarize(counts);
	}

	void CheckBufferRef(BufferRef a_ref, std::uint64_t a_address, std::uint64_t a_bytes, std::string_view a_what, std::source_location a_where)
	{
		if (!BufferRefChecksOn())
			return;
		// A null ref beside a real address: a shader that loads through it reads heap slot 0 as a buffer.
		if (!a_ref) {
			if (a_address) {
				auto& counts = CheckCounts();
				counts.checked.fetch_add(1, std::memory_order_relaxed);
				counts.mismatched.fetch_add(1, std::memory_order_relaxed);
				if (counts.logged.fetch_add(1, std::memory_order_relaxed) < kLoggedMismatches)
					logger::error("[ORG] ref check: {} ref is null, but its address is 0x{:x} ({} bytes) from {}", a_what, a_address, a_bytes, Caller(a_where));
			}
			return;
		}
		if (auto* host = RenderGraphRuntime::Get().Host())
			CheckBufferRef(host->Device(), a_ref, {}, a_address, a_bytes, a_what, a_where);
	}
}

#endif
