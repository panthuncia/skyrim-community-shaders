#pragma once

#include <string>

/**
 * @brief Bink video drawn by the render graph, in graph mode (Engine::GraphMode). The RE and design are in docs/development/p5/bink.md.
 *
 * Skyrim's Bink converts each frame on the CPU into BGRA 256x256 tiles, and the engine draws one quad per tile.
 * In graph mode a render-thread tick decodes into a staging buffer instead, and the Present anchor's epoch copies it into
 * one graph texture and draws the tiles. One client registered at Setup serves every movie, so a movie never rebuilds the graph.
 * Ticks on other threads keep the engine's path. A setup failure is fatal.
 */
namespace RenderGraph::Engine::Bink
{
	/** @brief Hooks the movie player and reserves the Present anchor. Must run at plugin load, before the render graph initializes. */
	void InstallHooks();

	/** @brief Whether InstallHooks detoured BSMoviePlayer::Tick. Any thread, after it. */
	bool TickHooked();

	/**
	 * @brief Plays a_file on the main thread at its next frame-job wait (SE 34554, AE 35409), for the test harness.
	 * The console's PlayBink does nothing as a compiled script and SKSE tasks run on job threads. Only in a cs-test run.
	 */
	void RequestPlay(std::string a_file);
}
