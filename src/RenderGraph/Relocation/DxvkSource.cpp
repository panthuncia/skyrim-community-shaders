// Guest mode: takes DXVK's moves of the pinless imports at ORG's commit and routes them to their importers.

#include "RenderGraph/Relocation/DxvkSource.h"

#include "DxvkExports.h"
#include "Globals.h"
#include "RenderGraph/DxvkImageImport.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/TextureDescription.h>

#include <Tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace RenderGraph::Relocation
{
	namespace
	{
		std::atomic<std::shared_ptr<DxvkSource>> installed;

		struct Counters
		{
			std::atomic<std::uint64_t> images{ 0 }, buffers{ 0 }, held{ 0 }, followed{ 0 }, followFailed{ 0 }, cannotFollow{ 0 }, unclaimed{ 0 };
			std::atomic<std::int64_t> storagesHeld{ 0 };
		};
		Counters& Count()
		{
			static Counters counters;
			return counters;
		}

		// An old storage, given back to DXVK by its last holder on any thread.
		std::shared_ptr<const void> Keep(void* a_storage)
		{
			Count().storagesHeld.fetch_add(1, std::memory_order_relaxed);
			return std::shared_ptr<const void>(a_storage, [](const void* a_held) {
				void* storage = const_cast<void*>(a_held);
				// After the runtime's shutdown DXVK's device is going, and it frees its storages itself.
				if (RenderGraphRuntime::Get().PinlessImports())
					if (const auto release = DxvkExports::Get().releaseInteropStorage)
						release(globals::d3d::device, 1, &storage);
				Count().storagesHeld.fetch_sub(1, std::memory_order_relaxed);
			});
		}

		bool SameHandle(rhi::ResourceHandle a_a, rhi::ResourceHandle a_b) noexcept
		{
			return a_a.index == a_b.index && a_a.generation == a_b.generation;
		}

		// Erases the weak entries that expired.
		template <class T>
		void Prune(std::vector<std::weak_ptr<T>>& a_list)
		{
			std::erase_if(a_list, [](const auto& a_weak) { return a_weak.expired(); });
		}
	}

	// ---- tickets ----

	DxvkSource::Ticket::Ticket(Ticket&& a_other) noexcept :
		source(std::move(a_other.source)), id(std::exchange(a_other.id, 0))
	{}

	DxvkSource::Ticket& DxvkSource::Ticket::operator=(Ticket&& a_other) noexcept
	{
		if (this != &a_other) {
			Reset();
			source = std::move(a_other.source);
			id = std::exchange(a_other.id, 0);
		}
		return *this;
	}

	DxvkSource::Ticket::~Ticket()
	{
		Reset();
	}

	void DxvkSource::Ticket::Reset() noexcept
	{
		if (source && id)
			source->EndTicket(id);
		source.reset();
		id = 0;
	}

	void DxvkSource::EndTicket(std::uint64_t a_id)
	{
		std::scoped_lock lock(mutex);
		tickets.erase(a_id);
	}

	// ---- registrations ----

	DxvkSource::Registration::Registration(std::shared_ptr<DxvkSource> a_source, std::uint64_t a_cookie, std::uint64_t a_serial) :
		source(std::move(a_source)), cookie(a_cookie), serial(a_serial)
	{}

	DxvkSource::Registration::~Registration()
	{
		// The old storages go with the members, after the lock.
		if (source)
			source->Forget(*this);
	}

	void DxvkSource::Forget(Registration& a_registration)
	{
		std::scoped_lock lock(mutex);
		const auto found = byCookie.find(a_registration.cookie);
		if (found == byCookie.end())
			return;
		std::erase(found->second, &a_registration);
		if (found->second.empty())
			byCookie.erase(found);
	}

	// ---- installation ----

	void DxvkSource::Install()
	{
		if (installed.load(std::memory_order_acquire))
			return;
		std::shared_ptr<DxvkSource> source(new DxvkSource());
		installed.store(source, std::memory_order_release);
		org::relocation::Service::Get().AddSource(source);
		logger::info("[ORG] relocations: DXVK's moves of pinless imports go through ORG's relocation service");
	}

	void DxvkSource::Uninstall()
	{
		const auto source = installed.exchange(nullptr, std::memory_order_acq_rel);
		if (!source)
			return;
		org::relocation::Service::Get().RemoveSource(source.get());
		// Moves polled but not committed: their storages go back now.
		source->ready.clear();
	}

	std::shared_ptr<DxvkSource> DxvkSource::Get()
	{
		return installed.load(std::memory_order_acquire);
	}

	DxvkSource::~DxvkSource() = default;

	DxvkSource::Stats DxvkSource::GetStats()
	{
		const auto& counters = Count();
		Stats stats;
		stats.images = counters.images.load(std::memory_order_relaxed);
		stats.buffers = counters.buffers.load(std::memory_order_relaxed);
		stats.storagesHeld = counters.storagesHeld.load(std::memory_order_relaxed);
		stats.held = counters.held.load(std::memory_order_relaxed);
		stats.followed = counters.followed.load(std::memory_order_relaxed);
		stats.followFailed = counters.followFailed.load(std::memory_order_relaxed);
		stats.cannotFollow = counters.cannotFollow.load(std::memory_order_relaxed);
		stats.unclaimed = counters.unclaimed.load(std::memory_order_relaxed);
		return stats;
	}

	DxvkSource::Ticket DxvkSource::Begin()
	{
		Ticket ticket;
		auto source = Get();
		if (!source)
			return ticket;
		{
			std::scoped_lock lock(source->mutex);
			ticket.id = ++source->nextTicket;
			source->tickets.insert(ticket.id);
		}
		ticket.source = std::move(source);
		return ticket;
	}

	std::shared_ptr<DxvkSource::Registration> DxvkSource::Hold(std::uint64_t a_cookie, std::uint64_t a_serial, std::function<void()> a_moved)
	{
		auto source = Get();
		if (!source)
			return nullptr;
		std::shared_ptr<Registration> registration(new Registration(source, a_cookie, a_serial));
		registration->moved = std::move(a_moved);
		std::scoped_lock lock(source->mutex);
		source->byCookie[a_cookie].push_back(registration.get());
		// Moves taken after the description and before this registration.
		if (const auto found = source->unclaimed.find(a_cookie); found != source->unclaimed.end())
			for (const auto& move : found->second)
				if (move.serial > a_serial) {
					registration->storages.push_back(move.storage);
					registration->superseded.store(true, std::memory_order_release);
				}
		return registration;
	}

	std::shared_ptr<DxvkSource::Registration> DxvkSource::Follow(std::uint64_t a_cookie, std::uint64_t a_serial,
		std::shared_ptr<org::ExternalTextureResource> a_resource)
	{
		auto source = Get();
		if (!source || !a_resource)
			return nullptr;
		std::shared_ptr<Registration> registration(new Registration(source, a_cookie, a_serial));
		registration->follows = true;
		registration->resource = a_resource;
		std::scoped_lock lock(source->mutex);
		source->byCookie[a_cookie].push_back(registration.get());
		if (const auto found = source->unclaimed.find(a_cookie); found != source->unclaimed.end())
			for (const auto& move : found->second)
				if (move.serial > a_serial)
					source->AddMove(*registration, move.serial, move.next, move.storage);
		return registration;
	}

	// ---- the poll ----

	void DxvkSource::AddMove(Registration& a_registration, std::uint64_t a_serial, const DxvkOrgInteropResourceInfo& a_next, std::shared_ptr<const void> a_storage)
	{
		a_registration.superseded.store(true, std::memory_order_release);
		if (a_registration.cannotFollow) {
			// It stays on the storage it reads, so it keeps every storage after it.
			a_registration.storages.push_back(std::move(a_storage));
			return;
		}
		auto& move = a_registration.move;
		if (!move)
			move.emplace();
		// Several moves in one poll: the newest serial wins. The older storages stay held until the newest one is released.
		if (a_serial > move->serial) {
			move->serial = a_serial;
			move->next = a_next;
		}
		move->storages.push_back(std::move(a_storage));
		if (!std::exchange(a_registration.queued, true))
			follows.push_back(a_registration.weak_from_this());
	}

	std::optional<org::relocation::Relocation> DxvkSource::MakeRelocation(Registration& a_registration)
	{
		auto move = std::move(*a_registration.move);
		a_registration.move.reset();
		const auto stay = [&] {
			// The import keeps reading its storage. The registration holds this move's storages with it until it goes.
			a_registration.cannotFollow = true;
			for (auto& storage : move.storages)
				a_registration.storages.push_back(std::move(storage));
			Count().cannotFollow.fetch_add(1, std::memory_order_relaxed);
			return std::nullopt;
		};
		const auto resource = a_registration.resource.lock();
		if (!resource)
			return std::nullopt;  // the storages go now: nothing reads them
		auto* host = RenderGraphRuntime::Get().Host();
		const auto& description = resource->GetDescription();
		const auto home = HomeLayoutOf(move.next.image.layout);
		// The switch keeps the description, so the new image must rest where the old one did.
		if (!host || move.next.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE || !home || *home != description.initialLayout)
			return stay();
		// Only the image fields matter to the import. A view's own fields never change in a move.
		auto next = ImportDxvkImage(host->Device(), move.next.image, resource->GetName().c_str());
		if (!next)
			return stay();
		org::relocation::Relocation relocation;
		relocation.resource = resource;
		relocation.source = org::relocation::Source::Dxvk;
		// What the import may still read until the switch: the storages the registration held, and this move's.
		auto parts = std::move(a_registration.storages);
		for (auto& storage : move.storages)
			parts.push_back(std::move(storage));
		// The registration holds them too until the switch is seen. If ORG does not adopt the move, the import stays on them.
		a_registration.storages = parts;
		a_registration.adopting = next.Get().GetHandle();
		a_registration.serial = move.serial;
		relocation.next = std::move(next);
		relocation.retired = std::make_shared<const std::vector<std::shared_ptr<const void>>>(std::move(parts));
		adopting.push_back(a_registration.weak_from_this());
		Count().followed.fetch_add(1, std::memory_order_relaxed);
		return relocation;
	}

	void DxvkSource::Collect()
	{
		const auto poll = DxvkExports::Get().pollInteropRelocations2;
		if (!poll || !RenderGraphRuntime::Get().PinlessImports())
			return;
		ZoneScopedN("CS.ORG.Relocation.DxvkPoll");
		std::vector<DxvkOrgInteropRelocation2> taken;
		{
			std::array<DxvkOrgInteropRelocation2, 128> batch{};
			for (;;) {
				std::uint32_t count = 0;
				if (FAILED(poll(globals::d3d::device, batch.data(), static_cast<std::uint32_t>(batch.size()), &count)))
					break;
				count = (std::min)(count, static_cast<std::uint32_t>(batch.size()));
				taken.insert(taken.end(), batch.begin(), batch.begin() + count);
				if (count < batch.size())
					break;
			}
		}
		// Each taken storage is held from here, so every path releases it.
		std::vector<std::shared_ptr<const void>> storages;
		storages.reserve(taken.size());
		for (const auto& move : taken) {
			storages.push_back(Keep(move.storage));
			(move.kind == DXVK_ORG_INTEROP_RESOURCE_IMAGE ? Count().images : Count().buffers).fetch_add(1, std::memory_order_relaxed);
		}
		std::vector<Registration*> notify;
		// Registrations reached under the lock. Held until after it, since the last owner's release takes the lock.
		std::vector<std::shared_ptr<Registration>> alive;
		const auto reach = [&](const std::weak_ptr<Registration>& a_weak) -> Registration* {
			auto registration = a_weak.lock();
			if (!registration)
				return nullptr;
			alive.push_back(std::move(registration));
			return alive.back().get();
		};
		const bool offStreamIdle = RenderGraphRuntime::Get().OffStreamQueuesIdle();
		{
			std::scoped_lock lock(mutex);
			// Follow moves of the last commit: once adopted, ORG's retirement alone holds their storages.
			for (const auto& weak : adopting) {
				auto* registration = reach(weak);
				if (!registration || !registration->adopting)
					continue;
				const auto resource = registration->resource.lock();
				if (resource && SameHandle(resource->GetAPIResource().GetHandle(), *registration->adopting))
					registration->storages.clear();
				else if (resource)
					Count().followFailed.fetch_add(1, std::memory_order_relaxed);
				registration->adopting.reset();
			}
			adopting.clear();

			for (std::size_t i = 0; i < taken.size(); ++i) {
				const auto& move = taken[i];
				if (const auto found = byCookie.find(move.cookie); found != byCookie.end()) {
					// The storage this move leaves is the one the newest import before it describes. An older import reads an
					// older storage, which an earlier move already handed to it, so it does not hold this one.
					std::uint64_t reader = 0;
					for (const auto* raw : found->second)
						if (raw->serial < move.storageSerial)
							reader = (std::max)(reader, raw->serial);
					for (auto* raw : found->second) {
						// Imported after this move: it reads the new storage already.
						if (raw->serial >= move.storageSerial)
							continue;
						// Its owner is dropping it. Its import is ending.
						auto* registration = reach(raw->weak_from_this());
						if (!registration)
							continue;
						if (registration->follows) {
							AddMove(*registration, move.storageSerial, move.next, storages[i]);
							continue;
						}
						if (registration->serial == reader)
							registration->storages.push_back(storages[i]);
						if (!registration->superseded.exchange(true, std::memory_order_acq_rel)) {
							Count().held.fetch_add(1, std::memory_order_relaxed);
							notify.push_back(registration);
						}
					}
				}
				// An import that began before this poll may have described the old storage and register later.
				if (!tickets.empty())
					unclaimed[move.cookie].push_back({ move.storageSerial, nextTicket, move.next, storages[i] });
			}
			// A kept move goes once no import that began before it is in flight.
			std::uint64_t kept = 0;
			std::erase_if(unclaimed, [&](auto& a_entry) {
				std::erase_if(a_entry.second, [&](const Unclaimed& a_move) { return tickets.empty() || *tickets.begin() > a_move.mark; });
				kept += a_entry.second.size();
				return a_entry.second.empty();
			});
			Count().unclaimed.store(kept, std::memory_order_relaxed);

			// Follow moves are committed only while the compute and copy queues will wait for the stream again.
			if (offStreamIdle) {
				for (const auto& weak : follows) {
					auto* registration = reach(weak);
					if (!registration)
						continue;
					registration->queued = false;
					if (!registration->move)
						continue;
					if (auto relocation = MakeRelocation(*registration))
						ready.push_back(std::move(*relocation));
				}
				follows.clear();
			} else {
				Prune(follows);
			}
		}
		storages.clear();
		// Outside the lock: the importers take their own locks, and may drop registrations.
		for (auto* registration : notify)
			if (registration->moved)
				registration->moved();
	}

	size_t DxvkSource::Poll(std::span<org::relocation::Relocation> a_out)
	{
		// The commit asks again while it fills the whole span. DXVK is polled once per commit: the first call.
		if (!collected) {
			Collect();
			collected = true;
		}
		const std::size_t count = (std::min)(a_out.size(), ready.size());
		for (std::size_t i = 0; i < count; ++i)
			a_out[i] = std::move(ready[i]);
		ready.erase(ready.begin(), ready.begin() + static_cast<std::ptrdiff_t>(count));
		// A short answer ends this commit's polls.
		if (count < a_out.size())
			collected = false;
		return count;
	}
}
