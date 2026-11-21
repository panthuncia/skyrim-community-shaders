#pragma once

#include <Render/RunGate.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace org
{
	class RecordingContext;
}

namespace org::services
{
	struct ComputeProgram;
}
using ComputeProgram = org::services::ComputeProgram;

namespace RenderGraph
{
	/**
	 * @brief CS's run gate: org::RunGate with the host's current run and ComputeProgram dispatches.
	 *
	 * The frame anchors close every gate of a client at the start of each run, so work only happens through entries the
	 * client's commit opened. Open and Close run on the render thread, record helpers on the host's thread.
	 */
	class RunGate : public org::RunGate
	{
	public:
		using org::RunGate::RunGate;

		/** @brief A closed gate. Render thread, with the runtime active. Never null: no host or no entries is fatal. */
		static std::shared_ptr<RunGate> Create(std::string_view a_name, std::uint32_t a_entries = 1);

		using org::RunGate::Close;
		using org::RunGate::OpenDispatch;
		using org::RunGate::OpenDraw;

		/** @brief Closes every entry for the current run. The frame anchors do this before the commits. */
		void Close() const { Close(Current()); }

		/** @brief a_entry dispatches a_x x a_y x a_z groups in the current run. */
		void OpenDispatch(std::uint32_t a_entry, std::uint32_t a_x, std::uint32_t a_y = 1, std::uint32_t a_z = 1) const
		{
			OpenDispatch(Current(), a_entry, a_x, a_y, a_z);
		}

		/** @brief a_entry draws a_vertices vertices a_instances times in the current run. The default is the fullscreen triangle. */
		void OpenDraw(std::uint32_t a_entry, std::uint32_t a_vertices = 3, std::uint32_t a_instances = 1) const
		{
			OpenDraw(Current(), a_entry, a_vertices, a_instances);
		}

		/** @brief The host's current run, which is a commit's own. */
		static org::RunRef Current();

		/** @brief Records a_program's dispatch through a_entry. */
		void Dispatch(org::RecordingContext& a_recording, const ComputeProgram& a_program, std::span<const std::uint32_t> a_constants, std::uint32_t a_entry) const;
	};

	/** @brief org::FirstRun, opened in the host's current run. Render thread. */
	class FirstRun : public org::FirstRun
	{
	public:
		using org::FirstRun::Open;

		/** @brief In the commit: opens a_entry with a_x x a_y x a_z groups while the work is pending. A copy or clear takes one group. */
		void Open(const RunGate& a_gate, std::uint32_t a_entry, std::uint32_t a_x = 1, std::uint32_t a_y = 1, std::uint32_t a_z = 1)
		{
			// The current run is read only while the work is pending. Open ignores the run otherwise.
			Open(a_gate, Pending() ? RunGate::Current() : org::RunRef{}, a_entry, a_x, a_y, a_z);
		}
	};
}
