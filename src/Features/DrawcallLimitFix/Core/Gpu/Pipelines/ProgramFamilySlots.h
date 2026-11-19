#pragma once

// Core's fixed slot per program family, whichever feature takes it: its raster key bit, spare key bits, stage cache key bit,
// program id bit and Z key salt. Adding a family means the enum value, its slot here, and the feature's registration.

#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace DCLF
{
	/** @brief A program family's slot. */
	struct ProgramFamilySlot
	{
		Api::ProgramFamily family;
		std::uint32_t keyBit;
		std::uint32_t spareShift;  // kProgramFamilySpareBits of them
		std::uint64_t stageKeyBit;
		std::uint64_t programIdBit;
		std::uint64_t zSalt;  // XORed into its pipelines' Z key's vertex hash
	};
	/** @brief The spare key bits a family may use, at its spareShift. */
	inline constexpr std::uint32_t kProgramFamilySpareBits = 7u;

	inline constexpr std::array<ProgramFamilySlot, static_cast<std::size_t>(Api::ProgramFamily::Count)> kProgramFamilySlots = { {
		{ Api::ProgramFamily::Effect, 1u << 17, 21, 1ull << 37, 1ull << 63, 0xEFFEC7EFFEC7EFFEull },
	} };
	static_assert(kProgramFamilySlots[static_cast<std::size_t>(Api::ProgramFamily::Effect)].family == Api::ProgramFamily::Effect, "kProgramFamilySlots is by family");

	constexpr const ProgramFamilySlot& ProgramFamilySlotOf(Api::ProgramFamily a_family) { return kProgramFamilySlots[static_cast<std::size_t>(a_family)]; }

	/** @brief The family whose key bit a_rasterFlags carry, or Count for a Lighting key. */
	constexpr Api::ProgramFamily KeyFamily(std::uint32_t a_rasterFlags)
	{
		for (const auto& slot : kProgramFamilySlots)
			if (a_rasterFlags & slot.keyBit)
				return slot.family;
		return Api::ProgramFamily::Count;
	}
}
