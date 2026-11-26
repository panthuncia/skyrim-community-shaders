#pragma once

// Water as DCLF scene objects. Water shapes are tracked under the water roots,
// classified by the water port and counted by a census. Plain water is claimed while the water epochs draw it; the rest stays
// the batch hooks'.
// The port's pure functions are Features/Water/Port. This folder registers them with Core:
// - WaterSceneFeature.cpp: ObjectClass::Water and its category roots.
// - WaterScenePort.cpp: the material port for BSWaterShaderProperty and BSWaterShaderMaterial.
// - WaterSceneCensus.cpp: what was tracked and classified, and the report line.
// - WaterSceneRows.cpp: the per-object water row (a table family), the water material pool and the frame row.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <array>
#include <bit>
#include <cstdint>

namespace DCLF
{
	enum class Ineligible : std::uint8_t;
	namespace SceneCapture
	{
		struct PropertyRecord;
		struct LeafView;
	}
}

namespace DCLF::Features::Water::WaterScene
{
	/** @brief The water port's material kind. Effects registers kind 1 before it, and RegisterPort checks this. */
	inline constexpr std::uint8_t kMaterialKindWater = 2;

	/** @brief t58: the water material pool, a WaterMaterialRow per water material. The shading row's WaterMaterial names one. */
	inline constexpr std::uint32_t kMaterialPoolRegister = 58;
	/** @brief t59: the frame row, one WaterFrameRow. */
	inline constexpr std::uint32_t kFrameRowRegister = 59;
	/**
	 * @brief The object record word that carries a water object's row, with the row's tag in the next word.
	 * Words 4 and 5 are DCLFTreeParams.xy, which only a tree's shading block fills. A water property is never a tree's.
	 */
	inline constexpr std::uint32_t kWaterRecordWord = 4;

	// The stencil reference, readable from a water key. The key's spare bits 21-23 hold the index of the property's water-type bit
	// (BSWaterShaderProperty +0x8C, one of 2..0x80): the mask writes and tests 1 << index. Every plain surface uses reference 1.
	// Bits 24-27 read as terrain and forward bits on every key, so they stay clear.
	inline constexpr std::uint32_t kStencilIndexShift = 21;
	inline constexpr std::uint32_t kStencilIndexMask = 7u << kStencilIndexShift;
	inline constexpr std::uint8_t kSurfaceStencilRef = 1;

	/** @brief The index of a water-type bit, 1 to 7, or 0 when a_bit is not a single bit from 2 to 0x80. */
	constexpr std::uint32_t StencilIndexOf(std::uint32_t a_bit)
	{
		return (a_bit >= 2 && a_bit <= 0x80 && std::has_single_bit(a_bit)) ? static_cast<std::uint32_t>(std::countr_zero(a_bit)) : 0u;
	}
	/** @brief A water key's stencil index (its raster flags' bits 21-23). */
	constexpr std::uint32_t StencilIndexOfKey(std::uint32_t a_rasterFlags) { return (a_rasterFlags & kStencilIndexMask) >> kStencilIndexShift; }
	/** @brief The mask's stencil reference for a water key, the water-type bit itself. 0 for a key with no index. */
	constexpr std::uint8_t MaskStencilRefOfKey(std::uint32_t a_rasterFlags)
	{
		const std::uint32_t index = StencilIndexOfKey(a_rasterFlags);
		return index ? static_cast<std::uint8_t>(1u << index) : std::uint8_t{ 0 };
	}

	// A water property record's capture (WaterScenePort.cpp). PropertyRecord::feature holds the water flags and the stencil index,
	// PropertyRecord::projected holds the plane and the cell and flow offsets.
	inline constexpr std::uint32_t kFeatureFlagsMask = 0x00FFFFFFu;  // BSWaterShaderProperty +0x88, kUnderwater (bit 5) cleared
	inline constexpr std::uint32_t kFeatureStencilShift = 24;         // 3 bits: StencilIndexOf(+0x8C)
	inline constexpr std::uint32_t kFeatureCaptured = 1u << 31;       // set by every water capture
	// The material reads the global reflection cube at t3: it has no static cube, or its +0x17D byte picks the global one (SetupMaterial).
	inline constexpr std::uint32_t kFeatureGlobalCube = 1u << 27;
	/** @brief WaterObjectRow::waterFlags: the draw reads the frame's reflection cube at t3 (Water.hlsl's kDCLFWaterFlagGlobalCube). */
	inline constexpr std::uint32_t kRowGlobalCube = 1u << 31;
	/** @brief kUnderwater: GetRenderPasses copies the camera's state into it, so the record never keeps it. */
	inline constexpr std::uint32_t kWaterFlagUnderwater = 0x20;
	inline constexpr std::size_t kValuePlane = 0;    // 4 floats: the property's plane (+0xA0), normal and constant
	inline constexpr std::size_t kValueOffsets = 4;  // 4 ints as float bits: cellX, cellY, flowX, flowY (+0x90..+0x9C)

	// BSWaterShaderMaterial's texture fields (NiSourceTexture pointers): the static cube, the three normal maps and the flow normals.
	inline constexpr std::uint16_t kMaterialStaticReflection = 0x38;
	inline constexpr std::uint16_t kMaterialNormal1 = 0x40;
	inline constexpr std::uint16_t kMaterialNormal2 = 0x48;
	inline constexpr std::uint16_t kMaterialNormal3 = 0x50;
	inline constexpr std::uint16_t kMaterialFlowNormals = 0x58;
	inline constexpr std::uint16_t kMaterialTextureFields[] = { kMaterialStaticReflection, kMaterialNormal1, kMaterialNormal2, kMaterialNormal3, kMaterialFlowNormals };

	/**
	 * @brief A water object's row in the table family: what SetupGeometry reads of the property, fixed until an engine write.
	 * Port::ObjectRow is this row with the frame's flowmap values and Unified Water's rewrite applied, which needs the frame row
	 * and the placement row's world translate (Port::EvaluateObject). So the family holds only these event-scoped inputs.
	 */
	struct WaterObjectRow
	{
		std::int32_t cellX = 0, cellY = 0, flowX = 0, flowY = 0;  // as the property holds them
		std::array<float, 4> plane{};
		std::uint32_t waterFlags = 0;          // +0x88 without kUnderwater
		std::uint32_t stencilBit = 0;          // +0x8C
		std::uint32_t surfaceDescriptor = 0;   // the surface pass's raw descriptor, without the Interior bit
		std::uint32_t generation = 0;          // the row's listing, also its tag in the record
	};
	static_assert(sizeof(WaterObjectRow) == 48);

	/** @brief A water material's rows as the GPU reads them: Port::MaterialRow's values, unwritten components zero. */
	struct WaterMaterialRow
	{
		std::array<std::array<float, 4>, 6> vs{};   // VS b1 c0-c5
		std::array<std::array<float, 4>, 14> ps{};  // PS b1 c0-c13
	};
	static_assert(sizeof(WaterMaterialRow) == 320);

	/** @brief The frame row's float4 rows, in order. */
	enum class FrameRowEntry : std::uint32_t
	{
		QPosAdjust,          // VS b0 c0 (Port::TechniqueRows)
		VposOffset,          // PS b0 c0, as with the Depth bit
		PosAdjust,           // PS b0 c1
		CameraData,          // PS b0 c2
		SunDir,              // PS b0 c3
		SunColor,            // PS b0 c4
		ProjData,            // PS b2 c5: projection [0][0] and [1][1]
		PreviousPosAdjust,   // xyz, w the framebuffer range
		Eye,                 // the main accumulator's eye position, w the reflection timer
		Flowmap,             // the flowmap size and the mesh flow cell offset as the engine holds them, w Unified Water's rewrite on
		UnifiedWaterFlow,    // Unified Water's flowmap width, height and offsets
		DisplacementOffset,  // the displacement mesh's CellTexCoordOffset
		WaterLighting,       // the water lighting the colours are multiplied by
		Flags,               // as uints: camera under water, interior, kRAW_WATER target, Linear Lighting
		ViewProj0,           // the view-projection SetupGeometry uses (jittered or not), four rows
		ViewProj1,
		ViewProj2,
		ViewProj3,
		Count
	};
	inline constexpr std::uint32_t kFrameRows = static_cast<std::uint32_t>(FrameRowEntry::Count);

	/** @brief The frame row the GPU reads at kFrameRowRegister. */
	struct WaterFrameRow
	{
		std::array<std::array<float, 4>, kFrameRows> rows{};
	};
	static_assert(sizeof(WaterFrameRow) == kFrameRows * 16);

	/** @brief Install time, main thread. Registers the water object class and everything else in this folder. */
	void Install(Api::InstallContext& a_context);

	// Within this folder.

	/** @brief Registers the material port. Must follow the object class's registration. */
	void RegisterPort(Api::InstallContext& a_context);
	/** @brief Registers the census's component and report line. */
	void RegisterCensus(Api::InstallContext& a_context);
	/** @brief Registers the water row's table family, the material pool and the frame row. */
	void RegisterRows(Api::InstallContext& a_context);

	/** @brief The roots the category capture last listed. Written on the render thread, read by the census on the scene lane. */
	struct Roots
	{
		const void* waterLod = nullptr;     // TES::objLODWaterRoot, "WaterLOD"
		const void* waterSystem = nullptr;  // TESWaterSystem::waterRoot
	};
	Roots ListedRoots();

	/** @brief What the port's derive decided for one shape, for the census. */
	struct Classification
	{
		enum Attribute : std::uint32_t
		{
			kLodBit = 1u << 0,        // the property's LOD bit (vanilla LOD water, technique 9)
			kWading = 1u << 1,        // water flag bit 0: the Wading descriptor bit
			kHint13 = 1u << 2,        // the surface pass's accumulation hint is 13
			kVertexColors = 1u << 3,  // the stencil pass is 6E or 70
			kFlowmap = 1u << 4,
			kSkinned = 1u << 5,
		};
		enum class Refusal : std::uint8_t
		{
			None,             // plain water, which phase 2 claims
			Wading,           // wading or hint 13 (phase 4)
			Stencil,          // no single water-type bit at +0x8C
			NoLightLimitFix,  // light passes without Light Limit Fix (plan section 4)
			Skinned,
			NotCaptured,      // the property record has no water capture
			Count
		};
		std::uint32_t attributes = 0;
		Refusal refusal = Refusal::None;
		std::uint32_t waterFlags = 0;
		std::uint32_t stencilBit = 0;
		std::uint32_t surfaceDescriptor = 0;
		std::uint32_t stencilPass = 0;
		std::uint8_t surfaceHint = 0;
	};
	/** @brief Pure. The port's classification of a water leaf whose property record has the water capture. */
	Classification ClassifyLeaf(const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf);
	/** @brief Render thread. The water shapes Core tracks, as of the last walk: native draws those its epochs do not claim from these. */
	void TrackedShapes(std::vector<const void*>& a_out);
	/** @brief Any thread the port's derive runs on. Queues a_classification of the geometry keyed a_geometry for the census. */
	void NoteClassified(const void* a_geometry, const Classification& a_classification);
	/** @brief Render thread. The material pool's sizes for the census line. */
	struct PoolSizes
	{
		std::uint32_t materialRows = 0, materialMembers = 0;
		bool cameraUnderwater = false;
	};
	PoolSizes CurrentPoolSizes();
}
