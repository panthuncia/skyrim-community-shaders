#pragma once

#include <string>

/**
 * @brief Bink video drawn by the render graph (org-full-port.md P5.11), behind CS_ORG_BINK=1. The RE and the design are in
 * docs/development/p5/bink.md.
 *
 * Bink has no YUV path in Skyrim: BinkCopyToBufferRect converts each decoded frame on the CPU into BGRA bytes, one 256x256 tile
 * (smaller power-of-two tiles at the right and bottom edges) at a time, through the renderer's texture lock and unlock, and the
 * engine draws one textured quad per tile into kFRAMEBUFFER before the Present that shows the frame.
 *
 * With the switch on, a tick of the player that runs on the render thread (FrameAnchors::IsRenderOwner) and presents itself (the blocking play loop: console and
 * Papyrus PlayBink, the main menu's intro movie) decodes into a host-visible staging buffer (RenderGraph::FrameStaging) instead
 * of the engine's dynamic textures, and its tile draw is withheld when the Present anchor can run it (FrameAnchors::CanRun). The
 * Present anchor's epoch then copies the staging buffer into one graph texture holding the whole video and draws the tiles from
 * it with the same shaders built for ORG (package/Shaders/Bink/BinkCopy.hlsl), into the framebuffer the Present shows. One client,
 * registered at Setup, serves every movie of the session: a movie never rebuilds the graph.
 *
 * Everything else keeps the engine's path: a tick on another thread (without CS_ORG_LOADING_THREAD=1, the startup logo, which
 * plays on the MoviePlayer sequence thread under Renderer::Lock, where no anchor runs then), a movie drawn inside another frame
 * (player mode 2), a movie larger than the movie texture, every tick before the client is in the Present anchor's built graph,
 * the rest of a movie one of whose frames did not draw, and every tick after the graph path failed.
 */
namespace RenderGraph::Engine::Bink
{
	/** @brief Whether CS_ORG_BINK=1 asked for Bink on the render graph (not on VR, whose player was not checked). */
	bool Requested();

	/**
	 * @brief Main thread, at plugin load (Hooks::Install), before the render graph initializes: hooks the movie player when
	 * requested or when the CS_ORG_BINK_SHOT diagnostic is set, and reserves the Present anchor when requested.
	 */
	void InstallHooks();

	/** @brief State::Setup, once the graph is up: the Present anchor's client, registered once for the session (CS_ORG_BINK only). */
	void Setup();

	/**
	 * @brief Any thread: plays a_file (from Data\Video) on the engine's main thread at its next frame-job wait (SE 34554, AE
	 * 35409, in Main::Update; RenderGraph::Engine::MainTick), through MoviePlayer::PlayMovie with the console's PlayBink defaults.
	 * The test harness's dclf:bink: the console's PlayBink does nothing as a compiled script, and SKSE tasks run on job threads
	 * here. Installed only while the harness runs (CS_DCLF_TEST_COMMANDS is set); otherwise logged and ignored.
	 */
	void RequestPlay(std::string a_file);
}
