#pragma once

// Tree wind (dclf-architecture.md, "Tree wind on the GPU"): GPU animation of the trees DCLF owns.
// The feature lists trees in the scene's tables, ports technique 12, seeds the trees at frame start, and clocks them on the GPU
// (TreeWindCS.hlsl) into wind rows read at t124. It also stands down the engine's tree manager for those trees.
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"  // the texture registers, the record flags

#include <cstddef>
#include <cstdint>
#include <vector>

namespace DCLF::Features::TreeWind
{
	namespace Address
	{
		inline constexpr Engine::Offset kTreeManager{ { 400330 }, { 514181 } };  // BSTreeManager*
		inline constexpr Engine::Offset kTreeFirstMultiBound{ { 105606 }, { 98955 } };  // FUN_14147d640(tree node): its object (+0x118), its first child's AsMultiBoundNode (vfunc 0x30), or null
		inline constexpr Engine::Offset kTreeWindFadeStart{ { 390974 }, { 513234 } };  // float: the tree wind fade start
		inline constexpr Engine::Offset kTreeWindFadeEnd{ { 390975 }, { 513235 } };  // float: the tree wind fade end
		inline constexpr Engine::Offset kTreeWindTimerScale{ { 207170 }, { 259742 } };  // float: the tree wind timer scale
		inline constexpr Engine::Offset kFrameSeconds{ { 410199 }, { 523660 } };  // float: the frame's seconds (GetSecondsPassed)
	}

	// TreeParams and WindTimers are per object, not the pipeline template's, which a zeroed per-object copy would clobber.
	inline constexpr std::uint32_t kTechniqueTreeAnim = 12;
	inline constexpr std::uint32_t kObjectTreeAnim = 1u << 6;

	/**
	 * @brief A tree's static row, from which TreeWindCS clocks a listed BSTreeNode.
	 * It replaces the engine's tree manager (FUN_1404381e0): timer +0x164, camera distance +0x158 within range manager +0x80, and
	 * gust amplitude +0x15C = 0.25 * sum over k in {1,3,5,7} of sin(k * pi * manager+0x78 * timer) * model scale (+0xB0).
	 */
	struct TreeStatic
	{
		float position[3]{};       // the node's world translate (+0xA0)
		float leafFrequency = 1.0f;  // +0x160
		float modelAmplitude = 0.0f; // the model's +0xB0
		float timer = 0.0f;          // the node's clock when listed: +0x164, +0x168, +0x15C
		float previousTimer = 0.0f;
		float amplitude = 1.0f;
		std::uint32_t generation = 0;  // new for every listing: the clock row takes the values above again
		std::uint32_t animated = 0;    // a model is attached (the manager advances only such a node)
		// Which of the slot's two seed rows holds this generation's values.
		std::uint32_t seedOdd = 0;
		std::uint32_t padding = 0;
	};
	static_assert(sizeof(TreeStatic) == 48);

	// A member drawing under a tree node. kNodelessTree means the engine's defaults.
	struct TreeObject
	{
		std::uint32_t object, tree;
	};
	// A member whose property names no listed tree node gets the nodeless defaults.
	inline constexpr std::uint32_t kNoTree = ~0u;
	inline constexpr std::uint32_t kNodelessTree = ~0u - 1;
	// Carried in DCLFWindTimers.zw: the tree slot, then its generation.
	inline constexpr std::uint32_t kTreeRecordWord = 10;

	// The shading feature block's first word in the record and in the shading row. TreeParams in words 0-3, WindTimers in 4-7.
	inline constexpr std::uint32_t kRecordBlockWord = 4;
	inline constexpr std::uint32_t kShadingBlockWord = 16;
	inline constexpr std::uint32_t kBlockTreeParams = 0;
	inline constexpr std::uint32_t kBlockWindTimers = 4;
	inline constexpr std::uint32_t kBlockWords = 6;
	inline constexpr std::uint32_t kTreeParamsVariable = 4;
	inline constexpr std::uint32_t kWindTimersVariable = 5;

	/** @brief t124: the trees' wind, read by the vertex stage. It alternates by frame. */
	inline constexpr std::uint32_t kWindRegister = kTextureRegisters - 4;

	constexpr const char* kShader = "DrawcallLimitFix/Features/TreeWind/TreeWindCS.hlsl";
	// Buffer indices only. Per-frame values are in the frame row.
	struct TreeWindConstants
	{
		std::uint32_t treesIndex = 0, clocksIndex = 0, frameIndex = 0, padding0 = 0;
		std::uint32_t windIndices[2]{};
		std::uint32_t padding1[2]{};
	};
	constexpr std::uint32_t kTreeWindEntryRows = 3;
	static_assert(sizeof(TreeWindConstants) == 32);
	constexpr std::uint32_t kTreeWindConstantWords = sizeof(TreeWindConstants) / 4;
	constexpr std::uint32_t kTreeWindGroup = 64;
	constexpr std::uint32_t kInitialTrees = 1024;

	/** @brief A tree's clock on the GPU, reseeded on a new generation. Must match TreeWindCS.hlsl's TreeClock. */
	struct TreeClock
	{
		float timer, previousTimer, amplitude;
		std::uint32_t generation, frame;
		std::uint32_t padding[3];
	};
	static_assert(sizeof(TreeClock) == 32);

	/** @brief The frame's inputs to the tree clocks. */
	struct TreeWindFrame
	{
		float deltaTime = 0.0f;  // the frame's seconds (GetSecondsPassed's global)
		float camera[3]{};       // the main camera's world position
		float windSpeed = 0.0f;  // the tree manager's +0x78
		float maxDistance2 = 0.0f;  // its +0x80
		float windMagnitude = 0.0f;
		float fadeStart = 0.0f, fadeEnd = 0.0f;
		float timerScale = 0.0f;
	};

	struct TreeWindFrameRow
	{
		std::uint32_t treeCount = 0, windSlot = 0, frame = 0;  // windSlot: the wind buffer this render writes (RenderSerial's parity)
		std::uint32_t treeSeedsIndex = 0;  // the seeds pool's backing the frame reads (SeedsIndex)
		TreeWindFrame inputs{};
		std::uint32_t padding2[2]{};
	};
	static_assert(sizeof(TreeWindFrameRow) == 64);

	Api::Feature Feature();
	bool ListAll();
	void RegisterTables(Api::InstallContext& a_context);
	/** @brief The component's registration. */
	Api::CoordinatorComponentId ComponentId();
	void RegisterObjects(Api::InstallContext& a_context);
	void RegisterSeeds(Api::InstallContext& a_context);
	/** @brief The seeds' backing the current frame reads, 0 before the first frame. Render thread. */
	std::uint32_t SeedsIndex();
	void RegisterScene(Api::InstallContext& a_context);
	void RegisterPass(Api::InstallContext& a_context);
	void RegisterAnimation(Api::InstallContext& a_context);
	void RegisterReport(Api::InstallContext& a_context);
}
