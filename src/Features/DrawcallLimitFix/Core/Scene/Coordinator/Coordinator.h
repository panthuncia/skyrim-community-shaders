#pragma once

// The scene coordinator. It owns the tracked set, the scene mirror, the slot tables and the publications the render thread installs.
// It runs on the scene lane, one pass at a time, woken by producers through SceneWake. Under the render-thread parities a pass runs inline.
// The render thread reaches it only through Scene::Channels and the publications.
// Its state is split into private bases, one per header in State/.

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "Features/DrawcallLimitFix/Api/Reports.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/MaterialBindings.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/SharedBindings.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialSources.h"
#include "Features/DrawcallLimitFix/Core/Shading/GeometryPort.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneMirror.h"
#include "Features/DrawcallLimitFix/Api/EntryRanges.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneIdentity.h"
#include <ORGModuleServices/Async/SlotTable.h>
#include <ORGModuleServices/RetirementChain.h>
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "RenderGraph/Async/EventQueue.h"
#include "RenderGraph/Async/LatestSlot.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include <ORGModuleServices/Async/PublicationLog.h>
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/FrameTables.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Lookups.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Eligibility.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Publication.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Core/Scene/Channels.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/EntrySpaceTable.h"

namespace DCLF
{
	namespace Scene
	{
		struct SwitchEvent;
	}
	namespace Diagnostics
	{
		struct SceneView;
	}
	struct FrameGlobals;
	struct PartTimer;
	struct EntrySpace;
	struct PipelineCatalog;
}

#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Timing.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Tracked.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Mirror.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Dependents.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Categories.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Attach.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Candidates.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Placements.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Hidden.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Events.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Shading.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Frame.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Slots.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Materials.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Bindings.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Walk.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Joins.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Residents.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Components.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/ShadowSets.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Set.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Publication.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/State/Pump.h"

namespace DCLF
{
	namespace Scene
	{
		class Coordinator;

		/** @brief Counts a fade root that has no mirror record. */
		void NoteFadeRootUnmirrored();
	}

	/**
	 * @brief The scene work: tracked geometry records, kept current from engine events and published for the frame.
	 *
	 * Engine references it lets go of go through the retirement chain to the retire queue, released on the main thread, never on its threads.
	 * It tracks everything under attached cells' Static, Dynamic, MultiBound and Actor nodes. Terrain, markers and water are not tracked.
	 */
	class Scene::Coordinator :
		private CoordinatorTracked,
		private CoordinatorMirror,
		private CoordinatorDependents,
		private CoordinatorCategories,
		private CoordinatorAttach,
		private CoordinatorCandidates,
		private CoordinatorPlacements,
		private CoordinatorHidden,
		private CoordinatorEvents,
		private CoordinatorShading,
		private CoordinatorFrame,
		private CoordinatorSlots,
		private CoordinatorMaterials,
		private CoordinatorBindings,
		private CoordinatorWalk,
		private CoordinatorJoins,
		private CoordinatorResidents,
		private CoordinatorComponents,
		private CoordinatorShadowSets,
		private CoordinatorSet,
		private CoordinatorPublication,
		private CoordinatorPump
	{
		friend struct Diagnostics::SceneView;
		friend class Api::CoordinatorComponentContext;

	public:
		using Tables = SceneTables;

		struct Stats
		{
			std::uint32_t tracked = 0;
			std::uint32_t objects = 0;
			std::uint32_t geometries = 0;
			std::uint32_t pipelines = 0;
			std::uint32_t materials = 0;
			std::uint32_t shadowMaskPipelines = 0;  // pipelines whose technique binds the shadow mask (not derived yet)
			std::uint32_t categoryNodes = 0;
			std::uint64_t attachedEvents = 0;
			std::uint64_t detachedEvents = 0;
			std::uint64_t detachMoves = 0;  // detached geometries attached again in the same batch (kept, not erased)
			std::uint64_t validationDrops = 0;
			std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> ineligible{};
			// Screen-door fade bindings, summed until TakeFadingDrawn.
			std::uint32_t fadingDrawn = 0;
			std::uint32_t fadingFrames = 0;  // frames in that sum with at least one
			std::uint32_t skinned = 0;  // skinned candidates this frame, and their palette rows
			std::uint32_t boneRows = 0;
			std::uint32_t projectedUV = 0;  // candidates with the ProjectedUV bit, and terrain ones
			std::uint32_t landBlend = 0;
			std::uint32_t derivedHits = 0;
			std::uint32_t slotsSwept = 0;
			std::uint32_t geometriesRefreshed = 0;
			std::uint32_t geometriesRelocated = 0;  // slots made stale by a move of their buffers (pinless imports), since the start
			// Ineligible::UnstableBuffer objects by UnstableCause.
			std::array<std::uint32_t, 3> unstableCauses{};
			std::uint32_t slotViolations = 0;  // objects whose slots failed CheckObjectSlots (the gate: 0)
			std::uint32_t shadowCasters = 0;  // records the engine would draw into a shadow map
			std::array<std::uint32_t, 16> shadowRejects{};  // by ShadowReject, over the frame's records
			std::uint32_t geometriesAlive = 0, pipelinesAlive = 0, materialsAlive = 0;
			// Objects left native by Ineligible::Technique, by technique id. 63 is refraction.
			std::array<std::uint32_t, 64> techniqueRejects{};
			std::uint32_t classifyHits = 0;
			// In ms. Measured only with CS_DCLF_PROFILE, so the loop makes no clock calls otherwise.
			std::array<double, static_cast<std::size_t>(BuildPart::Count)> partMs{};
			std::array<double, static_cast<std::size_t>(BuildPart::Count)> accumulatePartMs{};
			std::array<double, static_cast<std::size_t>(ScenePart::Count)> scenePartMs{};
			std::array<double, static_cast<std::size_t>(ScenePart::Count)> scenePartFrameMs{};
			std::array<double, static_cast<std::size_t>(ScenePart::Count)> scenePartMaxMs{};
			std::array<std::uint64_t, 8> lightByTrait{};
			std::uint64_t lightPlaced = 0, lightSkins = 0;
			std::uint64_t moveEvents = 0;
			std::uint64_t perFrameRelookups = 0;
			std::uint64_t hiddenEvents = 0, verdictsChecked = 0, verdictsSkipped = 0;
			std::uint64_t hiddenRetaken = 0;  // statics classified again for a hidden event on their chain
			std::uint64_t hiddenAheadAnnounced = 0;  // hidden events announced again at a capture newer than them (hiddenAhead)
			// CS_DCLF_PROFILE only.
			std::array<double, static_cast<std::size_t>(EvaluateKind::Count)> evaluateKindMs{};
			std::array<std::uint64_t, static_cast<std::size_t>(EvaluateKind::Count)> evaluateKindCount{};
			std::uint32_t materialsEvaluated = 0;
			std::uint32_t materialCacheEntries = 0;
			std::uint32_t materialCacheEvicted = 0;
			std::uint32_t materialEvictedMember = 0;  // of them, slots a membership resident had been bound to (materialMember)
			std::uint32_t materialWrites = 0;
			std::uint32_t materialsRewritten = 0;
			std::uint32_t materialsHeld = 0;
			std::uint32_t frameMaterialSamples = 0;
		};

		void ResetTimes()
		{
			stats.partMs = stats.accumulatePartMs = {};
			stats.scenePartMs = {};
			stats.scenePartMaxMs = {};
			stats.lightByTrait = {};
			stats.lightPlaced = stats.lightSkins = 0;
			stats.moveEvents = stats.perFrameRelookups = 0;
			stats.hiddenEvents = stats.verdictsChecked = stats.verdictsSkipped = 0;
			stats.hiddenRetaken = stats.hiddenAheadAnnounced = 0;
			stats.evaluateKindMs = {};
			stats.evaluateKindCount = {};
		}

		void EndSceneFrame()
		{
			for (std::size_t i = 0; i < stats.scenePartFrameMs.size(); ++i)
				stats.scenePartMaxMs[i] = std::max(stats.scenePartMaxMs[i], stats.scenePartFrameMs[i]);
			stats.scenePartFrameMs = {};
		}


		static Coordinator& Get();

		/** @brief Render thread. Allows the pump's wakes once DCLF is installed. */
		void StartScenePump();
		/** @brief Render thread. With a_full false a pass applies events only. */
		void SetScenePassMode(bool a_full);
		/** @brief Fallbacks that read what a pass writes must refuse while this is true. */
		bool SceneTaskInFlight() const;
		/** @brief The pump stopped for good and no scene pass runs again. */
		bool ScenePumpFailed() const;
		/** @brief Scene lane only. The fade census as JSON, from the registrations at Api::ReportPoint::CaptureCensus. */
		json FadeCensus();
		/** @brief Scene lane only. The sizes of the coordinator's growable containers, one line each. */
		std::vector<std::string> SceneGrowthLines();

		// A member's accumulated half is patched once from its membership pass and kept until a rewrite, a release or a failed patch.
		/** @brief Ends every membership. Call when the frame globals a membership pass reads change. */
		void EndAllResidency();
		struct ResidentStats
		{
			std::uint64_t joined = 0, failed = 0, rewritten = 0, released = 0, frames = 0, resident = 0;
			std::uint64_t membershipQueued = 0;  // records BindByMembership handed a pass
			std::uint64_t materialWaits = 0, materialsServed = 0;  // joins that waited for a material capture, captures the render thread served
			std::uint64_t materialsStale = 0;  // requests whose property had another material at the serve (the swap's event not applied yet)
			std::uint64_t membershipKept = 0;    // members written again whose binding stands
			std::uint64_t layerUnpaired = 0;     // a base or a layer that joined without the other, and left again
			std::array<std::uint64_t, 4> failedBy{};  // (unused), no record, a frame verdict, material or extras
		};
		ResidentStats TakeResidentStats() { return std::exchange(residentStats, {}); }
		struct AccumulateInputStats
		{
			std::uint64_t ancestorMapsPosted = 0, ancestorMapsTaken = 0, pipelineFramesPosted = 0, pipelineFramesTaken = 0;
			std::uint64_t pipelineBlocksMade = 0, pipelineBlocksWaited = 0;
			std::uint64_t ancestorValuesMoved = 0;
			std::uint64_t ancestorFound = 0, ancestorNoKey = 0, ancestorNotMapped = 0;
		};
		AccumulateInputStats TakeAccumulateInputStats();


		/** @brief Scene writes what does not depend on the main pass. Accumulate patches in what the main pass draws with. Object indices are fixed from Scene on. */
		enum class Phase : std::uint32_t
		{
			Scene,
			Accumulate
		};

		/** @brief Builds one half of the tables from the tracked set. */
		void BuildFrame(Phase a_phase);

		using RevisionRequest = DCLF::RevisionRequest;
		using ScenePublication = DCLF::ScenePublication;
		struct ConstantsPostStats
		{
			std::uint64_t pipelinesApplied = 0, stale = 0;
			std::uint64_t techniquesEvaluated = 0, techniquesWritten = 0, techniqueInputsMoved = 0;
		};
		ConstantsPostStats TakeConstantsPostStats() { return std::exchange(constantsPostStats, ConstantsPostStats{}); }
		struct TablesPublication
		{
			std::uint64_t published = 0, reused = 0, made = 0;
			double ms = 0.0, maxMs = 0.0;
			std::uint64_t materialsReplayed = 0, materialSlots = 0;
			double restMs = 0.0;
			std::uint64_t objectsReplayed = 0, objectSlots = 0, wholeCopies = 0;
			std::uint64_t geometriesReplayed = 0, geometrySlots = 0, geometryWholeCopies = 0;
			std::array<std::uint64_t, Extensions::kTableFamilySlots> familiesKept{};
			std::size_t pool = 0;
			std::uint64_t trimmed = 0;  // free pooled tables let go of past the two kept (TrimTablesPool)
			std::uint64_t lookupsCopied = 0, lookupsShared = 0, lookupsChunks = 0;
			double lookupsMs = 0.0, lookupsMaxMs = 0.0;
		};
		/** @brief The retirement chain's counts since the last call, and what waits in it now. */
		std::string TakeRetirementReport()
		{
			const auto s = std::exchange(retirementStats, RetirementStats{});
			// The oldest held snapshot keeps every later node alive.
			std::vector<std::tuple<std::uint64_t, long, const void*>> held;
			for (const auto& pooled : tablesPool)
				if (pooled.use_count() > 1 && pooled != publishedTables)
					held.emplace_back(pooled->publishedSequence, pooled.use_count() - 1, pooled.get());
			std::sort(held.begin(), held.end());
			std::string heldText;
			for (std::size_t i = 0; i < held.size() && i < 8; ++i)
				heldText += fmt::format(" {}:{}", std::get<0>(held[i]), std::get<1>(held[i]));
			if (!held.empty())
				heldText += fmt::format(" (oldest at {})", std::get<2>(held.front()));
			return fmt::format("{} retired, {} recycled in {} batches, {} dropped by a clear; {} object slots retiring; pipeline slots {} of {} alive "
							   "({} top cuts); {} snapshots held past the pool (publication:holders){}, published {}; engine references {} released "
							   "(through publication {}, publication {})",
				s.retired, s.recycled, s.batches, s.dropped, tables.objectsRetiring, tables.pipelineSlots.AliveCount(), tables.pipelines.size(),
				s.pipelinesTrimmed, held.size(), heldText, publishedTables ? publishedTables->publishedSequence : 0, std::exchange(referencesReleased, 0),
				EngineReleases::pushedThrough.load(std::memory_order_relaxed), publicationSequence);
		}
		TablesPublication TakeTablesPublication() { return std::exchange(tablesPublication, TablesPublication{ .pool = tablesPool.size() }); }
		const Stats& GetStats() const { return stats; }
		std::pair<std::uint32_t, std::uint32_t> TakeFadingDrawn()
		{
			const std::pair result{ stats.fadingDrawn, stats.fadingFrames };
			stats.fadingDrawn = stats.fadingFrames = 0;
			return result;
		}
		/** @brief DCLF's scene step (FrameView::SceneStep). On a scene thread this is sceneFrame, which lags the render thread. */
		std::uint32_t SceneStep() const;
		/** @brief The pass as a 32-bit stamp. Never 0, which means never, nor Tables::kSlotFree. */
		std::uint32_t PassStamp() const { return static_cast<std::uint32_t>(passSerial % 0xFFFFFFFEull) + 1u; }
		std::shared_ptr<const EntrySpace> CoordinatorSunCandidates() const { return sunCandidates; }
		std::shared_ptr<const EntrySpace> CoordinatorLightCandidates() const { return lightCandidates; }


		/** @brief The geometry's object slot in this walk's tables, or -1 when DCLF does not draw it. */
		std::int32_t FindObject(const RE::BSGeometry* a_geometry) const;
		std::int32_t FindLayerObject(const RE::BSGeometry* a_geometry) const;
		const AccumulatedPass* FindAccumulatedLayerPass(const RE::BSGeometry* a_geometry) const;
		RE::BSShaderProperty* SlotPropertyOf(std::uint32_t a_slot) const { return SlotProperty(a_slot); }
		/** @brief Decides the DCLF set after the joins. An object joins only once everything its phases draw with is ready. */
		void CommitSet();
		/** @brief Applies the set commits since the last publication. A slot freed or reassigned since then takes no phase. */
		void ApplySet();

		/** @brief One scene pass. Publishes only when something changed. */
		void RunSceneWork();
		std::pair<std::uint64_t, std::string> TakeUndrawnClaims() { return { std::exchange(undrawnClaims, 0), std::exchange(undrawnFirst, {}) }; }
		struct SetStats
		{
			std::uint64_t commits = 0, evaluated = 0, joined = 0, left = 0, readinessEvents = 0, resyncs = 0;
			std::uint64_t members = 0, waiting = 0, rebinding = 0;  // summed over the commits
			// Objects out of the set although some phases are ready. A claim covers the whole object.
			std::uint64_t partial = 0;
			std::uint64_t publications = 0;
			std::uint64_t patchedMember = 0;  // must be 0: the joins patched a member's binding (CommitSet keeps rebinds out)
			std::uint64_t leftAfterCommit = 0;  // slots taken out of the commit's phases after it (LeaveSet)
			// Why a bound object waits, by readiness source. Index 8 is the reflection's forward pipeline.
			std::array<std::uint64_t, 14> waitingBy{};
			// Why a member left, by its commit's verdict.
			std::array<std::uint64_t, 5> leftBy{};
			std::array<std::uint64_t, 14> leftWaiting{};
			std::uint64_t waitingRequeued = 0;  // waiting slots taken again for a readiness source they wait on
			std::uint64_t fitRequeued = 0;      // members taken again because the scene buffers' fit moved below them
			std::string firstWaiting;
		};
		SetStats TakeSetStats() { return std::exchange(setStats, {}); }
		void NoteFadeRootUnmirrored() { ++placementStats.fadeRootsUnmirrored; }
		/** @brief Reads the tracked map on the caller's thread. */
		bool IsTracked(const RE::BSGeometry* a_geometry) const;
		/** @brief The index ranges a ranged geometry draws, such as object LOD's visible segments, or null. */
		const std::vector<Api::EntryRange>* EntryRangesOf(const RE::BSGeometry* a_shape) const
		{
			const auto it = entryRanges.find(a_shape);
			return it != entryRanges.end() ? &it->second : nullptr;
		}

		using TrackSource = CoordinatorTracked::TrackSource;
		bool GetTrackInfo(const RE::BSGeometry* a_geometry, std::uint32_t& a_frame, TrackSource& a_source) const;
		RE::NiNode* CategoryNodeOf(RE::NiAVObject* a_object) const { return FindCategoryNodeLive(a_object, nullptr); }
		/** @brief a_cause is 0 for a signature change, 1 when a detach forced it. */
		bool GetCategoryInfo(const RE::NiNode* a_node, std::uint32_t& a_frame, std::uint8_t& a_cause) const;


		Ineligible Classify(RE::BSGeometry* a_geometry) const;


		const AccumulatedPass* FindAccumulatedPass(const RE::BSGeometry* a_geometry) const;


		using PlacementPlan = DCLF::PlacementPlan;
		using ShadingItem = DCLF::ShadingItem;
		using FadeSeedItem = DCLF::FadeSeedItem;
		const PlacementPlan* PeekPlacementPlan() const { return placementPlanReady.get(); }


		/** @brief True on the lane and its fan-out chunks, where SceneStep returns the coordinator's scene step. */
		static inline thread_local bool sceneWorkThread = false;

	private:
		struct EventBatch;
		struct BatchUnwindGuard;  // a batch back to the render thread on a throw (Internal.h)
		struct BatchIndex;
		struct ShardScope;

		ConstantsPostStats constantsPostStats;
		TablesPublication tablesPublication;
		ResidentStats residentStats;
		SetStats setStats;
		Stats stats;
		/**
		 * @brief A material reference taken and dropped the engine's way (AE). Equal properties share one material from the database
		 * (manager 0x143187758), and its release (FUN_1414f7a40) removes it from the database at zero. A BSTSmartPointer would just delete it.
		 */
		class MaterialReference
		{
		public:
			MaterialReference() = default;
			MaterialReference(const MaterialReference&) = delete;
			MaterialReference& operator=(const MaterialReference&) = delete;
			MaterialReference(MaterialReference&& a_other) noexcept :
				material(std::exchange(a_other.material, nullptr)) {}
			MaterialReference& operator=(MaterialReference&& a_other) noexcept
			{
				if (this != &a_other) {
					reset();
					material = std::exchange(a_other.material, nullptr);
				}
				return *this;
			}
			~MaterialReference() { reset(); }
			void reset(RE::BSShaderMaterial* a_material = nullptr);
			void Adopt(RE::BSShaderMaterial* a_material) { material = a_material; }
			explicit operator bool() const { return material != nullptr; }

		private:
			RE::BSShaderMaterial* material = nullptr;
		};
		// Keeps each material slot's pointer from being reused while a publication may name it.
		std::vector<MaterialReference> materialOwners;
		/**
		 * @brief What the coordinator let go of that a publication may still name. Each publication holds the chain node it was published with,
		 * so a batch is freed once the last frame that could read it is done. Engine references go to the retire queue (EngineReleases).
		 */
		struct RetiredBatch
		{
			std::vector<Tables::RetiredSlot> slots;
			std::vector<std::shared_ptr<const void>> imports;
			std::vector<RE::NiPointer<RE::NiRefObject>> references;
			std::vector<std::shared_ptr<void>> owners;  // whose last release runs the engine's destructors (event batches, material references)
			std::uint64_t lastPublication = 0;           // the last publication that held its node (EngineReleases::PushRetired)
			bool Empty() const { return slots.empty() && imports.empty() && references.empty() && owners.empty(); }
		};
		org::services::RetirementChain<RetiredBatch> retirement;
		std::size_t referencesReleased = 0;  // since the last report
		void ReleaseMaterial(MaterialReference&& a_material)
		{
			if (a_material)
				retirement.Open().owners.push_back(std::make_shared<MaterialReference>(std::move(a_material)));
		}
		/**
		 * @brief Keeps alive the views a material slot's record names. The engine can release a texture on any thread, and the capture
		 * the record came from may be replaced first. Never AddRef a pointer read from the record, it may already be dead.
		 */
		struct MaterialViewPins
		{
			std::vector<ID3D11ShaderResourceView*> named;                 // the record's views, in record order (pins kept while the same)
			std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> views;  // copied from the capture's references
			std::shared_ptr<const MaterialPort::FrameViewHold> frame;     // the frame's, when the record names a view the capture does not
		};
		std::vector<std::shared_ptr<MaterialViewPins>> materialViewPins;
		/** @brief Call after a_slot's record was stored from a_held. Both must still be alive. */
		void PinMaterialViews(std::uint32_t a_slot, const MaterialPort::HeldSnapshot& a_held, const MaterialPort::MaterialFrame& a_frame);
		void ReleaseMaterialViews(std::uint32_t a_slot);
		struct RetirementStats
		{
			std::uint64_t retired = 0, recycled = 0, dropped = 0, batches = 0;
			std::uint64_t pipelinesTrimmed = 0;  // recycles that cut the pipeline table's top (RecycleRetired)
		} retirementStats;

		void MirrorSubtree(const void* a_root, Ineligible a_reason, std::vector<std::pair<RE::BSGeometry*, Ineligible>>& a_out, bool& a_unmirrored) const;
		MirrorReadStats& MirrorReads() const { return mirrorReadSink ? *mirrorReadSink : mirrorReads; }
		RE::NiNode* MirrorCategoryNode(const void* a_object, Ineligible& a_reason, bool& a_unmirrored) const;
		void PrepareMirrorCaptures(const std::vector<const SceneCapture::Records*>& a_captures, std::size_t a_records);
		void PinBatch(const std::vector<RE::NiPointer<RE::NiRefObject>>& a_pins)
		{
			for (const auto& pin : a_pins) {
				batchPins.insert_or_assign(pin.get(), pin.get());
				++referenceStats.pins;
			}
		}
		template <class T>
		RE::NiPointer<T> Pinned(const T* a_key)
		{
			if (const auto it = batchPins.find(a_key); it != batchPins.end()) {
				++referenceStats.pinned;
				return RE::NiPointer<T>(static_cast<T*>(it->second));
			}
			return nullptr;
		}
		/** @brief Pure lookup, safe in a prepare. FromPin makes the reference. */
		RE::NiRefObject* PinOf(const void* a_key) const
		{
			const auto it = a_key ? batchPins.find(a_key) : batchPins.end();
			return it != batchPins.end() ? it->second : nullptr;
		}
		template <class T>
		RE::NiPointer<T> FromPin(RE::NiRefObject* a_pin)
		{
			if (!a_pin)
				return nullptr;
			++referenceStats.pinned;
			return RE::NiPointer<T>(static_cast<T*>(a_pin));
		}
		/** @brief The nearest ancestor of the contribution's classes, such as Light Limit Fix's room node. Null without one. */
		const void* AncestorKeyOf(const void* a_object) const;
		void ApplyMirrorEvents(const EventBatch& a_batch, BatchIndex& a_index);
		void PinMirrorBatch(const EventBatch& a_batch, std::vector<const SceneCapture::Records*>& a_captures, std::size_t& a_records);
		void CountMirrorWork();

		void ListDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistDependents(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_root);
		void UnlistCategoryMember(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		/** @brief a_live means a_root is a live walk's pointer rather than a batch pin. */
		bool OwnRoot(const RE::NiAVObject* a_root, bool a_live);
		void ReleaseRootOwner(const RE::NiAVObject* a_root);
		void ReleaseRootOwners();
		RE::NiPointer<RE::NiAVObject> OwnedRoot(const RE::NiAVObject* a_root);

		/** @brief a_force is set when a batch saw a detach. */
		void RefreshCategoryNodes(bool a_force = false);
		void DropRemovedCategories(const CategoryCapture& a_capture, bool a_removedRoot);
		void TrackAppearedCategories(const CategoryCapture& a_capture, const std::vector<RE::NiNode*>& a_added,
			const std::vector<RE::NiAVObject*>& a_addedRoots, std::uint8_t a_cause);
		/** @brief Runs first in the pass, before the drains. */
		void TakeCategoryCapture();
		/** @brief From the mirror's chain. The result is a key only. CS_DCLF_PARITY checks it against the live chain. */
		RE::NiNode* FindCategoryNode(RE::NiAVObject* a_object, Ineligible* a_parentReason, bool* a_unmirrored = nullptr) const;
		/** @brief The object must be alive. */
		RE::NiNode* FindCategoryNodeLive(RE::NiAVObject* a_object, Ineligible* a_parentReason) const;
		Api::EngineView ViewRootOf(const void* a_node) const
		{
			for (std::size_t v = 0; v < viewRoots.size(); ++v)
				if (a_node && a_node == viewRoots[v])
					return static_cast<Api::EngineView>(v);
			return Api::EngineView::Count;
		}
		bool ViewRoot(const void* a_node) const { return ViewRootOf(a_node) != Api::EngineView::Count; }
		bool ViewRootEntry(const RE::NiNode* a_categoryNode) const { return ViewRoot(a_categoryNode); }
		const RE::NiNode* FirstViewRoot() const
		{
			for (const auto* root : viewRoots)
				if (root)
					return root;
			return nullptr;
		}
		bool CategoryNodeOwn(const RE::NiAVObject* a_node) const
		{
			return categoryNodes.contains(static_cast<RE::NiNode*>(const_cast<RE::NiAVObject*>(a_node)));
		}

		/**
		 * @brief Tracks the geometries under a_root. A root with no mirror chain is captured at the next frame start and retried once.
		 * An attach event's root without one was out of the world, and its world attach brings its own event.
		 */
		void AddSubtree(RE::NiAVObject* a_root, SubtreeSource a_source = SubtreeSource::AttachEvent);
		/** @brief With neither a pin nor a_live the geometry is left out and counted. */
		void AddGeometry(RE::BSGeometry* a_geometry, RE::NiNode* a_categoryNode, Ineligible a_parentReason, bool a_live, GeometryPrep& a_prep);
		HeldKeys HeldKeysOf(const void* a_geometry) const;
		void PrepareSubtree(RE::NiAVObject* a_root, SubtreeSource a_source, SubtreePrep& a_out) const;
		void PrepareSubtreeWalk(RE::NiAVObject* a_root, SubtreeSource a_source, SubtreePrep& a_out) const;
		void PrepareGeometry(RE::BSGeometry* a_geometry, Ineligible a_reason, const RE::NiNode* a_category, GeometryPrep& a_out) const;
		void MergeSubtree(SubtreePrep& a_prep);
		/** @brief The caller merges the results in order. Nothing in between may write the mirror, category nodes or portal roots. */
		void PrepareSubtrees(const std::vector<std::pair<RE::NiAVObject*, SubtreeSource>>& a_roots, std::uint32_t a_mode, std::vector<SubtreePrep>& a_out);
		void AddSubtrees(const std::vector<std::pair<RE::NiAVObject*, SubtreeSource>>& a_roots);
		std::string TakeAttachLine();
		void HoldProperties(Tracked& a_entry) { HoldProperties(a_entry, HeldKeysOf(a_entry.geometry.get())); }
		void HoldProperties(Tracked& a_entry, const HeldKeys& a_keys);
		void ValidateSlice();
		void EraseTracked(RE::BSGeometry* a_geometry);

		/**
		 * @brief The cascade cull (FUN_140e305c0) walks only the objectArray entries the full-frustum cull (FUN_141511f30) passed.
		 * A static's entry is its reference root, an actor's is its cell's container, a terrain block's is its nearest BSMultiBoundNode.
		 */
		void ResolveSunEntry(Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live = false);
		const void* MirrorSunEntry(const void* a_geometry) const;
		static const RE::NiAVObject* LiveSunEntry(const RE::BSGeometry& a_geometry);
		static const RE::NiAVObject* LiveLightRoot(const RE::BSGeometry& a_geometry, const RE::NiNode* a_category, bool& a_actor);

		/**
		 * @brief The sun entries DCLF can take wholly out of the cascade culls. The snapshot is rebuilt only on a walk that changed nothing,
		 * so an exclusion for an older generation is never applied and cell loads do not rebuild it every frame.
		 */
		void UpdateSunCandidates(bool a_full);
		void DropSunCandidates();
		/** @brief kPrimaryEngine when the geometry is not tracked. */
		std::uint8_t PrimaryVerdictOf(RE::BSGeometry* a_geometry) const;
		static Eligibility::EntryVerdict VerdictOf(const Tracked& a_tracked) { return { a_tracked.slot != kNoObjectSlot, a_tracked.candidateFrame != 0, a_tracked.candidateReason }; }
		void MarkListedRootDirty(const RE::NiAVObject* a_entry)
		{
			if (a_entry)
				sunEntriesDirty.push_back(a_entry);
		}
		void MarkLightEntryDirty(const RE::NiAVObject* a_entry)
		{
			if (a_entry)
				lightEntriesDirty.push_back(a_entry);
		}
		/** @brief Its actor's root, since actors have no sun entry. Otherwise its sun entry unless that is a category node. */
		const RE::NiAVObject* LightEntryOf(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_live) const;
		void UpdateLightCandidates(bool a_full);
		/** @brief Null also for a node that became a category node, which would otherwise take every entry under it. */
		const std::vector<RE::BSGeometry*>* LightDependentsOf(const RE::NiAVObject* a_root) const;

		void TouchPlacedDependent(const RE::NiAVObject* a_root, const void* a_geometry)
		{
			if (a_root && plannedRoots.contains(a_root))
				placementDependentsTouched.emplace_back(a_root, a_geometry);
		}
		/** @brief Drops a stale member from moverSet. */
		std::optional<PlannedKey> PlannedMoverNow(RE::BSGeometry* a_geometry);
		/** @brief Forgets a still root that is no longer listed. */
		bool PlannedRootNow(const RE::NiAVObject* a_root);
		std::optional<PlannedKey> PlannedDependentNow(const RE::NiAVObject* a_root, const void* a_geometry) const;
		void AddPlannedSlots(const PlannedKey& a_key, const PlannedOwner& a_owner);
		void RemovePlannedSlots(const PlannedKey& a_key, const PlannedOwner& a_owner);
		/** @brief a_reset diffs against an empty set. */
		void DiffPlacementSet(PlacementPlan& a_plan, bool a_reset);
		void ReportPlacementDeltas();
		/** @brief The node is a key and is not dereferenced. */
		void ApplyMotionUpdate(const void* a_node);
		/** @brief Movers are listed in every plan and sampled every frame. */
		bool IsPlacedMover(const Tracked& a_tracked) const;
		/** @brief a_written means its plan key may have changed, so a member is compared at the next plan. True when moverSet changed. */
		bool NoteMover(RE::BSGeometry* a_geometry, const Tracked& a_tracked, bool a_written = true);
		void SyncPlacementSet();
		/** @brief The light path's gate. a_why is 1 moved, 2 hidden event owed, 3 first person, 4 no light path. */
		bool MoverDue(const Tracked& a_tracked, const RE::BSGeometry* a_geometry, std::uint32_t& a_why) const;
		void ReportMoverGate();
		void QueueRoots();
		PlacementPlan::Item PlanItemOf(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		PlannedKey PlacedKeyOf(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		PlacementPlan& WalkPlan();
		void PublishPlacementPlan();
		void DrainMoveEvents(bool a_full);
		bool MovedRecently(const void* a_key) const
		{
			const auto it = movedFrame.find(a_key);
			return it != movedFrame.end() && sceneFrame - it->second <= 1;
		}
		const void* MoveKeyOf(const RE::BSGeometry& a_geometry, const RE::NiNode* a_categoryNode) const;
		/** @brief Whether anything under a reference root moves, so its sun entry's bound is not fixed. Walked once per root. */
		bool RootMoves(const RE::NiAVObject* a_root, const bool* a_now = nullptr);
		bool RootMovesNow(const RE::NiAVObject* a_root) const;
		static bool PlacementMatters(const Tracked& a_tracked);
		/**
		 * @brief The root is a key and is never dereferenced. With a_whole false only a changed motion verdict reclassifies the dependents.
		 */
		void ScheduleRoot(const RE::NiAVObject* a_root, bool a_whole = true);
		void ApplyNodeEvent(RE::NiAVObject* a_node);
		std::pair<const void*, const void*> MoveKeysOf(const RE::BSGeometry& a_geometry, const RE::NiNode* a_categoryNode) const;

		void ListHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void UnlistHiddenChain(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void DrainHiddenEvents();

		static bool SwitchSelects(const SceneCapture::NodeRecord& a_switch, const void* a_child)
		{
			return (a_switch.kind & SceneCapture::kKindSwitch) && a_switch.switchChild && a_switch.switchChild == a_child && a_switch.switchCurrent;
		}
		void ApplySwitchEvents(bool a_full);
		/** @brief Drains each engine queue only up to where it stood when the pass began. */
		std::shared_ptr<EventBatch> CollectEvents();
		void ApplyEvents(std::shared_ptr<EventBatch> a_batch);
		void RefreshBatchCategories(const BatchIndex& a_index);
		void ApplyAttachDetach(const BatchIndex& a_index);
		void ApplyDetaches(const std::vector<RE::BSGeometry*>& a_detached);
		void HoldLeafProperties(const BatchIndex& a_index);
		/** @brief During a load, requests the batch's diffuse textures early so the hold after the load waits less. Reads no other engine memory. */
		void PrewarmLoadTextures(const BatchIndex& a_index);

		void NameShading(std::uint32_t a_slot, bool a_member);
		/** @brief Every slot is named while the shading events are not installed. */
		void NameShadingEvents(std::vector<std::uint32_t>* a_slots = nullptr);
		void NameShadingKeys(const std::vector<const void*>& a_keys, std::vector<std::uint32_t>* a_slots);

		void TakeAccumulateInputs();
		/**
		 * @brief Looks every object's ancestor value up again when a new map arrives. Joins look it up only on evaluation, so an object bound
		 * before Light Limit Fix's room filled would keep the empty value.
		 */
		void RefreshAncestorValues();
		/** @brief An actor the engine moves to another room keeps its resident slots, and no join writes them again. */
		void RefreshAncestorValue(Tracked& a_entry);
		void MakeNewPipelineConstants();
		/** @brief Runs first in each pass. */
		void TakeFrameInputs();
		void ApplyConstantsPosts();
		void RefreshTechniqueRows();
		void EvaluateTechniqueRow(std::uint32_t a_row);
		std::uint32_t TechniqueRowFor(std::uint32_t a_passDescriptor);

		void InvalidateObjectIndices() { ++objectStamp; }
		std::uint32_t AcquireObjectSlot(Tracked& a_tracked, RE::BSGeometry* a_geometry);
		void SweepObjectSlots();
		/** @brief Frees the slot outside the walk so entries tracked later reuse it instead of growing the arrays. */
		void ReleaseObjectSlot(Tracked& a_entry);
		std::uint32_t AcquireLayerSlot(Tracked& a_tracked, RE::BSGeometry* a_geometry, std::uint32_t a_base);
		void ReleaseLayerSlot(Tracked& a_entry);
		/** @brief A layer slot draws its layer's property. */
		RE::BSShaderProperty* SlotProperty(std::uint32_t a_slot) const;
		void ListMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot);
		void UnlistMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot);
		void UpdateSlotReferences();
		void ClearGeometrySlot(std::uint32_t a_slot);
		void FreeGeometrySlot(std::uint32_t a_slot);
		/** @brief The slot table's own state is the caller's to update. */
		void ClearMaterialSlot(std::uint32_t a_slot);
		void IndexObjectHead(std::uint32_t a_object, std::uint32_t a_head);
		void IndexLink(std::uint32_t a_from, std::uint32_t a_to);
		/** @brief Sorted and unchecked. a_chains also takes every head whose chain reaches one of a_slots. */
		void ObjectsUnder(const std::vector<std::uint32_t>& a_slots, bool a_chains, std::vector<std::uint32_t>& a_out) const;
		std::uint32_t AllocateGeometrySlot();
		std::uint32_t AllocatePipelineSlot();
		std::uint32_t AllocateMaterialSlot();
		void PrefetchGeometryBuffers(std::size_t a_first);
		/** @brief Returns Tables::kSlotFree when the buffers cannot be made stable for the graph. */
		std::uint32_t ResolveGeometrySource(const GeometrySource& a_source, PartTimer& a_timer);
		std::uint32_t ResolveLayerGeometrySlot(const SceneCapture::GeometryRecord& a_geometry, PartTimer& a_timer);
		std::uint32_t ResolveGeometrySlot(const SceneCapture::GeometryRecord& a_geometry, const SceneCapture::GeometryRecord::Partition* a_partition, PartTimer& a_timer);
		/** @brief Slots for a ranged entry's drawn ranges, linked by nextPartition. Tables::kSlotFree if any fails. */
		std::uint32_t ResolveEntryRangeSlots(const SceneCapture::GeometryRecord& a_geometry, const std::vector<Api::EntryRange>& a_ranges, PartTimer& a_timer);

		void SweepSlots();
		/** @brief Teardown must leave nothing pointing at dead slots. */
		void ResetSlotTables();
		/** @brief Every bound object must name live slots of this frame, or it is neutralised. */
		void CheckObjectSlots(bool a_resolveBuffers);

		void DrainMaterialCaptures();
		/** @brief Released through the retire queue, on the main thread. */
		void HandBackHeld(std::unique_ptr<MaterialPort::HeldSnapshot>&& a_held);
		/**
		 * @brief Keeps the material records current from the frame's sources, with no engine read. TexcoordOffset is not in the record,
		 * the frame values patch it into every material row.
		 */
		void RefreshMaterialRecords();
		void RewriteCapturedMaterials();
		void RefreshMaterialSignatures();
		void NoteMaterialRecord(std::uint32_t a_slot, const MaterialPort::MaterialSnapshot& a_snapshot);
		/** @brief Pure and safe on any thread. For a rewrite the slot's frame parts are kept, and `same` reports an unchanged record. */
		void EvaluateMaterial(const MaterialPort::MaterialSnapshot& a_snapshot, std::uint32_t a_pass, const MaterialPort::MaterialFrame& a_sources, std::uint32_t a_slot,
			MaterialEvaluation& a_out) const;
		/** @brief a_body must be pure. Returns whether the pool ran. */
		bool PrepareMaterials(std::string_view a_zone, std::size_t a_count, const std::function<void(std::size_t, std::size_t)>& a_body);
		void PrepareJoinMaterials(std::size_t a_count);
		void ResetMaterialRecords();

		void UpdateMaterialBindings();
		void ResolveMaterialBinding(std::uint32_t a_slot, bool a_force = false);
		/** @brief A binding superseded by a texture move is never kept or cached. It is held until the new one is answered. */
		void FollowRelocatedTextures();
		struct MaterialTextureBinder;
		/** @brief Reuses a block another slot of a_material sealed for the same owners. Null when none is held. */
		std::shared_ptr<const Lookups::MaterialOwners> SealMaterialOwners(const RE::BSShaderMaterial* a_material, const Lookups::MaterialOwners* a_held,
			Lookups::MaterialOwners& a_next, std::uint32_t a_texturesMoved, std::uint32_t a_featuresMoved);
		void VersionMaterialBindings();
		GpuTextures::Binding MaterialViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, std::uint32_t a_slot);
		/** @brief Called once no publication names the slot. */
		void RetireMaterialBinding(std::uint32_t a_slot);
		/** @brief An answer to an earlier request is stale after this. */
		void ResetMaterialBindings();
		void UpdateSharedBindings();
		void FollowRelocatedSharedTextures();
		std::uint32_t DrainSharedReplies();
		void TakeFixedBindings();
		void ResolveMaskBindings();
		void ApplyShadowTextureChanges(const std::vector<std::pair<ID3D11ShaderResourceView*, bool>>& a_changes);
		void SealSharedBindingBlock();
		GpuTextures::Binding SharedViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, SharedBindings::Kind a_kind);
		void ResolveProjectedBindings(std::uint32_t a_textures);
		/** @brief False when the entry stood and nothing was looked up. */
		bool ResolveMaskBinding(std::uint32_t a_slot, ID3D11ShaderResourceView* a_frameMask);
		void ResolveShadowTextureBinding(ID3D11ShaderResourceView* a_view);
		/** @brief Called once no publication names the slot. */
		void RetireMaskBinding(std::uint32_t a_slot);
		/** @brief Keeps the render thread's posted capture. */
		void ResetSharedBindings();
		/** @brief Resolves against the pipeline lane's newest catalog. */
		void ResolveLookups();
		/** @brief Shares the last copy again if nothing changed. */
		void PublishLookups();
		/** @brief Generations move past the last one and versions continue. */
		void ResetLookups();

		/** @brief By the mirror's parents. -1 when there is none or it was not read. */
		std::int32_t NearestFadeNodeBsx(const SceneCapture::LeafView& a_leaf) const;
		Ineligible ClassifyFrame(const Tracked& a_tracked) const;
		/**
		 * @brief Focus shadows register the target's subtree whole, hidden parts included. So a hidden actor part still casts there,
		 * and no other engine view draws a hidden node.
		 */
		bool HiddenFocusCaster(const Tracked& a_tracked, Ineligible a_reason, const SceneCapture::LeafView& a_leaf) const;
		/** @brief The billboard face mode (0-5) when DCLF turns the shape on the GPU, else -1. */
		std::int32_t BillboardModeOf(const Tracked& a_tracked) const;
		/**
		 * @brief Draws a first-person decal as a forward object after the view's opaque buckets, as the engine's accumulator does.
		 * Only the opaque group writes depth (modes 3 and 1).
		 */
		void ViewDecalForward(const Tracked& a_tracked, LightingDescriptors& a_descriptors) const;
		/**
		 * @brief The engine flips kHidden around its views, so those bits come from the frame-start capture (FrameGlobals::cullHidden).
		 * Live bits would drop the player's face on cube-map reflection update frames.
		 */
		bool HiddenForWalk(const RE::NiAVObject* a_object) const;
		bool HiddenForWalk(const void* a_object, const SceneCapture::NodeRecord& a_record) const;
		/** @brief True when changed. */
		bool WriteObjectExtras(std::uint32_t a_object);
		void BuildScenePhase();
		/** @brief a_keepIndices keeps object indices for a delta walk. */
		void BeginWalk(bool a_keepIndices = false);

		/**
		 * @brief Evaluates only entries whose inputs can have changed. Every other slot keeps its record.
		 */
		void DeltaWalk();
		void ScheduleFullEvaluation();
		void SchedulePerFrameEntries();
		void ScheduleEventEntries();
		void RestoreKeptMembers(std::array<std::vector<std::uint32_t>, Extensions::kPublishedEntryClasses>& a_kept);
		void BuildFullOrder();
		/** @brief False when the entry gets no record. */
		bool WriteObject(RE::BSGeometry* a_geometry, Tracked& a_tracked, PartTimer& a_timer, Ineligible& a_bucket);
		/** @brief Without a_member or a layer the slot is released. */
		void WriteLayer(RE::BSGeometry* a_geometry, const SceneCapture::LeafView& a_leaf, Tracked& a_tracked, std::uint32_t a_base, bool a_member, bool a_keepMember,
			PartTimer& a_timer);
		std::uint32_t PerFrameTraits(const Tracked& a_tracked, const RE::BSGeometry& a_geometry) const;
		std::uint32_t PerFrameTraitsOf(bool a_entryClassTrait, bool a_ownerClassTrait, const RE::NiNode* a_categoryNode, const RE::BSGeometry& a_geometry) const;
		/** @brief a_full writes it in full even if it only moved. */
		void Schedule(RE::BSGeometry* a_geometry, Tracked& a_tracked, bool a_full = true);
		bool KeepSkin(RE::BSGeometry* a_geometry, Tracked& a_tracked, const EvalResult& a_result);
		/** @brief Whether an actor's earlier record still stands except for placement and palette. */
		bool FrameVerdictKept(const Tracked& a_tracked, EvalCounters& a_counters) const;
		void Reclassify(RE::BSGeometry* a_geometry, Tracked& a_tracked);
		void MoveBucket(Tracked& a_tracked, Ineligible a_bucket);
		void FinishDeltaWalk(PartTimer& a_timer);
		void ScheduleStaleGeometry();
		/** @brief Marks stale the slots drawing a buffer whose import DXVK moved. */
		void NoteRelocatedGeometry(const std::vector<ID3D11Buffer*>& a_buffers);
		void ScheduleRefreshedGeometry(PartTimer& a_timer);
		/**
		 * @brief Evaluations are pure and merges run in index order. Fan-out mode 0 runs each in turn, 1 evaluates all then merges,
		 * 2 merges pool chunks as they finish.
		 */
		void EvaluateRound(PartTimer& a_timer, std::size_t a_first);
		void EvaluateStreamed(PartTimer& a_timer, std::size_t a_first, std::size_t a_count, std::size_t a_chunks, bool a_light, bool a_profileKinds,
			std::uint64_t a_mirrorWrites, std::uint64_t a_layout);
		/** @brief Pure. */
		void EvaluateEntry(std::size_t a_index, bool a_light, EvalCounters& a_counters, EvalResult& a_out, PartTimer& a_timer) const;
		void EvaluateWrite(RE::BSGeometry* a_geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out, PartTimer& a_timer) const;
		/** @brief False when rejected with no record. */
		bool EvaluateKeptVerdict(const Tracked& a_tracked, const SceneCapture::LeafView& a_leaf, bool a_rereads, EvalCounters& a_counters, EvalResult& a_out) const;
		/** @brief False when rejected with no record. */
		bool EvaluateStaticVerdict(const Tracked& a_tracked, const SceneCapture::LeafView& a_leaf,
			EvalCounters& a_counters, EvalResult& a_out, PartTimer& a_timer) const;
		void EvaluateRecordInputs(RE::BSGeometry* a_geometry, const SceneCapture::LeafView& a_leaf, EvalResult& a_out) const;
		void EvaluateShadowInputs(const SceneCapture::LeafView& a_leaf, EvalResult& a_out) const;
		void EvaluateClassified(RE::BSGeometry* a_geometry, const Tracked& a_tracked, EvalCounters& a_counters, EvalResult& a_out) const;
		std::pair<bool, std::uint32_t> PerFrameMerge(const Tracked& a_tracked, const EvalResult& a_result);
		static std::pair<bool, std::uint32_t> PerFrameFrom(std::uint32_t a_traits, Ineligible a_reason, bool a_mayRecord);
		void MergeEntry(std::size_t a_index, bool a_profileKinds, EvalResult& a_result, PartTimer& a_timer);
		/** @brief True when the kept record stands. Otherwise the entry is evaluated in full for the write. */
		bool MergeLight(RE::BSGeometry* a_geometry, Tracked& a_tracked, EvalResult& a_result, PartTimer& a_timer);
		bool MergeWrite(RE::BSGeometry* a_geometry, Tracked& a_tracked, EvalResult& a_result, PartTimer& a_timer, Ineligible& a_bucket);
		/**
		 * @brief Logs a permanent refusal of a geometry that casts a shadow, once per reason. It stays the engine's, whose draws
		 * graph mode skips, so it casts no shadow.
		 */
		void NoteRefusedCaster(RE::BSGeometry* a_geometry, Ineligible a_reason);
		void NoteUnstable(RE::BSGeometry* a_geometry, const SceneCapture::GeometryRecord& a_record);
		std::uint32_t unstableLogged = 0;  // NoteUnstable's lines, this session
		// Cached per geometry slot key so the buffer descriptions are read once, not every walk.
		ankerl::unordered_dense::map<const void*, std::uint8_t> unstableCauseOf;
		/** @brief Tables::kSlotFree means the buffers are unstable and the entry was rebucketed. */
		std::uint32_t ResolveWriteGeometry(RE::BSGeometry* a_geometry, const SceneCapture::GeometryRecord& a_record, const EvalResult& a_result, PartTimer& a_timer,
			Ineligible& a_bucket);
		ObjectRecord RecordOfWrite(RE::BSGeometry* a_geometry, const SceneCapture::LeafView& a_leaf, const EvalResult& a_result, std::uint32_t a_geometrySlot,
			PartTimer& a_timer, std::uint32_t& a_boneRows);
		void MergeShadowColumns(std::uint32_t a_slot, std::uint32_t a_geometrySlot, const SceneCapture::LeafView& a_leaf, const EvalResult& a_result,
			ObjectRecord& a_object);
		/** @brief a_keepHalf keeps the record's accumulate half. */
		void MergeRecordRow(RE::BSGeometry* a_geometry, Tracked& a_tracked, const EvalResult& a_result, Api::EntryWrite& a_face, std::uint32_t a_slot,
			std::uint32_t a_geometrySlot, bool a_keepHalf, ObjectRecord& a_object, Ineligible a_bucket, PartTimer& a_timer);
		/** @brief Must run in chunk order. */
		void MergeCounters(EvalCounters& a_counters);
		void MergeMirrorReads(MirrorReadStats& a_counts);
		std::uint32_t FanoutMode(std::size_t a_count, std::size_t a_least = kFanoutMinEntries) const;
		/** @brief The caches witness the object, not the switches, so a live toggle the classification reads drops them all. */
		void InvalidateVerdicts();

		void BuildAccumulatePhase();
		struct JoinFrame;
		struct JoinBinding;
		void RunJoins(JoinFrame& a_frame);
		void MergeJoin(const OrderEntry& a_entry, JoinResult& a_result, JoinFrame& a_frame);
		std::uint32_t BindJoinPipeline(const JoinResult& a_result, const LightingDescriptors& a_descriptors, bool a_layer, PartTimer& a_timer);
		std::uint32_t MakeJoinPipeline(const JoinResult& a_result, const LightingDescriptors& a_descriptors, bool a_layer);
		bool BindJoinMaterial(std::uint32_t a_objectId, const JoinResult& a_result, const LightingDescriptors& a_descriptors, Tracked::Derived& a_derived,
			PartTimer& a_timer, std::uint32_t& a_materialSlot);
		void RecordJoinDerivation(const OrderEntry& a_entry, const JoinResult& a_result, const JoinFrame& a_frame,
			const LightingDescriptors& a_descriptors, const JoinBinding& a_binding, Tracked::Derived& a_derived);
		void PatchJoin(const OrderEntry& a_entry, const JoinResult& a_result, JoinFrame& a_frame, ObjectRecord& a_object, std::uint32_t a_objectId,
			const LightingDescriptors& a_descriptors, const JoinBinding& a_binding);
		void SettleJoinResidency();
		void ApplyAccumulatePatch(const AccumulatePatch& a_patch);
		void LeaveMainForRebind(std::uint32_t a_slot);
		bool RebindKeepsDrawing(std::uint32_t a_slot) const;
		void StageBinding(std::uint32_t a_slot, std::uint32_t a_pipeline, std::uint32_t a_material);
		void UnstageBinding(std::uint32_t a_slot);
		void AddStagedReferences(const StagedBinding& a_staged);
		void ReleaseStagedReferences(const StagedBinding& a_staged);
		void RequestSettledPipeline(const OrderEntry& a_entry, const JoinResult& a_result);
		/** @brief Binds the bind queue's records as residents from membership passes. They stay until rewritten. */
		void BindByMembership();
		void EvaluateJoinPre(std::uint32_t a_slot, JoinPre& a_out) const;
		void EvaluateJoin(const OrderEntry& a_entry, bool a_classify, JoinResult& a_out) const;
		/** @brief Pure. */
		void KeyJoin(const Tracked& a_entry, bool a_layer, const SceneCapture::PropertyRecord& a_property, bool a_interior, JoinResult& a_out) const;
		/** @brief Defined in AccumulatePhase.cpp. */
		template <class Result, class Evaluate>
		void EvaluateJoins(std::size_t a_count, bool a_pool, std::vector<Result>& a_results, Evaluate&& a_evaluate);

		bool MemberBindingStands(std::uint32_t a_slot, const Tracked& a_entry) const;
		/** @brief The coordinator's view of residency. IsMember is the frame's. */
		bool ResidentObject(std::int32_t a_object) const { return a_object >= 0 && IsResidentSlot(static_cast<std::uint32_t>(a_object)); }
		bool IsResidentSlot(std::uint32_t a_slot) const { return a_slot < residentPos.size() && residentPos[a_slot] != kNotResident; }
		void MarkResidentSlot(std::uint32_t a_slot, const ResidentPatch& a_patch);
		/** @brief a_restore resets its accumulated half now. */
		void DropResidentSlot(std::uint32_t a_slot, bool a_restore);
		void ResetAccumulatedHalf(std::uint32_t a_slot);
		/** @brief Uses a template when no resident used a slot this frame. */
		void KeepResidentsAlive();
		void NoteResidentTouched(std::uint32_t a_slot) { residentsTouched.push_back(a_slot); }

		void ApplyFadeNodeAnswers();
		void TakeCoordinatorInputs();
		/** @brief The slot's fade node if it is a_node, else null. */
		RE::NiPointer<RE::NiAVObject> HeldFadeNode(std::uint32_t a_slot, const void* a_node);
		/** @brief Requests the reference from the render thread at most once per key. True when held now. */
		bool AcquireFadeNode(Tracked& a_entry, const void* a_node, const char* a_path);

		// Each call runs every registered component's hook, in registration order.
		void ComponentsResident(std::uint32_t a_slot);
		void ComponentsLeft(std::uint32_t a_slot);
		/** @brief Runs before ComponentsRewritten and before ComponentsResident. */
		void ComponentsSlotWritten(std::uint32_t a_slot);
		/** @brief Runs right before the slot's object is reset. */
		void ComponentsSlotReleased(std::uint32_t a_slot);
		/** @brief Runs before the entries under the switch are scheduled. */
		void ComponentsSwitched(const void* a_switch);
		void ComponentsAdmitted(std::span<const Api::StandInRoot> a_roots);
		/** @brief With a_rebuild every resident is counted again. Otherwise a_touched, sorted and unique. */
		void ComponentsRecount(bool a_rebuild, std::span<const std::uint32_t> a_touched);
		/** @brief Runs before the tables are cleared. */
		void ComponentsReset();
		void ComponentsAllLeft();
		void ComponentsAccumulated(std::uint32_t a_slot, std::uint64_t a_chain);
		void ComponentsRewritten(std::uint32_t a_slot, bool a_written, std::uint32_t a_partitionsBefore, std::uint32_t a_partitionsNow);
		void ComponentsJoinsDone();
		void ComponentsWalkBegun();
		void ComponentsScheduling(bool a_full);
		void ComponentsWalkEnded();
		/** @brief Runs after the entry's slot is released. */
		void ComponentsForgot(const RE::BSGeometry* a_geometry);
		void ComponentRecycled(std::uint32_t a_component, std::uint32_t a_first, std::uint32_t a_count);
		void ComponentsListed(RE::BSGeometry& a_geometry, const Tracked& a_tracked, Api::EntryClass a_class);
		bool ComponentsResolve(Api::EntryWrite& a_write, Api::EntryClass a_class);
		void ComponentsStreamed(std::uint32_t a_slot, Api::EntryWrite& a_write, Api::EntryClass a_class);
		void ComponentsChannelKeys(Api::ChannelApply a_apply, const EventBatch& a_batch);
		void ComponentsChannelKeys(Api::ChannelApply a_apply);
		/** @brief a_veto is Api::kVetoHold, kVetoObserve or 0. */
		void SetEntryVeto(const void* a_entry, std::uint8_t a_veto);
		Api::ObservedState ObservedSlotOf(std::uint32_t a_slot) const;
		Api::TrackedState TrackedStateOf(const void* a_entry) const;
		/** @brief True when a component asked for it or an entry is vetoed. */
		bool CommitLive() const;
		void ComponentsReportText(Api::ReportPoint a_point, std::string& a_text);
		void ComponentsReportLines(Api::ReportPoint a_point, std::uint32_t a_frame);
		/** @brief Scene lane only. Makes the components first if needed. */
		const std::shared_ptr<void>& ComponentState(std::uint32_t a_component);
		void ComponentsStacked(Api::EngineChannelId a_channel, const Api::Stacked& a_event);
		void ComponentsDetached(const void* a_key, const Api::Mark& a_mark);
		/** @brief Must run before the mirror's records for the detach go. */
		void UnmarkDetached(const void* a_root, std::span<const void* const> a_nodes);
		void ComponentsUnderMark(const RE::BSGeometry* a_geometry, const Api::Mark& a_mark);
		void ComponentsSettle(bool a_live);
		void ComponentsTake();
		void ComponentsPublished();

		ShadowInputs ShadowInputsOf(std::uint32_t a_slot) const;
		void RefreshShadowSets();
		void ResetShadowIndex(std::vector<std::uint32_t>& a_changed);
		void ApplyShadowInputs(std::uint32_t a_slot, const ShadowInputs& a_inputs, bool a_live, bool a_add, std::uint32_t& a_moved);
		void EmitShadowSets(bool a_rebuild, std::uint32_t a_moved);

		/** @brief Counts claimed phases the publication's records do not draw. Any count is a defect. */
		void CheckClaimsDrawable();
		static constexpr std::uint8_t WaitCauseOf(std::uint32_t a_why)
		{
			switch (a_why) {
			case 0:
			case 1:
			case 2:
			case 3:
				return kWaitLookups;
			case 7:
			case 13:
				return kWaitShadow;
			case 9:
				return kWaitConstants;
			case 10:
			case 12:
				return kWaitFit;
			default:
				return kWaitOther;
			}
		}
		void NoteSetPhases(const RE::BSGeometry* a_geometry) { setPhaseLog.Push(a_geometry); }
		std::size_t TakeClaimsEntry();
		void PublishClaims();
		void RefreshPending();
		void QueueSet(std::uint32_t a_slot);
		/**
		 * @brief Takes a rebound slot out of the applied set after its commit. Otherwise the commit's copy keeps the phases and nobody draws
		 * the record. A base and its layer leave together.
		 */
		void LeaveSet(std::uint32_t a_slot, std::uint8_t a_lost, bool a_partner = true);
		void MarkSetApply(std::uint32_t a_slot);
		/** @brief Changes only on a toggle, load, failure or setup, so the views the engine asks for never change the set. */
		static std::uint32_t SetCapability();
		std::uint8_t SetParticipation(std::uint32_t a_slot, std::uint32_t a_drawn) const;
		/** @brief Pure. CommitSet applies the side effects. */
		SlotDecision DecideSlot(std::uint32_t a_slot, bool a_live, std::uint32_t a_drawn) const;
		struct SetCommit;
		void CommitMembershipEvents(const SetCommit& a_commit);
		void CommitReadinessSources();
		void CommitShadowModeRequeues(std::size_t a_objects);
		std::uint8_t CommitDecideSlot(std::uint32_t a_slot, const SetCommit& a_commit, bool& a_wait, bool* a_partial = nullptr);
		void CommitApplySlot(std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait, SetCommit& a_commit);
		void CommitQueuedSlots(SetCommit& a_commit);
		void CommitComponentsSettle(SetCommit& a_commit);
		/** @brief Hold applies nothing. Observe applies nothing but records what it would have applied. */
		std::uint8_t VetoedPhases(std::uint32_t a_slot, std::uint8_t a_phases, bool a_wait, std::uint32_t a_drawn, bool a_live);
		bool EntryVetoed(std::uint32_t a_slot) const;
		/** @brief a_why is the waitingBy index when not ready. */
		bool MainReady(std::uint32_t a_slot, std::uint32_t& a_why) const;
		/** @brief MainReady for a rebind's pipeline and material, before the slot is bound to them. */
		bool MainReadyWith(std::uint32_t a_slot, std::uint32_t a_pipeline, std::uint32_t a_material, std::uint32_t& a_why) const;
		Api::EngineView ViewRootOfSlot(std::uint32_t a_slot) const;

		void PublishTables();
		void ReplayTableColumns(Tables& a_snapshot, bool a_replay);
		// A kept publication may still name these references, so they go through the retirement chain and the retire queue.
		template <class T>
		void HandBack(RE::NiPointer<T>&& a_reference)
		{
			if (a_reference)
				retirement.Open().references.emplace_back(std::move(a_reference));
		}
		template <class T>
		void HandBack(std::vector<RE::NiPointer<T>>& a_references)
		{
			for (auto& reference : a_references)
				HandBack(std::move(reference));
			a_references.clear();
		}
		/** @brief Deltas go onto the log before the publication reaches the snapshot builder, so whoever adopts it finds its node. */
		void PublishScene();
		void TakeDeltas(PublicationDeltas& a_deltas);
		void DropReturnedPublications();
		void RetireIntoChain();
		void RecycleRetired();

		void AddScenePartMs(ScenePart a_part, double a_ms)
		{
			stats.scenePartMs[static_cast<std::size_t>(a_part)] += a_ms;
			stats.scenePartFrameMs[static_cast<std::size_t>(a_part)] += a_ms;
		}
		PublishKey CurrentPublishKey() const;
		bool DeltasPending() const;
		bool PublicationNeeded();
		void NotePublished();
		/** @brief False means the pass defers. */
		bool PublicationDue(PublishReason& a_why) const;
		void PublishIfDue();
		void ReportPublicationPacing();
		void ScenePass();
	};
}
