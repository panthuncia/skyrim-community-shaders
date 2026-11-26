#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "RenderGraph/Async/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"

namespace DCLF
{
	/**
	 * @brief Hooks every point where the engine hands a pass to a batch renderer, and withholds the passes DCLF draws itself.
	 *
	 * A member of the frame's DCLF set, or an object waiting to join, has its passes withheld from every view DCLF draws.
	 * During a load's hold, every pass of DCLF's classes into those views is withheld. Runs on whatever thread the engine registers on.
	 *
	 * Lighting passes are also recorded for observers only, since DCLF draws nothing from registrations. The hook is on
	 * BSBatchRenderer::RegisterPass (vfunc 0x02), because only there do technique and subPass exist.
	 * Registration is concurrent, so entries go into a fixed buffer with an atomic cursor that the render thread drains once a frame.
	 */
	class PassCapture
	{
	public:
		struct Entry
		{
			const RE::BSGeometry* geometry = nullptr;
			const RE::BSRenderPass* pass = nullptr;
			const RE::BSBatchRenderer* batch = nullptr;
			std::uint32_t technique = 0;  // techniqueID, the batch group's key
			std::uint32_t subPass = 0;
			std::uint32_t passEnum = 0;
			bool withheld = false;
			std::uint8_t fadeState = 0xFF;  // the fade node's LOD state (+0x153), 0xFF without one
			std::uint8_t hint = 0;
		};

		struct Stats
		{
			std::uint32_t captured = 0;  // entries the last drain saw
			std::uint32_t overflowed = 0;
			std::uint32_t threads = 0;
			// Casters' Utility passes withheld from shadow views, by render mode (0xD plain, 0xE clamped, 0xF paraboloid).
			std::array<std::uint32_t, 3> shadowWithheld{};
			std::uint32_t volumetricWithheld = 0;  // volumetric-only passes (hint 8) kept out of batch group 15
			std::uint32_t directWithheld = 0;  // shadow passes of hints 11, 7 and 3, which bypass RegisterPass
			std::uint32_t mainWithheld = 0;
			std::uint32_t occlusionWithheld = 0;
			std::uint32_t holdWithheld = 0;
			// Of those, lit passes into the main views the frame's claims do not draw: the load hold waits for them.
			std::uint32_t holdNobody = 0;
			// Passes of objects waiting to join that the hooks withheld.
			std::uint32_t pendingWithheld = 0;
			// Passes of lit objects withheld until their base mips are in.
			std::uint32_t baseMipsWithheld = 0;
			// Passes of DCLF's classes handed to the engine. Menu scenes are not counted.
			std::uint32_t dclfKept = 0;
			// Lit objects' passes into the main views that nobody draws.
			std::uint32_t mainNobodyWaiting = 0;
			std::uint32_t mainNobodyKept = 0;
			const RE::BSGeometry* mainNobodyFirst = nullptr;
			// By draw family, its engine shapes' passes into the main views while DCLF draws the family.
			std::array<std::uint32_t, static_cast<std::size_t>(Api::DrawFamily::Count)> familyWithheld{};
			// By view, an OwnRenderers view's withheld member passes.
			std::array<std::uint32_t, Extensions::kEngineViews> viewWithheld{};
			std::array<std::array<std::uint32_t, static_cast<std::size_t>(Api::DrawFamily::Count)>, Extensions::kEngineViews> viewFamilyWithheld{};
		};

		/** @brief Render thread, before the main cull. Whether the engine's main-view passes a draw family replaces are withheld this frame. */
		void SetFamilyWithheld(Api::DrawFamily a_family, bool a_withheld) { familyHeld[static_cast<std::size_t>(a_family)].store(a_withheld, std::memory_order_release); }

		/**
		 * @brief Render thread, at BeginSceneFrame. Whether a view's epochs draw its members this frame.
		 * While set, those members' passes into the view's renderers are withheld.
		 */
		void SetViewCovered(Api::EngineView a_view, bool a_covered) { viewRenderers[static_cast<std::size_t>(a_view)].coverage.store(a_covered ? 2 : 1, std::memory_order_release); }
		/** @brief A view's coverage. */
		bool ViewCovered(Api::EngineView a_view) const
		{
			const auto coverage = viewRenderers[static_cast<std::size_t>(a_view)].coverage.load(std::memory_order_acquire);
			return coverage ? coverage == 2 : Extensions::ViewEntryOf(a_view).coveredInitially;
		}
		/** @brief Whether a_batch is one of an InMainRenderers view's batch renderers. */
		bool IsViewRenderer(Api::EngineView a_view, const RE::BSBatchRenderer* a_batch) const
		{
			const auto renderers = std::atomic_load(&viewRenderers[static_cast<std::size_t>(a_view)].renderers);
			return renderers && renderers->contains(a_batch);
		}
		/**
		 * @brief An OwnRenderers view's renderers, such as the water reflection faces. Rebuilt whenever any renderer changes, since the
		 * engine makes groups as geometry first uses them. The other setters below gate when this view's passes are withheld.
		 */
		void SetViewRenderers(Api::EngineView a_view, const Api::ViewAccumulators& a_accumulators);
		void SetViewWithholdActive(Api::EngineView a_view, bool a_active) { viewRenderers[static_cast<std::size_t>(a_view)].withholdActive.store(a_active, std::memory_order_release); }
		void SetViewRendering(Api::EngineView a_view, bool a_rendering) { viewRenderers[static_cast<std::size_t>(a_view)].rendering.store(a_rendering, std::memory_order_release); }
		void SetViewFamilyOwned(Api::EngineView a_view, Api::DrawFamily a_family, bool a_owned)
		{
			auto& owned = viewRenderers[static_cast<std::size_t>(a_view)].familyOwned;
			const std::uint32_t bit = 1u << static_cast<std::uint32_t>(a_family);
			if (a_owned)
				owned.fetch_or(bit, std::memory_order_release);
			else
				owned.fetch_and(~bit, std::memory_order_release);
		}
		/** @brief Whether a_batch is one of an OwnRenderers view's batch renderers. */
		bool IsOwnViewRenderer(Api::EngineView a_view, const RE::BSBatchRenderer* a_batch) const { return IsViewRenderer(a_view, a_batch); }
		/** @brief Render thread. Whether an OwnRenderers view's render is running. */
		bool OwnViewRendering() const;
		/** @brief An InMainRenderers view's pass counts since the last call. */
		struct ViewCounts
		{
			std::uint32_t withheld = 0, memberKept = 0, registered = 0;
		};
		ViewCounts TakeViewCounts(Api::EngineView a_view)
		{
			auto& view = viewRenderers[static_cast<std::size_t>(a_view)];
			return { view.withheld.exchange(0, std::memory_order_relaxed), view.memberKept.exchange(0, std::memory_order_relaxed),
				view.registered.exchange(0, std::memory_order_relaxed) };
		}

		static constexpr std::uint32_t kShadowModes = 3;

		static PassCapture& Get();

		void Install();
		/** @brief The patch sites Install needs that another plugin changed, or empty. Writes nothing. */
		static std::string SitesMissing();
		/** @brief Checks the sites Install writes. The hooks do not chain, so these must be the engine's own. */
		static void Preflight();
		bool Installed() const { return installed; }
		/** @brief Marks a menu scene's render on this thread. Its registrations go to no view DCLF draws, so a load's hold withholds none. */
		class MenuSceneScope
		{
		public:
			MenuSceneScope();
			~MenuSceneScope();
			MenuSceneScope(const MenuSceneScope&) = delete;
			MenuSceneScope& operator=(const MenuSceneScope&) = delete;
		};

		/** @brief Makes the hook a plain pass-through while set. */

		/**
		 * @brief Render thread. The frame's DCLF set, immutable and swapped whole, so every registration of the frame reads the same one.
		 * Null withholds nothing.
		 */
		void PublishSet(std::shared_ptr<const SetSnapshot> a_set);
		std::shared_ptr<const SetSnapshot> CurrentSet() const { return std::atomic_load(&frameSet); }

		/** @brief A geometry of DCLF's classes whose passes went to the engine, as first seen in the window. */
		struct KeptSample
		{
			std::string name;
			std::string owner;  // the nearest ancestor with a reference: its form ID and base object's name
			std::string where;
			std::string shader;
			std::uint32_t renderMode = ~0u;  // where known
			std::uint32_t passes = 0;
			std::uint8_t phases = 0;
			std::uint8_t pending = 0;
			bool hidden = false;  // app-culled, itself or an ancestor
			bool skinned = false;
		};
		/** @brief Render thread. The kept geometries since the last call, most passes first. */
		std::vector<KeptSample> TakeKeptSamples(std::size_t a_max);
		/** @brief One class of pass the map census saw, withheld or handed to the engine. */
		struct MapClass
		{
			std::string shader;
			std::string sample;
			bool dclf = false;
			std::uint32_t withheld = 0, kept = 0;  // into the main views
			std::uint32_t otherWithheld = 0, otherKept = 0;  // into any other renderer
			// Why a pass of DCLF's classes was kept.
			std::uint32_t noSet = 0, notMember = 0, notMain = 0, mainNotDrawn = 0;
			std::uint32_t underLodRoot = 0;  // under TES::lodLandRoot
		};
		/** @brief Render thread. While the map menu is open, counts every pass the hooks see by class. */
		void SetMapCensus(bool a_on);
		/** @brief Render thread. The map census's classes since the last call, most passes first. */
		std::vector<MapClass> TakeMapCensus();
		/** @brief Render thread. Of the kept geometries under the LOD root, how many the frame's set now holds and how many wait. */
		std::size_t MapKeptMembersNow(std::size_t& a_kept, std::size_t& a_waiting) const;
		/** @brief Render thread, before the frame's shadow registrations. Forgets which covered views kept passes for the engine. */
		void ResetCoveredKept();
		/** @brief Render thread, once the shadow registrations are done. Whether a covered view's accumulator kept a pass for the engine. */
		bool CoveredKept(const void* a_accumulator) const;
		/** @brief Render thread. Accumulators of uncovered shadow views that kept passes went to, since the last call. */
		std::vector<const void*> TakeUncoveredAccumulators();
		/** @brief Render thread. The samples of the load hold's undrawn passes since the last call, and clears them. */
		std::vector<std::string> TakeHoldNobodySamples();

		/**
		 * @brief Admission latency (CS_DCLF_STATS): for each lit object the engine registered into the main views that nobody drew,
		 * the scene steps until DCLF first drew it. Split by what it was at its first undrawn frame: not tracked yet, or waiting to join.
		 */
		struct AdmissionStats
		{
			std::vector<std::uint32_t> untracked, waiting;  // steps, one per object admitted in the window
			std::uint32_t open = 0;                         // still undrawn at the report
			std::uint32_t abandoned = 0;                    // undrawn for kAdmissionAbandonSteps, then dropped (gone or never admitted)
			std::vector<std::pair<std::uint32_t, std::string>> slowest;  // the slowest admissions: steps, and what the object was
		};
		/** @brief Render thread, on report frames. The window's admissions, and clears them. */
		AdmissionStats TakeAdmissionStats();
		/**
		 * @brief Render thread, before registrations. The main views' batch renderers: the Z-prepass accumulator (*0x14338c828) and the
		 * main one (*0x14338c830) with their groups. Rebuilt only when an accumulator changes.
		 */
		void RefreshMainRenderers();
		/** @brief Whether a_batch is one of the main views' batch renderers. */
		bool IsMainRenderer(const RE::BSBatchRenderer* a_batch) const
		{
			const auto renderers = std::atomic_load(&mainRenderers);
			return renderers && renderers->contains(a_batch);
		}
		/** @brief Render thread, before Precipitation::SetupMask. The occlusion map's phase if DCLF draws it, else 0. */
		void SetOcclusionPhase(std::uint8_t a_phase) { occlusionPhase.store(a_phase, std::memory_order_release); }
		/** @brief The shadow views' render modes: 0xD plain, 0xE clamped, 0xF paraboloid. */
		static constexpr std::uint32_t kFirstShadowMode = 0xD;
		/** @brief Which batch renderers belong to shadow views, with each view's render mode index. */
		using ShadowRendererMap = ankerl::unordered_dense::map<const RE::BSBatchRenderer*, std::uint8_t>;
		void SetShadowBatchRenderers(std::shared_ptr<const ShadowRendererMap> a_renderers);
		/**
		 * @brief The covered shadow views' accumulators. A pass into a renderer none of the sets names, made by the engine during this
		 * registration, belongs to the view whose accumulator registers it.
		 */
		using ShadowAccumulatorMap = ankerl::unordered_dense::map<const void*, std::uint8_t>;
		void SetShadowAccumulators(std::shared_ptr<const ShadowAccumulatorMap> a_accumulators);
		/**
		 * @brief Render thread, around the engine's focus accumulation. While set, a shadow registration into an unknown accumulator
		 * belongs to the covered focus view, since focus descriptors are made on first use.
		 */
		static void SetFocusAccumulation(bool a_covered);
		/** @brief Whether a_batch is a shadow view's renderer this frame, with its mode index. */
		bool ShadowModeOfBatch(const RE::BSBatchRenderer* a_batch, std::uint32_t& a_mode) const;
		/** @brief The set a pass into a_batch is tested against: the frame's when DCLF draws that shadow view this frame, else null. */
		std::shared_ptr<const SetSnapshot> CastersForBatch(const RE::BSBatchRenderer* a_batch, std::uint8_t* a_phase = nullptr) const;
		/** @brief Whether the frame's set withholds a shadow phase's members from its views. */
		bool CastersWithheld(std::uint8_t a_phase) const;
		/** @brief The render graph draws the shadow views and CS_DCLF_PARITY_BOTH is off. */
		static bool ShadowWithholdingEnabled();

		/** @brief Render thread. Takes everything registered since the last call. */
		std::span<const Entry> Drain();
		/** @brief Render thread. The last Drain's entries, for diagnostics. */
		std::span<const Entry> LastDrain() const { return lastDrain; }

		const Stats& GetStats() const { return stats; }
		Stats& MutableStats() { return stats; }

		/**
		 * @brief The batch group list a pass lands in. The engine's classifier reads only the property flags and the alpha property,
		 * so DCLF can compute it without registering.
		 */
		static std::uint32_t SubPassOf(const RE::BSGeometry* a_geometry, std::uint64_t a_propertyFlags);
		/** @brief SubPassOf from the alpha property's testing bit. */
		static std::uint32_t SubPassOf(bool a_alphaTesting, std::uint64_t a_propertyFlags);

	private:
		PassCapture();
		static constexpr std::size_t kCapacity = 32768;

		bool installed = false;
		std::vector<Entry> entries{ kCapacity };
		std::atomic<std::size_t> cursor{ 0 };
		std::atomic<std::uint32_t> overflow{ 0 };
		std::atomic<std::uint32_t> lastThread{ 0 };
		std::atomic<std::uint32_t> threadCount{ 0 };
		Stats stats;
		// Valid until the next frame's registrations.
		std::span<const Entry> lastDrain;
		std::shared_ptr<const ShadowRendererMap> shadowRenderers;
		std::shared_ptr<const ShadowAccumulatorMap> shadowAccumulators;
		std::array<std::atomic<std::uint32_t>, kShadowModes> shadowWithheld{};
		std::atomic<std::uint32_t> volumetricWithheld{ 0 };
		std::atomic<std::uint32_t> directWithheld{ 0 };
		std::atomic<std::uint32_t> mainWithheld{ 0 }, occlusionWithheld{ 0 }, holdWithheld{ 0 }, holdNobody{ 0 };
		std::atomic<std::uint32_t> pendingWithheld{ 0 }, baseMipsWithheld{ 0 };
		mutable std::atomic<std::uint32_t> dclfKept{ 0 };
		mutable std::atomic<std::uint32_t> mainNobodyWaiting{ 0 }, mainNobodyKept{ 0 };
		mutable std::atomic<const RE::BSGeometry*> mainNobodyFirst{ nullptr };
		static constexpr std::size_t kKeptSamples = 256;
		mutable std::mutex keptLock;
		// Admission latency (CS_DCLF_STATS): the geometries undrawn since a step, and the admitted ones' steps, under admissionLock.
		struct Admission
		{
			std::uint32_t first = 0;
			bool waiting = false;
			std::string what;  // name, owner and shader at the first undrawn frame
			// Frames spent on each reason: SetSnapshot::pendingWhy's (kSetWaitReasons, then kPendingComponentWhy), then not pending at all.
			std::array<std::uint16_t, 16> frames{};
		};
		mutable std::vector<std::pair<std::uint32_t, std::string>> admittedSlow;
		mutable std::mutex admissionLock;
		mutable ankerl::unordered_dense::map<const RE::BSGeometry*, Admission> admissionOpen;
		mutable std::vector<std::uint32_t> admittedUntracked, admittedWaiting;
		mutable std::atomic<bool> admissionAny{ false };
		mutable std::uint32_t admissionAbandoned = 0;
		void NoteUndrawn(const RE::BSGeometry* a_geometry, bool a_waiting) const;
		void NoteDrawn(const RE::BSGeometry* a_geometry) const;
		// The load hold's undrawn passes: a line per geometry, the first few (under keptLock). Taken at the hold's release.
		std::vector<std::string> holdNobodySamples;
		ankerl::unordered_dense::set<const RE::BSGeometry*> holdNobodySeen;
		mutable ankerl::unordered_dense::map<const RE::BSGeometry*, KeptSample> keptSamples;
		mutable ankerl::unordered_dense::set<const void*> uncoveredAccumulators;  // under keptLock
		// The covered views' accumulators that kept a pass this frame, under coveredKeptLock. Rare: a caster DCLF does not hold yet.
		mutable std::mutex coveredKeptLock;
		mutable std::vector<const void*> coveredKept;
		mutable std::atomic<bool> anyCoveredKept{ false };
		// Under mapLock.
		std::atomic<bool> mapCensus{ false };
		mutable std::mutex mapLock;
		mutable ankerl::unordered_dense::map<std::uint64_t, MapClass> mapClasses;
		// Compared by address only, under mapLock.
		mutable ankerl::unordered_dense::set<const RE::BSGeometry*> mapKept;
		void NoteMapPass(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, bool a_withheld) const;
		std::array<std::atomic<std::uint32_t>, static_cast<std::size_t>(Api::DrawFamily::Count)> familyWithheld{};
		std::array<std::atomic<bool>, static_cast<std::size_t>(Api::DrawFamily::Count)> familyHeld{};
		static_assert(static_cast<std::size_t>(Api::DrawFamily::Count) <= 32, "ViewRenderers::familyOwned is a 32-bit mask by family");
		/** @brief The OwnRenderers views' test at every main-mode insertion point, after WithholdMain. */
		bool WithholdOwn(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass);
		std::atomic<std::uint8_t> occlusionPhase{ 0 };
		struct OcclusionGroupHook;
		friend struct OcclusionGroupHook;
		std::shared_ptr<const SetSnapshot> frameSet;
		using RendererSet = ankerl::unordered_dense::set<const RE::BSBatchRenderer*>;
		std::shared_ptr<const RendererSet> mainRenderers;
		std::array<const void*, 2> mainAccumulators{};  // render thread
		std::vector<const RE::BSBatchRenderer*> mainSources;  // render thread
		// Per view: its renderers, their source, its coverage (0 not set yet, 1 not covered, 2 covered), its gates and counts.
		struct ViewRenderers
		{
			std::shared_ptr<const RendererSet> renderers;
			Api::ViewAccumulators accumulators;
			std::vector<const RE::BSBatchRenderer*> sources;  // render thread
			std::atomic<std::uint8_t> coverage{ 0 };
			std::atomic<std::uint32_t> withheld{ 0 }, memberKept{ 0 }, registered{ 0 };
			std::atomic<bool> rendering{ false }, withholdActive{ false };
			std::atomic<std::uint32_t> familyOwned{ 0 };  // a bit per draw family
			std::atomic<std::uint32_t> ownWithheld{ 0 };
			std::array<std::atomic<std::uint32_t>, static_cast<std::size_t>(Api::DrawFamily::Count)> ownFamilyWithheld{};
		};
		std::array<ViewRenderers, Extensions::kEngineViews> viewRenderers;
		/**
		 * @brief The main-view test at every insertion point. A main view's group renderer made this frame counts too.
		 */
		bool WithholdMain(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass);
		/** @brief True, and counted, while a load's hold is in effect and a_pass is of DCLF's classes. Call only for views DCLF draws. */
		bool HeldByLoad(const RE::BSRenderPass* a_pass);
		/** @brief Whether the engine must not draw a_geometry in a_phase. A pass withheld only because its object waits is counted. */
		bool HeldPhase(const SetSnapshot& a_set, const RE::BSGeometry* a_geometry, std::uint8_t a_phase);
		void NotePending(const SetSnapshot& a_set, const RE::BSGeometry* a_geometry, std::uint8_t a_phases);
		template <std::uint32_t Group>
		struct MainGroupHook;
		template <std::uint32_t Group>
		friend struct MainGroupHook;
		struct MainListHook;
		friend struct MainListHook;
		/** @brief The shadow test at a direct group insertion. True when the pass is withheld. */
		bool WithholdAtGroup(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::atomic<std::uint32_t>& a_counter);
		struct VolumetricGroupHook;
		friend struct VolumetricGroupHook;
		template <std::uint32_t Hint>
		struct DirectGroupHook;
		template <std::uint32_t Hint>
		friend struct DirectGroupHook;

		struct Hook;
		friend struct Hook;
		void Record(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_withheld);
		bool Withhold(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass);
		/** @brief Counts a pass handed to the engine, with a sample by geometry. Nothing is vouched for: the engine's context drops its draws. */
		void KeepForEngine(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass) const;
		/**
		 * @brief Whether a_batch is a main-view renderer now, read live. The engine makes a group's renderer at its first pass, after the
		 * render thread built the sets, and that frame's passes must still be withheld.
		 */
		bool MainRendererNow(const RE::BSBatchRenderer* a_batch) const;
	};
}
