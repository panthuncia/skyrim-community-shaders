#pragma once

#include "RenderGraph/BufferRef.h"

// GPU layout of the TreeInstances draw family: records, rows and the list header its draws read. TreeLodCullCS.hlsl and
// DistantTree.hlsl mirror it, so it stays compile-time. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Gpu/OwnedRows.h"

#include <array>
#include <cstdint>

namespace DCLF::TreeInstances
{
	// Each distant tree group is one instanced shape whose records the refill (FUN_140504a40) packs, at most 75, into 32-byte records.
	inline constexpr std::uint32_t kMaxGroupInstances = 75;  // the refill's cap (0x4B), and the property's (+0x88)
	inline constexpr std::uint32_t kInstanceShorts = 16;     // shorts per instance record (AddGroup's p2)

	/**
	 * @brief One instance record as the refill packs it, halfs: position (block-relative), scale (0 when hidden), cos and sin of
	 * rotZ, alpha, 1.0, then zero. DistantTree.hlsl reads it as TEXCOORD4 and TEXCOORD5.
	 */
	struct Instance
	{
		std::array<std::uint16_t, kInstanceShorts> shorts{};
		bool operator==(const Instance&) const = default;
	};
	static_assert(sizeof(Instance) == 32);

	// The GPU's tables: a shape row and 75 instance records per shape slot, a mesh row per mesh slot, one draw row, and the
	// visible list (header, visible instance indices, then the retest list for phase 2).
	struct ShapeRow
	{
		float translate[3]{};
		std::uint32_t mesh = 0;
		std::uint32_t count = 0;  // the slot's records; 0 for a free slot
		float extent = 0.0f;      // the type's: an instance's bound is its scale times this, about its position
		std::uint32_t pad[2]{};
	};
	static_assert(sizeof(ShapeRow) == 32);

	struct MeshRow
	{
		RenderGraph::BufferRef vertexRef;
		RenderGraph::BufferRef indexRef;  // R16 indices
		std::uint32_t vertexStride = 0;
		std::uint32_t indexCount = 0;
		std::uint32_t texcoordOffset = 0;  // TEXCOORD0's, a half2; the position is a float4 at 0 (VertexInput.cpp)
		std::uint32_t pad = 0;
	};
	static_assert(sizeof(MeshRow) == 32);

	struct DrawRow
	{
		RenderGraph::BufferRef instances, shapes, meshes, visible;  // visible: the list's indices (past the arguments)
		DescriptorIndex textureIndex, samplerIndex;  // OwnedRow's (TreeLodTextureBinding)
		float alphaRef = 0.0f;
		std::uint32_t shapeSlots = 0;
		std::uint32_t maxIndices = 0;
		RenderGraph::BufferRef stats;
		std::uint32_t frame = 0;
	};
	static_assert(sizeof(DrawRow) == 64);
	/**
	 * @brief The visible list's header: three DrawInstanced argument sets (phase 1 depth, phase 2 depth, colour) and the retest count.
	 * The depth commit writes it with no instances.
	 */
	struct VisibleHeader
	{
		std::uint32_t phaseOne[4]{}, phaseTwo[4]{}, colour[4]{};
		std::uint32_t retests = 0, pad[3]{};
	};
	static_assert(sizeof(VisibleHeader) == 64);
	inline constexpr std::uint32_t kVisibleHeaderWords = sizeof(VisibleHeader) / 4;
	/** @brief CS_DCLF_TREE_LOD_STAGES' stamps: kStageSlots rings of kStageWords words, each stage writing the draw row's frame. */
	inline constexpr std::uint32_t kStageSlots = 8, kStageWords = 32;
	enum StageWord : std::uint32_t
	{
		kStageDepthVS = 0,
		kStagePhaseTwoVS = 1,
		kStageDepthPS = 2,
		kStageColourVS = 3,
		kStageColourPS = 4,
		kStageColourSawPhaseOne = 5,
		kStageColourSawPhaseTwo = 6,
		kStageColourSawColour = 7,
		kStageDepthClip = 8,
		kStageColourClip = 12,
		kStageColourEntry = 16,
		kStageDepthEntry = 17,
		kStageColourProbe = 18,
		kStageDepthProbe = 25,
		kStageProbeWords = 7,
	};

	/** @brief Whether a geometry's shader property is a BSDistantTreeShaderProperty. Any thread. */
	inline bool IsEngineShape(const RE::BSGeometry* a_shape)
	{
		const auto* property = a_shape ? a_shape->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		static const std::uintptr_t treeVtable = RE::VTABLE_BSDistantTreeShaderProperty[0].address();
		return Engine::VtableIs(property, treeVtable);
	}
}
