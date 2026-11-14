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
#	include "RenderGraph/Backend.h"
#	include "RenderGraph/BuildManager.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/Engine/D3D12Device.h"
#	include "RenderGraph/Engine/Native/OpCensus.h"
#	include "RenderGraph/Engine/Skeleton/SkeletonSites.h"
#	include "RenderGraph/ExternalWork.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GenerateMips.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/Switches.h"

#	include <OpenRenderGraph/GraphHost.h>

#	if defined(CS_ORG_HAS_D3D12)
#		include <d3d12.h>
#	endif

#	include <Resources/ExternalTextureResource.h>
#	include <Resources/PixelBuffer.h>
#	include <Resources/TextureDescription.h>
#	include <Resources/VersionedTexture.h>

#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <bit>
#	include <cstring>
#	include <chrono>
#	include <mutex>
#	include <span>
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

		/** @brief Whether the native device is D3D12 (CS_ORG_NATIVE_BACKEND=d3d12). */
		bool OnD3D12()
		{
			return ::RenderGraph::Backend::IsD3D12();
		}

		/** @brief The ID3D12Resource behind a BasicRHI resource of the D3D12 device, or null. */
		void* D3D12ResourceOf(const rhi::Resource& a_resource)
		{
			rhi::D3D12ResourceInfo info{};
			return rhi::QueryNativeResource(a_resource, rhi::RHI_IID_D3D12_RESOURCE, &info, sizeof(info)) ? info.resource : nullptr;
		}

		/** @brief A D3D12 buffer's GPU virtual address (0 for a texture, or without the D3D12 backend). */
		uint64_t D3D12AddressOf(void* a_resource)
		{
#	if defined(CS_ORG_HAS_D3D12)
			if (!a_resource)
				return 0;
			auto* resource = static_cast<ID3D12Resource*>(a_resource);
			return resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? resource->GetGPUVirtualAddress() : 0;
#	else
			(void)a_resource;
			return 0;
#	endif
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
				VkPhysicalDeviceVulkan13Features vk13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
				vk13.pNext = has(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) ? &robustness : nullptr;
				VkPhysicalDeviceVulkan12Features vk12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
				vk12.pNext = &vk13;
				VkPhysicalDeviceVulkan11Features vk11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
				vk11.pNext = &vk12;
				VkPhysicalDeviceFeatures2 supported{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
				supported.pNext = &vk11;
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

				// Vulkan 1.1, 1.2 and 1.3: every supported feature, as DXVK enables them (DCLF's counted indirect draws need
				// drawIndirectCount, the graph the memory model's device scope), into BasicRHI's structs of those versions. None is added
				// where BasicRHI has none: the chain may hold the single-feature structs instead, and both together are invalid.
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

				// Extensions added here: the create info's list copied once, then pointed at.
				self->extensions.assign(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
				const auto addExtension = [&](const char* a_name) {
					if (std::ranges::none_of(self->extensions, [&](const char* e) { return std::strcmp(e, a_name) == 0; }))
						self->extensions.push_back(a_name);
					info->enabledExtensionCount = static_cast<uint32_t>(self->extensions.size());
					info->ppEnabledExtensionNames = self->extensions.data();
				};

				// Null descriptors (DXVK enables them, and BasicRHI reads the flag from the create info).
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

				// Device-generated commands, which RenderGraphRuntime asks DXVK for (kRequestedFeatures): DCLF's GPU-driven draws and its
				// first-person epochs use them. BasicRHI reads the extension and the feature from the create info.
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
		 * @brief BasicRHI's device on the adapter the game chose. The one place the backend is picked: a D3D12 device goes here too
		 * (CS_ORG_BACKEND, the D3D12 lane), and everything after it uses rhi calls except the image description for the graph's
		 * imports (DescribeResource), which is Vulkan's.
		 */
		bool CreateBasicRhiDevice(IDXGIAdapter* a_adapter, std::shared_ptr<rhi::DevicePtr>& a_out, std::string& a_reason)
		{
			// CS_ORG_NATIVE_BACKEND=d3d12: BasicRHI's D3D12 device on the same adapter. Everything after this point is the same
			// code, except where a function asks OnD3D12().
			if (::RenderGraph::Backend::Requested() == rhi::Backend::D3D12) {
				D3D12DeviceOptions options{};
				// CS_ORG_NATIVE_VALIDATION=1: the D3D12 debug layer (SARP_D3D12_GPU_VALIDATION=1 adds GPU-based validation).
				options.debug = Switches::On("CS_ORG_NATIVE_VALIDATION");
				options.crashMarkers = (Switches::Equals("DXVK_DEBUG", "hang") || Aftermath::WantsCrashAnalysis()) && !Switches::Off("CS_ORG_CRASH_MARKERS");
				options.framesInFlight = RenderGraphRuntime::kGameFramesInFlight;
				DXGI_ADAPTER_DESC adapter{};
				if (a_adapter && SUCCEEDED(a_adapter->GetDesc(&adapter)))
					options.adapterLuid = (static_cast<uint64_t>(static_cast<uint32_t>(adapter.AdapterLuid.HighPart)) << 32) | adapter.AdapterLuid.LowPart;
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
			// CS_ORG_NATIVE_VALIDATION=1: Vulkan validation layers through BasicRHI (P6.4).
			ci.enableDebug = Switches::On("CS_ORG_NATIVE_VALIDATION");
			// Hang diagnostics where DXVK's hang mode would be on (DXVK_DEBUG=hang, CS_GPU_CRASH_ANALYSIS); CS_ORG_CRASH_MARKERS=0 off.
			ci.enableCrashMarkers = (Switches::Equals("DXVK_DEBUG", "hang") || Aftermath::WantsCrashAnalysis()) && !Switches::Off("CS_ORG_CRASH_MARKERS");
			// Nsight Aftermath's checkpoints, as on DXVK's adopted device: with crash markers on, every debug label (the graph's passes)
			// sets an NV checkpoint that its dump resolves to the pass name.
			if (Aftermath::IsEnabled()) {
				ci.vulkanRegisterCheckpoint = &Aftermath::RegisterOrgCheckpoint;
				ci.vulkanDetailedCheckpoints = Switches::Equals("CS_ORG_DETAILED_CHECKPOINTS", "1");
			}
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
			::RenderGraph::Backend::SetActive(rhi::Backend::Vulkan);
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
			// A depth-stencil staging texture: where the GPU's depth and stencil planes of the subresource lie (StencilTexelBytes).
			// The CPU sees D3D11's interleaved texels at offset; copies move them between the two (RepackDepthStencil).
			uint64_t depthOffset = 0, stencilOffset = 0;
			uint32_t depthRowPitch = 0, stencilRowPitch = 0;
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
			// The format the context's views take: format itself, except a D3D12 typeless depth image's depth format (its DSV).
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
				Buffer,   // a buffer
				Image,    // a texture the GPU uses (DEFAULT, IMMUTABLE, DYNAMIC)
				Staging,  // a STAGING texture: a buffer laid out by subresource
				Shell,    // a streamed texture's shell: no storage, read through its window (CreateStreamShell)
			};
			/**
			 * @brief Who keeps an image's state (org-native-frame-graph.md, Phase 2).
			 * - Host: the native device made it with BasicRHI, and the graph imports it (textures made before the graph was up, ones
			 *   ORG cannot describe, and wrapped images).
			 * - Tracked: ORG made it (graphOwned, a persistent PixelBuffer that is never aliased) and its graphs track it.
			 * - Exempt: made with its contents and only read since. It rests in the shader-read state and graphs read it untracked
			 *   (org::Resource::GraphOwnership::ExternalImmutableShaderResource).
			 */
			enum class Ownership : uint8_t
			{
				Host,
				Tracked,
				Exempt,
			};
			Kind kind = Kind::Buffer;
			Ownership ownership = Ownership::Host;
			// A tracked texture's ORG resource, which owns the image. Declared before resource, so the import below goes first.
			std::shared_ptr<org::PixelBuffer> graphOwned;
			rhi::ResourcePtr resource;
			std::string name;
			// Buffers and staging textures: the Vulkan buffer, its size, and whether the CPU can map it (upload or readback memory).
			VkBuffer vkBuffer = VK_NULL_HANDLE;
			VkDeviceAddress address = 0;  // a buffer's device address, which DCLF's imports read (0 when the buffer has none)
			VkBufferUsageFlags usage = 0;
			uint64_t size = 0;
			bool hostVisible = false;
			bool readback = false;
			// A buffer the graph imported (DescribeResource): a graph execution may read it, so the CPU waits for the graph's work
			// before it writes or reads the memory (WaitGraphReaders).
			std::atomic<bool> graphShared{ false };
			D3D11_BUFFER_DESC bufferDesc{};
			// Images: how the graph imports it. Images and staging textures: the shape; staging textures: the subresources' layout.
			DxvkOrgInteropImageInfo image{};
			Shape shape;
			std::vector<SubresourceLayout> layout;
			// A DYNAMIC texture's CPU side: Map writes it, Unmap copies it into the image.
			std::shared_ptr<NativeResource> upload;
			// The value of the last immediate work that read or wrote it (Immediate), so Map waits only for that.
			std::atomic<uint64_t> lastUse{ 0 };
			// A depth-stencil staging texture: whether a copy from an image wrote its planes since the CPU's texels were last made from
			// them (Map makes them then), and whether that image's depth plane holds floats (DepthPlaneFloat).
			std::atomic<bool> planesNewer{ false };
			bool planeFloat = false;
			// GenerateMips' scratch texture (MipScratchOf): a UAV twin of this image the mip chain is built in. Made on first use.
			std::shared_ptr<NativeResource> mipScratch;
			// A shell's window (StreamSetWindow), and whether the streamer enabled it (StreamEnableView).
			std::mutex windowLock;
			winrt::com_ptr<ID3D11ShaderResourceView> window;
			bool streamEnabled = false;
			// What the censuses and the undeclared-access validator read (OpCensus, DescribeForCensus): the D3D11 description's bind
			// flags, usage and dimension, whether it was made with data, and whether the context wrote it since (marked only while
			// one of them is on).
			UINT bindFlags = 0;
			D3D11_USAGE d3dUsage = D3D11_USAGE_DEFAULT;
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
			bool initialData = false;
			std::atomic<bool> written{ false };
			const uint64_t serial;  // unique for the session: the censuses' key
			// An exempt texture: whether it rests in the shader-read state (the immediate context moves it out and back around its
			// own work), the immediate value its contents were uploaded at (its readiness), whether a write promoted it to tracked,
			// and the graph resource made for it on the first lookup (ImmutableTextureOf).
			std::atomic<bool> restsShaderRead{ false };
			uint64_t readyValue = 0;
			std::atomic<bool> promoted{ false };
			std::mutex graphLock;
			std::shared_ptr<org::ExternalTextureResource> exemptResource;
			// The engine's back buffer under the graph present: the graph-owned version the frame draws into now. Resolve
			// answers it for every handle of this record, so the context's work and the imports follow the version. Read and
			// written with the registry's lock.
			std::shared_ptr<NativeResource> renamed;

			NativeResource();
			~NativeResource();
			NativeResource(const NativeResource&) = delete;
			NativeResource& operator=(const NativeResource&) = delete;
		};

		uint64_t NextSerial()
		{
			static std::atomic<uint64_t> next{ 0 };
			return ++next;
		}

		/** @brief The live NativeResource records, so DescribeResource can tell one from another owner. */
		struct Registry
		{
			std::mutex lock;
			std::unordered_set<const void*> live;
			// Tracked and exempt images by their VkImage (ID3D12Resource on D3D12), for GraphTextureOfImage.
			std::unordered_map<std::uint64_t, NativeResource*> byImage;
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
			// The exempt texture's import does not own the image, which goes with this record: no import may find it after this.
			if (exemptResource)
				::RenderGraph::ForgetMadeImage(exemptResource.get());
			auto& registry = GetRegistry();
			std::lock_guard guard(registry.lock);
			registry.live.erase(this);
			if (ownership != Ownership::Host)
				if (const auto found = registry.byImage.find(reinterpret_cast<std::uint64_t>(image.image)); found != registry.byImage.end() && found->second == this)
					registry.byImage.erase(found);
		}

		std::shared_ptr<org::Resource> LedgerResourceOf(NativeResource& a_record);

		/** @brief Points every handle of a_record at a_target (null: at a_record itself again). Any thread. */
		void Rename(NativeResource& a_record, std::shared_ptr<NativeResource> a_target)
		{
			auto& registry = GetRegistry();
			std::lock_guard guard(registry.lock);
			a_record.renamed = std::move(a_target);
		}

		/** @brief The record a_record's handles stand for now: its rename, or itself. Any thread. */
		std::shared_ptr<NativeResource> CurrentOf(const std::shared_ptr<NativeResource>& a_record)
		{
			if (!a_record)
				return nullptr;
			auto& registry = GetRegistry();
			std::lock_guard guard(registry.lock);
			return a_record->renamed ? a_record->renamed : a_record;
		}

		/** @brief Lists a tracked or exempt image under its image handle (GraphTextureOfImage). */
		void NoteOwnedImage(NativeResource& a_record)
		{
			auto& registry = GetRegistry();
			std::lock_guard guard(registry.lock);
			registry.byImage[reinterpret_cast<std::uint64_t>(a_record.image.image)] = &a_record;
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

		/**
		 * @brief The immediate context's work, recorded with BasicRHI commands on both backends, one operation at a time (a copy, a
		 * clear, a mip chain), in the order D3D11 code makes the calls. Two routes to the queue:
		 * - once the render graph is up, ORG's external client (Route): a tracked image's barriers come from its ledger, and every
		 *   other resource moves from Common into the state its command needs and back;
		 * - before that (the engine's startup textures), the context's own lists (RunRhi), with the same barriers.
		 * Both signal this context's timeline, so Map, Wait and the queries cover either. Each slot has descriptors of its own (an
		 * RTV, a DSV, and UAVs in a shader-visible and a CPU heap), reused once the timeline has passed the slot's last work, when what
		 * that work kept alive (its upload buffer) is released too. Nothing waits unless a caller asks (Map of memory the GPU still
		 * uses).
		 */
		class Immediate
		{
		public:
			/** @brief UAV descriptors per slot: one per clear an operation records (a mip each), since D3D12 reads a UAV clear's at execution. */
			static constexpr uint32_t kUavsPerSlot = 16;

			/**
			 * @brief The descriptors of a slot, for the BasicRHI clears. RTV and DSV are read when a clear is recorded, so one is reused;
			 * the UAVs are kUavsPerSlot in a row from uavShaderVisible and uavCpu, the same index in each heap.
			 */
			struct Descriptors
			{
				rhi::DescriptorSlot rtv, dsv, uavShaderVisible, uavCpu;
			};

			bool Init(rhi::Device a_device)
			{
				device = a_device;
				queue = device.GetQueue(rhi::QueueKind::Graphics);
				const auto heapOf = [&](rhi::DescriptorHeapType a_type, uint32_t a_capacity, bool a_shaderVisible, const char* a_name, rhi::DescriptorHeapPtr& a_out) {
					return device.CreateDescriptorHeap(rhi::DescriptorHeapDesc{ a_type, a_capacity, a_shaderVisible, a_name }, a_out) == rhi::Result::Ok && a_out;
				};
				const auto heap = [&](rhi::DescriptorHeapType a_type, bool a_shaderVisible, const char* a_name, rhi::DescriptorHeapPtr& a_out) {
					return heapOf(a_type, kSlots, a_shaderVisible, a_name, a_out);
				};
				bool ok = heap(rhi::DescriptorHeapType::RTV, false, "CS native immediate RTVs", rtvHeap) &&
				     heap(rhi::DescriptorHeapType::DSV, false, "CS native immediate DSVs", dsvHeap) &&
				     heapOf(rhi::DescriptorHeapType::CbvSrvUav, kSlots * kUavsPerSlot, true, "CS native immediate UAVs", uavHeap) &&
				     heapOf(rhi::DescriptorHeapType::CbvSrvUav, kSlots * kUavsPerSlot, false, "CS native immediate UAVs (CPU)", uavCpuHeap);
				for (uint32_t i = 0; i < kSlots; ++i) {
					auto& slot = slots[i];
					ok = ok && device.CreateCommandAllocator(rhi::QueueKind::Graphics, slot.allocator) == rhi::Result::Ok && slot.allocator &&
					     device.CreateCommandList(rhi::QueueKind::Graphics, slot.allocator.Get(), slot.list) == rhi::Result::Ok && slot.list;
					if (ok) {
						slot.list->SetName(fmt::format("CS native immediate {}", i).c_str());
						// A list is made recording; it begins again at its first use.
						slot.list->End();
					}
				}
				return ok && device.CreateTimeline(timeline, 0, "CS native immediate context") == rhi::Result::Ok && timeline;
			}

			/**
			 * @brief Records a_record on the context's own list and submits it on the graphics queue: a_record gets the list and the
			 * slot's descriptors, and records its own barriers, which order it with the work before and after. a_keep lives until the
			 * work is done; each of a_used gets the work's value as its last use. Returns that value, 0 when the submission failed
			 * (logged). Only for work before the render graph is up (no graph epoch runs then): from then on the context's work goes
			 * through Route. Any thread, one at a time.
			 */
			uint64_t RunRhi(const std::string& a_label, const std::function<void(rhi::CommandList&, const Descriptors&)>& a_record,
				std::vector<std::shared_ptr<const void>> a_keep, std::span<NativeResource* const> a_used)
			{
				return Submit(
					a_label,
					[&](Slot& a_slot, uint32_t a_index) {
						const Descriptors descriptors = DescriptorsOf(a_index);
						BindHeaps(a_slot.list.Get());
						a_record(a_slot.list.Get(), descriptors);
					},
					std::move(a_keep), a_used);
			}

			/**
			 * @brief BasicRHI commands through ORG's external client (ExternalWork), once the render graph is up: the one route the
			 * context's work takes then. a_accesses names each tracked image's graph resource in the state its commands need, so the
			 * client moves it there from wherever the ledger says (closed: and back to its home after). a_untracked are the operation's
			 * other images, which keep the context's contract: they rest in Common (an exempt one in the shader-read state, which it
			 * leaves around the work), and a_record moves them itself. A slot's descriptors serve the commands, so they stay valid until
			 * the work is done. The work signals this context's timeline, so Map, Wait and the queries cover it. Returns the work's value,
			 * 0 when the client did not take it (the operation is lost; ExternalWork logs it). Any thread, one at a time.
			 */
			uint64_t Route(const std::string& a_label, std::span<const ::RenderGraph::ExternalWork::Access> a_accesses, std::span<NativeResource* const> a_untracked,
				const std::function<void(rhi::CommandList&, const Descriptors&)>& a_record, std::vector<std::shared_ptr<const void>> a_keep,
				std::span<NativeResource* const> a_used)
			{
				// A region's held anchor runs go first, before this context's lock: submitting them can run CS code that uses the context
				// again (Route from inside Route threw "resource deadlock would occur"). ExternalWork::Submit's own flush then finds nothing.
				::FrameAnchors::FlushRegions("native context");
				std::lock_guard guard(lock);
				const uint32_t index = AcquireSlotLocked();
				auto& slot = slots[index];
				const Descriptors descriptors = DescriptorsOf(index);
				// The client submits on this queue too. The lock keeps this context's signals in order.
				const rhi::TimelinePoint signal{ timeline->GetHandle(), value + 1 };
				const bool submitted = ::RenderGraph::ExternalWork::Submit(
					a_label, a_accesses,
					[&](rhi::CommandList& a_list) {
						BeginLabel(a_list, a_label);
						BindHeaps(a_list);
						LeaveShaderRead(a_list, a_untracked, false);
						a_record(a_list, descriptors);
						LeaveShaderRead(a_list, a_untracked, true);
						EndLabel(a_list);
					},
					{}, { &signal, 1 });
				if (!submitted)
					return 0;
				slot.value = ++value;
				slot.keep = std::move(a_keep);
				for (auto* used : a_used)
					if (used)
						used->lastUse.store(value, std::memory_order_release);
				if (!a_accesses.empty())
					::RenderGraph::RawAccess::Count(::RenderGraph::RawAccess::Kind::Routed);
				return value;
			}

			/**
			 * @brief Work that records its own barriers and names no image a graph tracks (a new image's first contents): Route once the
			 * render graph is up, else RunRhi.
			 */
			uint64_t RunUntracked(const std::string& a_label, const std::function<void(rhi::CommandList&, const Descriptors&)>& a_record,
				std::vector<std::shared_ptr<const void>> a_keep, std::span<NativeResource* const> a_used)
			{
				if (RenderGraphRuntime::Get().IsActive())
					return Route(a_label, {}, {}, a_record, std::move(a_keep), a_used);
				return RunRhi(a_label, a_record, std::move(a_keep), a_used);
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

			/** @brief The next slot, once the GPU is done with its last work; what that work kept is released. With the lock. */
			uint32_t AcquireSlotLocked()
			{
				const uint32_t index = static_cast<uint32_t>(next++ % kSlots);
				auto& slot = slots[index];
				if (slot.value && timeline->GetCompletedValue() < slot.value)
					(void)timeline->HostWait(slot.value);
				slot.keep.clear();
				return index;
			}

			/** @brief Slot a_index's descriptors (none without the BasicRHI context's heaps). */
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

			/** @brief Binds the shader-visible UAV heap, which UAV clears and the mip chain's dispatches read. */
			void BindHeaps(rhi::CommandList& a_list) const
			{
				if (uavHeap)
					a_list.SetDescriptorHeaps(uavHeap->GetHandle(), std::nullopt);
			}

			template <class Body>
			uint64_t Submit(const std::string& a_label, Body&& a_body, std::vector<std::shared_ptr<const void>> a_keep, std::span<NativeResource* const> a_used)
			{
				std::lock_guard guard(lock);
				const uint32_t index = AcquireSlotLocked();
				auto& slot = slots[index];
				slot.allocator->Recycle();
				slot.list->Recycle(slot.allocator.Get());
				// The operation's name and its resource, so a device fault's crash markers name the work that ran.
				BeginLabel(slot.list.Get(), a_label);
				// An exempt texture rests in the shader-read state, and the operations expect Common: it goes there for the operation and
				// back after it.
				LeaveShaderRead(slot.list.Get(), a_used, false);
				a_body(slot, index);
				LeaveShaderRead(slot.list.Get(), a_used, true);
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
				for (auto* record : a_used)
					if (record)
						record->lastUse.store(value, std::memory_order_release);
				return value;
			}

			/**
			 * @brief Moves every exempt image of a_used from the shader-read state into Common (a_back false), or back (a_back true).
			 * Images still being uploaded (restsShaderRead false) are left alone.
			 */
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
			rhi::Device device;
			rhi::Queue queue;
			rhi::DescriptorHeapPtr rtvHeap, dsvHeap, uavHeap, uavCpuHeap;
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

		/** @brief A typeless depth format's shader resource twin (R24_UNORM_X8 for R24G8, ...), or the format itself. */
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

		/** @brief A texture native mode refused to make, with the whole request: the first 32 are logged, each with its reason. */
		void LogRefused(const TextureRequest& a_request, std::string_view a_why);

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

		/**
		 * @brief A BasicRHI SRV swizzle (D3D12's shader component mapping encoding: three bits a channel, 0-3 a source channel, 4
		 * zero, 5 one; 0 for none) as Vulkan components.
		 */
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

		/**
		 * @brief A depth-stencil format's D3D11 texel size: 4 bytes (D24S8: depth in the low 24 bits, stencil in the top byte) or 8
		 * (D32S8X24: float depth, then the stencil byte). 0 for a format without stencil.
		 */
		uint32_t StencilTexelBytes(const Shape& a_shape)
		{
			return (a_shape.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? a_shape.blockBytes : 0u;
		}

		/**
		 * @brief One subresource's depth-stencil texels between D3D11's interleaved form (a_main, a_texel bytes each) and the GPU's
		 * planes: a_depth (4 bytes a texel: a float when a_float, else 24-bit UNORM in the low bits) and a_stencil (1 byte a texel).
		 * a_unpack: planes to D3D11's form, else D3D11's form to planes.
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

		/** @brief The request's description as the censuses read it (NativeResource::bindFlags and the rest). */
		void NoteRequest(NativeResource& a_record, const TextureRequest& a_request)
		{
			a_record.bindFlags = a_request.bindFlags;
			a_record.d3dUsage = a_request.usage;
			a_record.dimension = a_request.dimension;
			a_record.initialData = a_request.data && a_request.data->pSysMem;
		}

		/**
		 * @brief Whether a texture is exempt from tracking: only a shader resource (no render target, depth or UAV binding), not
		 * DYNAMIC and without CPU access, and made with its contents. The write census found one such texture written after creation
		 * in a whole session (the loading screen's), which NoteWrite promotes.
		 */
		bool IsExemptRequest(const TextureRequest& a_request)
		{
			return (a_request.bindFlags & ~UINT(D3D11_BIND_SHADER_RESOURCE)) == 0 && (a_request.bindFlags & D3D11_BIND_SHADER_RESOURCE) &&
			       a_request.usage != D3D11_USAGE_DYNAMIC && a_request.usage != D3D11_USAGE_STAGING && !a_request.cpuAccess && a_request.sampleCount <= 1 &&
			       a_request.data && a_request.data->pSysMem;
		}

		/** @brief A tracked texture kept host-made for a_why: logged once per reason, with the first texture's name. */
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

		/**
		 * @brief The lock around every access to the device's queues, which BasicRHI takes through its sharing hooks
		 * (rhi::vulkan::ShareVulkanDevice). Every submission is BasicRHI's now. The shared device is kept for its whole-device waits,
		 * which wait for BasicRHI's timelines instead of vkDeviceWaitIdle.
		 */
		std::mutex g_queueLock;

		void LockQueue(void*, VkQueue) { g_queueLock.lock(); }
		void UnlockQueue(void*, VkQueue) { g_queueLock.unlock(); }

		/**
		 * @brief RenderGraphRuntime::EmitCommands in native mode, on both backends: a client's BasicRHI recording into a command
		 * list of its own on the graphics queue, submitted at the call with the client's timeline waits and signals, between full
		 * memory barriers. For work that is not a graph pass (DCLF's frame signal, a site not yet moved into its epoch). A site's
		 * draws belong in its epoch as a pass recorded at execution, which the graph records inline (org::GraphHost::
		 * SetInlineAtExecution): no submission of its own.
		 */
		class Commands
		{
		public:
			bool Init(rhi::Device a_device)
			{
				queue = a_device.GetQueue(rhi::QueueKind::Graphics);
				for (auto& slot : slots) {
					if (a_device.CreateCommandAllocator(rhi::QueueKind::Graphics, slot.allocator) != rhi::Result::Ok || !slot.allocator ||
						a_device.CreateCommandList(rhi::QueueKind::Graphics, slot.allocator.Get(), slot.list) != rhi::Result::Ok || !slot.list)
						return false;
					slot.list->SetName("CS native commands");
					// A list is made recording; it begins again at its first use (D3D12 resets only a closed list).
					slot.list->End();
				}
				return a_device.CreateTimeline(timeline, 0, "CS native commands") == rhi::Result::Ok && timeline;
			}

			/**
			 * @brief The caller's contract (RenderGraphRuntime::EmitCommands): a_record runs exactly once when this returns true, and
			 * never when it returns false. It starts with nothing bound and binds what it uses. Any thread, one call at a time.
			 */
			bool Run(void (*a_record)(void*, rhi::CommandList&), void* a_user, std::span<const rhi::TimelinePoint> a_waits,
				std::span<const rhi::TimelinePoint> a_signals, const char* a_label)
			{
				std::lock_guard guard(lock);
				auto& slot = slots[next++ % kSlots];
				if (slot.value && timeline->GetCompletedValue() < slot.value && timeline->HostWait(slot.value) != rhi::Result::Ok)
					return false;
				slot.allocator->Recycle();
				slot.list->Recycle(slot.allocator.Get());
				auto list = slot.list.Get();
				BeginLabel(list, a_label ? a_label : "CS native commands");
				// The work lands between the graph's submissions before and after it, as DXVK's stream would put it: ordered by the
				// queue, with every access before and after it made visible.
				auto full = rhi::FullMemoryBarrier();
				rhi::BarrierBatch barriers{};
				barriers.globals = { &full, 1u };
				list.Barriers(barriers);
				if (a_record)
					a_record(a_user, list);
				list.Barriers(barriers);
				EndLabel(list);
				list.End();
				thread_local std::vector<rhi::TimelinePoint> waits, signals;
				waits.assign(a_waits.begin(), a_waits.end());
				signals.assign(a_signals.begin(), a_signals.end());
				signals.push_back({ timeline->GetHandle(), value + 1 });
				rhi::CommandList lists[] = { list };
				rhi::SubmitDesc submit{};
				submit.waits = { waits.data(), static_cast<uint32_t>(waits.size()) };
				submit.signals = { signals.data(), static_cast<uint32_t>(signals.size()) };
				if (queue.Submit({ lists, 1 }, submit) != rhi::Result::Ok) {
					// The recording ran, so the call counts as taken. Its signals are not given: a failed submission is a lost device.
					static std::atomic_flag logged;
					if (!logged.test_and_set())
						logger::error("[ORG native] commands ('{}') could not be submitted; such work is lost (logged once)", a_label ? a_label : "");
					slot.value = 0;
					return true;
				}
				slot.value = ++value;
				return true;
			}

		private:
			// Out-of-graph work is a few calls a frame: enough lists that a reuse never waits for recent work.
			static constexpr uint32_t kSlots = 64;

			struct Slot
			{
				rhi::CommandAllocatorPtr allocator;
				rhi::CommandListPtr list;
				uint64_t value = 0;
			};

			std::mutex lock;
			rhi::Queue queue;
			std::array<Slot, kSlots> slots;
			rhi::TimelinePtr timeline;
			uint64_t value = 0;
			uint64_t next = 0;
		};

		uint64_t CopyRhi(const std::string& a_label, const std::shared_ptr<NativeResource>& a_destination, const std::shared_ptr<NativeResource>& a_source, bool a_all,
			uint32_t a_destinationSub, uint32_t a_x, uint32_t a_y, uint32_t a_z, uint32_t a_sourceSub, const D3D11_BOX* a_box, bool& a_recorded, bool* a_routed);

		struct NativeState;
		NativeState* g_device = nullptr;

		/** @brief Native mode's device: BasicRHI's device, the swap chain, the back buffer and the present's objects. */
		struct NativeState
		{
			static constexpr uint32_t kPresentSlots = 3;

			std::shared_ptr<rhi::DevicePtr> device;
			rhi::Queue graphics;
			Immediate immediate;
			// GenerateMips' compute programs (ORG's GenerateMips.hlsl): for 2D textures, and for arrays and cubes. Loaded once the render
			// graph is up (LoadMipPrograms).
			std::shared_ptr<const GraphPasses::GenerateMipsProgram> mips2D, mipsArray;
			Commands commands;

			// The swap chain on the game's window, and the back buffer the engine draws into (through the graph's passes).
			rhi::SwapchainPtr swapchain;
			HWND window = nullptr;
			DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
			rhi::Format swapchainFormat = rhi::Format::R8G8B8A8_UNorm;
			uint32_t bufferCount = 2;
			uint32_t width = 0, height = 0;
			uint32_t frameLatency = 2;
			winrt::com_ptr<ID3D11Texture2D> backBuffer;

			// The present's copies, one command list per slot, reused once the timeline passed its last use.
			std::array<rhi::CommandAllocatorPtr, kPresentSlots> allocators;
			std::array<rhi::CommandListPtr, kPresentSlots> lists;
			std::array<uint64_t, kPresentSlots> slotValues{};
			rhi::TimelinePtr presentTimeline;
			uint64_t presentValue = 0;
			uint64_t presents = 0;
			bool resizeRequested = false;
			// CS_ORG_NATIVE_PRESENT_PROBE=1: about once a second, right after a present's copy is submitted, the context copies the back
			// buffer into a staging texture of its own, as a D3D11 capture does (CopyResource into STAGING, then Map), and a later
			// present logs its mean colour. It reads through the capture path, on the same queue after the frame's work.
			struct Probe
			{
				std::shared_ptr<NativeResource> staging;
				uint64_t value = 0;  // the context work that copies it, 0 when none is pending
				std::chrono::steady_clock::time_point last{};
			} probe;
			std::shared_ptr<NativeResource> backBufferRecord;  // the engine's back buffer: its record (CurrentOf: the version it stands for)
			// The graph present (BasicRenderer's presentation colour): the back buffer the engine sees stands for one graph-owned
			// texture per present slot, published as the versions of one slot. Every importer of the back buffer gets the slot, so the
			// frame's passes write the current version. The present's tail copies that version into the swap chain image through
			// ORG's external client, and the next present slot's version becomes current. Empty when the switch is off, before the
			// render graph is up, on D3D12, or when the versions could not be made.
			struct GraphBackBuffer
			{
				std::vector<std::shared_ptr<NativeResource>> records;  // one graph-owned record per version
				std::vector<std::shared_ptr<const org::TextureVersion>> versions;
				std::shared_ptr<org::VersionedTexture> slot;
				uint32_t current = 0;  // the version the frame draws into
				uint64_t tails = 0;    // presents copied through the tail
			} graph;
			std::atomic<bool> lost = false;
			std::mutex presentLock;

			// CS_ORG_NATIVE_VALIDATION=1: BasicRHI queues the validation layer's messages; each present drains them into the log.
			bool validation = false;

			winrt::com_ptr<IDXGIAdapter> adapter;
			ID3D11Device* deviceHandle = nullptr;  // the engine holds it for the process
			winrt::com_ptr<IDXGIDevice1> dxgiDevice;

			std::atomic<bool> loggedCpuAccess = false, loggedFormat = false, loggedCreate = false, loggedUpload = false, loggedMultisample = false;

			rhi::Device Rhi() { return device->Get(); }

			/**
			 * @brief The texture, by class:
			 * - exempt (IsExemptRequest): a BasicRHI image whose contents go in through InitializeShaderRead, resting in the shader-read
			 *   state from then on;
			 * - tracked: a persistent ORG texture (MakeGraphOwned) once the render graph is up, resting in Common between executions;
			 * - otherwise (before the graph, or a texture ORG cannot describe): a BasicRHI image in Common, its initial data uploaded.
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
				// D3D12: a depth texture made typeless (D3D11's way to give it a depth view and a shader resource view) stays typeless, as
				// D3D11 makes it. A typed D24/D32 resource cannot have an SRV there; its DSVs and SRVs name their own formats instead.
				// Vulkan resolves it to the depth format (its views reinterpret the aspect).
				const bool typelessDepth = OnD3D12() && (a_request.bindFlags & D3D11_BIND_DEPTH_STENCIL) && dxgiFormat != a_request.format;
				const rhi::Format format = rhi::helpers::ToRHI(typelessDepth ? a_request.format : dxgiFormat);
				const rhi::Format viewFormat = rhi::helpers::ToRHI(dxgiFormat);
				rhi::FormatSupport support{};
				std::string refused;
				if (typelessDepth) {
					// The depth view's format must be a depth format, and the SRV's colour twin readable.
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
					// D3D12 checks every binding, as its resource creation does. Vulkan keeps the checks it had.
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

				// An exempt image is described in the shader-read state it rests in; any other rests in Common (GENERAL).
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
					NoteOwnedImage(*record);
					a_out = std::move(record);
					return S_OK;
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

			/**
			 * @brief A tracked texture made by ORG (Phase 2 of org-native-frame-graph.md): a persistent PixelBuffer, materialized and never
			 * aliased, named by a stable id ("cs.native.texture.<serial>"). The record imports its image for the context's operations and
			 * the D3D11 views. It is moved to Common (with its contents) at creation and its tracker says so; from then on the ledgers
			 * carry its layout, and the context's work takes it from them (Immediate::Route). False, with nothing made, when ORG cannot make it as D3D11 asked (logged once per
			 * reason): the caller makes it with BasicRHI instead.
			 */
			bool MakeGraphOwned(const TextureRequest& a_request, DXGI_FORMAT a_dxgiFormat, rhi::Format a_format, rhi::Format a_viewFormat, const rhi::FormatSupport& a_support,
				bool a_typelessDepth, bool a_cube, uint32_t a_mips, uint32_t a_layers, uint32_t a_depth, std::shared_ptr<NativeResource>& a_out)
			{
				// The ORG runtime exists once the render graph is up; the engine's startup textures come before it.
				if (!RenderGraphRuntime::Get().IsActive())
					return false;
				const bool depthStencil = (a_request.bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
				const bool renderTarget = (a_request.bindFlags & D3D11_BIND_RENDER_TARGET) != 0;
				const bool unordered = (a_request.bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
				const bool volume = a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
				const char* kept = nullptr;
				if (a_request.dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
					kept = "a 1D texture";
				// GENERATE_MIPS only asks the context for mip generation, which blits within the image whoever made it.
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
				// A depth texture is read in its colour twin (R24_UNORM_X8 for R24G8, as GraphResources' depth textures are).
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
				// One entry per slice and mip, slice-major, as a PixelBuffer reads them.
				for (uint32_t slice = 0; slice < a_layers; ++slice)
					for (uint32_t mip = 0; mip < a_mips; ++mip)
						desc.imageDimensions.push_back({ (std::max)(a_request.width >> mip, 1u), (std::max)(a_request.height >> mip, 1u), 0, 0 });

				auto record = std::make_shared<NativeResource>();
				record->name = a_request.name;
				NoteRequest(*record, a_request);
				try {
					auto texture = org::PixelBuffer::CreateShared(desc);
					texture->SetName(fmt::format("cs.native.texture.{}", record->serial));
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
				// Its contents through ORG's external client: the ledger moves it out of Undefined and knows where it is after.
				if (!InitializeTracked(*record, a_request, a_format, a_mips, a_layers, a_depth)) {
					LogOnce(loggedUpload, a_request.name, "its initial layout or contents could not be written");
					return false;
				}
				record->ownership = NativeResource::Ownership::Tracked;
				NoteOwnedImage(*record);
				// The image's one graph resource, for the duplicate check (imports find it through GraphTextureOf).
				::RenderGraph::NoteImageResource(reinterpret_cast<std::uint64_t>(record->image.image), record->graphOwned, fmt::format("cs.native.texture.{}", record->serial));
				// A DYNAMIC texture's CPU side, which Map hands out and Unmap copies in.
				if (a_request.usage == D3D11_USAGE_DYNAMIC) {
					TextureRequest cpu = a_request;
					cpu.data = nullptr;
					cpu.name = a_request.name + " (upload)";
					if (FAILED(MakeStaging(cpu, true, record->upload)))
						return false;
				}
				a_out = std::move(record);
				return true;
			}

			/**
			 * @brief An image record's description as importers read it (DescribeResource), in the layout the image rests in
			 * (a_resting): the shader-read layout for an exempt texture, GENERAL (Common, the home every execution gives it back in
			 * while external home states are on) for any other.
			 */
			static bool FillImage(NativeResource& a_record, const rhi::Resource& a_resource, const TextureRequest& a_request, DXGI_FORMAT a_dxgiFormat, rhi::Format a_format,
				const rhi::FormatSupport& a_support, uint32_t mips, uint32_t layers, uint32_t depth, VkImageLayout a_resting)
			{
				// On D3D12 the description carries the ID3D12Resource where the VkImage goes (RenderGraph::ImportDxvkImage imports it
				// with rhi::dx12::import_resource), and no Vulkan format, flags or usage.
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
				// A format BasicRHI stores as another (A8 as R8, the 16-bit packed formats, ...) is read through the swizzle its views add,
				// as DXVK describes its own images: the graph's imports and DCLF's views then read the channels the format names.
				image.components = ComponentsOf(a_support.srvSwizzle);
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
				// A depth-stencil format: room for the GPU's two planes of each subresource, after the CPU's texels. Its copies to and
				// from images go through them (4-byte depth texels, 1-byte stencil texels).
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
				if (Rhi().CreateCommittedResource(desc, a_record.resource) != rhi::Result::Ok || !a_record.resource || !DescribeBuffer(a_record, a_record.resource.Get())) {
					LogOnce(loggedCreate, a_record.name, "BasicRHI could not create the buffer");
					return false;
				}
				a_record.size = a_size;
				a_record.hostVisible = a_heap != rhi::HeapType::DeviceLocal;
				return true;
			}

			/**
			 * @brief A buffer record's native handle and address, as DescribeResource hands them out: the VkBuffer and its device
			 * address on Vulkan; on D3D12 the ID3D12Resource in the VkBuffer's place and its GPU virtual address.
			 */
			static bool DescribeBuffer(NativeResource& a_record, const rhi::Resource& a_buffer)
			{
				if (OnD3D12()) {
					void* resource = D3D12ResourceOf(a_buffer);
					if (!resource)
						return false;
					a_record.vkBuffer = reinterpret_cast<VkBuffer>(resource);
					// Its GPU virtual address (an index buffer view's); shaders read the buffer through a view ref of its import (RenderGraph::ImportBufferRef).
					a_record.address = static_cast<VkDeviceAddress>(g_device ? g_device->Rhi().GetBufferDeviceAddress({ a_buffer.GetHandle(), 0 }) : D3D12AddressOf(resource));
					a_record.usage = 0;
					return true;
				}
				rhi::VulkanResourceInfo info{};
				if (!rhi::vulkan::get_resource_info(a_buffer, info) || !info.resource)
					return false;
				a_record.vkBuffer = rhi::vulkan::from_native_void<VkBuffer>(info.resource);
				a_record.address = static_cast<VkDeviceAddress>(info.deviceAddress);
				a_record.usage = static_cast<VkBufferUsageFlags>(info.usage);
				return true;
			}

			/** @brief GenerateMips' programs, once the render graph's shader compiler is up. Render thread, at startup. */
			void LoadMipPrograms()
			{
				mips2D = GraphPasses::LoadGenerateMips(Rhi(), false);
				mipsArray = GraphPasses::LoadGenerateMips(Rhi(), true);
				logger::info("[ORG native] GenerateMips: the context builds mip chains with a compute shader ({}2D textures, {}arrays and cubes)",
					mips2D ? "" : "not for ", mipsArray ? "" : "not for ");
			}

			/**
			 * @brief GenerateMips' scratch texture for a_image: a texture of its size, mips and slices with unordered access, in
			 * a_format (the UNORM twin of an sRGB format), resting in Common. Null when it cannot be made.
			 */
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
				// Undefined to Common, where it rests between mip chains.
				const rhi::TextureSubresourceRange all{ 0, shape.mips, 0, shape.layers, 0, 1 };
				const auto handle = record->resource->GetHandle();
				NativeResource* const used[] = { record.get() };
				const auto initialize = [&](rhi::CommandList& a_list, const Immediate::Descriptors&) {
					RecordInitialize(a_list, handle, all, {}, {}, rhi::ResourceLayout::Common, rhi::ResourceAccessType::Common);
				};
				return immediate.RunUntracked("mip scratch " + a_image.name, initialize, {}, used) ? record : nullptr;
			}

			/** @brief A short-lived device buffer for one copy's rows (CopyRhi's bounce), kept until that copy's work is done. */
			std::shared_ptr<NativeResource> MakeBounce(uint64_t a_size)
			{
				auto record = std::make_shared<NativeResource>();
				record->name = "Native::Bounce";
				if (!MakeBufferResource(*record, (std::max)(a_size, uint64_t(1)), rhi::HeapType::DeviceLocal, false))
					return nullptr;
				return record;
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

			/** @brief One subresource of a new image's initial data in its upload buffer (D3D11's order: mips of slice 0, then slice 1, ...). */
			struct InitialCopy
			{
				uint32_t mip, slice;
				rhi::CopyableFootprint footprint;
			};

			/**
			 * @brief A new image's initial data in an upload buffer (a_upload, none without data), one footprint per subresource, and the
			 * range its first barrier covers (a_all: both planes of a D3D12 depth-stencil texture). False when it cannot be staged.
			 */
			bool StageInitialData(rhi::ResourceHandle a_texture, const TextureRequest& a_request, rhi::Format a_format, uint32_t a_mips, uint32_t a_layers,
				uint32_t a_depth, rhi::ResourcePtr& a_upload, std::vector<InitialCopy>& a_copies, rhi::TextureSubresourceRange& a_all)
			{
				const rhi::ResourceHandle texture = a_texture;
				a_copies.clear();
				// D3D12 lays a depth-stencil texture's stencil out as a second plane, with a layout of its own: both start defined.
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

			/**
			 * @brief Records a new image's first barrier from Undefined, its initial data's copies, and the move into a_layout (with
			 * access a_access), whose barrier makes the copies visible to all later work on the queue.
			 */
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
			 * @brief Moves a new image into Common (GENERAL on Vulkan), writing its initial data on the way. Waits for the GPU, so the
			 * image is ready when the handle exists. Common is the home every graph execution gives a host-made or tracked texture back
			 * in until Phase 4 turns external home states off, and the layout the immediate context's raw operations expect.
			 */
			bool Initialize(rhi::ResourceHandle a_texture, const TextureRequest& a_request, rhi::Format a_format, uint32_t a_mips, uint32_t a_layers, uint32_t a_depth)
			{
				auto upload = std::make_shared<rhi::ResourcePtr>();
				std::vector<InitialCopy> copies;
				rhi::TextureSubresourceRange all{};
				if (!StageInitialData(a_texture, a_request, a_format, a_mips, a_layers, a_depth, *upload, copies, all))
					return false;
				const std::string label = (copies.empty() ? "initial layout " : "initial contents ") + a_request.name;
				// An image no graph tracks yet: through ORG's external client once the graph is up (on the context's own list before).
				// Waited for, so a graph that imports it later on another queue finds it written.
				const uint64_t value = immediate.RunUntracked(
					label,
					[&](rhi::CommandList& a_list, const Immediate::Descriptors&) {
						RecordInitialize(a_list, a_texture, all, *upload, copies, rhi::ResourceLayout::Common, rhi::ResourceAccessType::Common);
					},
					{ upload }, {});
				return value && immediate.Wait(value);
			}

			/**
			 * @brief A tracked texture's first contents (MakeGraphOwned, with the BasicRHI context), through ORG's external client: its
			 * PixelBuffer is declared as a copy destination (in Common when there is nothing to copy), so the ledger moves it out of
			 * Undefined and holds where it is after. No wait: every later use, graph or context, is ordered by that ledger.
			 */
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
				return immediate.Route((copies.empty() ? "initial layout " : "initial contents ") + a_request.name, { &access, 1 }, {}, record, { upload }, used) != 0;
			}

			/**
			 * @brief The transfer of an exempt texture (BasicRenderer's MaterialTextureTransferService): its contents are copied in and it
			 * ends in the shader-read state, where it rests from then on. Submitted on the graphics queue without a wait; the image is
			 * ready once the immediate context's timeline reaches the returned value (0 on failure). The graph's work on that queue is
			 * submitted after it, so it is ordered by the transfer's last barrier; ImmutableTextureOf waits for the value before it hands
			 * the texture to a graph.
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

			/** @brief A buffer: device-local without CPU access or contents, else in upload memory with its contents written. */
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
				backBufferRecord = record;
				D3D11_TEXTURE2D_DESC desc{ width, height, 1, 1, format, { 1, 0 }, D3D11_USAGE_DEFAULT, request.bindFlags, 0, 0 };
				Tripwire::Options options;
				options.name = request.name;
				options.owner = std::move(record);
				backBuffer = Tripwire::CreateTexture2D(desc, std::move(options));
				return backBuffer != nullptr;
			}

			/** @brief Forgets the graph present's versions: the made-image registry lets go of them, and the back buffer is its own again. */
			void DropGraphBackBuffer()
			{
				for (const auto& record : graph.records)
					if (record && record->graphOwned)
						::RenderGraph::ForgetMadeImage(record->graphOwned.get());
				if (backBufferRecord)
					Rename(*backBufferRecord, nullptr);
				graph = {};
			}

			/**
			 * @brief The graph present: makes the back buffer's graph-owned versions at its size and format, one slot for all
			 * of them, and points the engine's back buffer at the first. Each version's image is registered as a made image of the slot
			 * (RegisterMadeImage, SetMadeImageSlot), so every importer of the back buffer (the anchors' engine target, ShareImage,
			 * ImportTexture) gets the slot, and the graph's passes declare it. With the present lock, once the render graph is up.
			 * @return False, logged, when the versions could not be made: the present copies by hand then.
			 */
			bool MakeGraphBackBuffer()
			{
				// One version per present slot, as BasicRenderer's presentation colours.
				const uint32_t count = kPresentSlots;
				DropGraphBackBuffer();
				if (!backBufferRecord || !RenderGraphRuntime::Get().IsActive())
					return false;
				GraphBackBuffer made;
				for (uint32_t i = 0; i < count; ++i) {
					TextureRequest request;
					request.width = width;
					request.height = height;
					request.format = format;
					request.bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
					request.name = fmt::format("Native::BackBuffer.{}", i);
					std::shared_ptr<NativeResource> record;
					if (FAILED(MakeTexture(request, record)) || !record || record->ownership != NativeResource::Ownership::Tracked || !record->graphOwned) {
						logger::warn(
							"[ORG native] graph present: back buffer version {} could not be made as a graph-owned texture ({}x{}, DXGI format {}); "
							"the present copies the back buffer by hand",
							i, width, height, static_cast<int>(format));
						return false;
					}
					made.records.push_back(std::move(record));
				}
				try {
					made.slot = org::VersionedTexture::Create(made.records.front()->graphOwned);
					made.versions.push_back(made.slot->Current());
					for (uint32_t i = 1; i < count; ++i)
						made.versions.push_back(made.slot->Make(made.records[i]->graphOwned));
				} catch (const std::exception& e) {
					logger::warn("[ORG native] graph present: the back buffer's slot could not be made ({}); the present copies the back buffer by hand", e.what());
					return false;
				}
				for (const auto& record : made.records) {
					::RenderGraph::RegisterMadeImage(reinterpret_cast<std::uint64_t>(record->image.image), record->graphOwned, record->graphOwned->GetDescription(),
						"Native::BackBuffer");
					::RenderGraph::SetMadeImageSlot(record->graphOwned.get(), made.slot);
				}
				graph = std::move(made);
				Rename(*backBufferRecord, graph.records.front());
				logger::info(
					"[ORG native] graph present: the back buffer is a graph-owned slot of {} version{} ({}x{}, DXGI "
					"format {}); the present copies the current one into the swap chain image through ORG's external client{}",
					count, count == 1 ? "" : "s", width, height, static_cast<int>(format), count > 1 ? ", and the next present slot's version becomes current" : "");
				return true;
			}

			/**
			 * @brief After a tail: the next present slot's version becomes the slot's current one, and the engine's back buffer points at
			 * it. Tickets prepared for the old version are prepared again (NoteNewVersions). The render owner only, so no frame's
			 * passes are between two versions.
			 */
			void NextGraphVersion()
			{
				if (graph.versions.size() < 2)
					return;
				graph.current = (graph.current + 1) % static_cast<uint32_t>(graph.versions.size());
				graph.slot->Adopt(graph.versions[graph.current]);
				Rename(*backBufferRecord, graph.records[graph.current]);
				if (auto* host = RenderGraphRuntime::Get().Host())
					host->NoteNewVersions();
			}

			/** @brief The render graph is up: the graph present's versions. */
			void OnGraphReady()
			{
				std::lock_guard guard(presentLock);
				(void)MakeGraphBackBuffer();
			}

			bool MakePresentObjects()
			{
				for (uint32_t i = 0; i < kPresentSlots; ++i) {
					if (Rhi().CreateCommandAllocator(rhi::QueueKind::Graphics, allocators[i]) != rhi::Result::Ok || !allocators[i] ||
						Rhi().CreateCommandList(rhi::QueueKind::Graphics, allocators[i].Get(), lists[i]) != rhi::Result::Ok || !lists[i])
						return false;
					lists[i]->SetName(fmt::format("CS native present {}", i).c_str());
					lists[i]->End();
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
				// The old back buffer goes with the engine's last reference to it (its views were released before ResizeBuffers). Its
				// graph present versions go with it.
				const bool graphPresent = graph.slot != nullptr;
				DropGraphBackBuffer();
				backBuffer = nullptr;
				if (!MakeBackBuffer()) {
					logger::error("[ORG native] the back buffer could not be made again at {}x{}", width, height);
					return E_OUTOFMEMORY;
				}
				if (graphPresent) {
					// The new versions are a new slot: graphs built with the old one are built again.
					::GraphResources::RequestStructuralRebuild("native back buffer resized: its graph present slot is made again");
					(void)MakeGraphBackBuffer();
				}
				logger::info("[ORG native] swap chain and back buffer resized to {}x{}", width, height);
				return S_OK;
			}

			/**
			 * @brief Makes the swap chain again at the back buffer's size, after Present reported a changed mode or a failure.
			 * The back buffer stays: the engine did not ask for this, so it keeps drawing through its views of the current one.
			 * (Making a new back buffer here left the engine drawing into the old one, and every present copied the new, black one.)
			 */
			void RecreateSwapchain()
			{
				WaitPresents();
				if (swapchain->ResizeBuffers(bufferCount, width, height, swapchainFormat, 0) != rhi::Result::Ok)
					logger::error("[ORG native] the swap chain could not be made again at {}x{}", width, height);
				else
					logger::info("[ORG native] swap chain made again at {}x{} (the back buffer is kept)", width, height);
			}

			HRESULT Present(UINT a_syncInterval, UINT a_flags)
			{
				std::lock_guard guard(presentLock);
				if (a_flags & DXGI_PRESENT_TEST)
					return S_OK;
				if (lost.load(std::memory_order_relaxed))
					return S_OK;  // the device-loss report and the graph's stop already named it; the game's frame still ends
				if (std::exchange(resizeRequested, false))
					RecreateSwapchain();
				if (!backBuffer)
					return S_OK;
				static const bool probeEnabled = Switches::On("CS_ORG_NATIVE_PRESENT_PROBE");
				if (probeEnabled)
					ReadProbe();

				const uint32_t slot = static_cast<uint32_t>(presents % kPresentSlots);
				if (slotValues[slot] && presentTimeline->HostWait(slotValues[slot]) != rhi::Result::Ok) {
					lost = true;
					logger::error("[ORG native] a present's copy never finished on the GPU; nothing more is shown");
					return S_OK;
				}
				auto& list = lists[slot];
				allocators[slot]->Recycle();
				list->Recycle(allocators[slot].Get());

				// D3D12: Present does not block for the frame latency, so the frame paces on the swap chain's waitable object here.
				// Vulkan's Present blocks by itself.
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
				// What the frame drew into: the graph present's current version, or the back buffer itself.
				const auto source = CurrentOf(backBufferRecord);
				if (!source)
					return S_OK;
				// The graph present's tail. Only the render owner moves to the next version. A loading screen or the logo
				// presenting from another thread copies the current version below, and it stays current.
				if (graph.slot && source->graphOwned) {
					if (::FrameAnchors::IsRenderOwner()) {
						const HRESULT result = PresentThroughLedger(*source->graphOwned, source, slot, target, a_syncInterval, true);
						if (result != E_PENDING)
							return result;
						static std::atomic_flag logged;
						if (!logged.test_and_set())
							logger::warn(
								"[ORG native] graph present: ORG's external client refused the present's tail; the present copies on its own "
								"path, and the version stays current (logged once)");
					} else {
						static std::atomic_flag logged;
						if (!logged.test_and_set())
							logger::info(
								"[ORG native] graph present: a present on thread {}, which does not own rendering (a loading screen or the "
								"logo), copies the current version on the old path; the version stays current (logged once)",
								::GetCurrentThreadId());
					}
				}
				// The back buffer is wherever the frame's last pass left it: the copy goes through ORG's external client, which takes its
				// layout from the ledger.
				if (const auto ledger = LedgerResourceOf(*source)) {
					const HRESULT result = PresentThroughLedger(*ledger, source, slot, target, a_syncInterval, false);
					if (result != E_PENDING)
						return result;
				}
				// Before the render graph is up (the logo) nothing has moved the back buffer out of Common, and the queue orders this copy
				// after the frame's writes.
				const rhi::ResourceHandle sourceImage = source->resource->GetHandle();
				const uint32_t copyWidth = (std::min)(source->shape.width, width), copyHeight = (std::min)(source->shape.height, height);
				std::array<rhi::TextureBarrier, 2> before{};
				before[0].texture = sourceImage;
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
				to.width = copyWidth;
				to.height = copyHeight;
				rhi::TextureCopyRegion from{};
				from.texture = sourceImage;
				from.width = copyWidth;
				from.height = copyHeight;
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
				return FinishPresent(signal, a_syncInterval, source);
			}

			/**
			 * @brief The present's copy through ORG's external client. The back buffer's ledger resource (a_backBuffer, a_record's) is read
			 * as a copy source, so the client moves it there from wherever the frame left it, after the frame's last write. With home
			 * states off the ledger holds that state for the next frame's first pass; with them on the client gives it back in Common.
			 * The swap chain image is not the graph's: its barriers are recorded here, as on the own path, and leave it ready to present.
			 * The present waits for the submission's signal.
			 * - a_tail: the graph present's tail. After the present, the next present slot's
			 *   version becomes current (NextGraphVersion).
			 * - Otherwise: the back buffer's copy with home states off.
			 * Returns E_PENDING, with nothing submitted, when the client refused: the caller copies on its own path.
			 */
			HRESULT PresentThroughLedger(org::Resource& a_backBuffer, const std::shared_ptr<NativeResource>& a_record, uint32_t a_slot,
				rhi::ResourceHandle a_target, UINT a_syncInterval, bool a_tail)
			{
				const auto source = a_backBuffer.GetAPIResource().GetHandle();
				// The same size as the swap chain image, unless a mode change made the swap chain again alone (then the smaller one).
				const uint32_t copyWidth = (std::min)(a_record->shape.width, width), copyHeight = (std::min)(a_record->shape.height, height);
				const std::array accesses{ ::RenderGraph::ExternalWork::CopySource(a_backBuffer) };
				const rhi::TimelinePoint signal{ presentTimeline->GetHandle(), presentValue + 1 };
				const bool submitted = ::RenderGraph::ExternalWork::Submit(
					a_tail ? "native present tail" : "native present copy", accesses,
					[&](rhi::CommandList& a_list) {
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
						a_list.CopyTextureRegion(to, from);
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
					a_record, { &signal, 1 });
				if (!submitted)
					return E_PENDING;
				slotValues[a_slot] = ++presentValue;
				const HRESULT result = FinishPresent(signal, a_syncInterval, a_record);
				if (a_tail) {
					// After the present, so the probe in it read the version this frame drew.
					++graph.tails;
					NextGraphVersion();
				}
				return result;
			}

			/** @brief After the present's copy of a_source was submitted (signalling a_signal): the probe, the present itself and its result. */
			HRESULT FinishPresent(const rhi::TimelinePoint& a_signal, UINT a_syncInterval, const std::shared_ptr<NativeResource>& a_source)
			{
				static const bool probeEnabled = Switches::On("CS_ORG_NATIVE_PRESENT_PROBE");
				++presents;
				if (probeEnabled)
					RecordProbe(a_source);
				// The raw access check's counts, every 600 frames.
				if (presents % 600 == 0)
					::RenderGraph::RawAccess::Summary();

				rhi::PresentSyncDesc sync{};
				sync.queue = graphics;
				sync.wait = a_signal;
				const rhi::Result presented = swapchain->Present(a_syncInterval > 0, sync);
				if (presents == 1)
					Stage("first present", fmt::format("{}x{}, result {}", width, height, static_cast<int>(presented)));
				// BasicRHI only queues the validation layer's messages (its messenger callback); they reach the log when drained.
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

			/** @brief About once a second: the context copies a_source, the back buffer just presented, into the probe's staging texture. */
			void RecordProbe(const std::shared_ptr<NativeResource>& a_source)
			{
				const auto now = std::chrono::steady_clock::now();
				if (probe.value || !a_source || now - probe.last < std::chrono::seconds(1))
					return;
				probe.last = now;
				if (!probe.staging || probe.staging->shape.width != width || probe.staging->shape.height != height) {
					TextureRequest request;
					request.width = width;
					request.height = height;
					request.format = format;
					request.usage = D3D11_USAGE_STAGING;
					request.cpuAccess = D3D11_CPU_ACCESS_READ;
					request.name = "Native::PresentProbe";
					probe.staging.reset();
					if (FAILED(MakeStaging(request, false, probe.staging)) || !probe.staging)
						return;
				}
				const auto source = a_source;
				const auto staging = probe.staging;
				bool recorded = false;
				probe.value = CopyRhi("present probe", staging, source, false, 0, 0, 0, 0, 0, nullptr, recorded, nullptr);
				if (!recorded) {
					probe.value = 0;
					static std::atomic_flag logged;
					if (!logged.test_and_set())
						logger::warn("[ORG native] present probe: the context could not copy the back buffer into staging memory (logged once)");
				}
			}

			/** @brief Once the probe's copy is done: logs the back buffer's mean colour and how much of it is not black. */
			void ReadProbe()
			{
				if (!probe.value || !immediate.Done(probe.value) || !probe.staging)
					return;
				probe.value = 0;
				void* mapped = nullptr;
				probe.staging->resource->Map(&mapped);
				if (!mapped)
					return;
				const auto& entry = probe.staging->layout[0];
				const auto* bytes = static_cast<const uint8_t*>(mapped) + entry.offset;
				const bool packed10 = format == DXGI_FORMAT_R10G10B10A2_UNORM;
				const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM;
				double sum[3]{};
				uint64_t lit = 0, pixels = 0;
				const uint32_t probeWidth = probe.staging->shape.width, probeHeight = probe.staging->shape.height;
				for (uint32_t y = 0; y < probeHeight; y += 2) {
					const auto* row = reinterpret_cast<const uint32_t*>(bytes + uint64_t(y) * entry.rowPitch);
					for (uint32_t x = 0; x < probeWidth; x += 2) {
						const uint32_t texel = row[x];
						double r, g, b;
						if (packed10) {
							r = (texel & 0x3FF) / 1023.0;
							g = ((texel >> 10) & 0x3FF) / 1023.0;
							b = ((texel >> 20) & 0x3FF) / 1023.0;
						} else {
							const double c0 = (texel & 0xFF) / 255.0, c2 = ((texel >> 16) & 0xFF) / 255.0;
							r = bgra ? c2 : c0;
							g = ((texel >> 8) & 0xFF) / 255.0;
							b = bgra ? c0 : c2;
						}
						sum[0] += r;
						sum[1] += g;
						sum[2] += b;
						lit += (r + g + b) > 0.0 ? 1 : 0;
						++pixels;
					}
				}
				probe.staging->resource->Unmap(0, 0);
				if (!pixels)
					return;
				logger::info("[ORG native] present probe after {} presents: back buffer {}x{} (DXGI format {}), mean RGB {:.4f} {:.4f} {:.4f}, {:.1f}% of pixels not black",
					presents, probeWidth, probeHeight, static_cast<int>(format), sum[0] / pixels, sum[1] / pixels, sum[2] / pixels, 100.0 * double(lit) / double(pixels));
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
			// GetDesc answers the mips made, as D3D11 does: a description asking for 0 (the whole chain) names the count (ImageSpace
			// checks a mip target's mip against it).
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
				if (registry.live.contains(owner.get())) {
					auto record = std::const_pointer_cast<NativeResource>(std::static_pointer_cast<const NativeResource>(owner));
					// The back buffer under graph present: its current version.
					return record->renamed ? record->renamed : record;
				}
			}
			// A port's handle around a native one (its fields hold handles that wrap the object the native device made).
			const auto info = Tripwire::Describe(a_object);
			return info.wrapped && info.wrapped != a_object && Tripwire::IsHandle(info.wrapped) ? Resolve(info.wrapped) : nullptr;
		}

		/** @brief A census name: the resource handle's debug name (a view's: its resource's), else the record's own. */
		std::string CensusName(const NativeResource& a_record, const void* a_resource, ID3D11View* a_view)
		{
			winrt::com_ptr<ID3D11Resource> viewed;
			if (a_view) {
				// A native view keeps its resource: GetResource answers it without a tripwire call.
				a_view->GetResource(viewed.put());
				a_resource = viewed.get();
			}
			auto name = a_resource ? Tripwire::Describe(a_resource).name : std::string();
			return name.empty() ? a_record.name : name;
		}

		/**
		 * @brief One immediate operation, for the censuses (OpCensus) and DescribeForCensus's written mark. a_resource is the resource
		 * handle, or a_view the view the call named. Call only when OpCensus::Flags() is non-zero.
		 */
		void NoteOp(OpCensus::Op a_op, NativeResource& a_record, const void* a_resource, ID3D11View* a_view = nullptr)
		{
			if (OpCensus::IsWrite(a_op))
				a_record.written.store(true, std::memory_order_relaxed);
			if (!(OpCensus::Flags() & (OpCensus::kOps | OpCensus::kWrites)))
				return;
			const std::string name = CensusName(a_record, a_resource, a_view);
			OpCensus::Record(a_op, { a_record.serial, name, a_record.bindFlags, static_cast<uint32_t>(a_record.d3dUsage), static_cast<uint32_t>(a_record.dimension),
									   a_record.initialData });
		}

		/**
		 * @brief An operation that did not run on the immediate context's own path: a static graph pass took it, or ORG's external
		 * client (org-native-frame-graph.md, Phase 3). Counted apart by the op census, and a write still marks its resource
		 * (DescribeForCensus).
		 */
		void NoteRouted(OpCensus::Op a_op, NativeResource& a_record, OpCensus::Route a_route)
		{
			if (!OpCensus::Flags())
				return;
			if (OpCensus::IsWrite(a_op))
				a_record.written.store(true, std::memory_order_relaxed);
			OpCensus::RecordRouted(a_op, a_route);
		}

		/**
		 * @brief An operation on resources no graph tracks (Immediate: Vulkan's commands in GENERAL, or BasicRHI's with Common
		 * transitions), outside every graph pass and the ledgers. Logged once per (operation, resource) while the render graph is
		 * active, up to 64 lines, so a run shows what still takes this path (Phase 3). The op census counts every one.
		 */
		void NoteRaw(OpCensus::Op a_op, const NativeResource& a_record)
		{
			if (!RenderGraphRuntime::Get().IsActive())
				return;
			static std::mutex lock;
			static std::unordered_set<std::uint64_t> logged;
			constexpr std::size_t kMaxLines = 64;
			std::lock_guard guard(lock);
			if (logged.size() > kMaxLines)
				return;
			if (!logged.insert(a_record.serial * static_cast<std::uint64_t>(OpCensus::Op::Count) + static_cast<std::uint64_t>(a_op)).second)
				return;
			if (logged.size() > kMaxLines) {
				logger::info("[ORG native] more operations take the immediate context's own path; only the first {} are logged", kMaxLines);
				return;
			}
			logger::info(
				"[ORG native] {} on \"{}\" runs outside the ledgers (no graph pass or graph resource orders it; logged once per "
				"operation and resource)",
				OpCensus::Name(a_op), a_record.name);
		}

		/**
		 * @brief Counts an operation the BasicRHI route ran (RunOp): through the ledgers when it named a tracked image (a_routed), else
		 * on resources no graph tracks.
		 */
		void NoteRun(OpCensus::Op a_op, NativeResource& a_record, const void* a_resource, ID3D11View* a_view, bool a_routed)
		{
			if (a_routed) {
				NoteRouted(a_op, a_record, OpCensus::Route::ExternalWork);
				return;
			}
			if (OpCensus::Flags())
				NoteOp(a_op, a_record, a_resource, a_view);
			NoteRaw(a_op, a_record);
		}

		/**
		 * @brief The graph resource whose ledger an operation on a_record follows through ORG's external client (ExternalWork): a
		 * tracked texture's PixelBuffer, or an exempt texture a write promoted once it is graph-managed (its home is the shader-read
		 * state). When the graph moves native images (NativeGraphMovesImages), also a host-made image's graph import (the anchors',
		 * a twin's export). Null for anything else: host-made images no graph imported, buffers, staging textures, and exempt textures
		 * that are not promoted or not graph-managed yet. The work records on this resource's own handle, the one its barriers name.
		 */
		std::shared_ptr<org::Resource> LedgerResourceOf(NativeResource& a_record)
		{
			if (a_record.kind != NativeResource::Kind::Image)
				return nullptr;
			if (a_record.ownership == NativeResource::Ownership::Tracked)
				return a_record.graphOwned;
			if (a_record.ownership == NativeResource::Ownership::Host) {
				auto graph = ::RenderGraph::GraphResourceOfImage(reinterpret_cast<std::uint64_t>(a_record.image.image));
				return graph && graph->IsRenderGraphManaged() ? graph : nullptr;
			}
			if (a_record.ownership != NativeResource::Ownership::Exempt || !a_record.promoted.load(std::memory_order_acquire))
				return nullptr;
			std::lock_guard guard(a_record.graphLock);
			if (a_record.exemptResource && a_record.exemptResource->IsRenderGraphManaged())
				return a_record.exemptResource;
			return nullptr;
		}

		/**
		 * @brief The context is about to write a_record. An exempt texture is promoted to tracked, once: the context keeps moving it
		 * out of the shader-read state and back around its own work (Immediate::LeaveShaderRead), which orders the write with the
		 * graph's work on the graphics queue, and the graph resource made for it (ImmutableTextureOf) becomes graph-managed through the
		 * build manager, so graphs built from then on track it. Never synchronously: the change waits for the next poll, with no build
		 * running. Logged once per texture.
		 */
		void NoteWrite(NativeResource& a_record)
		{
			if (a_record.ownership != NativeResource::Ownership::Exempt || a_record.promoted.exchange(true, std::memory_order_acq_rel))
				return;
			std::shared_ptr<org::ExternalTextureResource> resource;
			{
				std::lock_guard guard(a_record.graphLock);
				resource = a_record.exemptResource;
			}
			logger::info("[ORG native] {}: an exempt texture written after creation; promoted to tracked{}", a_record.name,
				resource ? ", graphs built again" : " (no graph read it yet)");
			if (!resource)
				return;
			::RenderGraph::Builds::Change([resource] { resource->SetGraphOwnership(org::Resource::GraphOwnership::GraphManaged); },
				fmt::format("native texture '{}' written after creation", a_record.name));
		}

		/** @brief Map's census kind for a D3D11 map type. */
		OpCensus::Op MapOp(D3D11_MAP a_type)
		{
			switch (a_type) {
			case D3D11_MAP_READ:
				return OpCensus::Op::MapRead;
			case D3D11_MAP_READ_WRITE:
				return OpCensus::Op::MapReadWrite;
			case D3D11_MAP_WRITE_DISCARD:
				return OpCensus::Op::MapWriteDiscard;
			case D3D11_MAP_WRITE_NO_OVERWRITE:
				return OpCensus::Op::MapWriteNoOverwrite;
			default:
				return OpCensus::Op::MapWrite;
			}
		}

		/** @brief An operation on something the native device did not make: logged once per operation. */
		void Unknown(std::atomic_flag& a_logged, const char* a_operation, const void* a_object)
		{
			if (!a_logged.test_and_set())
				logger::warn("[ORG native] {} on \"{}\", which native mode did not make; nothing happens (logged once)", a_operation,
					a_object ? Tripwire::Describe(a_object).name : std::string("null"));
		}

		/** @brief The whole extent of a subresource's mip. */
		VkExtent3D MipExtent(const Shape& a_shape, uint32_t a_mip)
		{
			return { a_shape.Width(a_mip), a_shape.Height(a_mip), a_shape.Depth(a_mip) };
		}

		// ---- The context's work as BasicRHI commands -------------------------------------------------------------------------------

		/** @brief One resource an operation uses: the subresources, and the layout, access and stages its command needs. */
		struct Use
		{
			NativeResource* record = nullptr;
			rhi::TextureSubresourceRange range{};
			rhi::ResourceLayout layout = rhi::ResourceLayout::Common;
			rhi::ResourceAccessType access = rhi::ResourceAccessType::Common;
			rhi::ResourceSyncState sync = rhi::ResourceSyncState::All;
		};

		/** @brief A depth-stencil image's two planes (D3D12 transitions each plane), else one. */
		uint32_t PlanesOf(const NativeResource& a_record)
		{
			return (a_record.shape.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? 2u : 1u;
		}

		/** @brief Every subresource of an image record. */
		rhi::TextureSubresourceRange AllOf(const NativeResource& a_record)
		{
			return { 0, a_record.shape.mips, 0, a_record.shape.layers, 0, PlanesOf(a_record) };
		}

		/** @brief One subresource (D3D11's index: mip + layer * mips) of an image record. */
		rhi::TextureSubresourceRange SubresourceOf(const NativeResource& a_record, uint32_t a_sub)
		{
			return { a_sub % a_record.shape.mips, 1, a_sub / a_record.shape.mips, 1, 0, PlanesOf(a_record) };
		}

		/**
		 * @brief Moves each resource from Common into its use (a_leave false), or back into Common (a_leave true). Entering waits for
		 * all earlier work on it and leaving makes the operation's writes visible to all later work, so these barriers order the
		 * operation with the graph's epochs as the full barriers of the Vulkan path do.
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

		/** @brief A source and a destination of a copy. */
		Use CopySource(NativeResource& a_record, const rhi::TextureSubresourceRange& a_range)
		{
			return { &a_record, a_range, rhi::ResourceLayout::CopySource, rhi::ResourceAccessType::CopySource, rhi::ResourceSyncState::Copy };
		}

		Use CopyDest(NativeResource& a_record, const rhi::TextureSubresourceRange& a_range)
		{
			return { &a_record, a_range, rhi::ResourceLayout::CopyDest, rhi::ResourceAccessType::CopyDest, rhi::ResourceSyncState::Copy };
		}

		/** @brief An image's range for ORG's external client: every subresource as All, else the mips and slices it names. */
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

		/** @brief An operation's commands: the list, the slot's descriptors, and the resource each use names, in the uses' order. */
		using OpRecord = std::function<void(rhi::CommandList&, const Immediate::Descriptors&, std::span<const rhi::Resource>)>;

		/**
		 * @brief Runs one of the context's operations with BasicRHI commands, on the one route there is for it.
		 * - Once the render graph is up: through ORG's external client (Immediate::Route). Each use whose image the graph tracks
		 *   (LedgerResourceOf) is declared there in the state its commands need, and its commands name that graph resource's own
		 *   handle: the ledger moves it from wherever it is, whatever the home states are. Every other use rests in Common (an exempt
		 *   texture in the shader-read state), and moves into its state and back around the commands.
		 * - Before that (textures made at startup): on the context's own list (Immediate::RunRhi), each use moving from Common and back.
		 * a_uses names a resource once per range (a copy within one image names two). a_record records only the commands. Returns the
		 * work's value, 0 when nothing was submitted. a_routed, when given, says whether a tracked image went through the ledgers.
		 */
		uint64_t RunOp(const std::string& a_label, std::span<const Use> a_uses, const OpRecord& a_record, std::vector<std::shared_ptr<const void>> a_keep,
			bool* a_routed = nullptr)
		{
			const bool graph = RenderGraphRuntime::Get().IsActive();
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
				auto ledger = graph ? LedgerResourceOf(*use.record) : nullptr;
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
			if (graph)
				return g_device->immediate.Route(a_label, accesses, untracked, body, std::move(a_keep), used);
			return g_device->immediate.RunRhi(a_label, body, std::move(a_keep), used);
		}

		/** @brief A footprint's extent: block-compressed widths and heights in whole blocks, as a placed footprint takes them. */
		uint32_t FootprintExtent(const Shape& a_shape, uint32_t a_texels)
		{
			return a_shape.blocks ? (a_texels + 3u) & ~3u : a_texels;
		}

		/**
		 * @brief The destination and source regions of one subresource copy between two images, on the handles given (a record's own
		 * import, or its graph resource's). a_box null is the whole source subresource. The subresources must exist.
		 */
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

		/**
		 * @brief Whether a depth-stencil image's depth plane holds floats in a buffer copy: D32S8X24, and the D24 family where BasicRHI
		 * stores it as D32S8 (Vulkan devices without D24S8 attachments). Else 24-bit UNORM.
		 */
		bool DepthPlaneFloat(const NativeResource& a_image)
		{
			if (a_image.shape.blockBytes == 8)
				return true;
			return !OnD3D12() && a_image.image.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
		}

		/** @brief A buffer barrier on a bounce buffer, between two of its copy states (Common at the start). */
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

		/** @brief A region of a staging subresource: its rows, the bytes of each, and where each row starts in the staging buffer. */
		struct StagingRows
		{
			uint32_t rows = 0, rowBytes = 0, depth = 1;
			uint64_t start = 0;  // the region's first byte
			uint32_t rowPitch = 0, depthPitch = 0;
			uint64_t At(uint32_t a_z, uint32_t a_row) const { return start + uint64_t(a_z) * depthPitch + uint64_t(a_row) * rowPitch; }
		};

		/** @brief The region of staging subresource a_sub at (a_x, a_y, a_z), a_width x a_height x a_depth texels. */
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

		/** @brief The bounce buffer's row pitch for a region of a_rows (StagingRows): 256-byte rows, as every backend's footprints take. */
		uint32_t BouncePitch(const StagingRows& a_rows)
		{
			return (a_rows.rowBytes + 255u) & ~255u;
		}

		/**
		 * @brief One subresource region from a_source to a_destination with BasicRHI commands, whatever each is (a buffer, an image, a
		 * staging texture), on the handles given, with each already in its copy state (RunOp). a_box null is the whole subresource
		 * (the whole buffer).
		 * - A placed footprint starts at an aligned offset, so a staging region that does not start a subresource goes through
		 *   a_bounce (a buffer CopyRhi made for it, resting in Common): the image's region and the bounce at offset 0, the rows between
		 *   the bounce and the staging texture.
		 * - A depth image copies its depth plane; a depth-stencil image copies both planes, to or from the staging texture's plane areas
		 *   (CopyRhi repacks them with the CPU's texels).
		 * False when the pair cannot be copied.
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
			// The region copied, in texels: the box, or the whole source subresource.
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
					// D3D11 copies a depth subresource whole. Depth alone lands on the CPU's texels (same size); with stencil, each plane
					// in its own area.
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
				// Into the middle of a staging subresource: the region into the bounce buffer, then row by row into place.
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
				// From the middle of a staging subresource: row by row into the bounce buffer, then the region from it.
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
			// Staging to staging: row by row (the whole subresource at once when the layouts match). Texel sizes must match.
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

		/**
		 * @brief Moves a depth-stencil staging texture's texels between the CPU's form and its plane areas: a_unpack makes the CPU's
		 * texels from the planes, else the planes from the texels, in a_float's depth form. Every subresource, or a_sub alone. The
		 * caller waited for the GPU's last use of the memory.
		 */
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

		/** @brief Before the CPU reads or writes a depth-stencil staging texture's texels: makes them from the planes a copy wrote. */
		void UnpackStagingIfNewer(NativeResource& a_staging)
		{
			if (a_staging.planesNewer.exchange(false, std::memory_order_acq_rel))
				RepackStaging(a_staging, true, a_staging.planeFloat);
		}

		/**
		 * @brief A copy through RunOp: subresource a_sourceSub of a_source into a_destinationSub of a_destination at (a_x, a_y, a_z), or
		 * with a_all every subresource the two share (a buffer: the whole of the smaller). The source is read and the destination
		 * written: buffers and staging memory whole, images in the subresources copied. A staging region inside a subresource goes
		 * through a bounce buffer, and a depth-stencil staging texture's planes are made from the CPU's texels first, or marked newer
		 * after. a_recorded is false when the pair is not a copy native mode makes. Returns the work's value (0: nothing submitted).
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
			// A staging region that does not start its subresource: a bounce buffer for the footprint.
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
			// Depth-stencil staging memory: the CPU's texels into the planes the copy reads, in the image's depth form; the planes are
			// newer than the texels after a copy writes them.
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
				// Staging to staging copies the CPU's texels: the destination's are made from its planes first, so a region keeps the rest.
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

		/** @brief The use of an image a clear writes: as a render target, or as depth (both planes of a depth-stencil image). */
		Use ClearUse(NativeResource& a_record, const rhi::TextureSubresourceRange& a_range, bool a_depth)
		{
			const rhi::TextureSubresourceRange range{ a_range.baseMip, a_range.mipCount, a_range.baseLayer, a_range.layerCount, 0, PlanesOf(a_record) };
			if (a_depth)
				return { &a_record, range, rhi::ResourceLayout::DepthReadWrite, rhi::ResourceAccessType::DepthReadWrite, rhi::ResourceSyncState::DepthStencil };
			return { &a_record, range, rhi::ResourceLayout::RenderTarget, rhi::ResourceAccessType::RenderTarget, rhi::ResourceSyncState::RenderTarget };
		}

		/** @brief The use of a resource a UAV clear writes. */
		Use UnorderedUse(NativeResource& a_record, const rhi::TextureSubresourceRange& a_range)
		{
			return { &a_record, a_range, rhi::ResourceLayout::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceSyncState::ClearUnorderedAccessView };
		}

		/**
		 * @brief Clears image subresources through a render target view (colour) or a depth-stencil view, on a_handle, with the image
		 * already in its clear state (ClearUse, through RunOp).
		 */
		void ClearImageRhi(rhi::CommandList& a_list, const Immediate::Descriptors& a_descriptors, const NativeResource& a_record, rhi::ResourceHandle a_handle,
			const rhi::TextureSubresourceRange& a_range, const float a_color[4], bool a_depth, bool a_clearDepth, bool a_clearStencil, float a_depthValue,
			uint8_t a_stencil)
		{
			const auto& shape = a_record.shape;
			const bool layered = shape.layers > 1;
			// One mip at a time: a view names one mip.
			for (uint32_t mip = a_range.baseMip; mip < a_range.baseMip + a_range.mipCount; ++mip) {
				const rhi::TextureSubresourceRange range{ mip, 1, a_range.baseLayer, a_range.layerCount, 0, PlanesOf(a_record) };
				if (a_depth) {
					rhi::DsvDesc desc{};
					desc.dimension = layered ? rhi::DsvDim::Texture2DArray : rhi::DsvDim::Texture2D;
					desc.range = range;
					desc.formatOverride = shape.viewFormat;
					// Each mip's view overwrites the slot's DSV: D3D12 and Vulkan both read it when the clear is recorded.
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

		/**
		 * @brief Clears image subresources or a buffer range through an unordered access view, with float or uint values, on
		 * a_resource, already in the unordered-access state (UnorderedUse, through RunOp). One view per mip: a slot holds
		 * kUavsPerSlot, more than D3D11's most mips.
		 */
		void ClearUnorderedRhi(rhi::CommandList& a_list, const Immediate::Descriptors& a_descriptors, const NativeResource& a_record, const rhi::Resource& a_resource,
			const rhi::TextureSubresourceRange& a_range, uint64_t a_offset, uint64_t a_size, const uint32_t a_uints[4], const float a_floats[4])
		{
			static_assert(Immediate::kUavsPerSlot >= D3D11_REQ_MIP_LEVELS, "a slot holds a UAV per mip of any D3D11 texture");
			const bool image = a_record.kind == NativeResource::Kind::Image;
			const auto handle = a_resource.GetHandle();
			// Each clear its own pair of descriptors (the same index in the shader-visible and the CPU heap): D3D12 reads a UAV clear's
			// descriptor when the list executes, so a later clear of this operation must not overwrite it.
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
					// The union's first member is the buffer's: the texture members' other fields are not zeroed by desc{}.
					desc.texture2DArray.planeSlice = 0;
				} else {
					desc.dimension = rhi::UavDim::Texture2D;
					desc.texture2D.mipSlice = mip;
					desc.texture2D.planeSlice = 0;
				}
				clear(desc);
			}
		}

		/** @brief A Vulkan subresource range (ViewRange's) as BasicRHI's, with both planes of a depth-stencil range. */
		rhi::TextureSubresourceRange ToRhiRange(const VkImageSubresourceRange& a_range)
		{
			const uint32_t planes = (a_range.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && (a_range.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ? 2u : 1u;
			return { a_range.baseMipLevel, a_range.levelCount, a_range.baseArrayLayer, a_range.layerCount, 0, planes };
		}

		/** @brief The memory Map hands out for a record: its own (a buffer, a staging texture), or a DYNAMIC image's upload side. */
		NativeResource* CpuSide(NativeResource& a_record)
		{
			NativeResource* cpu = a_record.kind == NativeResource::Kind::Image ? a_record.upload.get() : &a_record;
			return cpu && cpu->hostVisible ? cpu : nullptr;
		}

		/** @brief The waits WaitGraphReaders made, for its log lines. */
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
		 * @brief Before the CPU writes or reads a buffer the graph imported (graphShared): waits for the graph's work up to the last frame
		 * boundary (GraphHost::SealedPoint), on the graph's own timelines. A buffer the graph never imported needs nothing: the DYNAMIC
		 * constant buffers the engine maps each frame have no GPU reader in native mode (draws take their constants from captures).
		 *
		 * The current frame's executions are not waited for. A mid-frame wait names only sealed work: later work of the frame may be
		 * queued behind this thread. A buffer mapped once a frame before its readers run is covered: its readers of earlier frames are
		 * sealed, and this frame's run after the write.
		 * @return S_OK; DXGI_ERROR_WAS_STILL_DRAWING when a_doNotWait and the work is not done; DXGI_ERROR_DEVICE_REMOVED on a failed wait.
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
			// The first few, then a running total now and then: enough to see what waits and what it costs.
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
			if (OpCensus::Flags())
				NoteOp(MapOp(a_type), *record, a_resource);
			// The GPU may still use the memory: a read waits for its writes, a write for its reads. NO_OVERWRITE promises not to touch
			// what is in use. A DYNAMIC buffer is not renamed. The context's own copies are waited for by their value; a buffer the
			// graph imported waits for the graph's work too (WaitGraphReaders). Any other buffer has no GPU reader.
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
			// A depth-stencil staging texture a copy wrote: its texels are made from the planes now (the copy is done: waited above,
			// or a read always waits).
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
			// Readback memory flushes nothing back; upload memory flushes what the CPU wrote.
			cpu->resource->Unmap(0, cpu->readback ? 0 : cpu->size);
			// A DYNAMIC texture: the subresource just written goes to the image now, in the context's order.
			if (record->kind == NativeResource::Kind::Image && record->upload && a_subresource < record->shape.Subresources()) {
				const auto image = record;
				const auto upload = record->upload;
				bool recorded = false, routed = false;
				CopyRhi("unmap (dynamic upload) " + image->name, image, upload, false, a_subresource, 0, 0, 0, a_subresource, nullptr, recorded, &routed);
				NoteRun(OpCensus::Op::UnmapUpload, *image, a_resource, nullptr, routed);
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
			const auto op = record->kind == NativeResource::Kind::Buffer ? OpCensus::Op::UpdateBuffer : OpCensus::Op::UpdateTexture;
			// A texture's update is counted where it runs (NoteRun).
			if (OpCensus::Flags() && record->kind != NativeResource::Kind::Image)
				NoteOp(op, *record, a_resource);
			if (record->kind == NativeResource::Kind::Buffer) {
				const uint64_t from = a_box ? a_box->left : 0;
				const uint64_t size = a_box ? a_box->right - a_box->left : record->size;
				if (!size || from + size > record->size)
					return;
				// Written by the CPU when the GPU is done with the buffer (the graph's readers waited for, as Map does), else through an
				// upload buffer in the context's order.
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
				NoteRaw(OpCensus::Op::UpdateBuffer, *record);
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
			// A depth-stencil texture: D3D11's interleaved texels split into the two planes (the whole subresource: D3D11 takes no box
			// for depth-stencil), each copied into its plane, in the depth form the image keeps.
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
				// Packing only reads the D3D11 texels.
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
				NoteRun(op, *record, a_resource, nullptr, routed);
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
			// The upload's copy, through the ledgers when the graph tracks the texture (a depth texture: its depth plane, whose texels
			// are D3D11's).
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
			NoteRun(op, *record, a_resource, nullptr, routed);
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
			// A subresource onto itself: D3D11's runtime drops the call, and Vulkan's copies must not overlap.
			if (destination == source && a_destinationSub == a_sourceSub)
				return;
			NoteWrite(*destination);
			// Through the ledgers for each side the graph tracks.
			bool recorded = false, routed = false;
			CopyRhi("copy region " + source->name + " to " + destination->name, destination, source, false, a_destinationSub, a_x, a_y, a_z, a_sourceSub, a_box, recorded,
				&routed);
			NoteRun(OpCensus::Op::CopyRegion, *destination, a_destination, nullptr, routed);
			NoteRun(OpCensus::Op::CopyRegionSource, *source, a_source, nullptr, routed);
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
			// A resource onto itself: D3D11's runtime drops the call.
			if (destination == source)
				return;
			NoteWrite(*destination);
			// The engine skeleton's static copy pass (kMAIN into its copy texture, the main depth into kPOST_ZPREPASS_COPY): on the graph
			// now, at its own anchor.
			if (Skeleton::Sites::TakeContextCopy(a_destination, a_source)) {
				NoteRouted(OpCensus::Op::Copy, *destination, OpCensus::Route::GraphPass);
				NoteRouted(OpCensus::Op::CopySource, *source, OpCensus::Route::GraphPass);
				return;
			}
			// Every subresource, through the ledgers for each side the graph tracks.
			bool recorded = false, routed = false;
			CopyRhi("copy " + source->name + " to " + destination->name, destination, source, true, 0, 0, 0, 0, 0, nullptr, recorded, &routed);
			NoteRun(OpCensus::Op::Copy, *destination, a_destination, nullptr, routed);
			NoteRun(OpCensus::Op::CopySource, *source, a_source, nullptr, routed);
			if (!recorded) {
				static std::atomic_flag logged;
				if (!logged.test_and_set())
					logger::warn("[ORG native] CopyResource from \"{}\" to \"{}\" is not a copy native mode makes (logged once)", source->name, destination->name);
			}
		}

		/** @brief The UNORM twin of an sRGB format GenerateMips' scratch takes (its UAVs cannot be sRGB), else the format itself. */
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

		/** @brief A barrier on GenerateMips' scratch texture (every subresource) between two of its states. */
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
		 * @brief GenerateMips with BasicRHI commands. ORG's GenerateMips.hlsl builds the chain in a scratch texture with unordered
		 * access (BasicRHI has no blit, and the engine's textures have no UAVs): mip 0 is copied in, each mip is the area-weighted
		 * average of the one above it (an sRGB texture averaged in linear space, through the UNORM scratch), and mips 1 to N-1 are
		 * copied back. Through the ledgers when the graph tracks the image (mip 0 read, the rest written). False, logged once a
		 * reason, when native mode cannot build this texture's mips.
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
			if (!program || !program->compute) {
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
				// A UAV of each scratch mip, all slices, in the slot's shader-visible heap (bound): the shader reads them by index.
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
				a_list.BindLayout(program->compute->layout->GetHandle());
				a_list.BindPipeline(program->compute->Pipeline());
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
			// A compute mip chain.
			if (shape.mips < 2 || shape.aspects != VK_IMAGE_ASPECT_COLOR_BIT)
				return;
			bool routed = false;
			if (GenerateMipsRhi(record, routed))
				NoteRun(OpCensus::Op::GenerateMips, *record, nullptr, a_view, routed);
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

		void ContextClearRenderTargetView(ID3D11RenderTargetView* a_view, const FLOAT a_color[4])
		{
			const auto record = Resolve(a_view);
			if (!record || record->kind != NativeResource::Kind::Image || !a_color) {
				static std::atomic_flag logged;
				Unknown(logged, "ClearRenderTargetView", a_view);
				return;
			}
			// The engine skeleton's static clear pass for the view (the TAA mask, the water targets and the skeleton's own views): on the
			// graph now, opened by its run gate.
			if (Skeleton::Sites::TakeContextColourClear(a_view, a_color)) {
				NoteRouted(OpCensus::Op::ClearRenderTarget, *record, OpCensus::Route::GraphPass);
				return;
			}
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			// Through the ledgers when the graph tracks the image.
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
			NoteRun(OpCensus::Op::ClearRenderTarget, *record, nullptr, a_view, routed);
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

		/** @brief A UAV clear of the view's subresources or buffer range, with a_value's four words (a_float: as floats). */
		void ClearUnorderedAccess(ID3D11UnorderedAccessView* a_view, const VkClearColorValue& a_value, const char* a_operation, bool a_float)
		{
			const auto record = Resolve(a_view);
			if (!record) {
				static std::atomic_flag logged;
				Unknown(logged, a_operation, a_view);
				return;
			}
			const auto op = a_float ? OpCensus::Op::ClearUavFloat : OpCensus::Op::ClearUavUint;
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			// Through the ledgers when the graph tracks the image.
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
			NoteRun(op, *record, nullptr, a_view, routed);
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
			// The engine skeleton's static clear pass for the view: on the graph now.
			if (Skeleton::Sites::TakeContextDepthClear(a_view, a_flags, a_depth, a_stencil)) {
				NoteRouted(OpCensus::Op::ClearDepthStencil, *record, OpCensus::Route::GraphPass);
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
			// Through the ledgers when the graph tracks the image.
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
			NoteRun(OpCensus::Op::ClearDepthStencil, *record, nullptr, a_view, routed);
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

	void OnGraphReady()
	{
		if (!g_device)
			return;
		g_device->OnGraphReady();
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
		/** @brief The image record behind a_object: a streamed texture's shell gives its window's, as importers read the shell. */
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

		/** @brief The tracked record's ORG texture, else null. */
		std::shared_ptr<org::GloballyIndexedResource> TrackedOf(const std::shared_ptr<NativeResource>& a_record)
		{
			return a_record && a_record->ownership == NativeResource::Ownership::Tracked ? a_record->graphOwned : nullptr;
		}
	}

	std::shared_ptr<org::GloballyIndexedResource> GraphTextureOf(const IUnknown* a_object)
	{
		return g_device ? TrackedOf(ImageRecordOf(a_object)) : nullptr;
	}

	std::shared_ptr<org::GloballyIndexedResource> GraphTextureOfImage(std::uint64_t a_image)
	{
		if (!g_device || !a_image)
			return nullptr;
		std::shared_ptr<NativeResource> record;
		{
			auto& registry = GetRegistry();
			std::lock_guard guard(registry.lock);
			if (const auto found = registry.byImage.find(a_image); found != registry.byImage.end())
				record = found->second->weak_from_this().lock();
		}
		return TrackedOf(record);
	}

	std::shared_ptr<org::ExternalTextureResource> ImmutableTextureOf(const IUnknown* a_texture, DXGI_FORMAT a_srvFormat)
	{
		const auto record = g_device ? ImageRecordOf(a_texture) : nullptr;
		if (!record || record->ownership != NativeResource::Ownership::Exempt)
			return nullptr;
		std::lock_guard guard(record->graphLock);
		if (record->exemptResource)
			return record->exemptResource;
		// Readiness: no graph reads it before its transfer is done.
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
			// A texture a write promoted already is tracked from the start (home: the shader-read state it rests in).
			if (!record->promoted.load(std::memory_order_acquire))
				resource->SetGraphOwnership(org::Resource::GraphOwnership::ExternalImmutableShaderResource);
			record->exemptResource = std::move(resource);
		} catch (const std::exception& e) {
			logger::warn("[ORG native] {}: could not be made an exempt graph texture: {}", record->name, e.what());
			return nullptr;
		}
		::RenderGraph::NoteImageResource(reinterpret_cast<std::uint64_t>(record->image.image), record->exemptResource, id);
		// The image's one graph resource: a read-only import of the texture (FrameAnchors) returns it rather than importing it again.
		::RenderGraph::RegisterMadeImage(reinterpret_cast<std::uint64_t>(record->image.image), record->exemptResource, desc, id);
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

	bool DescribeForCensus(const void* a_object, ResourceFacts& a_out)
	{
		auto record = a_object ? Resolve(a_object) : nullptr;
		if (record && record->kind == NativeResource::Kind::Shell) {
			// A streamed texture's shell has no storage: shaders read its window, so the window is what is described.
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
		// Before the first context call: the census switches (CS_ORG_NATIVE_OP_CENSUS, CS_ORG_NATIVE_WRITE_CENSUS).
		OpCensus::Configure();
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
		if (!state->commands.Init(state->Rhi()))
			Fail("the native device", "the command lists or timeline for commands outside the graph could not be made");
		if (!state->immediate.Init(state->Rhi()))
			Fail("the native device", "the context's command lists, timeline or descriptor heaps could not be made");

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
		// SDR and the frame latency, as the self-test configures its swap chain. HDR Display switches the colour space later
		// (SetSwapChainColorSpace).
		(void)ConfigureSwapchain(state->swapchain.Get(), { .hdr = false, .maximumFrameLatency = state->frameLatency });
		if (!state->MakePresentObjects())
			Fail("the native swap chain", "the present's command lists or timeline could not be made");
		g_device = state;
		if (!state->MakeBackBuffer())
			Fail("the native back buffer", "BasicRHI could not make it (see the [ORG native] line above)");
		const auto caps = state->swapchain->Caps();
		Stage("swap chain", fmt::format("{} {}x{} DXGI format {}, {} images, tearing {}, HDR10 {}; the game asked for {} ({})", ::RenderGraph::Backend::Name(::RenderGraph::Backend::Active()), state->width, state->height,
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
		// A streamed texture's shell is read through its window, as DXVK's binds read it; without one there is nothing to describe.
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
			// The graph imports it from here, so a CPU access waits for the graph's work from now on (WaitGraphReaders).
			record->graphShared.store(true, std::memory_order_release);
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

	bool EmitCommands(void (*a_record)(void*, rhi::CommandList&), void* a_user, const rhi::TimelinePoint* a_waits, std::uint32_t a_waitCount,
		const rhi::TimelinePoint* a_signals, std::uint32_t a_signalCount, const char* a_label)
	{
		return g_device && g_device->commands.Run(a_record, a_user, { a_waits, a_waitCount }, { a_signals, a_signalCount }, a_label);
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
		// The censuses and the undeclared-access validator read the wrap's description, as they read a texture made here.
		NoteRequest(*record, request);
		static std::atomic<bool> loggedDescribe = false, loggedInitialize = false;
		rhi::FormatSupport support{};
		(void)g_device->Rhi().QueryFormatSupport(format, support);
		if (!NativeState::FillImage(*record, a_image, request, dxgiFormat, format, support, request.mipLevels, layers, depth, VK_IMAGE_LAYOUT_GENERAL)) {
			LogOnce(loggedDescribe, request.name, "BasicRHI did not describe the image to wrap");
			return nullptr;
		}
		// Into GENERAL with its contents, or zeroed as DXVK's wrap zeroes it.
		if (!g_device->Initialize(a_image.GetHandle(), request, format, request.mipLevels, layers, depth)) {
			LogOnce(loggedInitialize, request.name, "the image to wrap could not be moved into GENERAL or given its contents");
			return nullptr;
		}
		// A block-compressed image cannot be cleared (DynamicCubemaps' BC6H cubemaps): those are left as made, which their owners
		// write before reading.
		if (!a_data && !record->shape.blocks) {
			// Zeroed through whatever view its bind flags allow; an image with none of them is left as made, as above.
			const bool renderTarget = (request.bindFlags & D3D11_BIND_RENDER_TARGET) != 0;
			const bool depthStencil = (request.bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
			const bool unordered = (request.bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
			if (renderTarget || depthStencil || unordered) {
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
		auto record = MakeWrapRecord(a_name);
		record->kind = NativeResource::Kind::Buffer;
		if (!NativeState::DescribeBuffer(*record, a_buffer)) {
			static std::atomic<bool> logged = false;
			LogOnce(logged, a_name ? a_name : "a buffer", "BasicRHI did not describe the buffer to wrap");
			return nullptr;
		}
		record->size = a_desc.ByteWidth;
		record->bufferDesc = a_desc;
		// The censuses' description. Its owner fills it through a mapping, which no census sees, so it counts as made without data.
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
		// {7D1E2C44-9B3A-4F61-8E25-3C6A0B9D4F12}: the private-data slot of a shell SRV's gone tag.
		constexpr GUID kStreamGoneGuid = { 0x7d1e2c44, 0x9b3a, 0x4f61, { 0x8e, 0x25, 0x3c, 0x6a, 0x0b, 0x9d, 0x4f, 0x12 } };

		/** @brief Held by a shell SRV (its private data): released with the view, it runs the streamer's gone callback then. */
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
		// The view through the native device (D3D11's default view of the texture); it shares the texture's record.
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
				return E_FAIL;  // a shell already, as DXVK refuses a second enable
		}
		// The gone callback with the view's last reference (its private data goes with it), before anything could name it again.
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
		// Engine draws do not read shells in native mode (they are ported or skipped), so none counts as a native bind.
		return 0;
	}

	bool SetSwapChainColorSpace(bool a_hdr)
	{
		if (!g_device || !g_device->swapchain)
			return false;
		std::lock_guard guard(g_device->presentLock);
		const auto wanted = a_hdr ? rhi::SwapchainColorSpace::Hdr10St2084 : rhi::SwapchainColorSpace::SrgbNonlinear;
		if (g_device->swapchain->ColorSpace() == wanted)
			return true;
		// The swap chain's images are made again: the queued presents' copies into them finish first.
		g_device->WaitPresents();
		const bool configured = ConfigureSwapchain(g_device->swapchain.Get(), { .hdr = a_hdr, .maximumFrameLatency = g_device->frameLatency });
		const bool applied = g_device->swapchain->ColorSpace() == wanted;
		logger::info("[ORG native] swap chain colour space: {} asked, {} now ({} images, DXGI format {} back buffer){}", a_hdr ? "HDR10 (PQ, Rec. 2020)" : "SDR",
			g_device->swapchain->ColorSpace() == rhi::SwapchainColorSpace::Hdr10St2084 ? "HDR10" : "SDR", g_device->swapchain->ImageCount(),
			static_cast<int>(g_device->format), configured ? "" : "; the swap chain refused it (see the [ORG] line above)");
		return applied;
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
	void OnGraphReady() {}
	void OnShaderCompilerReady() {}
	HRESULT CreateDeviceAndSwapChain(IDXGIAdapter*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**)
	{
		Fail("this build", "it was built without OpenRenderGraph");
	}
	bool DescribeResource(const IUnknown*, DxvkOrgInteropResourceInfo&) { return false; }
	bool DescribeForCensus(const void*, ResourceFacts&) { return false; }
	bool OwnedResources() { return false; }
	std::shared_ptr<org::GloballyIndexedResource> GraphTextureOf(const IUnknown*) { return nullptr; }
	std::shared_ptr<org::GloballyIndexedResource> GraphTextureOfImage(std::uint64_t) { return nullptr; }
	std::shared_ptr<org::ExternalTextureResource> ImmutableTextureOf(const IUnknown*, DXGI_FORMAT) { return nullptr; }
	std::shared_ptr<org::ExternalTextureResource> PromotedTextureOf(const IUnknown*) { return nullptr; }
	bool IsTrackedTexture(const IUnknown*) { return false; }
	HRESULT Present(UINT, UINT) { return E_FAIL; }
	bool EmitCommands(void (*)(void*, rhi::CommandList&), void*, const rhi::TimelinePoint*, std::uint32_t, const rhi::TimelinePoint*, std::uint32_t, const char*)
	{
		return false;
	}
	winrt::com_ptr<ID3D11Resource> WrapTexture(const rhi::Resource&, const DxvkOrgInteropTextureWrap&, DxvkOrgInteropResourceInfo&, const char*, const D3D11_SUBRESOURCE_DATA*)
	{
		return nullptr;
	}
	winrt::com_ptr<ID3D11Buffer> WrapBuffer(const rhi::Resource&, const D3D11_BUFFER_DESC&, const char*) { return nullptr; }
	void Shutdown() {}
	bool SetSwapChainColorSpace(bool) { return false; }
	bool DescribeVulkanDevice(DxvkOrgInteropDeviceInfo&) { return false; }
	HRESULT __stdcall RunOnGraphicsQueue(ID3D11Device*, void (*)(void*, VkQueue_T*), void*) { return E_FAIL; }
	bool CreateStreamShell(const D3D11_TEXTURE2D_DESC&, ID3D11Texture2D**, ID3D11ShaderResourceView**) { return false; }
	HRESULT __stdcall StreamEnableView(ID3D11ShaderResourceView*, void(__stdcall*)(std::uint64_t), void(__stdcall*)(std::uint64_t),
		const D3D11_SUBRESOURCE_DATA*(__stdcall*)(std::uint64_t, BOOL), std::uint64_t)
	{
		return E_NOTIMPL;
	}
	void __stdcall StreamSetWindow(ID3D11ShaderResourceView*, ID3D11ShaderResourceView*) {}
	ID3D11ShaderResourceView* __stdcall StreamGetWindow(ID3D11ShaderResourceView*) { return nullptr; }
	std::uint64_t __stdcall StreamGetBinds(ID3D11ShaderResourceView*) { return 0; }
}

#endif
