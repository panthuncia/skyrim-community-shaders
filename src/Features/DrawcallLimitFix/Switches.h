#pragma once

#include <string>

namespace DCLF
{
	/**
	 * @brief The value of a `CS_DCLF_*` switch, from the environment or from the switches file.
	 *
	 * The environment is checked first, then `<Documents>\My Games\Skyrim Special Edition\SKSE\
	 * CommunityShaders-DCLF.ini`, a flat `NAME=value` file in the directory CS already writes its log to.
	 * The file exists because the game does not reliably inherit the environment of whatever started it:
	 * Mod Organizer 2 hands the game its own environment block, so switches exported by a launcher script
	 * never arrive, and a run silently behaves as if every switch were off. Reading a file removes that
	 * whole class of "the run did not test what it was meant to test".
	 *
	 * Read once, on first use, so a switch cannot change under a running frame. Returned by value: the
	 * cache is a hash map whose values move when it grows, so a reference into it would not stay valid.
	 *
	 * @return the value, or an empty string when the switch is set in neither place.
	 */
	std::string SwitchValue(const char* a_name);

	/** @brief Whether a switch is set to "1". */
	bool SwitchEnabled(const char* a_name);

	/** @brief Every switch that is set, as `NAME=value` pairs, for the startup log. */
	std::string SwitchSummary();
}
