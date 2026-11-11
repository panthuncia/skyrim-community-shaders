// The shadow mask family's engine side: the two shadow mask calls' hooks (the world's and a menu scene's), which let the engine's
// function run with its draws latched and hand them to the anchor's pass; the family's DrawTriShape taker, which latches a shadow mask
// draw in place of drawing it; and its BSUtilityShader::SetupGeometry watcher, which has ConstantMirror watch the constant buffers the
// draws read. Render thread.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/Capture/ConstantMirror.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/ShadowMask/ShadowMask.h"

#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "State.h"
#include "Utils/Logging.h"

#include <rhi_helpers.h>

#include <format>
#include <optional>

namespace DCLF::Features::ShadowMask
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		// The most draws one batch latches (ShadowMaskDraws' latch has room for this many): one per shadow-casting light, which the
		// engine caps far below this.
		constexpr std::size_t kMaxLatched = 32;
		// Batches a view's shape must stay the same before its client is made: the ported features publish their resources in the first
		// frames, and a client is made once (Ready), so it waits for them to settle.
		constexpr std::uint32_t kSettleBatches = 30;
		constexpr std::size_t kViews = static_cast<std::size_t>(View::Count);

		/** @brief The refusals and fallbacks, each logged once per reason. */
		Util::ReasonLog& Refusals()
		{
			static Util::ReasonLog log("[DCLF] shadow mask: the engine draws", spdlog::level::warn);
			return log;
		}

		/**
		 * @brief The batch being latched (render thread). As the water's: a view's batches are the graph's only after a probe (a batch the
		 * engine draws while every draw is checked as the graph would take it) found every draw latchable; a draw a later batch cannot
		 * latch makes the rest of that batch the engine's and the view probes again.
		 */
		struct Capture
		{
			bool active = false;
			bool probe = false;
			bool failed = false;
			std::array<bool, kViews> clean{};
			std::array<std::optional<Shape>, kViews> settling;
			std::array<std::uint32_t, kViews> settled{};
			// The published graph textures each view's draws were seen sampling, per slot (Shape::slotIds).
			std::array<std::array<std::string, kSlots>, kViews> knownSlots;
			View view = View::World;
			Shape shape;                               // the batch's: what its view's client must have been made for
			std::array<std::string, kSlots> observed;  // the published ids the batch's draws sampled
			std::shared_ptr<Run> run;
			Family::Passes passes;
		};

		Capture& GetCapture()
		{
			static Capture capture;
			return capture;
		}

		std::uint32_t FormatOf(ID3D11RenderTargetView* a_rtv)
		{
			if (!a_rtv)
				return 0;
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			Unwrap(a_rtv)->GetDesc(&desc);
			return static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
		}

		/**
		 * @brief The draw the engine is about to make, latched: the shadow state SetDirtyStates would apply, the constants the Utility
		 * shader's Setup functions wrote (ConstantMirror), the slots' textures and samplers, the geometry (the screen quad). Null when
		 * latched (or, in a probe, when it could be); otherwise why the graph cannot draw it, and the engine draws it.
		 */
		const char* TryLatchDraw(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& run = *a_capture.run;
			if (run.draws.size() >= kMaxLatched)
				return "more draws than the batch latches";
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* ps = rss.currentPixelShader;
			const auto* state = globals::state;
			if (!vs || !ps || !state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Utility)
				return "the bound shader is not BSUtilityShader";
			// The function binds kSHADOW_MASK as RT0 and no depth (SetCurrentDepthStencilTarget(none)). A target the frame left bound at
			// RT1 or later is not written: the pixel stage has one output.
			if (static_cast<std::uint32_t>(rss.renderTargets[0]) != kShadowMaskTarget || rss.depthStencil != static_cast<std::uint32_t>(-1) ||
				rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return "its target is not kSHADOW_MASK, or a depth target is bound";

			const auto engine = Pipeline::EngineDrawState::Latch();
			Draw draw{};
			draw.key.descriptor = state->currentPixelDescriptor;
			draw.key.defineStage = Family::DefineStage::Pixel;
			draw.key.blend = Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra);
			draw.key.raster = Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor);
			draw.key.colourFormats[0] = FormatOf(globals::game::renderer->GetRuntimeData().renderTargets[kShadowMaskTarget].RTV);
			if (!draw.key.blend || !draw.key.raster || !draw.key.colourFormats[0])
				return "no blend or rasterizer state object, or no shadow mask target";

			// PS b0 (SetupTechnique: VPOSOffset, the cascades' splits, the filter's parameters) and PS b2 (SetupGeometry: the light's
			// projections), as the Setup functions wrote them, and the alpha test reference SetDirtyStates would write into b11.
			auto& constants = draw.constants;
			if (!Family::CopyRows(ps->constantBuffers[0].buffer, constants.psTechnique, kTechniqueRows) ||
				!Family::CopyRows(ps->constantBuffers[2].buffer, constants.psGeometry, kGeometryRows))
				return "ConstantMirror has no copy of its constants yet";
			if (!ps->constantBuffers[2].buffer)
				return "no pixel PerGeometry";
			constants.alphaTestRef = rss.alphaTestEnabled ? rss.alphaTestRef : 0.0f;

			Family::ProgramState programState{};
			draw.program = ShadowMaskDraws::Program(draw.key, *state->currentShader, programState);
			if (!draw.program)
				return programState == Family::ProgramState::Building ? "its ORG program is still building" : "its ORG program cannot be built";

			// The screen quad, read by device address while its leases are held (the run keeps them until the GPU is done).
			Family::Owners owners;
			Family::Geometry geometry;
			std::string geometryDetail;
			if (const auto result = Family::LatchGeometry(a_shape, a_start, a_triangles, vs->vertexDesc, engine.topology, geometry, owners, &geometryDetail);
				result != Family::GeometryResult::Ok) {
				if (!geometryDetail.empty())
					Refusals().Report(std::format("a {} draw whose geometry is refused: {}", NameOf(a_capture.view), Family::ReasonOf(result)), geometryDetail);
				return Family::ReasonOf(result);
			}
			constants.vertexAddress[0] = static_cast<std::uint32_t>(geometry.vertexAddress);
			constants.vertexAddress[1] = static_cast<std::uint32_t>(geometry.vertexAddress >> 32);
			constants.indexAddress[0] = static_cast<std::uint32_t>(geometry.indexAddress);
			constants.indexAddress[1] = static_cast<std::uint32_t>(geometry.indexAddress >> 32);
			constants.vertexLayout[0] = static_cast<std::uint32_t>(geometry.layout);
			constants.vertexLayout[1] = static_cast<std::uint32_t>(geometry.layout >> 32);
			constants.vertexStride = geometry.stride;
			constants.firstIndex = geometry.firstIndex;
			draw.indexCount = geometry.indexCount;

			// The eight slots as the shadow state holds them (the scene depth, the light's shadow maps, the focus shadows' stencil and maps;
			// an empty slot reads a null view of its declared kind) and every slot's sampler.
			for (std::uint32_t t = 0; t < kSlots; ++t) {
				auto* view = Unwrap(rss.PSTexture[t]);
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, SlotKind(t), GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), constants.textures[t], owners, &failure);
				if (slot == Family::SlotResult::Published) {
					// A depth or target a graph pass writes: read by its id, which the view's client declares, so the graph orders the write
					// before the masks. The id joins the view's shape (MaskBatch).
					std::string id;
					if (!Family::PublishedSlot(view, id, constants.textures[t]))
						return "a slot holds a published graph texture without a shader resource view";
					auto& seen = a_capture.observed[t];
					if (seen.empty())
						seen = id;
					else if (seen != id)
						return "a slot holds two published graph textures in one batch";
					if (!a_capture.probe && a_capture.shape.slotIds[t] != id)
						return "a slot holds a published graph texture its client was not made for";
				} else if (slot != Family::SlotResult::Ok) {
					Refusals().Report(std::format("a {} draw's texture t{} could not be imported", NameOf(a_capture.view), t), GpuTextures::DescribeFailure(view, t, failure));
					return "a texture it samples could not be imported";
				}
				constants.samplers[t] = Family::SlotSampler(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying(), owners);
				if (constants.samplers[t] == GpuTextures::kInvalid)
					return "its sampler modes have no graph sampler";
			}

			// The viewport SetDirtyStates would set.
			draw.viewport = Family::LatchViewport(engine.depthBias);

			// The imports are kept, a probe's too (ShadowMaskDraws::Keep): the next batch finds them instead of importing again.
			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			if (a_capture.probe)
				return nullptr;
			run.draws.push_back(std::move(draw));
			return nullptr;
		}

		/** @brief A refusal noted: the batch fails, and the reason is logged once per reason and view. */
		void Refuse(Capture& a_capture, std::string_view a_reason)
		{
			a_capture.failed = true;
			Refusals().Report(std::format("a {} draw: {}", NameOf(a_capture.view), a_reason), a_capture.probe ? "found by a probe" : "");
		}

		/**
		 * @brief A shadow mask function's call: for a taken view, the function runs with its draws latched, then the anchor's pass draws
		 * them at this point. The arguments are passed through as the engine passed them.
		 */
		template <View kView>
		struct MaskBatch
		{
			static void thunk(void* a_arg1, void* a_arg2, void* a_arg3, void* a_arg4)
			{
				auto& capture = GetCapture();
				if (capture.active || !TakesView(kView) || !ShellState::Running() || !FrameAnchors::IsRenderOwner() || !ShadowMaskDraws::Available()) {
					func(a_arg1, a_arg2, a_arg3, a_arg4);
					return;
				}
				Family::BeginBatch();
				const auto index = static_cast<std::size_t>(kView);
				auto& known = capture.knownSlots[index];
				Shape shape;
				shape.view = kView;
				shape.slotIds = known;
				auto& clean = capture.clean[index];
				// A view's client is made once, for a shape that held for kSettleBatches batches.
				auto& settling = capture.settling[index];
				if (!settling || *settling != shape) {
					settling = shape;
					capture.settled[index] = 0;
				}
				const bool settled = ++capture.settled[index] >= kSettleBatches;
				// Until the view's client is made and in the built graph, the engine draws its batches as probes: they learn the published
				// textures the draws sample (the shape) and ask for the programs.
				const bool ready = settled && ShadowMaskDraws::Ready(shape);
				const bool probe = !ready || !clean;
				// Draws are withheld only when the anchor's run will draw them (a probe withholds nothing).
				if (!probe && !ShadowMaskDraws::CanRun(kView)) {
					Refusals().Report(std::format("a {} batch whose anchor cannot run now", NameOf(kView)), "the ring is full or the client left the built graph");
					func(a_arg1, a_arg2, a_arg3, a_arg4);
					return;
				}
				auto run = std::make_shared<Run>();
				run->view = kView;
				capture.view = kView;
				capture.run = run;
				capture.shape = shape;
				capture.observed = {};
				capture.probe = probe;
				capture.failed = false;
				capture.passes.Begin();
				capture.active = true;
				func(a_arg1, a_arg2, a_arg3, a_arg4);
				capture.active = false;
				capture.run.reset();
				if (!capture.passes.End()) {
					Refusals().Report(capture.passes.Describe(std::format("{} shadow mask", NameOf(kView))));
					Refuse(capture, "a pass of the batch drew through another draw function than DrawTriShape");
				}
				// The published ids the batch sampled join the view's (a slot keeps its id once seen); a new one is the next batch's shape.
				for (std::uint32_t t = 0; t < kSlots; ++t)
					if (!capture.observed[t].empty())
						known[t] = capture.observed[t];
				if (probe) {
					clean = ready && !capture.failed;
					if (clean)
						logger::info("[DCLF] shadow mask: every {} shadow mask draw can be drawn by the graph; its batches are the graph's from the next", NameOf(kView));
					ShadowMaskDraws::Keep(std::move(run->owners));
					return;
				}
				if (capture.failed) {
					clean = false;
					Refusals().Report(std::format("a {} batch the graph had taken, from a draw it could not latch on", NameOf(kView)), "the view probes again");
				}
				// A batch whose draws all stayed the engine's (no light cast this frame) has nothing to run.
				if (run->draws.empty()) {
					ShadowMaskDraws::Keep(std::move(run->owners));
					return;
				}
				// A run that was not submitted lost its draws for this frame: the view probes again.
				if (!ShadowMaskDraws::Submit(std::move(run)))
					clean = false;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief The family's DrawTriShape taker: during a latched shadow mask batch, the draw is latched instead. A taken draw then applies
		 * the engine's pending state as the draw function's own SetDirtyStates would: the kSHADOW_MASK clear the function asked for lands
		 * before the anchor's run (on D3D11, or the engine skeleton's clear pass), and the dirty bits are consumed as before.
		 */
		bool TakeDraw(const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& capture = GetCapture();
			if (!capture.active || !capture.run)
				return false;
			capture.passes.OnDraw();
			// A probe checks every draw; a batch the graph draws stops at the first draw it cannot take, so the engine's draws of the
			// batch stay after the ones latched before.
			if (capture.failed && !capture.probe)
				return false;
			if (const char* reason = TryLatchDraw(capture, a_shape, a_start, a_triangles)) {
				Refuse(capture, reason);
				return false;
			}
			if (capture.probe)
				return false;
			Engine::ApplyPendingState();
			return true;
		}

		/**
		 * @brief After BSUtilityShader::SetupGeometry: during a latched batch, ConstantMirror watches the pixel stage's b0 and b2. A buffer
		 * is mirrored from its next write on, so a batch's first sight of one draws on the engine.
		 */
		void OnSetupGeometry(RE::BSRenderPass* a_pass)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return;
			capture.passes.OnSetup(a_pass);
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			auto& mirror = ConstantMirror::Get();
			if (const auto* ps = rss.currentPixelShader) {
				for (const std::uint32_t group : { 0u, 2u })
					if (ps->constantBuffers[group].buffer)
						mirror.Watch(reinterpret_cast<ID3D11Buffer*>(ps->constantBuffers[group].buffer));
			}
		}
	}

	void PreflightHooks()
	{
		if (!Enabled())
			return;
		const bool known = Address::kWorldMasks.Known() && Address::kMenuMasks.Known() && Family::Address::kDrawTriShape.Known();
		HookSites::RequireKnown("the shadow mask calls", known);
		if (!known)
			return;
		HookSites::RequireCall("the world's shadow masks (Main::RenderShadowMasks' call)", REL::Offset(Address::kWorldMasks).address());
		HookSites::RequireCall("a menu scene's shadow masks (UI3DSceneManager::Render's call)", REL::Offset(Address::kMenuMasks).address());
	}

	void InstallHooks()
	{
		if (!Address::kWorldMasks.Known() || !Address::kMenuMasks.Known() || !Family::AddDrawTaker("shadow mask", &TakeDraw)) {
			logger::warn("[DCLF] shadow mask: this runtime has no address for the shadow mask calls; the shadow masks stay the engine's");
			return;
		}
		stl::write_thunk_call<MaskBatch<View::World>>(REL::Offset(Address::kWorldMasks).address());
		stl::write_thunk_call<MaskBatch<View::Menu>>(REL::Offset(Address::kMenuMasks).address());
		Family::WatchSetupGeometry<RE::BSShader::Type::Utility>(RE::VTABLE_BSUtilityShader[0], &OnSetupGeometry);
		logger::info("[DCLF] shadow mask: hooks installed (the world's and the menu scenes' shadow masks, BSUtilityShader::SetupGeometry)");
	}
}
