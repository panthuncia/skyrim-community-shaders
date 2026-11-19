#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DCLF::Diagnostics::RefTrace
{
	/**
	 * @brief CS_DCLF_REF_TRACE. DCLF stages record the buffer view refs each load and store site uses into a host-visible table.
	 * Makes the table once the graph is up. Render thread, each frame.
	 */
	void Ensure();
	/** @brief The table's view slot, the value of the stages' DEVICE_REF_TRACE define, or 0 while there is none. Any thread. */
	std::uint32_t Slot();
	/** @brief Notes that a stage asked for the define. a_traced is false when the table was not made yet. Any thread. */
	void NoteRequest(bool a_traced);
	/** @brief Logs the table's new and grown entries once a second. Render thread. */
	void Report();
	/** @brief A compute program's trace defines, empty when off. a_stage tells its sites from a graphics stage's. */
	std::vector<std::pair<std::wstring, std::wstring>> ComputeDefines(std::uint32_t a_stage);
	/** @brief Logs a_text under a_what when it changed, for the first 16 changes. Any thread, with the trace on. */
	void NoteChanged(std::string_view a_what, std::string a_text);
}
