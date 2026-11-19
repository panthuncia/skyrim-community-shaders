#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace org
{
	class Buffer;
}

namespace org::services
{
	struct ShrinkPolicy;
	struct BackingAllocation;
	struct BackingPoolHooks;
	struct BackingRequest;
}

namespace DCLF
{
	/** @brief DCLF's hooks for org::services::BackingPool: how a backing is made and where a released one is destroyed. */
	namespace PooledBackings
	{
		/** @brief The device address of a_buffer, or 0 without a graph host. */
		std::uint64_t DeviceAddressOf(const org::Buffer& a_buffer);

		/**
		 * @brief BackingPoolHooks::allocate. Makes a buffer without an ECS entity, since an acquirer off the render thread may not touch
		 * the host's world. Throws when it cannot be made.
		 */
		org::services::BackingAllocation Allocate(const org::services::BackingRequest& a_request);

		/**
		 * @brief BackingPoolHooks::retire. Destroys a released buffer on the graph host's cleanup lane, never on the releasing thread.
		 * Lock-free.
		 */
		void Retire(std::shared_ptr<org::Buffer> a_buffer);

		/** @brief Hooks with Allocate and Retire set. */
		org::services::BackingPoolHooks Hooks();

		/**
		 * @brief spareFree for pools that publish about once a frame. One free backing per frame in flight but the newest.
		 * With fewer, the pool releases a surplus backing and remakes it a frame later.
		 */
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
