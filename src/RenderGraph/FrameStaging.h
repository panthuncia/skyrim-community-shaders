#pragma once

#include <Render/FrameArena.h>
#include <Render/RunRef.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace RenderGraph
{
	/** @brief A client's staging region: an oversize region of the runtime's frame arena (org::FrameArena::AcquireOversize). */
	using Staging = std::shared_ptr<const org::FrameArena::Region>;

	/**
	 * @brief A staging region of at least a_bytes for a client's per-run CPU data, such as vertices and texture rows.
	 *
	 * It has a chunk of the frame arena to itself and starts at the chunk's first byte, so the chunk's view reads it at offset 0.
	 * It is not zeroed. It goes back to the arena when its last holder lets it go, which must be the recording that reads it
	 * (`a_recording.Retain(frame)`), so it comes back once the GPU is done with it. A steady frame reuses the freed chunks.
	 * Fatal, naming a_area, when it cannot be made. Any thread, in graph mode.
	 */
	Staging AcquireStaging(std::string_view a_area, std::uint64_t a_bytes);

	/** @brief Every live RunSlots, so a stage's commit can move its staging run's values onto the run it submits. Thread-safe. */
	class RunSlotsRegistry
	{
	public:
		struct Entry
		{
			virtual void MoveRun(const org::RunRef& a_from, const org::RunRef& a_to) const = 0;
		};

		static void MoveAll(const org::RunRef& a_from, const org::RunRef& a_to)
		{
			auto& registry = Get();
			std::scoped_lock lock(registry.mutex);
			for (const auto* entry : registry.entries)
				entry->MoveRun(a_from, a_to);
		}

		static void Add(const Entry* a_entry)
		{
			auto& registry = Get();
			std::scoped_lock lock(registry.mutex);
			registry.entries.push_back(a_entry);
		}

		static void Remove(const Entry* a_entry)
		{
			auto& registry = Get();
			std::scoped_lock lock(registry.mutex);
			std::erase(registry.entries, a_entry);
		}

	private:
		static RunSlotsRegistry& Get()
		{
			// Never destroyed, since RunSlots in static objects remove themselves during static destruction.
			static auto* registry = new RunSlotsRegistry;
			return *registry;
		}

		std::mutex mutex;
		std::vector<const Entry*> entries;
	};

	/**
	 * @brief One value per run, stored by a client's commit and taken by the pass that records that run.
	 * A value is found only by its own run, so a later run of the same lease index takes nothing. Thread-safe.
	 */
	template <class T>
	class RunSlots final : public RunSlotsRegistry::Entry
	{
	public:
		RunSlots() { RunSlotsRegistry::Add(this); }
		~RunSlots() { RunSlotsRegistry::Remove(this); }
		RunSlots(const RunSlots&) = delete;
		RunSlots& operator=(const RunSlots&) = delete;

		void MoveRun(const org::RunRef& a_from, const org::RunRef& a_to) const override
		{
			std::scoped_lock lock(mutex);
			if (a_from.lease >= entries.size() || !(entries[a_from.lease].run == a_from))
				return;
			auto value = std::exchange(entries[a_from.lease].value, nullptr);
			if (entries.size() <= a_to.lease)
				entries.resize(static_cast<std::size_t>(a_to.lease) + 1);
			entries[a_to.lease] = { a_to, std::move(value) };
		}

		void Store(const org::RunRef& a_run, std::shared_ptr<const T> a_value) const
		{
			std::scoped_lock lock(mutex);
			if (entries.size() <= a_run.lease)
				entries.resize(static_cast<std::size_t>(a_run.lease) + 1);
			entries[a_run.lease] = { a_run, std::move(a_value) };
		}
		/** @brief The run's value, left in place for a pass that reads it more than once. */
		std::shared_ptr<const T> Find(const org::RunRef& a_run) const
		{
			std::scoped_lock lock(mutex);
			if (a_run.lease >= entries.size() || !(entries[a_run.lease].run == a_run))
				return nullptr;
			return entries[a_run.lease].value;
		}
		std::shared_ptr<const T> Take(const org::RunRef& a_run) const
		{
			std::scoped_lock lock(mutex);
			if (a_run.lease >= entries.size() || !(entries[a_run.lease].run == a_run))
				return nullptr;
			return std::exchange(entries[a_run.lease].value, nullptr);
		}

		void Clear() const
		{
			std::scoped_lock lock(mutex);
			entries.clear();
		}

	private:
		struct Entry
		{
			org::RunRef run{};
			std::shared_ptr<const T> value;
		};
		mutable std::mutex mutex;
		mutable std::vector<Entry> entries;  // by lease index
	};
}

