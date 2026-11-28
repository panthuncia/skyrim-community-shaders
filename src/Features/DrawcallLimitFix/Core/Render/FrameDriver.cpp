// FrameDriver: the render thread's DCLF work at each engine point of the frame.

#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameTicket.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Native/RenderThread.h"
#include "RenderGraph/Engine/Native/WorldPass.h"
#include "Features/DrawcallLimitFix/Core/Handoff/WholeCull.h"
#include "Features/DrawcallLimitFix/Core/Render/LoadHold.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"

#include "Deferred.h"
#include "Features/DrawcallLimitFix.h"
#include "Globals.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "Testing/Testing.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Scene.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameData.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/Segments.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/LatePoses/LatePoseOverrides.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PaletteRows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PrimaryView.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowEpochs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowPoints.h"
#include "RenderGraph/Async/Retirement.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"
#include "Snapshot/Retire.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Publication.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		double MillisecondsSince(std::chrono::steady_clock::time_point a_start)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
		}
	}

	namespace
	{
		// The frame whose build point already ran at Renderer::Begin.
		std::uint64_t s_buildPointFrame = 0;
		// DCLF's scene source (RegisterSceneSource).
		RenderGraph::Frame::Revision::FragmentSourceId s_sceneSource = RenderGraph::Frame::Revision::kMaxFragmentSources;
		// Main thread: IndirectDraws::SceneRecordedFrames at the last Begin.
		std::uint64_t s_sceneRecordedSeen = 0;
	}

	// Scene/FrameView/FrameView.cpp. Whether DCLF's first claims are installed. Render thread.
	bool DrawsWorld();

	namespace
	{
		/**
		 * @brief Render thread, inside a revision's adoption point, after its versions. A resize points the engine's target views at new
		 * versions and keeps the views, so DCLF's imports of them still name the old images (the shadow mask every colour draw reads:
		 * black lighting, then garbage once the old memory is reused). Their imports are superseded, so every binding looks them up again.
		 */
		void FollowEngineTargetVersions()
		{
			namespace Vars = RenderGraph::Frame::Vars;
			static RenderGraph::Engine::Targets::ShapeTable s_seen{};
			const auto table = Vars::Adopted(Vars::EngineTargetShapes());
			if (table == s_seen)
				return;
			if (auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)) {
				auto& textures = GpuTextures::Get();
				auto& colours = renderer->GetRuntimeData().renderTargets;
				for (std::size_t i = 0; i < std::size(colours) && i < table.colours.size(); ++i)
					if (!(table.colours[i] == s_seen.colours[i])) {
						textures.NoteViewChanged(colours[i].SRV);
						textures.NoteViewChanged(colours[i].SRVCopy);
					}
				auto& depths = renderer->GetDepthStencilData().depthStencils;
				for (std::size_t i = 0; i < std::size(depths) && i < table.depths.size(); ++i)
					if (!(table.depths[i] == s_seen.depths[i])) {
						textures.NoteViewChanged(depths[i].depthSRV);
						textures.NoteViewChanged(depths[i].stencilSRV);
					}
			}
			s_seen = table;
		}
	}

	void RegisterSceneSource()
	{
		namespace Revision = RenderGraph::Frame::Revision;
		Revision::FragmentSource source;
		source.name = "DCLF's scene";
		// The GPU traces sum an epoch under the feature its label names.
		const auto label = [](RenderGraphRuntime::Segment a_segment) -> std::pair<std::string, std::string> {
			using S = RenderGraphRuntime::Segment;
			switch (a_segment) {
			case S::ZPrepass:
				return { "CS DCLF: Z-prepass", "CS DCLF: Z-prepass inputs" };
			case S::MainOpaque:
				return { "CS DCLF: main opaque", "CS DCLF: main opaque inputs" };
			case S::ShadowView:
				return { "CS DCLF: shadow view", "CS DCLF: shadow view inputs" };
			case S::SkyOcclusion:
				return { "CS DCLF: Skylighting occlusion", "CS DCLF: Skylighting occlusion inputs" };
			case S::Reflection:
				return { "CS DCLF: reflection faces", "CS DCLF: reflection faces inputs" };
			case S::Forward:
				return { "CS DCLF: forward", "CS DCLF: forward inputs" };
			case S::FirstPersonDepth:
				return { "CS DCLF: first-person depth", "CS DCLF: first-person depth inputs" };
			case S::FirstPerson:
				return { "CS DCLF: first person", "CS DCLF: first-person inputs" };
			case S::WaterMask:
				return { "CS DCLF: water mask", "CS DCLF: water mask inputs" };
			case S::WaterSurface:
				return { "CS DCLF: water surface", "CS DCLF: water surface inputs" };
			default:
				return { "CS DCLF", "CS DCLF: inputs" };
			}
		};
		for (std::uint32_t segment = 0; segment < static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count); ++segment)
			if (IsDclfSegment(static_cast<RenderGraphRuntime::Segment>(segment))) {
				source.segments.push_back(segment);
				source.labels.push_back(label(static_cast<RenderGraphRuntime::Segment>(segment)));
			}
		source.taken = [](const std::shared_ptr<const void>& a_scene) { IndirectDraws::Get().SceneTaken(a_scene); };
		source.adopt = [](const std::shared_ptr<const void>& a_scene) { IndirectDraws::Get().InstallScene(a_scene); };
		source.drop = [](const std::shared_ptr<const void>& a_scene, Revision::FragmentDrop a_why, std::string_view a_text, std::span<const std::uint32_t> a_epochs) {
			IndirectDraws::Get().DropScene(a_scene, a_why, a_text, a_epochs);
		};
		// Whether DCLF draws and a frame's coverage ran a DCLF epoch since the last Begin. A menu without a scene pass or a load screen
		// posts no scene, so a revision with DCLF's epochs adopts without one then, and DCLF stops (left).
		source.drawing = [] {
			const std::uint64_t runs = IndirectDraws::Get().SceneRecordedFrames();
			const bool ran = std::exchange(s_sceneRecordedSeen, runs) != runs;
			return ran && !Scene::FrameView::Get().NotDrawingYet();
		};
		// A revision left DCLF's scene on old versions. DCLF stops drawing here, at the adoption point, until its next scene covers a frame.
		source.left = [](std::uint64_t) {
			Scene::FrameView::Get().StopDrawing();
			FrameDriver::Get().NoteStoppedDrawing();
		};
		// What a Begin adopts of DCLF's scene. A routed render (native's render half) adopts exactly its ticket's fragment, at the ticket's
		// first render; a repeat adopts nothing. A loading screen's or movie's frame leaves the scene to a later Begin. Any other frame
		// (guest, native's frames that render on the producer) takes the newest posted one, and its game half installs what was adopted.
		source.take = [](std::optional<Revision::Fragment>& a_given) { return FrameDriver::Get().TakeAtBegin(a_given); };
		// The graph conditions for the adopted variables, after the adoption point's revision work.
		source.afterAdoption = [] {
			TracyCZoneN(afterScene, "CS.DCLF.AfterSceneStep", true);
			FrameDriver::Get().NoteAdopted(IndirectDraws::Get().AdoptedPublication());
			IndirectDraws::Get().AfterSceneStep();
			// The game half's family decisions read the revision adopted here.
			IndirectDraws::Get().PublishFamilyRenderState();
			TracyCZoneEnd(afterScene);
		};
		// Native mode with segments: the build point at Renderer::Begin, before the frame's schedule is chosen. BeginFrame then skips it.
		source.frameStart = [](std::uint64_t a_frame) {
			IndirectDraws::Get().BuildPoint();
			s_buildPointFrame = a_frame;
		};
		source.drawsWorld = [] { return DrawsWorld(); };
		source.epochFailed = [](std::string_view a_label, std::string_view a_error) { DrawcallLimitFix::OnGraphEpochFailed(a_label, std::string(a_error)); };
		s_sceneSource = Revision::RegisterFragmentSource(std::move(source));
		Revision::OnVersionsAdopted([] { FollowEngineTargetVersions(); });
	}

	namespace
	{
		std::array<TicketPartCapture, kTicketParts> s_ticketParts{};  // install
		std::array<TicketPartFold, kTicketParts> s_ticketPartFolds{};  // install
	}

	void RegisterTicketPart(TicketPart a_part, TicketPartCapture a_capture, TicketPartFold a_fold)
	{
		s_ticketParts[static_cast<std::size_t>(a_part)] = a_capture;
		s_ticketPartFolds[static_cast<std::size_t>(a_part)] = a_fold;
	}

	const void* TicketPartOf(TicketPart a_part)
	{
		const auto* ticket = FrameDriver::Get().Ticket();
		return ticket ? ticket->parts[static_cast<std::size_t>(a_part)].get() : nullptr;
	}

	std::shared_ptr<const void> TicketPartHeld(TicketPart a_part)
	{
		const auto* ticket = FrameDriver::Get().Ticket();
		return ticket ? ticket->parts[static_cast<std::size_t>(a_part)] : nullptr;
	}

	bool TicketUtilityShader()
	{
		const auto* ticket = FrameDriver::Get().Ticket();
		return ticket && ticket->utilityShader;
	}

	RenderGraph::Frame::Revision::FragmentSourceId SceneSource()
	{
		return s_sceneSource;
	}

	void IndirectDraws::NoteInputChanged(std::string_view a_what)
	{
		RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::ClientInputChanged, RenderGraph::Frame::SegmentId::Count,
			fmt::format("DCLF: {}", a_what));
	}

	FrameDriver& FrameDriver::Get()
	{
		static FrameDriver driver;
		return driver;
	}

	namespace
	{
		/** @brief The sequence of the publication the frame revision adopted last, 0 for none. Render owner. */
		std::uint64_t AdoptedSequence()
		{
			const auto adopted = std::static_pointer_cast<const ScenePublication>(IndirectDraws::Get().AdoptedPublication());
			return adopted ? adopted->sequence : 0;
		}
	}

	RenderGraph::Frame::Revision::FragmentTake FrameDriver::TakeAtBegin(std::optional<RenderGraph::Frame::Revision::Fragment>& a_given)
	{
		using Take = RenderGraph::Frame::Revision::FragmentTake;
		if (expected) {
			// The ticket's scene goes to the revision once; a repeat of the ticket takes nothing.
			if (!expected->sceneHanded.exchange(true, std::memory_order_acq_rel))
				a_given = std::exchange(expected->scene, std::nullopt);
			return Take::Given;
		}
		const auto& snapshot = Snapshot::Current();
		if (snapshot && (snapshot->kind == Snapshot::FrameKind::Loading || snapshot->kind == Snapshot::FrameKind::Movie))
			return Take::Skip;
		// A repeated render keeps the scene its ticket installed.
		if (RepeatsTicket())
			return Take::Given;
		return Take::Newest;
	}

	bool FrameDriver::RepeatsTicket() const
	{
		return ticket && Snapshot::Render().repeat && ticket->snapshot == Snapshot::CurrentId();
	}

	void FrameDriver::NoteAdopted(std::shared_ptr<const void> a_publication)
	{
		renderAdopted.store(std::move(a_publication), std::memory_order_release);
	}

	void FrameDriver::NoteStoppedDrawing()
	{
		coveredLast.store(0, std::memory_order_release);
	}

	void FrameDriver::SceneFrameStart()
	{
		// The posts below wake the coordinator once, when this scope ends.
		ScenePassWakeBatch wakes;
		// Both halves at the frame start point, after the frame's Begin (guest, and native's frames the engine's skeleton draws). A repeated
		// render of a snapshot renders its ticket again: the scene source took no new scene at its Begin, so the publication stays the
		// ticket's. A frame revision adopted with a held scene can still bring a newer one: the ticket is made again.
		const bool repeat = RepeatsTicket();
		if (!repeat || AdoptedSequence() > ticket->publication) {
			if (repeat)
				ticketsRemade.fetch_add(1, std::memory_order_relaxed);
			MakeTicket(Snapshot::LastGameId(), false);
			RenderTicket(gameTicket);
		}
		// Native: the rest of the driver's game half reads the update's globals, not the render side's adopted ones.
		if (Scene::FrameView::InGameHalf())
			FrameGlobals::BeginGameHalf(ticket->globals);
		BeginFrame();
	}

	void FrameDriver::SceneGameStart()
	{
		ScenePassWakeBatch wakes;
		MakeTicket(Snapshot::LastGameId(), true);
		// The rest of the driver's game half reads the update's globals, not the render side's adopted ones.
		if (Scene::FrameView::InGameHalf())
			FrameGlobals::BeginGameHalf(gameTicket->globals);
	}

	void FrameDriver::TicketSuperseded(const FrameTicket& a_old, const FrameTicket& a_fresh)
	{
		// The parts every update adds to carry the old ticket's updates, which no render will see otherwise.
		for (std::size_t part = 0; part < kTicketParts; ++part)
			if (const auto fold = s_ticketPartFolds[part])
				a_fresh.parts[part] = fold(a_old.parts[part], a_fresh.parts[part]);
		// Neither ticket reached a render yet: the game thread owns both scenes until the publish. A revision may wait for exactly the
		// old one's fragment, so the fresh ticket takes it when it brought none; a fresh one supersedes it.
		if (a_old.sceneHanded.exchange(true, std::memory_order_acq_rel) || !a_old.scene)
			return;
		if (!a_fresh.scene) {
			a_fresh.scene = std::exchange(a_old.scene, std::nullopt);
			ticketScenesCarried.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void FrameDriver::ExpectTicket(std::shared_ptr<const FrameTicket> a_ticket)
	{
		expected = std::move(a_ticket);
	}

	void FrameDriver::SceneRenderStart()
	{
		if (!expected)
			::RenderGraph::Fatal("Drawcall Limit Fix", "a routed render's frame start without its ticket");
		RenderTicket(std::exchange(expected, nullptr));
		BeginFrame();
	}

	void FrameDriver::RenderTicket(std::shared_ptr<const FrameTicket> a_ticket)
	{
		if (ticket == a_ticket)
			return;
		// The render side's reference retires through the engine lane: it may be the ticket's last (its late-pose plan holds engine objects).
		if (ticket)
			Snapshot::Retire::AfterThisFrame(Snapshot::Retire::Lane::Engine, std::move(ticket));
		ticket = std::move(a_ticket);
	}

	void FrameDriver::UnrenderedUpdate(std::uint64_t a_snapshot)
	{
		ScenePassWakeBatch wakes;
		MakeTicket(a_snapshot, false);
	}

	void FrameDriver::MakeTicket(std::uint64_t a_snapshot, bool a_takeScene)
	{
		auto previous = std::move(gameTicket);
		gameTicket = GameFrameStart(a_snapshot, a_takeScene);
		ticketsMade.fetch_add(1, std::memory_order_relaxed);
		if (!previous)
			return;
		// It may hold engine references (the late-pose plan), so it goes to the engine lane, after the newest frame that may have rendered
		// it. A ticket no render took retires unused; its sampling job ran all the same.
		if (!previous->rendered.load(std::memory_order_acquire))
			ticketsUnrendered.fetch_add(1, std::memory_order_relaxed);
		Snapshot::Retire::AfterThisFrame(Snapshot::Retire::Lane::Engine, std::move(previous));
	}

	std::shared_ptr<const FrameTicket> FrameDriver::GameFrameStart(std::uint64_t a_snapshot, bool a_takeScene)
	{
		// The game half reads and writes the engine once per game update: its reads are the update's capture.
		const Snapshot::EngineReads::CaptureScope capture;
		auto t = std::make_shared<FrameTicket>();
		t->snapshot = a_snapshot;
		// Start a load's hold before anything checks whether a load screen is up.
		LoadHold::OnFrameStart();
		Diagnostics::Notify(Diagnostics::FrameBegin{});
		auto& scene = Scene::Coordinator::Get();
		auto& view = Scene::FrameView::Get();
		auto& draws = IndirectDraws::Get();
		// The engine only reads its scene graph from here to Present, so workers may read it.
		EngineReadWindow::Open();
		scene.SetScenePassMode(true);
		// A classify input change drops the cached verdicts, because the caches witness the objects, not the inputs.
		static Extensions::ClassifyInputSet inputsSeen = Extensions::SampleClassifyInputs();
		if (const auto inputs = Extensions::SampleClassifyInputs(); inputs != std::exchange(inputsSeen, inputs))
			view.RequestVerdictsInvalidated();
		ScopedPerfEvent event("CS DCLF: scene tables");
		{
			DCLF_ZONE("CS.DCLF.BF.ViewBeginFrame");
			RenderThreadBudget::Part captures(RenderThreadBudget::Bucket::SceneCaptures);
			view.BeginFrame();
		}
		t->globals = view.FrameGlobalsOfFrame();
		t->sceneStep = view.SceneStep();
		// The game half's readers see the update's globals.
		const FrameGlobals::Scope globals(t->globals);
		TracyCZoneN(bfCamera, "CS.DCLF.BF.Camera", true);
		PrimaryCull::Get().BeginFrameCamera();
		Extensions::PaletteRowsFrameStart();
		TracyCZoneEnd(bfCamera);
		// Load screen frames have no claims, so they are never waited for or required to be covered.
		t->loading = Scene::FrameView::IsLoadingScreenUp();
		t->utilityShader = CS_ENGINE_READ(Shaders, CapturePoint, globals::game::utilityShader) != nullptr;
		{
			// The publication the frame revision adopted at this frame's Begin, with the staged builds it was recorded on.
			// Nothing waits here: a newer snapshot not ready yet leaves the adopted one, recorded on the graphs that run.
			// A routed update runs before its render's Begin: it takes the newest posted fragment into the ticket, whose render adopts it,
			// and installs its publication (none new: the publication the render side adopted last).
			DCLF_ZONE("CS.DCLF.BF.Adopt");
			RenderThreadBudget::Part exchange(RenderThreadBudget::Bucket::Exchange);
			std::shared_ptr<const void> publication;
			if (a_takeScene) {
				t->scene = RenderGraph::Frame::Revision::TakePostedFragment(s_sceneSource);
				publication = t->scene ? draws.PublicationOf(t->scene->value) : renderAdopted.load(std::memory_order_acquire);
			} else
				publication = draws.AdoptedPublication();
			view.HandOverAtFrameStart(publication);
		}
		t->publication = view.InstalledSequence();
		// The views' game work: what they read of the engine, for the render half's view start (a ticket part).
		RunViewWork(Api::ViewPoint::FrameStartGame);
		// Entries changed since the sun exclusion's snapshot leave it, and the engine culls them.
		Extensions::ShadowExclusionFrameStart();
		{
			DCLF_ZONE("CS.DCLF.BF.Claims");
			// While DCLF draws, the installed publication's claims install. The first installation, and the first after a stop, waits for a
			// render whose coverage of DCLF's claims epochs passed, after the last stop: until then the engine draws everything. The render
			// half checks coverage.
			RenderThreadBudget::Part exchange(RenderThreadBudget::Bucket::Exchange);
			if (view.HasInstalled() && (!view.NotDrawingYet() || coveredLast.load(std::memory_order_acquire) == view.StopsApplied() + 1))
				view.InstallClaims();
			Reports::GateReleases(view.SceneStep());
			// Release the LOD gates the installed publications flipped, before the culls.
			Extensions::ComponentsAdopt(view.SceneStep(), view.AdoptedClaims(), view.AdoptedDeltas());
			view.ClearAdoptedDeltas();
		}
		// Serve what the coordinator asked of the frame start, outside load screens. Switch catch-ups write engine memory, so they go before the culls.
		{
			const auto eventsStart = std::chrono::steady_clock::now();
			if (!view.NoteLoadingScreen(t->loading))
				view.ServeFrameRequests();
			counters.timing.eventsMs += MillisecondsSince(eventsStart);
		}
		// Native answers the scene's material requests here, on the game side (guest at EarlyPrepass).
		if (RenderGraph::Engine::Native::Active()) {
			DCLF_ZONE("CS.DCLF.BF.ServeMaterialRequests");
			view.ServeMaterialRequests();
		}
		TracyCZoneN(bfFamilies, "CS.DCLF.BF.SourcesAndFamilies", true);
		{
			DCLF_ZONE("CS.DCLF.BF.ShadowSourcesFrameStart");
			Extensions::ShadowSourcesFrameStart(view.SceneStep());
		}
		{
			DCLF_ZONE("CS.DCLF.BF.RefreshMainRenderers");
			PassCapture::Get().RefreshMainRenderers();
		}
		// Draw families decide before the main camera's cull registers its passes, and before the frame values' inputs are taken.
		{
			DCLF_ZONE("CS.DCLF.BF.DecideDrawFamilies");
			draws.DecideDrawFamilies(view.SceneStep());
		}
		{
			DCLF_ZONE("CS.DCLF.BF.PublishListFilter");
			PrimaryCull::Get().PublishListFilter();
		}
		TracyCZoneEnd(bfFamilies);
		{
			DCLF_ZONE("CS.DCLF.BF.TakeFrameValueInputs");
			// The capture reads the shading changes the take empties.
			auto fields = Extensions::CaptureShadingFields(view);
			t->latePlan = LatePoses::PrepareFrame();
			auto plans = view.TakePlacementPlans();
			auto shading = view.TakeShadingItems();
			// First person's rows and palettes must be taken before Main::Draw moves the skeleton into its root's space. Sampled whether
			// or not this frame's render covers the root views: a render that draws them must find them.
			bool rootViews = false;
			for (std::uint32_t i = 0; i < Extensions::Views().rootCount; ++i)
				rootViews = rootViews || Extensions::ViewRegistered(Extensions::Views().rootViews[i]);
			const auto claims = rootViews ? view.FrameClaims() : nullptr;
			auto samples = FrameValues::Get().TakeRootViews(plans, claims.get());
			// The ticket's sampling job, rendered or not. Every render's upload job comes after it on the frame values' chain.
			FrameValues::Get().KickSample(std::move(plans), std::move(shading), std::move(fields), t->globals, std::move(samples));
		}
		// Native's game half: what it installed, for its renders.
		if (Scene::FrameView::InGameHalf())
			t->installed = view.TakeInstalledFrame();
		// The features' values the render half reads instead of the engine.
		for (std::size_t part = 0; part < kTicketParts; ++part)
			if (const auto take = s_ticketParts[part]) {
				DCLF_ZONE("CS.DCLF.BF.TicketParts");
				t->parts[part] = take();
			}
		return t;
	}

	void FrameDriver::BeginFrame()
	{
		// The render half reads the ticket, the snapshot and CS's frame. The live-read detector counts any engine read here.
		const Snapshot::EngineReads::FrameStartScope frameStartReads;
		const auto& t = *ticket;
		const bool first = !t.rendered.exchange(true, std::memory_order_acq_rel);
		// The render draws DCLF's work only when the frame revision adopted the publication the ticket installed. A routed render whose
		// ticket's fragment was held for a revision still in preparation, or dropped, draws none.
		const bool paired = AdoptedSequence() == t.publication;
		if (!paired)
			ticketsUnpaired.fetch_add(1, std::memory_order_relaxed);
		occlusionWanted = occlusionDrawn = occlusionTaken = 0;
		// An extension added or removed last frame is built here, before anything decides what the frame's recordings cover.
		// In native mode with segments this already ran at Renderer::Begin, and no build may attach after that.
		if (s_buildPointFrame != RenderGraph::Frame::Driver::Current().frame)
			IndirectDraws::Get().BuildPoint();
		auto& view = Scene::FrameView::Get();
		auto& draws = IndirectDraws::Get();
		// Each render has its own scene step: the epochs' once-a-frame guards and commits are keyed by it.
		view.StepRender(t.sceneStep, first);
		// Native: the render side reads what its ticket's game half installed, not what a newer game half installs meanwhile.
		view.AdoptInstalledFrame(t.installed);
		FrameGlobals::Adopt(t.globals);
		{
			DCLF_ZONE("CS.DCLF.BF.SyncFrameTables");
			view.SyncFrameTables();
		}
		draws.PostLookupInputs();
		// Native: the draw families' mirrors are drained on the render side, whose commits read them (guest drained them in its game half).
		if (t.installed) {
			DCLF_ZONE("CS.DCLF.BF.ServeDrawFamilies");
			Extensions::ServeDrawFamilies(view.SceneStep());
		}
		// Views start before the coverage, which decides whether their members are withheld.
		RunViewWork(Api::ViewPoint::FrameStartBeforeCoverage);
		{
			DCLF_ZONE("CS.DCLF.BF.Coverage");
			// While DCLF does not draw (before its first covered frame, or after an adoption stopped it) the engine draws everything. While
			// it draws, an uncovered frame is a tap error, because the adopted snapshot stays current until a Begin adopts a newer one with
			// its builds. Load screen frames are the engine's (EngineState::LoadScreen).
			RenderThreadBudget::Part exchange(RenderThreadBudget::Bucket::Exchange);
			const bool covered = draws.DecideCoverage();
			coveredLast.store(covered ? view.StopsAsked() + 1 : 0, std::memory_order_release);
			if (!(covered && view.HasInstalled()) && !view.NotDrawingYet() && !t.loading && paired) {
				// The graph itself stopped, so no snapshot can cover a frame again and that reason is the defect.
				// "GPU completion timeline failed" means a timeline read UINT64_MAX, which Windows sets on a removed device's fences.
				// A lost device ends the game here with its reason logged. A fatal message box would hide behind the full-screen window.
				const HRESULT removed = globals::d3d::device ? globals::d3d::device->GetDeviceRemovedReason() : S_OK;
				if (removed != S_OK || Testing::DeviceLossSeen()) {
					const auto reason = fmt::format("Drawcall Limit Fix: the GPU device was lost (D3D11's removed reason {:#x}, BasicRHI saw it {}) before frame {}; "
													"the game ends. The GPU work it was running is in the log above (crash markers with DXVK_DEBUG=hang, or the hang "
													"report's stream order)",
						static_cast<std::uint32_t>(removed), Testing::DeviceLossSeen(), view.SceneStep());
					logger::critical("{}", reason);
					Testing::ReportDeviceLost("D3D11 device removed at DCLF's frame start", 0, "", "");
					spdlog::default_logger()->flush();
					::TerminateProcess(::GetCurrentProcess(), 0xDEAD0007u);
				}
				if (auto& runtime = RenderGraphRuntime::Get(); !runtime.IsActive())
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the render graph stopped before frame {} ({}), and DCLF draws only through it once it "
													 "draws. D3D11's device removed reason: {:#x}",
						view.SceneStep(), runtime.GetDisabledReason(), static_cast<std::uint32_t>(globals::d3d::device->GetDeviceRemovedReason())));
				RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::Uncovered, RenderGraph::Frame::SegmentId::Count,
					fmt::format("DCLF: frame {} is not covered by the scene it adopted at its Begin: {}", view.SceneStep(), draws.CoverageState()));
			}
		}
		TracyCZoneN(bfKicks, "CS.DCLF.BF.AheadAndKicks", true);
		{
			DCLF_ZONE("CS.DCLF.BF.PostAheadContext");
			draws.PostAheadContext();
		}
		// Tasks on DCLF's executor that are never joined.
		{
			DCLF_ZONE("CS.DCLF.BF.RootStatesKick");
			Extensions::RootStatesKick();
		}
		{
			DCLF_ZONE("CS.DCLF.BF.KickVisibilityReadback");
			draws.KickVisibilityReadback();
		}
		TracyCZoneEnd(bfKicks);
		{
			DCLF_ZONE("CS.DCLF.BF.ViewWorkAfterTreeLod");
			RunViewWork(Api::ViewPoint::FrameStartAfterTreeLod);
		}
		{
			DCLF_ZONE("CS.DCLF.BF.FrameValuesKick");
			// Producers' uploads may only wait for CPU frames signalled before the last present.
			::RenderGraph::Frame::Retirement::SealCpuFrames();
			FrameValues::Get().SetLatePoses(t.latePlan);
			// The render's upload of what the ticket's sampling job (and any before it) left. A repeated render uploads again.
			(void)FrameValues::Get().Kick(t.globals);
		}
		{
			DCLF_ZONE("CS.DCLF.BF.MakeFramePayload");
			(void)draws.MakeFramePayload();
		}
	}

	void FrameDriver::DepthCommitted()
	{
		Extensions::TriggerScope trigger(Draws::CoreTriggerOf(Draws::CoreTrigger::DepthCommitted));
		// Late poses are parts a plugin hid after the shadow maps. Confirm them before the depth epoch, so later epochs read the late copies.
		// Native confirmed them at the end of its game half, into the ticket; only the ticket's first depth pass uses them.
		const bool nativeLate = ticket && ticket->latePosesConfirmed && std::exchange(latePosesStep, ticket->sceneStep) != ticket->sceneStep;
		if (RenderGraph::Engine::Native::Active() ? nativeLate : LatePoses::ConfirmFrame())
			FrameValues::Get().UseLatePoses();
		// The depth epoch hosts the view passes, such as reflection faces and first-person depth, so they draw after its index pool copies.
		// Drawn against the previous frame's pool, a reused slot would read old indices with new vertices and fault the device.
		// A frame whose depth epoch did not commit runs the view work after, with stale inputs, and draws nothing.
		bool ran = false;
		const std::function<void()> work = [&] {
			ran = true;
			RunViewWork(Api::ViewPoint::DepthCommitted);
		};
		IndirectDraws::Get().CaptureDepthPass(work);
		if (!ran)
			RunViewWork(Api::ViewPoint::DepthCommitted);
	}

	void FrameDriver::RunViewWork(Api::ViewPoint a_point)
	{
		const auto& frame = Scene::FrameView::Get();
		const Api::ViewPointContext context{ frame.SceneStep(), &frame.GetTables(), FrameCatalog() };
		const auto point = static_cast<std::size_t>(a_point);
		for (const auto& view : Extensions::Views().views)
			for (const auto run : view.work[point])
				run(context);
	}

	void FrameDriver::BeforePresent()
	{
		// Queue the frame signal before the present. EndFrame runs after the present and would ride the next frame's command list.
		Direct::QueueFrameSignal();
		FrameValues::Get().WaitSignalSubmitted();
		// Native checks at the update's end, on the game side.
		if (!RenderGraph::Engine::Native::Active())
			LatePoses::CheckAtPresent();
	}

	void FrameDriver::GameHalfStart()
	{
		Scene::FrameView::SetGameHalf(true);
	}

	void FrameDriver::GameHalfEnd()
	{
		const Snapshot::EngineReads::CaptureScope capture;
		const bool confirmed = LatePoses::ConfirmFrame();
		if (gameTicket)
			gameTicket->latePosesConfirmed = confirmed;
		Scene::FrameView::SetGameHalf(false);
		FrameGlobals::EndGameHalf();
	}

	void FrameDriver::UpdateEnd()
	{
		if (!RenderGraph::Engine::Native::Active())
			return;
		const Snapshot::EngineReads::CaptureScope capture;
		LatePoses::CheckAtPresent();
	}

	void FrameDriver::EndFrame()
	{
		LoadHold::OnFrameEnd();
		// Guest: the engine's next update jobs follow Present, so no worker may be inside its memory from here until the next game half.
		// Native closes at the end of the update's renders, on the game side (Snapshot's update hook).
		if (!RenderGraph::Engine::Native::Active()) {
			RenderThreadBudget::Part boundary(RenderThreadBudget::Bucket::EngineBoundary);
			EngineReadWindow::CloseAtPresent();
		}
		const auto start = std::chrono::steady_clock::now();
		TracyCZoneN(efCull, "CS.DCLF.EF.PrimaryCull", true);
		PrimaryCull::Get().EndFrame();
		TracyCZoneEnd(efCull);
		// End the CPU pipeline's frame first, so the retirement points bound below cover it.
		Direct::EndFrame();
		TracyCZoneN(efBind, "CS.DCLF.EF.BindAdoptedAndValues", true);
		// Keep what the frame read from reuse until its GPU work retires.
		IndirectDraws::Get().BindAdopted();
		FrameValues::Get().EndFrame();
		FrameData::EndFrame();
		TracyCZoneEnd(efBind);
		TracyCZoneN(efReports, "CS.DCLF.EF.Reports", true);
		// Once per frame number. Menus and loads hold the number still while Presents continue.
		static std::uint32_t reportedFrame = ~0u;
		if (const std::uint32_t frame = Scene::FrameView::Get().SceneStep(); std::exchange(reportedFrame, frame) != frame)
			Reports::RenderThread(frame);
		// Native's render thread leaves the load screen note to the game half (each ticket's), which owns the scene pass's load state
		// and the switch events' thread.
		if (!RenderGraph::Engine::Native::RenderThread::OnRenderThread())
			Scene::FrameView::Get().NoteLoadingScreen(Scene::FrameView::LoadingScreenInFrame());
		TracyCZoneEnd(efReports);
		counters.timing.eventsMs += MillisecondsSince(start);
	}

	void FrameDriver::AddShadowViewsTime(std::chrono::steady_clock::time_point a_start)
	{
		shadowViewsMs += MillisecondsSince(a_start);
	}

	void FrameDriver::BeforeShadowCasters()
	{
		const auto start = std::chrono::steady_clock::now();
		shadowViewsMs = 0.0;
		// Decide shadow coverage before the engine keeps its first light, since its registrations and the sun's claims depend on it.
		Draws::BeginShadowViews();
		// Native's game half lists the views only. The render side decides their coverage on its copy (AdoptShadowViews).
		if (const auto* epochs = Extensions::ShadowEpochs(); epochs && !Scene::FrameView::InGameHalf())
			epochs->beforeShadowCasters();
		AddShadowViewsTime(start);
	}

	void FrameDriver::ShadowCasterKept(const RE::BSShadowLight* a_light, std::uint32_t a_firstSlot, std::uint32_t a_slots)
	{
		const auto start = std::chrono::steady_clock::now();
		Draws::ShadowLightKept(a_light, a_firstSlot, a_slots);
		AddShadowViewsTime(start);
	}

	void FrameDriver::ShadowFocusAccumulating(const RE::BSShadowLight* a_light, bool a_begin)
	{
		const auto start = std::chrono::steady_clock::now();
		Draws::ShadowFocusAccumulating(a_light, a_begin);
		AddShadowViewsTime(start);
	}

	void FrameDriver::ShadowCastersSelected()
	{
		const auto start = std::chrono::steady_clock::now();
		// Listed in the order the engine renders them.
		Draws::ListShadowViews();
		AddShadowViewsTime(start);
	}

	void FrameDriver::BeforeShadowMaps()
	{
		ScopedPerfEvent event("CS DCLF: shadow views");
		const auto start = std::chrono::steady_clock::now();
		Draws::NoteShadowRevisionInputs();
		if (const auto* epochs = Extensions::ShadowEpochs())
			epochs->beforeShadowMaps();
		const double ms = std::exchange(shadowViewsMs, 0.0) + MillisecondsSince(start);
		counters.timing.sceneMs += ms;
		counters.timing.sceneMaxMs = std::max(counters.timing.sceneMaxMs, ms);
	}

	void FrameDriver::AfterShadowMaps()
	{
		// One epoch draws the shadow views and the occlusion maps taken before the engine drew the shadow maps.
		const std::uint32_t wanted = std::exchange(occlusionWanted, 0u);
		occlusionTaken = wanted;
		PassCapture::Get().SetOcclusionPhase(0);
		const auto* epochs = Extensions::ShadowEpochs();
		occlusionDrawn = epochs ? epochs->afterShadowMaps(wanted) : 0u;
		occlusionEverDrawn |= occlusionDrawn;
		for (std::uint32_t map = 0; map < occlusionNeedStreak.size(); ++map)
			if (occlusionDrawn & (1u << map))
				occlusionNeedStreak[map] = 0;
	}

	void FrameDriver::EarlyPrepass()
	{
		// Runs after the registration jobs finish. Reads none of the coordinator's state.
		ScopedPerfEvent event("CS DCLF: accumulator tables and pipelines");
		const auto start = std::chrono::steady_clock::now();
		auto& view = Scene::FrameView::Get();
		view.PostAccumulateInputs();
		view.PrepareAccumulatePhase();
		const double ms = MillisecondsSince(start);
		counters.timing.buildMs += ms;
		counters.timing.buildMaxMs = std::max(counters.timing.buildMaxMs, ms);
		++counters.timing.frames;
	}

	void FrameDriver::Prepass()
	{
		auto& view = Scene::FrameView::Get();
		// The only point in the frame where `currentAccumulator` is the main camera's, so latch it for the frame.
		view.LatchAccumulator();
		// The main camera's shadow state is current here.
		view.RefreshFrameConstants();
		Diagnostics::Notify<Diagnostics::FramePoint::PrepassEnd>();
	}

	bool FrameDriver::OcclusionReady(std::uint32_t a_map)
	{
		auto& capture = PassCapture::Get();
		capture.SetOcclusionPhase(0);
		if (a_map >= Extensions::kOcclusionMaps || !Extensions::OcclusionOn(a_map))
			return false;
		// DCLF draws the map in this frame's shadow epoch when the revision's shadow shape has its view. Otherwise the engine renders it whole.
		const auto* epochs = Extensions::ShadowEpochs();
		const bool ready = epochs && epochs->occlusionReady(a_map);
		const std::uint8_t phase = Extensions::OcclusionMapOf(a_map).phase;
		auto& view = Scene::FrameView::Get();
		const auto set = view.FrameClaims();
		// Occluders still joining the map's phase are left out until they join. The engine's cull is no fallback, it would cull the whole scene.
		const bool taken = ready && set && (set->drawn & phase);
		occlusionFrames.native[a_map] += ready ? 0 : 1;
		occlusionFrames.joining[a_map] += taken && view.SetLacking(phase) > 0 ? 1 : 0;
		occlusionWanted |= taken ? 1u << a_map : 0u;
		if (const auto current = capture.CurrentSet(); taken && current && (current->drawn & phase))
			capture.SetOcclusionPhase(phase);
		return taken;
	}

	bool FrameDriver::OcclusionHeld(std::uint32_t a_map) const { return a_map < 32 && (occlusionHeld & (1u << a_map)); }

	bool FrameDriver::OcclusionNeedsEngine(std::uint32_t a_map)
	{
		if (a_map < 32)
			occlusionHeld &= ~(1u << a_map);
		const bool needed = a_map >= Extensions::kOcclusionMaps || !(occlusionDrawn & (1u << a_map));
		// A map this frame took for DCLF's shadow epoch (OcclusionReady) is DCLF's. Not drawing it is a tap error, not the engine's map.
		if (needed && a_map < Extensions::kOcclusionMaps && (occlusionTaken & (1u << a_map)) && !Scene::FrameView::Get().NotDrawingYet())
			RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::Uncovered, RenderGraph::Frame::SegmentId::Count,
				fmt::format("DCLF: occlusion map {} of frame {} was taken for the shadow epoch, which did not draw it", a_map, Scene::FrameView::Get().SceneStep()));
		// The engine's cull of the map reads the scene lists, so make them whole again.
		if (needed)
			PrimaryCull::Get().RestoreSceneLists();
		if (a_map < Extensions::kOcclusionMaps)
			occlusionFrames.engine[a_map] += needed ? 1 : 0;
		// A map DCLF should draw but the engine must render. Held during a load's hold, otherwise counted.
		if (needed && a_map < Extensions::kOcclusionMaps && Extensions::OcclusionOn(a_map)) {
			// A held map still renders at the engine's point but without the engine's cull, so it has no occluders until DCLF takes it.
			// A map gets a new layout when it first draws (rain starts the precipitation map), and DCLF cannot draw it for a few
			// frames: a re-entry holds those frames. A map DCLF never drew counts only for kOcclusionReentryFrames.
			const std::uint32_t streak = ++occlusionNeedStreak[a_map];
			if (!LoadHold::Active() && ((occlusionEverDrawn & (1u << a_map)) || streak <= kOcclusionReentryFrames))
				LoadHold::BeginReentry("an occlusion map DCLF cannot draw this frame (its layout is new, or the graph rebuilds)", Snapshot::Current().get());
			if (LoadHold::Active()) {
				LoadHold::NoteHeld(LoadHold::View::Occlusion);
				occlusionHeld |= 1u << a_map;
				return needed;
			}
			WholeCull::Note(WholeCull::Reason::OcclusionNotTaken);
		}
		return needed;
	}

	FrameDriver::OcclusionFrames FrameDriver::TakeOcclusionFrames()
	{
		auto frames = std::exchange(occlusionFrames, OcclusionFrames{});
		for (std::uint32_t map = 0; map < Extensions::kOcclusionMaps; ++map)
			frames.lacking[map] = Scene::FrameView::Get().SetLacking(Extensions::OcclusionMapOf(map).phase);
		frames.withheld = PassCapture::Get().GetStats().occlusionWithheld;
		return frames;
	}

	void FrameDriver::BeforeOpaquePass()
	{
		// The engine binds and clears the G-buffer only when a draw applies its state, so apply it here as the first draw would.
		// A frame where DCLF owns every lighting draw has no native draw to do it. The pass end is too late, the clouds have rebound t26.
		// Native's RenderWorld binds nothing through the engine: the capture takes its targets from CS's frame.
		if (!::RenderGraph::Engine::Native::WorldPass::InRenderWorld())
			Engine::ApplyPendingState();
		if (const auto* view = Extensions::PrimaryView(); view && view->beforeOpaque())
			captureFrame = Scene::FrameView::Get().SceneStep();
	}

	void FrameDriver::AfterOpaquePass()
	{
		if (const auto* view = Extensions::PrimaryView())
			view->afterOpaque();
	}

	void FrameDriver::NativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
	{
		auto& view = Scene::FrameView::Get();
		// A ProjectedUV draw has SetupGeometry's four projected textures bound here. Hair binds none.
		if (globals::deferred->deferredPass && (PassDescriptorOf(a_pass->passEnum) & 0x8000u) && ((PassDescriptorOf(a_pass->passEnum) >> 24) & 0x3f) != 6)
			view.NoteProjectedTextures();
		if (Diagnostics::Armed<Diagnostics::NativeLightingDraw>()) {
			const std::uint32_t frame = view.SceneStep();
			Diagnostics::Notify(Diagnostics::NativeLightingDraw{ a_pass, a_renderFlags, frame, globals::deferred->deferredPass, captureFrame == frame });
		}
	}
}
