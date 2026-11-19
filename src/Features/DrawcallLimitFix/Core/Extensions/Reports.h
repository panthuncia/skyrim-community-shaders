#pragma once

// Core's side of Api/Reports.h.

#include "Features/DrawcallLimitFix/Api/Reports.h"

#include <cstdint>
#include <string>

namespace DCLF::Extensions
{
	/** @brief Render thread. Every registered feature's logged lines at a_point, in order. */
	void ReportLinesAt(Api::ReportPoint a_point, std::uint32_t a_frame);
	/**
	 * @brief Render thread. Appends every registered feature's text lines at a_point, in order.
	 * The coordinator calls coordinator components' lines itself, with their context.
	 */
	void ReportTextAt(Api::ReportPoint a_point, std::string& a_text);
}
