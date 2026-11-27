// Native's routed game updates and their render half. See RenderThread.h.
// Main::DrawFrame (AE 1.6.1170 0x140643c00 id 36555, SE 1.5.97 0x1405b1020 id 35556): the order the game frame and the render half
// follow (snap7-thread.md section 2).

#include "RenderGraph/Engine/Native/RenderThread.h"

#include "Features/DrawcallLimitFix.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Scaleform/Scaleform.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "Snapshot/FrameSnapshot.h"
#include "Snapshot/Publication.h"
#include "RenderGraph/Async/Retirement.h"

#include "Globals.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace RenderGraph::Engine::Native::RenderThread
{
	namespace
	{
		template <class T>
		T& Global(std::uint64_t a_se, std::uint64_t a_ae)
		{
			return *reinterpret_cast<T*>(REL::RelocationID(a_se, a_ae).address());
		}

		thread_local bool t_routedRender = false;
		thread_local bool t_renderThread = false;

		/** @brief The render thread and its park handshake. */
		struct Thread
		{
			std::mutex mutex;
			std::condition_variable changed;
			bool started = false;
			bool parkAsked = false;  // a producer asked the render thread to stop after its frame
			bool parked = false;     // the render thread stopped (or has not rendered yet)
			std::atomic<bool> running{ false };
			// Parks by producer since the last line, and the producers' wait.
			std::uint64_t parks = 0, drawFrameParks = 0, loadingParks = 0, movieParks = 0, interfaceParks = 0;
			double parkWaitMs = 0.0, parkWaitMaxMs = 0.0;
		};

		Thread& Get()
		{
			static auto* thread = new Thread;  // never destroyed: the thread runs until the process ends
			return *thread;
		}

		/** @brief Render thread. The GPU point of the last render's submissions, until a commit waits for it. */
		thread_local std::optional<::org::GraphHost::GpuPoint> t_lastRender;

		void Loop()
		{
			t_renderThread = true;
			::SetThreadDescription(::GetCurrentThread(), L"CS native render thread");
			// Its Begins adopt the newest published snapshot and publish no frame-only one.
			const Frame::Driver::GameSnapshotRenderScope scope;
			auto& thread = Get();
			for (;;) {
				{
					std::unique_lock lock(thread.mutex);
					if (thread.parkAsked) {
						thread.parked = true;
						thread.running.store(false, std::memory_order_release);
						thread.changed.notify_all();
						thread.changed.wait(lock, [&] { return !thread.parkAsked; });
						thread.parked = false;
						thread.running.store(true, std::memory_order_release);
					}
				}
				RenderHalf();
				t_lastRender = ::RenderGraph::Frame::Retirement::SubmittedGpuPoint();
			}
		}

		/** @brief Main::DrawFrame (SE 35556, AE 36555): a producer that renders on the game thread parks the render thread first. */
		struct DrawFrame
		{
			static void thunk(RE::Main* a_main)
			{
				Park("Main::DrawFrame");
				func(a_main);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Stats
		{
			std::atomic<std::uint64_t> routed{ 0 }, unrouted{ 0 }, interfaceInline{ 0 }, renders{ 0 };
		} g_stats;

		/** @brief MenuControls' debug camera path test (SE 51363, AE 52210): Main::DrawFrame then draws the path's frame. */
		bool CameraPathActive()
		{
			auto* controls = RE::MenuControls::GetSingleton();
			using Fn = bool (*)(RE::MenuControls*);
			static REL::Relocation<Fn> active{ REL::RelocationID(51363, 52210) };
			return controls && active(controls);
		}

		/** @brief The thread's scrap heap (MemoryManager::GetThreadScrapHeap), marked before the world and released after it. */
		struct ScrapHeapScope
		{
			ScrapHeapScope()
			{
				heap = RE::MemoryManager::GetSingleton()->GetThreadScrapHeap();
				using Mark = void (*)(RE::ScrapHeap*);
				static REL::Relocation<Mark> mark{ REL::RelocationID(66889, 68150) };
				mark(heap);
			}
			~ScrapHeapScope()
			{
				using Release = void (*)(RE::ScrapHeap*);
				static REL::Relocation<Release> release{ REL::RelocationID(66890, 68151) };
				release(heap);
			}
			ScrapHeapScope(const ScrapHeapScope&) = delete;
			ScrapHeapScope& operator=(const ScrapHeapScope&) = delete;
			RE::ScrapHeap* heap = nullptr;
		};
	}

	bool Routes(const Snapshot::FrameSnapshot& a_snapshot)
	{
		if (!Native::Active() || !Scene::Owns())
			return false;
		const bool routed = a_snapshot.kind == Snapshot::FrameKind::World && !a_snapshot.menus.paused && !a_snapshot.menus.map && !CameraPathActive();
		(routed ? g_stats.routed : g_stats.unrouted).fetch_add(1, std::memory_order_relaxed);
		return routed;
	}

	std::shared_ptr<const FramePayload> GameFrame(RE::Main* a_main, const Snapshot::FrameSnapshot& a_snapshot)
	{
		ZoneScopedN("CS.Native.GameFrame");
		auto payload = std::make_shared<FramePayload>();
		auto* ui = RE::UI::GetSingleton();
		// The menus' PreDisplay (SE 79948, AE 82085): UI game logic.
		using PreDisplay = void (*)(RE::UI*);
		static REL::Relocation<PreDisplay> preDisplay{ REL::RelocationID(79948, 82085) };
		preDisplay(ui);
		// The world branch: no menu freezes the background (the snapshot's kind) and the player has 3D.
		Global<bool>(516934, 403436) = false;  // gIsInMenuMode2
		{
			const ScrapHeapScope scrap;
			// WorldPtToScreenPt3's camera (SE 50744, AE 51639) from the world scene graph's camera.
			using SetScreenCamera = void (*)(RE::NiCamera*);
			static REL::Relocation<SetScreenCamera> setScreenCamera{ REL::RelocationID(50744, 51639) };
			setScreenCamera(RE::Main::WorldRootCamera());
			payload->prepared = Scene::PrepareRouted(a_main, a_snapshot);
		}
		// Volumetric lighting's RenderEffect (the console's TVL flags) and its target, read here; the render half draws it.
		if (Global<const bool>(528190, 415135) && Global<const bool>(528189, 415134)) {
			payload->volumetricLighting = true;
			payload->volumetricTarget = Global<const std::int32_t>(513720, 391674) == 0 ? 0x4D : 0x4F;
		}
		// The frame jobs, and the UI jobs' wait (SE 34551 / AE 35406, SE 68167 / AE 69516).
		using FrameJobs = bool (*)(bool);
		static REL::Relocation<FrameJobs> frameJobs{ REL::RelocationID(34551, 35406) };
		using WaitForJobTask = void (*)(std::uint32_t, std::uint32_t);
		static REL::Relocation<WaitForJobTask> waitForJobTask{ REL::RelocationID(68167, 69516) };
		if (frameJobs(true))
			waitForJobTask(0, 2);
		// The interface: recorded alone, without render state. A menu the UI jobs opened that pauses or shows the map draws engine work
		// in its displays, so the render half calls DrawInterface itself, on the render owner.
		if (Scaleform::RecordsUiOnly(a_snapshot))
			Scaleform::ProduceUi(ui);
		else {
			payload->interfaceInline = true;
			g_stats.interfaceInline.fetch_add(1, std::memory_order_relaxed);
		}
		// The save screenshot request (SE 35888, AE 36859), with the player's 3D. No menu freezes the background, so the frozen frame's
		// requests (36858, 36857) are not asked.
		if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player->GetParentCell() && player->Get3D2()) {
			using Request = void (*)();
			static REL::Relocation<Request> request{ REL::RelocationID(35888, 36859) };
			request();
		}
		// The world scene graph's upkeep (SE 74986, AE 76748) and the render passes' returns (SE 100722, AE 107502).
		using SceneGraphUpkeep = void (*)(RE::NiNode*);
		static REL::Relocation<SceneGraphUpkeep> sceneGraphUpkeep{ REL::RelocationID(74986, 76748) };
		sceneGraphUpkeep(RE::Main::WorldRootNode());
		using ReturnPasses = void (*)();
		static REL::Relocation<ReturnPasses> returnPasses{ REL::RelocationID(100722, 107502) };
		returnPasses();
		return payload;
	}

	void RenderHalf()
	{
		ZoneScopedN("CS.Native.RenderHalf");
		const bool outer = std::exchange(t_routedRender, true);
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		using Lock = void (*)(RE::BSGraphics::Renderer*);
		static REL::Relocation<Lock> lock{ REL::RelocationID(75458, 77243) };
		static REL::Relocation<Lock> unlock{ REL::RelocationID(75459, 77244) };
		using Begin = void (*)(RE::BSGraphics::Renderer*, std::uint32_t);
		static REL::Relocation<Begin> begin{ REL::RelocationID(75460, 77245) };
		using End = void (*)(RE::BSGraphics::Renderer*);
		static REL::Relocation<End> end{ REL::RelocationID(75461, 77246) };
		lock(renderer);
		// CS's frame start (claim, adoption, OnAdopted, OnBegin) and the engine's Begin body; FaceTint's render frame after it.
		begin(renderer, 0);
		const auto& snapshot = Snapshot::Current();
		const auto* payload = snapshot ? static_cast<const FramePayload*>(snapshot->nativeFrame.get()) : nullptr;
		if (!payload)
			RenderGraph::Fatal("native render half", fmt::format("the adopted snapshot {} has no routed frame payload", snapshot ? snapshot->id : 0));
		g_stats.renders.fetch_add(1, std::memory_order_relaxed);
		Scene::RecordRouted(payload->prepared);
		if (payload->volumetricLighting) {
			using RenderEffect = void (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
			static REL::Relocation<RenderEffect> renderEffect{ REL::RelocationID(99025, 105676) };
			renderEffect(RE::ImageSpaceManager::GetSingleton(), 0x22, payload->volumetricTarget, 0, 0);
		}
		if (payload->interfaceInline) {
			using DrawInterface = void (*)(RE::UI*);
			static REL::Relocation<DrawInterface> drawInterface{ REL::RelocationID(79947, 82084) };
			drawInterface(RE::UI::GetSingleton());
		} else
			Scaleform::ReplaySnapshotUi(*snapshot);
		end(renderer);
		unlock(renderer);
		// Dynamic resolution's scale for the next frame (SE 508794 / AE 380760, SE 75579 / AE 77385, SE 75708 / AE 77517).
		if (Global<const bool>(508794, 380760)) {
			using Scale = float (*)(RE::BSGraphics::Renderer*);
			static REL::Relocation<Scale> scale{ REL::RelocationID(75579, 77385) };
			using SetScale = void (*)(void*, float);
			static REL::Relocation<SetScale> setScale{ REL::RelocationID(75708, 77517) };
			if (const float value = scale(renderer); value != 0.0f)
				setScale(RE::BSGraphics::State::GetSingleton(), value);
		}
		t_routedRender = outer;
	}

	void Superseded(const Snapshot::FrameSnapshot& a_old, const Snapshot::FrameSnapshot& a_fresh)
	{
		const auto* old = static_cast<const FramePayload*>(a_old.nativeFrame.get());
		const auto* fresh = static_cast<const FramePayload*>(a_fresh.nativeFrame.get());
		if (old && fresh)
			globals::features::drawcallLimitFix.NativeTicketSuperseded(old->prepared.sceneTicket, fresh->prepared.sceneTicket);
	}

	void OnAdopted()
	{
		if (!t_routedRender)
			return;
		// The adopted snapshot's DCLF ticket: this Begin's revision adopts its scene (a repeat adopts nothing).
		const auto& snapshot = Snapshot::Current();
		if (const auto* payload = snapshot ? static_cast<const FramePayload*>(snapshot->nativeFrame.get()) : nullptr)
			globals::features::drawcallLimitFix.ExpectNativeTicket(payload->prepared.sceneTicket);
	}

	void WaitForLastRender()
	{
		if (!t_lastRender)
			return;
		if (!t_lastRender->Wait())
			RenderGraph::Fatal("native render thread", "the wait for the last render's GPU work failed");
		t_lastRender.reset();
	}

	void InstallHooks()
	{
		if (REL::Module::IsVR() || !Native::Requested())
			return;
		stl::detour_thunk<DrawFrame>(REL::RelocationID(35556, 36555));
		if (!DrawFrame::func.address())
			RenderGraph::Fatal("native render thread", "Main::DrawFrame could not be hooked, so its frames could not park the render thread");
	}

	void OnPublished()
	{
		auto& thread = Get();
		std::unique_lock lock(thread.mutex);
		if (!thread.started) {
			thread.started = true;
			thread.running.store(true, std::memory_order_release);
			std::thread(&Loop).detach();
			logger::info("[ORG native] render thread: started at the first routed update; it renders the published snapshots from here on");
			return;
		}
		if (thread.parkAsked) {
			thread.parkAsked = false;
			thread.changed.notify_all();
		}
	}

	void Park(const char* a_why)
	{
		if (t_renderThread)
			return;
		auto& thread = Get();
		std::unique_lock lock(thread.mutex);
		if (!thread.started || thread.parkAsked)
			return;
		const auto start = std::chrono::steady_clock::now();
		thread.parkAsked = true;
		thread.changed.wait(lock, [&] { return thread.parked; });
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		++thread.parks;
		thread.parkWaitMs += ms;
		thread.parkWaitMaxMs = (std::max)(thread.parkWaitMaxMs, ms);
		const std::string_view why = a_why;
		++(why == "Main::DrawFrame"     ? thread.drawFrameParks :
			why == "the loading screen" ? thread.loadingParks :
			why == "a movie tick"       ? thread.movieParks :
										  thread.interfaceParks);
	}

	bool Running()
	{
		return Get().running.load(std::memory_order_acquire);
	}

	bool OnRenderThread()
	{
		return t_renderThread;
	}

	bool InRoutedRender()
	{
		return t_routedRender;
	}

	std::string TakeStatsLine()
	{
		if (!Native::Active())
			return {};
		auto& thread = Get();
		std::uint64_t parks = 0, drawFrame = 0, loading = 0, movie = 0, interfaces = 0;
		double waitMs = 0.0, maxMs = 0.0;
		{
			const std::lock_guard lock(thread.mutex);
			parks = std::exchange(thread.parks, 0);
			drawFrame = std::exchange(thread.drawFrameParks, 0);
			loading = std::exchange(thread.loadingParks, 0);
			movie = std::exchange(thread.movieParks, 0);
			interfaces = std::exchange(thread.interfaceParks, 0);
			waitMs = std::exchange(thread.parkWaitMs, 0.0);
			maxMs = std::exchange(thread.parkWaitMaxMs, 0.0);
		}
		return fmt::format(
			"[ORG native] routed updates: {} routed (CS's game frame, render half from the payload), {} on Main::DrawFrame, {} with the "
			"interface drawn by the producer; {} routed renders; render thread parked {} times (Main::DrawFrame {}, loading screen {}, "
			"movie {}, interface {}), the producers waited {:.2f} ms in all, at most {:.2f} ms",
			g_stats.routed.exchange(0), g_stats.unrouted.exchange(0), g_stats.interfaceInline.exchange(0), g_stats.renders.exchange(0), parks, drawFrame,
			loading, movie, interfaces, waitMs, maxMs);
	}
}
