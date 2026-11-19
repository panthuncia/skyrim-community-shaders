#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <Render/RunRef.h>

#	include <atomic>
#	include <cstdint>
#	include <memory>
#	include <mutex>
#	include <string>
#	include <utility>
#	include <vector>

namespace org
{
	class LatchBlock;
}

namespace RenderGraph
{
	/**
	 * @brief A pool of host-visible staging buffers for a client's per-run CPU data, reused instead of made every run.
	 *
	 * A block returns to the pool when its last holder lets it go. That holder must be the recording that used it, so the block
	 * comes back only once the GPU is done with it. The pool keeps kGameFramesInFlight times runsPerFrame spares, so a steady
	 * frame makes no block. With maxBlocks set, Acquire returns null when none is spare and the caller keeps the engine's path.
	 * Acquire runs on the render thread. Blocks come back on any thread, and may outlive the pool.
	 *
	 * Usage: `auto staging = RenderGraph::FrameStaging::Create({ .name = "cs.scaleform.staging", .minimumBytes = 256u << 10,
	 * .runsPerFrame = 6 });` then, in the commit,
	 * `frame->staging = staging->Acquire(bytes);` and in Record `a_recording.Retain(frame);`.
	 */
	class FrameStaging : public std::enable_shared_from_this<FrameStaging>
	{
	public:
		struct Desc
		{
			std::string name;
			std::uint32_t minimumBytes = 64u << 10;  // the smallest block made
			std::uint32_t runsPerFrame = 1;          // each run holds one block
			std::uint32_t maxBlocks = 0;             // 0 means no limit
		};

		/** @brief A pool. Any thread. */
		static std::shared_ptr<FrameStaging> Create(Desc a_desc);

		/** @brief A block of at least a_bytes. Null when it cannot be made or maxBlocks are out. */
		std::shared_ptr<org::LatchBlock> Acquire(std::uint64_t a_bytes);

		/** @brief Blocks that exist now, out or spare. */
		std::uint32_t Made() const { return made.load(std::memory_order_relaxed); }

		/** @brief Blocks in the pool now. */
		std::uint32_t Spare() const;

		explicit FrameStaging(Desc a_desc);

	private:
		void GiveBack(std::shared_ptr<org::LatchBlock> a_block);

		Desc desc;
		std::uint32_t keep = 0;  // the most spare blocks kept
		mutable std::mutex mutex;
		std::vector<std::shared_ptr<org::LatchBlock>> spare;
		std::atomic<std::uint32_t> made{ 0 };
		std::atomic_flag churnLogged;
	};

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

#endif
