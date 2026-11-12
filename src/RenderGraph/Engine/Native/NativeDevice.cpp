#include "RenderGraph/Engine/Native/Native.h"

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include <rhi_helpers.h>

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
		struct NativeResource
		{
			rhi::ResourcePtr resource;
			DxvkOrgInteropImageInfo image{};
			bool isImage = false;
			std::string name;

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

		/** @brief One-shot command lists on the graphics queue for creation-time work (initial layout, initial data), waited for. */
		class OneShot
		{
		public:
			bool Init(rhi::Device a_device)
			{
				device = a_device;
				queue = device.GetQueue(rhi::QueueKind::Graphics);
				return device.CreateCommandAllocator(rhi::QueueKind::Graphics, allocator) == rhi::Result::Ok && allocator &&
				       device.CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), list) == rhi::Result::Ok && list &&
				       device.CreateTimeline(timeline, 0, "CS native one-shot") == rhi::Result::Ok && timeline;
			}

			/** @brief Records a_record, submits it and waits for the GPU. Any thread (one at a time). */
			template <class F>
			bool Run(F&& a_record)
			{
				std::lock_guard guard(lock);
				allocator->Recycle();
				list->Recycle(allocator.Get());
				a_record(list.Get());
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
				if (a_request.usage == D3D11_USAGE_STAGING || a_request.usage == D3D11_USAGE_DYNAMIC || a_request.cpuAccess) {
					LogOnce(loggedCpuAccess, a_request.name, "native textures with CPU access (STAGING, DYNAMIC) are not built: their Map has nothing behind it");
					return E_NOTIMPL;
				}
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

				// The image as the graph imports it (DescribeResource): GENERAL, which is the graph's Common home state.
				rhi::VulkanResourceInfo info{};
				if (!rhi::vulkan::get_resource_info(record->resource.Get(), info) || !info.resource) {
					LogOnce(loggedCreate, a_request.name, "BasicRHI did not describe the image it made");
					return E_FAIL;
				}
				auto& image = record->image;
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
				record->isImage = true;

				if (!Initialize(*record, a_request, format, mips, layers, depth)) {
					LogOnce(loggedUpload, a_request.name, "its initial layout or contents could not be written");
					return E_FAIL;
				}
				a_out = std::move(record);
				return S_OK;
			}

			/**
			 * @brief Moves a new image into GENERAL, writing its initial data on the way: an upload buffer, one copy per subresource
			 * (D3D11's order: mips of slice 0, then slice 1, ...). Waits for the GPU, so the image is ready when the handle exists.
			 */
			bool Initialize(NativeResource& a_record, const TextureRequest& a_request, rhi::Format a_format, uint32_t a_mips, uint32_t a_layers, uint32_t a_depth)
			{
				const rhi::ResourceHandle texture = a_record.resource->GetHandle();
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

				return oneShot.Run([&](rhi::CommandList a_list) {
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
				rhi::ResourceDesc desc{};
				desc.type = rhi::ResourceType::Buffer;
				desc.heapType = (a_desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ) ? rhi::HeapType::Readback : hostVisible ? rhi::HeapType::Upload : rhi::HeapType::DeviceLocal;
				desc.resourceFlags = (a_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) ? rhi::RF_AllowUnorderedAccess : rhi::RF_None;
				desc.debugName = "CS native buffer";
				desc.buffer.sizeBytes = (std::max)(a_desc.ByteWidth, 1u);
				auto record = std::make_shared<NativeResource>();
				record->name = "Native::Buffer";
				if (Rhi().CreateCommittedResource(desc, record->resource) != rhi::Result::Ok || !record->resource) {
					LogOnce(loggedCreate, record->name, "BasicRHI could not create the buffer");
					return E_OUTOFMEMORY;
				}
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
		const auto owner = Tripwire::OwnerOf(a_object);
		if (!owner)
			return false;
		{
			auto& registry = GetRegistry();
			std::lock_guard guard(registry.lock);
			if (!registry.live.contains(owner.get()))
				return false;
		}
		const auto* record = static_cast<const NativeResource*>(owner.get());
		if (!record->isImage)
			return false;
		a_info = {};
		a_info.version = DXVK_ORG_INTEROP_VERSION;
		a_info.kind = DXVK_ORG_INTEROP_RESOURCE_IMAGE;
		a_info.image = record->image;
		return true;
	}

	HRESULT Present(UINT a_syncInterval, UINT a_flags)
	{
		return g_device ? g_device->Present(a_syncInterval, a_flags) : E_FAIL;
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
	void Shutdown() {}
}

#endif
