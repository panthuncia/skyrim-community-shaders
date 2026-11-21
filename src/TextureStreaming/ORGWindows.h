#pragma once

#include "TextureStreaming/Streamer.h"

#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/**
* @brief Texture streaming's windows as ORG textures, with CS_ORG_TEXTURE_STREAMING=1.
*
* A window is an image from ORG's allocator, wrapped for D3D11 so the shell can name it, and imported so DCLF binds it by index.
* Its mips are the wrap's initial data, which DXVK writes in its own stream. So frames never wait and the graph has no pass for them.
* A worker builds and wraps the window, and the render thread installs it after the next Present.
* On any failure the backend turns off and every window is rebuilt through D3D11, which stays the default.
*/
namespace TextureStreaming::ORG
{
	/** @brief Render thread, after each Present. Releases let-go windows and installs new ones. */
	void FrameTick();

	/** @brief Any thread. Requested, the graph is up and wraps with data, and nothing failed. */
	bool Available();

	/** @brief Held by the texture that shows it and by each DCLF import of it. */
	class Window;

	/** @brief The shell redirects the engine's binds here. Lives as long as a_window. */
	ID3D11ShaderResourceView* ViewOf(const Window& a_window);

	/** @brief For a bindless read. Lives as long as a_window. */
	std::uint32_t IndexOf(const Window& a_window);

	struct Upload
	{
		D3D11_TEXTURE2D_DESC desc{};               // its top mip's size and mip count
		std::string path;                          // for names and the owner label
		std::vector<D3D11_SUBRESOURCE_DATA> mips;  // each mip's bytes, only read during the call
		/** @brief Render thread. The mips are in DXVK's stream, so the window may be shown from here on. */
		std::function<void(const std::shared_ptr<Window>& a_window)> install;
		/** @brief Render thread. The graph could not make the window, so rebuild on D3D11. */
		std::function<void()> retry;
	};

	/**
	* @brief On a window build's thread. Queues the window for the next FrameTick.
	* @return Done once installed or retried. Null when the graph cannot take it, and the caller builds on D3D11.
	*/
	std::shared_ptr<PendingWindow> Queue(Upload a_upload);

	/** @brief For the reports. */
	struct Counters
	{
		std::uint64_t made = 0;
		std::uint64_t installFrames = 0;
		std::uint64_t retried = 0;
	};
	Counters GetCounters();
}
