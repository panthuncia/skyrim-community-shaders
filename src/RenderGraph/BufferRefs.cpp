// Buffer view refs from the graph host's device (BufferRefs.h). Built with OpenRenderGraph only: nothing else includes the header.

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/BufferRefs.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/PassBuilders.h>
#	include <Render/PreparedPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/Buffers/VersionedBuffer.h>

#	include <chrono>
#	include <filesystem>
#	include <format>
#	include <span>
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
		// The ORG buffer is known here: the validator judges it by the graph's own rule, not as a handle.
		if (UndeclaredAccess::On())
			UndeclaredAccess::NoteResource(a_buffer, "BufferRefOf", a_where);
		auto* host = RenderGraphRuntime::Get().Host();
		return host ? MakeBufferRef(host->Device(), const_cast<org::Buffer&>(a_buffer).GetAPIResource().GetHandle(), a_offset, a_where) : BufferRef{};
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

	struct DeclaredRefs::Entry
	{
		std::string what;
		org::ResourceBindingToken binding;
		std::shared_ptr<const RefLatch> latch;
		std::shared_ptr<org::VersionedBuffer> late;  // a late-bound buffer: checked against its current version
		std::unique_ptr<std::atomic<bool>> reported = std::make_unique<std::atomic<bool>>(false);
	};

	namespace
	{
		template <class T>
		org::ResourceBindingToken Declare(org::RenderPassBuilder& a_builder, T& a_buffer, RefAccess a_access, std::string_view a_what,
			std::shared_ptr<const RefLatch> a_latch, DeclaredRefs& a_out, std::shared_ptr<org::VersionedBuffer> a_late = nullptr)
		{
			const auto binding = a_access == RefAccess::Write ? a_builder.UnorderedAccess(a_buffer, std::span<const org::UavView>{}).Resource() :
			                                                    a_builder.ShaderResource(a_buffer, std::span<const org::SrvView>{}).Resource();
			if (a_latch) {
				if (!a_out.entries)
					a_out.entries = std::make_shared<std::vector<DeclaredRefs::Entry>>();
				a_out.entries->push_back({ .what = std::string(a_what), .binding = binding, .latch = std::move(a_latch), .late = std::move(a_late) });
			}
			return binding;
		}
	}

	org::ResourceBindingToken DeclareRef(org::RenderPassBuilder& a_builder, const std::shared_ptr<org::Buffer>& a_buffer, RefAccess a_access, std::string_view a_what,
		std::shared_ptr<const RefLatch> a_latch, DeclaredRefs& a_out)
	{
		return Declare(a_builder, a_buffer, a_access, a_what, std::move(a_latch), a_out);
	}

	org::ResourceBindingToken DeclareRef(org::RenderPassBuilder& a_builder, org::VersionedBuffer& a_buffer, RefAccess a_access, std::string_view a_what,
		std::shared_ptr<const RefLatch> a_latch, DeclaredRefs& a_out)
	{
		return Declare(a_builder, a_buffer, a_access, a_what, std::move(a_latch), a_out);
	}

	org::ResourceBindingToken DeclareLateRef(org::RenderPassBuilder& a_builder, const std::shared_ptr<org::VersionedBuffer>& a_buffer, RefAccess a_access,
		std::string_view a_what, std::shared_ptr<const RefLatch> a_latch, DeclaredRefs& a_out)
	{
		return Declare(a_builder, *a_buffer, a_access, a_what, std::move(a_latch), a_out, a_buffer);
	}

	BufferRef DeclaredBufferRef(const org::FramePreparationContext& a_preparation, const org::ResourceBindingToken& a_binding, std::uint64_t a_offset)
	{
		// The preparation's bindings hold the version a versioned buffer binds this frame, at the slot its declaration captured.
		const auto resource = a_preparation.ResolveCapturedResource(a_preparation.CaptureResource(a_binding));
		rhi::Device device = a_preparation.device;
		if (!device)
			if (auto* host = RenderGraphRuntime::Get().Host())
				device = host->Device();
		return MakeBufferRef(device, resource.GetHandle(), a_offset, std::source_location::current());
	}

	void CheckDeclaredRefs(const DeclaredRefs& a_refs, const org::FramePreparationContext& a_preparation, std::string_view a_site)
	{
		if (!a_refs.entries)
			return;
		for (const auto& entry : *a_refs.entries) {
			const BufferRef latched = entry.latch->Get();
			if (!latched || entry.reported->load(std::memory_order_relaxed))
				continue;
			BufferRef declared{};
			try {
				if (entry.late) {
					// No version published yet: nothing the latch could carry.
					const auto current = entry.late->Get();
					auto* host = RenderGraphRuntime::Get().Host();
					if (!current || !host)
						continue;
					declared = MakeBufferRef(host->Device(), current->GetAPIResource().GetHandle(), 0, std::source_location::current());
				} else {
					declared = DeclaredBufferRef(a_preparation, entry.binding);
				}
			} catch (const std::exception& e) {
				if (!entry.reported->exchange(true, std::memory_order_relaxed))
					logger::warn("[ORG] declared view mismatch: {} reads '{}' through buffer view ref {{slot {}}}, and its declaration does not resolve: {}", a_site,
						entry.what, latched.viewSlot, e.what());
				continue;
			}
			if (declared.viewSlot != latched.viewSlot && !entry.reported->exchange(true, std::memory_order_relaxed))
				logger::warn("[ORG] declared view mismatch: {} reads '{}' through buffer view ref {{slot {}}}, the declared buffer's bound backing is slot {}",
					a_site, entry.what, latched.viewSlot, declared.viewSlot);
		}
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
