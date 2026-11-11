#pragma once

// MenuViews (CS_ORG_MENU3D, CS_ORG_MAP; docs/development/p5/menus.md): the engine's menu scene renders as DCLF views of their own.
//
// The menu scenes are the engine's SubmitAccumulator calls outside the world frame's: UI3DSceneManager::Render (the inventory, magic,
// book, journal, stats, lockpicking and loading-screen models) and the local map render (LocalMapRenderer::Render). The race menu and the
// world map are not among them: they are the main world render with another camera, which DCLF's main view already draws.
//
// This first stage is a diagnostic probe, behind CS_ORG_MENU3D and CS_ORG_MAP, the plan asks for before anything is withheld. At each menu scene's SubmitAccumulator the feature reads
// what a view of the scene would need (its camera, its targets and stencil, its root's geometry, whether DCLF's scene tracks that geometry
// and has a frame of its own this frame) and logs it once per change. Nothing is withheld: the engine draws every menu scene, and the
// feature registers no graph client, so it causes no graph rebuild. The view itself (menus.md, "The menu 3D view") is built on top of
// these verdicts.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <cstdint>
#include <string>

namespace DCLF::Features::MenuViews
{
	// Engine::Offsets (Address Library ids and call offsets) of the calls the feature takes (menus.md, "RE").
	namespace Address
	{
		// UI3DSceneManager::Render's SubmitAccumulator(camera, colour accumulator (+0x10), flags | 0x200) (SE 51855 +0x658, AE 52727 +0x659).
		inline constexpr Engine::Offset kMenu3DSubmit{ { 52727, 0x659 }, { 51855, 0x658 } };
		// LocalMapRenderer::Render's first SubmitAccumulator(local map camera (+0x302B8), accumulator (+0x302C8), 0): the land and objects into
		// kLOCAL_MAP_SWAP (SE 16094 +0x340, AE 16335 +0x342).
		inline constexpr Engine::Offset kLocalMapSubmit{ { 16335, 0x342 }, { 16094, 0x340 } };
		// Its second, after BSShaderManager::SetRenderMode(0x13): the water pass (SE 16094 +0x430, AE 16335 +0x433).
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

	// MenuViewsFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	/** @brief CS_ORG_MENU3D=1 (read once). */
	bool Menu3DEnabled();
	/** @brief CS_ORG_MAP=1 (read once). */
	bool MapEnabled();

	// MenuViewsProbe.cpp: the engine hooks (from the feature's install) and the probe's report.
	void InstallHooks();
	void PreflightHooks();
	/** @brief The probe's counters since the last report, taken. */
	std::string Report();
}
