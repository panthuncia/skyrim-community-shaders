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
 * Lines: "[ORG] frame sequence (<window>, frame F, S distinct sequences in W frames):" and then one line per event, in order.
 * Render thread, but On (any thread).
 */
namespace RenderGraph::FrameSequence
{
	/** @brief Whether the census is on. Any thread. */
	bool On();

	/**
	 * @brief An anchor run: its name, whether it joined a held region execution, its active clients' ids (comma separated) and the
	 * point's engine targets.
	 */
	void NoteRun(const char* a_anchor, bool a_held, const std::string& a_clients, std::int32_t a_input, std::int32_t a_output);

	/** @brief An epoch was submitted (its label). */
	void NoteEpoch(const char* a_label);

	/** @brief Other GPU work or a boundary: the present, external work, the native context, a GPU wait, a build point. */
	void NoteBoundary(std::string_view a_what);

	/** @brief The frame's end (RenderGraphRuntime::SealPresented). */
	void EndFrame();
}
