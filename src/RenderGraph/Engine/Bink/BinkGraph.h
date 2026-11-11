#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

/**
 * @brief The render-graph side of Bink (Bink.h): the tile textures, the staging buffers the decoder writes, the Present anchor's
 * client and its pass. The engine side (BinkHooks.cpp) decides per tick whether the graph draws, and feeds this.
 *
 * Threads: everything here runs on the main thread, in a tick of the movie player (Begin, CreateMovie, Lock, Queue, EndTick) or in
 * the Present anchor's run that follows (the client's commit). The pass's Prepare and Record run where the graph runs them, and
 * read only the frame slot the commit wrote.
 */
namespace RenderGraph::Engine::Bink
{
	/** @brief One tile of a movie: its rectangle of the video (the texture's size) and its quad in clip space. */
	struct TileDesc
	{
		std::uint32_t x = 0, y = 0, width = 0, height = 0;
		float rect[4]{};  // left, top, right, bottom
	};

	/** @brief A movie's tiles, in the engine's order (rows of whole tiles and the right edge tile, then the bottom row). */
	struct Layout
	{
		std::uint32_t videoWidth = 0, videoHeight = 0;
		std::vector<TileDesc> tiles;
	};

	/**
	 * @brief The tiles BSMovie::CreateTileTextures (SE 87904, AE 90275) makes, with the quads it builds, computed as it computes them.
	 * @param a_tileSize The whole tiles' edge (256).
	 * @param a_columns, a_rows The whole tiles across and down.
	 * @param a_edgeWidth, a_edgeHeight The edge tiles' power-of-two size; 0 when the video divides evenly.
	 * @param a_scale, a_x, a_y The movie's scale and its offset in pixels (MoviePlayer::CalculateScale and the two after it).
	 * @param a_screenWidth, a_screenHeight The screen size the player reads (BSGraphics::State).
	 */
	Layout MakeLayout(std::uint32_t a_videoWidth, std::uint32_t a_videoHeight, std::uint32_t a_tileSize, std::uint32_t a_columns, std::uint32_t a_rows,
		std::uint32_t a_edgeWidth, std::uint32_t a_edgeHeight, float a_scale, float a_x, float a_y, std::uint32_t a_screenWidth, std::uint32_t a_screenHeight);

	namespace Graph
	{
		class Movie;

		/**
		 * @brief Main thread, at the start of a tick that could draw on the graph: whether this tick may. Registers the Present
		 * anchor's client the first time (that tick stays the engine's: the client joins the graph from the next run), checks the
		 * framebuffer and the program for its format, and counts the frames queued that never ran (three in a row turn the graph path
		 * off). False: the engine draws this tick.
		 */
		bool Begin();

		/** @brief Main thread: the movie's ORG tile textures and staging pool; null, logged, when they cannot be made. */
		std::shared_ptr<Movie> CreateMovie(const Layout& a_layout);

		/**
		 * @brief Main thread, from the renderer's texture lock: where the decoder writes tile a_tile of this tick's frame, and its row
		 * pitch. The first call of a tick takes a staging buffer from the movie's pool. Null when there is none to take.
		 */
		std::byte* Lock(Movie& a_movie, std::uint32_t a_tile, std::uint32_t& a_rowPitch);

		/**
		 * @brief Main thread, in place of the engine's tile draw: the movie's frame queued for the Present anchor's run (this tick's
		 * staging buffer, uploaded first, or the last frame uploaded when this tick decoded nothing). False when there is nothing the
		 * graph can draw: the engine's draw runs.
		 */
		bool Queue(const std::shared_ptr<Movie>& a_movie);

		/** @brief Main thread, at the end of every tick that called Begin: drops a staging buffer the tick took but did not queue. */
		void EndTick(Movie& a_movie);

		/** @brief Main thread: the engine's tile textures hold a newer frame than the graph's (a tick decoded on the engine's path). */
		void MarkStale(Movie& a_movie);

		/** @brief Main thread, when a movie's tiles are freed: unregisters the client, so the Present anchor costs nothing again. */
		void MovieEnded();
	}
}
