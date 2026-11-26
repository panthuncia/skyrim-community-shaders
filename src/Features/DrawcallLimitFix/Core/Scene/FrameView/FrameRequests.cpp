// FrameView's service of the coordinator's requests and its engine captures: switch catch-ups, fade node and material requests,
// the category capture and the mirror's capture requests. Render thread, since everything here reads or writes live engine state.

#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "RenderGraph/Fatal.h"
#include "Utils/Hash.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Extensions/AncestorValues.h"
#include "Snapshot/EngineReads.h"

namespace DCLF::Scene
{
	namespace
	{
		// The Actor node holds an exterior's actors. It is walked whatever the actors' toggle says, since ClassifyFrame decides that live.
		constexpr std::array<std::uint32_t, 4> kDrawnCategories{ 0 /*Actor*/, 3 /*Static*/, 4 /*Dynamic*/, 5 /*MultiBound*/ };

		constexpr std::uint32_t kObjectClasses = static_cast<std::uint32_t>(Api::ObjectClass::Count);

		/** @brief One bit per object class taken that lists a category root, by categoryRoot or categoryRoots. */
		std::uint32_t RootListing()
		{
			const auto& roots = Extensions::RegisteredCategoryRoots();
			const auto& gathers = Extensions::RegisteredCategoryRootGathers();
			std::uint32_t listing = 0;
			for (std::uint32_t c = 0; c < kObjectClasses; ++c)
				if ((roots[c] || gathers[c]) && Extensions::ObjectClassOn(static_cast<Api::ObjectClass>(c)))
					listing |= 1u << c;
			return listing;
		}

		template <class AddFn>
		class CategoryRootVisitor final : public Api::CategoryRootSink
		{
		public:
			explicit CategoryRootVisitor(AddFn& a_add) :
				add(a_add) {}
			void Add(RE::NiNode* a_root) override
			{
				if (a_root)
					add(a_root);
			}

		private:
			AddFn& add;
		};

		/** @brief Every root the classes in a_listing gather through categoryRoots. Render thread. */
		template <class AddFn>
		void ForEachGatheredRoot(std::uint32_t a_listing, AddFn&& a_add)
		{
			const auto& gathers = Extensions::RegisteredCategoryRootGathers();
			for (std::uint32_t c = 0; c < kObjectClasses; ++c)
				if ((a_listing & (1u << c)) && gathers[c]) {
					CategoryRootVisitor<std::remove_reference_t<AddFn>> visitor(a_add);
					gathers[c](visitor);
				}
		}

		template <class AddFn>
		class UnheldRootVisitor final : public Api::UnheldRootSink
		{
		public:
			explicit UnheldRootVisitor(AddFn& a_add) :
				add(a_add) {}
			void Add(RE::NiNode* a_root) override
			{
				if (a_root)
					add(a_root);
			}

		private:
			AddFn& add;
		};

		/** @brief The caller must be inside UnheldRootsScope. */
		template <class AddFn>
		void ForEachUnheldRoot(AddFn&& a_add)
		{
			const auto& unheld = Extensions::RegisteredUnheldRoots();
			if (!unheld.gather)
				return;
			UnheldRootVisitor<std::remove_reference_t<AddFn>> visitor(a_add);
			unheld.gather(visitor);
		}

		/** @brief Keeps the unheld roots' owner from removing roots between their gather and their mirror pushes. */
		class UnheldRootsScope
		{
		public:
			UnheldRootsScope() = default;
			~UnheldRootsScope()
			{
				if (entered)
					unheld.leave();
			}
			UnheldRootsScope(const UnheldRootsScope&) = delete;
			UnheldRootsScope& operator=(const UnheldRootsScope&) = delete;

			void Enter()
			{
				if (unheld.gather && !entered) {
					unheld.enter();
					entered = true;
				}
			}

		private:
			const Api::UnheldRootsContribution& unheld = Extensions::RegisteredUnheldRoots();
			bool entered = false;
		};

		/**
		 * @brief The frame start's engine reads that push nothing onto the tracker's stack, run on DCLF's pool inside the engine-read window.
		 *
		 * The mirror's own captures stay on the render thread. Other threads attach and detach inside the window, and a capture pushed
		 * behind such an edit's event would be applied over it. Each item takes its own lease, so Close never waits on more than one item.
		 */
		struct FrameStartJob
		{
			// The render thread already holds it. The task writes only its children and pins, then posts it.
			std::shared_ptr<CategoryCapture> capture;
			std::vector<RE::NiNode*> pinNodes;        // category nodes new to the coordinator's set: children's keys and subtrees
			std::vector<RE::NiAVObject*> pinRoots;    // roots new to it: subtrees
			std::vector<FadeNodeRequest> fadeRequests;

			std::size_t next = 0;
			std::atomic<bool> running{ false };

			std::size_t CategoryItems() const { return capture ? pinNodes.size() + pinRoots.size() + 1 : 0; }
			std::size_t Items() const { return CategoryItems() + fadeRequests.size(); }
			bool Empty() const { return !Items(); }
		};
		std::unique_ptr<FrameStartJob> frameStartJob;  // render thread: the job in flight or being made, null when none

		void RunFrameStartItem(FrameStartJob& a_job, std::size_t a_item)
		{
			auto& channels = Channels::Get();
			if (a_job.capture) {
				auto& capture = *a_job.capture;
				auto pinSubtree = [&](RE::NiAVObject* a_root) {
					Engine::ForEachLive(a_root, [&](RE::NiAVObject& a_object, std::uint32_t) {
						capture.pins.emplace_back(&a_object);
						if (auto* geometry = a_object.AsGeometry()) {
							const auto& data = geometry->GetGeometryRuntimeData();
							for (RE::NiRefObject* property : { static_cast<RE::NiRefObject*>(data.shaderProperty.get()), static_cast<RE::NiRefObject*>(LayerPropertyOf(*geometry)),
									 static_cast<RE::NiRefObject*>(data.alphaProperty.get()) })
								if (property)
									capture.pins.emplace_back(property);
						}
					});
				};
				if (a_item < a_job.pinNodes.size()) {
					auto* node = a_job.pinNodes[a_item];
					auto& keys = capture.children[node];
					for (auto& child : Engine::Children(*node))
						if (child) {
							keys.push_back(child.get());
							pinSubtree(child.get());
						}
					return;
				}
				a_item -= a_job.pinNodes.size();
				if (a_item < a_job.pinRoots.size()) {
					pinSubtree(a_job.pinRoots[a_item]);
					return;
				}
				a_item -= a_job.pinRoots.size();
				if (a_item == 0) {
					// Posted latest-wins after the render thread's pushes for it, so the pass that takes it drains them too. A newer capture loses nothing.
					channels.categoryPosted.Post(std::make_unique<std::shared_ptr<const CategoryCapture>>(a_job.capture));
					WakeScenePass(SceneWake::Capture);
					return;
				}
				a_item -= 1;
			}
			if (a_item < a_job.fadeRequests.size()) {
				// An in-world geometry whose property still names the node draws with it, so the node is alive. Otherwise the answer is none.
				auto& request = a_job.fadeRequests[a_item];
				FadeNodeAnswer answer;
				answer.geometry = request.geometry.get();
				answer.fadeNode = request.fadeNode;
				if (auto* geometry = request.geometry.get(); geometry && SceneCapture::InWorld(geometry))
					if (auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get(); property && property->fadeNode && property->fadeNode == request.fadeNode)
						answer.node.reset(property->fadeNode);
				channels.fadeNodeAnswers.Push(std::move(answer));
				EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(request.geometry)));
				if (a_item + 1 == a_job.fadeRequests.size())
					WakeScenePass(SceneWake::Capture);
			}
		}

		/** @brief Runs items until a lease is refused. The rest wait for the next window. */
		void RunFrameStartJob(FrameStartJob& a_job)
		{
			DCLF_ZONE("CS.DCLF.Ingest.FrameStartJob");
			for (const std::size_t items = a_job.Items(); a_job.next < items; ++a_job.next) {
				const EngineReadWindow::Lease lease;
				if (!lease)
					break;
				RunFrameStartItem(a_job, a_job.next);
			}
			a_job.running.store(false, std::memory_order_release);
		}

		void DispatchFrameStartJob(FrameStartJob& a_job)
		{
			a_job.running.store(true, std::memory_order_release);
			if (!::RenderGraph::Frame::SharedExecutor().Dispatch(SceneScheduler::Scope(), ::RenderGraph::Frame::Executor::Preparation, org::async::TaskDispatch::Cpu, "frame-start captures",
					[job = &a_job](const auto&) { RunFrameStartJob(*job); }))
				::RenderGraph::Fatal("Drawcall Limit Fix", "the frame start's captures were refused by DCLF's executor");
		}

		template <class Mix>
		void MixViewRootInputs(Mix&& a_mix)
		{
			const auto& views = Extensions::Views();
			for (std::uint32_t i = 0; i < views.rootCount; ++i) {
				const auto view = views.rootViews[i];
				if (!Extensions::ViewRegistered(view))
					continue;
				Api::ViewRootCapture capture;
				Extensions::ViewEntryOf(view).captureRoot(capture);
				for (std::uint32_t n = 0; n < capture.inputCount; ++n)
					a_mix(capture.inputs[n]);
			}
		}

		/** @brief The signature inputs that cost O(1) to read, compared every frame. The rest is read only after a watched edit or a change here. */
		std::uint64_t CategoryInputs()
		{
			Util::Fnv1a hash;
			const auto& views = Extensions::Views();
			std::uint32_t listing = RootListing();
			for (std::uint32_t i = 0; i < views.rootCount; ++i)
				listing |= Extensions::ViewRegistered(views.rootViews[i]) ? 1u << (kObjectClasses + i) : 0u;
			hash.Mix(listing);
			for (const auto root : Extensions::RegisteredCategoryRoots())
				if (root)
					hash.Mix(root());
			ForEachGatheredRoot(listing, [&hash](RE::NiNode* a_root) { hash.Mix(a_root); });
			if (const auto& unheld = Extensions::RegisteredUnheldRoots(); unheld.owner)
				hash.Mix(unheld.owner());
			if (auto* tes = CS_ENGINE_READ(TES, RecordTime, RE::TES::GetSingleton())) {
				hash.Mix(tes->objRoot);
				hash.Mix(tes->interiorCell);
				const auto* grid = tes->gridCells;
				hash.Mix(grid);
				if (grid) {
					hash.Mix((static_cast<std::uint64_t>(grid->unk08) << 32) | grid->unk0C);
					hash.Mix(grid->length);
					hash.Mix(grid->cells);
				}
			}
			MixViewRootInputs([&hash](const void* a_input) { hash.Mix(a_input); });
			return hash.value;
		}

		/**
		 * @brief A cheap check that the category nodes may have changed. A missed change means geometry under a new node is never tracked,
		 * so it folds in everything CaptureCategories reads to find nodes.
		 */
		std::uint64_t ReadCategorySignature(std::vector<const void*>* a_watch)
		{
			Util::Fnv1a hash;
			auto mix = [&hash](const void* a_value) { hash.Mix(a_value); };
			auto watch = [a_watch](const void* a_node) {
				if (a_watch && a_node)
					a_watch->push_back(a_node);
			};
			auto mixCell = [&](RE::TESObjectCELL* a_cell) {
				mix(a_cell);
				if (!a_cell || !a_cell->IsAttached())
					return;
				auto* loaded = a_cell->GetRuntimeData().loadedData;
				mix(loaded);
				RE::NiNode* cell3D = loaded ? loaded->cell3D.get() : nullptr;
				mix(cell3D);
				if (cell3D) {
					hash.Mix(cell3D->GetChildren().size());
					watch(cell3D);
					watch(cell3D->parent);
				}
				if (const auto* graph = loaded ? loaded->portalGraph.get() : nullptr) {
					mix(graph->portalSharedNode.get());
					watch(graph->portalSharedNode.get());
					hash.Mix(graph->alwaysRenderChildren.size());
					for (const auto& child : graph->alwaysRenderChildren) {
						mix(child.get());
						mix(child ? child->parent : nullptr);
						watch(child.get());
					}
				}
			};
			if (auto* tes = CS_ENGINE_READ(TES, RecordTime, RE::TES::GetSingleton())) {
				if (auto* objRoot = tes->objRoot)
					hash.Mix(objRoot->GetChildren().size());
				mix(tes->objRoot);
				watch(tes->objRoot);
				const std::uint32_t rootListing = RootListing();
				const auto& roots = Extensions::RegisteredCategoryRoots();
				for (std::uint32_t c = 0; c < kObjectClasses; ++c)
					if (roots[c])
						mix(rootListing & (1u << c) ? roots[c]() : nullptr);
				ForEachGatheredRoot(rootListing, [&mix](RE::NiNode* a_root) { mix(a_root); });
				hash.Mix(rootListing);
				MixViewRootInputs(mix);
				mix(tes->interiorCell);
				Engine::ForEachLoadedCell(*tes, mixCell);
			}
			if (const auto& unheld = Extensions::RegisteredUnheldRoots(); unheld.owner)
				mix(unheld.owner());
			// Watched whether or not a cell is loaded yet.
			for (auto* sceneNode : CS_ENGINE_READ(ShaderManagerState, RecordTime, RE::BSShaderManager::State::GetSingleton()).shadowSceneNode)
				watch(sceneNode);
			return hash.value;
		}

		struct CategoryWatch
		{
			bool read = false;
			std::uint64_t edits = 0, inputs = 0, signature = 0;
			std::uint64_t unheldEdits = 0;  // SceneTracker::UnheldEdits at the last gather of the unheld roots
		} categoryWatch;

		/** @brief Read again on a forced capture, a watched edit or a moved input. */
		std::uint64_t RefreshCategorySignature(bool a_force)
		{
			// Take the edits first. An edit made during the read moves them again, and the next frame reads again.
			auto& watch = categoryWatch;
			const std::uint64_t edits = SceneTracker::CategoryEdits();
			const std::uint64_t inputs = CategoryInputs();
			if (a_force || !watch.read || edits != watch.edits || inputs != watch.inputs) {
				std::vector<const void*> watched;
				watch.signature = ReadCategorySignature(&watched);
				SceneTracker::WatchCategoryNodes(std::move(watched));
				const auto& unheld = Extensions::RegisteredUnheldRoots();
				SceneTracker::WatchUnheldOwner(unheld.owner ? unheld.owner() : nullptr);
				watch.read = true;
				watch.edits = edits;
				watch.inputs = inputs;
			}
			return watch.signature;
		}

		/**
		 * @brief The particle manager's master particle systems. The engine frees a master whose count drops below 3, so the capture
		 * keeps no reference past its first application. Gathered before any other list, so a root another list names stays unheld.
		 */
		void GatherUnheldRoots(CategoryCapture& a_capture)
		{
			ForEachUnheldRoot([&](RE::NiNode* a_root) {
				a_capture.nodes.insert(a_root);
				a_capture.roots.try_emplace(a_root, CategoryCapture::Root{ {}, a_root, a_root });
				a_capture.unheld.insert(a_root);
				a_capture.pins.emplace_back(a_root);
			});
		}

		/** @brief Must run inside UnheldRootsScope. */
		void GatherCategoryNodes(CategoryCapture& a_capture)
		{
			auto& current = a_capture.nodes;
			auto& currentRoots = a_capture.roots;

			GatherUnheldRoots(a_capture);

			auto addCell = [&](RE::TESObjectCELL* a_cell) {
				if (!a_cell || !a_cell->IsAttached())
					return;
				auto* loaded = a_cell->GetRuntimeData().loadedData;
				if (!loaded || !loaded->cell3D)
					return;
				auto& children = loaded->cell3D->GetChildren();
				for (auto category : kDrawnCategories) {
					const auto index = static_cast<std::uint16_t>(category);
					if (index < children.free_idx() && children[index]) {
						if (auto* node = children[index]->AsNode())
							current.insert(node);
					}
				}
				// The engine moves references out of the category nodes into multibounds in exteriors and portal graph rooms in interiors.
				if (auto* multiBound = loaded->multiBoundNode.get())
					current.insert(multiBound);
				if (auto* graph = loaded->portalGraph.get()) {
					for (auto& room : graph->rooms) {
						if (room)
							current.insert(room.get());
					}
					if (auto* shared = graph->portalSharedNode.get()) {
						current.insert(shared);
						// Parentless references the graph draws, such as Bleak Falls Barrow's chamber pieces. Held, so a root that leaves the list can still be walked.
						for (const auto& child : graph->alwaysRenderChildren) {
							if (child && !child->parent)
								currentRoots.try_emplace(child.get(), CategoryCapture::Root{ child, shared, child.get() });
						}
					}
				}
			};

			if (auto* tes = CS_ENGINE_READ(TES, RecordTime, RE::TES::GetSingleton())) {
				if (auto* objRoot = tes->objRoot) {
					for (auto& child : Engine::Children(*objRoot)) {
						if (auto* multiBound = child ? netimmerse_cast<RE::BSMultiBoundNode*>(child.get()) : nullptr)
							current.insert(multiBound);
					}
				}
				// A root two classes list is one node.
				if (const std::uint32_t rootListing = RootListing()) {
					const auto& roots = Extensions::RegisteredCategoryRoots();
					for (std::uint32_t c = 0; c < kObjectClasses; ++c)
						if ((rootListing & (1u << c)) && roots[c])
							if (auto* root = roots[c]())
								current.insert(root);
					ForEachGatheredRoot(rootListing, [&current](RE::NiNode* a_root) { current.insert(a_root); });
				}
				Engine::ForEachLoadedCell(*tes, addCell);
			}
			// The first-person skeleton (PlayerCharacter +0x8F8) has no parent. As a category node and parentless root, its equipment attaches
			// reach the tracker as events. Its kHidden is view-scoped, so ClassifyFrame stops at it.
			{
				const auto& views = Extensions::Views();
				for (std::uint32_t i = 0; i < views.rootCount; ++i) {
					const auto view = views.rootViews[i];
					if (!Extensions::ViewRegistered(view))
						continue;
					Api::ViewRootCapture root;
					Extensions::ViewEntryOf(view).captureRoot(root);
					if (auto* node = root.root) {
						current.insert(node);
						currentRoots.try_emplace(node, CategoryCapture::Root{ RE::NiPointer<RE::NiAVObject>(node), node, node });
						a_capture.viewRoots[static_cast<std::size_t>(view)] = node;
					}
				}
			}
		}

		/** @brief A capture that differs from a_previous only by its unheld roots. No cell or portal graph is walked. */
		void DeriveUnheldCapture(const CategoryCapture& a_previous, CategoryCapture& a_out)
		{
			a_out.viewRoots = a_previous.viewRoots;
			GatherUnheldRoots(a_out);
			for (auto* node : a_previous.nodes)
				if (!a_previous.unheld.contains(node))
					a_out.nodes.insert(node);
			for (const auto& [root, entry] : a_previous.roots)
				if (!a_previous.unheld.contains(root))
					a_out.roots.try_emplace(root, entry);
		}

		/** @brief None applied makes everything new. */
		void PlanCategoryPins(FrameStartJob& a_job, const CategoryCapture& a_capture, const std::deque<std::shared_ptr<const CategoryCapture>>& a_held,
			std::uint64_t a_applied)
		{
			const CategoryCapture* base = nullptr;
			for (const auto& held : a_held)
				if (held->generation == a_applied)
					base = held.get();
			const bool all = !base;
			for (auto* node : a_capture.nodes)
				if (all || !base->nodes.contains(node))
					a_job.pinNodes.push_back(node);
			for (const auto& [root, entry] : a_capture.roots)
				if (all || !base->roots.contains(root) || !base->nodes.contains(entry.category))
					a_job.pinRoots.push_back(entry.object);
		}

		/**
		 * @brief Pushes new parentless roots and detaches gone ones onto the tracker's stack, mirror-only. Each is read and pushed at once,
		 * since other threads attach and detach inside the window. A gone unheld root is not walked, since nothing holds it.
		 */
		void PushCategoryRoots(const CategoryCapture& a_capture, const CategoryCapture* a_previous, std::uint64_t& a_records, std::uint64_t& a_captures)
		{
			const auto& currentRoots = a_capture.roots;
			auto& tracker = SceneTracker::Get();
			std::vector<const void*> roots;
			roots.reserve(currentRoots.size());
			for (const auto& [root, entry] : currentRoots)
				roots.push_back(root);
			SceneCapture::SetDrawnRoots(roots);
			if (a_previous)
				for (const auto& [root, entry] : a_previous->roots)
					if (!currentRoots.contains(root) && entry.root) {
						auto* event = new SceneTracker::Event{};
						event->type = SceneTracker::EventType::Detached;
						SceneTracker::CollectGeometry(entry.root.get(), event->removed, &event->removedNodes);
						event->detachedRoot = root;
						tracker.PushCaptured(event);
					}
			for (const auto& [root, entry] : currentRoots)
				if (!a_previous || !a_previous->roots.contains(root))
					if (auto records = SceneCapture::CaptureAttached(*root)) {
						a_records += records->nodes.size();
						++a_captures;
						auto* event = new SceneTracker::Event{};
						event->type = SceneTracker::EventType::Attached;
						event->captured = std::move(records);
						tracker.PushCaptured(event);
					}
		}
	}

	void FrameView::ServeFrameRequests()
	{
		DCLF_ZONE("CS.DCLF.FrameRequests");
		RenderThreadBudget::Part budget(RenderThreadBudget::Bucket::SceneCaptures);
		// Engine writes, before the culls. The requests' references are dropped here on the render thread.
		{
			std::vector<std::shared_ptr<SwitchCatchUp>> requests;
			channels.switchCatchUps.Drain([&requests](std::shared_ptr<SwitchCatchUp>&& a_request) {
				if (a_request)
					requests.push_back(std::move(a_request));
			});
			for (const auto& request : requests) {
				attachedRoots.clear();
				for (const auto& root : request->attached)
					attachedRoots.push_back(root.get());
				CatchUpSwitches(attachedRoots, request->switches);
			}
			attachedRoots.clear();
			if (worldCatchUpPending)
				CatchUpSwitches({}, {});
		}
		{
			// A job left unfinished when a load came between is dropped, since it names the old world.
			if (frameStartJob && !frameStartJob->running.load(std::memory_order_acquire)) {
				if (frameStartJob->next >= frameStartJob->Items())
					frameStartJob.reset();
				else if (loadSinceCapture)
					frameStartJob.reset();  // an unposted capture's roots were pushed by the render thread; the forced capture follows them
			}
			if (frameStartJob) {
				if (!frameStartJob->running.load(std::memory_order_acquire))
					DispatchFrameStartJob(*frameStartJob);
			} else {
				frameStartJob = std::make_unique<FrameStartJob>();
				// Forced after a detach, which can take a category node before the signature sees its cell go, or after a load.
				const bool detached = channels.categoryDetachSeen.exchange(false, std::memory_order_acq_rel);
				CaptureCategories(std::exchange(categoryCapturePending, false) || detached);
				ServeFadeNodeRequests();
				if (frameStartJob->Empty())
					frameStartJob.reset();
				else
					DispatchFrameStartJob(*frameStartJob);
			}
			CaptureMirrorRequests();
			Diagnostics::Notify<Diagnostics::ScenePoint::MirrorProbePoint>();
		}
		Extensions::ServeDrawFamilies(sceneStep);
	}

	void FrameView::ServeFadeNodeRequests()
	{
		channels.fadeNodeRequests.Drain([&](FadeNodeRequest&& a_request) { frameStartJob->fadeRequests.push_back(std::move(a_request)); });
	}

	void FrameView::CatchUpSwitches(std::span<RE::NiAVObject* const> a_attached, std::span<const SwitchEvent> a_switches)
	{
		const bool world = std::exchange(worldCatchUpPending, false);
		if (!world && a_attached.empty() && a_switches.empty())
			return;
		DCLF_ZONE("CS.DCLF.Ingest.SwitchCatchUps");
		const auto start = std::chrono::steady_clock::now();
		const auto* root = static_cast<const RE::NiAVObject*>(CS_ENGINE_READ(Data, RecordTime, RE::Main::WorldRootNode()));
		auto inWorld = [root](const RE::NiAVObject* a_object) {
			for (std::uint32_t depth = 0; root && a_object && depth <= Engine::kMaxParentDepth; ++depth, a_object = a_object->parent)
				if (a_object == root)
					return true;
			return false;
		};
		std::vector<RE::NiAVObject*> stack;
		auto catchUpUnder = [&](RE::NiAVObject* a_from, std::atomic<std::uint64_t>& a_count) {
			stack.assign(1, a_from);
			std::uint64_t count = 0;
			while (!stack.empty()) {
				auto* object = stack.back();
				stack.pop_back();
				auto* node = object ? object->AsNode() : nullptr;
				if (!node)
					continue;
				if (auto* switchNode = node->AsSwitchNode(); switchNode && CatchUpSwitch(*switchNode))
					++count;
				for (auto& child : Engine::Children(*node))
					if (child)
						stack.push_back(child.get());
			}
			a_count.fetch_add(count, std::memory_order_relaxed);
		};
		if (world && root)
			catchUpUnder(const_cast<RE::NiAVObject*>(root), channels.catchUpsByAttach);
		for (auto* attachedRoot : a_attached)
			if (inWorld(attachedRoot))
				catchUpUnder(attachedRoot, channels.catchUpsByAttach);
		for (const auto& event : a_switches) {
			auto* node = event.node.get();
			auto* switchNode = node ? node->AsSwitchNode() : nullptr;
			if (!switchNode || !inWorld(node) || (!event.structural && SwitchIndexOf(node) == event.before))
				continue;
			if (CatchUpSwitch(*switchNode))
				channels.catchUpsBySwitch.fetch_add(1, std::memory_order_relaxed);
		}
		channels.catchUpNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
			std::memory_order_relaxed);
	}

	bool FrameView::CatchUpSwitch(RE::NiSwitchNode& a_switch)
	{
		// NiSwitchNode::OnVisible: childRevID.SetAt(index, revID) (FUN_140d29990), then the child's UpdateDownwardPass (vtable slot 0x2C).
		const auto state = Engine::ReadSwitch(a_switch);
		const auto selection = Engine::SelectionOf(a_switch, state);
		if (!selection.child || selection.current)
			return false;
		auto* base = reinterpret_cast<std::byte*>(&a_switch);
		using SetRevision = void (*)(void*, std::uint32_t, const std::uint32_t*);
		static const REL::Relocation<SetRevision> setRevision{ REL::Offset(Engine::Address::kSwitchSetRevision) };
		setRevision(base + 0x138, selection.index, reinterpret_cast<const std::uint32_t*>(base + 0x134));
		struct UpdateData
		{
			float time;
			std::uint32_t flags;
		} data{ *reinterpret_cast<const float*>(base + 0x130), (state.flags >> 1) & 1u };
		auto* child = selection.child;
		using UpdateDownwardPass = void (*)(RE::NiAVObject*, UpdateData*, std::uint32_t);
		(*reinterpret_cast<UpdateDownwardPass* const*>(child))[0x2C](child, &data, 0);
		return true;
	}

	bool FrameView::TakeSwitchChanges(std::vector<const RE::NiAVObject*>& a_out)
	{
		a_out.clear();
		a_out.swap(frameSwitchChanges);
		return std::exchange(frameSwitchResync, false);
	}

	void FrameView::CaptureCategories(bool a_force)
	{
		// The set changes only when a cell attaches or detaches. The signature is read again only after a watched edit or an input move.
		// The unheld roots come and go every few frames apart from it.
		DCLF_ZONE("CS.DCLF.Ingest.CaptureCategories");
		const auto start = std::chrono::steady_clock::now();
		const auto timed = [&] { categoryCaptureNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()); };
		SceneTracker::RetireCategoryNodes();
		// The coordinator let go of its copy before moving the generation, so these engine references are released here on the render thread.
		const std::uint64_t applied = channels.categoryAppliedGeneration.load(std::memory_order_acquire);
		while (categoryCapturesHeld.size() > 1 && categoryCapturesHeld.front()->generation < applied)
			categoryCapturesHeld.pop_front();
		// Otherwise every node under the captured cells would be held until the next cell change.
		const std::uint64_t pinsReleased = channels.categoryPinsReleased.load(std::memory_order_acquire);
		for (const auto& held : categoryCapturesHeld)
			if (held->generation <= pinsReleased && !held->pins.empty())
				std::exchange(held->pins, {});
		const std::uint64_t signature = RefreshCategorySignature(a_force);
		// Read before the gather. An edit made during it moves them again.
		const std::uint64_t unheldEdits = SceneTracker::UnheldEdits();
		const bool full = a_force || loadSinceCapture || !categoryCapture || signature != categoryCapture->signature;
		if (!full && unheldEdits == categoryWatch.unheldEdits)
			return timed();
		categoryWatch.unheldEdits = unheldEdits;
		UnheldRootsScope unheldScope;
		unheldScope.Enter();
		auto capture = std::make_shared<CategoryCapture>();
		capture->generation = ++categoryCaptures;
		capture->signature = signature;
		if (full) {
			++categoryCapturesMade;
			GatherCategoryNodes(*capture);
		} else {
			DeriveUnheldCapture(*categoryCapture, *capture);
		}
		capture->held.reserve(capture->nodes.size());
		for (auto* node : capture->nodes)
			if (!capture->unheld.contains(node))
				capture->held.emplace_back(node);
		auto& job = *frameStartJob;
		job.capture = capture;
		PlanCategoryPins(job, *capture, categoryCapturesHeld, applied);
		PushCategoryRoots(*capture, categoryCapture.get(), categoryMirrorRecords, categoryMirrorCaptures);
		categoryCapturesHeld.push_back(capture);
		categoryCapture = std::move(capture);
		loadSinceCapture = false;
		timed();
	}

	void FrameView::CaptureMirrorRequests()
	{
		// Objects the mirror lacks, such as references moved into multibounds without a hook. Read and pushed at once on the render
		// thread, since other threads attach and detach inside the window.
		std::vector<RE::NiPointer<RE::NiAVObject>> requests;
		channels.mirrorCaptureRequests.Drain([&requests](RE::NiPointer<RE::NiAVObject>&& a_request) { requests.push_back(std::move(a_request)); });
		if (requests.empty())
			return;
		DCLF_ZONE("CS.DCLF.Ingest.MirrorRequests");
		auto& tracker = SceneTracker::Get();
		ankerl::unordered_dense::set<const RE::NiAVObject*> captured;
		for (const auto& object : requests) {  // a geometry, or (AddSubtree) a subtree's root
			if (!object)
				continue;
			bool covered = false;
			for (const RE::NiAVObject* at = object.get(); at && !covered; at = at->parent)
				covered = captured.contains(at);
			if (covered)
				continue;
			captured.insert(object.get());
			if (auto records = SceneCapture::CaptureAttached(*object)) {
				categoryMirrorRecords += records->nodes.size();
				++categoryMirrorCaptures;
				auto* event = new SceneTracker::Event{};
				event->type = SceneTracker::EventType::Attached;
				event->captured = std::move(records);
				tracker.PushCaptured(event, true);
			}
		}
	}

	void FrameView::PrepareAccumulatePhase()
	{
		DCLF_ZONE("CS.DCLF.Accumulate.Prepare");
		// No sceneBuilt test, since a scene pass may be running. The queue is empty when no join ran.
		ServeMaterialRequests();
		Diagnostics::Notify<Diagnostics::ScenePoint::AccumulatePrepared>();
	}

	void FrameView::ServeMaterialRequests()
	{
		channels.materialRequests.Drain([&](MaterialRequest&& a_request) {
			// A property whose material changed since is stale, and the join asks again. The reference is dropped here on the render thread.
			const bool current = a_request.property && a_request.property->material == a_request.material;
			if (current)
				MaterialPort::PushCapture(a_request.material);
			channels.materialRequestsAnswered.Push(MaterialAnswer{ a_request.material, current });
		});
	}

	void FrameView::PostAccumulateInputs()
	{
		DCLF_ZONE("CS.DCLF.Accumulate.Inputs");
		DrainCapture();
		PostAncestorMap();
	}

	void FrameView::DrainCapture()
	{
		auto& capture = PassCapture::Get();
		if (!capture.Installed())
			return;
		// Always drained, since the buffer is fixed-capacity and would overflow. Nothing reads it.
		(void)capture.Drain();
	}

	void FrameView::PostAncestorMap()
	{
		// Light Limit Fix swaps its room map at its own point, so only a copy is posted.
		if (!Extensions::AncestorValueOn())
			return;
		const auto& value = *Extensions::AncestorValue();
		const std::uint64_t generation = value.generation();
		if (generation == ancestorMapPostedGeneration)
			return;
		auto map = std::make_shared<Extensions::AncestorValueMap>();
		Api::AncestorMapSink sink(*map);
		value.fill(sink);
		auto input = std::make_shared<AncestorMapInput>();
		input->map = std::move(map);
		input->generation = generation;
		channels.ancestorMapPosted.store(std::shared_ptr<const AncestorMapInput>(std::move(input)), std::memory_order_release);
		ancestorMapPostedGeneration = generation;
		channels.accumulateInputs.ancestorMapsPosted.fetch_add(1, std::memory_order_relaxed);
	}

	void FrameView::PostPipelineFrame(const GeometryPort::PipelineFrame& a_frame)
	{
		channels.pipelineFramePosted.store(std::make_shared<const GeometryPort::PipelineFrame>(a_frame), std::memory_order_release);
		channels.accumulateInputs.pipelineFramesPosted.fetch_add(1, std::memory_order_relaxed);
	}

	void FrameView::CaptureLightingShader()
	{
		// The engine's BSLightingShader instance (0x14338ca00). Read before the frame inputs' post, so the scene lane reads only what was set before it.
		if (EngineShaders::Lighting())
			return;
		static const REL::Relocation<RE::BSShader**> lightingShader{ REL::Offset(Engine::Address::kLightingShader) };
		if (auto* shader = *lightingShader.get(); shader && shader->shaderType.get() == RE::BSShader::Type::Lighting)
			EngineShaders::SetLighting(shader);
	}

	void FrameView::LatchAccumulator()
	{
		auto* accumulator = *CS_ENGINE_READ(Accumulator, CapturePoint, globals::game::currentAccumulator).get();
		if (!accumulator || accumulator == latchedAccumulator)
			return;
		if (latchedAccumulator) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				logger::warn("[DCLF] the main camera's accumulator changed ({} -> {}); the tables follow it",
					static_cast<const void*>(latchedAccumulator), static_cast<const void*>(accumulator));
			}
		}
		latchedAccumulator = accumulator;
	}
}
