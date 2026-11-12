#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DCLF::Diagnostics::RefTrace
{
	/**
	 * @brief CS_DCLF_REF_TRACE: the DCLF stages record the buffer view refs each load and store site uses (Common/DeviceRef.hlsli,
	 * DEVICE_REF_TRACE) into a host-visible table, which Report logs. Render thread, each frame: makes the table once the graph is up.
	 */
	void Ensure();
	/** @brief Any thread: the table's view slot, the value of the stages' DEVICE_REF_TRACE define; 0 while there is none. */
	std::uint32_t Slot();
	/** @brief Any thread: a stage request asked for the define (a_traced: it got it, else the table was not made yet). */
	void NoteRequest(bool a_traced);
	/** @brief Render thread, each frame: logs the table's new and grown entries once a second. */
	void Report();
	/**
	 * @brief A compute program's defines for the trace (empty with the trace off or before the table): DEVICE_REF_TRACE and
	 * DEVICE_REF_TRACE_STAGE a_stage (2 BuildDraws, 3 the texture transforms), which tells its sites from a graphics stage's.
	 */
	std::vector<std::pair<std::wstring, std::wstring>> ComputeDefines(std::uint32_t a_stage);
	/**
	 * @brief Any thread, with the trace on: logs a_text under a_what when it differs from the last a_what logged, the first 16 times
	 * a_what changes (the frame record's backings, which buffer each slot is).
	 */
	void NoteChanged(std::string_view a_what, std::string a_text);
}
