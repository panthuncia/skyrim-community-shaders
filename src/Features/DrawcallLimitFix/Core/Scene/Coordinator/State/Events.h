#pragma once

// The structural events: properties and nodes changed, switch events, and the entries' drawn ranges.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorEvents
	{
		// Drawn index ranges of each ranged entry, such as object LOD's. Set from the attach's capture, then by its component.
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::vector<Api::EntryRange>> entryRanges;
		std::vector<const void*> propertyChanged;
		std::vector<RE::NiPointer<RE::NiAVObject>> nodeChanged;
		// One per node. Applied by the next walk.
		struct SwitchPending
		{
			RE::NiPointer<RE::NiAVObject> node;
			std::int32_t before = -1;
			bool structural = false;
		};
		std::vector<SwitchPending> switchPending;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> switchPendingIndex;
		// switchResync means PrimaryCull must read them all.
		std::vector<const RE::NiAVObject*> switchesApplied;
		bool switchResync = true;
	};
}
