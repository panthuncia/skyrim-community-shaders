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
#include "RenderGraph/PublishedResources.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/Logging.h"

#include <rhi_helpers.h>

#include <algorithm>
#include <format>
#include <string>

namespace DCLF::Features::ShadowMask
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		// The most draws one batch latches (ShadowMaskDraws' latch has room for this many): one per shadow-casting light, which the
		// engine caps far below this.
		constexpr std::size_t kMaxLatched = 32;
		// Batches a view's probes run before its client is made: its programs and ConstantMirror's copies are asked for first.
		constexpr std::uint32_t kSettleBatches = 30;
		// Batches in a row after which a view that still stays the engine's says why (Lasting): about two seconds of frames.
		constexpr std::uint32_t kLastingBatches = 120;
		constexpr std::size_t kViews = static_cast<std::size_t>(View::Count);

		/** @brief The refusals and fallbacks, each logged once per reason. */
		Util::ReasonLog& Refusals()
		{
			static Util::ReasonLog log("[DCLF] shadow mask: the engine draws", spdlog::level::warn);
			return log;
		}

		/**
		 * @brief Why a view stays the engine's for good, logged once per view and reason. Refusals logs each reason only the first time
		 * it is seen, so a refusal that lasts would otherwise go quiet after the first frames.
		 */
		Util::ReasonLog& Lasting()
		{
			static Util::ReasonLog log("[DCLF] shadow mask: a view stays the engine's", spdlog::level::warn);
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
			bool ready = false;  // the view's client is in the built graph: its published inputs are read
			std::array<bool, kViews> clean{};
			std::array<std::uint32_t, kViews> settled{};
			std::array<std::uint32_t, kViews> refusedProbes{};  // probes in a row that found a draw the graph cannot take, once ready
			std::array<std::uint32_t, kViews> joining{};        // batches in a row whose view had settled but was not ready
			std::array<std::string, kViews> lastRefusal{};      // the view's last refusal, for the Lasting log
			std::string refusalDetail;                          // what TryLatchDraw's last refusal was about (a slot, the geometry)
			View view = View::World;
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
			a_capture.refusalDetail.clear();
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
				a_capture.refusalDetail = std::move(geometryDetail);
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

			// The slots the ORG build reads as the shadow state holds them (the scene depth, the light's shadow maps; an empty slot reads a
			// null view of its declared kind) and their samplers. Every other slot reads a null view and the first sampler, whatever is
			// bound there (SlotRead: the focus shadows' stencil at t5 among them, which the engine's draw reads as 0).
			for (std::uint32_t t = 0; t < kSlots; ++t) {
				const bool read = SlotRead(t);
				auto* view = read ? Unwrap(rss.PSTexture[t]) : nullptr;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, SlotKind(t), GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), constants.textures[t], owners, &failure);
				if (slot == Family::SlotResult::Published) {
					// A depth or target a graph pass writes: read by its id, which the view's client declares, so the graph orders the write
					// before the masks. Before the client is ready, a probe only learns programs.
					const auto id = PublishedResources::ExportedId(view);
					const auto result = ShadowMaskDraws::ReadPublished(a_capture.view, id, constants.textures[t]);
					if (result != Family::InputResult::Graph && a_capture.ready) {
						Refusals().Report(std::format("a {} draw's t{} holds '{}'", NameOf(a_capture.view), t, id), Family::ReasonOf(result));
						a_capture.refusalDetail = std::format("t{} holds '{}': {}", t, id, Family::ReasonOf(result));
						return "a slot holds a published graph texture its client cannot read";
					}
				} else if (slot != Family::SlotResult::Ok) {
					a_capture.refusalDetail = GpuTextures::DescribeFailure(view, t, failure);
					Refusals().Report(std::format("a {} draw's texture t{} could not be imported", NameOf(a_capture.view), t), a_capture.refusalDetail);
					return "a texture it samples could not be imported";
				}
				constants.samplers[t] = read ? Family::SlotSampler(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying(), owners) :
				                               Family::SlotSampler(0, 0, owners);
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
			auto& lastReason = a_capture.lastRefusal[static_cast<std::size_t>(a_capture.view)];
			lastReason = a_reason;
			if (!a_capture.refusalDetail.empty())
				lastReason += std::format(" ({})", a_capture.refusalDetail);
			Refusals().Report(std::format("a {} draw: {}", NameOf(a_capture.view), a_reason), a_capture.probe ? "found by a probe" : "");
		}

		/**
		 * @brief After a probe that did not make a_view clean: counts it, and once the view has stayed the engine's for kLastingBatches
		 * batches in a row, logs why (Lasting). Either its client is not in the anchor's built graph yet (not made, refused, or still
		 * joining), or its probes keep finding a draw the graph cannot take (the last refusal is named).
		 */
		void NoteUnclean(Capture& a_capture, View a_view, bool a_ready)
		{
			const auto index = static_cast<std::size_t>(a_view);
			if (!a_ready) {
				a_capture.refusedProbes[index] = 0;
				if (a_capture.settled[index] < kSettleBatches || ++a_capture.joining[index] < kLastingBatches)
					return;
				Lasting().Report(std::format("its {} client is not in the EngineDraws anchor's built graph", NameOf(a_view)),
					ShadowMaskDraws::HasClient(a_view) ? std::format("{}+ batches after it was made (FrameAnchors::RequestJoin)", kLastingBatches) :
					                                     std::string("it was not made (see the line above for why)"));
				return;
			}
			a_capture.joining[index] = 0;
			if (++a_capture.refusedProbes[index] < kLastingBatches)
				return;
			const auto& reason = a_capture.lastRefusal[index];
			Lasting().Report(std::format("every {} probe finds a draw the graph cannot take: {}", NameOf(a_view), reason.empty() ? "no reason recorded" : reason),
				std::format("{}+ probes in a row since its client joined", kLastingBatches));
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
				Shape shape;
				shape.view = kView;
				auto& clean = capture.clean[index];
				// A view's client is made once, after kSettleBatches batches.
				auto& settled = capture.settled[index];
				settled = (std::min)(settled + 1, kSettleBatches);
				// Until the view's client is made and in the built graph, the engine draws its batches as probes: they ask for the programs
				// and ConstantMirror's copies.
				const bool ready = settled >= kSettleBatches && ShadowMaskDraws::Ready(shape);
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
				capture.ready = ready;
				capture.probe = probe;
				capture.failed = false;
				capture.passes.Begin();
				capture.active = true;
				func(a_arg1, a_arg2, a_arg3, a_arg4);
				capture.active = false;
				capture.run.reset();
				if (!capture.passes.End()) {
					capture.refusalDetail = capture.passes.Describe(std::format("{} shadow mask", NameOf(kView)));
					Refusals().Report(capture.refusalDetail);
					Refuse(capture, "a pass of the batch drew through another draw function than DrawTriShape");
				}
				if (probe) {
					clean = ready && !capture.failed;
					if (clean) {
						capture.refusedProbes[index] = 0;
						capture.joining[index] = 0;
						logger::info("[DCLF] shadow mask: every {} shadow mask draw can be drawn by the graph; its batches are the graph's from the next", NameOf(kView));
					} else {
						NoteUnclean(capture, kView, ready);
					}
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
			// The draw no longer reaches SetDirtyStates' hook (State::Draw), where Volumetric Shadows, Exponential Height Fog and
			// Skylighting take the sun's shadow cascades from its t4: give them here.
			if (globals::state->currentPixelDescriptor & static_cast<std::uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderShadowmask))
				globals::state->OnShadowMaskDraw(Unwrap(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().PSTexture[4]));
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
