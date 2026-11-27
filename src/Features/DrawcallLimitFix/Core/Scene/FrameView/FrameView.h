#pragma once

// The render thread's view of the scene: the publication the frame installed, the publication log up to it, the frame's captures
// and the requests it serves for the coordinator. Render thread only, plus the frame's builds that read the installed publication.
// It reaches the coordinator only through Scene::Channels.

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Scene/Channels.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/FrameTables.h"

namespace Snapshot
{
	struct FrameSnapshot;
}

namespace DCLF
{
	struct FrameGlobals;
	struct EntrySpace;
	struct PipelineCatalog;
	namespace Diagnostics
	{
		struct SceneView;
	}
}

namespace DCLF::Scene
{
	class FrameView
	{
		friend struct Diagnostics::SceneView;

	public:
		using Tables = SceneTables;
		static FrameView& Get();
		/**
		 * @brief DCLF's scene step: moves once per world render, only while DCLF runs (at the game half's BeginFrame, or at a render's
		 * StepRender), and at an update the cadence renders nothing for. Not the CS frame (RenderGraph::FrameClock). Every step comes
		 * from one counter, so steps are unique and grow on each side. Inside native's game half (GameHalfScope, on its thread) it is
		 * the game half's step; anywhere else the render's.
		 */
		std::uint32_t SceneStep() const { return t_gameHalf ? gameStep : renderStep; }
		/**
		 * @brief Render half: the render's scene step. A ticket's first render keeps the ticket's step when no step was taken since
		 * (always in guest and on one thread); otherwise, and for a repeated render, it takes the next.
		 */
		void StepRender(std::uint32_t a_ticketStep, bool a_first);
		/**
		 * @brief Renders since the start, one per StepRender. Written by the render owner; other threads read the newest value. Buffers
		 * that alternate per render pick their slot from it, since scene steps skip the numbers game halves take.
		 */
		std::uint64_t RenderSerial() const { return renderSerial.load(std::memory_order_acquire); }
		/**
		* @brief At a CS Begin, before DCLF's BeginFrame of that frame: the next step the counter gives. On one thread it is the step
		* the beginning frame runs.
		*/
		std::uint32_t NextSceneStep() const { return stepCounter.load(std::memory_order_acquire) + 1; }
		/** @brief Native's game half on this thread, from FrameDriver::GameHalfStart to GameHalfEnd (no scope spans them). */
		static void SetGameHalf(bool a_in) { t_gameHalf = a_in; }
		/** @brief Whether this thread is inside native's game half. */
		static bool InGameHalf() { return t_gameHalf; }
		/**
		 * @brief What the frame start installed, as the render side reads it: the publication and its tables, lookups, catalog,
		 * candidates, light entries and claims. Native's game half installs into FrameView's own members; its frame start ends with
		 * a copy for its ticket (TakeInstalledFrame), which the render half adopts (AdoptInstalledFrame). The accessors below read the
		 * game half's members inside the game half, the adopted frame anywhere else, and the members while none is adopted (guest).
		 */
		struct InstalledFrame
		{
			std::shared_ptr<const ScenePublication> publication;
			std::shared_ptr<const Tables> tables;
			std::shared_ptr<const Lookups> lookups;
			std::shared_ptr<const PipelineCatalog> catalog;
			std::uint32_t tablesGeneration = 0;
			std::shared_ptr<const EntrySpace> sunCandidates, lightCandidates;
			std::uint64_t lightEntriesAppeared = 0;
			std::shared_ptr<const ankerl::unordered_dense::set<RE::NiNode*>> categoryNodes;
			std::shared_ptr<const ankerl::unordered_dense::set<const RE::NiAVObject*>> lightEntries;
			std::shared_ptr<const SetSnapshot> claims;
			ToggleSet toggles{};      // the installed publication's toggles (Toggles::Frame on the render side)
			bool notDrawing = true;   // NotDrawingYet as the game half left it
			std::uint64_t stops = 0;  // the render side's stops the game half had applied (StopDrawing)
		};
		/** @brief Native's game half, at its frame start's end: the copy its ticket carries. */
		std::shared_ptr<const InstalledFrame> TakeInstalledFrame();
		/** @brief Render half, frame start: the ticket's installed frame. Null (guest) keeps reading the members. */
		void AdoptInstalledFrame(std::shared_ptr<const InstalledFrame> a_frame);
		/** @brief 0 before the first publication. A reader that keeps data checks it against EngineReleases::Alive. */
		std::uint64_t InstalledSequence() const;
		/** @brief For a reader whose slot indices must outlive the frame. */
		std::shared_ptr<const void> InstalledRetirement() const;

		/**
		 * @brief The four textures a ProjectedUV draw binds, at pixel slots 3, 8, 10 and 11. Engine globals that only the
		 * ReloadProjectedUVTextures console command changes.
		 */
		struct ProjectedTextures
		{
			static constexpr std::array<std::uint32_t, 4> kSlots{ 3, 8, 10, 11 };
			std::array<ID3D11ShaderResourceView*, 4> views{};
			bool valid = false;
		};
		/** @brief Latched by each commit into its frame blocks. */
		struct FrameCapture
		{
			// Read by DCLF_BINDLESS draws from PS b13. A change here alone versions no pipeline.
			std::array<float, 24> lighting{};
			// Read by the DCLF_BINDLESS vertex stage from VS b13.
			std::array<float, 12> fog{};
			// Kept while the sample has none, as while a cell loads.
			ID3D11ShaderResourceView* characterLightView = nullptr;
		};
		struct PublicationStats
		{
			std::uint64_t installed = 0, kept = 0, skipped = 0;
		};
		/**
		 * @brief On the first sighting of a load screen, withdraws the set and raises Channels::loadingSeen. a_loading is the frame's
		 * value: the game half's capture, or LoadingScreenInFrame at Present. Returns a_loading.
		 */
		bool NoteLoadingScreen(bool a_loading);
		/** @brief Render thread, frame start, not during a load. */
		void ServeFrameRequests();
		/**
		 * @brief NiSwitchNode::OnVisible's catch-up (AE 0x140d29700), run outside the cull. True when it ran.
		 * Render thread only, since engine code writes the subtree's transforms.
		 */
		static bool CatchUpSwitch(RE::NiSwitchNode& a_switch);
		/** @brief Render thread, before the culls. Skips subtrees a loader is still assembling. After a load, every switch in the world. */
		void CatchUpSwitches(std::span<RE::NiAVObject* const> a_attached, std::span<const Scene::SwitchEvent> a_switches);
		/** @brief The switches are keys, never dereferenced. True when the caller must reread every switch. */
		bool TakeSwitchChanges(std::vector<const RE::NiAVObject*>& a_out);
		/**
		 * @brief Nothing may walk the scene graph while a load screen is up. The loading menu a load's hold keeps open does not count,
		 * because the load is done and the world's frames under it build the tables.
		 */
		static bool IsLoadingScreenUp();
		/**
		 * @brief Render side: IsLoadingScreenUp as the frame sees it. Native reads the adopted snapshot's menus (captured by the game
		 * side, its own for a loading screen's frame); guest reads the live UI.
		 */
		static bool LoadingScreenInFrame();
		/** @brief LoadingScreenInFrame for a_frame, the snapshot of the half that asks (native; guest reads the live UI). */
		static bool LoadingScreenIn(const Snapshot::FrameSnapshot* a_frame);
		/**
		 * @brief Render side: also true while a load's hold keeps the world off the screen. Run-comparable frame counters must not
		 * advance then.
		 */
		static bool LoadShown();
		/** @brief LoadShown for a_frame, the snapshot of the half that asks: the game half passes Snapshot::Building(). */
		static bool LoadShownIn(const Snapshot::FrameSnapshot* a_frame);
		/** @brief LoadShown from the live UI: game side only (a capture). */
		static bool LoadShownNow();
		/**
		 * @brief Call where `globals::game::currentAccumulator` is set. That global is null before the main pass even though the
		 * accumulator is built before the shadow maps, so earlier readers use the latch.
		 */
		void LatchAccumulator();
		/**
		 * @brief Prepass. Must run against the main camera, since before it the renderer holds the shadow camera's state.
		 * The Z-prepass epoch runs earlier on last frame's values, which is harmless only because World is patched per object.
		 */
		void RefreshFrameConstants();
		void NoteProjectedTextures();
		/** @brief Never the coordinator's tables. Empty before the first publication. */
		const Tables& GetTables() const { return View(); }
		const FrameTables& GetFrameTables() const { return frameTables; }
		/** @brief Render thread, frame start, before any build. */
		void SyncFrameTables()
		{
			const Tables& view = View();
			frameTables.SyncPipelines(view.pipelines, view.pipelineBindingVersion, GetTablesGeneration(), view.pipelinesStamp);
		}
		/** @brief Render thread, EarlyPrepass. Does no scene work, the next scene pass takes the inputs. */
		void PostAccumulateInputs();
		/** @brief Render thread, Prepass. Posted latest-wins. */
		void PostPipelineFrame(const GeometryPort::PipelineFrame& a_frame);
		/** @brief The first installation applies the claims whole, since the engine's hooks hold none before it. */
		void InstallClaims();
		/** @brief Any thread. Null during a load or before the first installation. Immutable and swapped whole. */
		std::shared_ptr<const SetSnapshot> FrameClaims() const { return RenderSide() ? std::atomic_load(&renderClaims) : std::atomic_load(&frameClaims); }
		bool HasInstalled() const { return RenderSide() ? renderFrame->publication != nullptr : installed != nullptr; }
		const ScenePublication* InstalledPublication() const { return RenderSide() ? renderFrame->publication.get() : installed.get(); }
		/**
		 * @brief True before DCLF's first covered frame, and after an adoption stopped it (StopDrawing) until its next covered frame.
		 * Its epochs draw nothing then, and the engine draws the scene. While DCLF draws the claims stay: the adopted snapshot stays
		 * in use until a Begin adopts a newer current one. The render side reads its adopted frame's value; a stop it asked after
		 * that frame's game half counts at once.
		 */
		bool NotDrawingYet() const
		{
			if (RenderSide())
				return renderFrame->notDrawing || stopsAsked.load(std::memory_order_acquire) > renderFrame->stops;
			return !drawingStarted.load(std::memory_order_acquire) || stopped;
		}
		/** @brief Any thread. How many times the render side stopped DCLF (StopDrawing). */
		std::uint64_t StopsAsked() const { return stopsAsked.load(std::memory_order_acquire); }
		/** @brief Game half: the stops it applied, after its frame start's hand-over. */
		std::uint64_t StopsApplied() const { return stopsApplied; }
		/** @brief Any thread. Whether DCLF ever drew in the session. What its graphs are built for may not change after that. */
		bool EverDrew() const { return drawingStarted.load(std::memory_order_acquire); }
		/**
		 * @brief The render owner, inside a Begin's adoption point. A frame revision left DCLF's scene on old versions, so DCLF stops
		 * drawing. Every claim is withdrawn, as at a load, until the next covered frame installs them again whole. The render side's
		 * claims go at once. The claims state is the game half's: native's game half applies the stop at its next frame start
		 * (HandOverAtFrameStart); guest, and a render side with no adopted frame, apply it here.
		 */
		void StopDrawing();
		/** @brief Render thread, frame start before the culls. The deltas are by component, oldest first, cleared by ClearAdoptedDeltas. */
		const SetSnapshot* AdoptedClaims() const;
		std::span<const std::vector<std::shared_ptr<const void>>> AdoptedDeltas() const { return frameComponentDeltas; }
		void ClearAdoptedDeltas();
		PublicationStats TakePublicationStats() { return std::exchange(publicationStats, PublicationStats{}); }
		/** @brief Render thread, frame start. Reads no coordinator state. Installs a_adopted when it is newer. */
		void HandOverAtFrameStart(std::shared_ptr<const void> a_adopted);
		const std::shared_ptr<const Tables>& AcceptedTables() const { return RenderSide() ? renderFrame->tables : acceptedTables; }
		const FrameCapture& GetFrameCapture() const { return frameCapture; }
		/** @brief 0 before the first. Moves on a slot table reset or InvalidateVerdicts. */
		std::uint32_t GetTablesGeneration() const { return RenderSide() ? renderFrame->tablesGeneration : frameTablesGeneration; }
		std::shared_ptr<const EntrySpace> GetSunCandidates() const { return RenderSide() ? renderFrame->sunCandidates : frameSunCandidates; }
		/** @brief Entries whose every tracked geometry is a table object, so DCLF can take them out of the point lights' culls. */
		std::shared_ptr<const EntrySpace> GetLightCandidates() const { return RenderSide() ? renderFrame->lightCandidates : frameLightCandidates; }
		/** @brief An entry LocalLightCull cut for holding no geometry is judged again when this moves. */
		std::uint64_t GetLightEntriesAppeared() const { return RenderSide() ? renderFrame->lightEntriesAppeared : frameLightEntriesAppeared; }
		/** @brief Written only at frame start, from the publication log. */
		bool IsLightEntry(const RE::NiAVObject* a_node) const
		{
			if (RenderSide())
				return renderFrame->lightEntries && renderFrame->lightEntries->contains(a_node) && !IsCategoryNode(a_node);
			return frameLightEntrySet.contains(a_node) && !IsCategoryNode(a_node);
		}
		bool IsCategoryNode(const RE::NiAVObject* a_node) const
		{
			const auto& nodes = RenderSide() ? renderFrame->categoryNodes : frameCategoryNodes;
			return nodes && nodes->contains(static_cast<RE::NiNode*>(const_cast<RE::NiAVObject*>(a_node)));
		}
		/** @brief Made by the scene lane and immutable. */
		const Lookups& GetLookups() const
		{
			const auto& lookups = RenderSide() ? renderFrame->lookups : frameLookups;
			return lookups ? *lookups : EmptyLookups();
		}
		static const Lookups& EmptyLookups()
		{
			static const Lookups empty;
			return empty;
		}
		bool IsMember(std::int32_t a_object) const
		{
			const auto& resident = View().residentSlot;
			return a_object >= 0 && static_cast<std::size_t>(a_object) < resident.size() && resident[a_object] != 0;
		}
		/**
		 * @brief Render thread, frame start. The frame reads only its own state, immutable publications and the log.
		 * Everything else arrives as a message.
		 */
		void BeginFrame();
		/** @brief Render thread, EarlyPrepass. Reads nothing of the coordinator's, since a scene pass may be running. */
		void PrepareAccumulatePhase();
		/**
		 * @brief Answers every material request: reads each property's material and captures it. Native: the game half (DCLF's frame
		 * start), so a request posted after it is answered at the next update. Guest: EarlyPrepass (PrepareAccumulatePhase).
		 */
		void ServeMaterialRequests();
		/** @brief Render thread, or any thread between commits. */
		std::uint8_t SetPhasesOf(std::int32_t a_object) const
		{
			const auto& phases = View().setPhases;
			return a_object >= 0 && static_cast<std::size_t>(a_object) < phases.size() ? phases[a_object] : std::uint8_t{ 0 };
		}
		/** @brief An occlusion map DCLF draws leaves joining objects out, and the engine's cull is no fallback for them. */
		std::uint32_t SetLacking(std::uint8_t a_phase) const
		{
			const auto& lacking = Extensions::ResolvedViewBits().lackingPhases;
			const bool lod = a_phase && (a_phase & lacking[2]) == a_phase;
			return a_phase && a_phase == lacking[0] ? frameLackingCount[0] : a_phase && a_phase == lacking[1] ? frameLackingCount[1] : lod ? frameLackingCount[2] : 0u;
		}
		/** @brief Render thread. Posted latest-wins for the next scene pass. */
		void SetStandInRoots(const std::vector<Api::StandInRoot>& a_roots)
		{
			// No wake. The frame's next pass takes it.
			channels.standInRootsPosted.Post(std::make_unique<std::vector<Api::StandInRoot>>(a_roots));
		}
		/** @brief A claimed geometry stays alive, because the retirement chain keeps what a held publication names. */
		bool HeldByInstalled(const RE::BSGeometry* a_geometry) const
		{
			return installed && installed->claims && installed->claims->phases.contains(a_geometry);
		}
		/** @brief How many references the held category captures pin each of a_keys. Tells DCLF's holds from the engine's. */
		void CapturePinsOf(std::span<const void* const> a_keys, std::span<std::uint32_t> a_counts) const
		{
			std::ranges::fill(a_counts, 0u);
			if (a_keys.empty())
				return;
			ankerl::unordered_dense::map<const void*, std::size_t> index;
			index.reserve(a_keys.size());
			for (std::size_t i = 0; i < a_keys.size(); ++i)
				index.try_emplace(a_keys[i], i);
			for (const auto& capture : categoryCapturesHeld)
				for (const auto& pin : capture->pins)
					if (const auto it = index.find(pin.get()); it != index.end())
						++a_counts[it->second];
		}
		/**
		 * @brief Import leases of cleared geometry slots, held until the GPU retires the execution. In-flight frames read those
		 * buffers by device address, which DXVK cannot see, so it would otherwise reuse the memory.
		 */
		std::vector<std::shared_ptr<const void>> TakeRetiredImports() { return std::exchange(frameRetiredImports, {}); }
		/** @brief Released with the next main commit's imports. */
		void RetireImport(std::shared_ptr<const void> a_owner) { frameRetiredImports.push_back(std::move(a_owner)); }
		std::vector<FadeSeedItem> TakeComponentSeeds(Api::CoordinatorComponentId a_component)
		{
			return a_component.index < frameComponentSeeds.size() ? std::exchange(frameComponentSeeds[a_component.index], {}) : std::vector<FadeSeedItem>{};
		}
		const std::shared_ptr<const FrameGlobals>& FrameGlobalsOfFrame() const { return frameGlobals; }
		std::vector<ShadingItem> TakeShadingItems() { return std::exchange(frameShading, {}); }
		std::span<const ShadingItem> ShadingChanges() const { return frameShading; }
		/** @brief Held by the newest publication until the next install. */
		std::span<RE::BSGeometry* const> NewestObjectGeometry() const;
		/** @brief Oldest first. FrameValues draws with the newest. */
		std::vector<std::shared_ptr<const PlacementPlan>> TakePlacementPlans() { return std::exchange(framePlans, {}); }
		/** @brief Call when a classification toggle changes or the feature comes back on. */
		void RequestVerdictsInvalidated() { ++verdictsRequested; }
		std::vector<ShadingItem> PeekShadingItems() const;
		void DeliverPrimaryNotes();

	private:
		Channels& channels = Channels::Get();
		void CaptureCategories(bool a_force);
		void CaptureMirrorRequests();
		void ServeFadeNodeRequests();
		/** @brief Must run every frame to drain the fixed-capacity buffer. Only the observers read its contents. */
		void DrainCapture();
		void CaptureLightingShader();
		/** @brief Posts only when the map's generation moves. */
		void PostAncestorMap();
		void PostFrameInputs();
		void RefreshCharacterLightView();
		void PostPipelineConstants(std::uint32_t a_slot);
		const Tables& View() const
		{
			const auto& tables = RenderSide() ? renderFrame->tables : acceptedTables;
			return tables ? *tables : EmptyTables();
		}
		/** @brief Whether the caller reads the adopted installed frame: outside native's game half once one is adopted. */
		bool RenderSide() const { return !t_gameHalf && renderFrame != nullptr; }
		static const Tables& EmptyTables()
		{
			static const Tables empty;
			return empty;
		}
		void PublishFrameClaims(std::shared_ptr<const SetSnapshot> a_claims);
		/** @brief The coordinator drops it. */
		void ReturnPublication(std::shared_ptr<const ScenePublication>&& a_publication);
		std::shared_ptr<const ScenePublication> Newest() const { return RenderSide() ? renderFrame->publication : installed; }
		FrameCapture frameCapture;  // GetFrameCapture
		FrameTables frameTables;    // GetFrameTables: the frame's values, out of the tables (render thread)
		std::shared_ptr<const CategoryCapture> categoryCapture;  // render thread: the newest made
		// Their engine references are dropped here on the render thread.
		std::deque<std::shared_ptr<const CategoryCapture>> categoryCapturesHeld;
		std::uint64_t categoryCaptures = 0;  // render thread: the last capture's generation
		std::uint64_t captureNs = 0, categoryCaptureNs = 0, categoryCapturesMade = 0, captureFrames = 0;
		std::uint64_t categoryMirrorCaptures = 0, categoryMirrorRecords = 0;  // CaptureCategories' captures for the mirror
		std::shared_ptr<const FrameGlobals> frameGlobals;
		std::uint64_t ancestorMapPostedGeneration = ~0ull;  // render thread: the ancestor map's generation last posted
		// The first frame after a load catches up every switch and forces a category capture.
		bool worldCatchUpPending = false;
		bool categoryCapturePending = false;
		std::vector<RE::NiAVObject*> attachedRoots;  // the ingestion's attached subtrees, for CatchUpSwitches (scratch)
		std::shared_ptr<const EntrySpace> frameSunCandidates, frameLightCandidates;
		std::uint64_t frameLightEntriesAppeared = 0;
		std::uint32_t frameTablesGeneration = 0;  // GetTablesGeneration
		std::vector<const RE::NiAVObject*> frameSwitchChanges;
		bool frameSwitchResync = true;
		bool switchResyncNext = false;  // NoteLoadingScreen: PrimaryCull rereads every switch from the next frame start
		std::vector<std::shared_ptr<const void>> frameRetiredImports;
		std::vector<std::shared_ptr<const PlacementPlan>> framePlans;
		std::vector<ShadingItem> frameShading;
		std::vector<std::vector<FadeSeedItem>> frameComponentSeeds;  // by coordinator component
		ankerl::unordered_dense::set<const RE::NiAVObject*> frameLightEntrySet;
		std::shared_ptr<const ankerl::unordered_dense::set<RE::NiNode*>> frameCategoryNodes;  // the installed publication's (IsCategoryNode)
		RE::BSGraphics::BSShaderAccumulator* latchedAccumulator = nullptr;
		std::shared_ptr<const Lookups> frameLookups;
		std::shared_ptr<const PipelineCatalog> frameCatalog;
		std::shared_ptr<const Tables> acceptedTables;
		ProjectedTextures projectedTextures;
		bool loadingPosted = false;  // render thread: the load's marker is posted
		bool loadSinceCapture = false;  // render thread: a load came since the last category capture (the job before it is dropped)
		std::vector<std::vector<std::shared_ptr<const void>>> frameComponentDeltas;
		std::shared_ptr<const ScenePublication> installed;
		// Render side (native): the installed frame its ticket carried, and its claims for FrameClaims from any thread.
		std::shared_ptr<const InstalledFrame> renderFrame;
		std::shared_ptr<const SetSnapshot> renderClaims;
		// Game half: the light entries' copy for the tickets, made again when the set changed.
		std::shared_ptr<const ankerl::unordered_dense::set<const RE::NiAVObject*>> lightEntriesCopy;
		bool lightEntriesChanged = true;
		// Cursors into the log. The notes cursor never passes the deltas cursor. Null means the log's first node.
		org::async::PublicationLog<PublicationDeltas>::Node* deltasCursor = nullptr;
		org::async::PublicationLog<PublicationDeltas>::Node* notesCursor = nullptr;
		std::vector<std::pair<std::vector<const RE::BSGeometry*>, std::vector<const RE::BSGeometry*>>> installNotes;
		bool installPending = false;
		std::shared_ptr<const SetSnapshot> notedClaims;
		// Written by the render thread through PublishFrameClaims, read from any thread.
		std::shared_ptr<const SetSnapshot> frameClaims;
		std::array<std::uint32_t, 3> frameLackingCount{};
		PublicationStats publicationStats;
		std::atomic<bool> drawingStarted{ false };  // the first claims installed (InstallClaims; NotDrawingYet until then), never cleared
		bool stopped = false;  // game half: an adoption stopped DCLF and no covered frame installed the claims since
		// Stops the render side asked (StopDrawing), and those the game half applied (ApplyAskedStops).
		std::atomic<std::uint64_t> stopsAsked{ 0 };
		std::uint64_t stopsApplied = 0;
		/** @brief Game half: withdraws every claim for the stops asked since the last call. */
		void ApplyAskedStops();
		std::optional<GeometryPort::PipelineFrame> geometryPortFrame;
		std::uint32_t geometryPortFrameNumber = ~0u;
		// The first refresh evaluates every pipeline in full. That seeds frame lighting variables the sample's pipeline does not write.
		bool constantsRefreshed = false;
		std::uint32_t verdictsRequested = 0;  // render thread: bumped by each request to drop the cached verdicts
		// SceneStep. The game half's step is written only by the game half; the render's only by the render owner.
		std::atomic<std::uint32_t> stepCounter{ 0 };
		std::uint32_t gameStep = 0;
		std::uint32_t renderStep = 0;
		std::atomic<std::uint64_t> renderSerial{ 0 };
		static inline thread_local bool t_gameHalf = false;
		struct GeometryStats
		{
			std::uint64_t frames = 0, full = 0, samples = 0, changed = 0, lightingVersions = 0;
		} geometryStats;
		std::atomic<std::uint64_t> pipelinesPosted{ 0 };  // taken by the coordinator's report
		std::uint64_t shadingFrames = 0;
	};
}
