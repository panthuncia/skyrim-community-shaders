#pragma once

// Blood splatter (CS_ORG_BLOOD_SPLATTER, docs/development/p5/blood-splatter.md): the engine's screen blood (BSBloodSplatterShader's
// splatter and flare passes, drawn into the back buffer after the ImageSpace chain) drawn by the render graph in the engine's place.
//
// The engine's CPU code runs unchanged. At the screen splatter accumulator's RenderBatches (FinishAccumulatingScreenSplatter, from
// Main::RenderScreenSplatter: BloodSplatterCapture.cpp) the family lets the batch run with its draw choke point latched: each pass goes
// through the engine's BeginPass, SetupTechnique and SetupGeometry, and DrawTriShape records the draw instead of drawing it. The batch's
// draws are then drawn by an ORG pass at the EngineDraws anchor, in the engine's order, with CS's BloodSplatter.hlsl built for ORG
// (BloodSplatterDraws.cpp).
//
// The client is made once, at the first frame the render graph runs (the frame-job wait, before any blood is drawn), so the first hit
// does not rebuild the anchor's graph. Anything the family cannot draw stays the engine's.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

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
		// BSGraphics::Renderer::DrawTriShape (R1).
		inline constexpr Engine::Offset kDrawTriShape{ { 77263 }, { 75477 } };
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

	/** @brief What a draw's pipeline is made of. */
	struct PipelineKey
	{
		std::uint32_t descriptor = 0;  // 0 splatter, 1 flare
		const void* blend = nullptr;
		const void* raster = nullptr;
		std::uint32_t colourFormat = 0;
		bool operator==(const PipelineKey&) const = default;
	};

	/** @brief One latched draw. */
	struct Draw
	{
		PipelineKey key;
		std::shared_ptr<const void> program;  // the RasterProgram
		std::uint32_t indexCount = 0;
		std::uint32_t viewport[4]{};
		DrawConstants constants{};
	};

	/** @brief A batch's draws, as the anchor's pass records them. */
	struct Run
	{
		std::vector<Draw> draws;
		std::uint64_t revision = 0;
		std::vector<std::shared_ptr<const void>> owners;
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
		/** @brief Makes the client once (the first frame the graph runs). Main thread. */
		void EnsureClient();
		/** @brief Whether the client exists, imports the back buffer the engine binds now, and was prepared once. */
		bool Ready();
		/** @brief The program for a key, compiled the first time; null when it cannot be built. a_shader: the engine's shader. */
		std::shared_ptr<const void> Program(const PipelineKey& a_key, const void* a_shader);
		/** @brief Runs the anchor with a_run's draws. */
		void Submit(std::shared_ptr<Run> a_run);
		/** @brief One status line. */
		std::string Status();
	}
}
