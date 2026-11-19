#include "SkinPartitionWatch.h"

#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace DCLF::SkinPartitionWatch
{
	namespace
	{
		// skee stores the copy at once or at the next main loop tick, so a few frames would do. The margin covers a load's long frames.
		constexpr auto kWatchFor = std::chrono::seconds(5);
		// A slider drag makes many copies, so past this the oldest are dropped.
		constexpr std::size_t kMaxWatched = 512;

		struct Watched
		{
			const void* partition;
			std::chrono::steady_clock::time_point until;
		};
		std::mutex mutex;
		std::vector<Watched> watched;
		std::atomic<std::uint32_t> watchedCount{ 0 };
		std::atomic<std::uint64_t> copies{ 0 };

		void Note(const void* a_partition)
		{
			const auto until = std::chrono::steady_clock::now() + kWatchFor;
			std::lock_guard lock(mutex);
			copies.fetch_add(1, std::memory_order_relaxed);
			for (auto& entry : watched)
				if (entry.partition == a_partition) {
					entry.until = until;
					return;
				}
			if (watched.size() >= kMaxWatched)
				watched.erase(watched.begin());
			watched.push_back({ a_partition, until });
			watchedCount.store(static_cast<std::uint32_t>(watched.size()), std::memory_order_relaxed);
		}

		struct CreateDeepCopy
		{
			// Only the source is known here, the partition the plan holds. The copy is the caller's and is stored later.
			static void thunk(RE::NiObject* a_this, RE::NiPointer<RE::NiObject>& a_out)
			{
				func(a_this, a_out);
				static const REL::Relocation<const RE::NiRTTI*> partitionRtti{ RE::NiSkinPartition::Ni_RTTI };
				if (a_this && a_this->GetRTTI() == partitionRtti.get())
					Note(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		// It chains, so no entry check.
		stl::detour_thunk<CreateDeepCopy>(REL::RelocationID(68839, 70191));
		HookSites::NoteDetour("NiObject::CreateDeepCopy (RaceMenu's skin partition copies)", CreateDeepCopy::func.address());
		if (CreateDeepCopy::func.address())
			logger::info("[DCLF] skin partition copies watched (NiObject::CreateDeepCopy): RaceMenu's morphs of still actors are drawn too");
	}

	bool Any() { return watchedCount.load(std::memory_order_relaxed) != 0; }

	void Collect(std::vector<const void*>& a_out)
	{
		a_out.clear();
		const auto now = std::chrono::steady_clock::now();
		std::lock_guard lock(mutex);
		std::erase_if(watched, [&](const Watched& a_entry) { return a_entry.until < now; });
		watchedCount.store(static_cast<std::uint32_t>(watched.size()), std::memory_order_relaxed);
		for (const auto& entry : watched)
			a_out.push_back(entry.partition);
	}

	Counts GetCounts()
	{
		return { copies.load(std::memory_order_relaxed), watchedCount.load(std::memory_order_relaxed) };
	}
}
