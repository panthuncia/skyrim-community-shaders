#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/**
 * @brief The frame sequence census (CS_ORG_FRAME_SEQUENCE=N, off by default): the ordered list of a frame's anchor runs and GPU
 * submission points, logged for one frame every N frames (N=1 means every 600).
 *
 * It is for the phase plan: which work runs between which DCLF epochs, how often each anchor repeats in a phase, and what splits a
 * held execution. Each frame's sequence of anchors and epochs is hashed, and the report says how many distinct sequences the window
 * saw.
 *
 * Lines: "[ORG] frame sequence (<window>, frame F, S distinct sequences in W frames, T own tickets in the session):" and then one line
 * per event, in order.
 * Render thread, but On (any thread).
 */
namespace RenderGraph::FrameSequence
{
	/** @brief Whether the census is on. Any thread. */
	bool On();

	/** @brief How an anchor run was taken: staged into its stage's ticket, the stage's commit (its end tap), or a ticket of its own. */
	enum class RunKind : std::uint8_t
	{
		Held,
		Commit,
		Own,
	};

	/**
	 * @brief An anchor run: its name, how it was taken, its active clients' ids (comma separated) and the point's engine targets. A frame
	 * with an own ticket is logged whatever N says (the first 32 of them), and the session's own tickets are counted.
	 */
	void NoteRun(const char* a_anchor, RunKind a_kind, const std::string& a_clients, std::int32_t a_input, std::int32_t a_output);

	/** @brief An epoch was submitted (its label). */
	void NoteEpoch(const char* a_label);

	/** @brief Other GPU work or a boundary: the present, external work, the native context, a GPU wait, a build point. */
	void NoteBoundary(std::string_view a_what);

	/** @brief Engine work no stage had a place for (a mismatch): logged like an own ticket, its frame printed. */
	void NoteUnplaced(std::string_view a_what);

	/** @brief The frame's end (RenderGraphRuntime::SealPresented). */
	void EndFrame();
}
