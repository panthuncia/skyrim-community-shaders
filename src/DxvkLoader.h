#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <filesystem>
#include <cs_dxvk_api.h>

// Loads the prefixed DXVK DLLs from CommunityShaders/bin so they do not alias
// Skyrim's process-wide System32 d3d11.dll and dxgi.dll modules.
namespace DxvkLoader
{
	struct Api
	{
		HMODULE d3d11Module = nullptr;
		HMODULE dxgiModule = nullptr;
		PFN_csDxvkSetTearingPreference setTearingPreference = nullptr;
		PFN_csDxvkGetPresenterSurfaceState getPresenterSurfaceState = nullptr;
		PFN_csDxvkSetFrameGenOwnershipQuery setFrameGenOwnershipQuery = nullptr;
		PFN_csDxvkSetPresentCallback setPresentBeginCallback = nullptr;
		PFN_csDxvkSetPresentCallback setPresentCompletedCallback = nullptr;
		PFN_csDxvkRequestSwapchainRecreate requestSwapchainRecreate = nullptr;
		PFN_csDxvkSetSwapchainTornDownCallback setSwapchainTornDownCallback = nullptr;
		PFN_csDxvkSetTargetFrameRate setTargetFrameRate = nullptr;
		PFN_csDxvkSetSyncPresent setSyncPresent = nullptr;
		PFN_csDxvkSetPresentQueueDepth setPresentQueueDepth = nullptr;
		PFN_csDxvkEnqueueInteropCommandBuffer enqueueInteropCommandBuffer = nullptr;
		PFN_csDxvkGetPresentWaitSemaphoreState getPresentWaitSemaphoreState = nullptr;
		PFN_csDxvkClearPresentWaitSemaphore clearPresentWaitSemaphore = nullptr;
		PFN_csDxvkCancelPresentWaitSemaphore cancelPresentWaitSemaphore = nullptr;
		PFN_csDxvkReleaseQueuedPresentWaitSemaphoresAfterIdle releaseQueuedPresentWaitSemaphoresAfterIdle = nullptr;

		[[nodiscard]] bool HasFrameGenerationControl() const;
		[[nodiscard]] bool HasPresentWaitInterop() const;
	};

	/** @brief Loads DXVK before the game creates its D3D11 device. */
	bool Load();

	/** @brief Returns whether DXVK loaded successfully. */
	bool IsLoaded();
	/** @brief Returns the validated DXVK extension table, or an empty table in native mode. */
	const Api& GetApi();

	/** @brief Returns whether settings or CS_NATIVE_D3D11 request the native runtime. */
	bool NativeModeRequested();

	/** @brief Returns the module-relative renderer runtime directory. */
	std::filesystem::path GetRuntimeDir();

	/** @brief Returns DXVK's D3D11CreateDeviceAndSwapChain export. */
	decltype(&D3D11CreateDeviceAndSwapChain) GetD3D11CreateDeviceAndSwapChain();

	/** @brief Returns DXVK's CreateDXGIFactory export. */
	decltype(&CreateDXGIFactory) GetCreateDXGIFactory();
}
