#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <Render/RenderGraph/ScheduleTrace.h>

#	include <cstdint>
#	include <memory>

/**
 * @brief The menu's "Render graph schedule" window and the DevBench tool communityshaders.schedule.
 * The graph traces its schedule while either wants it. See docs/development/render-graph-schedule.md.
 */
namespace RenderGraphSchedule
{
	enum class Requester : std::uint8_t
	{
		Window,
		DevBench,
	};

	/** @brief The graph traces while any requester wants it. Any thread. */
	void SetTracing(Requester a_requester, bool a_enabled);
	/** @brief False without a graph. Any thread. */
	bool TracingEnabled();

	/** @brief Null without a graph, while tracing is off, or before a traced frame. Any thread. */
	std::shared_ptr<const org::PersistentScheduleSnapshot> Snapshot();

	/** @brief Opens the window, which starts tracing. UI thread. */
	void OpenWindow();
	/** @brief Draws the window while it is open. UI thread, inside an ImGui frame. */
	void DrawWindow();
}

#endif
