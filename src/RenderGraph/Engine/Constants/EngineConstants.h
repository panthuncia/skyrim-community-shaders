#pragma once

#include <d3d11.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace org
{
	class LatchBlock;
}

/**
 * @brief The engine's constant-buffer uses a consumer asks for, captured into graph memory (org-full-port.md P5.7,
 * docs/development/p5/constants.md).
 *
 * The engine's constant buffers are pooled by size and rewritten with WRITE_DISCARD before each use, so a buffer pointer never
 * identifies a shader, and the D3D11 buffer only holds the latest use. With CS_ORG_ENGINE_CONSTANTS=1 the capture tracks every
 * constant buffer's writes and binds, and a consumer that withholds an engine draw (an ORG pass that draws the engine's work, at its
 * own choke point once the draw's constants are bound) calls Capture: the bytes of every constant buffer bound to the asked stages
 * are copied into a per-frame upload ring of LatchBlocks, once per buffer version (Map or UpdateSubresource) and bound window
 * however many captures use them. Nothing is copied for draws nobody asked about. The UseId Capture returns names that use; the
 * consumer asks for its constants by stage and slot, and reads them from the ring.
 *
 * The engine keeps writing and binding its D3D11 buffers as before (until P5.13): the capture only reads.
 *
 * What is tracked, and how (immediate context only):
 * - Map, Unmap and UpdateSubresource of constant buffers (the Globals.cpp hooks call OnMap, OnUnmap, OnUpdateSubresource).
 * - VS/PS/CS SetConstantBuffers and SetConstantBuffers1 (with their windows), and ClearState: the bound buffer of every slot.
 * - The engine's custom-buffer mapper (MapCustomConstantBuffer, 75561 / 77362): ImageSpace b2, grass b7/b8, bones b9/b10, Effect
 *   VS b11. Its size argument bounds how much of the 0xf00-byte buffer a use copies.
 *
 * Threads: the hooks run on whichever thread drives the immediate context (Renderer::Lock serializes them). Uses are captured, and
 * the frame ends, on the render thread only (FrameAnchors::IsRenderOwner): the main thread, or with CS_ORG_LOADING_THREAD=1 the
 * thread rendering under Renderer::Lock (a loading screen, the startup logo).
 * The consumer API is main-thread only.
 *
 * Lifetime: a frame's ring chunks are retired at the end of the frame (after the Present, through
 * RenderGraphRuntime::ReleaseAfterD3D11), so they are reused only once the GPU is past every D3D11 command issued until then, which
 * includes every anchor's epoch of that frame. A use's location is valid for passes recorded in the same frame. A kept recording
 * must not bake it: it reads the block's SRV index and the offset from its own latch, written at commit.
 */
namespace RenderGraph::Engine::Constants
{
	/** @brief Whether CS_ORG_ENGINE_CONSTANTS=1. Any thread. */
	bool Requested();

	/** @brief The shader stage of a constant-buffer slot. */
	enum class Stage : std::uint8_t
	{
		Vertex,
		Pixel,
		Compute,
	};
	inline constexpr std::uint32_t kStages = 3;
	/** @brief D3D11's constant-buffer slots per stage. */
	inline constexpr std::uint32_t kSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;

	/** @brief A Capture mask bit. */
	constexpr std::uint32_t StageBit(Stage a_stage) { return 1u << static_cast<std::uint32_t>(a_stage); }
	inline constexpr std::uint32_t kDrawStages = StageBit(Stage::Vertex) | StageBit(Stage::Pixel);

	/**
	 * @brief Which engine global a buffer is (BSGraphics::Renderer::CreateConstantBufferPools, 75592 / 77400), or Other for CS's
	 * and other plugins' buffers.
	 */
	enum class Group : std::uint8_t
	{
		Other,
		Custom,         // the 4 rotating 0xf00-byte buffers of MapCustomConstantBuffer
		AlphaTest,      // PS b11, written by SetDirtyStates
		VSTechnique,    // VS b0 size-class pool
		VSMaterial,     // VS b1 pool
		VSGeometry,     // VS b2 pool
		PSTechnique,    // PS b0 pool
		PSMaterial,     // PS b1 pool
		PSGeometry,     // PS b2 pool
		Compute,        // CS b0 (SetComputeShaderConstants, bound by DispatchCSShader)
		PerFrame,       // VS/PS b12
		InstanceGroup,  // grass VS b7 (MapCustomConstantBuffer type 7)
	};
	inline constexpr std::uint32_t kGroups = 12;

	/** @brief One draw or dispatch the capture saw: the capture frame it belongs to and its index in that frame. */
	struct UseId
	{
		std::uint64_t frame = 0;
		std::uint32_t index = UINT32_MAX;

		bool Valid() const { return index != UINT32_MAX; }
	};

	/** @brief One bound slot's constants for one use. */
	struct SlotConstants
	{
		Stage stage = Stage::Vertex;
		std::uint8_t slot = 0;
		Group group = Group::Other;
		std::uint8_t poolIndex = 0;  // the size class within its pool (bytes = poolIndex * 16), or the custom ring's index
		std::uint16_t chunk = 0;     // the frame's ring chunk
		std::uint32_t offset = 0;    // in the chunk, 16-byte aligned
		std::uint32_t size = 0;      // bytes of the bound window
	};

	/**
	 * @brief Where a slot's bytes are for a pass: a ByteAddressBuffer (the chunk's SRV) at a byte offset. The block stays valid while
	 * a_block is held; its contents are this frame's until the frame retires.
	 */
	struct Location
	{
		std::shared_ptr<const org::LatchBlock> block;
		std::uint32_t srvIndex = 0;
		std::uint32_t offset = 0;
		std::uint32_t size = 0;
	};

	/** @name The consumer API (main thread). */
	///@{

	/** @brief Whether the capture runs: requested, the render graph active, and no ring failure. */
	bool Active();

	/**
	 * @brief Captures the slots bound now for the stages in a_stageMask (StageBit) as a new use, for a consumer that withholds the
	 * engine's draw at its own choke point (the bindings are the engine's once its Setup* calls returned). Invalid when inactive.
	 */
	UseId Capture(std::uint32_t a_stageMask);

	/** @brief Every slot of a use, in stage then slot order; empty for a use of another frame. */
	std::span<const SlotConstants> Slots(UseId a_use);

	/** @brief The constants a use bound at a_stage's slot a_slot; null when nothing was bound there or the use is of another frame. */
	const SlotConstants* Find(UseId a_use, Stage a_stage, std::uint32_t a_slot);

	/** @brief A captured slot's bytes, on the CPU (host-visible ring memory). Valid this frame. */
	std::span<const std::byte> Bytes(const SlotConstants& a_slot);

	/** @brief A captured slot's location for a pass's shader. */
	Location Where(const SlotConstants& a_slot);

	///@}

	/** @name Seams (the hooks that feed the capture). Inert unless requested. */
	///@{

	/** @brief Hooks::Install: the custom-buffer mapper (75561 / 77362). */
	void InstallGameHooks();

	/** @brief globals::InstallD3DHooks: the immediate context's constant-buffer binds and ClearState. */
	void InstallD3DHooks(ID3D11DeviceContext* a_context);

	/** @brief The immediate context's Map, after it succeeded. */
	void OnMap(ID3D11Resource* a_resource, D3D11_MAP a_mapType, void* a_data);

	/** @brief The immediate context's Unmap, before it runs. */
	void OnUnmap(ID3D11Resource* a_resource);

	/** @brief The immediate context's UpdateSubresource, before it runs. */
	void OnUpdateSubresource(ID3D11Resource* a_resource, const D3D11_BOX* a_box, const void* a_data);

	/** @brief After the Present, on the presenting thread: the frame's chunks are retired, the uses cleared, the summary logged. */
	void EndFrame();

	///@}

	/** @brief Totals since the start. Main thread. */
	struct Stats
	{
		std::uint64_t frames = 0;
		std::uint64_t uses = 0;      // uses captured (Capture calls)
		std::uint64_t slots = 0;     // bound slots recorded
		std::uint64_t copies = 0;    // versions copied into the ring
		std::uint64_t bytes = 0;     // bytes copied
		std::uint64_t unwritten = 0; // bound slots whose buffer was never written since it was first seen
		std::uint32_t chunks = 0;    // ring chunks made
	};
	Stats GetStats();
}
