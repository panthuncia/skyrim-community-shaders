#pragma once

#include "RenderGraph/BufferRef.h"

// Tree LOD's scene resources: the TreeInstances tables, the cull program and the counts' readbacks. Included by the sources that use them.

#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeInstancesLayout.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"

#include "RenderGraph/ComputeProgram.h"
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <Render/RunReadback.h>
#include <Resources/Buffers/Buffer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	struct SceneBuffers;
}

namespace DCLF::Features::TreeLod
{
	// One thread per instance in phase 1, or per retest entry in phase 2.
	inline constexpr const char* kTreeLodCullShader = "DrawcallLimitFix/Features/TreeLod/TreeLodCullCS.hlsl";
	inline constexpr std::uint32_t kTreeLodCullGroup = 64;
	struct TreeLodCullConstants
	{
		std::uint32_t shapesIndex = 0, instancesIndex = 0, visibleIndex = 0, latchIndex = 0, latchOffset = 0, drawIndex = 0;
		std::uint32_t phase = 0;
		std::uint32_t retestOffset = 0;  // bytes into the visible list
		std::uint32_t hzbIndex = 0, hzbSizePacked = 0, hzbMips = 0, padding = 0;
	};
	inline constexpr std::uint32_t kTreeLodCullConstantWords = sizeof(TreeLodCullConstants) / 4;

	/** @brief Tree LOD cull counts, read back per run region on the graph host's thread. */
	struct TreeLodCounts
	{
		std::shared_ptr<org::RunReadback> readback;  // the visible header, then the stage ring, per stable run
		std::vector<std::uint8_t> filled;            // by region: its copy holds a frame's counts
		std::atomic<std::uint64_t> frames{ 0 }, phaseOne{ 0 }, retests{ 0 }, phaseTwo{ 0 };
		std::uint32_t stagesJudged = 0;
		std::uint32_t probeLogged = 0;
		std::uint64_t stageFrames = 0, stageMisses = 0, stageLogged = 0, clipDiffers = 0, clipLogged = 0;
	};

	/**
	 * @brief Tree LOD's tables, read by buffer view ref in the vertex stage. Made once at setup.
	 * Null without the cull program or the buffer view refs, and tree LOD then stays the engine's.
	 */
	struct TreeLodTables
	{
		Draws::Versioned shapes, instances, meshes, visible;
		std::array<org::services::FilledVersion, 3> filled;  // shapes, instances, meshes
		std::shared_ptr<org::Buffer> draw;
		RenderGraph::BufferRef drawRef;
		std::shared_ptr<const ComputeProgram> cull;
		std::atomic<bool> ready{ false };
		std::uint64_t uploads = 0, slotsSent = 0;  // since the last report
		std::shared_ptr<TreeLodCounts> counts;
		std::shared_ptr<org::Buffer> stages;
		RenderGraph::BufferRef stagesRef;
		// The last depth commit's draw row. A trees view's faces reuse the tables with their own lists.
		TreeInstances::DrawRow row{};
	};

	inline constexpr std::uint32_t kShapeSlotsInput = 0, kMeshSlotsInput = 1, kInputWords = 2;

	/** @brief The tables in a_scene, or null before they are made. */
	TreeLodTables* TablesOf(const Draws::SceneBuffers& a_scene);
	/** @brief TablesOf, sharing ownership. */
	std::shared_ptr<TreeLodTables> SharedTablesOf(const Draws::SceneBuffers& a_scene);
	Api::SceneResourcesId TablesId();

	/**
	 * @brief Binds the engine's tree LOD atlas (static at 0x1433dcd18) and slot 0's sampler into a_row. False with no texture.
	 */
	bool TreeLodTextureBinding(OwnedRow<TreeInstances::DrawRow>& a_row);

	/** @brief Whether the engine's tree LOD atlas is there to draw with. */
	bool TreeLodAtlasReady();
}
