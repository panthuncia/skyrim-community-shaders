#pragma once

#include <cstdint>

namespace org
{
	class Buffer;
}

namespace org::services
{
	struct ShrinkPolicy;
	struct BackingPoolHooks;
}

namespace DCLF
{
	/** @brief DCLF's settings for OMS's graph host backing pools (GraphHostBackings.h), read from its switches. */
	namespace PooledBackings
	{
		/**
		 * @brief The device address of a_buffer, or 0 without a graph host. Marks it FollowAtIdle: the caller reads the address
		 * again after an idle move (the scene's set does, SceneBufferSet).
		 */
		std::uint64_t DeviceAddressOf(const org::Buffer& a_buffer);

		/**
		 * @brief OMS's graph host hooks on CS's graph host, for long-lived backings: allocate off the render thread, retire on the
		 * cleanup lane. A backing asked with its device address is marked FollowAtIdle and reports no address (Backing::address
		 * is 0), since an idle move would leave it stale. Readers use DeviceAddressOf.
		 */
		org::services::BackingPoolHooks Hooks();

		/**
		 * @brief Hooks for a pool whose backings are made and reused about once a frame or scene revision: each is made in ORG's
		 * PerFrame memory group (BackingFlags::perFrame), apart from long-lived buffers and textures, and never moves. Backing::address
		 * stays valid for the backing's life.
		 */
		org::services::BackingPoolHooks PerFrameHooks();

		/** @brief spareFree for pools that publish about once a frame (OMS PerFrameSpares over CS's frames in flight). */
		std::uint32_t PerFrameSpares();

		/**
		 * @brief How many copies a per-frame pool keeps of a class (CS_DCLF_SCENE_COPIES, default 3, at least 2).
		 * A producer reuses the copy the oldest in-flight frame bound, and its uploads wait on the GPU.
		 */
		std::uint32_t SceneCopies();

		/**
		 * @brief The shrink policy for every growable scene buffer (CS_DCLF_SHRINK). By default a buffer asked at most a quarter of its
		 * capacity for 5 s shrinks. Off means grow-only and eager means at once.
		 */
		const org::services::ShrinkPolicy& Shrink();
	}
}
