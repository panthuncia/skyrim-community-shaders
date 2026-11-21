#pragma once

#include "RenderGraph/BufferRef.h"

#include <cstdint>
#include <optional>

struct ID3D11Buffer;
struct ID3D11Device;

namespace org
{
	class RenderPassBuilder;
}

/**
* @brief The engine's vertex and index buffers as ORG buffers, in graph mode (Engine::GraphMode).
*
* Static geometry buffers are ORG buffers in GPUUpload memory, and the engine's fields get DXVK buffers made from them.
* The dynamic ring is an ORG upload ring of persistently mapped chunks. A full chunk is retired through ReleaseAfterD3D11
* instead of the engine's event query, so nothing spins. Each chunk is a version of one late-bound buffer, and every pass
* that reads the ring declares it. Buffers made before the graph runs stay D3D11. A failure once it runs is fatal.
*/
namespace RenderGraph::Engine::Buffers
{
	void InstallGameHooks();

	/** @brief After InitD3D. Detours the device's CreateBuffer when requested. */
	void InstallDeviceHooks(ID3D11Device* a_device);

	/** @brief After Present, on the presenting thread. Releases the ended frame's full ring chunks. */
	void EndFrame();

	struct Direct
	{
		std::uint64_t vkBuffer = 0;
		std::uint64_t size = 0;
		std::uint64_t address = 0;   // 0 without a graph host
		BufferRef ref;               // null without a graph host
	};

	/** @brief Nullopt for a plain D3D11 buffer or outside graph mode. a_buffer must stay alive during the call. Any thread. */
	std::optional<Direct> Resolve(ID3D11Buffer* a_buffer);

	/** @brief Like Resolve, without making a ref. Any thread. */
	bool Owns(ID3D11Buffer* a_buffer);

	/** @brief Every pass that reads a ring ref from Resolve must call this at build. */
	void DeclareRingRead(org::RenderPassBuilder& a_builder);

	struct Stats
	{
		std::uint64_t created = 0, createdBytes = 0;
		std::uint64_t fallbacks = 0;
		std::uint32_t ringChunks = 0;
		bool ringActive = false;
	};
	Stats GetStats();
}
