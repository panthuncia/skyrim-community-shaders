#pragma once

#include "Feature.h"

/**
 * @brief Drawcall Limit Fix: replaces the native opaque render loop with GPU-driven indirect draws
 * executed by the render graph on DXVK's Vulkan device.
 *
 * Phase 2 (this state): static rigid geometry under the Static, Dynamic and MultiBound nodes of attached
 * cells is tracked and turned into tables each frame (Phase 1), and drawn by the render graph with indirect
 * commands into off-screen copies of the main pass's targets (IndirectDraws); the frame itself still comes
 * from the native draws. See docs/development/drawcall-limit-fix.md.
 */
struct DrawcallLimitFix : Feature
{
	virtual inline std::string GetName() override { return "Drawcall Limit Fix"; }
	virtual inline std::string GetShortName() override { return "DrawcallLimitFix"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	virtual bool IsInMenu() const override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { "Replaces the game's per-object opaque draw loop with GPU-driven indirect draws on the Vulkan render graph.",
			{ "Tracks static scene geometry directly from the scene graph",
				"Builds per-object draw tables for indirect execution" } };
	}

	virtual void PostPostLoad() override;
	virtual void Reset() override;
	virtual void Prepass() override;
	virtual void DrawSettings() override;

	/** @brief Deferred::EndDeferred, before the deferred composite (Phase 2 debug view). */
	void BeforeDeferredComposite();

	/** @brief Called after the Lighting shader's SetupGeometry for every native lighting draw. */
	void OnNativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);

	/**
	 * @brief Hybrid path (CS_DCLF_HYBRID=1): whether the native loop leaves this pass to DCLF.
	 *
	 * True only inside the main camera's depth and opaque ranges, and only for geometry the indirect draws
	 * drew in the frame before. Shadow, reflection and cubemap passes go through the same batch renderer and
	 * are never skipped.
	 */
	bool SkipNativePass(RE::BSRenderPass* a_pass);

	/**
	 * @brief After the hybrid Z-prepass: rebuild what was derived from the depth buffer before it ran.
	 *
	 * The engine's prepass depth copy and Terrain Blending's blended depth are both built at the end of the
	 * native depth pass, so they hold a scene without DCLF's objects, and every effect that reads depth
	 * afterwards paints the background over them.
	 */
	void RefreshDepthConsumers();

	/** @brief How many native passes were skipped in the last frame, and how many were offered. */
	struct SkipStats
	{
		std::uint32_t skipped = 0;
		std::uint32_t offered = 0;
		std::uint32_t skippedInDepth = 0;
		std::uint32_t skippedInOpaque = 0;
		std::uint32_t notInTables = 0;
		std::uint32_t kept = 0;
	};
	const SkipStats& GetSkipStats() const { return skipStats; }

private:
	struct Hooks
	{
		/** @brief The main camera's depth pass: DCLF's objects are skipped inside it too. */
		struct Main_RenderDepth
		{
			static void thunk(bool a_a1, bool a_a2);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The call sites of BSBatchRenderer::RenderPassImmediately, as Light Limit Fix hooks them. */
		template <int N>
		struct BSBatchRenderer_RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install();
	};

	bool installed = false;
	bool inDepthPass = false;
	std::uint32_t captureFrame = ~0u;  // the frame whose main-pass bindings have been captured
	SkipStats skipStats;
	SkipStats skipCounters;  // accumulating; published into skipStats every frame
	std::vector<std::string> skipSamples;  // names of a few skipped passes, for the report
};
