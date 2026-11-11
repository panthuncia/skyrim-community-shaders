// The shadow mask family's engine side: the two shadow mask calls' hooks (the world's and a menu scene's), which let the engine's
// function run with its draws latched and hand them to the anchor's pass; the family's DrawTriShape taker, which latches a shadow mask
// draw in place of drawing it; and its BSUtilityShader::SetupGeometry watcher, which counts a batch's passes. Render thread.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

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
		using Stage = Family::Constants::Stage;

		// The most draws one batch latches (ShadowMaskDraws' latch has room for this many): one per shadow-casting light, which the
		// engine caps far below this.
		constexpr std::size_t kMaxLatched = 32;

		/** @brief The inherent refusals, each logged once per reason. */
		Util::ReasonLog& Refusals()
		{
			static Util::ReasonLog log("[DCLF] shadow mask: the engine draws", spdlog::level::warn);
			return log;
		}

		/** @brief What the family leaves to the engine on purpose (Flush::EngineDraws): a batch or its rest. */
		const Family::Flush::Reason& EngineReason()
		{
			static const Family::Flush::Reason reason{ "shadow mask: left to the engine" };
			return reason;
		}

		/** @brief The batch being latched (render thread). */
		struct Capture
		{
			bool active = false;
			bool failed = false;  // a draw was refused: the latched part ran, the rest of the batch is the engine's
			View view = View::World;
			std::shared_ptr<Run> run;
			Family::Passes passes;
			Family::EngineRest engineRest;  // the rest of a refused batch, vouched for until the batch returns
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

		/** @brief The latched part of the batch, run now: in the engine's order, before anything the engine draws after it. */
		void FlushLatched(Capture& a_capture)
		{
			auto run = std::move(a_capture.run);
			if (!run)
				return;
			if (run->draws.empty()) {
				ShadowMaskDraws::Keep(std::move(run->owners));
				return;
			}
			ShadowMaskDraws::Submit(std::move(run));
		}

		/** @brief A refusal: the latched part runs now and the rest of the batch is the engine's; logged once per reason and view. */
		void Refuse(Capture& a_capture, std::string_view a_reason, std::string_view a_detail = {})
		{
			a_capture.failed = true;
			Refusals().Report(std::format("a {} draw: {}", NameOf(a_capture.view), a_reason), a_detail);
			FlushLatched(a_capture);
			if (!a_capture.engineRest)
				a_capture.engineRest.emplace(EngineReason());
		}

		/**
		 * @brief The draw the engine is about to make, latched: the shadow state SetDirtyStates would apply, the constants the Utility
		 * shader's Setup functions wrote (the engine constants capture), the slots' textures and samplers, the geometry (the screen quad).
		 * Null when latched; otherwise why the graph cannot draw it (a_detail: what it was about), and the engine draws it.
		 */
		const char* TryLatchDraw(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles, std::string& a_detail)
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
			// The descriptor CS looks its shader up by (State::ModifyShaderLookup), which ShaderCompilationTask compiled the stages for.
			draw.key.descriptor = state->modifiedPixelDescriptor;
			draw.key.defineStage = Family::DefineStage::Pixel;
			if (!Family::DescribeStates(draw.key, Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra), nullptr,
					Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor)))
				return "a blend or rasterizer state with no description";
			draw.key.colourFormats[0] = FormatOf(globals::game::renderer->GetRuntimeData().renderTargets[kShadowMaskTarget].RTV);
			if (!draw.key.colourFormats[0])
				return "no shadow mask target";

			// PS b0 (SetupTechnique: VPOSOffset, the cascades' splits, the filter's parameters) and PS b2 (SetupGeometry: the light's
			// projections), as the engine constants capture saw them, and the alpha test reference SetDirtyStates would write into b11.
			auto& constants = draw.constants;
			const auto use = Family::CaptureConstants();
			if (!use.Valid())
				return "the engine constants capture is not active";
			if (!Family::ConstantBytes(use, Stage::Pixel, 2))
				return "no pixel PerGeometry";
			Family::CopyRows(use, Stage::Pixel, 0, constants.psTechnique, kTechniqueRows);
			Family::CopyRows(use, Stage::Pixel, 2, constants.psGeometry, kGeometryRows);
			constants.alphaTestRef = rss.alphaTestEnabled ? rss.alphaTestRef : 0.0f;

			draw.program = ShadowMaskDraws::Program(draw.key, *state->currentShader);
			if (!draw.program)
				return "its ORG program cannot be built";

			// The screen quad, read by device address while its leases are held (the run keeps them until the GPU is done).
			Family::Owners owners;
			Family::Geometry geometry;
			if (const auto result = Family::LatchGeometry(a_shape, a_start, a_triangles, vs->vertexDesc, engine.topology, geometry, owners, &a_detail);
				result != Family::GeometryResult::Ok)
				return Family::ReasonOf(result);
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
					// before the masks.
					const auto id = PublishedResources::ExportedId(view);
					const auto result = ShadowMaskDraws::ReadPublished(a_capture.view, id, constants.textures[t]);
					if (result != Family::InputResult::Graph) {
						a_detail = std::format("t{} holds '{}': {}", t, id, Family::ReasonOf(result));
						return result == Family::InputResult::PublishedLate ? "a slot holds a texture published during play" : "a slot holds a published texture its client does not list";
					}
				} else if (slot != Family::SlotResult::Ok) {
					a_detail = GpuTextures::DescribeFailure(view, t, failure);
					return "a texture it samples could not be imported";
				}
				constants.samplers[t] = SamplerRead(t) ? Family::SlotSampler(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying(), owners) :
				                                         Family::SlotSampler(0, 0, owners);
				if (constants.samplers[t] == GpuTextures::kInvalid)
					return "its sampler modes have no graph sampler";
			}

			// The viewport SetDirtyStates would set.
			draw.viewport = Family::LatchViewport(engine.depthBias);

			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			run.draws.push_back(std::move(draw));
			return nullptr;
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
				if (capture.active || !TakesView(kView)) {
					func(a_arg1, a_arg2, a_arg3, a_arg4);
					return;
				}
				if (!ShellState::Running() || !FrameAnchors::IsRenderOwner() || !ShadowMaskDraws::Available() || !ShadowMaskDraws::CanTake(kView)) {
					Family::Flush::EngineDraws scope(EngineReason());
					func(a_arg1, a_arg2, a_arg3, a_arg4);
					return;
				}
				Family::BeginBatch();
				capture.view = kView;
				capture.run = std::make_shared<Run>();
				capture.run->view = kView;
				capture.failed = false;
				capture.passes.Begin();
				capture.active = true;
				func(a_arg1, a_arg2, a_arg3, a_arg4);
				capture.active = false;
				if (!capture.passes.End())
					Refusals().Report(capture.passes.Describe(std::format("{} shadow mask", NameOf(kView))));
				// A batch whose draws all stayed the engine's (no light cast this frame) has nothing to run.
				FlushLatched(capture);
				capture.engineRest.reset();
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
			if (!capture.active)
				return false;
			capture.passes.OnDraw();
			if (capture.failed || !capture.run)
				return false;
			std::string detail;
			if (const char* reason = TryLatchDraw(capture, a_shape, a_start, a_triangles, detail)) {
				Refuse(capture, reason, detail);
				return false;
			}
			Engine::ApplyPendingState();
			// The draw no longer reaches SetDirtyStates' hook (State::Draw), where Volumetric Shadows, Exponential Height Fog and
			// Skylighting take the sun's shadow cascades from its t4: give them here.
			if (globals::state->currentPixelDescriptor & static_cast<std::uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderShadowmask))
				globals::state->OnShadowMaskDraw(Unwrap(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().PSTexture[4]));
			return true;
		}

		/** @brief After BSUtilityShader::SetupGeometry: counts the pass; one that will not reach DrawTriShape ends the latched part. */
		void OnSetupGeometry(RE::BSRenderPass* a_pass)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return;
			if (!capture.passes.OnSetup(a_pass) && !capture.failed)
				Refuse(capture, "a pass the dispatcher draws through another function than DrawTriShape");
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
		// Both views' clients are made on the main thread once the graph runs (the main menu), before any world frame.
		Family::AddSetup("shadow mask", [] { return ShadowMaskDraws::Setup(); });
		logger::info("[DCLF] shadow mask: hooks installed (the world's and the menu scenes' shadow masks, BSUtilityShader::SetupGeometry)");
	}
}
