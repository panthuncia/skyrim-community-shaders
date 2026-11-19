#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/RasterProgram.h"

#	include <atomic>
#	include <cstdint>
#	include <functional>
#	include <map>
#	include <memory>
#	include <mutex>
#	include <string>
#	include <utility>

namespace RenderGraph
{
	enum class ProgramStatus : std::uint8_t
	{
		Missing,
		Building,  // queued or compiling on the build thread
		Ready,
		Failed,    // logged once and never built again
	};

	namespace ProgramCacheDetail
	{
		/** @brief Resolves the desc's defines on the caller's thread, since CS's common defines read State. Never the build thread. */
		void ResolveDefines(RasterProgram::Desc& a_desc);
		void ResolveDefines(ComputeProgram::Desc& a_desc);

		/** @brief A desc's name for the logs. */
		std::string Label(const RasterProgram::Desc& a_desc);
		std::string Label(const ComputeProgram::Desc& a_desc);

		/** @brief Builds a_desc on the calling thread. Null when it failed. */
		std::shared_ptr<const RasterProgram> Load(const RasterProgram::Desc& a_desc);
		std::shared_ptr<const ComputeProgram> Load(const ComputeProgram::Desc& a_desc);

		/** @brief Queues a_job on the one program build thread. False after StopProgramBuilds. Any thread. */
		bool Post(std::function<void()> a_job);

		/** @brief Whether the render graph runs, with a device and a compiler. */
		bool Available();
	}

	/** @brief Drops queued builds before the device goes. Their keys stay Building, so nothing draws with them. */
	void StopProgramBuilds();

	/**
	 * @brief A keyed cache of graph programs built off the render thread.
	 *
	 * Get returns null until the key's program is built, and the caller keeps the engine's draw meanwhile.
	 * A failed key stays Failed and is not built again. Get, StatusOf and Clear run on the render thread.
	 *
	 * Usage:
	 *   RenderGraph::ProgramCache<RasterProgram, MyKey> programs{ "Sky" };
	 *   if (auto program = programs.Get(key, [&] { return DescFor(key); })) { draw on the graph } else { leave it to the engine }
	 */
	template <class Program, class Key, class Compare = std::less<Key>>
	class ProgramCache
	{
	public:
		using Desc = typename Program::Desc;

		explicit ProgramCache(std::string a_name) :
			name(std::move(a_name)) {}

		/** @brief The key's program when built, else null. The first call for a key calls a_describe once and queues the build. */
		std::shared_ptr<const Program> Get(const Key& a_key, const std::function<Desc()>& a_describe)
		{
			std::shared_ptr<Entry> entry;
			{
				std::scoped_lock lock(mutex);
				if (const auto found = entries.find(a_key); found != entries.end())
					return found->second->program.load(std::memory_order_acquire);
				if (!ProgramCacheDetail::Available())
					return nullptr;
				entry = std::make_shared<Entry>();
				entries.emplace(a_key, entry);
			}
			Desc desc = a_describe();
			ProgramCacheDetail::ResolveDefines(desc);
			const bool queued = ProgramCacheDetail::Post([entry, desc = std::move(desc), cacheName = name]() { Build(*entry, desc, cacheName); });
			if (!queued)
				Finish(*entry, nullptr);
			return nullptr;
		}

		/**
		 * @brief The key's program now, waiting for or building it on this thread. a_builtHere is set when this call built it.
		 * Render thread. Never call it after StopProgramBuilds, since a dropped build never finishes.
		 */
		std::shared_ptr<const Program> GetNow(const Key& a_key, const std::function<Desc()>& a_describe, bool* a_builtHere = nullptr)
		{
			std::shared_ptr<Entry> entry;
			bool fresh = false;
			{
				std::scoped_lock lock(mutex);
				if (const auto found = entries.find(a_key); found != entries.end())
					entry = found->second;
				else if (!ProgramCacheDetail::Available())
					return nullptr;
				else {
					entry = std::make_shared<Entry>();
					entries.emplace(a_key, entry);
					fresh = true;
				}
			}
			if (fresh) {
				Desc desc = a_describe();
				ProgramCacheDetail::ResolveDefines(desc);
				Build(*entry, desc, name);
				if (a_builtHere)
					*a_builtHere = true;
			} else
				entry->status.wait(ProgramStatus::Building, std::memory_order_acquire);
			return entry->program.load(std::memory_order_acquire);
		}

		/** @brief GetNow that logs a key setup did not queue, so missing enumerations are found. Render thread. */
		std::shared_ptr<const Program> Require(const Key& a_key, const std::function<Desc()>& a_describe)
		{
			bool builtHere = false;
			auto program = GetNow(a_key, a_describe, &builtHere);
			if (builtHere && program)
				logger::info("[ORG] {} programs: a program was built on the render thread at its first use (not queued at setup)", name);
			return program;
		}

		ProgramStatus StatusOf(const Key& a_key) const
		{
			std::scoped_lock lock(mutex);
			const auto found = entries.find(a_key);
			return found == entries.end() ? ProgramStatus::Missing : found->second->status.load(std::memory_order_acquire);
		}

		/** @brief Forgets every key. Programs callers hold stay valid. */
		void Clear()
		{
			std::scoped_lock lock(mutex);
			entries.clear();
		}

	private:
		struct Entry
		{
			std::atomic<std::shared_ptr<const Program>> program;
			std::atomic<ProgramStatus> status{ ProgramStatus::Building };
		};

		/** @brief Marks the entry Ready or Failed and wakes every GetNow waiting on it. */
		static void Finish(Entry& a_entry, std::shared_ptr<const Program> a_program)
		{
			const bool built = a_program != nullptr;
			if (built)
				a_entry.program.store(std::move(a_program), std::memory_order_release);
			a_entry.status.store(built ? ProgramStatus::Ready : ProgramStatus::Failed, std::memory_order_release);
			a_entry.status.notify_all();
		}

		static void Build(Entry& a_entry, const Desc& a_desc, const std::string& a_cacheName)
		{
			std::shared_ptr<const Program> program;
			try {
				program = ProgramCacheDetail::Load(a_desc);
			} catch (const std::exception& e) {
				logger::error("[ORG] {} programs: building {} threw: {}", a_cacheName, ProgramCacheDetail::Label(a_desc), e.what());
			}
			if (!program)
				logger::error("[ORG] {} programs: {} could not be built; what needs it stays with the engine", a_cacheName, ProgramCacheDetail::Label(a_desc));
			Finish(a_entry, std::move(program));
		}

		std::string name;
		mutable std::mutex mutex;
		std::map<Key, std::shared_ptr<Entry>, Compare> entries;
	};
}

#endif
