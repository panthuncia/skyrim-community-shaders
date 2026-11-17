#pragma once

#include <functional>

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

	/**
	 * @brief Runs a_work at the next frame boundary: in the next OnBegin, before the frame's schedules are chosen, on the thread that
	 * begins the frame. For work that publishes a new version of a slot (a Resize of a versioned texture), which only a frame boundary
	 * may do. Runs a_work now when the driver is not installed. Render owner.
	 */
	void AtNextBegin(std::function<void()> a_work);

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
		StaleRecording,    // the segment's recording bound a version that a slot replaced while it was made; its work that frame is dropped
		HostSkipped,       // a clear or copy waiting for its host epoch saw another epoch submitted first
		HostMissing,       // a clear or copy waiting for its host epoch was still waiting at the next Begin (dropped)
		LostToBuild,       // a build of a segment's graph gave its held ticket back between two of its taps (what they wrote is dropped)
		InstanceNotDeclared,  // a step's hook fired more often in one segment run than the description declared instances of its client
		NoPass,               // an engine effect step fired its data-only hook, and CS has no pass for that step at all
		Refused,              // a client could not draw the engine's work at its hook (a Scaleform display the graph cannot draw), in native mode
		Untappable,           // a step's client is not in the build its stage runs, the stage's schedule is current, and no build of it is on its way
		Starved,              // a staged build's recordings were asked for again and again: it would never switch in
		Count
	};

	/** @brief "not declared", "repeated", ... */
	const char* TapErrorName(TapError a_error);

	/**
	 * @brief The mismatch policy: the one place a mismatch between the frame's schedule and what the engine does is handled (render
	 * owner). a_what names the tap or the client and the difference. a_segment SegmentId::Count: work hosted in another epoch, outside
	 * the declared segments. Every kind of TapError is a mismatch. With MismatchFatal it stops the game with a message that names the
	 * frame, the segment, the tap and the difference. Otherwise it is counted, logged once per kind for the session, and the caller
	 * draws nothing of that work.
	 */
	void NoteTapError(TapError a_error, SegmentId a_segment, std::string_view a_what);

	/** @brief a_count clears or copies a tap handed to host a_host (a static name) were written at its commit (render owner). */
	void NoteHosted(const char* a_host, std::uint32_t a_count);

	/** @brief A segment's end tap fired this frame (render owner). Returns false when it already had: a Repeated tap error. */
	bool NoteTap(SegmentId a_segment, std::string_view a_tap);

	/** @brief A segment's recording was asked for, in frame a_frame (a_reason names what changed). */
	void NoteRecording(SegmentId a_segment, std::uint64_t a_frame, std::string_view a_reason);

	/**
	 * @brief A segment's schedule for frame slot a_slot switched to a new description at this Begin (FrameAnchors::BeginFrame).
	 * a_lag: frames since a slot of the segment began to wait for that description. a_what names what changed. Each switch is logged, at most 16 a
	 * segment in 5 s; the rest are counted and logged once the window passes, so startup does not use up the lines.
	 */
	void NoteSwitch(SegmentId a_segment, std::uint64_t a_frame, std::uint32_t a_slot, std::uint64_t a_lag, std::string_view a_what);

	/** @brief How a declared segment runs in a frame. */
	enum class Scheduled : std::uint8_t
	{
		Current,  // on the schedule of the frame's description
		Lagging,  // on the slot's older schedule, while the recording of the new description is made off the render thread
		Absent,   // no schedule for the slot yet (the first frames, or the frames after a build of its graph): its work is not drawn
		Count
	};

	/** @brief Counts how a declared segment runs this frame (render owner). */
	void NoteScheduled(SegmentId a_segment, Scheduled a_how);

	/** @brief A staged build of the segment's graph switched in whole with its recordings at this Begin (render owner). */
	/** @brief A stage asked for a build because its key changed (absent passes, FrameAnchors::UpdateKey). */
	void NoteKeyBuild(SegmentId a_segment);

	void NoteBuildSwitch(SegmentId a_segment);

	/**
	 * @brief A client or module step had work at its tap that the running schedule leaves out, while the frame's own description has
	 * it on: the schedule lags, the work is not drawn this frame. Counted, not a mismatch (render owner).
	 */
	void NoteLaggedWork(SegmentId a_segment);

	/**
	 * @brief A step's client had work at its tap that the running schedule leaves out (render owner). Lagged work (NoteLaggedWork) when
	 * FrameAnchors::TapMayLag allows it: the client's build or its stage's recording is on its way. Otherwise the work would never be
	 * drawn: TapError::Untappable, named by a_what.
	 */
	void NoteStepLag(SegmentId a_segment, std::string_view a_client, std::string_view a_what);

	/** @brief A staged build of the segment's graph has had a_count of its recordings asked for again so far (the census keeps the highest). */
	void NoteStagedAskedAgain(SegmentId a_segment, std::uint32_t a_count);

	/** @brief A tap of a segment that has no schedule this frame (Scheduled::Absent): it draws nothing (render owner). */
	void NoteAbsentTap(SegmentId a_segment);

	/** @brief A recording was asked for after the frame's Begin: a mid-frame re-record, which the schedule rule forbids (counted, render owner). */
	void NoteLateRequest(SegmentId a_segment);

	/**
	 * @brief Whether a mismatch between the schedule and the engine stops the game (NoteTapError). Development builds: every build but
	 * the Release profile (CS_RELEASE_PROFILE), so Dev-Fast, Perf-Fast and the test stages. CS_ORG_MISMATCH_FATAL=0 or =1 overrides.
	 */
	bool MismatchFatal();

	/** @brief The tap errors so far, all kinds (any thread). */
	std::uint64_t TapErrors();

	/**
	 * @brief The census line (CS_ORG_EPOCH_STATS's report): frames by kind; each segment's taps, recordings asked for, schedule switches
	 * and how its frames ran (current, lagging, absent); the switches' lag; the tap errors by kind, since the last report. "[ORG] declared
	 * frame: ...".
	 */
	void LogStats();
}
