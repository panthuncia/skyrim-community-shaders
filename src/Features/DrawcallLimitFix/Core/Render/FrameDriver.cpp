// FrameDriver: the render thread's DCLF work at each engine point of the frame.

#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "Features/DrawcallLimitFix/Core/Handoff/WholeCull.h"
#include "Features/DrawcallLimitFix/Core/Render/LoadHold.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
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
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/Segments.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
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
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
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
		// DCLF draws on the main thread only.
		source.mainThreadOnly = true;
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
		source.left = [](std::uint64_t) { Scene::FrameView::Get().StopDrawing(); };
		// The graph conditions for the adopted variables, after the adoption point's revision work.
		source.afterAdoption = [] {
			TracyCZoneN(afterScene, "CS.DCLF.AfterSceneStep", true);
			IndirectDraws::Get().AfterSceneStep();
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

	void FrameDriver::BeginFrame()
	{
		// The engine's update is over, so a detach still waiting for a re-attach is a removal.
		SceneTracker::Get().PushHeldDetaches();
		// Start a load's hold before anything checks whether a load screen is up.
		LoadHold::OnFrameStart();
		Diagnostics::Notify(Diagnostics::FrameBegin{});
		occlusionWanted = occlusionDrawn = occlusionTaken = 0;
		// An extension added or removed last frame is built here, before anything decides what the frame's recordings cover.
		// In native mode with segments this already ran at Renderer::Begin, and no build may attach after that.
		if (s_buildPointFrame != RenderGraph::Frame::Driver::Current().frame)
			IndirectDraws::Get().BuildPoint();
		auto& scene = Scene::Coordinator::Get();
		auto& view = Scene::FrameView::Get();
		// The engine's update is done, so workers may read its scene graph until Present.
		EngineReadWindow::Open();
		// The posts below wake the coordinator once, when this scope ends.
		ScenePassWakeBatch wakes;
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
		TracyCZoneN(bfCamera, "CS.DCLF.BF.Camera", true);
		PrimaryCull::Get().BeginFrameCamera();
		if (PrimaryCull::Get().Installed())
			Extensions::RootStatesFrameStart();
		Extensions::PaletteRowsFrameStart();
		TracyCZoneEnd(bfCamera);
		TracyCZoneN(bfAdopt, "CS.DCLF.BF.Adopt", true);
		auto& draws = IndirectDraws::Get();
		// Load screen frames have no claims, so they are never waited for or required to be covered.
		const bool loading = Scene::FrameView::IsLoadingScreenUp();
		{
			// The frame revision adopted the snapshot at this frame's Begin (plan step 10b), with the staged builds it was recorded on.
			// Nothing waits here: a newer snapshot not ready yet leaves the adopted one, recorded on the graphs that run.
			RenderThreadBudget::Part exchange(RenderThreadBudget::Bucket::Exchange);
			view.HandOverAtFrameStart(draws.AdoptedPublication());
			{
				DCLF_ZONE("CS.DCLF.BF.SyncFrameTables");
				view.SyncFrameTables();
			}
		}
		TracyCZoneEnd(bfAdopt);
		TracyCZoneN(bfViewsStart, "CS.DCLF.BF.ViewsStartAndLookups", true);
		// Views start before the coverage decision that withholds their members.
		RunViewWork(Api::ViewPoint::FrameStartBeforeCoverage);
		// Entries changed since the sun exclusion's snapshot leave it, and the engine culls them.
		Extensions::ShadowExclusionFrameStart();
		draws.PostLookupInputs();
		TracyCZoneEnd(bfViewsStart);
		{
			DCLF_ZONE("CS.DCLF.BF.Claims");
			// While DCLF does not draw (before its first covered frame, or after an adoption stopped it) the engine draws everything. While
			// it draws, an uncovered frame is a tap error, because the adopted snapshot stays current until a Begin adopts a newer one with
			// its builds. Load screen frames are the engine's (EngineState::LoadScreen).
			RenderThreadBudget::Part exchange(RenderThreadBudget::Bucket::Exchange);
			const bool covered = draws.DecideCoverage();
			// Native mode bring-up: log why claims are not installed, once every 300 such frames.
			if (!(covered && view.HasInstalled()) && RenderGraph::Engine::Native::Active()) {
				static std::uint32_t s_uncovered = 0;
				if (s_uncovered++ % 300 == 0)
					logger::info("[DCLF] native: frame {} has no claims (covered {}, a publication installed {}, loading {}): {}", view.SceneStep(), covered,
						view.HasInstalled(), loading, draws.CoverageState());
			}
			if (covered && view.HasInstalled())
				view.InstallClaims();
			else if (!view.NotDrawingYet() && !loading) {
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
			Reports::GateReleases(view.SceneStep());
			// Release the LOD gates the installed publications flipped, before the culls.
			Extensions::ComponentsAdopt(view.SceneStep(), view.AdoptedClaims(), view.AdoptedDeltas());
			view.ClearAdoptedDeltas();
		}
		TracyCZoneN(bfKicks, "CS.DCLF.BF.AheadAndKicks", true);
		// First person's rows and palettes must be taken before Main::Draw moves the skeleton into its root's space.
		bool rootViewsCovered = false;
		for (std::uint32_t i = 0; i < Extensions::Views().rootCount; ++i)
			rootViewsCovered = rootViewsCovered || draws.ViewCovered(Extensions::Views().rootViews[i]);
		FrameValues::Get().SetRootViewClaims(rootViewsCovered ? view.FrameClaims() : nullptr);
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
		// Serve what the coordinator asked of the render thread, outside load screens. Switch catch-ups write engine memory, so they go before the culls.
		{
			const auto eventsStart = std::chrono::steady_clock::now();
			if (!view.NoteLoadingScreen())
				view.ServeFrameRequests();
			counters.timing.eventsMs += MillisecondsSince(eventsStart);
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
		// Draw families decide before the main camera's cull registers its passes.
		{
			DCLF_ZONE("CS.DCLF.BF.DecideDrawFamilies");
			draws.DecideDrawFamilies(view.SceneStep());
		}
		{
			DCLF_ZONE("CS.DCLF.BF.ViewWorkAfterTreeLod");
			RunViewWork(Api::ViewPoint::FrameStartAfterTreeLod);
		}
		{
			DCLF_ZONE("CS.DCLF.BF.PublishListFilter");
			PrimaryCull::Get().PublishListFilter();
		}
		TracyCZoneEnd(bfFamilies);
		// The capture reads the shading changes the take empties.
		{
			DCLF_ZONE("CS.DCLF.BF.FrameValuesKick");
			TracyCZoneN(bfFields, "CS.DCLF.BF.CaptureShadingFields", true);
			auto fields = Extensions::CaptureShadingFields(view);
			TracyCZoneEnd(bfFields);
			// Producers' uploads may only wait for CPU frames signalled before the last present.
			::RenderGraph::Frame::Retirement::SealCpuFrames();
			FrameValues::Get().SetLatePoses(LatePoses::PrepareFrame());
			(void)FrameValues::Get().Kick(view.TakePlacementPlans(), view.TakeShadingItems(), std::move(fields), view.FrameGlobalsOfFrame());
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
		if (LatePoses::ConfirmFrame())
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
		LatePoses::CheckAtPresent();
	}

	void FrameDriver::EndFrame()
	{
		LoadHold::OnFrameEnd();
		// The engine's next update follows Present, so no worker may be inside its memory from here.
		{
			RenderThreadBudget::Part boundary(RenderThreadBudget::Bucket::EngineBoundary);
			EngineReadWindow::Close();
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
		TracyCZoneN(efReports, "CS.DCLF.EF.ReportsAndReleases", true);
		// Once per frame number. Menus and loads hold the number still while Presents continue.
		static std::uint32_t reportedFrame = ~0u;
		if (const std::uint32_t frame = Scene::FrameView::Get().SceneStep(); std::exchange(reportedFrame, frame) != frame)
			Reports::RenderThread(frame);
		Scene::FrameView::Get().NoteLoadingScreen();
		// Dropped on the engine's main thread, where nothing reads them.
		EngineReleases::Release();
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
		if (const auto* epochs = Extensions::ShadowEpochs())
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
				LoadHold::BeginReentry("an occlusion map DCLF cannot draw this frame (its layout is new, or the graph rebuilds)");
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
