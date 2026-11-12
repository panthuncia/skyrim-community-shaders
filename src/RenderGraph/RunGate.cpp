#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/RunGate.h"

#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <OpenRenderGraph/Runtime.h>
#	include <Render/LatchBlock.h>
#	include <Render/RunRef.h>

#	include <cstring>
#	include <vector>

namespace RenderGraph
{
	std::shared_ptr<RunGate> RunGate::Create(std::string_view a_name, std::uint32_t a_entries)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !a_entries) {
			logger::warn("[ORG] run gate {}: not made (no render graph host, or no entries)", a_name);
			return nullptr;
		}
		// Closed in every run until a commit opens an entry: a run's region is zeroed when it is allocated.
		auto latch = org::LatchBlock::Create(std::string(a_name), a_entries * kEntryBytes);
		return std::make_shared<RunGate>(std::move(latch), a_entries);
	}

	RunGate::RunGate(std::shared_ptr<org::LatchBlock> a_latch, std::uint32_t a_entries) :
		latch(std::move(a_latch)), entries(a_entries), words((a_entries + 63) / 64)
	{
		open = std::make_unique<std::atomic<std::uint64_t>[]>(static_cast<std::size_t>(org::Runtime::kMaxSlots) * words);
	}

	void RunGate::Write(std::uint32_t a_entry, const std::array<std::uint32_t, 4>& a_words, bool a_work) const
	{
		if (a_entry >= entries)
			return;
		const auto run = RenderGraphRuntime::Get().Host()->CurrentRun();
		latch->Write(run, Offset(a_entry), std::as_bytes(std::span(a_words)));
		if (run.lease >= org::Runtime::kMaxSlots)
			return;
		auto& word = open[static_cast<std::size_t>(run.lease) * words + a_entry / 64];
		const std::uint64_t bit = std::uint64_t{ 1 } << (a_entry % 64);
		if (a_work)
			word.fetch_or(bit, std::memory_order_release);
		else
			word.fetch_and(~bit, std::memory_order_release);
	}

	void RunGate::Close() const
	{
		const auto run = RenderGraphRuntime::Get().Host()->CurrentRun();
		const auto bytes = latch->Bytes(run);
		std::memset(bytes.data(), 0, static_cast<std::size_t>(entries) * kEntryBytes);
		if (run.lease >= org::Runtime::kMaxSlots)
			return;
		for (std::uint32_t i = 0; i < words; ++i)
			open[static_cast<std::size_t>(run.lease) * words + i].store(0, std::memory_order_release);
	}

	bool RunGate::IsOpen(const org::RunRef& a_run, std::uint32_t a_entry) const
	{
		if (a_entry >= entries || a_run.lease >= org::Runtime::kMaxSlots)
			return true;
		const auto word = open[static_cast<std::size_t>(a_run.lease) * words + a_entry / 64].load(std::memory_order_acquire);
		return ((word >> (a_entry % 64)) & 1) != 0;
	}

	void RunGate::OpenDispatch(std::uint32_t a_entry, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z) const
	{
		Write(a_entry, { a_x, a_y, a_z, 0 }, a_x && a_y && a_z);
	}

	void RunGate::OpenDraw(std::uint32_t a_entry, std::uint32_t a_vertices, std::uint32_t a_instances) const
	{
		Write(a_entry, { a_vertices, a_instances, 0, 0 }, a_vertices && a_instances);
	}

	void RunGate::Dispatch(org::RecordingContext& a_recording, const ComputeProgram& a_program, std::span<const std::uint32_t> a_constants, std::uint32_t a_entry) const
	{
		if (a_entry < entries)
			a_program.DispatchIndirect(a_recording, a_constants, *latch, Offset(a_entry));
	}
}

#endif
