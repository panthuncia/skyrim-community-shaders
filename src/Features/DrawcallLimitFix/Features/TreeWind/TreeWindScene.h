#pragma once

// Tree wind's scene resources. Included by the feature's sources that use them.

#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"

#include "RenderGraph/ComputeProgram.h"
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <Resources/Buffers/Buffer.h>

#include <array>
#include <cstdint>
#include <memory>

namespace DCLF::Draws
{
	struct SceneBuffers;
}

namespace DCLF::Features::TreeWind
{
	/**
	 * @brief The trees' GPU state. Made once at setup, used on the render thread.
	 * Without the program, the wind rows still exist and stay zero at t124, and the pass stays out of the graph.
	 */
	struct TreeWindScene
	{
		// Tree rows (the commits' uploads) and GPU-only clocks, by tree slot.
		Draws::Versioned trees, treeClocks;
		std::shared_ptr<org::Buffer> treeFrameBuffer;  // TreeWindFrameRow
		// One wind buffer per render parity (FrameView::RenderSerial). The Z-prepass writes this render's and the draws read the other.
		std::array<Draws::Versioned, 2> treeWindRows;
		/** @brief The wind buffer render a_render's draws read, which the render before wrote. */
		const Draws::Versioned& WindReadBuffer(std::uint64_t a_render) const { return treeWindRows[(a_render + 1) & 1]; }
		org::services::FilledContents<> treesFilled;  // the tree rows' content epoch (SceneTables::treesContentEpoch)
		/** @brief The tree rows' content epoch held by the version a_sealed names, ~0 for none. */
		std::uint64_t TreesContentEpoch(const Draws::NamedVersions& a_sealed) const { return treesFilled.Since(a_sealed.Of(trees)).value_or(~0ull); }
		std::uint32_t treeCount = 0;
		std::uint32_t treeFrame = 0;  // the scene frame of treeInputs
		TreeWindFrame treeInputs{};
		// The previous frame's, which wrote the buffer this frame's draws read. ~0u until a frame has run.
		std::uint32_t previousTreeFrame = ~0u;
		TreeWindFrame previousTreeInputs{};
		std::uint64_t previousTreesContentEpoch = ~0ull;  // the tree rows that frame's pass read
		std::uint64_t frameTreesContentEpoch = ~0ull;     // this frame's
		std::shared_ptr<const ComputeProgram> treeWind;
	};

	/** @brief The trees' GPU state in a_scene, or null before it is made. */
	TreeWindScene* SceneOf(const Draws::SceneBuffers& a_scene);
	/** @brief SceneOf, sharing ownership. */
	std::shared_ptr<TreeWindScene> SharedSceneOf(const Draws::SceneBuffers& a_scene);
}
