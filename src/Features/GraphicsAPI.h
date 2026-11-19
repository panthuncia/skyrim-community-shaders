#pragma once

#include "Feature.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @brief Graphics API: which API Community Shaders runs the game on. The choice takes effect at the next start.
 *
 * - DXVK Vulkan: DXVK translates the game's D3D11 to Vulkan, and the render graph shares DXVK's device. The default.
 * - Native Vulkan: no DXVK. The render graph makes its own Vulkan device and owns Present (CS_ORG_NATIVE=1).
 * - Native D3D12: as native Vulkan, on BasicRHI's D3D12 device (CS_ORG_NATIVE=1, CS_ORG_NATIVE_BACKEND=d3d12).
 *
 * Where the mode comes from, first match wins:
 * 1. The environment: CS_ORG_NATIVE (and CS_ORG_NATIVE_BACKEND) already set, by the user or the old CommunityShaders.env.
 * 2. A test run (CommunityShaders-Test.json): its own switches only, so the menu choice never changes what a test runs.
 * 3. The override in the feature's ini (Data\Shaders\Features\GraphicsAPI.ini, [Override] Mode = dxvk | vulkan | d3d12).
 * 4. The menu choice, saved in SettingsUser.json.
 * 5. DXVK Vulkan.
 */
struct GraphicsAPI : Feature
{
	enum class Mode : int
	{
		DXVK,
		NativeVulkan,
		NativeD3D12,
		Count
	};

	/** @brief What decided the mode this session runs on. */
	enum class Source : int
	{
		Default,
		Menu,
		Ini,
		TestRun,
		Environment,
	};

	static GraphicsAPI* GetSingleton();

	/**
	 * @brief SKSEPlugin_Load, after the test configuration and before native mode's switches: picks the mode (see the struct's
	 * comment) and sets CS_ORG_NATIVE and CS_ORG_NATIVE_BACKEND for it. Reads SettingsUser.json itself: State has not loaded yet.
	 */
	static void ApplyAtBoot();

	/** @brief The mode this session runs on. */
	static Mode BootMode();

	/** @brief What decided BootMode(). */
	static Source BootSource();

	/** @brief "dxvk", "vulkan" or "d3d12": the settings and ini spelling. */
	static const char* Key(Mode a_mode);

	/** @brief The mode a key names; Mode::Count when it names none. Ignores ASCII case. */
	static Mode FromKey(std::string_view a_key);

	/** @brief Whether this build can run a_mode (native modes need the render graph; D3D12 needs its backend). */
	static bool Available(Mode a_mode);

	std::string GetName() override { return "Graphics API"; }
	std::string GetDisplayName() override;
	std::string GetShortName() override { return "GraphicsAPI"; }
	std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	bool IsCore() const override { return true; }
	bool IsAlwaysEnabled() const override { return true; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;

	void DrawSettings() override;
	void LoadSettings(json& o_json) override;
	void SaveSettings(json& o_json) override;
	void RestoreDefaultSettings() override;

	// The menu's choice, applied at the next start.
	Mode selected = Mode::DXVK;
};
