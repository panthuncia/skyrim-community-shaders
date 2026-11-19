#pragma once

// Diagnostic events from the grass sink, mirror and depth commit, for the parity check and the report.

#include <cstdint>

namespace DCLF::Features::Grass
{
	class Mirror;

	/** @brief Any thread, from the capture's sink. */
	struct GrassCaptured
	{
		enum class Kind : std::uint8_t
		{
			Add,
			RemoveGroup,
			Remove,
			Rejected
		};
		const void* shape;
		std::uint32_t group;
		std::uint32_t count;
		std::uint64_t seq;
		Kind kind;
	};

	/** @brief Render thread. Sent for applied and stale events alike. */
	struct GrassEventDrained
	{
		const void* shape;
	};

	/** @brief Render thread. */
	struct GrassDrained
	{
		const Mirror& mirror;
		std::uint32_t frame;
	};

	/** @brief The depth commit could not draw grass on a frame that withheld the engine's passes, leaving a hole. */
	struct GrassMissed
	{
		std::uint32_t frame;
		std::uint64_t instanceSlots, bufferInstances, bucketSlots, bufferBuckets, shapeSlots, bufferShapes, meshSlots, bufferMeshes;
	};
}
