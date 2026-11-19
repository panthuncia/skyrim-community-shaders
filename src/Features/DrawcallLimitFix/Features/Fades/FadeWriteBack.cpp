// The fade write-back. A task on DCLF's executor turns harvested fade events into stores.
// The main thread writes those stores to the engine nodes at Main::Update's entry.

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneExecutor.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace DCLF::Features::Fades
{
	namespace
	{
		// The GPU never resends a milestone once listed. A lost store leaves the node stuck mid-fade until the root is listed again.
		std::atomic<int> lostLines{ 0 };
		template <class... A>
		void LogLost(std::format_string<A...> a_format, A&&... a_args)
		{
			if (lostLines.fetch_add(1, std::memory_order_relaxed) < 200)
				logger::info("{}", std::format(a_format, std::forward<A>(a_args)...));
		}
		// The task holds this lock while it judges, so a census never sees a half-grown appliedFrame or a write-back being destroyed.
		std::mutex censusMutex;
		std::atomic<FadeWriteBack*> censusWriteBack{ nullptr };

		FadeScene* CurrentScene()
		{
			const auto* scene = IndirectDraws::Get().Scene();
			return scene ? SceneOf(*scene) : nullptr;
		}
	}

	void WriteFadeStores(bool a_live, const std::function<bool(const void*)>& a_written)
	{
		const auto* fades = CurrentScene();
		if (!fades || !fades->fadeWriteBack)
			return;
		auto& writeBack = *fades->fadeWriteBack;
		// No cull or animation runs now, so every reader of the node sees these stores this frame.
		std::unique_ptr<FadeWriteBack::Stores> stores{ writeBack.ready.exchange(nullptr, std::memory_order_acq_rel) };
		if (!stores)
			return;
		writeBack.readyStores.store(0, std::memory_order_relaxed);
		// A released publication means the nodes may be gone. That would be a defect, so the stores are dropped and logged.
		if (!a_live || !EngineReleases::Alive(stores->publication)) {
			for (const auto& store : stores->stores)
				LogLost("[DCLF] fade write-back: store dropped ({}): node {} fade {:.3f}", a_live ? "publication released" : "not live", store.node, store.fade);
			writeBack.dropped.fetch_add(stores->stores.size(), std::memory_order_relaxed);
			return;
		}
		DCLF_ZONE("CS.DCLF.FadeWriteBack.Stores");
		const auto start = std::chrono::steady_clock::now();
		std::uint64_t applied = 0, notWritten = 0;
		for (const auto& store : stores->stores) {
			if (!a_written(store.node)) {
				LogLost("[DCLF] fade write-back: store dropped (not written by DCLF): node {} fade {:.3f}", store.node, store.fade);
				++notWritten;
				continue;
			}
			// The flag bits are changed atomically because the engine owns the other bits.
			auto* node = static_cast<std::byte*>(store.node);
			std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(node + 0xF4));
			flags.fetch_and(~kFadeFlagMask, std::memory_order_relaxed);
			flags.fetch_or(store.flags, std::memory_order_relaxed);
			*reinterpret_cast<float*>(node + 0x130) = store.fade;
			// Only the LOD step and OnVisible otherwise write these, and neither runs now.
			std::uint32_t fields = SceneCapture::NodeRecord::kFadeCurrent;
			if (store.lod) {
				*reinterpret_cast<float*>(node + 0x14C) = store.blend;
				*reinterpret_cast<std::uint8_t*>(node + 0x152) = static_cast<std::uint8_t>(store.levels);
				*reinterpret_cast<std::uint8_t*>(node + 0x153) = static_cast<std::uint8_t>(store.levels >> 8);
				fields |= SceneCapture::NodeRecord::kFadeLevel;
			}
			// Keeps the mirror current. This is a plain update, not a fade event.
			SceneTracker::Get().PushUpdate(SceneCapture::Update{ fields, SceneCapture::CaptureNodeFields(*static_cast<const RE::NiAVObject*>(store.node), fields) });
			++applied;
		}
		writeBack.applied.fetch_add(applied, std::memory_order_relaxed);
		writeBack.notWritten.fetch_add(notWritten, std::memory_order_relaxed);
		writeBack.storeNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
			std::memory_order_relaxed);
	}

	void PostCpuFadeEvents(FadeWriteBack& a_writeBack, std::uint32_t a_frame, std::vector<FadeEvent> a_events)
	{
		a_writeBack.harvested.fetch_add(1, std::memory_order_relaxed);
		if (a_events.empty())
			return;
		auto* batch = new FadeWriteBack::Batch();
		batch->frame = a_frame;
		a_writeBack.events.fetch_add(a_events.size(), std::memory_order_relaxed);
		a_writeBack.harvestedWithEvents.fetch_add(1, std::memory_order_relaxed);
		batch->events = std::move(a_events);
		a_writeBack.Push(batch);
	}

	void KickFadeWriteBack()
	{
		const auto* scene = IndirectDraws::Get().Scene();
		auto fades = scene ? SharedSceneOf(*scene) : nullptr;
		if (!fades || !fades->fadeWriteBack)
			return;
		auto& writeBack = *fades->fadeWriteBack;
		if (censusWriteBack.load(std::memory_order_relaxed) != &writeBack) {
			std::lock_guard lock(censusMutex);
			censusWriteBack.store(&writeBack, std::memory_order_relaxed);
		}
		// One task at a time. A running task or the next one picks up the new batches.
		if (bool idle = false; !writeBack.running.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
			writeBack.busy.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		std::shared_ptr<const SceneTables> tables = Scene::FrameView::Get().AcceptedTables();
		const std::uint64_t publication = Scene::FrameView::Get().InstalledSequence();
		auto retirement = Scene::FrameView::Get().InstalledRetirement();
		auto find = [fades = std::move(fades), tables, publication, retirement = std::move(retirement)] {
			DCLF_ZONE("CS.DCLF.FadeWriteBack");
			auto& state = *fades->fadeWriteBack;
			const auto list = tables ? Extensions::RootListOf(*tables) : Api::RootListView{};
			// Sort by frame. The stack is newest first, and recordings may finish out of order.
			std::vector<FadeWriteBack::Batch*> batches;
			for (auto* batch = state.batches.exchange(nullptr, std::memory_order_acquire); batch; batch = batch->next)
				batches.push_back(batch);
			state.pendingBatches.fetch_sub(batches.size(), std::memory_order_relaxed);
			std::reverse(batches.begin(), batches.end());
			std::stable_sort(batches.begin(), batches.end(), [](const auto* a_a, const auto* a_b) { return a_a->frame < a_b->frame; });
			auto stores = std::make_unique<FadeWriteBack::Stores>();
			stores->tables = tables;
			stores->publication = publication;
			stores->retirement = retirement;
			// Uncontended outside a diagnostic capture.
			std::unique_lock censusLock(censusMutex);
			for (auto* batch : batches) {
				for (const auto& event : batch->events) {
					if (!tables)
						break;
					if (event.root >= state.appliedFrame.size())
						state.appliedFrame.resize(std::size_t(event.root) + 1, 0u);
					// Skip roots DCLF no longer writes, and events older than the last one applied.
					if (event.root >= list.rows.size() || list.rows[event.root].generation != event.generation ||
						!(list.rows[event.root].bits & kFadeRootWritten) || !list.nodes[event.root] || batch->frame < state.appliedFrame[event.root]) {
						LogLost("[DCLF] fade write-back: event stale: root {} gen {} (list {} rows, row gen {}, written {}, node {}), frame {} applied {}, fade {:.3f}", event.root,
							event.generation, list.rows.size(), event.root < list.rows.size() ? list.rows[event.root].generation : 0u,
							event.root < list.rows.size() && (list.rows[event.root].bits & kFadeRootWritten) != 0, event.root < list.rows.size() ? list.nodes[event.root] : nullptr,
							batch->frame, state.appliedFrame[event.root], event.currentFade);
						state.stale.fetch_add(1, std::memory_order_relaxed);
						continue;
					}
					state.appliedFrame[event.root] = batch->frame;
					const std::uint32_t plan = list.rows[event.root].bits & kFadeRootPlanMask;
					stores->stores.push_back({ const_cast<void*>(list.nodes[event.root]), event.flags & kFadeFlagMask, event.currentFade,
						plan == kFadeRootLeaf || plan == kFadeRootTree, event.levels, event.blend });
				}
				delete batch;
			}
			censusLock.unlock();
			// Stores from an earlier task that were not written yet go first.
			if (std::unique_ptr<FadeWriteBack::Stores> earlier{ state.ready.exchange(nullptr, std::memory_order_acq_rel) }) {
				earlier->stores.insert(earlier->stores.end(), stores->stores.begin(), stores->stores.end());
				earlier->tables = std::move(stores->tables);
				// Keep the earlier retirement node, because an older node holds every newer one.
				earlier->publication = std::min(earlier->publication, stores->publication);
				stores = std::move(earlier);
			}
			if (!stores->stores.empty()) {
				state.readyStores.store(stores->stores.size(), std::memory_order_relaxed);
				state.ready.store(stores.release(), std::memory_order_release);
			}
			state.running.store(false, std::memory_order_release);
			state.running.notify_all();
		};
		if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "fade write-back",
				[find = std::move(find)](const auto&) { find(); }))
			stl::report_and_fail("Drawcall Limit Fix: the fade write-back was refused by DCLF's executor");
	}

	void WaitFadeWriteBack()
	{
		const auto* fades = CurrentScene();
		if (!fades || !fades->fadeWriteBack)
			return;
		auto& running = fades->fadeWriteBack->running;
		while (running.load(std::memory_order_acquire))
			running.wait(true, std::memory_order_acquire);
		// Called on teardown, toggle or load, when the roots may no longer stand in, so the stores are dropped.
		std::unique_ptr<FadeWriteBack::Stores> dropped{ fades->fadeWriteBack->ready.exchange(nullptr, std::memory_order_acq_rel) };
		fades->fadeWriteBack->readyStores.store(0, std::memory_order_relaxed);
		if (dropped)
			for (const auto& store : dropped->stores)
				LogLost("[DCLF] fade write-back: store dropped (write-back wait): node {} fade {:.3f}", store.node, store.fade);
	}

	void ForgetWriteBackCensus(const FadeWriteBack* a_writeBack)
	{
		std::lock_guard lock(censusMutex);
		if (censusWriteBack.load(std::memory_order_relaxed) == a_writeBack)
			censusWriteBack.store(nullptr, std::memory_order_relaxed);
	}

	WriteBackCensus PeekWriteBackCensus(bool a_frames)
	{
		WriteBackCensus census;
		std::lock_guard lock(censusMutex);
		const auto* writeBack = censusWriteBack.load(std::memory_order_relaxed);
		if (!writeBack)
			return census;
		const auto& w = *writeBack;
		census.present = true;
		census.applied = w.applied.load(std::memory_order_relaxed);
		census.stale = w.stale.load(std::memory_order_relaxed);
		census.busy = w.busy.load(std::memory_order_relaxed);
		census.notWritten = w.notWritten.load(std::memory_order_relaxed);
		census.dropped = w.dropped.load(std::memory_order_relaxed);
		census.harvested = w.harvested.load(std::memory_order_relaxed);
		census.harvestedWithEvents = w.harvestedWithEvents.load(std::memory_order_relaxed);
		census.events = w.events.load(std::memory_order_relaxed);
		census.overflowed = w.overflowed.load(std::memory_order_relaxed);
		census.missed = w.missed.load(std::memory_order_relaxed);
		census.setsAdded = w.setsAdded.load(std::memory_order_relaxed);
		census.pendingBatches = w.pendingBatches.load(std::memory_order_relaxed);
		census.readyStores = w.readyStores.load(std::memory_order_relaxed);
		census.appliedFrameSize = w.appliedFrame.size();
		if (a_frames)
			census.appliedFrame = w.appliedFrame;
		return census;
	}
}
