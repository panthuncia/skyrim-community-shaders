#pragma once

// Hooks that make DCLF the only writer of fade state on the roots it lists.
// The engine's fade writers skip those roots, and the GPU's results are written back at Main::Update's entry.
// The animation job's fade updates are queued so FadeStateCS can repeat them.

#include "Features/DrawcallLimitFix/Features/Fades/FadeRecords.h"

#include <cstddef>
#include <cstdint>
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
	 * @brief Render thread. The animation job's fade updates since the last list jobs, which FadeStateCS repeats before its cull test.
	 * Swapped into a_roots, whose old storage the next drain fills.
	 */
	void TakeAnimatedBatch(std::vector<AnimatedFadeRoot>& a_roots, AnimatedFadeInputs& a_inputs);
	/** @brief Render thread. Keeps the batch so the CPU stage's observer frame can step it a frame later. Swapped: a_roots gets old storage. */
	void KeepAnimatedBatchForObserver(std::vector<AnimatedFadeRoot>& a_roots, const AnimatedFadeInputs& a_inputs);
	void TakeObserverAnimatedBatch(std::vector<AnimatedFadeRoot>& a_roots, AnimatedFadeInputs& a_inputs);
	/** @brief The batch's updates counted per root slot of the frame's tables. Slots are sorted and unique. */
	struct AnimatedStamps
	{
		std::vector<std::uint32_t> slots;
		std::vector<std::uint32_t> counts;  // per entry of slots
	};
	/**
	 * @brief Counts a_roots per slot of the frame's tables, into a_stamps. A slot counts only if the tables still list the same node at
	 * the same generation, since the slots were resolved against the writer tables. Render thread.
	 */
	void CountAnimatedRoots(std::span<const AnimatedFadeRoot> a_roots, AnimatedStamps& a_stamps);
	/** @brief Render thread. Swaps in the batch's counts if they were made before the list jobs' wait. */
	bool TakeAnimatedStamps(AnimatedStamps& a_stamps);
	/**
	 * @brief Render thread, right before it waits for the list jobs. Turns the queued updates into the batch and counts it while the
	 * job threads cull. Nothing pushes between the frame start and the list jobs' end.
	 */
	void PrepareAnimatedFades();
	/** @brief Render thread, when the list jobs finish. Adds updates queued during the wait, which are then counted at the commit. */
	void DrainAnimatedFades();
	/**
	 * @brief Render thread. Lets the CPU fade stage's producer do the drain instead. This is safe because nothing pushes
	 * until the frame's sampling is done, and Main::Update's entry waits for the drain.
	 */
	void HandAnimatedDrainToWorker();
	/** @brief CPU fade stage producer, after a hand-off. */
	void TakeAnimatedBatchOnWorker(std::vector<AnimatedFadeRoot>& a_roots, AnimatedFadeInputs& a_inputs);
	/** @brief Drains and drops a handed-off batch whose producer never ran. */
	void DropAnimatedDrain();
	void WaitAnimatedDrain();

	/**
	 * @brief Distance at which the engine's fade value drops below OnVisible's fade-out threshold.
	 * Positive values scale with the camera's LOD factor. Negative values have a constant folded in. 0 means never by distance.
	 */
	float FadeDistanceOf(const SceneCapture::NodeRecord& a_root);
}
