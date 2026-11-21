#pragma once

// Tree LOD's hook point for its depth-commit hole log. Render thread.

#include <cstdint>

namespace DCLF::Features::TreeLod
{
	/** @brief The depth commit could not draw tree LOD on a frame that withheld the engine's passes (a hole): the tables' shape slots and the buffer's. */
	struct TreeLodMissed
	{
		std::uint32_t frame;
		std::uint64_t shapeSlots, bufferSlots;
	};
}
