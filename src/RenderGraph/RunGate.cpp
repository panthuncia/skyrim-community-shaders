#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/RunGate.h"

#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>

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

	void RunGate::Write(std::uint32_t a_entry, const std::array<std::uint32_t, 4>& a_words) const
	{
		if (a_entry >= entries)
			return;
		latch->Write(RenderGraphRuntime::Get().Host()->CurrentRun(), Offset(a_entry), std::as_bytes(std::span(a_words)));
	}

	void RunGate::Close() const
	{
		const auto bytes = latch->Bytes(RenderGraphRuntime::Get().Host()->CurrentRun());
		std::memset(bytes.data(), 0, static_cast<std::size_t>(entries) * kEntryBytes);
	}

	void RunGate::OpenDispatch(std::uint32_t a_entry, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z) const
	{
		Write(a_entry, { a_x, a_y, a_z, 0 });
	}

	void RunGate::OpenDraw(std::uint32_t a_entry, std::uint32_t a_vertices, std::uint32_t a_instances) const
	{
		Write(a_entry, { a_vertices, a_instances, 0, 0 });
	}

	void RunGate::Dispatch(org::RecordingContext& a_recording, const ComputeProgram& a_program, std::span<const std::uint32_t> a_constants, std::uint32_t a_entry) const
	{
		if (a_entry < entries)
			a_program.DispatchIndirect(a_recording, a_constants, *latch, Offset(a_entry));
	}
}

#endif
