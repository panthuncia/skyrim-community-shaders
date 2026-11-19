#pragma once

// MenuViews (CS_ORG_MENU3D, CS_ORG_MAP; docs/development/p5/menus.md): the engine's menu scene renders as DCLF views of their own.
// Menu scenes are UI3DSceneManager::Render's models and the local map. The race menu and world map are the main world render with
// another camera, which DCLF's main view already draws.
// This stage is a diagnostic probe. It logs what a view of each scene would need and withholds nothing, so the engine draws every
// menu scene and no graph client is registered.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <cstdint>
#include <string>

namespace DCLF::Features::MenuViews
{
	namespace Address
	{
		// UI3DSceneManager::Render's colour SubmitAccumulator (SE 51855 +0x658, AE 52727 +0x659).
		inline constexpr Engine::Offset kMenu3DSubmit{ { 52727, 0x659 }, { 51855, 0x658 } };
		// Its depth pass with keepDepth: StartAccumulating (SE 51855 +0x43A, AE 52727 +0x435).
		inline constexpr Engine::Offset kMenu3DDepthSubmit{ { 52727, 0x435 }, { 51855, 0x43A } };
		// Its call that renders the loading screen model's shadow lights, scheme 4 only (SE 51855 +0x2B7, AE 52727 +0x2AD).
		inline constexpr Engine::Offset kMenu3DShadowRender{ { 52727, 0x2AD }, { 51855, 0x2B7 } };
		// LocalMapRenderer::Render's first SubmitAccumulator: land and objects into kLOCAL_MAP_SWAP (SE 16094 +0x340, AE 16335 +0x342).
		inline constexpr Engine::Offset kLocalMapSubmit{ { 16335, 0x342 }, { 16094, 0x340 } };
		// Its second, after SetRenderMode(0x13): the water pass (SE 16094 +0x430, AE 16335 +0x433).
		inline constexpr Engine::Offset kLocalMapWaterSubmit{ { 16335, 0x433 }, { 16094, 0x430 } };
	}

	/** @brief The menu scene a SubmitAccumulator call renders. */
	enum class MenuScene : std::uint8_t
	{
		Menu3D,         // UI3DSceneManager::Render: a light scheme's model (RE::INTERFACE_LIGHT_SCHEME)
		LocalMap,       // LocalMapRenderer::Render's land and objects pass
		LocalMapWater,  // its water pass (render mode 0x13)
		Count
	};

	Api::Feature Feature();

	/** @brief Whether CS_ORG_MENU3D=1. Read once. */
	bool Menu3DEnabled();
	/** @brief Whether CS_ORG_MAP=1. Read once. */
	bool MapEnabled();

	void InstallHooks();
	void PreflightHooks();
	/** @brief Takes the probe's counters since the last report. */
	std::string Report();
}
