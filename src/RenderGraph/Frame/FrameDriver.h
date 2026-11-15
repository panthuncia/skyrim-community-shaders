#pragma once

#include "RenderGraph/Frame/FrameDescription.h"

#include <cstdint>
#include <string_view>

/**
 * @brief The frame driver: at each Renderer::Begin it builds the frame's description (FrameDescription.h) from state CS reads
 * itself, and it keeps the declared frame's tap census. docs/development/org-native-frame-graph.md, "The declared frame".
 *
 * - **Kind.** Decided at Begin from the thread that begins the frame, the menu stack (the engine's own freeze-background test,
 *   MenuManager SE 79938 / AE 82075), the player's 3D, the loading screen's start and end (ServingThread::SetLoadingScreenActive,
 *   SE 34552 / AE 35407) and the Bink player's tick (BSMoviePlayer::Tick, SE 87890 / AE 90259). Never from the census flags, which
 *   are written after the present and so are one frame stale.
 * - **Segments.** FrameAnchors fills each declared segment's fields (FrameAnchors::BeginFrame) and asks for the recording of any
 *   description it has none for, so the host's thread records it while the engine draws the shadow maps.
 * - **Taps.** A segment's end tap notes itself here. A tap the description does not cover, or a declared segment whose tap never
 *   fired by the next Begin, is a tap error: counted, and logged with "[ORG] frame tap error" (rate limited). Nothing else happens.
 *
 * Threads: OnBegin on the thread that begins the frame (inside Renderer::Lock). The rest on the render owner, but the counters,
 * which any thread may read.
 */
namespace RenderGraph::Frame::Driver
{
	/** @brief Installs the driver's hooks (Engine::InstallGameHooks, after the Bink hooks). Logs the switch's state. */
	void Install();

	/** @brief Whether the driver runs: always with the render graph's game hooks, so the kind is known in both modes. */
	bool Installed();

	/** @brief Renderer::Begin's detour, before the engine's Begin, after the render owner was claimed. */
	void OnBegin();

	/** @brief ServingThread::SetLoadingScreenActive's detour: a_active before the loop starts, false once it ended. */
	void OnLoadingScreen(bool a_active);

	/**
	 * @brief Whether a loading screen started during the current frame (OnLoadingScreen). The engine then leaves the rest of the frame's
	 * world work undone: work waiting for an epoch of that world work is abandoned with it, not a tap error. Any thread.
	 */
	bool LoadStartedThisFrame();

	/** @brief Marks the calling thread as inside BSMoviePlayer::Tick: a Begin from there begins a Bink frame. */
	class MovieTickScope
	{
	public:
		MovieTickScope();
		~MovieTickScope();
		MovieTickScope(const MovieTickScope&) = delete;
		MovieTickScope& operator=(const MovieTickScope&) = delete;

	private:
		bool outer = false;
	};

	/** @brief The current frame's description (render owner). Before the first Begin, a main-menu frame with nothing declared. */
	const Description& Current();

	/** @brief The tap errors, by what went wrong. */
	enum class TapError : std::uint8_t
	{
		NotDeclared,       // a segment's tap fired in a frame whose description does not contain the segment
		Repeated,          // a segment's tap fired a second time in one frame
		NotInDescription,  // a client had work at the tap, but the description turned its passes off
		Missing,           // the description declared a segment, and its tap did not fire by the next Begin
		SlotMismatch,      // the segment's run was not the frame slot's published copy
		NoRecording,       // the segment had no recording for its description at its tap
		HostSkipped,       // a clear or copy waiting for its host epoch saw another epoch submitted first
		HostMissing,       // a clear or copy waiting for its host epoch was still waiting at the next Begin (dropped)
		LostToBuild,       // a build of a segment's graph gave its held ticket back between two of its taps (what they wrote is dropped)
		InstanceNotDeclared,  // a step's hook fired more often in one segment run than the description declared instances of its client
		PostNotDeclared,      // the world post chain drew other ImageSpace effects than its post description declared (PostChain.h)
		Count
	};

	/** @brief "not declared", "repeated", ... */
	const char* TapErrorName(TapError a_error);

	/**
	 * @brief Counts and logs a tap error (render owner). a_what names the tap or the client. a_segment SegmentId::Count: work hosted
	 * in another epoch, outside the declared segments.
	 */
	void NoteTapError(TapError a_error, SegmentId a_segment, std::string_view a_what);

	/** @brief a_count clears or copies a tap handed to host a_host (a static name) were written at its commit (render owner). */
	void NoteHosted(const char* a_host, std::uint32_t a_count);

	/** @brief A segment's end tap fired this frame (render owner). Returns false when it already had: a Repeated tap error. */
	bool NoteTap(SegmentId a_segment, std::string_view a_tap);

	/** @brief A segment's recording was asked for, in frame a_frame (a_reason names what changed). */
	void NoteRecording(SegmentId a_segment, std::uint64_t a_frame, std::string_view a_reason);

	/** @brief A segment's tap waited a_waitUs for its recording. */
	void NoteRecordingWait(SegmentId a_segment, double a_waitUs);

	/** @brief The tap errors so far, all kinds (any thread). */
	std::uint64_t TapErrors();

	/**
	 * @brief The census line (CS_ORG_EPOCH_STATS's report): frames by kind, each segment's taps and recordings, and the tap errors
	 * by kind, since the last report. "[ORG] declared frame: ...".
	 */
	void LogStats();
}
