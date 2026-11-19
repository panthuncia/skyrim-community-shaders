#pragma once

// The dependents lists: the entries under each property, root and category node, and the root owners.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorDependents
	{
		std::vector<const RE::NiAVObject*> dirtyRoots;
		ankerl::unordered_dense::map<const void*, std::vector<RE::BSGeometry*>> propertyDependents;
		/** @brief Positions in a dependents list, so a removal swaps the last entry in without a search. Every insertion must go through List. */
		using DependentsAt = ankerl::unordered_dense::map<std::pair<const void*, const RE::BSGeometry*>, std::uint32_t>;
		DependentsAt propertyDependentsAt;
		// Unused until those lists' insertions go through List. Until then their Unlist searches.
		DependentsAt rootDependentsAt, lightDependentsAt, hiddenDependentsAt;
		/** @brief Lets a cell that goes away, or a category hidden event, take its members without a walk over every entry. */
		ankerl::unordered_dense::map<const RE::NiNode*, std::vector<RE::BSGeometry*>> categoryMembers;
		DependentsAt categoryMembersAt;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::vector<RE::BSGeometry*>> rootDependents;
		/** @brief A reference to every listed root. References handed out are copies, never made from a key, which may name a dead node. */
		ankerl::unordered_dense::map<const RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>> rootOwners;
		std::uint64_t unownedRoots = 0;  // since the last report: roots handed out unlisted (a defect)
		// Once its last dependent leaves, the root is a key that may be gone.
		ankerl::unordered_dense::map<const RE::NiAVObject*, const void*> rootReference;
		ankerl::unordered_dense::map<const void*, const RE::NiAVObject*> referenceRoot;
	};
}
