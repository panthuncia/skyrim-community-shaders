#pragma once

// Every switch the game knows: one table (SwitchRegistry.cpp), plus the switches DCLF's features register. The test
// configuration's names are checked against it, and CS_TEST_LIST_SWITCHES=1 writes it to CommunityShaders-Switches.json,
// which is cs-test's schema (tools/cs-test/switches.json).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Testing::SwitchRegistry
{
	/** @brief What a switch is for. DCLF's startup summary groups its switches by this. DCLF's Api::SwitchKind has the same order. */
	enum class Kind : std::uint8_t
	{
		Feature,
		Parity,
		Diagnostic,
		Test,
		Other,
	};

	/** @brief The values a switch takes, for cs-test's typed keys. Bool is "1" or "0". */
	enum class Type : std::uint8_t
	{
		String,
		Bool,
		Int,
		Enum,
	};

	/** @brief One switch of the table. */
	struct Row
	{
		std::string_view name;
		std::string_view key;           // cs-test's typed config key (<table>.<name>), or empty: configs set it in [env]
		Type type = Type::String;
		std::string_view defaultValue;  // the text it acts as when unset, or empty
		std::string_view choices;       // an Enum's values, separated by '|'
		Kind kind = Kind::Other;
		std::string_view description;
	};

	/** @brief The table's row for a_name, or null. DCLF's features' switches are not in it. */
	const Row* Find(std::string_view a_name);

	/** @brief A kind's name in lower case: feature, parity, diagnostic, test or other. */
	std::string_view KindName(Kind a_kind);

	/** @brief What the registry says about a name. */
	enum class Known
	{
		Yes,
		No,
		Unchecked,  ///< a CS_DCLF_ name that may be a DCLF feature's switch, which is not registered (yet, or DCLF did not install)
	};

	/** @brief Whether a_name is a known switch. Families read by a computed name (DXVK_, VK_, CS_GPU_COUNTERS_, CS_NVPERF_) are known by their prefix. */
	Known Check(std::string_view a_name);

	/** @brief Any thread: a switch was read by name. A name the registry does not know is logged once, so the table cannot drift unseen. */
	void NoteRead(std::string_view a_name);

	/** @brief kDataLoaded: checks the names read before DCLF's registry was complete. With DCLF off, its feature names stay unchecked. */
	void CheckPendingReads();

	/**
	 * @brief kPostPostLoad, after the features: with CS_TEST_LIST_SWITCHES=1, writes CommunityShaders-Switches.json in the log
	 * folder and ends the process (outcome passed in a test run).
	 */
	void DumpIfAsked();
}
