#pragma once

#include "RenderGraph/BufferRef.h"

// Grass's GPU tables, cull programs and count readbacks, held with the scene's tables.

#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Readback/RunRanges.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassLayout.h"
#include "Features/DrawcallLimitFix/Features/Grass/Grass.h"

#include "RenderGraph/ComputeProgram.h"
#include <ORGModuleServices/GrowableBufferTypes.h>
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

namespace DCLF::Features::Grass
{
	// One thread per instance slot in phase 1, or per retest entry in phase 2.
	inline constexpr const char* kGrassCullShader = "DrawcallLimitFix/Features/Grass/GrassCullCS.hlsl";
	inline constexpr std::uint32_t kGrassCullGroup = 64;
	struct GrassCullConstants
	{
		std::uint32_t drawIndex = 0, instancesIndex = 0, ownersIndex = 0, shapesIndex = 0, bucketsIndex = 0, frameIndex = 0, visibleIndex = 0, drawsIndex = 0;
		std::uint32_t latchIndex = 0, latchOffset = 0;
		std::uint32_t phase = 0;
		std::uint32_t retestOffset = 0;  // bytes into the visible list
		std::uint32_t hzbIndex = 0, hzbSizePacked = 0, hzbMips = 0, padding = 0;
	};
	inline constexpr std::uint32_t kGrassCullConstantWords = sizeof(GrassCullConstants) / 4;
	static_assert(kGrassCullConstantWords == 16);

	// One thread per draw slot (bucket * 3 + tier). The colour mode also zeroes the counts.
	inline constexpr const char* kGrassArgsShader = "DrawcallLimitFix/Features/Grass/GrassArgsCS.hlsl";
	inline constexpr std::uint32_t kGrassArgsGroup = 64;
	inline constexpr std::uint32_t kArgsPhaseOne = 1, kArgsPhaseTwo = 2, kArgsColour = 3;
	struct GrassArgsConstants
	{
		std::uint32_t drawIndex = 0, bucketsIndex = 0, meshesIndex = 0, drawsIndex = 0;
		std::uint32_t mode = 0, drawSlots = 0, padding[2]{};
	};
	inline constexpr std::uint32_t kGrassArgsConstantWords = sizeof(GrassArgsConstants) / 4;
	static_assert(kGrassArgsConstantWords == 8);

	/** @brief Cull counts read back per run range, on the graph host's thread. */
	struct GrassCounts
	{
		Draws::RunRanges ranges{ Draws::kReadbackRanges };
		std::vector<std::shared_ptr<org::Buffer>> readback;  // by range
		std::vector<std::uint8_t> filled;                    // by range: its copy holds a frame's counts
		std::atomic<std::uint64_t> frames{ 0 }, live{ 0 }, beyond{ 0 }, outside{ 0 }, thinned{ 0 }, faded{ 0 }, tested{ 0 }, phaseOne{ 0 }, retests{ 0 }, phaseTwo{ 0 };
		std::atomic<std::uint64_t> phaseOneDraws{ 0 }, phaseTwoDraws{ 0 }, colourDraws{ 0 }, pbrDraws{ 0 };
	};

	/**
	 * @brief Grass's GPU tables (dclf-architecture.md, "Grass: the draws"). Render thread, except for the handles.
	 * Setup returns null without the programs or buffer view refs, and grass then stays native.
	 */
	struct GrassTables
	{
		Draws::Versioned instances, owners, frame, visible, shapes, buckets, meshes;
		std::array<org::services::FilledVersion, 5> filled;  // instances, owners, shapes, buckets, meshes
		std::shared_ptr<org::Buffer> draw, draws;
		RenderGraph::BufferRef drawRef;
		std::shared_ptr<const ComputeProgram> cull, args;
		std::atomic<bool> ready{ false };
		std::shared_ptr<GrassCounts> counts;
		float prevTimeBase = 0.0f;
		bool countsZeroed = false, owned = false;
		bool smallStart = false;  // CS_DCLF_TABLE_START=small: the tables start and stay small (no floors in Require)
		std::uint32_t bindingGeneration = ~0u;
		std::uint64_t relocationCursor = 0;  // GpuTextures::RelocatedViews
		std::vector<std::uint32_t> unbound;
		// Slot counts as of the last commit that fit. An outgrown commit draws these until growth is adopted.
		std::uint32_t sentInstanceSlots = 0, sentShapeSlots = 0, sentBucketSlots = 0;
		// Since the last report.
		std::uint64_t uploads = 0, shapesSent = 0, bucketsSent = 0, unboundBuckets = 0, overBuckets = 0, outgrown = 0;
		GrassInstances::DrawRow row{};
	};

	// Words posted with the revision's inputs, so the tables can grow to the mirror's slot counts.
	inline constexpr std::uint32_t kInstanceSlotsInput = 0, kShapeSlotsInput = 1, kBucketSlotsInput = 2, kMeshSlotsInput = 3, kInputWords = 4;

	/** @brief Null before the scene's tables are made, or without them. */
	GrassTables* TablesOf(const Draws::SceneBuffers& a_scene);
	std::shared_ptr<GrassTables> SharedTablesOf(const Draws::SceneBuffers& a_scene);
	Api::SceneResourcesId TablesId();
}
