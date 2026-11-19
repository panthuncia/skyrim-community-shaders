#pragma once

#include <cstdint>
#include <string_view>

/**
 * @brief Which engine render targets native mode gives memory. See org-native-frame-graph.md, "Unbacked engine targets".
 *
 * The table in EngineTargetUse.cpp names each target no stage or pass uses, with the reason. Those are made with no memory.
 * GPU work that reaches one is a recipe mismatch, fatal in development builds and skipped in release.
 * The engine may still hold the pointer and read the description. Guest mode backs every target.
 */
namespace RenderGraph::Frame::EngineTargetUse
{
	/** @brief Whether render target a_target has no memory in this session. */
	bool ColourUnbacked(std::uint32_t a_target);

	/** @brief Whether the engine's own clears of a_target are dropped, for an unbacked target nothing reads. */
	bool ColourClearsDropped(std::uint32_t a_target);

	/** @brief Whether the engine's 3D target named a_label has no memory in this session. */
	bool VolumeUnbacked(std::string_view a_label);

	/** @brief Why the target has no memory, "" when it is backed. */
	std::string_view ColourReason(std::uint32_t a_target);
	std::string_view VolumeReason(std::string_view a_label);

	/** @brief Logs the table once in native mode. */
	void LogOnce();
}
