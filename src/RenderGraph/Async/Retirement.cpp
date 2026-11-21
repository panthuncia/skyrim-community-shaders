// Retirement: CS's retirement clock, OMS's join of the graph host's retired callback and a CPU pipeline's frame timeline.
#include "RenderGraph/Async/Retirement.h"

#include "RenderGraph/RenderGraphRuntime.h"

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/GraphHostRetirement.h>

#include <utility>

namespace RenderGraph::Frame::Retirement
{
	namespace
	{
		org::services::GraphHostRetirement& State()
		{
			static org::services::GraphHostRetirement retirement([]() -> const org::GraphHost* { return RenderGraphRuntime::Get().Host(); });
			return retirement;
		}
	}

	const std::shared_ptr<org::services::RetirementClock>& Clock()
	{
		return State().Join().Clock();
	}

	std::uint64_t SubmittedPoint()
	{
		return State().Join().SubmittedPoint();
	}

	org::GraphHost::GpuPoint SubmittedGpuPoint()
	{
		return State().SubmittedGpuPoint();
	}

	void SealCpuFrames()
	{
		State().Join().SealCpuFrames();
	}

	org::GraphHost::GpuPoint SealedGpuPoint()
	{
		return State().SealedGpuPoint();
	}

	bool Sealed(const org::GraphHost::GpuPoint& a_point)
	{
		return State().Sealed(a_point);
	}

	void NoteGraphSubmissionsRetired(std::uint64_t a_submissions)
	{
		State().Join().NoteGraphSubmissionsRetired(a_submissions);
	}

	void SetCpuFrameTimeline(std::shared_ptr<rhi::TimelinePtr> a_timeline)
	{
		State().SetCpuFrameTimeline(std::move(a_timeline));
	}

	void NoteCpuFrameOpened(std::uint64_t a_value)
	{
		State().Join().NoteCpuFrameOpened(a_value);
	}

	void NoteCpuFrameSignalled(std::uint64_t a_value)
	{
		State().Join().NoteCpuFrameSignalled(a_value);
	}

	std::string StateText()
	{
		return State().Join().StateText();
	}

	void Poll()
	{
		State().Join().Poll();
	}
}
