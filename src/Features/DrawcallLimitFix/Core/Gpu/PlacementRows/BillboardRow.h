#pragma once

#include <cstdint>

namespace RE
{
	class BSGeometry;
}

namespace DCLF
{
	struct BindlessPlacement;

	/**
	 * @brief Placement rows for billboards, effect leaves under an NiBillboardNode that DCLF turns to each view's camera on the GPU.
	 *
	 * The engine turns a billboard during culling (NiBillboardNode::OnVisible, 0x140d3f720), so a leaf's world is whichever camera
	 * culled it last. DCLF reads none of that. The row holds the node's unturned base and the leaf's unturned world, and the vertex
	 * stage builds the turn from the drawing view's camera, so one row serves every view.
	 *
	 * The row layout is decoded by Effect.hlsl's DCLFBillboardWorld and must not change without it.
	 * PreviousWorld holds the base rotation, the leaf's bound radius and the face mode. Bound is the pivot with a radius that covers
	 * every orientation.
	 */
	namespace PlacementRows
	{
		/**
		 * @brief An NiBillboardNode's face mode, userFlags (+0x128) & 7. Read at the offset because CommonLib's member is at the wrong
		 * offset in this build and reads 0 for ROTATE_ABOUT_UP nodes.
		 */
		std::uint32_t BillboardFaceMode(const void* a_node);

		/**
		 * @brief Writes a billboard leaf's row into a_out and returns true. Returns false and leaves a_out alone for any other geometry.
		 * Call inside the engine-read window. Nothing a cull writes is read.
		 */
		bool SampleBillboardRow(const RE::BSGeometry& a_geometry, BindlessPlacement& a_out);
	}
}
