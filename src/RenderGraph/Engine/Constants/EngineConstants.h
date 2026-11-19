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
* @brief Captures the engine's constant-buffer contents for draws that ORG passes take over, with CS_ORG_ENGINE_CONSTANTS=1.
*
* The engine pools its constant buffers by size and rewrites them with WRITE_DISCARD before each use. So a buffer never
* identifies a shader, and D3D11 only holds the latest use. The hooks track every write and bind. When a consumer withholds a
* draw it calls Capture, which copies the bound buffers into a per-frame upload ring, once per buffer version.
*
* Hooks run on whichever thread drives the immediate context, under Renderer::Lock. Captures and the frame end happen on the
* render owner thread only. A frame's chunks are retired after Present, so a location is only valid within that frame.
* A kept recording must read it from its own latch instead of baking it in.
*/
namespace RenderGraph::Engine::Constants
{
	bool Requested();

	enum class Stage : std::uint8_t
	{
		Vertex,
		Pixel,
		Compute,
	};
	inline constexpr std::uint32_t kStages = 3;
	inline constexpr std::uint32_t kSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;

	constexpr std::uint32_t StageBit(Stage a_stage) { return 1u << static_cast<std::uint32_t>(a_stage); }
	inline constexpr std::uint32_t kDrawStages = StageBit(Stage::Vertex) | StageBit(Stage::Pixel);

	/** @brief Which engine global a buffer is (CreateConstantBufferPools, 75592 / 77400). Other for CS and other plugins. */
	enum class Group : std::uint8_t
	{
		Other,
		Custom,         // the 4 rotating 0xf00-byte buffers of MapCustomConstantBuffer
		AlphaTest,      // PS b11, written by SetDirtyStates
		VSTechnique,    // VS b0 size-class pool
		VSMaterial,     // VS b1
		VSGeometry,     // VS b2
		PSTechnique,    // PS b0
		PSMaterial,     // PS b1
		PSGeometry,     // PS b2
		Compute,        // CS b0, bound by DispatchCSShader
		PerFrame,       // VS/PS b12
		InstanceGroup,  // grass VS b7
	};
	inline constexpr std::uint32_t kGroups = 12;

	struct UseId
	{
		std::uint64_t frame = 0;
		std::uint32_t index = UINT32_MAX;

		bool Valid() const { return index != UINT32_MAX; }
	};

	struct SlotConstants
	{
		Stage stage = Stage::Vertex;
		std::uint8_t slot = 0;
		Group group = Group::Other;
		std::uint8_t poolIndex = 0;  // bytes = poolIndex * 16, or the custom ring's index
		std::uint16_t chunk = 0;
		std::uint32_t offset = 0;    // 16-byte aligned
		std::uint32_t size = 0;
	};

	/** @brief A ByteAddressBuffer at a byte offset. Valid while a_block is held, with this frame's contents until the frame retires. */
	struct Location
	{
		std::shared_ptr<const org::LatchBlock> block;
		std::uint32_t srvIndex = 0;
		std::uint32_t offset = 0;
		std::uint32_t size = 0;
	};

	/** @name The consumer API (main thread). */
	///@{

	/** @brief Requested, the render graph active, and no ring failure. */
	bool Active();

	/** @brief Captures the slots bound now for the stages in a_stageMask. Call after the engine's Setup* calls returned. Invalid when inactive. */
	UseId Capture(std::uint32_t a_stageMask);

	/** @brief In stage then slot order. Empty for a use of another frame. */
	std::span<const SlotConstants> Slots(UseId a_use);

	/** @brief Null when nothing was bound there or the use is of another frame. */
	const SlotConstants* Find(UseId a_use, Stage a_stage, std::uint32_t a_slot);

	/** @brief Host-visible ring memory. Valid this frame. */
	std::span<const std::byte> Bytes(const SlotConstants& a_slot);

	Location Where(const SlotConstants& a_slot);

	/**
	* @brief The engine's CS b0 buffer, possibly a tripwire handle. A consumer that withholds a dispatch must bind it, unwrapped,
	* before Capture, since the withheld DispatchCSShader would have. Null before the pools exist.
	*/
	ID3D11Buffer* EngineComputeBuffer();

	///@}

	/** @name Hooks that feed the capture. Inert unless requested. */
	///@{

	/** @brief Hooks the custom-buffer mapper (75561 / 77362). */
	void InstallGameHooks();

	void InstallD3DHooks(ID3D11DeviceContext* a_context);

	/** @brief Call after a successful Map. */
	void OnMap(ID3D11Resource* a_resource, D3D11_MAP a_mapType, void* a_data);

	/** @brief Call before Unmap runs. */
	void OnUnmap(ID3D11Resource* a_resource);

	/** @brief Call before UpdateSubresource runs. */
	void OnUpdateSubresource(ID3D11Resource* a_resource, const D3D11_BOX* a_box, const void* a_data);

	/** @brief After Present, on the presenting thread. */
	void EndFrame();

	///@}

	/** @brief Totals since the start. Main thread. */
	struct Stats
	{
		std::uint64_t frames = 0;
		std::uint64_t uses = 0;
		std::uint64_t slots = 0;
		std::uint64_t copies = 0;    // versions copied into the ring
		std::uint64_t bytes = 0;
		std::uint64_t unwritten = 0; // bound slots whose buffer was never written since first seen
		std::uint32_t chunks = 0;
	};
	Stats GetStats();
}
