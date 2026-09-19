#pragma once

#include "Feature.h"

/**
 * @brief Drawcall Limit Fix: replaces the native opaque render loop with GPU-driven indirect draws
 * executed by the render graph on DXVK's Vulkan device.
 *
 * Phase 1 (this state): scene capture only. Static rigid geometry under the Static, Dynamic and
 * MultiBound nodes of attached cells is tracked and turned into CPU tables each frame (objects,
 * geometry, pipeline keys, indirect draw templates). Nothing is drawn yet; CS_DCLF_CAPTURE_PARITY=1
 * checks the tables against the native draws. See docs/development/drawcall-limit-fix.md.
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

	/** @brief Called after the Lighting shader's SetupGeometry for every native lighting draw. */
	void OnNativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);

private:
	bool installed = false;
};
