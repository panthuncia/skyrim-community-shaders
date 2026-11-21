#pragma once

// The depth commit's hole event, for the report.

#include <cstdint>

namespace DCLF::Features::Grass
{
	/** @brief The depth commit could not draw grass on a frame that withheld the engine's passes, leaving a hole. */
	struct GrassMissed
	{
		std::uint32_t frame;
		std::uint64_t instanceSlots, bufferInstances, bucketSlots, bufferBuckets, shapeSlots, bufferShapes, meshSlots, bufferMeshes;
	};
}
