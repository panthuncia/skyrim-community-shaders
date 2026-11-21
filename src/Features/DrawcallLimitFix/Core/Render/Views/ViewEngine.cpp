// The view engine. Core's calls for features that take an engine view: the slot's setup at frame start, its epochs' inputs and
// submission, its coverage and its slot readiness. Render thread, except ViewDrawable and ViewSlotReady, which run in the scene
// lane's commit, and ViewCovered.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/ViewSites.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/FrameRecords.h"

#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF
{
	const ViewEpochState* IndirectDraws::ViewState(Api::EngineView a_view) const
	{
		return impl ? &impl->View(a_view) : nullptr;
	}

	const TargetFormats* IndirectDraws::MainTargetFormats() const
	{
		return impl ? &impl->formats : nullptr;
	}

	bool IndirectDraws::SetViewTargets(Api::EngineView a_view, const ForwardTargets& a_targets)
	{
		auto& state = impl->View(a_view);
		if (a_targets == state.targets)
			return false;
		state.targets = a_targets;
		state.PublishTargets();
		return true;
	}

	void IndirectDraws::PostViewInputs()
	{
		impl->PostRevisionInputs();
	}

	void IndirectDraws::SetViewDepthRange(Api::EngineView a_view, float a_range)
	{
		if (a_range > 0.0f && a_range != ViewDepthRange(a_view)) {
			impl->View(a_view).depthRange = a_range;
			impl->PostRevisionInputs();
		}
	}

	float IndirectDraws::ViewDepthRange(Api::EngineView a_view) const
	{
		return impl ? impl->DepthRangeOf(a_view) : Extensions::ViewEntryOf(a_view).shape.depthRange;
	}

	bool IndirectDraws::ExchangeViewForwardCompatible(Api::EngineView a_view, bool a_compatible)
	{
		return impl->View(a_view).forwardCompatible.exchange(a_compatible, std::memory_order_acq_rel);
	}

	IndirectDraws::ViewPipelineCounts IndirectDraws::ResolveViewPipelines(Api::EngineView a_view, const SceneTables& a_tables, const PipelineCatalog* a_catalog,
		std::span<const std::uint8_t> a_wanted, bool a_programs)
	{
		auto& state = impl->View(a_view);
		auto& drawPipelines = DrawPipelines::Get();
		ViewPipelineCounts counts;
		state.slotPipelines.assign(a_tables.pipelines.size(), rhi::PipelineHandle{});
		for (std::size_t p = 0; p < a_tables.pipelines.size(); ++p) {
			ForwardPipelineKey key;
			if ((!a_wanted.empty() && !a_wanted[p]) || !ViewForwardKey(a_view, a_tables, p, key))
				continue;
			++counts.slots;
			drawPipelines.RequestForward(key);
			// With a_programs, also count built programs and pipelines still to come, which the frame waits on.
			const auto* entry = a_programs && a_catalog ? a_catalog->FindForward(key) : nullptr;
			counts.programs += entry && entry->program ? 1u : 0u;
			const auto pipeline = ForwardPipelineOf(a_catalog, key, state.targets);
			if (!pipeline.valid()) {
				counts.waited |= a_programs && !(entry && entry->failed);
				continue;
			}
			state.slotPipelines[p] = pipeline;
			++counts.ready;
		}
		return counts;
	}

	void IndirectDraws::DropViewPipelines(Api::EngineView a_view)
	{
		impl->View(a_view).slotPipelines.clear();
	}

	bool IndirectDraws::SetUpView(Api::EngineView a_view)
	{
		return impl && impl->SetupView(a_view);
	}

	bool IndirectDraws::ImportViewTarget(Api::EngineView a_view, ID3D11Texture2D* a_texture)
	{
		if (!impl)
			return false;
		// A DepthTarget view imports kPOST_ZPREPASS_COPY. Otherwise it is the engine's colour target with DCLF's own depth.
		if (Extensions::ViewEntryOf(a_view).targetImport == Api::ViewTargetImport::DepthTarget)
			return impl->ImportViewDepthTarget(a_view);
		return impl->ImportViewColourTarget(a_view, a_texture);
	}

	IndirectDraws::ViewInputsTaken IndirectDraws::TakeViewInputs(Api::EngineView a_view, ViewInputsFrom a_from, ViewListed& a_listed)
	{
		const std::uint32_t frameNumber = Scene::FrameView::Get().SceneStep();
		a_listed = {};
		if (a_from == ViewInputsFrom::ColourThisFrame) {
			if (impl->committedFrame[kAsyncColour] != frameNumber || Scene::FrameView::Get().NotDrawingYet())
				return ViewInputsTaken::Stale;
			a_listed.owners = { impl->committedPayload[kAsyncColour], nullptr };
			a_listed.ownerCount = 1;
			return ViewInputsTaken::Taken;
		}
		// The inputs are stale unless this frame's depth commit ran on the adopted set.
		const auto main = impl->resources;
		const auto& depthCommit = main ? main->depthCommitted : Resources::Committed{};
		if (!main || !impl->scene || depthCommit.frame != frameNumber || depthCommit.adoptedSerial != impl->Adopted().Serial())
			return ViewInputsTaken::Stale;
		const auto& draws = impl->installedDraws;
		const MainPayload* listed = impl->InstalledPayload() && draws ? draws->payloads[kAsyncZPrepass].get() : nullptr;
		if (!listed)
			return ViewInputsTaken::Stale;
		// Only the main part of the scene list. The shadow frame inputs after it are not main inputs.
		a_listed.inputs = static_cast<std::uint32_t>(listed->resident.Count() + listed->body->inputList.size());
		// A DepthThisFrame view imports its target at frame start. A DepthThisFrameTargetAfter view imports it after taking its inputs.
		const auto& resources = impl->View(a_view).resources;
		if (a_from == ViewInputsFrom::DepthThisFrame && (!resources || !resources->target))
			return ViewInputsTaken::NoTarget;
		a_listed.owners = { draws->payloads[0].get(), draws->payloads[1].get() };
		a_listed.ownerCount = 2;
		return ViewInputsTaken::Taken;
	}

	namespace
	{
		/**
		 * @brief A view's frame textures, t16 and up, from this frame's sources only. Colour pass captures and Light Limit Fix's
		 * clusters belong to the previous frame or the main camera by then, so they read null.
		 * A published texture is read by its graph id, because an import of its export would not be ordered after the producer.
		 */
		void ResolveViewFrameTextures(ViewEpochState& a_state, std::vector<std::shared_ptr<const void>>& a_owners)
		{
			auto& textures = GpuTextures::Get();
			if (a_state.frameTextureGeneration != textures.Generation()) {
				a_state.frameTextureViews = {};
				a_state.frameTextureBindings = {};
				a_state.frameTextureGeneration = textures.Generation();
			}
			a_state.frameTextures.fill(kInvalidIndex);
			const auto frameTextures = Extensions::RegisteredFrameTextures();
			const auto frameTextureAt = Extensions::FrameTextureIndices();
			for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t) {
				const auto at = frameTextureAt[t];
				const auto* const texture = at != Extensions::kNoFrameTexture && at < frameTextures.size() ? &frameTextures[at] : nullptr;
				// A per-epoch view: the faces read their own published ids here, bound by their graphs, never the main view's texture.
				if (texture && texture->FacesOwnIds()) {
					std::uint32_t own = 0;
					if (Extensions::DeclaredTextureIndex(Extensions::GraphTextureReader::Faces, t, texture->name, texture->facesGraphResources, own)) {
						HoldFrameTexture(a_state.frameTextureViews[t], a_state.frameTextureBindings[t], nullptr, t, 0u, a_owners);
						a_state.frameTextures[t] = own;
					} else {
						a_state.frameTextures[t] = HoldFrameTexture(a_state.frameTextureViews[t], a_state.frameTextureBindings[t], nullptr, t, textures.NullIndex(), a_owners);
					}
					continue;
				}
				// A registration without a view reads null.
				auto* const view = t == kCharacterLightRegister ? Scene::FrameView::Get().GetFrameCapture().characterLightView :
				                   texture && texture->view     ? static_cast<ID3D11ShaderResourceView*>(texture->view()) :
				                                                  nullptr;
				std::uint32_t graphIndex = 0;
				const auto reader = Extensions::GraphTextureReader::Faces;
				if (texture && Extensions::FrameTextureIndex(reader, t, *texture, view, graphIndex)) {
					// The graph owns the published texture, so there is no import to hold.
					HoldFrameTexture(a_state.frameTextureViews[t], a_state.frameTextureBindings[t], nullptr, t, 0u, a_owners);
					a_state.frameTextures[t] = graphIndex;
					continue;
				}
				// The character light's noise reads the published HDR downsample level.
				if (t == kCharacterLightRegister && (Extensions::AdaptedLuminanceTextureIndex(reader, t, view, graphIndex) ||
														Extensions::EngineTargetTextureIndex(reader, t, "character light", view, Extensions::CharacterLightGraphTextureIds(), graphIndex))) {
					HoldFrameTexture(a_state.frameTextureViews[t], a_state.frameTextureBindings[t], nullptr, t, 0u, a_owners);
					a_state.frameTextures[t] = graphIndex;
					continue;
				}
				a_state.frameTextures[t] = HoldFrameTexture(a_state.frameTextureViews[t], a_state.frameTextureBindings[t], view, t, view ? 0u : textures.NullIndex(), a_owners);
			}
		}

		/** @brief Owners for a view with its own frame constants: its payloads' bindings and the adopted snapshot. */
		ExecutionOwners OwnFrameOwners(std::span<const MainPayload* const> a_payloads, const std::shared_ptr<const void>& a_snapshot)
		{
			auto owners = ExecutionOwners::Make();
			if (!owners)
				return owners;
			for (const auto* payload : a_payloads)
				if (payload)
					owners.AddPayload(*payload);
			owners.Add(a_snapshot);
			return owners;
		}
	}

	IndirectDraws::ViewSubmit IndirectDraws::Impl::CommitHostedView(ViewKind a_view, std::uint32_t a_epoch, const IndirectDraws::ViewListed& a_listed,
		std::span<const ViewFace> a_faces, std::uint32_t* a_drawn, std::array<std::uint64_t, 2>* a_nullTextures)
	{
		using R = SceneRevisions;
		auto& commit = hostedCommit;
		// Only valid inside the host's commit, before the host's recording is submitted.
		if (commit.host == kNoEpoch || commit.host != EpochTraitsOf(a_epoch).hostedBy || !commit.owners)
			return ViewSubmit::NoRevision;
		// Trust the selected revision's shape, because coverage was decided before the engine drew.
		const auto fragment = revisions.active ? revisions.active->Fragment(R::kShapeSlot + a_epoch) : nullptr;
		if (!EpochCovered(a_epoch) || !fragment)
			return ViewSubmit::NoRevision;
		if (a_faces.empty())
			return ViewSubmit::Submitted;  // the host's recording variant without this epoch's work
		const auto shape = fragment->Value<ViewFrame>();
		// Everything the view reads is held with the host's owners until the host's execution retires.
		auto& owners = *commit.owners;
		for (std::uint32_t i = 0; i < a_listed.ownerCount; ++i)
			if (const auto* payload = a_listed.owners[i]; payload && payload != commit.payload)
				owners.insert(owners.end(), payload->body->bindingOwners.begin(), payload->body->bindingOwners.end());
		auto& state = View(a_view);
		const bool ownFrame = state.resources && state.resources->frameConstants;
		if (ownFrame)
			ResolveViewFrameTextures(state, owners);
		ViewFamilyRows rows{};
		const bool trees = TraitsOf(a_view).trees;
		if (trees)
			rows = ViewFaceRows(a_view, *shape, owners);
		const std::uint32_t drawn = CommitView(*shape, a_faces, a_listed.inputs, trees ? &rows : nullptr);
		commit.drawn |= 1u << a_epoch;
		if (a_drawn)
			*a_drawn = drawn;
		if (a_nullTextures && ownFrame)
			*a_nullTextures = state.nullTextures;
		return ViewSubmit::Submitted;
	}

	void IndirectDraws::Impl::PostHostedView(ViewKind a_view, std::uint32_t a_epoch, std::span<const ViewFace> a_faces)
	{
		using R = SceneRevisions;
		const auto fragment = revisions.active ? revisions.active->Fragment(R::kShapeSlot + a_epoch) : nullptr;
		const auto& state = View(a_view);
		if (!fragment || !state.resources)
			return;
		const auto shape = fragment->Value<ViewFrame>();
		const auto& traits = TraitsOf(a_view);
		const auto type = a_view == Api::EngineView::Reflection ? Direct::ViewType::Reflection : Direct::ViewType::FirstPerson;
		// Build each drawn face's cull from the same values CommitView latches.
		std::vector<Direct::CullView> faces;
		const std::uint32_t frameNumber = Scene::FrameView::Get().SceneStep();
		for (std::uint32_t f = 0; f < a_faces.size() && f < traits.faces; ++f) {
			if (!a_faces[f].drawn)
				continue;
			BuildDrawsLatch latch{};
			latch.cullFlags = traits.frustumCull ? 1u : 0u;
			latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;
			LatchCullCamera(latch, a_faces[f].viewProj, a_faces[f].eye);
			faces.push_back(type == Direct::ViewType::Reflection ? Direct::ReflectionCullView(f, latch) : Direct::FirstPersonCullView(latch));
		}
		std::vector<Direct::HostedFaceDraw> families;
		auto keep = std::make_shared<std::vector<std::shared_ptr<const void>>>();
		const auto& view = *state.resources;
		const auto& adopted = Adopted();
		for (std::size_t d = 0; traits.trees && d < Extensions::kDrawFamilies; ++d) {
			const auto& drawn = shape->faces[d];
			if (!Extensions::DrawFamilyHasFaces(static_cast<Api::DrawFamily>(d)) || !drawn.pipeline.valid() || !drawn.groups)
				continue;
			for (const auto& face : faces) {
				const auto& list = view.faceLists[d][face.viewIndex];
				if (!list || !adopted.Names(list))
					continue;
				const auto& version = adopted.Of(list).version;
				if (!version || !version->buffer)
					continue;
				families.push_back({ static_cast<std::uint32_t>(d), face.viewIndex, view.faceRowsRef[d][face.viewIndex], version->buffer->GetAPIResource().GetHandle(),
					drawn.pipeline, drawn.signature });
				keep->push_back(version);
			}
		}
		Direct::HostedViewCommit commit{ type, shape, state.resources, faces, families, std::move(keep) };
		Direct::HostedViewCommitted(commit);
	}

	IndirectDraws::ViewSubmit IndirectDraws::Impl::SubmitDirectViewEpoch(std::uint32_t a_epoch, RenderGraphRuntime::Segment a_segment,
		const IndirectDraws::ViewListed& a_listed, std::span<const ViewFace> a_faces)
	{
		if (!a_faces.empty() || !EpochCovered(a_epoch))
			return IndirectDraws::ViewSubmit::NoRevision;
		const std::span<const MainPayload* const> payloads(a_listed.owners.data(), a_listed.ownerCount);
		auto owners = EpochOwners(payloads);
		if (!owners)
			return IndirectDraws::ViewSubmit::NoOwners;
		bool reached = false;
		const auto revision = RevisionOf(a_epoch);
		const bool ok = Direct::ExecuteEpoch(a_segment, [&] {
			reached = Direct::FirstPersonColourReached();
			if (revision && revision.recordings && !revision.recordings->recordings.empty())
				SubmitRevisionRecording(a_epoch, *revision.recordings, 0);
			owners.Seal(scene.get());
		}, owners.Handle());
		if (!ok)
			return IndirectDraws::ViewSubmit::Failed;
		return reached ? IndirectDraws::ViewSubmit::Submitted : IndirectDraws::ViewSubmit::NoRevision;
	}

	IndirectDraws::ViewSubmit IndirectDraws::SubmitViewEpoch(Api::EngineView a_view, std::uint32_t a_epoch, const ViewListed& a_listed,
		std::span<const ViewFace> a_faces, std::uint32_t* a_drawn, std::array<std::uint64_t, 2>* a_nullTextures)
	{
		const auto& epochs = ViewEpochsOf(a_view);
		const std::uint32_t epoch = epochs.epochs[a_epoch];
		// A hosted epoch commits from inside its host's commit, and the host's recording carries its passes.
		// In CPU mode the view is also posted for the depth site, which draws it.
		if (EpochTraitsOf(epoch).hostedBy != kNoEpoch) {
			const auto submitted = impl->CommitHostedView(a_view, epoch, a_listed, a_faces, a_drawn, a_nullTextures);
			// A GPU-driven frame posts only first person, whose forward part the CPU draws.
			const bool posted = Direct::CpuFrame() || a_view == Api::EngineView::FirstPerson;
			if (!posted || submitted != ViewSubmit::Submitted || a_faces.empty())
				return submitted;
			impl->PostHostedView(a_view, epoch, a_faces);
			return submitted;
		}
		// Only hosted epochs may commit inside another epoch's commit.
		if (impl->hostedCommit.host != kNoEpoch) {
			static bool logged = false;
			if (!std::exchange(logged, true))
				logger::error("[DCLF] the {} epoch was submitted inside the {} epoch's commit, which only its hosted epochs commit into", EpochTraitsOf(epoch).name,
					EpochTraitsOf(impl->hostedCommit.host).name);
			return ViewSubmit::Failed;
		}
		// In CPU mode the only epoch of its own is first person's colour, which draws what the first-person depth pass culled.
		if (Direct::CpuFrame())
			return impl->SubmitDirectViewEpoch(epoch, epochs.segments[a_epoch], a_listed, a_faces);
		// Trust the selected revision, because coverage was decided before the engine drew.
		const auto revision = impl->RevisionOf(epoch);
		if (!revision)
			return ViewSubmit::NoRevision;
		std::shared_ptr<const ViewFrame> shape;
		if (!a_faces.empty())
			shape = revision.shape->Value<ViewFrame>();
		auto& state = impl->View(a_view);
		const bool ownFrame = !a_faces.empty() && state.resources && state.resources->frameConstants;
		const std::span<const MainPayload* const> payloads(a_listed.owners.data(), a_listed.ownerCount);
		auto owners = ownFrame ? OwnFrameOwners(payloads, impl->adoptedSnapshot) : impl->EpochOwners(payloads);
		if (!owners)
			return ViewSubmit::NoOwners;
		if (ownFrame)
			ResolveViewFrameTextures(state, owners.List());
		ViewFamilyRows rows{};
		const bool trees = !a_faces.empty() && TraitsOf(a_view).trees;
		if (trees)
			rows = impl->ViewFaceRows(a_view, *shape, owners.List());
		std::uint32_t drawn = 0;
		// First person's forward part is drawn by the CPU, and its run is handed to the epoch's late pass ahead of execution.
		const bool handedOff = a_faces.empty() && a_view == Api::EngineView::FirstPerson && Direct::FirstPersonColourReached();
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(epochs.segments[a_epoch], [&](org::RenderGraph&) {
			if (!a_faces.empty())
				drawn = impl->CommitView(*shape, a_faces, a_listed.inputs, trees ? &rows : nullptr);
			impl->SubmitRevisionRecording(epoch, *revision.recordings, 0);
			owners.Seal(impl->scene.get());
			Direct::AdoptSiteRun();
		}, owners.Handle());
		if (!ok && handedOff)
			Direct::ForwardHandOffCancel();
		if (!ok)
			return ViewSubmit::Failed;
		if (a_drawn)
			*a_drawn = drawn;
		if (a_nullTextures && ownFrame)
			*a_nullTextures = state.nullTextures;
		return ViewSubmit::Submitted;
	}

	bool IndirectDraws::ViewCovered(Api::EngineView a_view) const
	{
		return impl && impl->ViewCovered(a_view);
	}

	bool IndirectDraws::ViewDrawable(Api::EngineView a_view) const
	{
		// A capability, not a per-frame state. It reads the published targets, never the render thread's own copy.
		// Each member still waits for its own pipelines, and the engine draws it until the epochs are covered.
		return impl && Extensions::ViewRegistered(a_view) && impl->View(a_view).LaneTargets().colour != DXGI_FORMAT_UNKNOWN;
	}

	bool IndirectDraws::ViewSlotReady(Api::EngineView a_view, const void* a_tables, const void* a_lookups, const PipelineCatalog* a_catalog, std::uint32_t a_slot,
		std::uint32_t* a_why) const
	{
		if (Extensions::ViewEntryOf(a_view).membership == Api::ViewMembership::LodTechniques) {
			// Resolve from a_catalog, as the revision shapes do, never from the render thread's per-frame pipeline list.
			const auto& tables = *static_cast<const SceneTables*>(a_tables);
			const ForwardTargets targets = impl->View(a_view).LaneTargets();  // the published copy, not the render thread's
			if (!a_catalog || targets.colour == DXGI_FORMAT_UNKNOWN || a_slot >= tables.objects.size())
				return false;
			const std::uint32_t p = tables.objects[a_slot].pipelineIndex;
			ForwardPipelineKey key;
			return p < tables.pipelines.size() && ViewForwardKey(a_view, tables, p, key) && ForwardPipelineOf(a_catalog, key, targets).valid();
		}
		// a_why codes: 1 no catalog or targets, 2 a forward key the forward part cannot draw, 3 no key of the view's,
		// 4 pipeline not in a_catalog, 5 no Z-prepass group pipeline.
		auto fail = [&](std::uint32_t a_cause) {
			if (a_why)
				*a_why = a_cause;
			return false;
		};
		const auto& tables = *static_cast<const SceneTables*>(a_tables);
		const auto& state = impl->View(a_view);
		const ForwardTargets targets = state.LaneTargets();
		if (!a_catalog || targets.colour == DXGI_FORMAT_UNKNOWN || a_slot >= tables.objects.size())
			return fail(1);
		const std::uint32_t p = tables.objects[a_slot].pipelineIndex;
		// A forward key draws with the main set's colour pipeline into the main pass's attachments, so the view's depth must match
		// the main pass's format.
		if (p < tables.pipelines.size() && RasterForward(tables.pipelines[p].rasterFlags))
			return (!RasterRefraction(tables.pipelines[p].rasterFlags) && state.forwardCompatible.load(std::memory_order_acquire) &&
			           IndirectStateValid(*a_catalog)) ||
			       fail(2);
		ForwardPipelineKey key;
		if (p >= tables.pipelines.size() || !ViewForwardKey(a_view, tables, p, key))
			return fail(3);
		if (!ForwardPipelineOf(a_catalog, key, targets).valid())
			return fail(4);
		// Read in place, because building the indirect state per slot copied every pipeline's handles.
		const auto* lookups = static_cast<const Lookups*>(a_lookups);
		const auto setIndex = lookups && p < lookups->pipelines.size() ? lookups->pipelines[p].setIndex : Lookups::kNone;
		return (setIndex != Lookups::kNone && ZGroupPipelineReady(*a_catalog, setIndex)) || fail(5);
	}
}
