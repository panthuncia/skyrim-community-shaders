#include "Snapshot/Publication.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Snapshot/CoreFrame.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/FaceTint.h"
#include "Snapshot/FrameCamera.h"
#include "Snapshot/PrecipitationOcclusion.h"
#include "Snapshot/Retire.h"
#include "Snapshot/SideEffects.h"
#include "Snapshot/UiCommandStream.h"
#include "State.h"
#include "Testing/Testing.h"
#include "Utils/Setting.h"

#include "RenderGraph/Async/LatestSlot.h"
#include "RenderGraph/Engine/MainTick.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Native/RenderScene.h"
#include "RenderGraph/Engine/Native/RenderThread.h"
#include "RenderGraph/Engine/Native/WaterEffects.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/FrameAnchors.h"
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
		// The side effects log one line per this many game updates (native).
		constexpr std::uint64_t kSideEffectLineUpdates = 600;
		// The lifetimes line (snapshots alive, engine references held, retire lanes) also comes once per this many game updates.
		constexpr std::uint64_t kLifetimeLineUpdates = 1800;

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

		/** @brief The producers' state. The thread that holds the publish claim (g_publishing). */
		struct Producer
		{
			std::vector<CaptureFn> captures;
			std::vector<UnrenderedFn> unrendered;
			std::vector<UpdateEndFn> updateEnds;
			std::shared_ptr<const FrameSnapshot> previous;  // the last published, adopted or not
			std::shared_ptr<const FrameSnapshot> building;  // the newest game update's (Capture), never a frame-only one
			std::uint64_t nextId = 1;
			RE::FormID previousCell = 0;  // the player's cell at the previous update, by form id (an address can be reused)
			RE::NiPoint3 previousPlayer{};
			bool havePlayer = false;
			std::uint64_t updates = 0;  // game updates seen by the cadence mode
			std::uint64_t lineUpdates = 0;  // game updates since the start, for the periodic lines
		};

		/** @brief Counters since the last load's line. Atomic: the render side adds to them too. */
		struct Stats
		{
			std::atomic<std::uint64_t> published{ 0 }, frameOnly{ 0 }, replaced{ 0 }, adopted{ 0 }, repeats{ 0 }, renders{ 0 };
			std::atomic<std::uint64_t> blocksShared{ 0 }, blocksMade{ 0 }, captureNs{ 0 };
			std::atomic<std::uint32_t> mostFolded{ 0 };
		};

		/** @brief The render side's state. Render owner (the main thread in phase 0). */
		struct Consumer
		{
			std::shared_ptr<const FrameSnapshot> current;
			RenderIdentity render{};
			std::chrono::steady_clock::time_point lastAdopt{};
			std::uint64_t windowSizeServed = 0;  // the newest window size request this render side asked the renderer for
		};

		// The producer, the consumer and the slot hold snapshots, whose blocks may hold engine references. They are never destroyed:
		// nothing may release engine objects while the process is torn down.
		Producer& Game()
		{
			static auto* producer = new Producer;
			return *producer;
		}

		Consumer& RenderSide()
		{
			static auto* consumer = new Consumer;
			return *consumer;
		}

		Stats& Counters()
		{
			static Stats stats;
			return stats;
		}

		// Latest wins. The producer takes back what the consumer did not adopt before it posts the next (Publish), so the two only
		// ever exchange the pointer: one producer at a time (g_publishing), one consumer.
		RenderGraph::Frame::LatestSlot<std::shared_ptr<const FrameSnapshot>>& Slot()
		{
			static auto* slot = new RenderGraph::Frame::LatestSlot<std::shared_ptr<const FrameSnapshot>>;
			return *slot;
		}

		std::atomic<std::uint64_t> g_currentId{ 0 };
		std::atomic<std::uint64_t> g_lastGameId{ 0 };       // the newest game update's snapshot id published
		std::atomic<std::uint64_t> g_lastRenderFrame{ 0 };  // the render frame of the newest adoption
		// The window proc's newest resize request: the size (width in the low word) and its serial, the serial stored last.
		std::atomic<std::uint64_t> g_windowSize{ 0 };
		std::atomic<std::uint64_t> g_windowSizeSerial{ 0 };

		/** @brief The newest resize request, for the snapshot being built. A request racing in may pair an older serial with its size: harmless. */
		WindowSize PendingWindowSize()
		{
			WindowSize size{};
			size.serial = g_windowSizeSerial.load(std::memory_order_acquire);
			const auto packed = g_windowSize.load(std::memory_order_acquire);
			size.width = static_cast<std::uint32_t>(packed);
			size.height = static_cast<std::uint32_t>(packed >> 32);
			return size;
		}

		/** @brief The window size event against the previous snapshot. */
		void FillWindowSize(Events& a_events, const FrameSnapshot* a_previous)
		{
			a_events.windowSize = PendingWindowSize();
			if (a_events.windowSize.serial != (a_previous ? a_previous->events.windowSize.serial : 0))
				a_events.Add(Event::WindowResized);
		}
		// Set while a producer builds and publishes. The game update's publish and the frame-only ones never overlap (Publication.h),
		// so finding it set is fatal: a producer never waits.
		std::atomic_flag g_publishing;
		thread_local bool t_inGameUpdate = false;
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

		/** @brief The menu states. The freeze test walks the menu stack, so only the main thread asks it (a_freezeTest). */
		MenuStates CaptureMenus(bool a_freezeTest)
		{
			MenuStates menus{};
			auto* ui = CS_ENGINE_READ(UI, CapturePoint, RE::UI::GetSingleton());
			if (!ui)
				return menus;
			menus.main = ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
			menus.loading = ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
			menus.map = ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
			menus.stats = ui->IsMenuOpen(RE::StatsMenu::MENU_NAME);
			menus.console = ui->IsMenuOpen(RE::Console::MENU_NAME);
			menus.freezesBackground = a_freezeTest && FreezesBackground(ui);
			menus.paused = ui->GameIsPaused();
			menus.open = ui->menuStack.size();
			return menus;
		}

		/**
		* @brief A main-thread frame's kind, as Main's frame (SE 35556 / AE 36555) decides what it draws: a menu that freezes the
		* background first, then the world when the player has 3D (Get3D2, vtable +0x380), else the main menu.
		*/
		FrameKind MainThreadKind(const MenuStates& a_menus, bool a_has3D)
		{
			return a_menus.freezesBackground ? FrameKind::MenuOverWorld : a_has3D ? FrameKind::World : FrameKind::MainMenu;
		}

		/** @brief The kind and menu events (kind changed, opened, closed, paused, resumed) of a_snapshot against a_previous. */
		void MenuEvents(FrameSnapshot& a_snapshot, const FrameSnapshot& a_previous)
		{
			auto& events = a_snapshot.events;
			const auto& menus = a_snapshot.menus;
			if (a_snapshot.kind != a_previous.kind)
				events.Add(Event::KindChanged);
			if (menus.open > a_previous.menus.open) {
				events.Add(Event::MenuOpened);
				events.menusOpened += menus.open - a_previous.menus.open;
			} else if (menus.open < a_previous.menus.open) {
				events.Add(Event::MenuClosed);
				events.menusClosed += a_previous.menus.open - menus.open;
			}
			if (menus.paused != a_previous.menus.paused)
				events.Add(menus.paused ? Event::Paused : Event::Resumed);
		}

		/** @brief Holds the publish claim for one build and publish. A second producer at the same time is fatal. */
		struct PublishClaim
		{
			PublishClaim()
			{
				if (g_publishing.test_and_set(std::memory_order_acquire))
					RenderGraph::Fatal("snapshot publication", "two producers published a snapshot at the same time");
			}
			~PublishClaim() { g_publishing.clear(std::memory_order_release); }
			PublishClaim(const PublishClaim&) = delete;
			PublishClaim& operator=(const PublishClaim&) = delete;
		};

		/**
		* @brief The frame driver's engine values. The VL descriptor is made on its first read with the Low preset's default; the engine's
		* ImageSpace setup then sets the INI's quality, and a quality change (SE 100299) sets it again, so it is read every capture.
		*/
		EngineValues CaptureEngineValues()
		{
			EngineValues values{};
			values.taken = true;
			if (const auto* tes = CS_ENGINE_READ(TES, CapturePoint, RE::TES::GetSingleton()))
				values.interior = tes->interiorCell != nullptr;
			static REL::Relocation<const std::byte* (*)()> descriptor{ REL::RelocationID(100297, 107014) };
			if (const auto* vl = descriptor()) {
				values.volumetricLighting = true;
				std::memcpy(values.volumetricLightingSize.data(), vl + 0x30, sizeof(values.volumetricLightingSize));
			}
			return values;
		}

		CameraView CaptureMainCamera()
		{
			CameraView view{};
			if (const auto* camera = RE::Main::WorldRootCamera())
				FrameCamera::Capture(*camera, view);
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
			a_snapshot.menus = CaptureMenus(true);
			const auto& menus = a_snapshot.menus;
			a_snapshot.realDelta = CS_ENGINE_READ(Timer, CapturePoint, RE::BSTimer::GetSingleton())->realTimeDelta;
			a_snapshot.gameDelta = menus.paused ? 0.0f : CS_ENGINE_READ(Timer, CapturePoint, RE::GetSecondsSinceLastFrame());
			a_snapshot.gameTime = (a_previous ? a_previous->gameTime : 0.0) + a_snapshot.gameDelta;
			if (const auto* calendar = CS_ENGINE_READ(Calendar, CapturePoint, RE::Calendar::GetSingleton()))
				a_snapshot.gameDaysPassed = calendar->GetDaysPassed();
			auto* player = CS_ENGINE_READ(Player, CapturePoint, RE::PlayerCharacter::GetSingleton());
			const bool has3D = player && player->Get3D2();
			const auto* cell = player ? player->GetParentCell() : nullptr;
			const RE::FormID cellId = cell ? cell->GetFormID() : 0;
			a_snapshot.interior = cell && cell->IsInteriorCell();
			a_snapshot.worldLoaded = has3D && cell;
			// Taken before the water update (WaterEffects::Capture), as RenderPlayerView takes it before SetupSunAndSky: in first
			// person the reflection cube's update hides the root and leaves it to the first-person block to show again.
			const auto* firstPersonRoot = player ? player->Get3D1(true) : nullptr;
			a_snapshot.firstPersonShown = firstPersonRoot && !firstPersonRoot->GetFlags().any(RE::NiAVObject::Flag::kHidden);
			a_snapshot.kind = MainThreadKind(menus, has3D);
			a_snapshot.loadEpoch = g_loadEpoch.load(std::memory_order_acquire);
			// Game side: the settings' raise count when the snapshot is built, not the variables the last render Begin took.
			a_snapshot.settingsRevision = CS::SettingRegistry::Serial();
			a_snapshot.engine = CaptureEngineValues();

			// Native: Main::SetupSunAndSky's game half sets the camera's FOV and moves the sky with it, so it runs before the camera is taken.
			if (a_snapshot.kind == FrameKind::World)
				RenderGraph::Engine::Native::WaterEffects::SetupSunAndSky();
			auto& main = a_snapshot.cameras.current[static_cast<std::size_t>(ViewId::Main)];
			main = CaptureMainCamera();
			if (a_previous)
				a_snapshot.cameras.previous = a_previous->cameras.current;

			auto& events = a_snapshot.events;
			events.gameDelta = a_snapshot.gameDelta;
			FillWindowSize(events, a_previous);
			if (g_loadStartedPending.exchange(false, std::memory_order_acq_rel)) {
				events.Add(Event::LoadStarted);
				++events.loadsStarted;
			}
			const bool loadEnded = g_loadEndedPending.exchange(false, std::memory_order_acq_rel);
			if (loadEnded)
				events.Add(Event::FirstAfterLoad);
			if (!a_previous)
				return;
			MenuEvents(a_snapshot, *a_previous);
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
				const bool cellChanged = a_game.havePlayer && cellId != a_game.previousCell;
				if (!loadEnded && !events.Has(Event::LoadStarted) && (moved || cellChanged)) {
					events.Add(Event::Teleport);
					++events.teleports;
				}
				a_game.previousPlayer = position;
				a_game.previousCell = cellId;
				a_game.havePlayer = true;
			}
		}

		/** @brief One line per load, with the counters since the previous line. */
		void LogStats(const FrameSnapshot& a_snapshot, const char* a_why)
		{
			auto& stats = Counters();
			const auto published = stats.published.exchange(0);
			const auto frameOnly = stats.frameOnly.exchange(0);
			const auto replaced = stats.replaced.exchange(0);
			const auto adopted = stats.adopted.exchange(0);
			const auto repeats = stats.repeats.exchange(0);
			const auto renders = stats.renders.exchange(0);
			const auto shared = stats.blocksShared.exchange(0);
			const auto made = stats.blocksMade.exchange(0);
			const auto ns = stats.captureNs.exchange(0);
			const auto folded = stats.mostFolded.exchange(0);
			logger::info(
				"[Snapshot] {} (load epoch {}, snapshot {}, {}): {} published ({} frame-only: loading screen, movie or outside an update), {} adopted, "
				"{} replaced unadopted (at most {} updates in one), {} render frames, {} repeats; blocks {} shared, {} new; capture {:.1f} us a game snapshot",
				a_why, a_snapshot.loadEpoch, a_snapshot.id, FrameKindName(a_snapshot.kind), published, frameOnly, adopted, replaced, folded, renders, repeats,
				shared, made, published > frameOnly ? static_cast<double>(ns) / 1000.0 / static_cast<double>(published - frameOnly) : 0.0);
			logger::info("{}", Retire::TakeStatsLine());
		}

		void Publish(std::shared_ptr<FrameSnapshot> a_fresh)
		{
			auto& stats = Counters();
			// What the render side did not adopt hands its events to the fresh one.
			// It was never rendered, so it retires at once (on the main thread; a serving-thread producer hands it to the main thread).
			if (const auto unadopted = Slot().Take()) {
				a_fresh->events.MergeOlder((*unadopted)->events);
				stats.replaced.fetch_add(1, std::memory_order_relaxed);
				Retire::LetGo(std::move(*unadopted));
			}
			const auto folded = a_fresh->events.gameUpdates;
			for (auto most = stats.mostFolded.load(std::memory_order_relaxed); folded > most && !stats.mostFolded.compare_exchange_weak(most, folded);) {}
			std::shared_ptr<const FrameSnapshot> published = std::move(a_fresh);
			Retire::LetGo(std::exchange(Game().previous, published));
			Slot().Post(std::make_unique<std::shared_ptr<const FrameSnapshot>>(std::move(published)));
			stats.published.fetch_add(1, std::memory_order_relaxed);
		}

		std::shared_ptr<FrameSnapshot> Build();
		void LogFirst(const FrameSnapshot& a_snapshot);

		/**
		* @brief Main::Update's call of Main::DrawFrame (SE 35565 +0x5D2, AE 36564 +0xA97): build the snapshot, then either CS's game frame
		* and the render half from its payload (a routed update, native) or the publish and Main::DrawFrame.
		*/
		struct MainUpdate_DrawFrame
		{
			static void thunk(RE::Main* a_main)
			{
				auto fresh = Build();
				// The test steps count game frames: one per update.
				Testing::OnGameUpdate();
				// Native: the render half's game-visible side effects come from the game side (Snapshot/SideEffects.h).
				SideEffects::AfterCapture(*fresh);
				const bool outer = std::exchange(t_inGameUpdate, true);
				const auto renders = RendersThisUpdate();
				namespace Routed = RenderGraph::Engine::Native::RenderThread;
				if (Routed::Routes(*fresh)) {
					RoutedUpdate(a_main, std::move(fresh), renders);
					t_inGameUpdate = outer;
					EndUpdate();
					return;
				}
				Publish(fresh);
				LogFirst(*fresh);
				// A load that starts inside a render (a console coc) ends the update's repeats: the engine's kept scene lists still name
				// the cells it unloaded, and only the next update builds them again.
				const auto epoch = g_loadEpoch.load(std::memory_order_acquire);
				std::uint32_t rendered = 0;
				{
					const SideEffects::RenderHalfScope scope;
					for (; rendered < renders && g_loadEpoch.load(std::memory_order_acquire) == epoch; ++rendered) {
						func(a_main);
					}
				}
				// The update's game side ends here: the job stages below start the engine's next update jobs, so no DCLF worker may be
				// in engine memory past this point. Guest closed at its Present already. Phase 7 closes at the publish.
				DCLF::EngineReadWindow::Close();
				SideEffects::AfterRenderHalf(rendered);
				// The cadence rendered nothing for this update: its game halves run all the same.
				if (!rendered)
					for (const auto fn : Game().unrendered)
						fn(*Game().previous);
				// The update's game-side end, rendered or not.
				for (const auto fn : Game().updateEnds)
					fn(*Game().previous);
				t_inGameUpdate = outer;
				EndUpdate();
			}

			/**
			* @brief A routed update: CS's game frame in place of Main::DrawFrame, the update's game-side end, the game side of the Present
			* point, then the publish with the frame's payload and its renders.
			*/
			static void RoutedUpdate(RE::Main* a_main, std::shared_ptr<FrameSnapshot> a_fresh, std::uint32_t a_renders)
			{
				namespace Routed = RenderGraph::Engine::Native::RenderThread;
				a_fresh->nativeFrame = Routed::GameFrame(a_main, *a_fresh);
				// The update's game side ends here: the job stages below start the engine's next update jobs, so no DCLF worker may be in
				// engine memory past this point.
				DCLF::EngineReadWindow::Close();
				SideEffects::AfterRenderHalf(a_renders);
				for (const auto fn : Game().updateEnds)
					fn(*a_fresh);
				// The steps and milestones run at the update's publish (they ran at its Present before).
				Testing::OnGamePresent();
				Publish(a_fresh);
				LogFirst(*a_fresh);
				// The render half from the payload alone. A load that starts in a render ends the update's repeats.
				const auto epoch = g_loadEpoch.load(std::memory_order_acquire);
				for (std::uint32_t rendered = 0; rendered < a_renders && g_loadEpoch.load(std::memory_order_acquire) == epoch; ++rendered)
					Routed::RenderHalf();
			}

			/** @brief The update's end: the update-rate cap's wait and the periodic lines. */
			static void EndUpdate()
			{
				// Phase 7's "publish, then wait for the next update slot". Off unless CS_UPDATE_RATE_CAP=1.
				PaceGameUpdate();
				const auto updates = ++Game().lineUpdates;
				if (updates % kSideEffectLineUpdates == 0) {
					if (const auto line = SideEffects::TakeStatsLine(); !line.empty())
						logger::info("{}", line);
					if (const auto line = RenderGraph::Engine::Native::WaterEffects::TakeStatsLine(); !line.empty())
						logger::info("{}", line);
					if (const auto line = RenderGraph::Engine::Native::RenderThread::TakeStatsLine(); !line.empty())
						logger::info("{}", line);
				}
				if (updates % kLifetimeLineUpdates == 0)
					logger::info("{}", Retire::TakeStatsLine());
				// Main::Update's game logic follows. A dropped update's game half opened the window again.
				DCLF::EngineReadWindow::Close();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void RegisterCapture(CaptureFn a_capture)
	{
		Game().captures.push_back(a_capture);
	}

	void RegisterUnrenderedUpdate(UnrenderedFn a_fn)
	{
		Game().unrendered.push_back(a_fn);
	}

	void RegisterUpdateEnd(UpdateEndFn a_fn)
	{
		Game().updateEnds.push_back(a_fn);
	}

	void InstallHooks()
	{
		if (REL::Module::IsVR())
			return;
		stl::write_thunk_call<MainUpdate_DrawFrame>(REL::RelocationID(35565, 36564).address() + REL::Relocate(0x5D2, 0xA97));
		// The engine lane drains once the update's frame jobs are joined, right after Main::DrawFrame: they run beside it (the cell
		// animations, Havok), and an engine object's last release there would race them. The engine-read window is closed by then.
		if (!RenderGraph::Engine::MainTick::Add("the snapshot retire queue's engine lane", [] { Retire::DrainEngine(); }))
			RenderGraph::Fatal("snapshot lifetimes", "the frame-job wait is not hooked, so engine references would never be let go");
		SideEffects::InstallHooks();
		FaceTint::InstallHooks();
		RegisterCapture(&PrecipitationOcclusion::Capture);
		RenderGraph::Engine::Native::WaterEffects::InstallHooks();
		RenderGraph::Engine::Native::Scene::InstallHooks();
		(void)CadenceMode();
		logger::info("[Snapshot] frame snapshots: published at Main::Update's call of Main::DrawFrame, adopted at Renderer::Begin");
	}

	namespace
	{
		/** @brief Builds the next game update's snapshot, unpublished: Building() returns it from here on. */
		std::shared_ptr<FrameSnapshot> Build()
		{
			ZoneScopedN("CS.Snapshot.Capture");
			const PublishClaim claim;
			// Engine reads from here on are captures, not live reads.
			const EngineReads::CaptureScope scope;
			const auto start = std::chrono::steady_clock::now();
			auto& game = Game();
			const FrameSnapshot* previous = game.previous.get();
			auto fresh = std::make_shared<FrameSnapshot>();
			fresh->id = game.nextId++;
			FillCore(*fresh, previous, game);
			// Native: the update's UI stream, set once its DrawInterface finished it (Snapshot/UiCommandStream.h).
			static const bool native = RenderGraph::Engine::Native::Requested();
			if (native)
				fresh->ui = std::make_shared<const UiCommandStream>();
			Builder builder(*fresh, previous);
			// The core block first: feature captures may read it through Builder::Built().
			CaptureCore(builder);
			// Native: RenderWaterEffects' game half, before the feature captures read what the water update sets (the underwater state and
			// effects, the water shader's globals).
			RenderGraph::Engine::Native::WaterEffects::Capture(*fresh);
			for (const auto capture : game.captures)
				capture(builder);
			auto& stats = Counters();
			stats.blocksShared.fetch_add(builder.shared, std::memory_order_relaxed);
			stats.blocksMade.fetch_add(builder.made, std::memory_order_relaxed);
			stats.captureNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
				std::memory_order_relaxed);
			g_lastGameId.store(fresh->id, std::memory_order_release);
			Retire::LetGo(std::exchange(game.building, fresh));
			return fresh;
		}

		void LogFirst(const FrameSnapshot& a_snapshot)
		{
			if (a_snapshot.id == 1 || a_snapshot.events.Has(Event::FirstAfterLoad))
				LogStats(a_snapshot, a_snapshot.id == 1 ? "first snapshot" : "load ended");
		}
	}

	void Capture()
	{
		auto fresh = Build();
		Publish(fresh);
		LogFirst(*fresh);
	}

	void CaptureFrame(FrameSource a_source)
	{
		ZoneScopedN("CS.Snapshot.CaptureFrame");
		const PublishClaim claim;
		const EngineReads::CaptureScope scope;
		auto& game = Game();
		const FrameSnapshot* previous = game.previous.get();
		// The last snapshot's world data, shared by pointer: blocks, cameras, scene, game time, settings revision.
		auto fresh = previous ? std::make_shared<FrameSnapshot>(*previous) : std::make_shared<FrameSnapshot>();
		fresh->sub = previous ? previous->sub + 1 : 1;
		fresh->gameDelta = 0.0f;
		fresh->realDelta = 0.0f;
		fresh->cameras.previous = fresh->cameras.current;
		fresh->events = {};
		fresh->events.gameUpdates = 0;
		fresh->loadEpoch = g_loadEpoch.load(std::memory_order_acquire);
		fresh->menus = CaptureMenus(a_source == FrameSource::MainThread);
		switch (a_source) {
		case FrameSource::MainThread:
			{
				auto* player = CS_ENGINE_READ(Player, CapturePoint, RE::PlayerCharacter::GetSingleton());
				const bool has3D = player && player->Get3D2();
				fresh->worldLoaded = has3D && player->GetParentCell();
				fresh->kind = MainThreadKind(fresh->menus, has3D);
				fresh->engine = CaptureEngineValues();
				break;
			}
		case FrameSource::LoadingScreen:
			fresh->kind = FrameKind::Loading;
			break;
		case FrameSource::Movie:
			fresh->kind = FrameKind::Movie;
			break;
		}
		// Native: the frame's UI stream, set once its DrawInterface finished it. Never the copied snapshot's holder.
		static const bool native = RenderGraph::Engine::Native::Requested();
		fresh->ui = native ? std::make_shared<const UiCommandStream>() : nullptr;
		fresh->nativeFrame = nullptr;
		if (g_loadStartedPending.exchange(false, std::memory_order_acq_rel)) {
			fresh->events.Add(Event::LoadStarted);
			++fresh->events.loadsStarted;
		}
		FillWindowSize(fresh->events, previous);
		if (previous)
			MenuEvents(*fresh, *previous);
		Counters().frameOnly.fetch_add(1, std::memory_order_relaxed);
		Publish(std::move(fresh));
	}

	void Adopt()
	{
		// Begin runs inside Renderer::Lock on every thread, so adoptions never overlap. The render owner reads what this sets.
		auto& side = RenderSide();
		auto& stats = Counters();
		const auto now = std::chrono::steady_clock::now();
		const auto lastRendered = side.render.frame;
		side.render.delta = side.lastAdopt.time_since_epoch().count() ? std::chrono::duration<float>(now - side.lastAdopt).count() : 0.0f;
		side.lastAdopt = now;
		side.render.frame = RenderGraph::FrameClock::BeginFrame();
		g_lastRenderFrame.store(side.render.frame, std::memory_order_release);
		stats.renders.fetch_add(1, std::memory_order_relaxed);
		Retire::DrainRender();
		if (auto taken = Slot().Take()) {
			// The snapshot rendered last never renders again: it retires once the GPU finished its last frame.
			Retire::RenderedSnapshot(std::exchange(side.current, std::move(*taken)), lastRendered);
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

	const std::shared_ptr<const FrameSnapshot>& Building()
	{
		return Game().building;
	}

	const RenderIdentity& Render()
	{
		return RenderSide().render;
	}

	float VisualDelta()
	{
		const auto& side = RenderSide();
		return side.current && !side.current->menus.paused ? side.render.delta : 0.0f;
	}

	bool InGameUpdate()
	{
		return t_inGameUpdate;
	}

	float GameStep()
	{
		const auto& side = RenderSide();
		return side.current && !side.render.repeat ? side.current->events.gameDelta : 0.0f;
	}

	bool HistoryBreak()
	{
		static const bool native = RenderGraph::Engine::Native::Requested();
		const auto& side = RenderSide();
		if (!native || !side.current || side.render.repeat)
			return false;
		const auto& events = side.current->events;
		return events.Has(Event::CameraCut) || events.Has(Event::Teleport) || events.Has(Event::LoadStarted) || events.Has(Event::FirstAfterLoad);
	}

	void NoteWindowResize(std::uint32_t a_width, std::uint32_t a_height)
	{
		g_windowSize.store(std::uint64_t(a_width) | (std::uint64_t(a_height) << 32), std::memory_order_release);
		g_windowSizeSerial.fetch_add(1, std::memory_order_acq_rel);
	}

	bool WindowResizeToApply(std::uint32_t& a_width, std::uint32_t& a_height)
	{
		auto& side = RenderSide();
		if (!side.current)
			return false;
		const auto& size = side.current->events.windowSize;
		if (size.serial <= side.windowSizeServed)
			return false;
		side.windowSizeServed = size.serial;
		a_width = size.width;
		a_height = size.height;
		return true;
	}

	std::uint64_t LastGameId()
	{
		return g_lastGameId.load(std::memory_order_acquire);
	}

	std::uint64_t LastRenderFrame()
	{
		return g_lastRenderFrame.load(std::memory_order_acquire);
	}

	void PaceGameUpdate()
	{
		static const bool active = ::Switches::On("CS_UPDATE_RATE_CAP");
		if (!active)
			return;
		using clock = std::chrono::steady_clock;
		static clock::time_point next{};
		const auto cap = globals::state->updateRateCap.load(std::memory_order_relaxed);
		const auto now = clock::now();
		if (!cap) {
			next = {};
			return;
		}
		const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / static_cast<double>(cap)));
		// The first update, or one that fell a whole slot behind, starts the slots again rather than catching up in a burst.
		if (next == clock::time_point{} || now > next + period)
			next = now;
		if (now < next) {
			ZoneScopedN("CS.Snapshot.UpdateRateWait");
			static const HANDLE timer = [] {
				const HANDLE made = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
				if (!made)
					RenderGraph::Fatal("snapshot publication", fmt::format("the update-rate cap's timer could not be made (error {})", ::GetLastError()));
				return made;
			}();
			// Relative due time in 100 ns units.
			LARGE_INTEGER due{};
			due.QuadPart = -(std::max<LONGLONG>)(1, std::chrono::duration_cast<std::chrono::nanoseconds>(next - now).count() / 100);
			if (!::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) || ::WaitForSingleObject(timer, INFINITE) != WAIT_OBJECT_0)
				RenderGraph::Fatal("snapshot publication", fmt::format("the update-rate cap's wait failed (error {})", ::GetLastError()));
		}
		next += period;
	}

	void OnLoadingScreen(bool a_active)
	{
		if (a_active) {
			g_loadEpoch.fetch_add(1, std::memory_order_acq_rel);
			g_loadStartedPending.store(true, std::memory_order_release);
		} else
			g_loadEndedPending.store(true, std::memory_order_release);
	}

	std::uint64_t LoadEpoch()
	{
		return g_loadEpoch.load(std::memory_order_acquire);
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
