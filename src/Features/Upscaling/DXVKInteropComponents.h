#pragma once

#include <cs_dxvk_api.h>
#include <d3d11.h>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>
#include <winrt/base.h>

struct IDXGIVkInteropDevice;
struct ID3DLowLatencyDevice;

enum class DXVKPresentWaitState : uint32_t
{
	kNone = 0,
	kPending = 1,
	kQueued = 2,
	kUncertain = 3,
	kReleased = 4,
};

/** Owns DXVK's COM bridge, Vulkan handles, and submission-queue lock state. */
class DXVKDeviceQueueInteropOwner
{
private:
	friend class DXVKInterop;

	mutable std::recursive_mutex mutex;
	bool available = false;
	winrt::com_ptr<IDXGIVkInteropDevice> interopDevice;
	winrt::com_ptr<ID3DLowLatencyDevice> lowLatencyDevice;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamilyIndex = 0;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
	PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
	PFN_vkDestroyImageView vkDestroyImageView = nullptr;
	bool submissionQueueLockUncertain = false;
};

/** Owns one-shot semaphore generations from reservation through acknowledged release. */
class DXVKPresentWaitTracker
{
private:
	friend class DXVKInterop;

	struct Submission
	{
		uint32_t slot = UINT32_MAX;
		uint64_t generation = 0;
	};
	struct InputCompletion
	{
		VkSemaphore semaphore = VK_NULL_HANDLE;
		uint64_t value = 0;
	};

	mutable std::recursive_mutex mutex;
	std::vector<VkSemaphore> semaphores;
	std::vector<bool> inUse;
	std::vector<InputCompletion> inputCompletions;
	uint32_t pendingSlot = UINT32_MAX;
	uint64_t pendingGeneration = 0;
	std::vector<Submission> outstanding;
	PFN_csDxvkEnqueueInteropCommandBuffer enqueueCommandBuffer = nullptr;
	PFN_csDxvkGetPresentWaitSemaphoreState getState = nullptr;
	PFN_csDxvkClearPresentWaitSemaphore clear = nullptr;
	PFN_csDxvkCancelPresentWaitSemaphore cancel = nullptr;
	PFN_csDxvkReleaseQueuedPresentWaitSemaphoresAfterIdle releaseAfterIdle = nullptr;
	bool terminalFault = false;
	bool synchronousPresentControlAvailable = false;
	bool presentQueueSplit = false;
};

/** Owns command buffers and fences. Exactly one transaction holds mutex while a slot is mutable. */
class DXVKCommandRing
{
private:
	friend class DXVKInterop;

	mutable std::recursive_mutex mutex;
	VkCommandPool pool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> commandBuffers;
	std::vector<VkFence> fences;
	bool faulted = false;
	mutable bool submissionsIdleProven = false;
	uint32_t framesInFlight = 0;
	uint32_t frameIndex = 0;
};

/** Owns deferred and quarantined resources; slot indices refer to DXVKCommandRing slots. */
class DXVKResourceRetirementQueue
{
private:
	friend class DXVKInterop;

	struct FSRPresentViewGroup
	{
		uint32_t slot = UINT32_MAX;
		std::vector<VkImageView> views;
	};

	// Lock order when both are required: DXVKCommandRing::mutex, then mutex.
	mutable std::recursive_mutex mutex;
	bool destructionTerminalFault = false;
	std::vector<std::vector<VkImageView>> pendingViewDeletes;
	std::vector<std::vector<winrt::com_ptr<ID3D11Resource>>> pendingResourceReleases;
	std::vector<winrt::com_ptr<ID3D11Resource>> retainedPresentResources;
	std::vector<FSRPresentViewGroup> pendingFSRPresentViewGroups;
	std::vector<FSRPresentViewGroup> quarantinedFSRPresentViewGroups;
	bool fsrSwapchainTeardownConfirmed = false;
};
