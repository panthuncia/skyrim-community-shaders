#pragma once

#include <functional>

#include "RenderGraph/Frame/FrameDescription.h"

#include <cstdint>
#include <string_view>

/**
* @brief The frame driver. At each Renderer::Begin it builds the frame's description and keeps the declared frame's tap census.
*
* The frame kind comes from the beginning thread, the menu stack (MenuManager SE 79938, AE 82075), the player's 3D, the loading
* screen (SE 34552, AE 35407) and the Bink tick (SE 87890, AE 90259). Never from the census flags, which are a frame stale.
* FrameAnchors fills each segment's fields and asks for missing recordings, made while the engine draws the shadow maps.
* A tap the description does not cover, or a segment whose tap never fired, is a tap error, counted and logged.
* OnBegin runs on the beginning thread inside Renderer::Lock. The rest is render owner, except the counters.
*/
namespace RenderGraph::Frame::Driver
{
	/** @brief Installs the driver's hooks (Engine::InstallGameHooks, after the Bink hooks). Logs the switch's state. */
	void Install();

	/** @brief Whether the driver runs. Always with the render graph's game hooks, so the kind is known in both modes. */
	bool Installed();

	/** @brief Renderer::Begin's detour, before the engine's Begin, after the render owner was claimed. */
	void OnBegin();

	/** @brief ServingThread::SetLoadingScreenActive's detour: a_active before the loop starts, false once it ended. */
	void OnLoadingScreen(bool a_active);

	/** @brief Whether a loading screen started this frame. The engine then drops the rest of the frame's world work, so waits on it are not tap errors. */
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
		StaleRecording, // the recording bound a version a slot replaced while it was made. Its work that frame is dropped
		HostSkipped,       // a clear or copy waiting for its host epoch saw another epoch submitted first
		HostMissing,       // a clear or copy waiting for its host epoch was still waiting at the next Begin (dropped)
		LostToBuild, // a build of the segment's graph took back its held ticket between two taps. What they wrote is dropped
		InstanceNotDeclared,  // a step's hook fired more often in one segment run than the description declared instances of its client
		NoPass,               // an engine effect step fired its data-only hook, and CS has no pass for that step at all
		Refused,              // a client could not draw the engine's work at its hook (a Scaleform display the graph cannot draw), in native mode
		Untappable, // a step's client is not in the build its stage runs, and no build with it is coming
		Starved,              // a staged build's recordings were asked for again and again: it would never switch in
		Unbacked,             // GPU work reached an engine target native mode keeps without memory (Frame/EngineTargetUse.h)
		NotExecuted,          // a skeleton clear or copy run did not execute what it took (nothing draws it another way)
		OwnEpoch,             // an anchor would run an epoch of its own
		MissedSettingsWriter,  // a feature's size settings changed with no settings event (the missed-writer check)
		OutsideAdoption,       // a version adopted or a build attached outside the frame's adoption point with no named exception (rule R1)
		DclfInputChanged,      // what DCLF's graphs are built for (main targets, main resources, shadow format) changed after it first drew
		Count
	};

	/** @brief "not declared", "repeated", ... */
	const char* TapErrorName(TapError a_error);

	/** @brief The mismatch policy, the one place a schedule-versus-engine mismatch is handled. Render owner.
	*  With MismatchFatal it stops the game with a message. Otherwise it counts, logs once per kind, and the caller draws nothing. */
	void NoteTapError(TapError a_error, SegmentId a_segment, std::string_view a_what);

	/** @brief GPU work reached an engine target with no memory. In release builds each target's first use is logged. Any thread. */
	void NoteUnbackedUse(std::string_view a_target, std::string_view a_what);

	/** @brief a_count clears or copies handed to host a_host were written at its commit. Render owner. */
	void NoteHosted(const char* a_host, std::uint32_t a_count);

	/** @brief A segment's end tap fired this frame. Returns false for a repeat. Render owner. */
	bool NoteTap(SegmentId a_segment, std::string_view a_tap);

	/** @brief A segment's recording was asked for, in frame a_frame (a_reason names what changed). */
	void NoteRecording(SegmentId a_segment, std::uint64_t a_frame, std::string_view a_reason);

	/** @brief A segment's schedule for a_slot switched to a new description at this Begin. a_lag is frames since the slot began to wait.
	*  Logged at most 16 per segment in 5 s, the rest counted. */
	void NoteSwitch(SegmentId a_segment, std::uint64_t a_frame, std::uint32_t a_slot, std::uint64_t a_lag, std::string_view a_what);

	/** @brief How a declared segment runs in a frame. */
	enum class Scheduled : std::uint8_t
	{
		Current,  // on the schedule of the frame's description
		Lagging, // on the slot's older schedule, while the new description records off the render thread
		Absent, // no schedule for the slot yet, so its work is not drawn
		Count
	};

	/** @brief Counts how a declared segment runs this frame. Render owner. */
	void NoteScheduled(SegmentId a_segment, Scheduled a_how);

	/** @brief A stage asked for a build because its key changed. */
	void NoteKeyBuild(SegmentId a_segment);

	void NoteBuildSwitch(SegmentId a_segment);

	/** @brief Work the lagging schedule leaves out but the frame's description has on. Not drawn this frame, counted, not a mismatch. */
	void NoteLaggedWork(SegmentId a_segment);

	/** @brief A client's work the running schedule leaves out. Lagged work when TapMayLag allows, else TapError::Untappable. Render owner. */
	void NoteStepLag(SegmentId a_segment, std::string_view a_client, std::string_view a_what);

	/** @brief A tap of a segment with no schedule this frame. It draws nothing. Render owner. */
	void NoteAbsentTap(SegmentId a_segment);

	/** @brief A recording asked for after Begin, a forbidden mid-frame re-record. Counted. Render owner. */
	void NoteLateRequest(SegmentId a_segment);

	/** @brief Whether a mismatch stops the game. True in every build but the Release profile. CS_ORG_MISMATCH_FATAL overrides. */
	bool MismatchFatal();

	/** @brief Logs the declared frame census line for CS_ORG_EPOCH_STATS. */
	void LogStats();
}
