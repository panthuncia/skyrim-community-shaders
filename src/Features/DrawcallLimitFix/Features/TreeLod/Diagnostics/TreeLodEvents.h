#pragma once

// Tree LOD's hook points for its parity observers and its depth-commit hole log. Render thread.

#include <cstdint>

namespace DCLF::Features::TreeLod
{
	class Mirror;

	/** @brief Render thread, Mirror::Drain: an event of `shape` applied. */
	struct TreeLodEventDrained
	{
		const void* shape;
	};

	/** @brief Render thread: the mirror's events drained on frame `frame`. */
	struct TreeLodDrained
	{
		const Mirror& mirror;
		std::uint32_t frame;
	};

	/** @brief The depth commit could not draw tree LOD on a frame that withheld the engine's passes (a hole): the tables' shape slots and the buffer's. */
	struct TreeLodMissed
	{
		std::uint32_t frame;
		std::uint64_t shapeSlots, bufferSlots;
	};
}
