#include "Utils/SharedShaderStore.h"

#include <ORGModuleServices/ShaderStore.h>

#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"

namespace Util
{
	namespace
	{
		std::atomic<org::services::ShaderStore*> g_openedStore{ nullptr };

		/**
		 * @brief Deletes what the old cache left in Data/ShaderCache: everything except the store's own folder.
		 * This can be thousands of files, so it runs on its own thread, and a later start finishes what an exit interrupted.
		 */
		void RemoveOldCacheFiles()
		{
			std::thread([] {
				std::error_code ec;
				uint64_t removed = 0, bytes = 0;
				for (const auto& entry : std::filesystem::directory_iterator(L"Data/ShaderCache", ec)) {
					if (entry.path().filename().wstring() == kShaderStoreDirName)
						continue;
					// The size first, so the log says what the old layout took.
					std::error_code sizeError;
					if (entry.is_regular_file(sizeError))
						bytes += entry.file_size(sizeError);
					for (std::filesystem::recursive_directory_iterator it(entry.path(), sizeError), end; !sizeError && it != end; it.increment(sizeError)) {
						std::error_code fileError;
						if (it->is_regular_file(fileError))
							bytes += it->file_size(fileError);
					}
					std::error_code removeError;
					removed += std::filesystem::remove_all(entry.path(), removeError);
					if (removeError)
						logger::warn("Could not delete old shader cache entry {}: {}", WStringToString(entry.path().wstring()), removeError.message());
				}
				if (removed)
					logger::info("Deleted {} files ({} MB) of the old shader cache", removed, bytes >> 20);
			}).detach();
		}
	}

	std::shared_ptr<org::services::ShaderStore> SharedShaderStore()
	{
		// Never destroyed. ~ShaderCache runs among the static destructors and still uses the store, and the
		// index is saved as it changes, so nothing is lost at exit. Windows releases the store's mutex.
		static const auto* store = new std::shared_ptr<org::services::ShaderStore>([] {
			org::services::ShaderStoreOptions options;
			options.sourceRoot = L"Data";
			options.cacheDirectory = std::filesystem::path(L"Data/ShaderCache") / kShaderStoreDirName;
			options.persistent = SIE::ShaderCache::Instance().IsDiskCache();
			options.maxBytes = static_cast<uint64_t>(globals::state->shaderStoreMaxMB.load(std::memory_order_relaxed)) << 20;
			const bool persistent = options.persistent;
			auto created = std::make_shared<org::services::ShaderStore>(std::move(options));
			if (!created->OwnsIndex())
				logger::warn("Another game instance owns the shader cache: this one uses it but will not save its index");
			if (persistent) {
				const auto info = created->IndexInfo();
				logger::info("Shader store opened: {} variants, {} artifacts, {} MB stored ({} MB before compression), {} packs, {} MB on disk, {} recovered, {} evicted, "
							 "{} packs compacted ({} MB copied, {} MB freed). Index {:.1f} ms, packs {:.1f} ms, compaction {:.1f} ms.",
					info.variants, info.artifactsAtStart, info.storedBytes >> 20, info.rawBytes >> 20, info.packsAtStart, info.packBytesAtStart >> 20,
					info.recoveredAtStart, info.artifactsEvicted, info.packsCompacted, info.compactedBytes >> 20, info.reclaimedBytes >> 20,
					info.indexLoadMicroseconds / 1000.0, info.packScanMicroseconds / 1000.0, info.compactMicroseconds / 1000.0);
				if (info.overCapBytes)
					logger::warn("Shader store is {} MB over its size limit, because recent starts used all of it. Raise the limit in Advanced settings if you switch between builds or feature sets.", info.overCapBytes >> 20);
				RemoveOldCacheFiles();
			}
			g_openedStore = created.get();
			return created;
		}());
		return *store;
	}

	void BeginShaderStoreGeneration()
	{
		if (auto* store = g_openedStore.load())
			store->BeginGeneration();
	}

	void FinishShaderStoreBatch(std::string_view a_batch)
	{
		const auto store = SharedShaderStore();
		store->Flush();
		const auto info = store->IndexInfo();
		logger::info("Shader store after {}: {} Index: {} variants, {} dependency sets, {} entries, {} files, {} KB saved.", a_batch, store->Summary(), info.variants, info.dependencySets,
			info.dependencyEntries, info.files, info.savedBytes >> 10);
	}
}
