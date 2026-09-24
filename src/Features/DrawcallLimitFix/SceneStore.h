#pragma once

#include <array>
#include <map>
#include <vector>

#include "AsyncWorker.h"
#include "LightingDescriptors.h"
#include "ConstantEvaluator.h"
#include "Lookups.h"
#include "Records.h"

namespace DCLF
{
	/** @brief Attributes the time since the last call to one BuildPart (SceneStore.cpp; CS_DCLF_PROFILE). */
	struct PartTimer;

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
		Skinning,   // skinned objects: the engine's palette update and the row copy
		Count
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(BuildPart::Count)> kBuildPartNames{
		"walk", "pass-lookup", "classify-static", "classify-frame", "diagnostics",
		"resolve", "dedup", "pipeline-eval", "material-eval", "record",
		"dedup-hit", "loop-tail", "skinning"
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
			// Parallel to materials: a session-unique number, new whenever the slot's record is (re)written - a
			// first evaluation, or a stale record replaced by validation - but not when RefreshMaterialPatch writes
			// the frame's floats into it. A consumer that kept something derived from the record compares this
			// instead of the record's 2.3 KB.
			std::vector<std::uint32_t> materialVersion;
			std::vector<ObjectShading> shading;                   // parallel to objects
			// Linear Lighting's per-object emissive multiplier (LLPerGeometry, PS b8). It lives here rather
			// than being read off the property in the epoch because it is animated, so it has to be sampled
			// at the same point as the emissive colour that already folds it in - which is why it is written
			// by MakeShading and resampled by RefreshFrameConstants.
			std::vector<float> emissiveMult;                      // parallel to objects
			std::vector<ObjectLights> lights;                     // parallel to objects
			// Tree animation, per object. Only technique 12 fills it; everything else leaves the engine's
			// defaults, which is what the template block already carried for them.
			std::vector<ObjectTreeAnim> treeAnim;                 // parallel to objects
			// Advanced Skin's SkinPerGeometry (PS b7): the owning actor's sweat, water wetness, height and water depth,
			// Skin::GetWetness, which its SetupGeometry hook binds for every Lighting draw. Zero for everything not owned
			// by an actor (actorObjects lists those that are); refreshed every frame by RefreshFrameConstants.
			std::vector<std::array<float, 4>> skinWetness;        // parallel to objects
			std::vector<std::uint32_t> actorObjects;
			// Skins of several partitions (CS_DCLF_SKIN_PARTITIONS): bit i draws partition i, walking the
			// geometry slots' nextPartition links from the object's geometryIndex (partition 0). 0 for every
			// other object, which draws its one geometry. The scene phase sets it from the fade node's LOD level
			// for the shadow views; the accumulate phase replaces it from the registered pass for the main camera.
			std::vector<std::uint8_t> skinPartitions;             // parallel to objects
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
			// Decals (CS_DCLF_DECALS): each decal object's slot in its group's draw range, in the engine's
			// own draw order (group, technique bucket, batch list, chain position), and how many slots
			// each group has. The colour epoch writes a decal's sequence to its slot - either the draw or
			// a zero-count one when culled - so that overlapping decals land in the same order every
			// frame, which an atomic append cannot promise. ~0u for everything that is not a decal.
			std::vector<std::uint32_t> decalOrdinal;  // parallel to objects
			std::array<std::uint32_t, 2> decalCount{};
			// Skinning (CS_DCLF_SKINNED): every skinned object's bone palette rows end to end - the engine's
			// own NiSkinInstance::boneMatrices (three float4 rows a bone, absolute world space), copied
			// after its per-frame update - and the previous frame's palettes in the same layout. The epoch
			// packs both eye-relative into its bones buffer, current rows first. Per object, where its rows
			// start and how many; 0 for anything that is not skinned.
			std::vector<float> bones;
			std::vector<float> previousBones;
			std::vector<std::uint32_t> boneOffset;  // parallel to objects, in rows
			std::vector<std::uint32_t> boneRows;    // parallel to objects
			// Per-object extras (Records.h kExtraRows): the landscape blend parameters and the ProjectedUV
			// matrix and pixel parameters, filled at Prepass by RefreshFrameConstants for the objects that
			// carry kObjectLandBlend / kObjectProjectedUV. Rows of float4; per object the row offset, or
			// kNoExtraRows. The epoch appends them to the row buffer after the palettes.
			std::vector<float> extraRows;
			std::vector<std::uint32_t> extraOffset;  // parallel to objects
			// The Utility technique each object casts with, without a view's mode bits (ShadowViews.h:
			// ShadowUtilityTechnique), and why the engine would not draw it into a shadow map. Both are
			// decided by the scene phase, because every shadow view is drawn before the accumulate phase
			// runs. 0 and ShadowReject::NotLighting for an object that is not a caster.
			std::vector<std::uint32_t> shadowTechnique;  // parallel to objects
			std::vector<std::uint8_t> shadowReject;      // parallel to objects (ShadowReject)
			// The bound of the object's entry in the sun's full-frustum culling processes (their objectArray,
			// which the cascade culls walk): centre and radius, absolute world space; a negative radius when the
			// entry is never tested (an actor's, whose entry is its cell's container). SceneStore::SunEntryOf.
			std::vector<std::array<float, 4>> sunEntry;  // parallel to objects
			// NPC face shapes (Tracked::faceShape): per face object its positions in the snapshot the walk took
			// (FaceSnapshots::Shape), valid until the next walk, and the region of the positions buffer they go to.
			// The shadow epoch uploads a region when its generation changed, and binds it as the second stream.
			struct FaceStream
			{
				std::uint32_t object = 0;
				std::uint32_t region = kNoFaceRegion;  // first vertex in the positions buffer
				std::uint32_t vertexCount = 0;
				std::uint64_t generation = 0;
				const float* positions = nullptr;
			};
			std::vector<FaceStream> faceStreams;
			std::vector<std::uint32_t> faceStream;  // parallel to objects: index in faceStreams, or kNoFaceStream
			// What an alpha-tested caster's shadow draw samples: its material's diffuse view, and the material
			// as the key its binding record is shared under. Read off the property here; the shadow epoch's
			// build reads only the material's texture transform, which shader-property controllers
			// (BSLightingShaderPropertyFloatController) move between Main::Draw, where this walk starts, and
			// BeforeShadowMaps (the async scene probe saw a scrolling UV one frame behind). Null otherwise.
			std::vector<ID3D11ShaderResourceView*> shadowDiffuse;    // parallel to objects
			std::vector<const RE::BSShaderMaterial*> shadowMaterial;  // parallel to objects
			// The distinct diffuse views among them (a few hundred), so the lookups are refreshed per view
			// rather than per caster.
			std::vector<ID3D11ShaderResourceView*> shadowTextureSet;
			ankerl::unordered_dense::set<ID3D11ShaderResourceView*> shadowTextureSeen;
			// The distinct shadow pipelines the frame's casters need, without a view's mode bits: a
			// handful in practice (twelve techniques in the Whiterun exterior). What the shadow programs
			// are compiled for, and what the shadow pipelines are built from once a view's mode is known.
			std::vector<ShadowPipelineKey> shadowKeysUsed;

			/**
			 * @brief The three shared tables keep their slots across frames (CS_DCLF_DERIVED_CACHE).
			 *
			 * A slot is alive while its lastUsed frame is not kSlotFree, used this frame when it equals the
			 * frame, and swept (map entry erased, slot on the free list) after kSlotIdleFrames without use.
			 * The sweep runs before the loop, so no object of the frame can point at a slot it reuses. The
			 * keys are kept per slot so the sweep can find the map entry, and so a cached slot index can be
			 * checked against what it was derived for.
			 */
			static constexpr std::uint32_t kSlotFree = ~0u;
			static constexpr std::uint32_t kSlotIdleFrames = 64;
			std::vector<std::uint32_t> geometryLastUsed;  // parallel to geometries
			std::vector<const RE::BSGraphics::TriShape*> geometrySlotKey;
			std::vector<std::uint32_t> geometryFree;
			std::vector<std::uint32_t> pipelineLastUsed;  // parallel to pipelines
			std::vector<std::uint32_t> pipelineFree;
			std::vector<std::uint32_t> materialLastUsed;  // parallel to materials
			std::vector<std::pair<const RE::BSShaderMaterial*, std::uint32_t>> materialSlotKey;
			std::vector<std::uint32_t> materialFree;
			bool PipelineUsed(std::size_t a_slot, std::uint32_t a_frame) const { return a_slot < pipelineLastUsed.size() && pipelineLastUsed[a_slot] == a_frame; }
			bool PipelineAlive(std::size_t a_slot) const { return a_slot < pipelineLastUsed.size() && pipelineLastUsed[a_slot] != kSlotFree; }

			/** @brief Drops the per-object arrays; the slot tables persist. */
			void ClearFrame();
			/** @brief Drops everything. */
			void Clear();
		};

		struct Stats
		{
			std::uint32_t tracked = 0;
			std::uint32_t objects = 0;
			// Of those objects, the ones the engine's main-camera accumulator also holds this frame. The
			// remainder are candidates the engine culled, which the GPU culling is measured against.
			std::uint32_t nativeVisible = 0;
			// The sun's shadow mask (ShadowDir and DefShadow, pass descriptor bits 13 and 14): which objects'
			// descriptors carry it. Only an accumulated pass has those bits; a derived descriptor never does.
			std::uint32_t nativeShadowMasked = 0;   // accumulated, with both bits
			std::uint32_t derivedDescriptors = 0;   // not accumulated: the property derivation, no shadow bits
			std::uint32_t geometries = 0;
			std::uint32_t pipelines = 0;
			std::uint32_t materials = 0;
			std::uint32_t shadowMaskPipelines = 0;  // pipelines whose technique binds the shadow mask (not derived yet)
			std::uint32_t categoryNodes = 0;
			std::uint64_t attachedEvents = 0;
			std::uint64_t detachedEvents = 0;
			std::uint64_t validationDrops = 0;
			std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> ineligible{};
			// Of those, the ones the engine ITSELF drew in the main pass this frame - it registered a
			// lighting pass for them and DCLF declined it. This is what a coverage class is worth.
			//
			// Tracked count is not: it counts objects the engine culled as well, and ranking by it is what
			// made BSEffectShader look like the largest prize at 3057 when it draws ~13 a frame.
			std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> ineligibleDrawn{};
			// Decal candidates this frame, by group (Records.h ObjectDecalGroup - 1).
			std::array<std::uint32_t, 2> decals{};
			// CS_DCLF_FADING: objects given bindings with the screen-door fade (AdditionalAlphaMask), summed over
			// frames until the report takes them (TakeFadingDrawn), since fades are brief.
			std::uint32_t fadingDrawn = 0;
			std::uint32_t fadingFrames = 0;  // frames in that sum with at least one
			std::uint32_t skinned = 0;  // skinned candidates this frame, and their palette rows
			std::uint32_t boneRows = 0;
			std::uint32_t projectedUV = 0;  // candidates with the ProjectedUV bit, and terrain ones
			std::uint32_t landBlend = 0;
			// CS_DCLF_DERIVED_CACHE: accumulated objects served from their cached derivation, and under
			// `probe` how many were recomputed and how many disagreed (the gate: 0).
			std::uint32_t derivedHits = 0;
			std::uint32_t derivedChecked = 0;
			std::uint32_t derivedDiffers = 0;
			std::uint32_t slotsSwept = 0;
			std::uint32_t geometriesRefreshed = 0;
			std::uint32_t slotViolations = 0;  // objects whose slots failed CheckObjectSlots (the gate: 0)
			std::uint32_t shadowCasters = 0;  // records the engine would draw into a shadow map
			std::array<std::uint32_t, 16> shadowRejects{};  // by ShadowReject, over the frame's records
			// Objects the engine accumulated that the scene phase had left out of the tables, so the frame
			// cannot draw them. One frame of staleness at most (the verdict is cleared for them); the gate
			// is 0 in steady state.
			std::uint32_t accumulatedWithoutRecord = 0;  // slots re-resolved in place: TriShape reallocated at its address, or references evicted
			std::uint32_t geometriesAlive = 0, pipelinesAlive = 0, materialsAlive = 0;
			// CS_DCLF_CLASSIFY_CACHE: objects served from the cached verdict, and - under `probe` - how
			// many were recomputed and how many disagreed. Zero disagreements is the gate.
			// Objects left native by Ineligible::Technique, by technique id (index 63 = refraction). This
			// is what says which technique to bring into coverage next, instead of guessing.
			std::array<std::uint32_t, 64> techniqueRejects{};
			// CS_DCLF_COVERAGE_PROBE=1: objects left native by NotLightingShader, by property type. The
			// class is the largest one outside coverage and "not a lighting property" says nothing about
			// which shader would have to be brought in.
			std::map<const RE::NiRTTI*, std::uint32_t> propertyRejects;
			// Of those, how many are alpha blended. This is the scoping question for a second shader:
			// blended geometry is not drawn in the pass DCLF owns at all, so covering it would mean a new
			// epoch after the deferred composite rather than another shader inside the existing one.
			std::uint32_t rejectedBlended = 0;
			std::uint32_t rejectedOpaque = 0;
			std::uint32_t rejectedOpaqueAlphaTest = 0;
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
			std::uint32_t materialCacheEntries = 0;
			std::uint32_t materialCacheEvicted = 0;
			// MaterialSources: materials written this frame, and what became of their slots; the live
			// evaluations the frame-sourced components were taken from (one per signature).
			std::uint32_t materialWrites = 0;
			std::uint32_t materialsRewritten = 0;
			std::uint32_t materialsDropped = 0;
			std::uint32_t frameMaterialSamples = 0;
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

		/**
		 * @brief Which half of the frame's tables to build.
		 *
		 * The shadow views are drawn before the main camera's passes exist (engine notes: shadow maps),
		 * so the tables are built in two halves. Scene runs before the shadow maps and holds everything
		 * that does not depend on the accumulator - the object records a shadow epoch reads. Accumulate
		 * runs at EarlyPrepass, once the registration jobs have finished, and patches those records with
		 * what the main pass draws them with. Object indices are fixed from Scene onwards.
		 */
		enum class Phase : std::uint32_t
		{
			Scene,
			Accumulate
		};

		/** @brief Rebuilds one half of the CPU tables from the tracked set. */
		void BuildFrame(Phase a_phase);

		/**
		 * @brief CS_DCLF_ASYNC: the scene phase's walk runs on the worker from BeforeShadowMaps; this waits for it
		 * and completes the phase on the render thread (a rebuild inline when the walk met anything only the
		 * render thread may resolve). A no-op when nothing is pending. Every consumer of the scene tables after
		 * BeforeShadowMaps calls it first: AfterShadowMaps, EarlyPrepass, Present.
		 */
		void JoinScenePhase();
		/** @brief Drops a pending walk without completing the phase (teardown, the live toggle): no tables this frame. */
		void AbandonSceneJob();
		/** @brief Whether the scene walk is on the worker: nothing may read the per-object tables until the join. */
		bool ScenePending() const { return static_cast<bool>(sceneJob); }
		/** @brief Bumped when the join replaces the worker's walk with an inline one: anything built from the worker's is stale. */
		std::uint32_t GetSceneRebuilds() const { return sceneRebuilds; }
		/** @brief The `[DCLF] async scene` report line since the last call, or empty. */
		std::string SceneAsyncReport();

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

		/**
		 * @brief The four textures the engine binds for a ProjectedUV draw (pixel slots 3, 8, 10 and 11:
		 * the projected diffuse, normal and detail maps and the projection noise), as SetupGeometry left
		 * them at a native draw. They are globals of the engine, changed only by the ReloadProjectedUVTextures
		 * console command, so one capture stands; it is refreshed by every native projected draw seen.
		 */
		struct ProjectedTextures
		{
			static constexpr std::array<std::uint32_t, 4> kSlots{ 3, 8, 10, 11 };
			std::array<ID3D11ShaderResourceView*, 4> views{};
			bool valid = false;
		};
		void NoteProjectedTextures();
		const ProjectedTextures& GetProjectedTextures() const { return projectedTextures; }

		/** @brief Render flags the native main pass passes to SetupGeometry (learned from native draws). */
		void SetMainPassRenderFlags(std::uint32_t a_flags) { mainPassRenderFlags = a_flags; }
		std::uint32_t GetMainPassRenderFlags() const { return mainPassRenderFlags; }

		/** @brief Drops everything (feature disabled or game unloaded). */
		void Clear();

		const Tables& GetTables() const { return tables; }
		const Stats& GetStats() const { return stats; }
		/** @brief The screen-door fading objects given bindings since the last call, and in how many frames. */
		std::pair<std::uint32_t, std::uint32_t> TakeFadingDrawn()
		{
			const std::pair result{ stats.fadingDrawn, stats.fadingFrames };
			stats.fadingDrawn = stats.fadingFrames = 0;
			return result;
		}
		std::uint32_t GetFrame() const { return frame; }
		/**
		 * @brief The PerMaterial float positions refreshed every frame in the records without a new version
		 * (MaterialSources): the build repacks them into a reused group. PS: the shader object's and the
		 * engine globals' (RefreshFrameMaterials); VS: TexcoordOffset (RefreshTextureTransforms).
		 */
		const std::vector<std::uint32_t>& GetMaterialPatchedFloats() const;
		const std::vector<std::uint32_t>& GetMaterialPatchedVSFloats() const;
		/** @brief Bumped whenever the slot tables are reset or every cached verdict is dropped (InvalidateVerdicts). */
		std::uint32_t GetTablesGeneration() const { return tablesGeneration; }

		/**
		 * @brief The pre-resolved service results an epoch's build reads (Lookups.h). Filled by the render
		 * thread: the pipeline entries at EarlyPrepass, the descriptor entries inside an epoch's preparation.
		 */
		const Lookups& GetLookups() const { return lookups; }
		Lookups& MutableLookups() { return lookups; }

		/**
		 * @brief An NiSwitchNode's own fields, read at their SE/AE offsets (NiSwitchNode::OnVisible, AE
		 * 140d29700): CommonLib declares them after NiNode, whose declared size in a multi-runtime build is
		 * VR's, so its members read the wrong memory. False on VR, which DCLF does not run on.
		 */
		struct SwitchState
		{
			std::uint16_t flags = 0;  // bit 0: the update pass updates only the selected child
			std::int32_t index = -1;
			std::uint32_t revID = 0;
			const std::uint32_t* childRevID = nullptr;
			std::uint16_t childRevCapacity = 0;
		};
		static bool ReadSwitch(const RE::NiSwitchNode& a_switch, SwitchState& a_out);
		// Whether the switch node draws a_child (its direct child on the leaf's path) this frame.
		static bool SwitchSelects(const RE::NiSwitchNode& a_switch, const RE::NiAVObject* a_child);

		/**
		 * @brief The row of the engine's skin-partition LOD table a pass draws with (NiSkinPartition::Unk_25,
		 * AE 140d43a10): LODMode.index + LODMode.singleLevel * 4. For a geometry, the row both of
		 * GetRenderPasses and GetRenderPasses_ShadowMapOrMask give it: a kMeshLOD geometry's fade node LOD
		 * level (+0x152 & 0xF), cumulative; everything else level 3, every LOD byte.
		 */
		static std::uint32_t LodRowOf(const RE::BSGeometry& a_geometry, const RE::BSShaderProperty* a_property);
		static std::uint32_t LodRowOf(const RE::BSRenderPass& a_pass);
		/**
		 * @brief Which partitions of the skin the engine draws with this LOD row (bit i = partition i):
		 * BSDismemberSkinInstance::Unk_25 (AE 140d31f40) first skips a partition whose flag (Data byte 0) is
		 * clear, then NiSkinPartition::Unk_25 applies the table. 0 when it draws none.
		 */
		static std::uint32_t SkinPartitionMask(const RE::NiSkinInstance& a_skin, std::uint32_t a_lodRow);

		/** @brief Index into GetTables().objects for this frame, or -1 when the geometry is not drawn by DCLF. */
		std::int32_t FindObject(const RE::BSGeometry* a_geometry) const;
		/**
		 * @brief For reports: why a tracked geometry has no bindings this frame - the accumulate phase's
		 * verdict if it made one this frame (a_accumulate set), else the scene phase's cached one. None when it
		 * is eligible or not tracked.
		 */
		Ineligible ReasonThisFrame(const RE::BSGeometry* a_geometry, bool* a_accumulate = nullptr) const;

		/** @brief The main camera's batch renderers, as of the last BuildFrame. */
		const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>& GetMainBatchRenderers() const { return mainBatchRenderers; }

		/** @brief True when the geometry sits under a tracked category node (used by coverage checks). */
		bool IsTracked(const RE::BSGeometry* a_geometry) const;

		/** @brief How a tracked geometry came to be tracked (diagnostics: CaptureParity's untracked draws). */
		enum class TrackSource : std::uint8_t
		{
			AttachEvent,       // an attach event's subtree walk
			CategoryAppeared,  // the walk of a category node RefreshCategoryNodes found new
			Rescan,            // the full rescan after a load
		};
		/** @brief The frame a tracked geometry was added, and how; false when it is not tracked. */
		bool GetTrackInfo(const RE::BSGeometry* a_geometry, std::uint32_t& a_frame, TrackSource& a_source) const;
		/** @brief The category node an object hangs under, or null (diagnostics). */
		RE::NiNode* CategoryNodeOf(RE::NiAVObject* a_object) const { return FindCategoryNode(a_object, nullptr); }
		/**
		 * @brief The frame a category node was found, and why the refresh that found it ran (0 signature change,
		 * 1 forced by a detach or rescan, 2 the backstop); false when unknown (diagnostics).
		 */
		bool GetCategoryInfo(const RE::NiNode* a_node, std::uint32_t& a_frame, std::uint8_t& a_cause) const;

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
		/** @brief Every lighting pass the main-camera accumulator holds this frame, by geometry (diagnostics). */
		const ankerl::unordered_dense::map<const RE::BSGeometry*, AccumulatedPass>& GetAccumulatedPasses() const { return accumulatedPasses; }

	private:
		struct Tracked
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			RE::NiNode* categoryNode = nullptr;
			// Ineligible::UnsupportedParent or Billboard for what lies between the leaf and its category node
			// (ParentReason in SceneStore.cpp); Switch when a switch node lies there, which ClassifyFrame
			// decides per frame; else None.
			Ineligible parentReason = Ineligible::None;
			// When and how it was added (diagnostics, GetTrackInfo).
			std::uint32_t trackedFrame = 0;
			TrackSource trackedBy = TrackSource::AttachEvent;

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
			// The property's own type, recorded when the cast is resolved so the coverage probe costs a
			// pointer rather than an RTTI walk. Only meaningful when castResult is null.
			const RE::NiRTTI* castRtti = nullptr;

			/**
			 * @brief The positive derivation, cached (CS_DCLF_DERIVED_CACHE): everything the loop derives
			 * for an accumulated object that is fixed until a witness changes - the descriptors, the static
			 * object flags, the pipeline key and the three table slots. The witnesses are the four pointers
			 * StaticVerdict uses, the fade state, the accumulated pass's technique / sub-pass / hint (the
			 * per-frame bits live in the technique), the interior flag, whether the material alpha is
			 * below one, and the decal bias modes of the frame; a slot is also checked against its key
			 * before it is served, so a swept and reused slot cannot be handed back.
			 */
			struct Derived
			{
				bool valid = false;
				std::uint32_t generation = 0;  // the slot tables' generation the slots belong to
				const RE::BSGraphics::TriShape* triShape = nullptr;
				const RE::BSShaderProperty* property = nullptr;
				const RE::BSShaderMaterial* material = nullptr;
				std::uint8_t fadeState = 0;
				bool interior = false;
				bool alphaBelowOne = false;
				std::uint32_t technique = 0;
				std::uint32_t subPass = 0;
				std::uint32_t hint = 0;
				std::uint32_t biasWitness = 0;
				LightingDescriptors descriptors;
				std::uint32_t staticFlags = 0;
				PipelineKey key{};
				std::uint32_t geometrySlot = ~0u;
				std::uint32_t pipelineSlot = ~0u;
				std::uint32_t materialSlot = ~0u;
			};
			Derived derived;

			/**
			 * @brief The cull-only verdict (CS_DCLF_CULL_INPUT=native): whether the object is a culling
			 * candidate when the engine did not keep it, as the full classification last found it, with
			 * the frame it was found on. Refreshed every kCandidateRefreshFrames; a candidate is tested by
			 * the culling and drawn by nothing, so a verdict a few frames old costs at most a diagnostic.
			 */
			std::uint32_t candidateFrame = 0;  // 0: never classified
			Ineligible candidateReason = Ineligible::None;
			// An NPC face shape: a BSDynamicTriShape under a BSFaceGenNiNode, whose positions are FaceSnapshots'.
			// Resolved once, by the walk.
			bool faceShape = false;
			bool faceShapeResolved = false;
			// Owned by an actor (its GetUserData is an ActorCharacter): Advanced Skin gives its draws the actor's
			// wetness (Tables::skinWetness). Resolved once, by the walk.
			bool actorOwned = false;
			bool actorOwnedResolved = false;
			static constexpr std::uint32_t kCandidateRefreshFrames = 64;
			// The accumulate phase's verdict when it left the object without bindings, and the frame it did so
			// (ReasonThisFrame).
			Ineligible accumulateReason = Ineligible::None;
			std::uint32_t accumulateReasonFrame = 0;
			std::uint32_t skinUpdatedFrame = 0;  // the frame the engine's palette update last ran for it (render thread)
			// Its index in this walk's tables, valid while objectStamp equals SceneStore::objectStamp. Kept here
			// rather than in a geometry -> index map rebuilt by every walk: the map's insert was ~0.12 us an object,
			// and every consumer already has the entry (the accumulate phase) or looks it up by the same key.
			std::uint32_t objectStamp = 0;
			std::uint32_t objectId = 0;
			// The node whose bound decides whether the object is a sun caster candidate (SunEntryOf), resolved
			// once: the scene graph above a tracked geometry does not change while it is tracked.
			const RE::NiAVObject* sunEntryNode = nullptr;
			bool sunEntryResolved = false;
		};
		/**
		 * @brief The object's entry bound for the sun's cascade culls. The cascade cull (FUN_140e305c0) walks
		 * only the entries of the full-frustum culling processes' objectArray, which the full-frustum cull
		 * (FUN_141511f30) fills with the items passing their planes. Measured (CS_DCLF_CASCADE_PROBE, 0 false
		 * rejects): a static reference's entry is its reference root (the topmost ancestor carrying the
		 * geometry's userData); an actor's is its cell's container, never tested; a geometry without a
		 * reference (a terrain block) has its nearest BSMultiBoundNode.
		 */
		static std::array<float, 4> SunEntryOf(Tracked& a_tracked, const RE::BSGeometry& a_geometry);

		void RefreshCategoryNodes(bool a_force = false);
		// The signature the category set was last rebuilt for, and how many Presents it has been
		// unchanged; the backstop rebuild exists for anything the signature cannot see.
		std::uint64_t categorySignature = 0;
		std::uint32_t categoryIdleFrames = 0;
		RE::NiNode* FindCategoryNode(RE::NiAVObject* a_object, Ineligible* a_parentReason) const;
		void AddSubtree(RE::NiAVObject* a_root);
		void AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason);
		void ValidateSlice();
		void FindLightingShader();
		void CollectAccumulatedPasses();
		void AddAccumulatedPass(const RE::BSGeometry* a_geometry, const AccumulatedPass& a_pass);
		/** @brief Step B gate: the captured registrations against what the accumulator walk found. */
		/** @brief The main camera's batch renderers, for the capture hook's filter. Cheap; every frame. */
		bool RefreshMainBatchRenderers();
		/** @brief Fills accumulatedPasses from the capture; compares it against the walk when asked. */
		void CompareCapturedPasses(bool a_compare);
		/** @brief A cheap hash of everything RefreshCategoryNodes reads to find category nodes. */
		std::uint64_t CategorySignature() const;
		// a_accumulated: the frame's registered pass, whose captured fade state then stands in for the live one.
		Ineligible ClassifyFrame(const Tracked& a_tracked, const AccumulatedPass* a_accumulated = nullptr) const;
		/**
		 * @brief The nodes whose kHidden bit the engine flips while the asynchronous walk runs, with the bit the
		 * walk's views (the main camera and the sun) see. Taken on the render thread just before the walk is kicked,
		 * sorted by pointer; ClassifyFrame reads a listed node's bit from here instead of from the node.
		 *
		 * - ShadowSceneNode::OnVisible hides a portal graph's always-render children and its shared node for the
		 *   room traversal, then restores each bit. It runs in the main camera's cull jobs.
		 * - TESWaterReflections::Update hides the player's 3D while a cube-map reflection updates, then restores
		 *   it. Main::Draw calls it on the render thread while the cull jobs run.
		 * - Main::Draw hides the player's first-person skeleton just after the walk is kicked and keeps it hidden
		 *   for every world view: it is listed as hidden.
		 *
		 * Reading the nodes instead took the player's face shapes (classified every frame) out of the tables on
		 * the frames a reflection updated.
		 */
		void CaptureCullHiddenBits();
		bool HiddenForWalk(const RE::NiAVObject* a_object) const;
		std::vector<std::pair<const RE::NiAVObject*, bool>> cullHiddenBits;

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
		// The accumulate phase's iteration: the objects it has anything to do, which off the =tracked
		// culling input is the engine's accumulated passes rather than the whole tracked set. A member
		// for its capacity, like `order`.
		std::vector<OrderEntry> accumulateOrder;
		// The decal objects of the frame with their engine draw-order key, sorted after the loop into
		// Tables::decalOrdinal. A member for its capacity, like `order`.
		struct DecalOrderEntry
		{
			std::uint64_t key;  // group, technique, list, chain index - in that significance
			std::uint32_t object;
		};
		std::vector<DecalOrderEntry> decalOrder;
		ankerl::unordered_dense::set<RE::NiNode*> categoryNodes;
		// Diagnostics: the frame each category node was found and the refresh's cause (GetCategoryInfo), and
		// the source AddGeometry stamps on new entries.
		ankerl::unordered_dense::map<const RE::NiNode*, std::pair<std::uint32_t, std::uint8_t>> categoryFound;
		TrackSource addSource = TrackSource::AttachEvent;
		std::size_t validationCursor = 0;
		// Set while a load screen is up, so the first frame after it rebuilds the tracked set from
		// scratch instead of trusting anything discovered across the load (ProcessEvents).
		bool rescanPending = false;

		Tables tables;
		// The walk whose object indices the Tracked entries' objectStamp must match; a new walk or a teardown
		// takes a new value, which invalidates every entry's index at once.
		std::uint32_t objectStamp = 1;
		void InvalidateObjectIndices() { ++objectStamp; }
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
		/** @brief Whether the cross-frame material cache is serving (CS_DCLF_MATERIAL_CACHE). */
		static bool MaterialCacheEnabled();
		/**
		 * @brief The frame-sourced components of every record drawn this frame (MaterialSources): one live
		 * evaluation per signature at Prepass - the shader object's IBLParams, the engine globals, the
		 * character light's t11 - copied into every record of that signature. At Prepass, not EarlyPrepass:
		 * they are the frame's lighting state, and sampling them earlier left IBLParams a fraction of a frame
		 * behind the native draws during a fast lighting transition. Nothing depth-only reads them.
		 */
		void RefreshFrameMaterials();
		/** @brief End of the accumulate phase: this frame's TexcoordOffset into every record drawn this frame. */
		void RefreshTextureTransforms();
		/**
		 * @brief End of the accumulate phase: the materials written since the last frame (MaterialSources).
		 * A slot drawn this frame is re-evaluated; any other slot of such a material is dropped, as is its
		 * cache entry, so its next use evaluates it afresh.
		 */
		void ProcessMaterialWrites();
		ankerl::unordered_dense::set<const RE::BSShaderMaterial*> writtenMaterials;

	public:
		/** @brief The materials this frame's accumulate phase drained as written (diagnostics). */
		const ankerl::unordered_dense::set<const RE::BSShaderMaterial*>& GetWrittenMaterials() const { return writtenMaterials; }

	private:
		/** @brief The alarm: a record that disagrees with a live evaluation outside its frame-sourced components. */
		void NoteStaleMaterial(std::uint32_t a_slot, const std::pair<const RE::BSShaderMaterial*, std::uint32_t>& a_key, const MaterialRecord& a_served,
			const MaterialRecord& a_live);
		std::uint32_t materialValidationCursor = 0;
		static constexpr std::uint32_t kMaterialValidationsPerFrame = 8;
		static constexpr std::uint32_t kMaterialValidationStride = 4;
		static constexpr std::uint32_t kMaterialCacheIdleFrames = 64;
		std::uint32_t frame = 0;
		std::uint32_t tablesGeneration = 0;
		std::uint32_t materialVersions = 0;  // the last Tables::materialVersion handed out
		Lookups lookups;
		// Frame state the scene phase reads once and the accumulate phase reuses, so that both halves of
		// one frame see the same answer even though they run either side of the shadow maps.
		bool sceneBuilt = false;  // the scene phase ran and the records are this frame's
		bool frameResolveBuffers = false;
		bool frameInterior = false;
		std::array<std::uint32_t, 3> frameDecalBias{};
		bool graphWasActive = false;  // resolveBuffers of the previous BuildFrame, to log the flip
		std::uint32_t mainPassRenderFlags = 0;
		ProjectedTextures projectedTextures;
		/** @brief Fills one object's extras rows (Prepass: the main camera's state is current). */
		void RefreshObjectExtras(std::size_t a_object, const RE::BSLightingShaderProperty& a_property, const RE::BSGeometry& a_geometry);
		void BuildScenePhase();
		void BuildAccumulatePhase();
		/**
		 * @brief The scene phase's loop over the tracked set. On the render thread it resolves what it meets; on
		 * the worker (a_renderThread false) it may not call GpuResources or the engine's palette update, and
		 * counts a geometry slot or a skin that would need one as a miss instead - the join rebuilds inline.
		 */
		struct WalkResult
		{
			std::uint32_t geometryMisses = 0;
			std::uint32_t skinMisses = 0;
		};
		WalkResult SceneWalk(bool a_renderThread);
		/** @brief Clears the per-object tables and the walk's per-frame counters. */
		void BeginWalk();
		/**
		 * @brief A face shape's region of the positions buffer (Records.h kFacePositionVertices), kept while the
		 * walks see the shape; kNoFaceRegion when the buffer is full. EndFaceWalk frees the regions of shapes the
		 * walk did not see. The walk's thread alone.
		 */
		std::uint32_t FaceRegionOf(const RE::BSGeometry* a_geometry, std::uint32_t a_vertexCount);
		void EndFaceWalk();
		struct FaceRegion
		{
			std::uint32_t first = 0, count = 0, seenWalk = 0;
		};
		ankerl::unordered_dense::map<const RE::BSGeometry*, FaceRegion> faceRegions;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> faceRegionFree;  // (first, count), sorted, coalesced
		std::uint32_t faceRegionTop = 0;
		std::uint32_t faceWalk = 0;
		/** @brief Before the worker's walk: the render-thread calls it would make - Touch last frame's slots, update last frame's skins. */
		void PrepareSceneJob();
		AsyncWorker::JobHandle sceneJob;
		WalkResult sceneJobResult;
		std::vector<RE::BSGeometry*> skinnedObjects;    // this walk's skinned objects, in object order
		std::vector<RE::BSGeometry*> skinnedLastFrame;  // what PrepareSceneJob updates ahead
		std::vector<std::uint32_t> geometryTouched;     // per geometry slot: the frame PrepareSceneJob touched it
		std::uint32_t sceneRebuilds = 0;
		struct SceneAsync
		{
			std::uint32_t kicked = 0, used = 0, rebuilt = 0, failed = 0;
			std::uint32_t geometryMisses = 0, skinMisses = 0, touchFailures = 0;
			std::uint32_t probeCompared = 0, probeDiffer = 0;
			double waitMs = 0.0, waitMaxMs = 0.0;
		} sceneAsync;
		std::uint32_t AllocateGeometrySlot();
		std::uint32_t AllocatePipelineSlot();
		std::uint32_t AllocateMaterialSlot();
		/**
		 * @brief The geometry slot for a TriShape: found, refreshed in place, or newly resolved.
		 * @return the slot, or Tables::kSlotFree when the buffers cannot be made stable for the graph.
		 */
		std::uint32_t ResolveGeometrySlot(RE::BSGeometry& a_geometry, const RE::BSGraphics::TriShape* a_triShape,
			const RE::NiSkinPartition::Partition* a_skinPartition, PartTimer& a_timer, bool a_renderThread, bool& a_miss);
		/** @brief The cross-frame material cache and its validator; false when nothing can be evaluated. */
		bool EvaluateMaterialForSlot(const RE::BSShaderMaterial* a_material, std::uint32_t a_pass, bool a_cacheOn,
			bool a_probeAll, MaterialRecord& a_record);

		/** @brief Frees the slots unused for kSlotIdleFrames and their map entries; before the loop. */
		void SweepSlots();
		/**
		 * @brief Drops the slot tables, their maps and every cached derivation (by generation): the
		 * teardown paths, where the tables are cleared outright, must not leave a map or a Derived
		 * pointing at slots that no longer exist.
		 */
		void ResetSlotTables();

	public:
		/**
		 * @brief Drops every cached classification verdict and derivation: a live toggle that enters the
		 * classification (Toggles.h) changed, and the caches witness the object rather than the switches.
		 */
		void InvalidateVerdicts();

	private:

		/** @brief After the loop: every object with bindings names live slots of this frame, or is neutralised. */
		void CheckObjectSlots(bool a_resolveBuffers);
		/**
		 * @brief CS_DCLF_SLOT_PROBE: re-derives what the used slots serve (a fresh material evaluation, a
		 * fresh buffer resolve) and logs the first difference of the frame. Startup diagnostics for the
		 * persistent tables; every frame, so it is a probe and not a mode.
		 */
		void ProbeSlots(bool a_resolveBuffers);
		/** @brief Re-evaluates a few live materials a frame against what their slots serve. */
		void ValidateMaterialSlice();
		Stats stats;
	};
}
