#pragma once

// Grass draws the engine's grass in its place, from mirrored instance records, as the grass draw family.
// GrassShared's capture (shared with Grass Optimizations) feeds the render thread's mirror. GrassTables.cpp uploads it,
// GrassPasses.cpp culls it on the GPU and GrassDraws.cpp draws it.
//
// While DCLF owns grass the engine's grass passes are withheld, so nothing hooked on BSGrassShader::SetupGeometry runs.
// GrassShared::SetDclfOwned tells Grass Optimizations and Grass Collision to stand down, and Grass Collision's update runs from DCLF.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <cstdint>

namespace DCLF::Features::Grass
{
	// Address Library ids.
	namespace Address
	{
		inline constexpr Engine::Offset kGrassShader{ { 414752 }, { 527801 } };        // BSGrassShader*: the singleton (DistantTree's is 0x33dcd10)
		inline constexpr Engine::Offset kGrassManager{ { 400452 }, { 514292 } };       // BGSGrassManager*
		inline constexpr std::uintptr_t kGrassManagerNode = 0x68;        // its grass node (NiAVObject*), +0xF4 bit 0: ToggleGrass off
		inline constexpr Engine::Offset kWindTimer{ { 390957 }, { 513217 } };          // float: the shader manager's timer SetupGeometry's WindTimer reads
		inline constexpr Engine::Offset kWindSpeed{ { 414753 }, { 527802 } };          // float: the grass wind's strength (clamped to 60)
		inline constexpr Engine::Offset kWindGrassMultiplier{ { 391112 }, { 513337 } };  // float: fWindGrassMultiplier:Display
		inline constexpr Engine::Offset kAlphaParams{ { 414681 }, { 527758 } };        // float[2]: AlphaParam1, AlphaParam2 (the distance fade)
		inline constexpr Engine::Offset kShadowClampValue{ { 391115 }, { 513339 } };   // float: fShadowClampValue:Display
		inline constexpr Engine::Offset kFogNearColorW{ { 390973 }, { 513233 } };      // float: FogNearColor.w
	}

	Api::Feature Feature();

	// Installs the sink on GrassShared's capture once at PostPostLoad.
	void InstallHooks();
	// Checks the call sites before any hook is written. The thunks chain, so the sites must be CALLs.
	void PreflightHooks();

	void RegisterTables(Api::InstallContext& a_context);
	void RegisterPasses(Api::InstallContext& a_context);
	// Must register after the tables, which it uses.
	void RegisterFamily(Api::InstallContext& a_context);
	void RegisterReport(Api::InstallContext& a_context);
}
