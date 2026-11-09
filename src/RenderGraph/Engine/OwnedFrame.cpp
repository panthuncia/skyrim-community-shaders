#include "RenderGraph/Engine/OwnedFrame.h"

namespace RenderGraph::Engine
{
	OwnedFrame& OwnedFrame::Get()
	{
		static OwnedFrame s_frame;
		return s_frame;
	}
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/Engine/GuestDevice.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <array>
#	include <atomic>
#	include <condition_variable>
#	include <mutex>
#	include <utility>
#	include <vector>

namespace RenderGraph::Engine
{
	namespace
	{
		// Command buffers for the copy, reused once the host timeline passed their last use.
		constexpr uint32_t kCommandSlots = 4;
		// Frames queued behind DXVK's stream at once; the render thread waits for the oldest beyond that.
		constexpr uint32_t kFrameSlots = 8;

		rhi::Format SwapchainFormat(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				return rhi::Format::R10G10B10A2_UNorm;
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return rhi::Format::B8G8R8A8_UNorm;
			default:
				return rhi::Format::R8G8B8A8_UNorm;
			}
		}

		VkFormat VulkanFormat(rhi::Format a_format)
		{
			switch (a_format) {
			case rhi::Format::R10G10B10A2_UNorm:
				return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
			case rhi::Format::B8G8R8A8_UNorm:
				return VK_FORMAT_B8G8R8A8_UNORM;
			default:
				return VK_FORMAT_R8G8B8A8_UNORM;
			}
		}

		void NoCommands(void*, VkCommandBuffer) {}
	}

	struct OwnedFrame::State
	{
		// Device-level entry points from the loader (GuestDevice::DeviceProc), not volk's globals.
		struct Functions
		{
			PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = nullptr;
			PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
			PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
			PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
			PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
			PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
			PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
			PFN_vkCmdPipelineBarrier2 vkCmdPipelineBarrier2 = nullptr;
			PFN_vkCmdBlitImage vkCmdBlitImage = nullptr;
			PFN_vkQueueSubmit2 vkQueueSubmit2 = nullptr;
			PFN_vkWaitSemaphores vkWaitSemaphores = nullptr;
		} fn;

		std::shared_ptr<rhi::DevicePtr> device;
		rhi::SwapchainPtr swapchain;
		rhi::Queue graphics;
		VkDevice vkDevice = VK_NULL_HANDLE;
		VkQueue vkGraphics = VK_NULL_HANDLE;
		uint32_t graphicsFamily = 0;
		HWND window = nullptr;
		rhi::Format format = rhi::Format::R8G8B8A8_UNorm;
		uint32_t bufferCount = 2;
		uint32_t width = 0, height = 0;
		std::vector<VkImage> images;  // in the swap chain's image order, which CurrentImageIndex indexes
		rhi::SwapchainColorSpace colorSpace = rhi::SwapchainColorSpace::SrgbNonlinear;
		bool hdrRequested = false;

		// The guest's frame points: DXVK's stream signals one at each Present.
		VkSemaphore guestFrame = VK_NULL_HANDLE;
		uint64_t guestValue = 0;
		// The copy's points, which the swap chain's present waits for. Submission thread only.
		rhi::TimelinePtr hostTimeline;
		VkSemaphore hostSemaphore = VK_NULL_HANDLE;
		uint64_t hostValue = 0;
		VkCommandPool pool = VK_NULL_HANDLE;
		std::array<VkCommandBuffer, kCommandSlots> commands{};
		std::array<uint64_t, kCommandSlots> commandValues{};

		// One frame handed to DXVK's submission thread.
		struct Frame
		{
			State* state = nullptr;
			uint64_t sequence = 0;
			uint64_t guestValue = 0;
			bool vsync = false;
			VkImage source = VK_NULL_HANDLE;
			VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkExtent2D sourceExtent{};
		};
		std::array<Frame, kFrameSlots> frames{};
		uint64_t queued = 0;
		std::atomic<uint64_t> completed = 0;
		std::mutex completionMutex;
		std::condition_variable completionChanged;
		std::atomic<bool> needsResize = false;
		std::atomic<bool> lost = false;
		std::atomic<int> requestedColorSpace = -1;  // -1 none, 0 SDR, 1 HDR10

		// The guest's back buffer, described once per texture (a resize makes a new one).
		ID3D11Device* d3dDevice = nullptr;
		winrt::com_ptr<ID3D11Texture2D> backBuffer;
		DxvkOrgInteropImageInfo backBufferImage{};
		PFN_dxvkEmitExternalCommands emitExternalCommands = nullptr;
		PFN_dxvkEnqueueQueueCallback enqueueQueueCallback = nullptr;
		PFN_dxvkGetInteropResourceInfo getResourceInfo = nullptr;
		// HDRDisplay's output, shown instead of the back buffer when set for a frame (SetFrameSource). Described once per texture;
		// one DXVK cannot describe is remembered so that it is not asked again.
		winrt::com_ptr<ID3D11Texture2D> frameSource;
		winrt::com_ptr<ID3D11Texture2D> sourceTexture;
		DxvkOrgInteropImageInfo sourceImage{};
		winrt::com_ptr<ID3D11Texture2D> rejectedSource;

		std::mutex presentMutex;
		mutable std::mutex surfaceMutex;
		SurfaceState surface;

		void Publish()
		{
			std::lock_guard lock(surfaceMutex);
			++surface.serial;
			surface.format = colorSpace == rhi::SwapchainColorSpace::Hdr10St2084 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VulkanFormat(format);
			surface.requestedColorSpace = hdrRequested ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
			surface.effectiveColorSpace = colorSpace == rhi::SwapchainColorSpace::Hdr10St2084 ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		}

		bool ReadImages()
		{
			const VkSwapchainKHR vkSwapchain = rhi::vulkan::get_swapchain(swapchain.Get());
			uint32_t count = 0;
			if (fn.vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &count, nullptr) != VK_SUCCESS)
				return false;
			images.resize(count);
			return fn.vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &count, images.data()) == VK_SUCCESS;
		}

		// Every frame handed to DXVK presented: nothing on the submission thread touches the swap chain while it changes.
		void Drain()
		{
			std::unique_lock lock(completionMutex);
			completionChanged.wait(lock, [&] { return completed.load(std::memory_order_acquire) == queued; });
		}

		// Drained, and every copy done on the GPU: a source image nothing tracks for DXVK can be released.
		void WaitCopies()
		{
			Drain();
			if (!hostValue)
				return;
			VkSemaphoreWaitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
			wait.semaphoreCount = 1;
			wait.pSemaphores = &hostSemaphore;
			wait.pValues = &hostValue;
			if (fn.vkWaitSemaphores(vkDevice, &wait, UINT64_MAX) != VK_SUCCESS)
				lost = true;
		}

		// The image DXVK keeps a texture in. False when DXVK cannot describe it.
		bool Describe(ID3D11Texture2D* a_texture, DxvkOrgInteropImageInfo& a_image, const char* a_what)
		{
			DxvkOrgInteropResourceInfo info{};
			info.version = DXVK_ORG_INTEROP_VERSION;
			if (FAILED(getResourceInfo(d3dDevice, a_texture, &info)) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE) {
				logger::error("[ORG] Owned frame: DXVK could not describe {}", a_what);
				return false;
			}
			a_image = info.image;
			logger::info("[ORG] Owned frame: {} {}x{}, format {}, resting layout {}", a_what, info.image.extent.width, info.image.extent.height,
				static_cast<int>(info.image.format), static_cast<int>(info.image.layout));
			return true;
		}

		void Resize(uint32_t a_width, uint32_t a_height)
		{
			Drain();
			if (swapchain->ResizeBuffers(bufferCount, a_width, a_height, format, 0) != rhi::Result::Ok || !ReadImages()) {
				logger::error("[ORG] Owned frame: the swap chain could not be resized to {}x{}", a_width, a_height);
				return;
			}
			width = a_width;
			height = a_height;
			logger::info("[ORG] Owned frame: swap chain resized to {}x{} ({} images)", width, height, images.size());
			Publish();
		}

		void ApplyColorSpace(bool a_hdr)
		{
			hdrRequested = a_hdr;
			const auto wanted = a_hdr ? rhi::SwapchainColorSpace::Hdr10St2084 : rhi::SwapchainColorSpace::SrgbNonlinear;
			if (wanted != colorSpace) {
				const bool supported = swapchain->IsColorSpaceSupported(wanted);
				logger::info("[ORG] Owned frame: colour space {} requested; the output {} it", a_hdr ? "HDR10" : "sRGB", supported ? "supports" : "does not support");
				if (supported) {
					Drain();
					if (swapchain->SetColorSpace(wanted) == rhi::Result::Ok && ReadImages())
						colorSpace = wanted;
					else
						logger::error("[ORG] Owned frame: the colour space could not be set");
				}
			}
			Publish();
		}

		// DXVK's submission thread, in stream order behind the guest frame's flush, with DXVK's queue lock held.
		static void Tail(void* a_user, VkQueue)
		{
			GuestDevice::QueueHeldScope held;
			auto& frame = *static_cast<Frame*>(a_user);
			auto& self = *frame.state;
			self.PresentFrame(frame);
			self.completed.fetch_add(1, std::memory_order_acq_rel);
			{
				std::lock_guard lock(self.completionMutex);
			}
			self.completionChanged.notify_all();
		}

		void PresentFrame(const Frame& a_frame)
		{
			if (lost.load(std::memory_order_relaxed))
				return;
			const uint32_t slot = static_cast<uint32_t>(a_frame.sequence % kCommandSlots);
			if (commandValues[slot]) {
				VkSemaphoreWaitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
				wait.semaphoreCount = 1;
				wait.pSemaphores = &hostSemaphore;
				wait.pValues = &commandValues[slot];
				if (fn.vkWaitSemaphores(vkDevice, &wait, UINT64_MAX) != VK_SUCCESS) {
					lost = true;
					return;
				}
			}
			const VkCommandBuffer commandBuffer = commands[slot];
			fn.vkResetCommandBuffer(commandBuffer, 0);
			VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			fn.vkBeginCommandBuffer(commandBuffer, &begin);

			const uint32_t index = swapchain->CurrentImageIndex();
			const VkImage target = index < images.size() ? images[index] : VK_NULL_HANDLE;
			const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			if (target && a_frame.source) {
				// The guest's image leaves its resting layout only for the copy; the swap chain image is overwritten whole.
				std::array<VkImageMemoryBarrier2, 2> before{};
				before[0] = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
				before[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				before[0].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
				before[0].dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
				before[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
				before[0].oldLayout = a_frame.sourceLayout;
				before[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				before[0].srcQueueFamilyIndex = before[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				before[0].image = a_frame.source;
				before[0].subresourceRange = range;
				before[1] = before[0];
				before[1].srcAccessMask = 0;
				before[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
				before[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				before[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				before[1].image = target;
				VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				dependency.imageMemoryBarrierCount = static_cast<uint32_t>(before.size());
				dependency.pImageMemoryBarriers = before.data();
				fn.vkCmdPipelineBarrier2(commandBuffer, &dependency);

				// A blit converts the format and scales when the sizes differ for a frame (a resize in flight).
				VkImageBlit region{};
				region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				region.srcOffsets[1] = { static_cast<int32_t>(a_frame.sourceExtent.width), static_cast<int32_t>(a_frame.sourceExtent.height), 1 };
				region.dstSubresource = region.srcSubresource;
				region.dstOffsets[1] = { static_cast<int32_t>(width), static_cast<int32_t>(height), 1 };
				const bool sameSize = a_frame.sourceExtent.width == width && a_frame.sourceExtent.height == height;
				fn.vkCmdBlitImage(commandBuffer, a_frame.source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
					sameSize ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);

				std::array<VkImageMemoryBarrier2, 2> after = before;
				after[0].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
				after[0].srcAccessMask = 0;
				after[0].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				after[0].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
				after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				after[0].newLayout = a_frame.sourceLayout;
				after[1].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
				after[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
				after[1].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				after[1].dstAccessMask = 0;
				after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				after[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
				dependency.pImageMemoryBarriers = after.data();
				fn.vkCmdPipelineBarrier2(commandBuffer, &dependency);
			}
			fn.vkEndCommandBuffer(commandBuffer);

			// The copy waits for the guest frame's point, which the submission just before this callback signals.
			VkSemaphoreSubmitInfo wait{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
			wait.semaphore = guestFrame;
			wait.value = a_frame.guestValue;
			wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			VkSemaphoreSubmitInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
			signal.semaphore = hostSemaphore;
			signal.value = ++hostValue;
			signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			VkCommandBufferSubmitInfo commandInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
			commandInfo.commandBuffer = commandBuffer;
			VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
			submit.waitSemaphoreInfoCount = 1;
			submit.pWaitSemaphoreInfos = &wait;
			submit.commandBufferInfoCount = 1;
			submit.pCommandBufferInfos = &commandInfo;
			submit.signalSemaphoreInfoCount = 1;
			submit.pSignalSemaphoreInfos = &signal;
			if (const VkResult result = fn.vkQueueSubmit2(vkGraphics, 1, &submit, VK_NULL_HANDLE); result != VK_SUCCESS) {
				logger::error("[ORG] Owned frame: the copy's submission failed ({})", static_cast<int>(result));
				lost = true;
				return;
			}
			commandValues[slot] = hostValue;

			rhi::PresentSyncDesc sync{};
			sync.queue = graphics;
			sync.wait = { hostTimeline->GetHandle(), hostValue };
			const rhi::Result presented = swapchain->Present(a_frame.vsync, sync);
			if (presented == rhi::Result::ModeChanged) {
				needsResize = true;
			} else if (presented != rhi::Result::Ok) {
				static std::atomic<bool> s_reported = false;
				if (!s_reported.exchange(true))
					logger::error("[ORG] Owned frame: Present failed ({})", static_cast<int>(presented));
				if (presented == rhi::Result::DeviceLost || presented == rhi::Result::DeviceRemoved || presented == rhi::Result::DeviceHung)
					lost = true;
				else
					needsResize = true;
			}
		}
	};

	bool OwnedFrame::Active()
	{
		const auto* state = Get().state;
		return state && state->enqueueQueueCallback;
	}

	bool OwnedFrame::CreateSwapchain(const DXGI_SWAP_CHAIN_DESC& a_desc)
	{
		auto device = GuestDevice::Get().Device();
		if (!device || !*device)
			return false;
		auto* s = new State();
		s->device = device;
		s->window = a_desc.OutputWindow;
		s->format = SwapchainFormat(a_desc.BufferDesc.Format);
		s->bufferCount = (std::max)(a_desc.BufferCount, 2u);
		s->width = a_desc.BufferDesc.Width;
		s->height = a_desc.BufferDesc.Height;
		if (!s->width || !s->height) {
			RECT client{};
			::GetClientRect(s->window, &client);
			s->width = (std::max)(static_cast<uint32_t>(client.right - client.left), 1u);
			s->height = (std::max)(static_cast<uint32_t>(client.bottom - client.top), 1u);
		}

		auto& rhiDevice = *s->device;
		s->vkDevice = rhi::vulkan::get_device(rhiDevice.Get());
		bool resolved = true;
		resolved = (s->fn.vkGetSwapchainImagesKHR = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(GuestDevice::Get().DeviceProc("vkGetSwapchainImagesKHR"))) && resolved;
		resolved = (s->fn.vkCreateSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(GuestDevice::Get().DeviceProc("vkCreateSemaphore"))) && resolved;
		resolved = (s->fn.vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(GuestDevice::Get().DeviceProc("vkCreateCommandPool"))) && resolved;
		resolved = (s->fn.vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(GuestDevice::Get().DeviceProc("vkAllocateCommandBuffers"))) && resolved;
		resolved = (s->fn.vkResetCommandBuffer = reinterpret_cast<PFN_vkResetCommandBuffer>(GuestDevice::Get().DeviceProc("vkResetCommandBuffer"))) && resolved;
		resolved = (s->fn.vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(GuestDevice::Get().DeviceProc("vkBeginCommandBuffer"))) && resolved;
		resolved = (s->fn.vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(GuestDevice::Get().DeviceProc("vkEndCommandBuffer"))) && resolved;
		resolved = (s->fn.vkCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(GuestDevice::Get().DeviceProc("vkCmdPipelineBarrier2"))) && resolved;
		resolved = (s->fn.vkCmdBlitImage = reinterpret_cast<PFN_vkCmdBlitImage>(GuestDevice::Get().DeviceProc("vkCmdBlitImage"))) && resolved;
		resolved = (s->fn.vkQueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(GuestDevice::Get().DeviceProc("vkQueueSubmit2"))) && resolved;
		resolved = (s->fn.vkWaitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(GuestDevice::Get().DeviceProc("vkWaitSemaphores"))) && resolved;
		if (!resolved) {
			logger::error("[ORG] Owned frame: the Vulkan entry points could not be resolved");
			delete s;
			return false;
		}
		s->graphics = rhiDevice->GetQueue(rhi::QueueKind::Graphics);
		s->vkGraphics = rhi::vulkan::get_queue(s->graphics);
		s->graphicsFamily = rhi::vulkan::get_queue_family_index(s->graphics);
		if (rhiDevice->CreateSwapchain(s->window, s->width, s->height, s->format, s->bufferCount, true, s->swapchain) != rhi::Result::Ok || !s->swapchain ||
			!s->ReadImages()) {
			logger::error("[ORG] Owned frame: BasicRHI could not create a swap chain on the game's window");
			delete s;
			return false;
		}
		const auto caps = s->swapchain->Caps();
		logger::info("[ORG] Owned frame: swap chain {}x{}, {} images, tearing {}, present-mode switch {}, present wait {}, HDR metadata {}; output supports "
					 "HDR10 {}, scRGB {}",
			s->width, s->height, s->images.size(), caps.tearing, caps.presentModeSwitch, caps.presentWait, caps.hdrMetadata,
			s->swapchain->IsColorSpaceSupported(rhi::SwapchainColorSpace::Hdr10St2084), s->swapchain->IsColorSpaceSupported(rhi::SwapchainColorSpace::ScRgbLinear));
		// The render thread's own pacing (RenderGraphRuntime::SignalFramePace, CS_FRAME_LATENCY) bounds how far the CPU runs ahead. The
		// swap chain's limit blocks DXVK's submission thread, which would hold back the next frame's command lists from the GPU, so it
		// only backs that up: as many frames as the graph keeps in flight.
		(void)s->swapchain->SetMaximumFrameLatency(RenderGraphRuntime::kGameFramesInFlight);

		VkSemaphoreTypeCreateInfo type{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
		type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo semaphore{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		semaphore.pNext = &type;
		VkCommandPoolCreateInfo pool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool.queueFamilyIndex = s->graphicsFamily;
		VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocate.commandBufferCount = kCommandSlots;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		rhi::VulkanTimelineInfo native{};
		bool ok = s->fn.vkCreateSemaphore(s->vkDevice, &semaphore, nullptr, &s->guestFrame) == VK_SUCCESS &&
		          rhiDevice->CreateTimeline(s->hostTimeline, 0, "CS owned frame present") == rhi::Result::Ok && s->hostTimeline &&
		          rhi::QueryNativeTimeline(s->hostTimeline.Get(), rhi::RHI_IID_VK_TIMELINE, &native, sizeof(native)) &&
		          s->fn.vkCreateCommandPool(s->vkDevice, &pool, nullptr, &s->pool) == VK_SUCCESS;
		if (ok) {
			s->hostSemaphore = static_cast<VkSemaphore>(native.timeline);
			allocate.commandPool = s->pool;
			ok = s->fn.vkAllocateCommandBuffers(s->vkDevice, &allocate, s->commands.data()) == VK_SUCCESS;
		}
		if (!ok) {
			logger::error("[ORG] Owned frame: the present's timelines or command buffers could not be created");
			delete s;
			return false;
		}
		for (auto& frame : s->frames)
			frame.state = s;
		s->Publish();
		state = s;
		return true;
	}

	bool OwnedFrame::Initialize(IDXGISwapChain*, ID3D11Device* a_device)
	{
		if (!state)
			return false;
		const HMODULE d3d11 = ::GetModuleHandleW(L"dxvk_d3d11.dll");
		state->emitExternalCommands = reinterpret_cast<PFN_dxvkEmitExternalCommands>(::GetProcAddress(d3d11, "dxvkEmitExternalCommands"));
		state->getResourceInfo = reinterpret_cast<PFN_dxvkGetInteropResourceInfo>(::GetProcAddress(d3d11, "dxvkGetInteropResourceInfo"));
		const auto enqueue = reinterpret_cast<PFN_dxvkEnqueueQueueCallback>(::GetProcAddress(d3d11, "dxvkEnqueueQueueCallback"));
		if (!state->emitExternalCommands || !state->getResourceInfo || !enqueue) {
			logger::error("[ORG] Owned frame: this DXVK build lacks the interop exports the present needs; nothing will be shown");
			return false;
		}
		state->d3dDevice = a_device;
		state->enqueueQueueCallback = enqueue;
		logger::info("[ORG] Owned frame: ORG owns the swap chain and Present; DXVK's swap chain is headless");
		return true;
	}

	long OwnedFrame::Present(IDXGISwapChain* a_swapChain, unsigned a_syncInterval, unsigned a_flags,
		const std::function<long(IDXGISwapChain*, unsigned, unsigned)>& a_guestPresent)
	{
		auto& s = *state;
		std::lock_guard lock(s.presentMutex);
		if (s.lost.load(std::memory_order_relaxed))
			return DXGI_ERROR_DEVICE_REMOVED;
		if (a_flags & DXGI_PRESENT_TEST)
			return a_guestPresent(a_swapChain, a_syncInterval, a_flags);
		const bool showSource = std::exchange(s.frameSource, nullptr) != nullptr;

		// The guest's back buffer: described again when the engine's ResizeBuffers made a new one.
		winrt::com_ptr<ID3D11Texture2D> backBuffer;
		if (FAILED(a_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()))) || !backBuffer)
			return a_guestPresent(a_swapChain, a_syncInterval, a_flags);
		if (backBuffer.get() != s.backBuffer.get()) {
			DxvkOrgInteropImageInfo image{};
			if (!s.Describe(backBuffer.get(), image, "guest back buffer"))
				return a_guestPresent(a_swapChain, a_syncInterval, a_flags);
			s.backBuffer = backBuffer;
			s.backBufferImage = image;
		}

		// HDRDisplay's output this frame, if it set one (SetFrameSource described it).
		const DxvkOrgInteropImageInfo* source = showSource ? &s.sourceImage : &s.backBufferImage;

		// Swap chain changes, between frames: the guest's size, a present that asked for a rebuild, HDRDisplay's colour space.
		const auto& extent = s.backBufferImage.extent;
		if (extent.width != s.width || extent.height != s.height || s.needsResize.exchange(false))
			s.Resize(extent.width, extent.height);
		if (const int colorSpace = s.requestedColorSpace.exchange(-1); colorSpace >= 0)
			s.ApplyColorSpace(colorSpace == 1);

		// The guest frame is done where DXVK's stream stands: its submission signals the point the copy waits for.
		const VkSemaphoreSubmitInfo signal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, s.guestFrame, ++s.guestValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0 };
		DxvkOrgInteropExternalCommands commands{};
		commands.version = DXVK_ORG_EXTERNAL_COMMANDS_VERSION;
		commands.callback = &NoCommands;
		commands.signalCount = 1;
		commands.signals = &signal;
		commands.label = "CS owned frame: guest frame done";
		if (FAILED(s.emitExternalCommands(s.d3dDevice, &commands))) {
			logger::error("[ORG] Owned frame: the guest frame's point could not be emitted");
			return E_FAIL;
		}
		const long result = a_guestPresent(a_swapChain, a_syncInterval, a_flags);

		// A free frame slot: the oldest frame beyond kFrameSlots must have presented.
		{
			std::unique_lock completion(s.completionMutex);
			s.completionChanged.wait(completion, [&] { return s.queued - s.completed.load(std::memory_order_acquire) < kFrameSlots; });
		}
		auto& frame = s.frames[s.queued % kFrameSlots];
		frame.sequence = s.queued;
		frame.guestValue = s.guestValue;
		frame.vsync = a_syncInterval > 0;
		frame.source = source->image;
		frame.sourceLayout = source->layout;
		frame.sourceExtent = { source->extent.width, source->extent.height };
		if (FAILED(s.enqueueQueueCallback(s.d3dDevice, &State::Tail, &frame))) {
			logger::error("[ORG] Owned frame: the present could not be queued behind DXVK's stream");
			return E_FAIL;
		}
		++s.queued;
		return result;
	}

	bool OwnedFrame::SetFrameSource(ID3D11Texture2D* a_texture)
	{
		if (!state || !state->enqueueQueueCallback)
			return false;
		auto& s = *state;
		std::lock_guard lock(s.presentMutex);
		s.frameSource = nullptr;
		if (!a_texture || a_texture == s.rejectedSource.get())
			return false;
		if (a_texture != s.sourceTexture.get()) {
			DxvkOrgInteropImageInfo image{};
			if (!s.Describe(a_texture, image, "HDR output")) {
				s.rejectedSource.copy_from(a_texture);
				return false;
			}
			// The last texture is released once no queued copy reads it: DXVK does not track those reads.
			if (s.sourceTexture)
				s.WaitCopies();
			s.sourceTexture.copy_from(a_texture);
			s.sourceImage = image;
		}
		s.frameSource = s.sourceTexture;
		return true;
	}

	ID3D11Texture2D* OwnedFrame::GetFrameSource() const
	{
		if (!state)
			return nullptr;
		std::lock_guard lock(state->presentMutex);
		return state->frameSource.get();
	}

	void OwnedFrame::RequestColorSpace(bool a_hdr)
	{
		if (state)
			state->requestedColorSpace = a_hdr ? 1 : 0;
	}

	OwnedFrame::SurfaceState OwnedFrame::GetSurfaceState() const
	{
		if (!state)
			return {};
		std::lock_guard lock(state->surfaceMutex);
		return state->surface;
	}
}

#else  // !CS_HAS_RENDER_GRAPH

namespace RenderGraph::Engine
{
	bool OwnedFrame::Active() { return false; }
	bool OwnedFrame::CreateSwapchain(const DXGI_SWAP_CHAIN_DESC&) { return false; }
	bool OwnedFrame::Initialize(IDXGISwapChain*, ID3D11Device*) { return false; }
	long OwnedFrame::Present(IDXGISwapChain* a_swapChain, unsigned a_syncInterval, unsigned a_flags,
		const std::function<long(IDXGISwapChain*, unsigned, unsigned)>& a_guestPresent)
	{
		return a_guestPresent(a_swapChain, a_syncInterval, a_flags);
	}
	bool OwnedFrame::SetFrameSource(ID3D11Texture2D*) { return false; }
	ID3D11Texture2D* OwnedFrame::GetFrameSource() const { return nullptr; }
	void OwnedFrame::RequestColorSpace(bool) {}
	OwnedFrame::SurfaceState OwnedFrame::GetSurfaceState() const { return {}; }
}

#endif
