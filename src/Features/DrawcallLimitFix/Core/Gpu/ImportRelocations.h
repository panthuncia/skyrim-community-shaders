#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace DCLF::ImportRelocations
{
	/**
	 * @brief One move of a tracked import's resource (RenderGraphRuntime::TrackResources).
	 *
	 * DXVK moved the resource to new storage, copying its contents. An import describing an older storage still reads the old one,
	 * which `storage` keeps alive and valid: its importer attaches it to that import's owner, so it goes when the import's last user
	 * does (behind the GPU's use, as the import itself), and takes the new storage with a new import.
	 */
	struct Relocation
	{
		std::uint64_t cookie = 0;             // the resource's (DxvkOrgInteropTrackedResource::cookie)
		std::uint64_t serial = 0;             // the new storage's: an import described with an older serial reads the old storage
		std::shared_ptr<const void> storage;  // the old storage, released (RenderGraphRuntime::ReleaseStorage) with its last reference
	};

	/** @brief Since the start. Any thread. */
	struct Stats
	{
		std::uint64_t images = 0, buffers = 0;  // relocations taken
		std::int64_t storagesHeld = 0;          // old storages not yet released
	};

	/**
	 * @brief The render owner (FrameAnchors::IsRenderOwner), at every Present (IDXGISwapChain_Present): takes DXVK's relocations
	 * and hands each to its kind's importers (GpuTextures; both GpuResources instances, which take them at their next BeginFrame).
	 * Every Present, not Drawcall Limit Fix's frame start: whoever draws with an import (Scaleform on a loading screen, ImGui in a
	 * menu) does so in frames DCLF does not run, and an import that moves is read from its old storage until the move is taken.
	 * Safe outside DCLF's frame: the texture registry and the buffer inboxes take their own locks. Nothing without pinless imports.
	 */
	void Poll();

	Stats GetStats();
}
