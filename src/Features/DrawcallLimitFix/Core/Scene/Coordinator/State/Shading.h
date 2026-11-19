#pragma once

// The shading events: the slots named for the next frame's shading sample, and LOD fade changes.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorShading
	{
		std::vector<ShadingItem> shadingNamed;
		bool shadingNamedAll = false;  // NameShadingEvents named every slot once
		struct ShadingStats
		{
			std::uint64_t resampled = 0, lodFadeEvents = 0, emittanceEvents = 0;
		} shadingStats;
		std::vector<const void*> lodFadeChanged;
	};
}
