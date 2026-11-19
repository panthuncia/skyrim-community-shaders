#pragma once

#include <d3d11.h>

/**
* @brief The hooks behind the engine pipeline descriptions. The sampler anisotropy cap is always on.
* Everything else needs CS_ORG_ENGINE_SHADERS=1. Tripwire handles go into the engine's fields only while the guest context
* is installed, because only that context unwraps them.
*/
namespace RenderGraph::Engine::Pipeline
{
	/** @brief Anisotropy at most 8x, for performance. Applied to every sampler the device creates. */
	D3D11_SAMPLER_DESC CapSamplerAnisotropy(const D3D11_SAMPLER_DESC& a_desc);

	/** @brief Called once from the InitD3D hook, after the engine's blend table and before its sampler table. */
	void InstallDeviceHooks(ID3D11Device* a_device);

	/** @brief Only with CS_ORG_ENGINE_SHADERS=1. */
	void InstallGameHooks();

	/**
	* @brief Called at the end of Renderer::Init, the first point where the guest, all four state tables and both query pools exist.
	* Marks itself done only when it wrapped them.
	*/
	void InstallEngineHandlesOnce();
}
