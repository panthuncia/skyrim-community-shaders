#pragma once

#include <cstdint>

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
}
