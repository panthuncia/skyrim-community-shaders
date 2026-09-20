#include "LightingConstants.h"

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
