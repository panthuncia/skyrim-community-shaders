#pragma once

#include <string_view>

namespace org
{
	class GraphHost;
}

namespace RenderGraph
{
	/**
	 * @brief The one fatal call of the render graph and its ports. Graph mode has no D3D11 fallback, so a failure stops the game here.
	 *
	 * Logs "[ORG] fatal: <area>: <why>" as critical (tests match "fatal: <area>"), flushes the log, and fails through stl::report_and_fail, which shows
	 * "Community Shaders: <area>: <why>". Release keeps a soft path only for tap errors (Frame::Driver::MismatchFatal).
	 * Any thread.
	 * @param a_area What failed, as the logs name it, such as "Volumetric Shadows" or "frame revision".
	 * @param a_why What went wrong.
	 */
	[[noreturn]] void Fatal(std::string_view a_area, std::string_view a_why);

	/** @brief The render graph's host, at a port's setup. Fatal, naming a_area, when the graph does not run. */
	org::GraphHost& RequireGraphHost(std::string_view a_area);
}
