#pragma once

#include "RenderGraph/BufferRef.h"

// Blood splatter: the engine's screen blood, drawn by the render graph in the engine's place.
// The engine's batch runs unchanged, but DrawTriShape records each draw instead of drawing it.
// An ORG pass at the EngineDraws anchor then draws them in the engine's order with BloodSplatter.hlsl.
// Programs are prebuilt and the client is made at setup, so the first hit never waits or rebuilds.
// A draw the family cannot take ends the latched part, and the engine draws the rest of the batch.
// See docs/development/engine/blood-splatter.md.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DCLF::Features::BloodSplatter
{
	namespace Address
	{
		// FinishAccumulatingScreenSplatter's RenderBatches(accumulator, 0x5C006073, 0x5C006074, 0, -1).
		inline constexpr Engine::Offset kSplatterBatch{ { 106601, 0x21 }, { 99956, 0x1D } };
	}

	/** @brief Must match BloodSplatterOrgDraw in BloodSplatter.hlsl. */
	struct DrawConstants
	{
		float worldViewProj[4][4];  // VS b2 c0-c3 (rows)
		float lightLoc[4];          // VS b2 c4
		float ctrl[4];              // VS b2 c5
		float alpha[4];             // PS b2 c0
		std::uint32_t textures[4];  // t0-t3
		std::uint32_t samplers[4];  // s0-s3
		RenderGraph::BufferRef vertexRef;  // the vertex stage pulls vertices through this
		RenderGraph::BufferRef indexRef;
		std::uint32_t vertexLayout[2];
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
	};
	static_assert(sizeof(DrawConstants) == 176, "BloodSplatterOrgDraw's layout");

	/** @brief One latched draw. */
	struct Draw
	{
		Family::ProgramKey key;
		std::shared_ptr<const void> program;  // a RasterProgram
		std::uint32_t indexCount = 0;
		Family::Viewport viewport;
		DrawConstants constants{};
	};

	struct Run
	{
		std::vector<Draw> draws;
		std::uint64_t revision = 0;
		Family::Owners owners;

		/** @brief What the recording depends on: the draws' programs and viewports (BloodSplatterDraws.cpp). */
		std::uint64_t Hash() const;
	};

	Api::Feature Feature();

	/** @brief Read at install. */
	bool Enabled();

	void InstallHooks();
	void PreflightHooks();

	// Render thread, except Setup.
	namespace BloodSplatterDraws
	{
		/** @brief False for good once the client failed. */
		bool Available();
		/**
		 * @brief Main thread. Starts the stage precompile, queues the programs and makes the client.
		 * True once done or refused for good. False while the render graph has no host yet.
		 */
		bool Setup();
		/** @brief False means the engine draws the batch. Logged once per reason. */
		bool CanTake();
		/** @brief Null when the program cannot be built. */
		std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief Keeps a batch's imports and leases alive as long as a run would, when it ran nothing. */
		void Keep(Family::Owners&& a_owners);
		/** @brief False means the draws were not drawn. Logged. */
		bool Submit(std::shared_ptr<Run> a_run);
		std::string Status();
	}
}
