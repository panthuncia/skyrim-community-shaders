#include "LightingConstants.h"

#include <bit>

#include <cstring>

namespace DCLF
{
	namespace
	{
		void StoreRelative(float* a_out, const float (&a_world)[12], const RE::NiPoint3& a_posAdjust)
		{
			std::memcpy(a_out, a_world, sizeof(a_world));
			a_out[3] = a_world[3] - a_posAdjust.x;
			a_out[7] = a_world[7] - a_posAdjust.y;
			a_out[11] = a_world[11] - a_posAdjust.z;
		}

		// A variable's dword offset in the shader's cbuffer, or ~0 when the permutation lacks it (offset 0
		// is also what reflection leaves for missing variables, so it only counts for the group's first).
		std::uint32_t OffsetOf(std::span<const std::int8_t> a_table, std::uint32_t a_variable, std::uint32_t a_firstVariable)
		{
			if (a_variable >= a_table.size())
				return ~0u;
			const std::uint32_t offset = static_cast<std::uint8_t>(a_table[a_variable]);
			return (offset == 0 && a_variable != a_firstVariable) ? ~0u : offset;
		}
	}

	GeometryConstants ObjectGeometryConstants(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags, const RE::NiPoint3& a_eye,
		const RE::NiPoint3& a_previousEye)
	{
		const auto& object = a_tables.objects[a_objectIndex];
		GeometryConstants constants = a_tables.geometryConstants[object.pipelineIndex];
		const auto& vsLayout = LightingVSLayout();
		const auto& psLayout = LightingPSLayout();
		StoreRelative(&constants.vs.floats[vsLayout.offset[kVSWorld]], object.world, a_eye);
		// Render flag 0x10: the previous transform is the current one (engine notes: SetupGeometry).
		StoreRelative(&constants.vs.floats[vsLayout.offset[kVSPreviousWorld]], (a_renderFlags & 0x10) ? object.world : object.previousWorld, a_previousEye);
		const auto& shading = a_tables.shading[a_objectIndex];
		std::memcpy(&constants.ps.floats[psLayout.offset[kPSMaterialData]], shading.materialData, sizeof(shading.materialData));
		std::memcpy(&constants.ps.floats[psLayout.offset[kPSEmitColor]], shading.emitColor, sizeof(shading.emitColor));
		constants.ps.floats[psLayout.offset[kPSSSRParams] + 3] = shading.ssrSpecular;
		return constants;
	}

	GeometryPatchOffsets GeometryPatchOffsetsOf(std::span<const std::int8_t> a_vsTable, std::span<const std::int8_t> a_psTable)
	{
		const auto& vsLayout = LightingVSLayout();
		const auto& psLayout = LightingPSLayout();
		GeometryPatchOffsets offsets;
		offsets.vsWorld = OffsetOf(a_vsTable, kVSWorld, kVSFirstVariable[kPerGeometry]);
		offsets.vsWorldSize = vsLayout.size[kVSWorld];
		offsets.vsPreviousWorld = OffsetOf(a_vsTable, kVSPreviousWorld, kVSFirstVariable[kPerGeometry]);
		offsets.vsPreviousWorldSize = vsLayout.size[kVSPreviousWorld];
		offsets.psMaterialData = OffsetOf(a_psTable, kPSMaterialData, kPSFirstVariable[kPerGeometry]);
		offsets.psMaterialDataSize = psLayout.size[kPSMaterialData];
		offsets.psEmitColor = OffsetOf(a_psTable, kPSEmitColor, kPSFirstVariable[kPerGeometry]);
		offsets.psEmitColorSize = psLayout.size[kPSEmitColor];
		offsets.psSSRParams = OffsetOf(a_psTable, kPSSSRParams, kPSFirstVariable[kPerGeometry]);
		offsets.psSSRParamsSize = psLayout.size[kPSSSRParams];
		return offsets;
	}

	void PatchObjectGeometry(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags,
		const RE::NiPoint3& a_eye, const RE::NiPoint3& a_previousEye, const GeometryPatchOffsets& a_offsets,
		std::span<std::byte> a_vsOut, std::span<std::byte> a_psOut)
	{
		const auto& object = a_tables.objects[a_objectIndex];
		const auto& shading = a_tables.shading[a_objectIndex];

		// Writes a_count floats at a float offset, clipped to the group. Unwritten components (the quiet
		// NaN ConstantBlock uses as its sentinel) become zero, matching PackConstantGroup.
		auto write = [](std::span<std::byte> a_out, std::uint32_t a_offset, std::uint32_t a_count, const float* a_values, std::uint32_t a_available) {
			if (a_offset == ~0u)
				return;
			for (std::uint32_t c = 0; c < a_count; ++c) {
				const std::size_t at = (std::size_t(a_offset) + c) * 4;
				if (at + 4 > a_out.size())
					return;
				float value = c < a_available ? a_values[c] : 0.0f;
				if (std::bit_cast<std::uint32_t>(value) == kUnwrittenBits)
					value = 0.0f;
				std::memcpy(a_out.data() + at, &value, 4);
			}
		};

		float world[16] = {};
		StoreRelative(world, object.world, a_eye);
		write(a_vsOut, a_offsets.vsWorld, a_offsets.vsWorldSize, world, 12);
		// Render flag 0x10: the previous transform is the current one (engine notes: SetupGeometry).
		StoreRelative(world, (a_renderFlags & 0x10) ? object.world : object.previousWorld, a_previousEye);
		write(a_vsOut, a_offsets.vsPreviousWorld, a_offsets.vsPreviousWorldSize, world, 12);

		write(a_psOut, a_offsets.psMaterialData, a_offsets.psMaterialDataSize, shading.materialData,
			static_cast<std::uint32_t>(std::size(shading.materialData)));
		write(a_psOut, a_offsets.psEmitColor, a_offsets.psEmitColorSize, shading.emitColor,
			static_cast<std::uint32_t>(std::size(shading.emitColor)));
		// Only the w component of SSRParams is per-object; the rest stays as the template packed it.
		if (a_offsets.psSSRParams != ~0u && a_offsets.psSSRParamsSize > 3) {
			const std::size_t at = (std::size_t(a_offsets.psSSRParams) + 3) * 4;
			if (at + 4 <= a_psOut.size()) {
				float value = shading.ssrSpecular;
				if (std::bit_cast<std::uint32_t>(value) == kUnwrittenBits)
					value = 0.0f;
				std::memcpy(a_psOut.data() + at, &value, 4);
			}
		}
	}

	std::size_t ConstantGroupSize(const StageLayout& a_layout, std::span<const std::int8_t> a_table, std::uint64_t a_variables, std::uint32_t a_firstVariable)
	{
		std::size_t end = 0;
		for (std::uint32_t i = 0; i < a_layout.count; ++i) {
			if (!((a_variables >> i) & 1))
				continue;
			if (const auto offset = OffsetOf(a_table, i, a_firstVariable); offset != ~0u)
				end = std::max<std::size_t>(end, (offset + a_layout.size[i]) * 4);
		}
		return (end + 15) & ~std::size_t(15);
	}

	std::size_t PackConstantGroup(const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::int8_t> a_table, std::uint64_t a_variables,
		std::uint32_t a_firstVariable, std::span<std::byte> a_out)
	{
		const std::size_t size = ConstantGroupSize(a_layout, a_table, a_variables, a_firstVariable);
		if (size > a_out.size())
			return 0;
		std::memset(a_out.data(), 0, size);
		for (std::uint32_t i = 0; i < a_layout.count; ++i) {
			if (!((a_variables >> i) & 1))
				continue;
			const auto offset = OffsetOf(a_table, i, a_firstVariable);
			if (offset == ~0u)
				continue;
			for (std::uint32_t c = 0; c < a_layout.size[i]; ++c) {
				if (a_block.Written(a_layout.offset[i] + c))
					std::memcpy(a_out.data() + (offset + c) * 4, &a_block.floats[a_layout.offset[i] + c], 4);
			}
		}
		return size;
	}
}
