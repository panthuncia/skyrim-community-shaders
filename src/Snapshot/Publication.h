#pragma once

#include "Snapshot/FrameSnapshot.h"

#include <cstdint>
#include <memory>

/**
* @brief Publication and adoption of frame snapshots (contract: Snapshot/FrameSnapshot.h).
*
* Game side (main thread): Capture() builds the next snapshot and publishes it, at Main::Update's call of Main::DrawFrame
* (SE 35565 +0x5D2, AE 36564 +0xA97). A render frame outside that call publishes a frame-only snapshot with CaptureFrame() at
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

	/** @brief Installs the publication hook at Main::Update's call of Main::DrawFrame (Hooks.cpp calls this). */
	void InstallHooks();

	/** @brief Builds and publishes the next snapshot. Main thread, once per game update. */
	void Capture();

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

	/** @brief The adopted snapshot. Null before the first adoption. Render owner (the thread whose Begin adopted it). */
	const std::shared_ptr<const FrameSnapshot>& Current();

	/** @brief The adopted snapshot's id, 0 before the first. Any thread (for the test runner's status and logs). */
	std::uint64_t CurrentId();

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

	/** @brief Called by the frame driver when a loading screen begins (true) or ends (false). Any thread. */
	void OnLoadingScreen(bool a_active);

	/** @brief The loading screens begun this session: the load epoch the next snapshot carries. Any thread. */
	std::uint64_t LoadEpoch();

	/**
	* @brief The cadence test mode (CS_SNAPSHOT_CADENCE=repeat:N|drop:N, native only). How many times the render half runs for
	* the game update that just published: N for repeat, 1 every Nth update and 0 otherwise for drop, else 1. Main thread.
	*/
	std::uint32_t RendersThisUpdate();
}
