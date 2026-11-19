#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <d3d11.h>

/**
 * @brief VRAM attribution's tracing: finds the creators of memory no owner scope or name claims.
 *
 * Always on (with DXVK's attribution exports):
 * - DXVK records each buffer's and image's creation stack; a resource without an owner or a name is traced to the first frame
 *   outside DXVK (SkyrimSE.exe -> the game, another module -> that plugin, CS -> the function).
 * - The game's texture loads (BSShaderManager::GetTexture) label their textures "Game/Textures/<folder>" with their path.
 *
 * Debug mode (CS_VRAM_DEBUG=1, from the environment or the switches file) is read by the attribution's callers. ORG's device memory
 * comes from BasicRHI's live ledger (VramLedger.cpp), in every mode.
 */
namespace Vram::Debug
{
	/** @brief Whether debug mode is on (CS_VRAM_DEBUG=1). */
	bool Enabled();

	/** @brief Turns DXVK's creation stacks on; right after dxvk_d3d11.dll loads, before the device is created. */
	void ConfigureDxvk(HMODULE a_dxvkD3D11);

	/** @brief Installs the game texture-load hook; with the other game hooks. */
	void InstallGameHooks();

	/** @brief The owner a game texture of a_path's folder is labelled with ("Game/Textures/landscape"). The string lives as long as the process. Any thread. */
	const char* GameTextureOwner(std::string_view a_path);

	/** @brief Labels a texture as a game texture of a_path's folder, with a_path as its name. Any thread. */
	void TagGameTexture(ID3D11Resource* a_resource, std::string_view a_path);

	/** @brief Labels textures whose queued loads have finished since the last call. Any thread. */
	void ProcessPendingTextures();

	/** @brief Where a stack enters from outside DXVK: its module, and a description of the frame (and the two after it). */
	struct CallSite
	{
		std::string module;  // file name, lower case
		std::string text;
	};

	/** @brief A DXVK resource's creation call site; empty module if no stack was recorded. */
	CallSite DxvkCreationSite(std::uint64_t a_cookie);
}
