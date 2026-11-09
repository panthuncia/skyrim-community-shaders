#include "ORGFlowmap.h"

#include "Utils/D3D.h"

namespace
{
	constexpr uint32_t kTileSize = 64;
	constexpr uint32_t kMipLevels = 6;
	constexpr size_t kBytesPerPixel = 4;
}

bool ORGFlowmap::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA("CS_ORG_UNIFIED_WATER", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

bool ORGFlowmap::Compose(const std::vector<Tile>& tiles, int32_t width, int32_t height, int32_t offsetX, int32_t offsetY, uint32_t mipLevels,
	const std::filesystem::path& path)
{
	if (width <= 0 || height <= 0) {
		logger::error("[Unified Water] [Flowmap] [ORG] Invalid flowmap size {}x{}", width, height);
		return false;
	}

	const size_t pixelWidth = static_cast<size_t>(width) * kTileSize;
	const size_t pixelHeight = static_cast<size_t>(height) * kTileSize;

	DirectX::ScratchImage flowmap;
	if (FAILED(flowmap.Initialize2D(DXGI_FORMAT_B8G8R8A8_UNORM, pixelWidth, pixelHeight, 1, kMipLevels))) {
		logger::error("[Unified Water] [Flowmap] [ORG] Failed to allocate a {}x{} image", pixelWidth, pixelHeight);
		return false;
	}
	// Cells without a tile, and mips not filled, are zero, as in a new D3D11 texture.
	std::memset(flowmap.GetPixels(), 0, flowmap.GetPixelsSize());

	mipLevels = std::min(mipLevels, kMipLevels);
	for (const auto& tile : tiles) {
		const auto column = static_cast<uint32_t>(tile.x + offsetX);
		const auto row = static_cast<uint32_t>(tile.y + offsetY);
		if (column >= static_cast<uint32_t>(width) || row >= static_cast<uint32_t>(height)) {
			logger::warn("[Unified Water] [Flowmap] [ORG] Flow texture at {},{} lies outside the flowmap", tile.x, tile.y);
			continue;
		}

		for (uint32_t mip = 0; mip < mipLevels; ++mip) {
			const DirectX::Image* src = tile.image.GetImage(mip, 0, 0);
			const DirectX::Image* dst = flowmap.GetImage(mip, 0, 0);
			const uint32_t tileSize = std::max(1u, kTileSize >> mip);
			if (!src || !dst || src->width != tileSize || src->height != tileSize) {
				logger::warn("[Unified Water] [Flowmap] [ORG] Flow texture at {},{} has no {}x{} mip {}", tile.x, tile.y, tileSize, tileSize, mip);
				break;
			}

			// The flowmap's rows run from the top, its cells from the bottom (the D3D11 path's dstY).
			const size_t dstX = static_cast<size_t>(column) * tileSize;
			const size_t dstY = dst->height - (static_cast<size_t>(row) + 1) * tileSize;
			const size_t rowBytes = tileSize * kBytesPerPixel;
			for (uint32_t line = 0; line < tileSize; ++line) {
				const uint8_t* from = src->pixels + line * src->rowPitch;
				uint8_t* to = dst->pixels + (dstY + line) * dst->rowPitch + dstX * kBytesPerPixel;
				std::memcpy(to, from, rowBytes);
			}
		}
	}

	if (const auto hr = Util::SaveImageToFile(path, flowmap); FAILED(hr)) {
		logger::error("[Unified Water] [Flowmap] [ORG] Failed to save flowmap to {}: hr={:08X}", path.string(), static_cast<uint32_t>(hr));
		return false;
	}

	logger::info("[Unified Water] [Flowmap] [ORG] Composed {} tiles on the CPU, no D3D11", tiles.size());
	return true;
}
