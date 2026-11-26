#include "Snapshot/Publication.h"

#include "RenderGraph/Async/LatestSlot.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/Revision.h"
#include "RenderGraph/FrameClock.h"
#include "RenderGraph/Switches.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace Snapshot
{
	const char* FrameKindName(FrameKind a_kind)
	{
		switch (a_kind) {
		case FrameKind::World:
			return "world";
		case FrameKind::MenuOverWorld:
			return "menu over world";
		case FrameKind::MainMenu:
			return "main menu";
		case FrameKind::Loading:
			return "loading";
		case FrameKind::Movie:
			return "movie";
		default:
			return "?";
		}
	}

	namespace
	{
		// A main camera move larger than this in one game update is a cut (game units; a sprint is about 10 a frame).
		constexpr float kCutDistance = 512.0f;
		// A turn larger than this in one update is a cut: cos(60 degrees).
		constexpr float kCutTurnCos = 0.5f;
		// A player move larger than this in one update without a loading screen is a teleport.
		constexpr float kTeleportDistance = 1024.0f;

		/** @brief The cadence test mode, parsed once. */
		struct Cadence
		{
			enum class Mode : std::uint8_t
			{
				Off,
				Repeat,
				Drop
			};
			Mode mode = Mode::Off;
			std::uint32_t n = 1;
		};

		/** @brief The game side's state. Main thread. */
		struct Producer
		{
			std::vector<CaptureFn> captures;
			std::shared_ptr<const FrameSnapshot> previous;  // the last published, adopted or not
			std::uint64_t nextId = 1;
			const RE::TESObjectCELL* previousCell = nullptr;  // a key, never read
			RE::NiPoint3 previousPlayer{};
			bool havePlayer = false;
			std::uint64_t updates = 0;  // game updates seen by the cadence mode
		};

		/** @brief Counters since the last load's line. Atomic: the render side adds to them too. */
		struct Stats
		{
			std::atomic<std::uint64_t> published{ 0 }, replaced{ 0 }, adopted{ 0 }, repeats{ 0 }, renders{ 0 };
			std::atomic<std::uint64_t> blocksShared{ 0 }, blocksMade{ 0 }, captureNs{ 0 };
			std::atomic<std::uint32_t> mostFolded{ 0 };
		};

		/** @brief The render side's state. Render owner (the main thread in phase 0). */
		struct Consumer
		{
			std::shared_ptr<const FrameSnapshot> current;
			RenderIdentity render{};
			std::chrono::steady_clock::time_point lastAdopt{};
		};

		Producer& Game()
		{
			static Producer producer;
			return producer;
		}

		Consumer& RenderSide()
		{
			static Consumer consumer;
			return consumer;
		}

		Stats& Counters()
		{
			static Stats stats;
			return stats;
		}

		// Latest wins. The producer takes back what the consumer did not adopt before it posts the next (Publish), so the two only
		// ever exchange the pointer: one producer, one consumer.
		RenderGraph::Frame::LatestSlot<std::shared_ptr<const FrameSnapshot>>& Slot()
		{
			static RenderGraph::Frame::LatestSlot<std::shared_ptr<const FrameSnapshot>> slot;
			return slot;
		}

		std::atomic<std::uint64_t> g_currentId{ 0 };
		std::atomic<std::uint64_t> g_loadEpoch{ 0 };
		std::atomic<bool> g_loadStartedPending{ false };
		std::atomic<bool> g_loadEndedPending{ false };

		const Cadence& CadenceMode()
		{
			static const Cadence cadence = [] {
				Cadence parsed{};
				const auto value = ::Switches::Value("CS_SNAPSHOT_CADENCE");
				if (value.empty())
					return parsed;
				const auto colon = value.find(':');
				const auto name = value.substr(0, colon);
				std::uint32_t n = 0;
				const auto digits = colon == std::string::npos ? std::string{} : value.substr(colon + 1);
				const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
				const bool number = error == std::errc{} && end == digits.data() + digits.size() && n >= 2 && n <= 16;
				if ((name != "repeat" && name != "drop") || !number)
					RenderGraph::Fatal("snapshot cadence", fmt::format("CS_SNAPSHOT_CADENCE={} is not repeat:N or drop:N with N from 2 to 16", value));
				if (!RenderGraph::Engine::Native::Requested()) {
					logger::warn("[Snapshot] CS_SNAPSHOT_CADENCE={}: the cadence test mode runs in native mode only; this run renders once per update", value);
					return parsed;
				}
				parsed.mode = name == "repeat" ? Cadence::Mode::Repeat : Cadence::Mode::Drop;
				parsed.n = n;
				logger::info("[Snapshot] CS_SNAPSHOT_CADENCE={}: {}", value,
					parsed.mode == Cadence::Mode::Repeat ? fmt::format("the render half runs {} times for each game update", n) :
														   fmt::format("the render half runs for one game update in {}", n));
				return parsed;
			}();
			return cadence;
		}

		/** @brief MenuManager's freeze-background test (SE 79938, AE 82075), as Main::DrawFrame asks it after Begin. */
		bool FreezesBackground(RE::UI* a_ui)
		{
			using Fn = bool (*)(RE::UI*);
			static REL::Relocation<Fn> freezes{ REL::RelocationID(79938, 82075) };
			return freezes(a_ui);
		}

		MenuStates CaptureMenus()
		{
			MenuStates menus{};
			auto* ui = RE::UI::GetSingleton();
			if (!ui)
				return menus;
			menus.main = ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
			menus.loading = ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
			menus.map = ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
			menus.stats = ui->IsMenuOpen(RE::StatsMenu::MENU_NAME);
			menus.console = ui->IsMenuOpen(RE::Console::MENU_NAME);
			menus.freezesBackground = FreezesBackground(ui);
			menus.paused = ui->GameIsPaused();
			menus.open = ui->menuStack.size();
			return menus;
		}

		CameraView CaptureMainCamera()
		{
			CameraView view{};
			const auto* camera = RE::Main::WorldRootCamera();
			if (!camera)
				return view;
			view.valid = true;
			std::memcpy(view.worldToCam, camera->GetRuntimeData().worldToCam, sizeof(view.worldToCam));
			view.position = camera->world.translate;
			view.rotation = camera->world.rotate;
			view.frustum = camera->GetRuntimeData2().viewFrustum;
			view.port = camera->GetRuntimeData2().port;
			return view;
		}

		float DistanceSquared(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
			return x * x + y * y + z * z;
		}

		/** @brief The core fields, from game data, and the events they imply against the previous snapshot. */
		void FillCore(FrameSnapshot& a_snapshot, const FrameSnapshot* a_previous, Producer& a_game)
		{
			a_snapshot.menus = CaptureMenus();
			const auto& menus = a_snapshot.menus;
			a_snapshot.realDelta = RE::BSTimer::GetSingleton()->realTimeDelta;
			a_snapshot.gameDelta = menus.paused ? 0.0f : RE::GetSecondsSinceLastFrame();
			a_snapshot.gameTime = (a_previous ? a_previous->gameTime : 0.0) + a_snapshot.gameDelta;
			if (const auto* calendar = RE::Calendar::GetSingleton())
				a_snapshot.gameDaysPassed = calendar->GetDaysPassed();
			auto* player = RE::PlayerCharacter::GetSingleton();
			const bool has3D = player && player->Get3D2();
			const auto* cell = player ? player->GetParentCell() : nullptr;
			a_snapshot.interior = cell && cell->IsInteriorCell();
			a_snapshot.kind = menus.loading           ? FrameKind::Loading :
			                  menus.main || !has3D    ? FrameKind::MainMenu :
			                  menus.freezesBackground ? FrameKind::MenuOverWorld :
			                                            FrameKind::World;
			a_snapshot.loadEpoch = g_loadEpoch.load(std::memory_order_acquire);
			a_snapshot.settingsRevision = RenderGraph::Frame::Revision::Mark().takenVariables;

			auto& main = a_snapshot.cameras.current[static_cast<std::size_t>(ViewId::Main)];
			main = CaptureMainCamera();
			if (a_previous)
				a_snapshot.cameras.previous = a_previous->cameras.current;

			auto& events = a_snapshot.events;
			events.gameDelta = a_snapshot.gameDelta;
			if (g_loadStartedPending.exchange(false, std::memory_order_acq_rel)) {
				events.Add(Event::LoadStarted);
				++events.loadsStarted;
			}
			const bool loadEnded = g_loadEndedPending.exchange(false, std::memory_order_acq_rel);
			if (loadEnded)
				events.Add(Event::FirstAfterLoad);
			if (!a_previous)
				return;
			if (a_snapshot.kind != a_previous->kind)
				events.Add(Event::KindChanged);
			if (menus.open > a_previous->menus.open) {
				events.Add(Event::MenuOpened);
				events.menusOpened += menus.open - a_previous->menus.open;
			} else if (menus.open < a_previous->menus.open) {
				events.Add(Event::MenuClosed);
				events.menusClosed += a_previous->menus.open - menus.open;
			}
			if (menus.paused != a_previous->menus.paused)
				events.Add(menus.paused ? Event::Paused : Event::Resumed);
			if (a_snapshot.settingsRevision != a_previous->settingsRevision)
				events.Add(Event::SettingsChanged);
			const auto& before = a_previous->cameras.Current(ViewId::Main);
			if (main.valid && before.valid) {
				// The view direction is the rotation's first column (Skyrim cameras look down their local X).
				const float turn = main.rotation.entry[0][0] * before.rotation.entry[0][0] + main.rotation.entry[1][0] * before.rotation.entry[1][0] +
				                   main.rotation.entry[2][0] * before.rotation.entry[2][0];
				if (loadEnded || DistanceSquared(main.position, before.position) > kCutDistance * kCutDistance || turn < kCutTurnCos) {
					events.Add(Event::CameraCut);
					++events.cameraCuts;
				}
			}
			if (player && has3D) {
				const auto position = player->GetPosition();
				const bool moved = a_game.havePlayer && DistanceSquared(position, a_game.previousPlayer) > kTeleportDistance * kTeleportDistance;
				const bool cellChanged = a_game.havePlayer && cell != a_game.previousCell;
				if (!loadEnded && !events.Has(Event::LoadStarted) && (moved || cellChanged)) {
					events.Add(Event::Teleport);
					++events.teleports;
				}
				a_game.previousPlayer = position;
				a_game.previousCell = cell;
				a_game.havePlayer = true;
			}
		}

		/** @brief One line per load, with the counters since the previous line. */
		void LogStats(const FrameSnapshot& a_snapshot, const char* a_why)
		{
			auto& stats = Counters();
			const auto published = stats.published.exchange(0);
			const auto replaced = stats.replaced.exchange(0);
			const auto adopted = stats.adopted.exchange(0);
			const auto repeats = stats.repeats.exchange(0);
			const auto renders = stats.renders.exchange(0);
			const auto shared = stats.blocksShared.exchange(0);
			const auto made = stats.blocksMade.exchange(0);
			const auto ns = stats.captureNs.exchange(0);
			const auto folded = stats.mostFolded.exchange(0);
			logger::info(
				"[Snapshot] {} (load epoch {}, snapshot {}, {}): {} published, {} adopted, {} replaced unadopted (at most {} updates in one), "
				"{} render frames, {} repeats; blocks {} shared, {} new; capture {:.1f} us a snapshot",
				a_why, a_snapshot.loadEpoch, a_snapshot.id, FrameKindName(a_snapshot.kind), published, adopted, replaced, folded, renders, repeats, shared, made,
				published ? static_cast<double>(ns) / 1000.0 / static_cast<double>(published) : 0.0);
		}

		void Publish(std::shared_ptr<FrameSnapshot> a_fresh)
		{
			auto& stats = Counters();
			// What the render side did not adopt hands its events to the fresh one.
			if (const auto unadopted = Slot().Take()) {
				a_fresh->events.MergeOlder((*unadopted)->events);
				stats.replaced.fetch_add(1, std::memory_order_relaxed);
			}
			const auto folded = a_fresh->events.gameUpdates;
			for (auto most = stats.mostFolded.load(std::memory_order_relaxed); folded > most && !stats.mostFolded.compare_exchange_weak(most, folded);) {}
			std::shared_ptr<const FrameSnapshot> published = std::move(a_fresh);
			Game().previous = published;
			Slot().Post(std::make_unique<std::shared_ptr<const FrameSnapshot>>(std::move(published)));
			stats.published.fetch_add(1, std::memory_order_relaxed);
		}

		/**
		* @brief What a skipped render half must still do for the game: signal the main thread's render stages 5 to 9
		* (SetJobTaskEvent, SE 68165, AE 69514). NiCamera::CalculateAndDrawShadowCasterLights signals 5, Main::Draw 6 and 7, and
		* Main::DrawFrame 6 to 9. Frame jobs wait on them (FUN_1406d2e80 waits for 5 or 6, FUN_1406d3660 for 8 then 9), and
		* Main::Update's frame-job wait (SE 34554, AE 35409) waits for those jobs, so without them the update hangs.
		*/
		void SignalRenderStages()
		{
			using Fn = void (*)(std::uint32_t);
			static REL::Relocation<Fn> setEvent{ REL::RelocationID(68165, 69514) };
			for (std::uint32_t stage = 5; stage <= 9; ++stage)
				setEvent(stage);
		}

		/** @brief Main::Update's call of Main::DrawFrame (SE 35565 +0x5D2, AE 36564 +0xA97): publish, then the render half. */
		struct MainUpdate_DrawFrame
		{
			static void thunk(RE::Main* a_main)
			{
				Capture();
				const auto renders = RendersThisUpdate();
				for (std::uint32_t i = 0; i < renders; ++i)
					func(a_main);
				if (renders == 0)
					SignalRenderStages();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void RegisterCapture(CaptureFn a_capture)
	{
		Game().captures.push_back(a_capture);
	}

	void InstallHooks()
	{
		if (REL::Module::IsVR())
			return;
		stl::write_thunk_call<MainUpdate_DrawFrame>(REL::RelocationID(35565, 36564).address() + REL::Relocate(0x5D2, 0xA97));
		(void)CadenceMode();
		logger::info("[Snapshot] frame snapshots: published at Main::Update's call of Main::DrawFrame, adopted at Renderer::Begin");
	}

	void Capture()
	{
		ZoneScopedN("CS.Snapshot.Capture");
		// The live-read detector's capture scope wraps the builder: `const EngineReads::CaptureScope scope;` (Snapshot/EngineReads.h, snap0/detect).
		const auto start = std::chrono::steady_clock::now();
		auto& game = Game();
		const FrameSnapshot* previous = game.previous.get();
		auto fresh = std::make_shared<FrameSnapshot>();
		fresh->id = game.nextId++;
		FillCore(*fresh, previous, game);
		Builder builder(*fresh, previous);
		for (const auto capture : game.captures)
			capture(builder);
		auto& stats = Counters();
		stats.blocksShared.fetch_add(builder.shared, std::memory_order_relaxed);
		stats.blocksMade.fetch_add(builder.made, std::memory_order_relaxed);
		stats.captureNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
			std::memory_order_relaxed);
		const bool firstAfterLoad = fresh->events.Has(Event::FirstAfterLoad);
		const bool first = fresh->id == 1;
		Publish(fresh);
		if (first || firstAfterLoad)
			LogStats(*game.previous, first ? "first snapshot" : "load ended");
	}

	void Adopt()
	{
		auto& side = RenderSide();
		auto& stats = Counters();
		const auto now = std::chrono::steady_clock::now();
		side.render.delta = side.lastAdopt.time_since_epoch().count() ? std::chrono::duration<float>(now - side.lastAdopt).count() : 0.0f;
		side.lastAdopt = now;
		side.render.frame = RenderGraph::FrameClock::BeginFrame();
		stats.renders.fetch_add(1, std::memory_order_relaxed);
		if (auto taken = Slot().Take()) {
			side.current = std::move(*taken);
			side.render.repeat = false;
			side.render.rendersOfSnapshot = 1;
			g_currentId.store(side.current->id, std::memory_order_release);
			stats.adopted.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		side.render.repeat = side.current != nullptr;
		++side.render.rendersOfSnapshot;
		if (side.render.repeat)
			stats.repeats.fetch_add(1, std::memory_order_relaxed);
	}

	const std::shared_ptr<const FrameSnapshot>& Current()
	{
		return RenderSide().current;
	}

	std::uint64_t CurrentId()
	{
		return g_currentId.load(std::memory_order_acquire);
	}

	const RenderIdentity& Render()
	{
		return RenderSide().render;
	}

	void OnLoadingScreen(bool a_active)
	{
		if (a_active) {
			g_loadEpoch.fetch_add(1, std::memory_order_acq_rel);
			g_loadStartedPending.store(true, std::memory_order_release);
		} else
			g_loadEndedPending.store(true, std::memory_order_release);
	}

	std::uint32_t RendersThisUpdate()
	{
		const auto& cadence = CadenceMode();
		auto& game = Game();
		const auto update = game.updates++;
		switch (cadence.mode) {
		case Cadence::Mode::Repeat:
			return cadence.n;
		case Cadence::Mode::Drop:
			return update % cadence.n == 0 ? 1u : 0u;
		default:
			return 1;
		}
	}
}
