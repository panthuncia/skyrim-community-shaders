// FrameGlobals.h's capture at the frame's start, the first-person subtree's cull bits, and the scoped reads.

#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Shading/GeometryPort.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Snapshot/FrameCamera.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"
#include "Snapshot/EngineReads.h"

#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/EngineStates.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/RootViewEvents.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "RenderGraph/FrameAnchors.h"
#include "Utils/Game.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include <ankerl/unordered_dense.h>
#include <atomic>

namespace DCLF
{
	namespace
	{
		using Engine::Global;

		// Address Library ids. The readers' notes name each.
		constexpr Engine::Offset kTechniqueByte12{ { 390937 }, { 513196 } };
		constexpr Engine::Offset kTechniqueByte7{ { 391632 }, { 513692 } };
		constexpr Engine::Offset kShadowGlobal{ { 391108 }, { 513334 } };
		constexpr std::size_t kAccumulatorDeferredShadow = 0x178;
		constexpr Engine::Offset kLodRadiusBase{ { 390882 }, { 513158 } };
		constexpr Engine::Offset kLodExponentScale{ { 390887 }, { 513163 } };
		constexpr Engine::Offset kLodPowBase{ { 390883 }, { 513159 } };

		// A setting's value. The setting is looked up once, since GetINISetting walks the collection by name, and read every frame.
		struct CachedSetting
		{
			const char* name;
			float fallback;
			RE::Setting* setting = nullptr;
			bool looked = false;
			float Get()
			{
				if (!std::exchange(looked, true))
					setting = RE::GetINISetting(name);
				return setting ? setting->GetFloat() : fallback;
			}
		};
		CachedSetting iniSpecularStart{ "fSpecularLODFadeStart:LightingShader", 0.09f };
		CachedSetting iniSpecularEnd{ "fSpecularLODFadeEnd:LightingShader", 0.10f };
		CachedSetting iniEnvmapStart{ "fEnvmapLODFadeStart:LightingShader", 0.09f };
		CachedSetting iniEnvmapEnd{ "fEnvmapLODFadeEnd:LightingShader", 0.10f };
		// GetRenderPasses' refraction LOD fade settings, AE 0x142033408 and 0x142033420.
		CachedSetting iniRefractionStart{ "fRefractionLODFadeStart:LightingShader", 0.025f };
		CachedSetting iniRefractionEnd{ "fRefractionLODFadeEnd:LightingShader", 0.03f };

		bool Hidden(const RE::NiAVObject* a_object) { return Engine::IsHidden(a_object); }

		// The capture's listed hidden bits. The nodes are almost always last frame's in the same order, so the last sort order is tried first.
		using HiddenList = std::vector<std::pair<const RE::NiAVObject*, bool>>;
		std::vector<const RE::NiAVObject*> sortedNodes;  // the last sort's input, in collection order
		std::vector<std::uint32_t> sortOrder;            // positions in it, sorted
		HiddenList sortScratch;

		/** @brief Sorts a_list as std::sort would, reusing the last order when it lists the same nodes in the same order. */
		void SortCullHidden(HiddenList& a_list)
		{
			const std::size_t n = a_list.size();
			bool same = n == sortedNodes.size();
			for (std::size_t i = 0; same && i < n; ++i)
				same = a_list[i].first == sortedNodes[i];
			sortScratch.resize(n);
			if (same) {
				for (std::size_t i = 0; i < n; ++i)
					sortScratch[i] = a_list[sortOrder[i]];
				// A node listed twice is ordered by its bits, so check them and sort afresh if they changed.
				if (std::is_sorted(sortScratch.begin(), sortScratch.end())) {
					a_list.swap(sortScratch);
					return;
				}
			}
			sortedNodes.resize(n);
			sortOrder.resize(n);
			for (std::size_t i = 0; i < n; ++i) {
				sortedNodes[i] = a_list[i].first;
				sortOrder[i] = static_cast<std::uint32_t>(i);
			}
			std::sort(sortOrder.begin(), sortOrder.end(), [&](std::uint32_t a_a, std::uint32_t a_b) { return a_list[a_a] < a_list[a_b]; });
			for (std::size_t i = 0; i < n; ++i)
				sortScratch[i] = a_list[sortOrder[i]];
			a_list.swap(sortScratch);
		}

		using SubtreeNode = RootSubtreeNode;

		/**
		 * @brief The first-person subtree's bits, kept up to date by its events and applied at the frame's start and the first-person culls.
		 * The live tree is walked only when membership changed. A detached key leaves at once, so a freed and reused address is a new node.
		 */
		struct Subtree
		{
			const RE::NiAVObject* root = nullptr;  // compared, and dereferenced only on the render thread inside the frame
			std::vector<SubtreeNode> nodes;        // an entry's index is stable until the next walk
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> index;
			std::vector<const RE::NiAVObject*> geometry;  // sorted
			std::vector<std::uint32_t> saved;             // entries whose culled bit was saved since the last culls
			bool edited = false;                          // an attach or detached key since the last walk
			bool culled = false;                          // the culls were seen for this root
			std::uint64_t changes = 0;                    // bits or membership changed
			std::uint64_t walks = 0;
		} subtree;

		bool CulledBit(const SubtreeNode& a_node) { return subtree.culled && a_node.saved ? a_node.culled : a_node.hidden; }

		/** @brief Every node and geometry under a_root as it is now. Render thread, inside the frame. */
		void LiveSubtree(const RE::NiAVObject* a_root, std::vector<SubtreeNode>& a_out)
		{
			a_out.clear();
			if (!const_cast<RE::NiAVObject*>(a_root)->AsNode())
				return;
			Engine::ForEachLive(a_root, [&](RE::NiAVObject& a_object, std::uint32_t a_depth) {
				if (a_depth)
					a_out.push_back({ &a_object, a_object.AsGeometry() != nullptr, Hidden(&a_object) });
			});
			std::ranges::sort(a_out, {}, &SubtreeNode::key);
		}

		/** @brief Walks the membership again. Indexed keys keep their bits and new ones take the live bit. */
		void WalkSubtree()
		{
			std::vector<SubtreeNode> next;
			LiveSubtree(subtree.root, next);
			std::vector<std::uint32_t> added;  // new keys. Their stores raise no event until the members are published.
			for (std::uint32_t i = 0; i < next.size(); ++i) {
				auto& node = next[i];
				if (const auto it = subtree.index.find(node.key); it != subtree.index.end()) {
					const auto& kept = subtree.nodes[it->second];
					node.hidden = kept.hidden;
					node.culled = kept.culled;
					node.saved = kept.saved;
				} else
					added.push_back(i);
			}
			subtree.nodes = std::move(next);
			subtree.index.clear();
			subtree.geometry.clear();
			subtree.saved.clear();
			for (std::uint32_t i = 0; i < subtree.nodes.size(); ++i) {
				const auto& node = subtree.nodes[i];
				subtree.index.emplace(node.key, i);
				if (node.geometry)
					subtree.geometry.push_back(node.key);
				if (node.saved)
					subtree.saved.push_back(i);
			}
			subtree.edited = false;
			++subtree.changes;
			++subtree.walks;
			// The members the hidden stores look up, since the hooks' threads do not read their objects' parents.
			std::vector<const void*> members;
			members.reserve(subtree.nodes.size());
			for (const auto& node : subtree.nodes)
				members.push_back(node.key);
			SceneTracker::WatchSubtreeMembers(std::move(members));
			// Read a new key's bit again after publishing it. A store before that is in this read, one after it raises an event.
			for (const std::uint32_t i : added)
				subtree.nodes[i].hidden = Hidden(subtree.nodes[i].key);
		}

		/**
		 * @brief Render thread, at an engine moment. Brings the subtree under a_root up to now, walking again if membership changed.
		 * A new root, or none, starts over.
		 */
		void SyncSubtree(const RE::NiAVObject* a_root)
		{
			auto& events = SceneTracker::SubtreeEvents();
			if (a_root != subtree.root) {
				// Drop the old root's events, then watch the new root before walking it. A store after the watch raises an event, one before is in the walk.
				events.Discard();
				SceneTracker::WatchSubtree(a_root);
				SceneTracker::WatchSubtreeMembers({});
				subtree = {};
				subtree.root = a_root;
				if (a_root)
					WalkSubtree();
				return;
			}
			if (!a_root)
				return;
			events.Drain([](SceneTracker::SubtreeEvent&& a_event) {
				using Kind = SceneTracker::SubtreeEvent::Kind;
				if (a_event.kind == Kind::Attached) {
					subtree.edited = true;
					return;
				}
				const auto it = subtree.index.find(a_event.node);
				if (it == subtree.index.end())
					return;  // not walked yet, so the next walk reads its bit
				auto& node = subtree.nodes[it->second];
				if (a_event.kind == Kind::Removed) {
					node.key = nullptr;
					subtree.index.erase(it);
					subtree.edited = true;
					return;
				}
				const bool hidden = a_event.kind == Kind::Hidden;
				if (node.hidden == hidden)
					return;
				if (subtree.culled && !node.saved) {
					node.culled = node.hidden;
					node.saved = true;
					subtree.saved.push_back(it->second);
				}
				node.hidden = hidden;
				++subtree.changes;
			});
			if (subtree.edited)
				WalkSubtree();
		}

		/** @brief The subtree at an engine moment, for the root view's diagnostics. */
		void NoteSubtreeSynced(const char* a_moment, bool a_frameStart)
		{
			Diagnostics::Notify(Diagnostics::RootSubtreeSynced{ a_moment, a_frameStart, subtree.root, subtree.nodes, subtree.culled, subtree.walks, subtree.changes });
		}

		// The render thread's latest capture and a few before it. An unscoped reader, which is a defect, may hold one past its replacement.
		std::shared_ptr<const FrameGlobals> latest = std::make_shared<FrameGlobals>();
		std::array<std::shared_ptr<const FrameGlobals>, 8> recent;
		std::uint32_t recentNext = 0;
		// A read off the render owner with no capture bound, counted by the live-read detector.
		constinit Snapshot::EngineReads::Site unscopedRead{ Snapshot::EngineReads::Object::FrameGlobals, Snapshot::EngineReads::Category::RecordTime, std::source_location::current() };
		std::uint64_t unscopedTaken = 0;
		thread_local const FrameGlobals* scoped = nullptr;
		// The driver's game half's capture (BeginGameHalf), on the game thread.
		thread_local std::shared_ptr<const FrameGlobals> gameHalf;

		/** @brief The first unscoped read's thread and callers, as module+offset, for the log line. */
		std::string UnscopedReader()
		{
			std::array<void*, 8> frames{};
			const auto count = ::RtlCaptureStackBackTrace(2, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
			std::string text = fmt::format("thread {}", ::GetCurrentThreadId());
			for (std::uint16_t i = 0; i < count; ++i) {
				HMODULE module = nullptr;
				char path[MAX_PATH]{};
				if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(frames[i]), &module) ||
					!module || !::GetModuleFileNameA(module, path, MAX_PATH)) {
					text += fmt::format(", {}", frames[i]);
					continue;
				}
				const char* slash = std::strrchr(path, '\\');
				text += fmt::format(", {}+{:#x}", slash ? slash + 1 : path, reinterpret_cast<std::uintptr_t>(frames[i]) - reinterpret_cast<std::uintptr_t>(module));
			}
			return text;
		}
	}

	void FrameGlobals::NoteFirstPersonCulled(const RE::NiAVObject* a_root)
	{
		// At RenderDepth's first-person draw. Apply events since the frame's start. The bits now are the culls'.
		// From here a bit is saved only for nodes an event changes before the next culls.
		SyncSubtree(a_root);
		if (!subtree.saved.empty() || subtree.culled != (a_root != nullptr))
			++subtree.changes;
		for (const std::uint32_t i : subtree.saved)
			subtree.nodes[i].saved = false;
		subtree.saved.clear();
		subtree.culled = a_root != nullptr;
		if (a_root)
			NoteSubtreeSynced("at the culls", false);
	}

	std::shared_ptr<const FrameGlobals> FrameGlobals::Capture()
	{
		DCLF_ZONE("CS.DCLF.FrameGlobals.Capture");
		const Snapshot::EngineReads::CaptureScope captureScope;
		auto g = std::make_shared<FrameGlobals>();
		g->loading = Scene::FrameView::IsLoadingScreenUp();
		g->interior = Util::IsInterior();
		g->decalBias = { 0u, DecalDepthBiasMode(1), DecalDepthBiasMode(2), DecalDepthBiasMode(3) };
		g->specularStart = iniSpecularStart.Get();
		g->specularEnd = iniSpecularEnd.Get();
		g->envmapStart = iniEnvmapStart.Get();
		g->envmapEnd = iniEnvmapEnd.Get();
		g->refractionStart = iniRefractionStart.Get();
		g->refractionEnd = iniRefractionEnd.Get();
		g->techniqueByte12 = Global<std::uint8_t>(kTechniqueByte12);
		g->techniqueByte7 = Global<std::uint8_t>(kTechniqueByte7);
		g->shadowGlobal = Global<std::uint8_t>(kShadowGlobal);
		if (const auto* accumulator = Global<std::uint8_t*>(Engine::Address::kMainAccumulator)) {
			g->accumulator = true;
			g->accumulatorDeferredShadow = accumulator[kAccumulatorDeferredShadow];
		}
		g->noSunShadowDir = Global<std::uint8_t>(Engine::Address::kNoSunShadowDir);
		g->screenDoorFades = Global<std::uint8_t>(Engine::Address::kScreenDoorFades);
		g->fadesOn = Global<std::uint8_t>(Engine::Address::kFadesOn);
		g->fadeLodUpdates = Global<std::uint8_t>(Engine::Address::kFadeLodUpdates);
		g->fadeSpecialA = Global<float>(Engine::Address::kFadeSpecialA);
		g->fadeSpecialB = Global<float>(Engine::Address::kFadeSpecialB);
		g->fadeDistanceMult = Global<float>(Engine::Address::kFadeDistanceMult);
		g->fadeOutThreshold = Global<float>(Engine::Address::kFadeOutThreshold);
		g->fadeDefaultScale = Global<float>(Engine::Address::kFadeDefaultScale);
		g->metricScale = Global<float>(Engine::Address::kMetricScale);
		for (std::uint32_t type = 0; type < g->fadeTypeDivisors.size(); ++type)
			g->fadeTypeDivisors[type] = Global<float>(Engine::Address::kFadeTypeDivisors + type * 4);
		g->lodRadiusBase = Global<float>(kLodRadiusBase);
		g->lodExponentScale = Global<float>(kLodExponentScale);
		g->lodPowBase = Global<float>(kLodPowBase);
		if (const auto* camera = CS_ENGINE_READ(Camera, CapturePoint, RE::Main::WorldRootCamera())) {
			g->eye = { camera->world.translate.x, camera->world.translate.y, camera->world.translate.z };
			g->lodAdjust = camera->GetRuntimeData2().lodAdjust;
		}
		CaptureExtrasInputs(g->landBlend, g->projectedGlobals);
		if (::Snapshot::FrameCamera::Owned())
			g->pipeline = std::make_shared<const GeometryPort::PipelineFrame>(GeometryPort::SamplePipelineFrame(true));
		Extensions::CaptureFrameGlobals(g->features);
		// Roots whose hidden bit the frame changes around the scene's views, as the views see it, at the frame's start.
		if (!g->loading) {
			// Numbered before the reads with a locked add, so a store whose event is numbered earlier is in them.
			g->cullHiddenSequence = SceneCapture::NextSequence();
			g->cullHidden.reserve(sortedNodes.size() + subtree.nodes.size());
			for (auto* sceneNode : CS_ENGINE_READ(ShaderManagerState, CapturePoint, RE::BSShaderManager::State::GetSingleton()).shadowSceneNode) {
				const auto* graph = sceneNode ? sceneNode->GetRuntimeData().portalGraph : nullptr;
				if (!graph)
					continue;
				for (const auto& child : graph->alwaysRenderChildren)
					if (child)
						g->cullHidden.emplace_back(child.get(), Hidden(child.get()));
				if (graph->portalSharedNode)
					g->cullHidden.emplace_back(graph->portalSharedNode.get(), Hidden(graph->portalSharedNode.get()));
			}
			// The features' nodes whose bit the engine changes around the views, such as Reflection's player 3D and FirstPerson's skeleton.
			Extensions::CollectCullHidden(g->cullHidden);
			// The first CategoryRoot view's root while its toggle is on. The subtree machinery holds a single root.
			const RE::NiAVObject* subtreeRoot = nullptr;
			const auto& views = Extensions::Views();
			for (std::uint32_t i = 0; i < views.rootCount && !subtreeRoot; ++i) {
				const auto view = views.rootViews[i];
				if (!Extensions::ViewRegistered(view))
					continue;
				Api::ViewRootCapture root;
				Extensions::ViewEntryOf(view).captureRoot(root);
				subtreeRoot = root.root;
			}
			SortCullHidden(g->cullHidden);
			// Every node under the first-person root, with the bit the engine's first-person culls saw last frame, or as it is now.
			// The frame start's bits are not the culls': the arms are hidden at the frame's start while the engine draws them.
			// So the first-person members' chains read their bits from here.
			SyncSubtree(subtreeRoot);
			if (subtreeRoot) {
				const auto listed = static_cast<std::ptrdiff_t>(g->cullHidden.size());
				g->cullHidden.reserve(g->cullHidden.size() + subtree.nodes.size());
				for (const auto& node : subtree.nodes)
					if (node.key)
						g->cullHidden.emplace_back(node.key, CulledBit(node));
				std::inplace_merge(g->cullHidden.begin(), g->cullHidden.begin() + listed, g->cullHidden.end());
				g->firstPersonGeometry = subtree.geometry;
				// The root view's diagnostics, with the membership just synced.
				NoteSubtreeSynced("at the frame's start", true);
			}
		}
		g->membershipWitness = PrimaryCull::MembershipWitness(*g);
		g->technique = SampleTechniqueInputs();
		g->shadowMaskHeld.copy_from(g->technique.shadowMask);  // the renderer's target holds it now, so it is alive
		g->material = MaterialPort::SampleFrame();
		return g;
	}

	void FrameGlobals::Adopt(std::shared_ptr<const FrameGlobals> a_globals)
	{
		if (!a_globals || a_globals == latest)
			return;
		recent[recentNext++ % recent.size()] = std::move(latest);
		latest = std::move(a_globals);
	}

	void FrameGlobals::BeginGameHalf(std::shared_ptr<const FrameGlobals> a_globals)
	{
		gameHalf = std::move(a_globals);
	}

	void FrameGlobals::EndGameHalf()
	{
		gameHalf.reset();
	}

	const FrameGlobals& FrameGlobals::Current()
	{
		if (scoped)
			return *scoped;
		if (gameHalf)
			return *gameHalf;
		// The thread that owns rendering reads the latest adopted capture: the one that adopted it, or a loading screen's serving thread
		// (its menu scene) or the main menu's frames, which take ownership inside Renderer::Lock after it.
		if (::FrameAnchors::IsRenderOwner())
			return *latest;
		unscopedRead.Count();
		if (unscopedRead.Live() == 1)
			logger::error(
				"[DCLF] the frame's engine globals were read off the render owner with no frame's capture bound (step 6e F2); it got the "
				"latest adopted. Reader: {}",
				UnscopedReader());
		return *latest;
	}

	std::uint64_t FrameGlobals::TakeUnscopedReads()
	{
		// Reporting thread only, so the last total needs no atomic.
		const auto total = unscopedRead.Live();
		return total - std::exchange(unscopedTaken, total);
	}

	FrameGlobals::Scope::Scope(std::shared_ptr<const FrameGlobals> a_globals) :
		held(std::move(a_globals)),
		previous(scoped)
	{
		scoped = held.get();
	}

	FrameGlobals::Scope::~Scope()
	{
		scoped = previous;
	}
}
