#include "Flowmap.h"

#include "Utils/D3D.h"
#include <DDSTextureLoader.h>
#include <DirectXTex.h>
#include <charconv>

bool Flowmap::TryGetFlowmap(RE::NiPointer<RE::NiSourceTexture>& outFlowmapTex) const
{
	if (!flowmapTex || !flowmapTex->rendererTexture || !flowmapTex->rendererTexture->texture || !flowmapTex->rendererTexture->resourceView)
		return false;

	outFlowmapTex = this->flowmapTex;
	return true;
}

void Flowmap::Reset()
{
	flowmapTex = nullptr;
	width = 0;
	height = 0;
	invWidth = 0.0f;
	invHeight = 0.0f;
	offsetX = 0;
	offsetY = 0;
}

bool Flowmap::LoadOrGenerateFlowmap(bool useMips)
{
	Reset();

	if (!LoadFlowmap()) {
		logger::info("[Unified Water] [Flowmap] Could not load flowmap - regenerating...");
		return RegenerateAndLoadFlowmap(useMips);
	}

	return true;
}

bool Flowmap::RegenerateAndLoadFlowmap(bool useMips)
{
	Reset();

	namespace fs = std::filesystem;
	const fs::path dir = Util::PathHelpers::GetDataPath() / "textures" / "water" / "flowmaps";

	std::error_code ec;
	fs::create_directories(dir, ec);

	if (!fs::exists(dir))
		return false;

	for (const auto& entry : fs::directory_iterator(dir, ec)) {
		if (ec)
			break;
		if (!entry.is_regular_file())
			continue;

		const auto& path = entry.path();
		if (path.extension() != ".dds")
			continue;

		std::error_code rec;
		fs::remove(path, rec);
		if (rec)
			logger::warn("[Unified Water] [Flowmap] Failed to remove '{}': {}", path.string(), rec.message());
	}

	if (!GenerateFlowmap(useMips)) {
		logger::error("[Unified Water] [Flowmap] Failed to generate flowmap");
		return false;
	}

	if (!LoadFlowmap()) {
		logger::error("[Unified Water] [Flowmap] Failed to load flowmap after generation");
		Reset();
		return false;
	}

	logger::debug("[Unified Water] [Flowmap] Flowmap regenerated and loaded");
	return true;
}

bool Flowmap::LoadFlowmap()
{
	namespace fs = std::filesystem;

	const fs::path dir = Util::PathHelpers::GetDataPath() / "textures" / "water" / "flowmaps";

	fs::directory_entry file;

	if (fs::exists(dir) && fs::is_directory(dir)) {
		for (const auto& entry : fs::directory_iterator(dir)) {
			if (!entry.is_regular_file()) {
				continue;
			}

			const std::wstring name = entry.path().filename().wstring();
			if (name.rfind(L"Tamriel-Flowmap", 0) == 0) {
				file = entry;
				break;
			}
		}
	}

	if (file.path().empty()) {
		logger::debug("[Unified Water] [Flowmap] No flowmap found");
		return false;
	}

	std::vector<std::string> tokens;
	std::istringstream iss(file.path().filename().stem().string());
	std::string token;

	while (std::getline(iss, token, '.')) {
		tokens.push_back(token);
	}

	if (tokens.size() != 5) {
		logger::error("[Unified Water] [Flowmap] Invalid file name");
		return false;
	}

	auto path = std::format(R"(textures\water\flowmaps\{})", file.path().filename().string().c_str());
	RE::NiPointer<RE::NiTexture> tex;
	RE::BSShaderManager::GetTexture(path.c_str(), true, tex, false);

	if (!tex || tex->GetRTTI() != globals::rtti::NiSourceTextureRTTI.get()) {
		logger::error("[Unified Water] [Flowmap] Failed to load flowmap from {}", path);
		return false;
	}

	const auto sourceTex = static_cast<RE::NiSourceTexture*>(tex.get());

	if (!sourceTex || !sourceTex->rendererTexture || !sourceTex->rendererTexture->texture) {
		logger::error("[Unified Water] [Flowmap] Flowmap invalid", path);
		return false;
	}

	flowmapTex = RE::NiPointer(sourceTex);

	auto parse_int = [&](const std::string& str, int32_t& out) -> bool {
		int temp;
		auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), temp);
		if (ec != std::errc{} || ptr != str.data() + str.size()) {
			logger::error("[Unified Water] [Flowmap] Failed to parse '{}' from filename", str);
			return false;
		}
		out = temp;
		return true;
	};

	if (!parse_int(tokens[1], width) || !parse_int(tokens[2], height) || !parse_int(tokens[3], offsetX) || !parse_int(tokens[4], offsetY)) {
		return false;
	}

	invWidth = 1.0f / static_cast<float>(width);
	invHeight = 1.0f / static_cast<float>(height);

	logger::debug("[Unified Water] [Flowmap] Flowmap loaded");
	return true;
}

namespace
{
	constexpr uint32_t kTileSize = 64;
	constexpr uint32_t kMipLevels = 6;
	constexpr size_t kBytesPerPixel = 4;

	/** @brief One decoded flow tile: its cell coordinates and its B8G8R8A8 image with at least 6 mips. */
	struct Tile
	{
		int32_t x = 0;
		int32_t y = 0;
		DirectX::ScratchImage image;
	};

	/**
	 * @brief Places the tiles in one flowmap image and saves it as a DDS file. The engine only reads that file, so this needs no
	 * GPU: the tiles are copied on the CPU.
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
		const std::filesystem::path& path)
	{
		if (width <= 0 || height <= 0) {
			logger::error("[Unified Water] [Flowmap] Invalid flowmap size {}x{}", width, height);
			return false;
		}

		const size_t pixelWidth = static_cast<size_t>(width) * kTileSize;
		const size_t pixelHeight = static_cast<size_t>(height) * kTileSize;

		DirectX::ScratchImage flowmap;
		if (FAILED(flowmap.Initialize2D(DXGI_FORMAT_B8G8R8A8_UNORM, pixelWidth, pixelHeight, 1, kMipLevels))) {
			logger::error("[Unified Water] [Flowmap] Failed to allocate a {}x{} image", pixelWidth, pixelHeight);
			return false;
		}
		// Cells without a tile, and mips not filled, are zero.
		std::memset(flowmap.GetPixels(), 0, flowmap.GetPixelsSize());

		mipLevels = std::min(mipLevels, kMipLevels);
		for (const auto& tile : tiles) {
			const auto column = static_cast<uint32_t>(tile.x + offsetX);
			const auto row = static_cast<uint32_t>(tile.y + offsetY);
			if (column >= static_cast<uint32_t>(width) || row >= static_cast<uint32_t>(height)) {
				logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} lies outside the flowmap", tile.x, tile.y);
				continue;
			}

			for (uint32_t mip = 0; mip < mipLevels; ++mip) {
				const DirectX::Image* src = tile.image.GetImage(mip, 0, 0);
				const DirectX::Image* dst = flowmap.GetImage(mip, 0, 0);
				const uint32_t tileSize = std::max(1u, kTileSize >> mip);
				if (!src || !dst || src->width != tileSize || src->height != tileSize) {
					logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} has no {}x{} mip {}", tile.x, tile.y, tileSize, tileSize, mip);
					break;
				}

				// The flowmap's rows run from the top, its cells from the bottom.
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
			logger::error("[Unified Water] [Flowmap] Failed to save flowmap to {}: hr={:08X}", path.string(), static_cast<uint32_t>(hr));
			return false;
		}
		return true;
	}
}

bool Flowmap::GenerateFlowmap(bool useMips)
{
	const auto t0 = std::chrono::steady_clock::now();

	const auto tamriel = RE::TESForm::LookupByEditorID<RE::TESWorldSpace>("Tamriel");
	if (!tamriel) {
		logger::error("[Unified Water] [Flowmap] Failed to load Tamriel WorldSpace");
		return false;
	}

	int32_t worldMinX, worldMinY, worldMaxX, worldMaxY;
	Util::WorldToCell(tamriel->minimumCoords, worldMinX, worldMinY);
	Util::WorldToCell(tamriel->maximumCoords, worldMaxX, worldMaxY);
	worldMaxX -= 1;
	worldMaxY -= 1;

	int32_t mapMinX = INT_MAX;
	int32_t mapMinY = INT_MAX;
	int32_t mapMaxX = INT_MIN;
	int32_t mapMaxY = INT_MIN;

	auto cells = std::vector<Tile>();
	cells.reserve(1024);

	{
		for (auto y = worldMinY; y < worldMaxY; ++y) {
			for (auto x = worldMinX; x < worldMaxX; ++x) {
				auto path = std::format(R"(Textures\Water\skyrim.esm\flow.{}.{}.dds)", x, y);
				auto stream = RE::BSResourceNiBinaryStream(path);

				if (!stream.good())
					continue;

				const auto size = stream.stream->totalSize;
				std::vector<uint8_t> buffer(size);
				stream.read(buffer.data(), size);

				DirectX::TexMetadata meta{};
				DirectX::ScratchImage src;
				auto hr = DirectX::LoadFromDDSMemory(buffer.data(), size, DirectX::DDS_FLAGS_NONE, &meta, src);
				if (FAILED(hr)) {
					logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} failed to load", x, y);
					continue;
				}

				DirectX::ScratchImage conv;
				if (DirectX::IsCompressed(meta.format)) {
					hr = DirectX::Decompress(src.GetImages(), src.GetImageCount(), src.GetMetadata(), DXGI_FORMAT_B8G8R8A8_UNORM, conv);
					if (FAILED(hr)) {
						logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} failed to decompress", x, y);
						continue;
					}
				} else if (meta.format != DXGI_FORMAT_B8G8R8A8_UNORM) {
					hr = DirectX::Convert(src.GetImages(), src.GetImageCount(), src.GetMetadata(), DXGI_FORMAT_B8G8R8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.0f, conv);
					if (FAILED(hr)) {
						logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} failed to convert to the correct format", x, y);
						continue;
					}
				} else {
					conv = std::move(src);
				}

				const auto& d = conv.GetMetadata();
				if (d.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || d.width != 64 || d.height != 64 || d.format != DXGI_FORMAT_B8G8R8A8_UNORM || d.mipLevels < 6) {
					logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} is invalid", x, y);
					continue;
				}

				mapMinX = std::min(mapMinX, x);
				mapMinY = std::min(mapMinY, y);
				mapMaxX = std::max(mapMaxX, x);
				mapMaxY = std::max(mapMaxY, y);

				cells.emplace_back(Tile{ x, y, std::move(conv) });
			}
		}
	}

	if (cells.empty()) {
		logger::error("[Unified Water] [Flowmap] No source flow textures found");
		return false;
	}

	const auto width = mapMaxX - mapMinX + 1;
	const auto height = mapMaxY - mapMinY + 1;
	const auto offsetX = -mapMinX;
	const auto offsetY = -mapMinY;

	logger::debug("[Unified Water] [Flowmap] Loaded {} flow textures, creating a {}x{} flow map...", cells.size(), width, height);

	const auto filename = std::format(L"Tamriel-Flowmap.{}.{}.{}.{}.dds", width, height, offsetX, offsetY);
	const auto path = Util::PathHelpers::GetDataPath() / "textures" / "water" / "flowmaps" / filename;

	if (!Compose(cells, width, height, offsetX, offsetY, useMips ? kMipLevels : 1u, path))
		return false;

	const auto t1 = std::chrono::steady_clock::now();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
	logger::info("[Unified Water] [Flowmap] Generated in {} ms from {} tiles, {}x{} cells", ms, cells.size(), width, height);

	return true;
}
