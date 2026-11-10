#pragma once

#include <d3d11.h>

/**
 * @brief The hooks behind the engine pipeline descriptions (Descriptions.h, org-full-port.md P4.6).
 *
 * The sampler anisotropy cap is always on. Everything else needs CS_ORG_ENGINE_SHADERS=1:
 * - The device's shader and input layout creation record descriptions (every creator, CS's own included).
 * - BSGraphics::Renderer's shader loaders (75547/75548/75551/75552/75556) and CreateInputLayout (75583), and Renderer::Init (the
 *   state tables and query pools), put tripwire handles that wrap the real objects into the engine's fields. Only while P4.1's
 *   guest context is installed (CS_ORG_TRIPWIRE_GUEST=1): the engine then binds them through a context that unwraps them.
 *   Without it the engine's fields keep the D3D11 objects.
 *
 * RE: docs/development/p4/p4.6-shaders-states.md.
 */
namespace RenderGraph::Engine::Pipeline
{
	/** @brief The engine's sampler limit: anisotropy at most 8x, for performance. Every sampler the device creates gets it. */
	D3D11_SAMPLER_DESC CapSamplerAnisotropy(const D3D11_SAMPLER_DESC& a_desc);

	/**
	 * @brief Detours the device's CreateSamplerState (the cap) and, with CS_ORG_ENGINE_SHADERS=1, its shader and input layout
	 * creation. Called once from the InitD3D hook, after the engine's blend table and before its sampler table.
	 */
	void InstallDeviceHooks(ID3D11Device* a_device);

	/** @brief Hooks::Install: the engine's creation functions and Renderer::Init, with CS_ORG_ENGINE_SHADERS=1 only. */
	void InstallGameHooks();
}
