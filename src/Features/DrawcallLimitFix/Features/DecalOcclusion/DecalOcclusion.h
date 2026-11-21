#pragma once

#include "RenderGraph/BufferRef.h"

// Decal occlusion, in graph mode: the render graph draws the engine's decal occlusion test and answers its query.
// See docs/development/p5/occlusion-queries.md.
//
// Before Main::Draw the engine tests each decal waiting to be placed (SE 35563, AE 36562). It draws the decal's target
// into kDECAL_OCCLUSION inside an occlusion query, and a later frame drops the decal if the count is 0.
// The engine's CPU code runs unchanged. Its query calls go nowhere, and each shape's DrawTriShape is latched instead of drawn.
// The graph then draws the latched shapes and answers the engine's query slot with the summed count.
// Only depth matters, so alpha-tested targets count as solid.
//
// A test the graph cannot take is skipped and answers 1, so the decal is placed. A pass drawn through something other
// than DrawTriShape, such as a dynamic or particle shape, still reaches D3D11.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DCLF::Features::DecalOcclusion
{
	/** @brief A test with more draws than this is skipped. */
	inline constexpr std::uint32_t kMaxDraws = 8;

	/** @brief Above 0, so the engine places the decal. */
	inline constexpr std::uint32_t kSkipAnswer = 1;

	/** @brief Mirrors DecalOcclusionDraw in RenderGraph/DecalOcclusion.hlsl. */
	struct DrawConstants
	{
		float clipRows[16];  // View-projection times world, as rows. clip = row . position.
		RenderGraph::BufferRef vertexRef;
		RenderGraph::BufferRef indexRef;
		std::uint32_t vertexLayout[2];
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
	};
	static_assert(sizeof(DrawConstants) == 96, "DecalOcclusionDraw's layout");
	static_assert(offsetof(DrawConstants, vertexRef) == 64 && offsetof(DrawConstants, vertexStride) == 88, "DecalOcclusionDraw's rows");

	struct Draw
	{
		std::uint32_t indexCount = 0;
		Family::Viewport viewport;
		DrawConstants constants{};
	};

	struct Run
	{
		std::vector<Draw> draws;
		bool clear = false;       // The decal's +0x31 asks for kDECAL_OCCLUSION to be cleared first.
		std::int32_t query = -1;  // Engine query ring slot.
		std::uint64_t revision = 0;
		Family::Owners owners;  // Leases on the geometry.

		/** @brief What the recording depends on: everything but the latched values and counts (DecalOcclusionDraws.cpp). */
		std::uint64_t Hash() const;
	};

	Api::Feature Feature();

	/** @brief Graph mode, read at install. */
	bool Enabled();

	// No preflight needed. The detour always calls the engine's test, so it chains with other plugins.
	void InstallHooks();

	// Render thread.
	namespace DecalDraws
	{
		/** @brief Stays false for good once the client has failed. */
		bool Available();
		/** @brief Main thread. Returns true once done or refused for good, false while the graph has no host. */
		bool Setup();
		bool HasClient();
		/** @brief True while the start stage's step holds the client. */
		bool CanTake();
		/** @brief Holds a test's leases as long as a run would, when the test ran nothing. */
		void Keep(Family::Owners&& a_owners);
		bool Submit(std::shared_ptr<Run> a_run);
		std::string Status();
	}
}
