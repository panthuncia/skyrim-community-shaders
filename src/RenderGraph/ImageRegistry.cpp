// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/ImageRegistry.h"

#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <algorithm>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace RenderGraph::ImageRegistry
{
	namespace
	{
		/** @brief One image's entry. Weak, so the registry never keeps a resource or slot alive. */
		struct ImageRegistryRecord
		{
			std::weak_ptr<org::Resource> resource;
			const org::Resource* key = nullptr;
			std::weak_ptr<org::VersionedTexture> slot;
			std::shared_ptr<const org::TextureDescription> views;
			std::string owner;
			std::string name;
			Kind kind = Kind::Import;
		};

		struct ImageRegistryState
		{
			std::mutex mutex;
			std::unordered_map<std::uint64_t, ImageRegistryRecord> byImage;
			std::unordered_map<const org::Resource*, std::uint64_t> imageOf;
			std::size_t pruneAt = 256;
		};

		ImageRegistryState& ImageRegistryRecords()
		{
			// Never destroyed, since twins and native records retire their resources during static destruction too.
			static auto* state = new ImageRegistryState;
			return *state;
		}

		std::string_view KindText(Kind a_kind)
		{
			switch (a_kind) {
			case Kind::Made:
				return "made by ORG";
			case Kind::Owned:
				return "native graph-owned";
			default:
				return "an import";
			}
		}

		/** @brief Drops a_record and its resource's image mapping. Under the lock. */
		void EraseImageRecord(ImageRegistryState& a_state, std::unordered_map<std::uint64_t, ImageRegistryRecord>::iterator a_record)
		{
			if (const auto mapped = a_state.imageOf.find(a_record->second.key); mapped != a_state.imageOf.end() && mapped->second == a_record->first)
				a_state.imageOf.erase(mapped);
			a_state.byImage.erase(a_record);
		}

		/** @brief Drops the entries whose resource is gone. Under the lock. */
		void PruneImageRecords(ImageRegistryState& a_state)
		{
			std::erase_if(a_state.byImage, [](const auto& a_record) { return a_record.second.resource.expired(); });
			std::erase_if(a_state.imageOf, [&](const auto& a_mapped) {
				const auto record = a_state.byImage.find(a_mapped.second);
				return record == a_state.byImage.end() || record->second.key != a_mapped.first;
			});
			a_state.pruneAt = (std::max<std::size_t>)(256, 2 * a_state.byImage.size());
		}
	}

	void Register(std::uint64_t a_image, Entry a_entry)
	{
		if (!a_image || !a_entry.resource)
			return;
		const org::Resource* const key = a_entry.resource.get();
		auto& state = ImageRegistryRecords();
		// Declared before the lock, so a resource whose last reference this holds is released after the lock.
		std::shared_ptr<org::Resource> live;
		std::string duplicate;
		{
			const std::lock_guard lock(state.mutex);
			if (state.byImage.size() >= state.pruneAt)
				PruneImageRecords(state);
			// A resource names one image. An entry under another image is stale.
			if (const auto mapped = state.imageOf.find(key); mapped != state.imageOf.end() && mapped->second != a_image) {
				if (const auto old = state.byImage.find(mapped->second); old != state.byImage.end() && old->second.key == key)
					state.byImage.erase(old);
				state.imageOf.erase(mapped);
			}
			if (const auto found = state.byImage.find(a_image); found != state.byImage.end()) {
				auto& record = found->second;
				live = record.resource.lock();
				if (live && record.key == key) {
					// The same resource again: its owner adds what it knows now.
					record.resource = a_entry.resource;
					if (a_entry.slot)
						record.slot = a_entry.slot;
					if (a_entry.views)
						record.views = std::move(a_entry.views);
					if (!a_entry.owner.empty())
						record.owner = std::move(a_entry.owner);
					if (!a_entry.name.empty())
						record.name = std::move(a_entry.name);
					if (a_entry.kind != Kind::Import)
						record.kind = a_entry.kind;
					return;
				}
				if (live) {
					duplicate = fmt::format(
						"image 0x{:X} has two live graph resources: '{}' ({}, from {}) and '{}' ({}, from {}). Each tracks the "
						"image's layout, so their moves would race. The first must be retired before another resource of the image is registered.",
						a_image, record.name, KindText(record.kind), record.owner, a_entry.name, KindText(a_entry.kind), a_entry.owner);
				} else {
					// The old resource is gone, and the handle names a new image.
					EraseImageRecord(state, found);
				}
			}
			if (duplicate.empty()) {
				ImageRegistryRecord record;
				record.resource = a_entry.resource;
				record.key = key;
				record.slot = a_entry.slot;
				record.views = std::move(a_entry.views);
				record.owner = std::move(a_entry.owner);
				record.name = std::move(a_entry.name);
				record.kind = a_entry.kind;
				state.byImage.emplace(a_image, std::move(record));
				state.imageOf[key] = a_image;
			}
		}
		if (!duplicate.empty())
			RenderGraph::Fatal("image registry", duplicate);
	}

	void SetSlot(const org::Resource* a_resource, std::shared_ptr<org::VersionedTexture> a_slot)
	{
		if (!a_resource)
			return;
		auto& state = ImageRegistryRecords();
		const std::lock_guard lock(state.mutex);
		const auto mapped = state.imageOf.find(a_resource);
		if (mapped == state.imageOf.end())
			return;
		if (const auto record = state.byImage.find(mapped->second); record != state.byImage.end() && record->second.key == a_resource)
			record->second.slot = a_slot;
	}

	std::optional<Entry> Find(std::uint64_t a_image)
	{
		if (!a_image)
			return std::nullopt;
		auto& state = ImageRegistryRecords();
		// Declared before the lock, so the references it takes are released after the lock.
		std::optional<Entry> result;
		{
			const std::lock_guard lock(state.mutex);
			const auto found = state.byImage.find(a_image);
			if (found == state.byImage.end())
				return std::nullopt;
			Entry entry;
			entry.resource = found->second.resource.lock();
			if (!entry.resource)
				return std::nullopt;
			entry.slot = found->second.slot.lock();
			entry.views = found->second.views;
			entry.owner = found->second.owner;
			entry.name = found->second.name;
			entry.kind = found->second.kind;
			result = std::move(entry);
		}
		return result;
	}

	std::optional<Entry> Find(const IUnknown* a_d3d11)
	{
		if (!a_d3d11)
			return std::nullopt;
		DxvkOrgInteropResourceInfo info{};
		if (!RenderGraphRuntime::Get().DescribeResource(const_cast<IUnknown*>(a_d3d11), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return std::nullopt;
		return Find(reinterpret_cast<std::uint64_t>(info.image.image));
	}

	bool Rekey(std::uint64_t a_old, std::uint64_t a_new)
	{
		if (!a_old || !a_new || a_old == a_new)
			return false;
		auto& state = ImageRegistryRecords();
		std::string duplicate;
		{
			const std::lock_guard lock(state.mutex);
			const auto found = state.byImage.find(a_old);
			if (found == state.byImage.end())
				return false;
			if (const auto existing = state.byImage.find(a_new); existing != state.byImage.end()) {
				if (!existing->second.resource.expired() && existing->second.key != found->second.key)
					duplicate = fmt::format("image 0x{:X} has two live graph resources after a relocation: '{}' ({}, from {}) moved into it, and '{}' "
											"({}, from {}) is registered for it.",
						a_new, found->second.name, KindText(found->second.kind), found->second.owner, existing->second.name,
						KindText(existing->second.kind), existing->second.owner);
				else
					EraseImageRecord(state, existing);
			}
			if (duplicate.empty()) {
				ImageRegistryRecord record = std::move(found->second);
				state.byImage.erase(found);
				state.imageOf[record.key] = a_new;
				state.byImage.emplace(a_new, std::move(record));
			}
		}
		if (!duplicate.empty())
			RenderGraph::Fatal("image registry", duplicate);
		return true;
	}

	void Retire(const org::Resource* a_resource)
	{
		if (!a_resource)
			return;
		auto& state = ImageRegistryRecords();
		const std::lock_guard lock(state.mutex);
		const auto mapped = state.imageOf.find(a_resource);
		if (mapped == state.imageOf.end())
			return;
		if (const auto record = state.byImage.find(mapped->second); record != state.byImage.end() && record->second.key == a_resource)
			state.byImage.erase(record);
		state.imageOf.erase(mapped);
	}
}

