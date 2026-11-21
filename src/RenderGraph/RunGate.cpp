// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/RunGate.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/RunRef.h>

#include <string>

namespace RenderGraph
{
	std::shared_ptr<RunGate> RunGate::Create(std::string_view a_name, std::uint32_t a_entries)
	{
		// Made at setup, where the graph runs. A gate that cannot be made is fatal in every build.
		if (!RenderGraphRuntime::Get().Host() || !a_entries)
			RenderGraph::Fatal("run gates", fmt::format("{} could not be made ({})", a_name, !a_entries ? "no entries" : "no render graph host"));
		std::string error;
		auto gate = org::RunGate::Create<RunGate>(a_name, a_entries, &error);
		if (!gate)
			RenderGraph::Fatal("run gates", fmt::format("{} could not be made ({})", a_name, error));
		return gate;
	}

	org::RunRef RunGate::Current()
	{
		return RenderGraphRuntime::Get().Host()->CurrentRun();
	}

	void RunGate::Dispatch(org::RecordingContext& a_recording, const ComputeProgram& a_program, std::span<const std::uint32_t> a_constants, std::uint32_t a_entry) const
	{
		if (a_entry < Entries())
			a_program.DispatchIndirect(a_recording, a_constants, Latch(), Offset(a_entry));
	}
}
