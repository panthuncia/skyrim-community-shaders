#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <ankerl/unordered_dense.h>

#include "RegistrationCensus.h"

namespace RE
{
	class BSBatchRenderer;
	class BSGeometry;
	class BSRenderPass;
	class NiAVObject;
	class NiCullingProcess;
}

namespace DCLF::Diagnostics
{

	/** @brief Engine points that carry no data. Render thread unless noted. */
	enum class EnginePoint : std::uint8_t
	{
		PrimaryCullInstalled,   // call sites patched at PostPostLoad, observers install their own engine hooks here
		CutSynced,              // the cut followed the frame's snapshot, before switch selections and admission
		FullFrustumDone,        // the fade roots' nodes stand until the list jobs
		ListJobsDone,           // the list jobs finished, the cull is done
		ListJobsMerged,         // ... the stand-in ran and the jobs' outputs were gathered
		SceneListsPublishing,   // this frame's scene lists are decided next
		EngineSceneListsBuilt,  // the engine built the lists, before DCLF's filter
		PortalWalkBuilt,        // right after Main::Update's walk, before the programs are encoded
		LightSelectionBegin,    // before the engine's CalculateActiveShadowCasterLights runs
		LightSelectionMade,     // ... after it and DCLF's selection
		SunAccumulateBegin,     // before the engine's, cascades captured from DCLF's views
		SunViewsUpdated,        // the sun's views made from the cameras UpdateCamera set up
		LightViewsFrame,        // first light of a new frame, when the last frame had local views
		FocusViewsFrame,        // first host of a new frame, when the last frame had focus hosts
	};

	/** @brief The frame's list processes, after the full-frustum cull. Render thread. */
	struct CutFrameBegin
	{
		RE::NiCullingProcess* const* processes;
		std::uint32_t count;
	};

	/** @brief The list jobs' outputs were reset for `count` processes. Render thread. */
	struct CutSlotsReset
	{
		std::uint32_t count;
	};

	/** @brief A list job reached an eligible entry, before the stand-in's verdict. Runs on the job's thread. */
	struct StandInEntry
	{
		int slot;
		std::uint32_t entry;
		const RE::NiAVObject* root;
	};

	/** @brief A list job's stand-in handed a member to the engine's registration. */
	struct StandInHandedOver
	{
		int slot;
		std::uint32_t entry;
		std::uint32_t member;
		const RE::BSGeometry* geometry;
	};

	/** @brief The engine builds its lists whole this frame. `roots` are what the filter would have left out, or null for no filter. */
	struct SceneListsWhole
	{
		std::shared_ptr<const void> hold;
		const ankerl::unordered_dense::set<const RE::NiAVObject*>* roots;
		bool keepable;
	};

	/** @brief Main thread at Main::Update's entry, after the visibility and fade writes. */
	struct UpdateEntry
	{
		bool running;
	};

	// Sun accumulation. Render thread unless noted.

	/** @brief This frame's exclusion was applied. `skipExcluded` is false when the cascades skip nothing. */
	struct SunExclusionApplied
	{
		bool skipExcluded;
	};

	/** @brief The sun's cascade bits after its Accumulate, or 0 where the frame's are reset. */
	struct SunBitsSet
	{
		std::uint32_t bits;
	};

	/** @brief A cascade of the sun's Accumulate with no DCLF view this frame. */
	struct SunCascadeMissing
	{
		std::uint32_t frame;
		std::uint32_t descriptor;
	};

	/** @brief An unclaimed caster the engine's cascade registration built passes for. */
	struct SunCasterRegistered
	{
		const RE::BSGeometry* geometry;
	};

	/** @brief A cascade cull (FUN_140e305c0) returned. Its process holds the cascade's planes. */
	struct SunCascadeCulled
	{
		const void* fullProcess;
		const RE::NiCullingProcess* process;
		std::uint32_t cascade;
	};

	// Point light culls.

	/** @brief This frame's exclusion was selected. `whole` means the culls skip nothing. Render thread. */
	struct LightCullsSelected
	{
		bool whole;
	};

	/** @brief A pass handed to a batch renderer, withheld or not. `source` is the insertion's hint, 0 for RegisterPass. Any registration thread. */
	struct PointLightPass
	{
		const RE::BSBatchRenderer* batch;
		const RE::BSRenderPass* pass;
		bool withheld;
		std::uint32_t source;
	};

	/**
	 * @brief On a frame the culls keep whole, a geometry's main registration with the bits DCLF would give it and the bits the
	 * engine's culls left. Any registration thread.
	 */
	struct PointLightBits
	{
		const RE::BSGeometry* geometry;
		std::uint32_t entry;
		std::uint32_t bits;
		const std::array<std::uint32_t, 3>& reach;
		std::uint32_t engine;
		bool first;
		std::uint32_t sphere;
	};

	// Shadow light views, render thread.

	/** @brief A pass into a covered focus view's renderer. Any registration thread. */
	struct FocusRegistration
	{
		std::uint32_t descriptor;
		const void* geometry;
	};

	// Registrations, on whatever thread the engine registers.

	/** @brief The hint 1 list insertion withheld a member's pass from the main views. */
	struct MemberListPassWithheld
	{
		const RE::BSRenderPass* pass;
	};

	/** @brief The geometry group 5 insertion withheld a member's pass from the main views. */
	struct MemberGroup5PassWithheld
	{
		const RE::BSRenderPass* pass;
	};

	/** @brief A registration hook passed a pass on to the engine's batch renderer. */
	struct PassKept
	{
		const RE::BSBatchRenderer* batch;
		const RE::BSRenderPass* pass;
		RegistrationCensus::Site site;
		bool face;
		std::uint8_t occlusionPhase;
	};
}
