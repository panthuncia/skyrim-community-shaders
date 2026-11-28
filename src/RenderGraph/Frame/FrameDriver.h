#pragma once

#include <functional>

#include "RenderGraph/Frame/FrameDescription.h"

#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

/**
* @brief The frame driver. At each Renderer::Begin it builds the frame's description and keeps the declared frame's tap census.
*
* The frame kind is the adopted frame snapshot's (Snapshot/FrameSnapshot.h): a game update's, decided on the game thread from the
* menu stack (MenuManager SE 79938, AE 82075) and the player's 3D, or a frame-only snapshot's published at Begin for a loading
* screen (the serving thread), a Bink tick (SE 87890, AE 90259) or a main-thread frame outside Main::Update. Never from the
* census flags, which are a frame stale.
* FrameAnchors fills each segment's fields and asks for missing recordings, made while the engine draws the shadow maps.
* A tap the description does not cover, or a segment whose tap never fired, is a tap error, counted and logged.
* OnBegin runs on the beginning thread, inside Renderer::Lock except on native's render thread. The rest is render owner, except the
* counters.
*/
namespace FrameAnchors
{
	enum class Reason : std::uint8_t;
}

namespace RenderGraph::Frame::Driver
{
	/** @brief Installs the driver's hooks (Engine::InstallGameHooks, after the Bink hooks). Logs the switch's state. */
	void Install();

	/** @brief Whether the driver runs. Always with the render graph's game hooks, so the kind is known in both modes. */
	bool Installed();

	/** @brief Renderer::Begin's detour, before the engine's Begin, after the render owner was claimed and the frame clock moved. */
	void OnBegin();

	/**
	* @brief Renderer::Begin's CS part: the render owner's claim, the snapshot's adoption, its resize event, menus and window state, then
	* OnBegin. The detour runs it before the engine's body. Native's render thread runs it alone, without Renderer::Lock, and the
	* engine's body only when it needs it (RenderThread). The other producers park the render thread first, so Begins never overlap.
	*/
	void StartRendererFrame(RE::BSGraphics::Renderer* a_renderer);

	/** @brief The engine's own Renderer::Begin body, without the detour's CS part. Under Renderer::Lock. */
	void EngineBegin(RE::BSGraphics::Renderer* a_renderer, std::uint32_t a_flags);

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
		bool outerNext = false;
	};

	/**
	* @brief Marks the calling thread as one that renders published game snapshots (phase 7's render thread): its Begin adopts the
	* newest published snapshot and publishes no frame-only one, as a Begin inside Main::Update's call of Main::DrawFrame does.
	*/
	class GameSnapshotRenderScope
	{
	public:
		GameSnapshotRenderScope();
		~GameSnapshotRenderScope();
		GameSnapshotRenderScope(const GameSnapshotRenderScope&) = delete;
		GameSnapshotRenderScope& operator=(const GameSnapshotRenderScope&) = delete;

	private:
		bool outer = false;
	};

	/**
	* @brief Whether this thread is in a movie tick that has not reached its Begin yet. The tick decodes and clears the framebuffer
	* and the main depth before its Begin, between frames. That work belongs to the Bink frame the tick's Begin starts, not to
	* Current(), which for a movie's first tick is the frame before it (a world frame for a movie played in game).
	*/
	bool MovieFrameNext();

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
		Unbacked,             // GPU work reached an engine target the engine target set keeps without memory (Engine/Targets/EngineTargetSet.h)
		NotExecuted,          // a skeleton clear or copy run did not execute what it took (nothing draws it another way)
		OwnEpoch,             // an anchor would run an epoch of its own
		MissedSettingsWriter,  // a bound CS::Setting changed with no raise (the missed-writer check)
		OutsideAdoption,       // a version adopted or a build attached outside the frame's adoption point with no named exception (rule R1)
		ClientInputChanged,    // what a client's graphs are built for (main targets, main resources, shadow format) changed after it first drew
		LateChange,            // a registration or structural build request made inside the frame, outside its adoption point (rule R1)
		EngineDraw,            // an engine draw or dispatch reached the D3D11 context in graph mode (the flush latch withheld its state)
		EngineWork,            // an engine clear, copy, map or query that graph mode has no place for reached the D3D11 context
		Uncovered,             // an epoch the recipe has a fragment source cover was not covered while the source draws (cleanup plan A3, step 6)
		SceneMismatch,         // a DCLF scene check found tracked state the scene does not match (a missed detach, a record naming a freed slot)
		DrawInputs,            // a DCLF draw's inputs were incomplete (past its buffers, a frame buffer not filled, a frame texture not resolved)
		EngineLeak,            // the engine drew a main-view pass of an object DCLF's frame set holds
		OutsideLedgers,        // native mode: the immediate context ran work no graph pass or graph resource orders
		UndeclaredAccess,      // development builds: CS handed a shader a raw index or ref of a resource no pass declared
		D3D11Path,             // a ported client ran its D3D11 path in a graph mode
		Count
	};

	/** @brief "not declared", "repeated", ... */
	const char* TapErrorName(TapError a_error);

	/** @brief What went wrong for a_error, as one sentence for its message. */
	const char* TapErrorMeaning(TapError a_error);

	/** @brief "guest mode", "native Vulkan mode", "native D3D12 mode", or "D3D11 mode" outside the graph modes. Any thread. */
	const char* ModeName();

	/**
	* @brief The mismatch policy, the one place a schedule-versus-engine mismatch is handled. Render owner.
	* With MismatchFatal it stops the game with a message. Otherwise it counts every error. The first of each kind logs the full
	* error with a_where and the frame, and raises a violation warning (RaiseViolationWarning). The caller draws nothing.
	*/
	void NoteTapError(TapError a_error, SegmentId a_segment, std::string_view a_what, const std::source_location& a_where = std::source_location::current());

	/** @brief GPU work reached an engine target with no memory. In release builds each target's first use is logged. Any thread. */
	void NoteUnbackedUse(std::string_view a_target, std::string_view a_what, const std::source_location& a_where = std::source_location::current());

	/**
	* @brief A rule violation the game keeps running past, in builds where MismatchFatal is false. The menu shows a warning
	* for each kind until the game exits. Raising a kind again changes nothing. Any thread.
	*/
	void RaiseViolationWarning(std::string_view a_kind);

	/** @brief The violation kinds raised this session, in the order they first happened. Empty in a clean session. Any thread. */
	std::vector<std::string> ViolationWarnings();

	/** @brief a_count clears or copies handed to host a_host were written at its commit. Render owner. */
	void NoteHosted(const char* a_host, std::uint32_t a_count);

	/** @brief A segment's end tap fired this frame. Returns false for a repeat. Render owner. */
	bool NoteTap(SegmentId a_segment, std::string_view a_tap);

	/** @brief A segment's recording was asked for, in frame a_frame (a_reason names what changed). */
	void NoteRecording(SegmentId a_segment, std::uint64_t a_frame, std::string_view a_reason);

	/** @brief A segment's schedule for a_slot switched to a new description at this Begin. a_lag is frames since the slot began to wait.
	*  Logged at most 16 per segment in 5 s, the rest counted. */
	void NoteSwitch(SegmentId a_segment, std::uint64_t a_frame, std::uint32_t a_slot, std::uint64_t a_lag, std::string_view a_what);

	/**
	* @brief Counts how a declared segment runs this frame: Reason None (on the schedule of the frame's description), Behind (on
	* the slot's older schedule while the new one records) or Absent (no schedule for the slot yet). Render owner.
	*/
	void NoteScheduled(SegmentId a_segment, FrameAnchors::Reason a_how);

	/** @brief A stage asked for a build because its key changed. */
	void NoteKeyBuild(SegmentId a_segment);

	void NoteBuildSwitch(SegmentId a_segment);

	/** @brief Work the lagging schedule leaves out but the frame's description has on. Not drawn this frame, counted, not a mismatch. */
	void NoteLaggedWork(SegmentId a_segment);

	/** @brief A client's work the running schedule leaves out. Lagged work when FrameAnchors::Place(client).MayLag(), else TapError::Untappable. Render owner. */
	void NoteStepLag(SegmentId a_segment, std::string_view a_client, std::string_view a_what);

	/** @brief A tap of a segment with no schedule this frame. It draws nothing. Render owner. */
	void NoteAbsentTap(SegmentId a_segment);

	/** @brief A recording asked for after Begin, a forbidden mid-frame re-record. Counted. Render owner. */
	void NoteLateRequest(SegmentId a_segment);

	/** @brief Whether a mismatch stops the game. True in every build but the Release profile. CS_ORG_MISMATCH_FATAL overrides. */
	bool MismatchFatal();
}
