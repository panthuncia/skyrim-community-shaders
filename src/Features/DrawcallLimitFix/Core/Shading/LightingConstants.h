#pragma once

// The Lighting shader's constant layout and DCLF's per-object rows: variable groups, the frame blocks the bindless stages read
// instead of per-pipeline constants, the object, shading and placement rows, and cbuffer packing.
// Sample* run on the render thread. The rest are pure.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/FrameTables.h"

namespace DCLF
{
	/** @brief Constant groups PerTechnique, PerMaterial and PerGeometry, b0-b2 in Lighting.hlsl. */
	inline constexpr std::uint32_t kPerTechnique = 0;
	inline constexpr std::uint32_t kPerMaterial = 1;
	inline constexpr std::uint32_t kPerGeometry = 2;

	// Lighting variable indices (ShaderConstants::LightingVS / LightingPS).
	inline constexpr std::uint32_t kVSWorld = 0;
	inline constexpr std::uint32_t kVSPreviousWorld = 1;
	inline constexpr std::uint32_t kVSLandBlendParams = 3;  // MTLand, per object
	inline constexpr std::uint32_t kVSTreeParams = 4;   // TreeAnim wind, per object
	inline constexpr std::uint32_t kVSWindTimers = 5;
	inline constexpr std::uint32_t kVSTextureProj = 6;      // ProjectedUV, per object
	inline constexpr std::uint32_t kVSLeftEyeCenter = 9;     // first PerMaterial VS variable
	inline constexpr std::uint32_t kVSTexcoordOffset = 11;   // the frame's, because texture transforms patch the rows
	inline constexpr std::uint32_t kVSHighDetailRange = 12;  // first PerTechnique VS variable
	inline constexpr std::uint32_t kPSNumLights = 0;
	inline constexpr std::uint32_t kPSPointLightPosition = 1;
	inline constexpr std::uint32_t kPSPointLightColor = 2;
	inline constexpr std::uint32_t kPSDirLightDirection = 3;  // first PerGeometry PS variable
	inline constexpr std::uint32_t kPSMaterialData = 7;
	inline constexpr std::uint32_t kPSEmitColor = 8;
	inline constexpr std::uint32_t kPSShadowLightMaskSelect = 10;
	inline constexpr std::uint32_t kPSProjectedUVParams = 12;  // ProjectedUV, per object: 12, 13 (colour), 14 (globals)
	inline constexpr std::uint32_t kPSSSRParams = 16;
	inline constexpr std::uint32_t kPSFogColor = 19;      // first PerTechnique PS variable
	inline constexpr std::uint32_t kPSLODTexParams = 24;  // first PerMaterial PS variable
	inline constexpr std::uint32_t kPSIBLParams = 29;     // read by no Lighting stage. A record keeps its first.

	constexpr std::uint64_t VariableBits(std::uint32_t a_first, std::uint32_t a_last)
	{
		return ((a_last >= 63 ? ~0ull : ((1ull << (a_last + 1)) - 1))) & ~((1ull << a_first) - 1);
	}

	/** @brief Which constant group each Lighting variable belongs to, by group. */
	inline constexpr std::uint64_t kVSGroups[3] = { VariableBits(12, 15), VariableBits(9, 11), VariableBits(0, 8) | (1ull << 16) };
	inline constexpr std::uint64_t kPSGroups[3] = { (1ull << 11) | (1ull << 19) | (1ull << 20), VariableBits(21, 50), VariableBits(0, 10) | VariableBits(12, 18) };
	/** @brief Each group's first variable, the only one a constant table may place at offset 0. */
	inline constexpr std::uint32_t kVSFirstVariable[3] = { kVSHighDetailRange, kVSLeftEyeCenter, kVSWorld };
	inline constexpr std::uint32_t kPSFirstVariable[3] = { kPSFogColor, kPSLODTexParams, kPSDirLightDirection };

	/** @brief Per-object light assignment, which the Light Limit Fix shaders never read. */
	inline constexpr std::uint64_t kPSLightAssignment = (1ull << kPSNumLights) | (1ull << kPSPointLightPosition) | (1ull << kPSPointLightColor) |
	                                                    (1ull << kPSShadowLightMaskSelect);

	// PerGeometry variables that are frame globals, the same in every pipeline: DirLightDirection, DirLightColor, DirectionalAmbient,
	// AmbientSpecularTintAndFresnelPower and AmbientColor. CS_DCLF_PERSISTENT_PARITY checks this. For EyePosition see WritesEyePosition.
	inline constexpr std::uint32_t kVSEyePosition = 2;
	/**
	 * @brief Whether SetupGeometry (AE 0x1414dd040) writes EyePosition for a pass: Envmap, Eye, technique 0x10, descriptor bit 9 or any of 0x21c00.
	 * The value is always the camera less posAdjust. Other passes leave a stale value none of their draws read.
	 */
	inline constexpr bool WritesEyePosition(std::uint32_t a_passDescriptor)
	{
		const std::uint32_t technique = (a_passDescriptor >> 24) & 0x3f;
		return technique == 1 || technique == 0xb || technique == 0x10 || (a_passDescriptor & 0x21e00u) != 0;
	}
	inline constexpr std::uint32_t kPSFrameGeometry[5] = { 3, 4, 5, 6, 18 };
	// The bindless pixel stage reads these from DCLFFrameLighting in PS b13, so no pipeline's block changes with the sun.
	inline constexpr std::uint32_t kFrameLightingRegister = 13;
	inline constexpr std::uint32_t kFrameLightingRows = 6;
	using FrameLighting = std::array<float, kFrameLightingRows * 4>;
	// Fog drifts with the time of day, so the bindless vertex stage reads it from DCLFFrameFog in VS b13 and technique rows never change.
	// PS FogColor drifts too, but no Lighting stage reads its colour. The forward stage reads its w.
	inline constexpr std::uint32_t kVSFrameFog[3] = { 13, 14, 15 };
	inline constexpr std::uint32_t kFrameFogRegister = 13;
	inline constexpr std::uint32_t kFrameFogRows = 3;
	using FrameFog = std::array<float, kFrameFogRows * 4>;
	/**
	 * @brief What the bindless path leaves out of a pipeline's PerGeometry block, packed as zero: frame lighting, what the record and extras
	 * supply, and what Lighting.hlsl never reads. SSRParams stays, since only its w is per object.
	 */
	inline constexpr std::uint64_t kVSBindlessGeometryUnread = (1ull << kVSWorld) | (1ull << kVSPreviousWorld) | (1ull << kVSEyePosition) | (1ull << kVSLandBlendParams) |
	                                                           (1ull << kVSTreeParams) | (1ull << kVSWindTimers) | (1ull << kVSTextureProj);
	inline constexpr std::uint64_t kPSBindlessGeometryUnread = (1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 6) | (1ull << 18) | (1ull << kPSMaterialData) |
	                                                           (1ull << kPSEmitColor) | (7ull << kPSProjectedUVParams);

	/** @brief Whether two PerGeometry evaluations agree in everything a DCLF_BINDLESS draw reads from the pipeline's block. */
	bool SameBindlessGeometry(const GeometryConstants& a_a, const GeometryConstants& a_b);

	/**
	 * @brief Merges a PS PerGeometry block's frame lighting into a_out where a_written lacks it.
	 * Techniques write different components, so the frame merges every evaluation.
	 */
	void MergeFrameLighting(const ConstantBlock& a_ps, FrameLighting& a_out, std::uint32_t& a_written);
	/** @brief Merges the vertex fog a technique writes into a_out where a_written lacks it. */
	void MergeFrameFog(const TechniqueConstants& a_technique, FrameFog& a_out, std::uint32_t& a_written);
	/** @brief Copies a_from's fog floats into a_to, so a technique row keeps the fog it was made with. */
	void KeepTechniqueFog(const TechniqueConstants& a_from, TechniqueConstants& a_to);

	/**
	 * @brief An object's PerGeometry values as its native draw binds them. The eyes are passed in,
	 * because by packing time the engine has moved the camera on.
	 */
	struct BindlessPlacement;
	struct BindlessShading;
	struct ExtrasFrame;
	GeometryConstants ObjectGeometryConstants(const SceneTables& a_tables, const FrameTables& a_frame, std::uint32_t a_objectIndex, const BindlessPlacement& a_placement,
		const BindlessShading& a_shading, const ExtrasFrame& a_extrasFrame, const RE::NiPoint3& a_eye, const RE::NiPoint3& a_previousEye);

	/** @brief Packs a constant group into a cbuffer layout using the reflected constant table. Unwritten components become 0. Returns bytes spanned. */
	std::size_t PackConstantGroup(const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables,
		std::uint32_t a_firstVariable, std::span<std::byte> a_out);

	/** @brief Where the per-object PerGeometry variables land in a group packed once per pipeline, in floats. ~0 means not declared. */
	struct GeometryPatchOffsets
	{
		std::uint32_t vsWorld = ~0u, vsWorldSize = 0;
		std::uint32_t vsPreviousWorld = ~0u, vsPreviousWorldSize = 0;
		std::uint32_t psMaterialData = ~0u, psMaterialDataSize = 0;
		std::uint32_t psEmitColor = ~0u, psEmitColorSize = 0;
		std::uint32_t psSSRParams = ~0u, psSSRParamsSize = 0;
		// The feature block's variables, in its order, a word each at most.
		std::array<std::uint32_t, 8> vsBlock{ ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
		std::array<std::uint32_t, 8> vsBlockSize{};
		std::uint32_t vsLandBlendParams = ~0u, vsLandBlendParamsSize = 0;
		std::uint32_t vsTextureProj = ~0u, vsTextureProjSize = 0;
		std::array<std::uint32_t, 3> psProjectedUVParams{ ~0u, ~0u, ~0u };
		std::array<std::uint32_t, 3> psProjectedUVParamsSize{};
	};

	/** @brief The object's extras rows, or null when it has none. */
	const float* ExtraRowsOf(const SceneTables& a_tables, std::uint32_t a_objectIndex);

	/**
	 * @brief Per-frame inputs that complete an object's static extras with its placement. Sampled by each main commit.
	 * The vertex part goes to VS b13 c3-c7 and the pixel part to PS b13 c16.
	 */
	struct ExtrasFrame
	{
		// VS b13 c3: the land blend position in xy, the clock's blend of two positions in BSShaderManager::State.
		float landBlend[4]{};
		// VS b13 c4-c7: the ProjectedUV projection, a fixed rotation about Z at posAdjust, row-major with zero translation.
		// TextureProj's rows are the columns of World x this, or this alone for Envmap.
		float projection[16]{};
		// PS b13 c16: ProjectedUVParams3 from FUN_1414e00c0's globals: diffuse and detail tiling, 0, and the projected-normals switch.
		float projectedGlobals[4]{};
	};
	static_assert(sizeof(ExtrasFrame) == 96);
	inline constexpr std::size_t kExtrasFrameVertexBytes = offsetof(ExtrasFrame, projectedGlobals);
	/** @brief Game side, FrameGlobals::Capture: ExtrasFrame's inputs from the engine's globals (the land blend position, ProjectedUVParams3). */
	void CaptureExtrasInputs(std::array<float, 2>& a_landBlend, std::array<float, 4>& a_projectedGlobals);
	/** @brief Render thread. This frame's ExtrasFrame: the frame globals' inputs and the projection at the frame's eye. */
	ExtrasFrame SampleExtrasFrame();
	/**
	 * @brief How TextureProj is made: World x projection, the projection alone for Envmap, or the shape's own materialProjection.
	 * ProjectedUV and land blend never coexist, so this rides in the static land-blend row's x.
	 */
	inline constexpr float kTextureProjWorld = 0.0f, kTextureProjProjection = 1.0f, kTextureProjShape = 2.0f;
	/** @brief An object's extras rows as its draw completes them. a_world is absolute row-major 3x4. */
	void CompleteExtras(const float* a_static, std::uint32_t a_objectFlags, const float (&a_world)[12], const ExtrasFrame& a_frame, float* a_out);

	GeometryPatchOffsets GeometryPatchOffsetsOf(std::span<const std::uint8_t> a_vsTable, std::span<const std::uint8_t> a_psTable);

	/** @brief Writes one object's per-object PerGeometry variables over its pipeline's packed template, without walking the table. */
	void PatchObjectGeometry(const SceneTables& a_tables, std::uint32_t a_objectIndex, const BindlessPlacement& a_placement,
		const BindlessShading& a_shading, const RE::NiPoint3& a_eye, const RE::NiPoint3& a_previousEye, const GeometryPatchOffsets& a_offsets,
		std::span<std::byte> a_vsOut, std::span<std::byte> a_psOut);

	/**
	 * @brief One object's record: what changes with its structure. Placement and shading have their own rows,
	 * so neither a move nor a flicker rewrites a record.
	 */
	struct BindlessObject
	{
		// Values native shaders read from their own constant buffers: the ancestor value, such as Light Limit Fix's room index in PS b3,
		// and the alpha test reference in PS b11.
		std::int32_t ancestorValue;
		// kRecord* bits. Bits 9-31 hold the fade root slot + 1, by which the pixel stage reads FadeStateCS's fade.
		std::uint32_t recordFlags;
		float alphaTestRef;
		// The LOD fades the pass applies while the fade node has them apply. Otherwise the shading row's MaterialData carries them.
		std::uint32_t lodFades;
		// The feature block, words 4-11. Zero except a table family's reference, such as TreeWind's tree slot and generation in words 10-11.
		std::uint32_t featureBlock[8];
		// Skinning: current and previous palette rows in VS t122 and the row count, three per bone. Absolute, like World. Zero otherwise.
		std::uint32_t boneOffset;
		std::uint32_t previousBoneOffset;
		std::uint32_t boneRows;
		// The object's extras rows in VS and PS t126, or 0.
		std::uint32_t extraOffset;
	};
	static_assert(sizeof(BindlessObject) == 64);

	/**
	 * @brief One object's shading in PS t121: what its property's controllers and fades animate, plus its actor's wetness.
	 * FrameValues samples these rows at the frame's start.
	 */
	struct BindlessShading
	{
		// MaterialData, EmitColor, and SSRParams.w in the last float. Unwritten components are zero.
		ObjectShading shading;
		// Advanced Skin's SkinPerGeometry in PS b7: the owning actor's wetness, zero for everything else.
		float skinPerGeometry[4];
		// A bit per float SetupGeometry writes, for CPU comparisons. The shader reads none.
		std::uint32_t written;
		// Advanced Skin's row in its material table (Integrations/AdvancedSkin/SkinMaterials.cpp), zero for the disabled profile.
		std::uint32_t skinMaterial;
		// The water material pool's row (Features/Water/Scene/WaterRows.cpp), zero for none. A shading field, so samples leave it.
		std::uint32_t waterMaterial;
		std::uint32_t padding;
		// The feature block. The shading contribution that claims it derives the first blockWords, such as TreeWind's TreeParams and WindTimers.xy,
		// which a member draws with until TreeWindCS has an entry for its tree. The rest is zero.
		float featureBlock[8];
		std::uint32_t landscapeSeams[8];  // descriptor index + 1. Zero disables seam resources for this quad.
	};
	static_assert(sizeof(BindlessShading) == 128);

	/**
	 * @brief An object's shading from its property now, as SetupGeometry writes it for a_pass. Wetness is left as is.
	 * With a_member, fades are DCLF's and stay out of alpha. Any thread inside the engine-read window. Port properties use SamplePortShading.
	 */
	void SampleShading(const RE::BSShaderProperty& a_property, std::uint32_t a_pass, bool a_member, BindlessShading& a_out);
	// BindlessObject::recordFlags: bit 0 is a Lighting object flag's record bit, such as SubsurfaceScattering's beast race,
	// and the alpha-blended bit marks an alpha-tested object whose alpha property blends.
	inline constexpr std::uint32_t kRecordAlphaBlended = 1u << 1;
	// Refraction: the RENDER_NORMAL variants a refraction draw takes from here, not its pipeline. Skinning is the record's palette.
	inline constexpr std::uint32_t kRecordRefraction = 1u << 2;
	inline constexpr std::uint32_t kRecordRefractionFalloff = 1u << 3;
	inline constexpr std::uint32_t kRecordRefractionClamp = 1u << 4;
	inline constexpr std::uint32_t kRecordRefractionNoNormals = 1u << 5;
	inline constexpr std::uint32_t kRecordRefractionVertexColors = 1u << 6;
	inline constexpr std::uint32_t kRecordRefractionTwoSided = 1u << 7;
	// A refraction leaf under an NiBillboardNode, turned to the camera by the refraction stage with its placement row's face mode.
	// Shares bit 8 with kRecordDecalNudge, which is safe because a refraction object is never in a decal group.
	inline constexpr std::uint32_t kRecordRefractionBillboard = 1u << 8;
	/** @brief The refraction record bits for a_key, or none for a non-refraction key. */
	inline constexpr std::uint32_t RefractionRecordFlagsOf(const PipelineKey& a_key)
	{
		if (!(a_key.rasterFlags & kRasterRefraction))
			return 0u;
		return kRecordRefraction | ((a_key.rasterFlags & kRasterRefractionFalloff) ? kRecordRefractionFalloff : 0u) |
		       ((a_key.rasterFlags & kRasterRefractionClamp) ? kRecordRefractionClamp : 0u) |
		       ((a_key.passDescriptor & (1u << 2)) ? kRecordRefractionNoNormals : 0u) |     // the pass's ModelSpaceNormals
		       ((a_key.passDescriptor & (1u << 0)) ? kRecordRefractionVertexColors : 0u) |  // its VC
		       ((a_key.rasterFlags & kRasterTwoSided) ? kRecordRefractionTwoSided : 0u) |
		       (RasterLightingSpare(a_key.rasterFlags) ? kRecordRefractionBillboard : 0u);
	}
	// A program family's record bits share bits 3-7 with refraction, which is safe because a family's record never has kRecordRefraction.
	/** @brief The record bits a_key's program family sets. None for a Lighting key. */
	inline std::uint32_t FamilyRecordFlagsOf(const PipelineKey& a_key, std::uint32_t a_objectFlags)
	{
		const auto& rules = Extensions::FamilyRules(a_key.rasterFlags);
		const std::uint32_t turned = rules.turnedKeyBits ? (a_key.rasterFlags >> rules.spareShift) & kProgramFamilySpareBits : 0u;
		return ((a_objectFlags & kObjectSuppressExternalEmittance) ? rules.records.suppressEmittance : 0u) |
		       (turned ? rules.records.turned | ((turned - 1) << rules.records.turnedModeShift) : 0u);
	}
	/**
	 * @brief Whether a_key's family keeps the alpha threshold as its reference whatever the alpha test.
	 * This is the Effect Blood permutation, whose AlphaTestRef.x SetupGeometry (0x1414eab10) writes as threshold / 255.
	 */
	inline bool FamilyAlphaRefKey(const PipelineKey& a_key) { return (a_key.passDescriptor & Extensions::FamilyRules(a_key.rasterFlags).records.alphaRefPassBits) != 0; }
	/** @brief The alpha test reference: threshold / 255 when alpha tested or a Blood effect, else 0. */
	inline float AlphaTestRefOf(std::uint32_t a_objectFlags, const PipelineKey* a_key)
	{
		const bool blood = a_key && !(a_objectFlags & kObjectNoBindings) && FamilyAlphaRefKey(*a_key);
		return (a_objectFlags & kObjectAlphaTest) || blood ? ((a_objectFlags >> kObjectAlphaThresholdShift) & 0xFF) / 255.0f : 0.0f;
	}
	/**
	 * @brief A decal's draw moves its clip position toward the eye, on top of the engine's depth bias.
	 * This covers float error between decal and host far from the origin, and the exact depth tie of bias-free interior decals,
	 * which DCLF's two vertex modules do not reproduce.
	 */
	inline constexpr std::uint32_t kRecordDecalNudge = 1u << 8;
	inline constexpr std::uint32_t DecalRecordFlagsOf(const PipelineKey& a_key) { return (a_key.rasterFlags & kRasterDecalNudge) ? kRecordDecalNudge : 0u; }
	inline constexpr std::uint32_t kRecordFadeRootShift = 9;
	/** @brief An object's record flags with its fade root slot + 1 in bits 9-31, cut off at 23 bits like the table families' writes. */
	inline std::uint32_t RecordFlagsOf(std::uint32_t a_objectFlags, std::uint32_t a_fadeRoot)
	{
		return Extensions::LightingRecordFlagsOf(a_objectFlags) | ((a_objectFlags & kObjectAlphaBlended) ? kRecordAlphaBlended : 0u) |
		       (a_fadeRoot < (1u << (32 - kRecordFadeRootShift)) - 1 ? (a_fadeRoot + 1) << kRecordFadeRootShift : 0u);
	}

	/**
	 * @brief One object's placement in VS and PS t123: what a move changes. World and PreviousWorld are absolute,
	 * and the shaders subtract the drawing camera's eye, so one row serves every epoch and camera.
	 */
	struct BindlessPlacement
	{
		float world[12];
		float previousWorld[12];
		// BuildDrawsCS's world bound and sun entry sphere. Without one the radius is +max, inside every process.
		float bound[4];
		float sunEntry[4];
		// The fade-out tests' centre and the draw's LOD fades' centre.
		float lodFadeNode[4];
		// The forward sort key. A blended leaf under a BSOrderedNode draws in that node's group by depth and child order, per FUN_1414b39d0.
		// Holds the nearest such node's bound centre, and in w as int bits the leaf's 1-based depth-first ordinal. All 0 otherwise.
		float forwardSort[4];
	};
	static_assert(sizeof(BindlessPlacement) == 160);
	// Must match the shaders' float4 row indices.
	static_assert(offsetof(BindlessPlacement, bound) == 6 * 16 && offsetof(BindlessPlacement, sunEntry) == 7 * 16 && offsetof(BindlessPlacement, lodFadeNode) == 8 * 16 &&
				  offsetof(BindlessPlacement, forwardSort) == 9 * 16);

	/** @brief A palette block's rows: current at twice its boneOffset, previous right after. Blocks never overlap, so neither do these. */
	struct PaletteRows
	{
		std::uint32_t current = 0, previous = 0;
	};
	constexpr PaletteRows PaletteRowsOf(std::uint32_t a_offset, std::uint32_t a_rows) { return { 2 * a_offset, 2 * a_offset + a_rows }; }

	/** @brief Fills one placement from the same inputs PatchObjectGeometry uses. */
	void BuildObjectRecord(const SceneTables& a_tables, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags, BindlessObject& a_out);

	/** @brief World made relative to an eye the way the engine and the shaders do it. */
	void StoreRelativeTo(float* a_out, const float (&a_world)[12], const RE::NiPoint3& a_eye);

	/** @brief The packed dword offset of a block's float, or ~0 when no variable covers it. */
	std::uint32_t PackedPositionOf(const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_firstVariable,
		std::uint32_t a_float);

	/** @brief Bytes a group spans in a cbuffer layout, a multiple of 16. */
	std::size_t ConstantGroupSize(const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_firstVariable);
}
