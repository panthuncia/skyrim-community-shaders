#pragma once

// Only for translation units built with the render graph, since it needs BasicRHI.

#include <rhi.h>

#include <string>

/**
 * @brief Which BasicRHI backend the render graph runs on.
 *
 * CS_ORG_NATIVE_BACKEND picks "vulkan" (the default) or "d3d12". D3D12 runs only in native mode, since DXVK
 * gives no D3D12 device to share. Native mode's device creation calls SetActive.
 */
namespace RenderGraph::Backend
{
	/** @brief CS_ORG_NATIVE_BACKEND, read once. An unknown value is logged and Vulkan is kept. */
	rhi::Backend Requested();

	const char* Name(rhi::Backend a_backend);

	/** @brief The device's creator calls it once. */
	void SetActive(rhi::Backend a_backend);

	/** @brief Vulkan until SetActive says otherwise. Any thread. */
	rhi::Backend Active();

	inline bool IsD3D12() { return Active() == rhi::Backend::D3D12; }

	/** @brief Whether this build has BasicRHI's D3D12 backend (the CS_ORG_D3D12 CMake option). */
	bool D3D12Built();

	/** @brief Why the requested backend cannot run, or empty when it can. D3D12 needs native mode and a D3D12 build. */
	std::string RequestError();
}
