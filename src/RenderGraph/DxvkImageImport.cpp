// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/DxvkImageImport.h"

#include "RenderGraph/Backend.h"
#include "RenderGraph/BufferRefs.h"
#include "RenderGraph/Engine/Native/Native.h"
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

// BasicRHI's D3D12 import (rhi_interop_dx12.h), declared here: that header brings DirectX-Headers' d3d12.h, which this translation
// unit's Windows SDK headers would meet.
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
		/** @brief The newest graph import of each image (ImportDxvkTexture), held weakly, for GraphResourceOfImage. */
		struct GraphImports
		{
			std::mutex mutex;
			std::unordered_map<std::uint64_t, std::weak_ptr<org::ExternalTextureResource>> byImage;
			std::size_t pruneAt = 256;
		};
		GraphImports& Imported()
		{
			static GraphImports imports;
			return imports;
		}

		void NoteGraphImport(std::uint64_t a_image, const std::shared_ptr<org::ExternalTextureResource>& a_resource)
		{
			if (!a_image || !a_resource || !Engine::Native::Active())
				return;
			auto& imports = Imported();
			const std::lock_guard lock(imports.mutex);
			imports.byImage[a_image] = a_resource;
			if (imports.byImage.size() >= imports.pruneAt) {
				std::erase_if(imports.byImage, [](const auto& a_entry) { return a_entry.second.expired(); });
				imports.pruneAt = (std::max<std::size_t>)(256, 2 * imports.byImage.size());
			}
		}

		rhi::ResourcePtr ImportImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, const char* a_name, bool a_exclusive)
		{
			if (!HomeLayoutOf(a_image.layout))
				return {};
			// D3D12 (native mode): the description carries the ID3D12Resource where the VkImage goes (Native::DescribeResource).
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
			// In GENERAL, DXVK and the graph share the image with memory barriers alone. In any other layout the graph uses it
			// exclusively and moves it between layouts, starting from and returning to this one. An exclusive import of an image in
			// GENERAL (native mode's graph imports) does the same from GENERAL.
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
		// A raw import: the native context and raw views use it between graph executions, where the image rests in its home.
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
		// Native mode imports every image exclusively: the graph moves it into each pass's layout. Each image is one graph resource
		// (FrameAnchors' imports find its slot first), so one tracker follows its layout.
		const bool exclusive = NativeGraphMovesImages();
		auto resource = ImportImage(a_device, a_image, a_name, exclusive);
		if (!resource)
			return nullptr;
		// The home state: the graph finds the image in it and gives it back in it (external home states).
		a_desc.initialLayout = *home;
		// Common-only (simultaneous access, every pass in GENERAL) is for DXVK's images resting in GENERAL.
		const bool commonOnly = *home == rhi::ResourceLayout::Common && !exclusive;
		auto result = org::ExternalTextureResource::CreateShared(std::move(resource), a_desc, commonOnly);
		if (result) {
			result->SetName(a_name);
			NoteImageResource(reinterpret_cast<std::uint64_t>(a_image.image), result, a_name ? a_name : "");
			NoteGraphImport(reinterpret_cast<std::uint64_t>(a_image.image), result);
		}
		return result;
	}

	bool NativeGraphMovesImages()
	{
		// Native mode has one model on both backends: the graph's ledgers carry every image's state between executions.
		return Engine::Native::Active();
	}

	std::shared_ptr<org::Resource> GraphResourceOfImage(std::uint64_t a_image)
	{
		if (!a_image)
			return nullptr;
		if (auto owned = Engine::Native::GraphTextureOfImage(a_image))
			return owned;
		if (auto made = MadeImageOf(a_image); made.resource)
			return made.resource;
		auto& imports = Imported();
		const std::lock_guard lock(imports.mutex);
		const auto found = imports.byImage.find(a_image);
		return found != imports.byImage.end() ? found->second.lock() : nullptr;
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
		/** @brief One graph resource of an image, as the duplicate check knows it. */
		struct ImageResource
		{
			std::weak_ptr<org::Resource> resource;
			const org::Resource* key = nullptr;
			std::string name;
			bool retired = false;
		};
		struct ImageResources
		{
			std::mutex mutex;
			std::unordered_map<std::uint64_t, std::vector<ImageResource>> byImage;
			std::unordered_map<const org::Resource*, std::uint64_t> imageOf;
			std::size_t pruneAt = 256;
			std::uint32_t logged = 0;
			std::uint64_t found = 0;
		};
		ImageResources& Images()
		{
			static ImageResources images;
			return images;
		}
		bool DuplicateCheckOn()
		{
			static const bool on = Switches::On("CS_ORG_IMAGE_DUPLICATE_CHECK");
			return on;
		}
		/** @brief Drops the entries of resources that are gone. */
		void Prune(ImageResources& a_images, std::vector<ImageResource>& a_entries)
		{
			std::erase_if(a_entries, [&](const ImageResource& a_entry) {
				if (!a_entry.resource.expired())
					return false;
				a_images.imageOf.erase(a_entry.key);
				return true;
			});
		}
		void PruneAll(ImageResources& a_images)
		{
			for (auto it = a_images.byImage.begin(); it != a_images.byImage.end();) {
				Prune(a_images, it->second);
				it = it->second.empty() ? a_images.byImage.erase(it) : std::next(it);
			}
			a_images.pruneAt = (std::max<std::size_t>)(256, 2 * a_images.imageOf.size());
		}
	}

	void NoteImageResource(std::uint64_t a_image, const std::shared_ptr<org::Resource>& a_resource, std::string_view a_name)
	{
		if (!DuplicateCheckOn() || !a_image || !a_resource)
			return;
		auto& images = Images();
		const std::lock_guard lock(images.mutex);
		if (images.imageOf.size() >= images.pruneAt)
			PruneAll(images);
		auto& entries = images.byImage[a_image];
		Prune(images, entries);
		std::string other;
		bool duplicate = false;
		bool known = false;
		for (auto& entry : entries) {
			if (entry.key == a_resource.get()) {
				entry.retired = false;
				known = true;
			} else if (!entry.retired) {
				other = entry.name;
				duplicate = true;
			}
		}
		if (!known) {
			entries.push_back({ a_resource, a_resource.get(), std::string(a_name), false });
			images.imageOf[a_resource.get()] = a_image;
		}
		if (!duplicate)
			return;
		++images.found;
		if (images.logged++ < 32)
			logger::warn(
				"[ORG] duplicate image: '{}' and '{}' are two live graph resources of image 0x{:x}; each tracks its layout, and their moves "
				"are not ordered with each other ({} found so far)",
				a_name, other, a_image, images.found);
	}

	void RetireImageResource(const org::Resource* a_resource)
	{
		if (!DuplicateCheckOn() || !a_resource)
			return;
		auto& images = Images();
		const std::lock_guard lock(images.mutex);
		const auto image = images.imageOf.find(a_resource);
		if (image == images.imageOf.end())
			return;
		const auto entries = images.byImage.find(image->second);
		if (entries == images.byImage.end())
			return;
		for (auto& entry : entries->second)
			if (entry.key == a_resource)
				entry.retired = true;
	}

	namespace
	{
		/** @brief A registered made image (RegisterMadeImage). Held weakly: its maker decides its life. */
		struct MadeEntry
		{
			std::weak_ptr<org::Resource> resource;
			const org::Resource* key = nullptr;
			std::weak_ptr<org::VersionedTexture> slot;
			std::shared_ptr<const org::TextureDescription> views;
			std::string name;
		};
		struct MadeImages
		{
			std::mutex mutex;
			std::unordered_map<std::uint64_t, MadeEntry> byImage;
			std::unordered_map<const org::Resource*, std::uint64_t> imageOf;
			std::size_t pruneAt = 256;
		};
		MadeImages& Made()
		{
			static MadeImages made;
			return made;
		}
	}

	void RegisterMadeImage(std::uint64_t a_image, const std::shared_ptr<org::Resource>& a_resource, const org::TextureDescription& a_views,
		std::string_view a_name)
	{
		if (!a_image || !a_resource)
			return;
		auto& made = Made();
		const std::lock_guard lock(made.mutex);
		if (made.byImage.size() >= made.pruneAt) {
			std::erase_if(made.byImage, [&](const auto& a_entry) {
				if (!a_entry.second.resource.expired())
					return false;
				made.imageOf.erase(a_entry.second.key);
				return true;
			});
			made.pruneAt = (std::max<std::size_t>)(256, 2 * made.byImage.size());
		}
		// A new image under a handle an old entry still names: the old one's image is gone, so its entry goes too.
		if (const auto old = made.byImage.find(a_image); old != made.byImage.end())
			made.imageOf.erase(old->second.key);
		made.byImage[a_image] = { a_resource, a_resource.get(), {}, std::make_shared<const org::TextureDescription>(a_views), std::string(a_name) };
		made.imageOf[a_resource.get()] = a_image;
	}

	void SetMadeImageSlot(const org::Resource* a_resource, const std::shared_ptr<org::VersionedTexture>& a_slot)
	{
		if (!a_resource)
			return;
		auto& made = Made();
		const std::lock_guard lock(made.mutex);
		const auto image = made.imageOf.find(a_resource);
		if (image == made.imageOf.end())
			return;
		if (const auto entry = made.byImage.find(image->second); entry != made.byImage.end() && entry->second.key == a_resource)
			entry->second.slot = a_slot;
	}

	void ForgetMadeImage(const org::Resource* a_resource)
	{
		if (!a_resource)
			return;
		auto& made = Made();
		const std::lock_guard lock(made.mutex);
		const auto image = made.imageOf.find(a_resource);
		if (image == made.imageOf.end())
			return;
		if (const auto entry = made.byImage.find(image->second); entry != made.byImage.end() && entry->second.key == a_resource)
			made.byImage.erase(entry);
		made.imageOf.erase(image);
	}

	MadeImage MadeImageOf(std::uint64_t a_image)
	{
		if (!a_image)
			return {};
		auto& made = Made();
		const std::lock_guard lock(made.mutex);
		const auto entry = made.byImage.find(a_image);
		if (entry == made.byImage.end())
			return {};
		MadeImage result;
		result.resource = entry->second.resource.lock();
		if (!result.resource)
			return {};
		result.slot = entry->second.slot.lock();
		// A slot that moved on to another version no longer stands for this resource.
		if (result.slot && result.slot->CurrentResource() != result.resource)
			result.slot.reset();
		result.views = entry->second.views;
		result.name = entry->second.name;
		return result;
	}

	namespace
	{
		// A described buffer as BasicRHI sees it: one import per native buffer, which every range of it shares (BufferRefs: its raw view
		// pair). The map holds them weakly: the leases' keep holds them.
		struct ImportedBuffer
		{
			rhi::ResourcePtr resource;
			std::uint64_t bytes = 0;  // what the import's views cover (Vulkan); D3D12 imports the whole resource
			std::uint64_t base = 0;       // the import's device address (Vulkan), which its views are written from
			std::uint64_t described = 0;  // the buffer's start as the description gave it when imported: a reused handle value's differs
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
		// The ref check's import lines: the first 32.
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
		const std::uint64_t end = a_offset + a_bytes;
		auto& imports = Imports();
		std::shared_ptr<ImportedBuffer> imported;
		{
			const std::lock_guard lock(imports.mutex);
			auto& slot = imports.byBuffer[a_buffer];
			imported = slot.lock();
			// The description's address names the buffer: a cached import of another buffer whose handle value this one reuses
			// would read the old buffer's memory. Imported again.
			const std::uint64_t base = a_address >= a_offset ? a_address - a_offset : 0;
			if (imported && !Backend::IsD3D12() && a_address && imported->described && imported->described != base) {
				if (BufferRefChecksOn() && LogImportMismatch())
					logger::error("[ORG] ref check: the cached import of VkBuffer 0x{:x} was of a buffer at 0x{:x}, but the buffer described now starts at 0x{:x} "
								  "(offset {}, address 0x{:x}) from {}:{}: a reused handle; imported again",
						a_buffer, imported->described, base, a_offset, a_address, std::filesystem::path(a_where.file_name()).filename().string(), a_where.line());
				imported.reset();
			}
			if (!imported || (!Backend::IsD3D12() && imported->bytes < end)) {
				// A range past the import's views (or no import): import it again, covering every range seen so far. The older import
				// stays while the leases that took it hold it.
				auto fresh = std::make_shared<ImportedBuffer>();
				if (Backend::IsD3D12()) {
					fresh->resource = ImportD3D12Resource(a_device, reinterpret_cast<void*>(a_buffer), "CS described buffer");
				} else {
					rhi::vulkan::ImportedBufferDesc desc{};
					desc.buffer = reinterpret_cast<VkBuffer>(a_buffer);
					desc.size = (std::max)(end, imported ? imported->bytes : 0);
					// A described buffer has a device address (its description's), which the views are written from.
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
					// The import's address is what its views are written from: it must be the description's.
					if (fresh->resource && a_address && fresh->base != base && BufferRefChecksOn() && LogImportMismatch())
						logger::error("[ORG] ref check: VkBuffer 0x{:x} imported at 0x{:x}, but described at 0x{:x} (offset {}, address 0x{:x})", a_buffer,
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
		if (ref && BufferRefChecksOn() && !Backend::IsD3D12()) {
			// The handle's address now, from the driver: the import must still be this buffer's.
			const std::uint64_t now = rhi::vulkan::get_vk_buffer_device_address(a_device, reinterpret_cast<VkBuffer>(a_buffer));
			if (now != imported->base && LogImportMismatch())
				logger::error("[ORG] ref check: VkBuffer 0x{:x} is at 0x{:x} now, its import at 0x{:x} (offset {}, {} bytes, described address 0x{:x}) from {}:{}",
					a_buffer, now, imported->base, a_offset, a_bytes, a_address, std::filesystem::path(a_where.file_name()).filename().string(), a_where.line());
			CheckBufferRef(a_device, ref, imported->resource->GetHandle(), a_address, a_bytes, "ImportBufferRef", a_where);
		}
		if (ref)
			a_keep = std::move(imported);
		return ref;
	}
}
