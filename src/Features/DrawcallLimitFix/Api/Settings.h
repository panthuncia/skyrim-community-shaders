#pragma once

// A feature's own CS_* switches and live toggles, registered beside Core's.

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

	/** @brief A registered toggle. */
	struct ToggleId
	{
		std::uint32_t bit = ~0u;
		explicit operator bool() const { return bit != ~0u; }
	};

	/**
	 * @brief A live toggle, seeded from its switch and editable from the menu. Changes take effect at the next frame's start.
	 * after or before places it on the menu next to another toggle. With neither it goes last.
	 */
	struct FeatureToggle
	{
		std::string_view name;     // also the test harness key, dclf:<name>=<0|1>
		std::string_view label;
		std::string_view tooltip;
		std::string_view section;  // the menu heading it opens, or empty
		SwitchId seed;
		std::string_view onValue = "1";  // the seed value, besides unset, that turns it on
		bool entersClassification = false;  // a change drops the classification caches
		bool offByDefault = false;          // unset seeds it off
		std::string_view after;
		std::string_view before;    // set at most one of after and before
		std::string_view needs[2];  // toggles that must also be on, registered before this one
	};
}
