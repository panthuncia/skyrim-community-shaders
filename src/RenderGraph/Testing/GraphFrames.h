#pragma once

#include <cstdint>
#include <string_view>

/**
* @brief Per-client frame counts for test runs: the frames a render graph client's work ran on the graph. The test exit writes
* them to the status file's "report.graph_frames", which cs-test's expect.graph_frames checks. Compiled out of Release builds (CS_RELEASE_PROFILE), where these do nothing.
*/
namespace RenderGraph::TestSupport::GraphFrames
{
	/** @brief One client id's counts. Made once per id and never freed, so a registration keeps a pointer to it. */
	struct Counts;

#if defined(CS_RELEASE_PROFILE)
	inline Counts* CountsOf(std::string_view) { return nullptr; }
	inline void NoteGraph(Counts*, std::uint64_t) {}
	inline void Report() {}
#else
	/** @brief The counts of client a_client, made on first use. Any thread. FrameAnchors keeps it with each registration. */
	Counts* CountsOf(std::string_view a_client);

	/** @brief Render owner. The client's work committed in a stage of frame a_frame (FrameAnchors' taps). Counted once a frame. Null does nothing. */
	void NoteGraph(Counts* a_counts, std::uint64_t a_frame);

	/** @brief Logs one "[ORG] graph frames:" line and writes the counts to the test status file's report. The test exit calls it. */
	void Report();
#endif
}
