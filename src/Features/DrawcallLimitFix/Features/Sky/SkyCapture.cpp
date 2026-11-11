// The sky family's engine side: the sky batch's hook, which lets the engine's batch run with its draws latched and hands them to the
// anchor's pass; DrawTriShape's detour, which latches a sky draw in place of drawing it; and BSSkyShader::SetupGeometry's, which has
// ConstantMirror watch the constant buffers the sky's draws read. Render thread.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/ConstantMirror.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuResources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/PublishedResources.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

#include <rhi_helpers.h>

#include <cstring>

namespace DCLF::Features::Sky
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		// The most draws one batch latches; past it a draw is the engine's (SkyDraws' latch has room for this many).
		constexpr std::size_t kMaxLatched = 128;

		/** @brief BSGraphics::TriShape's head (R1): the static vertex and index buffers and the VertexDesc. */
		struct EngineTriShape
		{
			ID3D11Buffer* vertexBuffer;
			ID3D11Buffer* indexBuffer;
			std::uint64_t vertexDesc;
		};

		/**
		 * @brief The batch being latched (render thread).
		 *
		 * A view's batches are the graph's only after a probe: a batch that the engine draws while every draw is checked as the graph
		 * would take it (its program built, its constants mirrored, its textures and buffers imported). A probe with every draw
		 * latchable makes the view's next batches the graph's; a draw a later batch cannot latch makes that batch's remaining draws the
		 * engine's and the view probes again. So nothing is withheld until its program exists.
		 */
		struct Capture
		{
			bool active = false;
			bool probe = false;   // the engine draws the batch; the draws are only checked
			bool failed = false;  // a draw of the batch could not be latched: the rest are the engine's
			std::array<bool, static_cast<std::size_t>(View::Count)> clean{};  // the view's last probe latched every draw
			View view = View::Main;
			std::shared_ptr<Run> run;
			std::array<std::uint32_t, 3> colourFormats{};  // the main pass's targets, or the face's (one)
			std::uint32_t depthFormat = 0;
			std::uint64_t fallbacks = 0;  // draws the engine drew during a latched batch
			bool cloudsCleared = false;   // the faces' occlusion was cleared for a weather without cloud layers
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

		std::uint32_t FormatOf(ID3D11DepthStencilView* a_dsv)
		{
			if (!a_dsv)
				return 0;
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			Unwrap(a_dsv)->GetDesc(&desc);
			return static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
		}

		bool IsTarget(std::int32_t a_target) { return a_target >= 0 && a_target < static_cast<std::int32_t>(RE::RENDER_TARGETS::kTOTAL); }
		bool IsDepth(std::int32_t a_depth) { return a_depth >= 0 && a_depth < static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); }

		/**
		 * @brief The engine depth target whose depth SRV is a_view, or -1. The main pass's t17 is Util::GetCurrentSceneDepthSRV(true)
		 * (VanillaBinding::BindSceneDepth), the Z-prepass copy unless Terrain Blending's blended depth replaces it.
		 */
		std::int32_t DepthOfView(ID3D11ShaderResourceView* a_view)
		{
			const auto& depths = globals::game::renderer->GetDepthStencilData().depthStencils;
			for (std::int32_t d = 0; d < static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); ++d)
				if (a_view && Unwrap(depths[d].depthSRV) == a_view)
					return d;
			return -1;
		}

		/**
		 * @brief The batch's view and targets from the shadow state (what its draws would bind): a water reflection face, or the main
		 * camera's GBuffer pass. False for anything else (another accumulator, a cubemap of another kind, the forward pass), which stays
		 * the engine's.
		 */
		bool CurrentShape(const void* a_accumulator, Shape& a_shape, std::uint32_t& a_face, Capture& a_capture)
		{
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			auto* renderer = globals::game::renderer;
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			auto& cloudShadows = globals::features::cloudShadows;
			// The sky's pixel stages read Cloud Shadows' occlusion (t25, t26), which the graph has only on its ORG path.
			if (cloudShadows.loaded && !ORGCloudShadows::Get().IsActive())
				return false;
			a_shape.cloudShadows = cloudShadows.loaded;
			// The headers' textures the sky's pixel stage may read (kHeaderRegisters), as CS binds them for the pass.
			for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h) {
				ID3D11ShaderResourceView* view = nullptr;
				globals::d3d::context->PSGetShaderResources(kHeaderRegisters[h], 1, &view);
				if (!view)
					continue;
				a_shape.headerIds[h] = PublishedResources::ExportedId(view);
				if (a_shape.headerIds[h].empty())
					a_shape.headerViews[h] = view;
				// The binding keeps the view alive; the shape keeps its identity.
				view->Release();
			}
			a_shape.depth = static_cast<std::int32_t>(rss.depthStencil);
			if (!IsDepth(a_shape.depth))
				return false;
			a_capture.colourFormats = {};
			a_capture.depthFormat = FormatOf(depths[a_shape.depth].views[0]);

			if (rss.cubeMapRenderTarget == static_cast<std::uint32_t>(RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS)) {
				a_shape.view = View::Face;
				a_face = rss.cubeMapRenderTargetView;
				const auto& cube = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];
				a_capture.colourFormats[0] = FormatOf(cube.cubeSideRTV[0]);
				return a_face < 6 && a_capture.colourFormats[0] && a_shape.depth == static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS);
			}
			if (rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return false;
			// The main camera's accumulator, inside CS's deferred pass (its GBuffer bound: Deferred::StartDeferred).
			static REL::Relocation<void**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
			if (a_accumulator != *mainAccumulator || !globals::deferred || !globals::deferred->deferredPass)
				return false;
			a_shape.view = View::Main;
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			for (std::size_t i = 0; i < a_shape.colours.size(); ++i) {
				a_shape.colours[i] = static_cast<std::int32_t>(rss.renderTargets[i]);
				if (!IsTarget(a_shape.colours[i]))
					return false;
				a_capture.colourFormats[i] = FormatOf(targets[a_shape.colours[i]].RTV);
				if (!a_capture.colourFormats[i])
					return false;
			}
			// t17: the scene depth CS binds, a published graph texture's export (Terrain Blending's ORG path) or an engine depth target.
			// When it is the depth target itself, kMAIN's depth (the same depth, VanillaBinding::BindSkyResources' choice).
			auto* sceneDepth = Util::GetCurrentSceneDepthSRV(true);
			a_shape.sceneDepthId = sceneDepth ? PublishedResources::ExportedId(sceneDepth) : std::string();
			a_shape.sceneDepth = a_shape.sceneDepthId.empty() ? DepthOfView(sceneDepth) : -1;
			if (a_shape.sceneDepth == a_shape.depth) {
				const auto main = static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
				a_shape.sceneDepth = main != a_shape.depth ? main : -1;
			}
			return a_capture.depthFormat != 0;
		}

		/** @brief Which of t0-t2 a technique (BSSkyShader's raw index, ShaderCache::SkyShaderTechniques) samples. */
		std::array<bool, 3> TexturesOf(std::uint32_t a_technique)
		{
			switch (a_technique) {
			case 1:  // SunGlare: TEX DITHER
				return { true, false, true };
			case 2:  // MoonAndStarsMask
			case 3:  // Stars
			case 4:  // Clouds
			case 6:  // CloudsFade
			case 7:  // Texture
				return { true, false, false };
			case 5:  // CloudsLerp
				return { true, true, false };
			case 8:  // Sky: DITHER
				return { false, false, true };
			default:  // SunOcclude
				return { false, false, false };
			}
		}

		/**
		 * @brief The draw the engine is about to make, latched: the shadow state SetDirtyStates would apply, the constants the shaders'
		 * SetupGeometry wrote (ConstantMirror), the textures and samplers, the geometry. False when it cannot be drawn by the graph: the
		 * engine draws it then.
		 */
		bool TryLatchDraw(Capture& a_capture, const EngineTriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& run = *a_capture.run;
			if (!a_shape || !a_shape->vertexBuffer || !a_shape->indexBuffer || run.draws.size() >= kMaxLatched)
				return false;
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* ps = rss.currentPixelShader;
			const auto* state = globals::state;
			if (!vs || !ps || !state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Sky)
				return false;

			// Cloud Shadows' override (ModifySky, from SetupGeometry): on a face its fourth target, which this draw writes on the graph.
			// Its flags are taken only once the draw is latched (LatchDraw); a draw the engine makes keeps them for SkyShaderHacks.
			auto& cloudShadows = globals::features::cloudShadows;
			const bool occlusion = a_capture.view == View::Face && cloudShadows.loaded && cloudShadows.overrideSky && cloudShadows.cloudShadowBlendState;

			const auto engine = Pipeline::EngineDrawState::Latch();
			auto* depthStencil = Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode);
			if (!depthStencil)
				return false;
			D3D11_DEPTH_STENCIL_DESC depthDesc{};
			depthStencil->GetDesc(&depthDesc);
			// The raster program has no stencil reference; the sky uses none.
			if (depthDesc.StencilEnable)
				return false;

			auto& mirror = ConstantMirror::Get();
			const auto vertexBytes = mirror.Contents(reinterpret_cast<const ID3D11Buffer*>(vs->constantBuffers[2].buffer));
			const auto pixelBytes = mirror.Contents(reinterpret_cast<const ID3D11Buffer*>(ps->constantBuffers[2].buffer));
			if (vertexBytes.size() < kVertexConstantBytes || pixelBytes.size() < 2 * sizeof(float))
				return false;

			Draw draw{};
			draw.key.view = a_capture.view;
			draw.key.occlusion = occlusion;
			draw.key.vertexDescriptor = state->currentVertexDescriptor;
			draw.key.pixelDescriptor = state->currentPixelDescriptor;
			draw.key.blend = occlusion ? static_cast<const void*>(cloudShadows.cloudShadowBlendState) :
			                             Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra);
			draw.key.depthStencil = depthStencil;
			draw.key.raster = Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor);
			draw.key.alphaToCoverage = engine.alphaToCoverage;
			if (a_capture.view == View::Main) {
				draw.key.colourFormats = a_capture.colourFormats;
				draw.key.depthFormat = a_capture.depthFormat;
			} else {
				draw.key.colourFormats = { a_capture.colourFormats[0], occlusion ? static_cast<std::uint32_t>(rhi::helpers::ToRHI(DXGI_FORMAT_R8_UNORM)) : 0u, 0u };
				// Cloud Shadows binds no depth target for its occlusion draws (SkyShaderHacks).
				draw.key.depthFormat = occlusion ? 0u : a_capture.depthFormat;
			}
			if (!draw.key.blend || !draw.key.raster)
				return false;
			draw.program = SkyDraws::Program(draw.key, state->currentShader);
			if (!draw.program)
				return false;

			// The geometry, read by device address while its leases are held (the run keeps them until the GPU is done).
			auto& buffers = GpuResources::Frame();
			auto vertices = buffers.Acquire(a_shape->vertexBuffer);
			auto indices = buffers.Acquire(a_shape->indexBuffer);
			if (!vertices || !indices || !vertices->buffer.address || !indices->buffer.address)
				return false;
			std::uint64_t mask = vs->vertexDesc;
			draw.vertexAddress = vertices->buffer.address;
			draw.indexAddress = indices->buffer.address;
			draw.vertexLayout = mask & a_shape->vertexDesc;
			draw.vertexStride = static_cast<std::uint32_t>(a_shape->vertexDesc & 0xF) * 4;
			draw.firstIndex = a_start;
			draw.indexCount = a_triangles * 3;

			// The textures the technique samples, and every slot's sampler (an unused slot takes the first: it is never sampled).
			auto& textures = GpuTextures::Get();
			const auto used = TexturesOf(state->currentPixelDescriptor & 0xFF);
			std::vector<std::shared_ptr<const void>> owners{ vertices->owner, indices->owner };
			for (std::uint32_t t = 0; t < 3; ++t) {
				auto binding = textures.NullBinding();
				if (used[t]) {
					auto* view = Unwrap(rss.PSTexture[t]);
					binding = view ? textures.ResolveBinding(view, 64 + t) : textures.NullBinding();
				}
				if (binding.index == GpuTextures::kInvalid)
					return false;
				draw.textures[t] = binding.index;
				owners.push_back(std::move(binding.owner));
				auto sampler = used[t] ? textures.SamplerBinding(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying()) :
				                         textures.SamplerBinding(0, 0);
				if (sampler.index == GpuTextures::kInvalid)
					return false;
				draw.samplers[t] = sampler.index;
				owners.push_back(std::move(sampler.owner));
			}

			// The constants: PerGeometry as the vertex stage reads it (c0 to c16.xy), the pixel stage's PParams, the alpha test reference
			// SetDirtyStates would write (PS b11: the reference while the test is on, else 0) and CS's extra descriptor for this draw
			// (State::UpdateSkyShaderPermutation ran in SetupGeometry).
			std::memcpy(&draw.constants, vertexBytes.data(), kVertexConstantBytes);
			std::memcpy(draw.constants.pParams, pixelBytes.data(), sizeof(draw.constants.pParams));
			draw.constants.alphaTestRef = rss.alphaTestEnabled ? rss.alphaTestRef : 0.0f;
			draw.constants.extraShaderDescriptor = state->permutationData.ExtraShaderDescriptor;

			// The viewport SetDirtyStates would set. Its depth range starts from the camera state's; the sky's depth bias modes (0, 2, 3)
			// take nothing off it (the bias viewport offsets, SE 0x141794530 / AE 0x1419db270, are 0 there).
			static REL::Relocation<const float*> cameraDepthRange{ REL::Offset(Engine::Address::kCameraDepthRange) };
			draw.viewport[0] = static_cast<std::uint32_t>(rss.viewPort.TopLeftX);
			draw.viewport[1] = static_cast<std::uint32_t>(rss.viewPort.TopLeftY);
			draw.viewport[2] = static_cast<std::uint32_t>(rss.viewPort.Width);
			draw.viewport[3] = static_cast<std::uint32_t>(rss.viewPort.Height);
			draw.depthRange[0] = cameraDepthRange.get()[0];
			draw.depthRange[1] = cameraDepthRange.get()[1];

			// A probe only checks: the engine draws it, with Cloud Shadows' overrides.
			if (a_capture.probe)
				return true;
			run.draws.push_back(std::move(draw));
			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			// Its D3D11 overrides (SkyShaderHacks) run only at a draw's state flush, which a latched draw never reaches.
			cloudShadows.overrideSky = false;
			cloudShadows.bindLayerSelfShadow = false;
			return true;
		}

		/**
		 * @brief A draw latched (true: the graph draws it) or the engine's. A draw that fails marks the batch: its later draws are the
		 * engine's too, so the engine's draws of the batch stay in order after the ones latched before (which the anchor draws after
		 * the batch: one batch drawn out of order, then the view probes again).
		 */
		bool LatchDraw(Capture& a_capture, const EngineTriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			if (a_capture.failed)
				return false;
			const bool latched = TryLatchDraw(a_capture, a_shape, a_start, a_triangles);
			if (!latched)
				a_capture.failed = true;
			return latched && !a_capture.probe;
		}

		/**
		 * @brief A weather without cloud layers draws no cloud on the faces, so nothing writes their occlusion: it is cleared once, as
		 * CloudShadows::ReflectionsPrepass clears its last layer.
		 */
		void NoteCloudLayers(Capture& a_capture)
		{
			const auto* sky = globals::game::sky;
			const bool layers = sky && sky->clouds && sky->clouds->numLayers > 0;
			if (layers) {
				a_capture.cloudsCleared = false;
			} else if (!a_capture.cloudsCleared) {
				SkyDraws::ClearCloudFaces();
				a_capture.cloudsCleared = true;
			}
		}

		/**
		 * @brief FinishAccumulating's RenderBatches of the sky techniques (SE 99938 +0x247, AE 106583 +0x237): for the main camera's
		 * GBuffer pass and a reflection face, the batch runs with its draws latched, then the anchor's pass draws them at this point.
		 */
		struct SkyBatch
		{
			static void thunk(void* a_accumulator, std::uint32_t a_first, std::uint32_t a_last, std::uint32_t a_flags, std::int32_t a_group)
			{
				auto& capture = GetCapture();
				if (capture.active || !ShellState::Running() || !FrameAnchors::OnMainThread() || !SkyDraws::Available()) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				Shape shape;
				std::uint32_t face = 0;
				if (!CurrentShape(a_accumulator, shape, face, capture)) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				if (shape.view == View::Face && shape.cloudShadows)
					NoteCloudLayers(capture);
				auto run = std::make_shared<Run>();
				run->view = shape.view;
				run->face = face;
				auto& clean = capture.clean[static_cast<std::size_t>(shape.view)];
				if (!SkyDraws::Ready(shape)) {
					// The client is new or was made again: an empty run builds and prepares it, the engine draws this batch, and the
					// view probes again (the new targets may need other programs).
					clean = false;
					if (shape.view == View::Face)
						SkyDraws::UseCloudFaces(false);
					SkyDraws::Submit(std::move(run));
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				const bool probe = !clean;
				// Cloud Shadows' copy reads the faces' cloud occlusion only while the graph draws the faces' clouds.
				if (shape.view == View::Face && shape.cloudShadows)
					SkyDraws::UseCloudFaces(!probe);
				capture.view = shape.view;
				capture.run = run;
				capture.probe = probe;
				capture.failed = false;
				capture.active = true;
				func(a_accumulator, a_first, a_last, a_flags, a_group);
				capture.active = false;
				capture.run.reset();
				if (probe) {
					clean = !capture.failed;
					if (clean)
						logger::info("[DCLF] sky: every {} sky draw can be drawn by the graph; its batches are the graph's from the next", shape.view == View::Main ? "main" : "face");
					return;
				}
				if (capture.failed) {
					clean = false;
					logger::warn("[DCLF] sky: a {} sky draw could not be drawn by the graph; the engine draws the batches while the view probes again",
						shape.view == View::Main ? "main" : "face");
				}
				SkyDraws::Submit(std::move(run));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSGraphics::Renderer::DrawTriShape (SE 75477, AE 77263): during a latched sky batch, the draw is latched instead. */
		struct DrawTriShape
		{
			static void thunk(void* a_renderer, EngineTriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
			{
				auto& capture = GetCapture();
				if (capture.active && capture.run) {
					if (LatchDraw(capture, a_shape, a_start, a_triangles))
						return;
					++capture.fallbacks;
				}
				func(a_renderer, a_shape, a_start, a_triangles);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSSkyShader::SetupGeometry (vtable slot 6, chained after Cloud Shadows' and the frame annotations'): during a latched
		 * batch, ConstantMirror watches the vertex and pixel stages' PerGeometry buffers, which the draw reads back. A buffer is
		 * mirrored from its next write on, so a batch's first sight of one draws on the engine.
		 */
		struct SetupGeometry
		{
			static void thunk(RE::BSShader* a_this, RE::BSRenderPass* a_pass, std::uint32_t a_flags)
			{
				func(a_this, a_pass, a_flags);
				if (!GetCapture().active)
					return;
				const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
				auto& mirror = ConstantMirror::Get();
				if (const auto* vs = rss.currentVertexShader; vs && vs->constantBuffers[2].buffer)
					mirror.Watch(reinterpret_cast<ID3D11Buffer*>(vs->constantBuffers[2].buffer));
				if (const auto* ps = rss.currentPixelShader; ps && ps->constantBuffers[2].buffer)
					mirror.Watch(reinterpret_cast<ID3D11Buffer*>(ps->constantBuffers[2].buffer));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void PreflightHooks()
	{
		if (!Enabled())
			return;
		HookSites::RequireKnown("the sky batch", Address::kSkyBatch.Known() && Address::kDrawTriShape.Known());
		if (!Address::kSkyBatch.Known() || !Address::kDrawTriShape.Known())
			return;
		HookSites::RequireCall("the sky batch (FinishAccumulating's RenderBatches of the sky techniques)", REL::Offset(Address::kSkyBatch).address());
	}

	void InstallHooks()
	{
		if (!Address::kSkyBatch.Known() || !Address::kDrawTriShape.Known()) {
			logger::warn("[DCLF] sky: this runtime has no address for the sky batch; the sky stays the engine's");
			return;
		}
		stl::write_thunk_call<SkyBatch>(REL::Offset(Address::kSkyBatch).address());
		stl::detour_thunk<DrawTriShape>(REL::Offset(Address::kDrawTriShape).address());
		HookSites::NoteDetour("BSGraphics::Renderer::DrawTriShape (the sky's draws)", DrawTriShape::func.address());
		stl::write_vfunc<0x6, SetupGeometry>(RE::VTABLE_BSSkyShader[0]);
		logger::info("[DCLF] sky: hooks installed (the sky batch, DrawTriShape, BSSkyShader::SetupGeometry)");
	}
}
