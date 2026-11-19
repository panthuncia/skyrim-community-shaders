#pragma once

// Core's side of program families: each family's entry, resolved once at install after the material ports,
// and the rules Core reads per object per frame as data, never calling a family.

#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ProgramFamilySlots.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace DCLF::Extensions
{
	inline constexpr std::size_t kProgramFamilies = static_cast<std::size_t>(Api::ProgramFamily::Count);

	/**
	 * @brief What Core reads of a key's family per object per frame.
	 * The extra entry at kProgramFamilies, also used for untaken families, describes a plain Lighting key.
	 */
	struct ProgramFamilyRules
	{
		std::uint32_t keyBit = 0;      // 0 for a Lighting key
		std::uint32_t spareShift = 0;
		std::uint8_t kind = 0;         // material kind of its keys' shader
		bool rowBlocks = true;
		bool depthSegment = true;      // false for ForwardOrDecal, with no depth segment input or Z-prepass bucket
		bool rootViewsForwardOnly = false;
		bool turnedKeyBits = false;
		std::uint32_t projectedPassBit = 0;  // 0 means no ProjectedUV binding
		Api::ProgramFamilyContribution::Records records;
	};
	using ProgramFamilyRulesTable = std::array<ProgramFamilyRules, kProgramFamilies + 1>;

	/** @brief A family's entry, with its strings owned. */
	struct ProgramFamilyEntry
	{
		bool taken = false;
		Api::ProgramFamilyContribution contribution;
		std::string name, keyName, sourcePath, nativeNote;
		std::uint8_t kind = 0;
	};
	using ProgramFamilySet = std::array<ProgramFamilyEntry, kProgramFamilies>;

	/** @brief Main thread, at the end of InstallFeatures, after the material ports. Fails if a family's shader type has no material port. */
	void ResolveProgramFamilies(const std::array<Api::ProgramFamilyContribution, kProgramFamilies>& a_registered);
	/** @brief Any thread, after InstallFeatures. */
	const ProgramFamilySet& ProgramFamilies();
	inline const ProgramFamilyEntry& ProgramFamilyOf(Api::ProgramFamily a_family) { return ProgramFamilies()[static_cast<std::size_t>(a_family)]; }
	/** @brief The taken family whose programs use a_shaderType, or Count. */
	Api::ProgramFamily FamilyOfShader(std::uint8_t a_shaderType);

	/** @brief Any thread, after InstallFeatures. Rules by family, then a Lighting key's. */
	const ProgramFamilyRulesTable& FamilyRulesTable();
	/** @brief The rules for a key's raster flags. */
	inline const ProgramFamilyRules& FamilyRules(std::uint32_t a_rasterFlags)
	{
		return FamilyRulesTable()[static_cast<std::size_t>(KeyFamily(a_rasterFlags))];
	}
	/** @brief The rules for a record of material kind a_kind. */
	const ProgramFamilyRules& FamilyRulesOfKind(std::uint8_t a_kind);
}
