#pragma once

// Hooks that make DCLF the only writer of fade state on the roots it lists.
// The engine's fade writers skip those roots, and the GPU's results are written back at Main::Update's entry.
// The animation job's fade updates are queued so FadeStateCS can repeat them. The game half drains them into its ticket.

#include "Features/DrawcallLimitFix/Features/Fades/FadeRecords.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF::SceneCapture
{
	struct NodeRecord;
}

namespace DCLF::Features::Fades
{
	/** @brief Main thread, at Main::Update's entry. Publishes the writer tables, then writes the fade stores back. */
	void WriteFadeWriters();
	/**
	 * @brief Whether DCLF is the node's only fade writer this frame. Any thread.
	 * The engine's fade update and OnVisible skip their stores on such nodes.
	 */
	bool FadeWrittenByDclf(const void* a_node);
	/** @brief Any thread. The tables are immutable. */
	bool WrittenByTables(const void* a_tables, const void* a_node);
	/** @brief Engine fade writes skipped since the last call. */
	struct FadeWriterCounts
	{
		std::uint64_t gateSkipped = 0, onVisibleSkipped = 0;
		std::size_t roots = 0;  // roots in the writer set
	};
	FadeWriterCounts TakeFadeWriterCounts();
	/** @brief Like TakeFadeWriterCounts but does not reset. Any thread. */
	FadeWriterCounts PeekFadeWriterCounts();

	/**
	 * @brief One animation-job fade update, resolved against the writer tables. Readers of other tables accept the slot only
	 * if the node and generation still match there, which avoids a hash lookup.
	 */
	struct AnimatedFadeRoot
	{
		const void* node = nullptr;
		std::uint32_t slot = 0;
		std::uint32_t generation = 0;
	};
	/**
	 * @brief The animation job's fade updates of one game update, or of several when a ticket no render took folds into the next.
	 * The game half drains them into its ticket (TicketPart::AnimatedFades); the render side reads the ticket's copy. Immutable.
	 */
	struct AnimatedBatch
	{
		std::vector<AnimatedFadeRoot> roots;
		AnimatedFadeInputs inputs{};
		std::uint64_t serial = 0;  // grows with each batch, folds included
	};
	/**
	 * @brief Render side. The rendered ticket's batch, or null when there is none or this render or an earlier one took it (a repeat).
	 * FadeStateCS's commit and the CPU fade stage's kick both ask, and the first one in a frame gets it.
	 */
	std::shared_ptr<const AnimatedBatch> TakeAnimatedBatch();
	/** @brief Render side. Keeps the batch so the CPU stage's observer frame can step it a frame later. */
	void KeepAnimatedBatchForObserver(std::shared_ptr<const AnimatedBatch> a_batch);
	std::shared_ptr<const AnimatedBatch> TakeObserverAnimatedBatch();
	/** @brief The batch's updates counted per root slot of the frame's tables. Slots are sorted and unique. */
	struct AnimatedStamps
	{
		std::vector<std::uint32_t> slots;
		std::vector<std::uint32_t> counts;  // per entry of slots
	};
	/**
	 * @brief Counts a_roots per slot of the frame's tables, into a_stamps. A slot counts only if the tables still list the same node at
	 * the same generation, since the slots were resolved against the writer tables. Render side.
	 */
	void CountAnimatedRoots(std::span<const AnimatedFadeRoot> a_roots, AnimatedStamps& a_stamps);

	/**
	 * @brief Distance at which the engine's fade value drops below OnVisible's fade-out threshold.
	 * Positive values scale with the camera's LOD factor. Negative values have a constant folded in. 0 means never by distance.
	 */
	float FadeDistanceOf(const SceneCapture::NodeRecord& a_root);
}
