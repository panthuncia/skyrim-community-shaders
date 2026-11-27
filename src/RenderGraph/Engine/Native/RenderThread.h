#pragma once

#include "RenderGraph/Engine/Native/RenderScene.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace Snapshot
{
	struct FrameSnapshot;
}

/**
* @brief Native's routed game updates and their render half (render-thread roadmap, phase 7; design in
* docs/development/plans/snap7-thread.md, section 3).
*
* A routed update (an unpaused world update without the map, and not Main::DrawFrame's debug camera path) does not call Main::DrawFrame.
* The game thread runs CS's game frame in its place (GameFrame): DrawFrame's game items in its order (the menus' PreDisplay, the
* WorldPtToScreenPt3 camera, the driver's game half between the scrap heap's mark and release, the volumetric lighting flag, the frame
* jobs, the UI recorded without render state, the save screenshot request, the scene graph's and the render passes' upkeep), and hands
* the render half a FramePayload, which the snapshot carries. The render half (RenderHalf) draws it from the payload alone:
* Renderer::Lock around Begin (CS's adoption and frame start), then without the lock DCLF's render half from the payload's ticket and the
* driver's render half, then Renderer::Lock again around volumetric lighting, the UI point's replay and Renderer::End (CS's Present
* hook: the render side's frame end only). A repeated render draws the same payload again.
*
* The render half runs on CS's render thread, created at the first routed update's publish. Its loop renders the newest published
* snapshot, or the adopted one again when none is new, and never waits for the game. A producer that must render on its own thread
* (Main::DrawFrame for an unrouted update or a frame outside Main::Update, the loading screen's serving loop, a Bink tick, a routed
* update whose interface the render half must draw on the producer) parks the render thread first: it finishes its frame and stops.
* That wait, at a route change, is the only one between the threads. The next routed update's publish wakes it.
*/
namespace RenderGraph::Engine::Native::RenderThread
{
	/** @brief What a routed update's game frame leaves for its render half. Values only (and the driver's handed objects). */
	struct FramePayload
	{
		Scene::Prepared prepared;         // the driver's game half
		bool volumetricLighting = false;  // the console's TVL flags: RenderEffect 0x22 after the world
		std::uint32_t volumetricTarget = 0;
		// The UI could not be recorded alone (a menu that pauses or the map opened in the update's UI jobs): the render half calls
		// DrawInterface itself, on the render owner, where Main::DrawFrame called it.
		bool interfaceInline = false;
	};

	/**
	* @brief Game thread, after the update's capture: whether native routes this update (design 3.1). False outside native mode.
	*/
	bool Routes(const Snapshot::FrameSnapshot& a_snapshot);

	/** @brief Game thread: CS's game frame in place of Main::DrawFrame, for a routed update. Returns its render half's payload. */
	std::shared_ptr<const FramePayload> GameFrame(RE::Main* a_main, const Snapshot::FrameSnapshot& a_snapshot);

	/**
	* @brief The render half of the newest published routed snapshot (adopted at its Begin), or of the adopted one again. Renders on the
	* calling thread, which becomes the render owner at its Begin.
	*/
	void RenderHalf();

	/** @brief Native only: the producers' park hooks (Main::DrawFrame's entry). Snapshot::InstallHooks calls it. */
	void InstallHooks();

	/**
	* @brief Game thread, after a routed update's publish: starts the render thread at the first, wakes it when parked. The render thread
	* renders the published snapshot from then on.
	*/
	void OnPublished();

	/** @brief The producer that parks the render thread, counted by kind in the stats line. */
	enum class ParkReason : std::uint8_t
	{
		DrawFrame,         // Main::DrawFrame
		LoadingScreen,     // the loading screen's serving loop
		MovieTick,         // a Bink tick
		Interface,         // a routed update whose interface the producer draws
		UnrenderedUpdate,  // an update the render thread does not render
		Count
	};

	/**
	* @brief A producer that renders on its own thread: the render thread finishes its frame and stops until the next routed publish.
	* Returns at once on the render thread, before it exists, or while parked. a_reason names the producer for the counts.
	*/
	void Park(ParkReason a_reason);

	/**
	 * @brief Render thread: waits for the GPU work of the render before this one, once per render. DCLF's depth commit calls it:
	 * without it a render overlapping the one before lost the device (snap7-devloss.md). Elsewhere it returns at once.
	 */
	void WaitForLastRender();

	/** @brief Whether the render thread exists and is not parked: the game thread's update-rate cap applies. Any thread. */
	bool Running();

	/** @brief Whether the calling thread is CS's render thread. */
	bool OnRenderThread();

	/** @brief Renderer::Begin's detour, right after the adoption: a routed render names its payload's DCLF ticket. */
	void OnAdopted();

	/**
	 * @brief Game thread, at a publish: a_old (dropped by the cadence, or replaced before a render took it) is never rendered. Its DCLF
	 * scene fragment goes on in a_fresh's ticket when that one took none.
	 */
	void Superseded(const Snapshot::FrameSnapshot& a_old, const Snapshot::FrameSnapshot& a_fresh);

	/** @brief The waits the stats line totals: the game thread's and the render thread's, by what they wait on. */
	enum class Wait : std::uint8_t
	{
		UpdateCap,    // game thread: the update-rate cap (Snapshot::PaceGameUpdate)
		FrameJobs,    // game thread: the game frame's wait for the UI jobs
		ReflexSleep,  // game thread: Reflex's sleep at the input poll
		RenderLock,   // render thread: taking Renderer::Lock
		LastRender,   // render thread: the depth commit's wait for the last render's GPU work
		Count
	};

	/** @brief Adds a_duration to a_wait's total for the stats line. Any thread. */
	void CountWait(Wait a_wait, std::chrono::steady_clock::duration a_duration);

	/** @brief Whether this thread is inside a routed render half: its Present belongs to no game update. */
	bool InRoutedRender();

	/**
	* @brief One line for the snapshot log, since the last line: routed and unrouted updates, inline interfaces, parks; renders of fresh
	* and repeated snapshots, the time from a snapshot's publish to its first render's adoption and end; each thread's waits. Empty
	* outside native.
	*/
	std::string TakeStatsLine();
}
