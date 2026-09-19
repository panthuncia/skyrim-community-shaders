#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "ConstantEvaluator.h"
#include "SceneStore.h"

namespace DCLF
{
	/** @brief Constant groups (cbuffer PerTechnique, PerMaterial, PerGeometry in Lighting.hlsl; b0-b2). */
	inline constexpr std::uint32_t kPerTechnique = 0;
	inline constexpr std::uint32_t kPerMaterial = 1;
	inline constexpr std::uint32_t kPerGeometry = 2;

	// Lighting variable indices (ShaderConstants::LightingVS / LightingPS).
	inline constexpr std::uint32_t kVSWorld = 0;
	inline constexpr std::uint32_t kVSPreviousWorld = 1;
	inline constexpr std::uint32_t kVSLeftEyeCenter = 9;     // first PerMaterial VS variable
	inline constexpr std::uint32_t kVSHighDetailRange = 12;  // first PerTechnique VS variable
	inline constexpr std::uint32_t kPSNumLights = 0;
	inline constexpr std::uint32_t kPSPointLightPosition = 1;
	inline constexpr std::uint32_t kPSPointLightColor = 2;
	inline constexpr std::uint32_t kPSDirLightDirection = 3;  // first PerGeometry PS variable
	inline constexpr std::uint32_t kPSMaterialData = 7;
	inline constexpr std::uint32_t kPSEmitColor = 8;
	inline constexpr std::uint32_t kPSShadowLightMaskSelect = 10;
	inline constexpr std::uint32_t kPSSSRParams = 16;
	inline constexpr std::uint32_t kPSFogColor = 19;      // first PerTechnique PS variable
	inline constexpr std::uint32_t kPSLODTexParams = 24;  // first PerMaterial PS variable

	constexpr std::uint64_t VariableBits(std::uint32_t a_first, std::uint32_t a_last)
	{
		return ((a_last >= 63 ? ~0ull : ((1ull << (a_last + 1)) - 1))) & ~((1ull << a_first) - 1);
	}

	/** @brief Which constant group each Lighting variable belongs to, by group. */
	inline constexpr std::uint64_t kVSGroups[3] = { VariableBits(12, 15), VariableBits(9, 11), VariableBits(0, 8) | (1ull << 16) };
	inline constexpr std::uint64_t kPSGroups[3] = { (1ull << 11) | (1ull << 19) | (1ull << 20), VariableBits(21, 50), VariableBits(0, 10) | VariableBits(12, 18) };
	/** @brief Each group's first variable (the only one a constant table may place at offset 0). */
	inline constexpr std::uint32_t kVSFirstVariable[3] = { kVSHighDetailRange, kVSLeftEyeCenter, kVSWorld };
	inline constexpr std::uint32_t kPSFirstVariable[3] = { kPSFogColor, kPSLODTexParams, kPSDirLightDirection };

	/** @brief Per-object light assignment, which the Light Limit Fix shaders never read. */
	inline constexpr std::uint64_t kPSLightAssignment = (1ull << kPSNumLights) | (1ull << kPSPointLightPosition) | (1ull << kPSPointLightColor) |
	                                                    (1ull << kPSShadowLightMaskSelect);

	/**
	 * @brief An object's PerGeometry values as its native draw binds them: the per-frame block of its pass
	 * descriptor with the object's transforms (camera-relative) and shading on top.
	 */
	GeometryConstants ObjectGeometryConstants(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags);

	/**
	 * @brief Writes a constant group into the byte layout of a shader's cbuffer, using the shader's constant
	 * table (variable index -> dword offset, as Community Shaders reflects it). Components the engine
	 * does not write become 0. Returns the bytes the group spans (0 when the table places nothing).
	 */
	std::size_t PackConstantGroup(const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::int8_t> a_table, std::uint64_t a_variables,
		std::uint32_t a_firstVariable, std::span<std::byte> a_out);

	/** @brief Bytes a group spans in a shader's cbuffer layout (16-byte multiple). */
	std::size_t ConstantGroupSize(const StageLayout& a_layout, std::span<const std::int8_t> a_table, std::uint64_t a_variables, std::uint32_t a_firstVariable);
}
