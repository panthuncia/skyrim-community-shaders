#pragma once

#include "Snapshot/FrameSnapshot.h"

#include <cstdint>
#include <memory>
#include <source_location>

/**
* @brief Publication and adoption of frame snapshots (contract: Snapshot/FrameSnapshot.h).
*
* Game side (main thread): the publication hook builds the next snapshot and publishes it, at Main::Update's call of
* Main::DrawFrame (SE 35565 +0x5D2, AE 36564 +0xA97). A render frame outside that call publishes a frame-only snapshot with CaptureFrame() at
* its Begin: a loading screen's (serving thread), a Bink movie's, or a main-thread frame outside Main::Update. Render side:
* Adopt() at each render frame's start on the thread that renders it (Renderer::Begin, before the frame driver's OnBegin);
* recording reads Current(). Both modes publish and adopt. Guest adopts at once on the same thread, so nothing it draws changes.
*
* Two producers, one consumer. The game update's publish (main thread, outside Renderer::Lock) and the frame-only publishes
* (at Begin, inside Renderer::Lock) never overlap: the serving thread renders only while the main thread is inside the load
* coordinator, between its SetLoadingScreenActive(true) and (false), and the startup logo plays before Main::Update first runs.
* A publish that finds another one under way is fatal. Adoption is at Begin, inside Renderer::Lock, so one thread adopts at a time.
*/
namespace Snapshot
{
	/** @brief A feature's capture: reads game data and calls a_builder.Set with its block. Runs on the game thread, inside the capture scope. */
	using CaptureFn = void (*)(Builder& a_builder);

	/** @brief Adds a feature capture. Call at load (PostPostLoad); captures run in registration order. A capture that has nothing to say sets nothing. */
	void RegisterCapture(CaptureFn a_capture);

	/** @brief The game half of an update the cadence test mode renders no frame for. Main thread, after the update's capture. */
	using UnrenderedFn = void (*)(const FrameSnapshot& a_snapshot);

	/** @brief Adds an unrendered update's game half. Call at load (PostPostLoad). */
	void RegisterUnrenderedUpdate(UnrenderedFn a_fn);

	/**
	* @brief Game-side work at the end of an update's call of Main::DrawFrame. Main thread, once per game update, before the update's
	* game logic. An update the game thread renders runs it after its renders (none on a dropped update). A routed update (native)
	* runs it after CS's game frame and before the publish.
	*/
	using UpdateEndFn = void (*)(const FrameSnapshot& a_snapshot);

	/** @brief Adds an update's game-side end. Call at load (PostPostLoad). */
	void RegisterUpdateEnd(UpdateEndFn a_fn);

	/** @brief Installs the publication hook at Main::Update's call of Main::DrawFrame (Hooks.cpp calls this). */
	void InstallHooks();

	/** @brief Where a frame-only snapshot comes from: the producer that renders a frame no game update stands for. */
	enum class FrameSource : std::uint8_t
	{
		MainThread,     // a main-thread frame outside Main::Update's call of Main::DrawFrame: the kind is decided as for a game update
		LoadingScreen,  // the serving thread's loading screen loop (SE 38085, AE 39039): Loading
		Movie,          // BSMoviePlayer::Tick (SE 87890, AE 90259), any thread: Movie
	};

	/**
	* @brief Builds and publishes a frame-only snapshot (FrameSnapshot's id rule): the last snapshot's world data with this frame's
	* kind, menus, load epoch and events, and a fresh UI stream holder in native. Renderer::Begin, before Adopt, inside Renderer::Lock.
	*/
	void CaptureFrame(FrameSource a_source);

	/** @brief Takes the newest published snapshot, if one is new, as the render frame's. Renderer::Begin, on the thread that renders. */
	void Adopt();

	/**
	 * @brief The adopted snapshot. Null before the first adoption. Render owner (the thread whose Begin adopted it).
	 * A read from another thread while the native render thread runs races its adoption: each such call site is logged once.
	 */
	const std::shared_ptr<const FrameSnapshot>& Current(std::source_location a_where = std::source_location::current());

	/** @brief a_T's block in the adopted snapshot, or null before the first adoption or when its feature captured none. Render owner. */
	template <FeatureBlock T>
	const T* CurrentBlock(std::source_location a_where = std::source_location::current())
	{
		const auto& snapshot = Current(a_where);
		return snapshot ? snapshot->Block<T>() : nullptr;
	}

	/**
	* @brief The adopted snapshot's events.gameDelta the first time a caller steps for it, else 0. a_lastId is the caller's record of
	* the snapshot it last stepped for, so a repeated snapshot holds still and a caller that skipped a frame still steps once. Render owner.
	*/
	inline float StepOnce(std::uint64_t& a_lastId)
	{
		const auto& snapshot = Current();
		if (!snapshot || snapshot->id == a_lastId)
			return 0.0f;
		a_lastId = snapshot->id;
		return snapshot->events.gameDelta;
	}

	/** @brief The adopted snapshot's id, 0 before the first. Any thread (for the test runner's status and logs). */
	std::uint64_t CurrentId();

	/**
	* @brief The newest game update's snapshot, built at the publication hook, for the game half's readers. Null before the first.
	* Game thread. It is the snapshot the update publishes for the render side: a cadence repeat renders it again, a dropped update's
	* is never adopted, and a frame-only snapshot (CaptureFrame) never replaces it. Its id is LastGameId().
	*/
	const std::shared_ptr<const FrameSnapshot>& Building();

	/** @brief Render identity, separate from the snapshot's game identity. */
	struct RenderIdentity
	{
		std::uint64_t frame = 0;              // the render frame (RenderGraph::FrameClock::BeginFrame at the adoption)
		float delta = 0.0f;                   // wall-clock seconds since the previous adoption point
		bool repeat = false;                  // this render frame adopted nothing new: it renders the previous frame's snapshot again
		std::uint32_t rendersOfSnapshot = 0;  // how many render frames have adopted Current(), this one included
	};

	/** @brief The render frame's identity. Render owner. */
	const RenderIdentity& Render();

	/**
	* @brief The time step of visual-only animation (glare, lens adaptation, ENB timers): the render frame's wall-clock delta, 0
	* while the adopted snapshot is paused. It advances on repeated snapshots too. Render owner.
	*/
	float VisualDelta();

	/**
	* @brief The game time world state steps by in this render frame: the adopted snapshot's events.gameDelta (summed over the
	* snapshots it replaced) on its first render, 0 on a repeat. So a repeated snapshot holds the world still and a dropped one
	* loses no time. Render owner.
	*/
	float GameStep();

	/**
	* @brief Whether this render frame breaks temporal histories (TAA and the upscalers, SSGI, water SSR, clouds): its snapshot
	* carries a camera cut, a teleport, a load start or the first frame after a load, counting the events of the snapshots it
	* replaced, and this is its first render. So a cut resets histories even when the snapshot that carried it was never
	* rendered, and a repeat does not reset them again. Native only: guest keeps its own reset triggers. Render owner.
	*/
	bool HistoryBreak();

	/**
	* @brief Whether this thread is inside a game update's render half (Main::Update's call of Main::DrawFrame, all of its cadence
	* renders). A Present outside it belongs to no game update: the main menu, a loading screen's thread. Any thread.
	*/
	bool InGameUpdate();

	/**
	* @brief The window proc (game thread): the renderer is asked for a new client size. Native publishes it with the next
	* snapshot (Events::windowSize) instead of letting the engine flag it at once.
	*/
	void NoteWindowResize(std::uint32_t a_width, std::uint32_t a_height);

	/**
	* @brief After Adopt, render owner. True once for each new resize request the adopted snapshot carries, with its size: the
	* render side asks the renderer for it at its frame start.
	*/
	bool WindowResizeToApply(std::uint32_t& a_width, std::uint32_t& a_height);

	/** @brief The newest game update's snapshot id published, 0 before the first. Any thread. */
	std::uint64_t LastGameId();

	/** @brief The render frame of the newest adoption (RenderIdentity::frame), 0 before the first. Any thread. */
	std::uint64_t LastRenderFrame();

	/**
	* @brief Game thread, after a routed update's publish while the render thread renders: waits for the next slot of the update-rate cap
	* (State::updateRateCap, decision 2). Updates the game thread renders itself are paced by their own Present.
	*/
	void PaceGameUpdate();

	/** @brief Called by the frame driver when a loading screen begins (true) or ends (false). Any thread. */
	void OnLoadingScreen(bool a_active);

	/** @brief The loading screens begun this session: the load epoch the next snapshot carries. Any thread. */
	std::uint64_t LoadEpoch();
}
