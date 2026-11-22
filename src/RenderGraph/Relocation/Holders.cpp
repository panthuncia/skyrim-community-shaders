// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/Relocation/Holders.h"

#include <Resources/BackedResource.h>
#include <Resources/GloballyIndexedResource.h>
#include <Resources/Relocation.h>
#include <Resources/Resource.h>

#include <algorithm>
#include <exception>
#include <mutex>
#include <optional>
#include <vector>

namespace RenderGraph::Relocation
{
	namespace
	{
		org::BackedResource* BackedOf(const org::Resource& a_resource)
		{
			return dynamic_cast<org::BackedResource*>(const_cast<org::Resource*>(&a_resource));
		}

		struct BufferMovesListeners
		{
			std::mutex mutex;
			std::vector<std::function<void()>> listeners;
		};

		BufferMovesListeners& Listeners()
		{
			static BufferMovesListeners listeners;
			return listeners;
		}

		/** @brief The whole-resource SRV index, or nothing when the resource has no primary view. */
		std::optional<std::uint32_t> IndexOf(const org::GloballyIndexedResource& a_resource)
		{
			try {
				if (!a_resource.HasSRV() || a_resource.GetNumSRVSlices() == 0 || a_resource.GetNumSRVMipLevels() == 0)
					return std::nullopt;
				return a_resource.GetSRVInfo(0).slot.index;
			} catch (const std::exception&) {
				return std::nullopt;
			}
		}
	}

	void Pin(const org::Resource& a_resource)
	{
		auto* backed = BackedOf(a_resource);
		// Hot callers (buffer refs, device addresses) pin the same buffers again and again. The class is an atomic load.
		if (!backed || backed->GetRelocationClass() == org::relocation::Class::Pinned)
			return;
		backed->SetRelocationClass(org::relocation::Class::Pinned);
	}

	void FollowAtIdle(const org::Resource& a_resource)
	{
		auto* backed = BackedOf(a_resource);
		if (!backed)
			return;
		// Hot callers (buffer refs, device addresses) mark the same buffers again and again. The class is an atomic load.
		const auto current = backed->GetRelocationClass();
		if (current == org::relocation::Class::Pinned || current == org::relocation::Class::Written)
			return;
		auto* record = backed->CurrentRelocationRecord();
		if (!record)
			return;
		// ORG pins mapped and shared memory by default. Saying so on the resource keeps the next call on the fast path.
		if (record->GetClass() == org::relocation::Class::Pinned) {
			backed->SetRelocationClass(org::relocation::Class::Pinned);
			return;
		}
		// A ReadOnly resource comes back to Written too: once its address went out it must not move while the GPU runs.
		backed->SetRelocationClass(org::relocation::Class::Written);
	}

	void Seal(const std::shared_ptr<org::Resource>& a_resource)
	{
		if (!a_resource)
			return;
		auto* backed = BackedOf(*a_resource);
		if (!backed)
			return;
		const auto current = backed->GetRelocationClass();
		// Only FollowAtIdle sets Written on a resource. Its followers follow idle moves only.
		if (current == org::relocation::Class::Pinned || current == org::relocation::Class::ReadOnly || current == org::relocation::Class::Written)
			return;
		// Without an owner record (no backing yet, or placed in an alias heap) nothing could move it.
		if (!backed->CurrentRelocationRecord())
			return;
		backed->SetRelocationClass(org::relocation::Class::ReadOnly);
	}

	void AddBufferMovesListener(std::function<void()> a_listener)
	{
		if (!a_listener)
			return;
		auto& listeners = Listeners();
		std::scoped_lock lock(listeners.mutex);
		listeners.listeners.push_back(std::move(a_listener));
	}

	void FollowBufferMoves()
	{
		const auto& journal = org::relocation::Service::Get().Journal();
		static std::uint64_t cursor = journal.Head();
		if (journal.Head() == cursor)
			return;
		static std::vector<org::relocation::JournalEntry> entries;
		entries.clear();
		const bool complete = journal.Read(cursor, entries);
		// A texture has no address, so its entry carries 0 for both.
		const auto buffers = std::ranges::count_if(entries, [](const auto& a_entry) { return a_entry.oldAddress != a_entry.newAddress; });
		if (complete && buffers == 0)
			return;
		std::vector<std::function<void()>> listeners;
		{
			auto& registry = Listeners();
			std::scoped_lock lock(registry.mutex);
			listeners = registry.listeners;
		}
		if (complete)
			logger::info("[ORG] relocation: {} buffers with device addresses moved; {} followers refresh them", buffers, listeners.size());
		else
			logger::warn("[ORG] relocation: journal entries were lost, so buffers may have moved; {} followers refresh their addresses", listeners.size());
		for (const auto& listener : listeners)
			listener();
	}

	std::uint64_t JournalHead()
	{
		return org::relocation::Service::Get().Journal().Head();
	}

	bool MovedSince(std::uint64_t a_from, std::uint64_t a_resourceID)
	{
		const auto& journal = org::relocation::Service::Get().Journal();
		if (journal.Head() == a_from)
			return false;
		std::vector<org::relocation::JournalEntry> entries;
		if (!journal.Read(a_from, entries))
			return true;
		return std::ranges::any_of(entries, [&](const auto& a_entry) { return a_entry.resourceID == a_resourceID; });
	}

	JournalCursor::JournalCursor() :
		cursor(org::relocation::Service::Get().Journal().Head())
	{}

	bool JournalCursor::Read(std::vector<std::uint64_t>& a_moved)
	{
		const auto& journal = org::relocation::Service::Get().Journal();
		if (journal.Head() == cursor)
			return true;
		std::vector<org::relocation::JournalEntry> entries;
		const bool complete = journal.Read(cursor, entries);
		for (const auto& entry : entries)
			a_moved.push_back(entry.resourceID);
		return complete;
	}

	bool JournalCursor::Behind() const
	{
		return org::relocation::Service::Get().Journal().Head() != cursor;
	}

	bool IndexCache::Store(const Entry& a_entry)
	{
		const auto resource = a_entry.resource.lock();
		const auto cell = a_entry.cell.lock();
		if (!resource || !cell)
			return false;
		if (const auto index = IndexOf(*resource))
			cell->store(*index, std::memory_order_release);
		return true;
	}

	void IndexCache::Track(const std::shared_ptr<const org::GloballyIndexedResource>& a_resource, const std::shared_ptr<Cell>& a_cell)
	{
		if (!a_resource || !a_cell)
			return;
		Entry entry{ a_resource, a_cell };
		std::scoped_lock lock(mutex);
		// Stored under the lock, so a Refresh that read a move of this resource cannot store an older index after it.
		Store(entry);
		entries.emplace(a_resource->GetGlobalResourceID(), std::move(entry));
		if (entries.size() < pruneAt)
			return;
		std::erase_if(entries, [](const auto& a_pair) { return a_pair.second.resource.expired() || a_pair.second.cell.expired(); });
		pruneAt = (std::max<std::size_t>)(64, entries.size() * 2);
	}

	void IndexCache::Refresh()
	{
		std::scoped_lock lock(mutex);
		if (!cursor.Behind())
			return;
		moved.clear();
		if (!cursor.Read(moved)) {
			// Entries were lost: every cell is read again.
			std::erase_if(entries, [](const auto& a_pair) { return !Store(a_pair.second); });
			pruneAt = (std::max<std::size_t>)(64, entries.size() * 2);
			return;
		}
		std::ranges::sort(moved);
		const auto unique = std::ranges::unique(moved);
		moved.erase(unique.begin(), unique.end());
		for (const auto id : moved) {
			auto [first, last] = entries.equal_range(id);
			while (first != last)
				first = Store(first->second) ? std::next(first) : entries.erase(first);
		}
	}
}
