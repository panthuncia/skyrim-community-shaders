#pragma once

#include <cstdint>
#include <type_traits>

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
		kObjectSuppressExternalEmittance = 1u << 2,  // ExtraShaderDescriptors::SuppressExternalEmittance in the permutation buffer
		// The engine's main-camera accumulator holds this object this frame, so its own culling (frustum,
		// occlusion planes, rooms and portals) kept it. The tables carry the whole tracked set so that the
		// GPU culling has a real input to reject from; this bit is what tells the two apart, and it is the
		// reference the culling is measured against (BuildDrawsCS: RequireNativeVisible).
		kObjectNativeVisible = 1u << 3,
		// The object is a culling candidate only: it cannot be drawn this frame, so no material or
		// pipeline entry was built for it and its materialIndex and pipelineIndex mean nothing. Anything
		// that indexes the tables with them must check this first - the tables can be empty entirely (the
		// first frame after a teleport has tracked geometry but nothing accumulated yet), so even index 0
		// is not safe.
		kObjectNoBindings = 1u << 5,
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
		// Phase 2: the buffers' device addresses (GpuResources), 0 when the render graph is off.
		std::uint64_t vertexAddress = 0;
		std::uint64_t indexAddress = 0;
		std::uint64_t vertexBytes = 0;
		std::uint64_t indexBytes = 0;
	};

	/**
	 * @brief Per-object light assignment Light Limit Fix passes per draw (StrictLightData, PS b3). In the
	 * main pass there are no strict lights (NumStrictLights is 0); what varies is the object's room in
	 * interiors and the shadow mask channels of its shadow-casting lights.
	 */
	struct ObjectLights
	{
		std::int32_t roomIndex = -1;
		std::uint32_t shadowBitMask = 0;
	};

	/**
	 * @brief Community Shaders' permutation buffer (State::PermutationCB, b4) for a pipeline, as
	 * BeginTechnique and the SetupGeometry hooks leave it for the draw. Per-object bits
	 * (kObjectSuppressExternalEmittance) are added on top of extraShaderDescriptor.
	 */
	struct PipelinePermutation
	{
		std::uint32_t vertexShaderDescriptor = 0;  // descriptor BeginTechnique receives
		std::uint32_t pixelShaderDescriptor = 0;   // received pixel descriptor bits the shader lookup dropped
		std::uint32_t extraShaderDescriptor = 0;
		std::uint32_t extraFeatureDescriptor = 0;
	};

	/** @brief Everything that selects a pipeline: the final shader descriptors and fixed-function state. */
	struct PipelineKey
	{
		std::uint32_t vertexDescriptor = 0;  // after State::ModifyShaderLookup, as the native draw uses
		std::uint32_t pixelDescriptor = 0;
		std::uint32_t rasterFlags = 0;     // PipelineRasterFlags
		std::uint32_t passDescriptor = 0;  // raw technique the Setup* functions read (selects the per-frame constants)
		std::uint64_t vertexLayout = 0;    // geometry's BSGraphics::VertexDesc without the stride (VertexInput.h)

		bool operator==(const PipelineKey&) const = default;
	};
	static_assert(std::has_unique_object_representations_v<PipelineKey>);

	struct PipelineKeyHash
	{
		using is_avalanching = void;
		std::uint64_t operator()(const PipelineKey& a_key) const noexcept
		{
			return ankerl::unordered_dense::detail::wyhash::hash(&a_key, sizeof(a_key));
		}
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
	 * @brief One indirect draw, in the argument order of the command signature (BasicRHI packs arguments
	 * like D3D12, 4-byte aligned): pipeline set index, push data (the DrawBindings record's address),
	 * vertex buffer view, index buffer view (D3D12 VBV / IBV layouts), DrawIndexed.
	 *
	 * The scene tables hold the geometry part with the pipeline's table index; the main-pass epoch
	 * replaces it with the pipeline set index and fills in the record address.
	 */
#pragma pack(push, 4)
	struct DrawSequence
	{
		std::uint32_t pipelineIndex;
		std::uint64_t bindingsAddress;
		// The object's index in SceneStore::Tables::objects, pushed as the third root constant word so the
		// shaders can reach per-object data without it travelling in the binding record. The three words
		// are one contiguous Constant indirect argument, so this must stay adjacent to bindingsAddress.
		std::uint32_t objectIndex;
		std::uint64_t vertexBufferAddress;  // GeometryRecord::vertexAddress (0 without the render graph)
		std::uint32_t vertexBufferSize;
		std::uint32_t vertexStride;
		std::uint64_t indexBufferAddress;  // GeometryRecord::indexAddress
		std::uint32_t indexBufferSize;
		std::uint32_t indexFormat;  // DXGI_FORMAT_R16_UINT
		std::uint32_t indexCount;
		std::uint32_t instanceCount;
		std::uint32_t firstIndex;
		std::int32_t vertexOffset;
		std::uint32_t firstInstance;
	};
#pragma pack(pop)
	static_assert(sizeof(DrawSequence) == 68);
	static_assert(offsetof(DrawSequence, bindingsAddress) == 4);
	static_assert(offsetof(DrawSequence, objectIndex) == 12);
	static_assert(offsetof(DrawSequence, vertexBufferAddress) == 16);
	static_assert(offsetof(DrawSequence, indexBufferAddress) == 32);
	static_assert(offsetof(DrawSequence, indexCount) == 48);
}
