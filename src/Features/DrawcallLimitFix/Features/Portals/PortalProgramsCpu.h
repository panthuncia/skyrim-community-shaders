#pragma once

// PortalPrograms.hlsli's evaluation on the CPU, for the CPU-driven pipeline. Same arithmetic and comparisons as the shader.
// A producer invariant failure rejects the bound, as the GPU does. Any thread. Reads only the words it is given.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"

#include <cstdint>
#include <memory>
#include <span>

namespace DCLF::Portals
{
	/**
	 * @brief PortalTestAt on the CPU: 1 in view, 0 culled, -1 no program.
	 * a_nodeFlags bits: always-draw 11, preprocessed 12, preprocess-hidden 20. A member's query passes 0.
	 */
	int PortalTestCpuAt(std::span<const std::uint32_t> a_words, std::size_t a_baseWord, std::uint32_t a_program, const float a_centre[3], float a_radius,
		std::uint32_t a_nodeFlags);
	/** @brief PortalTestCpu for the main camera's programs, at word 0. */
	int PortalTestCpu(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags);

	/**
	 * @brief PortalTestCpu over a_count bounds of one program, four at a time.
	 * A program not in forward topological order, or too long, falls back to testing per bound.
	 */
	void PortalTestCpuBatch(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		const std::uint16_t* a_lanes, std::uint32_t a_count, std::uint8_t* a_rejected);

	/** @brief Whether a strict light culls object a_object of the bound. */
	bool StrictGateRejectsCpu(const Direct::StrictGates& a_gates, std::uint32_t a_context, std::uint32_t a_object, const float a_centre[3], float a_radius);

	/** @brief The gates a strict light's CPU cull reads, held by a_hold while the cull reads them. */
	Direct::StrictGates StrictGatesOf(std::span<const std::uint32_t> a_gates, std::span<const std::uint32_t> a_atlas, std::span<const std::uint32_t> a_objectGates,
		std::shared_ptr<const void> a_hold);
}
