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
	/**
	 * @brief The parts of SceneStore::BuildFrame, each measuring one thing.
	 *
	 * Timed only under CS_DCLF_PROFILE=1. The split exists because the single "classify" bucket it
	 * replaced covered nine unrelated pieces of work, so nothing could be optimised against it.
	 */
	enum class BuildPart : std::uint32_t
	{
		Walk,            // the pass map, built before the loop
		PassLookup,      // FindAccumulatedPass per object
		ClassifyStatic,  // type, skin, property, material alpha, DeriveLightingDescriptors
		ClassifyFrame,   // the parent chain (hidden, actor) and the fade
		Diagnostics,     // the derivation counters, which only feed a log line
		Resolve,         // GpuResources::Resolve for the vertex and index buffers
		Dedup,           // the geometry, pipeline and material map probes
		PipelineEval,    // the new-pipeline body: EvaluateGeometry and EvaluateTechnique
		MaterialEval,    // the new-material body: EvaluateMaterial (the stand-in)
		Record,          // transforms, emittance, room index, shading, the draw
		// The two that "record" used to absorb, which is why it read as the largest part.
		DedupHit,   // the three map probes on the HIT path; Dedup above only ever measured the misses
		LoopTail,   // per TRACKED object: the continue path of a rejected one, and the iteration itself
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(BuildPart::Count)> kBuildPartNames{
		"walk", "pass-lookup", "classify-static", "classify-frame", "diagnostics",
		"resolve", "dedup", "pipeline-eval", "material-eval", "record",
		"dedup-hit", "loop-tail"
	};

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
			// Linear Lighting's per-object emissive multiplier (LLPerGeometry, PS b8). It lives here rather
			// than being read off the property in the epoch because it is animated, so it has to be sampled
			// at the same point as the emissive colour that already folds it in - which is why it is written
			// by MakeShading and resampled by RefreshFrameConstants.
			std::vector<float> emissiveMult;                      // parallel to objects
			std::vector<ObjectLights> lights;                     // parallel to objects
			std::vector<GeometryConstants> geometryConstants;     // parallel to pipelines (per-frame PerGeometry values)
			std::vector<std::uint8_t> geometryConstantsValid;     // parallel to pipelines
			// The property whose lighting pass supplied each pipeline's per-frame constants, kept so
			// RefreshFrameConstants can re-evaluate them once the main camera's state is current.
			std::vector<RE::BSShaderProperty*> geometryTemplate;  // parallel to pipelines
			// Whether each pipeline's template came from an object the engine itself kept. The template
			// supplies the scene light list, so one taken from a culled object hands its lighting to every
			// visible object on that pipeline - the blown-out interior defect. This flag is what lets a
			// later native-visible object take the template over, which replaces the ordering guarantee
			// that a persistent table cannot keep.
			std::vector<std::uint8_t> geometryTemplateNative;     // parallel to pipelines
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
			// CS_DCLF_CLASSIFY_CACHE: objects served from the cached verdict, and - under `probe` - how
			// many were recomputed and how many disagreed. Zero disagreements is the gate.
			std::uint32_t castResolved = 0;  // RTTI casts actually walked (the rest reused a witness)
			std::uint32_t classifyHits = 0;
			std::uint32_t classifyChecked = 0;
			std::uint32_t classifyDiffers = 0;
			// BuildFrame time by part, summed since the last ResetTimes (ms). Only filled under
			// CS_DCLF_PROFILE=1; with it off the loop makes no clock calls at all.
			//
			// The parts used to be four, and the one called "classification" accrued everything since the
			// previous object - both pass lookups, both classify calls, the derivation counters, the buffer
			// resolves, the three dedup probes, the transforms, the room index, the shading and the draw
			// build. Nine things under one label, which is no basis for optimising any of them.
			std::array<double, static_cast<std::size_t>(BuildPart::Count)> partMs{};
			// The property-derived descriptor against the accumulated one (Phase 5 readiness): objects
			// compared, and objects the derivation would have left native.
			//
			// The comparison is reported in two halves, because they mean different things. Outside
			// kRuntimePassBits the derivation is meant to be exact, and a difference is a defect. Inside
			// kRuntimePassBits it is guessing at values GetRenderPasses computes from per-frame light and
			// shadow assignment, so a difference there is expected until those bits are derived properly —
			// and how large it is decides whether GetRenderPasses can ever be skipped outright. The earlier
			// counter masked the runtime half out entirely, which made "0 differ" read as a much stronger
			// result than it was.
			std::uint32_t derivationChecked = 0;
			std::uint32_t derivationDiffers = 0;        // differ outside kRuntimePassBits
			std::uint32_t derivationBits = 0;           // OR of those differing bits
			std::uint32_t derivationRuntimeDiffers = 0;  // differ inside kRuntimePassBits
			std::uint32_t derivationRuntimeBits = 0;     // OR of those differing bits
			// Objects differing per bit position, over the whole descriptor, so a single dominant bit can
			// be told apart from a smear across several.
			std::array<std::uint32_t, 32> derivationBitCounts{};
			std::uint32_t derivationNative = 0;
			// Material evaluations this frame, and how many were skipped because the object cannot be
			// drawn. CS_DCLF_MATERIAL_CACHE=probe additionally reports how many evaluated records came out
			// byte-identical to the previous frame's, which is what decides whether a cross-frame cache is
			// possible at all: SetupMaterial reads per-frame engine state, so it may well not be.
			std::uint32_t materialsEvaluated = 0;
			std::uint32_t materialsSkipped = 0;
			std::uint32_t materialsUnchanged = 0;
			std::uint32_t materialsChanged = 0;
			// Which component of a changed record moved: bit 0 VS floats, 1 PS floats, 2 textures,
			// 3 address modes, 4 filter modes, 5 the written-texture mask.
			std::uint32_t materialDiffMask = 0;
			std::uint32_t materialsFromCache = 0;     // records served without calling SetupMaterial
			std::uint32_t materialsValidated = 0;     // cache entries re-evaluated and compared this frame
			std::uint32_t materialCacheStale = 0;     // of those, ones that disagreed: must be 0
			std::uint32_t materialDriftFloats = 0;    // frame-global floats patched into every served record
			std::uint32_t materialCacheEntries = 0;
			std::uint32_t materialCacheEvicted = 0;
			std::uint32_t materialPatchResamples = 0;  // Prepass resamples of the patched floats
			bool materialDiffLogged = false;
			// The Stage 4c gate, and the invariant it checks:
			//
			//   if any native-visible object draws on pipeline p, then geometryTemplate[p] came from a
			//   native-visible object.
			//
			// Only that implication matters. A pipeline used *solely* by culled candidates is templated by
			// one of them and always will be, because there is no native-visible object on it to elect -
			// and no visible object is harmed by its light list. Counting those as defects reported 12 of
			// 31 under CS_DCLF_CULL_INPUT=tracked and meant nothing.
			//
			// templateDefects is measured from the objects after the loop, not from the election, so it
			// is a real check on the result rather than a restatement of the code that produced it.
			std::uint32_t templateDefects = 0;      // native-visible objects on a culled-templated pipeline; must be 0
			std::uint32_t pipelinesCulledOnly = 0;  // pipelines no native-visible object draws on (informational)
			std::uint32_t templateUpgrades = 0;     // templates a later native-visible object took over
		};

		void ResetTimes() { stats.partMs = {}; }

		/** @brief Whether CS_DCLF_PROFILE=1 - the per-part timing in BuildFrame. Read once. */
		static bool ProfileEnabled();

		static SceneStore& Get();

		/** @brief Present-time: follow loaded cells and apply queued scene graph events. */
		void ProcessEvents();

		/**
		 * @brief Whether a load screen is up, i.e. the scene graph is being rebuilt under us.
		 *
		 * Nothing may walk the scene graph while this holds, and frame counters meant to be comparable
		 * between runs should not advance across it.
		 */
		static bool IsLoadingScreenUp();

		/** @brief Main-pass start: rebuild the CPU tables from the tracked set. */
		void BuildFrame();

		/**
		 * @brief Latches the main camera's accumulator, from a point in the frame where it is identifiable.
		 *
		 * Call where `globals::game::currentAccumulator` is set - it is a *currently rendering* pointer, so
		 * it is null before the main pass begins even though the accumulator itself has been fully built
		 * since before the shadow maps. BuildFrame runs earlier than that now and reads the latch instead.
		 * A change of accumulator is logged once; the pointer is stable in practice.
		 */
		void LatchAccumulator();

		/**
		 * @brief Re-evaluates the per-pipeline per-frame constants against the main camera's state.
		 *
		 * BuildFrame runs at EarlyPrepass so both DCLF epochs share a table generation, but there the
		 * renderer's shadow state still belongs to the shadow-map camera just drawn. Most of what
		 * SetupGeometry writes is per frame, and one of those - EyePosition - is relative to posAdjust,
		 * so evaluating it that early produced the shadow camera's eye and capture parity failed on that
		 * one variable across every draw. The object, geometry and material tables are camera-independent
		 * and stay where they are built; only this is deferred.
		 *
		 * Call from Prepass. The Z-prepass epoch runs in between and so uses the previous frame's values
		 * for these - harmless, because vertex position comes from World, which is patched per object at
		 * epoch time with that epoch's own eye, and never from these.
		 */
		void RefreshFrameConstants();

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
		/** @brief Whether a negative verdict follows only from what the pointer witnesses cover. */
		static bool CacheableVerdict(Ineligible a_reason);
		static Ineligible ClassifyStatic(RE::BSGeometry& a_geometry, LightingDescriptors* a_descriptors, const AccumulatedPass* a_accumulated = nullptr,
			bool a_wantDerived = true, RE::BSLightingShaderProperty** a_castCache = nullptr);

		/** @brief The lighting pass the main-camera accumulator holds for a geometry this frame, or null. */
		const AccumulatedPass* FindAccumulatedPass(const RE::BSGeometry* a_geometry) const;

	private:
		struct Tracked
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			RE::NiNode* categoryNode = nullptr;
			bool unsupportedParent = false;

			/**
			 * @brief A cached "this object cannot be drawn", and the witnesses that keep it honest.
			 *
			 * Only *negative* verdicts are cached. A positive one is not a property of the object: the
			 * descriptors it produces come from the engine's per-frame render pass, so it has to be
			 * derived again each frame. A negative is a property of the object - of its type, its skin
			 * instance, its shader property's flags and its material - and re-deriving it every frame is
			 * the single largest piece of waste in BuildFrame. In the Whiterun exterior 6097 of 9022
			 * tracked objects produce one, every frame, and it is thrown away.
			 *
			 * The witnesses are pointers that change when the thing behind them does, compared in full
			 * every frame. Cheap, and they close the one dangerous case: a stale *positive* over freed
			 * renderer data is a device loss, which is why positives are not cached at all and why
			 * rendererData is witnessed even so - a negative that became stale the other way would
			 * silently leave an object native forever.
			 *
			 * Entry lifetime covers attach, detach, cell teardown and the post-load rescan, because the
			 * whole Tracked is destroyed. ValidateSlice covers what has no cheap witness.
			 */
			struct StaticVerdict
			{
				bool cached = false;
				Ineligible reason = Ineligible::None;
				const RE::BSGraphics::TriShape* rendererData = nullptr;
				const RE::BSShaderProperty* property = nullptr;
				const RE::BSShaderMaterial* material = nullptr;
				std::uint8_t fadeState = 0;
			};
			StaticVerdict verdict;

			/**
			 * @brief The RTTI cast result, remembered against the property pointer that produced it.
			 *
			 * `netimmerse_cast` walks a chain of RTTI pointers. They are pointers, not strings, so it is
			 * cheap in instructions - but every step is a dependent load into a different allocation, and
			 * at ~3000 classifications a frame those misses are the one part of ClassifyStatic that is
			 * neither computation a memo can remove nor memory BuildFrame re-reads later anyway. Whether
			 * a property is a BSLightingShaderProperty is a property of its type, so the pointer is a
			 * complete witness.
			 */
			const RE::BSShaderProperty* castProperty = nullptr;
			RE::BSLightingShaderProperty* castResult = nullptr;
		};

		void RefreshCategoryNodes(bool a_force = false);
		// The signature the category set was last rebuilt for, and how many Presents it has been
		// unchanged; the backstop rebuild exists for anything the signature cannot see.
		std::uint64_t categorySignature = 0;
		std::uint32_t categoryIdleFrames = 0;
		RE::NiNode* FindCategoryNode(RE::NiAVObject* a_object, bool* a_unsupportedParent) const;
		void AddSubtree(RE::NiAVObject* a_root);
		void AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, bool a_unsupportedParent);
		void ValidateSlice();
		void FindLightingShader();
		void CollectAccumulatedPasses();
		/** @brief Step B gate: the captured registrations against what the accumulator walk found. */
		/** @brief The main camera's batch renderers, for the capture hook's filter. Cheap; every frame. */
		bool RefreshMainBatchRenderers();
		/** @brief Fills accumulatedPasses from the capture; compares it against the walk when asked. */
		void CompareCapturedPasses(bool a_compare);
		/** @brief A cheap hash of everything RefreshCategoryNodes reads to find category nodes. */
		std::uint64_t CategorySignature() const;
		Ineligible ClassifyFrame(const Tracked& a_tracked) const;

		ankerl::unordered_dense::map<RE::BSGeometry*, Tracked> tracked;
		/**
		 * @brief BuildFrame's iteration order: the objects the engine kept, then the rest.
		 *
		 * Members rather than locals so their capacity survives the frame, and each entry carries the
		 * accumulated pass it was found under so the loop does not look it up a second time.
		 */
		struct OrderEntry
		{
			RE::BSGeometry* geometry;
			Tracked* tracked;  // mutable: BuildFrame updates the cached verdict through it
			const AccumulatedPass* accumulated;
		};
		std::vector<OrderEntry> order;
		ankerl::unordered_dense::set<RE::NiNode*> categoryNodes;
		std::size_t validationCursor = 0;
		// Set while a load screen is up, so the first frame after it rebuilds the tracked set from
		// scratch instead of trusting anything discovered across the load (ProcessEvents).
		bool rescanPending = false;

		Tables tables;
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> objectIndex;
		// The per-frame dedup maps. Members, not locals, so their buckets survive the frame: as locals
		// they were three hash maps allocated and freed every frame to hold the same contents, which is
		// the waste Stage 1 removed from the ordering vectors and left here. They are cleared, reserved
		// and refilled by BuildFrame; nothing outside it may read them.
		ankerl::unordered_dense::map<const RE::BSGraphics::TriShape*, std::uint32_t> geometryIndex;
		ankerl::unordered_dense::map<PipelineKey, std::uint32_t, PipelineKeyHash> pipelineIndex;
		ankerl::unordered_dense::map<std::pair<const RE::BSShaderMaterial*, std::uint32_t>, std::uint32_t> materialIndex;
		// Lighting passes of the main-camera accumulator's batches this frame, by geometry.
		ankerl::unordered_dense::map<const RE::BSGeometry*, AccumulatedPass> accumulatedPasses;
		// The accumulator CollectAccumulatedPasses last saw non-null (Step A probe).
		RE::BSGraphics::BSShaderAccumulator* latchedAccumulator = nullptr;
		// The batch renderers the main accumulator draws from, refreshed by CollectAccumulatedPasses.
		// Registration is captured from every batch renderer in the game, including the shadow cameras',
		// so the captured set has to be filtered to these before it can be compared or used.
		ankerl::unordered_dense::set<const RE::BSBatchRenderer*> mainBatchRenderers;
		// CS_DCLF_MATERIAL_CACHE=probe only: the previous frame's record per (material, pass descriptor).
		// It holds a reference on the material so a freed one cannot be mistaken for a new allocation at
		// the same address (BSShaderMaterial is BSIntrusiveRefCounted).
		struct MaterialProbe
		{
			RE::BSTSmartPointer<RE::BSShaderMaterial> material;
			MaterialRecord record;
			std::uint32_t lastUsed = 0;
		};
		// The cross-frame material cache, keyed by (material, pass descriptor). It holds a reference on
		// the material, so a freed one cannot be mistaken for a new allocation at the same address.
		// Unlike the probe it replaced it is swept: entries unused for kMaterialCacheIdleFrames are
		// dropped, because a cell change retires most of its contents at once and nothing else would.
		ankerl::unordered_dense::map<std::pair<const RE::BSShaderMaterial*, std::uint32_t>, MaterialProbe> materialCache;
		// Floats of the PS PerMaterial group that are NOT properties of the material at all: the engine
		// reads them off the BSLightingShader object (variable 29, IBLParams, comes from this+0xcc..0xf0
		// with a day/night selector at this+0xf0), so they hold the same value for every material in a
		// frame and change as the frame's lighting does.
		//
		// The POSITIONS are cumulative for the session and the VALUES are refreshed every frame from one
		// live evaluation. Cumulative is the whole correctness argument. The first version learned the
		// positions afresh each frame as "floats that differ from the cached copy", which works only
		// while the value drifts continuously: `set gamehour to 22` steps it once and then freezes it, so
		// the next frame saw no difference, learned an empty set, patched nothing, and served every
		// material its pre-step value for ever - capture parity failed on 220,500 of 276,300 draws with
		// IBLParams.y reading 0.757 against a native 0.106. Once a position is known to vary it stays in
		// the set, so a step is patched by the same mechanism as a drift.
		std::vector<std::uint32_t> materialPatched;      // cumulative positions
		std::vector<float> materialPatchValues;          // this frame's values, parallel
		bool materialPatchValuesFresh = false;
		void NotePatchedFloat(std::uint32_t a_index);
		/** @brief Whether the cross-frame material cache is serving (CS_DCLF_MATERIAL_CACHE). */
		static bool MaterialCacheEnabled();
		// One (material, pass) the patched floats can be resampled from. They are shader-level, so any
		// material answers for all of them; this just keeps a live one to ask.
		RE::BSTSmartPointer<RE::BSShaderMaterial> materialPatchSource;
		std::uint32_t materialPatchSourcePass = 0;
		/**
		 * @brief Resamples the patched floats at Prepass and rewrites them into every material record.
		 *
		 * BuildFrame runs at EarlyPrepass, and the floats it patches are not material properties at all -
		 * they are the frame's lighting state, read off the BSLightingShader object. Sampling them that
		 * early left them a fraction of a frame behind what the native draws read, which capture parity
		 * saw as IBLParams differing in the sixth decimal during a fast lighting transition. This is the
		 * same reason, and the same place, that the animated per-object shading is resampled.
		 */
		void RefreshMaterialPatch();
		std::uint32_t materialValidationCursor = 0;
		static constexpr std::uint32_t kMaterialValidationsPerFrame = 8;
		static constexpr std::uint32_t kMaterialValidationStride = 4;
		static constexpr std::uint32_t kMaterialCacheIdleFrames = 64;
		std::uint32_t frame = 0;
		std::uint32_t mainPassRenderFlags = 0;
		Stats stats;
	};
}
