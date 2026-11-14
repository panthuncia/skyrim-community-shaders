#pragma once

// Only for translation units built with the render graph (CS_HAS_RENDER_GRAPH): it needs BasicRHI.

#include <rhi.h>

#include <string>

/**
 * @brief Which BasicRHI backend the render graph runs on, and what that changes for the rest of CS.
 *
 * CS_ORG_NATIVE_BACKEND picks it: "vulkan" (the default) or "d3d12". Vulkan runs on DXVK's device (adoption, the owned frame) or on
 * the native mode's own device (CS_ORG_NATIVE=1). D3D12 runs only in the native mode: DXVK translates the game's D3D11 into
 * Vulkan, so there is no D3D12 device to share with it. The native mode's device creation calls Engine::CreateD3D12Device
 * (Engine/D3D12Device.h) when D3D12 is requested, then SetActive.
 */
namespace RenderGraph::Backend
{
	/** @brief CS_ORG_NATIVE_BACKEND, read once. An unknown value is logged and Vulkan is kept. */
	rhi::Backend Requested();

	/** @brief "Vulkan" or "D3D12", for logs and messages. */
	const char* Name(rhi::Backend a_backend);

	/** @brief Records the backend of the device the render graph runs on. The device's creator calls it once. */
	void SetActive(rhi::Backend a_backend);

	/** @brief The backend of the render graph's device: Vulkan until SetActive says otherwise. Callable from any thread. */
	rhi::Backend Active();

	/** @brief Active() is D3D12. */
	inline bool IsD3D12() { return Active() == rhi::Backend::D3D12; }

	/** @brief Whether this build has BasicRHI's D3D12 backend (the CS_ORG_D3D12 CMake option). */
	bool D3D12Built();

	/**
	 * @brief Why the requested backend cannot run in this configuration, or empty when it can. D3D12 needs the native mode
	 * (CS_ORG_NATIVE=1) and a build with the D3D12 backend.
	 */
	std::string RequestError();
}
