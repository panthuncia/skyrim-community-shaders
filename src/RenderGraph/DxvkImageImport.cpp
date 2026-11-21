// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/DxvkImageImport.h"

#include "RenderGraph/Backend.h"
#include "RenderGraph/BufferRefs.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/ImageRegistry.h"
#include "RenderGraph/Switches.h"

#include <Resources/ExternalTextureResource.h>
#include <Resources/TextureDescription.h>
#include <Resources/VersionedTexture.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Declared here because rhi_interop_dx12.h brings DirectX-Headers' d3d12.h, which clashes with this unit's Windows SDK headers.
struct ID3D12Resource;
namespace rhi::dx12
{
	Result import_resource(rhi::Device device, ID3D12Resource* resource, rhi::ResourcePtr& out) noexcept;
}

namespace RenderGraph
{
	std::optional<rhi::ResourceLayout> HomeLayoutOf(VkImageLayout a_layout)
	{
		switch (a_layout) {
		case VK_IMAGE_LAYOUT_GENERAL:
			return rhi::ResourceLayout::Common;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			return rhi::ResourceLayout::ShaderResource;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			return rhi::ResourceLayout::RenderTarget;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			return rhi::ResourceLayout::DepthReadWrite;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
			return rhi::ResourceLayout::DepthRead;
		default:
			return std::nullopt;
		}
	}

	namespace
	{
		rhi::ResourcePtr ImportImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, const char* a_name, bool a_exclusive)
		{
			if (!HomeLayoutOf(a_image.layout))
				return {};
			// In D3D12 native mode the description carries the ID3D12Resource where the VkImage goes.
			if (Backend::IsD3D12()) {
				rhi::ResourcePtr resource;
				if (rhi::dx12::import_resource(a_device, reinterpret_cast<ID3D12Resource*>(a_image.image), resource) != rhi::Result::Ok)
					return {};
				if (a_name)
					resource->SetName(a_name);
				return resource;
			}
			rhi::vulkan::ImportedImageDesc import{};
			import.image = a_image.image;
			import.createInfo.flags = a_image.flags;
			import.createInfo.imageType = a_image.type;
			import.createInfo.format = a_image.format;
			import.createInfo.extent = a_image.extent;
			import.createInfo.mipLevels = a_image.mipLevels;
			import.createInfo.arrayLayers = a_image.arrayLayers;
			import.createInfo.samples = a_image.samples;
			import.createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			import.createInfo.usage = a_image.usage;
			import.createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			import.createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			import.currentLayout = a_image.layout;
			// In GENERAL, DXVK and the graph share the image with memory barriers alone.
			// In any other layout, or when exclusive, the graph moves it between layouts and returns it to this one.
			import.simultaneousAccess = a_image.layout == VK_IMAGE_LAYOUT_GENERAL && !a_exclusive;
			import.debugName = a_name;
			rhi::ResourcePtr resource;
			if (rhi::vulkan::import_image(a_device, import, resource) != rhi::Result::Ok)
				return {};
			return resource;
		}
	}

	rhi::ResourcePtr ImportDxvkImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, const char* a_name)
	{
		// A raw import, used between graph executions while the image rests in its home layout.
		return ImportImage(a_device, a_image, a_name, false);
	}

	rhi::ResourcePtr ImportD3D12Resource(rhi::Device a_device, void* a_resource, const char* a_name)
	{
		rhi::ResourcePtr resource;
		if (!a_resource || rhi::dx12::import_resource(a_device, static_cast<ID3D12Resource*>(a_resource), resource) != rhi::Result::Ok || !resource)
			return {};
		if (a_name)
			resource->SetName(a_name);
		return resource;
	}

	std::shared_ptr<org::ExternalTextureResource> ImportDxvkTexture(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image,
		org::TextureDescription a_desc, const char* a_name)
	{
		const auto home = HomeLayoutOf(a_image.layout);
		if (!home)
			return nullptr;
		// Native mode imports every image exclusively. Each image must be one graph resource so one tracker follows its layout.
		const bool exclusive = NativeGraphMovesImages();
		auto resource = ImportImage(a_device, a_image, a_name, exclusive);
		if (!resource)
			return nullptr;
		// The graph finds the image in its home state and gives it back in it.
		a_desc.initialLayout = *home;
		const bool commonOnly = *home == rhi::ResourceLayout::Common && !exclusive;
		auto result = org::ExternalTextureResource::CreateShared(std::move(resource), a_desc, commonOnly);
		if (result) {
			result->SetName(a_name);
			// The image's one graph resource. A second live one is fatal.
			ImageRegistry::Register(reinterpret_cast<std::uint64_t>(a_image.image),
				{ result, nullptr, std::make_shared<const org::TextureDescription>(a_desc), "ImportDxvkTexture", a_name ? a_name : "", ImageRegistry::Kind::Import });
		}
		return result;
	}

	bool NativeGraphMovesImages()
	{
		// Native mode has one model on both backends, where the graph's ledgers carry every image's state.
		return Engine::Native::Active();
	}

	namespace RawAccess
	{
		namespace
		{
			struct Counters
			{
				std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Kind::Count)> counts{};
				std::array<std::uint64_t, static_cast<std::size_t>(Kind::Count)> reported{};
				std::mutex mutex;
				std::unordered_set<std::uint64_t> logged;
			};
			Counters& Get()
			{
				static Counters counters;
				return counters;
			}
		}

		bool On()
		{
			static const bool on = Switches::On("CS_ORG_RAW_ACCESS_CHECK");
			return on;
		}

		void Count(Kind a_kind)
		{
			if (a_kind < Kind::Count)
				Get().counts[static_cast<std::size_t>(a_kind)].fetch_add(1, std::memory_order_relaxed);
		}

		bool LogOnce(std::uint64_t a_key)
		{
			constexpr std::size_t kMaxLines = 64;
			auto& counters = Get();
			const std::lock_guard lock(counters.mutex);
			if (counters.logged.size() >= kMaxLines)
				return false;
			return counters.logged.insert(a_key).second;
		}

		void Summary()
		{
			if (!On())
				return;
			auto& counters = Get();
			std::array<std::uint64_t, static_cast<std::size_t>(Kind::Count)> now{};
			for (std::size_t i = 0; i < now.size(); ++i)
				now[i] = counters.counts[i].load(std::memory_order_relaxed);
			{
				const std::lock_guard lock(counters.mutex);
				if (now == counters.reported)
					return;
				counters.reported = now;
			}
			logger::info(
				"[ORG raw access] so far: {} native context operations on tracked images outside the graph ({} where the graph may hold "
				"another layout); {} sent through external work; {} DCLF views in GENERAL of images the graph moves; {} DCLF views made on "
				"the graph resource",
				now[static_cast<std::size_t>(Kind::ContextTracked)], now[static_cast<std::size_t>(Kind::ContextRace)],
				now[static_cast<std::size_t>(Kind::Routed)], now[static_cast<std::size_t>(Kind::ViewGeneral)],
				now[static_cast<std::size_t>(Kind::ViewFromGraph)]);
		}
	}

	namespace
	{
		// One import per native buffer, shared by every range of it. Held weakly, since the leases keep them alive.
		struct ImportedBuffer
		{
			rhi::ResourcePtr resource;
			std::uint64_t bytes = 0;  // what the import's views cover on Vulkan, D3D12 imports the whole resource
			std::uint64_t base = 0;       // the import's device address on Vulkan
			std::uint64_t described = 0;  // the buffer's start as described at import, which differs for a reused handle value
		};
		struct ImportedBuffers
		{
			std::mutex mutex;
			std::unordered_map<std::uint64_t, std::weak_ptr<ImportedBuffer>> byBuffer;
			std::size_t pruneAt = 64;
		};
		ImportedBuffers& Imports()
		{
			static ImportedBuffers imports;
			return imports;
		}
		// Logs the first 32 import mismatches.
		bool LogImportMismatch()
		{
			static std::atomic<std::uint32_t> logged{ 0 };
			return logged.fetch_add(1, std::memory_order_relaxed) < 32;
		}
	}

	BufferRef ImportBufferRef(rhi::Device a_device, std::uint64_t a_buffer, std::uint64_t a_offset, std::uint64_t a_bytes, std::uint64_t a_address,
		std::shared_ptr<const void>& a_keep, std::source_location a_where)
	{
		a_keep.reset();
		if (!a_device || !a_buffer || !a_bytes)
			return {};
		// A raw view loads whole words, so it covers the last partial word too. Imported buffers are padded to whole words.
		const std::uint64_t end = a_offset + ((a_bytes + 3) & ~std::uint64_t(3));
		auto& imports = Imports();
		std::shared_ptr<ImportedBuffer> imported;
		{
			const std::lock_guard lock(imports.mutex);
			auto& slot = imports.byBuffer[a_buffer];
			imported = slot.lock();
			// A reused handle value of another buffer would read the old buffer's memory, so import again.
			const std::uint64_t base = a_address >= a_offset ? a_address - a_offset : 0;
			if (imported && !Backend::IsD3D12() && a_address && imported->described && imported->described != base) {
				if (LogImportMismatch())
					logger::error("[ORG] the cached import of VkBuffer 0x{:x} was of a buffer at 0x{:x}, but the buffer described now starts at 0x{:x} "
								  "(offset {}, address 0x{:x}) from {}:{}: a reused handle; imported again",
						a_buffer, imported->described, base, a_offset, a_address, std::filesystem::path(a_where.file_name()).filename().string(), a_where.line());
				imported.reset();
			}
			if (!imported || (!Backend::IsD3D12() && imported->bytes < end)) {
				// A range past the import's views is imported again over every range seen so far. Leases keep the older import alive.
				auto fresh = std::make_shared<ImportedBuffer>();
				if (Backend::IsD3D12()) {
					fresh->resource = ImportD3D12Resource(a_device, reinterpret_cast<void*>(a_buffer), "CS described buffer");
				} else {
					rhi::vulkan::ImportedBufferDesc desc{};
					desc.buffer = reinterpret_cast<VkBuffer>(a_buffer);
					desc.size = (std::max)(end, imported ? imported->bytes : 0);
					desc.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
					desc.concurrentSharing = true;
					desc.debugName = "CS described buffer";
					if (rhi::vulkan::import_buffer(a_device, desc, fresh->resource) != rhi::Result::Ok)
						fresh->resource.Reset();
					fresh->bytes = desc.size;
					rhi::VulkanResourceInfo info{};
					if (fresh->resource && rhi::vulkan::get_resource_info(fresh->resource.Get(), info))
						fresh->base = info.deviceAddress;
					fresh->described = a_address ? base : 0;
					// The views are written from the import's address, so it must match the description's.
					if (fresh->resource && a_address && fresh->base != base && LogImportMismatch())
						logger::error("[ORG] VkBuffer 0x{:x} imported at 0x{:x}, but described at 0x{:x} (offset {}, address 0x{:x})", a_buffer,
							fresh->base, base, a_offset, a_address);
				}
				if (!fresh->resource)
					return {};
				imported = std::move(fresh);
				slot = imported;
			}
			if (imports.byBuffer.size() >= imports.pruneAt) {
				std::erase_if(imports.byBuffer, [](const auto& a_entry) { return a_entry.second.expired(); });
				imports.pruneAt = (std::max<std::size_t>)(64, 2 * imports.byBuffer.size());
			}
		}
		const BufferRef ref = ToBufferRef(a_device.GetBufferViewRef({ imported->resource->GetHandle(), a_offset }));
		if (ref)
			a_keep = std::move(imported);
		return ref;
	}
}
