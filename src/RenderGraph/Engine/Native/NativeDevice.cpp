#include "RenderGraph/Engine/Native/Native.h"

#include <mutex>
#include <set>
// Records mirror D3D11 description member names, and locals that copy them keep those names.
#pragma warning(disable: 4458)

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include <rhi_helpers.h>

#include <d3d11_1.h>
#include <functional>

#include "Aftermath.h"
#include "Features/Upscaling.h"
#include "Features/Upscaling/NativePresenter.h"
#include "Features/Upscaling/Streamline.h"
#include "Globals.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/Engine/Tripwire/GuestDevice.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Backend.h"
#include "RenderGraph/BuildManager.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/DxvkImageImport.h"
#include "RenderGraph/Engine/D3D12Device.h"
#include "RenderGraph/Engine/Skeleton/SkeletonSites.h"
#include "RenderGraph/ExternalWork.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/BoundaryCheck.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/Revision.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/ImageRegistry.h"
#include "RenderGraph/GpuTimingSource.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Switches.h"

#include <OpenRenderGraph/GraphHost.h>

#if defined(CS_ORG_HAS_D3D12)
#	include <d3d12.h>
#endif

#include <Resources/ExternalTextureResource.h>
#include <Resources/PixelBuffer.h>
#include <Resources/Relocation.h>
#include <Resources/TextureDescription.h>
#include <Resources/VersionedTexture.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <chrono>
#include <mutex>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RenderGraph::Engine::Native
{
	namespace
	{
		// ---- Device creation -------------------------------------------------------------------------------------------------

		// CS's own code must resolve Vulkan entry points through BasicRHI's loader, not volk's globals.
		PFN_vkGetInstanceProcAddr LoaderProc()
		{
			static const auto s_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(::GetProcAddress(::LoadLibraryW(L"vulkan-1.dll"), "vkGetInstanceProcAddr"));
			return s_proc;
		}

		bool OnD3D12()
		{
			return ::RenderGraph::Backend::IsD3D12();
		}

		void* D3D12ResourceOf(const rhi::Resource& a_resource)
		{
			rhi::D3D12ResourceInfo info{};
			return rhi::QueryNativeResource(a_resource, rhi::RHI_IID_D3D12_RESOURCE, &info, sizeof(info)) ? info.resource : nullptr;
		}

		/** @brief A D3D12 buffer's resource and where the buffer starts in it: a small buffer may be a range of a shared resource. */
		void* D3D12BufferRangeOf(const rhi::Resource& a_buffer, uint64_t& a_offset)
		{
			rhi::D3D12BufferRangeInfo info{};
			a_offset = 0;
			if (!rhi::QueryNativeResource(a_buffer, rhi::RHI_IID_D3D12_BUFFER_RANGE, &info, sizeof(info)))
				return nullptr;
			a_offset = info.offset;
			return info.resource;
		}

		/** @brief A D3D12 buffer's GPU virtual address. 0 for a texture or off D3D12. */
		uint64_t D3D12AddressOf(void* a_resource)
		{
#if defined(CS_ORG_HAS_D3D12)
			if (!a_resource)
				return 0;
			auto* resource = static_cast<ID3D12Resource*>(a_resource);
			return resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? resource->GetGPUVirtualAddress() : 0;
#else
			(void)a_resource;
			return 0;
#endif
		}

		VkBaseOutStructure* FindStruct(const void* a_chain, VkStructureType a_type)
		{
			for (auto* s = static_cast<VkBaseOutStructure*>(const_cast<void*>(a_chain)); s; s = s->pNext)
				if (s->sType == a_type)
					return s;
			return nullptr;
		}

		/**
		 * @brief Enables what DXVK's device enabled, since the ports were written against it. That is every supported core feature
		 * except robustBufferAccess and sparse binding, plus null descriptors and the graph's requested Vulkan 1.2 features.
		 */
		struct FeatureHook
		{
			VkPhysicalDeviceFeatures core{};
			VkPhysicalDeviceRobustness2FeaturesEXT robustness2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
			VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT generatedCommands{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT };
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
					return true;  // the graph's binding check names anything missing

				uint32_t count = 0;
				enumerate(physical, nullptr, &count, nullptr);
				std::vector<VkExtensionProperties> available(count);
				enumerate(physical, nullptr, &count, available.data());
				const auto has = [&](const char* a_name) {
					return std::ranges::any_of(available, [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, a_name) == 0; });
				};

				VkPhysicalDeviceRobustness2FeaturesEXT robustness{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
				VkPhysicalDeviceVulkan13Features vk13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
				vk13.pNext = has(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) ? &robustness : nullptr;
				VkPhysicalDeviceVulkan12Features vk12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
				vk12.pNext = &vk13;
				VkPhysicalDeviceVulkan11Features vk11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
				vk11.pNext = &vk12;
				VkPhysicalDeviceFeatures2 supported{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
				supported.pNext = &vk11;
				getFeatures2(physical, &supported);

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

				// Enable every supported 1.1 to 1.3 feature, as DXVK does. DCLF needs drawIndirectCount.
				// Skip versions BasicRHI has no struct for, because the chain may hold single-feature structs and both together are invalid.
				const auto orAll = [](void* a_into, const void* a_from, size_t a_size) {
					auto* into = reinterpret_cast<VkBool32*>(static_cast<std::byte*>(a_into) + sizeof(VkBaseOutStructure));
					const auto* from = reinterpret_cast<const VkBool32*>(static_cast<const std::byte*>(a_from) + sizeof(VkBaseOutStructure));
					for (size_t i = 0; i < (a_size - sizeof(VkBaseOutStructure)) / sizeof(VkBool32); ++i)
						into[i] |= from[i];
				};
				auto* chain12 = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(FindStruct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES));
				if (chain12)
					orAll(chain12, &vk12, sizeof(vk12));
				// Protected memory changes how queues and memory are made, and nothing uses it.
				vk11.protectedMemory = VK_FALSE;
				if (auto* chain11 = FindStruct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES))
					orAll(chain11, &vk11, sizeof(vk11));
				if (auto* chain13 = FindStruct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES))
					orAll(chain13, &vk13, sizeof(vk13));

				self->extensions.assign(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
				const auto addExtension = [&](const char* a_name) {
					if (std::ranges::none_of(self->extensions, [&](const char* e) { return std::strcmp(e, a_name) == 0; }))
						self->extensions.push_back(a_name);
					info->enabledExtensionCount = static_cast<uint32_t>(self->extensions.size());
					info->ppEnabledExtensionNames = self->extensions.data();
				};

				// BasicRHI reads the null descriptor flag from the create info.
				bool nullDescriptor = false;
				if (vk13.pNext && robustness.nullDescriptor) {
					auto* chainRobustness =
						reinterpret_cast<VkPhysicalDeviceRobustness2FeaturesEXT*>(FindStruct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT));
					if (!chainRobustness) {
						self->robustness2.pNext = const_cast<void*>(info->pNext);
						info->pNext = &self->robustness2;
						chainRobustness = &self->robustness2;
					}
					chainRobustness->nullDescriptor = VK_TRUE;
					addExtension(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
					nullDescriptor = true;
				}

				// DCLF's GPU-driven draws need device-generated commands. BasicRHI reads them from the create info.
				bool generated = false;
				if (has(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME)) {
					VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT supportedGenerated{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT };
					VkPhysicalDeviceFeatures2 query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
					query.pNext = &supportedGenerated;
					getFeatures2(physical, &query);
					if (supportedGenerated.deviceGeneratedCommands) {
						auto* chainGenerated = reinterpret_cast<VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT*>(
							FindStruct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT));
						if (!chainGenerated) {
							self->generatedCommands.pNext = const_cast<void*>(info->pNext);
							info->pNext = &self->generatedCommands;
							chainGenerated = &self->generatedCommands;
						}
						chainGenerated->deviceGeneratedCommands = VK_TRUE;
						chainGenerated->dynamicGeneratedPipelineLayout |= supportedGenerated.dynamicGeneratedPipelineLayout;
						addExtension(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
						generated = true;
					}
				}
				self->summary = chain12 ? fmt::format("every supported core and Vulkan 1.1-1.3 feature, null descriptors {}, device-generated commands {}, memory model device scope {}, separate depth/stencil layouts {}, indirect count {}",
											  nullDescriptor, generated, static_cast<bool>(chain12->vulkanMemoryModelDeviceScope), static_cast<bool>(chain12->separateDepthStencilLayouts),
											  static_cast<bool>(chain12->drawIndirectCount)) :
				                          fmt::format("every supported core feature, null descriptors {}; BasicRHI's chain has no Vulkan 1.2 struct, so its own 1.2 features", nullDescriptor);
				return true;
			}
		};

		/**
		 * @brief Initializes Streamline for the native backend before the device exists, so the device can come up through it.
		 * False when Streamline cannot run, and the upscalers fall back to TAA.
		 */
		bool NativeStreamline(rhi::DeviceCreateInfo::StreamlineHost& a_host, uint64_t a_adapterLuid)
		{
			if (!globals::features::upscaling.loaded)
				return false;
			auto* streamline = Streamline::GetSingleton();
			if (!streamline->InitializeForNative(a_adapterLuid) || !streamline->InterposerModule())
				return false;
			a_host.interposer = streamline->InterposerModule();
			a_host.features = streamline->LoadedFeatures().data();
			a_host.featureCount = static_cast<uint32_t>(streamline->LoadedFeatures().size());
			a_host.pluginDirectory = streamline->PluginDirectory().c_str();
			return true;
		}

		uint64_t g_adapterLuid = 0;

		bool CreateBasicRhiDevice(IDXGIAdapter* a_adapter, std::shared_ptr<rhi::DevicePtr>& a_out, std::string& a_reason)
		{
			static rhi::DeviceCreateInfo::StreamlineHost streamlineHost{};
			if (DXGI_ADAPTER_DESC described{}; a_adapter && SUCCEEDED(a_adapter->GetDesc(&described)))
				g_adapterLuid = (static_cast<uint64_t>(static_cast<uint32_t>(described.AdapterLuid.HighPart)) << 32) | described.AdapterLuid.LowPart;
			// After the adapter's LUID: DLSS-G loads only when this adapter has the hardware.
			const bool streamline = NativeStreamline(streamlineHost, g_adapterLuid);
			// D3D12 backend on the same adapter. Code after this is shared except where it checks OnD3D12().
			if (::RenderGraph::Backend::Requested() == rhi::Backend::D3D12) {
				D3D12DeviceOptions options{};
				// SARP_D3D12_GPU_VALIDATION=1 adds GPU-based validation.
				options.debug = Switches::On("CS_ORG_NATIVE_VALIDATION");
				// Crash markers (DRED) follow DXVK's hang mode.
				options.crashMarkers = Switches::Equals("DXVK_DEBUG", "hang");
				options.framesInFlight = RenderGraphRuntime::kGameFramesInFlight;
				DXGI_ADAPTER_DESC adapter{};
				if (a_adapter && SUCCEEDED(a_adapter->GetDesc(&adapter)))
					options.adapterLuid = (static_cast<uint64_t>(static_cast<uint32_t>(adapter.AdapterLuid.HighPart)) << 32) | adapter.AdapterLuid.LowPart;
				options.streamlineHost = streamline ? &streamlineHost : nullptr;
				if (!Engine::CreateD3D12Device(options, a_out, a_reason))
					return false;
				::RenderGraph::Backend::SetActive(rhi::Backend::D3D12);
				Stage("device", fmt::format("BasicRHI D3D12 device on '{}' (debug layer {}, GPU-based validation {}, DRED {})",
									stl::utf16_to_utf8(adapter.Description).value_or("?"), options.debug,
									options.debug && Switches::On("SARP_D3D12_GPU_VALIDATION"), options.debug || options.crashMarkers));
				return true;
			}
			FeatureHook hook;
			rhi::DeviceCreateInfo::VulkanCreateHooks hooks{ &hook, nullptr, &FeatureHook::OnDevice };
			rhi::DeviceCreateInfo ci{};
			ci.backend = rhi::Backend::Vulkan;
			ci.framesInFlight = RenderGraphRuntime::kGameFramesInFlight;
			ci.enableDebug = Switches::On("CS_ORG_NATIVE_VALIDATION");
			// Crash markers and detailed checkpoints follow DXVK's hang mode.
			ci.enableCrashMarkers = Switches::Equals("DXVK_DEBUG", "hang");
			// With crash markers on, every debug label sets an Aftermath checkpoint, so dumps name the pass.
			if (Aftermath::IsEnabled()) {
				ci.vulkanRegisterCheckpoint = &Aftermath::RegisterOrgCheckpoint;
				ci.vulkanDetailedCheckpoints = ci.enableCrashMarkers;
			}
			// The graph binds through descriptor buffers alone, as on DXVK's device.
			ci.vulkanBindingModels = rhi::vulkan::BindingModel_DescriptorBuffer;
			ci.vulkanCreateHooks = &hooks;
			// Use the adapter the game's INI picked, at RendererData +0x00.
			DXGI_ADAPTER_DESC adapter{};
			if (a_adapter && SUCCEEDED(a_adapter->GetDesc(&adapter))) {
				ci.adapterLuid = (static_cast<uint64_t>(static_cast<uint32_t>(adapter.AdapterLuid.HighPart)) << 32) | adapter.AdapterLuid.LowPart;
				ci.requireAdapterLuid = true;
			}
			a_out = std::make_shared<rhi::DevicePtr>();
			ci.streamlineHost = streamline ? &streamlineHost : nullptr;
			if (rhi::CreateVulkanDevice(ci, *a_out, false) != rhi::Result::Ok || !*a_out) {
				a_reason = ci.requireAdapterLuid ? "rhi::CreateVulkanDevice failed on the game's adapter (see BasicRHI's log lines above)" :
				                                   "rhi::CreateVulkanDevice failed (see BasicRHI's log lines above)";
				a_out.reset();
				return false;
			}
			::RenderGraph::Backend::SetActive(rhi::Backend::Vulkan);
			const bool debugUtils = ci.enableDebug;
			Stage("device", fmt::format("BasicRHI Vulkan device on '{}' ({}; validation {}, crash markers {})", stl::utf16_to_utf8(adapter.Description).value_or("?"),
								hook.summary.empty() ? std::string("BasicRHI's own feature set") : hook.summary, debugUtils, ci.enableCrashMarkers));
			return true;
		}

		// ---- Native resources ------------------------------------------------------------------------------------------------

		/** @brief Where one D3D11 subresource lies in a CPU-side copy of a texture. */
		struct SubresourceLayout
		{
			uint64_t offset = 0;
			uint32_t rowPitch = 0;
			uint32_t depthPitch = 0;
			// Depth-stencil staging keeps the GPU's separate planes at these offsets. The CPU sees D3D11's interleaved texels at offset.
			uint64_t depthOffset = 0, stencilOffset = 0;
			uint32_t depthRowPitch = 0, stencilRowPitch = 0;
		};

		/** @brief A texture's shape. Rows and row bytes count blocks for compressed formats. */
		struct Shape
		{
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
			rhi::Format format = rhi::Format::Unknown;
			uint32_t width = 1, height = 1, depth = 1, mips = 1, layers = 1;
			bool blocks = false;
			uint32_t blockBytes = 4;
			// The aspect a buffer copy takes is depth for depth-stencil formats.
			VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
			VkImageAspectFlags copyAspect = VK_IMAGE_ASPECT_COLOR_BIT;
			// Differs from format only for a D3D12 typeless depth image, whose views take its depth format.
			rhi::Format viewFormat = rhi::Format::Unknown;

			uint32_t Width(uint32_t a_mip) const { return (std::max)(width >> a_mip, 1u); }
			uint32_t Height(uint32_t a_mip) const { return (std::max)(height >> a_mip, 1u); }
			uint32_t Depth(uint32_t a_mip) const { return (std::max)(depth >> a_mip, 1u); }
			uint32_t Subresources() const { return mips * layers; }
			uint32_t BlockSize() const { return blocks ? 4u : 1u; }
			uint32_t Rows(uint32_t a_height) const { return (a_height + BlockSize() - 1) / BlockSize(); }
			uint32_t RowBytes(uint32_t a_width) const { return (a_width + BlockSize() - 1) / BlockSize() * blockBytes; }
		};

		struct NativeResource : std::enable_shared_from_this<NativeResource>
		{
			enum class Kind : uint8_t
			{
				Buffer,
				Image,    // DEFAULT, IMMUTABLE or DYNAMIC
				Staging,  // a buffer laid out by subresource
				Shell,    // no storage, read through its window
			};
			/**
			 * @brief Who keeps an image's state. Host images are imported by the graph. Tracked images are ORG textures.
			 * Exempt images were made with contents, are only read, and rest in the shader-read state untracked.
			 */
			enum class Ownership : uint8_t
			{
				Host,
				Tracked,
				Exempt,
			};
			Kind kind = Kind::Buffer;
			Ownership ownership = Ownership::Host;
			// Declared before resource so the import below is destroyed first.
			std::shared_ptr<org::PixelBuffer> graphOwned;
			rhi::ResourcePtr resource;
			std::string name;
			VkBuffer vkBuffer = VK_NULL_HANDLE;
			VkDeviceSize offset = 0;  // where the buffer starts in vkBuffer (a packed D3D12 buffer shares its resource)
			VkDeviceAddress address = 0;  // 0 when the buffer has none
			VkBufferUsageFlags usage = 0;
			uint64_t size = 0;
			bool hostVisible = false;
			bool readback = false;
			// Set once the graph imports the buffer. The CPU must then wait for graph work before touching its memory.
			std::atomic<bool> graphShared{ false };
			D3D11_BUFFER_DESC bufferDesc{};
			DxvkOrgInteropImageInfo image{};
			Shape shape;
			std::vector<SubresourceLayout> layout;
			// A DYNAMIC texture's CPU side. Map writes it and Unmap copies it into the image.
			std::shared_ptr<NativeResource> upload;
			// Lets Map wait only for the last immediate work that used this resource.
			std::atomic<uint64_t> lastUse{ 0 };
			// Whether a GPU copy wrote the depth-stencil planes since Map last rebuilt the CPU texels.
			std::atomic<bool> planesNewer{ false };
			bool planeFloat = false;
			// A UAV twin that GenerateMips builds the chain in, made on first use.
			std::shared_ptr<NativeResource> mipScratch;
			std::mutex windowLock;
			winrt::com_ptr<ID3D11ShaderResourceView> window;
			bool streamEnabled = false;
			// Read by the undeclared-access check.
			UINT bindFlags = 0;
			D3D11_USAGE d3dUsage = D3D11_USAGE_DEFAULT;
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
			bool initialData = false;
			std::atomic<bool> written{ false };
			const uint64_t serial;
			// Exempt texture state. The immediate context moves it out of shader-read and back around its own work.
			std::atomic<bool> restsShaderRead{ false };
			uint64_t readyValue = 0;
			std::atomic<bool> promoted{ false };
			std::mutex graphLock;
			std::shared_ptr<org::ExternalTextureResource> exemptResource;

			NativeResource();
			~NativeResource();
			NativeResource(const NativeResource&) = delete;
			NativeResource& operator=(const NativeResource&) = delete;

			/**
			 * @brief A graph-owned texture's backing moved (an ORG relocation). Under graphLock, points the record at the new image:
			 * a_next is the context's import of it and a_image its description. Returns the old import, which the caller releases once
			 * no work can use it.
			 */
			rhi::ResourcePtr Rebind(rhi::ResourcePtr a_next, const DxvkOrgInteropImageInfo& a_image);
		};

		rhi::ResourcePtr NativeResource::Rebind(rhi::ResourcePtr a_next, const DxvkOrgInteropImageInfo& a_image)
		{
			std::lock_guard guard(graphLock);
			image = a_image;
			return std::exchange(resource, std::move(a_next));
		}

		uint64_t NextSerial()
		{
			static std::atomic<uint64_t> next{ 0 };
			return ++next;
		}

		/** @brief The live NativeResource records, so DescribeResource can tell them from other owners. */
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

		NativeResource::NativeResource() :
			serial(NextSerial())
		{
			auto& registry = GetRegistry();
			std::lock_guard guard(registry.lock);
			registry.live.insert(this);
		}

		NativeResource::~NativeResource()
		{
			// The exempt import does not own the image, so no lookup may find either graph resource after this record goes.
			::RenderGraph::ImageRegistry::Retire(exemptResource.get());
			if (ownership == Ownership::Tracked)
				::RenderGraph::ImageRegistry::Retire(graphOwned.get());
			auto& registry = GetRegistry();
			std::lock_guard guard(registry.lock);
			registry.live.erase(this);
		}

		std::shared_ptr<org::Resource> LedgerResourceOf(NativeResource& a_record);

		/** @brief Registers a tracked texture's ORG texture as its image's graph resource. An exempt one registers on its first lookup (ImmutableTextureOf). */
		void NoteOwnedImage(NativeResource& a_record)
		{
			if (a_record.ownership != NativeResource::Ownership::Tracked || !a_record.graphOwned)
				return;
			::RenderGraph::ImageRegistry::Register(reinterpret_cast<std::uint64_t>(a_record.image.image),
				{ a_record.graphOwned, nullptr, nullptr, "native", fmt::format("cs.native.texture.{}", a_record.serial), ::RenderGraph::ImageRegistry::Kind::Owned });
		}

		/** @brief The native image of a BasicRHI texture as the record keeps it: the VkImage, or on D3D12 the ID3D12Resource. Null on failure. */
		VkImage NativeImageOf(const rhi::Resource& a_resource)
		{
			if (OnD3D12())
				return reinterpret_cast<VkImage>(D3D12ResourceOf(a_resource));
			rhi::VulkanResourceInfo info{};
			if (!rhi::vulkan::get_resource_info(a_resource, info) || !info.resource)
				return VK_NULL_HANDLE;
			return rhi::vulkan::from_native_void<VkImage>(info.resource);
		}

		/** @brief Starts a debug label so crash markers name the work. A no-op where unsupported. */
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

		/**
		 * @brief Native mode's one way to submit work outside the graphs: the immediate context's operations, DCLF's commands outside
		 * its epochs and the present's copy, one at a time in call order, all through ORG's external client (made with the device).
		 * Each slot has its own descriptors and keeps what its work reads, reused once the context's timeline passes the slot's work.
		 */
		class Immediate
		{
		public:
			/** @brief One UAV per clear in an operation, since D3D12 reads a UAV clear's descriptor at execution. */
			static constexpr uint32_t kUavsPerSlot = 16;

			/** @brief A slot's descriptors. RTV and DSV are read at record time, so one each is enough. */
			struct Descriptors
			{
				rhi::DescriptorSlot rtv, dsv, uavShaderVisible, uavCpu;
			};

			/** @brief What a submission uses and orders against. */
			struct Work
			{
				std::span<const ::RenderGraph::ExternalWork::Access> accesses;  // graph resources, moved through the ledger
				std::span<NativeResource* const> untracked;  // records outside the ledgers; exempt images among them leave shader-read for the work
				std::span<NativeResource* const> used;       // every record the work uses, so a CPU access waits only for it
				std::span<const rhi::TimelinePoint> waits;
				std::span<const rhi::TimelinePoint> signals;
				bool bindHeaps = true;  // false for commands that start with nothing bound
			};

			bool Init(rhi::Device a_device)
			{
				const auto heapOf = [&](rhi::DescriptorHeapType a_type, uint32_t a_capacity, bool a_shaderVisible, const char* a_name, rhi::DescriptorHeapPtr& a_out) {
					return a_device.CreateDescriptorHeap(rhi::DescriptorHeapDesc{ a_type, a_capacity, a_shaderVisible, a_name }, a_out) == rhi::Result::Ok && a_out;
				};
				const auto heap = [&](rhi::DescriptorHeapType a_type, bool a_shaderVisible, const char* a_name, rhi::DescriptorHeapPtr& a_out) {
					return heapOf(a_type, kSlots, a_shaderVisible, a_name, a_out);
				};
				const bool ok = heap(rhi::DescriptorHeapType::RTV, false, "CS native immediate RTVs", rtvHeap) &&
				                heap(rhi::DescriptorHeapType::DSV, false, "CS native immediate DSVs", dsvHeap) &&
				                heapOf(rhi::DescriptorHeapType::CbvSrvUav, kSlots * kUavsPerSlot, true, "CS native immediate UAVs", uavHeap) &&
				                heapOf(rhi::DescriptorHeapType::CbvSrvUav, kSlots * kUavsPerSlot, false, "CS native immediate UAVs (CPU)", uavCpuHeap);
				return ok && a_device.CreateTimeline(timeline, 0, "CS native immediate context") == rhi::Result::Ok && timeline;
			}

			/**
			 * @brief Records and submits a_record through ORG's external client. Returns the work's value on the context's timeline, or 0
			 * only after the client's shutdown at the window's close.
			 */
			uint64_t Run(const std::string& a_label, const Work& a_work, const std::function<void(rhi::CommandList&, const Descriptors&)>& a_record,
				std::vector<std::shared_ptr<const void>> a_keep)
			{
				// Commit stages before taking this lock. Committing can run CS code that re-enters Run and would deadlock.
				::FrameAnchors::BeforeGpuWork("native context");
				// Drain pending epochs before the lock too, since a pass recorded there may use the context.
				RenderGraphRuntime::Get().DrainSubmissions(a_label.c_str());
				std::lock_guard guard(lock);
				const uint32_t index = AcquireSlotLocked();
				auto& slot = slots[index];
				const Descriptors descriptors = DescriptorsOf(index);
				// The client submits on this queue too. The lock keeps this context's signals in order.
				std::vector<rhi::TimelinePoint> signals(a_work.signals.begin(), a_work.signals.end());
				signals.push_back({ timeline->GetHandle(), value + 1 });
				const bool submitted = ::RenderGraph::ExternalWork::Submit(
					a_label, a_work.accesses,
					[&](rhi::CommandList& a_list) {
						BeginLabel(a_list, a_label);
						if (a_work.bindHeaps)
							BindHeaps(a_list);
						LeaveShaderRead(a_list, a_work.untracked, false);
						a_record(a_list, descriptors);
						LeaveShaderRead(a_list, a_work.untracked, true);
						EndLabel(a_list);
					},
					{}, signals, a_work.waits);
				if (!submitted)
					return 0;
				slot.value = ++value;
				slot.keep = std::move(a_keep);
				for (auto* used : a_work.used)
					if (used)
						used->lastUse.store(value, std::memory_order_release);
				if (!a_work.accesses.empty())
					::RenderGraph::RawAccess::Count(::RenderGraph::RawAccess::Kind::Routed);
				return value;
			}

			/** @brief Work that records its own barriers and touches no graph-tracked image. */
			uint64_t RunUntracked(const std::string& a_label, const std::function<void(rhi::CommandList&, const Descriptors&)>& a_record,
				std::vector<std::shared_ptr<const void>> a_keep, std::span<NativeResource* const> a_used)
			{
				Work work;
				work.untracked = a_used;
				work.used = a_used;
				return Run(a_label, work, a_record, std::move(a_keep));
			}

			bool Done(uint64_t a_value) { return !a_value || timeline->GetCompletedValue() >= a_value; }

			/**
			 * @brief Releases what finished work kept. A slot is reused only after kSlots more operations, and native mode runs few
			 * once a scene settles, so without this a slot would keep a replaced version alive until the next load or longer.
			 */
			void ReleaseDone()
			{
				std::lock_guard guard(lock);
				ReleaseDoneLocked();
			}

			bool Wait(uint64_t a_value) { return Done(a_value) || timeline->HostWait(a_value) == rhi::Result::Ok; }

		private:
			// Context operations and DCLF's commands share the slots, a few dozen a frame, so a reuse never waits on recent work.
			static constexpr uint32_t kSlots = 64;

			struct Slot
			{
				uint64_t value = 0;
				std::vector<std::shared_ptr<const void>> keep;
			};

			/** @brief Clears the keep list of every slot whose work the GPU finished. Call with the lock. */
			void ReleaseDoneLocked()
			{
				if (!timeline)
					return;
				const uint64_t completed = timeline->GetCompletedValue();
				for (auto& slot : slots)
					if (slot.value && slot.value <= completed && !slot.keep.empty())
						slot.keep.clear();
			}

			/** @brief The next slot once the GPU is done with it. Releases what its last work kept. Call with the lock. */
			uint32_t AcquireSlotLocked()
			{
				ReleaseDoneLocked();
				const uint32_t index = static_cast<uint32_t>(next++ % kSlots);
				auto& slot = slots[index];
				if (slot.value && timeline->GetCompletedValue() < slot.value)
					(void)timeline->HostWait(slot.value);
				slot.keep.clear();
				return index;
			}

			Descriptors DescriptorsOf(uint32_t a_index) const
			{
				Descriptors descriptors{};
				if (rtvHeap) {
					descriptors.rtv = { rtvHeap->GetHandle(), a_index };
					descriptors.dsv = { dsvHeap->GetHandle(), a_index };
					descriptors.uavShaderVisible = { uavHeap->GetHandle(), a_index * kUavsPerSlot };
					descriptors.uavCpu = { uavCpuHeap->GetHandle(), a_index * kUavsPerSlot };
				}
				return descriptors;
			}

			void BindHeaps(rhi::CommandList& a_list) const
			{
				if (uavHeap)
					a_list.SetDescriptorHeaps(uavHeap->GetHandle(), std::nullopt);
			}

			/** @brief Moves exempt images between shader-read and Common. Images still uploading are left alone. */
			static void LeaveShaderRead(rhi::CommandList& a_list, std::span<NativeResource* const> a_used, bool a_back)
			{
				std::array<rhi::TextureBarrier, 4> barriers{};
				uint32_t count = 0;
				for (auto* used : a_used) {
					if (!used || used->kind != NativeResource::Kind::Image || !used->resource || !used->restsShaderRead.load(std::memory_order_acquire) ||
						count == barriers.size())
						continue;
					auto& barrier = barriers[count++];
					barrier.texture = used->resource->GetHandle();
					barrier.range = { 0, used->shape.mips, 0, used->shape.layers, 0, 1 };
					barrier.beforeSync = barrier.afterSync = rhi::ResourceSyncState::All;
					barrier.beforeAccess = a_back ? rhi::ResourceAccessType::Common : rhi::ResourceAccessType::ShaderResource;
					barrier.afterAccess = a_back ? rhi::ResourceAccessType::ShaderResource : rhi::ResourceAccessType::Common;
					barrier.beforeLayout = a_back ? rhi::ResourceLayout::Common : rhi::ResourceLayout::ShaderResource;
					barrier.afterLayout = a_back ? rhi::ResourceLayout::ShaderResource : rhi::ResourceLayout::Common;
				}
				if (count)
					a_list.Barriers({ { barriers.data(), count }, {}, {} });
			}

			std::mutex lock;
			rhi::DescriptorHeapPtr rtvHeap, dsvHeap, uavHeap, uavCpuHeap;
			std::array<Slot, kSlots> slots;
			uint64_t next = 0;
			rhi::TimelinePtr timeline;
			uint64_t value = 0;
		};

		void LogOnce(std::atomic<bool>& a_logged, std::string_view a_name, std::string_view a_why)
		{
			if (!a_logged.exchange(true, std::memory_order_relaxed))
				logger::error("[ORG native] {}: {}; resources like it are not made (logged once)", a_name, a_why);
		}

		DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R24G8_TYPELESS:
				return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			case DXGI_FORMAT_R32G8X24_TYPELESS:
				return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			case DXGI_FORMAT_R32_TYPELESS:
				return DXGI_FORMAT_R32_FLOAT;
			case DXGI_FORMAT_R16_TYPELESS:
				return DXGI_FORMAT_R16_UNORM;
			default:
				return a_format;
			}
		}

		struct TextureRequest;

		/** @brief Logs a refused texture with its reason. Only the first 32 are logged. */
		void LogRefused(const TextureRequest& a_request, std::string_view a_why);

		/** @brief The format a typeless texture is made in. Depth if a depth-stencil binding implies it, else UNORM. */
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

		/** @brief A BasicRHI swizzle, in D3D12's 3-bit-per-channel encoding, as Vulkan components. */
		VkComponentMapping ComponentsOf(rhi::ComponentMapping a_mapping)
		{
			if (!a_mapping)
				return { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
			const auto channel = [&](uint32_t a_index) {
				switch ((a_mapping >> (a_index * 3)) & 7u) {
				case 0:
					return VK_COMPONENT_SWIZZLE_R;
				case 1:
					return VK_COMPONENT_SWIZZLE_G;
				case 2:
					return VK_COMPONENT_SWIZZLE_B;
				case 3:
					return VK_COMPONENT_SWIZZLE_A;
				case 4:
					return VK_COMPONENT_SWIZZLE_ZERO;
				default:
					return VK_COMPONENT_SWIZZLE_ONE;
				}
			};
			return { channel(0), channel(1), channel(2), channel(3) };
		}

		/** @brief D3D11 depth-stencil texel size. 4 for D24S8, 8 for D32S8X24, 0 without stencil. */
		uint32_t StencilTexelBytes(const Shape& a_shape)
		{
			return (a_shape.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? a_shape.blockBytes : 0u;
		}

		/**
		 * @brief Converts one subresource between D3D11's interleaved depth-stencil texels and the GPU's separate planes.
		 * Depth is 4 bytes a texel, float or 24-bit UNORM. Stencil is 1 byte.
		 */
		void RepackDepthStencil(bool a_unpack, uint32_t a_texel, bool a_float, std::byte* a_main, uint32_t a_mainPitch, std::byte* a_depth, uint32_t a_depthPitch,
			std::byte* a_stencil, uint32_t a_stencilPitch, uint32_t a_width, uint32_t a_rows)
		{
			constexpr float kUnorm24 = 16777215.0f;
			for (uint32_t row = 0; row < a_rows; ++row) {
				std::byte* main = a_main + uint64_t(row) * a_mainPitch;
				std::byte* depth = a_depth + uint64_t(row) * a_depthPitch;
				std::byte* stencil = a_stencil + uint64_t(row) * a_stencilPitch;
				for (uint32_t x = 0; x < a_width; ++x) {
					std::byte* texel = main + uint64_t(x) * a_texel;
					uint32_t plane = 0;
					if (a_unpack) {
						std::memcpy(&plane, depth + uint64_t(x) * 4, 4);
						if (a_texel == 4) {
							uint32_t unorm = plane & 0xFFFFFFu;
							if (a_float) {
								float value = 0.0f;
								std::memcpy(&value, &plane, 4);
								unorm = static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * kUnorm24 + 0.5f);
							}
							const uint32_t packed = unorm | (uint32_t(std::to_integer<uint8_t>(stencil[x])) << 24);
							std::memcpy(texel, &packed, 4);
						} else {
							std::memcpy(texel, &plane, 4);
							texel[4] = stencil[x];
							std::memset(texel + 5, 0, 3);
						}
					} else {
						if (a_texel == 4) {
							uint32_t packed = 0;
							std::memcpy(&packed, texel, 4);
							plane = packed & 0xFFFFFFu;
							if (a_float) {
								const float value = static_cast<float>(plane) / kUnorm24;
								std::memcpy(&plane, &value, 4);
							}
							stencil[x] = static_cast<std::byte>(packed >> 24);
						} else {
							std::memcpy(&plane, texel, 4);
							stencil[x] = texel[4];
						}
						std::memcpy(depth + uint64_t(x) * 4, &plane, 4);
					}
				}
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

		struct TextureRequest
		{
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
			uint32_t width = 1, height = 1, depthOrArraySize = 1, mipLevels = 1, sampleCount = 1;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
			UINT bindFlags = 0, cpuAccess = 0, miscFlags = 0;
			const D3D11_SUBRESOURCE_DATA* data = nullptr;
			std::string name;
			// Graph-owned before RenderGraphRuntime::Initialize too. Only the back buffer asks: the ORG runtime exists from stage 3.
			bool beforeGraph = false;
		};

		void NoteRequest(NativeResource& a_record, const TextureRequest& a_request)
		{
			a_record.bindFlags = a_request.bindFlags;
			a_record.d3dUsage = a_request.usage;
			a_record.dimension = a_request.dimension;
			a_record.initialData = a_request.data && a_request.data->pSysMem;
		}

		/**
		 * @brief Whether a texture is exempt from tracking. It must be shader-only, made with contents, and without CPU access.
		 * Only one such texture is written later, the loading screen's, which NoteWrite promotes.
		 */
		bool IsExemptRequest(const TextureRequest& a_request)
		{
			return (a_request.bindFlags & ~UINT(D3D11_BIND_SHADER_RESOURCE)) == 0 && (a_request.bindFlags & D3D11_BIND_SHADER_RESOURCE) &&
			       a_request.usage != D3D11_USAGE_DYNAMIC && a_request.usage != D3D11_USAGE_STAGING && !a_request.cpuAccess && a_request.sampleCount <= 1 &&
			       a_request.data && a_request.data->pSysMem;
		}

		/** @brief Logs once per reason why a tracked texture stays host-made. */
		void LogHostMade(std::string_view a_why, std::string_view a_name)
		{
			static std::mutex lock;
			static std::unordered_set<std::string> logged;
			std::lock_guard guard(lock);
			if (logged.emplace(a_why).second)
				logger::info("[ORG native] {}: kept host-made, not graph-owned: {} (logged once per reason)", a_name, a_why);
		}

		void LogRefused(const TextureRequest& a_request, std::string_view a_why)
		{
			static std::atomic<uint32_t> logged{ 0 };
			const uint32_t count = logged.fetch_add(1, std::memory_order_relaxed);
			if (count >= 32) {
				if (count == 32)
					logger::error("[ORG native] more textures were refused; only the first 32 are logged");
				return;
			}
			logger::error("[ORG native] {} refused: {}. Request: dimension {}, {}x{}, depth or array {}, mips {}, samples {}, DXGI format {}, usage {}, bind {:#x}, "
						  "CPU access {:#x}, misc {:#x}{}",
				a_request.name, a_why, static_cast<int>(a_request.dimension), a_request.width, a_request.height, a_request.depthOrArraySize, a_request.mipLevels,
				a_request.sampleCount, static_cast<int>(a_request.format), static_cast<int>(a_request.usage), a_request.bindFlags, a_request.cpuAccess,
				a_request.miscFlags, a_request.data ? ", with data" : "");
		}

		// ---- Queue access and commands outside the graph ---------------------------------------------------------------------

		/** @brief Guards every access to the device's queues. BasicRHI takes it through its sharing hooks. */
		std::mutex g_queueLock;

		void LockQueue(void*, VkQueue) { g_queueLock.lock(); }
		void UnlockQueue(void*, VkQueue) { g_queueLock.unlock(); }

		uint64_t CopyRhi(const std::string& a_label, const std::shared_ptr<NativeResource>& a_destination, const std::shared_ptr<NativeResource>& a_source, bool a_all,
			uint32_t a_destinationSub, uint32_t a_x, uint32_t a_y, uint32_t a_z, uint32_t a_sourceSub, const D3D11_BOX* a_box, bool& a_recorded, bool* a_routed);

		/** @brief The swap chain's presentation. */
		struct PresentationState
		{
			std::uint64_t serial = 0;     // the request the swap chain was last made for. 0 is the swap chain made at startup
			std::uint32_t format = 0;     // the swap chain images' rhi::Format
			bool hdr10 = false;
			bool presentationLayer = false;
			bool vsync = false;  // with a presentation layer, the vsync the swap chain was made for. Its presents keep it
		};

		struct NativeState;
		NativeState* g_device = nullptr;

		struct NativeState
		{
			static constexpr uint32_t kPresentSlots = 3;

			std::shared_ptr<rhi::DevicePtr> device;
			rhi::Queue graphics;
			Immediate immediate;
			// GenerateMips' compute programs for 2D and for arrays and cubes. Loaded once the graph is up.
			std::shared_ptr<const ComputeProgram> mips2D, mipsArray;
			// ORG's runtime, made with the device for the external client. RenderGraphRuntime::Initialize takes it (TakeOrgRuntime).
			std::shared_ptr<org::Runtime> orgRuntime;

			rhi::SwapchainPtr swapchain;
			HWND window = nullptr;
			DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
			rhi::Format swapchainFormat = rhi::Format::R8G8B8A8_UNorm;
			// What the engine asked for last (ResizeBuffers). Render thread, under presentLock.
			uint32_t bufferCount = 2;
			uint32_t width = 0, height = 0;
			// The swap chain's own size and image count. The submission thread changes them after the presents queued before the
			// resize (ResizeSwapchain), under presentLock.
			uint32_t swapBufferCount = 2;
			uint32_t swapWidth = 0, swapHeight = 0;
			// The latest swap chain resize asked for. An older one still queued does nothing.
			std::atomic<uint64_t> resizesAsked = 0;
			uint32_t frameLatency = 2;
			// What GetBuffer answers: the startup version's handle, then kFRAMEBUFFER's field once the set adopted the slot.
			winrt::com_ptr<ID3D11Texture2D> backBuffer;

			// Each present slot's copy value on the present timeline. A slot is reused once the timeline passes it.
			std::array<uint64_t, kPresentSlots> slotValues{};
			rhi::TimelinePtr presentTimeline;
			uint64_t presentValue = 0;
			uint64_t presents = 0;
			bool resizeRequested = false;
			/**
			 * @brief kFRAMEBUFFER's slot, made with the swap chain. The present copies its current version. Native makes its versions
			 * until the engine target set adopts it at Engine::Setup (AdoptBackBufferSlot); the set makes them after that.
			 */
			std::shared_ptr<org::VersionedTexture> backBufferSlot;
			// Whether the set adopted the slot. GetBuffer then answers the set's field, and a resize makes no version here. Under presentLock.
			bool setAdopted = false;
			/** @brief What a present copies: the slot's current version when the present was asked for. */
			struct PresentSource
			{
				std::shared_ptr<org::Resource> graph;
				uint32_t width = 0, height = 0;
			};
			/**
			 * @brief The "native present copy" GPU range: a timestamp pair around each present's copy, resolved into a readback buffer
			 * and read when its slot comes round again. On while GpuTimingSource collects (CS_GPU_EVENT_TIMERS).
			 */
			struct PresentTimer
			{
				rhi::QueryPoolPtr pool;
				rhi::ResourcePtr readback;
				uint64_t stride = 8;
				double msPerTick = 0.0;
				std::array<bool, kPresentSlots> pending{};
			} presentTimer;
			std::atomic<bool> lost = false;
			std::mutex presentLock;
			// The presentation (NativePresenter::RequestConfig). Requests come from any thread, and NativePresenter counts them. The
			// submission thread replaces the swap chain under presentLock and publishes what it made under presentationLock, which
			// never waits on anything.
			std::mutex presentationLock;
			PresentationState adoptedPresentation{};
			std::function<void()> leaving;  // under presentationLock
			// With a presentation layer, the vsync the swap chain was made for. Submission thread, under presentLock.
			std::optional<bool> layerVsync;
			PresentationState framePresentation{};  // render owner
			// Whether the output took HDR10 when the swap chain was last made, for a presentation taken before it is replaced.
			std::atomic<bool> hdr10Supported = false;
			// The render thread queues a present only once the one before has finished.
			std::atomic<uint64_t> queuedPresents = 0, donePresents = 0;

			// Each present drains BasicRHI's queued validation messages into the log.
			bool validation = false;

			winrt::com_ptr<IDXGIAdapter> adapter;
			ID3D11Device* deviceHandle = nullptr;  // the engine holds it for the process
			winrt::com_ptr<IDXGIDevice1> dxgiDevice;

			std::atomic<bool> loggedCpuAccess = false, loggedFormat = false, loggedCreate = false, loggedUpload = false, loggedMultisample = false;

			rhi::Device Rhi() { return device->Get(); }

			/**
			 * @brief Makes a texture. Exempt ones rest in shader-read. Tracked ones become persistent ORG textures once the graph is up.
			 * Anything else is a BasicRHI image in Common.
			 */
			HRESULT MakeTexture(const TextureRequest& a_request, std::shared_ptr<NativeResource>& a_out)
			{
				if (a_request.usage == D3D11_USAGE_STAGING)
					return MakeStaging(a_request, false, a_out);
				if (a_request.sampleCount > 1) {
					LogOnce(loggedMultisample, a_request.name, "native multisampled textures are not built");
					return E_NOTIMPL;
				}
				const DXGI_FORMAT dxgiFormat = ResolveTypeless(a_request.format, a_request.bindFlags);
				// D3D12 keeps typeless depth typeless as D3D11 does, since a typed D24 or D32 resource cannot have an SRV there.
				// Vulkan resolves it to the depth format because its views can reinterpret the aspect.
				const bool typelessDepth = OnD3D12() && (a_request.bindFlags & D3D11_BIND_DEPTH_STENCIL) && dxgiFormat != a_request.format;
				const rhi::Format format = rhi::helpers::ToRHI(typelessDepth ? a_request.format : dxgiFormat);
				const rhi::Format viewFormat = rhi::helpers::ToRHI(dxgiFormat);
				rhi::FormatSupport support{};
				std::string refused;
				if (typelessDepth) {
					rhi::FormatSupport depthSupport{}, srvSupport{};
					const rhi::Format srvFormat = rhi::helpers::ToRHI(DepthSrvFormat(a_request.format));
					if (format == rhi::Format::Unknown || viewFormat == rhi::Format::Unknown || Rhi().QueryFormatSupport(viewFormat, depthSupport) != rhi::Result::Ok ||
						!(depthSupport.usage & rhi::FU_DepthStencil))
						refused = fmt::format("its depth format {} cannot be a depth-stencil view", static_cast<int>(dxgiFormat));
					else if ((a_request.bindFlags & D3D11_BIND_SHADER_RESOURCE) &&
							 (srvFormat == rhi::Format::Unknown || Rhi().QueryFormatSupport(srvFormat, srvSupport) != rhi::Result::Ok || !(srvSupport.usage & rhi::FU_ShaderResource)))
						refused = fmt::format("its shader resource format {} cannot be read", static_cast<int>(DepthSrvFormat(a_request.format)));
					support = srvSupport;
				} else {
					uint32_t needed = 0;
					if (!(a_request.bindFlags & D3D11_BIND_DEPTH_STENCIL) || (a_request.bindFlags & D3D11_BIND_SHADER_RESOURCE))
						needed |= rhi::FU_ShaderResource;
					if (a_request.bindFlags & D3D11_BIND_UNORDERED_ACCESS)
						needed |= rhi::FU_UnorderedAccess;
					// D3D12 checks every binding, as its resource creation does.
					if (OnD3D12() && (a_request.bindFlags & D3D11_BIND_RENDER_TARGET))
						needed |= rhi::FU_RenderTarget;
					if (OnD3D12() && (a_request.bindFlags & D3D11_BIND_DEPTH_STENCIL))
						needed |= rhi::FU_DepthStencil;
					if (format == rhi::Format::Unknown)
						refused = fmt::format("DXGI format {} has no BasicRHI format", static_cast<int>(dxgiFormat));
					else if (Rhi().QueryFormatSupport(format, support) != rhi::Result::Ok || (support.usage & needed) != needed)
						refused = fmt::format("the device's support for DXGI format {} is {:#x}, and the bind flags need {:#x} (1 shader resource, 2 render target, "
											  "4 depth-stencil, 8 unordered access)",
							static_cast<int>(dxgiFormat), support.usage, needed);
				}
				if (!refused.empty()) {
					LogRefused(a_request, refused);
					return E_INVALIDARG;
				}

				const bool cube = (a_request.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
				const uint32_t depth = a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? a_request.depthOrArraySize : 1;
				const uint32_t layers = a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1 : a_request.depthOrArraySize;
				const uint32_t mips = a_request.mipLevels ? a_request.mipLevels : FullMipCount(a_request.width, a_request.height, depth);
				const bool exempt = IsExemptRequest(a_request);
				if (!exempt) {
					std::shared_ptr<NativeResource> owned;
					if (MakeGraphOwned(a_request, dxgiFormat, format, viewFormat, support, typelessDepth, cube, mips, layers, depth, owned)) {
						a_out = std::move(owned);
						return S_OK;
					}
				}

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
				NoteRequest(*record, a_request);
				if (Rhi().CreateCommittedResource(desc, record->resource) != rhi::Result::Ok || !record->resource) {
					LogRefused(a_request, fmt::format("BasicRHI could not create the image (resource format {}, flags {:#x})", static_cast<int>(format), flags));
					return E_OUTOFMEMORY;
				}

				// An exempt image rests in shader-read and any other in Common.
				if (!FillImage(*record, record->resource.Get(), a_request, dxgiFormat, format, support, mips, layers, depth,
						exempt ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL)) {
					LogRefused(a_request, "BasicRHI did not describe the image it made");
					return E_FAIL;
				}
				record->shape.viewFormat = viewFormat;

				if (exempt) {
					record->readyValue = InitializeShaderRead(*record, a_request, format, mips, layers, depth);
					if (!record->readyValue) {
						LogOnce(loggedUpload, a_request.name, "its contents could not be transferred");
						return E_FAIL;
					}
					record->ownership = NativeResource::Ownership::Exempt;
					record->restsShaderRead.store(true, std::memory_order_release);
					a_out = std::move(record);
					return S_OK;
				}
				if (!Initialize(record->resource->GetHandle(), a_request, format, mips, layers, depth)) {
					LogOnce(loggedUpload, a_request.name, "its initial layout or contents could not be written");
					return E_FAIL;
				}
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

			/**
			 * @brief Makes a tracked texture as a persistent, never-aliased ORG PixelBuffer. From then on its layout lives in the ledgers.
			 * False when ORG cannot make it as D3D11 asked, and the caller falls back to BasicRHI.
			 */
			bool MakeGraphOwned(const TextureRequest& a_request, DXGI_FORMAT a_dxgiFormat, rhi::Format a_format, rhi::Format a_viewFormat, const rhi::FormatSupport& a_support,
				bool a_typelessDepth, bool a_cube, uint32_t a_mips, uint32_t a_layers, uint32_t a_depth, std::shared_ptr<NativeResource>& a_out)
			{
				// The engine's startup textures stay host-made until the graph is up. The back buffer is graph-owned from stage 3.
				if (!a_request.beforeGraph && !RenderGraphRuntime::Get().IsActive())
					return false;
				const bool depthStencil = (a_request.bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
				const bool renderTarget = (a_request.bindFlags & D3D11_BIND_RENDER_TARGET) != 0;
				const bool unordered = (a_request.bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
				const bool volume = a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
				const char* kept = nullptr;
				if (a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
					kept = "a 1D texture";
				// GENERATE_MIPS only asks the context for mip generation, which works on any image.
				else if (a_request.miscFlags & ~UINT(D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_GENERATE_MIPS))
					kept = "misc flags other than a cube or mip generation";
				else if (a_support.srvSwizzle)
					kept = "a format BasicRHI stores as another (read through a swizzle)";
				else if (a_typelessDepth)
					kept = "a typeless D3D12 depth texture";
				else if (depthStencil && (volume || a_layers > 1 || renderTarget || unordered))
					kept = "a depth texture that is not one 2D slice without RTVs or UAVs";
				else if (unordered && rhi::helpers::IsSRGB(a_format))
					kept = "an sRGB texture with a UAV";
				if (kept) {
					LogHostMade(kept, a_request.name);
					return false;
				}

				org::TextureDescription desc{};
				desc.type = volume ? rhi::ResourceType::Texture3D : rhi::ResourceType::Texture2D;
				desc.format = a_format;
				desc.isCubemap = a_cube;
				desc.arraySize = a_cube ? (std::max)(a_layers / 6, 1u) : a_layers;
				desc.isArray = a_layers > (a_cube ? 6u : 1u);
				desc.depth = a_depth;
				desc.hasSRV = true;
				const rhi::Format depthSrv = depthStencil ? rhi::helpers::ToRHI(DepthSrvFormat(a_request.format)) : rhi::Format::Unknown;
				desc.srvFormat = depthStencil && depthSrv != rhi::Format::Unknown && depthSrv != a_format ? depthSrv : a_format;
				desc.hasUAV = unordered;
				desc.uavFormat = a_format;
				desc.hasNonShaderVisibleUAV = unordered;
				desc.hasRTV = renderTarget;
				desc.rtvFormat = a_format;
				desc.hasDSV = depthStencil;
				desc.dsvFormat = a_format;
				desc.channels = depthStencil ? 1 : 4;
				desc.allowAlias = false;
				// Slice-major, as a PixelBuffer reads them.
				for (uint32_t slice = 0; slice < a_layers; ++slice)
					for (uint32_t mip = 0; mip < a_mips; ++mip)
						desc.imageDimensions.push_back({ (std::max)(a_request.width >> mip, 1u), (std::max)(a_request.height >> mip, 1u), 0, 0 });

				auto record = std::make_shared<NativeResource>();
				record->name = a_request.name;
				NoteRequest(*record, a_request);
				try {
					record->graphOwned = org::PixelBuffer::CreateShared(desc);
					record->graphOwned->SetName(fmt::format("cs.native.texture.{}", record->serial));
				} catch (const std::exception& e) {
					LogHostMade(fmt::format("ORG could not make it ({})", e.what()), a_request.name);
					return false;
				}
				if (!FinishGraphOwned(record, a_request, a_dxgiFormat, a_format, a_viewFormat, a_support, a_mips, a_layers, a_depth))
					return false;
				a_out = std::move(record);
				return true;
			}

			/** @brief Finishes MakeGraphOwned once the PixelBuffer has memory. Logs and returns false when a step fails. */
			bool FinishGraphOwned(const std::shared_ptr<NativeResource>& a_record, const TextureRequest& a_request, DXGI_FORMAT a_dxgiFormat, rhi::Format a_format,
				rhi::Format a_viewFormat, const rhi::FormatSupport& a_support, uint32_t a_mips, uint32_t a_layers, uint32_t a_depth)
			{
				const auto& record = a_record;
				try {
					auto texture = record->graphOwned;
					record->graphOwned = nullptr;
					if (!FillImage(*record, texture->GetAPIResource(), a_request, a_dxgiFormat, a_format, a_support, a_mips, a_layers, a_depth, VK_IMAGE_LAYOUT_GENERAL)) {
						LogHostMade("BasicRHI did not describe the image ORG made", a_request.name);
						return false;
					}
					record->shape.viewFormat = a_viewFormat;
					record->resource = ::RenderGraph::ImportDxvkImage(Rhi(), record->image, a_request.name.c_str());
					if (!record->resource) {
						LogHostMade("the image ORG made could not be imported for the context", a_request.name);
						return false;
					}
					record->graphOwned = std::move(texture);
				} catch (const std::exception& e) {
					LogHostMade(fmt::format("ORG could not make it ({})", e.what()), a_request.name);
					return false;
				}
				// Going through the external client lets the ledger move it out of Undefined and track it after.
				if (!InitializeTracked(*record, a_request, a_format, a_mips, a_layers, a_depth)) {
					LogOnce(loggedUpload, a_request.name, "its initial layout or contents could not be written");
					return false;
				}
				record->ownership = NativeResource::Ownership::Tracked;
				NoteOwnedImage(*record);
				if (a_request.usage == D3D11_USAGE_DYNAMIC) {
					TextureRequest cpu = a_request;
					cpu.data = nullptr;
					cpu.name = a_request.name + " (upload)";
					if (FAILED(MakeStaging(cpu, true, record->upload)))
						return false;
				}
				return true;
			}

			/** @brief Fills the description importers read, in the layout the image rests in. */
			static bool FillImage(NativeResource& a_record, const rhi::Resource& a_resource, const TextureRequest& a_request, DXGI_FORMAT a_dxgiFormat, rhi::Format a_format,
				const rhi::FormatSupport& a_support, uint32_t mips, uint32_t layers, uint32_t depth, VkImageLayout a_resting)
			{
				// On D3D12 the VkImage field carries the ID3D12Resource, with no Vulkan format, flags or usage.
				rhi::VulkanResourceInfo info{};
				void* d3d12 = nullptr;
				if (OnD3D12()) {
					d3d12 = D3D12ResourceOf(a_resource);
					if (!d3d12)
						return false;
				} else if (!rhi::vulkan::get_resource_info(a_resource, info) || !info.resource) {
					return false;
				}
				const bool cube = (a_request.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
				auto& image = a_record.image;
				image.image = d3d12 ? reinterpret_cast<VkImage>(d3d12) : rhi::vulkan::from_native_void<VkImage>(info.resource);
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
				image.layout = a_resting;
				image.viewType = image.type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D :
				                 cube                           ? (layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE) :
				                 image.type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) :
				                                                  (layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
				image.viewFormat = image.format;
				// BasicRHI stores some formats as others, such as A8 as R8, so the swizzle makes readers see the channels the format names.
				image.components = ComponentsOf(a_support.srvSwizzle);
				const bool depthFormat = (a_request.bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
				image.subresourceRange = { static_cast<VkImageAspectFlags>(depthFormat ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, mips, 0, layers };
				a_record.kind = NativeResource::Kind::Image;
				a_record.shape = ShapeOf(a_request, a_dxgiFormat, a_format, mips, layers, depth);
				return true;
			}

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
				shape.viewFormat = a_format;
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

			/** @brief A STAGING texture or DYNAMIC upload side, as a buffer with 512-byte subresource offsets and 256-byte rows. */
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
				NoteRequest(*record, a_request);
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
				// Depth-stencil staging keeps room for the GPU's two planes after the CPU's texels. Copies to and from images go through them.
				if (StencilTexelBytes(shape)) {
					for (auto& entry : record->layout) {
						const uint32_t mip = static_cast<uint32_t>(&entry - record->layout.data()) % mips;
						entry.depthRowPitch = (shape.Width(mip) * 4u + 255u) & ~255u;
						entry.depthOffset = offset;
						offset = (offset + uint64_t(entry.depthRowPitch) * shape.Height(mip) + 511) & ~uint64_t(511);
						entry.stencilRowPitch = (shape.Width(mip) + 255u) & ~255u;
						entry.stencilOffset = offset;
						offset = (offset + uint64_t(entry.stencilRowPitch) * shape.Height(mip) + 511) & ~uint64_t(511);
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

			bool MakeBufferResource(NativeResource& a_record, uint64_t a_size, rhi::HeapType a_heap, bool a_unorderedAccess)
			{
				rhi::ResourceDesc desc{};
				desc.type = rhi::ResourceType::Buffer;
				desc.heapType = a_heap;
				desc.resourceFlags = a_unorderedAccess ? rhi::RF_AllowUnorderedAccess : rhi::RF_None;
				desc.debugName = a_record.name.c_str();
				// Round up to whole words so a raw view reaches the last index of an odd-sized index list.
				desc.buffer.sizeBytes = (a_size + 3) & ~uint64_t(3);
				if (Rhi().CreateCommittedResource(desc, a_record.resource) != rhi::Result::Ok || !a_record.resource || !DescribeBuffer(a_record, a_record.resource.Get())) {
					LogOnce(loggedCreate, a_record.name, "BasicRHI could not create the buffer");
					return false;
				}
				a_record.size = a_size;
				a_record.hostVisible = a_heap != rhi::HeapType::DeviceLocal;
				return true;
			}

			/** @brief Fills a buffer record's handle and address. On D3D12 the VkBuffer field holds the ID3D12Resource. */
			static bool DescribeBuffer(NativeResource& a_record, const rhi::Resource& a_buffer)
			{
				if (OnD3D12()) {
					uint64_t offset = 0;
					void* resource = D3D12BufferRangeOf(a_buffer, offset);
					if (!resource)
						return false;
					a_record.vkBuffer = reinterpret_cast<VkBuffer>(resource);
					a_record.offset = offset;
					a_record.address =
						static_cast<VkDeviceAddress>(g_device ? g_device->Rhi().GetBufferDeviceAddress({ a_buffer.GetHandle(), 0 }) : D3D12AddressOf(resource) + offset);
					a_record.usage = 0;
					return true;
				}
				rhi::VulkanResourceInfo info{};
				if (!rhi::vulkan::get_resource_info(a_buffer, info) || !info.resource)
					return false;
				a_record.vkBuffer = rhi::vulkan::from_native_void<VkBuffer>(info.resource);
				a_record.offset = 0;
				a_record.address = static_cast<VkDeviceAddress>(info.deviceAddress);
				a_record.usage = static_cast<VkBufferUsageFlags>(info.usage);
				return true;
			}

			/** @brief Loads GenerateMips' programs once the graph's shader compiler is up. */
			void LoadMipPrograms()
			{
				mips2D = ComputeProgram::Load(Rhi(), ComputeProgram::GenerateMips(false));
				mipsArray = ComputeProgram::Load(Rhi(), ComputeProgram::GenerateMips(true));
				logger::info("[ORG native] GenerateMips: the context builds mip chains with a compute shader ({}2D textures, {}arrays and cubes)",
					mips2D ? "" : "not for ", mipsArray ? "" : "not for ");
			}

			/** @brief GenerateMips' UAV scratch texture for a_image, resting in Common. Null when it cannot be made. */
			std::shared_ptr<NativeResource> MakeMipScratch(const NativeResource& a_image, rhi::Format a_format)
			{
				const auto& shape = a_image.shape;
				auto record = std::make_shared<NativeResource>();
				record->name = a_image.name + " (mip scratch)";
				record->kind = NativeResource::Kind::Image;
				record->shape = shape;
				record->shape.format = record->shape.viewFormat = a_format;
				rhi::ResourceDesc desc{};
				desc.type = rhi::ResourceType::Texture2D;
				desc.heapType = rhi::HeapType::DeviceLocal;
				desc.resourceFlags = rhi::RF_AllowUnorderedAccess;
				desc.debugName = record->name.c_str();
				desc.texture.format = a_format;
				desc.texture.width = shape.width;
				desc.texture.height = shape.height;
				desc.texture.depthOrLayers = static_cast<uint16_t>(shape.layers);
				desc.texture.mipLevels = static_cast<uint16_t>(shape.mips);
				desc.texture.sampleCount = 1;
				if (Rhi().CreateCommittedResource(desc, record->resource) != rhi::Result::Ok || !record->resource)
					return nullptr;
				const rhi::TextureSubresourceRange all{ 0, shape.mips, 0, shape.layers, 0, 1 };
				const auto handle = record->resource->GetHandle();
				NativeResource* const used[] = { record.get() };
				const auto initialize = [&](rhi::CommandList& a_list, const Immediate::Descriptors&) {
					RecordInitialize(a_list, handle, all, {}, {}, rhi::ResourceLayout::Common, rhi::ResourceAccessType::Common);
				};
				return immediate.RunUntracked("mip scratch " + a_image.name, initialize, {}, used) ? record : nullptr;
			}

			/** @brief A short-lived device buffer for one copy, kept until that copy's work is done. */
			std::shared_ptr<NativeResource> MakeBounce(uint64_t a_size)
			{
				auto record = std::make_shared<NativeResource>();
				record->name = "Native::Bounce";
				if (!MakeBufferResource(*record, (std::max)(a_size, uint64_t(1)), rhi::HeapType::DeviceLocal, false))
					return nullptr;
				return record;
			}

			/** @brief A short-lived upload buffer for one context operation, kept until its work is done. */
			std::shared_ptr<NativeResource> MakeUpload(uint64_t a_size)
			{
				auto record = std::make_shared<NativeResource>();
				record->name = "Native::Upload";
				if (!MakeBufferResource(*record, (std::max)(a_size, uint64_t(1)), rhi::HeapType::Upload, false))
					return nullptr;
				return record;
			}

			/** @brief One subresource of a new image's initial data, in D3D11's mip-then-slice order. */
			struct InitialCopy
			{
				uint32_t mip, slice;
				rhi::CopyableFootprint footprint;
			};

			/** @brief Stages a new image's initial data in an upload buffer. False when it cannot be staged. */
			bool StageInitialData(rhi::ResourceHandle a_texture, const TextureRequest& a_request, rhi::Format a_format, uint32_t a_mips, uint32_t a_layers,
				uint32_t a_depth, rhi::ResourcePtr& a_upload, std::vector<InitialCopy>& a_copies, rhi::TextureSubresourceRange& a_all)
			{
				const rhi::ResourceHandle texture = a_texture;
				a_copies.clear();
				// D3D12 lays out stencil as a second plane with its own layout, so both planes must start defined.
				const bool twoPlanes = OnD3D12() && (a_format == rhi::Format::D24_UNorm_S8_UInt || a_format == rhi::Format::R24G8_Typeless ||
														a_format == rhi::Format::D32_Float_S8X24_UInt || a_format == rhi::Format::R32G8X24_Typeless);
				a_all = { 0, a_mips, 0, a_layers, 0, twoPlanes ? 2u : 1u };
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
							a_copies.push_back({ mip, slice, footprint });
							// 512 is D3D12's placement alignment, which every backend accepts.
							offset = (placed.totalBytes + range.baseOffset + 511) & ~uint64_t(511);
						}
					}
					rhi::ResourceDesc desc{};
					desc.type = rhi::ResourceType::Buffer;
					desc.heapType = rhi::HeapType::Upload;
					desc.resourceFlags = rhi::RF_None;
					desc.debugName = "CS native texture upload";
					desc.buffer.sizeBytes = (std::max)(offset, uint64_t(1));
					if (Rhi().CreateCommittedResource(desc, a_upload) != rhi::Result::Ok || !a_upload)
						return false;
					void* mapped = nullptr;
					a_upload->Map(&mapped);
					if (!mapped)
						return false;
					const bool blocks = rhi::helpers::IsBlockCompressed(a_format);
					const uint32_t blockBytes = rhi::helpers::BytesPerBlock(a_format);
					for (const auto& copy : a_copies) {
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
					a_upload->Unmap(0, desc.buffer.sizeBytes);
				}

				return true;
			}

			/** @brief Records the first barrier from Undefined, the initial copies, and the move into a_layout. */
			static void RecordInitialize(rhi::CommandList& a_list, rhi::ResourceHandle a_texture, const rhi::TextureSubresourceRange& a_all,
				const rhi::ResourcePtr& a_upload, const std::vector<InitialCopy>& a_copies, rhi::ResourceLayout a_layout, rhi::ResourceAccessType a_access)
			{
				rhi::TextureBarrier toCopy{};
				toCopy.texture = a_texture;
				toCopy.range = a_all;
				toCopy.beforeLayout = rhi::ResourceLayout::Undefined;
				toCopy.discard = true;
				if (!a_copies.empty()) {
					toCopy.afterSync = rhi::ResourceSyncState::Copy;
					toCopy.afterAccess = rhi::ResourceAccessType::CopyDest;
					toCopy.afterLayout = rhi::ResourceLayout::CopyDest;
					a_list.Barriers({ { &toCopy, 1 }, {}, {} });
					for (const auto& copy : a_copies) {
						rhi::BufferTextureCopyFootprint region{};
						region.texture = a_texture;
						region.buffer = a_upload->GetHandle();
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
				toCopy.afterAccess = a_access;
				toCopy.afterLayout = a_layout;
				a_list.Barriers({ { &toCopy, 1 }, {}, {} });
			}

			/**
			 * @brief Writes a new image's initial data and moves it into Common. Waits for the GPU so the image is ready when the handle exists.
			 * Common is what graph executions return host-made and tracked textures to, and what the context's raw operations expect.
			 */
			bool Initialize(rhi::ResourceHandle a_texture, const TextureRequest& a_request, rhi::Format a_format, uint32_t a_mips, uint32_t a_layers, uint32_t a_depth)
			{
				auto upload = std::make_shared<rhi::ResourcePtr>();
				std::vector<InitialCopy> copies;
				rhi::TextureSubresourceRange all{};
				if (!StageInitialData(a_texture, a_request, a_format, a_mips, a_layers, a_depth, *upload, copies, all))
					return false;
				const std::string label = (copies.empty() ? "initial layout " : "initial contents ") + a_request.name;
				// Waited for, so a graph that imports it later on another queue finds it written.
				const uint64_t value = immediate.RunUntracked(
					label,
					[&](rhi::CommandList& a_list, const Immediate::Descriptors&) {
						RecordInitialize(a_list, a_texture, all, *upload, copies, rhi::ResourceLayout::Common, rhi::ResourceAccessType::Common);
					},
					{ upload }, {});
				return value && immediate.Wait(value);
			}

			/** @brief A tracked texture's first contents through ORG's external client. No wait, since the ledger orders every later use. */
			bool InitializeTracked(NativeResource& a_record, const TextureRequest& a_request, rhi::Format a_format, uint32_t a_mips, uint32_t a_layers, uint32_t a_depth)
			{
				auto& texture = *a_record.graphOwned;
				const auto handle = texture.GetAPIResource().GetHandle();
				auto upload = std::make_shared<rhi::ResourcePtr>();
				std::vector<InitialCopy> copies;
				rhi::TextureSubresourceRange all{};
				if (!StageInitialData(handle, a_request, a_format, a_mips, a_layers, a_depth, *upload, copies, all))
					return false;
				const ::RenderGraph::ExternalWork::Access access{ &texture, {},
					{ rhi::ResourceAccessType::CopyDest, copies.empty() ? rhi::ResourceLayout::Common : rhi::ResourceLayout::CopyDest, rhi::ResourceSyncState::Copy } };
				NativeResource* const used[] = { &a_record };
				Immediate::Work work;
				work.accesses = { &access, 1 };
				work.used = used;
				const auto record = [&](rhi::CommandList& a_list, const Immediate::Descriptors&) {
					for (const auto& copy : copies) {
						rhi::BufferTextureCopyFootprint region{};
						region.texture = handle;
						region.buffer = (*upload)->GetHandle();
						region.mip = copy.mip;
						region.arraySlice = copy.slice;
						region.footprint = copy.footprint;
						a_list.CopyBufferToTexture(region);
					}
				};
				return immediate.Run((copies.empty() ? "initial layout " : "initial contents ") + a_request.name, work, record, { upload }) != 0;
			}

			/**
			 * @brief Uploads an exempt texture and leaves it in shader-read. No wait. Returns the timeline value it is ready at, or 0.
			 * ImmutableTextureOf waits for that value before handing the texture to a graph.
			 */
			uint64_t InitializeShaderRead(NativeResource& a_record, const TextureRequest& a_request, rhi::Format a_format, uint32_t a_mips, uint32_t a_layers,
				uint32_t a_depth)
			{
				const rhi::ResourceHandle texture = a_record.resource->GetHandle();
				auto upload = std::make_shared<rhi::ResourcePtr>();
				std::vector<InitialCopy> copies;
				rhi::TextureSubresourceRange all{};
				if (!StageInitialData(texture, a_request, a_format, a_mips, a_layers, a_depth, *upload, copies, all) || copies.empty())
					return 0;
				NativeResource* const used[] = { &a_record };
				return immediate.RunUntracked(
					"exempt transfer " + a_request.name,
					[&](rhi::CommandList& a_list, const Immediate::Descriptors&) {
						RecordInitialize(a_list, texture, all, *upload, copies, rhi::ResourceLayout::ShaderResource, rhi::ResourceAccessType::ShaderResource);
					},
					{ upload }, used);
			}

			/** @brief A buffer. Device-local without CPU access or contents, else in upload memory. */
			HRESULT MakeBuffer(const D3D11_BUFFER_DESC& a_desc, const D3D11_SUBRESOURCE_DATA* a_data, std::shared_ptr<NativeResource>& a_out)
			{
				const bool hostVisible = a_desc.Usage != D3D11_USAGE_DEFAULT || a_desc.CPUAccessFlags || (a_data && a_data->pSysMem);
				auto record = std::make_shared<NativeResource>();
				record->name = "Native::Buffer";
				record->bufferDesc = a_desc;
				record->bindFlags = a_desc.BindFlags;
				record->d3dUsage = a_desc.Usage;
				record->dimension = D3D11_RESOURCE_DIMENSION_BUFFER;
				record->initialData = a_data && a_data->pSysMem;
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

			/**
			 * @brief kFRAMEBUFFER's next version before the engine target set adopts the slot: InitD3D's GetBuffer and every frame up
			 * to Engine::Setup draw into it. A graph-owned texture from stage 3, so the present copies it through the ledger as it
			 * copies the set's versions. The first call makes the slot. Render thread, under presentLock after stage 3.
			 */
			bool MakeBackBuffer()
			{
				TextureRequest request;
				request.width = width;
				request.height = height;
				request.format = format;
				request.bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
				request.name = "Native::BackBuffer";
				request.beforeGraph = true;
				std::shared_ptr<NativeResource> record;
				if (FAILED(MakeTexture(request, record)))
					return false;
				if (!record->graphOwned)
					RenderGraph::Fatal("native device", "the back buffer is not graph-owned, so it cannot be a version of kFRAMEBUFFER's slot");
				auto image = std::static_pointer_cast<org::GloballyIndexedResource>(record->graphOwned);
				try {
					if (!backBufferSlot) {
						backBufferSlot = org::VersionedTexture::CreateImage(std::move(image));
					} else {
						const ::RenderGraph::Frame::Boundary::Caller caller("native back buffer");
						backBufferSlot->PublishImage(std::move(image));
					}
				} catch (const std::exception& e) {
					RenderGraph::Fatal("native device", fmt::format("the back buffer could not be a version of kFRAMEBUFFER's slot: {}", e.what()));
				}
				D3D11_TEXTURE2D_DESC desc{ width, height, 1, 1, format, { 1, 0 }, D3D11_USAGE_DEFAULT, request.bindFlags, 0, 0 };
				Tripwire::Options options;
				options.name = request.name;
				options.owner = std::move(record);
				backBuffer = Tripwire::CreateTexture2D(desc, std::move(options));
				return backBuffer != nullptr;
			}

			/** @brief The present's timeline and timer. Its copies are recorded through the external client. */
			bool MakePresentObjects()
			{
				if (Rhi().CreateTimeline(presentTimeline, 0, "CS native present") != rhi::Result::Ok || !presentTimeline)
					return false;
				MakePresentTimer();
				return true;
			}

			/** @brief The present copy's timestamps, when the GPU timers collect. Without them the copy is untimed, which is logged. */
			void MakePresentTimer()
			{
				if (!GpuTimingSource::Collecting())
					return;
				rhi::QueryPoolDesc desc{};
				desc.type = rhi::QueryType::Timestamp;
				desc.count = kPresentSlots * 2;
				const uint64_t ticksPerSecond = Rhi().GetTimestampCalibration(rhi::QueueKind::Graphics).ticksPerSecond;
				if (!ticksPerSecond || Rhi().CreateQueryPool(desc, presentTimer.pool) != rhi::Result::Ok || !presentTimer.pool) {
					logger::warn("[ORG native] the present copy has no GPU timer: the timestamp queries could not be made");
					presentTimer = {};
					return;
				}
				presentTimer.pool->SetName("CS native present copy");
				presentTimer.stride = (std::max)(presentTimer.pool->GetQueryResultInfo().elementSize, 8u);
				const rhi::ResourceDesc bufferDesc = rhi::helpers::ResourceDesc::Buffer(presentTimer.stride * desc.count, rhi::HeapType::Readback, {}, "CS native present copy timestamps");
				if (Rhi().CreateCommittedResource(bufferDesc, presentTimer.readback) != rhi::Result::Ok || !presentTimer.readback) {
					logger::warn("[ORG native] the present copy has no GPU timer: the timestamp readback could not be made");
					presentTimer = {};
					return;
				}
				presentTimer.msPerTick = 1000.0 / static_cast<double>(ticksPerSecond);
				logger::info("[ORG native] the present copy is timed on the GPU (\"native present copy\" in the [GpuEvents] lines)");
			}

			/** @brief Starts a_slot's timed range on a_list. */
			void BeginPresentTimer(rhi::CommandList& a_list, uint32_t a_slot)
			{
				if (!presentTimer.pool)
					return;
				a_list.ResetQueries(presentTimer.pool->GetHandle(), a_slot * 2, 2);
				a_list.WriteTimestamp(presentTimer.pool->GetHandle(), a_slot * 2, rhi::Stage::Top);
			}

			/** @brief Ends a_slot's timed range on a_list and resolves it for EndPresentSlot. */
			void EndPresentTimer(rhi::CommandList& a_list, uint32_t a_slot)
			{
				if (!presentTimer.pool)
					return;
				a_list.WriteTimestamp(presentTimer.pool->GetHandle(), a_slot * 2 + 1, rhi::Stage::Bottom);
				a_list.ResolveQueryData(presentTimer.pool->GetHandle(), a_slot * 2, 2, presentTimer.readback->GetHandle(), presentTimer.stride * a_slot * 2);
			}

			/** @brief After a_slot's last present finished on the GPU: hands its copy's time to GpuTimingSource. */
			void ReadPresentTimer(uint32_t a_slot)
			{
				if (!presentTimer.pool || !std::exchange(presentTimer.pending[a_slot], false))
					return;
				void* mapped = nullptr;
				presentTimer.readback->Map(&mapped, presentTimer.stride * a_slot * 2, presentTimer.stride * 2);
				if (!mapped)
					return;
				uint64_t ticks[2]{};
				std::memcpy(&ticks[0], static_cast<const std::byte*>(mapped), sizeof(uint64_t));
				std::memcpy(&ticks[1], static_cast<const std::byte*>(mapped) + presentTimer.stride, sizeof(uint64_t));
				presentTimer.readback->Unmap(0, 0);
				if (!ticks[0] || ticks[1] < ticks[0])
					return;
				GpuTimingSource::Range range;
				range.name = "native present copy";
				range.queue = GpuTimingSource::Queue::Graphics;
				// A sub-range keeps it out of the queue's walk, so it adds a row and changes no other range's self time.
				range.kind = GpuTimingSource::RangeKind::Sub;
				range.beginTick = ticks[0];
				range.endTick = ticks[1];
				range.ms = static_cast<double>(ticks[1] - ticks[0]) * presentTimer.msPerTick;
				GpuTimingSource::AddExternalRange(std::move(range));
			}

			/** @brief CS_ORG_PRESENT_COPY_SKIP=1 (development builds): the present keeps its barriers and skips its copy, for timing. */
			static bool SkipPresentCopy()
			{
#if defined(CS_RELEASE_PROFILE)
				return false;
#else
				static const bool skip = [] {
					const bool on = Switches::On("CS_ORG_PRESENT_COPY_SKIP");
					if (on)
						logger::warn("[ORG native] CS_ORG_PRESENT_COPY_SKIP=1: the present skips its copy; the screen shows garbage and timings stay valid");
					return on;
				}();
				return skip;
#endif
			}

			/** @brief The slot's current version as a present copies it. Empty when it has no readable description. Under presentLock. */
			PresentSource CurrentSource() const
			{
				PresentSource source;
				source.graph = backBufferSlot->CurrentResource();
				rhi::ResourceDesc desc{};
				if (!source.graph || !source.graph->TryGetRHIResourceDesc(desc))
					return {};
				source.width = desc.texture.width;
				source.height = desc.texture.height;
				return source;
			}

			/**
			 * @brief The engine target set adopted kFRAMEBUFFER's slot (its twin's first version joined it). GetBuffer answers the
			 * set's field from now on. Render thread, at Engine::Setup, before the first frame that draws into it.
			 */
			void AdoptBackBufferSlot(const std::shared_ptr<org::VersionedTexture>& a_slot, winrt::com_ptr<ID3D11Texture2D> a_field)
			{
				std::lock_guard guard(presentLock);
				if (setAdopted)
					RenderGraph::Fatal("native device", "the engine target set adopted the back buffer slot twice");
				if (!a_slot || a_slot != backBufferSlot)
					RenderGraph::Fatal("native device", "the engine target set's kFRAMEBUFFER is not the slot native made with the swap chain");
				if (!a_field)
					RenderGraph::Fatal("native device", "the engine target set adopted the back buffer slot without a field");
				setAdopted = true;
				// The engine released its views of the startup version at DeclareSet. A present still queued holds that version until it ran.
				backBuffer = std::move(a_field);
				logger::info("[ORG native] the engine target set adopted kFRAMEBUFFER's slot; it makes the slot's versions from now on");
			}

			/**
			 * @brief Waits for every queued copy, so the swap chain images can change. The submission thread's, before it replaces or
			 * resizes the swap chain, and the window's close. A failed wait is a lost device: nothing more is shown.
			 */
			void WaitPresents()
			{
				if (!presentValue || presentTimeline->HostWait(presentValue) == rhi::Result::Ok)
					return;
				lost = true;
				logger::error("[ORG native] the presents' copies never finished on the GPU; nothing more is shown");
			}

			/** @brief A swap chain resize for the submission thread. */
			struct ResizeRequest
			{
				uint32_t count = 0, width = 0, height = 0;
				uint64_t serial = 0;
			};

			/**
			 * @brief The engine's ResizeBuffers. Render thread, under presentLock. Takes the engine's new size and, before the engine
			 * target set adopted the slot, makes kFRAMEBUFFER's next version. The swap chain itself is resized by the
			 * submission thread after the presents queued so far (QueueSwapchainResize with a_request), so nothing waits here.
			 * Until kFRAMEBUFFER's version at the new size is adopted, the present copies the smaller extent.
			 */
			HRESULT Resize(uint32_t a_count, uint32_t a_width, uint32_t a_height, DXGI_FORMAT a_format, ResizeRequest& a_request)
			{
				if (!a_width || !a_height) {
					RECT client{};
					::GetClientRect(window, &client);
					a_width = (std::max)(static_cast<uint32_t>(client.right - client.left), 1u);
					a_height = (std::max)(static_cast<uint32_t>(client.bottom - client.top), 1u);
				}
				if (a_count)
					bufferCount = (std::max)(a_count, 2u);
				const DXGI_FORMAT previousFormat = format;
				if (a_format != DXGI_FORMAT_UNKNOWN)
					format = a_format;
				// Every version of the slot has the first one's format.
				if (format != previousFormat)
					RenderGraph::Fatal("native device", fmt::format("the swap chain was resized to DXGI format {} from {}; kFRAMEBUFFER cannot change its format",
						static_cast<int>(format), static_cast<int>(previousFormat)));
				const bool keepBackBuffer = backBuffer && a_width == width && a_height == height;
				width = a_width;
				height = a_height;
				a_request = { bufferCount, width, height, resizesAsked.fetch_add(1, std::memory_order_acq_rel) + 1 };
				// The engine target set makes kFRAMEBUFFER's next version on the revision lane, so only the swap chain follows.
				if (setAdopted) {
					// The set writes DisplaySize from the engine's new targets.
					logger::info("[ORG native] the engine resized to {}x{}; the swap chain follows after the queued presents, kFRAMEBUFFER with the engine target set",
						width, height);
					return S_OK;
				}
				// Nothing the frame binds changes when the size and format stay: the engine takes the same back buffer again.
				if (keepBackBuffer) {
					logger::info("[ORG native] the engine resized at its size {}x{}; the back buffer is kept", width, height);
					return S_OK;
				}
				// Before the set adopted the slot: the engine released its views before ResizeBuffers, so the old version's handle goes now.
				// A present still queued holds the old version until it ran.
				backBuffer = nullptr;
				if (!MakeBackBuffer()) {
					logger::error("[ORG native] kFRAMEBUFFER's next version could not be made at {}x{}", width, height);
					return E_OUTOFMEMORY;
				}
				// DisplaySize is the set's to write (DeclareSet), so nothing reads this size before it.
				logger::info("[ORG native] kFRAMEBUFFER's next version is made at {}x{}; the swap chain follows after the queued presents", width, height);
				return S_OK;
			}

			/**
			 * @brief Render owner, outside presentLock. Queues a_request's swap chain resize on the submission thread, after the presents
			 * queued so far. A newer request supersedes it. Never waits.
			 */
			void QueueSwapchainResize(const ResizeRequest& a_request)
			{
				RenderGraphRuntime::Get().QueueOnSubmitThread([this, a_request] {
					std::lock_guard guard(presentLock);
					if (resizesAsked.load(std::memory_order_acquire) != a_request.serial)
						return;
					ResizeSwapchain(a_request.count, a_request.width, a_request.height);
				});
			}

			/** @brief Submission thread, under presentLock. Resizes the swap chain once its images are free. A failure is fatal. */
			void ResizeSwapchain(uint32_t a_count, uint32_t a_width, uint32_t a_height)
			{
				ZoneScopedN("CS.Native.ResizeSwapchain");
				if (lost.load(std::memory_order_relaxed))
					return;
				WaitPresents();
				if (lost.load(std::memory_order_relaxed))
					return;
				if (swapchain->ResizeBuffers(a_count, a_width, a_height, swapchainFormat, 0) != rhi::Result::Ok)
					RenderGraph::Fatal("native device", fmt::format("the swap chain could not be resized to {}x{} with {} images", a_width, a_height, a_count));
				swapBufferCount = a_count;
				swapWidth = a_width;
				swapHeight = a_height;
				// The resize remade the images, so a mode change reported before it is served.
				resizeRequested = false;
				logger::info("[ORG native] swap chain resized to {}x{} on the submission thread", swapWidth, swapHeight);
			}

			/**
			 * @brief Remakes the swap chain at its size after Present reports a mode change or failure. The thread that presents, under
			 * presentLock: the submission thread when presents are queued. The back buffer stays, since the engine draws through its views.
			 */
			void RecreateSwapchain()
			{
				ResizeSwapchain(swapBufferCount, swapWidth, swapHeight);
			}

			/** @brief Publishes what the swap chain is now. Under presentLock. */
			void PublishPresentation(uint64_t a_serial, bool a_vsync)
			{
				const bool hdr10 = swapchain->ColorSpace() == rhi::SwapchainColorSpace::Hdr10St2084;
				const bool layer = swapchain->Caps().presentationLayer;
				hdr10Supported.store(swapchain->IsColorSpaceSupported(rhi::SwapchainColorSpace::Hdr10St2084), std::memory_order_relaxed);
				std::lock_guard guard(presentationLock);
				adoptedPresentation.serial = a_serial;
				adoptedPresentation.format = static_cast<std::uint32_t>(hdr10 ? rhi::Format::R10G10B10A2_UNorm : swapchainFormat);
				adoptedPresentation.hdr10 = hdr10;
				adoptedPresentation.presentationLayer = layer;
				adoptedPresentation.vsync = a_vsync;
			}

			/**
			 * @brief The submission thread's replacement: waits for the presents, destroys the swap chain, runs a_tornDown, creates the
			 * new one. Under presentLock, so a loading screen's present on its own thread cannot use the swap chain in between.
			 */
			void ReplacePresentation(const FrameGen::PresentationConfig& a_config, uint64_t a_serial)
			{
				ZoneScopedN("CS.Native.ReplacePresentation");
				const auto describe = [&] {
					return fmt::format("frame generation {}, {}, vsync {}, tearing {}", static_cast<int>(a_config.method), a_config.hdr ? "HDR10" : "SDR",
						a_config.vsync, a_config.tearing);
				};
				if (lost.load(std::memory_order_relaxed))
					return;
				const auto start = std::chrono::steady_clock::now();
				WaitPresents();
				if (lost.load(std::memory_order_relaxed))
					return;
				// HDR10 the output cannot show presents SDR, as before.
				const bool hdr = a_config.hdr && swapchain->IsColorSpaceSupported(rhi::SwapchainColorSpace::Hdr10St2084);
				if (a_config.hdr && !hdr)
					logger::warn("[ORG native] HDR is on, but the display path does not take HDR10 (ST.2084, Rec. 2020). Presenting SDR.");
				swapchain.Reset();
				// The step stored last, so a feature load change waiting for a replacement goes through whichever request runs.
				const auto tornDown = FrameGen::NativePresenter::Get().TornDown();
				if (tornDown && !tornDown())
					RenderGraph::Fatal("native device", fmt::format("the presentation's torn-down step failed for {}", describe()));
				rhi::SwapchainDesc desc{};
				desc.hwnd = window;
				desc.width = swapWidth;
				desc.height = swapHeight;
				desc.format = swapchainFormat;
				desc.bufferCount = swapBufferCount;
				desc.presentationLayer = a_config.method != FrameGen::Method::kNone;
				// A layer bakes the present mode in (FidelityFX's swap chain does), so make it with the mode the vsync asks for.
				// BasicRHI picks the first mode from allowTearing.
				desc.allowTearing = a_config.tearing && !(desc.presentationLayer && a_config.vsync);
				desc.colorSpace = hdr ? rhi::SwapchainColorSpace::Hdr10St2084 : rhi::SwapchainColorSpace::SrgbNonlinear;
				if (const auto created = Rhi().CreateSwapchain(desc, swapchain); created != rhi::Result::Ok || !swapchain)
					RenderGraph::Fatal("native device", fmt::format("the swap chain could not be made for {} (BasicRHI result {})", describe(), static_cast<int>(created)));
				if (swapchain->SetMaximumFrameLatency(frameLatency) != rhi::Result::Ok)
					RenderGraph::Fatal("native device", fmt::format("the new swap chain refused a frame latency of {}", frameLatency));
				// A fresh swap chain has no mode change pending.
				resizeRequested = false;
				layerVsync = desc.presentationLayer ? std::optional<bool>(a_config.vsync) : std::nullopt;
				PublishPresentation(a_serial, a_config.vsync);
				FrameGen::NativePresenter::Get().Replaced(a_serial);
				logger::info("[ORG native] presentation replaced (request {}): {}; {} images {}x{}, presentation layer {}, {:.1f} ms", a_serial, describe(),
					swapchain->ImageCount(), swapWidth, swapHeight, desc.presentationLayer,
					std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
			}

			/**
			 * @brief The engine's Present. A DXGI_PRESENT_TEST present does no work. The render owner queues the present on the
			 * submission thread. Two kinds run on the calling thread: presents before RenderGraphRuntime::Initialize, which made no
			 * submission thread yet, and presents of a thread that does not own rendering (a loading screen's), which cannot take the
			 * render owner's pacing counters. Every present copies the slot's current version through the external
			 * client.
			 */
			HRESULT Present(UINT a_syncInterval, UINT a_flags)
			{
				if (a_flags & DXGI_PRESENT_TEST)
					return S_OK;
				// Once a frame, what finished context work kept goes.
				if (g_device)
					g_device->immediate.ReleaseDone();
				if (RenderGraphRuntime::Get().QueuesPresent())
					return QueuePresent(a_syncInterval);
				// The render owner's epochs must reach the queue before this present, and so must what they wait for. Nothing is queued
				// before the graph host exists.
				RenderGraphRuntime::Get().DrainSubmissions("the present", true);
				for (const auto& wait : RenderGraphRuntime::Get().TakePresentWaits())
					wait();
				std::lock_guard guard(presentLock);
				return PresentLocked(a_syncInterval, CurrentSource());
			}

			/**
			 * @brief Queues the present on the submission thread after the frame's epochs. Render owner only.
			 * It queues only once the previous present finished, so the render thread runs at most one frame ahead.
			 */
			HRESULT QueuePresent(UINT a_syncInterval)
			{
				// Pace before taking presentLock. The previous present holds it through its frame latency wait.
				const uint64_t queued = queuedPresents.load(std::memory_order_relaxed);
				if (donePresents.load(std::memory_order_acquire) < queued) {
					ZoneScopedN("CS.Native.PresentPace");
					for (auto done = donePresents.load(std::memory_order_acquire); done < queued; done = donePresents.load(std::memory_order_acquire))
						donePresents.wait(done, std::memory_order_acquire);
				}
				PresentSource source;
				{
					std::lock_guard guard(presentLock);
					if (lost.load(std::memory_order_relaxed) || !backBuffer)
						return S_OK;
					// The version current now, since the render thread may adopt the next one before this present runs.
					source = CurrentSource();
					if (!source.graph)
						return S_OK;
				}
				queuedPresents.store(queued + 1, std::memory_order_relaxed);
				RenderGraphRuntime::Get().QueueOnSubmitThread([this, source = std::move(source), a_syncInterval, waits = RenderGraphRuntime::Get().TakePresentWaits()] {
					ZoneScopedN("CS.Native.QueuedPresent");
					for (const auto& wait : waits)
						wait();
					{
						std::lock_guard guard(presentLock);
						(void)PresentLocked(a_syncInterval, source);
					}
					donePresents.fetch_add(1, std::memory_order_release);
					donePresents.notify_all();
				});
				return S_OK;
			}

			/** @brief The present under presentLock: copies a_source, a version of kFRAMEBUFFER's slot, into the swap chain image. */
			HRESULT PresentLocked(UINT a_syncInterval, const PresentSource& a_source)
			{
				if (lost.load(std::memory_order_relaxed))
					return S_OK;  // already reported, and the game's frame still ends
				if (std::exchange(resizeRequested, false))
					RecreateSwapchain();
				if (!backBuffer || !a_source.graph)
					return S_OK;
				const uint32_t slot = static_cast<uint32_t>(presents % kPresentSlots);
				bool slotLost = false;
				if (slotValues[slot]) {
					ZoneScopedN("CS.Native.PresentSlotWait");
					slotLost = presentTimeline->HostWait(slotValues[slot]) != rhi::Result::Ok;
				}
				if (slotLost) {
					lost = true;
					logger::error("[ORG native] a present's copy never finished on the GPU; nothing more is shown");
					return S_OK;
				}
				ReadPresentTimer(slot);

				// D3D12 Present does not block for frame latency, so pace on the swap chain's waitable object. Vulkan's Present blocks itself.
				if (OnD3D12()) {
					const auto paced = swapchain->WaitForFrameLatency(1'000'000'000ull);
					if (paced != rhi::Result::Ok && paced != rhi::Result::Unsupported) {
						static std::atomic_flag logged;
						if (!logged.test_and_set())
							logger::warn("[ORG native] the swap chain's frame latency wait returned {} (logged once)", static_cast<int>(paced));
					}
				}
				const uint32_t index = swapchain->CurrentImageIndex();
				const rhi::ResourceHandle target = swapchain->Image(index);
				// The slot is the graph's, so the copy always goes through the ledger. A refusal has no other path.
				return PresentThroughLedger(*a_source.graph, a_source.graph, a_source.width, a_source.height, slot, target, a_syncInterval);
			}

			/**
			 * @brief The present's copy through ORG's external client, which moves the back buffer out of wherever the frame left it.
			 * The swap chain image is not the graph's, so its barriers are recorded here. A client that cannot submit is fatal.
			 */
			HRESULT PresentThroughLedger(org::Resource& a_backBuffer, std::shared_ptr<const void> a_keepAlive, uint32_t a_width, uint32_t a_height,
				uint32_t a_slot, rhi::ResourceHandle a_target, UINT a_syncInterval)
			{
				const auto source = a_backBuffer.GetAPIResource().GetHandle();
				// A mode change can remake the swap chain alone, and a resize lands before kFRAMEBUFFER's version, so copy the smaller size.
				const uint32_t copyWidth = (std::min)(a_width, swapWidth), copyHeight = (std::min)(a_height, swapHeight);
				const std::array accesses{ ::RenderGraph::ExternalWork::CopySource(a_backBuffer) };
				const rhi::TimelinePoint signal{ presentTimeline->GetHandle(), presentValue + 1 };
				const bool submitted = ::RenderGraph::ExternalWork::Submit(
					"native present copy", accesses,
					[&](rhi::CommandList& a_list) {
						BeginPresentTimer(a_list, a_slot);
						rhi::TextureBarrier before{};
						before.texture = a_target;
						before.beforeSync = rhi::ResourceSyncState::None;
						before.afterSync = rhi::ResourceSyncState::Copy;
						before.beforeAccess = rhi::ResourceAccessType::None;
						before.afterAccess = rhi::ResourceAccessType::CopyDest;
						before.beforeLayout = rhi::ResourceLayout::Undefined;
						before.afterLayout = rhi::ResourceLayout::CopyDest;
						before.discard = true;
						a_list.Barriers({ { &before, 1 }, {}, {} });
						rhi::TextureCopyRegion to{};
						to.texture = a_target;
						to.width = copyWidth;
						to.height = copyHeight;
						rhi::TextureCopyRegion from{};
						from.texture = source;
						from.width = copyWidth;
						from.height = copyHeight;
						if (!SkipPresentCopy())
							a_list.CopyTextureRegion(to, from);
						EndPresentTimer(a_list, a_slot);
						rhi::TextureBarrier after = before;
						after.beforeSync = rhi::ResourceSyncState::Copy;
						after.afterSync = rhi::ResourceSyncState::None;
						after.beforeAccess = rhi::ResourceAccessType::CopyDest;
						after.afterAccess = rhi::ResourceAccessType::Present;
						after.beforeLayout = rhi::ResourceLayout::CopyDest;
						after.afterLayout = rhi::ResourceLayout::Present;
						after.discard = false;
						a_list.Barriers({ { &after, 1 }, {}, {} });
					},
					std::move(a_keepAlive), { &signal, 1 });
				// The client shut down at the window's close: nothing is shown any more.
				if (!submitted)
					return S_OK;
				slotValues[a_slot] = ++presentValue;
				presentTimer.pending[a_slot] = static_cast<bool>(presentTimer.pool);
				return FinishPresent(signal, a_syncInterval);
			}

			/** @brief Runs the present after the copy was submitted. */
			HRESULT FinishPresent(const rhi::TimelinePoint& a_signal, UINT a_syncInterval)
			{
				// Diagnostic. Each present waits for the GPU, which tells a cross-frame memory reuse race from a GPU ordering bug.
				static const bool syncPresent = Switches::On("CS_ORG_NATIVE_SYNC_PRESENT");
				if (syncPresent)
					(void)Rhi().WaitIdle();
				++presents;
				if (presents % 600 == 0)
					::RenderGraph::RawAccess::Summary();

				rhi::PresentSyncDesc sync{};
				sync.queue = graphics;
				sync.wait = a_signal;
				// A presentation layer keeps the vsync it was made with. A change is a replacement (the frame generation controller asks
				// for it), never a recreate inside the present, which would skip the replacement's torn-down step.
				const bool vsync = layerVsync.value_or(a_syncInterval > 0);
				rhi::Result presented;
				{
					ZoneScopedN("CS.Native.SwapchainPresent");
					presented = swapchain->Present(vsync, sync);
				}
				if (presents == 1)
					Stage("first present", fmt::format("{}x{}, result {}", width, height, static_cast<int>(presented)));
				// BasicRHI only queues validation messages. They reach the log when drained.
				if (validation)
					Rhi().CheckDebugMessages();
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

		/** @brief The swap chain's format without sRGB, so the present's copy is between equal formats. */
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
		 * @brief The device handle's IDXGIDevice1. The engine sets frame latency through it, and GetAdapter lets CreateSwapChain find its
		 * factory. Methods the engine does not use fail.
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
				// A presentation replacement on the submission thread swaps the swap chain under presentLock.
				std::lock_guard guard(g_device->presentLock);
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
			// D3D11 reports the mips made, not 0, and ImageSpace checks a mip target's mip against it.
			Desc desc = a_desc;
			desc.MipLevels = record->shape.mips;
			Tripwire::Options options;
			options.name = a_request.name;
			options.owner = std::move(record);
			*a_out = a_factory(desc, std::move(options), nullptr).detach();
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
			// FLIP_DISCARD allows only buffer 0, as with DXGI.
			if (a_buffer != 0 || !g_device->backBuffer)
				return DXGI_ERROR_INVALID_CALL;
			return g_device->backBuffer->QueryInterface(a_riid, a_out);
		}

		/** @brief Runs the presentation's leaving step (NativePresenter::SetTornDown sets it) on this thread. */
		void RunPresentationLeaving()
		{
			std::function<void()> leaving;
			{
				std::lock_guard guard(g_device->presentationLock);
				leaving = g_device->leaving;
			}
			if (leaving)
				leaving();
		}

		/**
		 * @brief The size the engine's last resize asked for, width in the low word, once the set owns the back buffer. Zero before.
		 * The engine reads its size from GetBuffer's description, which is kFRAMEBUFFER's adopted one, so the set writes this instead.
		 */
		std::atomic<std::uint64_t> g_engineExtent{ 0 };

		HRESULT SwapChainResizeBuffers(UINT a_count, UINT a_width, UINT a_height, DXGI_FORMAT a_format, UINT)
		{
			// The engine passes 0x0, which means the window's client area, as with DXGI.
			UINT wantedWidth = a_width, wantedHeight = a_height;
			if (!wantedWidth || !wantedHeight) {
				RECT client{};
				::GetClientRect(g_device->window, &client);
				wantedWidth = (std::max)(static_cast<UINT>(client.right - client.left), 1u);
				wantedHeight = (std::max)(static_cast<UINT>(client.bottom - client.top), 1u);
			}
			RunPresentationLeaving();
			// The render thread only takes the new size. The submission thread resizes the swap chain after the queued presents.
			NativeState::ResizeRequest request;
			HRESULT result;
			{
				std::lock_guard guard(g_device->presentLock);
				result = g_device->Resize(a_count, a_width, a_height, a_format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_UNKNOWN : BackBufferFormat(a_format), request);
			}
			if (SUCCEEDED(result)) {
				g_device->QueueSwapchainResize(request);
				// The engine's size state follows the request; the swap chain follows on the submission thread.
				if (g_device->setAdopted)
					g_engineExtent.store(std::uint64_t(wantedWidth) | (std::uint64_t(wantedHeight) << 32), std::memory_order_release);
			}
			return result;
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
			// Prefer the output whose monitor holds most of the window.
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

		/** @brief D3D11_FORMAT_SUPPORT bits from what BasicRHI can do with a format. */
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

		/** @brief Answers D3D11 feature queries as a feature level 11.1 device would. Other features are refused and logged once. */
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

		/** @brief The native record behind a resource or view handle, or null. */
		std::shared_ptr<NativeResource> Resolve(const void* a_object)
		{
			if (auto owner = Tripwire::OwnerOf(a_object)) {
				auto& registry = GetRegistry();
				std::lock_guard guard(registry.lock);
				if (registry.live.contains(owner.get())) {
					return std::const_pointer_cast<NativeResource>(std::static_pointer_cast<const NativeResource>(owner));
				}
			}
			// A port's handle around a native one.
			IUnknown* const wrapped = Tripwire::WrappedOf(a_object);
			return wrapped && wrapped != a_object && Tripwire::IsHandle(wrapped) ? Resolve(wrapped) : nullptr;
		}

		/** @brief Marks a_record written since it was made, for the undeclared-access check's exemptions. Any thread. */
		void MarkWritten(NativeResource& a_record)
		{
			a_record.written.store(true, std::memory_order_relaxed);
		}

		/**
		 * @brief An operation on untracked resources outside every graph pass while the graph is active: no graph pass or graph
		 * resource orders it. Logged once per operation and resource. Loading-screen uploads and some feature clears still run this
		 * way, so it is not a tap error yet.
		 */
		void NoteRaw(const char* a_operation, const NativeResource& a_record)
		{
			if (!RenderGraphRuntime::Get().IsActive())
				return;
			static std::mutex s_mutex;
			static std::set<std::string> s_logged;
			auto key = fmt::format("{} on \"{}\"", a_operation, a_record.name);
			{
				std::scoped_lock lock(s_mutex);
				if (!s_logged.insert(key).second)
					return;
			}
			logger::info("[ORG native] {} runs outside the ledgers (no graph pass or graph resource orders it; logged once per operation and resource)", key);
		}

		/** @brief After a write RunOp ran: it marks its resource, and work nothing routed is outside the ledgers. */
		void NoteRun(const char* a_operation, NativeResource& a_record, bool a_routed)
		{
			MarkWritten(a_record);
			if (!a_routed)
				NoteRaw(a_operation, a_record);
		}

		/**
		 * @brief The graph resource whose ledger an operation on a_record follows, or null when none does.
		 * Commands must name this resource's own handle, since its barriers name that one.
		 */
		std::shared_ptr<org::Resource> LedgerResourceOf(NativeResource& a_record)
		{
			if (a_record.kind != NativeResource::Kind::Image)
				return nullptr;
			// An exempt texture follows a ledger only once a write promoted it.
			if (a_record.ownership == NativeResource::Ownership::Exempt && !a_record.promoted.load(std::memory_order_acquire))
				return nullptr;
			// The image's one graph resource: a tracked texture's own, a promoted exempt one's, or a host image's import.
			const auto entry = ::RenderGraph::ImageRegistry::Find(reinterpret_cast<std::uint64_t>(a_record.image.image));
			return entry && entry->resource->IsRenderGraphManaged() ? entry->resource : nullptr;
		}

		/**
		 * @brief Promotes an exempt texture to tracked before its first context write. The change goes through the build manager at the
		 * next poll, never synchronously. Logged once per texture.
		 */
		void NoteWrite(NativeResource& a_record)
		{
			if (a_record.ownership != NativeResource::Ownership::Exempt || a_record.promoted.exchange(true, std::memory_order_acq_rel))
				return;
			std::shared_ptr<org::ExternalTextureResource> resource;
			if (const auto entry = ::RenderGraph::ImageRegistry::Find(reinterpret_cast<std::uint64_t>(a_record.image.image)))
				resource = std::dynamic_pointer_cast<org::ExternalTextureResource>(entry->resource);
			logger::info("[ORG native] {}: an exempt texture written after creation; promoted to tracked{}", a_record.name,
				resource ? ", graphs built again" : " (no graph read it yet)");
			if (!resource)
				return;
			::RenderGraph::Builds::Ask({ .change = [resource] { resource->SetGraphOwnership(org::Resource::GraphOwnership::GraphManaged); } },
				fmt::format("native texture '{}' written after creation", a_record.name));
		}

		/** @brief Logs once per operation that its object was not made by the native device. */
		void Unknown(std::atomic_flag& a_logged, const char* a_operation, const void* a_object)
		{
			if (!a_logged.test_and_set())
				logger::warn("[ORG native] {} on \"{}\", which native mode did not make; nothing happens (logged once)", a_operation,
					a_object ? Tripwire::Describe(a_object).name : std::string("null"));
		}

		VkExtent3D MipExtent(const Shape& a_shape, uint32_t a_mip)
		{
			return { a_shape.Width(a_mip), a_shape.Height(a_mip), a_shape.Depth(a_mip) };
		}

		// ---- The context's work as BasicRHI commands -------------------------------------------------------------------------------

		/** @brief One resource an operation uses, with the layout, access and stages its command needs. */
		struct Use
		{
			NativeResource* record = nullptr;
			rhi::TextureSubresourceRange range{};
			rhi::ResourceLayout layout = rhi::ResourceLayout::Common;
			rhi::ResourceAccessType access = rhi::ResourceAccessType::Common;
			rhi::ResourceSyncState sync = rhi::ResourceSyncState::All;
		};

		/** @brief Two planes for a depth-stencil image, since D3D12 transitions each plane. */
		uint32_t PlanesOf(const NativeResource& a_record)
		{
			return (a_record.shape.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? 2u : 1u;
		}

		rhi::TextureSubresourceRange AllOf(const NativeResource& a_record)
		{
			return { 0, a_record.shape.mips, 0, a_record.shape.layers, 0, PlanesOf(a_record) };
		}

		/** @brief One subresource by D3D11 index, mip + layer * mips. */
		rhi::TextureSubresourceRange SubresourceOf(const NativeResource& a_record, uint32_t a_sub)
		{
			return { a_sub % a_record.shape.mips, 1, a_sub / a_record.shape.mips, 1, 0, PlanesOf(a_record) };
		}

		/**
		 * @brief Moves each resource from Common into its use, or back. These barriers order the operation with the graph's epochs.
		 */
		void Transition(rhi::CommandList& a_list, std::span<const Use> a_uses, bool a_leave)
		{
			std::vector<rhi::TextureBarrier> textures;
			std::vector<rhi::BufferBarrier> buffers;
			for (const auto& use : a_uses) {
				if (!use.record || !use.record->resource)
					continue;
				const auto before = a_leave ? use : Use{ use.record, use.range };
				const auto after = a_leave ? Use{ use.record, use.range } : use;
				if (use.record->kind == NativeResource::Kind::Image) {
					auto& barrier = textures.emplace_back();
					barrier.texture = use.record->resource->GetHandle();
					barrier.range = use.range;
					barrier.beforeSync = before.sync;
					barrier.afterSync = after.sync;
					barrier.beforeAccess = before.access;
					barrier.afterAccess = after.access;
					barrier.beforeLayout = before.layout;
					barrier.afterLayout = after.layout;
				} else {
					auto& barrier = buffers.emplace_back();
					barrier.buffer = use.record->resource->GetHandle();
					barrier.beforeSync = before.sync;
					barrier.afterSync = after.sync;
					barrier.beforeAccess = before.access;
					barrier.afterAccess = after.access;
				}
			}
			if (!textures.empty() || !buffers.empty())
				a_list.Barriers({ { textures.data(), static_cast<uint32_t>(textures.size()) }, { buffers.data(), static_cast<uint32_t>(buffers.size()) }, {} });
		}

		Use CopySource(NativeResource& a_record, const rhi::TextureSubresourceRange& a_range)
		{
			return { &a_record, a_range, rhi::ResourceLayout::CopySource, rhi::ResourceAccessType::CopySource, rhi::ResourceSyncState::Copy };
		}

		Use CopyDest(NativeResource& a_record, const rhi::TextureSubresourceRange& a_range)
		{
			return { &a_record, a_range, rhi::ResourceLayout::CopyDest, rhi::ResourceAccessType::CopyDest, rhi::ResourceSyncState::Copy };
		}

		/** @brief An image's range for ORG's external client. */
		org::RangeSpec RangeOf(const NativeResource& a_record, const rhi::TextureSubresourceRange& a_range)
		{
			org::RangeSpec spec{};
			const auto& shape = a_record.shape;
			if (a_range.baseMip != 0 || a_range.mipCount != shape.mips) {
				spec.mipLower = { org::BoundType::From, a_range.baseMip };
				spec.mipUpper = { org::BoundType::UpTo, a_range.baseMip + a_range.mipCount - 1 };
			}
			if (a_range.baseLayer != 0 || a_range.layerCount != shape.layers) {
				spec.sliceLower = { org::BoundType::From, a_range.baseLayer };
				spec.sliceUpper = { org::BoundType::UpTo, a_range.baseLayer + a_range.layerCount - 1 };
			}
			return spec;
		}

		/** @brief An operation's commands. The resources come in the uses' order. */
		using OpRecord = std::function<void(rhi::CommandList&, const Immediate::Descriptors&, std::span<const rhi::Resource>)>;

		/**
		 * @brief Runs one context operation with BasicRHI commands through the external client. Uses with a graph resource go through
		 * its ledger, others move from Common and back. Returns 0 only after the client's shutdown.
		 */
		uint64_t RunOp(const std::string& a_label, std::span<const Use> a_uses, const OpRecord& a_record, std::vector<std::shared_ptr<const void>> a_keep,
			bool* a_routed = nullptr)
		{
			std::vector<rhi::Resource> handles;
			std::vector<Use> moved;
			std::vector<NativeResource*> used, untracked;
			std::vector<::RenderGraph::ExternalWork::Access> accesses;
			handles.reserve(a_uses.size());
			const auto note = [](std::vector<NativeResource*>& a_into, NativeResource* a_record) {
				if (std::ranges::find(a_into, a_record) == a_into.end())
					a_into.push_back(a_record);
			};
			for (const auto& use : a_uses) {
				if (!use.record || !use.record->resource) {
					handles.emplace_back();
					continue;
				}
				note(used, use.record);
				auto ledger = LedgerResourceOf(*use.record);
				if (ledger) {
					handles.push_back(ledger->GetAPIResource());
					accesses.push_back({ ledger.get(), RangeOf(*use.record, use.range), { use.access, use.layout, use.sync } });
					a_keep.push_back(std::move(ledger));
					continue;
				}
				handles.push_back(use.record->resource.Get());
				moved.push_back(use);
				note(untracked, use.record);
			}
			if (a_routed)
				*a_routed = !accesses.empty();
			const auto body = [&](rhi::CommandList& a_list, const Immediate::Descriptors& a_descriptors) {
				Transition(a_list, moved, false);
				a_record(a_list, a_descriptors, handles);
				Transition(a_list, moved, true);
			};
			Immediate::Work work;
			work.accesses = accesses;
			work.untracked = untracked;
			work.used = used;
			return g_device->immediate.Run(a_label, work, body, std::move(a_keep));
		}

		/** @brief A footprint extent, in whole blocks for block-compressed formats. */
		uint32_t FootprintExtent(const Shape& a_shape, uint32_t a_texels)
		{
			return a_shape.blocks ? (a_texels + 3u) & ~3u : a_texels;
		}

		/** @brief The regions of one subresource copy between two images. A null a_box copies the whole source subresource. */
		std::pair<rhi::TextureCopyRegion, rhi::TextureCopyRegion> ImageCopyRegions(const NativeResource& a_destination, rhi::ResourceHandle a_destinationHandle,
			uint32_t a_destinationSub, uint32_t a_x, uint32_t a_y, uint32_t a_z, const NativeResource& a_source, rhi::ResourceHandle a_sourceHandle,
			uint32_t a_sourceSub, const D3D11_BOX* a_box)
		{
			const Shape& sourceShape = a_source.shape;
			const uint32_t sourceMip = a_sourceSub % sourceShape.mips;
			rhi::TextureCopyRegion from{};
			from.texture = a_sourceHandle;
			from.mip = sourceMip;
			from.arraySlice = a_sourceSub / sourceShape.mips;
			from.x = a_box ? a_box->left : 0;
			from.y = a_box ? a_box->top : 0;
			from.z = a_box ? a_box->front : 0;
			from.width = a_box ? a_box->right - a_box->left : sourceShape.Width(sourceMip);
			from.height = a_box ? a_box->bottom - a_box->top : sourceShape.Height(sourceMip);
			from.depth = a_box ? a_box->back - a_box->front : sourceShape.Depth(sourceMip);
			rhi::TextureCopyRegion to = from;
			to.texture = a_destinationHandle;
			to.mip = a_destinationSub % a_destination.shape.mips;
			to.arraySlice = a_destinationSub / a_destination.shape.mips;
			to.x = a_x;
			to.y = a_y;
			to.z = a_z;
			return { to, from };
		}

		/** @brief Whether the depth plane holds floats in a buffer copy. True for D32S8X24, and for D24 where BasicRHI stores it as D32S8. */
		bool DepthPlaneFloat(const NativeResource& a_image)
		{
			if (a_image.shape.blockBytes == 8)
				return true;
			return !OnD3D12() && a_image.image.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
		}

		void BounceBarrier(rhi::CommandList& a_list, const NativeResource& a_bounce, rhi::ResourceAccessType a_before, rhi::ResourceAccessType a_after)
		{
			rhi::BufferBarrier barrier{};
			barrier.buffer = a_bounce.resource->GetHandle();
			barrier.beforeSync = a_before == rhi::ResourceAccessType::Common ? rhi::ResourceSyncState::All : rhi::ResourceSyncState::Copy;
			barrier.afterSync = rhi::ResourceSyncState::Copy;
			barrier.beforeAccess = a_before;
			barrier.afterAccess = a_after;
			a_list.Barriers({ {}, { &barrier, 1 }, {} });
		}

		struct StagingRows
		{
			uint32_t rows = 0, rowBytes = 0, depth = 1;
			uint64_t start = 0;
			uint32_t rowPitch = 0, depthPitch = 0;
			uint64_t At(uint32_t a_z, uint32_t a_row) const { return start + uint64_t(a_z) * depthPitch + uint64_t(a_row) * rowPitch; }
		};

		StagingRows RowsOf(const NativeResource& a_staging, uint32_t a_sub, uint32_t a_x, uint32_t a_y, uint32_t a_z, uint32_t a_width, uint32_t a_height, uint32_t a_depth)
		{
			const auto& shape = a_staging.shape;
			const auto& entry = a_staging.layout[a_sub];
			const uint32_t block = shape.BlockSize();
			StagingRows rows;
			rows.rows = shape.Rows(a_height);
			rows.rowBytes = shape.RowBytes(a_width);
			rows.depth = a_depth;
			rows.rowPitch = entry.rowPitch;
			rows.depthPitch = entry.depthPitch;
			rows.start = entry.offset + uint64_t(a_z) * entry.depthPitch + uint64_t(a_y / block) * entry.rowPitch + uint64_t(a_x / block) * shape.blockBytes;
			return rows;
		}

		/** @brief The bounce buffer's row pitch, with rows aligned to 256 bytes as every backend's footprints require. */
		uint32_t BouncePitch(const StagingRows& a_rows)
		{
			return (a_rows.rowBytes + 255u) & ~255u;
		}

		/**
		 * @brief Records one subresource region copy between any two of buffer, image and staging texture. Footprints need aligned
		 * offsets, so a staging region inside a subresource goes through a_bounce. False when the pair cannot be copied.
		 */
		bool RecordCopyRhi(rhi::CommandList& a_list, const NativeResource& a_destination, rhi::ResourceHandle a_destinationHandle, uint32_t a_destinationSub,
			uint32_t a_x, uint32_t a_y, uint32_t a_z, const NativeResource& a_source, rhi::ResourceHandle a_sourceHandle, uint32_t a_sourceSub, const D3D11_BOX* a_box,
			const NativeResource* a_bounce)
		{
			using Kind = NativeResource::Kind;
			const bool sourceBuffer = a_source.kind == Kind::Buffer;
			const bool destinationBuffer = a_destination.kind == Kind::Buffer;
			if (sourceBuffer || destinationBuffer) {
				if (!sourceBuffer || !destinationBuffer)
					return false;
				const uint64_t from = a_box ? a_box->left : 0;
				const uint64_t size = a_box ? a_box->right - a_box->left : (std::min)(a_source.size, a_destination.size);
				if (from + size > a_source.size || a_x + size > a_destination.size)
					return false;
				a_list.CopyBufferRegion(a_destinationHandle, a_x, a_sourceHandle, from, size);
				return true;
			}
			const Shape& sourceShape = a_source.shape;
			const Shape& destinationShape = a_destination.shape;
			if (a_sourceSub >= sourceShape.Subresources() || a_destinationSub >= destinationShape.Subresources())
				return false;
			const uint32_t sourceMip = a_sourceSub % sourceShape.mips;
			const uint32_t sourceLayer = a_sourceSub / sourceShape.mips;
			const uint32_t destinationMip = a_destinationSub % destinationShape.mips;
			const uint32_t destinationLayer = a_destinationSub / destinationShape.mips;
			const uint32_t width = a_box ? a_box->right - a_box->left : sourceShape.Width(sourceMip);
			const uint32_t height = a_box ? a_box->bottom - a_box->top : sourceShape.Height(sourceMip);
			const uint32_t depth = a_box ? a_box->back - a_box->front : sourceShape.Depth(sourceMip);
			if (a_source.kind == Kind::Image && a_destination.kind == Kind::Image) {
				const auto [to, from] = ImageCopyRegions(a_destination, a_destinationHandle, a_destinationSub, a_x, a_y, a_z, a_source, a_sourceHandle, a_sourceSub, a_box);
				a_list.CopyTextureRegion(to, from);
				return true;
			}
			if (a_source.kind == Kind::Image && a_destination.kind == Kind::Staging) {
				rhi::BufferTextureCopyFootprint copy{};
				copy.texture = a_sourceHandle;
				copy.buffer = a_destinationHandle;
				copy.mip = sourceMip;
				copy.arraySlice = sourceLayer;
				const auto& entry = a_destination.layout[a_destinationSub];
				if (sourceShape.aspects != VK_IMAGE_ASPECT_COLOR_BIT) {
					// D3D11 copies a depth subresource whole. With stencil each plane goes to its own area.
					if (a_box || a_x || a_y || a_z)
						return false;
					copy.footprint = { entry.offset, entry.rowPitch, sourceShape.Width(sourceMip), sourceShape.Height(sourceMip), 1 };
					if (!StencilTexelBytes(sourceShape)) {
						a_list.CopyTextureToBuffer(copy);
						return true;
					}
					if (!StencilTexelBytes(destinationShape))
						return false;
					copy.footprint.offset = entry.depthOffset;
					copy.footprint.rowPitch = entry.depthRowPitch;
					copy.plane = 0;
					a_list.CopyTextureToBuffer(copy);
					copy.footprint.offset = entry.stencilOffset;
					copy.footprint.rowPitch = entry.stencilRowPitch;
					copy.plane = 1;
					a_list.CopyTextureToBuffer(copy);
					return true;
				}
				copy.x = a_box ? a_box->left : 0;
				copy.y = a_box ? a_box->top : 0;
				copy.z = a_box ? a_box->front : 0;
				copy.footprint = { entry.offset, entry.rowPitch, FootprintExtent(sourceShape, width), FootprintExtent(sourceShape, height), depth };
				if (!a_x && !a_y && !a_z) {
					a_list.CopyTextureToBuffer(copy);
					return true;
				}
				if (!a_bounce)
					return false;
				const auto rows = RowsOf(a_destination, a_destinationSub, a_x, a_y, a_z, width, height, depth);
				const uint32_t pitch = BouncePitch(rows);
				copy.buffer = a_bounce->resource->GetHandle();
				copy.footprint.offset = 0;
				copy.footprint.rowPitch = pitch;
				BounceBarrier(a_list, *a_bounce, rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopyDest);
				a_list.CopyTextureToBuffer(copy);
				BounceBarrier(a_list, *a_bounce, rhi::ResourceAccessType::CopyDest, rhi::ResourceAccessType::CopySource);
				for (uint32_t z = 0; z < rows.depth; ++z)
					for (uint32_t row = 0; row < rows.rows; ++row)
						a_list.CopyBufferRegion(a_destinationHandle, rows.At(z, row), copy.buffer, (uint64_t(z) * rows.rows + row) * pitch, rows.rowBytes);
				return true;
			}
			if (a_source.kind == Kind::Staging && a_destination.kind == Kind::Image) {
				rhi::BufferTextureCopyFootprint copy{};
				copy.texture = a_destinationHandle;
				copy.buffer = a_sourceHandle;
				copy.mip = destinationMip;
				copy.arraySlice = destinationLayer;
				copy.x = a_x;
				copy.y = a_y;
				copy.z = a_z;
				const auto& entry = a_source.layout[a_sourceSub];
				if (destinationShape.aspects != VK_IMAGE_ASPECT_COLOR_BIT) {
					if (a_box || a_x || a_y || a_z)
						return false;
					copy.footprint = { entry.offset, entry.rowPitch, destinationShape.Width(destinationMip), destinationShape.Height(destinationMip), 1 };
					if (!StencilTexelBytes(destinationShape)) {
						a_list.CopyBufferToTexture(copy);
						return true;
					}
					if (!StencilTexelBytes(sourceShape))
						return false;
					copy.footprint.offset = entry.depthOffset;
					copy.footprint.rowPitch = entry.depthRowPitch;
					copy.plane = 0;
					a_list.CopyBufferToTexture(copy);
					copy.footprint.offset = entry.stencilOffset;
					copy.footprint.rowPitch = entry.stencilRowPitch;
					copy.plane = 1;
					a_list.CopyBufferToTexture(copy);
					return true;
				}
				copy.footprint = { entry.offset, entry.rowPitch, FootprintExtent(sourceShape, width), FootprintExtent(sourceShape, height), depth };
				if (!a_box || (!a_box->left && !a_box->top && !a_box->front)) {
					a_list.CopyBufferToTexture(copy);
					return true;
				}
				if (!a_bounce)
					return false;
				const auto rows = RowsOf(a_source, a_sourceSub, a_box->left, a_box->top, a_box->front, width, height, depth);
				const uint32_t pitch = BouncePitch(rows);
				const auto bounce = a_bounce->resource->GetHandle();
				BounceBarrier(a_list, *a_bounce, rhi::ResourceAccessType::Common, rhi::ResourceAccessType::CopyDest);
				for (uint32_t z = 0; z < rows.depth; ++z)
					for (uint32_t row = 0; row < rows.rows; ++row)
						a_list.CopyBufferRegion(bounce, (uint64_t(z) * rows.rows + row) * pitch, a_sourceHandle, rows.At(z, row), rows.rowBytes);
				BounceBarrier(a_list, *a_bounce, rhi::ResourceAccessType::CopyDest, rhi::ResourceAccessType::CopySource);
				copy.buffer = bounce;
				copy.footprint.offset = 0;
				copy.footprint.rowPitch = pitch;
				a_list.CopyBufferToTexture(copy);
				return true;
			}
			// Texel sizes must match for a staging to staging copy.
			if (sourceShape.blockBytes != destinationShape.blockBytes || sourceShape.BlockSize() != destinationShape.BlockSize())
				return false;
			const auto& sourceEntry = a_source.layout[a_sourceSub];
			const auto& destinationEntry = a_destination.layout[a_destinationSub];
			if (!a_box && !a_x && !a_y && !a_z && sourceEntry.rowPitch == destinationEntry.rowPitch && sourceEntry.depthPitch == destinationEntry.depthPitch) {
				a_list.CopyBufferRegion(a_destinationHandle, destinationEntry.offset, a_sourceHandle, sourceEntry.offset,
					uint64_t(sourceEntry.depthPitch) * sourceShape.Depth(sourceMip));
				return true;
			}
			const auto from = RowsOf(a_source, a_sourceSub, a_box ? a_box->left : 0, a_box ? a_box->top : 0, a_box ? a_box->front : 0, width, height, depth);
			const auto to = RowsOf(a_destination, a_destinationSub, a_x, a_y, a_z, width, height, depth);
			for (uint32_t z = 0; z < from.depth; ++z)
				for (uint32_t row = 0; row < from.rows; ++row)
					a_list.CopyBufferRegion(a_destinationHandle, to.At(z, row), a_sourceHandle, from.At(z, row), from.rowBytes);
			return true;
		}

		/** @brief Converts depth-stencil staging between CPU texels and plane areas. The caller must have waited for the GPU. */
		void RepackStaging(NativeResource& a_staging, bool a_unpack, bool a_float, uint32_t a_sub = UINT32_MAX)
		{
			const uint32_t texel = StencilTexelBytes(a_staging.shape);
			if (!texel)
				return;
			void* data = nullptr;
			a_staging.resource->Map(&data);
			if (!data)
				return;
			auto* base = static_cast<std::byte*>(data);
			for (uint32_t sub = 0; sub < a_staging.layout.size(); ++sub) {
				if (a_sub != UINT32_MAX && sub != a_sub)
					continue;
				const auto& entry = a_staging.layout[sub];
				const uint32_t mip = sub % a_staging.shape.mips;
				RepackDepthStencil(a_unpack, texel, a_float, base + entry.offset, entry.rowPitch, base + entry.depthOffset, entry.depthRowPitch, base + entry.stencilOffset,
					entry.stencilRowPitch, a_staging.shape.Width(mip), a_staging.shape.Height(mip));
			}
			a_staging.resource->Unmap(0, a_unpack ? 0 : a_staging.size);
		}

		/** @brief Rebuilds the CPU texels from the planes if a copy wrote them since. */
		void UnpackStagingIfNewer(NativeResource& a_staging)
		{
			if (a_staging.planesNewer.exchange(false, std::memory_order_acq_rel))
				RepackStaging(a_staging, true, a_staging.planeFloat);
		}

		/**
		 * @brief A copy through RunOp, of one subresource or with a_all every shared one. a_recorded is false when native mode cannot
		 * make that copy. Returns 0 when nothing was submitted.
		 */
		uint64_t CopyRhi(const std::string& a_label, const std::shared_ptr<NativeResource>& a_destination, const std::shared_ptr<NativeResource>& a_source, bool a_all,
			uint32_t a_destinationSub, uint32_t a_x, uint32_t a_y, uint32_t a_z, uint32_t a_sourceSub, const D3D11_BOX* a_box, bool& a_recorded, bool* a_routed)
		{
			using Kind = NativeResource::Kind;
			a_recorded = true;
			const bool buffers = a_destination->kind == Kind::Buffer || a_source->kind == Kind::Buffer;
			const uint32_t count = buffers || !a_all ? 1u : (std::min)(a_destination->shape.Subresources(), a_source->shape.Subresources());
			const auto rangeOf = [&](NativeResource& a_record, uint32_t a_sub) {
				if (a_record.kind != Kind::Image)
					return rhi::TextureSubresourceRange{};
				return a_all ? AllOf(a_record) : SubresourceOf(a_record, a_sub);
			};
			if (!buffers && !a_all && (a_sourceSub >= a_source->shape.Subresources() || a_destinationSub >= a_destination->shape.Subresources())) {
				a_recorded = false;
				return 0;
			}
			std::vector<std::shared_ptr<const void>> keep{ a_destination, a_source };
			std::shared_ptr<NativeResource> bounce;
			if (!buffers && !a_all) {
				const bool intoStaging = a_source->kind == Kind::Image && a_destination->kind == Kind::Staging && (a_x || a_y || a_z);
				const bool fromStaging = a_source->kind == Kind::Staging && a_destination->kind == Kind::Image && a_box && (a_box->left || a_box->top || a_box->front);
				if (intoStaging || fromStaging) {
					const auto& staging = intoStaging ? *a_destination : *a_source;
					const uint32_t sub = intoStaging ? a_destinationSub : a_sourceSub;
					const uint32_t mip = a_sourceSub % a_source->shape.mips;
					const uint32_t width = a_box ? a_box->right - a_box->left : a_source->shape.Width(mip);
					const uint32_t height = a_box ? a_box->bottom - a_box->top : a_source->shape.Height(mip);
					const uint32_t depth = a_box ? a_box->back - a_box->front : a_source->shape.Depth(mip);
					const auto rows = RowsOf(staging, sub, 0, 0, 0, width, height, depth);
					bounce = g_device->MakeBounce(uint64_t(BouncePitch(rows)) * rows.rows * rows.depth);
					if (!bounce) {
						a_recorded = false;
						return 0;
					}
					keep.push_back(bounce);
				}
			}
			// Depth-stencil staging must have its planes made from the CPU texels before a copy reads them.
			const bool stencilSource = a_source->kind == Kind::Staging && StencilTexelBytes(a_source->shape);
			const bool stencilDestination = a_destination->kind == Kind::Staging && StencilTexelBytes(a_destination->shape);
			if (stencilSource) {
				if (!g_device->immediate.Wait(a_source->lastUse.load(std::memory_order_acquire))) {
					a_recorded = false;
					return 0;
				}
				UnpackStagingIfNewer(*a_source);
				if (a_destination->kind == Kind::Image)
					RepackStaging(*a_source, false, DepthPlaneFloat(*a_destination), a_all ? UINT32_MAX : a_sourceSub);
			}
			if (stencilDestination && a_source->kind == Kind::Staging) {
				// Make the destination's CPU texels from its planes first, so a partial region keeps the rest.
				if (!g_device->immediate.Wait(a_destination->lastUse.load(std::memory_order_acquire))) {
					a_recorded = false;
					return 0;
				}
				UnpackStagingIfNewer(*a_destination);
			}
			const std::array uses{ CopySource(*a_source, rangeOf(*a_source, a_sourceSub)), CopyDest(*a_destination, rangeOf(*a_destination, a_destinationSub)) };
			const uint64_t value = RunOp(
				a_label, uses,
				[&](rhi::CommandList& a_list, const Immediate::Descriptors&, std::span<const rhi::Resource> a_handles) {
					const auto from = a_handles[0].GetHandle();
					const auto to = a_handles[1].GetHandle();
					if (!a_all || buffers) {
						a_recorded = RecordCopyRhi(a_list, *a_destination, to, a_destinationSub, a_x, a_y, a_z, *a_source, from, a_sourceSub, a_box, bounce.get());
						return;
					}
					for (uint32_t sub = 0; sub < count; ++sub)
						a_recorded = RecordCopyRhi(a_list, *a_destination, to, sub, 0, 0, 0, *a_source, from, sub, nullptr, nullptr) && a_recorded;
				},
				std::move(keep), a_routed);
			if (value && a_recorded && stencilDestination && a_source->kind == Kind::Image) {
				a_destination->planeFloat = DepthPlaneFloat(*a_source);
				a_destination->planesNewer.store(true, std::memory_order_release);
			}
			return value;
		}

		/** @brief The use a clear writes, as render target or as depth on both planes. */
		Use ClearUse(NativeResource& a_record, const rhi::TextureSubresourceRange& a_range, bool a_depth)
		{
			const rhi::TextureSubresourceRange range{ a_range.baseMip, a_range.mipCount, a_range.baseLayer, a_range.layerCount, 0, PlanesOf(a_record) };
			if (a_depth)
				return { &a_record, range, rhi::ResourceLayout::DepthReadWrite, rhi::ResourceAccessType::DepthReadWrite, rhi::ResourceSyncState::DepthStencil };
			return { &a_record, range, rhi::ResourceLayout::RenderTarget, rhi::ResourceAccessType::RenderTarget, rhi::ResourceSyncState::RenderTarget };
		}

		Use UnorderedUse(NativeResource& a_record, const rhi::TextureSubresourceRange& a_range)
		{
			return { &a_record, a_range, rhi::ResourceLayout::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceSyncState::ClearUnorderedAccessView };
		}

		/** @brief Clears image subresources through an RTV or DSV. The image must already be in its clear state. */
		void ClearImageRhi(rhi::CommandList& a_list, const Immediate::Descriptors& a_descriptors, const NativeResource& a_record, rhi::ResourceHandle a_handle,
			const rhi::TextureSubresourceRange& a_range, const float a_color[4], bool a_depth, bool a_clearDepth, bool a_clearStencil, float a_depthValue,
			uint8_t a_stencil)
		{
			const auto& shape = a_record.shape;
			const bool layered = shape.layers > 1;
			// One mip at a time, since a view names one mip.
			for (uint32_t mip = a_range.baseMip; mip < a_range.baseMip + a_range.mipCount; ++mip) {
				const rhi::TextureSubresourceRange range{ mip, 1, a_range.baseLayer, a_range.layerCount, 0, PlanesOf(a_record) };
				if (a_depth) {
					rhi::DsvDesc desc{};
					desc.dimension = layered ? rhi::DsvDim::Texture2DArray : rhi::DsvDim::Texture2D;
					desc.range = range;
					desc.formatOverride = shape.viewFormat;
					// Reusing the slot's DSV is safe because D3D12 and Vulkan both read it when the clear is recorded.
					if (g_device->Rhi().CreateDepthStencilView(a_descriptors.dsv, a_handle, desc) != rhi::Result::Ok)
						continue;
					a_list.ClearDepthStencilView(a_descriptors.dsv, a_clearDepth, a_clearStencil, a_depthValue, a_stencil);
				} else {
					rhi::RtvDesc desc{};
					desc.dimension = shape.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? rhi::RtvDim::Texture3D :
					                 layered                                               ? rhi::RtvDim::Texture2DArray :
					                                                                         rhi::RtvDim::Texture2D;
					desc.range = range;
					if (g_device->Rhi().CreateRenderTargetView(a_descriptors.rtv, a_handle, desc) != rhi::Result::Ok)
						continue;
					rhi::ClearValue value{};
					value.type = rhi::ClearValueType::Color;
					value.format = shape.viewFormat;
					std::memcpy(value.rgba, a_color, sizeof(value.rgba));
					a_list.ClearRenderTargetView(a_descriptors.rtv, value);
				}
			}
		}

		/** @brief Clears image subresources or a buffer range through a UAV, one view per mip. The resource must be in UAV state. */
		void ClearUnorderedRhi(rhi::CommandList& a_list, const Immediate::Descriptors& a_descriptors, const NativeResource& a_record, const rhi::Resource& a_resource,
			const rhi::TextureSubresourceRange& a_range, uint64_t a_offset, uint64_t a_size, const uint32_t a_uints[4], const float a_floats[4])
		{
			static_assert(Immediate::kUavsPerSlot >= D3D11_REQ_MIP_LEVELS, "a slot holds a UAV per mip of any D3D11 texture");
			const bool image = a_record.kind == NativeResource::Kind::Image;
			const auto handle = a_resource.GetHandle();
			// Each clear needs its own descriptor pair, since D3D12 reads a UAV clear's descriptor when the list executes.
			uint32_t next = 0;
			const auto clear = [&](const rhi::UavDesc& a_desc) {
				if (next >= Immediate::kUavsPerSlot)
					return;
				const rhi::DescriptorSlot shaderVisible{ a_descriptors.uavShaderVisible.heap, a_descriptors.uavShaderVisible.index + next };
				const rhi::DescriptorSlot cpu{ a_descriptors.uavCpu.heap, a_descriptors.uavCpu.index + next };
				++next;
				if (g_device->Rhi().CreateUnorderedAccessView(shaderVisible, handle, a_desc) != rhi::Result::Ok ||
					g_device->Rhi().CreateUnorderedAccessView(cpu, handle, a_desc) != rhi::Result::Ok)
					return;
				const rhi::UavClearInfo info{ shaderVisible, cpu, a_resource };
				if (a_floats) {
					rhi::UavClearFloat value{};
					std::memcpy(value.v, a_floats, sizeof(value.v));
					a_list.ClearUavFloat(info, value);
				} else {
					rhi::UavClearUint value{};
					std::memcpy(value.v, a_uints, sizeof(value.v));
					a_list.ClearUavUint(info, value);
				}
			};
			if (!image) {
				rhi::UavDesc desc{};
				desc.dimension = rhi::UavDim::Buffer;
				desc.buffer.kind = rhi::BufferViewKind::Raw;
				desc.buffer.firstElement = a_offset / 4;
				desc.buffer.numElements = static_cast<uint32_t>(a_size / 4);
				clear(desc);
				return;
			}
			const auto& shape = a_record.shape;
			for (uint32_t mip = a_range.baseMip; mip < a_range.baseMip + a_range.mipCount; ++mip) {
				rhi::UavDesc desc{};
				if (shape.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
					desc.dimension = rhi::UavDim::Texture3D;
					desc.texture3D.mipSlice = mip;
					desc.texture3D.firstWSlice = 0;
					desc.texture3D.wSize = shape.Depth(mip);
				} else if (shape.layers > 1) {
					desc.dimension = rhi::UavDim::Texture2DArray;
					desc.texture2DArray.mipSlice = mip;
					desc.texture2DArray.firstArraySlice = a_range.baseLayer;
					desc.texture2DArray.arraySize = a_range.layerCount;
					// desc{} zeroes only the union's first member, the buffer's.
					desc.texture2DArray.planeSlice = 0;
				} else {
					desc.dimension = rhi::UavDim::Texture2D;
					desc.texture2D.mipSlice = mip;
					desc.texture2D.planeSlice = 0;
				}
				clear(desc);
			}
		}

		/** @brief A Vulkan subresource range as BasicRHI's, with both planes of a depth-stencil range. */
		rhi::TextureSubresourceRange ToRhiRange(const VkImageSubresourceRange& a_range)
		{
			const uint32_t planes = (a_range.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && (a_range.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ? 2u : 1u;
			return { a_range.baseMipLevel, a_range.levelCount, a_range.baseArrayLayer, a_range.layerCount, 0, planes };
		}

		/** @brief The memory Map hands out. A DYNAMIC image maps its upload side. */
		NativeResource* CpuSide(NativeResource& a_record)
		{
			NativeResource* cpu = a_record.kind == NativeResource::Kind::Image ? a_record.upload.get() : &a_record;
			return cpu && cpu->hostVisible ? cpu : nullptr;
		}

		struct GraphWaits
		{
			std::atomic<uint64_t> count{ 0 };
			std::atomic<uint64_t> microseconds{ 0 };
		};

		GraphWaits& GetGraphWaits()
		{
			static GraphWaits waits;
			return waits;
		}

		/**
		 * @brief Before the CPU touches a buffer the graph imported, waits for graph work up to the last sealed frame boundary.
		 * A mid-frame wait must name only sealed work, since later work of the frame may be queued behind this thread.
		 * Buffers the graph never imported need no wait, since native draws take constants from captures.
		 */
		HRESULT WaitGraphReaders(const NativeResource& a_record, bool a_doNotWait)
		{
			if (a_record.kind != NativeResource::Kind::Buffer || !a_record.graphShared.load(std::memory_order_acquire))
				return S_OK;
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return S_OK;
			const auto sealed = host->SealedPoint();
			if (sealed.Reached())
				return S_OK;
			if (a_doNotWait)
				return DXGI_ERROR_WAS_STILL_DRAWING;
			const auto start = std::chrono::steady_clock::now();
			if (!sealed.Wait())
				return DXGI_ERROR_DEVICE_REMOVED;
			const auto us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
			auto& waits = GetGraphWaits();
			const uint64_t count = waits.count.fetch_add(1, std::memory_order_relaxed) + 1;
			const uint64_t total = waits.microseconds.fetch_add(us, std::memory_order_relaxed) + us;
			// Log the first few, then at powers of two.
			if (count <= 8 || std::has_single_bit(count))
				logger::info("[ORG native] CPU access waited {:.3f} ms for graph readers of a buffer (bind {:#x}, usage {}, {} bytes); {} waits so far, {:.1f} ms in all",
					us / 1000.0, a_record.bindFlags, static_cast<uint32_t>(a_record.d3dUsage), a_record.size, count, total / 1000.0);
			return S_OK;
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
			if (a_type != D3D11_MAP_READ)
				MarkWritten(*record);
			// A read waits for the GPU's writes and a write for its reads. NO_OVERWRITE promises not to touch what is in use.
			// DYNAMIC buffers are not renamed.
			if (a_type != D3D11_MAP_WRITE_NO_OVERWRITE) {
				const uint64_t use = cpu->lastUse.load(std::memory_order_acquire);
				if (!g_device->immediate.Done(use)) {
					if (a_flags & D3D11_MAP_FLAG_DO_NOT_WAIT)
						return DXGI_ERROR_WAS_STILL_DRAWING;
					if (!g_device->immediate.Wait(use))
						return DXGI_ERROR_DEVICE_REMOVED;
				}
				if (const HRESULT graph = WaitGraphReaders(*cpu, (a_flags & D3D11_MAP_FLAG_DO_NOT_WAIT) != 0); FAILED(graph))
					return graph;
			}
			// The copy is done by now, so the staging texels can be rebuilt from the planes.
			if (cpu->kind == NativeResource::Kind::Staging && a_type != D3D11_MAP_WRITE_NO_OVERWRITE)
				UnpackStagingIfNewer(*cpu);
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
			// Readback memory flushes nothing back. Upload memory flushes what the CPU wrote.
			cpu->resource->Unmap(0, cpu->readback ? 0 : cpu->size);
			// A DYNAMIC texture's written subresource goes to the image now, in the context's order.
			if (record->kind == NativeResource::Kind::Image && record->upload && a_subresource < record->shape.Subresources()) {
				const auto image = record;
				const auto upload = record->upload;
				bool recorded = false, routed = false;
				CopyRhi("unmap (dynamic upload) " + image->name, image, upload, false, a_subresource, 0, 0, 0, a_subresource, nullptr, recorded, &routed);
				NoteRun("Unmap (dynamic upload)", *image, routed);
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
			// Image updates are noted where they run, in NoteRun.
			if (record->kind != NativeResource::Kind::Image)
				MarkWritten(*record);
			if (record->kind == NativeResource::Kind::Buffer) {
				const uint64_t from = a_box ? a_box->left : 0;
				const uint64_t size = a_box ? a_box->right - a_box->left : record->size;
				if (!size || from + size > record->size)
					return;
				// Write directly when the GPU is done with the buffer, else through an upload buffer in the context's order.
				if (record->hostVisible && !record->readback && g_device->immediate.Done(record->lastUse.load(std::memory_order_acquire)) &&
					WaitGraphReaders(*record, false) == S_OK) {
					void* data = nullptr;
					record->resource->Map(&data);
					if (data) {
						std::memcpy(static_cast<std::byte*>(data) + from, a_data, size);
						record->resource->Unmap(from, size);
					}
					return;
				}
				NoteRaw("UpdateSubresource (buffer)", *record);
				auto upload = g_device->MakeUpload(size);
				if (!upload)
					return;
				void* data = nullptr;
				upload->resource->Map(&data);
				if (!data)
					return;
				std::memcpy(data, a_data, size);
				upload->resource->Unmap(0, size);
				const std::array uses{ CopySource(*upload, {}), CopyDest(*record, {}) };
				RunOp(
					"update buffer " + record->name, uses,
					[&](rhi::CommandList& a_list, const Immediate::Descriptors&, std::span<const rhi::Resource> a_handles) {
						a_list.CopyBufferRegion(a_handles[1].GetHandle(), from, a_handles[0].GetHandle(), 0, size);
					},
					{ upload, record });
				return;
			}

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
				if (!g_device->immediate.Wait(record->lastUse.load(std::memory_order_acquire)))
					return;
				UnpackStagingIfNewer(*record);
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
			NoteWrite(*record);
			// D3D11 takes no box for depth-stencil, so the whole subresource is split into the two planes.
			if (const uint32_t texel = StencilTexelBytes(shape)) {
				const uint32_t width = shape.Width(mip), height = shape.Height(mip);
				const uint32_t depthPitch = (width * 4u + 255u) & ~255u, stencilPitch = (width + 255u) & ~255u;
				const uint64_t stencilOffset = (uint64_t(depthPitch) * height + 511) & ~uint64_t(511);
				auto planes = g_device->MakeUpload(stencilOffset + uint64_t(stencilPitch) * height);
				if (!planes)
					return;
				void* mapped = nullptr;
				planes->resource->Map(&mapped);
				if (!mapped)
					return;
				auto* base = static_cast<std::byte*>(mapped);
				// Packing only reads the D3D11 texels, so the cast is safe.
				RepackDepthStencil(false, texel, DepthPlaneFloat(*record), const_cast<std::byte*>(static_cast<const std::byte*>(a_data)), a_rowPitch, base, depthPitch,
					base + stencilOffset, stencilPitch, width, height);
				planes->resource->Unmap(0, planes->size);
				bool routed = false;
				const std::array uses{ CopySource(*planes, {}), CopyDest(*record, SubresourceOf(*record, a_subresource)) };
				RunOp(
					"update depth-stencil " + record->name, uses,
					[&](rhi::CommandList& a_list, const Immediate::Descriptors&, std::span<const rhi::Resource> a_handles) {
						rhi::BufferTextureCopyFootprint copy{};
						copy.texture = a_handles[1].GetHandle();
						copy.buffer = a_handles[0].GetHandle();
						copy.mip = mip;
						copy.arraySlice = a_subresource / shape.mips;
						copy.footprint = { 0, depthPitch, width, height, 1 };
						copy.plane = 0;
						a_list.CopyBufferToTexture(copy);
						copy.footprint = { stencilOffset, stencilPitch, width, height, 1 };
						copy.plane = 1;
						a_list.CopyBufferToTexture(copy);
					},
					{ planes, record }, &routed);
				NoteRun("UpdateSubresource (texture)", *record, routed);
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
			bool routed = false;
			const std::array uses{ CopySource(*upload, {}), CopyDest(*record, SubresourceOf(*record, a_subresource)) };
			RunOp(
				"update texture " + record->name, uses,
				[&](rhi::CommandList& a_list, const Immediate::Descriptors&, std::span<const rhi::Resource> a_handles) {
					rhi::BufferTextureCopyFootprint copy{};
					copy.texture = a_handles[1].GetHandle();
					copy.buffer = a_handles[0].GetHandle();
					copy.mip = mip;
					copy.arraySlice = a_subresource / shape.mips;
					copy.x = static_cast<uint32_t>(at.x);
					copy.y = static_cast<uint32_t>(at.y);
					copy.z = static_cast<uint32_t>(at.z);
					copy.footprint = { 0, rowPitch, FootprintExtent(shape, extent.width), FootprintExtent(shape, extent.height), extent.depth };
					a_list.CopyBufferToTexture(copy);
				},
				{ upload, record }, &routed);
			NoteRun("UpdateSubresource (texture)", *record, routed);
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
			// D3D11's runtime drops a copy onto itself, and Vulkan copies must not overlap.
			if (destination == source && a_destinationSub == a_sourceSub)
				return;
			NoteWrite(*destination);
			bool recorded = false, routed = false;
			CopyRhi("copy region " + source->name + " to " + destination->name, destination, source, false, a_destinationSub, a_x, a_y, a_z, a_sourceSub, a_box, recorded,
				&routed);
			NoteRun("CopySubresourceRegion", *destination, routed);
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
			// D3D11's runtime drops a copy onto itself.
			if (destination == source)
				return;
			NoteWrite(*destination);
			// The skeleton's static copy pass takes this copy on the graph.
			if (Skeleton::Sites::TakeContextCopy(a_destination, a_source)) {
				MarkWritten(*destination);
				return;
			}
			// Diagnostic for a main depth copy the skeleton did not take. Logged once.
			if (destination->name == "Engine::Depth::kPOST_ZPREPASS_COPY") {
				static std::atomic_flag loggedDepthCopy;
				if (!loggedDepthCopy.test_and_set())
					logger::warn("[ORG native] the main depth's copy reached the native context untaken (in a run: {}; destination {}, source {})", FrameAnchors::InRun(),
						static_cast<const void*>(a_destination), static_cast<const void*>(a_source));
			}
			bool recorded = false, routed = false;
			CopyRhi("copy " + source->name + " to " + destination->name, destination, source, true, 0, 0, 0, 0, 0, nullptr, recorded, &routed);
			NoteRun("CopyResource", *destination, routed);
			if (!recorded) {
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::warn("[ORG native] CopyResource from \"{}\" to \"{}\" is not a copy native mode makes (logged once)", source->name, destination->name);
			}
		}

		/** @brief The UNORM twin of an sRGB format, since GenerateMips' scratch UAVs cannot be sRGB. */
		rhi::Format LinearTwin(rhi::Format a_format)
		{
			switch (a_format) {
			case rhi::Format::R8G8B8A8_UNorm_sRGB:
				return rhi::Format::R8G8B8A8_UNorm;
			case rhi::Format::B8G8R8A8_UNorm_sRGB:
				return rhi::Format::B8G8R8A8_UNorm;
			case rhi::Format::B8G8R8X8_UNorm_sRGB:
				return rhi::Format::B8G8R8X8_UNorm;
			default:
				return a_format;
			}
		}

		void ScratchBarrier(rhi::CommandList& a_list, const NativeResource& a_scratch, const Use& a_before, const Use& a_after)
		{
			rhi::TextureBarrier barrier{};
			barrier.texture = a_scratch.resource->GetHandle();
			barrier.range = { 0, a_scratch.shape.mips, 0, a_scratch.shape.layers, 0, 1 };
			barrier.beforeSync = a_before.sync;
			barrier.afterSync = a_after.sync;
			barrier.beforeAccess = a_before.access;
			barrier.afterAccess = a_after.access;
			barrier.beforeLayout = a_before.layout;
			barrier.afterLayout = a_after.layout;
			a_list.Barriers({ { &barrier, 1 }, {}, {} });
		}

		/**
		 * @brief GenerateMips with a compute shader in a UAV scratch texture, since BasicRHI has no blit and engine textures have no UAVs.
		 * sRGB textures are averaged in linear space. False, logged once per reason, when the mips cannot be built.
		 */
		bool GenerateMipsRhi(const std::shared_ptr<NativeResource>& a_record, bool& a_routed)
		{
			const Shape& shape = a_record->shape;
			const auto refuse = [&](std::atomic_flag& a_logged, const char* a_why) {
				if (!a_logged.test_and_set())
					logger::warn("[ORG native] GenerateMips on \"{}\": {}; its mips stay as they are (logged once)", a_record->name, a_why);
				return false;
			};
			if (shape.dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
				static std::atomic_flag logged;
				return refuse(logged, "native mode builds mip chains of 2D textures, arrays and cubes only");
			}
			if (shape.blocks) {
				static std::atomic_flag logged;
				return refuse(logged, "a block-compressed texture has no mip chain to build");
			}
			const bool array = shape.layers > 1;
			const auto& program = array ? g_device->mipsArray : g_device->mips2D;
			if (!program) {
				static std::atomic_flag logged;
				return refuse(logged, "the GenerateMips compute shader is not loaded");
			}
			const rhi::Format format = LinearTwin(shape.format);
			if (!a_record->mipScratch) {
				rhi::FormatSupport support{};
				if (g_device->Rhi().QueryFormatSupport(format, support) != rhi::Result::Ok || !(support.usage & rhi::FU_UnorderedAccess) || support.srvSwizzle) {
					static std::atomic_flag logged;
					return refuse(logged, "its format takes no unordered access view, which the compute shader writes");
				}
				a_record->mipScratch = g_device->MakeMipScratch(*a_record, format);
				if (!a_record->mipScratch) {
					static std::atomic_flag logged;
					return refuse(logged, "its scratch texture could not be made");
				}
			}
			const auto scratch = a_record->mipScratch;
			const auto scratchHandle = scratch->resource->GetHandle();
			const uint32_t mips = shape.mips, layers = shape.layers;
			const uint32_t flags = format != shape.format ? 1u : 0u;  // GenerateMips.hlsl's sRGB bit
			const Use common{ scratch.get(), {} };
			const Use copyDest{ scratch.get(), {}, rhi::ResourceLayout::CopyDest, rhi::ResourceAccessType::CopyDest, rhi::ResourceSyncState::Copy };
			const Use copySource{ scratch.get(), {}, rhi::ResourceLayout::CopySource, rhi::ResourceAccessType::CopySource, rhi::ResourceSyncState::Copy };
			const Use unordered{ scratch.get(), {}, rhi::ResourceLayout::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceSyncState::ComputeShading };
			const std::array uses{ CopySource(*a_record, { 0, 1, 0, layers, 0, 1 }), CopyDest(*a_record, { 1, mips - 1, 0, layers, 0, 1 }) };
			const auto record = [&](rhi::CommandList& a_list, const Immediate::Descriptors& a_descriptors, std::span<const rhi::Resource> a_handles) {
				const auto image = a_handles[0].GetHandle();
				const auto copy = [&](rhi::ResourceHandle a_to, rhi::ResourceHandle a_from, uint32_t a_mip, uint32_t a_layer) {
					rhi::TextureCopyRegion to{};
					to.texture = a_to;
					to.mip = a_mip;
					to.arraySlice = a_layer;
					to.width = shape.Width(a_mip);
					to.height = shape.Height(a_mip);
					rhi::TextureCopyRegion from = to;
					from.texture = a_from;
					a_list.CopyTextureRegion(to, from);
				};
				ScratchBarrier(a_list, *scratch, common, copyDest);
				for (uint32_t layer = 0; layer < layers; ++layer)
					copy(scratchHandle, image, 0, layer);
				ScratchBarrier(a_list, *scratch, copyDest, unordered);
				// The shader reads the scratch mips' UAVs by index from the bound heap.
				const uint32_t first = a_descriptors.uavShaderVisible.index;
				for (uint32_t mip = 0; mip < mips; ++mip) {
					rhi::UavDesc desc{};
					desc.formatOverride = format;
					if (array) {
						desc.dimension = rhi::UavDim::Texture2DArray;
						desc.texture2DArray.mipSlice = mip;
						desc.texture2DArray.firstArraySlice = 0;
						desc.texture2DArray.arraySize = layers;
						desc.texture2DArray.planeSlice = 0;
					} else {
						desc.dimension = rhi::UavDim::Texture2D;
						desc.texture2D.mipSlice = mip;
						desc.texture2D.planeSlice = 0;
					}
					(void)g_device->Rhi().CreateUnorderedAccessView({ a_descriptors.uavShaderVisible.heap, first + mip }, scratchHandle, desc);
				}
				a_list.BindLayout(program->layout->GetHandle());
				a_list.BindPipeline(program->CurrentPipeline());
				for (uint32_t mip = 1; mip < mips; ++mip) {
					// The previous dispatch wrote this one's source.
					if (mip > 1)
						ScratchBarrier(a_list, *scratch, unordered, unordered);
					const std::array<uint32_t, 7> constants{ first + mip - 1, first + mip, shape.Width(mip - 1), shape.Height(mip - 1), shape.Width(mip), shape.Height(mip),
						flags };
					a_list.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, static_cast<uint32_t>(constants.size()), constants.data());
					a_list.Dispatch((shape.Width(mip) + 7) / 8, (shape.Height(mip) + 7) / 8, layers);
				}
				ScratchBarrier(a_list, *scratch, unordered, copySource);
				for (uint32_t mip = 1; mip < mips; ++mip)
					for (uint32_t layer = 0; layer < layers; ++layer)
						copy(image, scratchHandle, mip, layer);
				ScratchBarrier(a_list, *scratch, copySource, common);
			};
			return RunOp("generate mips " + a_record->name, uses, record, { a_record, scratch }, &a_routed) != 0;
		}

		void ContextGenerateMips(ID3D11ShaderResourceView* a_view)
		{
			const auto record = Resolve(a_view);
			if (!record || record->kind != NativeResource::Kind::Image) {
				static std::atomic_flag logged;
				Unknown(logged, "GenerateMips", a_view);
				return;
			}
			NoteWrite(*record);
			const Shape& shape = record->shape;
			if (shape.mips < 2 || shape.aspects != VK_IMAGE_ASPECT_COLOR_BIT)
				return;
			bool routed = false;
			if (GenerateMipsRhi(record, routed))
				NoteRun("GenerateMips", *record, routed);
		}

		/** @brief The image subresources an RTV, DSV or UAV covers. */
		template <class ViewDesc>
		VkImageSubresourceRange ViewRange(const ViewDesc& a_desc, const Shape& a_shape, VkImageAspectFlags a_aspects)
		{
			VkImageSubresourceRange range{ a_aspects, 0, 1, 0, a_shape.layers };
			// RTVs and UAVs number BUFFER 1 and textures from 2, and DSVs have no buffer.
			// After the shift all three number TEXTURE1D 1, TEXTURE1DARRAY 2, TEXTURE2D 3, TEXTURE2DARRAY 4.
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
			default:  // a 3D view, or one not narrowed, covers the whole first mip
				break;
			}
			range.layerCount = (std::min)(range.layerCount, a_shape.layers - (std::min)(range.baseArrayLayer, a_shape.layers));
			return range;
		}

		void ContextClearRenderTargetView(ID3D11RenderTargetView* a_view, const FLOAT a_color[4])
		{
			const auto record = Resolve(a_view);
			if (!record || record->kind != NativeResource::Kind::Image || !a_color) {
				static std::atomic_flag logged;
				Unknown(logged, "ClearRenderTargetView", a_view);
				return;
			}
			// The skeleton's static clear pass takes this clear on the graph.
			if (Skeleton::Sites::TakeContextColourClear(a_view, a_color)) {
				MarkWritten(*record);
				return;
			}
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			const auto range = ToRhiRange(ViewRange(desc, record->shape, VK_IMAGE_ASPECT_COLOR_BIT));
			const std::array<float, 4> color{ a_color[0], a_color[1], a_color[2], a_color[3] };
			const std::array uses{ ClearUse(*record, range, false) };
			bool routed = false;
			RunOp(
				"clear " + record->name, uses,
				[&](rhi::CommandList& a_list, const Immediate::Descriptors& a_descriptors, std::span<const rhi::Resource> a_handles) {
					ClearImageRhi(a_list, a_descriptors, *record, a_handles[0].GetHandle(), range, color.data(), false, false, false, 0.0f, 0);
				},
				{ record }, &routed);
			NoteRun("ClearRenderTargetView", *record, routed);
		}

		/** @brief A buffer UAV's byte range. */
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

		/** @brief A UAV clear of the view's subresources or buffer range. */
		void ClearUnorderedAccess(ID3D11UnorderedAccessView* a_view, const VkClearColorValue& a_value, const char* a_operation, bool a_float)
		{
			const auto record = Resolve(a_view);
			if (!record) {
				static std::atomic_flag logged;
				Unknown(logged, a_operation, a_view);
				return;
			}
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			rhi::TextureSubresourceRange range{};
			VkDeviceSize offset = 0, size = 0;
			if (record->kind == NativeResource::Kind::Image)
				range = ToRhiRange(ViewRange(desc, record->shape, VK_IMAGE_ASPECT_COLOR_BIT));
			else if (record->kind != NativeResource::Kind::Buffer || !UavBytes(*record, desc, offset, size))
				return;
			const std::array uses{ UnorderedUse(*record, range) };
			bool routed = false;
			RunOp(
				std::string(a_float ? "clear (float) " : "clear (uint) ") + record->name, uses,
				[&](rhi::CommandList& a_list, const Immediate::Descriptors& a_descriptors, std::span<const rhi::Resource> a_handles) {
					ClearUnorderedRhi(a_list, a_descriptors, *record, a_handles[0], range, offset, size, a_value.uint32, a_float ? a_value.float32 : nullptr);
				},
				{ record }, &routed);
			NoteRun(a_operation, *record, routed);
		}

		void ContextClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* a_view, const UINT a_values[4])
		{
			if (!a_values)
				return;
			VkClearColorValue value{};
			std::memcpy(value.uint32, a_values, sizeof(value.uint32));
			ClearUnorderedAccess(a_view, value, "ClearUnorderedAccessViewUint", false);
		}

		void ContextClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* a_view, const FLOAT a_values[4])
		{
			if (!a_values)
				return;
			VkClearColorValue value{};
			std::memcpy(value.float32, a_values, sizeof(value.float32));
			ClearUnorderedAccess(a_view, value, "ClearUnorderedAccessViewFloat", true);
		}

		void ContextClearDepthStencilView(ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
			const auto record = Resolve(a_view);
			if (!record || record->kind != NativeResource::Kind::Image) {
				static std::atomic_flag logged;
				Unknown(logged, "ClearDepthStencilView", a_view);
				return;
			}
			// The skeleton's static clear pass takes this clear on the graph.
			if (Skeleton::Sites::TakeContextDepthClear(a_view, a_flags, a_depth, a_stencil)) {
				MarkWritten(*record);
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
			const auto rhiRange = ToRhiRange(range);
			const bool depth = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, stencil = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
			const std::array uses{ ClearUse(*record, rhiRange, true) };
			bool routed = false;
			RunOp(
				"clear depth " + record->name, uses,
				[&](rhi::CommandList& a_list, const Immediate::Descriptors& a_descriptors, std::span<const rhi::Resource> a_handles) {
					ClearImageRhi(a_list, a_descriptors, *record, a_handles[0].GetHandle(), rhiRange, nullptr, true, depth, stencil, a_depth, a_stencil);
				},
				{ record }, &routed);
			NoteRun("ClearDepthStencilView", *record, routed);
		}

		/** @brief The context's ID3DUserDefinedAnnotation. Native mode has no D3D11 capture tool, so it records nothing. */
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
		 * @brief The engine's occlusion queries are detoured and measure nothing here. CS's own D3D11 timers still end queries at setup
		 * and in menus; they get the answers below.
		 */
		void ContextEnd(ID3D11Asynchronous*) {}

		/** @brief Answers at once with zeros, so a reader that polls never waits: occlusion says hidden, timestamps are disjoint. */
		HRESULT ContextGetData(ID3D11Asynchronous* a_query, void* a_data, UINT a_size, UINT)
		{
			if (!a_query)
				return E_INVALIDARG;
			if (!a_data || !a_size)
				return S_OK;
			std::memset(a_data, 0, a_size);
			winrt::com_ptr<ID3D11Query> query;
			if (FAILED(a_query->QueryInterface(IID_PPV_ARGS(query.put()))) || !query)
				return S_OK;
			D3D11_QUERY_DESC desc{};
			query->GetDesc(&desc);
			if (desc.Query == D3D11_QUERY_TIMESTAMP_DISJOINT && a_size >= sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT)) {
				auto* disjoint = static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(a_data);
				disjoint->Frequency = 1'000'000'000ull;
				disjoint->Disjoint = TRUE;
			} else if (desc.Query == D3D11_QUERY_EVENT && a_size >= sizeof(BOOL)) {
				*static_cast<BOOL*>(a_data) = TRUE;
			}
			return S_OK;
		}

		// ---- Wrapping a port's BasicRHI resources ----------------------------------------------------------------------------

		/** @brief A record that only describes an image or buffer the port keeps alive. */
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

	std::shared_ptr<org::VersionedTexture> BackBufferSlot()
	{
		return g_device ? g_device->backBufferSlot : nullptr;
	}

	void AdoptBackBufferSlot(const std::shared_ptr<org::VersionedTexture>& a_slot, winrt::com_ptr<ID3D11Texture2D> a_field)
	{
		if (!g_device)
			return;
		g_device->AdoptBackBufferSlot(a_slot, std::move(a_field));
	}

	DXGI_FORMAT DeclaredBackBufferFormat()
	{
		return g_device ? g_device->format : DXGI_FORMAT_UNKNOWN;
	}

	bool EngineSwapChainExtent(std::uint32_t& a_width, std::uint32_t& a_height)
	{
		const std::uint64_t extent = g_engineExtent.load(std::memory_order_acquire);
		if (!extent)
			return false;
		a_width = static_cast<std::uint32_t>(extent);
		a_height = static_cast<std::uint32_t>(extent >> 32);
		return true;
	}

	void OnShaderCompilerReady()
	{
		if (g_device)
			g_device->LoadMipPrograms();
	}

	bool OwnedResources()
	{
		return g_device != nullptr;
	}

	namespace
	{
		/**
		 * @brief Points a tracked record at its texture's current image, when the texture's backing moved. The ORG texture keeps
		 * its identity and its bindless indices. Only the native image and the context's import of it change. False when nothing
		 * changed.
		 */
		bool FollowMovedImage(NativeResource& a_record, std::vector<std::pair<std::uint64_t, std::uint64_t>>* a_rekeys = nullptr)
		{
			const auto texture = a_record.graphOwned;
			if (!texture)
				return false;
			const VkImage current = NativeImageOf(texture->GetAPIResource());
			const VkImage previous = a_record.image.image;
			if (!current || current == previous)
				return false;
			DxvkOrgInteropImageInfo image = a_record.image;
			image.image = current;
			auto import = ::RenderGraph::ImportDxvkImage(g_device->Rhi(), image, a_record.name.c_str());
			if (!import) {
				logger::error("[ORG native] {}: its texture moved, and the new image could not be imported for the context", a_record.name);
				return false;
			}
			auto old = a_record.Rebind(std::move(import), image);
			// A batch rekeys together, since one record may move into an image another record of the batch left.
			if (a_rekeys)
				a_rekeys->emplace_back(reinterpret_cast<std::uint64_t>(previous), reinterpret_cast<std::uint64_t>(current));
			else
				::RenderGraph::ImageRegistry::Rekey(reinterpret_cast<std::uint64_t>(previous), reinterpret_cast<std::uint64_t>(current));
			// Work recorded before the switch may still name the old import.
			if (old)
				RenderGraphRuntime::Get().ReleaseAfterD3D11(std::make_shared<rhi::ResourcePtr>(std::move(old)));
			return true;
		}

		/**
		 * @brief The records WrapGraphTexture made. A frame placement gives their textures new images with no journal entry, so they
		 * are checked every frame. Expired entries are dropped as they are found.
		 */
		struct GraphTextureRecords
		{
			std::mutex lock;
			std::vector<std::weak_ptr<NativeResource>> records;
		};

		GraphTextureRecords& GraphTextures()
		{
			// Never destroyed, since records can go during static destruction.
			static auto* records = new GraphTextureRecords;
			return *records;
		}

		/** @brief Points every live WrapGraphTexture record at its texture's current image. Render thread. */
		void FollowGraphTextures(std::vector<std::pair<std::uint64_t, std::uint64_t>>& a_rekeys)
		{
			std::vector<std::shared_ptr<NativeResource>> live;
			{
				auto& textures = GraphTextures();
				std::lock_guard guard(textures.lock);
				live.reserve(textures.records.size());
				std::erase_if(textures.records, [&](const std::weak_ptr<NativeResource>& a_weak) {
					auto record = a_weak.lock();
					if (!record)
						return true;
					live.push_back(std::move(record));
					return false;
				});
			}
			std::size_t followed = 0;
			for (const auto& record : live)
				if (FollowMovedImage(*record, &a_rekeys))
					++followed;
			if (followed) {
				static std::uint32_t logged = 0;
				if (logged < 32) {
					++logged;
					logger::info("[ORG native] graph textures: {} engine target versions have new images (a frame placement or a move)", followed);
				}
			}
		}
	}

	void FollowRelocations()
	{
		if (!g_device)
			return;
		// Before the journal, since a frame placement changes images without an entry. Both rekey as one batch.
		std::vector<std::pair<std::uint64_t, std::uint64_t>> rekeys;
		FollowGraphTextures(rekeys);
		const auto& journal = org::relocation::Service::Get().Journal();
		// From 0, so the first read covers every move before it.
		static std::uint64_t cursor = 0;
		if (journal.Head() == cursor) {
			::RenderGraph::ImageRegistry::RekeyAll(rekeys);
			return;
		}
		std::vector<org::relocation::JournalEntry> entries;
		const bool complete = journal.Read(cursor, entries);
		std::unordered_set<std::uint64_t> moved;
		for (const auto& entry : entries)
			moved.insert(entry.resourceID);
		// The tracked records whose texture moved. Without a complete journal, every tracked record is checked.
		std::vector<std::shared_ptr<NativeResource>> records;
		{
			auto& registry = GetRegistry();
			std::lock_guard guard(registry.lock);
			for (const void* live : registry.live) {
				auto* record = static_cast<NativeResource*>(const_cast<void*>(live));
				// ownership is set to Tracked only after graphOwned holds its final value (FinishGraphOwned).
				if (record->ownership != NativeResource::Ownership::Tracked)
					continue;
				const auto* texture = record->graphOwned.get();
				if (!texture || (complete && !moved.contains(texture->GetGlobalResourceID())))
					continue;
				// A record whose last reference is going is skipped. Its destructor waits for this lock.
				if (auto owned = record->weak_from_this().lock())
					records.push_back(std::move(owned));
			}
		}
		std::size_t followed = 0;
		for (const auto& record : records)
			if (FollowMovedImage(*record, &rekeys))
				++followed;
		::RenderGraph::ImageRegistry::RekeyAll(rekeys);
		if (followed) {
			static std::uint32_t logged = 0;
			if (logged < 32 || !complete) {
				++logged;
				logger::info("[ORG native] relocations: {} graph-owned textures moved to new images ({} journal entries{})", followed, entries.size(),
					complete ? "" : ", entries were lost so every tracked texture was checked");
			}
		}
	}

	std::shared_ptr<org::Runtime> TakeOrgRuntime()
	{
		return g_device ? std::move(g_device->orgRuntime) : nullptr;
	}

	namespace
	{
		/** @brief The image record behind a_object. A shell gives its window's. */
		std::shared_ptr<NativeResource> ImageRecordOf(const void* a_object)
		{
			auto record = a_object ? Resolve(a_object) : nullptr;
			if (record && record->kind == NativeResource::Kind::Shell) {
				winrt::com_ptr<ID3D11ShaderResourceView> window;
				{
					std::lock_guard guard(record->windowLock);
					window = record->window;
				}
				record = window ? Resolve(window.get()) : nullptr;
			}
			return record && record->kind == NativeResource::Kind::Image ? record : nullptr;
		}
	}

	std::shared_ptr<org::ExternalTextureResource> ImmutableTextureOf(const IUnknown* a_texture, DXGI_FORMAT a_srvFormat)
	{
		const auto record = g_device ? ImageRecordOf(a_texture) : nullptr;
		if (!record || record->ownership != NativeResource::Ownership::Exempt)
			return nullptr;
		std::lock_guard guard(record->graphLock);
		// Made on the first call, and found through the registry after it.
		if (const auto entry = ::RenderGraph::ImageRegistry::Find(reinterpret_cast<std::uint64_t>(record->image.image)))
			if (auto made = std::dynamic_pointer_cast<org::ExternalTextureResource>(entry->resource))
				return made;
		// No graph may read it before its transfer is done.
		if (!g_device->immediate.Wait(record->readyValue)) {
			logger::warn("[ORG native] {}: its transfer did not finish; not handed to the graph", record->name);
			return nullptr;
		}
		const auto& shape = record->shape;
		const bool cube = record->image.viewType == VK_IMAGE_VIEW_TYPE_CUBE || record->image.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
		org::TextureDescription desc{};
		desc.type = shape.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? rhi::ResourceType::Texture3D : rhi::ResourceType::Texture2D;
		desc.format = shape.viewFormat;
		desc.hasSRV = true;
		const rhi::Format srvFormat = a_srvFormat != DXGI_FORMAT_UNKNOWN ? rhi::helpers::ToRHI(a_srvFormat) : rhi::Format::Unknown;
		desc.srvFormat = srvFormat != rhi::Format::Unknown ? srvFormat : shape.viewFormat;
		desc.format = desc.srvFormat;
		desc.channels = 4;
		desc.depth = shape.depth;
		desc.isCubemap = cube && shape.layers % 6 == 0;
		const std::uint32_t slices = desc.isCubemap ? shape.layers / 6 : shape.layers;
		desc.isArray = slices > 1;
		desc.arraySize = slices;
		// One entry per mip, as an external texture reads them.
		for (std::uint32_t mip = 0; mip < shape.mips; ++mip)
			desc.imageDimensions.push_back({ shape.Width(mip), shape.Height(mip), 0, 0 });
		desc.initialLayout = rhi::ResourceLayout::ShaderResource;
		const std::string id = fmt::format("cs.native.texture.{}", record->serial);
		auto image = ::RenderGraph::ImportDxvkImage(g_device->Rhi(), record->image, id.c_str());
		if (!image) {
			logger::warn("[ORG native] {}: could not be imported as an exempt graph texture", record->name);
			return nullptr;
		}
		try {
			auto resource = org::ExternalTextureResource::CreateShared(std::move(image), desc, false);
			resource->SetName(id);
			// A texture already promoted is tracked from the start, with shader-read as its home.
			if (!record->promoted.load(std::memory_order_acquire))
				resource->SetGraphOwnership(org::Resource::GraphOwnership::ExternalImmutableShaderResource);
			record->exemptResource = std::move(resource);
		} catch (const std::exception& e) {
			logger::warn("[ORG native] {}: could not be made an exempt graph texture: {}", record->name, e.what());
			return nullptr;
		}
		// Registered so a read-only import returns this resource instead of importing the image again.
		::RenderGraph::ImageRegistry::Register(reinterpret_cast<std::uint64_t>(record->image.image),
			{ record->exemptResource, nullptr, std::make_shared<const org::TextureDescription>(desc), "native", id, ::RenderGraph::ImageRegistry::Kind::Made });
		return record->exemptResource;
	}

	std::shared_ptr<org::ExternalTextureResource> PromotedTextureOf(const IUnknown* a_object)
	{
		const auto record = g_device ? ImageRecordOf(a_object) : nullptr;
		if (!record || record->ownership != NativeResource::Ownership::Exempt || !record->promoted.load(std::memory_order_acquire))
			return nullptr;
		return ImmutableTextureOf(a_object, DXGI_FORMAT_UNKNOWN);
	}

	bool IsTrackedTexture(const IUnknown* a_object)
	{
		const auto record = g_device ? ImageRecordOf(a_object) : nullptr;
		return record && record->ownership != NativeResource::Ownership::Exempt;
	}

	bool DescribeFacts(const void* a_object, ResourceFacts& a_out)
	{
		auto record = a_object ? Resolve(a_object) : nullptr;
		if (record && record->kind == NativeResource::Kind::Shell) {
			// A shell has no storage. Shaders read its window, so the window is described.
			winrt::com_ptr<ID3D11ShaderResourceView> window;
			{
				std::lock_guard guard(record->windowLock);
				window = record->window;
			}
			record = window ? Resolve(window.get()) : nullptr;
		}
		if (!record)
			return false;
		a_out.serial = record->serial;
		a_out.bindFlags = record->bindFlags;
		a_out.usage = static_cast<std::uint32_t>(record->d3dUsage);
		a_out.dimension = static_cast<std::uint32_t>(record->dimension);
		a_out.initialData = record->initialData;
		a_out.written = record->written.load(std::memory_order_relaxed);
		return true;
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
		state->validation = Switches::On("CS_ORG_NATIVE_VALIDATION");
		if (state->validation)
			logger::info("[ORG native] validation: VK_LAYER_KHRONOS_validation requested (BasicRHI warns above when the layer is missing); its warnings and "
						 "errors are logged as \"Vulkan validation: ...\" lines, drained at every present");
		state->graphics = state->Rhi().GetQueue(rhi::QueueKind::Graphics);
		if (!OnD3D12()) {
			// BasicRHI's queue accesses take one lock.
			if (rhi::vulkan::ShareVulkanDevice(state->Rhi(), rhi::vulkan::QueueSubmissionHooks{ nullptr, &LockQueue, &UnlockQueue, nullptr }) != rhi::Result::Ok)
				Fail("the native device", "BasicRHI could not take the queue lock hooks");
		}
		// ORG's runtime and its external client, the one way to submit work outside the graphs, before the first context call.
		try {
			state->orgRuntime = RenderGraphRuntime::CreateOrgRuntime(*state->device);
		} catch (const std::exception& e) {
			Fail("the native device", fmt::format("ORG's runtime could not be made on it: {}", e.what()));
		}
		if (!state->orgRuntime)
			Fail("the native device", "ORG's runtime could not be made on it");
		::RenderGraph::ExternalWork::Start(state->orgRuntime, state->Rhi());
		if (!state->immediate.Init(state->Rhi()))
			Fail("the native device", "the context's timeline or descriptor heaps could not be made");

		// Native mode never takes exclusive fullscreen.
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
		state->swapBufferCount = state->bufferCount;
		state->swapWidth = state->width;
		state->swapHeight = state->height;
		rhi::SwapchainDesc swapchainDesc{};
		swapchainDesc.hwnd = state->window;
		swapchainDesc.width = state->width;
		swapchainDesc.height = state->height;
		swapchainDesc.format = state->swapchainFormat;
		swapchainDesc.bufferCount = state->bufferCount;
		swapchainDesc.allowTearing = true;
		// DLSS-G attaches to every swap chain made while it is loaded, and this one has no presentation layer.
		Streamline::GetSingleton()->UnloadDLSSGBeforeFirstSwapchain();
		if (state->Rhi().CreateSwapchain(swapchainDesc, state->swapchain) != rhi::Result::Ok || !state->swapchain)
			Fail("the native swap chain", "BasicRHI could not create a swap chain on the game's window");
		// HDR Display switches the colour space later.
		(void)ConfigureSwapchain(state->swapchain.Get(), { .hdr = false, .maximumFrameLatency = state->frameLatency });
		state->PublishPresentation(0, false);
		state->framePresentation = state->adoptedPresentation;
		if (!state->MakePresentObjects())
			Fail("the native swap chain", "the present's timeline could not be made");
		g_device = state;
		if (!state->MakeBackBuffer())
			Fail("the native back buffer", "BasicRHI could not make it (see the [ORG native] line above)");
		const auto caps = state->swapchain->Caps();
		Stage("swap chain", fmt::format("{} {}x{} DXGI format {}, {} images, tearing {}, HDR10 {}; the game asked for {} ({})", ::RenderGraph::Backend::Name(::RenderGraph::Backend::Active()), state->width, state->height,
								static_cast<int>(state->format), state->swapchain->ImageCount(), caps.tearing,
								state->swapchain->IsColorSpaceSupported(rhi::SwapchainColorSpace::Hdr10St2084), a_desc->Windowed ? "windowed" : "fullscreen",
								static_cast<int>(a_desc->BufferDesc.Format)));

		// Every call on these handles is a native service or a tripwire.
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
		// An engine target the set keeps without memory is a tripwire unbacked handle, as in guest mode. No graph may import one.
		if (Tripwire::IsUnbacked(a_object)) {
			const auto name = Tripwire::Describe(a_object).name;
			::RenderGraph::Frame::Driver::NoteUnbackedUse(name,
				fmt::format("an import (DescribeResource) of \"{}\", an engine target the set keeps without memory (Engine/Targets/EngineTargetSet.cpp lists no user of it)",
					name));
			return false;
		}
		const auto record = Resolve(a_object);
		if (!record)
			return false;
		// A shell is read through its window, as DXVK's binds read it.
		if (record->kind == NativeResource::Kind::Shell) {
			winrt::com_ptr<ID3D11ShaderResourceView> window;
			{
				std::lock_guard guard(record->windowLock);
				window = record->window;
			}
			return window && DescribeResource(window.get(), a_info);
		}
		a_info = {};
		a_info.version = DXVK_ORG_INTEROP_VERSION;
		if (record->kind == NativeResource::Kind::Buffer) {
			// From now on a CPU access waits for the graph's work.
			record->graphShared.store(true, std::memory_order_release);
			a_info.kind = DXVK_ORG_INTEROP_RESOURCE_BUFFER;
			a_info.buffer = { record->vkBuffer, record->offset, record->size, record->address, record->usage };
			return true;
		}
		if (record->kind != NativeResource::Kind::Image)
			return false;
		a_info.kind = DXVK_ORG_INTEROP_RESOURCE_IMAGE;
		a_info.image = record->image;
		// An SRV is described with its mips and slices, and only the depth aspect for a depth image, as DXVK does.
		if (Tripwire::KindOf(a_object) == Tripwire::Kind::ShaderResourceView) {
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
				break;  // cubes, 3D and 1D views cover the whole image
			}
		}
		return true;
	}

	HRESULT Present(UINT a_syncInterval, UINT a_flags)
	{
		return g_device ? g_device->Present(a_syncInterval, a_flags) : E_FAIL;
	}

	bool EmitCommands(void (*a_record)(void*, rhi::CommandList&), void* a_user, const rhi::TimelinePoint* a_waits, std::uint32_t a_waitCount,
		const rhi::TimelinePoint* a_signals, std::uint32_t a_signalCount, const char* a_label)
	{
		if (!g_device)
			return false;
		const std::string label = a_label ? a_label : "CS native commands";
		Immediate::Work work;
		work.waits = { a_waits, a_waitCount };
		work.signals = { a_signals, a_signalCount };
		// a_record starts with nothing bound.
		work.bindHeaps = false;
		// Full barriers make every access before and after visible, as DXVK's stream would.
		const auto record = [&](rhi::CommandList& a_list, const Immediate::Descriptors&) {
			auto full = rhi::FullMemoryBarrier();
			rhi::BarrierBatch barriers{};
			barriers.globals = { &full, 1u };
			a_list.Barriers(barriers);
			if (a_record)
				a_record(a_user, a_list);
			a_list.Barriers(barriers);
		};
		// a_record runs exactly once on true and never on false.
		return g_device->immediate.Run(label, work, record, {}) != 0;
	}

	namespace
	{
		/** @brief Zeroes a wrapped image that can be cleared, as DXVK's wrap does. Block-compressed images stay as made: their owners write them before reading. */
		void ZeroWrapped(const std::shared_ptr<NativeResource>& a_record, UINT a_bindFlags)
		{
			if (a_record->shape.blocks)
				return;
			const bool renderTarget = (a_bindFlags & D3D11_BIND_RENDER_TARGET) != 0;
			const bool depthStencil = (a_bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
			const bool unordered = (a_bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
			if (!renderTarget && !depthStencil && !unordered)
				return;
			const auto& record = a_record;
			const auto range = AllOf(*record);
			const std::array uses{ renderTarget || depthStencil ? ClearUse(*record, range, depthStencil) : UnorderedUse(*record, range) };
			RunOp(
				"zero " + record->name, uses,
				[&](rhi::CommandList& a_list, const Immediate::Descriptors& a_descriptors, std::span<const rhi::Resource> a_handles) {
					static constexpr float kZero[4]{};
					static constexpr uint32_t kZeroWords[4]{};
					const auto handle = a_handles[0].GetHandle();
					if (depthStencil)
						ClearImageRhi(a_list, a_descriptors, *record, handle, range, nullptr, true, true, (record->shape.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0, 0.0f, 0);
					else if (renderTarget)
						ClearImageRhi(a_list, a_descriptors, *record, handle, range, kZero, false, false, false, 0.0f, 0);
					else
						ClearUnorderedRhi(a_list, a_descriptors, *record, a_handles[0], range, 0, 0, kZeroWords, nullptr);
				},
				{ record });
		}

		/** @brief The texture handle over a wrap record, in the request's dimension. The handle keeps the record. */
		winrt::com_ptr<ID3D11Resource> WrapHandle(const std::shared_ptr<NativeResource>& a_record, const TextureRequest& a_request, uint32_t a_depth, uint32_t a_layers)
		{
			Tripwire::Options options;
			options.name = a_request.name;
			options.owner = a_record;
			winrt::com_ptr<ID3D11Resource> result;
			switch (a_request.dimension) {
			case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
				{
					const D3D11_TEXTURE3D_DESC desc{ a_request.width, a_request.height, a_depth, a_request.mipLevels, a_request.format, D3D11_USAGE_DEFAULT,
						a_request.bindFlags, 0, a_request.miscFlags };
					result.copy_from(Tripwire::CreateTexture3D(desc, std::move(options)).get());
					break;
				}
			case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
				{
					const D3D11_TEXTURE1D_DESC desc{ a_request.width, a_request.mipLevels, a_layers, a_request.format, D3D11_USAGE_DEFAULT, a_request.bindFlags, 0,
						a_request.miscFlags };
					result.copy_from(Tripwire::CreateTexture1D(desc, std::move(options)).get());
					break;
				}
			default:
				{
					const D3D11_TEXTURE2D_DESC desc{ a_request.width, a_request.height, a_request.mipLevels, a_layers, a_request.format, { a_request.sampleCount, 0 },
						D3D11_USAGE_DEFAULT, a_request.bindFlags, 0, a_request.miscFlags };
					result.copy_from(Tripwire::CreateTexture2D(desc, std::move(options)).get());
					break;
				}
			}
			return result;
		}

		/** @brief A wrap's request: the texture as D3D11 describes it, with no CPU access. */
		TextureRequest WrapRequestOf(const DxvkOrgInteropTextureWrap& a_wrap, const D3D11_SUBRESOURCE_DATA* a_data, const char* a_name)
		{
			return { static_cast<D3D11_RESOURCE_DIMENSION>(a_wrap.dimension), a_wrap.width, a_wrap.height, (std::max)(a_wrap.depthOrArraySize, 1u),
				(std::max)(a_wrap.mipLevels, 1u), (std::max)(a_wrap.sampleCount, 1u), static_cast<DXGI_FORMAT>(a_wrap.format), D3D11_USAGE_DEFAULT, a_wrap.bindFlags, 0,
				a_wrap.miscFlags, a_data, a_name ? a_name : "Native::Wrapped" };
		}
	}

	winrt::com_ptr<ID3D11Resource> WrapTexture(const rhi::Resource& a_image, const DxvkOrgInteropTextureWrap& a_wrap, DxvkOrgInteropResourceInfo& a_info,
		const char* a_name, const D3D11_SUBRESOURCE_DATA* a_data, WrapFlags a_flags)
	{
		if (!g_device)
			return nullptr;
		const bool deferred = a_flags == WrapFlags::DeferInitialize;
		// Deferred contents come from the version's birth on the graph, so there is nothing to upload here.
		if (deferred && a_data) {
			static std::atomic<bool> logged = false;
			LogOnce(logged, a_name ? a_name : "Native::Wrapped", "a deferred wrap was given initial data, which it cannot upload");
			return nullptr;
		}
		const auto dimension = static_cast<D3D11_RESOURCE_DIMENSION>(a_wrap.dimension);
		const TextureRequest request = WrapRequestOf(a_wrap, a_data, a_name);
		const DXGI_FORMAT dxgiFormat = ResolveTypeless(request.format, request.bindFlags);
		const rhi::Format format = rhi::helpers::ToRHI(dxgiFormat);
		const uint32_t depth = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? request.depthOrArraySize : 1;
		const uint32_t layers = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1 : request.depthOrArraySize;
		auto record = MakeWrapRecord(a_name);
		NoteRequest(*record, request);
		static std::atomic<bool> loggedDescribe = false, loggedInitialize = false;
		rhi::FormatSupport support{};
		(void)g_device->Rhi().QueryFormatSupport(format, support);
		if (!NativeState::FillImage(*record, a_image, request, dxgiFormat, format, support, request.mipLevels, layers, depth, VK_IMAGE_LAYOUT_GENERAL)) {
			LogOnce(loggedDescribe, request.name, "BasicRHI did not describe the image to wrap");
			return nullptr;
		}
		// Zeroed when there is no data, as DXVK's wrap does. A deferred wrap leaves both to the graph (WrapFlags::DeferInitialize).
		if (!deferred && !g_device->Initialize(a_image.GetHandle(), request, format, request.mipLevels, layers, depth)) {
			LogOnce(loggedInitialize, request.name, "the image to wrap could not be moved into GENERAL or given its contents");
			return nullptr;
		}
		if (!deferred && !a_data)
			ZeroWrapped(record, request.bindFlags);
		a_info = {};
		a_info.version = DXVK_ORG_INTEROP_VERSION;
		a_info.kind = DXVK_ORG_INTEROP_RESOURCE_IMAGE;
		a_info.image = record->image;
		return WrapHandle(record, request, depth, layers);
	}

	bool CanWrapGraphTexture(DXGI_FORMAT a_format, UINT a_bindFlags, std::string& a_why)
	{
		if (!g_device) {
			a_why = "native mode is not active";
			return false;
		}
		const rhi::Format format = rhi::helpers::ToRHI(ResolveTypeless(a_format, a_bindFlags));
		rhi::FormatSupport support{};
		if (format == rhi::Format::Unknown || g_device->Rhi().QueryFormatSupport(format, support) != rhi::Result::Ok) {
			a_why = fmt::format("DXGI format {} has no BasicRHI format", static_cast<int>(a_format));
			return false;
		}
		// The record's views would need a swizzle the ORG texture's views do not have (as MakeGraphOwned keeps such textures host-made).
		if (support.srvSwizzle) {
			a_why = fmt::format("BasicRHI stores DXGI format {} as another, read through a swizzle", static_cast<int>(a_format));
			return false;
		}
		return true;
	}

	winrt::com_ptr<ID3D11Resource> WrapGraphTexture(const std::shared_ptr<org::PixelBuffer>& a_texture, const DxvkOrgInteropTextureWrap& a_wrap,
		DxvkOrgInteropResourceInfo& a_info, const char* a_name, WrapFlags a_flags)
	{
		if (!g_device || !a_texture)
			return nullptr;
		const bool deferred = a_flags == WrapFlags::DeferInitialize;
		const auto dimension = static_cast<D3D11_RESOURCE_DIMENSION>(a_wrap.dimension);
		const TextureRequest request = WrapRequestOf(a_wrap, nullptr, a_name);
		const DXGI_FORMAT dxgiFormat = ResolveTypeless(request.format, request.bindFlags);
		const rhi::Format format = rhi::helpers::ToRHI(dxgiFormat);
		const uint32_t depth = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? request.depthOrArraySize : 1;
		const uint32_t layers = dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? 1 : request.depthOrArraySize;
		static std::atomic<bool> loggedDescribe = false, loggedImport = false, loggedInitialize = false;
		auto record = MakeWrapRecord(a_name);
		NoteRequest(*record, request);
		rhi::FormatSupport support{};
		(void)g_device->Rhi().QueryFormatSupport(format, support);
		// The record describes the texture's image as a tracked texture's does: GENERAL, the layout ORG's ledger names Common.
		if (!NativeState::FillImage(*record, a_texture->GetAPIResource(), request, dxgiFormat, format, support, request.mipLevels, layers, depth,
				VK_IMAGE_LAYOUT_GENERAL)) {
			LogOnce(loggedDescribe, request.name, "BasicRHI did not describe the ORG texture to wrap");
			return nullptr;
		}
		// The context's import of the image. Operations name the ledger's own handle; this one is for work outside the ledgers.
		record->resource = ::RenderGraph::ImportDxvkImage(g_device->Rhi(), record->image, request.name.c_str());
		if (!record->resource) {
			LogOnce(loggedImport, request.name, "the ORG texture's image could not be imported for the context");
			return nullptr;
		}
		// Tracked only once graphOwned holds its final value, as FollowRelocations reads them in that order.
		record->graphOwned = a_texture;
		record->ownership = NativeResource::Ownership::Tracked;
		if (!deferred) {
			// Registered as owned so the first contents go through the ledger. The caller registers it again with its slot.
			NoteOwnedImage(*record);
			if (!g_device->InitializeTracked(*record, request, format, request.mipLevels, layers, depth)) {
				LogOnce(loggedInitialize, request.name, "the ORG texture's first layout could not be written");
				return nullptr;
			}
			ZeroWrapped(record, request.bindFlags);
		}
		{
			auto& textures = GraphTextures();
			std::lock_guard guard(textures.lock);
			textures.records.push_back(record);
		}
		a_info = {};
		a_info.version = DXVK_ORG_INTEROP_VERSION;
		a_info.kind = DXVK_ORG_INTEROP_RESOURCE_IMAGE;
		a_info.image = record->image;
		return WrapHandle(record, request, depth, layers);
	}

	std::uint64_t FollowGraphTexture(const IUnknown* a_texture)
	{
		const auto record = g_device && a_texture ? Resolve(a_texture) : nullptr;
		if (!record || record->kind != NativeResource::Kind::Image || record->ownership != NativeResource::Ownership::Tracked || !record->graphOwned)
			return 0;
		(void)FollowMovedImage(*record);
		std::lock_guard guard(record->graphLock);
		return reinterpret_cast<std::uint64_t>(record->image.image);
	}

	winrt::com_ptr<ID3D11Buffer> WrapBuffer(const rhi::Resource& a_buffer, const D3D11_BUFFER_DESC& a_desc, const char* a_name)
	{
		if (!g_device)
			return nullptr;
		auto record = MakeWrapRecord(a_name);
		record->kind = NativeResource::Kind::Buffer;
		if (!NativeState::DescribeBuffer(*record, a_buffer)) {
			static std::atomic<bool> logged = false;
			LogOnce(logged, a_name ? a_name : "a buffer", "BasicRHI did not describe the buffer to wrap");
			return nullptr;
		}
		record->size = a_desc.ByteWidth;
		record->bufferDesc = a_desc;
		// Its owner fills it through a mapping no census sees, so it counts as made without data.
		record->bindFlags = a_desc.BindFlags;
		record->d3dUsage = a_desc.Usage;
		record->dimension = D3D11_RESOURCE_DIMENSION_BUFFER;
		Tripwire::Options options;
		options.name = record->name;
		options.owner = record;
		return Tripwire::CreateBuffer(a_desc, std::move(options));
	}

	namespace
	{
		// Private-data slot for a shell SRV's gone tag.
		constexpr GUID kStreamGoneGuid = { 0x7d1e2c44, 0x9b3a, 0x4f61, { 0x8e, 0x25, 0x3c, 0x6a, 0x0b, 0x9d, 0x4f, 0x12 } };

		/** @brief Held as a shell SRV's private data. Releasing the view runs the streamer's gone callback. */
		class StreamGoneTag final : public IUnknown
		{
		public:
			StreamGoneTag(void(__stdcall* a_gone)(std::uint64_t), std::uint64_t a_user) :
				gone(a_gone), user(a_user) {}
			~StreamGoneTag()
			{
				if (gone)
					gone(user);
			}
			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_out) override
			{
				if (!a_out)
					return E_POINTER;
				if (a_riid == __uuidof(IUnknown)) {
					AddRef();
					*a_out = static_cast<IUnknown*>(this);
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

		private:
			std::atomic<ULONG> refs{ 1 };
			void(__stdcall* gone)(std::uint64_t) = nullptr;
			std::uint64_t user = 0;
		};
	}

	bool CreateStreamShell(const D3D11_TEXTURE2D_DESC& a_desc, ID3D11Texture2D** a_texture, ID3D11ShaderResourceView** a_view)
	{
		if (!g_device || !g_device->deviceHandle || !a_texture || !a_view)
			return false;
		*a_texture = nullptr;
		*a_view = nullptr;
		auto record = std::make_shared<NativeResource>();
		record->kind = NativeResource::Kind::Shell;
		record->name = "Native::StreamShell";
		D3D11_TEXTURE2D_DESC desc = a_desc;
		if (!desc.MipLevels)
			desc.MipLevels = FullMipCount(desc.Width, desc.Height, 1);
		Tripwire::Options options;
		options.name = record->name;
		options.owner = std::move(record);
		auto texture = Tripwire::CreateTexture2D(desc, std::move(options));
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		if (!texture || FAILED(g_device->deviceHandle->CreateShaderResourceView(texture.get(), nullptr, view.put())) || !view)
			return false;
		*a_texture = texture.detach();
		*a_view = view.detach();
		return true;
	}

	HRESULT __stdcall StreamEnableView(ID3D11ShaderResourceView* a_shell, void(__stdcall*)(std::uint64_t), void(__stdcall* a_gone)(std::uint64_t),
		const D3D11_SUBRESOURCE_DATA*(__stdcall*)(std::uint64_t, BOOL), std::uint64_t a_user)
	{
		const auto record = Resolve(a_shell);
		if (!record || record->kind != NativeResource::Kind::Shell)
			return E_FAIL;
		{
			std::lock_guard guard(record->windowLock);
			if (std::exchange(record->streamEnabled, true))
				return E_FAIL;  // already a shell, and DXVK refuses a second enable
		}
		// The gone callback runs with the view's last reference, before anything could name it again.
		winrt::com_ptr<IUnknown> tag;
		tag.attach(new StreamGoneTag(a_gone, a_user));
		return a_shell->SetPrivateDataInterface(kStreamGoneGuid, tag.get());
	}

	void __stdcall StreamSetWindow(ID3D11ShaderResourceView* a_shell, ID3D11ShaderResourceView* a_window)
	{
		if (const auto record = Resolve(a_shell); record && record->kind == NativeResource::Kind::Shell) {
			std::lock_guard guard(record->windowLock);
			record->window.copy_from(a_window);
		}
	}

	ID3D11ShaderResourceView* __stdcall StreamGetWindow(ID3D11ShaderResourceView* a_shell)
	{
		const auto record = Resolve(a_shell);
		if (!record || record->kind != NativeResource::Kind::Shell)
			return nullptr;
		std::lock_guard guard(record->windowLock);
		ID3D11ShaderResourceView* window = nullptr;
		if (record->window)
			record->window.copy_to(&window);
		return window;
	}

	std::uint64_t __stdcall StreamGetBinds(ID3D11ShaderResourceView*)
	{
		// Engine draws never read shells in native mode, so none counts as a native bind.
		return 0;
	}

	rhi::QueueProgress GraphicsProgress()
	{
		return g_device && g_device->graphics ? g_device->graphics.Progress() : rhi::QueueProgress{};
	}

	namespace
	{
		/** @brief Vulkan views for external commands, destroyed with their last owner once the GPU is past them. */
		struct ImageViews
		{
			VkDevice device = VK_NULL_HANDLE;
			PFN_vkDestroyImageView destroy = nullptr;
			std::vector<VkImageView> views;
			~ImageViews()
			{
				for (auto view : views)
					if (view && destroy)
						destroy(device, view, nullptr);
			}
		};

		/** @brief The native list and each image's handle, view and state for external commands. False with a_why when one is missing. */
		bool DescribeImages(rhi::CommandList& a_list, std::span<const rhi::Resource> a_handles, std::span<const std::uint64_t> a_states,
			std::span<const std::string> a_names, void*& a_nativeList, std::vector<ExternalImageInfo>& a_infos, ImageViews& a_views, std::string& a_why)
		{
			const bool d3d12 = OnD3D12();
			a_infos.assign(a_handles.size(), {});
			a_nativeList = nullptr;
			const auto nameOf = [&](std::size_t a_index) { return a_index < a_names.size() ? a_names[a_index] : fmt::format("image {}", a_index); };
#if defined(CS_ORG_HAS_D3D12)
			if (d3d12) {
				// Use rhi_interop.h's getters because that header's d3d12.h is not this file's.
				rhi::D3D12CmdListInfo list{};
				if (rhi::QueryNativeCmdList(a_list, rhi::RHI_IID_D3D12_STREAMLINE_CMD_LIST, &list, sizeof(list)) && list.cmdList)
					a_nativeList = list.cmdList;
				else if (rhi::QueryNativeCmdList(a_list, rhi::RHI_IID_D3D12_CMD_LIST, &list, sizeof(list)))
					a_nativeList = list.cmdList;
				for (std::size_t i = 0; i < a_infos.size(); ++i) {
					rhi::D3D12ResourceInfo native{};
					if (!rhi::QueryNativeResource(a_handles[i], rhi::RHI_IID_D3D12_RESOURCE, &native, sizeof(native)) || !native.resource) {
						a_why = fmt::format("'{}' has no D3D12 resource", nameOf(i));
						return false;
					}
					auto* resource = static_cast<ID3D12Resource*>(native.resource);
					const auto desc = resource->GetDesc();
					a_infos[i] = { resource, 0, a_states[i], static_cast<std::uint32_t>(desc.Format), static_cast<std::uint32_t>(desc.Width), desc.Height, desc.MipLevels,
						desc.DepthOrArraySize, 0 };
				}
			}
#endif
			if (!d3d12) {
				// Native mode's own device, or in guest mode the one over DXVK's VkDevice.
				const VkDevice device = rhi::vulkan::get_device(RenderGraphRuntime::Get().Host()->Device());
				const auto getDeviceProc = rhi::vulkan::get_device_proc_addr();
				auto create = getDeviceProc ? reinterpret_cast<PFN_vkCreateImageView>(getDeviceProc(device, "vkCreateImageView")) : nullptr;
				a_views.device = device;
				a_views.destroy = getDeviceProc ? reinterpret_cast<PFN_vkDestroyImageView>(getDeviceProc(device, "vkDestroyImageView")) : nullptr;
				a_nativeList = rhi::vulkan::get_cmd_list(a_list);
				if (!create || !a_views.destroy || !a_nativeList) {
					a_why = a_nativeList ? "no vkCreateImageView" : "no native command buffer";
					return false;
				}
				for (std::size_t i = 0; i < a_infos.size(); ++i) {
					rhi::VulkanResourceInfo info{};
					if (!rhi::vulkan::get_resource_info(a_handles[i], info) || !info.resource) {
						a_why = fmt::format("image {} ('{}'): its graph resource has no Vulkan image (handle valid {})", i, nameOf(i), a_handles[i].IsValid());
						return false;
					}
					const auto format = static_cast<VkFormat>(info.nativeFormat);
					VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
					viewInfo.image = static_cast<VkImage>(info.resource);
					viewInfo.viewType = info.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
					viewInfo.format = format;
					const bool depth = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
					                   format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_X8_D24_UNORM_PACK32 || format == VK_FORMAT_D16_UNORM_S8_UINT;
					viewInfo.subresourceRange = { static_cast<VkImageAspectFlags>(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, info.mipLevels, 0,
						info.arrayLayers };
					VkImageView view = VK_NULL_HANDLE;
					if (const VkResult made = create(device, &viewInfo, nullptr, &view); made != VK_SUCCESS) {
						a_why = fmt::format("image {} ('{}', VkFormat {}): vkCreateImageView failed ({})", i, nameOf(i), static_cast<int>(format), static_cast<int>(made));
						return false;
					}
					a_views.views.push_back(view);
					a_infos[i] = { info.resource, reinterpret_cast<std::uint64_t>(view), a_states[i], info.nativeFormat, info.width, info.height, info.mipLevels,
						info.arrayLayers, info.usage };
				}
			}
			if (!a_nativeList) {
				a_why = "no native command list";
				return false;
			}
			return true;
		}
	}

	bool DescribeForCommands(rhi::CommandList& a_list, std::span<const rhi::Resource> a_images, void*& a_nativeList, std::vector<ExternalImageInfo>& a_infos,
		std::shared_ptr<const void>& a_views, std::string& a_why)
	{
		if (!RenderGraphRuntime::Get().Host()) {
			a_why = "no render graph host";
			return false;
		}
		const std::vector<std::uint64_t> states(a_images.size(), OnD3D12() ? 0u : static_cast<std::uint64_t>(VK_IMAGE_LAYOUT_GENERAL));
		auto views = std::make_shared<ImageViews>();
		const bool described = DescribeImages(a_list, a_images, states, {}, a_nativeList, a_infos, *views, a_why);
		a_views = std::move(views);
		return described;
	}

	uint64_t AdapterLuid()
	{
		return g_adapterLuid;
	}

	bool DescribeVulkanDevice(DxvkOrgInteropDeviceInfo& a_info)
	{
		if (!g_device || OnD3D12())
			return false;
		const auto device = g_device->Rhi();
		a_info = {};
		a_info.version = DXVK_ORG_INTEROP_VERSION;
		a_info.getInstanceProcAddr = LoaderProc();
		a_info.instance = rhi::vulkan::get_instance(device);
		a_info.instanceApiVersion = rhi::vulkan::get_api_version(device);
		a_info.physicalDevice = rhi::vulkan::get_physical_device(device);
		a_info.device = rhi::vulkan::get_device(device);
		a_info.graphicsQueue = rhi::vulkan::get_queue(g_device->graphics);
		a_info.graphicsQueueFamily = rhi::vulkan::get_queue_family_index(g_device->graphics);
		return a_info.getInstanceProcAddr && a_info.instance && a_info.physicalDevice && a_info.device && a_info.graphicsQueue;
	}

	HRESULT __stdcall RunOnGraphicsQueue(ID3D11Device*, void (*a_callback)(void*, VkQueue_T*), void* a_user)
	{
		if (!g_device || OnD3D12() || !a_callback)
			return E_FAIL;
		const VkQueue queue = rhi::vulkan::get_queue(g_device->graphics);
		std::lock_guard guard(g_queueLock);
		a_callback(a_user, queue);
		return S_OK;
	}

	void Shutdown()
	{
		if (!g_device)
			return;
		RenderGraphRuntime::Get().DrainSubmissions("the window closing", true);
		std::lock_guard guard(g_device->presentLock);
		g_device->WaitPresents();
		g_device->lost = true;  // nothing more is presented
		logger::info("[ORG native] the window closes: presents stopped after {} frames", g_device->presents);
	}
}

namespace FrameGen
{
	namespace
	{
		namespace NativeDevice = RenderGraph::Engine::Native;

		/** @brief What the swap chain is now. Any thread. */
		NativeDevice::PresentationState AdoptedPresentation(NativeDevice::NativeState& a_device)
		{
			std::lock_guard guard(a_device.presentationLock);
			return a_device.adoptedPresentation;
		}
	}

	NativePresenter& NativePresenter::Get()
	{
		static NativePresenter presenter;
		return presenter;
	}

	bool NativePresenter::Available() const
	{
		if (!NativeDevice::Active())
			return false;
		const auto* streamline = Streamline::GetSingleton();
		return streamline->IsFSRFGSupported() || streamline->IsDLSSGSupported();
	}

	SurfaceState NativePresenter::Surface() const
	{
		const auto* device = NativeDevice::g_device;
		if (!device)
			return {};
		const auto& state = device->framePresentation;
		SurfaceState surface;
		surface.format = static_cast<rhi::Format>(state.format);
		surface.hdr10 = state.hdr10;
		surface.serial = state.serial;
		// Ready while no replacement is in flight, so the frame's inputs match what it presents into. A revision asks for one at
		// a frame's Begin and is adopted with its result.
		surface.ready = state.serial == RequestedSerial();
		return surface;
	}

	void NativePresenter::Adopt(const PresentationConfig& a_config, const PresentationChange& a_change)
	{
		auto* device = NativeDevice::g_device;
		if (!device)
			return;
		// Frames see a new presentation only with the revision made for it.
		if (!a_change.atAdoption) {
			if (::FrameAnchors::IsRenderOwner())
				device->framePresentation = AdoptedPresentation(*device);
			return;
		}
		// A colour space change asked for inside the adoption point: the submission thread replaces the swap chain after the presents
		// queued so far, so earlier frames present into the old swap chain and the frames of the adopted revision into the new one.
		RequestConfig(a_config, "the colour space, at the frame revision's adoption");
		if (!::FrameAnchors::IsRenderOwner())
			return;
		auto state = AdoptedPresentation(*device);
		// ReplacePresentation presents SDR when the output does not take HDR10.
		const bool hdr10 = a_config.hdr && device->hdr10Supported.load(std::memory_order_relaxed);
		state.serial = RequestedSerial();
		state.hdr10 = hdr10;
		state.format = static_cast<std::uint32_t>(hdr10 ? rhi::Format::R10G10B10A2_UNorm : device->swapchainFormat);
		device->framePresentation = state;
	}

	void NativePresenter::RequestConfig(const PresentationConfig& a_config, const char* a_reason)
	{
		logger::info("[FrameGen] native presentation requested ({})", a_reason ? a_reason : "no reason given");
		if (!NativeDevice::g_device)
			return;
		NativeDevice::RunPresentationLeaving();
		const uint64_t serial = NextRequest();
		logger::info("[ORG native] presentation request {}: {}; the submission thread replaces the swap chain", serial, std::format("{}", a_config));
		// After the presents queued so far. The latest request wins, so a superseded one does nothing.
		RenderGraphRuntime::Get().QueueOnSubmitThread([a_config, serial] {
			auto* device = NativeDevice::g_device;
			if (!device || Get().RequestedSerial() != serial)
				return;
			std::lock_guard guard(device->presentLock);
			device->ReplacePresentation(a_config, serial);
		});
	}

	uint64_t NativePresenter::Replace(const PresentationConfig& a_config, const char* a_reason)
	{
		RequestConfig(a_config, a_reason);
		return NativeDevice::g_device ? RequestedSerial() : 0;
	}

	void NativePresenter::SetTornDown(std::function<bool()> a_tornDown)
	{
		// Also for replacements other callers ask for (HDR's colour space), so a pending feature load change goes through them.
		StoreTornDown(std::move(a_tornDown));
		auto* device = NativeDevice::g_device;
		if (!device)
			return;
		// DLSS-G's mode goes off before every swap chain change, whoever asks for it: every request and the engine's ResizeBuffers.
		std::lock_guard guard(device->presentationLock);
		device->leaving = [] { Streamline::GetSingleton()->TurnDLSSGOffBeforeSwapchainChange(); };
	}

	bool NativePresenter::LayerPaces() const
	{
		const auto* device = NativeDevice::g_device;
		return device && device->framePresentation.presentationLayer;
	}

	void NativePresenter::HoldPresent()
	{
		globals::d3d::context->Flush();
		auto* device = NativeDevice::g_device;
		if (!device || !device->graphics)
			return;
		// The CPU cannot run ahead and free per-frame resources, such as the upscaler's views, while the GPU still reads them.
		static std::deque<rhi::QueueProgress> frames;
		const auto progress = device->graphics.Progress();
		if (!progress.timeline || !progress.submitted)
			return;
		frames.push_back(progress);
		while (frames.size() > device->frameLatency) {
			auto oldest = frames.front();
			frames.pop_front();
			ZoneScopedN("CS.Native.HeldPresentPace");
			(void)oldest.timeline.HostWait(oldest.submitted, 250);
		}
	}

	void NativePresenter::SetColorSpace(bool a_hdr)
	{
		const auto* device = NativeDevice::g_device;
		if (!device)
			return;
		const auto& presentation = device->framePresentation;
		if (a_hdr == presentation.hdr10)
			logger::info("[HDR] swap chain color space is {} (native, request {})", a_hdr ? "HDR10 (PQ/BT.2020)" : "SDR (sRGB)", presentation.serial);
		else
			logger::warn("[HDR] the frame renders {} but the swap chain is {} (native, request {})", a_hdr ? "HDR" : "SDR", presentation.hdr10 ? "HDR10" : "SDR",
				presentation.serial);
	}

	std::optional<bool> NativePresenter::LayerVsync() const
	{
		auto* device = NativeDevice::g_device;
		if (!device)
			return std::nullopt;
		const auto adopted = AdoptedPresentation(*device);
		if (adopted.serial != RequestedSerial() || !adopted.presentationLayer)
			return std::nullopt;
		return adopted.vsync;
	}
}

