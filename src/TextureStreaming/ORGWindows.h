#pragma once

#include "TextureStreaming/Streamer.h"

#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief Texture streaming's windows as ORG textures (org-full-port.md P4.4, CS_ORG_TEXTURE_STREAMING=1).
 *
 * A window is an image ORG's allocator makes (owner "Game/Textures/<folder>", spillable), wrapped for D3D11 by the DXVK fork so
 * the shell can still name it for the engine's binds, and imported into the graph so Drawcall Limit Fix binds its own descriptor
 * by index. Its mips are the wrap's initial data: DXVK writes them in its own stream, so the frames never wait and the graph has no
 * pass for them. (A one-shot copy pass per batch, registered as a client of the Present anchor, rebuilt that anchor's graph twice
 * per batch: over 200 times in a minute of loading.)
 *
 * 1. A window build (a worker) reads the file, makes the image, and wraps it with its mips for D3D11 (Queue).
 * 2. The render thread, after each Present (FrameTick), installs the windows made since: the shells name them, and each build's
 *    completion (PendingWindow) is done, which is what opens Drawcall Limit Fix's gates.
 *
 * A window that is let go is released on the render thread after D3D11's last use of it (RenderGraphRuntime::ReleaseAfterD3D11).
 * When anything here fails, the backend turns itself off: every window it holds is built again through the D3D11 path, which
 * stays the default and the fallback. Synchronous builds (the safety net, an import that finds no window) always use D3D11.
 */
namespace TextureStreaming::ORG
{
	/** @brief Whether CS_ORG_TEXTURE_STREAMING=1. Read once. */
	bool Requested();

	/** @brief Render thread, after each Present: releases let-go windows and installs the ones made since. */
	void FrameTick();

	/** @brief Any thread: whether window builds go through the graph now (requested, the graph is up and wraps with data, nothing failed). */
	bool Available();

	/** @brief A window the graph owns. Held by the texture that shows it, and by each Drawcall Limit Fix import of it. */
	class Window;

	/** @brief The window's D3D11 view, which the shell redirects the engine's binds to. Lives as long as a_window. */
	ID3D11ShaderResourceView* ViewOf(const Window& a_window);

	/** @brief The window's descriptor in ORG's resource heap, for a bindless read. Lives as long as a_window. */
	std::uint32_t IndexOf(const Window& a_window);

	/** @brief What a window build hands over. */
	struct Upload
	{
		D3D11_TEXTURE2D_DESC desc{};               // the window's: its top mip's size, its mip count
		std::string path;                          // the file, for names and the owner label
		std::vector<D3D11_SUBRESOURCE_DATA> mips;  // each mip's bytes, read before the call; only read during it
		/** @brief Render thread: the mips are in DXVK's stream, so the window may be shown from here on. */
		std::function<void(const std::shared_ptr<Window>& a_window)> install;
		/** @brief Render thread: the graph could not make the window. The build is to be made again, on D3D11. */
		std::function<void()> retry;
	};

	/**
	 * @brief A window build's thread: makes the image and wraps it with the mips, and queues the window for the next FrameTick.
	 * @return The build's completion, done once the window is installed or retried. Null when the graph cannot take it (not
	 * Available, or the image or its wrap could not be made): the caller builds on D3D11 instead.
	 */
	std::shared_ptr<PendingWindow> Queue(Upload a_upload);

	/** @brief For the reports: windows the graph made, the frames that installed them, and builds it gave back to D3D11. */
	struct Counters
	{
		std::uint64_t uploaded = 0;
		std::uint64_t batches = 0;
		std::uint64_t retried = 0;
	};
	Counters GetCounters();
}
