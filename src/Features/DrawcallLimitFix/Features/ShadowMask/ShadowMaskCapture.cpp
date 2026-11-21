// Engine-side hooks for the shadow mask family. The world's and menu scene's shadow mask calls run with their draws latched,
// DrawTriShape latches each mask draw, and SetupGeometry counts the passes. Render thread.
// A call the family cannot take reaches the engine with nobody vouching for it. Under the flush latch it is skipped.
// Each such case is a family defect, and its reason is logged once.

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

		// One draw per shadow-casting light, which the engine caps far below this. A longer batch draws in several runs.
		constexpr std::size_t kMaxLatched = 32;

		/** @brief The inherent refusals, each logged once per reason. */
		Family::RefusalLog& Refusals()
		{
			static Family::RefusalLog log("[DCLF] shadow mask: the engine draws");
			return log;
		}

		/** @brief The batch being latched, on the render thread. */
		struct Capture
		{
			bool active = false;
			bool failed = false;  // a draw was refused: the latched part ran, the rest of the batch is the engine's
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
			a_rtv->GetDesc(&desc);
			return static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
		}

		/** @brief Runs the latched part of the batch now, before anything the engine draws after it. */
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

		/** @brief Runs the latched part and leaves the rest of the batch to the engine. Logged once per reason and view. */
		void Refuse(Capture& a_capture, std::string_view a_reason, std::string_view a_detail = {})
		{
			a_capture.failed = true;
			Refusals().Report(std::format("a {} draw: {}", NameOf(a_capture.view), a_reason), a_detail);
			FlushLatched(a_capture);
		}

		/** @brief Latches the draw the engine is about to make. Returns null when latched, else why the engine must draw it. */
		const char* TryLatchDraw(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles, std::string& a_detail)
		{
			if (a_capture.run->draws.size() >= kMaxLatched) {
				const auto view = a_capture.run->view;
				FlushLatched(a_capture);
				a_capture.run = std::make_shared<Run>();
				a_capture.run->view = view;
			}
			auto& run = *a_capture.run;
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* ps = rss.currentPixelShader;
			const auto* state = globals::state;
			if (!vs || !ps || !state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Utility)
				return "the bound shader is not BSUtilityShader";
			// The function binds kSHADOW_MASK as RT0 and no depth. Anything bound at RT1 or later is not written, since the pixel stage has one output.
			if (static_cast<std::uint32_t>(rss.renderTargets[0]) != kShadowMaskTarget || rss.depthStencil != static_cast<std::uint32_t>(-1) ||
				rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return "its target is not kSHADOW_MASK, or a depth target is bound";

			const auto engine = Pipeline::EngineDrawState::Latch();
			Draw draw{};
			draw.key.descriptor = state->modifiedPixelDescriptor;
			draw.key.defineStage = Family::DefineStage::Pixel;
			if (!Family::DescribeStates(draw.key, Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra), nullptr,
					Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor)))
				return "a blend or rasterizer state with no description";
			draw.key.colourFormats[0] = FormatOf(globals::game::renderer->GetRuntimeData().renderTargets[kShadowMaskTarget].RTV);
			if (!draw.key.colourFormats[0])
				return "no shadow mask target";

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

			// The screen quad is read by device address. The run holds its leases until the GPU is done.
			Family::Owners owners;
			Family::Geometry geometry;
			if (const auto result = Family::LatchGeometry(a_shape, a_start, a_triangles, vs->vertexDesc, engine.topology, geometry, owners, &a_detail);
				result != Family::GeometryResult::Ok)
				return Family::ReasonOf(result);
			constants.vertexRef = geometry.vertexRef;
			constants.indexRef = geometry.indexRef;
			constants.vertexLayout[0] = static_cast<std::uint32_t>(geometry.layout);
			constants.vertexLayout[1] = static_cast<std::uint32_t>(geometry.layout >> 32);
			constants.vertexStride = geometry.stride;
			constants.firstIndex = geometry.firstIndex;
			draw.indexCount = geometry.indexCount;

			// Slots the ORG build does not read get a null view and the first sampler, whatever is bound there.
			for (std::uint32_t t = 0; t < kSlots; ++t) {
				const bool read = SlotRead(t);
				auto* view = read ? Unwrap(rss.PSTexture[t]) : nullptr;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, SlotKind(t), GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), constants.textures[t], owners, &failure);
				if (slot == Family::SlotResult::Published) {
					// A depth or target a graph pass writes is read by published id, so the graph orders the write before the masks.
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

			draw.viewport = Family::LatchViewport(engine.depthBias);

			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			run.draws.push_back(std::move(draw));
			return nullptr;
		}

		/** @brief A shadow mask call hook. The draws are latched and the anchor's pass draws them here. */
		template <View kView>
		struct MaskBatch
		{
			static void thunk(void* a_arg1, void* a_arg2, void* a_arg3, void* a_arg4)
			{
				auto& capture = GetCapture();
				if (capture.active) {
					func(a_arg1, a_arg2, a_arg3, a_arg4);
					return;
				}
				if (!FrameAnchors::IsRenderOwner() || !ShadowMaskDraws::Available() || !ShadowMaskDraws::CanTake(kView)) {
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
				// No light cast this frame, so there is nothing to run.
				FlushLatched(capture);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Latches a draw during a latched batch. Applies the engine's pending state as SetDirtyStates would.
		 * That way the kSHADOW_MASK clear lands before the anchor's run, and the dirty bits are consumed as before.
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
			// The draw no longer reaches SetDirtyStates' hook, where Volumetric Shadows, Exponential Height Fog and Skylighting take the sun's
			// cascades from t4. Give them the cascades here.
			if (globals::state->currentPixelDescriptor & static_cast<std::uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderShadowmask))
				globals::state->OnShadowMaskDraw(Unwrap(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().PSTexture[4]));
			return true;
		}

		/** @brief Counts the pass after BSUtilityShader::SetupGeometry. A pass that will not reach DrawTriShape flushes first. */
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
		Family::AddSetup("shadow mask", [] { return ShadowMaskDraws::Setup(); });
		logger::info("[DCLF] shadow mask: hooks installed (the world's and the menu scenes' shadow masks, BSUtilityShader::SetupGeometry)");
	}
}
