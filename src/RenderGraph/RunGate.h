#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <array>
#	include <cstdint>
#	include <memory>
#	include <span>
#	include <string_view>

namespace org
{
	class LatchBlock;
	class RecordingContext;
}

struct ComputeProgram;

namespace RenderGraph
{
	/**
	 * @brief A per-run switch for a client's dispatches and draws that keeps their recording: the work's group or vertex counts
	 * are in a latch block, and each run's data decides whether it does anything.
	 *
	 * A pass that runs on some runs of its anchor only (a chain that is on this frame, a copy for the frame the upscaler ran)
	 * records an indirect dispatch or draw through its gate entry instead of reading a flag in Prepare. Nothing about the run goes
	 * into the pass's revision, so the recording kept for its frame slot is submitted again whatever the gate says: no
	 * preparation, no mismatch counter.
	 *
	 * The frame anchors close every gate of a client (ClientDesc::gates) at the start of each run, before any commit: a run does
	 * work only through the entries its client's commit opened. A client that is inactive for a run, or whose commit opens
	 * nothing, therefore runs its passes as no work, and never repeats an older run's counts.
	 *
	 * Threads: Open and Close on the render thread, inside the anchor's epoch (the client's commit). The record helpers on the
	 * graph host's thread.
	 */
	class RunGate
	{
	public:
		/** @brief One entry: three group counts and a pad, or four draw words (vertex count, instances, first vertex, first instance). */
		static constexpr std::uint32_t kEntryBytes = 16;

		/**
		 * @brief A gate with a_entries entries, closed. Render thread, with the runtime active (the latch has the host's frame
		 * slots). Null, logged, without a host.
		 */
		static std::shared_ptr<RunGate> Create(std::string_view a_name, std::uint32_t a_entries = 1);

		std::uint32_t Entries() const { return entries; }

		/** @brief Commit: every entry closed for this run (the frame anchors do this before the commits). */
		void Close() const;

		/** @brief Commit: a_entry dispatches a_x x a_y x a_z groups this run. */
		void OpenDispatch(std::uint32_t a_entry, std::uint32_t a_x, std::uint32_t a_y = 1, std::uint32_t a_z = 1) const;

		/** @brief Commit: a_entry draws a_vertices vertices (the fullscreen triangle by default) a_instances times this run. */
		void OpenDraw(std::uint32_t a_entry, std::uint32_t a_vertices = 3, std::uint32_t a_instances = 1) const;

		/** @brief Record: a_program's dispatch through a_entry, with a_constants pushed (ComputeProgram::DispatchIndirect). */
		void Dispatch(org::RecordingContext& a_recording, const ComputeProgram& a_program, std::span<const std::uint32_t> a_constants, std::uint32_t a_entry) const;

		/** @brief Record: the latch and offset a RasterProgram::DrawDesc takes for a_entry (indirect, indirectOffset). */
		const org::LatchBlock& Latch() const { return *latch; }
		std::uint32_t Offset(std::uint32_t a_entry) const { return a_entry * kEntryBytes; }

		RunGate(std::shared_ptr<org::LatchBlock> a_latch, std::uint32_t a_entries) :
			latch(std::move(a_latch)), entries(a_entries) {}

	private:
		void Write(std::uint32_t a_entry, const std::array<std::uint32_t, 4>& a_words) const;

		std::shared_ptr<org::LatchBlock> latch;
		std::uint32_t entries = 0;
	};
}

#endif
