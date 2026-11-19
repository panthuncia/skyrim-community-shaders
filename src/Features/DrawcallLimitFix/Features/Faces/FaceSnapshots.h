#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace RE
{
	class NiRefObject;
	class BSDynamicTriShape;
	class BSFaceGenNiNode;
	class BSGeometry;
}

namespace DCLF::Features::Faces
{
	/**
	 * @brief NPC face shape positions, copied out by hooks in the engine's own face morph job.
	 *
	 * This way DCLF never reads BSDynamicTriShape::dynamicData or takes its spin lock. One hook runs after each head's morph job
	 * (0x1404334f3). Another runs after the stage join (0x1406d36fd) to give a first snapshot to heads the stage never queues.
	 * See docs/development/skyrim-engine-notes.md, "Face morphing".
	 *
	 * Each head is triple-buffered, so neither side waits and all shapes of a head come from the same morph run.
	 * A retired record is freed only after a later morph stage completes, because no job outlives a stage join.
	 */
	class FaceSnapshots
	{
	public:
		static FaceSnapshots& Get();

		static bool Enabled();

		/** @brief Main thread only. */
		void Install();
		/** @brief Both hooks or neither. The stage join hook is what frees retired snapshots' engine references. */
		static void Preflight();

		/** @brief float4 per vertex. */
		struct ShapeView
		{
			const float* positions = nullptr;
			std::uint32_t vertexCount = 0;
			std::uint64_t generation = 0;  // unique across all heads
			// Pins the whole head update. The writer never reuses a backing that a view still owns.
			std::shared_ptr<const std::vector<float>> owner;
		};
		struct HeadView
		{
			struct ShapeRange
			{
				std::uint32_t ordinal = 0;
				std::uint32_t offsetFloats = 0;
				std::uint32_t vertexCount = 0;
			};
			// Fresh on every registration or rebuild. The head pointer is only a lookup key, not a lasting identity.
			std::uint64_t recordId = 0;
			std::uint64_t generation = 0;
			std::vector<ShapeRange> shapes;
			std::shared_ptr<const std::vector<float>> owner;
		};

		// The scene walk functions below run on one thread at a time.

		/** @brief Lists heads with a fresh snapshot and heads whose shapes changed, so callers need not visit every head each frame. */
		void BeginWalk(std::vector<const RE::BSFaceGenNiNode*>& a_published, std::vector<const RE::BSFaceGenNiNode*>& a_stale);
		struct ShapeInput
		{
			RE::BSDynamicTriShape* shape = nullptr;  // the caller holds a reference
			std::uint32_t vertexCount = 0;
		};
		/**
		 * @brief Registers the head or rebuilds its record, and returns the shape's positions. Empty while the head has no snapshot.
		 * Then the engine draws the whole head. a_shapes is called only on the head's first shape this walk.
		 */
		ShapeView Shape(const RE::BSGeometry* a_shape, RE::BSFaceGenNiNode* a_head, const std::function<void(std::vector<ShapeInput>&)>& a_shapes);
		/** @brief Hands over freed records' engine references. Release them where a last release is safe, never on the scene work. */
		void TakeReleased(std::vector<RE::NiRefObject*>& a_out);
		/** @brief Like Shape but registers nothing. */
		ShapeView View(const RE::BSGeometry* a_shape, const RE::BSFaceGenNiNode* a_head);
		HeadView HeadSnapshot(const RE::BSFaceGenNiNode& a_head);
		/** @brief Call when the head's last face shape leaves the scene. */
		void Release(const RE::BSFaceGenNiNode* a_head);
		void EndWalk();

		struct Stats
		{
			std::uint32_t heads = 0;           // live records
			std::uint32_t acquired = 0;        // heads with a fresh snapshot this walk
			std::uint32_t withoutSnapshot = 0; // heads the walk saw with none yet
			std::uint32_t rebuilt = 0;         // records rebuilt this walk
			std::uint32_t retired = 0, freed = 0;
		};
		Stats TakeStats();

		/** @brief Safe from any thread. */
		struct WriterStats
		{
			std::uint32_t jobCaptures = 0, stageCaptures = 0, mismatches = 0, stages = 0;
		};
		WriterStats TakeWriterStats();

		~FaceSnapshots();

	private:
		FaceSnapshots();
		struct Impl;
		std::unique_ptr<Impl> impl;
		struct MorphHook;
		struct StageJoinHook;
	};
}
