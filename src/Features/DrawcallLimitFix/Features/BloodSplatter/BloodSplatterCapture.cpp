// The blood splatter family's engine side: the screen splatter batch's hook, which lets the engine's batch run with its draws latched
// and hands them to the anchor's pass; the family's DrawTriShape taker, which latches a splatter draw in place of drawing it; its
// BSBloodSplatterShader::SetupGeometry watcher, which has ConstantMirror watch its constant buffers; and the main-thread tick where the
// client is made once. Render thread.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/Capture/ConstantMirror.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/BloodSplatter/BloodSplatter.h"

#include "Globals.h"
#include "RenderGraph/Engine/MainTick.h"
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

		// RE::RENDER_TARGETS::kFRAMEBUFFER: the back buffer, which Main::RenderScreenSplatter binds.
		constexpr std::int32_t kFramebuffer = 0;
		// The most draws one batch latches (BloodSplatterDraws' latch has room for this many). A hit draws a few splatters.
		constexpr std::size_t kMaxLatched = 32;

		/** @brief The refusals and fallbacks, each logged once per reason. */
		Util::ReasonLog& Refusals()
		{
			static Util::ReasonLog log("[DCLF] blood splatter: the engine draws", spdlog::level::warn);
			return log;
		}

		/** @brief The batch being latched, as the water's and the sky's (render thread). */
		struct Capture
		{
			bool active = false;
			bool probe = false;
			bool failed = false;
			bool clean = false;
			std::shared_ptr<Run> run;
			Family::Passes passes;
		};

		Capture& GetCapture()
		{
			static Capture capture;
			return capture;
		}

		/** @brief A refusal noted: the batch fails, its later draws are the engine's; logged once per reason and technique. */
		void Refuse(Capture& a_capture, std::string_view a_reason, std::uint32_t a_technique)
		{
			a_capture.failed = true;
			Refusals().Report(std::format("a draw of technique {}: {}", a_technique, a_reason), a_capture.probe ? "found by a probe" : "");
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
			draw.key.descriptor = state->currentPixelDescriptor;
			draw.key.defineStage = Family::DefineStage::Pixel;
			draw.key.blend = Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra);
			draw.key.raster = Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor);
			const auto& target = globals::game::renderer->GetRuntimeData().renderTargets[kFramebuffer];
			if (target.RTV) {
				D3D11_RENDER_TARGET_VIEW_DESC desc{};
				Unwrap(target.RTV)->GetDesc(&desc);
				draw.key.colourFormats[0] = static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
			}
			if (!draw.key.blend || !draw.key.raster || !draw.key.colourFormats[0])
				return "no blend or rasterizer state object, or no back buffer";

			// SetupGeometry's constants: VS b2 c0-c5 (WorldViewProj, LightLoc, Ctrl) and PS b2 c0 (Alpha).
			auto& constants = draw.constants;
			if (!vs->constantBuffers[2].buffer || !Family::CopyRows(vs->constantBuffers[2].buffer, constants.worldViewProj, 6) ||
				!Family::CopyRows(ps->constantBuffers[2].buffer, &constants.alpha, 1))
				return "ConstantMirror has no copy of its constants yet";

			Family::ProgramState programState{};
			draw.program = BloodSplatterDraws::Program(draw.key, *state->currentShader, programState);
			if (!draw.program)
				return programState == Family::ProgramState::Building ? "its ORG program is still building" : "its ORG program cannot be built";

			Family::Owners owners;
			Family::Geometry geometry;
			if (const auto result = Family::LatchGeometry(a_shape, a_start, a_triangles, vs->vertexDesc, engine.topology, geometry, owners);
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

			// The imports are kept, a probe's too (BloodSplatterDraws::Keep).
			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			if (a_capture.probe)
				return nullptr;
			run.draws.push_back(std::move(draw));
			return nullptr;
		}

		/** @brief FinishAccumulatingScreenSplatter's RenderBatches (SE 99956 +0x1D, AE 106601 +0x21): the screen blood's batch. */
		struct SplatterBatch
		{
			static void thunk(void* a_accumulator, std::uint32_t a_first, std::uint32_t a_last, std::uint32_t a_flags, std::int32_t a_group)
			{
				auto& capture = GetCapture();
				if (capture.active || !ShellState::Running() || !FrameAnchors::IsRenderOwner() || !BloodSplatterDraws::Available()) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				Family::BeginBatch();
				if (!BloodSplatterDraws::Ready()) {
					// The client is not made yet, or is joining the graph: the engine draws this batch, and a later one probes.
					capture.clean = false;
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				const bool probe = !capture.clean;
				// Draws are withheld only when the anchor's run will draw them (a probe withholds nothing).
				if (!probe && !BloodSplatterDraws::CanRun()) {
					Refusals().Report("a batch whose anchor cannot run now", "the ring is full or the client left the built graph");
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				auto run = std::make_shared<Run>();
				capture.run = run;
				capture.probe = probe;
				capture.failed = false;
				capture.passes.Begin();
				capture.active = true;
				func(a_accumulator, a_first, a_last, a_flags, a_group);
				capture.active = false;
				capture.run.reset();
				if (!capture.passes.End()) {
					Refusals().Report(capture.passes.Describe("screen blood"));
					Refuse(capture, "a pass of the batch drew through another draw function than DrawTriShape", 0xFF);
				}
				if (probe) {
					capture.clean = !capture.failed;
					if (capture.clean)
						logger::info("[DCLF] blood splatter: every screen blood draw can be drawn by the graph; its batches are the graph's from the next");
					BloodSplatterDraws::Keep(std::move(run->owners));
					return;
				}
				if (capture.failed) {
					capture.clean = false;
					Refusals().Report("a batch the graph had taken, from a draw it could not latch on", "it probes again");
				}
				// A run that was not submitted lost its draws for this frame: the family probes again.
				if (!BloodSplatterDraws::Submit(std::move(run)))
					capture.clean = false;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The family's DrawTriShape taker: during a latched splatter batch, the draw is latched instead. */
		bool TakeDraw(const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& capture = GetCapture();
			if (!capture.active || !capture.run)
				return false;
			capture.passes.OnDraw();
			if (capture.failed && !capture.probe)
				return false;
			if (const char* reason = TryLatchDraw(capture, a_shape, a_start, a_triangles)) {
				const auto* state = globals::state;
				Refuse(capture, reason, state ? state->currentPixelDescriptor & 0xFF : 0xFF);
				return false;
			}
			return !capture.probe;
		}

		/** @brief After BSBloodSplatterShader::SetupGeometry: during a latched batch, ConstantMirror watches both stages' b2. */
		void OnSetupGeometry(RE::BSRenderPass* a_pass)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return;
			capture.passes.OnSetup(a_pass);
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			auto& mirror = ConstantMirror::Get();
			if (const auto* vs = rss.currentVertexShader; vs && vs->constantBuffers[2].buffer)
				mirror.Watch(reinterpret_cast<ID3D11Buffer*>(vs->constantBuffers[2].buffer));
			if (const auto* ps = rss.currentPixelShader; ps && ps->constantBuffers[2].buffer)
				mirror.Watch(reinterpret_cast<ID3D11Buffer*>(ps->constantBuffers[2].buffer));
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
		// The client is made on the main thread once the graph runs, before any blood is drawn.
		::RenderGraph::Engine::MainTick::Add("the screen blood's client", [] {
			if (FrameAnchors::IsRenderOwner() && BloodSplatterDraws::Available())
				BloodSplatterDraws::EnsureClient();
		});
		logger::info("[DCLF] blood splatter: hooks installed (the screen blood batch, BSBloodSplatterShader::SetupGeometry)");
	}
}
