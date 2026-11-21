// The main epochs on the render thread: the Z-prepass and colour pass captures at their engine hooks, epochs that share
// a main epoch's shape such as Forward's, each epoch's commit of the installed payload, and the frame blocks from the capture.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ExecutionOwners.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Fatal.h"

namespace DCLF
{
	IndirectDraws::IndirectDraws() :
		impl(std::make_unique<Impl>())
	{}

	IndirectDraws::~IndirectDraws() = default;

	IndirectDraws& IndirectDraws::Get()
	{
		static IndirectDraws draws;
		return draws;
	}

	namespace
	{
		/**
		* @brief Whether a_bound is the engine's kMAIN depth in the version the frame runs. In guest mode the context answers the
		* D3D11 object of the current version, and the field holds a handle over it. In native mode the context answers a handle,
		* the field's or a port's around the same resource.
		*/
		bool IsEngineMainDepth(ID3D11Texture2D* a_bound)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_bound)
				return false;
			auto* field = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture;
			if (a_bound == field)
				return true;
			namespace Tripwire = RenderGraph::Engine::Tripwire;
			if (auto* object = Tripwire::Unwrap(field); object && Tripwire::Unwrap(a_bound) == object)
				return true;
			const auto graph = Tripwire::GraphResourceOf(field);
			return graph && Tripwire::GraphResourceOf(a_bound) == graph;
		}
	}

	void IndirectDraws::CaptureMainPass()
	{
		if (impl->pending)
			impl->pending->Release();
		impl->pending = CaptureBindings();
		// Checked now, in this frame's version. A later frame may run a new version of kMAIN after a resize.
		if (globals::game::renderer)
			impl->mainPassBindsMainDepth = IsEngineMainDepth(impl->pending->depth.get());
		impl->mainMinDepth = impl->pending->minDepth;
		impl->mainMaxDepth = impl->pending->maxDepth;
	}

	void IndirectDraws::CaptureDepthPass(const std::function<void()>& a_hostedWork)
	{
		// Only a native draw flushes the engine's pending world depth target and viewport to D3D11. With every such draw withheld,
		// RSGetViewports can still return the last shadow map's 4096x4096 viewport, which gives an out-of-bounds render area and can
		// lose the device. So apply the pending state here, before the first-person camera replaces it, as BeforeOpaquePass does.
		Engine::ApplyPendingState();
		auto capture = CaptureBindings();
		// Use the engine's main depth, not whatever is bound. Terrain Blending swaps the bound target while terrain draws.
		if (auto* renderer = globals::game::renderer) {
			// The field can hold a tripwire handle, so unwrap it to DXVK's texture.
			if (auto* main = RenderGraph::Engine::Tripwire::Unwrap(renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture)) {
				capture.depth = nullptr;
				capture.depth.copy_from(main);
			}
		}
		// The Z-prepass must write the depth the main pass tests against, or nothing reads DCLF's objects. It writes kMAIN, so the
		// main pass must bind kMAIN. This asks about the slot, not last frame's D3D11 object. After a resize the frame runs a new
		// version, and the Z-prepass must still run with the main opaque epoch: it hosts the main depth clear.
		if (!capture.depth || (impl->mainPassBindsMainDepth && !*impl->mainPassBindsMainDepth)) {
			if (!impl->loggedDepthMismatch) {
				impl->loggedDepthMismatch = true;
				logger::warn("[DCLF] The depth pass and the main pass bind different depth textures; the Z-prepass stays with the main pass");
			}
			capture.Release();
			return;
		}
		if (impl->pending)
			impl->pending->Release();
		impl->pending = std::move(capture);
		// The depth commit runs the hosted epochs' work, the reflection faces and first-person depth, inside its execution.
		impl->hostedWork = a_hostedWork ? &a_hostedWork : nullptr;
		RunEpoch(true);
		impl->hostedWork = nullptr;
	}

	void IndirectDraws::ExecuteColour()
	{
		// The colour epoch assembles from the main pass's own capture, because the depth pass lacked the pixel-stage bindings.
		// Both draw the same tables with the same camera, so the depths agree and the colour pass can test EQUAL.
		RunEpoch(false);
	}

	SharedEpochResult IndirectDraws::SubmitSharedEpoch(std::uint32_t a_epoch, const SharedEpochSubmit& a_submit)
	{
		if (!impl || a_epoch >= EpochCount())
			return {};
		const auto& traits = EpochTraitsOf(a_epoch);
		auto& f = impl->sharedEpochs[a_epoch];
		const std::uint32_t frame = Scene::FrameView::Get().SceneStep();
		// Once a frame. The engine's blended draw has two call sites, under water before the water and otherwise after it.
		if (f.submittedFrame == frame)
			return {};
		++f.reached;
		// It draws what this frame's commit of the shared epoch built and sorted. Without that commit there are no claims.
		const std::uint32_t source = traits.sharesWith;
		const bool mainSource = source != kNoEpoch && EpochTraitsOf(source).shape == ShapeKind::Main;
		const std::size_t jobIndex = mainSource && EpochTraitsOf(source).index == kColourShape ? kAsyncColour : kAsyncZPrepass;
		const MainPayload* shared = mainSource ? impl->committedPayload[jobIndex] : nullptr;
		if (!shared || impl->committedFrame[jobIndex] != frame || Scene::FrameView::Get().NotDrawingYet()) {
			++f.noSharedPayload;
			return {};
		}
		const auto revision = impl->RevisionOf(a_epoch);
		if (!revision) {
			++f.noRecording;
			return {};
		}
		const auto shape = revision.shape ? revision.shape->Value<PassFrame>() : nullptr;
		if (a_submit.empty && shape && a_submit.empty(*shape)) {
			++f.empty;
			return {};
		}
		// Held until this execution retires, since its draws read what the shared commit bound.
		const std::array<const MainPayload*, 1> payloads{ shared };
		auto owners = impl->EpochOwners(payloads);
		if (!owners) {
			++f.failed;
			return {};
		}
		SharedEpochResult result{};
		if (a_submit.prepare) {
			result.optional = a_submit.prepare(a_submit.context, impl->resources.get(), shape.get());
		}
		bool drawn = true;
		// In the GPU-driven graph, the CPU draws the forward family. Its run is handed to the epoch's late pass ahead of its execution.
		const bool cpuForwardHandOff = !revision.direct;
		if (cpuForwardHandOff && !Direct::ForwardReached(result.optional)) {
			++f.noSharedPayload;
			return {};
		}
		const bool ok = Direct::ExecuteEpoch(traits.segment, [&] {
			if (revision.direct)
				drawn = Direct::ForwardReached(result.optional);
			if (revision.recordings && !revision.recordings->recordings.empty())
				impl->SubmitRevisionRecording(a_epoch, *revision.recordings, 0);
			owners.Seal(impl->scene.get());
		}, owners.Handle());
		if (!drawn) {
			++f.noSharedPayload;
			return {};
		}
		if (!ok) {
			if (cpuForwardHandOff)
				Direct::ForwardHandOffCancel();
			++f.failed;
			return {};
		}
		f.submittedFrame = frame;
		++f.epochs;
		result.submitted = true;
		return result;
	}

	void IndirectDraws::RunEpoch(bool a_depthOnly)
	{
		const auto segment = a_depthOnly ? RenderGraphRuntime::Segment::ZPrepass : RenderGraphRuntime::Segment::MainOpaque;
		DCLF_ZONE("CS.DCLF.RunEpoch");
		ScopedPerfEvent event(segment == RenderGraphRuntime::Segment::ZPrepass ? "CS DCLF: Z-prepass (CPU)" : "CS DCLF: main opaque (CPU)");
		const auto start = std::chrono::steady_clock::now();
		const bool depthOnly = a_depthOnly;
		const std::size_t jobIndex = depthOnly ? kAsyncZPrepass : kAsyncColour;
		if (!impl->pending) {
			return;
		}
		auto capture = std::move(*impl->pending);
		impl->pending.reset();
		auto& pipelines = DrawPipelines::Get();
		auto* lighting = EngineShaders::Lighting();
		if (!lighting || !pipelines.Enabled() || !FrameIndirectStateValid()) {
			capture.Release();
			return;
		}
		bool ready = false;
		const auto resourcesBefore = impl->resources;
		try {
			// New main resources are made only before DCLF draws, so their graphs build like any other DCLF build.
			ready = impl->Setup(capture, depthOnly);
		} catch (const std::exception& e) {
			// Graph mode has no engine fallback, so this stops the game.
			::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the main pass's graph resources could not be created: {}", e.what()));
		}
		if (!ready) {
			capture.Release();
			++stats.notReady;
			return;
		}

		auto resources = impl->resources;
		stats.skipped = {};
		stats.missingTextures = {};
		stats.missingVertexConstants = stats.missingPixelConstants = 0;

		// The per-frame constant blocks from the capture's mirrors. Their slot mask feeds the build, and the commit uploads their bytes.
		FrameBlocks blocks;
		impl->PackFrameBlocks(capture, depthOnly, blocks);
		// Revision inputs only the capture has: the viewport, the depth range and the largest frame block sizes seen. A smaller block
		// is zeroed past its bytes. Taken whether or not the epoch is submitted, so revisions never wait for a commit.
		{
			auto& parity = impl->mainEpochInputs;
			const std::size_t shape = depthOnly ? kDepthShape : kColourShape;
			const MainViewport viewportBefore = parity.viewport[shape];
			const FrameBlockSizes sizesBefore = parity.blockSizes[shape];
			const bool knownBefore = parity.known[shape];
			const bool mainRange = impl->mainMaxDepth > 0.0f;
			parity.viewport[shape] = { capture.viewportWidth, capture.viewportHeight, mainRange ? impl->mainMinDepth : capture.minDepth,
				mainRange ? impl->mainMaxDepth : capture.maxDepth };
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				auto& sizes = parity.blockSizes[shape];
				sizes.vs[slot] = std::max(sizes.vs[slot], static_cast<std::uint32_t>(blocks.vs[slot].size()));
				sizes.ps[slot] = std::max(sizes.ps[slot], static_cast<std::uint32_t>(blocks.ps[slot].size()));
			}
			parity.known[shape] = true;
			// Post revision inputs only when they moved.
			if (!knownBefore || !(viewportBefore == parity.viewport[shape]) || !(sizesBefore == parity.blockSizes[shape]))
				impl->PostRevisionInputs();
		}
		// The frame slots and Z-prepass vertex inputs the builds ahead are made for. Taken on frames without claims too,
		// so the first publication's payloads exist before any frame is DCLF's.
		auto& masks = impl->epochMasks[jobIndex];
		masks.vsMask = blocks.vsMask;
		masks.psMask = blocks.psMask;
		masks.known = true;
		if (depthOnly) {
			impl->prepassInputs = true;
		}
		// Post new resources' context for the snapshot builder now, so the next frame's wait needs nothing posted after its adoption.
		const bool newResources = resources != resourcesBefore && resourcesBefore;
		if (newResources)
			impl->PostResourcesAhead();
		// Before DCLF's first covered frame there is nothing to draw, so nothing is submitted.
		if (Scene::FrameView::Get().NotDrawingYet()) {
			capture.Release();
			return;
		}
		// DecideCoverage already checked the revision and payload, so nothing is built or checked here. A payload built for
		// other main resources names their buffers, so the epoch is not submitted.
		const auto revision = impl->RevisionOf(EpochOfShape(ShapeKind::Main, static_cast<std::uint32_t>(depthOnly ? kDepthShape : kColourShape)));
		std::shared_ptr<MainPayload> ahead = impl->installedDraws ? impl->installedDraws->payloads[jobIndex] : nullptr;
		if (!revision || !ahead || ahead->inputs.addresses.identity != resources.get()) {
			// Only resources made at this frame's Setup lead here. Adopting a new payload mid-frame would mismatch the claims, so this
			// frame's colour epoch is not DCLF's, and a later Begin adopts a matching snapshot. Anything else is a defect.
			if (newResources)
				logger::info("[DCLF] main resources made at frame {}'s colour pass ({}x{}): its colour epoch is not drawn (no revision of the frame's is for them); "
							 "a later Begin adopts a snapshot made for them",
					Scene::FrameView::Get().SceneStep(), resources->width, resources->height);
			else if (revision && resources == resourcesBefore && impl->unbuilt[jobIndex]++ == 0)
				logger::warn("[DCLF] the {} epoch of frame {} has no payload for its resources in the installed publication: not submitted <- UNBUILT",
					depthOnly ? "Z-prepass" : "colour", Scene::FrameView::Get().SceneStep());
			capture.Release();
			return;
		}
		MainInputs in = impl->PrepareMainInputs(depthOnly, *resources, blocks.vsMask, blocks.psMask);
		auto& async = stats.async[jobIndex];
		const MainPayload* committed = nullptr;

		auto owners = ExecutionOwners::Make();
		if (!owners) {
			capture.Release();
			++stats.notReady;
			return;
		}
		const bool ok = Direct::ExecuteEpoch(segment, [&] {
			// The lookups stay fixed for the whole frame.
			const auto& lookups = Scene::FrameView::Get().GetLookups();
			in.lookupGeneration = lookups.generation;
			MainPayload* payload = ahead.get();
			++async.used;
			committed = payload;
			// Copied, because a frame that keeps the publication commits the payload again.
			owners.AddPayload(*payload);
			owners.Add(ahead);
			// Keep the adopted snapshot until this execution retires. It is released through the cleanup queue after the GPU is done.
			owners.Add(impl->adoptedSnapshot);
			impl->CommitMainPayload(capture, blocks, in, *payload, resources, stats, owners.List(), revision);
			// The rows this execution draws, which may have been latched by an earlier frame's depth commit.
			owners.Seal(impl->scene.get());
		}, owners.Handle());
		impl->committedPayload[jobIndex] = committed;
		impl->committedFrame[jobIndex] = Scene::FrameView::Get().SceneStep();
		capture.Release();
		++stats.epochs;
		if (!committed) {
			stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			return;
		}
		// Only the colour epoch reads back culling counters, since both epochs write the same count buffer.
		if (ok) {
			const MainPayload* depth = !depthOnly && impl->committedFrame[kAsyncZPrepass] == Scene::FrameView::Get().SceneStep() ? impl->committedPayload[kAsyncZPrepass] : nullptr;
			Diagnostics::Notify(Diagnostics::MainEpochDone{ resources, *committed, depth, depthOnly, impl->InstalledPayload(), stats });
		}

		stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		// The payload was built ahead, so the render thread's time is all remainder.
		stats.partMs[7] = stats.cpuMs;
		if (!ok)
			logger::error("[DCLF] The main-pass epoch failed; the render graph is disabled");
	}

	const Draws::SceneBuffers* IndirectDraws::Scene() const
	{
		return impl->scene.get();
	}

	namespace
	{
		// FrameParams.z in the game's PerFrame, FrameParams at c42.
		inline constexpr std::size_t kFrameParamsZOffset = 42 * 16 + 2 * sizeof(float);

		/** @brief The blocks the epoch always supplies: the game's PerFrame (b12), CS SharedData (b5), feature data and DCLF's fog slot (VS b13). */
		void SupplyFixedBlocks(FrameBlocks& a_out)
		{
			// The engine does not rewrite b12 through the hooked context every frame, so the mirror can be empty, and an empty slot
			// drops every object that reads it. Community Shaders caches this buffer between Map and Unmap, so take it from there.
			{
				const auto& cached = globals::game::frameBufferCached.data;
				const auto* bytes = reinterpret_cast<const std::byte*>(&cached);
				if (a_out.vs[kPerFrameVertexRegister].empty())
					a_out.vs[kPerFrameVertexRegister].assign(bytes, bytes + sizeof(cached));
				if (a_out.ps[kPerFrameVertexRegister].empty())
					a_out.ps[kPerFrameVertexRegister].assign(bytes, bytes + sizeof(cached));
			}
			// CS writes b5 through its ConstantBuffer helper, so the mirror never sees it. Pack the struct CS uploaded instead.
			// The Z-prepass needs these too, since its depth-only pixel stage reads them before the alpha test.
			if (auto* state = globals::state) {
				const auto* shared = reinterpret_cast<const std::byte*>(&state->lastSharedData);
				a_out.ps[kSharedDataRegister].assign(shared, shared + sizeof(State::SharedDataCB));
				if (!state->lastFeatureData.empty()) {
					const auto* feature = reinterpret_cast<const std::byte*>(state->lastFeatureData.data());
					a_out.ps[kFeatureDataRegister].assign(feature, feature + state->lastFeatureData.size());
				}
			}
			// VS b13 is DCLF's own fog block, latched by the commit. It stays empty here but is in the mask, because a pipeline
			// reading an unsupplied slot fails the constants check.
			a_out.vs[kFrameFogRegister].clear();
			a_out.vsMask |= 1u << kFrameFogRegister;
		}
	}

	void IndirectDraws::NoteBlendedFrameParams(float a_z)
	{
		impl->blendedFrameParamsZ = a_z;
	}

	void IndirectDraws::Impl::PackFrameBlocks(const Capture& a_capture, bool a_depthOnly, FrameBlocks& a_out)
	{
		RenderThreadBudget::Part budget(RenderThreadBudget::Bucket::Capture);
		// The colour epoch replays the Z-prepass's vertex-stage bytes, so both agree to the bit and EQUAL passes.
		const bool replayVertexInputs = !a_depthOnly && prepassInputs;
		MirrorFrameBlocks(a_capture, a_depthOnly, replayVertexInputs, a_out);
		SupplyFixedBlocks(a_out);
		CarryMissingBlocks(a_depthOnly, replayVertexInputs, a_out);
		// The forward epoch draws after the composite, where the engine has raised FrameParams.z. Lighting's forward stage needs
		// it for fog and fade. The colour epoch's Lighting is all deferred, so it does not notice.
		if (auto& perFrame = a_out.ps[kPerFrameVertexRegister]; !a_depthOnly && perFrame.size() >= kFrameParamsZOffset + sizeof(float))
			std::memcpy(perFrame.data() + kFrameParamsZOffset, &blendedFrameParamsZ, sizeof(float));
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (!a_out.vs[slot].empty())
				a_out.vsMask |= 1u << slot;
			if (!a_out.ps[slot].empty())
				a_out.psMask |= 1u << slot;
		}

		// Log once per pixel register whether it was bound and mirrored. Bound but not mirrored becomes a null address
		// and drops every object that reads it.
		if (!a_depthOnly && !loggedFrameConstants) {
			loggedFrameConstants = true;
			std::string text;
			const std::uint32_t perDrawPS = Extensions::PerDrawMask(Api::ShaderStage::Pixel);
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				if ((perDrawPS >> slot) & 1)
					continue;
				text += fmt::format("{}b{}={}/{}", text.empty() ? "" : " ", slot, a_capture.psBuffers[slot] ? "bound" : "unbound",
					((a_out.psMask >> slot) & 1) ? "supplied" : "empty");
			}
			logger::info("[DCLF] main-pass per-frame pixel constants: {}", text);
		}
	}

	void IndirectDraws::Impl::MirrorFrameBlocks(const Capture& a_capture, bool a_depthOnly, bool a_replayVertexInputs, FrameBlocks& a_out)
	{
		// Per-frame constant buffers are whatever the main pass binds outside the per-draw slots. The Z-prepass skips the pixel
		// stage's, because the depth pass has not bound them yet and the depth-only build does not read them.
		auto& mirror = ConstantMirror::Get();
		// Core's and the features' per-draw registers, such as Light Limit Fix's PS b3.
		const std::uint32_t perDrawVS = Extensions::PerDrawMask(Api::ShaderStage::Vertex);
		const std::uint32_t perDrawPS = Extensions::PerDrawMask(Api::ShaderStage::Pixel);
		if (a_depthOnly)
			for (auto& bytes : prepassVS)
				bytes.clear();
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (a_replayVertexInputs && !((perDrawVS >> slot) & 1)) {
				const auto& bytes = prepassVS[slot];
				if (!bytes.empty())
					a_out.vs[slot].assign(bytes.begin(), bytes.end());
			}
			for (auto [buffers, perDraw, out] : { std::tuple{ &a_capture.vsBuffers, perDrawVS, &a_out.vs }, std::tuple{ &a_capture.psBuffers, perDrawPS, &a_out.ps } }) {
				if (a_depthOnly && out == &a_out.ps)
					continue;
				if (a_replayVertexInputs && out == &a_out.vs)
					continue;
				auto* buffer = (*buffers)[slot];
				if (!buffer || ((perDraw >> slot) & 1))
					continue;
				mirror.Watch(buffer);
				const auto contents = mirror.Contents(buffer);
				if (!contents.empty())
					(*out)[slot].assign(contents.begin(), contents.end());
				if (a_depthOnly && out == &a_out.vs && !contents.empty())
					prepassVS[slot].assign(contents.begin(), contents.end());
			}
		}
	}

	void IndirectDraws::Impl::CarryMissingBlocks(bool a_depthOnly, bool a_replayVertexInputs, FrameBlocks& a_out)
	{
		// Some feature blocks are bound only in some frames, such as VS b7. A missing one is supplied from its last capture,
		// so the slots stay the same every frame and builds made ahead stay usable.
		auto& carry = carriedBlocks[a_depthOnly ? 0 : 1];
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			for (auto [out, kept, vertex] : { std::tuple{ &a_out.vs[slot], &carry.vs[slot], true }, std::tuple{ &a_out.ps[slot], &carry.ps[slot], false } }) {
				if (!out->empty()) {
					kept->assign(out->begin(), out->end());
					continue;
				}
				if (kept->empty() || (vertex && a_replayVertexInputs) || (!vertex && a_depthOnly))
					continue;
				out->assign(kept->begin(), kept->end());
				++frameSlotsCarried;
				if (const std::uint32_t bit = 1u << (slot + (vertex ? 0 : 16) + (a_depthOnly ? 0 : 8)); !(carriedLogged & bit)) {
					carriedLogged |= bit;
					logger::info("[DCLF] {} epoch, frame {}: {} b{} not bound at the capture, supplied from an earlier one ({} bytes)", a_depthOnly ? "zprepass" : "colour",
						Scene::FrameView::Get().SceneStep(), vertex ? "VS" : "PS", slot, out->size());
				}
				if (vertex && a_depthOnly)
					prepassVS[slot] = *out;
			}
		}
	}

	MainInputs IndirectDraws::Impl::PrepareMainInputs(bool a_depthOnly, const Resources& a_resources, std::uint32_t a_vsMask, std::uint32_t a_psMask)
	{
		MainInputs in;
		in.frameNumber = Scene::FrameView::Get().SceneStep();
		in.depthOnly = a_depthOnly;
		in.vsFrameMask = a_vsMask;
		in.psFrameMask = a_psMask;
		// Buffer addresses, indices and capacities come from the sealed set and layout. Their contents come from the installed payload.
		in.addresses.frameConstants = a_resources.frameConstantsAddress;
		in.addresses.placementsIndex = FrameValues::Get().PlacementsIndex();
		in.addresses.palettesIndex = FrameValues::Get().PalettesIndex();
		in.addresses.shadingIndex = FrameValues::Get().ShadingIndex();
		in.addresses.identity = &a_resources;
		in.tablesGeneration = Scene::FrameView::Get().GetTablesGeneration();
		in.lookupGeneration = Scene::FrameView::Get().GetLookups().generation;
		return in;
	}
}
