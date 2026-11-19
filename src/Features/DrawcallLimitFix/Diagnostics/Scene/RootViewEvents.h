#pragma once

// A CategoryRoot view's hook points, observed by the first-person feature's diagnostics. Core announces them and the feature
// keeps the counts, logs and report lines.

#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneView.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

#include <cstdint>
#include <span>
#include <utility>

namespace DCLF::Diagnostics
{
	/**
	 * @brief The root's subtree brought up to now by its events, at a render thread moment with the tree alive. Only with a root.
	 */
	struct RootSubtreeSynced
	{
		const char* moment;
		bool frameStart;
		const RE::NiAVObject* root;
		std::span<const RootSubtreeNode> nodes;
		bool culled;
		std::uint64_t walks, changes;
	};

	/**
	 * @brief An entry under the root view's root, or in its subtree, found hidden at chain depth `depth` by node `key`.
	 * Any pass thread. Pointers only, never dereferenced.
	 */
	struct RootViewHidden
	{
		const Scene::Coordinator& store;
		const void* geometry;
		const void* category;
		const void* root;
		bool sameRoot;
		const void* key;
		std::uint32_t depth;
		bool recordHidden;
		std::span<const std::pair<const RE::NiAVObject*, bool>> cullHidden;
	};

	/** @brief The set parity's commit check left a slot out of a root view, with the waiting reason the commit left. */
	struct SetSlotRedecided
	{
		const Scene::Coordinator& store;
		std::uint32_t slot;
		const SceneView::SlotDecision& decision;
		std::uint8_t waitWhy;
	};
}
