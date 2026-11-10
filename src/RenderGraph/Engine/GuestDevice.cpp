#include "RenderGraph/Engine/GuestDevice.h"

#include <cstdlib>
#include <cstring>

namespace RenderGraph::Engine
{
	bool OwnedFrameRequested()
	{
		static const bool s_requested = [] {
			char buf[8] = {};
			return GetEnvironmentVariableA("CS_ORG_OWNED_FRAME", buf, sizeof(buf)) && buf[0] == '1';
		}();
		return s_requested;
	}

	GuestDevice& GuestDevice::Get()
	{
		static GuestDevice s_device;
		return s_device;
	}
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Aftermath.h"
#	include "Features/Upscaling/DXVKInteropInterfaces.h"
#	include "RenderGraph/DxvkOrgInterop.h"

#	include <algorithm>
#	include <vector>

#	include <fmt/ranges.h>

namespace RenderGraph::Engine
{
	namespace
	{
		// What DXVK takes from the descriptor-buffer model's shared budgets, which BasicRHI must leave room for: its 2048-sampler pool
		// (DxvkSamplerPool), one 8 MiB resource descriptor buffer (DxvkResourceDescriptorHeap::addRanges) and its sampler descriptor
		// buffer, at most 64 bytes a sampler. The same figures RenderGraphRuntime's negotiation uses for an adopted device.
		constexpr uint32_t kDxvkSamplerObjects = 2048;
		constexpr uint64_t kDxvkResourceDescriptorBytes = 8ull << 20;
		constexpr uint64_t kDxvkSamplerDescriptorBytes = uint64_t(kDxvkSamplerObjects) * 64;

		// The header every Vulkan feature struct starts with; only VkBool32 members follow it.
		constexpr size_t kFeatureHeaderBytes = sizeof(VkBaseOutStructure);

		VkBaseOutStructure* FindStruct(const void* a_chain, VkStructureType a_type)
		{
			for (auto* s = static_cast<VkBaseOutStructure*>(const_cast<void*>(a_chain)); s; s = s->pNext)
				if (s->sType == a_type)
					return s;
			return nullptr;
		}

		void OrFeatures(void* a_into, const void* a_from, size_t a_size)
		{
			auto* into = reinterpret_cast<VkBool32*>(static_cast<std::byte*>(a_into) + kFeatureHeaderBytes);
			const auto* from = reinterpret_cast<const VkBool32*>(static_cast<const std::byte*>(a_from) + kFeatureHeaderBytes);
			for (size_t i = 0; i < (a_size - kFeatureHeaderBytes) / sizeof(VkBool32); ++i)
				into[i] |= from[i];
		}

		// The feature structs the device was created with that dxvkGetInteropDeviceInfo reports, copied out of BasicRHI's create info
		// (which does not outlive CreateVulkanDevice) for dxvkSetGuestDevice.
		struct EnabledFeatures
		{
			VkPhysicalDeviceFeatures2 core{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
			VkPhysicalDeviceVulkan11Features vk11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
			VkPhysicalDeviceVulkan12Features vk12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
			VkPhysicalDeviceVulkan13Features vk13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
			VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeap{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT };
			VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT };
			VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableDescriptorType{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT };
			VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT deviceGeneratedCommands{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT };
			VkPhysicalDeviceRobustness2FeaturesEXT robustness2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
			VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pipelineExecutableProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR };

			// Copies the structs of a create info's chain; the core features come from pEnabledFeatures when the chain has no
			// VkPhysicalDeviceFeatures2.
			void Capture(const VkDeviceCreateInfo& a_info)
			{
				if (a_info.pEnabledFeatures)
					core.features = *a_info.pEnabledFeatures;
				auto take = [](auto& a_into, const VkBaseOutStructure* a_from) {
					if (a_from->sType == a_into.sType)
						std::memcpy(reinterpret_cast<std::byte*>(&a_into) + kFeatureHeaderBytes, reinterpret_cast<const std::byte*>(a_from) + kFeatureHeaderBytes,
							sizeof(a_into) - kFeatureHeaderBytes);
				};
				for (auto* s = static_cast<const VkBaseOutStructure*>(a_info.pNext); s; s = s->pNext) {
					take(core, s);
					take(vk11, s);
					take(vk12, s);
					take(vk13, s);
					take(descriptorHeap, s);
					take(descriptorBuffer, s);
					take(mutableDescriptorType, s);
					take(deviceGeneratedCommands, s);
					take(robustness2, s);
					take(pipelineExecutableProperties, s);
				}
				core.pNext = &vk11;
				vk11.pNext = &vk12;
				vk12.pNext = &vk13;
				vk13.pNext = &descriptorHeap;
				descriptorHeap.pNext = &descriptorBuffer;
				descriptorBuffer.pNext = &mutableDescriptorType;
				mutableDescriptorType.pNext = &deviceGeneratedCommands;
				deviceGeneratedCommands.pNext = &robustness2;
				robustness2.pNext = &pipelineExecutableProperties;
				pipelineExecutableProperties.pNext = nullptr;
			}
		};

		void AppendUnique(std::vector<const char*>& a_names, const char* a_name)
		{
			if (std::none_of(a_names.begin(), a_names.end(), [&](const char* n) { return std::strcmp(n, a_name) == 0; }))
				a_names.push_back(a_name);
		}

		// The loader BasicRHI's volk loaded. Vulkan entry points in CS's own translation units are resolved through it, not volk's
		// globals.
		PFN_vkGetInstanceProcAddr LoaderProc()
		{
			static const auto s_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(::GetProcAddress(::LoadLibraryW(L"vulkan-1.dll"), "vkGetInstanceProcAddr"));
			return s_proc;
		}

		// Set by the owned frame's present on DXVK's submission thread, which holds DXVK's queue lock already (dxvkEnqueueQueueCallback).
		thread_local bool t_queueHeld = false;
	}

	struct GuestDevice::State
	{
		PFN_dxvkGetGuestInstanceRequirements getInstanceRequirements = nullptr;
		PFN_dxvkGetGuestDeviceRequirements getDeviceRequirements = nullptr;
		PFN_dxvkSetGuestDevice setGuestDevice = nullptr;

		DxvkOrgGuestInstanceRequirements instanceRequirements{ DXVK_ORG_GUEST_VERSION };
		std::string engineName;

		// Storage the creation hooks point BasicRHI's create infos at, until CreateVulkanDevice returns.
		std::vector<const char*> instanceExtensions;
		std::vector<const char*> deviceExtensions;
		VkPhysicalDeviceFeatures coreFeatures{};
		std::vector<std::vector<std::byte>> appendedFeatures;
		std::vector<VkDeviceQueueCreateInfo> queues;
		std::vector<std::vector<float>> priorities;

		// What the device was created with, copied for dxvkSetGuestDevice.
		std::vector<std::string> instanceExtensionNames;
		std::vector<std::string> deviceExtensionNames;
		std::vector<std::pair<uint32_t, uint32_t>> queueCounts;
		EnabledFeatures enabledFeatures;
		std::string hookFailure;

		std::shared_ptr<rhi::DevicePtr> device;
		winrt::com_ptr<IDXGIVkInteropDevice> interop;
		PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
		VkDevice vkDevice = VK_NULL_HANDLE;

		static bool OnInstance(void* a_user, void* a_createInfo)
		{
			auto* self = static_cast<State*>(a_user);
			auto* info = static_cast<VkInstanceCreateInfo*>(a_createInfo);
			self->instanceExtensions.assign(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
			for (uint32_t i = 0; i < self->instanceRequirements.extensionCount; ++i)
				AppendUnique(self->instanceExtensions, self->instanceRequirements.extensions[i]);
			info->enabledExtensionCount = static_cast<uint32_t>(self->instanceExtensions.size());
			info->ppEnabledExtensionNames = self->instanceExtensions.data();
			self->instanceExtensionNames.assign(self->instanceExtensions.begin(), self->instanceExtensions.end());
			logger::info("[ORG] Owned frame: instance extensions {} (DXVK asks for {})", fmt::join(self->instanceExtensionNames, ", "),
				self->instanceRequirements.extensionCount);
			if (info->pApplicationInfo && info->pApplicationInfo->apiVersion < self->instanceRequirements.apiVersion)
				self->hookFailure = "the instance's Vulkan version is below DXVK's";
			return self->hookFailure.empty();
		}

		static bool OnDevice(void* a_user, void* a_instance, void* a_physicalDevice, void* a_createInfo)
		{
			auto* self = static_cast<State*>(a_user);
			auto* info = static_cast<VkDeviceCreateInfo*>(a_createInfo);

			// DXVK binds descriptors as BasicRHI chose to (DeviceCreateInfo::vulkanBindingModels asks for descriptor buffers).
			uint32_t model = 0;
			if (const auto* heap = FindStruct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT);
				heap && reinterpret_cast<const VkPhysicalDeviceDescriptorHeapFeaturesEXT*>(heap)->descriptorHeap)
				model = DXVK_ORG_BINDING_MODEL_DESCRIPTOR_HEAP;
			else if (const auto* buffer = FindStruct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT);
					 buffer && reinterpret_cast<const VkPhysicalDeviceDescriptorBufferFeaturesEXT*>(buffer)->descriptorBuffer)
				model = DXVK_ORG_BINDING_MODEL_DESCRIPTOR_BUFFER;

			std::vector<const char*> instanceExtensions;
			for (const auto& name : self->instanceExtensionNames)
				instanceExtensions.push_back(name.c_str());

			DxvkOrgGuestDeviceQuery query{ DXVK_ORG_GUEST_VERSION, LoaderProc(), static_cast<VkInstance>(a_instance),
				static_cast<uint32_t>(instanceExtensions.size()), instanceExtensions.data(), static_cast<VkPhysicalDevice>(a_physicalDevice), model };
			DxvkOrgGuestDeviceRequirements requirements{ DXVK_ORG_GUEST_VERSION };
			if (!model || FAILED(self->getDeviceRequirements(&query, &requirements))) {
				self->hookFailure = model ? "DXVK cannot run on BasicRHI's device (see DXVK's log)" : "BasicRHI chose no descriptor model DXVK shares";
				return false;
			}

			self->deviceExtensions.assign(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
			for (uint32_t i = 0; i < requirements.extensionCount; ++i)
				AppendUnique(self->deviceExtensions, requirements.extensions[i]);
			info->enabledExtensionCount = static_cast<uint32_t>(self->deviceExtensions.size());
			info->ppEnabledExtensionNames = self->deviceExtensions.data();

			// Core features: BasicRHI passes pEnabledFeatures; a VkPhysicalDeviceFeatures2 in the chain instead takes them there.
			auto* features2 = reinterpret_cast<VkPhysicalDeviceFeatures2*>(FindStruct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2));
			auto orCore = [&](VkPhysicalDeviceFeatures& a_into) {
				auto* into = reinterpret_cast<VkBool32*>(&a_into);
				const auto* from = reinterpret_cast<const VkBool32*>(requirements.coreFeatures);
				for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); ++i)
					into[i] |= from[i];
			};
			if (features2) {
				orCore(features2->features);
			} else {
				self->coreFeatures = info->pEnabledFeatures ? *info->pEnabledFeatures : VkPhysicalDeviceFeatures{};
				orCore(self->coreFeatures);
				info->pEnabledFeatures = &self->coreFeatures;
			}

			// Every other struct: ORed into BasicRHI's of the same sType, else a copy at the head of the chain.
			uint32_t merged = 0;
			for (uint32_t i = 0; i < requirements.featureCount; ++i) {
				const auto& feature = requirements.features[i];
				if (auto* existing = FindStruct(info->pNext, feature.feature->sType)) {
					OrFeatures(existing, feature.feature, feature.size);
					++merged;
					continue;
				}
				auto& copy = self->appendedFeatures.emplace_back(feature.size);
				std::memcpy(copy.data(), feature.feature, feature.size);
				auto* head = reinterpret_cast<VkBaseOutStructure*>(copy.data());
				head->pNext = static_cast<VkBaseOutStructure*>(const_cast<void*>(info->pNext));
				info->pNext = head;
			}

			// Queues: at least as many of each family as DXVK asks for (BasicRHI already creates every queue of its families).
			self->queues.assign(info->pQueueCreateInfos, info->pQueueCreateInfos + info->queueCreateInfoCount);
			for (uint32_t i = 0; i < requirements.queueCount; ++i) {
				const auto& wanted = requirements.queues[i];
				auto it = std::ranges::find(self->queues, wanted.queueFamilyIndex, &VkDeviceQueueCreateInfo::queueFamilyIndex);
				if (it == self->queues.end()) {
					VkDeviceQueueCreateInfo added{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
					added.queueFamilyIndex = wanted.queueFamilyIndex;
					self->queues.push_back(added);
					it = self->queues.end() - 1;
				}
				if (it->queueCount < wanted.queueCount) {
					it->queueCount = wanted.queueCount;
					it->pQueuePriorities = nullptr;
				}
			}
			for (auto& queue : self->queues) {
				if (!queue.pQueuePriorities) {
					queue.pQueuePriorities = self->priorities.emplace_back(queue.queueCount, 1.0f).data();
				}
				self->queueCounts.emplace_back(queue.queueFamilyIndex, queue.queueCount);
			}
			info->queueCreateInfoCount = static_cast<uint32_t>(self->queues.size());
			info->pQueueCreateInfos = self->queues.data();

			self->deviceExtensionNames.assign(self->deviceExtensions.begin(), self->deviceExtensions.end());
			self->enabledFeatures.Capture(*info);
			logger::info("[ORG] Owned frame: DXVK adds {} device extensions, {} feature structs ({} merged), {} queue families", requirements.extensionCount,
				requirements.featureCount, merged, requirements.queueCount);
			return true;
		}

		uint32_t QueueCount(uint32_t a_family) const
		{
			for (const auto& [family, count] : queueCounts)
				if (family == a_family)
					return count;
			return 0;
		}
	};

	bool GuestDevice::Prepare(void* a_dxvkD3D11)
	{
		if (attempted)
			return false;
		attempted = true;
		if (!OwnedFrameRequested())
			return false;

		auto fail = [&](std::string a_reason) {
			Abandon(std::move(a_reason));
			return false;
		};

		state = new State();
		const auto module = static_cast<HMODULE>(a_dxvkD3D11);
		state->getInstanceRequirements = module ? reinterpret_cast<PFN_dxvkGetGuestInstanceRequirements>(::GetProcAddress(module, "dxvkGetGuestInstanceRequirements")) : nullptr;
		state->getDeviceRequirements = module ? reinterpret_cast<PFN_dxvkGetGuestDeviceRequirements>(::GetProcAddress(module, "dxvkGetGuestDeviceRequirements")) : nullptr;
		state->setGuestDevice = module ? reinterpret_cast<PFN_dxvkSetGuestDevice>(::GetProcAddress(module, "dxvkSetGuestDevice")) : nullptr;
		if (!state->getInstanceRequirements || !state->getDeviceRequirements || !state->setGuestDevice)
			return fail("this DXVK build has no guest-device exports");

		// The loader BasicRHI's volk loads answers which of DXVK's instance extensions exist.
		if (FAILED(state->getInstanceRequirements(LoaderProc(), &state->instanceRequirements)))
			return fail("DXVK could not report its instance requirements");
		state->engineName = state->instanceRequirements.engineName ? state->instanceRequirements.engineName : "DXVK";

		rhi::DeviceCreateInfo::VulkanCreateHooks hooks{ state, &State::OnInstance, &State::OnDevice };
		rhi::DeviceCreateInfo ci{};
		ci.backend = rhi::Backend::Vulkan;
		ci.enableDebug = false;
		// The NVIDIA profile trick (extern/dxvk/FORK.md): the engine name DXVK would report, "DXVK_HDR" when HDR is configured.
		ci.vulkanEngineName = state->engineName.c_str();
		// Hang diagnostics survive the inversion: BasicRHI's native markers where DXVK's hang mode would be on.
		char debug[16] = {};
		const bool hangMode = GetEnvironmentVariableA("DXVK_DEBUG", debug, sizeof(debug)) && std::strcmp(debug, "hang") == 0;
		// CS_ORG_CRASH_MARKERS=0 turns them off (they write a marker at every command list boundary and label).
		char markers[8] = {};
		const bool markersOff = GetEnvironmentVariableA("CS_ORG_CRASH_MARKERS", markers, sizeof(markers)) && markers[0] == '0';
		ci.enableCrashMarkers = (hangMode || Aftermath::WantsCrashAnalysis()) && !markersOff;
		ci.vulkanCreateHooks = &hooks;
		ci.vulkanBindingModels = rhi::vulkan::BindingModel_DescriptorBuffer;
		ci.vulkanGuestSamplerObjects = kDxvkSamplerObjects;
		ci.vulkanGuestResourceDescriptorBufferBytes = kDxvkResourceDescriptorBytes;
		ci.vulkanGuestSamplerDescriptorBufferBytes = kDxvkSamplerDescriptorBytes;

		// Streamline's interposer stays off: frame generation is unavailable in the owned frame for now (P1.4).
		state->device = std::make_shared<rhi::DevicePtr>();
		// A hook that fails aborts the creation; its reason is the one to report.
		if (rhi::CreateVulkanDevice(ci, *state->device, false) != rhi::Result::Ok || !*state->device)
			return fail(state->hookFailure.empty() ? std::string("rhi::CreateVulkanDevice failed") : state->hookFailure);

		auto& device = *state->device;
		const VkDevice vkDevice = rhi::vulkan::get_device(device.Get());
		const VkInstance vkInstance = rhi::vulkan::get_instance(device.Get());
		state->vkDevice = vkDevice;
		state->getDeviceProcAddr = LoaderProc() ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(LoaderProc()(vkInstance, "vkGetDeviceProcAddr")) : nullptr;
		const auto getDeviceQueue = state->getDeviceProcAddr ? reinterpret_cast<PFN_vkGetDeviceQueue>(state->getDeviceProcAddr(vkDevice, "vkGetDeviceQueue")) : nullptr;
		if (!getDeviceQueue)
			return fail("the Vulkan loader's entry points could not be resolved");
		const auto graphics = device->GetQueue(rhi::QueueKind::Graphics);
		const auto compute = device->GetQueue(rhi::QueueKind::Compute);
		const auto copy = device->GetQueue(rhi::QueueKind::Copy);
		const uint32_t graphicsFamily = rhi::vulkan::get_queue_family_index(graphics);
		const uint32_t computeFamily = rhi::vulkan::get_queue_family_index(compute);
		const uint32_t copyFamily = rhi::vulkan::get_queue_family_index(copy);
		auto queueAt = [&](uint32_t a_family, uint32_t a_index) {
			VkQueue queue = VK_NULL_HANDLE;
			getDeviceQueue(vkDevice, a_family, a_index, &queue);
			return queue;
		};
		// BasicRHI's primary queues are each family's first.
		if (queueAt(graphicsFamily, 0) != rhi::vulkan::get_queue(graphics))
			return fail("BasicRHI's graphics queue is not its family's first");

		DxvkOrgGuestDeviceDesc desc{ DXVK_ORG_GUEST_VERSION };
		desc.device = vkDevice;
		std::vector<const char*> extensions;
		for (const auto& name : state->deviceExtensionNames)
			extensions.push_back(name.c_str());
		desc.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
		desc.enabledExtensions = extensions.data();
		// DXVK submits everything to the graphics queue. The render graph keeps compute and copy; the copy family's second queue,
		// which BasicRHI's CreateQueue hands ORG's uploader, is the upload queue.
		desc.graphics = { rhi::vulkan::get_queue(graphics), graphicsFamily, 0 };
		if (computeFamily != graphicsFamily)
			desc.compute = { rhi::vulkan::get_queue(compute), computeFamily, 0 };
		if (copyFamily != graphicsFamily && copyFamily != computeFamily) {
			desc.copy = { rhi::vulkan::get_queue(copy), copyFamily, 0 };
			if (state->QueueCount(copyFamily) > 1)
				desc.upload = { queueAt(copyFamily, 1), copyFamily, 1 };
		}
		// DXVK reports these to the render graph, which checks BasicRHI's binding model in them (mutable descriptors DXVK does not enable).
		desc.enabledFeatures = &state->enabledFeatures.core;
		if (FAILED(state->setGuestDevice(&desc)))
			return fail("DXVK could not import BasicRHI's device (see DXVK's log)");

		logger::info("[ORG] Owned frame: BasicRHI created the Vulkan device (engine name '{}', crash markers {}); DXVK runs on it as a guest "
					 "on queue {}:0, the render graph keeps compute {} and copy {}",
			state->engineName, ci.enableCrashMarkers ? "on" : "off", graphicsFamily, desc.compute.queue ? std::to_string(computeFamily) : "none",
			desc.copy.queue ? std::to_string(copyFamily) : "none");
		disabledReason = "the game's D3D11 device has not been created";
		return true;
	}

	void GuestDevice::Confirm(void* a_d3d11Device)
	{
		if (!state || !state->device || !*state->device || active)
			return;
		auto* d3dDevice = static_cast<ID3D11Device*>(a_d3d11Device);
		if (!d3dDevice || FAILED(d3dDevice->QueryInterface(__uuidof(IDXGIVkInteropDevice), state->interop.put_void()))) {
			state->setGuestDevice(nullptr);
			disabledReason = "the game's D3D11 device was not created on BasicRHI's device";
			logger::error("[ORG] Owned frame off: {}", disabledReason);
			return;
		}
		// Until the render graph installs its own hooks (RenderGraphRuntime::Initialize), BasicRHI's queue accesses take DXVK's lock.
		rhi::vulkan::QueueSubmissionHooks hooks{ nullptr, &GuestDevice::LockQueue, &GuestDevice::UnlockQueue, nullptr };
		if (rhi::vulkan::ShareVulkanDevice(state->device->Get(), hooks) != rhi::Result::Ok) {
			disabledReason = "BasicRHI could not share its device with DXVK";
			logger::error("[ORG] Owned frame off: {}", disabledReason);
			return;
		}
		active = true;
		disabledReason.clear();
	}

	std::shared_ptr<rhi::DevicePtr> GuestDevice::Device() const
	{
		return state ? state->device : nullptr;
	}

	void GuestDevice::Abandon(std::string a_reason)
	{
		disabledReason = std::move(a_reason);
		logger::error("[ORG] Owned frame off: {}; DXVK creates its own device and presents", disabledReason);
		active = false;
		if (state && state->setGuestDevice)
			state->setGuestDevice(nullptr);
		if (state)
			state->device.reset();
	}

	void* GuestDevice::DeviceProc(const char* a_name) const
	{
		return state && state->getDeviceProcAddr ? reinterpret_cast<void*>(state->getDeviceProcAddr(state->vkDevice, a_name)) : nullptr;
	}

	void GuestDevice::LockQueue(void*, VkQueue_T*)
	{
		if (!t_queueHeld)
			Get().state->interop->LockSubmissionQueue();
	}

	void GuestDevice::UnlockQueue(void*, VkQueue_T*)
	{
		if (!t_queueHeld)
			Get().state->interop->ReleaseSubmissionQueue();
	}

	GuestDevice::QueueHeldScope::QueueHeldScope() { t_queueHeld = true; }
	GuestDevice::QueueHeldScope::~QueueHeldScope() { t_queueHeld = false; }
}

#else  // !CS_HAS_RENDER_GRAPH

namespace RenderGraph::Engine
{
	bool GuestDevice::Prepare(void*) { return false; }
	void GuestDevice::Confirm(void*) {}
	std::shared_ptr<rhi::DevicePtr> GuestDevice::Device() const { return nullptr; }
	void GuestDevice::Abandon(std::string) {}
	void* GuestDevice::DeviceProc(const char*) const { return nullptr; }
	void GuestDevice::LockQueue(void*, VkQueue_T*) {}
	void GuestDevice::UnlockQueue(void*, VkQueue_T*) {}
	GuestDevice::QueueHeldScope::QueueHeldScope() {}
	GuestDevice::QueueHeldScope::~QueueHeldScope() {}
}

#endif
