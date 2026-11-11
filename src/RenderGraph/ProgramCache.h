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
	/** @brief Where a key of a ProgramCache is. */
	enum class ProgramStatus : std::uint8_t
	{
		Missing,   // never asked for
		Building,  // queued or compiling on the build thread
		Ready,     // built: Get returns it
		Failed,    // the build failed (logged once); never built again until ClearFailed
	};

	namespace ProgramCacheDetail
	{
		/** @brief The desc's defines resolved now (CS's common defines read State: the caller's thread, never the build thread). */
		void ResolveDefines(RasterProgram::Desc& a_desc);
		void ResolveDefines(ComputeProgram::Desc& a_desc);

		/** @brief A desc's name for the logs: its sources, entry and own defines. */
		std::string Label(const RasterProgram::Desc& a_desc);
		std::string Label(const ComputeProgram::Desc& a_desc);

		/** @brief The program built from a_desc on the calling thread (RasterProgram::Load / ComputeProgram::Load); null when it failed. */
		std::shared_ptr<const RasterProgram> Load(const RasterProgram::Desc& a_desc);
		std::shared_ptr<const ComputeProgram> Load(const ComputeProgram::Desc& a_desc);

		/** @brief Queues a_job on the one program build thread (started at the first job). False after StopProgramBuilds. Any thread. */
		bool Post(std::function<void()> a_job);

		/** @brief Whether the render graph runs (a build has a device and a compiler). */
		bool Available();
	}

	/**
	 * @brief RenderGraphRuntime::Shutdown, before the device goes: the queued builds are dropped and the one in progress finishes.
	 * Their keys stay Building, so nothing draws with them.
	 */
	void StopProgramBuilds();

	/**
	 * @brief A keyed cache of graph programs (RasterProgram or ComputeProgram) built off the render thread.
	 *
	 * Get returns the key's program once it is built, and null until then: the caller keeps the engine's (or D3D11's) draw for that
	 * key meanwhile, and never withholds work it cannot draw yet. The first Get of a key makes its desc (a_describe, on the calling
	 * thread, with CS's common defines resolved there) and queues the build on the one program build thread, which compiles the
	 * shaders (or finds them in the shader store) and creates the pipeline. A failed build is logged once (with the program's
	 * label; the compile's own error is logged by Load), and the key stays Failed: it is not built again on every call.
	 *
	 * Hot reload is the programs' own (RasterProgram/ComputeProgram register with the pipeline registry): a built program's pipeline
	 * is swapped in place. ClearFailed lets a reload retry the keys that failed.
	 *
	 * Threads: Get, StatusOf, ClearFailed and Clear on the render thread (the cache's map is under a mutex anyway). The build thread
	 * only fills the key's entry.
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

		/**
		 * @brief The key's program when built; null while it builds, when it failed, or without a render graph. The first call for a
		 * key calls a_describe once and queues the build.
		 */
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
			const bool queued = ProgramCacheDetail::Post([entry, desc = std::move(desc), cacheName = name]() {
				std::shared_ptr<const Program> program;
				try {
					program = ProgramCacheDetail::Load(desc);
				} catch (const std::exception& e) {
					logger::error("[ORG] {} programs: building {} threw: {}", cacheName, ProgramCacheDetail::Label(desc), e.what());
				}
				// Require built it on the render thread meanwhile: that program stays.
				if (entry->program.load(std::memory_order_acquire))
					return;
				if (program) {
					entry->program.store(std::move(program), std::memory_order_release);
					entry->status.store(ProgramStatus::Ready, std::memory_order_release);
					return;
				}
				entry->status.store(ProgramStatus::Failed, std::memory_order_release);
				logger::error("[ORG] {} programs: {} could not be built; what needs it stays with the engine", cacheName, ProgramCacheDetail::Label(desc));
			});
			if (!queued)
				entry->status.store(ProgramStatus::Failed, std::memory_order_release);
			return nullptr;
		}

		/**
		 * @brief The key's program, built on the calling thread when the build thread has not finished it: the primary path's last
		 * resort for a key that setup did not queue, or that is still queued (docs/development/p5/primary-path.md, rule 2). Each
		 * synchronous build is logged, so missing enumerations are found. Null only when the key failed (logged once) or without a
		 * render graph. A queued build that finishes later finds the entry filled and drops its own program.
		 */
		std::shared_ptr<const Program> Require(const Key& a_key, const std::function<Desc()>& a_describe)
		{
			std::shared_ptr<Entry> entry;
			{
				std::scoped_lock lock(mutex);
				if (!ProgramCacheDetail::Available())
					return nullptr;
				if (const auto found = entries.find(a_key); found != entries.end()) {
					entry = found->second;
					if (auto program = entry->program.load(std::memory_order_acquire))
						return program;
					if (entry->status.load(std::memory_order_acquire) == ProgramStatus::Failed)
						return nullptr;
				} else {
					entry = std::make_shared<Entry>();
					entries.emplace(a_key, entry);
				}
			}
			Desc desc = a_describe();
			ProgramCacheDetail::ResolveDefines(desc);
			const std::string label = ProgramCacheDetail::Label(desc);
			logger::info("[ORG] {} programs: {} built on the render thread at its first use (not built at setup yet)", name, label);
			std::shared_ptr<const Program> program;
			try {
				program = ProgramCacheDetail::Load(desc);
			} catch (const std::exception& e) {
				logger::error("[ORG] {} programs: building {} threw: {}", name, label, e.what());
			}
			if (!program) {
				entry->status.store(ProgramStatus::Failed, std::memory_order_release);
				logger::error("[ORG] {} programs: {} could not be built; what needs it stays with the engine", name, label);
				return nullptr;
			}
			entry->program.store(program, std::memory_order_release);
			entry->status.store(ProgramStatus::Ready, std::memory_order_release);
			return program;
		}

		/** @brief Where the key is: Missing, Building, Ready or Failed. */
		ProgramStatus StatusOf(const Key& a_key) const
		{
			std::scoped_lock lock(mutex);
			const auto found = entries.find(a_key);
			return found == entries.end() ? ProgramStatus::Missing : found->second->status.load(std::memory_order_acquire);
		}

		/** @brief The failed keys forgotten: their next Get builds them again (after a shader reload fixed them). */
		void ClearFailed()
		{
			std::scoped_lock lock(mutex);
			std::erase_if(entries, [](const auto& a_entry) { return a_entry.second->status.load(std::memory_order_acquire) == ProgramStatus::Failed; });
		}

		/** @brief Every key forgotten. A build in progress completes into an entry nobody reads. Programs callers hold stay valid. */
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

		std::string name;
		mutable std::mutex mutex;
		std::map<Key, std::shared_ptr<Entry>, Compare> entries;
	};
}

#endif
