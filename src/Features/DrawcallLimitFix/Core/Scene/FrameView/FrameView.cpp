// FrameView's frame start and hand-over: frame inputs posted, the adopted publication installed with the log walked up to it,
// claims installed, and load screens noted. Render thread.

#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Engine/SceneClock.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Render/LoadHold.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"
#include "Snapshot/Retire.h"

namespace DCLF
{
	/** @brief Whether DCLF's first claims are installed. Render thread. */
	bool DrawsWorld()
	{
		return !Scene::FrameView::Get().NotDrawingYet();
	}

	/** @brief A load hold can start inside a frame, before its first clear, and then DCLF's epochs are not submitted. Render thread. */
	bool EpochsRun()
	{
		return DrawsWorld() && !LoadHold::Active();
	}

	/** @brief The main depth is cleared elsewhere too, as in a menu's 3D scene, and only RenderDepth's clear is the Z-prepass's. Render thread. */
	bool InDepthPass()
	{
		return ShellState::Passes().inDepthPass;
	}
}

namespace DCLF::Scene
{
	FrameView& FrameView::Get()
	{
		static FrameView view;
		return view;
	}

	void FrameView::BeginFrame()
	{
		// Switch events are taken on the render thread only.
		switchEventThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		gameStep = stepCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
		// Guest and native's frames on one thread render the step right after: their render half reads it before StepRender.
		if (!t_gameHalf)
			renderStep = gameStep;
		// The frame's engine globals, before anything of the frame reads them.
		const auto captureStart = std::chrono::steady_clock::now();
		// The Lighting shader first, since the globals' material sources read it.
		CaptureLightingShader();
		frameGlobals = FrameGlobals::Capture();
		SceneCapture::SetMainThread(::GetCurrentThreadId());
		if (!t_gameHalf)
			DCLF::SceneClock::Set(gameStep);
		captureNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - captureStart).count());
		++captureFrames;
		// Posted after the capture. The post wakes the pump.
		PostFrameInputs();
	}

	void FrameView::StepRender(std::uint32_t a_ticketStep, bool a_first)
	{
		std::uint32_t expected = a_ticketStep;
		// A first render keeps its ticket's step only while the counter still stands there; a repeat, or a render after another step
		// was taken, takes the next.
		if (!a_first || !stepCounter.compare_exchange_strong(expected, a_ticketStep, std::memory_order_acq_rel))
			renderStep = stepCounter.fetch_add(1, std::memory_order_acq_rel) + 1;
		else
			renderStep = a_ticketStep;
		renderSerial.fetch_add(1, std::memory_order_acq_rel);
		DCLF::SceneClock::Set(renderStep);
	}

	void FrameView::PostFrameInputs()
	{
		auto inputs = std::make_unique<FrameInputs>();
		inputs->globals = frameGlobals;
		inputs->frame = gameStep;
		// Sampled here so PrimaryCull reads the same sample this frame.
		// The frame's own capture: native's game half has bound none yet here, and the render owner's latest moves on its thread.
		inputs->membershipWitness = PrimaryCull::Get().SampleMembershipWitness(*frameGlobals);
		inputs->toggles = Toggles::Get().Active();
		inputs->togglesGeneration = Toggles::Get().Generation();
		inputs->verdictsGeneration = verdictsRequested;
		channels.frameInputs.Post(std::move(inputs));
		WakeScenePass(SceneWake::FrameInput);
	}

	std::uint64_t FrameView::InstalledSequence() const
	{
		const auto* publication = InstalledPublication();
		return publication ? publication->sequence : 0;
	}

	std::shared_ptr<const void> FrameView::InstalledRetirement() const
	{
		const auto* publication = InstalledPublication();
		return publication ? publication->retirementHold : nullptr;
	}

	std::shared_ptr<const FrameView::InstalledFrame> FrameView::TakeInstalledFrame()
	{
		auto frame = std::make_shared<InstalledFrame>();
		frame->publication = installed;
		frame->tables = acceptedTables;
		frame->lookups = frameLookups;
		frame->catalog = frameCatalog;
		frame->tablesGeneration = frameTablesGeneration;
		frame->sunCandidates = frameSunCandidates;
		frame->lightCandidates = frameLightCandidates;
		frame->lightEntriesAppeared = frameLightEntriesAppeared;
		frame->categoryNodes = frameCategoryNodes;
		if (std::exchange(lightEntriesChanged, false) || !lightEntriesCopy)
			lightEntriesCopy = std::make_shared<const ankerl::unordered_dense::set<const RE::NiAVObject*>>(frameLightEntrySet);
		frame->lightEntries = lightEntriesCopy;
		frame->claims = std::atomic_load(&frameClaims);
		frame->toggles = installed ? installed->toggles : Toggles::Get().Active();
		frame->notDrawing = NotDrawingYet();
		frame->stops = stopsApplied;
		return frame;
	}

	void FrameView::AdoptInstalledFrame(std::shared_ptr<const InstalledFrame> a_frame)
	{
		if (!a_frame || a_frame == renderFrame)
			return;
		std::atomic_store(&renderClaims, a_frame->claims);
		DrawPipelines::Get().HoldCatalog(a_frame->catalog, false);
		Toggles::Get().InstallFrame(a_frame->toggles);
		// The replaced frame may hold the last reference to a publication: it goes through the engine lane, like the tickets.
		if (renderFrame)
			Snapshot::Retire::AfterThisFrame(Snapshot::Retire::Lane::Engine, std::move(renderFrame));
		renderFrame = std::move(a_frame);
	}

	void FrameView::HandOverAtFrameStart(std::shared_ptr<const void> a_adopted)
	{
		DCLF_ZONE("CS.DCLF.Scene.HandOverAtFrameStart");
		// Nothing of the coordinator's is read here.
		DeliverPrimaryNotes();
		ApplyAskedStops();
		// A snapshot remade for the installed publication brings no new one.
		auto adopted = std::static_pointer_cast<const ScenePublication>(std::move(a_adopted));
		const bool fresh = adopted && (!installed || adopted->sequence > installed->sequence);
		// The log also holds publications the builder skipped.
		using Log = org::async::PublicationLog<PublicationDeltas>;
		Log::Node* at = deltasCursor ? deltasCursor : channels.publicationLog.First();
		const std::uint64_t upTo = fresh ? adopted->sequence : (installed ? installed->sequence : 0);
		auto append = [](auto& a_to, auto& a_from) {
			a_to.insert(a_to.end(), std::make_move_iterator(a_from.begin()), std::make_move_iterator(a_from.end()));
			a_from.clear();
		};
		for (Log::Node* next = Log::Next(at); next && next->value.publication <= upTo; next = Log::Next(at)) {
			at = next;
			auto& deltas = at->value;
			append(framePlans, deltas.plans);
			append(frameShading, deltas.shading);
			if (frameComponentSeeds.size() < deltas.componentSeeds.size())
				frameComponentSeeds.resize(deltas.componentSeeds.size());
			for (std::size_t c = 0; c < deltas.componentSeeds.size(); ++c)
				append(frameComponentSeeds[c], deltas.componentSeeds[c]);
			append(frameSwitchChanges, deltas.switches);
			frameSwitchResync = frameSwitchResync || std::exchange(deltas.switchResync, false);
			append(frameRetiredImports, deltas.retiredImports);
			for (std::size_t c = 0; c < deltas.classMembers.size(); ++c) {
				if (!deltas.classMembers[c].empty())
					Extensions::ForwardClassMembers(static_cast<Api::EntryClass>(c), deltas.classMembers[c]);
				deltas.classMembers[c].clear();
			}
			lightEntriesChanged = lightEntriesChanged || !deltas.lightEntries.empty();
			for (const auto& change : deltas.lightEntries) {
				if (change.kind > 0)
					frameLightEntrySet.insert(change.node);
				else if (change.kind < 0)
					frameLightEntrySet.erase(change.node);
				else
					frameLightEntrySet.clear();
			}
			deltas.lightEntries.clear();
		}
		deltasCursor = at;
		frameSwitchResync = frameSwitchResync || std::exchange(switchResyncNext, false);
		if (fresh) {
			// Never past the frame inputs' cursor.
			const std::uint64_t sequence = adopted->sequence;
			Log::Node* note = notesCursor ? notesCursor : channels.publicationLog.First();
			Log::Node* const bound = deltasCursor ? deltasCursor : channels.publicationLog.First();
			std::uint64_t oldestNs = 0;
			std::uint32_t oldestFrame = 0;
			while (note != bound) {
				Log::Node* next = Log::Next(note);
				if (!next || next->value.publication > sequence)
					break;
				note = next;
				auto& deltas = note->value;
				installNotes.emplace_back(std::move(deltas.joined), std::move(deltas.left));
				if (frameComponentDeltas.size() < deltas.componentDeltas.size())
					frameComponentDeltas.resize(deltas.componentDeltas.size());
				for (std::size_t c = 0; c < deltas.componentDeltas.size(); ++c)
					append(frameComponentDeltas[c], deltas.componentDeltas[c]);
				if (deltas.eventNs && (!oldestNs || deltas.eventNs < oldestNs)) {
					oldestNs = deltas.eventNs;
					oldestFrame = deltas.eventFrame;
				}
			}
			notesCursor = note;
			if (oldestNs) {
				const auto nowNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
				IndirectDraws::Get().NoteEventAdoption(nowNs > oldestNs ? static_cast<double>(nowNs - oldestNs) / 1.0e6 : 0.0, SceneStep() >= oldestFrame ? SceneStep() - oldestFrame : 0u);
			}
			channels.publicationLog.Release(note);
			TracyPlot("CS.DCLF.InstallDelayFrames", static_cast<std::int64_t>(SceneStep() - adopted->commitFrame));
			publicationStats.skipped += installed ? sequence - installed->sequence - 1 : 0;
			ReturnPublication(std::move(installed));
			installed = std::move(adopted);
			installPending = true;
			++publicationStats.installed;
		} else {
			++publicationStats.kept;
		}
		// The frame's tables, lookups and catalog all come from one publication, so indices match the set versions it binds.
		acceptedTables = installed ? installed->tables : nullptr;
		frameLookups = installed ? installed->lookups : nullptr;
		frameCatalog = installed ? installed->catalog : nullptr;
		frameTablesGeneration = installed ? installed->tablesGeneration : 0u;
		DrawPipelines::Get().HoldCatalog(frameCatalog, t_gameHalf);
		// A toggle takes effect for the frames with the first publication committed under it, which came with the adopted scene.
		if (installed) {
			Toggles::Get().InstallFrame(installed->toggles);
			frameSunCandidates = installed->sunCandidates;
			frameLightCandidates = installed->lightCandidates;
			frameLightEntriesAppeared = installed->lightEntriesAppeared;
			frameCategoryNodes = installed->categoryNodes;
		}
	}

	void FrameView::InstallClaims()
	{
		if (!installed)
			return;
		// No claim is withdrawn while DCLF draws. The first installation, and the first after a stop, install the claims whole.
		const bool whole = !drawingStarted.exchange(true, std::memory_order_acq_rel) || std::exchange(stopped, false);
		if (!whole && !installPending)
			return;
		installPending = false;
		auto& primary = PrimaryCull::Get();
		PublishFrameClaims(installed->claims);
		frameLackingCount = installed->lackingCount;
		if (whole) {
			// Before the first installation the engine's hooks hold no claim, so every main claim joins.
			std::vector<const RE::BSGeometry*> joined;
			for (const auto& [geometry, phases] : installed->claims->phases)
				if (phases & kSetMain)
					joined.push_back(geometry);
			if (!joined.empty())
				primary.NoteSetChanges(joined, {});
		} else {
			for (const auto& [joined, left] : installNotes)
				if (!joined.empty() || !left.empty())
					primary.NoteSetChanges(joined, left);
		}
		installNotes.clear();
		notedClaims = installed->claims;
	}

	void FrameView::StopDrawing()
	{
		if (!RenderSide()) {
			std::atomic_store(&renderClaims, std::shared_ptr<const SetSnapshot>{});
			stopsAsked.fetch_add(1, std::memory_order_acq_rel);
			ApplyAskedStops();
			return;
		}
		// The render side stops now. The game half withdraws the claims at its next frame start. Asked even while this side does not
		// draw: a ticket the game half made since may install the claims again.
		const bool drawing = !NotDrawingYet();
		std::atomic_store(&renderClaims, std::shared_ptr<const SetSnapshot>{});
		stopsAsked.fetch_add(1, std::memory_order_acq_rel);
		if (drawing)
			logger::info("[DCLF] frame {}: a frame revision left DCLF's scene on old versions, so the engine draws until DCLF's next scene", SceneStep());
	}

	void FrameView::ApplyAskedStops()
	{
		const std::uint64_t asked = stopsAsked.load(std::memory_order_acquire);
		if (asked == std::exchange(stopsApplied, asked) || !drawingStarted.load(std::memory_order_acquire) || stopped)
			return;
		stopped = true;
		// Every claimed main member leaves, so the engine's culls take the whole scene again.
		std::vector<const RE::BSGeometry*> left;
		if (notedClaims)
			for (const auto& [geometry, phases] : notedClaims->phases)
				if (phases & kSetMain)
					left.push_back(geometry);
		PublishFrameClaims(nullptr);
		if (!left.empty())
			PrimaryCull::Get().NoteSetChanges({}, left);
		installNotes.clear();
		if (!t_gameHalf)
			logger::info("[DCLF] frame {}: a frame revision left DCLF's scene on old versions, so the engine draws until DCLF's next scene", SceneStep());
	}

	void FrameView::PublishFrameClaims(std::shared_ptr<const SetSnapshot> a_claims)
	{
		PassCapture::Get().PublishSet(a_claims);
		std::atomic_store(&frameClaims, std::move(a_claims));
	}

	void FrameView::ReturnPublication(std::shared_ptr<const ScenePublication>&& a_publication)
	{
		// The coordinator drops it, so nothing is retired on the render thread. Only the coordinator writes the table pool.
		channels.retiredPublications.Push(std::move(a_publication));
	}

	void FrameView::DeliverPrimaryNotes()
	{
		auto& primary = PrimaryCull::Get();
		channels.primaryNotes.Drain([&primary](PrimaryNote&& a_note) {
			switch (a_note.kind) {
			case PrimaryNote::Kind::HiddenKey:
				primary.NoteHiddenKey(a_note.key);
				break;
			case PrimaryNote::Kind::MemberLost:
				primary.NoteMemberLost(static_cast<const RE::BSGeometry*>(a_note.key));
				break;
			case PrimaryNote::Kind::AllMembersLost:
				primary.NoteAllMembersLost();
				break;
			}
		});
	}

	std::vector<ShadingItem> FrameView::PeekShadingItems() const
	{
		std::vector<ShadingItem> items;
		using Log = org::async::PublicationLog<PublicationDeltas>;
		for (const Log::Node* next = Log::Next(deltasCursor ? deltasCursor : channels.publicationLog.First()); next; next = Log::Next(const_cast<Log::Node*>(next)))
			items.insert(items.end(), next->value.shading.begin(), next->value.shading.end());
		return items;
	}

	const SetSnapshot* FrameView::AdoptedClaims() const
	{
		// Before the first installation the engine draws everything.
		return !NotDrawingYet() && installed && installed->claims ? installed->claims.get() : nullptr;
	}

	void FrameView::ClearAdoptedDeltas()
	{
		for (auto& deltas : frameComponentDeltas)
			deltas.clear();
	}

	bool FrameView::IsLoadingScreenUp()
	{
		auto* ui = CS_ENGINE_READ(UI, Other, RE::UI::GetSingleton());
		return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) && !LoadHold::MenuHeld();
	}

	bool FrameView::LoadingScreenInFrame()
	{
		return LoadingScreenIn(Snapshot::Current().get());
	}

	bool FrameView::LoadingScreenIn(const Snapshot::FrameSnapshot* a_frame)
	{
		if (!::RenderGraph::Engine::Native::Active())
			return IsLoadingScreenUp();
		return a_frame && a_frame->menus.loading && !LoadHold::MenuHeld();
	}

	bool FrameView::LoadShown()
	{
		return LoadShownIn(Snapshot::Current().get());
	}

	bool FrameView::LoadShownIn(const Snapshot::FrameSnapshot* a_frame)
	{
		return LoadingScreenIn(a_frame) || LoadHold::Active() || LoadHold::MenuHeld();
	}

	bool FrameView::LoadShownNow()
	{
		return IsLoadingScreenUp() || LoadHold::Active() || LoadHold::MenuHeld();
	}

	bool FrameView::NoteLoadingScreen(bool a_loading)
	{
		// Switch events are taken on the render thread only.
		switchEventThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		// Nothing may walk the scene graph while a load screen is up. A load tears down and rebuilds TES::objRoot, so a live walk
		// gives a stale or garbage pointer. No table is written while the load is up.
		if (!a_loading) {
			if (std::exchange(loadingPosted, false)) {
				channels.loadingSeen.store(false, std::memory_order_release);
				SetScenePassLoading(false);
				WakeScenePass(SceneWake::LoadDrain);
			}
			return false;
		}
		if (!std::exchange(loadingPosted, true)) {
			channels.loadingSeen.store(true, std::memory_order_release);
			// The first category capture after a load is forced, and the job from before it is not resumed since it names the torn-down world.
			loadSinceCapture = true;
			categoryCapturePending = true;
			worldCatchUpPending = true;
			switchResyncNext = true;
		}
		// The set does not survive a load. It names geometry from the cell being torn down.
		PublishFrameClaims(nullptr);
		// A load pushes thousands of events a frame, so they wake no pass. One pass applies them once a Present and frame start.
		SetScenePassLoading(true);
		WakeScenePass(SceneWake::LoadDrain);
		return true;
	}

	std::span<RE::BSGeometry* const> FrameView::NewestObjectGeometry() const
	{
		const auto* publication = InstalledPublication();
		const Tables* source = publication ? publication->tables.get() : nullptr;
		return source ? std::span<RE::BSGeometry* const>(source->objectGeometry) : std::span<RE::BSGeometry* const>{};
	}
}

namespace DCLF
{
	bool InNativeGameHalf()
	{
		return Scene::FrameView::InGameHalf();
	}
}
