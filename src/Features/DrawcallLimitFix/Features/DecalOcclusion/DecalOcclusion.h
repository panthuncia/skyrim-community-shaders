#pragma once

// Decal occlusion (CS_ORG_OCCLUSION_QUERIES, docs/development/p5/occlusion-queries.md): the engine's decal occlusion test drawn by the
// render graph, with an ORG occlusion query that answers the engine's GetQueryData.
//
// At the frame's start (Main's frame, before Main::Draw) the engine tests each decal that waits to be placed (BGSDecalManager, state
// 3). The test (SE 35563, AE 36562) points a camera along the decal, acquires a query from the engine's ring, begins it, culls the
// decal's target into the decal manager's accumulator with render mode 0x1B (a BSUtilityShader silhouette pass for each shape), draws
// it into kDECAL_OCCLUSION with no colour target (SubmitAccumulator), and ends the query. Its read (SE 29262, AE 30117) polls the count
// in a later frame: a count of 0 drops the decal, any other count places it.
//
// The engine's CPU code runs unchanged. The family brackets the test's query (RenderGraph/Engine/Queries): its BeginQuery and EndQuery
// go nowhere. Each shape's draw reaches DrawTriShape (the engine draw families' one detour, Core/Engine/Family), which latches it
// instead of drawing it: the geometry, and the decal camera's view-projection times the shape's world (DecalOcclusionCapture.cpp). The
// DecalQueries anchor's pass then draws the positions into kDECAL_OCCLUSION, each draw counted by a query of the run's range, and the
// slot answers their sum (DecalOcclusionDraws.cpp, RenderGraph/DecalOcclusion.hlsl). Only the depth matters: no pixel stage, no alpha
// test (an alpha-tested target counts as solid).
//
// Nothing of the test reaches D3D11. A test the graph cannot take is skipped, logged once per reason, and its query answers 1: the
// decal is placed as a visible one. The exception is a pass the dispatcher draws through another function than DrawTriShape (a
// dynamic or particle shape): that draw still reaches D3D11, logged once.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DCLF::Features::DecalOcclusion
{
	/** @brief The most draws one test counts (queries a run). A test with more is skipped. */
	inline constexpr std::uint32_t kMaxDraws = 16;

	/** @brief What a skipped test's query answers: a count above 0, so the engine places the decal. */
	inline constexpr std::uint32_t kSkipAnswer = 1;

	/** @brief DecalOcclusionDraw in RenderGraph/DecalOcclusion.hlsl: a draw's latched values, 96 bytes. */
	struct DrawConstants
	{
		float clipRows[16];  // the decal camera's view-projection times the shape's world, as rows (clip = row . position)
		std::uint32_t vertexAddress[2];
		std::uint32_t indexAddress[2];
		std::uint32_t vertexLayout[2];
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
	};
	static_assert(sizeof(DrawConstants) == 96, "DecalOcclusionDraw's layout");
	static_assert(offsetof(DrawConstants, vertexAddress) == 64 && offsetof(DrawConstants, vertexStride) == 88, "DecalOcclusionDraw's rows");

	/** @brief One latched draw. */
	struct Draw
	{
		std::uint32_t indexCount = 0;
		Family::Viewport viewport;
		DrawConstants constants{};
	};

	/** @brief One test's draws, as the anchor's pass records them. */
	struct Run
	{
		std::vector<Draw> draws;
		bool clear = false;       // the decal asks for kDECAL_OCCLUSION cleared first (its +0x31)
		std::int32_t query = -1;  // the engine ring slot the run's draws count
		std::uint64_t revision = 0;
		Family::Owners owners;  // the geometry's leases
	};

	// DecalOcclusionFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	/** @brief Whether the family is on: CS_ORG_OCCLUSION_QUERIES=1 (read at install). */
	bool Enabled();

	// DecalOcclusionCapture.cpp: the engine hooks, from the feature's install. The test's detour always calls the engine's test, so it
	// chains with another plugin's: no preflight.
	void InstallHooks();

	// DecalOcclusionDraws.cpp: the render graph's side (render thread).
	namespace DecalDraws
	{
		/** @brief Reserves the DecalQueries anchor (the feature's install, before the render graph initializes). */
		void Reserve();
		/** @brief Whether the anchor exists and the runtime is up; false for good once the client failed. */
		bool Available();
		/**
		 * @brief The family's setup (Family::AddSetup, main thread): the program built for kDECAL_OCCLUSION's format and the client made.
		 * True once done (or refused for good, logged); false while the graph has no host.
		 */
		bool Setup();
		/** @brief Whether the client was made at setup and has not failed: the graph answers the tests' queries. */
		bool HasClient();
		/** @brief Whether a run of the anchor now would draw (FrameAnchors::CanRun). False is logged once. */
		bool CanTake();
		/** @brief Keeps a test's leases as long as a run's when it ran nothing. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs the anchor with a_run's draws. True when the run was submitted. */
		bool Submit(std::shared_ptr<Run> a_run);
		/** @brief One status line. */
		std::string Status();
	}
}
