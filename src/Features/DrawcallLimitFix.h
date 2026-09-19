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

private:
	bool installed = false;
};
