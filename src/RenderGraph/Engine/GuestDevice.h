#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct VkQueue_T;

namespace rhi
{
	class DevicePtr;
}

namespace RenderGraph::Engine
{
	/**
	 * @brief CS_ORG_OWNED_FRAME=1: BasicRHI creates the Vulkan instance and device, DXVK runs on them as a guest, and the ORG
	 * frame owns the swap chain and Present (OwnedFrame). Off by default: DXVK creates the device and presents, as before.
	 */
	bool OwnedFrameRequested();

	/**
	 * @brief The Vulkan device BasicRHI creates for the owned frame, with DXVK on it as a guest.
	 *
	 * Before the game creates its D3D11 device, Prepare asks DXVK what it needs (dxvkGetGuestInstanceRequirements,
	 * dxvkGetGuestDeviceRequirements), creates the device with rhi::CreateVulkanDevice with that added through BasicRHI's
	 * creation hooks, and hands it to DXVK (dxvkSetGuestDevice). The game's D3D11 device then runs on it. DXVK submits only to the
	 * graphics queue; the render graph keeps the compute and copy queues.
	 */
	class GuestDevice
	{
	public:
		static GuestDevice& Get();

		/**
		 * @brief Creates the device and hands it to DXVK, before the game's D3D11CreateDeviceAndSwapChain. Once.
		 * @param a_dxvkD3D11 dxvk_d3d11.dll.
		 * @return false when the owned frame is off or the device could not be made (logged): DXVK then creates its own.
		 */
		bool Prepare(void* a_dxvkD3D11);

		/**
		 * @brief After the game's device creation, with the D3D11 device (null when creation failed). On DXVK's device BasicRHI's queue
		 * accesses take DXVK's submission lock from then on (rhi::vulkan::ShareVulkanDevice).
		 */
		void Confirm(void* a_d3d11Device);

		/** @brief Whether the game's D3D11 device runs on BasicRHI's device. */
		bool Active() const { return active; }

		/**
		 * @brief BasicRHI's device once Prepare made it (null otherwise). While Active the render graph shares it instead of adopting
		 * DXVK's (RenderGraphRuntime::Initialize).
		 */
		std::shared_ptr<rhi::DevicePtr> Device() const;

		/** @brief A device-level Vulkan entry point of BasicRHI's device, from the loader (CS's own code does not use volk's globals). */
		void* DeviceProc(const char* a_name) const;

		/** @brief Before the game's device exists: takes the device back from DXVK and releases it, so DXVK creates its own. */
		void Abandon(std::string a_reason);

		/** @brief Why the owned frame is off (empty when Active). */
		const std::string& DisabledReason() const { return disabledReason; }

		/**
		 * @brief rhi::vulkan::QueueSubmissionHooks lock and unlock for BasicRHI's queue accesses: DXVK's submission lock, which
		 * DXVK takes around its own. Skipped on a thread inside QueueHeldScope.
		 */
		static void LockQueue(void* a_user, VkQueue_T* a_queue);
		static void UnlockQueue(void* a_user, VkQueue_T* a_queue);

		/** @brief Marks the calling thread as holding DXVK's queue lock already (a dxvkEnqueueQueueCallback), so LockQueue skips it. */
		class QueueHeldScope
		{
		public:
			QueueHeldScope();
			~QueueHeldScope();
			QueueHeldScope(const QueueHeldScope&) = delete;
			QueueHeldScope& operator=(const QueueHeldScope&) = delete;
		};

	private:
		GuestDevice() = default;

		struct State;
		State* state = nullptr;  // never freed: the device outlives static destruction, as DXVK's does
		bool attempted = false;
		bool active = false;
		std::string disabledReason = "CS_ORG_OWNED_FRAME is off";
	};
}
