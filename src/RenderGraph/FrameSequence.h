#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/**
 * @brief With CS_ORG_FRAME_SEQUENCE=N, logs one frame's ordered anchor runs and GPU submission points every N frames.
 *
 * N=1 means every 600. It shows which work runs between which DCLF epochs and what splits a held execution.
 * Each frame's sequence is hashed, and the report counts the distinct sequences a window saw. Render thread, except On.
 */
namespace RenderGraph::FrameSequence
{
	/** @brief Whether the census is on. Any thread. */
	bool On();

	/** @brief Staged into its stage's ticket, the stage's commit, or a ticket of its own. */
	enum class RunKind : std::uint8_t
	{
		Held,
		Commit,
		Own,
	};

	/** @brief An anchor run. A frame with its own ticket is logged regardless of N, up to 32 of them. */
	void NoteRun(const char* a_anchor, RunKind a_kind, const std::string& a_clients, std::int32_t a_input, std::int32_t a_output);

	void NoteEpoch(const char* a_label);

	/** @brief Other GPU work or a boundary, such as the present or a GPU wait. */
	void NoteBoundary(std::string_view a_what);

	/** @brief Engine work no stage had a place for. Logged like an own ticket. */
	void NoteUnplaced(std::string_view a_what);

	void EndFrame();
}
