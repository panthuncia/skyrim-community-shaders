#pragma once

#include "Snapshot/FrameSnapshot.h"

#include <cstdint>
#include <memory>

/**
* @brief Publication and adoption of frame snapshots (contract: Snapshot/FrameSnapshot.h).
*
* Game side (main thread): Capture() builds the next snapshot and publishes it, at Main::Update's call of Main::DrawFrame
* (SE 35565 +0x5D2, AE 36564 +0xA97). Render side: Adopt() at each render frame's start on the main thread
* (Renderer::Begin, before the frame driver's OnBegin); recording reads Current(). Both modes publish and adopt. Guest
* adopts at once on the same thread, so nothing it draws changes.
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

	/** @brief Takes the newest published snapshot, if one is new, as the render frame's. Renderer::Begin on the main thread. */
	void Adopt();

	/** @brief The adopted snapshot. Null before the first adoption. Render owner (main thread in phase 0). */
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

	/** @brief Called by the frame driver when a loading screen begins (true) or ends (false). Any thread. */
	void OnLoadingScreen(bool a_active);

	/**
	* @brief The cadence test mode (CS_SNAPSHOT_CADENCE=repeat:N|drop:N, native only). How many times the render half runs for
	* the game update that just published: N for repeat, 1 every Nth update and 0 otherwise for drop, else 1. Main thread.
	*/
	std::uint32_t RendersThisUpdate();
}
