#pragma once

// TreeLod: draws the tree LOD instances in the engine's place as the TreeInstances draw family.
// Early hooks feed an event channel (TreeLodHooks.cpp), the render thread keeps a mirror of the instance records (TreeLodMirror.h),
// and the depth commit uploads them to GPU tables (TreeLodTables.cpp). TreeLodCullCS culls them in the depth segment's two phases.
// Core keeps the family's slot: its pipelines, the ownership that withholds the engine's tree LOD passes, and trees views' lists.
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <cstdint>

namespace DCLF::Features::TreeLod
{
	namespace Address
	{
		inline constexpr Engine::Offset kTreeLodAtlas{ { 415172 }, { 528222 } };  // NiSourceTexture*: tree LOD's atlas
		inline constexpr Engine::Offset kDistantTreeShader{ { 415171 }, { 528221 } };  // BSDistantTreeShader*: the singleton
	}

	// The engine root a trees view's faces draw in the engine's place. One owner, listed in Features/Manifest.cpp.
	inline constexpr Api::ViewRoot kFacesRoot = Api::ViewRoot::LodTrees;

	// CS_DCLF_TREE_LOD_TINT, set at install. The main colour draw's push word 3.
	inline bool debugTint = false;
	// CS_DCLF_TREE_LOD_STAGES, set at install. Stage stamps read back with the counts.
	inline bool debugStages = false;

	Api::Feature Feature();

	void InstallHooks();
	// Checks the block update's call before any hook is written. It must be a CALL, as the thunk chains.
	void PreflightHooks();

	void RegisterTables(Api::InstallContext& a_context);
	void RegisterPasses(Api::InstallContext& a_context);
	// Register after the tables, whose resources the family uses.
	void RegisterFamily(Api::InstallContext& a_context);
	Api::DrawFamilyContribution::Faces FacesContribution();
	void RegisterFacesCull(Api::InstallContext& a_context);
	void RegisterReport(Api::InstallContext& a_context);
}
