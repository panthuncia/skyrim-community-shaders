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
		/** @brief The device address of a_buffer, or 0 without a graph host. */
		std::uint64_t DeviceAddressOf(const org::Buffer& a_buffer);

		/** @brief OMS's graph host hooks on CS's graph host: allocate off the render thread, retire on the cleanup lane. */
		org::services::BackingPoolHooks Hooks();

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
