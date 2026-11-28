#pragma once

#include "Features/Screenshot/ORGScreenshot.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace DirectX
{
	struct Image;
}

/**
* @brief A screenshot burst: captures of consecutive rendered frames for the test harness's flicker check (cs-test's `burst`
* step). Each frame is reduced to a grid of cell lumas on the screenshot worker; PNGs are optional. The files go into one folder:
* burst.json (the frames: Present, render frame, snapshot, repeat; the skipped frames; the frame times) and grid.bin.
*/
namespace ScreenshotBurst
{
	// The grid of cell lumas each frame becomes: 8x8 pixel cells at 1920x1080.
	constexpr std::uint32_t kCellsX = 240;
	constexpr std::uint32_t kCellsY = 135;
	// Readback buffers a burst asks the render graph for: a readback takes about eight capture points.
	constexpr std::size_t kRing = 12;

	/** @brief One captured frame. */
	struct Frame
	{
		std::uint32_t index = 0;  // the capture's place in the burst
		ORGScreenshot::Frame frame;
		double timeMs = 0.0;              // the capture's Present, from the burst's first
		std::vector<std::uint16_t> grid;  // kCellsY rows of kCellsX cell lumas, times 256; empty when the frame was lost
	};

	/** @brief A frame the burst did not capture, and why. */
	struct Skip
	{
		std::uint32_t present = 0;
		std::string why;
	};

	/** @brief Shared by the render thread, which asks the captures, and the worker, which reduces them and writes the files. */
	struct Burst
	{
		std::filesystem::path directory;
		std::uint32_t count = 0;
		bool png = true;
		std::uint64_t minSnapshot = 0;

		// Render thread.
		bool started = false;
		std::uint32_t asked = 0;     // captures asked, each one a frame
		std::uint32_t attempts = 0;  // frames since the first capture
		std::uint32_t handed = 0;    // captures handed to the worker
		std::uint32_t lastPresent = ~0u;
		std::uint32_t waited = 0;  // frames waited for the render graph's ring before the first capture
		std::uint32_t idle = 0;    // frames since the last capture was asked, once every one is
		double startMs = 0.0;
		double preFrameMs = 0.0;  // the median frame time of the frames before the burst
		std::string source;
		std::vector<Skip> skipped;

		// Worker.
		std::mutex mutex;
		std::vector<Frame> frames;
		std::uint32_t width = 0, height = 0;
	};

	/** @brief The cell lumas (Rec. 601, 8-bit, times 256) of an 8-bit BGRX or RGBA image. Empty for other formats. */
	std::vector<std::uint16_t> Grid(const DirectX::Image& a_image);

	/** @brief Writes burst.json and grid.bin. Worker. */
	void Write(Burst& a_burst);

	/** @brief Milliseconds on a steady clock. */
	double NowMs();
}
