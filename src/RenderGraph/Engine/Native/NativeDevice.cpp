#include "RenderGraph/Engine/Native/Native.h"
// The native state's records name members like the D3D11 descriptions they mirror (format, width, height); locals that copy
// them keep those names.
#pragma warning(disable: 4458)

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include <rhi_helpers.h>

#	include <d3d11_1.h>
#	include <functional>

#	include "Aftermath.h"
#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/Engine/Tripwire/GuestDevice.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/Switches.h"

#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <cstring>
#	include <mutex>
#	include <unordered_map>
#	include <unordered_set>
#	include <vector>

namespace RenderGraph::Engine::Native
{
	namespace
	{
		// ---- Device creation -------------------------------------------------------------------------------------------------

		// The loader BasicRHI's volk loaded. CS's own translation units resolve Vulkan entry points through it, not volk's globals.
		PFN_vkGetInstanceProcAddr LoaderProc()
		{
			static const auto s_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(::GetProcAddress(::LoadLibraryW(L"vulkan-1.dll"), "vkGetInstanceProcAddr"));
			return s_proc;
		}

		VkBaseOutStructure* FindStruct(const void* a_chain, VkStructureType a_type)
		{
			for (auto* s = static_cast<VkBaseOutStructure*>(const_cast<void*>(a_chain)); s; s = s->pNext)
				if (s->sType == a_type)
					return s;
			return nullptr;
		}

		/**
		 * @brief BasicRHI's device hook: what the game's D3D11 ran with on DXVK's device, added where the device supports it.
		 *
		 * The ports were written against DXVK's device, which enabled almost every core feature and robustness2's null descriptors,
		 * and the render graph asked DXVK for the features in RenderGraphRuntime's kRequestedFeatures. Native mode creates the device
		 * itself, so it enables the same: every supported core feature (but robustBufferAccess and sparse binding, which cost and
		 * which nothing uses), null descriptors, and the Vulkan 1.2 features the graph asked for.
		 */
		struct FeatureHook
		{
			VkPhysicalDeviceFeatures core{};
			VkPhysicalDeviceRobustness2FeaturesEXT robustness2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
			std::vector<const char*> extensions;
			std::string summary;

			static bool OnDevice(void* a_user, void* a_instance, void* a_physicalDevice, void* a_createInfo)
			{
				auto* self = static_cast<FeatureHook*>(a_user);
				auto* info = static_cast<VkDeviceCreateInfo*>(a_createInfo);
				const auto instance = static_cast<VkInstance>(a_instance);
				const auto physical = static_cast<VkPhysicalDevice>(a_physicalDevice);
				const auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(LoaderProc()(instance, "vkGetPhysicalDeviceFeatures2"));
				const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(LoaderProc()(instance, "vkEnumerateDeviceExtensionProperties"));
				if (!getFeatures2 || !enumerate)
					return true;  // BasicRHI's own set; the graph's binding check names what is missing

				uint32_t count = 0;
				enumerate(physical, nullptr, &count, nullptr);
				std::vector<VkExtensionProperties> available(count);
				enumerate(physical, nullptr, &count, available.data());
				const auto has = [&](const char* a_name) {
					return std::ranges::any_of(available, [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, a_name) == 0; });
				};

				// What the device supports.
				VkPhysicalDeviceRobustness2FeaturesEXT robustness{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
				VkPhysicalDeviceVulkan12Features vk12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
				vk12.pNext = has(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) ? &robustness : nullptr;
				VkPhysicalDeviceFeatures2 supported{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
				supported.pNext = &vk12;
				getFeatures2(physical, &supported);

				// Core features: into the chain's VkPhysicalDeviceFeatures2 when there is one, else pEnabledFeatures.
				VkPhysicalDeviceFeatures wanted = supported.features;
				wanted.robustBufferAccess = VK_FALSE;
				wanted.sparseBinding = VK_FALSE;
				wanted.sparseResidencyBuffer = wanted.sparseResidencyImage2D = wanted.sparseResidencyImage3D = VK_FALSE;
				wanted.sparseResidency2Samples = wanted.sparseResidency4Samples = wanted.sparseResidency8Samples = wanted.sparseResidency16Samples = VK_FALSE;
				wanted.sparseResidencyAliased = VK_FALSE;
				const auto orCore = [&](VkPhysicalDeviceFeatures& a_into) {
					auto* into = reinterpret_cast<VkBool32*>(&a_into);
					const auto* from = reinterpret_cast<const VkBool32*>(&wanted);
					for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); ++i)
						into[i] |= from[i];
				};
				if (auto* features2 = reinterpret_cast<VkPhysicalDeviceFeatures2*>(FindStruct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2))) {
					orCore(features2->features);
				} else {
					self->core = info->pEnabledFeatures ? *info->pEnabledFeatures : VkPhysicalDeviceFeatures{};
					orCore(self->core);
					info->pEnabledFeatures = &self->core;
				}

				// Vulkan 1.2: the features RenderGraphRuntime asks DXVK for, into BasicRHI's Vulkan 1.2 struct. None is added when
				// BasicRHI has none: the chain may hold the single-feature structs instead, and both together are invalid.
				auto* chain12 = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(FindStruct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES));
				if (chain12) {
					chain12->vulkanMemoryModel |= vk12.vulkanMemoryModel;
					chain12->vulkanMemoryModelDeviceScope |= vk12.vulkanMemoryModelDeviceScope;
					chain12->separateDepthStencilLayouts |= vk12.separateDepthStencilLayouts;
					chain12->descriptorBindingVariableDescriptorCount |= vk12.descriptorBindingVariableDescriptorCount;
				}

				// Null descriptors (DXVK enables them, and BasicRHI reads the flag from the create info).
				bool nullDescriptor = false;
				if (vk12.pNext && robustness.nullDescriptor) {
					auto* chainRobustness =
						reinterpret_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(FindStruct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT));
					if (!chainRobustness) {
						self->robustness2.pNext = const_cast<void*>(info->pNext);
						info->pNext = &self->robustness2;
						chainRobustness = &self->robustness2;
					}
					chainRobustness->nullDescriptor = VK_TRUE;
					self->extensions.assign(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
					if (std::ranges::none_of(self->extensions, [](const char* e) { return std::strcmp(e, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) == 0; }))
						self->extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
					info->enabledExtensionCount = static_cast<uint32_t>(self->extensions.size());
					info->ppEnabledExtensionNames = self->extensions.data();
					nullDescriptor = true;
				}
				self->summary = chain12 ? fmt::format("every supported core feature, null descriptors {}, memory model device scope {}, separate depth/stencil layouts {}",
											  nullDescriptor, static_cast<bool>(chain12->vulkanMemoryModelDeviceScope), static_cast<bool>(chain12->separateDepthStencilLayouts)) :
				                          fmt::format("every supported core feature, null descriptors {}; BasicRHI's chain has no Vulkan 1.2 struct, so its own 1.2 features", nullDescriptor);
				return true;
			}
		};

		/**
		 * @brief BasicRHI's device on the adapter the game chose. The one place the backend is picked: a D3D12 device goes here too
		 * (CS_ORG_BACKEND, the D3D12 lane), and everything after it uses rhi calls except the image description for the graph's
		 * imports (DescribeResource), which is Vulkan's.
		 */
		bool CreateBasicRhiDevice(IDXGIAdapter* a_adapter, std::shared_ptr<rhi::DevicePtr>& a_out, std::string& a_reason)
		{
			FeatureHook hook;
			rhi::DeviceCreateInfo::VulkanCreateHooks hooks{ &hook, nullptr, &FeatureHook::OnDevice };
			rhi::DeviceCreateInfo ci{};
			ci.backend = rhi::Backend::Vulkan;
			ci.framesInFlight = RenderGraphRuntime::kGameFramesInFlight;
			// CS_ORG_NATIVE_VALIDATION=1: Vulkan validation layers through BasicRHI (P6.4).
			ci.enableDebug = Switches::On("CS_ORG_NATIVE_VALIDATION");
			// Hang diagnostics where DXVK's hang mode would be on (DXVK_DEBUG=hang, CS_GPU_CRASH_ANALYSIS); CS_ORG_CRASH_MARKERS=0 off.
			ci.enableCrashMarkers = (Switches::Equals("DXVK_DEBUG", "hang") || Aftermath::WantsCrashAnalysis()) && !Switches::Off("CS_ORG_CRASH_MARKERS");
			// The graph binds on descriptor buffers alone, as on DXVK's device (RenderGraphRuntime's binding model check).
			ci.vulkanBindingModels = rhi::vulkan::BindingModel_DescriptorBuffer;
			ci.vulkanCreateHooks = &hooks;
			// The adapter the game's INI picked (RendererData +0x00), so the device is on the GPU the engine enumerated.
			DXGI_ADAPTER_DESC adapter{};
			if (a_adapter && SUCCEEDED(a_adapter->GetDesc(&adapter))) {
				ci.adapterLuid = (static_cast<uint64_t>(static_cast<uint32_t>(adapter.AdapterLuid.HighPart)) << 32) | adapter.AdapterLuid.LowPart;
				ci.requireAdapterLuid = true;
			}
			a_out = std::make_shared<rhi::DevicePtr>();
			// Streamline's interposer stays off: no upscaler or frame generation runs in native mode yet.
			if (rhi::CreateVulkanDevice(ci, *a_out, false) != rhi::Result::Ok || !*a_out) {
				a_reason = ci.requireAdapterLuid ? "rhi::CreateVulkanDevice failed on the game's adapter (see BasicRHI's log lines above)" :
				                                   "rhi::CreateVulkanDevice failed (see BasicRHI's log lines above)";
				a_out.reset();
				return false;
			}
			const bool debugUtils = ci.enableDebug;
			Stage("device", fmt::format("BasicRHI Vulkan device on '{}' ({}; validation {}, crash markers {})", stl::utf16_to_utf8(adapter.Description).value_or("?"),
								hook.summary.empty() ? std::string("BasicRHI's own feature set") : hook.summary, debugUtils, ci.enableCrashMarkers));
			return true;
		}

		// ---- Native resources ------------------------------------------------------------------------------------------------

		/**
		 * @brief A texture or buffer the native device made: the BasicRHI resource, and for a texture the image description the graph
		 * imports it with. Its handle keeps it (Options::owner), and a released handle's owner goes through ServiceRetired, so the
		 * memory outlives the graph's last use.
		 */
		/** @brief Where one D3D11 subresource lies in a CPU-side copy of a texture (a STAGING texture, a DYNAMIC texture's upload side). */
		struct SubresourceLayout
		{
			uint64_t offset = 0;
			uint32_t rowPitch = 0;
			uint32_t depthPitch = 0;
		};

		/** @brief A texture's shape, as the context's operations need it. Rows and row bytes count blocks for compressed formats. */
		struct Shape
		{
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
			rhi::Format format = rhi::Format::Unknown;
			uint32_t width = 1, height = 1, depth = 1, mips = 1, layers = 1;
			bool blocks = false;
			uint32_t blockBytes = 4;
			// Every aspect of the format, and the one a buffer copy takes (depth for a depth-stencil format).
			VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
			VkImageAspectFlags copyAspect = VK_IMAGE_ASPECT_COLOR_BIT;

			uint32_t Width(uint32_t a_mip) const { return (std::max)(width >> a_mip, 1u); }
			uint32_t Height(uint32_t a_mip) const { return (std::max)(height >> a_mip, 1u); }
			uint32_t Depth(uint32_t a_mip) const { return (std::max)(depth >> a_mip, 1u); }
			uint32_t Subresources() const { return mips * layers; }
			uint32_t BlockSize() const { return blocks ? 4u : 1u; }
			uint32_t Rows(uint32_t a_height) const { return (a_height + BlockSize() - 1) / BlockSize(); }
			uint32_t RowBytes(uint32_t a_width) const { return (a_width + BlockSize() - 1) / BlockSize() * blockBytes; }
		};

		struct NativeResource
		{
			enum class Kind : uint8_t
			{
				Buffer,   // a buffer
				Image,    // a texture the GPU uses (DEFAULT, IMMUTABLE, DYNAMIC)
				Staging,  // a STAGING texture: a buffer laid out by subresource
			};
			Kind kind = Kind::Buffer;
			rhi::ResourcePtr resource;
			std::string name;
			// Buffers and staging textures: the Vulkan buffer, its size, and whether the CPU can map it (upload or readback memory).
			VkBuffer vkBuffer = VK_NULL_HANDLE;
			VkDeviceAddress address = 0;  // a buffer's device address, which DCLF's imports read (0 when the buffer has none)
			VkBufferUsageFlags usage = 0;
			uint64_t size = 0;
			bool hostVisible = false;
			bool readback = false;
			D3D11_BUFFER_DESC bufferDesc{};
			// Images: how the graph imports it. Images and staging textures: the shape; staging textures: the subresources' layout.
			DxvkOrgInteropImageInfo image{};
			Shape shape;
			std::vector<SubresourceLayout> layout;
			// A DYNAMIC texture's CPU side: Map writes it, Unmap copies it into the image.
			std::shared_ptr<NativeResource> upload;
			// The value of the last immediate work that read or wrote it (Immediate), so Map waits only for that.
			std::atomic<uint64_t> lastUse{ 0 };

			NativeResource();
			~NativeResource();
			NativeResource(const NativeResource&) = delete;
			NativeResource& operator=(const NativeResource&) = delete;
		};

		/** @brief The live NativeResource records, so DescribeResource can tell one from another owner. */
		struct Registry
		{
			std::mutex lock;
			std::unordered_set<const void*> live;
		};

		Registry& GetRegistry()
		{
			static Registry registry;
			return registry;
		}

		NativeResource::NativeResource()
		{
			auto& registry = GetRegistry();
			std::lock_guard guard(registry.lock);
			registry.live.insert(this);
		}

		NativeResource::~NativeResource()
		{
			auto& registry = GetRegistry();
			std::lock_guard guard(registry.lock);
			registry.live.erase(this);
		}

		/** @brief A debug label on a BasicRHI list: its crash markers then name the work (labels are no-ops where unsupported). */
		void BeginLabel(rhi::CommandList& a_list, const std::string& a_label)
		{
			static constexpr float kColor[4] = { 0.4f, 0.7f, 1.0f, 1.0f };
			if (a_list.SupportsDebugLabels())
				a_list.vt->beginDebugLabel(&a_list, kColor, a_label.c_str());
		}

		void EndLabel(rhi::CommandList& a_list)
		{
			if (a_list.SupportsDebugLabels())
				a_list.vt->endDebugLabel(&a_list);
		}

		/** @brief One-shot command lists on the graphics queue for creation-time work (initial layout, initial data), waited for. */
		class OneShot
		{
		public:
			bool Init(rhi::Device a_device)
			{
				device = a_device;
				queue = device.GetQueue(rhi::QueueKind::Graphics);
				const bool ok = device.CreateCommandAllocator(rhi::QueueKind::Graphics, allocator) == rhi::Result::Ok && allocator &&
				                device.CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), list) == rhi::Result::Ok && list &&
				                device.CreateTimeline(timeline, 0, "CS native one-shot") == rhi::Result::Ok && timeline;
				if (ok)
					list->SetName("CS native creation");
				return ok;
			}

			/** @brief Records a_record, submits it and waits for the GPU. Any thread (one at a time). a_label names it in crash markers. */
			template <class F>
			bool Run(const std::string& a_label, F&& a_record)
			{
				std::lock_guard guard(lock);
				allocator->Recycle();
				list->Recycle(allocator.Get());
				BeginLabel(list.Get(), a_label);
				a_record(list.Get());
				EndLabel(list.Get());
				list->End();
				rhi::CommandList lists[] = { list.Get() };
				const rhi::TimelinePoint signal{ timeline->GetHandle(), ++value };
				rhi::SubmitDesc submit{};
				submit.signals = { &signal, 1 };
				if (queue.Submit({ lists, 1 }, submit) != rhi::Result::Ok)
					return false;
				return timeline->HostWait(value) == rhi::Result::Ok;
			}

		private:
			std::mutex lock;
			rhi::Device device;
			rhi::Queue queue;
			rhi::CommandAllocatorPtr allocator;
			rhi::CommandListPtr list;
			rhi::TimelinePtr timeline;
			uint64_t value = 0;
		};

		/**
		 * @brief The immediate context's work: one command list per operation (a copy, a clear, a mip chain), submitted on the graphics
		 * queue when the call is made. Calls reach it in the order D3D11 code makes them, and the graph's epochs submit on the same queue
		 * at their points, so each operation lands between the epochs before and after it. A full memory barrier before and after each
		 * one orders it with that work. Every image rests in GENERAL, so the commands use it there and leave it there.
		 *
		 * The commands are Vulkan's own (vkCmdCopyImage, vkCmdClearColorImage, ...) inside a BasicRHI command list. Nothing waits
		 * unless a caller asks (Map of a resource the GPU still uses). A list is reused once the timeline has passed it, and what an
		 * operation keeps alive (its upload buffer) is released then.
		 */
		class Immediate
		{
		public:
			struct Vulkan
			{
				PFN_vkCmdPipelineBarrier2 barrier = nullptr;
				PFN_vkCmdCopyBuffer copyBuffer = nullptr;
				PFN_vkCmdCopyImage copyImage = nullptr;
				PFN_vkCmdCopyBufferToImage copyBufferToImage = nullptr;
				PFN_vkCmdCopyImageToBuffer copyImageToBuffer = nullptr;
				PFN_vkCmdBlitImage blitImage = nullptr;
				PFN_vkCmdClearColorImage clearColorImage = nullptr;
				PFN_vkCmdClearDepthStencilImage clearDepthStencilImage = nullptr;
				PFN_vkCmdFillBuffer fillBuffer = nullptr;
			};

			bool Init(rhi::Device a_device)
			{
				device = a_device;
				queue = device.GetQueue(rhi::QueueKind::Graphics);
				const VkDevice vkDevice = rhi::vulkan::get_device(device);
				const auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(LoaderProc()(rhi::vulkan::get_instance(device), "vkGetDeviceProcAddr"));
				if (!getDeviceProcAddr)
					return false;
				const auto resolve = [&](auto& a_into, const char* a_name) {
					a_into = reinterpret_cast<std::remove_reference_t<decltype(a_into)>>(getDeviceProcAddr(vkDevice, a_name));
					return a_into != nullptr;
				};
				bool ok = resolve(vk.barrier, "vkCmdPipelineBarrier2") && resolve(vk.copyBuffer, "vkCmdCopyBuffer") && resolve(vk.copyImage, "vkCmdCopyImage") &&
				          resolve(vk.copyBufferToImage, "vkCmdCopyBufferToImage") && resolve(vk.copyImageToBuffer, "vkCmdCopyImageToBuffer") &&
				          resolve(vk.blitImage, "vkCmdBlitImage") && resolve(vk.clearColorImage, "vkCmdClearColorImage") &&
				          resolve(vk.clearDepthStencilImage, "vkCmdClearDepthStencilImage") && resolve(vk.fillBuffer, "vkCmdFillBuffer");
				for (uint32_t i = 0; i < kSlots; ++i) {
					auto& slot = slots[i];
					ok = ok && device.CreateCommandAllocator(rhi::QueueKind::Graphics, slot.allocator) == rhi::Result::Ok && slot.allocator &&
					     device.CreateCommandList(rhi::QueueKind::Graphics, slot.allocator.Get(), slot.list) == rhi::Result::Ok && slot.list;
					if (ok)
						slot.list->SetName(fmt::format("CS native immediate {}", i).c_str());
				}
				return ok && device.CreateTimeline(timeline, 0, "CS native immediate context") == rhi::Result::Ok && timeline;
			}

			/**
			 * @brief Records a_record and submits it. a_keep lives until the work is done; each of a_used gets the work's value as its
			 * last use. Returns that value, 0 when the submission failed (logged). Any thread, one at a time.
			 */
			uint64_t Run(const std::string& a_label, const std::function<void(const Vulkan&, VkCommandBuffer)>& a_record,
				std::vector<std::shared_ptr<const void>> a_keep, std::initializer_list<NativeResource*> a_used)
			{
				std::lock_guard guard(lock);
				auto& slot = slots[next++ % kSlots];
				if (slot.value && timeline->GetCompletedValue() < slot.value)
					(void)timeline->HostWait(slot.value);
				slot.keep.clear();
				slot.allocator->Recycle();
				slot.list->Recycle(slot.allocator.Get());
				const VkCommandBuffer commands = rhi::vulkan::get_cmd_list(slot.list.Get());
				// The operation's name and its resource, so a device fault's crash markers name the work that ran.
				BeginLabel(slot.list.Get(), a_label);
				FullBarrier(commands);
				a_record(vk, commands);
				FullBarrier(commands);
				EndLabel(slot.list.Get());
				slot.list->End();
				rhi::CommandList lists[] = { slot.list.Get() };
				const rhi::TimelinePoint signal{ timeline->GetHandle(), value + 1 };
				rhi::SubmitDesc submit{};
				submit.signals = { &signal, 1 };
				if (queue.Submit({ lists, 1 }, submit) != rhi::Result::Ok) {
					static std::atomic_flag logged;
					if (!logged.test_and_set())
						logger::error("[ORG native] an immediate context operation could not be submitted; such operations are lost");
					return 0;
				}
				slot.value = ++value;
				slot.keep = std::move(a_keep);
				for (auto* used : a_used)
					if (used)
						used->lastUse.store(value, std::memory_order_release);
				return value;
			}

			/** @brief The value of the last work submitted (0 before any). */
			uint64_t Submitted()
			{
				std::lock_guard guard(lock);
				return value;
			}

			/** @brief Whether the work of value a_value is done on the GPU (0 is). */
			bool Done(uint64_t a_value) { return !a_value || timeline->GetCompletedValue() >= a_value; }

			/** @brief Waits for the work of value a_value on the CPU. */
			bool Wait(uint64_t a_value) { return Done(a_value) || timeline->HostWait(a_value) == rhi::Result::Ok; }

		private:
			static constexpr uint32_t kSlots = 32;

			struct Slot
			{
				rhi::CommandAllocatorPtr allocator;
				rhi::CommandListPtr list;
				uint64_t value = 0;
				std::vector<std::shared_ptr<const void>> keep;
			};

			void FullBarrier(VkCommandBuffer a_commands) const
			{
				VkMemoryBarrier2 memory{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
				memory.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				memory.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
				memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				memory.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
				VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				dependency.memoryBarrierCount = 1;
				dependency.pMemoryBarriers = &memory;
				vk.barrier(a_commands, &dependency);
			}

			std::mutex lock;
			rhi::Device device;
			rhi::Queue queue;
			Vulkan vk;
			std::array<Slot, kSlots> slots;
			uint64_t next = 0;
			rhi::TimelinePtr timeline;
			uint64_t value = 0;
		};


		/** @brief Logs a failure kind once, with the first resource's name. */
		void LogOnce(std::atomic<bool>& a_logged, std::string_view a_name, std::string_view a_why)
		{
			if (!a_logged.exchange(true, std::memory_order_relaxed))
				logger::error("[ORG native] {}: {}; resources like it are not made (logged once)", a_name, a_why);
		}

		/** @brief A typeless format as the native image is made: the depth format a depth-stencil binding implies, else the UNORM one. */
		DXGI_FORMAT ResolveTypeless(DXGI_FORMAT a_format, UINT a_bindFlags)
		{
			const bool depth = (a_bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
			switch (a_format) {
			case DXGI_FORMAT_R24G8_TYPELESS:
				return DXGI_FORMAT_D24_UNORM_S8_UINT;
			case DXGI_FORMAT_R32G8X24_TYPELESS:
				return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			case DXGI_FORMAT_R32_TYPELESS:
				return depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32_FLOAT;
			case DXGI_FORMAT_R16_TYPELESS:
				return depth ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_R16_UNORM;
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				return DXGI_FORMAT_R16G16B16A16_FLOAT;
			case DXGI_FORMAT_R32G32B32A32_TYPELESS:
				return DXGI_FORMAT_R32G32B32A32_FLOAT;
			case DXGI_FORMAT_R16G16_TYPELESS:
				return DXGI_FORMAT_R16G16_FLOAT;
			case DXGI_FORMAT_R32G32_TYPELESS:
				return DXGI_FORMAT_R32G32_FLOAT;
			case DXGI_FORMAT_R8_TYPELESS:
				return DXGI_FORMAT_R8_UNORM;
			case DXGI_FORMAT_R8G8_TYPELESS:
				return DXGI_FORMAT_R8G8_UNORM;
			case DXGI_FORMAT_BC1_TYPELESS:
				return DXGI_FORMAT_BC1_UNORM;
			case DXGI_FORMAT_BC2_TYPELESS:
				return DXGI_FORMAT_BC2_UNORM;
			case DXGI_FORMAT_BC3_TYPELESS:
				return DXGI_FORMAT_BC3_UNORM;
			case DXGI_FORMAT_BC7_TYPELESS:
				return DXGI_FORMAT_BC7_UNORM;
			default:
				return a_format;
			}
		}

		uint32_t FullMipCount(uint32_t a_width, uint32_t a_height, uint32_t a_depth)
		{
			uint32_t size = (std::max)({ a_width, a_height, a_depth, 1u });
			uint32_t mips = 1;
			while (size > 1) {
				size >>= 1;
				++mips;
			}
			return mips;
		}

		/** @brief A D3D11 texture description reduced to what the native image needs. */
		struct TextureRequest
		{
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
			uint32_t width = 1, height = 1, depthOrArraySize = 1, mipLevels = 1, sampleCount = 1;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
			UINT bindFlags = 0, cpuAccess = 0, miscFlags = 0;
			const D3D11_SUBRESOURCE_DATA* data = nullptr;
			std::string name;
		};

		struct NativeState;
		NativeState* g_device = nullptr;

		/** @brief Native mode's device: BasicRHI's device, the swap chain, the back buffer and the present's objects. */
		struct NativeState
		{
			static constexpr uint32_t kPresentSlots = 3;

			std::shared_ptr<rhi::DevicePtr> device;
			rhi::Queue graphics;
			OneShot oneShot;
			Immediate immediate;

			// The swap chain on the game's window, and the back buffer the engine draws into (through the graph's passes).
			rhi::SwapchainPtr swapchain;
			HWND window = nullptr;
			DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
			rhi::Format swapchainFormat = rhi::Format::R8G8B8A8_UNorm;
			uint32_t bufferCount = 2;
			uint32_t width = 0, height = 0;
			uint32_t frameLatency = 2;
			winrt::com_ptr<ID3D11Texture2D> backBuffer;
			rhi::ResourceHandle backBufferImage{};

			// The present's copies, one command list per slot, reused once the timeline passed its last use.
			std::array<rhi::CommandAllocatorPtr, kPresentSlots> allocators;
			std::array<rhi::CommandListPtr, kPresentSlots> lists;
			std::array<uint64_t, kPresentSlots> slotValues{};
			rhi::TimelinePtr presentTimeline;
			uint64_t presentValue = 0;
			uint64_t presents = 0;
			bool resizeRequested = false;
			std::atomic<bool> lost = false;
			std::mutex presentLock;

			winrt::com_ptr<IDXGIAdapter> adapter;
			ID3D11Device* deviceHandle = nullptr;  // the engine holds it for the process
			winrt::com_ptr<IDXGIDevice1> dxgiDevice;

			std::atomic<bool> loggedCpuAccess = false, loggedFormat = false, loggedCreate = false, loggedUpload = false, loggedMultisample = false;

			rhi::Device Rhi() { return device->Get(); }

			/** @brief The texture: a BasicRHI image in GENERAL (the graph's Common home state), its initial data uploaded. */
			HRESULT MakeTexture(const TextureRequest& a_request, std::shared_ptr<NativeResource>& a_out)
			{
				if (a_request.usage == D3D11_USAGE_STAGING)
					return MakeStaging(a_request, false, a_out);
				if (a_request.sampleCount > 1) {
					LogOnce(loggedMultisample, a_request.name, "native multisampled textures are not built");
					return E_NOTIMPL;
				}
				const DXGI_FORMAT dxgiFormat = ResolveTypeless(a_request.format, a_request.bindFlags);
				const rhi::Format format = rhi::helpers::ToRHI(dxgiFormat);
				rhi::FormatSupport support{};
				uint32_t needed = 0;
				if (!(a_request.bindFlags & D3D11_BIND_DEPTH_STENCIL) || (a_request.bindFlags & D3D11_BIND_SHADER_RESOURCE))
					needed |= rhi::FU_ShaderResource;
				if (a_request.bindFlags & D3D11_BIND_UNORDERED_ACCESS)
					needed |= rhi::FU_UnorderedAccess;
				if (format == rhi::Format::Unknown || Rhi().QueryFormatSupport(format, support) != rhi::Result::Ok || (support.usage & needed) != needed) {
					LogOnce(loggedFormat, a_request.name, fmt::format("the device cannot use DXGI format {} as asked (bind flags {:#x})", static_cast<int>(a_request.format),
															  a_request.bindFlags));
					return E_INVALIDARG;
				}

				const bool cube = (a_request.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
				const uint32_t depth = a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? a_request.depthOrArraySize : 1;
				const uint32_t layers = a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1 : a_request.depthOrArraySize;
				const uint32_t mips = a_request.mipLevels ? a_request.mipLevels : FullMipCount(a_request.width, a_request.height, depth);

				rhi::ResourceDesc desc{};
				desc.type = a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? rhi::ResourceType::Texture3D :
				            a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? rhi::ResourceType::Texture1D :
				                                                                       rhi::ResourceType::Texture2D;
				desc.heapType = rhi::HeapType::DeviceLocal;
				uint32_t flags = rhi::RF_None;
				if (a_request.bindFlags & D3D11_BIND_RENDER_TARGET)
					flags |= rhi::RF_AllowRenderTarget;
				if (a_request.bindFlags & D3D11_BIND_DEPTH_STENCIL)
					flags |= rhi::RF_AllowDepthStencil;
				if (a_request.bindFlags & D3D11_BIND_UNORDERED_ACCESS)
					flags |= rhi::RF_AllowUnorderedAccess;
				if (cube)
					flags |= rhi::RF_TextureCubeCompatible;
				desc.resourceFlags = static_cast<rhi::ResourceFlags>(flags);
				desc.debugName = a_request.name.c_str();
				desc.texture.format = format;
				desc.texture.width = a_request.width;
				desc.texture.height = a_request.height;
				desc.texture.depthOrLayers = static_cast<uint16_t>(depth > 1 ? depth : layers);
				desc.texture.mipLevels = static_cast<uint16_t>(mips);
				desc.texture.sampleCount = 1;

				auto record = std::make_shared<NativeResource>();
				record->name = a_request.name;
				if (Rhi().CreateCommittedResource(desc, record->resource) != rhi::Result::Ok || !record->resource) {
					LogOnce(loggedCreate, a_request.name, "BasicRHI could not create the image");
					return E_OUTOFMEMORY;
				}

				if (!FillImage(*record, record->resource.Get(), a_request, dxgiFormat, format, mips, layers, depth)) {
					LogOnce(loggedCreate, a_request.name, "BasicRHI did not describe the image it made");
					return E_FAIL;
				}

				if (!Initialize(record->resource->GetHandle(), a_request, format, mips, layers, depth)) {
					LogOnce(loggedUpload, a_request.name, "its initial layout or contents could not be written");
					return E_FAIL;
				}
				// A DYNAMIC texture's CPU side, which Map hands out and Unmap copies in.
				if (a_request.usage == D3D11_USAGE_DYNAMIC) {
					TextureRequest cpu = a_request;
					cpu.data = nullptr;
					cpu.name = a_request.name + " (upload)";
					if (const HRESULT hr = MakeStaging(cpu, true, record->upload); FAILED(hr))
						return hr;
				}
				a_out = std::move(record);
				return S_OK;
			}

			/** @brief An image record's description as the graph imports it (DescribeResource): GENERAL, the graph's Common home state. */
			static bool FillImage(NativeResource& a_record, const rhi::Resource& a_resource, const TextureRequest& a_request, DXGI_FORMAT a_dxgiFormat, rhi::Format a_format,
				uint32_t mips, uint32_t layers, uint32_t depth)
			{
				rhi::VulkanResourceInfo info{};
				if (!rhi::vulkan::get_resource_info(a_resource, info) || !info.resource)
					return false;
				const bool cube = (a_request.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
				auto& image = a_record.image;
				image.image = rhi::vulkan::from_native_void<VkImage>(info.resource);
				image.type = a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? VK_IMAGE_TYPE_3D :
				             a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? VK_IMAGE_TYPE_1D :
				                                                                        VK_IMAGE_TYPE_2D;
				image.format = static_cast<VkFormat>(info.nativeFormat);
				image.flags = static_cast<VkImageCreateFlags>(info.flags);
				image.extent = { a_request.width, a_request.height, depth };
				image.mipLevels = mips;
				image.arrayLayers = layers;
				image.samples = VK_SAMPLE_COUNT_1_BIT;
				image.usage = static_cast<VkImageUsageFlags>(info.usage);
				image.layout = VK_IMAGE_LAYOUT_GENERAL;
				image.viewType = image.type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D :
				                 cube                           ? (layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) :
				                 image.type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) :
				                                                  (layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
				image.viewFormat = image.format;
				image.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
				const bool depthFormat = (a_request.bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
				image.subresourceRange = { static_cast<VkImageAspectFlags>(depthFormat ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, mips, 0, layers };
				a_record.kind = NativeResource::Kind::Image;
				a_record.shape = ShapeOf(a_request, a_dxgiFormat, a_format, mips, layers, depth);
				return true;
			}

			/** @brief The shape of a texture request, in its resolved format. */
			static Shape ShapeOf(const TextureRequest& a_request, DXGI_FORMAT a_dxgiFormat, rhi::Format a_format, uint32_t a_mips, uint32_t a_layers, uint32_t a_depth)
			{
				Shape shape;
				shape.dimension = a_request.dimension;
				shape.format = a_format;
				shape.width = a_request.width;
				shape.height = a_request.height;
				shape.depth = a_depth;
				shape.mips = a_mips;
				shape.layers = a_layers;
				shape.blocks = rhi::helpers::IsBlockCompressed(a_format);
				shape.blockBytes = (std::max)(rhi::helpers::BytesPerBlock(a_format), 1u);
				switch (a_dxgiFormat) {
				case DXGI_FORMAT_D24_UNORM_S8_UINT:
				case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
					shape.aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
					shape.copyAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
					break;
				case DXGI_FORMAT_D32_FLOAT:
				case DXGI_FORMAT_D16_UNORM:
					shape.aspects = shape.copyAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
					break;
				default:
					break;
				}
				return shape;
			}

			/**
			 * @brief A STAGING texture (or a DYNAMIC texture's CPU side, a_upload): a buffer in readback or upload memory, each
			 * subresource at a 512-byte offset with 256-byte rows, the rules every backend's buffer-texture copies accept.
			 */
			HRESULT MakeStaging(const TextureRequest& a_request, bool a_upload, std::shared_ptr<NativeResource>& a_out)
			{
				const DXGI_FORMAT dxgiFormat = ResolveTypeless(a_request.format, a_request.bindFlags);
				const rhi::Format format = rhi::helpers::ToRHI(dxgiFormat);
				if (format == rhi::Format::Unknown) {
					LogOnce(loggedFormat, a_request.name, fmt::format("native staging textures of DXGI format {} are not built", static_cast<int>(a_request.format)));
					return E_INVALIDARG;
				}
				const uint32_t depth = a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? a_request.depthOrArraySize : 1;
				const uint32_t layers = a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1 : a_request.depthOrArraySize;
				const uint32_t mips = a_request.mipLevels ? a_request.mipLevels : FullMipCount(a_request.width, a_request.height, depth);
				auto record = std::make_shared<NativeResource>();
				record->kind = NativeResource::Kind::Staging;
				record->name = a_request.name;
				record->shape = ShapeOf(a_request, dxgiFormat, format, mips, layers, depth);
				const auto& shape = record->shape;
				record->layout.resize(shape.Subresources());
				uint64_t offset = 0;
				for (uint32_t slice = 0; slice < layers; ++slice) {
					for (uint32_t mip = 0; mip < mips; ++mip) {
						auto& entry = record->layout[mip + slice * mips];
						entry.offset = offset;
						entry.rowPitch = (shape.RowBytes(shape.Width(mip)) + 255u) & ~255u;
						entry.depthPitch = entry.rowPitch * shape.Rows(shape.Height(mip));
						offset = (offset + uint64_t(entry.depthPitch) * shape.Depth(mip) + 511) & ~uint64_t(511);
					}
				}
				record->readback = !a_upload && (a_request.cpuAccess & D3D11_CPU_ACCESS_READ);
				if (!MakeBufferResource(*record, (std::max)(offset, uint64_t(1)), record->readback ? rhi::HeapType::Readback : rhi::HeapType::Upload, false))
					return E_OUTOFMEMORY;
				if (a_request.data) {
					void* mapped = nullptr;
					record->resource->Map(&mapped);
					if (!mapped)
						return E_FAIL;
					for (uint32_t sub = 0; sub < shape.Subresources(); ++sub)
						WriteSubresource(static_cast<std::byte*>(mapped), record->layout[sub], shape, sub % mips, a_request.data[sub]);
					record->resource->Unmap(0, record->size);
				}
				a_out = std::move(record);
				return S_OK;
			}

			/** @brief Copies one subresource's D3D11 data (its own pitches) into a layout entry. */
			static void WriteSubresource(std::byte* a_base, const SubresourceLayout& a_entry, const Shape& a_shape, uint32_t a_mip, const D3D11_SUBRESOURCE_DATA& a_data)
			{
				if (!a_data.pSysMem)
					return;
				const uint32_t rows = a_shape.Rows(a_shape.Height(a_mip));
				const uint32_t rowBytes = (std::min)(a_shape.RowBytes(a_shape.Width(a_mip)), a_entry.rowPitch);
				for (uint32_t z = 0; z < a_shape.Depth(a_mip); ++z)
					for (uint32_t row = 0; row < rows; ++row)
						std::memcpy(a_base + a_entry.offset + uint64_t(z) * a_entry.depthPitch + uint64_t(row) * a_entry.rowPitch,
							static_cast<const std::byte*>(a_data.pSysMem) + size_t(z) * a_data.SysMemSlicePitch + size_t(row) * a_data.SysMemPitch, rowBytes);
			}

			/** @brief The buffer behind a buffer or staging record: BasicRHI's resource and its Vulkan handle. */
			bool MakeBufferResource(NativeResource& a_record, uint64_t a_size, rhi::HeapType a_heap, bool a_unorderedAccess)
			{
				rhi::ResourceDesc desc{};
				desc.type = rhi::ResourceType::Buffer;
				desc.heapType = a_heap;
				desc.resourceFlags = a_unorderedAccess ? rhi::RF_AllowUnorderedAccess : rhi::RF_None;
				desc.debugName = a_record.name.c_str();
				desc.buffer.sizeBytes = a_size;
				rhi::VulkanResourceInfo info{};
				if (Rhi().CreateCommittedResource(desc, a_record.resource) != rhi::Result::Ok || !a_record.resource ||
					!rhi::vulkan::get_resource_info(a_record.resource.Get(), info) || !info.resource) {
					LogOnce(loggedCreate, a_record.name, "BasicRHI could not create the buffer");
					return false;
				}
				a_record.vkBuffer = rhi::vulkan::from_native_void<VkBuffer>(info.resource);
				a_record.address = static_cast<VkDeviceAddress>(info.deviceAddress);
				a_record.usage = static_cast<VkBufferUsageFlags>(info.usage);
				a_record.size = a_size;
				a_record.hostVisible = a_heap != rhi::HeapType::DeviceLocal;
				return true;
			}

			/** @brief A short-lived upload buffer for one context operation, kept until that operation's work is done. */
			std::shared_ptr<NativeResource> MakeUpload(uint64_t a_size)
			{
				auto record = std::make_shared<NativeResource>();
				record->name = "Native::Upload";
				if (!MakeBufferResource(*record, (std::max)(a_size, uint64_t(1)), rhi::HeapType::Upload, false))
					return nullptr;
				return record;
			}

			/**
			 * @brief Moves a new image into GENERAL, writing its initial data on the way: an upload buffer, one copy per subresource
			 * (D3D11's order: mips of slice 0, then slice 1, ...). Waits for the GPU, so the image is ready when the handle exists.
			 */
			bool Initialize(rhi::ResourceHandle a_texture, const TextureRequest& a_request, rhi::Format a_format, uint32_t a_mips, uint32_t a_layers, uint32_t a_depth)
			{
				const rhi::ResourceHandle texture = a_texture;
				const rhi::TextureSubresourceRange all{ 0, a_mips, 0, a_layers };
				rhi::ResourcePtr upload;
				struct Copy
				{
					uint32_t mip, slice;
					rhi::CopyableFootprint footprint;
				};
				std::vector<Copy> copies;
				if (a_request.data) {
					uint64_t offset = 0;
					for (uint32_t slice = 0; slice < a_layers; ++slice) {
						for (uint32_t mip = 0; mip < a_mips; ++mip) {
							rhi::FootprintRangeDesc range{};
							range.texture = texture;
							range.firstMip = mip;
							range.firstArraySlice = slice;
							range.baseOffset = offset;
							rhi::CopyableFootprint footprint{};
							const auto placed = Rhi().GetCopyableFootprints(range, &footprint, 1);
							if (placed.count != 1)
								return false;
							copies.push_back({ mip, slice, footprint });
							// The next subresource starts at D3D12's placement alignment, which every backend accepts.
							offset = (placed.totalBytes + range.baseOffset + 511) & ~uint64_t(511);
						}
					}
					rhi::ResourceDesc desc{};
					desc.type = rhi::ResourceType::Buffer;
					desc.heapType = rhi::HeapType::Upload;
					desc.resourceFlags = rhi::RF_None;
					desc.debugName = "CS native texture upload";
					desc.buffer.sizeBytes = (std::max)(offset, uint64_t(1));
					if (Rhi().CreateCommittedResource(desc, upload) != rhi::Result::Ok || !upload)
						return false;
					void* mapped = nullptr;
					upload->Map(&mapped);
					if (!mapped)
						return false;
					const bool blocks = rhi::helpers::IsBlockCompressed(a_format);
					const uint32_t blockBytes = rhi::helpers::BytesPerBlock(a_format);
					for (const auto& copy : copies) {
						const auto& source = a_request.data[copy.mip + copy.slice * a_mips];
						if (!source.pSysMem)
							return false;
						const uint32_t width = (std::max)(a_request.width >> copy.mip, 1u);
						const uint32_t height = (std::max)(a_request.height >> copy.mip, 1u);
						const uint32_t depth = (std::max)(a_depth >> copy.mip, 1u);
						const uint32_t rows = blocks ? (height + 3) / 4 : height;
						const uint32_t rowBytes = blocks ? ((width + 3) / 4) * blockBytes : width * blockBytes;
						for (uint32_t z = 0; z < depth; ++z) {
							for (uint32_t row = 0; row < rows; ++row) {
								const auto* from = static_cast<const std::byte*>(source.pSysMem) + static_cast<size_t>(z) * source.SysMemSlicePitch +
								                   static_cast<size_t>(row) * source.SysMemPitch;
								auto* to = static_cast<std::byte*>(mapped) + copy.footprint.offset + (static_cast<uint64_t>(z) * rows + row) * copy.footprint.rowPitch;
								std::memcpy(to, from, (std::min)(rowBytes, copy.footprint.rowPitch));
							}
						}
					}
					upload->Unmap(0, desc.buffer.sizeBytes);
				}

				return oneShot.Run((copies.empty() ? "initial layout " : "initial contents ") + a_request.name, [&](rhi::CommandList a_list) {
					rhi::TextureBarrier toCopy{};
					toCopy.texture = texture;
					toCopy.range = all;
					toCopy.beforeLayout = rhi::ResourceLayout::Undefined;
					toCopy.discard = true;
					if (!copies.empty()) {
						toCopy.afterSync = rhi::ResourceSyncState::Copy;
						toCopy.afterAccess = rhi::ResourceAccessType::CopyDest;
						toCopy.afterLayout = rhi::ResourceLayout::CopyDest;
						a_list.Barriers({ { &toCopy, 1 }, {}, {} });
						for (const auto& copy : copies) {
							rhi::BufferTextureCopyFootprint region{};
							region.texture = texture;
							region.buffer = upload->GetHandle();
							region.mip = copy.mip;
							region.arraySlice = copy.slice;
							region.footprint = copy.footprint;
							a_list.CopyBufferToTexture(region);
						}
						toCopy.beforeSync = rhi::ResourceSyncState::Copy;
						toCopy.beforeAccess = rhi::ResourceAccessType::CopyDest;
						toCopy.beforeLayout = rhi::ResourceLayout::CopyDest;
						toCopy.discard = false;
					}
					toCopy.afterSync = rhi::ResourceSyncState::All;
					toCopy.afterAccess = rhi::ResourceAccessType::Common;
					toCopy.afterLayout = rhi::ResourceLayout::Common;
					a_list.Barriers({ { &toCopy, 1 }, {}, {} });
				});
			}

			/** @brief A buffer: device-local without CPU access or contents, else in upload memory with its contents written. */
			HRESULT MakeBuffer(const D3D11_BUFFER_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data, std::shared_ptr<NativeResource>& a_out)
			{
				const bool hostVisible = a_desc.Usage != D3D11_USAGE_DEFAULT || a_desc.CPUAccessFlags || (a_data && a_data->pSysMem);
				auto record = std::make_shared<NativeResource>();
				record->name = "Native::Buffer";
				record->bufferDesc = a_desc;
				record->readback = (a_desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ) != 0;
				const auto heap = record->readback ? rhi::HeapType::Readback : hostVisible ? rhi::HeapType::Upload : rhi::HeapType::DeviceLocal;
				if (!MakeBufferResource(*record, (std::max)(a_desc.ByteWidth, 1u), heap, (a_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0))
					return E_OUTOFMEMORY;
				if (a_data && a_data->pSysMem && hostVisible) {
					void* mapped = nullptr;
					record->resource->Map(&mapped);
					if (!mapped)
						return E_FAIL;
					std::memcpy(mapped, a_data->pSysMem, a_desc.ByteWidth);
					record->resource->Unmap(0, a_desc.ByteWidth);
				}
				a_out = std::move(record);
				return S_OK;
			}

			// ---- The swap chain ----------------------------------------------------------------------------------------------

			/** @brief The back buffer: a native texture the engine's kFRAMEBUFFER views, which Present copies into the swap chain image. */
			bool MakeBackBuffer()
			{
				TextureRequest request;
				request.width = width;
				request.height = height;
				request.format = format;
				request.bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
				request.name = "Native::BackBuffer";
				std::shared_ptr<NativeResource> record;
				if (FAILED(MakeTexture(request, record)))
					return false;
				backBufferImage = record->resource->GetHandle();
				D3D11_TEXTURE2D_DESC desc{ width, height, 1, 1, format, { 1, 0 }, D3D11_USAGE_DEFAULT, request.bindFlags, 0, 0 };
				Tripwire::Options options;
				options.name = request.name;
				options.owner = std::move(record);
				backBuffer = Tripwire::CreateTexture2D(desc, std::move(options));
				return backBuffer != nullptr;
			}

			bool MakePresentObjects()
			{
				for (uint32_t i = 0; i < kPresentSlots; ++i) {
					if (Rhi().CreateCommandAllocator(rhi::QueueKind::Graphics, allocators[i]) != rhi::Result::Ok || !allocators[i] ||
						Rhi().CreateCommandList(rhi::QueueKind::Graphics, allocators[i].Get(), lists[i]) != rhi::Result::Ok || !lists[i])
						return false;
					lists[i]->SetName(fmt::format("CS native present {}", i).c_str());
				}
				return Rhi().CreateTimeline(presentTimeline, 0, "CS native present") == rhi::Result::Ok && presentTimeline;
			}

			/** @brief Every queued copy done on the GPU: the back buffer and the swap chain's images can change. */
			void WaitPresents()
			{
				if (presentValue)
					(void)presentTimeline->HostWait(presentValue, 2000);
			}

			HRESULT Resize(uint32_t a_count, uint32_t a_width, uint32_t a_height, DXGI_FORMAT a_format)
			{
				if (!a_width || !a_height) {
					RECT client{};
					::GetClientRect(window, &client);
					a_width = (std::max)(static_cast<uint32_t>(client.right - client.left), 1u);
					a_height = (std::max)(static_cast<uint32_t>(client.bottom - client.top), 1u);
				}
				if (a_count)
					bufferCount = (std::max)(a_count, 2u);
				if (a_format != DXGI_FORMAT_UNKNOWN)
					format = a_format;
				WaitPresents();
				if (swapchain->ResizeBuffers(bufferCount, a_width, a_height, swapchainFormat, 0) != rhi::Result::Ok) {
					logger::error("[ORG native] the swap chain could not be resized to {}x{}", a_width, a_height);
					return DXGI_ERROR_INVALID_CALL;
				}
				width = a_width;
				height = a_height;
				// The old back buffer goes with the engine's last reference to it (its views were released before ResizeBuffers).
				backBuffer = nullptr;
				if (!MakeBackBuffer()) {
					logger::error("[ORG native] the back buffer could not be made again at {}x{}", width, height);
					return E_OUTOFMEMORY;
				}
				logger::info("[ORG native] swap chain and back buffer resized to {}x{}", width, height);
				return S_OK;
			}

			HRESULT Present(UINT a_syncInterval, UINT a_flags)
			{
				std::lock_guard guard(presentLock);
				if (a_flags & DXGI_PRESENT_TEST)
					return S_OK;
				if (lost.load(std::memory_order_relaxed))
					return S_OK;  // the device-loss report and the graph's stop already named it; the game's frame still ends
				if (std::exchange(resizeRequested, false))
					(void)Resize(0, width, height, DXGI_FORMAT_UNKNOWN);
				if (!backBuffer)
					return S_OK;

				const uint32_t slot = static_cast<uint32_t>(presents % kPresentSlots);
				if (slotValues[slot] && presentTimeline->HostWait(slotValues[slot]) != rhi::Result::Ok) {
					lost = true;
					logger::error("[ORG native] a present's copy never finished on the GPU; nothing more is shown");
					return S_OK;
				}
				auto& list = lists[slot];
				allocators[slot]->Recycle();
				list->Recycle(allocators[slot].Get());

				const uint32_t index = swapchain->CurrentImageIndex();
				const rhi::ResourceHandle target = swapchain->Image(index);
				// The graph gives the back buffer back in its home state, Common (GENERAL), on the graphics queue, so this copy is
				// ordered after the frame's last write by queue submission order.
				std::array<rhi::TextureBarrier, 2> before{};
				before[0].texture = backBufferImage;
				before[0].beforeSync = rhi::ResourceSyncState::All;
				before[0].afterSync = rhi::ResourceSyncState::Copy;
				before[0].beforeAccess = rhi::ResourceAccessType::Common;
				before[0].afterAccess = rhi::ResourceAccessType::CopySource;
				before[0].beforeLayout = rhi::ResourceLayout::Common;
				before[0].afterLayout = rhi::ResourceLayout::CopySource;
				before[1].texture = target;
				before[1].beforeSync = rhi::ResourceSyncState::None;
				before[1].afterSync = rhi::ResourceSyncState::Copy;
				before[1].beforeAccess = rhi::ResourceAccessType::None;
				before[1].afterAccess = rhi::ResourceAccessType::CopyDest;
				before[1].beforeLayout = rhi::ResourceLayout::Undefined;
				before[1].afterLayout = rhi::ResourceLayout::CopyDest;
				before[1].discard = true;
				list->Barriers({ { before.data(), static_cast<uint32_t>(before.size()) }, {}, {} });
				rhi::TextureCopyRegion to{};
				to.texture = target;
				to.width = width;
				to.height = height;
				rhi::TextureCopyRegion from{};
				from.texture = backBufferImage;
				from.width = width;
				from.height = height;
				list->CopyTextureRegion(to, from);
				std::array<rhi::TextureBarrier, 2> after = before;
				after[0].beforeSync = rhi::ResourceSyncState::Copy;
				after[0].afterSync = rhi::ResourceSyncState::All;
				after[0].beforeAccess = rhi::ResourceAccessType::CopySource;
				after[0].afterAccess = rhi::ResourceAccessType::Common;
				after[0].beforeLayout = rhi::ResourceLayout::CopySource;
				after[0].afterLayout = rhi::ResourceLayout::Common;
				after[1].beforeSync = rhi::ResourceSyncState::Copy;
				after[1].afterSync = rhi::ResourceSyncState::None;
				after[1].beforeAccess = rhi::ResourceAccessType::CopyDest;
				after[1].afterAccess = rhi::ResourceAccessType::Present;
				after[1].beforeLayout = rhi::ResourceLayout::CopyDest;
				after[1].afterLayout = rhi::ResourceLayout::Present;
				after[1].discard = false;
				list->Barriers({ { after.data(), static_cast<uint32_t>(after.size()) }, {}, {} });
				list->End();

				rhi::CommandList submitted[] = { list.Get() };
				const rhi::TimelinePoint signal{ presentTimeline->GetHandle(), ++presentValue };
				rhi::SubmitDesc submit{};
				submit.signals = { &signal, 1 };
				if (graphics.Submit({ submitted, 1 }, submit) != rhi::Result::Ok) {
					--presentValue;
					lost = true;
					logger::error("[ORG native] the present's copy could not be submitted; nothing more is shown");
					return S_OK;
				}
				slotValues[slot] = presentValue;
				++presents;

				rhi::PresentSyncDesc sync{};
				sync.queue = graphics;
				sync.wait = signal;
				const rhi::Result presented = swapchain->Present(a_syncInterval > 0, sync);
				if (presents == 1)
					Stage("first present", fmt::format("{}x{}, result {}", width, height, static_cast<int>(presented)));
				if (presented == rhi::Result::ModeChanged) {
					resizeRequested = true;
				} else if (presented != rhi::Result::Ok) {
					static std::atomic_flag logged;
					if (!logged.test_and_set())
						logger::error("[ORG native] Present failed ({})", static_cast<int>(presented));
					if (presented == rhi::Result::DeviceLost || presented == rhi::Result::DeviceRemoved || presented == rhi::Result::DeviceHung)
						lost = true;
					else
						resizeRequested = true;
				}
				return S_OK;
			}

			void FillDesc(DXGI_SWAP_CHAIN_DESC& a_out) const
			{
				a_out = {};
				a_out.BufferDesc.Width = width;
				a_out.BufferDesc.Height = height;
				a_out.BufferDesc.Format = format;
				a_out.SampleDesc = { 1, 0 };
				a_out.BufferUsage = DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT;
				a_out.BufferCount = bufferCount;
				a_out.OutputWindow = window;
				a_out.Windowed = TRUE;
				a_out.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
				a_out.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
			}
		};

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

		/** @brief The back buffer's format: the swap chain's, without sRGB, so the present's copy is between equal formats. */
		DXGI_FORMAT BackBufferFormat(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			default:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			}
		}

		// ---- IDXGIDevice1 ----------------------------------------------------------------------------------------------------

		/**
		 * @brief What the device handle answers for IDXGIDevice and IDXGIDevice1. The engine sets its frame latency through it
		 * (InitD3D, SetMaximumFrameLatency(2)), which goes to the swap chain; GetAdapter returns the adapter the game chose, so
		 * Renderer::CreateSwapChain finds its factory. The rest are not used by the engine and fail.
		 */
		class DxgiDevice final : public IDXGIDevice1
		{
		public:
			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_out) override
			{
				if (!a_out)
					return E_POINTER;
				if (a_riid == __uuidof(IUnknown) || a_riid == __uuidof(IDXGIObject) || a_riid == __uuidof(IDXGIDevice) || a_riid == __uuidof(IDXGIDevice1)) {
					AddRef();
					*a_out = static_cast<IDXGIDevice1*>(this);
					return S_OK;
				}
				if (a_riid == __uuidof(ID3D11Device) && g_device && g_device->deviceHandle)
					return g_device->deviceHandle->QueryInterface(a_riid, a_out);
				*a_out = nullptr;
				return E_NOINTERFACE;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG left = --refs;
				if (!left)
					delete this;
				return left;
			}
			HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT* a_size, void*) override
			{
				if (a_size)
					*a_size = 0;
				return DXGI_ERROR_NOT_FOUND;
			}
			HRESULT STDMETHODCALLTYPE GetParent(REFIID a_riid, void** a_out) override { return GetAdapterAs(a_riid, a_out); }
			HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter** a_out) override { return GetAdapterAs(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(a_out)); }
			HRESULT STDMETHODCALLTYPE CreateSurface(const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE, const DXGI_SHARED_RESOURCE*, IDXGISurface** a_out) override
			{
				if (a_out)
					*a_out = nullptr;
				return E_NOTIMPL;
			}
			HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown* const*, DXGI_RESIDENCY* a_status, UINT a_count) override
			{
				for (UINT i = 0; a_status && i < a_count; ++i)
					a_status[i] = DXGI_RESIDENCY_FULLY_RESIDENT;
				return S_OK;
			}
			HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* a_priority) override
			{
				if (!a_priority)
					return E_POINTER;
				*a_priority = 0;
				return S_OK;
			}
			HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT a_frames) override
			{
				if (!g_device)
					return E_FAIL;
				g_device->frameLatency = a_frames ? a_frames : 3;
				(void)g_device->swapchain->SetMaximumFrameLatency(g_device->frameLatency);
				logger::info("[ORG native] the engine set the frame latency to {}", g_device->frameLatency);
				return S_OK;
			}
			HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* a_frames) override
			{
				if (!a_frames)
					return E_POINTER;
				*a_frames = g_device ? g_device->frameLatency : 3;
				return S_OK;
			}

		private:
			HRESULT GetAdapterAs(REFIID a_riid, void** a_out)
			{
				if (!a_out)
					return E_POINTER;
				*a_out = nullptr;
				if (!g_device || !g_device->adapter)
					return DXGI_ERROR_NOT_FOUND;
				return g_device->adapter->QueryInterface(a_riid, a_out);
			}

			std::atomic<ULONG> refs{ 1 };
		};

		// ---- The services the native handles call ----------------------------------------------------------------------------

		template <class Desc, class Out>
		HRESULT MakeTextureHandle(const TextureRequest& a_request, const Desc& a_desc, Out** a_out,
			winrt::com_ptr<Out> (*a_factory)(const Desc&, Tripwire::Options, Out*))
		{
			*a_out = nullptr;
			std::shared_ptr<NativeResource> record;
			if (const HRESULT hr = g_device->MakeTexture(a_request, record); FAILED(hr))
				return hr;
			Tripwire::Options options;
			options.name = a_request.name;
			options.owner = std::move(record);
			*a_out = a_factory(a_desc, std::move(options), nullptr).detach();
			return S_OK;
		}

		HRESULT CreateTexture1D(const D3D11_TEXTURE1D_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture1D** a_out)
		{
			TextureRequest request{ D3D11_RESOURCE_DIMENSION_TEXTURE1D, a_desc.Width, 1, a_desc.ArraySize, a_desc.MipLevels, 1, a_desc.Format, a_desc.Usage,
				a_desc.BindFlags, a_desc.CPUAccessFlags, a_desc.MiscFlags, a_data, "Native::Texture1D" };
			return MakeTextureHandle(request, a_desc, a_out, &Tripwire::CreateTexture1D);
		}

		HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture2D** a_out)
		{
			TextureRequest request{ D3D11_RESOURCE_DIMENSION_TEXTURE2D, a_desc.Width, a_desc.Height, a_desc.ArraySize, a_desc.MipLevels, a_desc.SampleDesc.Count,
				a_desc.Format, a_desc.Usage, a_desc.BindFlags, a_desc.CPUAccessFlags, a_desc.MiscFlags, a_data, "Native::Texture2D" };
			return MakeTextureHandle(request, a_desc, a_out, &Tripwire::CreateTexture2D);
		}

		HRESULT CreateTexture3D(const D3D11_TEXTURE3D_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture3D** a_out)
		{
			TextureRequest request{ D3D11_RESOURCE_DIMENSION_TEXTURE3D, a_desc.Width, a_desc.Height, a_desc.Depth, a_desc.MipLevels, 1, a_desc.Format, a_desc.Usage,
				a_desc.BindFlags, a_desc.CPUAccessFlags, a_desc.MiscFlags, a_data, "Native::Texture3D" };
			return MakeTextureHandle(request, a_desc, a_out, &Tripwire::CreateTexture3D);
		}

		HRESULT CreateBuffer(const D3D11_BUFFER_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Buffer** a_out)
		{
			*a_out = nullptr;
			std::shared_ptr<NativeResource> record;
			if (const HRESULT hr = g_device->MakeBuffer(a_desc, a_data, record); FAILED(hr))
				return hr;
			Tripwire::Options options;
			options.name = "Native::Buffer";
			options.owner = std::move(record);
			*a_out = Tripwire::CreateBuffer(a_desc, std::move(options)).detach();
			return S_OK;
		}

		HRESULT QueryDxgiDevice(REFIID a_riid, void** a_out)
		{
			return g_device->dxgiDevice->QueryInterface(a_riid, a_out);
		}

		HRESULT SwapChainPresent(UINT a_syncInterval, UINT a_flags)
		{
			return g_device->Present(a_syncInterval, a_flags);
		}

		HRESULT SwapChainGetBuffer(UINT a_buffer, REFIID a_riid, void** a_out)
		{
			if (!a_out)
				return E_POINTER;
			*a_out = nullptr;
			// FLIP_DISCARD: only buffer 0 can be read or written, as with DXGI.
			if (a_buffer != 0 || !g_device->backBuffer)
				return DXGI_ERROR_INVALID_CALL;
			return g_device->backBuffer->QueryInterface(a_riid, a_out);
		}

		HRESULT SwapChainResizeBuffers(UINT a_count, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT)
		{
			std::lock_guard guard(g_device->presentLock);
			return g_device->Resize(a_count, a_width, a_height, a_format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_UNKNOWN : BackBufferFormat(a_format));
		}

		HRESULT SwapChainGetDesc(DXGI_SWAP_CHAIN_DESC* a_out)
		{
			g_device->FillDesc(*a_out);
			return S_OK;
		}

		HRESULT SwapChainGetContainingOutput(IDXGIOutput** a_out)
		{
			if (!a_out)
				return E_POINTER;
			*a_out = nullptr;
			if (!g_device->adapter)
				return DXGI_ERROR_NOT_FOUND;
			// The output of the game's adapter whose monitor holds most of the window, else its first output.
			const HMONITOR monitor = ::MonitorFromWindow(g_device->window, MONITOR_DEFAULTTONEAREST);
			winrt::com_ptr<IDXGIOutput> first;
			for (UINT i = 0;; ++i) {
				winrt::com_ptr<IDXGIOutput> output;
				if (FAILED(g_device->adapter->EnumOutputs(i, output.put())))
					break;
				DXGI_OUTPUT_DESC desc{};
				if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
					*a_out = output.detach();
					return S_OK;
				}
				if (!first)
					first = output;
			}
			if (!first)
				return DXGI_ERROR_NOT_FOUND;
			*a_out = first.detach();
			return S_OK;
		}

		// ---- The device's queries --------------------------------------------------------------------------------------------

		/** @brief D3D11_FORMAT_SUPPORT bits from what BasicRHI's device can do with a format. Zero when it can do nothing. */
		UINT FormatSupportOf(DXGI_FORMAT a_format)
		{
			const rhi::Format format = rhi::helpers::ToRHI(ResolveTypeless(a_format, 0));
			rhi::FormatSupport support{};
			if (format == rhi::Format::Unknown || g_device->Rhi().QueryFormatSupport(format, support) != rhi::Result::Ok)
				return 0;
			UINT bits = 0;
			if (support.usage & rhi::FU_ShaderResource)
				bits |= D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_TEXTURE1D | D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_TEXTURE3D |
				        D3D11_FORMAT_SUPPORT_TEXTURECUBE | D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_MIP |
				        D3D11_FORMAT_SUPPORT_CPU_LOCKABLE | D3D11_FORMAT_SUPPORT_SHADER_GATHER | D3D11_FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT;
			if (support.usage & rhi::FU_RenderTarget)
				bits |= D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE | D3D11_FORMAT_SUPPORT_MIP_AUTOGEN | D3D11_FORMAT_SUPPORT_DISPLAY;
			if (support.usage & rhi::FU_DepthStencil)
				bits |= D3D11_FORMAT_SUPPORT_DEPTH_STENCIL | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON;
			if (support.usage & rhi::FU_UnorderedAccess)
				bits |= D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
			return bits;
		}

		HRESULT CheckFormatSupport(DXGI_FORMAT a_format, UINT* a_support)
		{
			if (!a_support)
				return E_POINTER;
			*a_support = FormatSupportOf(a_format);
			return *a_support ? S_OK : E_FAIL;
		}

		/**
		 * @brief The D3D11 features native mode answers, as a feature level 11.1 device on BasicRHI's would: typed UAV loads where
		 * the format can be stored to, no command lists, the rest of the optional caps off. Another feature is refused (logged once).
		 */
		HRESULT CheckFeatureSupport(D3D11_FEATURE a_feature, void* a_data, UINT a_size)
		{
			if (!a_data)
				return E_INVALIDARG;
			const auto answer = [&](auto a_value) {
				if (a_size != sizeof(a_value))
					return E_INVALIDARG;
				std::memcpy(a_data, &a_value, sizeof(a_value));
				return S_OK;
			};
			switch (a_feature) {
			case D3D11_FEATURE_THREADING:
				return answer(D3D11_FEATURE_DATA_THREADING{ TRUE, FALSE });
			case D3D11_FEATURE_DOUBLES:
				return answer(D3D11_FEATURE_DATA_DOUBLES{ FALSE });
			case D3D11_FEATURE_FORMAT_SUPPORT:
				{
					if (a_size != sizeof(D3D11_FEATURE_DATA_FORMAT_SUPPORT))
						return E_INVALIDARG;
					auto* data = static_cast<D3D11_FEATURE_DATA_FORMAT_SUPPORT*>(a_data);
					data->OutFormatSupport = FormatSupportOf(data->InFormat);
					return S_OK;
				}
			case D3D11_FEATURE_FORMAT_SUPPORT2:
				{
					if (a_size != sizeof(D3D11_FEATURE_DATA_FORMAT_SUPPORT2))
						return E_INVALIDARG;
					auto* data = static_cast<D3D11_FEATURE_DATA_FORMAT_SUPPORT2*>(a_data);
					data->OutFormatSupport2 = (FormatSupportOf(data->InFormat) & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) ?
					                              D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE :
					                              0;
					return S_OK;
				}
			case D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS:
				return answer(D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS{ TRUE });
			case D3D11_FEATURE_D3D11_OPTIONS:
				return answer(D3D11_FEATURE_DATA_D3D11_OPTIONS{});
			case D3D11_FEATURE_ARCHITECTURE_INFO:
				return answer(D3D11_FEATURE_DATA_ARCHITECTURE_INFO{ FALSE });
			case D3D11_FEATURE_D3D9_OPTIONS:
				return answer(D3D11_FEATURE_DATA_D3D9_OPTIONS{ TRUE });
			case D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT:
				return answer(D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT{});
			default:
				{
					static std::atomic_flag logged;
					if (!logged.test_and_set())
						logger::warn("[ORG native] CheckFeatureSupport({}) has no native answer; refused", static_cast<int>(a_feature));
					return E_INVALIDARG;
				}
			}
		}

		// ---- The immediate context -------------------------------------------------------------------------------------------

		/** @brief The native record behind a resource or view handle (a view's is its resource's), or null. */
		std::shared_ptr<NativeResource> Resolve(const void* a_object)
		{
			if (auto owner = Tripwire::OwnerOf(a_object)) {
				auto& registry = GetRegistry();
				std::lock_guard guard(registry.lock);
				if (registry.live.contains(owner.get()))
					return std::const_pointer_cast<NativeResource>(std::static_pointer_cast<const NativeResource>(owner));
			}
			// A port's handle around a native one (its fields hold handles that wrap the object the native device made).
			const auto info = Tripwire::Describe(a_object);
			return info.wrapped && info.wrapped != a_object && Tripwire::IsHandle(info.wrapped) ? Resolve(info.wrapped) : nullptr;
		}

		/** @brief An operation on something the native device did not make: logged once per operation. */
		void Unknown(std::atomic_flag& a_logged, const char* a_operation, const void* a_object)
		{
			if (!a_logged.test_and_set())
				logger::warn("[ORG native] {} on \"{}\", which native mode did not make; nothing happens (logged once)", a_operation,
					a_object ? Tripwire::Describe(a_object).name : std::string("null"));
		}

		/** @brief Where subresource a_sub of a staging record lies for a Vulkan buffer-image copy, with a region inside it. */
		VkBufferImageCopy StagingRegion(const NativeResource& a_staging, uint32_t a_sub, VkOffset3D a_offset, VkExtent3D a_extent, uint32_t a_imageLayer,
			VkOffset3D a_imageOffset, VkImageAspectFlags a_aspect)
		{
			const auto& shape = a_staging.shape;
			const auto& entry = a_staging.layout[a_sub];
			const uint32_t block = shape.BlockSize();
			VkBufferImageCopy copy{};
			copy.bufferOffset = entry.offset + uint64_t(a_offset.z) * entry.depthPitch + uint64_t(a_offset.y / block) * entry.rowPitch +
			                    uint64_t(a_offset.x / block) * shape.blockBytes;
			copy.bufferRowLength = entry.rowPitch / shape.blockBytes * block;
			copy.bufferImageHeight = entry.depthPitch / entry.rowPitch * block;
			copy.imageSubresource = { a_aspect, a_sub % shape.mips, a_imageLayer, 1 };
			copy.imageOffset = a_imageOffset;
			copy.imageExtent = a_extent;
			return copy;
		}

		/** @brief The whole extent of a subresource's mip. */
		VkExtent3D MipExtent(const Shape& a_shape, uint32_t a_mip)
		{
			return { a_shape.Width(a_mip), a_shape.Height(a_mip), a_shape.Depth(a_mip) };
		}

		/**
		 * @brief One subresource region from a_source to a_destination, whatever each is (a buffer, an image, a staging texture). a_box
		 * null is the whole subresource (the whole buffer). Records into a_commands; false when the pair cannot be copied.
		 */
		bool RecordCopy(const Immediate::Vulkan& a_vk, VkCommandBuffer a_commands, const NativeResource& a_destination, uint32_t a_destinationSub, VkOffset3D a_at,
			const NativeResource& a_source, uint32_t a_sourceSub, const D3D11_BOX* a_box)
		{
			using Kind = NativeResource::Kind;
			const bool sourceBuffer = a_source.kind == Kind::Buffer;
			const bool destinationBuffer = a_destination.kind == Kind::Buffer;
			if (sourceBuffer || destinationBuffer) {
				if (!sourceBuffer || !destinationBuffer)
					return false;
				const uint64_t from = a_box ? a_box->left : 0;
				const uint64_t size = a_box ? a_box->right - a_box->left : (std::min)(a_source.size, a_destination.size);
				if (from + size > a_source.size || a_at.x + size > a_destination.size)
					return false;
				const VkBufferCopy copy{ from, static_cast<VkDeviceSize>(a_at.x), size };
				a_vk.copyBuffer(a_commands, a_source.vkBuffer, a_destination.vkBuffer, 1, &copy);
				return true;
			}
			const Shape& sourceShape = a_source.shape;
			if (a_sourceSub >= sourceShape.Subresources() || a_destinationSub >= a_destination.shape.Subresources())
				return false;
			const uint32_t sourceMip = a_sourceSub % sourceShape.mips;
			const VkOffset3D from = a_box ? VkOffset3D{ static_cast<int32_t>(a_box->left), static_cast<int32_t>(a_box->top), static_cast<int32_t>(a_box->front) } :
			                                VkOffset3D{ 0, 0, 0 };
			const VkExtent3D extent = a_box ? VkExtent3D{ a_box->right - a_box->left, a_box->bottom - a_box->top, a_box->back - a_box->front } :
			                                  MipExtent(sourceShape, sourceMip);
			const uint32_t sourceLayer = a_sourceSub / sourceShape.mips;
			const uint32_t destinationLayer = a_destinationSub / a_destination.shape.mips;
			if (a_source.kind == Kind::Image && a_destination.kind == Kind::Image) {
				VkImageCopy copy{};
				copy.srcSubresource = { sourceShape.aspects, sourceMip, sourceLayer, 1 };
				copy.srcOffset = from;
				copy.dstSubresource = { a_destination.shape.aspects, a_destinationSub % a_destination.shape.mips, destinationLayer, 1 };
				copy.dstOffset = a_at;
				copy.extent = extent;
				a_vk.copyImage(a_commands, a_source.image.image, VK_IMAGE_LAYOUT_GENERAL, a_destination.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
				return true;
			}
			if (a_source.kind == Kind::Image && a_destination.kind == Kind::Staging) {
				const auto copy = StagingRegion(a_destination, a_destinationSub, a_at, extent, sourceLayer, from, sourceShape.copyAspect);
				a_vk.copyImageToBuffer(a_commands, a_source.image.image, VK_IMAGE_LAYOUT_GENERAL, a_destination.vkBuffer, 1, &copy);
				return true;
			}
			if (a_source.kind == Kind::Staging && a_destination.kind == Kind::Image) {
				const auto copy = StagingRegion(a_source, a_sourceSub, from, extent, destinationLayer, a_at, a_destination.shape.copyAspect);
				a_vk.copyBufferToImage(a_commands, a_source.vkBuffer, a_destination.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
				return true;
			}
			// Staging to staging: row by row would be needed for a region; the whole subresource is one buffer range.
			if (a_box || a_at.x || a_at.y || a_at.z)
				return false;
			const auto& sourceEntry = a_source.layout[a_sourceSub];
			const auto& destinationEntry = a_destination.layout[a_destinationSub];
			if (sourceEntry.rowPitch != destinationEntry.rowPitch || sourceEntry.depthPitch != destinationEntry.depthPitch)
				return false;
			const VkBufferCopy copy{ sourceEntry.offset, destinationEntry.offset, uint64_t(sourceEntry.depthPitch) * sourceShape.Depth(sourceMip) };
			a_vk.copyBuffer(a_commands, a_source.vkBuffer, a_destination.vkBuffer, 1, &copy);
			return true;
		}

		/** @brief The memory Map hands out for a record: its own (a buffer, a staging texture), or a DYNAMIC image's upload side. */
		NativeResource* CpuSide(NativeResource& a_record)
		{
			NativeResource* cpu = a_record.kind == NativeResource::Kind::Image ? a_record.upload.get() : &a_record;
			return cpu && cpu->hostVisible ? cpu : nullptr;
		}

		HRESULT ContextMap(ID3D11Resource* a_resource, UINT a_subresource, D3D11_MAP a_type, UINT a_flags, D3D11_MAPPED_SUBRESOURCE* a_out)
		{
			if (!a_out)
				return E_INVALIDARG;
			*a_out = {};
			const auto record = Resolve(a_resource);
			if (!record) {
				static std::atomic_flag logged;
				Unknown(logged, "Map", a_resource);
				return E_INVALIDARG;
			}
			NativeResource* cpu = CpuSide(*record);
			if (!cpu) {
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::warn("[ORG native] Map on \"{}\", which has no CPU access (DEFAULT usage); refused as D3D11 does (logged once)", record->name);
				return E_INVALIDARG;
			}
			if (cpu->kind == NativeResource::Kind::Staging && a_subresource >= cpu->layout.size())
				return E_INVALIDARG;
			// The GPU may still use the memory: a read waits for its writes, a write for its reads. NO_OVERWRITE promises not to touch
			// what is in use. A DYNAMIC buffer is not renamed: no native GPU work reads a buffer except the context's own copies.
			if (a_type != D3D11_MAP_WRITE_NO_OVERWRITE) {
				const uint64_t use = cpu->lastUse.load(std::memory_order_acquire);
				if (!g_device->immediate.Done(use)) {
					if (a_flags & D3D11_MAP_FLAG_DO_NOT_WAIT)
						return DXGI_ERROR_WAS_STILL_DRAWING;
					if (!g_device->immediate.Wait(use))
						return DXGI_ERROR_DEVICE_REMOVED;
				}
			}
			void* data = nullptr;
			cpu->resource->Map(&data);
			if (!data)
				return E_FAIL;
			if (cpu->kind == NativeResource::Kind::Staging) {
				const auto& entry = cpu->layout[a_subresource];
				a_out->pData = static_cast<std::byte*>(data) + entry.offset;
				a_out->RowPitch = entry.rowPitch;
				a_out->DepthPitch = entry.depthPitch;
			} else {
				a_out->pData = data;
				a_out->RowPitch = a_out->DepthPitch = static_cast<UINT>(cpu->size);
			}
			return S_OK;
		}

		void ContextUnmap(ID3D11Resource* a_resource, UINT a_subresource)
		{
			const auto record = Resolve(a_resource);
			if (!record)
				return;
			NativeResource* cpu = CpuSide(*record);
			if (!cpu)
				return;
			// Readback memory flushes nothing back; upload memory flushes what the CPU wrote.
			cpu->resource->Unmap(0, cpu->readback ? 0 : cpu->size);
			// A DYNAMIC texture: the subresource just written goes to the image now, in the context's order.
			if (record->kind == NativeResource::Kind::Image && record->upload && a_subresource < record->shape.Subresources()) {
				const auto image = record;
				const auto upload = record->upload;
				g_device->immediate.Run("unmap (dynamic upload) " + image->name,
					[&](const Immediate::Vulkan& a_vk, VkCommandBuffer a_commands) {
						(void)RecordCopy(a_vk, a_commands, *image, a_subresource, {}, *upload, a_subresource, nullptr);
					},
					{ image, upload }, { image.get(), upload.get() });
			}
		}

		void ContextUpdateSubresource(ID3D11Resource* a_resource, UINT a_subresource, const D3D11_BOX* a_box, const void* a_data, UINT a_rowPitch, UINT a_depthPitch)
		{
			if (!a_data)
				return;
			const auto record = Resolve(a_resource);
			if (!record) {
				static std::atomic_flag logged;
				Unknown(logged, "UpdateSubresource", a_resource);
				return;
			}
			if (record->kind == NativeResource::Kind::Buffer) {
				const uint64_t from = a_box ? a_box->left : 0;
				const uint64_t size = a_box ? a_box->right - a_box->left : record->size;
				if (!size || from + size > record->size)
					return;
				// Written by the CPU when the GPU is done with the buffer, else through an upload buffer in the context's order.
				if (record->hostVisible && !record->readback && g_device->immediate.Done(record->lastUse.load(std::memory_order_acquire))) {
					void* data = nullptr;
					record->resource->Map(&data);
					if (data) {
						std::memcpy(static_cast<std::byte*>(data) + from, a_data, size);
						record->resource->Unmap(from, size);
					}
					return;
				}
				auto upload = g_device->MakeUpload(size);
				if (!upload)
					return;
				void* data = nullptr;
				upload->resource->Map(&data);
				if (!data)
					return;
				std::memcpy(data, a_data, size);
				upload->resource->Unmap(0, size);
				g_device->immediate.Run("update buffer " + record->name,
					[&](const Immediate::Vulkan& a_vk, VkCommandBuffer a_commands) {
						const VkBufferCopy copy{ 0, from, size };
						a_vk.copyBuffer(a_commands, upload->vkBuffer, record->vkBuffer, 1, &copy);
					},
					{ upload, record }, { record.get() });
				return;
			}

			// A texture: the region's rows into an upload buffer laid out as a one-subresource staging texture, then copied in.
			const Shape& shape = record->shape;
			if (a_subresource >= shape.Subresources())
				return;
			const uint32_t mip = a_subresource % shape.mips;
			const VkOffset3D at = a_box ? VkOffset3D{ static_cast<int32_t>(a_box->left), static_cast<int32_t>(a_box->top), static_cast<int32_t>(a_box->front) } :
			                              VkOffset3D{ 0, 0, 0 };
			const VkExtent3D extent = a_box ? VkExtent3D{ a_box->right - a_box->left, a_box->bottom - a_box->top, a_box->back - a_box->front } : MipExtent(shape, mip);
			if (!extent.width || !extent.height || !extent.depth)
				return;
			const uint32_t rows = shape.Rows(extent.height);
			const uint32_t rowBytes = shape.RowBytes(extent.width);
			const uint32_t rowPitch = (rowBytes + 255u) & ~255u;
			const auto writeRows = [&](std::byte* a_base) {
				for (uint32_t z = 0; z < extent.depth; ++z)
					for (uint32_t row = 0; row < rows; ++row)
						std::memcpy(a_base + (uint64_t(z) * rows + row) * rowPitch, static_cast<const std::byte*>(a_data) + size_t(z) * a_depthPitch + size_t(row) * a_rowPitch,
							rowBytes);
			};
			if (record->kind == NativeResource::Kind::Staging) {
				// The CPU writes the staging memory itself, row by row at the region's place.
				if (!g_device->immediate.Wait(record->lastUse.load(std::memory_order_acquire)))
					return;
				void* data = nullptr;
				record->resource->Map(&data);
				if (!data)
					return;
				const auto& entry = record->layout[a_subresource];
				const uint32_t block = shape.BlockSize();
				for (uint32_t z = 0; z < extent.depth; ++z)
					for (uint32_t row = 0; row < rows; ++row)
						std::memcpy(static_cast<std::byte*>(data) + entry.offset + uint64_t(at.z + z) * entry.depthPitch + uint64_t(at.y / block + row) * entry.rowPitch +
										uint64_t(at.x / block) * shape.blockBytes,
							static_cast<const std::byte*>(a_data) + size_t(z) * a_depthPitch + size_t(row) * a_rowPitch, rowBytes);
				record->resource->Unmap(0, record->size);
				return;
			}
			auto upload = g_device->MakeUpload(uint64_t(rowPitch) * rows * extent.depth);
			if (!upload)
				return;
			void* data = nullptr;
			upload->resource->Map(&data);
			if (!data)
				return;
			writeRows(static_cast<std::byte*>(data));
			upload->resource->Unmap(0, upload->size);
			g_device->immediate.Run("update texture " + record->name,
				[&](const Immediate::Vulkan& a_vk, VkCommandBuffer a_commands) {
					VkBufferImageCopy copy{};
					copy.bufferRowLength = rowPitch / shape.blockBytes * shape.BlockSize();
					copy.bufferImageHeight = rows * shape.BlockSize();
					copy.imageSubresource = { shape.copyAspect, mip, a_subresource / shape.mips, 1 };
					copy.imageOffset = at;
					copy.imageExtent = extent;
					a_vk.copyBufferToImage(a_commands, upload->vkBuffer, record->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
				},
				{ upload, record }, { record.get() });
		}

		void ContextCopySubresourceRegion(ID3D11Resource* a_destination, UINT a_destinationSub, UINT a_x, UINT a_y, UINT a_z, ID3D11Resource* a_source,
			UINT a_sourceSub, const D3D11_BOX* a_box)
		{
			const auto destination = Resolve(a_destination);
			const auto source = Resolve(a_source);
			if (!destination || !source) {
				static std::atomic_flag logged;
				Unknown(logged, "CopySubresourceRegion", destination ? static_cast<const void*>(a_source) : a_destination);
				return;
			}
			bool recorded = false;
			g_device->immediate.Run("copy region " + source->name + " to " + destination->name,
				[&](const Immediate::Vulkan& a_vk, VkCommandBuffer a_commands) {
					recorded = RecordCopy(a_vk, a_commands, *destination, a_destinationSub,
						{ static_cast<int32_t>(a_x), static_cast<int32_t>(a_y), static_cast<int32_t>(a_z) }, *source, a_sourceSub, a_box);
				},
				{ destination, source }, { destination.get(), source.get() });
			if (!recorded) {
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::warn("[ORG native] CopySubresourceRegion from \"{}\" to \"{}\" is not a copy native mode makes (logged once)", source->name, destination->name);
			}
		}

		void ContextCopyResource(ID3D11Resource* a_destination, ID3D11Resource* a_source)
		{
			const auto destination = Resolve(a_destination);
			const auto source = Resolve(a_source);
			if (!destination || !source) {
				static std::atomic_flag logged;
				Unknown(logged, "CopyResource", destination ? static_cast<const void*>(a_source) : a_destination);
				return;
			}
			bool recorded = true;
			g_device->immediate.Run("copy " + source->name + " to " + destination->name,
				[&](const Immediate::Vulkan& a_vk, VkCommandBuffer a_commands) {
					if (destination->kind == NativeResource::Kind::Buffer || source->kind == NativeResource::Kind::Buffer) {
						recorded = RecordCopy(a_vk, a_commands, *destination, 0, {}, *source, 0, nullptr);
						return;
					}
					const uint32_t count = (std::min)(destination->shape.Subresources(), source->shape.Subresources());
					for (uint32_t sub = 0; sub < count; ++sub)
						recorded = RecordCopy(a_vk, a_commands, *destination, sub, {}, *source, sub, nullptr) && recorded;
				},
				{ destination, source }, { destination.get(), source.get() });
			if (!recorded) {
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::warn("[ORG native] CopyResource from \"{}\" to \"{}\" is not a copy native mode makes (logged once)", source->name, destination->name);
			}
		}

		void ContextGenerateMips(ID3D11ShaderResourceView* a_view)
		{
			const auto record = Resolve(a_view);
			if (!record || record->kind != NativeResource::Kind::Image) {
				static std::atomic_flag logged;
				Unknown(logged, "GenerateMips", a_view);
				return;
			}
			const Shape& shape = record->shape;
			if (shape.mips < 2 || shape.aspects != VK_IMAGE_ASPECT_COLOR_BIT)
				return;
			g_device->immediate.Run("generate mips " + record->name,
				[&](const Immediate::Vulkan& a_vk, VkCommandBuffer a_commands) {
					// Each mip from the one above it, linearly filtered; a barrier between, since each level reads the last one written.
					for (uint32_t mip = 1; mip < shape.mips; ++mip) {
						VkImageBlit blit{};
						blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, shape.layers };
						blit.srcOffsets[1] = { static_cast<int32_t>(shape.Width(mip - 1)), static_cast<int32_t>(shape.Height(mip - 1)), static_cast<int32_t>(shape.Depth(mip - 1)) };
						blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, shape.layers };
						blit.dstOffsets[1] = { static_cast<int32_t>(shape.Width(mip)), static_cast<int32_t>(shape.Height(mip)), static_cast<int32_t>(shape.Depth(mip)) };
						a_vk.blitImage(a_commands, record->image.image, VK_IMAGE_LAYOUT_GENERAL, record->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
						VkMemoryBarrier2 memory{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
						memory.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
						memory.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
						memory.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
						memory.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
						VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
						dependency.memoryBarrierCount = 1;
						dependency.pMemoryBarriers = &memory;
						a_vk.barrier(a_commands, &dependency);
					}
				},
				{ record }, { record.get() });
		}

		/** @brief The image subresources a render target, depth-stencil or unordered access view covers. */
		template <class ViewDesc>
		VkImageSubresourceRange ViewRange(const ViewDesc& a_desc, const Shape& a_shape, VkImageAspectFlags a_aspects)
		{
			VkImageSubresourceRange range{ a_aspects, 0, 1, 0, a_shape.layers };
			// Render target and unordered access views number BUFFER 1 and the textures from 2; depth-stencil views have no buffer.
			// Shifted, all three number TEXTURE1D 1, TEXTURE1DARRAY 2, TEXTURE2D 3, TEXTURE2DARRAY 4.
			int dimension = static_cast<int>(a_desc.ViewDimension);
			if constexpr (!std::is_same_v<ViewDesc, D3D11_DEPTH_STENCIL_VIEW_DESC>)
				--dimension;
			switch (dimension) {
			case 1:  // TEXTURE1D
				range.baseMipLevel = a_desc.Texture1D.MipSlice;
				range.layerCount = 1;
				break;
			case 2:  // TEXTURE1DARRAY
				range.baseMipLevel = a_desc.Texture1DArray.MipSlice;
				range.baseArrayLayer = a_desc.Texture1DArray.FirstArraySlice;
				range.layerCount = a_desc.Texture1DArray.ArraySize;
				break;
			case 3:  // TEXTURE2D
				range.baseMipLevel = a_desc.Texture2D.MipSlice;
				range.layerCount = 1;
				break;
			case 4:  // TEXTURE2DARRAY
				range.baseMipLevel = a_desc.Texture2DArray.MipSlice;
				range.baseArrayLayer = a_desc.Texture2DArray.FirstArraySlice;
				range.layerCount = a_desc.Texture2DArray.ArraySize;
				break;
			default:  // a 3D view, or one native mode does not narrow: the whole first mip
				break;
			}
			range.layerCount = (std::min)(range.layerCount, a_shape.layers - (std::min)(range.baseArrayLayer, a_shape.layers));
			return range;
		}

		void ClearColor(const std::shared_ptr<NativeResource>& a_record, const VkImageSubresourceRange& a_range, const VkClearColorValue& a_value)
		{
			g_device->immediate.Run("clear " + a_record->name,
				[&](const Immediate::Vulkan& a_vk, VkCommandBuffer a_commands) {
					a_vk.clearColorImage(a_commands, a_record->image.image, VK_IMAGE_LAYOUT_GENERAL, &a_value, 1, &a_range);
				},
				{ a_record }, { a_record.get() });
		}

		void ContextClearRenderTargetView(ID3D11RenderTargetView* a_view, const FLOAT a_color[4])
		{
			const auto record = Resolve(a_view);
			if (!record || record->kind != NativeResource::Kind::Image || !a_color) {
				static std::atomic_flag logged;
				Unknown(logged, "ClearRenderTargetView", a_view);
				return;
			}
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			VkClearColorValue value{};
			std::memcpy(value.float32, a_color, sizeof(value.float32));
			ClearColor(record, ViewRange(desc, record->shape, VK_IMAGE_ASPECT_COLOR_BIT), value);
		}

		/** @brief A buffer UAV's byte range: its elements times their size (structured stride, 4 for raw, the format's for typed). */
		bool UavBytes(const NativeResource& a_record, const D3D11_UNORDERED_ACCESS_VIEW_DESC& a_desc, VkDeviceSize& a_offset, VkDeviceSize& a_size)
		{
			if (a_desc.ViewDimension != D3D11_UAV_DIMENSION_BUFFER)
				return false;
			uint32_t element = 4;
			if (a_desc.Format == DXGI_FORMAT_UNKNOWN)
				element = (std::max)(a_record.bufferDesc.StructureByteStride, 4u);
			else if (!(a_desc.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW))
				element = (std::max)(rhi::helpers::BytesPerBlock(rhi::helpers::ToRHI(a_desc.Format)), 4u);
			a_offset = VkDeviceSize(a_desc.Buffer.FirstElement) * element;
			a_size = VkDeviceSize(a_desc.Buffer.NumElements) * element;
			if (a_offset >= a_record.size)
				return false;
			a_size = (std::min)(a_size, a_record.size - a_offset) & ~VkDeviceSize(3);
			return a_size != 0;
		}

		/** @brief A UAV clear: an image's subresources with a_value, or a buffer's range filled with a_word (vkCmdFillBuffer's one word). */
		void ClearUnorderedAccess(ID3D11UnorderedAccessView* a_view, const VkClearColorValue& a_value, uint32_t a_word, const char* a_operation)
		{
			const auto record = Resolve(a_view);
			if (!record) {
				static std::atomic_flag logged;
				Unknown(logged, a_operation, a_view);
				return;
			}
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			if (record->kind == NativeResource::Kind::Image) {
				ClearColor(record, ViewRange(desc, record->shape, VK_IMAGE_ASPECT_COLOR_BIT), a_value);
				return;
			}
			VkDeviceSize offset = 0, size = 0;
			if (record->kind != NativeResource::Kind::Buffer || !UavBytes(*record, desc, offset, size))
				return;
			g_device->immediate.Run("fill " + record->name,
				[&](const Immediate::Vulkan& a_vk, VkCommandBuffer a_commands) { a_vk.fillBuffer(a_commands, record->vkBuffer, offset, size, a_word); },
				{ record }, { record.get() });
		}

		void ContextClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* a_view, const UINT a_values[4])
		{
			if (!a_values)
				return;
			VkClearColorValue value{};
			std::memcpy(value.uint32, a_values, sizeof(value.uint32));
			ClearUnorderedAccess(a_view, value, a_values[0], "ClearUnorderedAccessViewUint");
		}

		void ContextClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* a_view, const FLOAT a_values[4])
		{
			if (!a_values)
				return;
			VkClearColorValue value{};
			std::memcpy(value.float32, a_values, sizeof(value.float32));
			uint32_t word = 0;
			std::memcpy(&word, &a_values[0], sizeof(word));
			ClearUnorderedAccess(a_view, value, word, "ClearUnorderedAccessViewFloat");
		}

		void ContextClearDepthStencilView(ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
			const auto record = Resolve(a_view);
			if (!record || record->kind != NativeResource::Kind::Image) {
				static std::atomic_flag logged;
				Unknown(logged, "ClearDepthStencilView", a_view);
				return;
			}
			VkImageAspectFlags aspects = 0;
			if (a_flags & D3D11_CLEAR_DEPTH)
				aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
			if (a_flags & D3D11_CLEAR_STENCIL)
				aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
			aspects &= record->shape.aspects;
			if (!aspects)
				return;
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			const VkImageSubresourceRange range = ViewRange(desc, record->shape, aspects);
			const VkClearDepthStencilValue value{ a_depth, a_stencil };
			g_device->immediate.Run("clear depth " + record->name,
				[&](const Immediate::Vulkan& a_vk, VkCommandBuffer a_commands) {
					a_vk.clearDepthStencilImage(a_commands, record->image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
				},
				{ record }, { record.get() });
		}

		/**
		 * @brief The context's ID3DUserDefinedAnnotation: State's debugger events. Native mode has no D3D11 capture to show them in,
		 * so it reports no tool attached and records nothing (the graph labels its own passes).
		 */
		class Annotation final : public ID3DUserDefinedAnnotation
		{
		public:
			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_out) override
			{
				if (!a_out)
					return E_POINTER;
				if (a_riid == __uuidof(IUnknown) || a_riid == __uuidof(ID3DUserDefinedAnnotation)) {
					AddRef();
					*a_out = static_cast<ID3DUserDefinedAnnotation*>(this);
					return S_OK;
				}
				*a_out = nullptr;
				return E_NOINTERFACE;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG left = --refs;
				if (!left)
					delete this;
				return left;
			}
			INT STDMETHODCALLTYPE BeginEvent(LPCWSTR) override { return ++depth; }
			INT STDMETHODCALLTYPE EndEvent() override { return depth > 0 ? --depth : -1; }
			void STDMETHODCALLTYPE SetMarker(LPCWSTR) override {}
			BOOL STDMETHODCALLTYPE GetStatus() override { return FALSE; }

		private:
			std::atomic<ULONG> refs{ 1 };
			std::atomic<INT> depth{ 0 };
		};

		// ---- Queries ---------------------------------------------------------------------------------------------------------

		/**
		 * @brief What End marked on each query: the context's work submitted so far. Keyed by the query handle; an entry is replaced
		 * by the next End of the same query and left when the query goes (a few bytes per query the engine ever made).
		 */
		struct Queries
		{
			std::mutex lock;
			std::unordered_map<const void*, uint64_t> ended;
		};

		Queries& GetQueries()
		{
			static Queries queries;
			return queries;
		}

		void ContextEnd(ID3D11Asynchronous* a_query)
		{
			if (!a_query)
				return;
			auto& queries = GetQueries();
			std::lock_guard guard(queries.lock);
			queries.ended[a_query] = g_device->immediate.Submitted();
		}

		/**
		 * @brief A query's answer once the context's work up to its End is done on the GPU (S_FALSE until then; the engine polls).
		 * Native mode measures nothing: an event is done, an occlusion query counts no samples and its predicate says hidden,
		 * timestamps are 0 and marked disjoint (so readers drop them), statistics are 0.
		 */
		HRESULT ContextGetData(ID3D11Asynchronous* a_query, void* a_data, UINT a_size, UINT)
		{
			if (!a_query)
				return E_INVALIDARG;
			uint64_t ended = 0;
			{
				auto& queries = GetQueries();
				std::lock_guard guard(queries.lock);
				const auto found = queries.ended.find(a_query);
				if (found == queries.ended.end())
					return S_FALSE;  // never ended
				ended = found->second;
			}
			if (!g_device->immediate.Done(ended))
				return S_FALSE;
			if (!a_data || !a_size)
				return S_OK;
			std::memset(a_data, 0, a_size);
			winrt::com_ptr<ID3D11Query> query;
			if (FAILED(a_query->QueryInterface(IID_PPV_ARGS(query.put()))) || !query)
				return S_OK;
			D3D11_QUERY_DESC desc{};
			query->GetDesc(&desc);
			switch (desc.Query) {
			case D3D11_QUERY_EVENT:
				if (a_size >= sizeof(BOOL))
					*static_cast<BOOL*>(a_data) = TRUE;
				break;
			case D3D11_QUERY_TIMESTAMP_DISJOINT:
				if (a_size >= sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT)) {
					auto* disjoint = static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(a_data);
					disjoint->Frequency = 1'000'000'000ull;
					disjoint->Disjoint = TRUE;
				}
				break;
			default:
				break;  // occlusion (0 samples), predicates (FALSE), timestamps and statistics (0)
			}
			return S_OK;
		}

		// ---- Wrapping a port's BasicRHI resources ----------------------------------------------------------------------------

		/** @brief A record that only describes an image or buffer someone else owns (the port keeps it alive past the handle). */
		std::shared_ptr<NativeResource> MakeWrapRecord(const char* a_name)
		{
			auto record = std::make_shared<NativeResource>();
			record->name = a_name ? a_name : "Native::Wrapped";
			return record;
		}

		HRESULT ContextQueryInterface(REFIID a_riid, void** a_out)
		{
			if (!a_out)
				return E_POINTER;
			*a_out = nullptr;
			if (a_riid != __uuidof(ID3DUserDefinedAnnotation))
				return E_NOINTERFACE;
			static Annotation* s_annotation = new Annotation();  // kept for the process, as the context is
			return s_annotation->QueryInterface(a_riid, a_out);
		}
	}

	bool Active()
	{
		return g_device != nullptr;
	}

	std::shared_ptr<rhi::DevicePtr> Device()
	{
		return g_device ? g_device->device : nullptr;
	}

	HRESULT CreateDeviceAndSwapChain(IDXGIAdapter* a_adapter, DXGI_SWAP_CHAIN_DESC* a_desc, IDXGISwapChain** a_swapChain, ID3D11Device** a_device,
		D3D_FEATURE_LEVEL* a_featureLevel, ID3D11DeviceContext** a_context)
	{
		if (g_device)
			Fail("a second device", "the game asked for a second D3D11 device; native mode makes one");
		if (!a_desc || !a_swapChain || !a_device || !a_context)
			Fail("the game's device creation", "D3D11CreateDeviceAndSwapChain was called without a swap chain, a device or a context");

		auto* state = new NativeState();
		state->adapter.copy_from(a_adapter);
		std::string reason;
		if (!CreateBasicRhiDevice(a_adapter, state->device, reason))
			Fail("the native device", reason);
		state->graphics = state->Rhi().GetQueue(rhi::QueueKind::Graphics);
		if (!state->oneShot.Init(state->Rhi()))
			Fail("the native device", "its command lists or timeline for resource creation could not be made");
		if (!state->immediate.Init(state->Rhi()))
			Fail("the native device", "the context's command lists, timeline or Vulkan entry points could not be made");

		// The swap chain on the game's window. Native mode never takes exclusive fullscreen.
		state->window = a_desc->OutputWindow;
		state->format = BackBufferFormat(a_desc->BufferDesc.Format);
		state->swapchainFormat = SwapchainFormat(a_desc->BufferDesc.Format);
		state->bufferCount = (std::max)(a_desc->BufferCount, 2u);
		state->width = a_desc->BufferDesc.Width;
		state->height = a_desc->BufferDesc.Height;
		if (!state->width || !state->height) {
			RECT client{};
			::GetClientRect(state->window, &client);
			state->width = (std::max)(static_cast<uint32_t>(client.right - client.left), 1u);
			state->height = (std::max)(static_cast<uint32_t>(client.bottom - client.top), 1u);
		}
		if (state->Rhi().CreateSwapchain(state->window, state->width, state->height, state->swapchainFormat, state->bufferCount, true, state->swapchain) !=
				rhi::Result::Ok ||
			!state->swapchain)
			Fail("the native swap chain", "BasicRHI could not create a swap chain on the game's window");
		(void)state->swapchain->SetMaximumFrameLatency(state->frameLatency);
		if (!state->MakePresentObjects())
			Fail("the native swap chain", "the present's command lists or timeline could not be made");
		g_device = state;
		if (!state->MakeBackBuffer())
			Fail("the native back buffer", "BasicRHI could not make it (see the [ORG native] line above)");
		const auto caps = state->swapchain->Caps();
		Stage("swap chain", fmt::format("{}x{} DXGI format {}, {} images, tearing {}, HDR10 {}; the game asked for {} ({})", state->width, state->height,
								static_cast<int>(state->format), state->swapchain->ImageCount(), caps.tearing,
								state->swapchain->IsColorSpaceSupported(rhi::SwapchainColorSpace::Hdr10St2084), a_desc->Windowed ? "windowed" : "fullscreen",
								static_cast<int>(a_desc->BufferDesc.Format)));

		// The handles the engine stores. Every call on them is a native service or a tripwire.
		Tripwire::NativeServices services{};
		services.createTexture1D = &CreateTexture1D;
		services.createTexture2D = &CreateTexture2D;
		services.createTexture3D = &CreateTexture3D;
		services.createBuffer = &CreateBuffer;
		services.queryDxgiDevice = &QueryDxgiDevice;
		services.present = &SwapChainPresent;
		services.getBuffer = &SwapChainGetBuffer;
		services.resizeBuffers = &SwapChainResizeBuffers;
		services.getDesc = &SwapChainGetDesc;
		services.getContainingOutput = &SwapChainGetContainingOutput;
		services.checkFeatureSupport = &CheckFeatureSupport;
		services.checkFormatSupport = &CheckFormatSupport;
		services.map = &ContextMap;
		services.unmap = &ContextUnmap;
		services.updateSubresource = &ContextUpdateSubresource;
		services.copyResource = &ContextCopyResource;
		services.copySubresourceRegion = &ContextCopySubresourceRegion;
		services.generateMips = &ContextGenerateMips;
		services.clearRenderTargetView = &ContextClearRenderTargetView;
		services.clearUnorderedAccessViewUint = &ContextClearUnorderedAccessViewUint;
		services.clearUnorderedAccessViewFloat = &ContextClearUnorderedAccessViewFloat;
		services.clearDepthStencilView = &ContextClearDepthStencilView;
		services.queryContext = &ContextQueryInterface;
		services.end = &ContextEnd;
		services.getData = &ContextGetData;
		Tripwire::SetNativeServices(services);
		state->dxgiDevice.attach(new DxgiDevice());

		auto device = Tripwire::CreateDevice({ .name = "Engine::Device (native)" });
		auto context = Tripwire::CreateDeviceContext({ .name = "Engine::ImmediateContext (native)" });
		Tripwire::PairDeviceAndContext(device.get(), context.get());
		DXGI_SWAP_CHAIN_DESC handleDesc{};
		state->FillDesc(handleDesc);
		auto swapChain = Tripwire::CreateSwapChain(handleDesc, { .name = "Engine::SwapChain (native)" });
		state->deviceHandle = device.get();
		Tripwire::InstallNativeDevice(device.get(), context.get());

		*a_device = device.detach();
		*a_context = context.detach();
		*a_swapChain = swapChain.detach();
		if (a_featureLevel)
			*a_featureLevel = D3D_FEATURE_LEVEL_11_1;
		Stage("handles", "the engine gets native device, context and swap chain handles");
		return S_OK;
	}

	bool DescribeResource(const IUnknown* a_object, DxvkOrgInteropResourceInfo& a_info)
	{
		const auto record = Resolve(a_object);
		if (!record)
			return false;
		a_info = {};
		a_info.version = DXVK_ORG_INTEROP_VERSION;
		if (record->kind == NativeResource::Kind::Buffer) {
			// As DXVK describes a buffer: its range of the Vulkan buffer, with the device address DCLF draws from.
			a_info.kind = DXVK_ORG_INTEROP_RESOURCE_BUFFER;
			a_info.buffer = { record->vkBuffer, 0, record->size, record->address, record->usage };
			return true;
		}
		if (record->kind != NativeResource::Kind::Image)
			return false;
		a_info.kind = DXVK_ORG_INTEROP_RESOURCE_IMAGE;
		a_info.image = record->image;
		// A shader resource view is described as DXVK describes one: its mips and slices, and the depth aspect alone for a depth image.
		const auto described = Tripwire::Describe(a_object);
		if (described.kind == Tripwire::Kind::ShaderResourceView) {
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			static_cast<ID3D11ShaderResourceView*>(const_cast<IUnknown*>(a_object))->GetDesc(&view);
			auto& range = a_info.image.subresourceRange;
			range.aspectMask = record->shape.copyAspect;
			switch (view.ViewDimension) {
			case D3D11_SRV_DIMENSION_TEXTURE2D:
				range.baseMipLevel = view.Texture2D.MostDetailedMip;
				range.levelCount = view.Texture2D.MipLevels == UINT(-1) ? record->shape.mips - range.baseMipLevel : view.Texture2D.MipLevels;
				range.baseArrayLayer = 0;
				range.layerCount = 1;
				a_info.image.viewType = VK_IMAGE_VIEW_TYPE_2D;
				break;
			case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
				range.baseMipLevel = view.Texture2DArray.MostDetailedMip;
				range.levelCount = view.Texture2DArray.MipLevels == UINT(-1) ? record->shape.mips - range.baseMipLevel : view.Texture2DArray.MipLevels;
				range.baseArrayLayer = view.Texture2DArray.FirstArraySlice;
				range.layerCount = view.Texture2DArray.ArraySize;
				a_info.image.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
				break;
			default:
				break;  // cubes, 3D and 1D views: the whole image, as made
			}
		}
		return true;
	}

	HRESULT Present(UINT a_syncInterval, UINT a_flags)
	{
		return g_device ? g_device->Present(a_syncInterval, a_flags) : E_FAIL;
	}

	winrt::com_ptr<ID3D11Resource> WrapTexture(const rhi::Resource& a_image, const DxvkOrgInteropTextureWrap& a_wrap, DxvkOrgInteropResourceInfo& a_info,
		const char* a_name, const D3D11_SUBRESOURCE_DATA* a_data)
	{
		if (!g_device)
			return nullptr;
		const auto dimension = static_cast<D3D11_RESOURCE_DIMENSION>(a_wrap.dimension);
		TextureRequest request{ dimension, a_wrap.width, a_wrap.height, (std::max)(a_wrap.depthOrArraySize, 1u), (std::max)(a_wrap.mipLevels, 1u),
			(std::max)(a_wrap.sampleCount, 1u), static_cast<DXGI_FORMAT>(a_wrap.format), D3D11_USAGE_DEFAULT, a_wrap.bindFlags, 0, a_wrap.miscFlags, a_data,
			a_name ? a_name : "Native::Wrapped" };
		const DXGI_FORMAT dxgiFormat = ResolveTypeless(request.format, request.bindFlags);
		const rhi::Format format = rhi::helpers::ToRHI(dxgiFormat);
		const uint32_t depth = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? request.depthOrArraySize : 1;
		const uint32_t layers = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1 : request.depthOrArraySize;
		auto record = MakeWrapRecord(a_name);
		static std::atomic<bool> loggedDescribe = false, loggedInitialize = false;
		if (!NativeState::FillImage(*record, a_image, request, dxgiFormat, format, request.mipLevels, layers, depth)) {
			LogOnce(loggedDescribe, request.name, "BasicRHI did not describe the image to wrap");
			return nullptr;
		}
		// Into GENERAL with its contents, or zeroed as DXVK's wrap zeroes it.
		if (!g_device->Initialize(a_image.GetHandle(), request, format, request.mipLevels, layers, depth)) {
			LogOnce(loggedInitialize, request.name, "the image to wrap could not be moved into GENERAL or given its contents");
			return nullptr;
		}
		// vkCmdClearColorImage cannot write a block-compressed image (DynamicCubemaps' BC6H cubemaps): those are left as made, which
		// their owners write before reading.
		if (!a_data && !record->shape.blocks) {
			const VkImageSubresourceRange all{ record->shape.aspects, 0, request.mipLevels, 0, layers };
			g_device->immediate.Run("zero " + record->name,
				[&](const Immediate::Vulkan& a_vk, VkCommandBuffer a_commands) {
					if (record->shape.aspects == VK_IMAGE_ASPECT_COLOR_BIT) {
						const VkClearColorValue zero{};
						a_vk.clearColorImage(a_commands, record->image.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
					} else {
						const VkClearDepthStencilValue zero{};
						a_vk.clearDepthStencilImage(a_commands, record->image.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
					}
				},
				{ record }, { record.get() });
		}
		a_info = {};
		a_info.version = DXVK_ORG_INTEROP_VERSION;
		a_info.kind = DXVK_ORG_INTEROP_RESOURCE_IMAGE;
		a_info.image = record->image;

		Tripwire::Options options;
		options.name = request.name;
		options.owner = record;
		const auto format11 = static_cast<DXGI_FORMAT>(a_wrap.format);
		winrt::com_ptr<ID3D11Resource> result;
		switch (dimension) {
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			{
				const D3D11_TEXTURE3D_DESC desc{ request.width, request.height, depth, request.mipLevels, format11, D3D11_USAGE_DEFAULT, request.bindFlags, 0, request.miscFlags };
				result.copy_from(Tripwire::CreateTexture3D(desc, std::move(options)).get());
				break;
			}
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			{
				const D3D11_TEXTURE1D_DESC desc{ request.width, request.mipLevels, layers, format11, D3D11_USAGE_DEFAULT, request.bindFlags, 0, request.miscFlags };
				result.copy_from(Tripwire::CreateTexture1D(desc, std::move(options)).get());
				break;
			}
		default:
			{
				const D3D11_TEXTURE2D_DESC desc{ request.width, request.height, request.mipLevels, layers, format11, { request.sampleCount, 0 }, D3D11_USAGE_DEFAULT,
					request.bindFlags, 0, request.miscFlags };
				result.copy_from(Tripwire::CreateTexture2D(desc, std::move(options)).get());
				break;
			}
		}
		return result;
	}

	winrt::com_ptr<ID3D11Buffer> WrapBuffer(const rhi::Resource& a_buffer, const D3D11_BUFFER_DESC& a_desc, const char* a_name)
	{
		if (!g_device)
			return nullptr;
		rhi::VulkanResourceInfo info{};
		if (!rhi::vulkan::get_resource_info(a_buffer, info) || !info.resource) {
			static std::atomic<bool> logged = false;
			LogOnce(logged, a_name ? a_name : "a buffer", "BasicRHI did not describe the buffer to wrap");
			return nullptr;
		}
		auto record = MakeWrapRecord(a_name);
		record->kind = NativeResource::Kind::Buffer;
		record->vkBuffer = rhi::vulkan::from_native_void<VkBuffer>(info.resource);
		record->address = static_cast<VkDeviceAddress>(info.deviceAddress);
		record->usage = static_cast<VkBufferUsageFlags>(info.usage);
		record->size = a_desc.ByteWidth;
		record->bufferDesc = a_desc;
		Tripwire::Options options;
		options.name = record->name;
		options.owner = record;
		return Tripwire::CreateBuffer(a_desc, std::move(options));
	}


	void Shutdown()
	{
		if (!g_device)
			return;
		std::lock_guard guard(g_device->presentLock);
		g_device->WaitPresents();
		g_device->lost = true;  // nothing more is presented
		logger::info("[ORG native] the window closes: presents stopped after {} frames", g_device->presents);
	}
}

#else  // !CS_HAS_RENDER_GRAPH

namespace RenderGraph::Engine::Native
{
	bool Active() { return false; }
	std::shared_ptr<rhi::DevicePtr> Device() { return nullptr; }
	HRESULT CreateDeviceAndSwapChain(IDXGIAdapter*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**)
	{
		Fail("this build", "it was built without OpenRenderGraph");
	}
	bool DescribeResource(const IUnknown*, DxvkOrgInteropResourceInfo&) { return false; }
	HRESULT Present(UINT, UINT) { return E_FAIL; }
	winrt::com_ptr<ID3D11Resource> WrapTexture(const rhi::Resource&, const DxvkOrgInteropTextureWrap&, DxvkOrgInteropResourceInfo&, const char*, const D3D11_SUBRESOURCE_DATA*)
	{
		return nullptr;
	}
	winrt::com_ptr<ID3D11Buffer> WrapBuffer(const rhi::Resource&, const D3D11_BUFFER_DESC&, const char*) { return nullptr; }
	void Shutdown() {}
}

#endif
