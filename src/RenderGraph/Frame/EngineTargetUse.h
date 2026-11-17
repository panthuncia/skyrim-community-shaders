#pragma once

#include <cstdint>
#include <string_view>

/**
 * @brief Which engine render targets native mode gives memory (docs/development/org-native-frame-graph.md, "Unbacked engine targets").
 *
 * Native mode runs every frame kind as CS-owned stages, so CS knows which engine targets any pass touches. The table in
 * EngineTargetUse.cpp names each target that no stage, no pass and no still-reachable old native path uses, with the reason
 * (usually the port that draws into its own image instead). Engine::Targets makes those as records with no image and no memory
 * (Engine::Native::MakeUnbackedTexture2D). Every target not in the table is backed. Guest mode backs every target (DXVK owns them).
 *
 * GPU work that reaches an unbacked target is a recipe mismatch (Frame::Driver, TapError::Unbacked): fatal in development builds. In
 * release the operation is skipped and the first one is logged. The engine may still hold the pointer and read the description.
 */
namespace RenderGraph::Frame::EngineTargetUse
{
	/** @brief Whether render target a_target (RE::RENDER_TARGET) has no memory in this session: native mode and listed. */
	bool ColourUnbacked(std::uint32_t a_target);

	/** @brief Whether the engine's own clears of render target a_target are declared dropped (an unbacked target nothing reads). */
	bool ColourClearsDropped(std::uint32_t a_target);

	/** @brief Whether the engine's 3D target named a_label (Create3DRenderTarget's name) has no memory in this session. */
	bool VolumeUnbacked(std::string_view a_label);

	/** @brief Why the target has no memory: its table entry's users line ("" when it is backed). */
	std::string_view ColourReason(std::uint32_t a_target);
	std::string_view VolumeReason(std::string_view a_label);

	/** @brief Logs the table once (native mode): which targets have no memory, and why. */
	void LogOnce();
}
