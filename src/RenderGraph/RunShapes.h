#pragma once

#include <cstdint>

/**
 * @brief With CS_ORG_EPOCH_STATS=1, reports how many runs a frame each frame anchor submits and whether they use different resources.
 *
 * A run's shape hashes what decides its resources: the gate entries opened, the point's engine targets and the active clients.
 * Different shapes in one frame mean a frame graph needs one pass per shape for that anchor.
 * The "[ORG] run order" line shows whether the order of shapes within a frame is stable.
 * Reported per census window as "[ORG] run shapes" and "[ORG] run order" lines.
 */
namespace RenderGraph::RunShapes
{
	/** @brief Whether CS_ORG_EPOCH_STATS=1 asks for the run shapes. Any thread. */
	bool On();

	/** @brief An anchor run starts, before its commits. Render thread. */
	void BeginRun();

	/** @brief A commit opened a_entry of a_gate in the open run. Render thread. */
	void NoteOpen(const void* a_gate, std::uint32_t a_entry);

	/** @brief The open run was submitted. a_name is a static string. Render thread. */
	void EndRun(std::uint32_t a_anchor, const char* a_name, std::int32_t a_input, std::int32_t a_output, std::uint64_t a_clients);

	/** @brief Closes each anchor's frame at the frame's end and reports a window when due. */
	void EndFrame();
}
