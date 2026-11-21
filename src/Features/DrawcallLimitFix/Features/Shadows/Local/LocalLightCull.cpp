// Point-light shadow culls that skip the entries DCLF draws. LocalLightCull.h has the overview.
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Core/Handoff/WholeCull.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Engine/FrameAddressSet.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "RenderGraph/Async/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/SunExclusion.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/Portals/StrictGates.h"
#include "RenderGraph/Fatal.h"

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <Tracy/Tracy.hpp>

namespace DCLF::Features::Shadows::Local::LocalLightCull
{
	namespace
	{
		constexpr DCLF::Engine::Offset kParabolicProcess1{ { 108600 }, { 101597 } };  // BSParabolicCullingProcess::Process1, vtable slot 0x16

		bool installed = false;
		// This frame's exclusion. Held until the next selection because the light culls that read it run in between.
		std::shared_ptr<SunExclusion> frameHeld;
		std::atomic<const SunExclusion*> frameExclusion{ nullptr };

		struct Stats
		{
			std::atomic<std::uint64_t> visited{ 0 }, skipped{ 0 };
			std::uint64_t frames = 0, live = 0, noExclusion = 0, noClaims = 0, stale = 0, masked = 0;
		};
		Stats stats;

		/** @brief The candidate index of a_object when it is an entry the exclusion excludes, else ~0u. */
		std::uint32_t ExcludedIndex(const SunExclusion& a_exclusion, const void* a_object)
		{
			const auto& entries = a_exclusion.candidates->entries;
			const auto it = entries.find(static_cast<const RE::NiAVObject*>(a_object));
			return it != entries.end() && a_exclusion.excluded[it->second] ? it->second : ~0u;
		}
		bool Excluded(const SunExclusion& a_exclusion, const void* a_object) { return ExcludedIndex(a_exclusion, a_object) != ~0u; }

		constexpr DCLF::Engine::Offset kNodeOnVisible{ { 70307 }, { 68956 } };  // NiNode::OnVisible, VTABLE_NiNode slot 0x34
		constexpr DCLF::Engine::Offset kCull{ { 70267 }, { 68916 } };           // NiAVObject::Cull(object, process, arg)
		constexpr std::uint32_t kAlwaysDraw = 1u << 11;

		bool filterInstalled = false;

		/** @brief One category node's children less its excluded entries, in child order. */
		struct CategoryFilter
		{
			// Held so the node is not freed or its address reused while a snapshot still names it. Released with the snapshot at Present.
			RE::NiPointer<RE::NiNode> node;
			std::vector<RE::NiPointer<RE::NiAVObject>> children;
			std::uint32_t cut = 0;               // the excluded entries left out
			std::uint32_t empty = 0;             // and the children holding no geometry (a tracked category node's)
			std::atomic<bool> dirty{ false };    // a child attached or detached since: the node is walked natively
			// The point lights whose cull reached the node's children this frame, by reach kind (stamped words: NoteReach).
			mutable std::array<std::atomic<std::uint64_t>, 3> reach{};
		};
		/** @brief A snapshot: built on DCLF's pool for a selection, read by the light culls and the detours until retired. */
		struct Filter
		{
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::shared_ptr<CategoryFilter>> nodes;
			const EntrySpace* candidates = nullptr;
			std::uint64_t exclusionVersion = ~0ull;
			std::uint64_t entriesAppeared = 0;  // FrameView::GetLightEntriesAppeared at the build
		};
		std::shared_ptr<const Filter> filterBuilt;
		std::atomic<const Filter*> filterCurrent{ nullptr };  // the detours' (any thread), the built one
		std::atomic<const Filter*> frameFilter{ nullptr };    // this frame's light culls': null on frames they cull whole
		std::atomic<std::uint64_t> dirtied{ 0 };              // nodes made dirty, ever
		std::uint64_t dirtiedSeen = 0;
		// Retired snapshots, released two Presents later: a detour that loaded one just before it was retired may still read it.
		std::array<std::vector<std::shared_ptr<const Filter>>, 2> retired;
		thread_local const Filter* lightFilter = nullptr;  // set for the call by the Accumulate thunk

		struct FilterStats
		{
			std::atomic<std::uint64_t> lights{ 0 }, filtered{ 0 }, walkedOwn{ 0 }, walkedNotLive{ 0 };
			std::atomic<std::uint64_t> nodes{ 0 }, children{ 0 }, cut{ 0 }, dirtyNodes{ 0 };
			std::uint64_t builds = 0, nodesRebuilt = 0, stale = 0, refused = 0;
			std::int64_t buildTicks = 0;
		};
		FilterStats filterStats;

		constexpr std::size_t kProcessCullMode = 0x30198;         // BSCullingProcess::cullMode
		constexpr std::size_t kProcessCompoundFrustum = 0x301A0;  // BSCullingProcess::compoundFrustum
		constexpr std::size_t kFrustumFreeOp = 0xBC;              // BSCompoundFrustum::freeOp
		constexpr std::size_t kProcessLightCentre = 0x30218;      // FUN_14151a1e0's sphere: the light's centre, then its radius
		constexpr std::size_t kProcessLightRadius = 0x30224;
		constexpr std::size_t kAccumulatorLightIndex = 0x160;     // the shadow-light count + 1 (0: no mask write, 0xFFFF: a clear)
		constexpr std::size_t kAccumulatorLightBit = 0x164;
		constexpr std::uint32_t kMaxLights = 32;
		constexpr std::uint32_t kNoLight = ~0u;
		// How a light's cull reached a node's children. Mode 4 adds the portal compound frustum, which DCLF does not test.
		enum ReachKind : std::uint32_t
		{
			kReachAll,
			kReachTested,
			kReachPortal,
			kReachKinds
		};
		static_assert(kReachKinds == kReachKindCount);

		/** @brief A point light of the frame, by Accumulate order: its mask bit, and the sphere its process tested. */
		struct FrameLight
		{
			std::uint32_t bit = 0;
			float centre[3]{};
			float radius = 0.0f;
			bool captured = false;
			// The sphere the light's cull will test (FUN_141519d20). Empty culls test against it, and parity frames compare it.
			float predictedCentre[3]{};
			float predictedRadius = 0.0f;
			bool predicted = false;
			// Portal-strict lights also pass their strict gates (Portals/StrictGates.h).
			const RE::BSShadowLight* light = nullptr;
			bool strict = false;
			// A spot light's cull tests its process's planes, not a sphere. The predicted volume is what it will test.
			bool spot = false;
			ShadowCameras::Planes volume;
		};
		std::array<FrameLight, kMaxLights> frameLights;
		std::atomic<std::uint32_t> frameLightNext{ 0 };      // the next light's ordinal (from the selection)
		std::atomic<std::uint32_t> lightStamp{ 1 };          // the frame's: the reach words' and the consumed geometries'
		std::atomic<bool> lightsReady{ false };              // a light accumulated since the selection
		thread_local std::uint32_t currentLight = kNoLight;  // the ordinal of the light whose Accumulate runs on this thread

		constexpr std::size_t kLightPortalStrict = 0x47;    // BSLight::portalStrict: its cull tests the portals' compound frustum
		std::atomic<bool> frameCullsEmpty{ false };         // the selection's: the point lights' casters withheld
		std::atomic<bool> frameSpotsEmpty{ false };         // the selection's: the spot views' casters withheld
		std::atomic<std::uint32_t> emptyLights{ 0 };        // the ordinals of the frame's lights culled empty
		FrameAddressSet mainSeen;                           // the geometries whose first clearing main registration this frame was seen
		struct EmptyStats
		{
			std::atomic<std::uint64_t> lights{ 0 }, notCovered{ 0 }, portalStrict{ 0 }, given{ 0 }, withBits{ 0 }, unstamped{ 0 };
			// Newly selected lights with no views yet. They cast no shadow until the views exist.
			std::atomic<std::uint64_t> late{ 0 };
			std::atomic<std::uint64_t> spots{ 0 }, spotsNotCovered{ 0 }, spotsStrict{ 0 };
			// Portal-strict counters. No graph means nothing to gate. No context means the bit is given ungated.
			std::atomic<std::uint64_t> strictLights{ 0 }, strictTested{ 0 }, strictRefused{ 0 }, strictNoGraph{ 0 }, strictNoContext{ 0 };
		};
		EmptyStats emptyStats;

		struct BitStats
		{
			std::atomic<std::uint64_t> lights{ 0 }, overflow{ 0 }, given{ 0 }, withBits{ 0 };
		};
		BitStats bitStats;

		/**
		 * @brief Starts a new frame of lights at the selection, on the render thread.
		 * Main::Draw's mask clear (FUN_1414cb640) runs only in third person, so this cannot rely on it.
		 */
		void ResetFrameLights()
		{
			lightsReady.store(false, std::memory_order_relaxed);
			frameLightNext.store(0, std::memory_order_relaxed);
			emptyLights.store(0, std::memory_order_relaxed);
			for (auto& light : frameLights) {
				light.captured = light.predicted = light.strict = light.spot = false;
				light.light = nullptr;
			}
			// The last frame's main registrations ended at its Main::Draw.
			mainSeen.NewRound();
			if (lightStamp.fetch_add(1, std::memory_order_relaxed) + 1 == 0)
				lightStamp.store(1, std::memory_order_relaxed);
		}

		/** @brief Inside a light's cull: it reached the children of a node (a_words: the node's or the entry's reach words). */
		void NoteReach(const RE::NiCullingProcess* a_process, std::atomic<std::uint64_t>* a_words)
		{
			const std::uint32_t light = currentLight;
			if (light == kNoLight || !a_words)
				return;
			const auto mode = DCLF::Engine::At<std::uint32_t>(a_process, kProcessCullMode);
			if (mode == 2)  // kAllFail
				return;
			const void* frustum = DCLF::Engine::At<void*>(a_process, kProcessCompoundFrustum);
			const std::uint32_t kind = mode == 1                                                                     ? kReachAll :
			                           frustum && DCLF::Engine::At<std::uint32_t>(frustum, kFrustumFreeOp) != 0 && mode != 3 ? kReachPortal :
			                                                                                                          kReachTested;
			// The static process is shared by every point light. Its sphere is this light's only during its cull.
			auto& sphere = frameLights[light];
			if (!sphere.captured) {
				std::memcpy(sphere.centre, &DCLF::Engine::At<float>(a_process, kProcessLightCentre), sizeof(sphere.centre));
				sphere.radius = DCLF::Engine::At<float>(a_process, kProcessLightRadius);
				sphere.captured = true;
			}
			const std::uint64_t stamp = static_cast<std::uint64_t>(lightStamp.load(std::memory_order_relaxed)) << 32;
			auto& word = a_words[kind];
			std::uint64_t old = word.load(std::memory_order_relaxed);
			for (;;) {
				const std::uint64_t next = ((old & ~0xFFFFFFFFull) == stamp ? old : stamp) | (1ull << light);
				if (next == old || word.compare_exchange_weak(old, next, std::memory_order_relaxed))
					break;
			}
		}

		/** @brief The lights of a reach word, this frame's. */
		std::uint32_t ReachOf(const std::atomic<std::uint64_t>& a_word)
		{
			const std::uint64_t word = a_word.load(std::memory_order_relaxed);
			return (word >> 32) == lightStamp.load(std::memory_order_relaxed) ? static_cast<std::uint32_t>(word) : 0u;
		}

		/** @brief FUN_14151a1e0, a bound against a light's sphere (its half-space test culls nothing inside the sphere). */
		bool InSphere(const FrameLight& a_light, const RE::NiBound& a_bound)
		{
			const float dx = a_bound.center.x - a_light.centre[0];
			const float dy = a_bound.center.y - a_light.centre[1];
			const float dz = a_bound.center.z - a_light.centre[2];
			return (std::sqrt(dx * dx + dy * dy + dz * dz) - a_bound.radius) - a_light.radius < 0.0f;
		}

		/** @brief A bound against a spot light's predicted volume (its process's planes: outside one when dot(n, c) - d < -r). */
		bool InVolume(const FrameLight& a_light, const RE::NiBound& a_bound)
		{
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!(a_light.volume.mask & (1u << p)))
					continue;
				const auto& plane = a_light.volume.plane[p];
				if (plane[0] * a_bound.center.x + plane[1] * a_bound.center.y + plane[2] * a_bound.center.z - plane[3] < -a_bound.radius)
					return false;
			}
			return true;
		}

		/**
		 * @brief Whether a portal-strict light's gates admit a geometry in its sphere (Portals/StrictGates.h).
		 * Safe on any thread because the selection's frame is not written until the next frame.
		 */
		bool StrictAdmits(const FrameLight& a_light, const RE::BSGeometry& a_geometry)
		{
			if (!a_light.strict)
				return true;
			emptyStats.strictTested.fetch_add(1, std::memory_order_relaxed);
			Portals::ContextHold context;
			std::uint32_t walk = PortalViews::kLightUntouched;
			if (const auto* selection = LightSelection::Get().Current(Scene::FrameView::Get().SceneStep()))
				if (const auto* entry = selection->LightOf(a_light.light)) {
					context = entry->portalContext;
					walk = entry->portalWalk;
				}
			if (!context) {
				(walk == PortalViews::kLightNoGraph ? emptyStats.strictNoGraph : emptyStats.strictNoContext).fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			thread_local Portals::GateChain chain;
			Portals::ChainOf(&a_geometry, chain);
			const float centre[3]{ a_geometry.worldBound.center.x, a_geometry.worldBound.center.y, a_geometry.worldBound.center.z };
			const bool admitted = Portals::Admit(*context, chain, centre, a_geometry.worldBound.radius);
			emptyStats.strictRefused.fetch_add(admitted ? 0 : 1, std::memory_order_relaxed);
			return admitted;
		}

		bool Hidden(const RE::NiAVObject* a_object) { return DCLF::Engine::IsHidden(a_object); }

		/** @brief A node whose OnVisible is NiNode's own through the vtable: exactly NiNode (the category nodes). */
		bool ExactNiNode(const RE::NiAVObject* a_object)
		{
			static const REL::Relocation<const RE::NiRTTI*> niNode{ RE::NiNode::Ni_RTTI };
			return a_object->GetRTTI() == niNode.get();
		}

		/**
		 * @brief Whether a subtree holds no geometry.
		 * Geometry attached later under a tracked category node rebuilds the filter when its entry appears.
		 */
		bool HoldsNoGeometry(const RE::NiAVObject* a_object)
		{
			// Past 64 levels it counts as holding one.
			return DCLF::Engine::ForEachLive(a_object, [](RE::NiAVObject& a_node, std::uint32_t a_depth) { return a_depth <= 64 && !a_node.AsGeometry(); });
		}

		std::shared_ptr<CategoryFilter> BuildCategory(const RE::NiNode& a_node, const SunExclusion& a_exclusion)
		{
			auto filter = std::make_shared<CategoryFilter>();
			filter->node.reset(const_cast<RE::NiNode*>(&a_node));
			const auto& children = a_node.GetChildren();
			const bool tracked = Scene::FrameView::Get().IsCategoryNode(&a_node);
			filter->children.reserve(children.size());
			for (std::uint16_t i = 0; i < children.free_idx(); ++i) {
				auto* child = children[i].get();
				if (!child)
					continue;
				if (Excluded(a_exclusion, child))
					++filter->cut;
				else if (tracked && !Scene::FrameView::Get().IsLightEntry(child) && HoldsNoGeometry(child))
					++filter->empty;
				else
					filter->children.emplace_back(child);
			}
			return filter;
		}

		/** @brief A filter build on the pool and what it was made from; its result goes back to the render thread whole. */
		struct FilterBuild
		{
			std::shared_ptr<SunExclusion> exclusion;  // held for the build, dropped on the render thread
			std::shared_ptr<const Filter> base;       // the nodes a partial build shares; null for a whole one
			std::uint64_t appeared = 0, dirtied = 0;
			bool appearedNow = false;
			std::shared_ptr<const Filter> built;      // null: a lease was refused (the window closed); built again
			std::uint64_t nodesRebuilt = 0;
			std::int64_t ticks = 0;
		};
		std::atomic<FilterBuild*> filterReady{ nullptr };  // the pool's result, taken at the next selection
		bool filterInFlight = false;                        // render thread: a build is on the pool

		/** @brief Builds the filter on the pool, whole for a new verdict or else rebuilding only the dirty nodes. */
		void BuildFilter(FilterBuild& a_build)
		{
			DCLF_ZONE("CS.DCLF.LightCullFilter");
			LARGE_INTEGER start{}, end{};
			QueryPerformanceCounter(&start);
			const auto& exclusion = *a_build.exclusion;
			auto next = std::make_shared<Filter>();
			next->candidates = exclusion.candidates.get();
			next->exclusionVersion = exclusion.version;
			next->entriesAppeared = a_build.appeared;
			if (!a_build.base) {
				for (const auto& [entry, index] : exclusion.candidates->entries) {
					if (!exclusion.excluded[index])
						continue;
					const EngineReadWindow::Lease lease;
					if (!lease)
						return;
					// The parent read once: a loader thread may detach the entry meanwhile.
					auto* parent = entry->parent;
					if (!parent || !ExactNiNode(parent) || next->nodes.contains(parent))
						continue;
					next->nodes.emplace(parent, BuildCategory(*parent, exclusion));
					++a_build.nodesRebuilt;
				}
			} else {
				next->nodes = a_build.base->nodes;
				for (auto& [node, category] : next->nodes)
					if (category->dirty.load(std::memory_order_acquire) || (a_build.appearedNow && category->empty)) {
						const EngineReadWindow::Lease lease;
						if (!lease)
							return;
						category = BuildCategory(*category->node, exclusion);
						++a_build.nodesRebuilt;
					}
			}
			a_build.built = std::move(next);
			QueryPerformanceCounter(&end);
			a_build.ticks = end.QuadPart - start.QuadPart;
		}

		/**
		 * @brief Installs a finished filter or starts a new build, on the render thread at the selection.
		 * Until a filter for the frame's verdict lands, the light culls walk every category node natively.
		 */
		void UpdateFilter(const std::shared_ptr<SunExclusion>& a_exclusion)
		{
			if (std::unique_ptr<FilterBuild> done{ filterReady.exchange(nullptr, std::memory_order_acq_rel) }) {
				filterInFlight = false;
				filterStats.nodesRebuilt += done->nodesRebuilt;
				if (!done->built) {
					++filterStats.refused;
				} else if (a_exclusion && done->built->exclusionVersion == a_exclusion->version) {
					dirtiedSeen = done->dirtied;
					if (filterBuilt)
						retired[0].push_back(std::move(filterBuilt));
					filterBuilt = std::move(done->built);
					++filterStats.builds;
					filterStats.buildTicks += done->ticks;
				} else {
					++filterStats.stale;
					retired[0].push_back(std::move(done->built));
				}
				// Engine references are released here on the render thread.
				if (done->base)
					retired[0].push_back(std::move(done->base));
			}
			if (!a_exclusion) {
				filterCurrent.store(nullptr, std::memory_order_release);
				if (filterBuilt)
					retired[0].push_back(std::move(filterBuilt));
				return;
			}
			const std::uint64_t appeared = Scene::FrameView::Get().GetLightEntriesAppeared();
			// A verdict translated to newer candidates keeps the same version. SelectFrame marked the moved entries' nodes dirty.
			const bool current = filterBuilt && filterBuilt->exclusionVersion == a_exclusion->version;
			// An entry appeared: the nodes that cut a child for holding no geometry judge it again (it may hold one now).
			const bool appearedNow = current && filterBuilt->entriesAppeared != appeared;
			const std::uint64_t dirtyNow = dirtied.load(std::memory_order_acquire);
			filterCurrent.store(current ? filterBuilt.get() : nullptr, std::memory_order_release);
			if ((current && !appearedNow && dirtyNow == dirtiedSeen) || filterInFlight)
				return;
			auto build = std::make_unique<FilterBuild>();
			build->exclusion = a_exclusion;
			build->base = current ? filterBuilt : nullptr;
			build->appeared = appeared;
			build->dirtied = dirtyNow;
			build->appearedNow = appearedNow;
			filterInFlight = true;
			if (!::RenderGraph::Frame::SharedExecutor().Dispatch(SceneScheduler::Scope(), ::RenderGraph::Frame::Executor::Preparation, org::async::TaskDispatch::Cpu, "CS.DCLF.LightCullFilter",
					[build = build.release()](const auto&) {
						BuildFilter(*build);
						filterReady.store(build, std::memory_order_release);
					}))
				::RenderGraph::Fatal("Drawcall Limit Fix", "the point lights' category filter was refused by DCLF's executor");
		}

		/**
		 * @brief NiNode::OnVisible detour (AE 0x140d1e2c0, vtable slot 0x34).
		 * Inside a point light's cull, a filtered category node culls its children less the excluded entries.
		 */
		struct NodeOnVisible
		{
			static void thunk(RE::NiNode* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
			{
				if (const auto* filter = currentLight != kNoLight ? filterCurrent.load(std::memory_order_acquire) : nullptr)
					if (const auto it = filter->nodes.find(a_node); it != filter->nodes.end()) {
						const auto& category = *it->second;
						const bool children = a_node->worldBound.radius != 0.0f || (a_node->GetFlags().underlying() & kAlwaysDraw);
						if (children)
							NoteReach(a_process, category.reach.data());
						if (lightFilter == filter && !category.dirty.load(std::memory_order_acquire)) {
							if (children) {
								using Cull = void (*)(RE::NiAVObject*, RE::NiCullingProcess*, std::int32_t);
								static const REL::Relocation<Cull> cull{ REL::Offset(kCull.Get()) };
								for (const auto& child : category.children)
									cull(child.get(), a_process, a_arg);
								filterStats.children.fetch_add(category.children.size(), std::memory_order_relaxed);
								filterStats.cut.fetch_add(category.cut + category.empty, std::memory_order_relaxed);
							}
							filterStats.nodes.fetch_add(1, std::memory_order_relaxed);
							return;
						}
						if (lightFilter == filter)
							filterStats.dirtyNodes.fetch_add(1, std::memory_order_relaxed);
					}
				func(a_node, a_process, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The point lights' bits for a geometry of a cut entry, from each light that reached it. */
		std::uint32_t BitsOf(const SunExclusion& a_exclusion, std::uint32_t a_entry, const RE::BSGeometry& a_geometry, std::array<std::uint32_t, kReachKinds>& a_reach)
		{
			a_reach = {};
			if (a_exclusion.lightReach)
				for (std::uint32_t k = 0; k < kReachKinds; ++k)
					a_reach[k] = ReachOf(a_exclusion.lightReach[a_entry * kReachKinds + k]);
			const auto& nodes = a_exclusion.candidates->entryNodes;
			const auto* node = a_entry < nodes.size() ? nodes[a_entry] : nullptr;
			if (const auto* filter = node && node->parent ? filterCurrent.load(std::memory_order_acquire) : nullptr)
				if (const auto it = filter->nodes.find(node->parent); it != filter->nodes.end())
					for (std::uint32_t k = 0; k < kReachKinds; ++k)
						a_reach[k] |= ReachOf(it->second->reach[k]);
			std::uint32_t bits = 0;
			for (std::uint32_t lights = a_reach[kReachAll] | a_reach[kReachTested] | a_reach[kReachPortal]; lights; lights &= lights - 1) {
				const auto light = static_cast<std::uint32_t>(std::countr_zero(lights));
				// A portal-strict light reached the entry through its gates: the geometry's own bound still meets its room frustum.
				if (((a_reach[kReachAll] >> light) & 1) || (InSphere(frameLights[light], a_geometry.worldBound) && StrictAdmits(frameLights[light], a_geometry)))
					bits |= frameLights[light].bit;
			}
			return bits;
		}

		/** @brief The bits a_lights give a_geometry by predicted sphere, with portal-strict lights also gated. */
		std::uint32_t SphereBits(std::uint32_t a_lights, const RE::BSGeometry& a_geometry)
		{
			const auto& bound = a_geometry.worldBound;
			std::uint32_t bits = 0;
			for (; a_lights; a_lights &= a_lights - 1) {
				const auto& light = frameLights[std::countr_zero(a_lights)];
				if (!light.predicted)
					continue;
				if (light.spot) {
					if (InVolume(light, bound) && StrictAdmits(light, a_geometry))
						bits |= light.bit;
					continue;
				}
				FrameLight sphere;
				std::memcpy(sphere.centre, light.predictedCentre, sizeof(sphere.centre));
				sphere.radius = light.predictedRadius;
				if (InSphere(sphere, bound) && StrictAdmits(light, a_geometry))
					bits |= light.bit;
			}
			return bits;
		}

		/** @brief The sphere a light's cull will test (FUN_141519d20, from Accumulate's descriptor: its camera and the light's radius). */
		void Predict(RE::BSShadowLight& a_light, FrameLight& a_frameLight)
		{
			auto& descriptors = a_light.GetRuntimeData().shadowmapDescriptors;
			const auto* camera = descriptors.empty() ? nullptr : descriptors[0].camera.get();
			auto* niLight = a_light.light.get();
			if (!camera || !niLight)
				return;
			a_frameLight.predictedCentre[0] = camera->world.translate.x;
			a_frameLight.predictedCentre[1] = camera->world.translate.y;
			a_frameLight.predictedCentre[2] = camera->world.translate.z;
			a_frameLight.predictedRadius = niLight->GetLightRuntimeData().radius.x;
			a_frameLight.spot = a_light.GetIsFrustumLight();
			if (a_frameLight.spot) {
				const auto* process = static_cast<const RE::NiCullingProcess*>(descriptors[0].cullingProcess);
				if (!process)
					return;
				ShadowCameras::CullPlanes(process, *camera, a_frameLight.volume);
			}
			a_frameLight.predicted = true;
		}

		/** @brief How a light's Accumulate culls (CullEmpty). */
		enum class LightCull
		{
			Engine,  // a view not DCLF's (or no selection): the engine's cull as it is
			Empty    // DCLF's views: no cull, the bookkeeping done here
		};

		/**
		 * @brief Decides how a light's Accumulate culls.
		 * Empty skips the cull, which would otherwise take the accumulation lock against the main camera's list jobs.
		 */
		LightCull CullEmpty(RE::BSShadowLight& a_light, std::uint32_t a_ordinal, std::uint32_t* a_count)
		{
			auto& descriptors = a_light.GetRuntimeData().shadowmapDescriptors;
			const bool spot = frameLights[a_ordinal].spot;
			const auto kind = spot ? ShadowViews::Kind::Frustum : ShadowViews::Kind::Parabolic;
			std::uint32_t views = 0;
			bool covered = true;
			for (const auto& view : ShadowViews::Get().All()) {
				if (view.light != &a_light || view.focus)
					continue;
				++views;
				covered = covered && view.kind == kind && view.covered && PassCapture::Get().CastersForBatch(view.batch);
			}
			auto* accumulator = descriptors.empty() ? nullptr : descriptors[0].shaderAccumulator.get();
			// No accumulator or no predicted planes: nothing to give its bits by, the engine's cull as it is.
			if (!accumulator || !frameLights[a_ordinal].predicted) {
				(spot ? emptyStats.spotsNotCovered : emptyStats.notCovered).fetch_add(1, std::memory_order_relaxed);
				return LightCull::Engine;
			}
			// A light whose views DCLF has not made yet is not handed to the engine's cull. It casts no shadow until they exist.
			const bool late = !covered || views < descriptors.size();
			if (late)
				emptyStats.late.fetch_add(1, std::memory_order_relaxed);
			// Portal-strict lights are always gated by DCLF's strict gates. No switch hands them back to the engine's whole cull.
			if (DCLF::Engine::At<bool>(&a_light, kLightPortalStrict))
				emptyStats.strictLights.fetch_add(1, std::memory_order_relaxed);
			const std::uint32_t count = *a_count;
			DCLF::Engine::At<std::uint32_t>(accumulator, kAccumulatorLightIndex) = count + 1;
			DCLF::Engine::At<std::uint32_t>(accumulator, kAccumulatorLightBit) = count < 0x1F ? 1u << count : 0u;
			// A spot light counts one view, a point light one per hemisphere.
			*a_count = count + (!spot && a_light.shadowMapCount == 2 ? 2 : 1);
			emptyLights.fetch_or(1u << a_ordinal, std::memory_order_relaxed);
			(spot ? emptyStats.spots : emptyStats.lights).fetch_add(1, std::memory_order_relaxed);
			return LightCull::Empty;
		}

		/** @brief Counts a light the engine culls whole while DCLF draws the shadows. */
		void NoteWholeLight(std::uint32_t a_ordinal, bool a_spot)
		{
			if (!installed || !PassCapture::ShadowWithholdingEnabled())
				return;
			using W = WholeCull::Reason;
			WholeCull::Note(a_ordinal == kNoLight ? (a_spot ? W::SpotPastMask : W::PointPastMask) : (a_spot ? W::SpotNotCovered : W::PointNotCovered));
		}

		/** @brief A light's ordinal for its Accumulate (kNoLight: DCLF not running, or past the mask's 32). */
		std::uint32_t TakeOrdinal()
		{
			if (!installed)
				return kNoLight;
			const std::uint32_t ordinal = frameLightNext.fetch_add(1, std::memory_order_relaxed);
			if (ordinal < kMaxLights)
				return ordinal;
			bitStats.overflow.fetch_add(1, std::memory_order_relaxed);
			return kNoLight;
		}

		/** @brief After a light's Accumulate: the bit its registration ORs into every geometry it reaches (FUN_1414b2140: the accumulator's +0x164). */
		void NoteLightBit(RE::BSShadowLight& a_light, std::uint32_t a_ordinal)
		{
			std::uint32_t bit = 0;
			for (const auto& descriptor : a_light.GetRuntimeData().shadowmapDescriptors)
				if (const auto* accumulator = descriptor.shaderAccumulator.get()) {
					const auto index = DCLF::Engine::At<std::uint32_t>(accumulator, kAccumulatorLightIndex);
					if (index != 0 && index != 0xFFFF)
						bit |= DCLF::Engine::At<std::uint32_t>(accumulator, kAccumulatorLightBit);
				}
			frameLights[a_ordinal].bit = bit;
			bitStats.lights.fetch_add(1, std::memory_order_relaxed);
			lightsReady.store(true, std::memory_order_release);
		}

		// BSShadowParabolicLight::Accumulate (vtable slot 9, 0x14151b960).
		std::atomic<std::int64_t> accumulateTicks{ 0 };
		std::atomic<std::uint64_t> accumulateCalls{ 0 };
		struct Accumulate
		{
			static void thunk(void* a_light, std::uint32_t* a_count, std::uint32_t* a_arg2, RE::NiAVObject* a_arg3)
			{
				LARGE_INTEGER start{}, end{};
				QueryPerformanceCounter(&start);
				// Past the mask's 32 lights a light culls everything.
				const std::uint32_t ordinal = TakeOrdinal();
				auto& light = *static_cast<RE::BSShadowLight*>(a_light);
				if (ordinal != kNoLight) {
					Predict(light, frameLights[ordinal]);
					frameLights[ordinal].light = &light;
					frameLights[ordinal].strict = DCLF::Engine::At<bool>(&light, kLightPortalStrict);
				}
				const auto mode = ordinal != kNoLight && !a_arg3 && frameCullsEmpty.load(std::memory_order_relaxed) ? CullEmpty(light, ordinal, a_count) :
				                                                                                                          LightCull::Engine;
				if (mode == LightCull::Engine && !a_arg3)
					NoteWholeLight(ordinal, false);
				if (mode != LightCull::Empty) {
					const Filter* filter = nullptr;
					if (filterInstalled) {
						filterStats.lights.fetch_add(1, std::memory_order_relaxed);
						filter = ordinal != kNoLight ? frameFilter.load(std::memory_order_acquire) : nullptr;
						(filter ? filterStats.filtered : filterStats.walkedNotLive).fetch_add(1, std::memory_order_relaxed);
					}
					currentLight = ordinal;
					lightFilter = filter;
					func(a_light, a_count, a_arg2, a_arg3);
					lightFilter = nullptr;
					currentLight = kNoLight;
				}
				if (ordinal != kNoLight) {
					NoteLightBit(light, ordinal);
				}
				QueryPerformanceCounter(&end);
				accumulateTicks.fetch_add(end.QuadPart - start.QuadPart, std::memory_order_relaxed);
				accumulateCalls.fetch_add(1, std::memory_order_relaxed);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// BSShadowFrustumLight::Accumulate (vtable slot 9, AE 0x14151aa60). Culled empty like a point light.
		std::atomic<std::int64_t> spotTicks{ 0 };
		std::atomic<std::uint64_t> spotCalls{ 0 };
		struct SpotAccumulate
		{
			static void thunk(void* a_light, std::uint32_t* a_count, std::uint32_t* a_arg2, RE::NiAVObject* a_arg3)
			{
				LARGE_INTEGER start{}, end{};
				QueryPerformanceCounter(&start);
				const std::uint32_t ordinal = TakeOrdinal();
				auto& light = *static_cast<RE::BSShadowLight*>(a_light);
				if (ordinal != kNoLight) {
					Predict(light, frameLights[ordinal]);
					frameLights[ordinal].light = &light;
					frameLights[ordinal].strict = DCLF::Engine::At<bool>(&light, kLightPortalStrict);
				}
				const auto mode = ordinal != kNoLight && !a_arg3 && frameSpotsEmpty.load(std::memory_order_relaxed) ? CullEmpty(light, ordinal, a_count) :
				                                                                                                         LightCull::Engine;
				if (mode == LightCull::Engine && !a_arg3)
					NoteWholeLight(ordinal, true);
				if (mode == LightCull::Engine)
					func(a_light, a_count, a_arg2, a_arg3);
				if (ordinal != kNoLight)
					NoteLightBit(light, ordinal);
				QueryPerformanceCounter(&end);
				spotTicks.fetch_add(end.QuadPart - start.QuadPart, std::memory_order_relaxed);
				spotCalls.fetch_add(1, std::memory_order_relaxed);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Process1
		{
			static void thunk(RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_arg)
			{
				if (a_object && currentLight != kNoLight)
					if (const auto* exclusion = frameExclusion.load(std::memory_order_acquire)) {
						stats.visited.fetch_add(1, std::memory_order_relaxed);
						if (const std::uint32_t index = ExcludedIndex(*exclusion, a_object); index != ~0u) {
							if (exclusion->lightReach)
								NoteReach(a_process, &exclusion->lightReach[index * kReachKinds]);
							stats.skipped.fetch_add(1, std::memory_order_relaxed);
							return;
						}
					}
				func(a_process, a_object, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Preflight()
	{
		const auto base = REL::Module::get().base();
		HookSites::RequireSlot("BSShadowParabolicLight::Accumulate", REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSShadowParabolicLight[0] }.address(), 0x9);
		HookSites::RequireSlot("BSShadowFrustumLight::Accumulate", REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSShadowFrustumLight[0] }.address(), 0x9);
		HookSites::RequireSlot("BSParabolicCullingProcess::Process1", REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSParabolicCullingProcess[0] }.address(), 0x16,
			base + kParabolicProcess1);
	}

	void Install()
	{
		static bool timed = false;
		if (!timed) {
			// Timed whether or not the skip is on, for A/B measurement.
			stl::write_vfunc<0x9, Accumulate>(RE::VTABLE_BSShadowParabolicLight[0]);
			stl::write_vfunc<0x9, SpotAccumulate>(RE::VTABLE_BSShadowFrustumLight[0]);
			timed = true;
		}
		if (installed)
			return;
		// These sites passed preflight, so a missing install is a failure. PostPostLoad fails at its end.
		if (!Extensions::SourceCullsInstalled(Api::ShadowSource::Sun)) {
			HookSites::NoteFailed("the point lights' shadow culls", "the sun's accumulation hooks are not installed");
			return;
		}
		// Preflight checked Process1's slot before any hook was written.
		const auto base = REL::Module::get().base();
		stl::write_vfunc<0x16, Process1>(RE::VTABLE_BSParabolicCullingProcess[0]);
		installed = true;
		// The filter walks the children itself, so it would skip another hook in this slot, such as Grass Optimizations'. It stays out then.
		{
			REL::Relocation<std::uintptr_t> nodeVtable{ RE::VTABLE_NiNode[0] };
			if (const auto held = reinterpret_cast<const std::uintptr_t*>(nodeVtable.address())[0x34]; held != base + kNodeOnVisible) {
				logger::warn("[DCLF] point lights' shadow culls: NiNode::OnVisible is hooked by {}; the category nodes are walked whole", HookSites::ModuleOf(held));
			} else {
				stl::write_vfunc<0x34, NodeOnVisible>(RE::VTABLE_NiNode[0]);
				filterInstalled = true;
			}
		}
		logger::info("[DCLF] point lights' shadow culls without DCLF's entries{}", filterInstalled ? ", their category nodes filtered" : "");
	}

	bool Installed() { return installed; }

	void SelectFrame(std::shared_ptr<SunExclusion> a_pending)
	{
		if (!installed)
			return;
		ResetFrameLights();
		++stats.frames;
		// The previous frame's culls are over, since they ran before its shadow epoch.
		std::shared_ptr<SunExclusion> next = std::move(a_pending);
		if (!next || !next->candidates)
			++stats.noExclusion, next.reset();
		else if (!PassCapture::Get().CastersWithheld(kSetCasterPoint))
			++stats.noClaims, next.reset();  // the registration withholds nothing of the mode: the engine draws its casters
		else if (const auto candidates = Scene::FrameView::Get().GetLightCandidates(); !candidates)
			++stats.stale, next.reset();  // no candidates this frame (a load, a reset)
		else if (next->candidates != candidates) {
			// Entries changed since the builder's snapshot fall back to the engine's cull. Their filter nodes are walked natively until rebuilt.
			std::vector<std::uint32_t> changed;
			auto translated = ForwardExclusion(next, candidates, &changed);
			if (!translated)
				++stats.stale;
			else if (filterInstalled && filterBuilt)
				for (const std::uint32_t e : changed)
					for (const auto* snapshot : { next->candidates.get(), candidates.get() }) {
						// The parent is only a key, since it may be detached meanwhile.
						if (!EngineReleases::Alive(snapshot->publication))
							continue;
						const auto* root = e < snapshot->Capacity() ? snapshot->entryNodes[e] : nullptr;
						if (const auto node = root ? filterBuilt->nodes.find(root->parent) : filterBuilt->nodes.end();
							node != filterBuilt->nodes.end() && !node->second->dirty.exchange(true, std::memory_order_acq_rel))
							dirtied.fetch_add(1, std::memory_order_release);
					}
			if (translated)
				++stats.masked;
			next = std::move(translated);
		}
		// Empty culls whenever the mode's casters are withheld, with or without an exclusion.
		frameCullsEmpty.store(PassCapture::Get().CastersWithheld(kSetCasterPoint), std::memory_order_relaxed);
		// The spot lights' need no exclusion (their casters are the plain and clamped set's): every frame those are withheld.
		frameSpotsEmpty.store(PassCapture::Get().CastersWithheld(kSetCaster), std::memory_order_relaxed);
		stats.live += next ? 1 : 0;
		frameExclusion.store(next.get(), std::memory_order_release);
		if (filterInstalled) {
			UpdateFilter(next);
			frameFilter.store(next ? filterCurrent.load(std::memory_order_relaxed) : nullptr, std::memory_order_release);
		}
		frameHeld = std::move(next);
	}

	void NoteStructure(const void* a_node)
	{
		const auto* a_parent = static_cast<const RE::NiNode*>(a_node);
		const auto* filter = filterInstalled && a_parent ? filterCurrent.load(std::memory_order_acquire) : nullptr;
		if (!filter)
			return;
		if (const auto it = filter->nodes.find(a_parent); it != filter->nodes.end() && !it->second->dirty.exchange(true, std::memory_order_acq_rel))
			dirtied.fetch_add(1, std::memory_order_release);
	}

	void NoteMainRegistration(RE::BSGeometry* a_geometry)
	{
		if (!installed || !a_geometry || !lightsReady.load(std::memory_order_acquire))
			return;
		const auto* exclusion = frameExclusion.load(std::memory_order_acquire);
		const std::uint32_t empty = emptyLights.load(std::memory_order_relaxed);
		if (!exclusion && !empty)
			return;
		const auto* property = a_geometry->GetGeometryRuntimeData().shaderProperty.get();
		void* lightData = property ? DCLF::Engine::At<void*>(property, DCLF::Engine::kPropertyLightData) : nullptr;
		if (!lightData)
			return;
		// Lights culled empty reached nothing. The first clearing main registration of a geometry takes their sphere bits instead.
		if (empty) {
			const auto found = mainSeen.Test(a_geometry, true);
			if (found == FrameAddressSet::Found::kFull)
				emptyStats.unstamped.fetch_add(1, std::memory_order_relaxed);
			if (found != FrameAddressSet::Found::kYes) {
				const std::uint32_t sphereBits = SphereBits(empty, *a_geometry);
				DCLF::Engine::At<std::uint32_t>(lightData, DCLF::Engine::kLightDataActiveMask) |= sphereBits;
				emptyStats.given.fetch_add(1, std::memory_order_relaxed);
				emptyStats.withBits.fetch_add(sphereBits ? 1 : 0, std::memory_order_relaxed);
			}
		}
		if (!exclusion)
			return;
		const auto& candidates = *exclusion->candidates;
		std::uint32_t entry = ~0u;
		if (const auto it = candidates.geometries.find(a_geometry); it != candidates.geometries.end() && exclusion->excluded[candidates.geometryEntry[it->second]]) {
			// Once a frame: the first main registration reads the mask and clears it, and the later ones read 0 (engine notes).
			const std::uint32_t stamp = lightStamp.load(std::memory_order_relaxed);
			if (exclusion->cleared[it->second].exchange(stamp, std::memory_order_relaxed) == stamp)
				return;
			entry = candidates.geometryEntry[it->second];
		}
		if (entry == ~0u)
			return;
		std::array<std::uint32_t, kReachKinds> reach{};
		const std::uint32_t bits = BitsOf(*exclusion, entry, *a_geometry, reach);
		DCLF::Engine::At<std::uint32_t>(lightData, DCLF::Engine::kLightDataActiveMask) |= bits;
		bitStats.given.fetch_add(1, std::memory_order_relaxed);
		bitStats.withBits.fetch_add(bits ? 1 : 0, std::memory_order_relaxed);
	}

	void EndFrame()
	{
		retired[1].clear();
		std::swap(retired[0], retired[1]);
	}

	Counters Take()
	{
		Counters c;
		c.accumulateCalls = accumulateCalls.exchange(0);
		c.accumulateTicks = accumulateTicks.exchange(0);
		c.installed = installed;
		if (!installed)
			return c;
		auto& s = stats;
		c.frames = s.frames, c.live = s.live, c.noExclusion = s.noExclusion, c.noClaims = s.noClaims, c.stale = s.stale, c.masked = s.masked;
		c.visited = s.visited.exchange(0), c.skipped = s.skipped.exchange(0);
		s.frames = s.live = s.noExclusion = s.noClaims = s.stale = s.masked = 0;
		c.filterInstalled = filterInstalled;
		if (filterInstalled) {
			auto& f = filterStats;
			c.nodes = f.nodes.exchange(0);
			c.lights = f.lights.exchange(0), c.filtered = f.filtered.load(), c.walkedNotLive = f.walkedNotLive.exchange(0);
			c.filterNodes = filterBuilt ? filterBuilt->nodes.size() : 0;
			c.children = f.children.exchange(0), c.cut = f.cut.exchange(0), c.dirtyNodes = f.dirtyNodes.exchange(0);
			c.builds = f.builds, c.nodesRebuilt = f.nodesRebuilt, c.buildTicks = f.buildTicks;
			f.filtered = 0;
			c.buildsStale = f.stale, c.buildsRefused = f.refused;
			f.builds = f.nodesRebuilt = f.stale = f.refused = 0;
			f.buildTicks = 0;
		}
		auto& b = bitStats;
		c.bitLights = b.lights.exchange(0), c.overflow = b.overflow.exchange(0), c.given = b.given.exchange(0), c.withBits = b.withBits.exchange(0);
		auto& e = emptyStats;
		c.emptyLights = e.lights.exchange(0), c.emptyNotCovered = e.notCovered.exchange(0), c.emptyPortalStrict = e.portalStrict.exchange(0);
		c.emptyGiven = e.given.exchange(0), c.emptyWithBits = e.withBits.exchange(0), c.emptyUnstamped = e.unstamped.exchange(0);
		c.emptyLate = e.late.exchange(0);
		c.emptySpots = e.spots.exchange(0), c.spotsNotCovered = e.spotsNotCovered.exchange(0), c.spotsStrict = e.spotsStrict.exchange(0);
		c.spotCalls = spotCalls.exchange(0), c.spotTicks = spotTicks.exchange(0);
		c.strictLights = e.strictLights.exchange(0), c.strictTested = e.strictTested.exchange(0), c.strictRefused = e.strictRefused.exchange(0);
		c.strictNoGraph = e.strictNoGraph.exchange(0), c.strictNoContext = e.strictNoContext.exchange(0);
		return c;
	}
}
