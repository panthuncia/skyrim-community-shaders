#pragma once

// Stands down the engine's tree manager for the trees DCLF stands in for. Included by the feature's sources.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Features::TreeWind::TreeAnimation
{
	/**
	 * @brief Keeps DCLF's trees off the tree manager's animation list (dclf-architecture.md, "Owned trees off the tree manager's
	 * animation list"). BSTreeManager (0x1420F6A18) advances wind for every node on the list (+0x50), but DCLF's trees get wind on
	 * the GPU. They are taken off under the manager's lock (+0x18), and its add, remove and clear are detoured to keep that so.
	 *
	 * The tree list loop (FUN_140437e50) visits a tree only when its multibound's cull result is 1, and no cull reaches a stood-in
	 * tree's multibound. So around the loop each stood-in tree's result is set from DCLF's verdict, then the engine's value is put
	 * back. Main thread, under the tree list's lock (+0x10), each written tree held until restored.
	 */
	void Install();
	bool Installed();
	/** @brief Sets the roots the animation list should not hold, and the tree nodes among them. Main thread. */
	void SetOwned(const std::vector<const RE::NiAVObject*>& a_roots, const std::vector<const RE::NiAVObject*>& a_trees);
	/**
	 * @brief SetOwned by delta. Left is applied first, then joined, so a root in both stays owned.
	 * The animation list is scanned only when a root joined. Main thread.
	 */
	void UpdateOwned(const std::vector<const RE::NiAVObject*>& a_left, const std::vector<const RE::NiAVObject*>& a_joined);
	/** @brief Records at Main::Update's entry whether DCLF runs this frame. Off means no tree is written. */
	void AtUpdateEntry(bool a_running);
	/** @brief Writes the stood-in trees' multibound results from DCLF's verdicts before the tree list loop. AE only. */
	void BeforeListLoop(std::byte* a_manager);
	void AfterListLoop();
	/** @brief Counts owned roots still on the animation list, a missed add. False when not installed. Render thread. */
	bool OwnedOnList(std::uint64_t& a_count);

	/** @brief The counters since the last Take, reset. */
	struct Counters
	{
		bool installed = false;
		std::uint32_t listed = 0, held = 0;  // on the list now, owned trees off it
		float updateMs = 0.0f;               // the manager's own measure of its last update
		std::size_t ownedTrees = 0;
		std::uint64_t engineAdds = 0, engineRemoves = 0, clears = 0;
		std::uint64_t syncs = 0, takenOff = 0, putBack = 0, takenOffAgain = 0;
		std::uint64_t treeLoops = 0, treesIn = 0, treesOut = 0, noMultiBound = 0;
		std::array<std::uint64_t, 3> replaced{};
		std::uint64_t wideIn = 0, wideOut = 0, wideNoVerdict = 0, wideNoMultiBound = 0;
		std::array<std::uint64_t, 3> wideReplaced{};
		std::uint64_t drySame = 0, dryInOut = 0, dryOutIn = 0, dryStale = 0;
	};
	Counters Take();
}
