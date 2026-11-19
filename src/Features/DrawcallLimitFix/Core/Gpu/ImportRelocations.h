#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace DCLF::ImportRelocations
{
	/**
	 * @brief One move of a tracked import's resource to new storage. An import of the old storage keeps reading it.
	 * `storage` keeps the old storage alive until that import's last user goes.
	 */
	struct Relocation
	{
		std::uint64_t cookie = 0;
		std::uint64_t serial = 0;  // imports described with an older serial read the old storage
		std::shared_ptr<const void> storage;  // released with its last reference
	};

	/** @brief Totals since the start. Any thread. */
	struct Stats
	{
		std::uint64_t images = 0, buffers = 0;
		std::int64_t storagesHeld = 0;  // not yet released
	};

	/**
	 * @brief Render owner, at every Present. Takes DXVK's relocations and hands each to its importers.
	 * Runs on every Present, not only DCLF frames, because loading screens and menus also draw with imports.
	 */
	void Poll();

	Stats GetStats();
}
