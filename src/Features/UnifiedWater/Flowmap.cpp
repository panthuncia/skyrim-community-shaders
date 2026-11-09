#include "Flowmap.h"
#include "ORGFlowmap.h"

#include "Utils/D3D.h"
#include <DDSTextureLoader.h>
#include <DirectXTex.h>
#include <charconv>
#include <fstream>
#include <optional>

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
	/** @brief FNV-1a of a file's bytes, logged so the D3D11 and ORG paths' flowmaps can be compared. */
	std::optional<uint64_t> HashFile(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
			return std::nullopt;

		uint64_t hash = 14695981039346656037ull;
		std::vector<char> chunk(1 << 20);
		while (file) {
			file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
			const auto count = static_cast<size_t>(file.gcount());
			for (size_t i = 0; i < count; ++i) {
				hash ^= static_cast<uint8_t>(chunk[i]);
				hash *= 1099511628211ull;
			}
		}
		return hash;
	}

	/** @brief Copies the tiles into one texture on a deferred context and saves it. */
	bool ComposeD3D11(const std::vector<ORGFlowmap::Tile>& cells, int32_t width, int32_t height, int32_t offsetX, int32_t offsetY, bool useMips,
		const std::filesystem::path& path)
	{
		auto dvc = globals::d3d::device;
		auto ctx = globals::d3d::context;
		if (!dvc || !ctx) {
			logger::error("[Unified Water] [Flowmap] D3D device/context not available");
			return false;
		}

		winrt::com_ptr<ID3D11DeviceContext> deferredCtx;
		if (FAILED(dvc->CreateDeferredContext(0, deferredCtx.put()))) {
			logger::error("[Unified Water] [Flowmap] Failed to create deferred context");
			return false;
		}

		Util::MultithreadScope guard(ctx);
		if (!guard) {
			logger::error("[Unified Water] [Flowmap] ID3D11Multithread not available");
			return false;
		}

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width * 64;
		desc.Height = height * 64;
		desc.MipLevels = 6;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc = { 1, 0 };
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = 0;

		winrt::com_ptr<ID3D11Texture2D> flowmap;
		if (FAILED(dvc->CreateTexture2D(&desc, nullptr, flowmap.put()))) {
			logger::error("[Unified Water] [Flowmap] Failed to create texture");
			return false;
		}
		Util::SetResourceName(flowmap.get(), "UnifiedWater::Flowmap");

		for (const auto& [x, y, image] : cells) {
			winrt::com_ptr<ID3D11Resource> res;
			auto hr = DirectX::CreateTexture(dvc, image.GetImages(), image.GetImageCount(), image.GetMetadata(), res.put());
			if (FAILED(hr) || !res) {
				logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} creation failed", x, y);
				continue;
			}

			winrt::com_ptr<ID3D11Texture2D> flowTex;
			hr = res->QueryInterface(IID_PPV_ARGS(flowTex.put()));
			if (FAILED(hr)) {
				logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} is not a Texture2D", x, y);
				continue;
			}

			D3D11_TEXTURE2D_DESC srcDesc{};
			flowTex->GetDesc(&srcDesc);

			const UINT sx = static_cast<UINT>(x + offsetX);
			const UINT sy = static_cast<UINT>(y + offsetY);
			const UINT dstX0 = sx * 64;

			const UINT maxMipLevel = useMips ? 6u : 1u;
			for (UINT mipLevel = 0; mipLevel < maxMipLevel; ++mipLevel) {
				const UINT srcSub = D3D11CalcSubresource(mipLevel, 0, srcDesc.MipLevels);
				const UINT dstSub = D3D11CalcSubresource(mipLevel, 0, desc.MipLevels);
				const UINT tileSize = std::max(1u, 64u >> mipLevel);
				const UINT flowmapHeight = std::max(1u, desc.Height >> mipLevel);
				const UINT dstX = dstX0 >> mipLevel;
				const UINT dstY = flowmapHeight - (sy + 1) * tileSize;

				deferredCtx->CopySubresourceRegion(flowmap.get(), dstSub, dstX, dstY, 0, flowTex.get(), srcSub, nullptr);
			}
		}

		winrt::com_ptr<ID3D11CommandList> commandList;
		if (deferredCtx && FAILED(deferredCtx->FinishCommandList(FALSE, commandList.put()))) {
			logger::error("[Unified Water] [Flowmap] FinishCommandList failed");
			return false;
		}

		ctx->ExecuteCommandList(commandList.get(), TRUE);

		const auto hr = Util::SaveTextureToFile(dvc, ctx, path, flowmap.get());
		if (FAILED(hr)) {
			logger::error("[Unified Water] [Flowmap] Failed to save flowmap to {}: hr={:08X}", path.string().c_str(), static_cast<uint32_t>(hr));
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

	auto cells = std::vector<ORGFlowmap::Tile>();
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

				// The checks the D3D11 path made on the created texture, made on the image both paths copy from.
				const auto& d = conv.GetMetadata();
				if (d.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || d.width != 64 || d.height != 64 || d.format != DXGI_FORMAT_B8G8R8A8_UNORM || d.mipLevels < 6) {
					logger::warn("[Unified Water] [Flowmap] Flow texture at {},{} is invalid", x, y);
					continue;
				}

				mapMinX = std::min(mapMinX, x);
				mapMinY = std::min(mapMinY, y);
				mapMaxX = std::max(mapMaxX, x);
				mapMaxY = std::max(mapMaxY, y);

				cells.emplace_back(ORGFlowmap::Tile{ x, y, std::move(conv) });
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

	bool written = false;
	const char* pathName = "D3D11";
	if (ORGFlowmap::Requested()) {
		written = ORGFlowmap::Compose(cells, width, height, offsetX, offsetY, useMips ? 6u : 1u, path);
		if (written)
			pathName = "ORG";
		else
			logger::warn("[Unified Water] [Flowmap] [ORG] Composing failed, falling back to D3D11");
	}
	if (!written)
		written = ComposeD3D11(cells, width, height, offsetX, offsetY, useMips, path);
	if (!written)
		return false;

	const auto t1 = std::chrono::steady_clock::now();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
	const auto hash = HashFile(path);
	logger::info("[Unified Water] [Flowmap] Generated in {} ms ({}), {}x{} cells, file hash {:016X}", ms, pathName, width, height, hash.value_or(0));

	return true;
}
