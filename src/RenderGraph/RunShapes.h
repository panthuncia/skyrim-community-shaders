#pragma once

#include <cstdint>

/**
 * @brief The frame anchors' run shapes (native frame graph plan, Phase 0), with CS_ORG_EPOCH_STATS=1: for each anchor, how many runs
 * a frame it submits (peak and median over the frames it ran) and whether its runs within one frame touch different resources.
 *
 * A run's shape is a hash of what decides the resources it uses: the run gate entries its clients' commits opened (each entry is a
 * pass with fixed declarations), the point's engine targets (FrameAnchors::Point input and output) and the set of active clients. Two
 * runs of a frame with different hashes ran different passes or targets: one frame graph has to give such an anchor one pass per
 * shape, or a pass whose declarations change per run.
 *
 * Run order: for an anchor that runs more than once in a frame, the frame's run shapes in order are hashed into its sequence. The
 * "[ORG] run order" line gives the frames with more than one run, how many distinct sequences they had, and the most common one's
 * run count and share. One sequence in nearly every frame means static gated passes in a fixed order can match the engine's order.
 *
 * Reported per window (CensusWindow) every CS_ORG_CENSUS_FRAMES frames in it and when the game leaves it, as "[ORG] run shapes" and
 * "[ORG] run order" lines.
 * FrameAnchors::LogRunStats's peak line stays as it was.
 */
namespace RenderGraph::RunShapes
{
	/** @brief Whether CS_ORG_EPOCH_STATS=1 asks for the run shapes. Any thread. */
	bool On();

	/** @brief An anchor run starts (FrameAnchors::Run, before its commits). Render thread. */
	void BeginRun();

	/** @brief A commit opened a_entry of a_gate in the run that is open (RunGate::OpenDispatch, OpenDraw). Render thread. */
	void NoteOpen(const void* a_gate, std::uint32_t a_entry);

	/**
	 * @brief The open run was submitted: anchor a_anchor (its index, named a_name, a static string), with the point's targets and a
	 * hash of its active clients. Render thread.
	 */
	void EndRun(std::uint32_t a_anchor, const char* a_name, std::int32_t a_input, std::int32_t a_output, std::uint64_t a_clients);

	/** @brief The frame's end (RenderGraphRuntime::SealPresented): closes each anchor's frame and reports a window when due. */
	void EndFrame();
}
