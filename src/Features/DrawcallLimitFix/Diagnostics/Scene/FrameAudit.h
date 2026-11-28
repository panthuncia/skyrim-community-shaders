#pragma once

#include <string>

namespace DCLF::Diagnostics
{
	/**
	 * @brief Render thread, diagnostics. Checks the installed tables and the frame's placement rows against the engine now, for every
	 * object: its geometry's buffers and its world transform. Returns one line of counts and the first mismatches. cs-test's shots log it.
	 */
	std::string AuditFrameRows();
}
