#pragma once

#include <cs_dxvk_api.h>
#include <d3d11.h>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>
#include <winrt/base.h>

class DXVKCommandRingState
{
protected:
	struct PresentWaitSubmission
	{
		uint32_t slot = UINT32_MAX;
		uint64_t generation = 0;
	};
	struct InputCompletion
	{
		VkSemaphore semaphore = VK_NULL_HANDLE;
		uint64_t value = 0;
	};

	mutable std::recursive_mutex commandRingMutex;
	VkCommandPool commandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> commandBuffers;
	std::vector<VkFence> commandFences;
	std::vector<VkSemaphore> presentWaitSemaphores;
	std::vector<bool> presentWaitInUse;
	std::vector<InputCompletion> inputCompletions;
	uint32_t pendingPresentWaitSlot = UINT32_MAX;
	uint64_t pendingPresentWaitGeneration = 0;
	std::vector<PresentWaitSubmission> outstandingPresentWaitSubmissions;
	PFN_csDxvkEnqueueInteropCommandBuffer enqueueInteropCommandBuffer = nullptr;
	PFN_csDxvkGetPresentWaitSemaphoreState getPresentWaitSemaphoreState = nullptr;
	PFN_csDxvkClearPresentWaitSemaphore clearPresentWaitSemaphore = nullptr;
	PFN_csDxvkCancelPresentWaitSemaphore cancelPresentWaitSemaphore = nullptr;
	PFN_csDxvkReleaseQueuedPresentWaitSemaphoresAfterIdle releaseQueuedPresentWaitSemaphoresAfterIdle = nullptr;
	bool presentWaitInteropTerminalFault = false;
	bool synchronousPresentControlAvailable = false;
	bool presentQueueSplit = false;
	bool commandRingFaulted = false;
	mutable bool commandRingSubmissionsIdleProven = false;
	mutable bool submissionQueueLockUncertain = false;
	uint32_t framesInFlight = 0;
	uint32_t commandFrameIndex = 0;
};

class DXVKResourceRetirementState
{
protected:
	struct FSRPresentViewGroup
	{
		uint32_t slot = UINT32_MAX;
		std::vector<VkImageView> views;
	};

	bool vulkanResourceDestructionTerminalFault = false;
	std::vector<std::vector<VkImageView>> pendingViewDeletes;
	std::vector<std::vector<winrt::com_ptr<ID3D11Resource>>> pendingResourceReleases;
	std::vector<winrt::com_ptr<ID3D11Resource>> retainedPresentResources;
	std::vector<FSRPresentViewGroup> pendingFSRPresentViewGroups;
	std::vector<FSRPresentViewGroup> quarantinedFSRPresentViewGroups;
	bool fsrSwapchainTeardownConfirmed = false;
};
