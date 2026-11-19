#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <array>
#	include <atomic>
#	include <cstdint>
#	include <memory>
#	include <span>
#	include <string_view>

namespace org
{
	class LatchBlock;
	class RecordingContext;
	struct RunRef;
}

struct ComputeProgram;

namespace RenderGraph
{
	/**
	 * @brief A per-run on/off switch for a client's dispatches and draws that keeps their recording.
	 *
	 * The pass records an indirect dispatch or draw whose counts live in a latch block, so the recording stays current whatever
	 * the gate says. The frame anchors close every gate of a client at the start of each run, so work only happens through
	 * entries the client's commit opened. Open and Close run on the render thread, record helpers on the host's thread.
	 */
	class RunGate
	{
	public:
		/** @brief Three group counts and a pad, or four draw words. */
		static constexpr std::uint32_t kEntryBytes = 16;

		/** @brief A closed gate. Render thread, with the runtime active. Null and logged without a host. */
		static std::shared_ptr<RunGate> Create(std::string_view a_name, std::uint32_t a_entries = 1);

		std::uint32_t Entries() const { return entries; }

		/** @brief Closes every entry for this run. The frame anchors do this before the commits. */
		void Close() const;

		/** @brief a_entry dispatches a_x x a_y x a_z groups this run. */
		void OpenDispatch(std::uint32_t a_entry, std::uint32_t a_x, std::uint32_t a_y = 1, std::uint32_t a_z = 1) const;

		/** @brief a_entry draws a_vertices vertices a_instances times this run. The default is the fullscreen triangle. */
		void OpenDraw(std::uint32_t a_entry, std::uint32_t a_vertices = 3, std::uint32_t a_instances = 1) const;

		// The same for a named run. A segment's end point writes what its taps gathered into the run that submits it.

		void Close(const org::RunRef& a_run) const;

		void OpenDispatch(const org::RunRef& a_run, std::uint32_t a_entry, std::uint32_t a_x, std::uint32_t a_y = 1, std::uint32_t a_z = 1) const;

		void OpenDraw(const org::RunRef& a_run, std::uint32_t a_entry, std::uint32_t a_vertices = 3, std::uint32_t a_instances = 1) const;

		/** @brief Copies the gate's record of which entries a_from has open onto a_to. The latch words travel with the run on their own. */
		void CopyOpen(const org::RunRef& a_from, const org::RunRef& a_to) const;

		/** @brief CopyOpen for every live gate, listed by its client or not. Render thread. */
		static void CopyOpenAll(const org::RunRef& a_from, const org::RunRef& a_to);

		/** @brief Clears every live gate's record of a_run, not its latch words. Render thread. */
		static void ClearOpenAll(const org::RunRef& a_run);

		/** @brief The host's current run, which is a commit's own. */
		static org::RunRef Current();

		/**
		 * @brief Whether the commit opened a_entry for a_run with work to do. Only for passes recorded at execution.
		 * True when the gate cannot tell, so a caller that skips on false never drops work.
		 */
		bool IsOpen(const org::RunRef& a_run, std::uint32_t a_entry) const;

		/** @brief Records a_program's dispatch through a_entry. */
		void Dispatch(org::RecordingContext& a_recording, const ComputeProgram& a_program, std::span<const std::uint32_t> a_constants, std::uint32_t a_entry) const;

		/** @brief The latch and offset a RasterProgram::DrawDesc takes for a_entry. */
		const org::LatchBlock& Latch() const { return *latch; }
		std::uint32_t Offset(std::uint32_t a_entry) const { return a_entry * kEntryBytes; }

		RunGate(std::shared_ptr<org::LatchBlock> a_latch, std::uint32_t a_entries);
		~RunGate();
		RunGate(const RunGate&) = delete;
		RunGate& operator=(const RunGate&) = delete;

	private:
		/** @brief a_work says whether the words make the dispatch or draw do something. */
		void Write(const org::RunRef& a_run, std::uint32_t a_entry, const std::array<std::uint32_t, 4>& a_words, bool a_work) const;

		std::shared_ptr<org::LatchBlock> latch;
		std::uint32_t entries = 0;
		// A bit per entry the current run opened with work, one row per lease index.
		// Close clears a row at each run's start, and the run is over before the lease index is reused.
		std::uint32_t words = 0;
		std::unique_ptr<std::atomic<std::uint64_t>[]> open;
	};
}

#endif
