// The sky family's engine side: the sky batch's hook, which lets the engine's batch run with its draws latched; the cloud group's
// hook, which latches the cloud layers drawn right after it and hands the draws to the anchor's pass; DrawTriShape's detour, which
// latches a sky draw in place of drawing it; and BSSkyShader::SetupGeometry's, which has ConstantMirror watch the constant buffers the
// sky's draws read. Render thread.

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
#include "RenderGraph/Switches.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/PublishedResources.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

#include <rhi_helpers.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <span>

namespace DCLF::Features::Sky
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		// The most draws one batch latches; past it a draw is the engine's (SkyDraws' latch has room for this many).
		constexpr std::size_t kMaxLatched = 128;

		/** @brief Why a sky draw could not be latched: each logged once per technique and view (Refuse). */
		enum class Refusal : std::uint8_t
		{
			None,
			Geometry,       // no TriShape or no buffers
			TooMany,        // past kMaxLatched draws in one batch
			Shader,         // the bound shader is not BSSkyShader, or no shader objects
			DepthStencil,   // no depth-stencil state, or one with stencil (the raster program has no stencil reference)
			Constants,      // ConstantMirror has no copy of the vertex stage's PerGeometry yet
			States,         // no blend or rasterizer state object for the draw
			Program,        // the ORG program could not be built
			Buffers,        // the vertex or index buffer could not be imported
			Texture,        // a texture the technique samples could not be imported
			Sampler,        // the sampler for the slot's modes is not in the graph's table
			OtherDrawPath,  // a sky pass of the batch drew through another draw function than DrawTriShape
		};

		const char* ReasonOf(Refusal a_refusal)
		{
			switch (a_refusal) {
			case Refusal::Geometry:
				return "no TriShape buffers";
			case Refusal::TooMany:
				return "more draws than the batch latches";
			case Refusal::Shader:
				return "the bound shader is not BSSkyShader";
			case Refusal::DepthStencil:
				return "its depth-stencil state is missing or uses stencil";
			case Refusal::Constants:
				return "ConstantMirror has no copy of its vertex constants yet";
			case Refusal::States:
				return "no blend or rasterizer state object";
			case Refusal::Program:
				return "its ORG program could not be built";
			case Refusal::Buffers:
				return "its vertex or index buffer could not be imported";
			case Refusal::Texture:
				return "a texture it samples could not be imported";
			case Refusal::Sampler:
				return "its sampler modes have no graph sampler";
			case Refusal::OtherDrawPath:
				return "a pass of the batch drew through another draw function than DrawTriShape";
			default:
				return "none";
			}
		}

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
			std::uint32_t passes = 0;     // the batch's SetupGeometry calls
			std::uint32_t drawn = 0;      // the batch's DrawTriShape calls
			std::uint32_t lastTechnique = 0;  // the last pass's raw technique (State::currentPixelDescriptor & 0xFF)
			const RE::BSRenderPass* lastPass = nullptr;  // the last pass SetupGeometry set up (TraceCloudDraw)
			std::array<std::uint32_t, 2> traced{};       // cloud draws traced (CS_ORG_SKY_TRACE): probed, latched
			std::uint32_t checked = 0;                   // the batch's draws checked so far (the trace's draw number)
			// The last pass's geometry type (BSGeometry +0x150, R1's dispatcher) while its draw has not reached DrawTriShape; 0xFF none.
			std::uint32_t pendingType = 0xFF;
			std::uint32_t otherTypes = 0;  // the geometry types of the batch's passes that drew elsewhere, a bit each
			std::set<std::uint32_t> logged;   // (refusal, technique, view) logged once each
			Shape shape;                      // the batch's view and targets (CurrentShape)
			// Geometry group 13 when the capture spans the cloud layers too: the batch's draws are handed over after it (CloudGroup).
			const RE::BSBatchRenderer::GeometryGroup* cloudGroup = nullptr;
			bool deferred = false;      // the sky batch ran; the capture is finished after the cloud group
			bool inClouds = false;      // the cloud group is being drawn
			bool skyShort = false;      // the sky batch had a pass that did not draw through DrawTriShape
			bool skyFailed = false;     // a refusal in the sky batch
			bool cloudsFailed = false;  // a refusal in the cloud group
			// A probe whose only refusals were cloud draws: the view's cloud layers stay the engine's for the session, so a cloud
			// technique the graph cannot draw never keeps the rest of the view's sky off the graph.
			std::array<bool, static_cast<std::size_t>(View::Count)> cloudsRefused{};
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
			// The cloud layers are geometry group 13, which the capture spans only through the cloud group's hook.
			a_shape.clouds = Address::kCloudGroup.Known();
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
				// The faces' cloud draws write Cloud Shadows' occlusion into its graph faces: without them the clouds stay the engine's.
				a_shape.clouds = a_shape.clouds && TakesClouds(View::Face) && (!a_shape.cloudShadows || ORGCloudShadows::Get().GraphFaces());
				return a_face < 6 && a_capture.colourFormats[0] && a_shape.depth == static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS);
			}
			if (rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return false;
			// The main camera's accumulator, inside CS's deferred pass (its GBuffer bound: Deferred::StartDeferred).
			static REL::Relocation<void**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
			if (a_accumulator != *mainAccumulator || !globals::deferred || !globals::deferred->deferredPass)
				return false;
			a_shape.view = View::Main;
			a_shape.clouds = a_shape.clouds && TakesClouds(View::Main);
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
			// Neither (Terrain Blending's D3D11 depth): the view itself, imported. Without it the sun's depth test read zero and hid it.
			if (sceneDepth && a_shape.sceneDepthId.empty() && a_shape.sceneDepth < 0)
				a_shape.sceneDepthView = sceneDepth;
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

		/** @brief A D3D11 blend description's first two targets, for the trace. */
		std::string BlendText(const void* a_blend)
		{
			if (!a_blend)
				return "none";
			D3D11_BLEND_DESC desc{};
			static_cast<ID3D11BlendState*>(const_cast<void*>(a_blend))->GetDesc(&desc);
			std::string out = fmt::format("a2c {} independent {}", desc.AlphaToCoverageEnable, desc.IndependentBlendEnable);
			for (int i = 0; i < 2; ++i) {
				const auto& t = desc.RenderTarget[i];
				out += fmt::format(" | rt{} on {} {}/{} op {} alpha {}/{} mask {:#x}", i, t.BlendEnable, static_cast<int>(t.SrcBlend), static_cast<int>(t.DestBlend),
					static_cast<int>(t.BlendOp), static_cast<int>(t.SrcBlendAlpha), static_cast<int>(t.DestBlendAlpha), t.RenderTargetWriteMask);
			}
			return out;
		}

		/**
		 * @brief CS_ORG_SKY_TRACE=1: the first 16 cloud draws (techniques 4-6) probes check and the first 64 batches latch, with what the engine's
		 * property holds (kBlendColor +0x88, fBlendValue +0xb8, usCloudLayer +0xbc, bFadeSecondTexture +0xbe, the sky object +0xc0) beside
		 * what the latch took, so a wrong constant, texture, buffer or state shows in one run.
		 */
		void TraceCloudDraw(Capture& a_capture, const Draw& a_draw, const EngineTriShape* a_shape, std::span<const std::byte> a_vertexBytes,
			std::span<const std::byte> a_pixelBytes)
		{
			static const bool trace = Switches::On("CS_ORG_SKY_TRACE");
			const std::uint32_t technique = a_draw.key.pixelDescriptor & 0xFF;
			auto& traced = a_capture.traced[a_capture.probe ? 0 : 1];
			if (!trace || technique < 4 || technique > 6 || traced >= (a_capture.probe ? 16u : 64u))
				return;
			++traced;
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* pass = a_capture.lastPass;
			const auto* property = pass ? pass->shaderProperty : nullptr;
			const float* blendColor = property ? &Engine::At<float>(property, 0x88) : nullptr;
			const auto& c = a_draw.constants;
			D3D11_BUFFER_DESC vertexDesc{};
			a_shape->vertexBuffer->GetDesc(&vertexDesc);
			D3D11_SHADER_RESOURCE_VIEW_DESC t0{};
			if (auto* view = Unwrap(rss.PSTexture[0]))
				view->GetDesc(&t0);
			logger::info(
				"[DCLF] sky trace: {} {} draw {} technique {} geometry {:p}: property layer {} sky object {} blend colour ({:.3f} {:.3f} {:.3f} {:.3f}) "
				"blend value {:.3f} fade second {} | latched blend colour0 ({:.3f} {:.3f} {:.3f} {:.3f}) colour1 ({:.3f} {:.3f} {:.3f}) uv offset ({:.3f} {:.3f}) "
				"vparams {:.3f} pparams ({:.3f} {:.3f}) alpha test {} ref {:.3f} | constant bytes vs {} ps {} | layout {:#x} stride {} first {} count {} vb usage {} "
				"bytes {} | t0 {} format {} mips {} sampler {}/{} | write mode {} blend mode {} occlusion {} | {}",
				a_capture.view == View::Main ? "main" : "face", a_capture.probe ? "probe" : "latched", a_capture.checked, technique,
				static_cast<const void*>(pass ? pass->geometry : nullptr), property ? Engine::At<std::uint16_t>(property, 0xbc) : 0xFFFF,
				property ? Engine::At<std::uint32_t>(property, 0xc0) : 0xFFFF, blendColor ? blendColor[0] : -1.0f, blendColor ? blendColor[1] : -1.0f,
				blendColor ? blendColor[2] : -1.0f, blendColor ? blendColor[3] : -1.0f, property ? Engine::At<float>(property, 0xb8) : -1.0f,
				property ? Engine::At<std::uint8_t>(property, 0xbe) : 0xFF, c.blendColor[0], c.blendColor[1], c.blendColor[2], c.blendColor[3], c.blendColor[4],
				c.blendColor[5], c.blendColor[6], c.texCoordOff[0], c.texCoordOff[1], c.vParams, c.pParams[0], c.pParams[1], rss.alphaTestEnabled, c.alphaTestRef,
				a_vertexBytes.size(), a_pixelBytes.size(), a_draw.vertexLayout, a_draw.vertexStride, a_draw.firstIndex, a_draw.indexCount,
				static_cast<int>(vertexDesc.Usage), vertexDesc.ByteWidth, a_draw.textures[0], static_cast<int>(t0.Format), t0.Texture2D.MipLevels,
				rss.PSTextureAddressMode[0].underlying(), rss.PSTextureFilterMode[0].underlying(), static_cast<std::uint32_t>(rss.alphaBlendWriteMode),
				static_cast<std::uint32_t>(rss.alphaBlendMode), a_draw.key.occlusion, BlendText(a_draw.key.blend));
		}

		/**
		 * @brief The draw the engine is about to make, latched: the shadow state SetDirtyStates would apply, the constants the shaders'
		 * SetupGeometry wrote (ConstantMirror), the textures and samplers, the geometry. False when it cannot be drawn by the graph: the
		 * engine draws it then.
		 */
		Refusal TryLatchDraw(Capture& a_capture, const EngineTriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& run = *a_capture.run;
			++a_capture.checked;
			if (!a_shape || !a_shape->vertexBuffer || !a_shape->indexBuffer)
				return Refusal::Geometry;
			if (run.draws.size() >= kMaxLatched)
				return Refusal::TooMany;
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* ps = rss.currentPixelShader;
			const auto* state = globals::state;
			if (!vs || !ps || !state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Sky)
				return Refusal::Shader;

			// Cloud Shadows' override (ModifySky, from SetupGeometry): on a face its fourth target, which this draw writes on the graph.
			// Its flags are taken only once the draw is latched (LatchDraw); a draw the engine makes keeps them for SkyShaderHacks.
			auto& cloudShadows = globals::features::cloudShadows;
			const bool occlusion = a_capture.view == View::Face && cloudShadows.loaded && cloudShadows.overrideSky && cloudShadows.cloudShadowBlendState;

			const auto engine = Pipeline::EngineDrawState::Latch();
			auto* depthStencil = Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode);
			if (!depthStencil)
				return Refusal::DepthStencil;
			D3D11_DEPTH_STENCIL_DESC depthDesc{};
			depthStencil->GetDesc(&depthDesc);
			// The raster program has no stencil reference; the sky uses none.
			if (depthDesc.StencilEnable)
				return Refusal::DepthStencil;

			// The PerGeometry groups as SetupGeometry wrote them. A technique's group holds only the rows its vanilla shader declares
			// (one may end before TexCoordOff, a pixel stage may have none): the rows past it are zero, which the technique's CS shader
			// does not read either. The matrices and EyePosition/VParams (c0-c12) are every technique's.
			auto& mirror = ConstantMirror::Get();
			const auto* vertexBuffer = reinterpret_cast<const ID3D11Buffer*>(vs->constantBuffers[2].buffer);
			const auto* pixelBuffer = reinterpret_cast<const ID3D11Buffer*>(ps->constantBuffers[2].buffer);
			const auto vertexBytes = vertexBuffer ? mirror.Contents(vertexBuffer) : std::span<const std::byte>{};
			const auto pixelBytes = pixelBuffer ? mirror.Contents(pixelBuffer) : std::span<const std::byte>{};
			if (vertexBytes.size() < 13 * 16)
				return Refusal::Constants;

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
				return Refusal::States;
			draw.program = SkyDraws::Program(draw.key, state->currentShader);
			if (!draw.program)
				return Refusal::Program;

			// The geometry, read by device address while its leases are held (the run keeps them until the GPU is done).
			auto& buffers = GpuResources::Frame();
			auto vertices = buffers.Acquire(a_shape->vertexBuffer);
			auto indices = buffers.Acquire(a_shape->indexBuffer);
			if (!vertices || !indices || !vertices->buffer.address || !indices->buffer.address)
				return Refusal::Buffers;
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
					return Refusal::Texture;
				draw.textures[t] = binding.index;
				owners.push_back(std::move(binding.owner));
				auto sampler = used[t] ? textures.SamplerBinding(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying()) :
				                         textures.SamplerBinding(0, 0);
				if (sampler.index == GpuTextures::kInvalid)
					return Refusal::Sampler;
				draw.samplers[t] = sampler.index;
				owners.push_back(std::move(sampler.owner));
			}

			// The constants: PerGeometry as the vertex stage reads it (c0 to c16.xy), the pixel stage's PParams, the alpha test reference
			// SetDirtyStates would write (PS b11: the reference while the test is on, else 0) and CS's extra descriptor for this draw
			// (State::UpdateSkyShaderPermutation ran in SetupGeometry).
			std::memcpy(&draw.constants, vertexBytes.data(), (std::min)(vertexBytes.size(), kVertexConstantBytes));
			std::memcpy(draw.constants.pParams, pixelBytes.data(), (std::min)(pixelBytes.size(), sizeof(draw.constants.pParams)));
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

			TraceCloudDraw(a_capture, draw, a_shape, vertexBytes, pixelBytes);
			// The imports are kept, a probe's too (SkyDraws::Keep): a probe that dropped them freed each texture's descriptor as soon as
			// it was made, so the next texture's import took the same index, and the next batch imported every texture again.
			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			// A probe only checks: the engine draws it, with Cloud Shadows' overrides.
			if (a_capture.probe)
				return Refusal::None;
			run.draws.push_back(std::move(draw));
			// Its D3D11 overrides (SkyShaderHacks) run only at a draw's state flush, which a latched draw never reaches.
			cloudShadows.overrideSky = false;
			cloudShadows.bindLayerSelfShadow = false;
			return Refusal::None;
		}

		/**
		 * @brief A draw latched (true: the graph draws it) or the engine's. A draw that fails marks the batch: its later draws are the
		 * engine's too, so the engine's draws of the batch stay in order after the ones latched before (which the anchor draws after
		 * the batch: one batch drawn out of order, then the view probes again).
		 */
		/** @brief A refusal noted: the batch fails, and the reason is logged once per technique and view. */
		void Refuse(Capture& a_capture, Refusal a_refusal, std::uint32_t a_technique)
		{
			a_capture.failed = true;
			(a_capture.inClouds ? a_capture.cloudsFailed : a_capture.skyFailed) = true;
			const std::uint32_t key = (static_cast<std::uint32_t>(a_refusal) << 16) | ((a_technique & 0xFF) << 8) | static_cast<std::uint32_t>(a_capture.view);
			if (a_capture.logged.insert(key).second)
				logger::warn("[DCLF] sky: a {} sky draw of technique {} stays the engine's: {}{}", a_capture.view == View::Main ? "main" : "face", a_technique,
					ReasonOf(a_refusal), a_capture.probe ? " (probe)" : "");
		}

		bool LatchDraw(Capture& a_capture, const EngineTriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			// A probe checks every draw, so one batch builds every program the view needs; a batch the graph draws stops at the first
			// draw it cannot take.
			if (a_capture.failed && !a_capture.probe)
				return false;
			const auto refusal = TryLatchDraw(a_capture, a_shape, a_start, a_triangles);
			if (refusal != Refusal::None) {
				const auto* state = globals::state;
				Refuse(a_capture, refusal, state ? state->currentPixelDescriptor & 0xFF : 0xFF);
				return false;
			}
			return !a_capture.probe;
		}

		/**
		 * @brief The accumulator's geometry group 13 when the capture can span it: a pass list (FinishAccumulating draws it with
		 * RenderPersistentPassList) of BSSkyShader TriShapes only, the cloud layers. Null otherwise, logged once per reason and view: an
		 * empty group, or one with a sky-object effect (BSEffectShader, hint 0x12) or another geometry type, is drawn by the engine after
		 * the anchor's run, in the engine's order.
		 */
		const RE::BSBatchRenderer::GeometryGroup* CloudGroupOf(const void* a_accumulator, Capture& a_capture)
		{
			const auto* batch = Engine::At<RE::BSBatchRenderer*>(a_accumulator, 0x130);
			const auto* group = batch ? batch->geometryGroups[kCloudGeometryGroup] : nullptr;
			if (!group || !group->passList.head)
				return nullptr;
			const char* reason = nullptr;
			std::uint32_t code = 0;
			if (!(group->flags & 1)) {
				reason = "it is not a pass list";
				code = 1;
			}
			for (const auto* pass = group->passList.head; pass && !reason; pass = pass->passGroupNext) {
				if (!pass->geometry)
					continue;
				if (!pass->shader || pass->shader->shaderType.get() != RE::BSShader::Type::Sky) {
					reason = "it holds a pass of another shader (a sky-object effect)";
					code = 2;
				} else if (Engine::At<std::uint8_t>(pass->geometry, 0x150) != static_cast<std::uint8_t>(RE::BSGeometry::Type::kTriShape)) {
					reason = "it holds a sky pass that is not a TriShape";
					code = 3;
				}
			}
			if (!reason)
				return group;
			if (a_capture.logged.insert(0xFE000000u | (code << 8) | static_cast<std::uint32_t>(a_capture.shape.view)).second)
				logger::info("[DCLF] sky: the {} cloud layers stay the engine's while {}", a_capture.shape.view == View::Main ? "main" : "face", reason);
			return nullptr;
		}

		/**
		 * @brief The end of a latched capture (after the sky batch, or after the cloud group when it spans it): its checks, the probe's
		 * verdict, and the run handed to the anchor.
		 */
		void Finish(Capture& a_capture)
		{
			a_capture.active = false;
			a_capture.deferred = false;
			a_capture.cloudGroup = nullptr;
			auto run = std::move(a_capture.run);
			const View view = a_capture.shape.view;
			const char* name = view == View::Main ? "main" : "face";
			auto& clean = a_capture.clean[static_cast<std::size_t>(view)];
			// Every pass must draw through DrawTriShape: one that drew through another function was the engine's, at its place in the
			// batch, ahead of the graph's draws.
			if (a_capture.pendingType != 0xFF)
				a_capture.otherTypes |= 1u << (a_capture.pendingType & 31);
			// A shortfall the sky batch did not have is the cloud group's (CloudGroup noted the batch's).
			a_capture.inClouds = a_capture.inClouds && !a_capture.skyShort;
			if (a_capture.drawn < a_capture.passes || a_capture.otherTypes) {
				if (a_capture.logged.insert(0xFF000000u | a_capture.otherTypes).second)
					logger::warn("[DCLF] sky: {} of {} {} sky passes drew without DrawTriShape (geometry types {:#x}, R1's dispatcher)",
						a_capture.passes > a_capture.drawn ? a_capture.passes - a_capture.drawn : 0, a_capture.passes, name, a_capture.otherTypes);
				Refuse(a_capture, Refusal::OtherDrawPath, a_capture.lastTechnique);
			}
			a_capture.inClouds = false;
			if (!run)
				return;
			if (a_capture.probe) {
				if (a_capture.failed && a_capture.cloudsFailed && !a_capture.skyFailed) {
					a_capture.cloudsRefused[static_cast<std::size_t>(view)] = true;
					logger::warn("[DCLF] sky: the {} cloud layers stay the engine's for the session: a cloud draw cannot be drawn by the graph (above)", name);
				}
				clean = !a_capture.failed;
				if (clean)
					logger::info("[DCLF] sky: every {} sky draw can be drawn by the graph; its batches are the graph's from the next", name);
				SkyDraws::Keep(std::move(run->owners));
				return;
			}
			if (a_capture.failed) {
				clean = false;
				logger::warn("[DCLF] sky: a {} sky draw could not be drawn by the graph; the engine draws the batches while the view probes again", name);
			}
			// The face's clouds drawn by the graph, all of them: Cloud Shadows' copy reads this face from its graph faces. A batch that
			// failed partway left some to the engine, which noted the D3D11 layer (SkyShaderHacks).
			if (view == View::Face && a_capture.shape.cloudShadows && !a_capture.failed &&
				std::ranges::any_of(run->draws, [](const Draw& a_draw) { return a_draw.key.occlusion; }))
				ORGCloudShadows::Get().NoteFace(run->face, ORGCloudShadows::FaceSource::Graph);
			SkyDraws::Submit(std::move(run));
		}

		/**
		 * @brief FinishAccumulating's RenderBatches of the sky techniques (SE 99938 +0x247, AE 106583 +0x237): for the main camera's
		 * GBuffer pass and a reflection face, the batch runs with its draws latched, then the anchor's pass draws them at this point, or
		 * after the cloud group when the capture spans it (CloudGroup).
		 */
		struct SkyBatch
		{
			static void thunk(void* a_accumulator, std::uint32_t a_first, std::uint32_t a_last, std::uint32_t a_flags, std::int32_t a_group)
			{
				auto& capture = GetCapture();
				// A capture still waiting for its cloud group (which FinishAccumulating always reaches next) is finished first: its draws
				// are drawn late rather than lost.
				if (capture.deferred)
					Finish(capture);
				if (capture.active || !ShellState::Running() || !FrameAnchors::OnRenderThread() || !SkyDraws::Available()) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				Shape shape;
				std::uint32_t face = 0;
				if (!CurrentShape(a_accumulator, shape, face, capture) || !TakesView(shape.view)) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				auto run = std::make_shared<Run>();
				run->view = shape.view;
				run->face = face;
				if (!SkyDraws::Ready(shape)) {
					// The client is new or was made again: an empty run builds and prepares it, the engine draws this batch, and the
					// view probes again (the new targets may need other programs).
					capture.clean[static_cast<std::size_t>(shape.view)] = false;
					SkyDraws::Submit(std::move(run));
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				capture.shape = std::move(shape);
				capture.view = capture.shape.view;
				capture.run = std::move(run);
				capture.probe = !capture.clean[static_cast<std::size_t>(capture.view)];
				capture.failed = false;
				capture.skyFailed = false;
				capture.cloudsFailed = false;
				capture.skyShort = false;
				capture.passes = 0;
				capture.drawn = 0;
				capture.pendingType = 0xFF;
				capture.otherTypes = 0;
				capture.checked = 0;
				capture.cloudGroup = capture.shape.clouds && !capture.cloudsRefused[static_cast<std::size_t>(capture.view)] ? CloudGroupOf(a_accumulator, capture) : nullptr;
				capture.active = true;
				func(a_accumulator, a_first, a_last, a_flags, a_group);
				capture.active = false;
				if (capture.cloudGroup) {
					capture.deferred = true;
					return;
				}
				Finish(capture);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief FinishAccumulating's RenderPersistentPassList of geometry group 13 (SE 99938 +0x284, AE 106583 +0x274), right after
		 * the sky batch with write mode 11: when the sky batch's capture spans it, the cloud layers are latched as the batch's draws
		 * were, and the capture is finished after them, so the anchor's pass draws the sky and its clouds in the engine's order.
		 */
		struct CloudGroup
		{
			static void thunk(RE::BSBatchRenderer::GeometryGroup* a_group, std::uint32_t a_flags)
			{
				auto& capture = GetCapture();
				if (!capture.deferred || capture.cloudGroup != a_group) {
					func(a_group, a_flags);
					return;
				}
				capture.skyShort = capture.drawn < capture.passes || capture.otherTypes || capture.pendingType != 0xFF;
				capture.active = true;
				capture.inClouds = true;
				func(a_group, a_flags);
				Finish(capture);
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
					++capture.drawn;
					capture.pendingType = 0xFF;
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
				auto& capture = GetCapture();
				if (!capture.active)
					return;
				++capture.passes;
				if (capture.pendingType != 0xFF)
					capture.otherTypes |= 1u << (capture.pendingType & 31);
				capture.pendingType = a_pass && a_pass->geometry ? Engine::At<std::uint8_t>(a_pass->geometry, 0x150) : 0xFE;
				capture.lastPass = a_pass;
				if (const auto* state = globals::state)
					capture.lastTechnique = state->currentPixelDescriptor & 0xFF;
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
		if ((TakesClouds(View::Main) || TakesClouds(View::Face)) && Address::kCloudGroup.Known())
			HookSites::RequireCall("the cloud group (FinishAccumulating's RenderPersistentPassList of group 13)", REL::Offset(Address::kCloudGroup).address());
	}

	void InstallHooks()
	{
		if (!Address::kSkyBatch.Known() || !Address::kDrawTriShape.Known()) {
			logger::warn("[DCLF] sky: this runtime has no address for the sky batch; the sky stays the engine's");
			return;
		}
		stl::write_thunk_call<SkyBatch>(REL::Offset(Address::kSkyBatch).address());
		if ((TakesClouds(View::Main) || TakesClouds(View::Face)) && Address::kCloudGroup.Known())
			stl::write_thunk_call<CloudGroup>(REL::Offset(Address::kCloudGroup).address());
		stl::detour_thunk<DrawTriShape>(REL::Offset(Address::kDrawTriShape).address());
		HookSites::NoteDetour("BSGraphics::Renderer::DrawTriShape (the sky's draws)", DrawTriShape::func.address());
		stl::write_vfunc<0x6, SetupGeometry>(RE::VTABLE_BSSkyShader[0]);
		logger::info("[DCLF] sky: hooks installed (the sky batch{}, DrawTriShape, BSSkyShader::SetupGeometry)",
			(TakesClouds(View::Main) || TakesClouds(View::Face)) && Address::kCloudGroup.Known() ? ", the cloud group" : "");
	}
}
