#pragma once

// Shadow mask (CS_ORG_SHADOW_MASK, docs/development/p5/shadow-mask.md): the engine's shadow mask passes drawn by the render graph in the
// engine's place. Each shadow-casting light of a scene is one BSUtilityShader pass over the screen quad (ImageSpaceManager's) into
// kSHADOW_MASK, which reads the scene depth and the light's shadow maps: the world's at Main::Draw (AE 107141, SE 100423, after the depth
// pass) and a menu scene's at UI3DSceneManager::Render (AE 52728, SE 51856: the inventory's, the magic menu's and the others' models).
//
// The engine's CPU code runs unchanged. At either call (ShadowMaskCapture.cpp) the family lets the engine's function run with its draws
// latched: each light's pass goes through MakeRenderPass, the Utility shader's SetupTechnique and SetupGeometry and render state as
// before, and DrawTriShape (the engine draw families' one detour, Core/Engine/Family) records the draw instead of drawing it, then applies
// the engine's pending state as the draw function would (the clear of kSHADOW_MASK the function asked for, the light's blend state). The
// draws are then drawn by an ORG pass at the EngineDraws anchor, in the engine's order, with CS's Utility.hlsl built for ORG (its shadow
// mask techniques, ShadowMaskDraws.cpp) and the pipeline the engine's state objects describe.
//
// The family is the primary path for the shadow masks (docs/development/p5/primary-path.md): its stages compile when CS loads Utility's
// RenderShadowmask* descriptors, its programs are prebuilt from the manifest, and both views' clients are made at setup with every
// published input their draws can read (ShadowMaskDraws.cpp, ListInputs); nothing registers one again during play. A draw it cannot take
// (its inherent refusals, docs/development/p5/shadow-mask.md "What stays the engine's") ends the batch's latched part: the latched draws
// run at that point and the engine draws the rest.

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

namespace DCLF::Features::ShadowMask
{
	// Engine::Offsets (Address Library ids) of the engine code the family hooks (docs/development/p5/shadow-mask.md, "RE").
	namespace Address
	{
		// Main::RenderShadowMasks (AE 107140, SE 100422) calls the world's shadow masks (AE 107141, SE 100423) when a light casts.
		inline constexpr Engine::Offset kWorldMasks{ { 107140, 0x7E }, { 100422, 0x90 } };
		// UI3DSceneManager::Render (AE 52727, SE 51855) calls the menu scene's shadow masks (AE 52728, SE 51856).
		inline constexpr Engine::Offset kMenuMasks{ { 52727, 0x461 }, { 51855, 0x466 } };
	}

	/** @brief Whose shadow masks a batch draws. */
	enum class View : std::uint8_t
	{
		World,  // Main::Draw's, after the depth pass: the sun's cascades and the world's shadow-casting lights
		Menu,   // a menu scene's (UI3DSceneManager::Render): the menu lights of the model's light scheme
		Count
	};

	/** @brief RE::RENDER_TARGET::kSHADOW_MASK: the one target the passes write. */
	inline constexpr std::uint32_t kShadowMaskTarget = 18;

	/** @brief The slots a draw samples (Utility.hlsl's t0-t7 and s0-s7). */
	inline constexpr std::uint32_t kSlots = 8;

	using Kind = GpuTextures::ViewKind;

	/** @brief Each slot's declared dimension in Utility.hlsl: the shadow maps (t3, t4) and the focus shadow maps (t6) are arrays. */
	constexpr Kind SlotKind(std::uint32_t a_slot)
	{
		return a_slot == 3 || a_slot == 4 || a_slot == 6 ? Kind::Texture2DArray : Kind::Texture2D;
	}

	/**
	 * @brief Whether Utility.hlsl's ORG build reads slot a_slot's texture: only the scene depth (t2) and the light's shadow map (t4).
	 * Every other slot is latched as a null view, whatever is bound there, so it can never refuse a draw.
	 * - t3: the plain view of the shadow map the filter-0 path samples. The ORG build reads the same map through t4's index instead
	 *   (Utility.hlsl), with s3's sampler: latched from t3, the sun's masks read a null view (every sample 0, so every pixel in
	 *   shadow), while t4 held the cascade array.
	 * - t0, t1, t7: the shadow mask techniques never use them. t0 often still holds the last material's texture (a streamed one may
	 *   not be importable yet).
	 * - t5, t6: the focus shadows' stencil and maps. The engine's draw reads the stencil from the view's red component
	 *   (TexStencilSampler.Load(...).x). DXVK's stencil view (X24_TYPELESS_G8_UINT) holds the stencil in green and zero in red, so the
	 *   engine's draw reads 0 and never samples t6. The ORG build reads 0 as well, without a view: BasicRHI cannot view the stencil
	 *   of a depth-stencil image, so GpuTextures rejects that view.
	 */
	constexpr bool SlotRead(std::uint32_t a_slot) { return a_slot == 2 || a_slot == 4; }

	/** @brief Whether Utility.hlsl's ORG build reads slot a_slot's sampler: the read slots', and s3, the filter-0 path's plain sampler. */
	constexpr bool SamplerRead(std::uint32_t a_slot) { return SlotRead(a_slot) || a_slot == 3; }

	/** @brief The rows of PS b0 (PerTechnique) and PS b2 (PerGeometry) the latch takes (Utility.hlsl's packoffsets). */
	inline constexpr std::uint32_t kTechniqueRows = 5;
	inline constexpr std::uint32_t kGeometryRows = 28;

	/** @brief UtilityOrgShadowMaskDraw in Utility.hlsl: a draw's latched values, 640 bytes. */
	struct DrawConstants
	{
		float psTechnique[kTechniqueRows][4];
		float psGeometry[kGeometryRows][4];
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
	static_assert(sizeof(DrawConstants) == 640 && sizeof(DrawConstants) % 16 == 0, "UtilityOrgShadowMaskDraw's layout");

	/** @brief One latched draw. */
	struct Draw
	{
		Family::ProgramKey key;
		std::shared_ptr<const void> program;  // the RasterProgram (ShadowMaskDraws.cpp)
		std::uint32_t indexCount = 0;
		Family::Viewport viewport;
		DrawConstants constants{};
	};

	/**
	 * @brief What a view's client is made for: its view. Its target is kSHADOW_MASK, and the published inputs its pass declares are
	 * fixed when the client is made (ShadowMaskDraws.cpp, ListInputs), so neither is part of it. Every slot is read by heap index,
	 * resolved per draw: a D3D11 view's import, a null view, or a published input's index once the client's graph declares it
	 * (ShadowMaskDraws::ReadPublished).
	 */
	struct Shape
	{
		View view = View::World;
		bool operator==(const Shape&) const = default;
	};

	/** @brief A batch's draws, as the anchor's pass records them. */
	struct Run
	{
		View view = View::World;
		std::vector<Draw> draws;
		std::uint64_t revision = 0;  // what the recording depends on (ShadowMaskDraws.cpp, RevisionOf)
		Family::Owners owners;       // the textures' imports and the buffers' leases
	};

	/** @brief "world" or "menu", for logs and client ids. */
	const char* NameOf(View a_view);

	/**
	 * @brief Whether the graph takes a_view's batches. CS_ORG_SHADOW_MASK_VIEWS (world, menu, or both comma separated; default both)
	 * keeps a view on the engine. Read once.
	 */
	bool TakesView(View a_view);

	// ShadowMaskFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	/** @brief Whether CS_ORG_SHADOW_MASK=1 turned the family on (read at install). */
	bool Enabled();

	// ShadowMaskCapture.cpp: the engine hooks, from the feature's install.
	void InstallHooks();
	void PreflightHooks();

	// ShadowMaskDraws.cpp: the render graph's side (render thread).
	namespace ShadowMaskDraws
	{
		/** @brief Whether the anchor exists and the runtime is up; false for good once a client failed. */
		bool Available();
		/** @brief Reserves the EngineDraws anchor (the feature's install, before the render graph initializes). */
		void Reserve();
		/**
		 * @brief The family's setup (Family::AddSetup, main thread): the stage precompile started, the manifest's programs queued, both
		 * views' clients made and their build asked for. True once done (or refused for good, logged); false while the graph has no host.
		 */
		bool Setup();
		/**
		 * @brief Whether a_view's batch is taken: its client exists, kSHADOW_MASK is still the import it was made with, and a run of the
		 * anchor now would draw (FrameAnchors::CanRun). False is logged once per reason; the engine draws the batch.
		 */
		bool CanTake(View a_view);
		/**
		 * @brief How a_view's client reads a_id, the published id a bound slot exports (Family::PublishedInputs::Resolve). NotListed
		 * when the view has no client.
		 */
		Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index);
		/** @brief The program for a key (Family::EngineDrawPrograms::Get): null when it cannot be built. */
		std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief Keeps a batch's imports and leases as long as a run's when it ran nothing, so the next batches find them. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs a_run's view at the anchor with its draws. True when the run was submitted; otherwise its draws were not drawn (logged). */
		bool Submit(std::shared_ptr<Run> a_run);
		/** @brief One status line. */
		std::string Status();
	}
}
