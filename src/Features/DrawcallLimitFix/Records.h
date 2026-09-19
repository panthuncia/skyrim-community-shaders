#pragma once

#include <cstdint>

struct ID3D11Buffer;

namespace DCLF
{
	/**
	 * @brief Per-object record. This is the GPU layout of the object table (16-byte aligned,
	 * no implicit padding), so it can be uploaded as is.
	 */
	struct ObjectRecord
	{
		float world[12];          // row-major 3x4, as Lighting.hlsl's World (translation in column 3)
		float previousWorld[12];  // same layout, from NiAVObject::previousWorld
		float boundCenter[3];     // world-space bounding sphere
		float boundRadius;
		std::uint32_t geometryIndex;
		std::uint32_t materialIndex;
		std::uint32_t pipelineIndex;
		std::uint32_t flags;  // ObjectFlags, alpha-test threshold in bits 8-15
	};
	static_assert(sizeof(ObjectRecord) == 128);
	static_assert(offsetof(ObjectRecord, boundCenter) == 96);
	static_assert(offsetof(ObjectRecord, geometryIndex) == 112);

	enum ObjectFlags : std::uint32_t
	{
		kObjectAlphaTest = 1u << 0,  // alpha test on, reference = threshold / 255 (as the native draw's AlphaTestRef)
		kObjectTwoSided = 1u << 1,   // no culling (the native main pass culls back faces otherwise)
		kObjectAlphaThresholdShift = 8,
	};

	/**
	 * @brief Geometry shared by every object that uses the same BSGraphics::TriShape.
	 * The buffers are the game's; Phase 2 resolves them to Vulkan handles and addresses.
	 */
	struct GeometryRecord
	{
		ID3D11Buffer* vertexBuffer = nullptr;
		ID3D11Buffer* indexBuffer = nullptr;
		std::uint64_t vertexDesc = 0;  // BSGraphics::VertexDesc (stride, attribute offsets, flags)
		std::uint32_t vertexStride = 0;
		std::uint32_t vertexCount = 0;
		std::uint32_t indexCount = 0;
		std::uint32_t firstIndex = 0;
	};

	/** @brief Everything that selects a pipeline: the final shader descriptors and fixed-function state. */
	struct PipelineKey
	{
		std::uint32_t vertexDescriptor = 0;  // after State::ModifyShaderLookup, as the native draw uses
		std::uint32_t pixelDescriptor = 0;
		std::uint32_t rasterFlags = 0;     // PipelineRasterFlags
		std::uint32_t passDescriptor = 0;  // raw technique the Setup* functions read (selects the per-frame constants)

		bool operator==(const PipelineKey&) const = default;
	};

	/**
	 * @brief Fixed-function state of the native main (deferred) pass for eligible objects, as the draw
	 * parity check observed it: depth test EQUAL against the Z-prepass (no write), stencil, blending and
	 * depth bias off, back-face culling unless two-sided.
	 */
	enum PipelineRasterFlags : std::uint32_t
	{
		kRasterTwoSided = 1u << 0,
	};

	/**
	 * @brief Per-object values of the PerGeometry pixel constants (everything else in that group is
	 * per frame). Components the engine does not write for this object hold kUnwrittenBits.
	 */
	struct ObjectShading
	{
		float materialData[4];  // MaterialData: envmap LOD fade, specular LOD fade, alpha
		float emitColor[3];     // EmitColor: emissive colour * emissive multiplier
		float ssrSpecular;      // SSRParams.w: specular LOD fade (0 when the pass disables it)
	};
	static_assert(sizeof(ObjectShading) == 32);

	/**
	 * @brief One indirect draw, in the token order of the DGC layout:
	 * execution-set index, push constant (draw id), index buffer view, DrawIndexed.
	 * The index buffer view has D3D12's IBV layout, which VK_EXT_device_generated_commands
	 * reads with VK_INDIRECT_COMMANDS_INPUT_MODE_DXGI_INDEX_BUFFER_EXT.
	 */
	struct DrawSequence
	{
		std::uint32_t pipelineIndex;
		std::uint32_t drawId;
		std::uint64_t indexBufferAddress;  // filled in Phase 2
		std::uint32_t indexBufferSize;
		std::uint32_t indexFormat;  // DXGI_FORMAT_R16_UINT
		std::uint32_t indexCount;
		std::uint32_t instanceCount;
		std::uint32_t firstIndex;
		std::int32_t vertexOffset;
		std::uint32_t firstInstance;
		std::uint32_t pad;
	};
	static_assert(sizeof(DrawSequence) == 48);
	static_assert(offsetof(DrawSequence, indexBufferAddress) == 8);
	static_assert(offsetof(DrawSequence, indexCount) == 24);
}
