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
 * @brief The engine's vertex and index buffers as ORG buffers (org-full-port.md P4.5, docs/development/p4/p4.5-buffers.md).
 *
 * CS_ORG_ENGINE_BUFFERS=1 turns it on; off, nothing is hooked and the engine's buffers stay D3D11.
 *
 * - Static geometry: the buffers the geometry facade's creators (75473, 75476, 75481) make are ORG buffers in GPUUpload memory,
 *   filled through their mapping. The engine's TriShape fields get a guest DXVK buffer made from each, or a tripwire handle
 *   wrapping it when P4.1's wrapping context is installed. Releases come from any thread and go through the tripwire retirement.
 * - The dynamic ring (75484): an ORG upload ring of persistently mapped chunks behind the engine's map contract. A full chunk is
 *   retired through RenderGraphRuntime::ReleaseAfterD3D11 instead of the engine's event query, so nothing spins. In native mode a full
 *   chunk is retired at the end of the frame it filled in, so every graph execution that reads it was submitted first.
 * - The ring is a late-bound org::VersionedBuffer: each chunk is a version, published when it becomes the current one. Every pass that
 *   reads the ring declares it (DeclareRingRead). The CPU writes into upload memory, so a read needs no barrier.
 *
 * Anything that fails falls back to the engine's D3D11 path for that buffer, or for the ring as a whole.
 */
namespace RenderGraph::Engine::Buffers
{
	/** @brief Whether CS_ORG_ENGINE_BUFFERS=1. Any thread. */
	bool Requested();

	/** @brief Hooks::Install: when requested, detours the geometry creators, the ring's map and its unmaps. */
	void InstallGameHooks();

	/** @brief After the game's device exists (InitD3D): when requested, detours the device's CreateBuffer. */
	void InstallDeviceHooks(ID3D11Device* a_device);

	/** @brief An engine buffer the ORG owns, as a pass reads it directly. */
	struct Direct
	{
		std::uint64_t vkBuffer = 0;  // VkBuffer
		std::uint64_t size = 0;
		std::uint64_t address = 0;   // device address of the first byte; 0 without a graph host
		BufferRef ref;               // the buffer view ref of the first byte, which shaders read through; null without a graph host
	};

	/**
	 * @brief The ORG buffer behind an engine buffer field (a tripwire handle, or a guest DXVK buffer with an ORG buffer attached).
	 * Nullopt when it is a plain D3D11 buffer or the switch is off. a_buffer stays alive during the call. Any thread.
	 */
	std::optional<Direct> Resolve(ID3D11Buffer* a_buffer);

	/** @brief Whether the ORG owns a_buffer (Resolve would answer), without making a ref. Any thread. */
	bool Owns(ID3D11Buffer* a_buffer);

	/**
	 * @brief Graph build: declares a read of the dynamic ring (a late-bound buffer whose versions are its chunks), with no views. Every
	 * pass that reads a ring ref Resolve handed out calls this. Nothing when the switch is off or the ring could not be made.
	 */
	void DeclareRingRead(org::RenderPassBuilder& a_builder);

	/** @brief Since the start. Any thread. */
	struct Stats
	{
		std::uint64_t created = 0, createdBytes = 0;  // static buffers made on ORG
		std::uint64_t fallbacks = 0;                  // static buffers left to D3D11
		std::uint32_t ringChunks = 0;                 // chunks the ring has made
		bool ringActive = false;
	};
	Stats GetStats();
}
