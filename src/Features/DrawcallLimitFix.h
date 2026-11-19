#pragma once

#include "Feature.h"

/**
 * @brief Drawcall Limit Fix: draws what the engine would draw per object with GPU-driven indirect draws, run by the render graph.
 * See docs/development/dclf-architecture.md.
 *
 * This is the feature's shell: install, engine hook thunks, render-thread lifecycle points, the menu toggle and settings UI.
 * Render thread unless noted. It is DCLF's one integration point with the rest of CS, and other CS code calls DCLF only through this header.
 * Without the render graph and ORGModuleServices (CS_HAS_DCLF), the rest of Features/DrawcallLimitFix/ is not built and every entry point is inert.
 */
struct DrawcallLimitFix : Feature
{
	virtual inline std::string GetName() override { return "Drawcall Limit Fix"; }
	virtual inline std::string GetShortName() override { return "DrawcallLimitFix"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	virtual bool IsInMenu() const override { return true; }
	/** @brief On by default, unlike other pre-release core features. */
	virtual bool IsDisabledByDefault() const override { return false; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { "Replaces the game's per-object opaque draw loop with GPU-driven indirect draws on the Vulkan render graph.",
			{ "Tracks static scene geometry directly from the scene graph",
				"Builds per-object draw tables for indirect execution" } };
	}

	/** @brief SKSE kPostLoad. SceneWriterFramework's provider election, when PostPostLoad will install DCLF. */
	void PostLoad();
	virtual void PostPostLoad() override;
	/** @brief SKSE kDataLoaded. SceneWriterFramework's integrity check, once every plugin has installed its hooks. */
	virtual void DataLoaded() override;
	virtual void SetupResources() override;
	/** @brief After every feature's SetupResources. Sets up the engine draw families. */
	virtual void PostSetupResources() override;
	virtual bool AppliesBootToggleLive() const override { return true; }
	virtual void Reset() override;
	/** @brief The Present hook, before the present. */
	void BeforePresent();
	virtual void Prepass() override;
	virtual void EarlyPrepass() override;
	virtual void DrawSettings() override;
	/** @brief The menu's live toggles by name, the culling mode and the compatibility monitor. A set CS_DCLF_* switch always beats a saved value. */
	virtual void SaveSettings(json& o_json) override;
	virtual void LoadSettings(json& o_json) override;
	/** @brief CS's "Clear shader cache", any thread. Rebuilds every live render graph pipeline from the shader files as they are now. */
	virtual void ClearShaderCache() override;

	/** @brief Where the main pass's opaque batches start. Captures what it binds for the colour epoch. */
	void BeforeOpaquePass();
	/**
	 * @brief The end of the main pass's opaque batches, which runs DCLF's colour epoch. Anything the engine draws after this blends onto
	 * the G-buffer or tests against opaque depth, so DCLF's objects must already be drawn: Terrain Blending's terrain, then blended decals.
	 */
	void AfterOpaquePass();
	/** @brief CS_DCLF_TARGET_PROBE=x,y: every bound G-buffer target averaged over a 64 x 64 block where the opaque pass ends. */
	void ProbeOpaqueTarget(std::uint32_t a_stage);

	/**
	 * @brief Before the engine draws the shadow maps. The scene graph and main camera cull are final, but the main accumulator's passes are not,
	 * because their registration jobs run alongside the shadow draws.
	 */
	void BeforeShadowMaps();
	/** @brief After Main_RenderShadowMaps returns. Runs the shadow epoch with the occlusion maps OcclusionReady took. */
	void AfterShadowMaps();

	/** @brief Called after the Lighting shader's SetupGeometry for every native lighting draw. */
	void OnNativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);

	/**
	 * @brief The occlusion maps, Skylighting's sky map and the precipitation mask, which DCLF draws in its shadow epoch.
	 * OcclusionReady, before the shadow maps, decides whether DCLF draws a map this frame. OcclusionNeedsEngine, at the engine's
	 * occlusion point, decides whether the engine must render it whole instead.
	 */
	enum OcclusionMap : std::uint32_t
	{
		kSkyOcclusion = 0,            // DCLF::Api::OcclusionMap::Sky
		kPrecipitationOcclusion = 1,  // DCLF::Api::OcclusionMap::Precipitation
	};
	/**
	 * @brief Before the shadow maps. Whether DCLF draws the map in this frame's shadow epoch.
	 * An occluder still joining the map's phase is left out until it joins, never handed to the engine's cull.
	 */
	bool OcclusionReady(OcclusionMap a_map);
	/**
	 * @brief At the engine's occlusion point. Whether the engine must cull and render the map because DCLF did not draw it this frame.
	 * The scene lists the frame's filter left out are put back first.
	 */
	bool OcclusionNeedsEngine(OcclusionMap a_map);
	/** @brief Once the map's camera is set up, before RenderMask. DCLF's view of the map, from that camera. */
	void OcclusionView(OcclusionMap a_map);
	/** @brief After OcclusionReady. Whether the engine also renders the map whole first as a parity reference. */
	bool OcclusionReferenceFrame(OcclusionMap a_map);
	/**
	 * @brief When DCLF draws the map, the engine's SetupMask culls only the engine-only occluders, so what DCLF cannot draw still occludes.
	 * Returns false when there are none, and the caller sets the projection and camera alone.
	 */
	bool OcclusionEngineOnlyMask(RE::Precipitation* a_precip);
	/** @brief Render thread, at Present. Whether to skip the present and keep the last frame, during a load without a loading menu. */
	bool LoadHoldsPresents() const;
	/** @brief After OcclusionNeedsEngine. The map is held by a load, so render it without the engine's cull. */
	bool OcclusionHeld(OcclusionMap a_map) const;
	void OcclusionReferenceRendered(OcclusionMap a_map);
	/**
	 * @brief Renders the precipitation mask, for Skylighting at its two points or for DCLF alone without Skylighting.
	 * With a_dclf, the engine's cull and registration are skipped except on a reference frame, and RenderMask sets the camera and clears it.
	 */
	void RenderPrecipitationMask(RE::Precipitation* a_precip, RE::BSGeometry* a_precipObject, bool a_dclf);
	/** @brief After Skylighting's maps. Without Skylighting, sets up and clears the precipitation mask DCLF draws this frame. */
	void RenderPrecipitationAtShadowMaps();

	// ---- Calls from the rest of CS. Each is inert in a build without DCLF.

	/** @brief Any thread, lock-free. A material was written in place by code that bypasses the hooked writers, such as CS's PBR materials. */
	static void NoteMaterialWritten(const RE::BSShaderMaterial* a_material);
	/** @brief Calls NoteMaterialWritten when the scope ends, for in-place rewrites with early returns. */
	struct MaterialWriteScope
	{
		const RE::BSShaderMaterial* material;
		~MaterialWriteScope() { NoteMaterialWritten(material); }
	};
	/**
	 * @brief CS's own writes to scene objects, made through SceneWriterFramework so every subscriber sees them like engine writes.
	 * A plain store where no provider serves it. Call on the thread the engine allows the write on.
	 */
	static void WriteHidden(RE::NiAVObject* a_object, bool a_hidden);
	static void WriteShaderProperty(RE::BSGeometry* a_geometry, RE::BSShaderProperty* a_property);
	static void WriteAlphaFlags(RE::NiAlphaProperty* a_alpha, std::uint16_t a_flags);
	/** @brief ShaderCache's precompile of a Lighting, Effect or DistantTree permutation. DCLF builds the same one as SPIR-V. */
	static void PrecompileShader(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor);
	/** @brief A descriptor the engine loaded for a_shader. Builds the engine draw families' ORG stages and keeps the shader object for their defines. */
	static void PrecompileFamilyStages(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor);
	/** @brief The D3D11 context's Map, Unmap and UpdateSubresource, for DCLF's constant buffer mirror and capture parity. */
	static void OnMap(ID3D11Resource* a_resource, void* a_data);
	static void OnUnmap(ID3D11Resource* a_resource);
	static void OnUpdateSubresource(ID3D11Resource* a_resource, const D3D11_BOX* a_box, const void* a_data);
	/** @brief CS_DCLF_TARGET_DUMP's points from other features, such as Upscaling. */
	static bool TargetDumpArmed();
	static void TargetDumpPoint(const char* a_label, ID3D11Texture2D* a_extra = nullptr);
	/** @brief Every render graph submission up to a_submissions completed on the GPU. Drives DCLF's retirement clock. */
	static void OnGraphSubmissionsRetired(std::uint64_t a_submissions);
	/**
	 * @brief Render thread. One of DCLF's epochs failed to build or prepare. DCLF hands drawing back to the engine for the session
	 * at its next frame start. The first failure is kept.
	 */
	static void OnGraphEpochFailed(std::string_view a_epoch, const std::string& a_error);
	/** @brief Render thread. Why one of DCLF's epochs failed, or empty. */
	static const std::string& GraphEpochFailure();

	/** @brief The feature's state for the diagnostic capture. */
	struct CaptureInfo
	{
		bool installed = false, switchedOn = false, running = false, faulted = false, pumpLagging = false;
		std::string unavailableReason;
	};
	/** @brief Render thread. The feature's state, for the diagnostic capture. */
	CaptureInfo GetCaptureInfo() const { return { installed, switchedOn, Running(), faulted, pumpLagging, unavailableReason }; }

	/** @brief Render thread. Whether the hooks are installed. */
	bool HooksInstalled() const { return installed; }

private:
	struct Hooks
	{
		/** @brief The main camera's depth pass. DCLF's objects are skipped inside it too. */
		struct Main_RenderDepth
		{
			static void thunk(bool a_firstPerson, bool a_a2);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Inside the depth pass at AE Main::RenderDepth +0x1AA, after the world's depth draws. The Z-prepass runs here.
		 * This is the last point where the world camera is current. In first person the depth pass then switches to the first-person camera
		 * and leaves it set until Main::Draw restores it, so a later Z-prepass would draw the world with the wrong camera.
		 */
		struct Main_RenderDepth_WorldDrawn
		{
			static void thunk(void* a_accumulator, bool a_a2);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Main::Draw's first call at AE +0xD3, before the main cull jobs are queued. The frame's scene phase starts here, about 1 ms before the shadow maps. */
		struct Main_Draw_Early
		{
			static std::int64_t thunk(void* a_main);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Main::Update's first call (AE 0x140645ea0 +0x3E) on the main thread, before the frame's job lists start.
		 * The one point every reader sees in the same frame, so the stood-in roots' kAccumulated writes and fade write-backs go here.
		 */
		struct Main_Update_Entry
		{
			static void thunk(void* a_stages);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Without Skylighting, Main::Draw's call of Precipitation::RenderOcclusion (AE FUN_1404076c0, SE FUN_1403ae920). Skipped when DCLF drew the mask. */
		struct Main_Precipitation_RenderOcclusion
		{
			static void thunk();
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSShaderAccumulator::FinishAccumulatingPreResolveDepth (vfunc 0x2A), a shadow view's draw. DCLF's casters draw into the same slice after it. */
		struct BSShaderAccumulator_FinishAccumulating
		{
			static void thunk(RE::BSGraphics::BSShaderAccumulator* a_accumulator, std::uint32_t a_renderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The BSBatchRenderer::RenderPassImmediately call sites, as Light Limit Fix hooks them. */
		template <int N>
		struct BSBatchRenderer_RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The sites Install writes, checked before any hook is written. */
		static void Preflight();
		static void Install();
	};

	bool installed = false;
	// The menu's on/off toggle, applied at the next Present. Off keeps the hooks and scene tracking, so turning back on needs no rescan,
	// but does no frame work at all.
	bool switchedOn = true;
	bool Running() const { return installed && switchedOn; }

	// The frame's start, from Main::Draw's early hook. Returns Running().
	bool BeginSceneFrame();
	// Off when the menu toggle is off or the feature is unloaded. Either way nothing may keep drawing, skipping or withholding.
	void UpdateActive();
	void SetActive(bool a_active);
	// The scene pump's watchdog. A stopped pump turns DCLF off for the session with the reason in the menu, rather than keeping the last snapshot forever.
	void WatchScenePump();
	// The graph's watchdog. A failed DCLF epoch turns DCLF off for the session, as a stopped pump does.
	void WatchGraphFailure();
	bool faulted = false;
	bool pumpLagging = false;
	std::chrono::steady_clock::time_point pumpLagSince;
	// Why DCLF is off for the session, shown in the menu.
	std::string unavailableReason;
	/** @brief Off for the session after its hooks went in. The hooks pass straight through, scene tracking stops and nothing is withheld. */
	void ForceOff();
	// The settings LoadSettings read, applied to the requested toggles once they are seeded, and again at each load.
	json savedSettings;
	void ApplySavedSettings();
};
