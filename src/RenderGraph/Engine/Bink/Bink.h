#pragma once

/**
 * @brief Bink video drawn by the render graph (org-full-port.md P5.11), behind CS_ORG_BINK=1. The RE and the design are in
 * docs/development/p5/bink.md.
 *
 * Bink has no YUV path in Skyrim: BinkCopyToBufferRect converts each decoded frame on the CPU into BGRA bytes, one 256x256 tile
 * (smaller power-of-two tiles at the right and bottom edges) at a time, through the renderer's texture lock and unlock, and the
 * engine draws one textured quad per tile into kFRAMEBUFFER before the Present that shows the frame.
 *
 * With the switch on, a tick of the player that runs on the main thread and presents itself (the blocking play loop: console and
 * Papyrus PlayBink, the main menu's intro movie) decodes into a host-visible staging buffer instead of the engine's dynamic
 * textures, and its tile draw is withheld. The Present anchor's epoch then copies the staging buffer into ORG tile textures and
 * draws the tiles with the same shaders built for ORG (package/Shaders/Bink/BinkCopy.hlsl), into the framebuffer the Present shows.
 *
 * Everything else keeps the engine's path: a tick on another thread (the startup logo plays on the MoviePlayer sequence thread,
 * under Renderer::Lock, where no anchor runs), a movie drawn inside another frame (player mode 2), the first tick of a movie (the
 * client joins the Present anchor's graph from the next run), and every tick after the graph path failed.
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
}
