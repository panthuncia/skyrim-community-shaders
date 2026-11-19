// Buffer view refs from the graph host's device. Built with OpenRenderGraph only.

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
#	include <algorithm>
#	include <filesystem>
#	include <format>
#	include <mutex>
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
		// The validator judges an ORG buffer by the graph's own rule, not as a handle.
		if (UndeclaredAccess::On())
			UndeclaredAccess::NoteResource(a_buffer, "BufferRefOf", a_where);
		auto* host = RenderGraphRuntime::Get().Host();
		return host ? MakeBufferRef(host->Device(), const_cast<org::Buffer&>(a_buffer).GetAPIResource().GetHandle(), a_offset, a_where) : BufferRef{};
	}

	BufferRef DeclaredRefOf(const org::Buffer& a_buffer, std::uint64_t a_offset, std::source_location a_where)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		return host ? MakeBufferRef(host->Device(), const_cast<org::Buffer&>(a_buffer).GetAPIResource().GetHandle(), a_offset, a_where) : BufferRef{};
	}

	BufferRef DeclaredRefOf(rhi::Device a_device, const org::Buffer& a_buffer, std::uint64_t a_offset, std::source_location a_where)
	{
		return MakeBufferRef(a_device, const_cast<org::Buffer&>(a_buffer).GetAPIResource().GetHandle(), a_offset, a_where);
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

	namespace
	{
		/** @brief One site's and buffer's latch checks, kept across graph builds. */
		struct MismatchCounts
		{
			std::string site, what;
			std::atomic<std::uint64_t> checked{ 0 }, mismatched{ 0 }, skipped{ 0 };
			std::atomic<bool> logged{ false };
			std::uint64_t reportedMismatched = 0, reportedSkipped = 0;  // at the last summary, under the registry's lock
		};

		struct MismatchRegistry
		{
			std::mutex lock;
			std::vector<std::unique_ptr<MismatchCounts>> entries;
			std::atomic<std::int64_t> lastSummary{ 0 };
		};

		MismatchRegistry& Mismatches()
		{
			static MismatchRegistry registry;
			return registry;
		}

		MismatchCounts& CountsOf(std::string_view a_site, std::string_view a_what)
		{
			auto& registry = Mismatches();
			std::lock_guard guard(registry.lock);
			for (const auto& entry : registry.entries)
				if (entry->site == a_site && entry->what == a_what)
					return *entry;
			auto& made = registry.entries.emplace_back(std::make_unique<MismatchCounts>());
			made->site = a_site;
			made->what = a_what;
			return *made;
		}

		// Every 10 seconds, the sites whose counts grew. A real mismatch grows every frame, a race between frames in flight shows as skips.
		void SummarizeMismatches()
		{
			auto& registry = Mismatches();
			const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
			auto last = registry.lastSummary.load(std::memory_order_relaxed);
			if (now - last < 10 || !registry.lastSummary.compare_exchange_strong(last, now))
				return;
			std::lock_guard guard(registry.lock);
			for (const auto& entry : registry.entries) {
				const auto mismatched = entry->mismatched.load(std::memory_order_relaxed);
				const auto skipped = entry->skipped.load(std::memory_order_relaxed);
				if (mismatched == entry->reportedMismatched && skipped == entry->reportedSkipped)
					continue;
				entry->reportedMismatched = mismatched;
				entry->reportedSkipped = skipped;
				logger::info("[ORG] declared view mismatch stats: {} reads '{}': {} mismatched of {} checked, {} skipped (the latch and the backing were "
							 "different frames)",
					entry->site, entry->what, mismatched, entry->checked.load(std::memory_order_relaxed), skipped);
			}
		}
	}

	struct DeclaredRefs::Entry
	{
		std::string what;
		org::ResourceBindingToken binding;
		std::shared_ptr<const RefLatch> latch;
		std::shared_ptr<org::VersionedBuffer> late;  // checked against its current version
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

	LateRefBuffer::LateRefBuffer(std::string_view a_name) :
		name(a_name)
	{
		auto standIn = org::Buffer::CreateUnmaterializedStructuredBuffer(1, 4, false);
		standIn->SetName(std::format("{}.stand-in", a_name));
		standIn->Materialize();
		buffer = org::VersionedBuffer::CreateLateBound(std::move(standIn));
	}

	BufferRef LateRefBuffer::Publish(const std::shared_ptr<org::Buffer>& a_backing, std::shared_ptr<const void> a_owner, BufferRef a_used)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!a_backing || !host) {
			latch->Set({});
			return {};
		}
		// The same backing again keeps its version.
		if (buffer->Get() != a_backing)
			buffer->PublishLate(a_backing, std::move(a_owner));
		const BufferRef ref = MakeBufferRef(host->Device(), a_backing->GetAPIResource().GetHandle(), 0, std::source_location::current());
		latch->Set(a_used ? a_used : ref);
		return ref;
	}

	void LateRefBuffer::Declare(org::RenderPassBuilder& a_builder, RefAccess a_access, DeclaredRefs& a_out) const
	{
		DeclareLateRef(a_builder, buffer, a_access, name, latch, a_out);
	}

	BufferRef DeclaredBufferRef(const org::FramePreparationContext& a_preparation, const org::ResourceBindingToken& a_binding, std::uint64_t a_offset)
	{
		// The preparation's bindings hold the version a versioned buffer binds this frame.
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
			// Read the frame before the latch, so a frame that ends during the check makes it a skip.
			const std::uint64_t frame = RefFrame();
			const auto latched = entry.latch->Take();
			if (!latched.ref)
				continue;
			// By site and buffer, across builds.
			auto& counts = CountsOf(a_site, entry.what);
			BufferRef declared{};
			bool matched = false;
			try {
				if (entry.late) {
					// The current version and the latch must both be this frame's. Otherwise it is a race between frames in flight, not a mismatch.
					const auto current = entry.late->Get();
					auto* host = RenderGraphRuntime::Get().Host();
					if (!current || !host)
						continue;  // no version published yet
					declared = MakeBufferRef(host->Device(), current->GetAPIResource().GetHandle(), 0, std::source_location::current());
					if (latched.frame != frame || RefFrame() != frame) {
						counts.skipped.fetch_add(1, std::memory_order_relaxed);
						continue;
					}
					matched = declared.viewSlot == latched.ref.viewSlot;
				} else {
					// The version bound to this preparation, which may be older than the latch's latest.
					declared = DeclaredBufferRef(a_preparation, entry.binding);
					matched = declared.viewSlot == latched.ref.viewSlot ||
					          std::ranges::any_of(latched.history, [&](const BufferRef& a_ref) { return a_ref && a_ref.viewSlot == declared.viewSlot; });
				}
			} catch (const std::exception& e) {
				counts.checked.fetch_add(1, std::memory_order_relaxed);
				counts.mismatched.fetch_add(1, std::memory_order_relaxed);
				if (!counts.logged.exchange(true, std::memory_order_relaxed))
					logger::warn("[ORG] declared view mismatch: {} reads '{}' through buffer view ref {{slot {}}}, and its declaration does not resolve: {}", a_site,
						entry.what, latched.ref.viewSlot, e.what());
				continue;
			}
			counts.checked.fetch_add(1, std::memory_order_relaxed);
			if (matched)
				continue;
			counts.mismatched.fetch_add(1, std::memory_order_relaxed);
			if (!counts.logged.exchange(true, std::memory_order_relaxed))
				logger::warn("[ORG] declared view mismatch: {} reads '{}' through buffer view ref {{slot {}}} (latched in frame {}), the declared buffer's bound "
							 "backing is slot {} (first; \"declared view mismatch stats\" lines count them)",
					a_site, entry.what, latched.ref.viewSlot, latched.frame, declared.viewSlot);
		}
		SummarizeMismatches();
	}

	void CheckBufferRef(BufferRef a_ref, std::uint64_t a_address, std::uint64_t a_bytes, std::string_view a_what, std::source_location a_where)
	{
		if (!BufferRefChecksOn())
			return;
		// A null ref beside a real address makes a shader read heap slot 0 as a buffer.
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
