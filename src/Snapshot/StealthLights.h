#pragma once

#include <cstdint>
#include <string>

/**
* @brief S12/S13 (decision 10): the stealth light level's light membership, from game data on the game thread (native).
*
* AIProcess::CalculateLightValue (SE 38900, AE 39946) sums lights the render half maintains: the world ShadowSceneNode's
* active lists for the player (ShadowSceneNode::ProcessQueuedLights applies the add and remove queues), and the skin
* property's light list for an NPC (the render half's light attach walk). In native CS answers both sums from a membership
* it rebuilds at the call: the active lists with the queues applied, and for an NPC the lights whose radius reaches its skin.
* The engine's lists are left alone; the renderer still uses them in phases 0-6.
*/
namespace Snapshot::StealthLights
{
	/** @brief Native only: redirects the light level's sums. Called by SideEffects::InstallHooks. */
	void InstallHooks();

	/** @brief The counters since the last call, for the side effects line, or empty outside native. */
	std::string TakeStatsLine();
}
