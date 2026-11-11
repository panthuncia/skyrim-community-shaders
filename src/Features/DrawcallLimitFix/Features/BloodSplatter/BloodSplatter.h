#pragma once

// Blood splatter (CS_ORG_BLOOD_SPLATTER, docs/development/p5/blood-splatter.md): the engine's screen blood (BSBloodSplatterShader's
// splatter and flare passes, drawn into the back buffer after the ImageSpace chain) drawn by the render graph in the engine's place.
//
// The engine's CPU code runs unchanged. At the screen splatter accumulator's RenderBatches (FinishAccumulatingScreenSplatter, from
// Main::RenderScreenSplatter: BloodSplatterCapture.cpp) the family lets the batch run with its draw choke point latched: each pass goes
// through the engine's BeginPass, SetupTechnique and SetupGeometry, and DrawTriShape (the engine draw families' one detour,
// Core/Engine/Family) records the draw instead of drawing it. The batch's draws are then drawn by an ORG pass at the EngineDraws anchor,
// in the engine's order, with CS's BloodSplatter.hlsl built for ORG (BloodSplatterDraws.cpp).
//
// The family is the primary path for the screen blood (docs/development/p5/primary-path.md): its stages compile when CS loads the
// shader's descriptors, its programs are prebuilt from the manifest, and its client is made once at setup (the first main-thread tick
// the render graph runs, Family::AddSetup), so the first hit neither rebuilds the anchor's graph nor waits. What it cannot draw (its
// inherent refusals, docs/development/p5/blood-splatter.md "What stays the engine's") ends the batch's latched part: the latched draws
// run at that point and the engine draws the rest.

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
	// Engine::Offsets (Address Library ids) of the engine code the family hooks (docs/development/p5/blood-splatter.md, "RE").
	namespace Address
	{
		// FinishAccumulatingScreenSplatter's RenderBatches(accumulator, 0x5C006073, 0x5C006074, 0, -1) (SE 99956, AE 106601).
		inline constexpr Engine::Offset kSplatterBatch{ { 106601, 0x21 }, { 99956, 0x1D } };
	}

	/** @brief BloodSplatterOrgDraw in BloodSplatter.hlsl: a draw's latched values, 176 bytes. */
	struct DrawConstants
	{
		float worldViewProj[4][4];  // VS b2 c0-c3 (rows)
		float lightLoc[4];          // VS b2 c4
		float ctrl[4];              // VS b2 c5
		float alpha[4];             // PS b2 c0
		std::uint32_t textures[4];  // t0-t3
		std::uint32_t samplers[4];  // s0-s3
		std::uint32_t vertexAddress[2];
		std::uint32_t indexAddress[2];
		std::uint32_t vertexLayout[2];
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
	};
	static_assert(sizeof(DrawConstants) == 176, "BloodSplatterOrgDraw's layout");

	/** @brief One latched draw. */
	struct Draw
	{
		Family::ProgramKey key;
		std::shared_ptr<const void> program;  // the RasterProgram
		std::uint32_t indexCount = 0;
		Family::Viewport viewport;
		DrawConstants constants{};
	};

	/** @brief A batch's draws, as the anchor's pass records them. */
	struct Run
	{
		std::vector<Draw> draws;
		std::uint64_t revision = 0;
		Family::Owners owners;
	};

	// BloodSplatterFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	/** @brief Whether CS_ORG_BLOOD_SPLATTER=1 turned the family on (read at install). */
	bool Enabled();

	// BloodSplatterCapture.cpp: the engine hooks, from the feature's install.
	void InstallHooks();
	void PreflightHooks();

	// BloodSplatterDraws.cpp: the render graph's side (render thread).
	namespace BloodSplatterDraws
	{
		/** @brief Whether the anchor exists and the runtime is up; false for good once the client failed. */
		bool Available();
		/** @brief Reserves the EngineDraws anchor (the feature's install). */
		void Reserve();
		/**
		 * @brief The family's setup (Family::AddSetup, main thread): the stage precompile started, the manifest's programs queued, the client
		 * made and its build asked for. True once done (or refused for good, logged); false while the render graph has no host yet.
		 */
		bool Setup();
		/**
		 * @brief Whether a batch is taken: the client exists, imports the back buffer the engine binds now, and a run of the anchor now
		 * would draw (FrameAnchors::CanRun). False is logged once per reason; the engine draws the batch.
		 */
		bool CanTake();
		/** @brief The program for a key (Family::EngineDrawPrograms::Get): null when it cannot be built. */
		std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief Keeps a batch's imports and leases as long as a run's, when it ran nothing. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs the anchor with a_run's draws. True when the run was submitted; otherwise its draws were not drawn (logged). */
		bool Submit(std::shared_ptr<Run> a_run);
		/** @brief One status line. */
		std::string Status();
	}
}
