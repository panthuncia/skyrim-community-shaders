#pragma once

#include <cstdint>

#include <ankerl/unordered_dense.h>

namespace RE
{
	class BSGeometry;
}

namespace DCLF
{
	/**
	 * @brief The DCLF set: the objects DCLF draws, by phase. The engine never draws them in a view DCLF draws.
	 *
	 * An object is in a phase when it is eligible, takes part in the phase and everything the phase draws it with is ready.
	 * Each phase is whole, so its views withhold exactly what its draws draw. CommitSet decides it once a frame, and SetSnapshot
	 * is the engine-facing copy the registration hooks read.
	 */
	enum SetPhase : std::uint8_t
	{
		kSetMain = 1u << 0,                    // the main camera's Z-prepass and colour pass
		kSetCaster = 1u << 1,                  // the plain and clamped shadow views (the sun's cascades, spot lights)
		// Bits 2 and 3 belong to the occlusion maps, declared by features.
		kSetCasterPoint = 1u << 4,             // the paraboloid shadow views (point lights)
	// Bits 5 and 6 belong to features' views, such as the reflection and first person.
	};
	/** @brief A feature's view phase is never one of these. */
	inline constexpr std::uint8_t kCoreSetPhases = kSetMain | kSetCaster | kSetCasterPoint;

	/** @brief The phase a shadow build's caster mode draws: 0 plain, 1 clamped, 2 paraboloid, then the occlusion maps' modes. */
	inline constexpr std::uint8_t SetPhaseOfMode(std::uint32_t a_mode)
	{
		return a_mode < 2 ? kSetCaster : kSetCasterPoint;
	}

	/**
	 * @brief The set for one frame, by geometry. Immutable once published.
	 *
	 * pending holds objects waiting to join. Neither DCLF nor the engine draws them yet, so an object shows only once DCLF draws it.
	 * Only the registration hooks read pending.
	 */
	struct SetSnapshot
	{
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint8_t> phases;
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint8_t> pending;
		std::uint32_t frame = 0;
		std::uint8_t drawn = 0;  // the phases DCLF draws this frame (a member's phases are within them)

		std::uint8_t PhasesOf(const RE::BSGeometry* a_geometry) const
		{
			const auto it = phases.find(a_geometry);
			return it != phases.end() ? it->second : std::uint8_t{ 0 };
		}
		/** @brief A member's phases, or a pending object's. */
		std::uint8_t WithheldPhasesOf(const RE::BSGeometry* a_geometry) const
		{
			const auto it = pending.find(a_geometry);
			return static_cast<std::uint8_t>(PhasesOf(a_geometry) | (it != pending.end() ? it->second : std::uint8_t{ 0 }));
		}
	};
}
