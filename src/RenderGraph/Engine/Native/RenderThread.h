#pragma once

#include "RenderGraph/Engine/Native/RenderScene.h"

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
* Renderer::Lock and Begin (CS's adoption and frame start), DrawFrame's render state, DCLF's render half from the payload's ticket, the
* driver's render half, volumetric lighting, the UI point's replay, Renderer::End (CS's Present hook: the render side's frame end only)
* and Unlock. A repeated render draws the same payload again.
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

	/** @brief Renderer::Begin's detour, right after the adoption: a routed render names its payload's DCLF ticket. */
	void OnAdopted();

	/** @brief Whether this thread is inside a routed render half: its Present belongs to no game update. */
	bool InRoutedRender();

	/** @brief One line for the snapshot log: routed and unrouted updates, inline interfaces, since the last line. Empty outside native. */
	std::string TakeStatsLine();
}
