#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

/**
 * @brief The render graph side of Bink. It owns the movie texture, the decoder's staging buffers and the Present anchor's client.
 * BinkHooks.cpp decides per tick whether the graph draws. Everything here runs on the render thread. A pass reads only the frame
 * its run's commit stored.
 */
namespace RenderGraph::Engine::Bink
{
	/** @brief One tile of a movie, with its video rectangle and its quad in clip space. */
	struct TileDesc
	{
		std::uint32_t x = 0, y = 0, width = 0, height = 0;
		float rect[4]{};  // left, top, right, bottom
	};

	/** @brief A movie's tiles in the engine's order, whole-tile rows with their right edge tile, then the bottom row. */
	struct Layout
	{
		std::uint32_t videoWidth = 0, videoHeight = 0;
		std::vector<TileDesc> tiles;
	};

	/**
	 * @brief The tiles and quads BSMovie::CreateTileTextures (SE 87904, AE 90275) makes, computed as it computes them.
	 * a_edgeWidth and a_edgeHeight are the edge tiles' power-of-two sizes, or 0 when the video divides evenly.
	 */
	Layout MakeLayout(std::uint32_t a_videoWidth, std::uint32_t a_videoHeight, std::uint32_t a_tileSize, std::uint32_t a_columns, std::uint32_t a_rows,
		std::uint32_t a_edgeWidth, std::uint32_t a_edgeHeight, float a_scale, float a_x, float a_y, std::uint32_t a_screenWidth, std::uint32_t a_screenHeight);

	namespace Graph
	{
		class Movie;

		/** @brief Makes the movie texture, sampler, targets and the client. Logged and off when a part cannot be made. */
		void Setup();

		/**
		 * @brief Whether this tick may draw on the graph. With a_betweenFrames a needed rebuild happens here, between two frames.
		 * False while that build is not attached yet, or when the framebuffer cannot be imported. Render thread.
		 */
		bool Begin(bool a_betweenFrames);

		/** @brief A movie's tiles laid out in staging and in the movie texture's chunks. Null only for a layout with no tiles. Render thread. */
		std::shared_ptr<Movie> CreateMovie(const Layout& a_layout, std::uint32_t a_tileSize);

		/** @brief Where the decoder writes a tile of this tick's frame. Null when no staging buffer is free. Render thread. */
		std::byte* Lock(Movie& a_movie, std::uint32_t a_tile, std::uint32_t& a_rowPitch);

		/**
		 * @brief Queues the movie's frame for the Present anchor in place of the engine's tile draw, onto HDR's UI texture with a_hdr.
		 * False when the graph has nothing to draw, and the engine's draw runs. Render thread.
		 */
		bool Queue(const std::shared_ptr<Movie>& a_movie, bool a_hdr);

		/** @brief Drops a staging buffer the tick took but did not queue. Render thread. */
		void EndTick(Movie& a_movie);

		/** @brief Marks that the engine's tile textures hold a newer frame than the graph's. Render thread. */
		void MarkStale(Movie& a_movie);

		/** @brief Resets for the next movie and drops the stages' kept recordings of this one. The client stays registered. Render thread. */
		void MovieEnded();
	}
}
