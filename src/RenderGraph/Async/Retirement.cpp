// Retirement: CS's retirement clock, advanced by the graph host's retired callback and a CPU pipeline's frame timeline.
#include "RenderGraph/Async/Retirement.h"

#include "RenderGraph/Async/EventQueue.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <ORGModuleServices/BackingPool.h>
#include <OpenRenderGraph/GraphHost.h>
#include <rhi.h>

#include <algorithm>
#include <atomic>
#include <deque>

namespace RenderGraph::Frame::Retirement
{
	namespace
	{
		/** @brief A point taken and the two counts it stands for. */
		struct TakenPoint
		{
			std::uint64_t point = 0, host = 0, cpu = 0;
		};

		struct Sources
		{
			// Points the render thread took, passed to the advancing run, oldest first.
			EventQueue<TakenPoint, 256> taken;
			// The host's retired submissions, from any thread, and the CPU frames signalled and their timeline, on the render thread.
			// The timeline is set once before the first signal, and Poll reads it after an acquire of signalled.
			std::atomic<std::uint64_t> hostRetired{ 0 };
			std::atomic<std::uint64_t> cpuSignalled{ 0 };
			// The value the open CPU frame signals at its end, or 0 before the first. What it binds mid-frame retires after its own work.
			std::atomic<std::uint64_t> cpuOpen{ 0 };
			std::shared_ptr<rhi::TimelinePtr> cpuTimeline;
			// Only one run at a time, and it owns the noted points it has not passed yet.
			std::atomic<bool> running{ false };
			std::deque<TakenPoint> pending;
			// The newest point noted. Callers are on several threads.
			std::atomic<std::uint64_t> lastTaken{ 0 };
			// CPU frames signalled before the last present.
			std::atomic<std::uint64_t> cpuSealed{ 0 };
		};

		Sources& State()
		{
			static Sources sources;
			return sources;
		}
	}

	const std::shared_ptr<org::services::RetirementClock>& Clock()
	{
		static const auto clock = std::make_shared<org::services::RetirementClock>();
		return clock;
	}

	std::uint64_t SubmittedPoint()
	{
		const auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return 0;
		auto& s = State();
		const std::uint64_t submissions = host->SubmissionsMade();
		// A point taken inside a CPU frame covers that frame's own work, which its end signal completes.
		// The colour site may execute behind host submissions made meanwhile.
		const std::uint64_t cpu = std::max(s.cpuSignalled.load(std::memory_order_relaxed), s.cpuOpen.load(std::memory_order_relaxed));
		const std::uint64_t point = submissions + cpu;
		// Note every point whatever its source, so points taken before the first CPU frame also retire after it.
		std::uint64_t last = s.lastTaken.load(std::memory_order_relaxed);
		while (point > last && !s.lastTaken.compare_exchange_weak(last, point, std::memory_order_relaxed)) {
		}
		if (point > last)
			s.taken.Push({ point, submissions, cpu });
		return point;
	}

	org::GraphHost::GpuPoint SubmittedGpuPoint()
	{
		const auto* host = RenderGraphRuntime::Get().Host();
		org::GraphHost::GpuPoint point = host ? host->SubmittedPoint() : org::GraphHost::GpuPoint{};
		auto& s = State();
		if (const std::uint64_t cpu = s.cpuSignalled.load(std::memory_order_relaxed); cpu && s.cpuTimeline)
			point.points.emplace_back(s.cpuTimeline, cpu);
		return point;
	}

	void SealCpuFrames()
	{
		auto& s = State();
		s.cpuSealed.store(s.cpuSignalled.load(std::memory_order_relaxed), std::memory_order_release);
	}

	org::GraphHost::GpuPoint SealedGpuPoint()
	{
		const auto* host = RenderGraphRuntime::Get().Host();
		org::GraphHost::GpuPoint point = host ? host->SealedPoint() : org::GraphHost::GpuPoint{};
		auto& s = State();
		if (const std::uint64_t cpu = s.cpuSealed.load(std::memory_order_acquire); cpu && s.cpuTimeline)
			point.points.emplace_back(s.cpuTimeline, cpu);
		return point;
	}

	bool Sealed(const org::GraphHost::GpuPoint& a_point)
	{
		const auto sealed = SealedGpuPoint();
		// A seal still queued on the submission thread has no values yet, so nothing can be said until the thread passes it.
		if (!sealed.Passed())
			return true;
		// Nothing is sealed before the first present.
		if (sealed.points.empty())
			return true;
		// A point still in the submission thread's queue names work that is not on the device yet.
		if (!a_point.Passed())
			return false;
		for (const auto& [timeline, value] : a_point.Resolve()) {
			if (!timeline || !*timeline || (*timeline)->GetCompletedValue() >= value)
				continue;
			const auto known = std::ranges::find_if(sealed.points, [&](const auto& a_sealed) { return a_sealed.first.get() == timeline.get(); });
			if (known == sealed.points.end() || known->second < value)
				return false;
		}
		return true;
	}

	void NoteGraphSubmissionsRetired(std::uint64_t a_submissions)
	{
		auto& s = State();
		std::uint64_t seen = s.hostRetired.load(std::memory_order_relaxed);
		while (seen < a_submissions && !s.hostRetired.compare_exchange_weak(seen, a_submissions, std::memory_order_release, std::memory_order_relaxed)) {
		}
		// Before any CPU frame the points are the host's counts.
		if (!s.cpuOpen.load(std::memory_order_acquire))
			Clock()->Advance(a_submissions);
		Poll();
	}

	void SetCpuFrameTimeline(std::shared_ptr<rhi::TimelinePtr> a_timeline)
	{
		State().cpuTimeline = std::move(a_timeline);
	}

	void NoteCpuFrameOpened(std::uint64_t a_value)
	{
		State().cpuOpen.store(a_value, std::memory_order_release);
	}

	void NoteCpuFrameSignalled(std::uint64_t a_value)
	{
		State().cpuSignalled.store(a_value, std::memory_order_release);
	}

	std::string StateText()
	{
		auto& s = State();
		const auto* host = RenderGraphRuntime::Get().Host();
		const std::uint64_t cpu = s.cpuTimeline && *s.cpuTimeline ? (*s.cpuTimeline)->GetCompletedValue() : 0;
		const auto front = s.pending.empty() ? TakenPoint{} : s.pending.front();
		return fmt::format("clock {} ({} waiting), last point taken {}, {} pending (first {}: host {} cpu {}), host retired {} of {} made, CPU frames "
						   "completed {} signalled {} open {}",
			Clock()->Completed(), Clock()->Waiting(), s.lastTaken.load(), s.pending.size(), front.point, front.host, front.cpu,
			s.hostRetired.load(), host ? host->SubmissionsMade() : 0, cpu, s.cpuSignalled.load(), s.cpuOpen.load());
	}

	void Poll()
	{
		auto& s = State();
		if (s.running.exchange(true, std::memory_order_acquire))
			return;
		s.taken.Drain([&](TakenPoint&& a_point) { s.pending.push_back(a_point); });
		const std::uint64_t host = s.hostRetired.load(std::memory_order_acquire);
		const bool cpuFrames = s.cpuOpen.load(std::memory_order_acquire) != 0;
		const std::uint64_t cpu = cpuFrames && s.cpuTimeline && *s.cpuTimeline ? (*s.cpuTimeline)->GetCompletedValue() : 0;
		// Both counts only rise, so noted points retire in order and the newest one both passed is the clock's.
		std::uint64_t reached = 0;
		while (!s.pending.empty() && s.pending.front().host <= host && s.pending.front().cpu <= cpu) {
			reached = s.pending.front().point;
			s.pending.pop_front();
		}
		if (reached)
			Clock()->Advance(reached);
		s.running.store(false, std::memory_order_release);
	}
}

