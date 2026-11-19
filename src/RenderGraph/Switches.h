#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief CS's environment switches, read once.
 *
 * Each switch keeps the value it had when first read, so later environment changes are not seen.
 * CommunityShaders.env is applied before any switch is read. Every switch is listed in docs/development/render-graph.md.
 * Any thread. Each call takes a lock, so keep a hot-path switch in a local static.
 */
namespace Switches
{
	/** @brief The value as first read, empty when unset. */
	std::string Value(std::string_view a_name);

	bool IsSet(std::string_view a_name);

	bool Equals(std::string_view a_name, std::string_view a_value);

	/** @brief Whether the value is a_value, ignoring ASCII case. */
	bool EqualsNoCase(std::string_view a_name, std::string_view a_value);

	/** @brief Whether the switch is "1", an opt-in. */
	inline bool On(std::string_view a_name) { return Equals(a_name, "1"); }

	/** @brief Whether the switch is "0", an opt-out of a default. */
	inline bool Off(std::string_view a_name) { return Equals(a_name, "0"); }

	/** @brief a_default when unset or not a number. */
	std::uint64_t Number(std::string_view a_name, std::uint64_t a_default = 0);

	/** @brief A comma-separated list with items trimmed and empty items dropped. Empty when unset. */
	std::vector<std::string> List(std::string_view a_name);
}
