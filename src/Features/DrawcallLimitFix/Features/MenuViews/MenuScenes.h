#pragma once

// The menu scenes' engine draw family (docs/development/p5/menus.md, "The menu scenes as an engine draw family"): the engine's draws
// of a menu scene latched and drawn by an ORG pass in the engine's place, as the water, the sky and the shadow masks are
// (Core/Engine/Family). The engine's CPU code runs unchanged: it culls, registers and sets up every pass, and DrawTriShape latches
// each draw instead of drawing it. A batch's draws are then drawn by the EngineDraws anchor, in the engine's order, with CS's shader
// built for ORG and the pipeline the engine's state objects describe.
//
// The batches (each one engine call; a refusal ends the latched part of its batch only):
// - the local map (LocalMapRenderer::Render, SE 16094, AE 16335): its land and objects (the first SubmitAccumulator: in the local
//   map mode a Lighting property's GetRenderPasses_LocalMap, SE 0x1412c7cd0, makes a BSUtilityShader RENDER_BASE_TEXTURE pass;
//   landscape keeps its Lighting passes) and its fog of war (the second, render mode 0x13: LOCALMAP_FOGOFWAR), into kLOCAL_MAP_SWAP
//   with kMAIN's depth;
// - a menu model (UI3DSceneManager::Render, SE 51855, AE 52727): its depth pass (StartAccumulating of the depth accumulator, with the
//   render's keepDepth: Utility RENDER_DEPTH into kMAIN's depth) and its colour pass (SubmitAccumulator of the colour accumulator:
//   Lighting and Effect into kMAIN or the framebuffer, kMOTION_VECTOR and the TAA mask). Its shadow masks are the shadow mask
//   family's batch, between the two.
//
// Programs: Utility.hlsl's ORG build (UTILITY_ORG_SCENE: the base texture, the fog of war, depth and the shadow maps, skinned or
// not), Lighting.hlsl's (LIGHTING_ORG) and Effect.hlsl's (EFFECT_ORG), both with the world's CS features off. A draw of a shader
// with no ORG build (tree LOD), a particle draw or one the family refuses ends its batch's latched part; the engine draws the rest.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace DCLF::Features::MenuViews::Scenes
{
	/** @brief The batch a run draws: each a client of its own. */
	enum class View : std::uint8_t
	{
		LocalMap,     // the local map's land and objects (LocalMapRenderer::Render's first SubmitAccumulator)
		LocalMapFog,  // its fog of war (the second, render mode 0x13)
		MenuDepth,    // a menu model's depth pass (UI3DSceneManager::Render's StartAccumulating, with keepDepth)
		MenuColour,   // its colour pass (UI3DSceneManager::Render's SubmitAccumulator)
		Count
	};

	/** @brief "local-map", "local-map-fog", "menu-depth", "menu-colour", for logs and client ids. */
	const char* NameOf(View a_view);

	/** @brief RE::RENDER_TARGET values the batches draw into. */
	inline constexpr std::int32_t kMainTarget = 1;               // kMAIN
	inline constexpr std::int32_t kNormalTaaMaskTarget = 4;      // kNORMAL_TAAMASK_SSRMASK
	inline constexpr std::int32_t kNormalTaaMaskSwapTarget = 5;  // kNORMAL_TAAMASK_SSRMASK_SWAP
	inline constexpr std::int32_t kMotionVectorTarget = 7;       // kMOTION_VECTOR
	inline constexpr std::int32_t kLocalMapSwapTarget = 17;      // kLOCAL_MAP_SWAP

	/** @brief The colour targets a view's client imports (a draw's targets are looked up in it); empty: none. */
	std::span<const std::int32_t> TargetsOf(View a_view);

	/** @brief The most colour targets a draw writes (the menu colour pass: RT0, the motion vectors, the TAA mask). */
	inline constexpr std::uint32_t kMaxColours = 3;

	/** @brief The texture slots a Utility draw's latch holds (Utility.hlsl's t0-t7 and s0-s7). */
	inline constexpr std::uint32_t kUtilitySlots = 8;

	using Kind = GpuTextures::ViewKind;

	/** @brief Each Utility slot's declared dimension: the shadow maps (t3, t4) and the focus shadow maps (t6) are arrays. */
	constexpr Kind UtilitySlotKind(std::uint32_t a_slot)
	{
		return a_slot == 3 || a_slot == 4 || a_slot == 6 ? Kind::Texture2DArray : Kind::Texture2D;
	}

	/**
	 * @brief Whether UTILITY_ORG_SCENE reads slot a_slot: the base texture (t0) and the grayscale palette (t7, GRAYSCALE_TO_ALPHA,
	 * GRAYSCALE_MASK). Every other slot is latched as a null view, whatever is bound there.
	 */
	constexpr bool UtilitySlotRead(std::uint32_t a_slot) { return a_slot == 0 || a_slot == 7; }

	/** @brief The rows of each constant group the Utility latch takes (Utility.hlsl's packoffsets). */
	inline constexpr std::uint32_t kVSTechniqueRows = 2;
	inline constexpr std::uint32_t kVSMaterialRows = 1;
	inline constexpr std::uint32_t kVSGeometryRows = 8;
	inline constexpr std::uint32_t kPSMaterialRows = 2;
	inline constexpr std::uint32_t kPSGeometryRows = 4;

	/** @brief UtilityOrgSceneDraw in Utility.hlsl: a Utility draw's latched values, 384 bytes. */
	struct UtilitySceneConstants
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
		std::uint32_t textures[kUtilitySlots];
		std::uint32_t samplers[kUtilitySlots];
		std::uint32_t vertexAddress[2];
		std::uint32_t indexAddress[2];
		std::uint32_t vertexLayout[2];
		std::uint32_t padding2[2];
	};
	static_assert(sizeof(UtilitySceneConstants) == 384 && sizeof(UtilitySceneConstants) % 16 == 0, "UtilityOrgSceneDraw's layout");

	/** @brief The rows of each constant group the Lighting latch takes (Lighting.hlsl's packoffsets). */
	inline constexpr std::uint32_t kLightingVSTechniqueRows = 4;
	inline constexpr std::uint32_t kLightingVSMaterialRows = 3;
	inline constexpr std::uint32_t kLightingVSGeometryRows = 15;
	inline constexpr std::uint32_t kLightingPSTechniqueRows = 3;
	inline constexpr std::uint32_t kLightingPSMaterialRows = 18;
	inline constexpr std::uint32_t kLightingPSGeometryRows = 30;
	/** @brief The texture and sampler slots a Lighting draw's latch holds (t0-t15, s0-s15). */
	inline constexpr std::uint32_t kLightingSlots = 16;

	/** @brief LightingOrgDraw in Lighting.hlsl: a Lighting draw's latched values, 1376 bytes. */
	struct LightingSceneConstants
	{
		float vsTechnique[kLightingVSTechniqueRows][4];
		float vsMaterial[kLightingVSMaterialRows][4];
		float vsGeometry[kLightingVSGeometryRows][4];
		float psTechnique[kLightingPSTechniqueRows][4];
		float psMaterial[kLightingPSMaterialRows][4];
		float psGeometry[kLightingPSGeometryRows][4];
		std::uint32_t descriptors[4];  // CS's permutation block: vertex, pixel, extra and extra feature descriptors
		float effectRadius;
		std::uint32_t baseTextureIsWorking;
		std::uint32_t renderToUI;
		float alphaTestRef;
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
		std::uint32_t nullTexture;
		std::uint32_t padding;
		std::uint32_t textures[kLightingSlots];
		std::uint32_t samplers[kLightingSlots];
		std::uint32_t vertexAddress[2];
		std::uint32_t indexAddress[2];
		std::uint32_t vertexLayout[2];
		std::uint32_t padding2[2];
	};
	static_assert(sizeof(LightingSceneConstants) == 1376 && sizeof(LightingSceneConstants) % 16 == 0, "LightingOrgDraw's layout");

	/** @brief The rows of each constant group the Effect latch takes (Effect.hlsl's packoffsets), and its slots (t0-t7, s0-s7). */
	inline constexpr std::uint32_t kEffectVSTechniqueRows = 3;
	inline constexpr std::uint32_t kEffectVSMaterialRows = 3;
	inline constexpr std::uint32_t kEffectVSGeometryRows = 15;
	inline constexpr std::uint32_t kEffectPSTechniqueRows = 2;
	inline constexpr std::uint32_t kEffectPSMaterialRows = 3;
	inline constexpr std::uint32_t kEffectPSGeometryRows = 12;
	inline constexpr std::uint32_t kEffectSlots = 8;
	/** @brief The slots Effect.hlsl declares (t0-t4, s0-s4); the rest are latched as null views. */
	inline constexpr std::uint32_t kEffectSlotsRead = 5;

	/** @brief EffectOrgDraw in Effect.hlsl: an Effect draw's latched values, 752 bytes. */
	struct EffectSceneConstants
	{
		float vsTechnique[kEffectVSTechniqueRows][4];
		float vsMaterial[kEffectVSMaterialRows][4];
		float vsGeometry[kEffectVSGeometryRows][4];
		float psTechnique[kEffectPSTechniqueRows][4];
		float psMaterial[kEffectPSMaterialRows][4];
		float psGeometry[kEffectPSGeometryRows][4];
		std::uint32_t descriptors[4];
		float effectRadius;
		std::uint32_t baseTextureIsWorking;
		std::uint32_t renderToUI;
		float alphaTestRef;
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
		std::uint32_t nullTexture;
		std::uint32_t padding;
		std::uint32_t textures[kEffectSlots];
		std::uint32_t samplers[kEffectSlots];
		std::uint32_t vertexAddress[2];
		std::uint32_t indexAddress[2];
		std::uint32_t vertexLayout[2];
		std::uint32_t padding2[2];
	};
	static_assert(sizeof(EffectSceneConstants) == 752 && sizeof(EffectSceneConstants) % 16 == 0, "EffectOrgDraw's layout");

	/** @brief FrameBuffer (b12) as a draw had it bound, c0-c44 (OrgSceneFrameRows in the ORG builds), latched with the draw. */
	inline constexpr std::uint32_t kFrameRows = 45;
	inline constexpr std::uint32_t kFrameBytes = kFrameRows * 16;
	/** @brief Its matrices (c0-c39), stored transposed: the builds load the rows raw, which reads a matrix column by column. */
	inline constexpr std::uint32_t kFrameMatrices = 10;

	/** @brief A skinned draw's palettes as Common/Skinned.hlsli's ORG_LATCHED_BONES reads them: the previous frame's (VS b9), then this one's (b10). */
	inline constexpr std::uint32_t kPaletteRows = 240;
	inline constexpr std::uint32_t kBonesBytes = 2 * kPaletteRows * 16;

	/** @brief The most draws one batch latches, and the most latch bytes: past either the rest of the batch is the engine's. */
	inline constexpr std::uint32_t kMaxDraws = 2048;
	inline constexpr std::uint32_t kLatchBytes = 1u << 20;

	/** @brief The shader a draw's program is built from. */
	enum class Shader : std::uint8_t
	{
		Utility,
		Lighting,
		Effect,
	};

	/** @brief One latched draw: its program, targets and the offsets of its values in its run's latch. */
	struct Draw
	{
		Shader shader = Shader::Utility;
		Family::ProgramKey key;
		std::shared_ptr<const void> program;              // the RasterProgram (MenuScenesDraws.cpp)
		std::array<std::uint8_t, kMaxColours> colours{};  // indices into the view's imports (TargetsOf)
		std::uint32_t colourCount = 0;
		std::uint32_t latchOffset = 0;  // its constants in the run's latch
		std::uint32_t bonesOffset = 0;  // its palettes in the run's latch (a skinned draw's; 0 otherwise, never read)
		std::uint32_t frameOffset = 0;  // its FrameBuffer rows in the run's latch
		std::uint32_t stencilRef = 0;
		std::uint32_t indexCount = 0;
		Family::Viewport viewport;
	};

	/** @brief A batch's draws, as the anchor's pass records them, and their latched bytes. */
	struct Run
	{
		View view = View::LocalMap;
		std::vector<Draw> draws;
		std::vector<std::byte> latch;  // every draw's constants and palettes, written into the client's latch at the run's commit
		std::uint64_t revision = 0;    // what the recording depends on (MenuScenesDraws.cpp, RevisionOf)
		Family::Owners owners;         // the textures' imports and the buffers' leases

		/** @brief Appends a_bytes to the latch, 16-byte aligned. Its offset, or ~0u when the latch is full. */
		std::uint32_t Append(std::span<const std::byte> a_bytes)
		{
			const std::size_t offset = (latch.size() + 15) & ~std::size_t{ 15 };
			if (offset + a_bytes.size() > kLatchBytes)
				return ~0u;
			latch.resize(offset + a_bytes.size());
			std::copy(a_bytes.begin(), a_bytes.end(), latch.begin() + static_cast<std::ptrdiff_t>(offset));
			return static_cast<std::uint32_t>(offset);
		}
	};

	/** @brief CS_ORG_MAP=1 (MenuViews::MapEnabled): the local map's batches are the family's. */
	bool Enabled();
	/** @brief Whether a_view's batches are the family's: CS_ORG_MAP for the local map's, CS_ORG_MENU3D for a menu model's. */
	bool ViewEnabled(View a_view);
	/** @brief Whether any view is the family's. */
	bool AnyEnabled();

	// MenuScenesCapture.cpp: the engine hooks, from the feature's install.
	void InstallHooks();
	void PreflightHooks();

	// MenuScenesDraws.cpp: the render graph's side (render thread).
	namespace SceneDraws
	{
		/** @brief Whether the anchor exists and the runtime is up; false for good once a client failed. */
		bool Available();
		/** @brief Reserves the EngineDraws anchor (the feature's install, before the render graph initializes). */
		void Reserve();
		/** @brief The family's setup (Family::AddSetup, main thread): the stages, the manifest's programs, the clients. True once done. */
		bool Setup();
		/** @brief Whether a_view's batch is taken now: its client exists, its targets are still its imports, and its anchor can run. */
		bool CanTake(View a_view);
		/** @brief How a_view's client reads a_id, the published id a bound slot exports. NotListed when the view has no client. */
		Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index);
		/** @brief The Utility program for a key: null when it cannot be built. */
		std::shared_ptr<const void> UtilityProgram(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief The Lighting program for a key: null when it cannot be built. */
		std::shared_ptr<const void> LightingProgram(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief The Effect program for a key: null when it cannot be built. */
		std::shared_ptr<const void> EffectProgram(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief The format of a_view's import a_index (rhi::Format value; 0 without one), and of its depth. */
		std::uint32_t ColourFormat(View a_view, std::uint32_t a_index);
		std::uint32_t DepthFormat(View a_view);
		/** @brief Keeps a batch's imports and leases as long as a run's when it ran nothing. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs a_run's view at the anchor with its draws. True when the run was submitted. */
		bool Submit(std::shared_ptr<Run> a_run);
		/** @brief One status line. */
		std::string Status();
	}
}
