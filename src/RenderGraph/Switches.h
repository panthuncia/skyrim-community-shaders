#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/**
 * @brief CS's environment switches (CS_ORG_*, CS_GPU_*, the diagnostics), read once.
 *
 * Each switch is read from the process environment the first time any accessor asks for it, and that value is kept for the
 * session: a later change to the environment is not seen. CommunityShaders.env sets the environment before any switch is read
 * (XSEPlugin.cpp), so the file and the real environment both reach these accessors. Every switch is listed in
 * docs/development/render-graph.md, "Switches".
 *
 * Callable from any thread. A switch read on a hot path should still be kept in a local static: each call takes a lock.
 */
namespace Switches
{
	/** @brief The switch's value as it was first read; empty when it is unset. */
	std::string Value(std::string_view a_name);

	/** @brief Whether the switch is set to a non-empty value. */
	bool IsSet(std::string_view a_name);

	/** @brief Whether the switch's value is exactly a_value. */
	bool Equals(std::string_view a_name, std::string_view a_value);

	/** @brief Whether the switch's value is a_value, ignoring ASCII case. */
	bool EqualsNoCase(std::string_view a_name, std::string_view a_value);

	/** @brief Whether the switch is "1": an opt-in diagnostic or path. */
	inline bool On(std::string_view a_name) { return Equals(a_name, "1"); }

	/** @brief Whether the switch is "0": an opt-out of a default. */
	inline bool Off(std::string_view a_name) { return Equals(a_name, "0"); }

	/** @brief The switch as an unsigned number; a_default when it is unset or not a number. */
	std::uint64_t Number(std::string_view a_name, std::uint64_t a_default = 0);
}
