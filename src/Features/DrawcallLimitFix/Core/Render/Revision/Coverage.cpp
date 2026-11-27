// The frame's side of the scene revisions, on the render thread. It runs the build point, posts revision inputs, and decides
// before the engine draws which epochs the adopted snapshot covers.

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/BuildState.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/RecipeTable.h"
#include "RenderGraph/Frame/Revision.h"
#include "RenderGraph/Frame/VariablesLane.h"
#include "Snapshot/FrameCamera.h"

namespace DCLF
{
	void IndirectDraws::BuildPoint()
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		auto& rv = impl->revisions;
		// DCLF's builds are only staged here, so the graph a frame's revision was recorded on stays for the whole frame.
		// A Begin attaches a staged build with the snapshot recorded on it (plan step 10a).
		rv.explicitBuilds = true;
		impl->RegisterGraphBuilds();
		// The graph conditions change at a Begin, with the revision that carried their variables, and ask for builds there (plan
		// step 10c). Only the first build point makes them. The attached graphs stay until a scene recorded on new builds is adopted.
		Draws::FirstGraphConditions(*host);
		impl->BuildIfAsked();
		// A failed build stops the graph, so nothing more may run on it.
		if (!RenderGraphRuntime::Get().IsActive())
			return;
		impl->SettleBuildState();
		impl->ReleaseTicketsForRecordings(*host, RenderGraphRuntime::Get().TakeSubmittedSegments());
	}

	std::uint64_t IndirectDraws::Impl::GraphBuildGeneration(const org::GraphHost& a_host)
	{
		std::uint64_t builds = 0;
		for (std::uint32_t segment = 0; segment < static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count); ++segment)
			if (DCLF::IsDclfSegment(static_cast<RenderGraphRuntime::Segment>(segment)))
				builds += a_host.BuildGeneration(RenderGraphRuntime::EpochOf(static_cast<RenderGraphRuntime::Segment>(segment)));
		return builds;
	}

	void IndirectDraws::Impl::BuildIfAsked()
	{
		ServeRevisionRequests();
		// Build through the runtime, so a lost device stops the graph instead of throwing into the frame.
		if (revisions.explicitBuilds && RenderGraphRuntime::Get().BuildIfRequested("Drawcall Limit Fix's build point"))
			++revisions.builds;
		PostRevisionInputs();
	}

	void IndirectDraws::Impl::ReleaseTicketsForRecordings(org::GraphHost& a_host, std::uint64_t a_submitted)
	{
		// A recording waits for the slot held by its epoch's unsubmitted ticket. Epochs the last frame skipped give their ticket back,
		// so a pending recording does not wait forever. Hosted epochs have no ticket of their own.
		if (!revisions.explicitBuilds || !recordingsOutstanding.load(std::memory_order_acquire))
			return;
		for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e)
			if (const auto segment = EpochTraitsOf(e).segment;
				EpochTraitsOf(e).hostedBy == kNoEpoch && !(a_submitted & (std::uint64_t{ 1 } << static_cast<std::uint32_t>(segment))))
				revisions.ticketsReleased += a_host.ReleaseEpochTicket(RenderGraphRuntime::EpochOf(segment)) ? 1 : 0;
	}

	void IndirectDraws::NoteRevisionInputs()
	{
		if (impl)
			impl->PostRevisionInputs();
	}

	namespace
	{
		/** @brief Whether a_next equals a_previous, ignoring generations. */
		bool SameRevisionInputs(const RevisionInputs& a_next, const RevisionInputs& a_previous)
		{
			return a_next.main == a_previous.main && a_next.shadow == a_previous.shadow && a_next.views == a_previous.views && a_next.scene == a_previous.scene &&
			       a_next.buildGeneration == a_previous.buildGeneration && SameHandle(a_next.resourceHeap, a_previous.resourceHeap) &&
			       SameHandle(a_next.samplerHeap, a_previous.samplerHeap) && a_next.shadowRasterStates == a_previous.shadowRasterStates && a_next.claims == a_previous.claims &&
			       a_next.shadowCandidates == a_previous.shadowCandidates && a_next.occlusionLayouts == a_previous.occlusionLayouts &&
			       a_next.shadowPlacements == a_previous.shadowPlacements && a_next.shadowActiveViews == a_previous.shadowActiveViews && a_next.mainKnown == a_previous.mainKnown && a_next.viewport == a_previous.viewport &&
			       a_next.blockSizes == a_previous.blockSizes &&
			       a_next.resourceInputs == a_previous.resourceInputs &&
			       a_next.shadowSlots == a_previous.shadowSlots && a_next.asyncEpochs == a_previous.asyncEpochs && a_next.cpuPipeline == a_previous.cpuPipeline &&
			       a_next.build == a_previous.build && a_next.frameVersionsSerial == a_previous.frameVersionsSerial &&
			       !a_next.uploads.owner_before(a_previous.uploads) && !a_previous.uploads.owner_before(a_next.uploads);
		}
	}

	namespace
	{
		/**
		 * @brief A native main shape's size: kMAIN's shape (its colour target, or its depth target for the Z-prepass) in the revision
		 * a_versions names, at that revision's dynamic resolution, as Renderer::UpdateViewPort sizes it. Zero before the first adoption.
		 */
		void NativeMainSize(const RenderGraph::Frame::Revision::FragmentVersions& a_versions, bool a_depth, MainViewport& a_out)
		{
			namespace Vars = RenderGraph::Frame::Vars;
			a_out.width = a_out.height = 0;
			if (!a_versions.variables)
				return;
			const auto shapes = Vars::Lane::ValueIn(*a_versions.variables, Vars::EngineTargetShapes());
			const auto ratios = Vars::Lane::ValueIn(*a_versions.variables, Vars::DynamicRatios());
			const auto& target = a_depth ? shapes.depths[static_cast<std::size_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN)] :
			                               shapes.colours[static_cast<std::size_t>(RE::RENDER_TARGETS::kMAIN)];
			a_out.width = static_cast<std::uint32_t>(static_cast<float>(target.width) * ratios.x);
			a_out.height = static_cast<std::uint32_t>(static_cast<float>(target.height) * ratios.y);
		}
	}

	void IndirectDraws::Impl::PostRevisionInputs()
	{
		// Inputs can be posted before the first build point, so register the build source first.
		RegisterGraphBuilds();
		RevisionInputs next;
		// A new display size's main resources once they are handed over, else the ones the frame runs.
		next.main = AheadMain();
		for (auto* participant : participants)
			participant->PostInputs(next);
		next.scene = scene;
		if (auto* host = RenderGraphRuntime::Get().Host()) {
			// Each segment records on its staged build when one is staged, else on its attached one. The staged one attaches with the
			// snapshot (plan step 10a). buildGeneration is their sum over DCLF's segments, so a staging makes every recording again.
			for (std::size_t segment = 0; segment < next.epochBuilds.size(); ++segment) {
				const auto kind = static_cast<RenderGraphRuntime::Segment>(segment);
				const auto epoch = RenderGraphRuntime::EpochOf(kind);
				const std::uint64_t staged = host->StagedGeneration(epoch);
				next.epochBuilds[segment] = staged ? staged : host->BuildGeneration(epoch);
				if (DCLF::IsDclfSegment(kind))
					next.buildGeneration += next.epochBuilds[segment];
			}
			if (auto* descriptors = host->Descriptors()) {
				next.resourceHeap = descriptors->GetSRVDescriptorHeap().GetHandle();
				next.samplerHeap = descriptors->GetSamplerDescriptorHeap().GetHandle();
			}
			next.asyncEpochs = host->AsyncEpochs();
			next.uploads = host->RetainUploads();
		}
		next.build = CurrentBuildState();
		// The conditions of the build the recordings are of, not the attached ones: a scene recorded on staged builds is adopted with
		// them, and the frame then runs their conditions. The view batches' kind and the recorded epochs follow these (plan step 10c).
		{
			const auto& conditions = next.build->Conditions();
			next.cpuPipeline = conditions.cpuPipeline;
		}
		next.claims = true;
		next.shadowCandidates = ShadowViews::Get().Candidates();
		next.mainKnown = mainEpochInputs.known;
		next.viewport = mainEpochInputs.viewport;
		next.blockSizes = mainEpochInputs.blockSizes;
		// The frame's versions to record against: a frame revision's pending ones while its closure has DCLF epochs (plan step 10b).
		auto frameVersions = RenderGraph::Frame::Revision::FrameVersionsFor(SceneSource());
		// Native: the main shapes draw into kMAIN's version in that revision, so their size is its shape there at the revision's
		// dynamic resolution. The engine's live viewport takes a new window size before the frame's targets do (decision 112).
		if (Snapshot::FrameCamera::Owned())
			for (const std::size_t shape : { kDepthShape, kColourShape })
				if (next.mainKnown[shape])
					NativeMainSize(frameVersions, shape == kDepthShape, next.viewport[shape]);
		// A Target view's size is its imported target's. Other views use the main colour viewport.
		for (const auto& slot : kViewSlots) {
			const auto& view = View(slot.view);
			const bool target = Extensions::ViewEntryOf(slot.view).area == Api::ViewArea::Target;
			next.views[ViewIndex(slot.view)] = { view.resources, view.targets,
				target ? (view.resources ? view.resources->width : 0u) : next.viewport[kColourShape].width,
				target ? (view.resources ? view.resources->height : 0u) : next.viewport[kColourShape].height, DepthRangeOf(slot.view) };
		}
		Extensions::PostSceneResourceInputs(next.resourceInputs);
		next.frameVersions = std::move(frameVersions.versions);
		next.frameVersionsSerial = frameVersions.serial;
		const auto& previous = postedInputs;
		next.mainGeneration = previous.mainGeneration + (next.main != previous.main ? 1u : 0u);
		const bool same = SameRevisionInputs(next, previous);
		if (same && previous.generation)
			return;
		next.generation = previous.generation + 1;
		postedInputs = next;
		revisionInputsSlot.Post(std::make_unique<RevisionInputs>(std::move(next)));
		(void)SnapshotPump();
		snapshotInputsMoved.store(true, std::memory_order_release);
		WakeSnapshotBuilder();
	}

	bool IndirectDraws::DecideCoverage()
	{
		// Must run before anything covers or draws. The view batches are null in GPU mode.
		Direct::FrameStart(impl->InstalledViewBatches());
		auto& rv = impl->revisions;
		auto* host = RenderGraphRuntime::Get().Host();
		impl->DecideEpochCoverage(host);
		// The frame claims draws only if every claims epoch is covered, or else DCLF would have nothing to draw them with.
		// A dormant epoch draws nothing in this install, so it claims nothing either.
		bool main = true;
		for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs && main; ++e)
			main = !EpochTraitsOf(e).claims || EpochTraitsOf(e).dormant || rv.covered[e];
		rv.uncoveredFrames += main ? 0 : 1;
		const std::uint32_t frame = Scene::FrameView::Get().SceneStep();
		// While DCLF draws, an epoch the recipe has it cover is covered or named the engine's. Anything else is a tap error. The claims
		// epochs are checked whole by the frame driver, which also tells a lost device or a stopped graph apart.
		if (main && !Scene::FrameView::Get().NotDrawingYet())
			for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e)
				if (const auto& traits = EpochTraitsOf(e);
					!rv.covered[e] && rv.engine[e] == EngineState::None && RenderGraph::Frame::Recipes::Table::CoveredSegment(traits.segment))
					RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::Uncovered, RenderGraph::Frame::SegmentId::Count,
						fmt::format("DCLF: the {} epoch of frame {} is not covered: {}", traits.name, frame, CoverageState()));
		for (const auto& slot : kViewSlots)
			if (Extensions::ViewEntryOf(slot.view).covered)
				impl->DecideViewCoverage(slot.view, host, main, frame);
		return main;
	}

	std::string IndirectDraws::CoverageState() const
	{
		const auto& s = *impl;
		const auto& adopted = s.adoptedSnapshot;
		const auto* payload = s.InstalledScenePayload();
		auto* host = RenderGraphRuntime::Get().Host();
		std::string epochs;
		for (std::uint32_t e = 0, count = EpochCount(); e < count; ++e)
			epochs += fmt::format("{}{} {}{}", e ? ", " : "", EpochTraitsOf(e).name,
				s.revisions.covered[e]                     ? std::string("covered") :
				s.revisions.engine[e] != EngineState::None ? fmt::format("the engine's ({})", EngineStateName(s.revisions.engine[e])) :
				                                             std::string("UNCOVERED"),
				EpochTraitsOf(e).claims ? " (claims)" : "");
		return fmt::format("adopted snapshot serial {} (revision {}, draws complete {}, current {}, versions stale {}, addresses current {}, frame versions {}, build "
						   "generation {} against {} attached, main generation {} against {}), newest taken dropped {}, payload {}, adopted set {}; epochs: {}",
			adopted ? adopted->serial : 0, adopted && adopted->revision, adopted && adopted->drawsComplete, s.AdoptedCurrent(), s.sceneVersionsStale,
			!s.adoptedSet || SceneBufferSet::Get().AddressesCurrent(*s.adoptedSet),
			adopted ? adopted->frameVersions : 0,
			adopted ? adopted->buildGeneration : 0, host ? Impl::GraphBuildGeneration(*host) : 0,
			adopted ? adopted->mainGeneration : 0, s.postedInputs.mainGeneration, s.snapshotStale, !payload ? "none" : FrameValues::Get().Available() ? "installed" : "installed, NO FRAME VALUES",
			s.adoptedSet ? "named" : "none", epochs);
	}

	void IndirectDraws::Impl::DecideEpochCoverage(org::GraphHost* a_host)
	{
		using R = SceneRevisions;
		auto& rv = revisions;
		auto* host = a_host;
		// An epoch is covered only when the adopted snapshot is current and its recording matches the graph as built now.
		// There is no fallback.
		rv.covered.fill(false);
		const auto& active = rv.active;
		const auto& versionsFragment = active ? active->Fragment(R::kVersionsSlot) : nullptr;
		const auto versions = versionsFragment ? versionsFragment->Value<NamedVersions>() : nullptr;
		const auto* payload = InstalledScenePayload();
		// The adopted snapshot must be current. A newer one that was stale leaves it in use: it is recorded on the graphs that run.
		// A scene that binds a version the frame does not read covers nothing (CheckSceneVersions).
		bool snapshot = adoptedSnapshot && adoptedSnapshot->drawsComplete && AdoptedCurrent() && !sceneVersionsStale && payload && PayloadPools::Available() &&
		                FrameValues::Get().Available();
		// After an idle buffer move, a scene sealed before it may name the old device addresses in its shapes and recordings. It
		// covers nothing, and the move wakes the producer to seal one with the new addresses.
		if (snapshot && adoptedSet && !SceneBufferSet::Get().AddressesCurrent(*adoptedSet)) {
			snapshot = false;
			++addressWaitFrames;
		}
		const bool current = host && snapshot && versions && versions.get() == adoptedSet;
		// Records how many frames passed between a growth's adoption and the first draws that use its versions.
		if (current && snapshot && growthAdoptedFrame != ~0u) {
			const std::uint32_t frameNow = Scene::FrameView::Get().SceneStep();
			growthToDraws.push_back(frameNow >= growthAdoptedFrame ? frameNow - growthAdoptedFrame : 0u);
			growthAdoptedFrame = ~0u;
		}
		// CPU pipeline. An epoch needs its shape, the view batches and a live site. If it has CPU pipeline passes it also needs a
		// current recording. A hosted epoch is covered only with its host.
		if (current && snapshot && Draws::AttachedGraphConditions().cpuPipeline) {
			const bool batches = Direct::FrameBatches() != nullptr;
			for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e) {
				const auto& traits = EpochTraitsOf(e);
				const bool live = traits.shape != ShapeKind::View || Direct::ViewSiteLive(traits.index);
				bool recorded = true;
				if (const auto owner = traits.hostedBy != kNoEpoch ? traits.hostedBy : e; Draws::CpuPipelineRecords(owner)) {
					const auto recordings = active->Get<RevisionRecordings>(R::kRecordingSlot + owner);
					recorded = recordings && !recordings->recordings.empty() && recordings->recordings.front() && host &&
					           host->EpochRecordingCurrent(*recordings->recordings.front()) && (traits.hostedBy == kNoEpoch || recordings->Carries(e));
				}
				rv.covered[e] = batches && live && recorded && active->Fragment(R::kShapeSlot + e) != nullptr && (traits.hostedBy == kNoEpoch || rv.covered[traits.hostedBy]);
			}
		} else if (current && snapshot)
			for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e) {
				// A hosted epoch uses its host's recordings, which must include its passes.
				const auto hostedBy = EpochTraitsOf(e).hostedBy;
				const auto recordings = active->Get<RevisionRecordings>(R::kRecordingSlot + (hostedBy != kNoEpoch ? hostedBy : e));
				rv.covered[e] = recordings && !recordings->recordings.empty() && recordings->recordings.front() &&
				                (hostedBy == kNoEpoch ? host->EpochRecordingCurrent(*recordings->recordings.front()) : rv.covered[hostedBy] && recordings->Carries(e));
				// Without forward batches nothing draws the forward objects. The adopted scene always has them in the GPU-driven graph,
				// so missing ones leave the epoch uncovered with no engine state: a tap error while DCLF draws.
				if (EpochTraitsOf(e).segment == RenderGraphRuntime::Segment::Forward && !Direct::ForwardBatches())
					rv.covered[e] = false;
				// The same for first person's colour.
				if (const auto& traits = EpochTraitsOf(e); traits.shape == ShapeKind::View && traits.index == Draws::ViewIndex(Api::EngineView::FirstPerson) &&
					!Direct::ForwardBatches())
					rv.covered[e] = false;
			}
		// Why the engine draws each uncovered epoch, from what this frame's Begin adopted only. None on an uncovered epoch is a defect.
		// A dormant epoch has no recording, so it is never covered.
		const bool notDrawing = Scene::FrameView::Get().NotDrawingYet();
		// The frame's ticket took it on the game side.
		const bool loading = FrameGlobals::Current().loading;
		for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e) {
			const auto& traits = EpochTraitsOf(e);
			if (traits.dormant)
				rv.covered[e] = false;
			const bool shape = active && active->Fragment(R::kShapeSlot + e) != nullptr;
			const bool hostEngine = traits.hostedBy != kNoEpoch && rv.engine[traits.hostedBy] != EngineState::None;
			rv.engine[e] = rv.covered[e] ? EngineState::None :
			               traits.dormant ? EngineState::Dormant :
			               notDrawing    ? EngineState::NotDrawing :
			               loading       ? EngineState::LoadScreen :
			               !shape        ? EngineState::NoShape :
			               hostEngine    ? EngineState::HostEngine :
			                               EngineState::None;
		}
		bool ran = false;
		for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e) {
			rv.uncovered[e] += rv.covered[e] || EpochTraitsOf(e).dormant ? 0 : 1;
			ran = ran || rv.covered[e];
		}
		// A frame revision with DCLF epochs in its closure waits for DCLF's scene only while DCLF's epochs run (plan 7.8).
		if (ran)
			sceneRecordedFrames.fetch_add(1, std::memory_order_release);
	}

	std::shared_ptr<const FamilyPipelines> IndirectDraws::AdoptedFamilyPipelines(Api::DrawFamily a_family) const
	{
		if (!impl)
			return nullptr;
		const auto& active = impl->revisions.active;
		if (!active)
			return nullptr;
		for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e) {
			const auto& traits = EpochTraitsOf(e);
			if (traits.shape != ShapeKind::Main || traits.index != kDepthShape)
				continue;
			const auto& fragment = active->Fragment(Impl::SceneRevisions::kShapeSlot + e);
			const auto frame = fragment ? fragment->Value<PassFrame>() : nullptr;
			return frame ? frame->families[static_cast<std::size_t>(a_family)] : nullptr;
		}
		return nullptr;
	}

	void IndirectDraws::Impl::DecideViewCoverage(ViewKind a_kind, org::GraphHost* a_host, bool a_main, std::uint32_t a_frame)
	{
		// The view's feature decides coverage from these inputs. Every input is filled regardless of the others.
		const auto& rv = revisions;
		const auto& epochs = ViewEpochsOf(a_kind).epochs;
		auto& view = View(a_kind);
		const auto& r = view.resources;
		Api::ViewCoverage coverage;
		coverage.main = a_main;
		coverage.on = Extensions::ViewRegistered(a_kind);
		coverage.epochsCovered = rv.covered[epochs[0]] && (epochs[1] == kNoEpoch || rv.covered[epochs[1]]);
		coverage.targetImported = r && r->target;
		coverage.rebuildPending = r && r->rebuildPending;
		coverage.mainResourcesCurrent = r && r->main && r->main == resources && r->main->native[1];
		coverage.rebuildRequested = a_host && RenderGraphRuntime::Get().RebuildRequestedUnstaged();
		coverage.frame = a_frame;
		const bool covered = Extensions::ViewEntryOf(a_kind).covered(coverage);
		SetViewCovered(a_kind, covered);
		PassCapture::Get().SetViewCovered(a_kind, covered);
	}

	bool IndirectDraws::Impl::EpochCovered(std::uint32_t a_epoch) const
	{
		return a_epoch < revisions.covered.size() && revisions.covered[a_epoch];
	}
}
