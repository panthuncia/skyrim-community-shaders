#pragma once

#include <string>

/**
 * @brief Bink video drawn by the render graph, behind CS_ORG_BINK=1. The RE and design are in docs/development/p5/bink.md.
 *
 * Skyrim's Bink converts each frame on the CPU into BGRA 256x256 tiles, and the engine draws one quad per tile.
 * With the switch on, a render-thread tick decodes into a staging buffer instead, and the Present anchor's epoch copies it into
 * one graph texture and draws the tiles. One client registered at Setup serves every movie, so a movie never rebuilds the graph.
 * Ticks on other threads, and every tick after the path failed for the session, keep the engine's path.
 */
namespace RenderGraph::Engine::Bink
{
	/** @brief Whether CS_ORG_BINK=1 asked for Bink on the render graph. Not on VR, whose player was not checked. */
	bool Requested();

	/** @brief Hooks the movie player and reserves the Present anchor. Must run at plugin load, before the render graph initializes. */
	void InstallHooks();

	/** @brief Whether InstallHooks detoured BSMoviePlayer::Tick. Any thread, after it. */
	bool TickHooked();

	/** @brief Registers the Present anchor's client once for the session. */
	void Setup();

	/**
	 * @brief Plays a_file on the main thread at its next frame-job wait (SE 34554, AE 35409), for the test harness.
	 * The console's PlayBink does nothing as a compiled script and SKSE tasks run on job threads. Only with CS_DCLF_TEST_COMMANDS.
	 */
	void RequestPlay(std::string a_file);
}
