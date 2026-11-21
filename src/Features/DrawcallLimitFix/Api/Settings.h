#pragma once

// A feature's own CS_* switches, registered beside Core's.

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	/** @brief What a switch is for. The startup summary groups switches by this. */
	enum class SwitchKind : std::uint8_t
	{
		Feature,
		Parity,
		Diagnostic,
		Test,
		Other,
	};

	/** @brief Which values of a feature switch count as a reduced featureset in the startup log. */
	enum class SwitchReduced : std::uint8_t
	{
		Never,      // not a feature
		UnlessOne,  // on when unset or "1"
		Zero,       // off only with "0"
	};

	/** @brief A registered switch. */
	struct SwitchId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/**
	 * @brief A switch, read once from the environment or the switches file when registered.
	 * after or before places it in the startup summary next to another switch. With neither it goes last.
	 */
	struct FeatureSwitch
	{
		std::string_view name;  // CS_DCLF_...
		SwitchKind kind = SwitchKind::Feature;
		SwitchReduced reduced = SwitchReduced::Never;
		std::string_view description;
		std::string_view after;
		std::string_view before;  // set at most one of after and before
	};
}
