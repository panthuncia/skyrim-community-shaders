#pragma once

// The menu scenes' engine draw family (docs/development/p5/menus.md, "The menu scenes as an engine draw family"): the engine's draws
// of a menu scene latched and drawn by an ORG pass in the engine's place, as the water, the sky and the shadow masks are
// (Core/Engine/Family). The engine's CPU code runs unchanged: it culls, registers and sets up every pass, and DrawTriShape latches
// each draw instead of drawing it. The batch's draws are then drawn by the EngineDraws anchor, in the engine's order, with CS's
// shader built for ORG and the pipeline the engine's state objects describe.
//
// The local map (LocalMapRenderer::Render, SE 16094, AE 16335) has two batches:
// - its land and objects, the first SubmitAccumulator: in the local map mode (BSShaderManager::State +0x219) a Lighting property's
//   GetRenderPasses_LocalMap (SE 0x1412c7cd0) makes a BSUtilityShader RENDER_BASE_TEXTURE pass, into kLOCAL_MAP_SWAP with kMAIN's
//   depth. Landscape (kLandscape) keeps its Lighting passes: the family has no Lighting program yet, so a Lighting pass ends the
//   latched part of the batch (an inherent refusal) and the engine draws the rest;
// - its fog of war, the second, in render mode 0x13 on the fog root (+0x30358): BSUtilityShader LOCALMAP_FOGOFWAR passes into the
//   same targets, depth mode 8.
//
// Utility.hlsl's ORG build draws both (UTILITY_ORG_SCENE). Its stages compile when CS loads a Utility descriptor of those techniques
// (StagesOf) and its programs are prebuilt from the manifest, as every family's (Core/Engine/Family/EngineDrawProgram.h).

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace DCLF::Features::MenuViews::Scenes
{
	/** @brief The batch a run draws. */
	enum class View : std::uint8_t
	{
		LocalMap,     // the local map's land and objects (LocalMapRenderer::Render's first SubmitAccumulator)
		LocalMapFog,  // its fog of war (the second, render mode 0x13)
		Count
	};

	/** @brief "local-map" or "local-map-fog", for logs and client ids. */
	const char* NameOf(View a_view);

	/** @brief RE::RENDER_TARGET::kLOCAL_MAP_SWAP: the colour target both batches write. */
	inline constexpr std::uint32_t kLocalMapSwapTarget = 17;

	/** @brief The texture slots a draw's latch holds (Utility.hlsl's t0-t7 and s0-s7). */
	inline constexpr std::uint32_t kSlots = 8;

	using Kind = GpuTextures::ViewKind;

	/** @brief Each slot's declared dimension in Utility.hlsl: the shadow maps (t3, t4) and the focus shadow maps (t6) are arrays. */
	constexpr Kind SlotKind(std::uint32_t a_slot)
	{
		return a_slot == 3 || a_slot == 4 || a_slot == 6 ? Kind::Texture2DArray : Kind::Texture2D;
	}

	/**
	 * @brief Whether UTILITY_ORG_SCENE reads slot a_slot: the base texture (t0) and the grayscale palette (t7, GRAYSCALE_TO_ALPHA).
	 * Every other slot is latched as a null view, whatever is bound there.
	 */
	constexpr bool SlotRead(std::uint32_t a_slot) { return a_slot == 0 || a_slot == 7; }

	/** @brief The rows of each constant group the latch takes (Utility.hlsl's packoffsets). */
	inline constexpr std::uint32_t kVSTechniqueRows = 1;
	inline constexpr std::uint32_t kVSMaterialRows = 1;
	inline constexpr std::uint32_t kVSGeometryRows = 8;
	inline constexpr std::uint32_t kPSMaterialRows = 2;
	inline constexpr std::uint32_t kPSGeometryRows = 4;

	/** @brief UtilityOrgSceneDraw in Utility.hlsl: a draw's latched values, 368 bytes. */
	struct SceneDrawConstants
	{
		float vsTechnique[kVSTechniqueRows][4];
		float vsMaterial[kVSMaterialRows][4];
		float vsGeometry[kVSGeometryRows][4];
		float psMaterial[kPSMaterialRows][4];
		float psGeometry[kPSGeometryRows][4];
		float alphaTestRef;
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
		std::uint32_t padding;
		std::uint32_t textures[kSlots];
		std::uint32_t samplers[kSlots];
		std::uint32_t vertexAddress[2];
		std::uint32_t indexAddress[2];
		std::uint32_t vertexLayout[2];
		std::uint32_t padding2[2];
	};
	static_assert(sizeof(SceneDrawConstants) == 368 && sizeof(SceneDrawConstants) % 16 == 0, "UtilityOrgSceneDraw's layout");

	/** @brief The most draws one batch latches: past it the rest of the batch is the engine's. */
	inline constexpr std::uint32_t kMaxDraws = 2048;

	/** @brief One latched draw. */
	struct Draw
	{
		Family::ProgramKey key;
		std::shared_ptr<const void> program;  // the RasterProgram (MenuScenesDraws.cpp)
		std::uint32_t stencilRef = 0;
		std::uint32_t indexCount = 0;
		Family::Viewport viewport;
		SceneDrawConstants constants{};
	};

	/** @brief A batch's draws, as the anchor's pass records them. */
	struct Run
	{
		View view = View::LocalMap;
		std::vector<Draw> draws;
		std::uint64_t revision = 0;  // what the recording depends on (MenuScenesDraws.cpp, RevisionOf)
		Family::Owners owners;       // the textures' imports and the buffers' leases
	};

	/** @brief CS_ORG_MAP=1 (MenuViews::MapEnabled): the local map's batches are the family's. */
	bool Enabled();

	// MenuScenesCapture.cpp: the engine hooks (the local map's two SubmitAccumulator calls), from the feature's install.
	void InstallHooks();
	void PreflightHooks();

	// MenuScenesDraws.cpp: the render graph's side (render thread).
	namespace SceneDraws
	{
		/** @brief Whether the anchor exists and the runtime is up; false for good once a client failed. */
		bool Available();
		/** @brief Reserves the EngineDraws anchor (the feature's install, before the render graph initializes). */
		void Reserve();
		/** @brief The family's setup (Family::AddSetup, main thread): the stages, the manifest's programs, both clients. True once done. */
		bool Setup();
		/** @brief Whether a_view's batch is taken now: its client exists, its targets are still its imports, and its anchor can run. */
		bool CanTake(View a_view);
		/** @brief How a_view's client reads a_id, the published id a bound slot exports. NotListed when the view has no client. */
		Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index);
		/** @brief The program for a key: null when it cannot be built. */
		std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief The formats of a_view's targets as imported (rhi::Format values; 0 without a client). */
		std::uint32_t ColourFormat(View a_view);
		std::uint32_t DepthFormat(View a_view);
		/** @brief Keeps a batch's imports and leases as long as a run's when it ran nothing. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs a_run's view at the anchor with its draws. True when the run was submitted. */
		bool Submit(std::shared_ptr<Run> a_run);
		/** @brief One status line. */
		std::string Status();
	}
}
