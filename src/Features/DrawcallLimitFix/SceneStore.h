#pragma once

#include <array>
#include <vector>

#include "LightingDescriptors.h"
#include "ConstantEvaluator.h"
#include "Records.h"

namespace DCLF
{
	/**
	 * @brief The render thread's view of the static scene content Drawcall Limit Fix can draw.
	 *
	 * Only the render thread touches it: ProcessEvents (every Present) applies the SceneTracker's
	 * events and the loaded-cell changes; BuildFrame (at the start of the main pass) refreshes the
	 * per-frame state of every tracked geometry and rebuilds the CPU tables.
	 *
	 * Tracked content is everything under the Static, Dynamic and MultiBound category nodes of
	 * attached cells (engine notes: cell 3D category nodes). Actors, terrain, markers and water live
	 * under the other category nodes and are never tracked.
	 */
	class SceneStore
	{
	public:
		struct Tables
		{
			std::vector<ObjectRecord> objects;
			std::vector<RE::BSGeometry*> objectGeometry;  // parallel to objects
			std::vector<GeometryRecord> geometries;
			std::vector<PipelineKey> pipelines;
			std::vector<MaterialRecord> materials;
			std::vector<ObjectShading> shading;                   // parallel to objects
			std::vector<ObjectLights> lights;                     // parallel to objects
			std::vector<GeometryConstants> geometryConstants;     // parallel to pipelines (per-frame PerGeometry values)
			std::vector<std::uint8_t> geometryConstantsValid;     // parallel to pipelines
			std::vector<TechniqueConstants> techniqueConstants;   // parallel to pipelines (per-frame PerTechnique values, filter modes)
			std::vector<PipelinePermutation> permutations;        // parallel to pipelines
			std::vector<DrawSequence> draws;  // one per object (templates: pipelineIndex is the table index)

			void Clear();
		};

		struct Stats
		{
			std::uint32_t tracked = 0;
			std::uint32_t objects = 0;
			// Of those objects, the ones the engine's main-camera accumulator also holds this frame. The
			// remainder are candidates the engine culled, which the GPU culling is measured against.
			std::uint32_t nativeVisible = 0;
			std::uint32_t geometries = 0;
			std::uint32_t pipelines = 0;
			std::uint32_t materials = 0;
			std::uint32_t shadowMaskPipelines = 0;  // pipelines whose technique binds the shadow mask (not derived yet)
			std::uint32_t categoryNodes = 0;
			std::uint64_t attachedEvents = 0;
			std::uint64_t detachedEvents = 0;
			std::uint64_t validationDrops = 0;
			std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> ineligible{};
			// BuildFrame time by part, summed since the last ResetTimes (ms): accumulator walk, classification,
			// per-pipeline evaluation, material evaluation.
			std::array<double, 4> partMs{};
			// The property-derived descriptor against the accumulated one, outside kRuntimePassBits (Phase 5
			// readiness): objects compared, objects that differ, the differing bits, and objects the
			// derivation would have left native.
			std::uint32_t derivationChecked = 0;
			std::uint32_t derivationDiffers = 0;
			std::uint32_t derivationBits = 0;
			std::uint32_t derivationNative = 0;
		};

		void ResetTimes() { stats.partMs = {}; }

		static SceneStore& Get();

		/** @brief Present-time: follow loaded cells and apply queued scene graph events. */
		void ProcessEvents();

		/** @brief Main-pass start: rebuild the CPU tables from the tracked set. */
		void BuildFrame();

		/** @brief Render flags the native main pass passes to SetupGeometry (learned from native draws). */
		void SetMainPassRenderFlags(std::uint32_t a_flags) { mainPassRenderFlags = a_flags; }
		std::uint32_t GetMainPassRenderFlags() const { return mainPassRenderFlags; }

		/** @brief Drops everything (feature disabled or game unloaded). */
		void Clear();

		const Tables& GetTables() const { return tables; }
		const Stats& GetStats() const { return stats; }
		std::uint32_t GetFrame() const { return frame; }

		/** @brief Index into GetTables().objects for this frame, or -1 when the geometry is not drawn by DCLF. */
		std::int32_t FindObject(const RE::BSGeometry* a_geometry) const;

		/** @brief True when the geometry sits under a tracked category node (used by coverage checks). */
		bool IsTracked(const RE::BSGeometry* a_geometry) const;

		/** @brief True when the object hangs under a drawn category node of an attached cell. */
		bool IsUnderDrawnCategory(RE::NiAVObject* a_object) const { return FindCategoryNode(a_object, nullptr) != nullptr; }

		/** @brief Full eligibility (static and per-frame) of a tracked geometry; NotTriShape if untracked. */
		Ineligible Classify(RE::BSGeometry* a_geometry) const;

		/** @brief Static eligibility of an arbitrary geometry, without the per-frame checks. */
		static Ineligible ClassifyStatic(RE::BSGeometry& a_geometry, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated = nullptr);

		/** @brief The lighting pass the main-camera accumulator holds for a geometry this frame, or null. */
		const AccumulatedPass* FindAccumulatedPass(const RE::BSGeometry* a_geometry) const;

	private:
		struct Tracked
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			RE::NiNode* categoryNode = nullptr;
			bool unsupportedParent = false;
		};

		void RefreshCategoryNodes();
		RE::NiNode* FindCategoryNode(RE::NiAVObject* a_object, bool* a_unsupportedParent) const;
		void AddSubtree(RE::NiAVObject* a_root);
		void AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, bool a_unsupportedParent);
		void ValidateSlice();
		void FindLightingShader();
		void CollectAccumulatedPasses();
		Ineligible ClassifyFrame(const Tracked& a_tracked) const;

		ankerl::unordered_dense::map<RE::BSGeometry*, Tracked> tracked;
		ankerl::unordered_dense::set<RE::NiNode*> categoryNodes;
		std::size_t validationCursor = 0;

		Tables tables;
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> objectIndex;
		// Lighting passes of the main-camera accumulator's batches this frame, by geometry.
		ankerl::unordered_dense::map<const RE::BSGeometry*, AccumulatedPass> accumulatedPasses;
		std::uint32_t frame = 0;
		std::uint32_t mainPassRenderFlags = 0;
		Stats stats;
	};
}
