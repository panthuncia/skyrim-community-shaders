// Water as scene objects: ObjectClass::Water and the roots water hangs under.
//
// Where drawn water hangs (phase 0's census, read in Ghidra on SE 1.5.97 and AE 1.6.1170):
// - Unified Water's tiles and vanilla LOD water: TES::objLODWaterRoot (TES +0x90), the NiNode "WaterLOD" under "LODRoot".
//   It is the node BGSTerrainManager hands out (Address Library 516171 / 402322); TES's constructor takes it (SE 516930).
//   TESWaterSystem::AddLODWater (SE 31404) attaches a terrain chunk's 'Water' BSMultiBoundNode under it after AddWater, and
//   Unified Water's BGSTerrainBlock_Attach does the same with its tiles. Both are AttachChild calls, so they are structural events.
// - Procedural water: TESWaterSystem::waterRoot (+0x90), a plain NiNode the water system's constructor (SE 0x1404c5050)
//   attaches under TES::objRoot ("ObjectLODRoot"). AddWater (SE 31388) attaches BGSAutoWater's clones under it.
// - Cell water and water inside statics (a horse trough, 'Water1024'): under objRoot's BSMultiBoundNode children. Core already
//   tracks those as category nodes, so they need no root here. An entry has one category node, the nearest, so nothing is
//   tracked twice: the trough's Lighting shapes and its water shape are each one entry under the same multibound.
// AddWater itself attaches no shape. It writes the property's plane and flags, then SetMaterial, which is a property event.

#include "Features/DrawcallLimitFix/Features/Water/Scene/WaterScene.h"

#include <atomic>

namespace DCLF::Features::Water::WaterScene
{
	namespace
	{
		// The render thread's last listing. Keys only: the census compares them, never dereferences them.
		std::atomic<const void*> listedWaterLod{ nullptr };
		std::atomic<const void*> listedWaterSystem{ nullptr };

		/** @brief Render thread, every frame and at each category capture. Two reads, so it costs nothing to repeat. */
		void GatherRoots(Api::CategoryRootSink& a_sink)
		{
			RE::NiNode* lod = nullptr;
			RE::NiNode* system = nullptr;
			if (const auto* tes = RE::TES::GetSingleton())
				lod = tes->objLODWaterRoot;
			if (const auto* water = RE::TESWaterSystem::GetSingleton())
				system = water->waterRoot.get();
			listedWaterLod.store(lod, std::memory_order_relaxed);
			listedWaterSystem.store(system, std::memory_order_relaxed);
			a_sink.Add(lod);
			a_sink.Add(system);
		}
	}

	Roots ListedRoots()
	{
		return { listedWaterLod.load(std::memory_order_relaxed), listedWaterSystem.load(std::memory_order_relaxed) };
	}

	void Install(Api::InstallContext& a_context)
	{
		a_context.RegisterObjectClass({ .objectClass = Api::ObjectClass::Water, .categoryRoots = &GatherRoots });
		// Must register after the object class.
		RegisterPort(a_context);
		RegisterCensus(a_context);
		RegisterRows(a_context);

	}
}
