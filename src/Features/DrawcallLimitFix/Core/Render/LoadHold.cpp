// LoadHold: the hold after every load, until DCLF's claims settle (LoadHold.h).

#include "Features/DrawcallLimitFix/Core/Render/LoadHold.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Publication.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "Utils/SwitchFile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace DCLF::LoadHold
{
	namespace
	{
		enum class HoldState : std::uint8_t
		{
			Idle,       // no hold
			Closing,    // the menu's close is kept; the loading screen's last iterations still present
			Holding,    // the world's frames run off the screen
			Releasing,  // the close posted; the menu is open until the UI takes it
		};

		// Two settled frames in a row end it. The world's first frame's culls are joined by the pass after the one that walked the load's
		// cells, so a publication committed two frames after the first one has seen them; a load that changed nothing publishes nothing
		// new, so after a few frames the installed one counts as well.
		constexpr std::uint32_t kSettledFrames = 2;
		constexpr std::uint32_t kJoinFrames = 2;
		constexpr std::uint32_t kQuietFrames = 8;
		// No progress for this long (what waits did not shrink), or a hold this long, ends it with what still waits logged.
		constexpr double kStallMs = 5000.0;
		constexpr double kCapMs = 60000.0;

		std::atomic<HoldState> state{ HoldState::Idle };
		std::atomic<bool> withMenu{ false };
		// Render thread: a re-entry ended at a stall or the cap; none starts again until a frame without the condition (reentryThisFrame).
		bool reentryBlocked = false;
		bool reentryThisFrame = false;

		// One hold's counts. The main thread writes them in Begin, before it publishes the state; the render thread reads them after.
		struct Hold
		{
			std::chrono::steady_clock::time_point start, progress;
			std::uint32_t firstFrame = 0, lastScene = 0, frames = 0, settled = 0;
			std::uint32_t held = 0, heldSun = 0, heldOcclusion = 0, heldShadow = 0;
			const char* reentry = nullptr;  // a re-entry's condition (BeginReentry), null for a load's hold
			std::uint32_t best = ~0u;  // the least that waited so far: less than it is progress
			// The last frame's claims, by object and publication frame (the pool reuses the objects).
			const void* claims = nullptr;
			std::uint32_t claimsFrame = 0;
			std::uint64_t withheld = 0;  // passes of DCLF's classes withheld (PassCapture::HeldByLoad), as the frames' drains counted them
			bool heldThisFrame = false;
			// What the hold waited for: the world frames each condition was not met in, and the most objects that waited by reason.
			std::uint32_t waitedPublication = 0, waitedObjects = 0, waitedClaims = 0, waitedFamilies = 0, waitedViews = 0;
			std::array<std::uint32_t, kSetWaitReasons> peakWaiting{};
			std::uint64_t builds = 0;  // the pipeline lane's finished builds (ready or failed), main and shadow: a build is progress
		};
		Hold hold;

		// After a hold: what of DCLF's classes is still withheld because it is not ready, or handed to the engine, logged at most every
		// kAfterLogMs. Render thread.
		constexpr double kAfterLogMs = 10000.0;
		struct After
		{
			std::chrono::steady_clock::time_point since{};
			std::uint64_t pending = 0, baseMips = 0, kept = 0;
			std::uint32_t frames = 0, familyFrames = 0;
			std::array<std::uint32_t, kSetWaitReasons> peakWaiting{};
			bool logged = false;  // the last window logged a line: the next one logs even when quiet, to say so
		};
		After after;

		double MillisecondsSince(std::chrono::steady_clock::time_point a_start)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
		}

		bool MenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		}

		/** @brief Main thread, the coordinator's close: a hold begins while DCLF runs. True when the close is kept (a menu to keep open). */
		bool Begin()
		{
			if (!ShellState::Running())
				return false;
			const bool menu = MenuOpen();
			hold = {};
			hold.start = hold.progress = std::chrono::steady_clock::now();
			withMenu.store(menu, std::memory_order_relaxed);
			// Without a menu the main thread renders next: the hold starts now, with the presents held.
			state.store(menu ? HoldState::Closing : HoldState::Holding, std::memory_order_release);
			logger::info("[DCLF] load hold: started ({})", menu ? "the loading menu kept open" : "no loading menu: the presents held, the last frame stays on screen");
			return menu;
		}

		/** @brief Main thread, a loading screen's start: a load inside a hold ends it (the new load posts its own close). */
		void OnLoadBegin()
		{
			if (state.exchange(HoldState::Idle, std::memory_order_acq_rel) != HoldState::Idle)
				logger::info("[DCLF] load hold: a load began during the hold, which ends ({} frames, {:.0f} ms)", hold.frames, MillisecondsSince(hold.start));
		}

		/** @brief The objects still waiting by reason (all of them, the waited ones marked), for the log. */
		std::string WaitingText(const ScenePublication* a_publication)
		{
			std::string text;
			if (a_publication)
				for (std::size_t why = 0; why < kSetWaitReasons; ++why)
					if (const auto count = a_publication->waiting[why])
						text += fmt::format("{}{} {}{}", text.empty() ? "" : ", ", kSetWaitReasonNames[why], count, (kSetTransientWaits >> why) & 1u ? "" : " (not waited for)");
			return text.empty() ? "none" : text;
		}

		/** @brief What the hold waited for, as frames per condition and the most objects that waited by reason (for the log). */
		std::string WaitedText()
		{
			std::string peaks;
			for (std::size_t why = 0; why < kSetWaitReasons; ++why)
				if (hold.peakWaiting[why])
					peaks += fmt::format("{}{} {}", peaks.empty() ? "" : ", ", kSetWaitReasonNames[why], hold.peakWaiting[why]);
			return fmt::format(
				"the load's publication {} frames, waiting objects {} frames (at most: {}), claims changing {} frames, draw families {} frames, a view not "
				"covered {} frames",
				hold.waitedPublication, hold.waitedObjects, peaks.empty() ? "none" : peaks, hold.waitedClaims, hold.waitedFamilies, hold.waitedViews);
		}

		/** @brief Render thread: the hold ends (a_why: what ended it; a_warn: a stall or the cap, logged with what still waits). */
		void Release(const char* a_why, bool a_warn)
		{
			const double ms = MillisecondsSince(hold.start);
			const bool menu = withMenu.load(std::memory_order_relaxed);
			const char* mode = menu ? "the loading menu kept open" : hold.reentry ? "a re-entry, the presents held" : "the presents held (no loading menu)";
			if (hold.reentry && a_warn)
				reentryBlocked = true;
			if (menu && MenuOpen()) {
				// The coordinator's close, posted now. The menu closes at the UI's next pass over its messages.
				state.store(HoldState::Releasing, std::memory_order_release);
				if (auto* queue = RE::UIMessageQueue::GetSingleton())
					queue->AddMessage(RE::BSFixedString(RE::LoadingMenu::MENU_NAME), RE::UI_MESSAGE_TYPE::kHide, nullptr);
			} else {
				state.store(HoldState::Idle, std::memory_order_release);
			}
			// The window after the hold starts here.
			after = {};
			after.since = std::chrono::steady_clock::now();
			if (!a_warn) {
				logger::info(
					"[DCLF] load hold: {} frames, {:.0f} ms ({}), {}; {} passes of DCLF's classes withheld; {} frames with a view DCLF did not cover yet "
					"(its engine cull skipped: {} sun, {} occlusion maps; {} frames with no shadow view DCLF's{}{}); waited for: {}",
					hold.frames, ms, mode, a_why, hold.withheld, hold.held, hold.heldSun, hold.heldOcclusion, hold.heldShadow, hold.reentry ? ": " : "",
					hold.reentry ? hold.reentry : "", WaitedText());
				return;
			}
			const auto& view = Scene::FrameView::Get();
			const auto* publication = view.InstalledPublication();
			const auto claims = PassCapture::Get().CurrentSet();
			logger::warn(
				"[DCLF] load hold: released after {} frames, {:.0f} ms ({}), {}; {} passes of DCLF's classes withheld. Still waiting: objects by reason: {}; "
				"draw families {}; claims {}; the installed publication {} (built {}, committed at frame {}, the world's first frame {}); a view held this frame: "
				"{}; waited for: {}. What is not ready stays withheld until it is",
				hold.frames, ms, mode, a_why, hold.withheld, WaitingText(publication), IndirectDraws::Get().DrawFamiliesWaiting(),
				claims ? (hold.settled ? "installed, unchanged" : "installed, changing") : "not installed", publication ? publication->sequence : 0ull,
				publication && publication->built, publication ? publication->commitFrame : 0u, hold.firstFrame, hold.heldThisFrame ? "yes" : "no", WaitedText());
		}

		/** @brief Render thread, one world frame of a hold: whether it is settled (LoadHold.h); a_waits: what still waits, as one count. */
		bool Settled(std::uint32_t& a_waits)
		{
			const auto* publication = Scene::FrameView::Get().InstalledPublication();
			const bool fresh = publication && publication->built &&
			                   (publication->commitFrame >= hold.firstFrame + kJoinFrames || hold.frames >= kQuietFrames);
			const std::uint32_t transient = publication ? publication->TransientWaiting() : 0u;
			// The coordinator's signal (ScenePublication::settled: built, nothing waiting on a transient reason).
			const bool objectsSettled = publication && publication->settled;
			const auto claims = PassCapture::Get().CurrentSet();
			const bool stable = claims && claims.get() == hold.claims && claims->frame == hold.claimsFrame;
			hold.claims = claims.get();
			hold.claimsFrame = claims ? claims->frame : 0u;
			const std::uint32_t families = IndirectDraws::Get().DrawFamiliesWaiting();
			a_waits = transient + families + (hold.heldThisFrame ? 1u : 0u);
			hold.waitedPublication += fresh ? 0u : 1u;
			hold.waitedObjects += fresh && !objectsSettled ? 1u : 0u;
			hold.waitedClaims += stable ? 0u : 1u;
			hold.waitedFamilies += families ? 1u : 0u;
			hold.waitedViews += hold.heldThisFrame ? 1u : 0u;
			if (fresh)
				for (std::size_t why = 0; why < kSetWaitReasons; ++why)
					if ((kSetTransientWaits >> why) & 1u)
						hold.peakWaiting[why] = (std::max)(hold.peakWaiting[why], publication->waiting[why]);
			// Not fresh yet: the load's objects are not in the tables, so nothing counts as progress or stall.
			if (!fresh)
				a_waits = ~0u;
			return fresh && objectsSettled && stable && !families && !hold.heldThisFrame;
		}
	}

	namespace
	{
		/**
		 * @brief Render thread, the end of a frame DCLF drew outside a load: what of DCLF's classes was withheld because it was not ready
		 * (a waiting object, base mips, a waiting draw family) or handed to the engine, logged by reason at most every kAfterLogMs.
		 */
		void WatchAfterHold()
		{
			const auto& stats = PassCapture::Get().GetStats();
			if (after.since == std::chrono::steady_clock::time_point{})
				after.since = std::chrono::steady_clock::now();
			after.pending += stats.pendingWithheld;
			after.baseMips += stats.baseMipsWithheld;
			after.kept += stats.dclfKept;
			++after.frames;
			after.familyFrames += IndirectDraws::Get().DrawFamiliesWaiting() ? 1u : 0u;
			if (const auto* publication = Scene::FrameView::Get().InstalledPublication())
				for (std::size_t why = 0; why < kSetWaitReasons; ++why)
					after.peakWaiting[why] = (std::max)(after.peakWaiting[why], publication->waiting[why]);
			const double ms = MillisecondsSince(after.since);
			if (ms < kAfterLogMs)
				return;
			const bool any = after.pending || after.baseMips || after.kept || after.familyFrames;
			if (any || after.logged) {
				std::string peaks;
				for (std::size_t why = 0; why < kSetWaitReasons; ++why)
					if (after.peakWaiting[why])
						peaks += fmt::format("{}{} {}", peaks.empty() ? "" : ", ", kSetWaitReasonNames[why], after.peakWaiting[why]);
				logger::info(
					"[DCLF] not ready, withheld: {} passes of waiting objects (objects waiting, at most: {}), {} passes waiting for base mips, {} of {} "
					"frames with a draw family waiting; handed to the engine: {} passes of DCLF's classes (refused, or not bound yet); last {:.0f} ms",
					after.pending, peaks.empty() ? "none" : peaks, after.baseMips, after.familyFrames, after.frames, after.kept, ms);
			}
			// The shadow views DCLF does not cover that passes went to: whose they are.
			for (const void* accumulator : PassCapture::Get().TakeUncoveredAccumulators())
				logger::info("[DCLF] handed to the engine: a shadow view DCLF does not cover, accumulator {}", ShadowViews::Get().DescribeAccumulator(accumulator));
			// The geometries behind the passes handed to the engine, most first: what DCLF does not draw of its classes, and where.
			for (const auto& sample : PassCapture::Get().TakeKeptSamples(after.kept ? 12 : 0))
				logger::info(
					"[DCLF] handed to the engine: '{}' ({}{}{}), {} passes into {} ({}), shader {}; set phases {:#x}, waiting phases {:#x}", sample.name,
					sample.owner.empty() ? "no reference" : sample.owner, sample.hidden ? ", hidden" : "", sample.skinned ? ", skinned" : "", sample.passes,
					sample.where,
					sample.renderMode != ~0u ? fmt::format("accumulator render mode {:#x}", sample.renderMode) :
					                           std::string("render mode unknown: not the shadow modes' registration"),
					sample.shader, sample.phases, sample.pending);
			after = {};
			after.logged = any;
			after.since = std::chrono::steady_clock::now();
		}
	}

	namespace
	{
		// The map census's window: a line set every this many frames while the map menu is open, and one at its close.
		constexpr std::uint32_t kMapCensusFrames = 120;
		constexpr std::size_t kMapCensusClasses = 24;

		void LogMapCensus(std::uint32_t a_frames, const char* a_why)
		{
			const auto classes = PassCapture::Get().TakeMapCensus();
			std::size_t kept = 0, waiting = 0;
			const std::size_t members = PassCapture::Get().MapKeptMembersNow(kept, waiting);
			logger::info("[DCLF] map census ({}, {} mode): {} frames, {} classes of passes registered (main: the main views' renderers; other: any other, "
						 "the Z-prepass's registration among them); of the {} geometries under the LOD root kept since the map opened, {} are main members "
						 "of the frame's set now and {} wait in it",
				a_why, RenderGraph::Engine::Native::Active() ? "native" : "guest", a_frames, classes.size(), kept, members, waiting);
			for (std::size_t i = 0; i < classes.size() && i < kMapCensusClasses; ++i) {
				const auto& c = classes[i];
				logger::info("[DCLF] map census: {} ('{}', {}): main withheld {} kept {}, other withheld {} kept {}{}", c.shader, c.sample,
					c.dclf ? "DCLF's class" : "not DCLF's class", c.withheld, c.kept, c.otherWithheld, c.otherKept,
					c.dclf && (c.kept || c.otherKept) ?
						fmt::format("; kept because: no set {}, not in the set {}, in it without the main phase {}, with the main phase {}; under the LOD root {}",
							c.noSet, c.notMember, c.notMain, c.mainNotDrawn, c.underLodRoot) :
						std::string());
			}
		}

		bool mapOpen = false;
		std::uint32_t mapFrames = 0;

		/**
		 * @brief Render thread, at a frame's start: the map menu opening is a camera cut. The engine's map registers the LOD land, objects
		 * and clouds under TES::lodLandRoot in its first frames only, and DCLF's set, culled with the world camera, does not hold them yet.
		 * So the frame is held as a re-entry (BeginReentry): DCLF's classes are withheld from the engine and the last frame stays on screen
		 * until the set made with the map camera is installed and settled; DCLF then draws them. Closing the map is the cut back, held the
		 * same way. CS_DCLF_MAP_HOLD=0 turns both holds off.
		 * The map census starts here too (PassCapture::SetMapCensus).
		 */
		void WatchMapStart()
		{
			auto* ui = RE::UI::GetSingleton();
			const bool now = ui && ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
			if (now == mapOpen)
				return;
			mapOpen = now;
			static const bool mapHold = Util::SwitchFile::Value("CS_DCLF_MAP_HOLD") != "0";
			if (!now) {
				PassCapture::Get().SetMapCensus(false);
				LogMapCensus(mapFrames, "the map closed");
				mapFrames = 0;
				// Closing it is the cut back: the world's objects are not in the set culled with the map camera.
				if (mapHold)
					BeginReentry("the map menu closed (a camera cut back: the world is not in the set culled with the map camera)");
				return;
			}
			mapFrames = 0;
			PassCapture::Get().SetMapCensus(true);
			if (mapHold)
				BeginReentry("the map menu opened (a camera cut: the map's LOD is not in the set culled with the world camera)");
		}

		/** @brief Render thread, at a frame's end: the map census's line every kMapCensusFrames frames while the map is open. */
		void WatchMap()
		{
			if (mapOpen && ++mapFrames >= kMapCensusFrames) {
				LogMapCensus(mapFrames, "the map open");
				mapFrames = 0;
			}
		}
	}

	bool Active() { return state.load(std::memory_order_acquire) == HoldState::Holding; }

	bool MenuHeld()
	{
		const auto now = state.load(std::memory_order_acquire);
		return (now == HoldState::Holding || now == HoldState::Releasing) && withMenu.load(std::memory_order_relaxed);
	}

	bool PresentsHeld() { return state.load(std::memory_order_acquire) == HoldState::Holding && !withMenu.load(std::memory_order_relaxed); }

	void NoteHeld(View a_view)
	{
		if (!Active())
			return;
		++(a_view == View::Sun ? hold.heldSun : a_view == View::Occlusion ? hold.heldOcclusion : hold.heldShadow);
		if (!hold.heldThisFrame)
			++hold.held;
		hold.heldThisFrame = true;
	}

	void BeginReentry(const char* a_why)
	{
		reentryThisFrame = true;
		const auto now = state.load(std::memory_order_acquire);
		if (now == HoldState::Idle && !reentryBlocked && ShellState::Running() && !Scene::FrameView::Get().NotDrawingYet() &&
			!Scene::FrameView::LoadShown()) {
			hold = {};
			hold.start = hold.progress = std::chrono::steady_clock::now();
			hold.reentry = a_why;
			withMenu.store(false, std::memory_order_relaxed);
			state.store(HoldState::Holding, std::memory_order_release);
			logger::info("[DCLF] load hold: started (a re-entry: {}): the presents held, the last frame stays on screen", a_why);
		}
		NoteHeld(View::Shadow);
	}

	void OnFrameStart(bool a_running)
	{
		if (a_running)
			WatchMapStart();
		const auto now = state.load(std::memory_order_acquire);
		if (now == HoldState::Idle || now == HoldState::Releasing)
			return;
		if (!a_running) {
			Release("Drawcall Limit Fix does not run", false);
			return;
		}
		// The world's first frame under the kept menu: the load is done, and the tables are built from here (IsLoadingScreenUp: false).
		if (now == HoldState::Closing) {
			state.store(HoldState::Holding, std::memory_order_release);
			logger::info("[DCLF] load hold: the world's first frame under the kept loading menu, {:.0f} ms after the load's end", MillisecondsSince(hold.start));
		}
	}

	void OnFrameEnd(bool a_running)
	{
		WatchMap();
		const auto now = state.load(std::memory_order_acquire);
		if (now == HoldState::Releasing) {
			if (!MenuOpen())
				state.store(HoldState::Idle, std::memory_order_release);
			return;
		}
		const bool reentry = std::exchange(reentryThisFrame, false);
		if (!reentry)
			reentryBlocked = false;
		if (now != HoldState::Holding) {
			// Outside a load: what is not ready yet is withheld, never handed to the engine; logged by reason.
			if (now == HoldState::Idle && a_running && !Scene::FrameView::LoadShown())
				WatchAfterHold();
			return;
		}
		if (!a_running) {
			Release("Drawcall Limit Fix does not run", false);
			return;
		}
		if (withMenu.load(std::memory_order_relaxed) && !MenuOpen()) {
			Release("the loading menu closed by itself", true);
			return;
		}
		// Only the frames that drew the world count (the scene frame advanced).
		const std::uint32_t scene = Scene::FrameView::Get().GetFrame();
		if (scene == 0 || scene == std::exchange(hold.lastScene, scene)) {
			hold.heldThisFrame = false;
			return;
		}
		if (!hold.firstFrame)
			hold.firstFrame = scene;
		++hold.frames;
		hold.withheld += PassCapture::Get().GetStats().holdWithheld;
		std::uint32_t waits = 0;
		hold.settled = Settled(waits) ? hold.settled + 1 : 0;
		// A cold shader cache builds for many seconds while the counts stand still: every finished build counts as progress.
		const auto pipelines = DrawPipelines::Get().GetStats();
		const std::uint64_t builds = std::uint64_t(pipelines.ready) + pipelines.failed + pipelines.shadowReady + pipelines.shadowFailed;
		if (builds != std::exchange(hold.builds, builds))
			hold.progress = std::chrono::steady_clock::now();
		if (waits == ~0u || waits < hold.best) {
			hold.best = waits == ~0u ? hold.best : waits;
			hold.progress = std::chrono::steady_clock::now();
		}
		if (hold.settled >= kSettledFrames)
			Release("settled", false);
		else if (MillisecondsSince(hold.start) >= kCapMs)
			Release("at the cap", true);
		else if (MillisecondsSince(hold.progress) >= kStallMs)
			Release("no progress", true);
		hold.heldThisFrame = false;
	}

	namespace
	{
		/** @brief The coordinator's call of ServingThread::SetLoadingScreenActive(true) (SE 13214 / AE 13363 +0x11A): a loading screen starts. */
		struct LoadingScreenStart
		{
			static void thunk(bool a_active)
			{
				if (a_active)
					OnLoadBegin();
				func(a_active);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The coordinator's close of the loading menu (+0x211: UIMessageQueue::AddMessage("Loading Menu", kHide)): kept for a hold. */
		struct LoadingMenuClose
		{
			static void thunk(RE::UIMessageQueue* a_queue, const RE::BSFixedString& a_menu, RE::UI_MESSAGE_TYPE a_type, RE::IUIMessageData* a_data)
			{
				if (a_type == RE::UI_MESSAGE_TYPE::kHide && a_menu.c_str() && std::string_view(a_menu.c_str()) == RE::LoadingMenu::MENU_NAME && Begin())
					return;
				func(a_queue, a_menu, a_type, a_data);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		const auto coordinator = REL::RelocationID(13214, 13363).address();
		const auto start = coordinator + 0x11A, close = coordinator + 0x211;
		if (!Engine::CallsTo(start, REL::RelocationID(34552, 35407).address()) || !Engine::CallsTo(close, REL::RelocationID(13530, 13631).address())) {
			logger::warn(
				"[DCLF] load hold: the load coordinator's calls are not the engine's (another plugin's patch, or this runtime's code differs); "
				"loads are not held, and their objects show as they join");
			return;
		}
		stl::write_thunk_call<LoadingScreenStart>(start);
		stl::write_thunk_call<LoadingMenuClose>(close);
		logger::info("[DCLF] load hold installed: the loading menu stays up after each load until DCLF draws its objects");
	}
}
