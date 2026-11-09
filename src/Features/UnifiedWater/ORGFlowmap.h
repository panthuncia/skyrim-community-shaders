#pragma once

#include <DirectXTex.h>

/**
 * @brief Unified Water's flowmap built without D3D11 (CS_ORG_UNIFIED_WATER=1).
 *
 * The D3D11 path copies every 64x64 flow tile into one texture on a deferred context, then reads the texture back to save
 * it as a DDS file. The engine only ever reads that file. So this path does not use the GPU: it places the decoded tiles
 * in a CPU image and saves it. Nothing goes through a render graph pass, because a pass would only add an upload and a
 * readback around the same copies. The bytes match the D3D11 path's file.
 */
namespace ORGFlowmap
{
	/** @brief One decoded flow tile: its cell coordinates and its B8G8R8A8 image with at least 6 mips. */
	struct Tile
	{
		int32_t x = 0;
		int32_t y = 0;
		DirectX::ScratchImage image;
	};

	/** @brief Whether CS_ORG_UNIFIED_WATER=1 is set. */
	bool Requested();

	/**
	 * @brief Places the tiles in one flowmap image and saves it as a DDS file.
	 * @param tiles The decoded tiles.
	 * @param width The flowmap's width in cells.
	 * @param height The flowmap's height in cells.
	 * @param offsetX Added to a tile's cell x to get its column.
	 * @param offsetY Added to a tile's cell y to get its row, counted from the bottom.
	 * @param mipLevels How many of the 6 mips to fill; the rest stay zero.
	 * @param path The DDS file to write.
	 * @return True if the file was written.
	 */
	bool Compose(const std::vector<Tile>& tiles, int32_t width, int32_t height, int32_t offsetX, int32_t offsetY, uint32_t mipLevels,
		const std::filesystem::path& path);
}
