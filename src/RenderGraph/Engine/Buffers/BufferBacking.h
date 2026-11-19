#pragma once

// Shared by the static buffers and the dynamic ring. Only built with the render graph.

#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <winrt/base.h>

namespace org
{
	class Buffer;
	class VersionedBuffer;
}

namespace RenderGraph::Engine::Buffers::detail
{
	/** @brief A persistently mapped ORG buffer in GPUUpload memory, and the DXVK buffer made from it. */
	struct Backing
	{
		// Declared first, so it is released after the guest buffer that reads it.
		std::shared_ptr<org::Buffer> buffer;
		winrt::com_ptr<ID3D11Buffer> guest;
		std::uint8_t* data = nullptr;
	};

	/** @brief Nullopt when the graph is inactive, the memory cannot be had or mapped, or DXVK refuses the wrap. Logged once per kind. Any thread. */
	std::optional<Backing> MakeBacking(const D3D11_BUFFER_DESC& a_desc, const char* a_name);

	/**
	* @brief A tripwire handle while the guest context is installed, else the guest buffer with the ORG buffer attached.
	* Either way the ORG buffer retires after the last reference. Null on failure, and the caller drops the backing. Any thread.
	*/
	winrt::com_ptr<ID3D11Buffer> EngineView(const D3D11_BUFFER_DESC& a_desc, const Backing& a_backing, const char* a_name);

	struct Counters
	{
		std::atomic<std::uint64_t> created{ 0 }, createdBytes{ 0 }, fallbacks{ 0 };
		std::atomic<std::uint32_t> ringChunks{ 0 };
		std::atomic<bool> ringActive{ false };
	};
	Counters& GetCounters();

	/** @brief The map (75484) and the three unmaps. */
	void InstallRingHooks();

	/** @brief Made at the first call. Null, logged once, when it could not be made. Any thread. */
	const std::shared_ptr<org::VersionedBuffer>& RingVersions();

	/** @brief Any thread. */
	bool IsRingChunk(const org::Buffer* a_buffer);

	void ReleaseEndedFrames();
}
