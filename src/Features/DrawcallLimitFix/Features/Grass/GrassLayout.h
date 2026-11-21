#pragma once

#include "RenderGraph/BufferRef.h"

// GPU layout of the grass draw family (dclf-architecture.md, "Grass: the draws").
// GrassCullCS.hlsl, GrassArgsCS.hlsl and RunGrass.hlsl's DCLF_PULLED path mirror these structs byte for byte. Change both together.

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Gpu/OwnedRows.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace DCLF::GrassInstances
{
	inline constexpr std::uint32_t kInstanceShorts = 16;

	/**
	 * @brief One engine instance record, 16 halfs. [0..2] position relative to the shape origin, [3] colour scale, [13] scale variance.
	 * The rotation rows are (h4, h5, h6), (h8, h9, h10) and (h12, h7, h11).
	 */
	struct Instance
	{
		std::array<std::uint16_t, kInstanceShorts> shorts{};
		bool operator==(const Instance&) const = default;
	};
	static_assert(sizeof(Instance) == 32);

	struct ShapeRow
	{
		float origin[3]{};  // the shape's world translation at capture
		std::uint32_t bucket = 0;
		std::uint32_t firstInstance = 0;
		std::uint32_t count = 0;          // 0 when free or not uploaded
		float spawnStart = 0.0f;          // fade clock when the shape arrived, for the spawn fade-in
		std::uint32_t flags = 0;
	};
	static_assert(sizeof(ShapeRow) == 32);

	/** @brief Indices are R16, read as words. */
	struct MeshRow
	{
		RenderGraph::BufferRef vertexRef;
		RenderGraph::BufferRef indexRef;
		std::uint64_t vertexDesc = 0;  // the engine's vertex descriptor
		std::uint32_t indexCount = 0;
		std::uint32_t vertexStride = 0;
	};
	static_assert(sizeof(MeshRow) == 32);

	// Tier 0 is the full mesh. Tiers 1 and 2 are Grass Optimizations' _LOD0 and _LOD1 meshes.
	inline constexpr std::uint32_t kTiers = 3;
	inline constexpr std::uint32_t kNoMesh = ~0u;
	inline constexpr std::uint32_t kBucketTruePbr = 1u << 0;       // drawn by the second colour program
	inline constexpr std::uint32_t kBucketSphereNormal = 1u << 1;
	inline constexpr std::uint32_t kBucketTierOne = 1u << 2;
	inline constexpr std::uint32_t kBucketTierTwo = 1u << 3;

	struct BucketRow
	{
		std::uint32_t mesh[kTiers]{ kNoMesh, kNoMesh, kNoMesh };
		std::uint32_t flags = 0;
		// Tiers 0 and 1 fill upward from their base. Tier 2 shares tier 1's range and fills downward from its end.
		std::uint32_t visibleBase[kTiers]{};
		std::uint32_t instanceCount = 0;  // capacity of each visible range. 0 for a free or not yet drawable slot.
		DescriptorIndex diffuseIndex, depthSampler, colourSampler;
		float alphaRef = 0.0f;
		float boundCenter[3]{};
		float modelRadius = 0.0f;
		float wavePeriod = 0.0f;
		float distScale = 1.0f, minPixelScale = 1.0f;  // tier 0 mesh cost bias: 1/sqrt(cost) and sqrt(cost)
		std::uint32_t pbrFlags = 0;
		float scaleMask[3]{};
		std::uint32_t pad = 0;
		DescriptorIndex normalIndex, rmaosIndex, featuresIndex, pbrSampler;
		float pbrParams1[4]{};  // roughness scale, specular level, emissive scale, 0
		float pbrParams2[4]{};  // subsurface colour rgb, opacity
		float windDirection[2]{ 0.0f, 1.0f };  // WindVector.xy, the shapes' model Y axis in world space
		float lodMeshCost[2]{ 1.0f, 1.0f };    // sqrt(cost) of tiers 1 and 2
		float pbrParams3[4]{};                 // fuzz colour rgb, weight
		float pbrParams4[4]{};                 // subsurface balance, front and back specular weights, back roughness scale
		DescriptorIndex fuzzIndex, emissiveIndex;
		float canopyHeight[kTiers]{ 1.0f, 1.0f, 1.0f };  // model-space height per tier
		float fuzzRoughness = -1.0f;                      // TruePBR's PBRParams5.x: negative uses the base roughness
		float occlusionRadius = 0.0f;                    // the canopy sphere's radius, the largest of the drawn tiers'
		std::uint32_t pad2 = 0;
	};
	static_assert(sizeof(BucketRow) == 224);
	static_assert(offsetof(BucketRow, flags) == 12 && offsetof(BucketRow, visibleBase) == 16 && offsetof(BucketRow, instanceCount) == 28 &&
				  offsetof(BucketRow, diffuseIndex) == 32 && offsetof(BucketRow, alphaRef) == 44 && offsetof(BucketRow, boundCenter) == 48 &&
				  offsetof(BucketRow, modelRadius) == 60 && offsetof(BucketRow, wavePeriod) == 64 && offsetof(BucketRow, distScale) == 68 &&
				  offsetof(BucketRow, minPixelScale) == 72 && offsetof(BucketRow, pbrFlags) == 76 && offsetof(BucketRow, scaleMask) == 80 &&
				  offsetof(BucketRow, normalIndex) == 96 && offsetof(BucketRow, pbrSampler) == 108 && offsetof(BucketRow, pbrParams1) == 112 &&
				  offsetof(BucketRow, pbrParams2) == 128 && offsetof(BucketRow, windDirection) == 144 &&
				  offsetof(BucketRow, lodMeshCost) == 152 && offsetof(BucketRow, pbrParams3) == 160 && offsetof(BucketRow, pbrParams4) == 176 &&
				  offsetof(BucketRow, fuzzIndex) == 192 && offsetof(BucketRow, emissiveIndex) == 196 && offsetof(BucketRow, canopyHeight) == 200 &&
				  offsetof(BucketRow, fuzzRoughness) == 212 && offsetof(BucketRow, occlusionRadius) == 216);

	// InstanceFrame flags, stored in fadeFlags' high 16 bits.
	inline constexpr std::uint32_t kFrameTierMask = 3u;
	inline constexpr std::uint32_t kFrameCollision = 1u << 2;
	inline constexpr std::uint32_t kFrameSimpleShading = 1u << 3;

	/** @brief Per-instance values the cull writes for the vertex stage. Packed tight because large lists have millions of instances. */
	struct InstanceFrame
	{
		std::uint32_t wind = 0;       // halfs: wind at this frame's timer (low) and the previous frame's (high)
		std::uint32_t fadeFlags = 0;  // fade as unorm16 (low), flags (high)
	};
	static_assert(sizeof(InstanceFrame) == 8);

	inline constexpr std::uint32_t kCullMidLod = 1u << 0;
	inline constexpr std::uint32_t kCullFarLod = 1u << 1;
	inline constexpr std::uint32_t kCullOcclusion = 1u << 2;  // test the HZB

	/** @brief Mirrors Grass Optimizations' CullParams. */
	struct CullRow
	{
		float minPixelSize = 0.0f, fullDetailPx = 0.0f, minKeep = 1.0f, fadeBand = 0.15f;
		float costBias = 0.0f, costBiasStart = 0.0f, projScale = 0.0f, maxDistSq = 0.0f;
		float edgeFadeStart = 1.0f, alphaParam1 = 0.0f, alphaParam2 = 0.0f, invisibleFadeCull = 0.0f;
		float simpleShadingPx = 0.0f, collisionDistSq = 0.0f, midLodPx = 0.0f, farLodPx = 0.0f;
		float lodBandPx = 0.0f;
		std::uint32_t flags = 0;
		float occlusionDepthBias = 0.0f;  // separation behind an occluder, as a fraction of its view depth
		float occlusionTightness = 1.0f;  // scales the occlusion sphere; below 1 culls more
		std::uint32_t pad[12]{};
	};
	static_assert(sizeof(CullRow) == 128 && offsetof(CullRow, alphaParam1) == 36 && offsetof(CullRow, alphaParam2) == 40 && offsetof(CullRow, flags) == 68 &&
				  offsetof(CullRow, occlusionDepthBias) == 72 && offsetof(CullRow, occlusionTightness) == 76);

	/** @brief The single per-frame row, written by each depth commit. Holds the engine's grass constants from SetupGeometry's b2. */
	struct DrawRow
	{
		RenderGraph::BufferRef instances, shapes, buckets, meshes, owners, frame, visible;
		std::uint32_t instanceSlots = 0, shapeSlots = 0;  // 0 means grass draws nothing this frame
		float windVector[4]{};  // z is the strength. xy come from each bucket.
		float timeBase = 0.0f, prevTimeBase = 0.0f, fadeNow = 0.0f, fadeInTimeRcp = 0.0f;
		float dirLightDirection[4]{};  // w: ShadowClampValue
		float dirLightColor[4]{};      // w: FogNearColor.w
		float ambientColor[4]{};
		DescriptorIndex shadowMaskIndex;
		DescriptorIndex collisionIndex = DescriptorIndex::Sentinel(~0u);  // ~0u means no collision displacement
		std::uint32_t bucketSlots = 0;
		std::uint32_t flags = 0;
		std::array<std::byte, 64> collision{};  // GrassCollision::PerFrame
		float eye[4]{};
		std::uint32_t reserved[4]{};
		CullRow cull;
	};
	static_assert(sizeof(DrawRow) == 384);
	static_assert(offsetof(DrawRow, owners) == 32 && offsetof(DrawRow, frame) == 40 && offsetof(DrawRow, visible) == 48 && offsetof(DrawRow, instanceSlots) == 56 &&
				  offsetof(DrawRow, windVector) == 64 && offsetof(DrawRow, timeBase) == 80 && offsetof(DrawRow, dirLightDirection) == 96 &&
				  offsetof(DrawRow, dirLightColor) == 112 && offsetof(DrawRow, ambientColor) == 128 && offsetof(DrawRow, shadowMaskIndex) == 144 &&
				  offsetof(DrawRow, collisionIndex) == 148 && offsetof(DrawRow, bucketSlots) == 152 && offsetof(DrawRow, flags) == 156 &&
				  offsetof(DrawRow, collision) == 160 && offsetof(DrawRow, eye) == 224 && offsetof(DrawRow, cull) == 256);

	// The draws buffer is fixed size, not versioned. A draw slot is bucket * 3 + tier. After the header come two count arrays,
	// then four DrawArgs arrays: phase 1 depth, phase 2 depth, and one per colour program.
	inline constexpr std::uint32_t kMaxBuckets = 1024;
	inline constexpr std::uint32_t kMaxDrawSlots = kTiers * kMaxBuckets;

	/** @brief The draw counts are read by ExecuteIndirect as its count. */
	struct DrawsHeader
	{
		std::uint32_t phaseOneDraws = 0, phaseTwoDraws = 0, colourDraws[2]{};
		std::uint32_t retests = 0;            // length of the retest list
		std::uint32_t tested = 0;             // instances that reached the occlusion test in phase 1
		std::uint32_t phaseOneInstances = 0, phaseTwoInstances = 0;
		std::uint32_t stats[8]{};             // kStat*
	};
	static_assert(sizeof(DrawsHeader) == 64 && offsetof(DrawsHeader, retests) == 16 && offsetof(DrawsHeader, phaseOneInstances) == 24 &&
				  offsetof(DrawsHeader, stats) == 32);
	inline constexpr std::uint32_t kStatLive = 0, kStatOutside = 1, kStatThinned = 2, kStatFaded = 3, kStatFar = 4;

	/**
	 * @brief The instance word, then non-indexed DrawInstanced arguments. D3D12 below Shader Model 6.8 writes the word into a push constant.
	 * Vulkan skips it and starts at vertexCount (FamilyPipelines::argumentsOffset).
	 */
	struct DrawArgs
	{
		std::uint32_t instanceWord = 0;
		std::uint32_t vertexCount = 0, instanceCount = 0, firstVertex = 0, firstInstance = 0;
	};
	static_assert(sizeof(DrawArgs) == 20 && offsetof(DrawArgs, vertexCount) == 4);

	inline constexpr std::uint64_t kCountsOffset = sizeof(DrawsHeader);
	inline constexpr std::uint64_t kPhaseOneCountsOffset = kCountsOffset + kMaxDrawSlots * sizeof(std::uint32_t);
	inline constexpr std::uint64_t kPhaseOneArgs = kPhaseOneCountsOffset + kMaxDrawSlots * sizeof(std::uint32_t);
	inline constexpr std::uint64_t kPhaseTwoArgs = kPhaseOneArgs + kMaxDrawSlots * sizeof(DrawArgs);
	inline constexpr std::uint64_t kColourArgs[2] = { kPhaseTwoArgs + kMaxDrawSlots * sizeof(DrawArgs), kPhaseTwoArgs + 2ull * kMaxDrawSlots * sizeof(DrawArgs) };
	inline constexpr std::uint64_t kDrawsBytes = kColourArgs[1] + kMaxDrawSlots * sizeof(DrawArgs);
	static_assert(kCountsOffset == 64 && kPhaseOneCountsOffset == 12352 && kPhaseOneArgs == 24640 && kPhaseTwoArgs == 86080 && kColourArgs[0] == 147520 &&
				  kColourArgs[1] == 208960 && kDrawsBytes == 270400);

	// The visible list holds 12 bytes per instance slot. Visible entries fill the first 2 words per slot.
	// The retest list of phase 1's occluded instances starts at kRetestOffsetPerUnit * units bytes.
	inline constexpr std::uint32_t kVisibleUnitBytes = 12;
	inline constexpr std::uint32_t kRetestOffsetPerUnit = 8;

	/** @brief True when the shader property is a BSGrassShaderProperty. Safe from any thread. */
	inline bool IsEngineShape(const RE::BSGeometry* a_shape)
	{
		const auto* property = a_shape ? a_shape->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		static const std::uintptr_t grassVtable = RE::VTABLE_BSGrassShaderProperty[0].address();
		return Engine::VtableIs(property, grassVtable);
	}
}
