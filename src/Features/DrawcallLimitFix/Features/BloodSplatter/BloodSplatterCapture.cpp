// The blood splatter family's engine side: the screen splatter batch's hook, which lets the engine's batch run with its draws latched
// and hands them to the anchor's pass; DrawTriShape's detour, which latches a splatter draw in place of drawing it;
// BSBloodSplatterShader::SetupGeometry's, which has ConstantMirror watch its constant buffers; and the frame-job wait's, where the client
// is made once. Render thread.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/Capture/ConstantMirror.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuResources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/BloodSplatter/BloodSplatter.h"
#include "Features/DrawcallLimitFix/Features/Water/TextureFailure.h"

#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/PublishedResources.h"
#include "State.h"

#include <rhi_helpers.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>

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

		/** @brief BSGraphics::TriShape's head (R1). */
		struct EngineTriShape
		{
			ID3D11Buffer* vertexBuffer;
			ID3D11Buffer* indexBuffer;
			std::uint64_t vertexDesc;
		};

		/** @brief The batch being latched, as the water's and the sky's (render thread). */
		struct Capture
		{
			bool active = false;
			bool probe = false;
			bool failed = false;
			bool clean = false;
			std::shared_ptr<Run> run;
			std::uint32_t passes = 0, drawn = 0;
			std::set<std::pair<const char*, std::uint32_t>> logged;  // (reason, technique) logged once each
			std::set<std::string> failures;                          // texture import failures (TextureFailure.h), logged once each
		};

		Capture& GetCapture()
		{
			static Capture capture;
			return capture;
		}

		/** @brief Logged once per reason and technique: the batch fails, its later draws are the engine's. */
		void Refuse(Capture& a_capture, const char* a_reason, std::uint32_t a_technique)
		{
			a_capture.failed = true;
			if (a_capture.logged.insert({ a_reason, a_technique }).second)
				logger::warn("[DCLF] blood splatter: a draw of technique {} stays the engine's: {}{}", a_technique, a_reason, a_capture.probe ? " (probe)" : "");
		}

		/** @brief A splatter draw latched; the reason when it cannot be (the engine draws it then). */
		const char* TryLatchDraw(Capture& a_capture, const EngineTriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& run = *a_capture.run;
			if (!a_shape || !a_shape->vertexBuffer || !a_shape->indexBuffer)
				return "no TriShape buffers";
			if (run.draws.size() >= kMaxLatched)
				return "more draws than the batch latches";
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* ps = rss.currentPixelShader;
			const auto* state = globals::state;
			if (!vs || !ps || !state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::BloodSplatter)
				return "the bound shader is not BSBloodSplatterShader";
			const auto engine = Pipeline::EngineDrawState::Latch();
			if (engine.topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
				return "its topology is not a triangle list";
			// SetupTechnique turns the depth test off (depth mode 0) and the screen blood uses no stencil: the program has no depth target.
			if (engine.depthMode != 0 || engine.stencilMode != 0)
				return "it tests depth or stencil";
			if (static_cast<std::int32_t>(rss.renderTargets[0]) != kFramebuffer ||
				static_cast<std::int32_t>(rss.renderTargets[1]) >= 0 || rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return "its targets are not the back buffer alone";

			Draw draw{};
			draw.key.descriptor = state->currentPixelDescriptor;
			draw.key.blend = Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra);
			draw.key.raster = Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor);
			const auto& target = globals::game::renderer->GetRuntimeData().renderTargets[kFramebuffer];
			if (target.RTV) {
				D3D11_RENDER_TARGET_VIEW_DESC desc{};
				Unwrap(target.RTV)->GetDesc(&desc);
				draw.key.colourFormat = static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
			}
			if (!draw.key.blend || !draw.key.raster || !draw.key.colourFormat)
				return "no blend or rasterizer state object, or no back buffer";

			// SetupGeometry's constants: VS b2 c0-c5 (WorldViewProj, LightLoc, Ctrl) and PS b2 c0 (Alpha).
			auto& mirror = ConstantMirror::Get();
			const auto vertexBytes = vs->constantBuffers[2].buffer ? mirror.Contents(reinterpret_cast<const ID3D11Buffer*>(vs->constantBuffers[2].buffer)) :
			                                                         std::span<const std::byte>{};
			const auto pixelBytes = ps->constantBuffers[2].buffer ? mirror.Contents(reinterpret_cast<const ID3D11Buffer*>(ps->constantBuffers[2].buffer)) :
			                                                        std::span<const std::byte>{};
			if (vertexBytes.size() < 5 * 16 || (ps->constantBuffers[2].buffer && pixelBytes.empty()))
				return "ConstantMirror has no copy of its constants yet";
			auto& constants = draw.constants;
			std::memcpy(constants.worldViewProj, vertexBytes.data(), (std::min)(vertexBytes.size(), std::size_t{ 6 * 16 }));
			std::memcpy(constants.alpha, pixelBytes.data(), (std::min)(pixelBytes.size(), sizeof(constants.alpha)));

			draw.program = BloodSplatterDraws::Program(draw.key, state->currentShader);
			if (!draw.program)
				return "its ORG program could not be built";

			auto& buffers = GpuResources::Frame();
			auto vertices = buffers.Acquire(a_shape->vertexBuffer);
			auto indices = buffers.Acquire(a_shape->indexBuffer);
			if (!vertices || !indices || !vertices->buffer.address || !indices->buffer.address)
				return "its vertex or index buffer could not be imported";
			const std::uint64_t layout = vs->vertexDesc & a_shape->vertexDesc;
			constants.vertexAddress[0] = static_cast<std::uint32_t>(vertices->buffer.address);
			constants.vertexAddress[1] = static_cast<std::uint32_t>(vertices->buffer.address >> 32);
			constants.indexAddress[0] = static_cast<std::uint32_t>(indices->buffer.address);
			constants.indexAddress[1] = static_cast<std::uint32_t>(indices->buffer.address >> 32);
			constants.vertexLayout[0] = static_cast<std::uint32_t>(layout);
			constants.vertexLayout[1] = static_cast<std::uint32_t>(layout >> 32);
			constants.vertexStride = static_cast<std::uint32_t>(a_shape->vertexDesc & 0xF) * 4;
			constants.firstIndex = a_start;
			draw.indexCount = a_triangles * 3;

			// t0-t3 as the shadow state holds them (the splatter's colour and alpha, the flare's colour and HDR texture).
			auto& textures = GpuTextures::Get();
			std::vector<std::shared_ptr<const void>> owners{ vertices->owner, indices->owner };
			for (std::uint32_t t = 0; t < 4; ++t) {
				auto* view = Unwrap(rss.PSTexture[t]);
				if (view && !PublishedResources::ExportedId(view).empty())
					return "a texture slot holds a published graph texture";
				const auto rejections = Water::RejectionCount();
				auto binding = view ? textures.ResolveBinding(view, 64 + t) : textures.NullBinding();
				if (binding.index == GpuTextures::kInvalid) {
					if (view)
						if (auto failure = Water::DescribeTextureFailure(view, t, rejections); a_capture.failures.insert(failure).second)
							logger::warn("[DCLF] blood splatter: a draw's texture could not be imported: {}", failure);
					return "a texture it samples could not be imported";
				}
				constants.textures[t] = binding.index;
				owners.push_back(std::move(binding.owner));
				auto sampler = textures.SamplerBinding(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying());
				if (sampler.index == GpuTextures::kInvalid)
					return "its sampler modes have no graph sampler";
				constants.samplers[t] = sampler.index;
				owners.push_back(std::move(sampler.owner));
			}
			draw.viewport[0] = static_cast<std::uint32_t>(rss.viewPort.TopLeftX);
			draw.viewport[1] = static_cast<std::uint32_t>(rss.viewPort.TopLeftY);
			draw.viewport[2] = static_cast<std::uint32_t>(rss.viewPort.Width);
			draw.viewport[3] = static_cast<std::uint32_t>(rss.viewPort.Height);

			if (a_capture.probe)
				return nullptr;
			run.draws.push_back(std::move(draw));
			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			return nullptr;
		}

		/** @brief FinishAccumulatingScreenSplatter's RenderBatches (SE 99956 +0x1D, AE 106601 +0x21): the screen blood's batch. */
		struct SplatterBatch
		{
			static void thunk(void* a_accumulator, std::uint32_t a_first, std::uint32_t a_last, std::uint32_t a_flags, std::int32_t a_group)
			{
				auto& capture = GetCapture();
				if (capture.active || !ShellState::Running() || !FrameAnchors::OnRenderThread() || !BloodSplatterDraws::Available()) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				if (!BloodSplatterDraws::Ready()) {
					// The client has not run yet: an empty run prepares it, the engine draws this batch, and the next one probes.
					capture.clean = false;
					BloodSplatterDraws::Submit(std::make_shared<Run>());
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				const bool probe = !capture.clean;
				auto run = std::make_shared<Run>();
				capture.run = run;
				capture.probe = probe;
				capture.failed = false;
				capture.passes = 0;
				capture.drawn = 0;
				capture.active = true;
				func(a_accumulator, a_first, a_last, a_flags, a_group);
				capture.active = false;
				capture.run.reset();
				if (capture.drawn < capture.passes)
					Refuse(capture, "a pass of the batch drew through another draw function than DrawTriShape", 0xFF);
				if (probe) {
					capture.clean = !capture.failed;
					if (capture.clean)
						logger::info("[DCLF] blood splatter: every screen blood draw can be drawn by the graph; its batches are the graph's from the next");
					return;
				}
				if (capture.failed) {
					capture.clean = false;
					logger::warn("[DCLF] blood splatter: a draw could not be drawn by the graph; the engine draws the batches while it probes again");
				}
				BloodSplatterDraws::Submit(std::move(run));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief DrawTriShape (SE 75477, AE 77263): during a latched splatter batch, the draw is latched instead. */
		struct DrawTriShape
		{
			static void thunk(void* a_renderer, EngineTriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
			{
				auto& capture = GetCapture();
				if (capture.active && capture.run) {
					++capture.drawn;
					if (!capture.failed || capture.probe) {
						if (const char* reason = TryLatchDraw(capture, a_shape, a_start, a_triangles)) {
							const auto* state = globals::state;
							Refuse(capture, reason, state ? state->currentPixelDescriptor & 0xFF : 0xFF);
						} else if (!capture.probe) {
							return;
						}
					}
				}
				func(a_renderer, a_shape, a_start, a_triangles);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSBloodSplatterShader::SetupGeometry (vtable slot 6): during a latched batch, ConstantMirror watches both stages' b2. */
		struct SetupGeometry
		{
			static void thunk(RE::BSShader* a_this, RE::BSRenderPass* a_pass, std::uint32_t a_flags)
			{
				func(a_this, a_pass, a_flags);
				auto& capture = GetCapture();
				if (!capture.active)
					return;
				++capture.passes;
				const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
				auto& mirror = ConstantMirror::Get();
				if (const auto* vs = rss.currentVertexShader; vs && vs->constantBuffers[2].buffer)
					mirror.Watch(reinterpret_cast<ID3D11Buffer*>(vs->constantBuffers[2].buffer));
				if (const auto* ps = rss.currentPixelShader; ps && ps->constantBuffers[2].buffer)
					mirror.Watch(reinterpret_cast<ID3D11Buffer*>(ps->constantBuffers[2].buffer));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief JobListManager's frame-job wait (SE 34554, AE 35409), called only by Main::Update, on the main thread: the client is made
		 * there once the graph runs, before any blood is drawn.
		 */
		struct WaitForFrameJobs
		{
			static void thunk()
			{
				func();
				if (FrameAnchors::OnRenderThread() && BloodSplatterDraws::Available())
					BloodSplatterDraws::EnsureClient();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void PreflightHooks()
	{
		if (!Enabled())
			return;
		const bool known = Address::kSplatterBatch.Known() && Address::kDrawTriShape.Known();
		HookSites::RequireKnown("the screen blood batch", known);
		if (known)
			HookSites::RequireCall("the screen blood batch (FinishAccumulatingScreenSplatter's RenderBatches)", REL::Offset(Address::kSplatterBatch).address());
	}

	void InstallHooks()
	{
		if (!Address::kSplatterBatch.Known() || !Address::kDrawTriShape.Known()) {
			logger::warn("[DCLF] blood splatter: this runtime has no address for the screen blood batch; it stays the engine's");
			return;
		}
		stl::write_thunk_call<SplatterBatch>(REL::Offset(Address::kSplatterBatch).address());
		stl::detour_thunk<DrawTriShape>(REL::Offset(Address::kDrawTriShape).address());
		HookSites::NoteDetour("BSGraphics::Renderer::DrawTriShape (the screen blood's draws)", DrawTriShape::func.address());
		stl::write_vfunc<0x6, SetupGeometry>(RE::VTABLE_BSBloodSplatterShader[0]);
		stl::detour_thunk<WaitForFrameJobs>(REL::RelocationID(34554, 35409));
		logger::info("[DCLF] blood splatter: hooks installed (the screen blood batch, DrawTriShape, BSBloodSplatterShader::SetupGeometry)");
	}
}
