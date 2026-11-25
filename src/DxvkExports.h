#pragma once

// The DXVK fork's exports from dxvk_d3d11.dll, resolved once for every user.

#include "RenderGraph/DxvkOrgInterop.h"

#include <cstddef>
#include <cstdint>
#include <string>

/** @brief dxvk_d3d11's DxvkViewTraceEntry (d3d11_view_srv.cpp, DXVK_VIEW_TRACE=1), as dxvkFindDestroyedView fills it. */
struct DxvkDestroyedView
{
	std::uintptr_t view, viewEnd, stream, streamEnd;
	std::uint32_t kind, thread;
	std::uint64_t tick;
	std::uint32_t frameCount;
	void* frames[24];
};

// Each fork export CS calls: its type, the table's field and the export's name. A new export goes here and nowhere else.
#define CS_DXVK_EXPORTS(X)                                                                                              \
	X(RequestSwapchainRecreateFn, requestSwapchainRecreate, dxvkRequestSwapchainRecreate)                               \
	X(SetSwapchainTornDownCallbackFn, setSwapchainTornDownCallback, dxvkSetSwapchainTornDownCallback)                   \
	X(GetPresenterSurfaceStateFn, getPresenterSurfaceState, dxvkGetPresenterSurfaceState)                               \
	X(SetSyncPresentFn, setSyncPresent, dxvkSetSyncPresent)                                                             \
	X(SetPresentQueueDepthFn, setPresentQueueDepth, dxvkSetPresentQueueDepth)                                           \
	X(SetTearingPreferenceFn, setTearingPreference, dxvkSetTearingPreference)                                           \
	X(PFN_dxvkRequestDeviceFeatures, requestDeviceFeatures, dxvkRequestDeviceFeatures)                                  \
	X(PFN_dxvkRequestDeviceExtensions, requestDeviceExtensions, dxvkRequestDeviceExtensions)                            \
	X(PFN_dxvkSetBindingModelCallback, setBindingModelCallback, dxvkSetBindingModelCallback)                            \
	X(PFN_dxvkGetInteropDeviceInfo, getInteropDeviceInfo, dxvkGetInteropDeviceInfo)                                     \
	X(PFN_dxvkCreateBufferFromVkBuffer, createBufferFromVkBuffer, dxvkCreateBufferFromVkBuffer)                         \
	X(PFN_dxvkCreateTextureFromVkImage, createTextureFromVkImage, dxvkCreateTextureFromVkImage)                         \
	X(PFN_dxvkCreateTextureFromVkImageWithData, createTextureFromVkImageWithData, dxvkCreateTextureFromVkImageWithData) \
	X(PFN_dxvkSetDeviceTeardownCallback, setDeviceTeardownCallback, dxvkSetDeviceTeardownCallback)                      \
	X(PFN_dxvkEnqueueInteropSubmission, enqueueInteropSubmission, dxvkEnqueueInteropSubmission)                         \
	X(PFN_dxvkEnqueueInteropSubmissions, enqueueInteropSubmissions, dxvkEnqueueInteropSubmissions)                      \
	X(PFN_dxvkGetInteropResourceInfo, getInteropResourceInfo, dxvkGetInteropResourceInfo)                               \
	X(PFN_dxvkGetInteropResourceInfos, getInteropResourceInfos, dxvkGetInteropResourceInfos)                            \
	X(PFN_dxvkTrackInteropResources, trackInteropResources, dxvkTrackInteropResources)                                  \
	X(PFN_dxvkUntrackInteropResources, untrackInteropResources, dxvkUntrackInteropResources)                            \
	X(PFN_dxvkPollInteropRelocations2, pollInteropRelocations2, dxvkPollInteropRelocations2)                            \
	X(PFN_dxvkReleaseInteropStorage, releaseInteropStorage, dxvkReleaseInteropStorage)                                  \
	X(PFN_dxvkEnqueueQueueCallback, enqueueQueueCallback, dxvkEnqueueQueueCallback)                                     \
	X(PFN_dxvkEmitCommandBufferCallback, emitCommandBufferCallback, dxvkEmitCommandBufferCallback)                      \
	X(PFN_dxvkSetCommandBufferBoundaryCallbacks, setCommandBufferBoundaryCallbacks, dxvkSetCommandBufferBoundaryCallbacks) \
	X(PFN_dxvkEmitExternalCommands, emitExternalCommands, dxvkEmitExternalCommands)                                     \
	X(PFN_dxvkEmitDeferredExternalCommands, emitDeferredExternalCommands, dxvkEmitDeferredExternalCommands)             \
	X(PFN_dxvkSetInteropOptions, setInteropOptions, dxvkSetInteropOptions)                                              \
	X(PFN_dxvkGetSubmissionCounter, getSubmissionCounter, dxvkGetSubmissionCounter)                                     \
	X(PFN_dxvkSetSubmissionTrace, setSubmissionTrace, dxvkSetSubmissionTrace)                                           \
	X(PFN_dxvkReadSubmissionTrace, readSubmissionTrace, dxvkReadSubmissionTrace)                                        \
	X(ResolveCheckpointMarkerFn, resolveCheckpointMarker, dxvkResolveCheckpointMarker)                                  \
	X(SetNextShaderNameFn, setNextShaderName, dxvkSetNextShaderName)                                                    \
	X(FindDestroyedViewFn, findDestroyedView, dxvkFindDestroyedView)                                                    \
	X(PFN_dxvkPushResourceOwner, pushResourceOwner, dxvkPushResourceOwner)                                              \
	X(PFN_dxvkPopResourceOwner, popResourceOwner, dxvkPopResourceOwner)                                                 \
	X(PFN_dxvkSetResourceOwner, setResourceOwner, dxvkSetResourceOwner)                                                 \
	X(PFN_dxvkRequestMemorySnapshot, requestMemorySnapshot, dxvkRequestMemorySnapshot)                                  \
	X(PFN_dxvkReadMemorySnapshot, readMemorySnapshot, dxvkReadMemorySnapshot)                                           \
	X(PFN_dxvkGetMemoryHeapReports, getMemoryHeapReports, dxvkGetMemoryHeapReports)                                     \
	X(PFN_dxvkConfigureMemoryDebug, configureMemoryDebug, dxvkConfigureMemoryDebug)                                     \
	X(PFN_dxvkGetResourceCreationStack, getResourceCreationStack, dxvkGetResourceCreationStack)                         \
	X(PFN_dxvkForceResourcePlacement, forceResourcePlacement, dxvkForceResourcePlacement)                               \
	X(PFN_dxvkSetResourcesSpillable, setResourcesSpillable, dxvkSetResourcesSpillable)                                  \
	X(PFN_dxvkSetDefragPressurePercent, setDefragPressurePercent, dxvkSetDefragPressurePercent)                         \
	X(SpirvIngestVersionFn, spirvIngestVersion, dxvkSpirvIngestVersion)                                                 \
	X(SpirvContainerHashFn, spirvContainerHash, dxvkSpirvContainerHash)                                                 \
	X(StreamEnableViewFn, streamEnableView, dxvkStreamEnableView)                                                       \
	X(StreamSetWindowFn, streamSetWindow, dxvkStreamSetWindow)                                                          \
	X(StreamGetWindowFn, streamGetWindow, dxvkStreamGetWindow)                                                          \
	X(StreamGetBindsFn, streamGetBinds, dxvkStreamGetBinds)

/**
 * @brief DXVK's exports. The table is resolved on the first Get() after dxvk_d3d11.dll is loaded, and stays for the process.
 *
 * Every export is required in graph mode. DxvkLoader stops the game there when one is missing, so graph code calls them
 * without checks. Plain D3D11 mode (CS_ORG=0, VR) still runs on a DXVK without some of them, so code that runs there
 * checks for null. Native mode loads no DXVK, and its table is empty.
 */
struct DxvkExports
{
	using RequestSwapchainRecreateFn = void (*)();
	using SetSwapchainTornDownCallbackFn = void (*)(bool (*)());
	using GetPresenterSurfaceStateFn = std::uint64_t (*)(std::uint32_t*, std::uint32_t*, std::uint32_t*);
	using SetSyncPresentFn = void (*)(std::uint32_t);
	using SetPresentQueueDepthFn = void (*)(std::uint32_t);
	using SetTearingPreferenceFn = void (*)(std::uint32_t);
	using ResolveCheckpointMarkerFn = BOOL(__stdcall*)(ID3D11Device*, const void*, char*, UINT);
	using SetNextShaderNameFn = void(__stdcall*)(const char*);
	using FindDestroyedViewFn = std::uint32_t(__stdcall*)(std::uintptr_t, DxvkDestroyedView*, std::uint32_t);
	using SpirvIngestVersionFn = std::uint32_t(__stdcall*)();
	using SpirvContainerHashFn = void(__stdcall*)(const void*, std::size_t, std::uint32_t*);
	using StreamMissingFn = void(__stdcall*)(std::uint64_t);
	using StreamGoneFn = void(__stdcall*)(std::uint64_t);
	using StreamFillFn = const D3D11_SUBRESOURCE_DATA*(__stdcall*)(std::uint64_t, BOOL);
	using StreamEnableViewFn = HRESULT(__stdcall*)(ID3D11ShaderResourceView*, StreamMissingFn, StreamGoneFn, StreamFillFn, std::uint64_t);
	using StreamSetWindowFn = void(__stdcall*)(ID3D11ShaderResourceView*, ID3D11ShaderResourceView*);
	using StreamGetWindowFn = ID3D11ShaderResourceView*(__stdcall*)(ID3D11ShaderResourceView*);
	using StreamGetBindsFn = std::uint64_t(__stdcall*)(ID3D11ShaderResourceView*);

#define CS_DXVK_EXPORT_FIELD(a_type, a_field, a_name) a_type a_field = nullptr;
	CS_DXVK_EXPORTS(CS_DXVK_EXPORT_FIELD)
#undef CS_DXVK_EXPORT_FIELD

	/**
	 * @brief Any thread. The table. Before dxvk_d3d11.dll is loaded, an empty table that is not kept, so a later call
	 * resolves the real one. In native mode (CS_ORG_NATIVE=1), always the empty table, with no module lookup.
	 */
	static const DxvkExports& Get();

	/** @brief Whether dxvk_d3d11.dll was found and the table resolved from it. */
	bool Loaded() const noexcept { return loaded; }

	/** @brief The exports this DXVK build lacks, comma-separated. Empty when it has every one. */
	std::string Missing() const;

private:
	bool loaded = false;
};
