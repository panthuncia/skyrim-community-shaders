// The blood splatter family's engine side: the screen splatter batch's hook, which lets the engine's batch run with its draws latched
// and hands them to the anchor's pass; the family's DrawTriShape taker, which latches a splatter draw in place of drawing it; its
// BSBloodSplatterShader::SetupGeometry watcher, which counts the batch's passes; and the setup where the client is made once. Render
// thread.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/BloodSplatter/BloodSplatter.h"

#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "State.h"
#include "Utils/Logging.h"

#include <rhi_helpers.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace DCLF::Features::BloodSplatter
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;
		using Stage = Family::Constants::Stage;

		// RE::RENDER_TARGETS::kFRAMEBUFFER: the back buffer, which Main::RenderScreenSplatter binds.
		constexpr std::int32_t kFramebuffer = 0;
		// The most draws one batch latches (BloodSplatterDraws' latch has room for this many). A hit draws a few splatters.
		constexpr std::size_t kMaxLatched = 32;

		/** @brief The inherent refusals, each logged once per reason. */
		Util::ReasonLog& Refusals()
		{
			static Util::ReasonLog log("[DCLF] blood splatter: the engine draws", spdlog::level::warn);
			return log;
		}

		/** @brief What the family leaves to the engine on purpose (Flush::EngineDraws): a batch or its rest. */
		const Family::Flush::Reason& EngineReason()
		{
			static const Family::Flush::Reason reason{ "blood splatter: left to the engine" };
			return reason;
		}

		/** @brief The batch being latched (render thread). */
		struct Capture
		{
			bool active = false;
			bool failed = false;  // a draw was refused: the latched part ran, the rest of the batch is the engine's
			std::shared_ptr<Run> run;
			Family::Passes passes;
			Family::EngineRest engineRest;  // the rest of a refused batch, vouched for until the batch returns
		};

		Capture& GetCapture()
		{
			static Capture capture;
			return capture;
		}

		/** @brief The latched part of the batch, run now: in the engine's order, before anything the engine draws after it. */
		void FlushLatched(Capture& a_capture)
		{
			auto run = std::move(a_capture.run);
			if (!run)
				return;
			if (run->draws.empty()) {
				BloodSplatterDraws::Keep(std::move(run->owners));
				return;
			}
			BloodSplatterDraws::Submit(std::move(run));
		}

		/** @brief A refusal: the latched part runs now and the rest of the batch is the engine's; logged once per reason and technique. */
		void Refuse(Capture& a_capture, std::string_view a_reason, std::uint32_t a_technique)
		{
			a_capture.failed = true;
			Refusals().Report(std::format("a draw of technique {}: {}", a_technique, a_reason));
			FlushLatched(a_capture);
			if (!a_capture.engineRest)
				a_capture.engineRest.emplace(EngineReason());
		}

		/** @brief A splatter draw latched (null); the reason when it cannot be (the engine draws it then). */
		const char* TryLatchDraw(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& run = *a_capture.run;
			if (run.draws.size() >= kMaxLatched)
				return "more draws than the batch latches";
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* ps = rss.currentPixelShader;
			const auto* state = globals::state;
			if (!vs || !ps || !state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::BloodSplatter)
				return "the bound shader is not BSBloodSplatterShader";
			const auto engine = Pipeline::EngineDrawState::Latch();
			// SetupTechnique turns the depth test off (depth mode 0) and the screen blood uses no stencil: the program has no depth target.
			if (engine.depthMode != 0 || engine.stencilMode != 0)
				return "it tests depth or stencil";
			if (static_cast<std::int32_t>(rss.renderTargets[0]) != kFramebuffer || static_cast<std::int32_t>(rss.renderTargets[1]) >= 0 ||
				rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return "its targets are not the back buffer alone";

			Draw draw{};
			// The descriptor CS looks its shader up by (State::ModifyShaderLookup), which ShaderCompilationTask compiled the stages for.
			draw.key.descriptor = state->modifiedPixelDescriptor;
			draw.key.defineStage = Family::DefineStage::Pixel;
			if (!Family::DescribeStates(draw.key, Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra), nullptr,
					Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor)))
				return "a blend or rasterizer state with no description";
			const auto& target = globals::game::renderer->GetRuntimeData().renderTargets[kFramebuffer];
			if (target.RTV) {
				D3D11_RENDER_TARGET_VIEW_DESC desc{};
				Unwrap(target.RTV)->GetDesc(&desc);
				draw.key.colourFormats[0] = static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
			}
			if (!draw.key.colourFormats[0])
				return "no back buffer";

			// SetupGeometry's constants, VS b2 c0-c5 (WorldViewProj, LightLoc, Ctrl) and PS b2 c0 (Alpha), as the engine constants capture saw them.
			auto& constants = draw.constants;
			const auto use = Family::CaptureConstants();
			if (!use.Valid())
				return "the engine constants capture is not active";
			if (!Family::ConstantBytes(use, Stage::Vertex, 2))
				return "no vertex PerGeometry";
			Family::CopyRows(use, Stage::Vertex, 2, constants.worldViewProj, 6);
			Family::CopyRows(use, Stage::Pixel, 2, &constants.alpha, 1);

			draw.program = BloodSplatterDraws::Program(draw.key, *state->currentShader);
			if (!draw.program)
				return "its ORG program cannot be built";

			Family::Owners owners;
			Family::Geometry geometry;
			std::string geometryDetail;
			if (const auto result = Family::LatchGeometry(a_shape, a_start, a_triangles, vs->vertexDesc, engine.topology, geometry, owners, &geometryDetail);
				result != Family::GeometryResult::Ok) {
				if (!geometryDetail.empty())
					Refusals().Report(std::format("a draw of technique {} whose geometry is refused: {}", state->currentPixelDescriptor & 0xFF, Family::ReasonOf(result)),
						geometryDetail);
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

			// t0-t3 as the shadow state holds them (the splatter's colour and alpha, the flare's colour and HDR texture).
			for (std::uint32_t t = 0; t < 4; ++t) {
				auto* view = Unwrap(rss.PSTexture[t]);
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, GpuTextures::ViewKind::Texture2D, GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), constants.textures[t],
					owners, &failure);
				if (slot == Family::SlotResult::Published)
					return "a texture slot holds a published graph texture";
				if (slot != Family::SlotResult::Ok) {
					Refusals().Report(std::format("a draw's texture t{} could not be imported", t), GpuTextures::DescribeFailure(view, t, failure));
					return "a texture it samples could not be imported";
				}
				constants.samplers[t] = Family::SlotSampler(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying(), owners);
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

		/** @brief FinishAccumulatingScreenSplatter's RenderBatches (SE 99956 +0x1D, AE 106601 +0x21): the screen blood's batch. */
		struct SplatterBatch
		{
			static void thunk(void* a_accumulator, std::uint32_t a_first, std::uint32_t a_last, std::uint32_t a_flags, std::int32_t a_group)
			{
				auto& capture = GetCapture();
				if (capture.active) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				// The client's back buffer, and an anchor that can run now: otherwise the engine draws the batch (logged once each).
				if (!ShellState::Running() || !FrameAnchors::IsRenderOwner() || !BloodSplatterDraws::Available() || (Family::BeginBatch(), !BloodSplatterDraws::CanTake())) {
					Family::Flush::EngineDraws scope(EngineReason());
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				capture.run = std::make_shared<Run>();
				capture.failed = false;
				capture.passes.Begin();
				capture.active = true;
				func(a_accumulator, a_first, a_last, a_flags, a_group);
				capture.active = false;
				if (!capture.passes.End())
					Refusals().Report(capture.passes.Describe("screen blood"));
				FlushLatched(capture);
				capture.engineRest.reset();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The family's DrawTriShape taker: during a latched splatter batch, the draw is latched instead. */
		bool TakeDraw(const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return false;
			capture.passes.OnDraw();
			if (capture.failed || !capture.run)
				return false;
			if (const char* reason = TryLatchDraw(capture, a_shape, a_start, a_triangles)) {
				const auto* state = globals::state;
				Refuse(capture, reason, state ? state->currentPixelDescriptor & 0xFF : 0xFF);
				return false;
			}
			return true;
		}

		/** @brief After BSBloodSplatterShader::SetupGeometry: counts the pass; one that will not reach DrawTriShape ends the latched part. */
		void OnSetupGeometry(RE::BSRenderPass* a_pass)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return;
			if (!capture.passes.OnSetup(a_pass) && !capture.failed)
				Refuse(capture, "a pass the dispatcher draws through another function than DrawTriShape", 0xFF);
		}
	}

	void PreflightHooks()
	{
		if (!Enabled())
			return;
		const bool known = Address::kSplatterBatch.Known() && Family::Address::kDrawTriShape.Known();
		HookSites::RequireKnown("the screen blood batch", known);
		if (known)
			HookSites::RequireCall("the screen blood batch (FinishAccumulatingScreenSplatter's RenderBatches)", REL::Offset(Address::kSplatterBatch).address());
	}

	void InstallHooks()
	{
		if (!Address::kSplatterBatch.Known() || !Family::AddDrawTaker("blood splatter", &TakeDraw)) {
			logger::warn("[DCLF] blood splatter: this runtime has no address for the screen blood batch; it stays the engine's");
			return;
		}
		stl::write_thunk_call<SplatterBatch>(REL::Offset(Address::kSplatterBatch).address());
		Family::WatchSetupGeometry<RE::BSShader::Type::BloodSplatter>(RE::VTABLE_BSBloodSplatterShader[0], &OnSetupGeometry);
		// The client is made on the main thread once the graph runs (the main menu), before any blood is drawn.
		Family::AddSetup("blood splatter", [] { return BloodSplatterDraws::Setup(); });
		logger::info("[DCLF] blood splatter: hooks installed (the screen blood batch, BSBloodSplatterShader::SetupGeometry)");
	}
}
