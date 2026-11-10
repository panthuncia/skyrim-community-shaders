#pragma once

// Shared by the static buffers and the dynamic ring (EngineBuffers.h). Only built with the render graph.

#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <winrt/base.h>

namespace org
{
	class Buffer;
}

namespace RenderGraph::Engine::Buffers::detail
{
	/** @brief An ORG buffer in GPUUpload memory, persistently mapped, and the guest DXVK buffer made from it. */
	struct Backing
	{
		// Declared first, so it is released after the guest buffer that reads it.
		std::shared_ptr<org::Buffer> buffer;
		winrt::com_ptr<ID3D11Buffer> guest;
		std::uint8_t* data = nullptr;
	};

	/**
	 * @brief A new backing of a_desc.ByteWidth bytes, its guest buffer bound as a_desc.BindFlags (DEFAULT usage, no CPU access).
	 * Nullopt, the first failure of each kind logged, when the render graph is not active, the memory cannot be had or mapped, or
	 * DXVK refuses the wrap. Any thread.
	 */
	std::optional<Backing> MakeBacking(const D3D11_BUFFER_DESC& a_desc, const char* a_name);

	/**
	 * @brief What the engine's field holds for a backing: a tripwire handle wrapping the guest buffer while P4.1's wrapping context is
	 * the engine's (Tripwire::GuestContextInstalled), else the guest buffer with the ORG buffer attached (Tripwire::AttachGraphResource).
	 * Either way the ORG buffer is retired through Tripwire::ServiceRetired once the last reference goes. Null when the ORG buffer
	 * could not be tied to it (the caller then drops the backing). Any thread.
	 */
	winrt::com_ptr<ID3D11Buffer> EngineView(const D3D11_BUFFER_DESC& a_desc, const Backing& a_backing, const char* a_name);

	/** @brief The counters behind Buffers::GetStats. */
	struct Counters
	{
		std::atomic<std::uint64_t> created{ 0 }, createdBytes{ 0 }, fallbacks{ 0 };
		std::atomic<std::uint32_t> ringChunks{ 0 };
		std::atomic<bool> ringActive{ false };
	};
	Counters& GetCounters();

	/** @brief InstallGameHooks' part for the ring: the map (75484) and the three unmaps. */
	void InstallRingHooks();
}
