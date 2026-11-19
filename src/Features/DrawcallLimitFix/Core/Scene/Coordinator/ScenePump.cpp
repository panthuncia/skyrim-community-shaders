// The coordinator's scene pump. Scene passes run on the scene lane, one at a time, whenever a producer wakes the pump.
// The frame never kicks or joins them. Also decides whether a pass publishes and when.

#include "Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Diagnostics/Capture/Capture.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

#include <ORGModuleServices/Async/SerializedTaskPump.h>

#include <algorithm>
#include <format>
#include <ranges>
#include <sstream>
#include <thread>
#include <type_traits>

namespace DCLF
{
	namespace
	{
		// Never destroyed, since a wake may come while the process tears down.
		std::atomic<org::async::SerializedTaskPump*> scenePump{ nullptr };
		// A producer whose bit is already set leaves the Notify to the one that set it. Any set bit means a pass is queued.
		std::atomic<std::uint32_t> scenePassReasons{ 0 };
		// Under the parities, wakes are dropped and the frame start runs the pass.
		std::atomic<bool> scenePassesInline{ false };
		std::atomic<bool> scenePassesLoading{ false };
		// One cache line per source, since the job threads count them.
		struct alignas(64) DeferredCount
		{
			std::atomic<std::uint64_t> count{ 0 };
			std::byte padding[64 - sizeof(std::atomic<std::uint64_t>)]{};  // a line each, explicit (C4324)
		};
		std::array<DeferredCount, static_cast<std::size_t>(SceneWake::Count)> scenePassDeferred{};
		// The pump is closed for good, logged once.
		std::atomic<bool> scenePumpFailed{ false };
		std::atomic<bool> throwNextPass{ false };
		thread_local std::uint32_t wakeBatchDepth = 0;
		thread_local std::uint32_t wakeBatched = 0;

		/** @brief Notifies the pump only when no pass was queued yet. */
		void WakeScenePassBits(std::uint32_t a_bits)
		{
			auto* pump = scenePump.load(std::memory_order_acquire);
			if (!pump || !a_bits || scenePassesInline.load(std::memory_order_relaxed))
				return;
			// Pairs with the fence ScenePass makes after taking the reasons. Either that pass's drains see the push, or this read sees the
			// reasons taken and this wake queues the next pass.
			std::atomic_thread_fence(std::memory_order_seq_cst);
			if ((scenePassReasons.load(std::memory_order_relaxed) & a_bits) == a_bits)
				return;
			if (scenePassReasons.fetch_or(a_bits, std::memory_order_acq_rel) != 0)
				return;
			(void)pump->Notify();
		}
		thread_local bool passInline = false;

		const std::shared_ptr<const FrameGlobals>& NoFrameGlobals()
		{
			static const auto none = std::make_shared<const FrameGlobals>();
			return none;
		}
	}

	void WakeScenePass(SceneWake a_source)
	{
		static_assert(static_cast<std::uint32_t>(SceneWake::Count) <= 32, "a bit per source");
		// A load pushes thousands of events a frame, so during a load they drain once a Present instead.
		const bool engineEvent = a_source >= SceneWake::Attach && a_source <= SceneWake::SwitchEvent;
		if (engineEvent && scenePassesLoading.load(std::memory_order_relaxed))
			return;
		// Inside the frame's render the engine sets and undoes these for its own views, so the frame's next pass takes them.
		// An attach, a detach or a switch still wakes.
		const bool renderToggle = a_source == SceneWake::Hidden || a_source == SceneWake::Leaf || a_source == SceneWake::PropertyUpdate ||
		                          a_source == SceneWake::AlphaUpdate || a_source == SceneWake::PropertyEvent;
		if (renderToggle && EngineReadWindow::IsOpen()) {
			scenePassDeferred[static_cast<std::size_t>(a_source)].count.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const std::uint32_t bit = 1u << static_cast<std::uint32_t>(a_source);
		if (wakeBatchDepth) {
			wakeBatched |= bit;
			return;
		}
		WakeScenePassBits(bit);
	}

	void SetScenePassLoading(bool a_loading)
	{
		scenePassesLoading.store(a_loading, std::memory_order_relaxed);
	}

	std::array<std::uint64_t, static_cast<std::size_t>(SceneWake::Count)> TakeScenePassDeferred()
	{
		std::array<std::uint64_t, static_cast<std::size_t>(SceneWake::Count)> taken{};
		for (std::size_t s = 0; s < taken.size(); ++s)
			taken[s] = scenePassDeferred[s].count.exchange(0, std::memory_order_relaxed);
		return taken;
	}

	ScenePassWakeBatch::ScenePassWakeBatch()
	{
		++wakeBatchDepth;
	}

	ScenePassWakeBatch::~ScenePassWakeBatch()
	{
		if (--wakeBatchDepth == 0)
			WakeScenePassBits(std::exchange(wakeBatched, 0u));
	}

}

namespace DCLF::Scene
{
	void Coordinator::StartScenePump()
	{
		// Never destroyed. The lane is the scene work's alone, so a long pass never holds up the frame's builds.
		static auto* pump = SceneScheduler::MakeLanePump(
			SceneScheduler::Executor(), SceneScheduler::SceneLaneScope(), SceneExecutor::Scene, org::async::TaskDispatch::Controlled, "DCLF scene pass",
			[this] { ScenePass(passInline); },
			[] {
				if (!scenePumpFailed.exchange(true, std::memory_order_acq_rel))
					logger::error("[DCLF] the scene pump stopped (its task refused by the scene lane, or a pass threw past its handler); no scene pass runs from here on");
			});
		// A returned retirement does not wake, since that could wake a pass whose drop returns the next one.
		scenePump.store(pump, std::memory_order_release);
		// Catches what was pushed before the pump existed.
		WakeScenePass(SceneWake::Attach);
	}

	void Coordinator::SetScenePassMode(bool a_full, bool a_inline)
	{
		passesFull.store(a_full, std::memory_order_release);
		scenePassesInline.store(a_inline, std::memory_order_relaxed);
	}

	void Coordinator::ThrowNextScenePass()
	{
		throwNextPass.store(true, std::memory_order_release);
		WakeScenePass(SceneWake::FrameInput);
	}

	bool Coordinator::ScenePumpFailed() const
	{
		return scenePumpFailed.load(std::memory_order_acquire);
	}

	bool Coordinator::SceneTaskInFlight() const
	{
		const auto* pump = scenePump.load(std::memory_order_acquire);
		return pump && !pump->IsIdle();
	}

	void Coordinator::RunScenePassInline()
	{
		auto* pump = scenePump.load(std::memory_order_acquire);
		if (!pump)
			return;
		// Under the parities, a pass the lane already started ends first, then this frame's runs here.
		passInline = true;
		while (!pump->TryRunInline()) {
			if (scenePumpFailed.load(std::memory_order_acquire))
				break;
			std::this_thread::yield();
		}
		passInline = false;
		FrameView::Get().DeliverPrimaryNotes();
	}

	void Coordinator::ApplyEventsInline()
	{
		const bool full = passesFull.exchange(false, std::memory_order_acq_rel);
		RunScenePassInline();
		passesFull.store(full, std::memory_order_release);
	}

	void Coordinator::ScenePass(bool a_inline)
	{
		DCLF_ZONE("CS.DCLF.ScenePass");
		if (throwNextPass.exchange(false, std::memory_order_acq_rel)) [[unlikely]]
			throw 0;  // not a std::exception: past the pass's handler, into the pump's (SerializedTaskPump::RunOwned), which stops it
		// Take the sources before anything else. This pairs with WakeScenePassBits' fence.
		const std::uint32_t sources = scenePassReasons.exchange(0, std::memory_order_acq_rel);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		passSources = sources;
		passTookFrameInputs = false;
		passDeferred = false;
		const std::uint64_t publishedBefore = pumpStats.published;
		const auto start = std::chrono::steady_clock::now();
		const std::uint64_t eventsOnlyBefore = pumpStats.eventsOnly;
		// On the lane no engine memory may be read without a lease. Inline the render thread stays itself.
		if (!a_inline)
			sceneWorkThread = true;
		const bool marked = EngineReadWindow::sceneWork;
		if (!a_inline)
			EngineReadWindow::sceneWork = true;
		// Inputs posted after this belong to the next pass, and their post wakes it.
		TakeFrameInputs();
		try {
			passGlobals = sceneInputs.globals ? sceneInputs.globals : NoFrameGlobals();
			FrameGlobals::Scope scope(passGlobals);
			// The whole pass reads the toggles its frame inputs carried, never the frame's or the live ones.
			ToggleSnapshotScope toggles(&passSnapshot.toggles);
			RunSceneWork(!a_inline);
			RunActorProbeIfAsked();
			// This lane owns the census state.
			DCLF::Capture::RunSceneLaneWork();
		} catch (const std::exception& e) {
			// Never throw out of the drain, or the pump closes for good.
			if (pumpStats.failed++ < 4)
				logger::error("[DCLF] a scene pass threw: {}; the tables are whatever it left, and the next wake runs another", e.what());
			holdPrimaryNotes = false;
			holdLostMembers = false;
			inSceneTask = false;
		}
		EngineReadWindow::sceneWork = marked;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		auto& ps = pumpStats;
		ps.passMs.push_back(ms);
		++ps.passes;
		ps.inlinePasses += a_inline ? 1 : 0;
		// A pass counts under every source it took.
		const bool changed = ps.published != publishedBefore || passDeferred;
		for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(SceneWake::Count); ++s)
			if ((sources >> s) & 1u) {
				++ps.bySource[s];
				ps.unchangedBySource[s] += changed ? 0u : 1u;
			}
		if (!sources) {
			++ps.bySource[static_cast<std::size_t>(SceneWake::Count)];
			ps.unchangedBySource[static_cast<std::size_t>(SceneWake::Count)] += changed ? 0u : 1u;
		}
		if (ps.eventsOnly == eventsOnlyBefore) {
			ps.fullMs += ms;
			ps.fullMaxMs = (std::max)(ps.fullMaxMs, ms);
		}
		if (!ps.any || sceneFrame != ps.lastFrame) {
			if (!ps.any)
				ps.firstFrame = sceneFrame;
			ps.any = true;
			ps.lastFrame = sceneFrame;
			++ps.framesWithPass;
			ps.passesThisFrame = 0;
		}
		ps.maxPassesInFrame = (std::max)(ps.maxPassesInFrame, ++ps.passesThisFrame);
		TracyPlot("CS.DCLF.ScenePassMs", ms);
		{
			// The snapshot builder may publish for the frame now.
			DCLF_ZONE("CS.DCLF.Scene.NoteScenePassDone");
			IndirectDraws::Get().NoteScenePassDone(sceneFrame);
		}
		DCLF_ZONE_NAMED(passReportsZone, "CS.DCLF.Scene.PassReports", true);
		Reports::ScenePassEnd(*this, sceneFrame);
		ReportMoverGate();
		ReportPublicationPacing();
	}

	void Coordinator::ReportPublicationPacing()
	{
		const std::uint32_t window = sceneFrame / Reports::Interval();
		if (window == paceWindow)
			return;
		paceWindow = window;
		auto& p = paceStats;
		if (p.passes) {
			const double frames = static_cast<double>((std::max)(p.frames, 1u));
			const double publications = static_cast<double>((std::max)(p.published, std::uint64_t(1)));
			const auto reason = [&](PublishReason a_why) { return p.byReason[static_cast<std::size_t>(a_why)]; };
			logger::info("[DCLF] publication pacing (W3: a publication only once the snapshot builder took the last, or for the frame's pass): {} full passes over {} frames "
						 "({:.2f} a frame, max {}), {} publications ({:.2f} a frame, max {}; {} the frame's pass, {} the last taken, {} inline, {} the first), {} passes deferred "
						 "({} folded into a later publication); claims {} replayed ({:.1f} keys each), {} copied whole, {} shared again; tables' whole-copied rest "
						 "{:.3f} ms a publication; paced parity {} checks, {} tables differ, {} claims differ{}{}",
				p.passes, p.frames, static_cast<double>(p.passes) / frames, p.maxPasses, p.published, static_cast<double>(p.published) / frames, p.maxPublished,
				reason(PublishReason::Frame), reason(PublishReason::Taken), reason(PublishReason::Inline), reason(PublishReason::First), p.deferred, p.folded,
				p.claimsReplayed, static_cast<double>(p.claimsKeys) / static_cast<double>((std::max)(p.claimsReplayed, std::uint64_t(1))), p.claimsWhole,
				p.claimsShared, p.restMs / publications, p.parityChecks, p.parityTables, p.parityClaims,
				!p.parityChecks ? "" : (p.parityTables || p.parityClaims) ? " <- PACED PUBLICATION DIFFERS; first: " : " <- OK", p.parityFirst);
			static constexpr std::array<const char*, std::tuple_size_v<decltype(PublishKey::words)> + 2> kKeyNames{ "tables generation", "change log",
				"geometry log", "material log", "extras log", "version counter", "constants stamp", "family stamps", "objects", "geometries", "pipelines",
				"materials", "sun candidates", "light candidates", "light entries appeared", "commit toggles", "pipelines applied", "lookups", "catalog" };
			std::string moved;
			for (std::size_t w = 0; w < kKeyNames.size(); ++w)
				if (p.keyMoved[w])
					moved += fmt::format("{}{} {}", moved.empty() ? "" : ", ", kKeyNames[w], p.keyMoved[w]);
			static constexpr std::array<const char*, 12> kDeltaNames{ "joined", "left", "placement plan", "plans held", "shading named", "seed requests",
				"switches applied", "switch resync", "retired imports", "class member changes", "light entry changes", "component deltas" };
			static constexpr std::array<const char*, 6> kLookupsNames{ "instance", "versions/generation", "shadow/shared", "material log", "changes",
				"chunk writes" };
			std::string deltas, lookupsWords;
			for (std::size_t d = 0; d < kDeltaNames.size(); ++d)
				if (p.deltaPending[d])
					deltas += fmt::format("{}{} {}", deltas.empty() ? "" : ", ", kDeltaNames[d], p.deltaPending[d]);
			for (std::size_t w = 0; w < kLookupsNames.size(); ++w)
				if (p.lookupsMoved[w])
					lookupsWords += fmt::format("{}{} {}", lookupsWords.empty() ? "" : ", ", kLookupsNames[w], p.lookupsMoved[w]);
			logger::info("[DCLF] publication causes (the passes that needed one, each cause counted): {} forced or first, {} set applications, {} set snapshot "
						 "or node sets, {} deltas ({}), {} publish key moved ({}; lookups: {})",
				p.neededBy[0], p.neededBy[1], p.neededBy[2], p.neededBy[3], deltas.empty() ? "none" : deltas, p.neededBy[4],
				moved.empty() ? "none" : moved, lookupsWords.empty() ? "none" : lookupsWords);
			const double columns = static_cast<double>(p.replayColumns + p.replaySkipped);
			logger::info("[DCLF] publication replay (W6: the logs collected once, the columns and records by jobs, the nearest pooled snapshot): per publication {:.1f} "
						 "log entries for {:.1f} object slots, {:.1f} material-log entries for {:.1f} records; slot columns {:.1f} copied in {:.1f} runs, {:.1f}% skipped (no "
						 "cause covers them); {} of {} replays on the pool, {} snapshots nearer than the first free; replay {:.3f} ms a publication; pass fixed cost: forward "
						 "pipelines counted {} times ({} kept), waiting slots compacted {} times ({} kept)",
				static_cast<double>(p.replayLogEntries) / publications, static_cast<double>(p.replaySlots) / publications,
				static_cast<double>(p.replayMaterialEntries) / publications, static_cast<double>(p.replayMaterials) / publications,
				static_cast<double>(p.replayColumns) / publications, static_cast<double>(p.replayRuns) / publications,
				columns > 0.0 ? 100.0 * static_cast<double>(p.replaySkipped) / columns : 0.0, p.replayPooled, p.published, p.replayNearer, p.replayMs / publications,
				p.forwardCounted, p.forwardKept, p.waitingCompacted, p.waitingKept);
		}
		const std::uint32_t lastFrame = p.lastFrame;
		p = {};
		p.lastFrame = lastFrame;
	}

	bool Coordinator::PublicationDue(bool a_task, PublishReason& a_why) const
	{
		// Otherwise a publication posted now would only replace the last one in the work slot. The pass defers and its changes stay pending.
		if (!publishedKeyValid || !publicationSequence)
			return a_why = PublishReason::First, true;
		if (!a_task)
			return a_why = PublishReason::Inline, true;
		if (passTookFrameInputs || (passSources & (1u << static_cast<std::uint32_t>(SceneWake::FrameInput))))
			return a_why = PublishReason::Frame, true;
		if (IndirectDraws::Get().SnapshotWorkTaken() >= publicationSequence)
			return a_why = PublishReason::Taken, true;
		return false;
	}

	void Coordinator::ReportMoverGate()
	{
		// The coordinator's clock may skip frame numbers.
		const std::uint32_t window = sceneFrame / Reports::Interval();
		if (window == moverGateWindow)
			return;
		moverGateWindow = window;
		auto& g = moverGateStats;
		if (g.perFrameWalks) {
			const double walks = static_cast<double>(g.perFrameWalks);
			const bool witnessed = g.witnessChecks != 0;
			logger::info("[DCLF] mover gate (A1: the light path by event, the plan as a delta): {} per-frame walks ({} ungated); per walk {:.1f} per-frame entries taken "
						 "({:.1f} moved, {:.1f} hidden event owed, {:.1f} first person, {:.1f} with no light path), {:.1f} left for want of an event; placement set {} movers, "
						 "{} roots ({} still); {} plans ({} with written slots, {} with the set changed; {} candidates the same as the last), {} per-frame walks made none; "
						 "mirror updates classifying again {} nodes, {} geometries; gate parity {} checks, {} would differ{}{}",
				g.perFrameWalks, g.ungatedWalks, static_cast<double>(g.due) / walks, static_cast<double>(g.moved) / walks, static_cast<double>(g.owed) / walks,
				static_cast<double>(g.firstPerson) / walks, static_cast<double>(g.fullEveryFrame) / walks, static_cast<double>(g.skipped) / walks, moverSet.size(),
				movingRoots.size() + stillRoots.size(), stillRoots.size(), g.plans, g.plansWritten, g.plansChanged, g.candidatesSame, g.perFrameWithoutPlan, g.mirrorNodes,
				g.mirrorGeometries, g.witnessChecks, g.witnessDiffer, !witnessed ? "" : g.witnessDiffer ? " <- GATE MISSED; first: " : " <- OK", g.firstDiffer);
		}
		g = {};
	}

	bool Coordinator::DeltasPending() const
	{
		return !publicationJoined.empty() || !publicationLeft.empty() || placementPlanReady || !placementPlansHeld.empty() || !shadingNamed.empty() ||
		       !componentSeedRequests.empty() || !switchesApplied.empty() || switchResync || !retiredImports.empty() ||
		       tables.ClassMemberChangesPending() || !lightEntryChanges.empty() || !componentDeltaRequests.empty();
	}

	Coordinator::PublishKey Coordinator::CurrentPublishKey() const
	{
		PublishKey key;
		key.words = { tablesGeneration, tables.changeLog.End(), tables.geometryLog.End(), tables.materialLog.End(), tables.extrasBlockLog.End(),
			tables.versionCounter, tables.constantsStamp, tables.families.StampSum(), tables.objects.size(), tables.geometries.size(),
			tables.pipelines.size(), tables.materials.size(), sunCandidatesGeneration, lightCandidatesGeneration, lightEntriesAppeared, setCommitToggles,
			constantsPostStats.pipelinesApplied };
		key.lookups = lookups.ChangeKey();
		key.catalog = lookupsCatalog.get();
		return key;
	}

	bool Coordinator::PublicationNeeded()
	{
		// A pass that changed none of these publishes nothing, and the last publication's snapshot stands.
		const PublishKey key = CurrentPublishKey();
		const std::array<bool, 5> causes{ !publishedKeyValid || publishForced, !setApply.empty(), setSnapshotDirty || nodeSetsDirty, DeltasPending(),
			!(key == publishedKey) };
		bool needed = false;
		for (std::size_t c = 0; c < causes.size(); ++c)
			if (causes[c]) {
				needed = true;
				++paceStats.neededBy[c];
			}
		if (!needed) {
			++pumpStats.unchanged;
			return false;
		}
		if (causes[3]) {
			const std::array<bool, 12> deltas{ !publicationJoined.empty(), !publicationLeft.empty(), static_cast<bool>(placementPlanReady), !placementPlansHeld.empty(),
				!shadingNamed.empty(), !componentSeedRequests.empty(), !switchesApplied.empty(), switchResync, !retiredImports.empty(),
				tables.ClassMemberChangesPending(), !lightEntryChanges.empty(), !componentDeltaRequests.empty() };
			for (std::size_t d = 0; d < deltas.size(); ++d)
				paceStats.deltaPending[d] += deltas[d];
		}
		if (causes.back() && publishedKeyValid) {
			for (std::size_t w = 0; w < key.words.size(); ++w)
				paceStats.keyMoved[w] += key.words[w] != publishedKey.words[w];
			paceStats.keyMoved[key.words.size()] += key.lookups != publishedKey.lookups;
			for (std::size_t w = 0; w < key.lookups.size(); ++w)
				paceStats.lookupsMoved[w] += key.lookups[w] != publishedKey.lookups[w];
			paceStats.keyMoved[key.words.size() + 1] += key.catalog != publishedKey.catalog;
		}
		return true;
	}

	void Coordinator::NotePublished()
	{
		publishedKey = CurrentPublishKey();
		publishedKeyValid = true;
		publishForced = false;
	}

}
