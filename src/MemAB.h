#pragma once

/**
 * @brief What the ORG/DXVK branch's test harness and VRAM dump give it, for A/B memory runs of this build (tools/cs-ab there).
 *
 * Off unless CommunityShaders.env beside the log (or the real environment) sets CS_DCLF_TEST_* or CS_AB_PROBE. The commands
 * use the same names and forms as that branch's, so one command list drives both builds through the same path.
 */
namespace MemAB
{
	/** @brief Reads CommunityShaders.env into the environment. Before anything reads a switch. */
	void ApplyEnvironmentFile();
	/** @brief Starts tracking every resource the device creates. Right after device creation. */
	void Track(ID3D11Device* a_device);
	/** @brief Runs what is due. Each Present. */
	void OnPresent();
	/** @brief A resource's name for the dump (Util::SetResourceName). */
	void NoteName(ID3D11DeviceChild* a_resource, const char* a_name);
}
