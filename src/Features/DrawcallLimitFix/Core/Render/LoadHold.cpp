// LoadHold: the hold after every load, until DCLF's claims settle.

#include "Features/DrawcallLimitFix/Core/Render/LoadHold.h"
#include "Snapshot/EngineReads.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Publication.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "Snapshot/Publication.h"
#include "Testing/Testing.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DCLF::LoadHold
{
	namespace
	{
		enum class HoldState : std::uint8_t
		{
			Idle,       // no hold
			Closing,  // menu close kept, last loading screen iterations still present
			Holding,  // world frames run off screen
			Releasing,  // close posted, menu open until the UI takes it
		};

		// The pass after the one that walked the load's cells joins the first frame's culls, so a publication two frames later has seen them.
		// A load that changed nothing publishes nothing new, so after a few frames the installed one counts too.
		constexpr std::uint32_t kSettledFrames = 2;
		constexpr std::uint32_t kJoinFrames = 2;
		constexpr std::uint32_t kQuietFrames = 8;
		// No progress for this long ends the hold and logs what still waits. A hold has no time cap: a cold shader cache can build for
		// minutes after a feature change, and releasing the hold before the shadow epoch can record leaves it to the engine.
		constexpr double kStallMs = 5000.0;
		// While builds are in flight, the time without a finished build that counts as a stall. A cold build can take seconds.
		constexpr double kBuildStallMs = 30000.0;

		std::atomic<HoldState> state{ HoldState::Idle };
		std::atomic<bool> withMenu{ false };
		// The loading model is the Mist Menu's, so its end is kept with the loading menu's close and held frames keep drawing it.
		enum class MistEnd : std::uint8_t
		{
			None,
			FadeOut,
			Hide,
		};
		std::atomic<MistEnd> mistEndKept{ MistEnd::None };
		bool mistHideArg = false;
		void PostMistEnd();

		// The test runner treats any state but Idle as a hold.
		void ReportHold(HoldState a_state)
		{
			Testing::Holds::Set("dclf:load-hold", a_state != HoldState::Idle);
			if (a_state == HoldState::Idle)
				Testing::Holds::Set("dclf:load-hold-world", false);
		}

		void StoreState(HoldState a_state)
		{
			state.store(a_state, std::memory_order_release);
			ReportHold(a_state);
		}

		HoldState ExchangeState(HoldState a_state)
		{
			const auto previous = state.exchange(a_state, std::memory_order_acq_rel);
			ReportHold(a_state);
			return previous;
		}
		// A re-entry that ended at a stall does not start again until a frame without its condition.
		bool reentryBlocked = false;
		bool reentryThisFrame = false;

		// The main thread writes these in Begin before publishing the state. The render thread reads them after.
		struct Hold
		{
			std::chrono::steady_clock::time_point start, progress;
			std::uint32_t firstFrame = 0, lastScene = 0, frames = 0, settled = 0;  // frames: game updates under the hold
			std::uint64_t loadEpoch = 0;  // the load epoch whose snapshots count
			std::uint32_t held = 0, heldSun = 0, heldOcclusion = 0, heldShadow = 0;
			const char* reentry = nullptr;  // null for a load's hold
			std::uint32_t best = ~0u;  // fewer than this is progress
			// The pool reuses claim objects, so compare by object and publication frame.
			const void* claims = nullptr;
			std::uint32_t claimsFrame = 0;
			std::uint64_t withheld = 0;
			bool heldThisFrame = false;
			// Frames each condition was unmet, and the most objects that waited by reason.
			std::uint32_t waitedPublication = 0, waitedObjects = 0, waitedClaims = 0, waitedFamilies = 0, waitedViews = 0, waitedUndrawn = 0;
			std::uint32_t peakUndrawn = 0, undrawn = 0;
			std::vector<std::string> undrawnSamples;  // the last frame's, logged at the release  // lit main-view passes the claims did not draw: the most, and this frame's
			std::array<std::uint32_t, kSetWaitReasons> peakWaiting{};
			std::uint64_t builds = 0;  // any finished build counts as progress
			// Nearby actors the engine has not given 3D yet, at the hold's first world frame and at its release (~0u: not counted).
			std::uint32_t actorsWithout3DFirst = ~0u, actorsWithout3DLast = ~0u;
		};
		Hold hold;

		// Render thread. After a hold, log what is still withheld or handed to the engine at most every kAfterLogMs.
		constexpr double kAfterLogMs = 10000.0;
		struct After
		{
			std::chrono::steady_clock::time_point since{};
			std::uint64_t pending = 0, baseMips = 0, kept = 0;
			std::uint32_t frames = 0, familyFrames = 0;
			std::array<std::uint32_t, kSetWaitReasons> peakWaiting{};
			bool logged = false;  // log the next window even when quiet, to say so
			// Actors without 3D at the release, until the frame the count first reaches 0 (logged once).
			std::uint32_t actorsWithout3D = 0, actorFrames = 0;
		};
		After after;

		/**
		 * @brief Main thread. The high-process actors that are enabled and have no 3D yet: the engine's queued loads of the actors
		 * near the player. A measurement for the hold (dclf-open-defects.md, actors drawn by nobody after a load).
		 */
		std::uint32_t ActorsWithout3D()
		{
			auto* lists = CS_ENGINE_READ(Data, RecordTime, RE::ProcessLists::GetSingleton());
			if (!lists)
				return 0;
			std::uint32_t count = 0;
			for (const auto& handle : lists->highActorHandles) {
				const auto actor = handle.get();
				if (actor && !actor->IsDisabled() && !actor->IsDeleted() && !actor->Get3D())
					++count;
			}
			return count;
		}

		double MillisecondsSince(std::chrono::steady_clock::time_point a_start)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
		}

		bool MenuOpen()
		{
			auto* ui = CS_ENGINE_READ(UI, Other, RE::UI::GetSingleton());
			return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		}

		/** @brief Render side: the loading menu as the frame sees it (native: the adopted snapshot's menus; guest: the live UI). */
		bool MenuOpenInFrame()
		{
			if (!::RenderGraph::Engine::Native::Active())
				return MenuOpen();
			const auto& snapshot = Snapshot::Current();
			return snapshot && snapshot->menus.loading;
		}

		// ActorsWithout3D as the game side last counted it (OnFrameStart). Read by the render side.
		std::atomic<std::uint32_t> s_actorsWithout3D{ 0 };

		/** @brief Render side: the actors without 3D (native: the game half's count; guest: counted now). */
		std::uint32_t ActorsWithout3DInFrame()
		{
			return ::RenderGraph::Engine::Native::Active() ? s_actorsWithout3D.load(std::memory_order_relaxed) : ActorsWithout3D();
		}

		/** @brief Main thread. Starts a hold. True when the menu's close is kept. */
		bool Begin()
		{
			const bool menu = MenuOpen();
			hold = {};
			hold.start = hold.progress = std::chrono::steady_clock::now();
			hold.loadEpoch = Snapshot::LoadEpoch();
			withMenu.store(menu, std::memory_order_relaxed);
			// Without a menu the main thread renders next, so hold the presents now.
			StoreState(menu ? HoldState::Closing : HoldState::Holding);
			logger::info("[DCLF] load hold: started ({})", menu ? "the loading menu kept open" : "no loading menu: the presents held, the last frame stays on screen");
			return menu;
		}

		/** @brief Main thread. A load inside a hold ends it, since the new load posts its own close. */
		void OnLoadBegin()
		{
			// The new load shows the Mist Menu again, so drop the last one's kept end.
			mistEndKept.store(MistEnd::None, std::memory_order_relaxed);
			if (ExchangeState(HoldState::Idle) != HoldState::Idle)
				logger::info("[DCLF] load hold: a load began during the hold, which ends ({} frames, {:.0f} ms)", hold.frames, MillisecondsSince(hold.start));
		}

		std::string WaitingText(const ScenePublication* a_publication)
		{
			std::string text;
			if (a_publication)
				for (std::size_t why = 0; why < kSetWaitReasons; ++why)
					if (const auto count = a_publication->waiting[why])
						text += fmt::format("{}{} {}{}", text.empty() ? "" : ", ", kSetWaitReasonNames[why], count, (kSetTransientWaits >> why) & 1u ? "" : " (not waited for)");
			return text.empty() ? "none" : text;
		}

		std::string WaitedText()
		{
			std::string peaks;
			for (std::size_t why = 0; why < kSetWaitReasons; ++why)
				if (hold.peakWaiting[why])
					peaks += fmt::format("{}{} {}", peaks.empty() ? "" : ", ", kSetWaitReasonNames[why], hold.peakWaiting[why]);
			return fmt::format(
				"the load's publication {} frames, waiting objects {} frames (at most: {}), claims changing {} frames, draw families {} frames, a view not "
				"covered {} frames, passes the claims did not draw {} frames (at most {} a frame)",
				hold.waitedPublication, hold.waitedObjects, peaks.empty() ? "none" : peaks, hold.waitedClaims, hold.waitedFamilies, hold.waitedViews,
				hold.waitedUndrawn, hold.peakUndrawn);
		}

		/** @brief Render thread. Ends the hold. a_warn logs what still waits. */
		void Release(const char* a_why, bool a_warn)
		{
			const double ms = MillisecondsSince(hold.start);
			const bool menu = withMenu.load(std::memory_order_relaxed);
			const char* mode = menu ? "the loading menu kept open" : hold.reentry ? "a re-entry, the presents held" : "the presents held (no loading menu)";
			if (hold.reentry && a_warn)
				reentryBlocked = true;
			if (menu && MenuOpenInFrame()) {
				// The menu closes at the UI's next pass over its messages.
				StoreState(HoldState::Releasing);
				if (auto* queue = RE::UIMessageQueue::GetSingleton())
					queue->AddMessage(RE::BSFixedString(RE::LoadingMenu::MENU_NAME), RE::UI_MESSAGE_TYPE::kHide, nullptr);
				PostMistEnd();
			} else {
				StoreState(HoldState::Idle);
				PostMistEnd();
			}
			hold.actorsWithout3DLast = ActorsWithout3DInFrame();
			if (const auto& samples = hold.undrawnSamples; !samples.empty()) {
				std::string text;
				for (const auto& sample : samples)
					text += (text.empty() ? "" : "; ") + sample;
				logger::info("[DCLF] load hold: passes the claims did not draw in its last frame, first geometries: {}", text);
			}
			after = {};
			after.since = std::chrono::steady_clock::now();
			after.actorsWithout3D = hold.actorsWithout3DLast;
			if (!a_warn) {
				logger::info(
					"[DCLF] load hold: {} frames, {:.0f} ms ({}), {}; {} passes of DCLF's classes withheld; {} frames with a view DCLF did not cover yet "
					"(its engine cull skipped: {} sun, {} occlusion maps; {} frames with no shadow view DCLF's{}{}); waited for: {}; actors without 3D: {} at "
					"the first world frame, {} now",
					hold.frames, ms, mode, a_why, hold.withheld, hold.held, hold.heldSun, hold.heldOcclusion, hold.heldShadow, hold.reentry ? ": " : "",
					hold.reentry ? hold.reentry : "", WaitedText(), hold.actorsWithout3DFirst, hold.actorsWithout3DLast);
				return;
			}
			const auto& view = Scene::FrameView::Get();
			const auto* publication = view.InstalledPublication();
			const auto claims = PassCapture::Get().CurrentSet();
			logger::warn(
				"[DCLF] load hold: released after {} frames, {:.0f} ms ({}), {}; {} passes of DCLF's classes withheld. Still waiting: objects by reason: {}; "
				"draw families {}; claims {}; the installed publication {} (built {}, committed at frame {}, the world's first frame {}); a view held this frame: "
				"{}; waited for: {}; actors without 3D: {} at the first world frame, {} now. What is not ready stays withheld until it is",
				hold.frames, ms, mode, a_why, hold.withheld, WaitingText(publication), IndirectDraws::Get().DrawFamiliesWaiting(),
				claims ? (hold.settled ? "installed, unchanged" : "installed, changing") : "not installed", publication ? publication->sequence : 0ull,
				publication && publication->built, publication ? publication->commitFrame : 0u, hold.firstFrame, hold.heldThisFrame ? "yes" : "no", WaitedText(),
				hold.actorsWithout3DFirst, hold.actorsWithout3DLast);
		}

		/** @brief Render thread. Whether one world frame of a hold is settled. */
		bool Settled(std::uint32_t& a_waits)
		{
			const auto* publication = Scene::FrameView::Get().InstalledPublication();
			const bool fresh = publication && publication->built &&
			                   (publication->commitFrame >= hold.firstFrame + kJoinFrames || hold.frames >= kQuietFrames);
			const std::uint32_t transient = publication ? publication->TransientWaiting() : 0u;
			const bool objectsSettled = publication && publication->settled;
			const auto claims = PassCapture::Get().CurrentSet();
			const bool stable = claims && claims.get() == hold.claims && claims->frame == hold.claimsFrame;
			hold.claims = claims.get();
			hold.claimsFrame = claims ? claims->frame : 0u;
			const std::uint32_t families = IndirectDraws::Get().DrawFamiliesWaiting();
			a_waits = transient + families + (hold.heldThisFrame ? 1u : 0u) + hold.undrawn;
			hold.waitedPublication += fresh ? 0u : 1u;
			hold.waitedObjects += fresh && !objectsSettled ? 1u : 0u;
			hold.waitedClaims += stable ? 0u : 1u;
			hold.waitedFamilies += families ? 1u : 0u;
			hold.waitedViews += hold.heldThisFrame ? 1u : 0u;
			hold.waitedUndrawn += hold.undrawn ? 1u : 0u;
			hold.peakUndrawn = (std::max)(hold.peakUndrawn, hold.undrawn);
			if (fresh)
				for (std::size_t why = 0; why < kSetWaitReasons; ++why)
					if ((kSetTransientWaits >> why) & 1u)
						hold.peakWaiting[why] = (std::max)(hold.peakWaiting[why], publication->waiting[why]);
			// The load's objects are not in the tables yet, so nothing counts as progress or stall.
			if (!fresh)
				a_waits = ~0u;
			return fresh && objectsSettled && stable && !families && !hold.heldThisFrame && !hold.undrawn;
		}
	}

	namespace
	{
		/** @brief Render thread. Logs what of DCLF's classes was withheld or handed to the engine, at most every kAfterLogMs. */
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
			for (const void* accumulator : PassCapture::Get().TakeUncoveredAccumulators())
				logger::info("[DCLF] handed to the engine: a shadow view DCLF does not cover, accumulator {}", ShadowViews::Get().DescribeAccumulator(accumulator));
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
		// While the map menu is open, a census line every this many frames, and one at its close.
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
		 * @brief The map menu opening is a camera cut. The engine registers the map's LOD under TES::lodLandRoot only in its first frames,
		 * and DCLF's set does not hold it yet, so the frame is held as a re-entry until the map camera's set settles.
		 * Closing the map is held the same way.
		 */
		void WatchMapStart()
		{
			auto* ui = CS_ENGINE_READ(UI, RecordTime, RE::UI::GetSingleton());
			const bool now = ui && ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
			if (now == mapOpen)
				return;
			mapOpen = now;
			if (!now) {
				PassCapture::Get().SetMapCensus(false);
				LogMapCensus(mapFrames, "the map closed");
				mapFrames = 0;
				// The world's objects are not in the set culled with the map camera.
				BeginReentry("the map menu closed (a camera cut back: the world is not in the set culled with the map camera)", Snapshot::Building().get());
				return;
			}
			mapFrames = 0;
			PassCapture::Get().SetMapCensus(true);
			BeginReentry("the map menu opened (a camera cut: the map's LOD is not in the set culled with the world camera)", Snapshot::Building().get());
		}

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

	void BeginReentry(const char* a_why, const Snapshot::FrameSnapshot* a_frame)
	{
		reentryThisFrame = true;
		const auto now = state.load(std::memory_order_acquire);
		// The first world frames after a load's hold can still need a re-entry, because the graph rebuilds for re-made targets
		// and shadow views and occlusion maps are not DCLF's for a frame.
		if ((now == HoldState::Idle || now == HoldState::Releasing) && !reentryBlocked && !Scene::FrameView::Get().NotDrawingYet() &&
			!Scene::FrameView::LoadShownIn(a_frame)) {
			hold = {};
			hold.start = hold.progress = std::chrono::steady_clock::now();
			hold.reentry = a_why;
			hold.loadEpoch = a_frame ? a_frame->loadEpoch : Snapshot::LoadEpoch();
			withMenu.store(false, std::memory_order_relaxed);
			StoreState(HoldState::Holding);
			logger::info("[DCLF] load hold: started (a re-entry: {}): the presents held, the last frame stays on screen", a_why);
		}
		NoteHeld(View::Shadow);
	}

	void NoteUndrawable(View a_view, std::string_view a_why)
	{
		if (Active()) {
			NoteHeld(a_view);
			return;
		}
		// Before the world draws, or under a loading screen, nothing is drawn yet.
		auto& view = Scene::FrameView::Get();
		if (view.NotDrawingYet() || Scene::FrameView::LoadShown())
			return;
		::RenderGraph::Frame::Driver::NoteTapError(::RenderGraph::Frame::Driver::TapError::Uncovered, ::RenderGraph::Frame::SegmentId::Count,
			fmt::format("DCLF: frame {}: {}", view.SceneStep(), a_why));
	}

	void OnFrameStart()
	{
		WatchMapStart();
		// The render side's measurement of the actors still loading, counted on the game side.
		if (::RenderGraph::Engine::Native::Active())
			s_actorsWithout3D.store(ActorsWithout3D(), std::memory_order_relaxed);
		const auto now = state.load(std::memory_order_acquire);
		if (now == HoldState::Idle || now == HoldState::Releasing)
			return;
		// The world's first frame under the kept menu. The load is done, so tables are built from here.
		if (now == HoldState::Closing) {
			StoreState(HoldState::Holding);
			Testing::Holds::Set("dclf:load-hold-world", true);
			logger::info("[DCLF] load hold: the world's first frame under the kept loading menu, {:.0f} ms after the load's end", MillisecondsSince(hold.start));
		}
	}

	void OnFrameEnd()
	{
		WatchMap();
		const auto now = state.load(std::memory_order_acquire);
		if (now == HoldState::Releasing) {
			if (!MenuOpenInFrame())
				StoreState(HoldState::Idle);
			return;
		}
		const bool reentry = std::exchange(reentryThisFrame, false);
		if (!reentry)
			reentryBlocked = false;
		if (now != HoldState::Holding) {
			// Outside a load, what is not ready is withheld, never handed to the engine.
			if (now == HoldState::Idle && !Scene::FrameView::LoadShown()) {
				WatchAfterHold();
				if (after.actorsWithout3D) {
					++after.actorFrames;
					if (ActorsWithout3DInFrame() == 0) {
						logger::info("[DCLF] load hold: the last of the {} actors without 3D at the release got it {} frames later", after.actorsWithout3D,
							after.actorFrames);
						after.actorsWithout3D = 0;
					}
				}
			}
			return;
		}
		if (withMenu.load(std::memory_order_relaxed) && !MenuOpenInFrame()) {
			Release("the loading menu closed by itself", true);
			return;
		}
		// Only frames that drew the world count, and of those only the first render of a game update's snapshot from the hold's
		// load epoch: the hold counts snapshots, not render frames, so it lasts the same game updates at any cadence.
		const std::uint32_t scene = Scene::FrameView::Get().SceneStep();
		const auto& snapshot = Snapshot::Current();
		if (scene == 0 || scene == std::exchange(hold.lastScene, scene) || !snapshot || Snapshot::Render().repeat || snapshot->sub != 0 ||
			snapshot->loadEpoch != hold.loadEpoch) {
			hold.heldThisFrame = false;
			return;
		}
		if (!hold.firstFrame) {
			hold.firstFrame = scene;
			hold.actorsWithout3DFirst = ActorsWithout3DInFrame();
		}
		hold.frames += snapshot->events.gameUpdates;
		const auto& passes = PassCapture::Get().GetStats();
		hold.withheld += passes.holdWithheld;
		hold.undrawn = passes.holdNobody;
		hold.undrawnSamples = PassCapture::Get().TakeHoldNobodySamples();
		std::uint32_t waits = 0;
		hold.settled = Settled(waits) ? hold.settled + 1 : 0;
		// A cold shader cache builds for many seconds while the counts stand still, so every finished build counts as progress.
		// A pipeline waits for its SPIR-V stages first, so a finished shader program counts too.
		const auto pipelines = DrawPipelines::Get().GetStats();
		const auto programs = ShaderPrograms::Get().GetStats();
		const std::uint64_t programsDone = std::uint64_t(programs.ready) + programs.failed + programs.shadowReady + programs.shadowFailed;
		const std::uint64_t builds =
			std::uint64_t(pipelines.ready) + pipelines.failed + pipelines.shadowReady + pipelines.shadowFailed + programsDone;
		const bool compiling = pipelines.buildsInFlight > 0 || std::uint64_t(programs.requested) + programs.shadowRequested > programsDone;
		if (builds != std::exchange(hold.builds, builds))
			hold.progress = std::chrono::steady_clock::now();
		if (waits == ~0u || waits < hold.best) {
			hold.best = waits == ~0u ? hold.best : waits;
			hold.progress = std::chrono::steady_clock::now();
		}
		// The shadow epoch cannot record without its pipelines, so builds in flight get longer before they count as a stall.
		// A build that never finishes still ends the hold.
		const double stallMs = compiling ? kBuildStallMs : kStallMs;
		if (hold.settled >= kSettledFrames)
			Release("settled", false);
		else if (MillisecondsSince(hold.progress) >= stallMs)
			Release(compiling ? "no build finished" : "no progress", true);
		hold.heldThisFrame = false;
	}

	namespace
	{
		/** @brief ServingThread::SetLoadingScreenActive(true) in the load coordinator (SE 13214 / AE 13363 +0x11A). */
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

		bool KeepsMistEnd(MistEnd a_end)
		{
			if (state.load(std::memory_order_acquire) != HoldState::Closing || !withMenu.load(std::memory_order_relaxed))
				return false;
			mistEndKept.store(a_end, std::memory_order_release);
			logger::info("[DCLF] load hold: the Mist Menu's {} kept with the loading menu's close", a_end == MistEnd::FadeOut ? "fade-out" : "hide");
			return true;
		}

		/** @brief The coordinator's fade-out of the Mist Menu (+0x2E5). It hides itself once faded. */
		struct MistMenuFadeOut
		{
			static void thunk(std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4)
			{
				if (!KeepsMistEnd(MistEnd::FadeOut))
					func(a1, a2, a3, a4);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The coordinator's hide of the Mist Menu (+0x3C0), its other way out of a load. */
		struct MistMenuHide
		{
			static void thunk(const RE::BSFixedString& a_menu, RE::UI_MESSAGE_TYPE a_type, bool a_arg)
			{
				mistHideArg = a_arg;
				if (!KeepsMistEnd(MistEnd::Hide))
					func(a_menu, a_type, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void PostMistEnd()
		{
			switch (mistEndKept.exchange(MistEnd::None, std::memory_order_acq_rel)) {
			case MistEnd::FadeOut:
				MistMenuFadeOut::func(0, 0, 0, 0);
				break;
			case MistEnd::Hide:
				MistMenuHide::func(RE::BSFixedString(RE::MistMenu::MENU_NAME), RE::UI_MESSAGE_TYPE::kHide, mistHideArg);
				break;
			default:
				break;
			}
		}

		/** @brief The coordinator's close of the loading menu (+0x211, AddMessage("Loading Menu", kHide)), kept for a hold. */
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
		const auto start = coordinator + 0x11A, close = coordinator + 0x211, mistFade = coordinator + 0x2E5, mistHide = coordinator + 0x3C0;
		if (!Engine::CallsTo(start, REL::RelocationID(34552, 35407).address()) || !Engine::CallsTo(close, REL::RelocationID(13530, 13631).address()) ||
			!Engine::CallsTo(mistFade, REL::RelocationID(51456, 52317).address()) || !Engine::CallsTo(mistHide, REL::RelocationID(80077, 82180).address())) {
			logger::warn(
				"[DCLF] load hold: the load coordinator's calls are not the engine's (another plugin's patch, or this runtime's code differs); "
				"loads are not held, and their objects show as they join");
			return;
		}
		stl::write_thunk_call<LoadingScreenStart>(start);
		stl::write_thunk_call<LoadingMenuClose>(close);
		stl::write_thunk_call<MistMenuFadeOut>(mistFade);
		stl::write_thunk_call<MistMenuHide>(mistHide);
		logger::info("[DCLF] load hold installed: the loading menu stays up after each load until DCLF draws its objects");
	}
}
