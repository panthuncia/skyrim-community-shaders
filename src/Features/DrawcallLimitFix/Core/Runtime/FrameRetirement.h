#pragma once

#include <OpenRenderGraph/GraphHost.h>

#include <cstdint>
#include <string>
#include <memory>

namespace org::services
{
	class RetirementClock;
}

namespace DCLF
{
	/**
	 * @brief DCLF's one retirement clock: the GPU's progress over everything DCLF submits, which every pooled lease is bound against.
	 * It has two sources, the graph host's retired callback and the CPU pipeline's per-frame timeline signals. It tracks exact completion.
	 * Never use host frame numbers: an async epoch's ticket is numbered when prepared, so a later-numbered completion says nothing
	 * about work submitted after it. A point counts both sources when taken, and retires once both have passed it. Point 0 means nothing.
	 */
	const std::shared_ptr<org::services::RetirementClock>& FrameRetirement();

	/**
	 * @brief Submitting thread. The point that retires once everything submitted so far is done, or 0 without a graph host.
	 * A frame binds what it read with this after its last submission, so a backing is not reused before the frame completes.
	 */
	std::uint64_t SubmittedRetirementPoint();

	/** @brief Submitting thread. The GPU points behind SubmittedRetirementPoint, for a GPU wait on it. */
	org::GraphHost::GpuPoint SubmittedGpuPoint();

	/** @brief Render thread, at the frame's start. Joins the CPU frames signalled so far, all before the last present, into the sealed point. */
	void SealCpuFrames();

	/**
	 * @brief Any thread. The GPU points of the work the last present sent to the device.
	 * A producer whose signal later work of the frame waits for may wait only on these. Work submitted since may only reach the device
	 * at the frame's present, which waits for that very signal. Use SubmittedGpuPoint for binds, not for those waits.
	 */
	org::GraphHost::GpuPoint SealedGpuPoint();

	/** @brief Any thread. Whether every point of a_point is reached or covered by SealedGpuPoint. */
	bool Sealed(const org::GraphHost::GpuPoint& a_point);

	/** @brief The graph host's retired callback, any thread. Every host submission up to a_submissions is complete. */
	void NoteGraphSubmissionsRetired(std::uint64_t a_submissions);

	/** @brief Render thread. The CPU pipeline's frame timeline, made once, and each frame's signal queued behind its callbacks. */
	void SetCpuFrameTimeline(std::shared_ptr<rhi::TimelinePtr> a_timeline);
	void NoteCpuFrameSignalled(std::uint64_t a_value);
	/**
	 * @brief Render thread, at a CPU frame's start. a_value is the signal the frame's end will queue.
	 * Every point taken until then includes it, so nothing the frame binds retires before its work.
	 */
	void NoteCpuFrameOpened(std::uint64_t a_value);

	/**
	 * @brief Any thread. Advances the clock to the newest point both sources passed.
	 * Only one call runs at a time. Others return at once and the next run catches up, so nobody waits.
	 */
	void PollRetirement();
	/**
	 * @brief Render thread, while the host waits for GPU work without submitting. Retires the host's submissions once they are done.
	 * The host only reports them when it reuses a slot, so a wait covered by them would otherwise wait for itself.
	 */
	void PollHostRetirement();
	/** @brief Any thread, read racily for a stall's message. The clock, the noted points and both sources' counts. */
	std::string RetirementState();
}
