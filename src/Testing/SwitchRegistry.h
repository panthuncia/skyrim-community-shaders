#pragma once

// Every switch the game knows: DCLF's registry and its features' registered switches, then a table of the other families. The test
// configuration's names are checked against it, and CS_TEST_LIST_SWITCHES=1 writes it to CommunityShaders-Switches.json.

#include <string>
#include <string_view>
#include <vector>

namespace Testing::SwitchRegistry
{
	/** @brief One switch: its name, kind (DCLF's, in lower case, or "other"), description and family (dclf, org, cs, basicrhi, dxvk, vk). */
	struct Info
	{
		std::string name, kind, description, family;
	};

	/** @brief What the registry says about a name. */
	enum class Known
	{
		Yes,
		No,
		Unchecked,  ///< a CS_DCLF_ name that may be a DCLF feature's switch, which is not registered (yet, or DCLF did not install)
	};

	/** @brief Whether a_name is a known switch. External families (DXVK_, VK_) are known by their prefix. */
	Known Check(std::string_view a_name);

	/** @brief Every known switch, by name. */
	std::vector<Info> All();

	/** @brief Any thread: a switch was read by name. A name the registry does not know is logged once, so the table cannot drift unseen. */
	void NoteRead(std::string_view a_name);

	/** @brief kDataLoaded: checks the names read before DCLF's registry was complete. With DCLF off, its feature names stay unchecked. */
	void CheckPendingReads();

	/**
	 * @brief kPostPostLoad, after the features: with CS_TEST_LIST_SWITCHES=1, writes CommunityShaders-Switches.json in the log
	 * folder and ends the process.
	 */
	void DumpIfAsked();
}
