#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

/**
 * @brief The render-graph side of Bink (Bink.h): the movie texture, the staging buffers the decoder writes, the Present anchor's
 * client and its passes. The engine side (BinkHooks.cpp) decides per tick whether the graph draws, and feeds this.
 *
 * Threads: everything here runs on the render thread (FrameAnchors::IsRenderOwner: the main thread, or with
 * CS_ORG_LOADING_THREAD=1 the startup logo's MoviePlayer thread, which holds Renderer::Lock), in a tick of the movie player (Begin, CreateMovie, Lock, Queue, EndTick) or in
 * the Present anchor's run that follows (the client's commit). The pass's Prepare and Record run where the graph runs them, and
 * read only the frame the commit stored for their run.
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
		 * @brief State::Setup, once the graph is up: the movie texture, the staging pool and the sampler, and the Present anchor's client,
		 * registered once for the session. Nothing when CS_ORG_BINK is off; logged and off when anything cannot be made.
		 */
		void Setup();

		/**
		 * @brief Render thread, at the start of a tick that could draw on the graph: whether this tick may. The client joined the
		 * Present anchor's graph at setup's build point and the tile program was built at setup, so the only refusals are the ring
		 * being full this frame (FrameAnchors::CanRun), a framebuffer that cannot be imported, and the ticks between a resize and the
		 * next build. A frame queued earlier that did not draw is logged as an error. False: the engine draws this tick.
		 */
		bool Begin();

		/** @brief Render thread: the movie's tiles and their place in a staging buffer; null, logged, when the graph path cannot draw it. */
		std::shared_ptr<Movie> CreateMovie(const Layout& a_layout);

		/**
		 * @brief Render thread, from the renderer's texture lock: where the decoder writes tile a_tile of this tick's frame, and its row
		 * pitch. The first call of a tick takes a staging buffer from the movie's pool. Null when there is none to take.
		 */
		std::byte* Lock(Movie& a_movie, std::uint32_t a_tile, std::uint32_t& a_rowPitch);

		/**
		 * @brief Render thread, in place of the engine's tile draw: the movie's frame queued for the Present anchor's run (this tick's
		 * staging buffer, uploaded first, or the last frame uploaded when this tick decoded nothing). False when there is nothing the
		 * graph can draw: the engine's draw runs.
		 */
		bool Queue(const std::shared_ptr<Movie>& a_movie);

		/** @brief Render thread, at the end of every tick that called Begin: drops a staging buffer the tick took but did not queue. */
		void EndTick(Movie& a_movie);

		/** @brief Render thread: the engine's tile textures hold a newer frame than the graph's (a tick decoded on the engine's path). */
		void MarkStale(Movie& a_movie);

		/** @brief Render thread, when a movie's tiles are freed: the next movie starts from a clean count. The client stays registered. */
		void MovieEnded();
	}
}
