#pragma once

// MenuViews (graph mode; docs/development/engine/menus.md): the engine's menu scene renders as DCLF views of their own.
// Menu scenes are UI3DSceneManager::Render's models and the local map. The race menu and world map are the main world render with
// another camera, which DCLF's main view already draws.

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

	Api::Feature Feature();

	/** @brief Whether CS draws a light scheme's menu scene natively (MenuScenesNative.cpp). */
	bool NativeOwns(std::int32_t a_scheme);

	/** @brief Whether the menu 3D scene is the family's, which is graph mode. Read once. */
	bool Menu3DEnabled();
	/** @brief Whether the local map is the family's, which is graph mode. Read once. */
	bool MapEnabled();
}
