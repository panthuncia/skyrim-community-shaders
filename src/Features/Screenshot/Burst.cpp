#include "Burst.h"

#include <DirectXTex.h>

#include <algorithm>
#include <chrono>
#include <fstream>

namespace ScreenshotBurst
{
	std::vector<std::uint16_t> Grid(const DirectX::Image& a_image)
	{
		std::vector<std::uint16_t> grid;
		std::size_t r = 0, b = 2;
		switch (a_image.format) {
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			r = 2;
			b = 0;
			break;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			break;
		default:
			return grid;
		}
		if (a_image.width < kCellsX || a_image.height < kCellsY || !a_image.pixels)
			return grid;
		grid.resize(std::size_t(kCellsX) * kCellsY);
		std::vector<std::uint64_t> sums(kCellsX);
		std::vector<std::size_t> xs(kCellsX + 1);
		for (std::uint32_t x = 0; x <= kCellsX; ++x)
			xs[x] = x * a_image.width / kCellsX;
		for (std::uint32_t cy = 0; cy < kCellsY; ++cy) {
			const std::size_t y0 = cy * a_image.height / kCellsY, y1 = (cy + 1) * a_image.height / kCellsY;
			std::ranges::fill(sums, 0);
			for (std::size_t y = y0; y < y1; ++y) {
				const auto* row = a_image.pixels + y * a_image.rowPitch;
				for (std::uint32_t cx = 0; cx < kCellsX; ++cx) {
					std::uint64_t sum = 0;
					for (std::size_t x = xs[cx]; x < xs[cx + 1]; ++x) {
						const auto* p = row + x * 4;
						// Rec. 601 weights in 1/1000.
						sum += 299u * p[r] + 587u * p[1] + 114u * p[b];
					}
					sums[cx] += sum;
				}
			}
			for (std::uint32_t cx = 0; cx < kCellsX; ++cx) {
				const auto pixels = (y1 - y0) * (xs[cx + 1] - xs[cx]);
				grid[std::size_t(cy) * kCellsX + cx] = static_cast<std::uint16_t>(std::min<std::uint64_t>(65535, sums[cx] * 256 / (1000 * pixels)));
			}
		}
		return grid;
	}

	void Write(Burst& a_burst)
	{
		std::scoped_lock lock(a_burst.mutex);
		auto& frames = a_burst.frames;
		std::ranges::sort(frames, {}, &Frame::index);
		std::error_code error;
		std::filesystem::create_directories(a_burst.directory, error);
		json out;
		out["version"] = 1;
		out["count"] = a_burst.count;
		out["width"] = a_burst.width;
		out["height"] = a_burst.height;
		out["cells"] = { kCellsX, kCellsY };
		out["source"] = a_burst.source;
		out["png"] = a_burst.png;
		out["pre_frame_ms"] = a_burst.preFrameMs;
		out["waited_frames"] = a_burst.waited;
		json list = json::array();
		std::ofstream grid(a_burst.directory / "grid.bin", std::ios::binary | std::ios::trunc);
		std::uint32_t written = 0;
		for (const auto& frame : frames) {
			const bool kept = frame.grid.size() == std::size_t(kCellsX) * kCellsY;
			list.push_back({ { "index", frame.index },
				{ "present", frame.frame.present },
				{ "render_frame", frame.frame.renderFrame },
				{ "snapshot", frame.frame.snapshot },
				{ "renders_of_snapshot", frame.frame.rendersOfSnapshot },
				{ "repeat", frame.frame.repeat },
				{ "t_ms", frame.timeMs },
				{ "grid", kept ? static_cast<int>(written) : -1 } });
			if (kept) {
				grid.write(reinterpret_cast<const char*>(frame.grid.data()), static_cast<std::streamsize>(frame.grid.size() * sizeof(std::uint16_t)));
				++written;
			}
		}
		out["frames"] = std::move(list);
		json skipped = json::array();
		for (const auto& skip : a_burst.skipped)
			skipped.push_back({ { "present", skip.present }, { "why", skip.why } });
		out["skipped"] = std::move(skipped);
		std::ofstream(a_burst.directory / "burst.json", std::ios::trunc) << out.dump(1);
		logger::info("[TEST] burst {}: {} of {} frames written ({} with a grid), {} skipped", a_burst.directory.filename().string(), frames.size(), a_burst.count,
			written, a_burst.skipped.size());
	}

	double NowMs()
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
}
