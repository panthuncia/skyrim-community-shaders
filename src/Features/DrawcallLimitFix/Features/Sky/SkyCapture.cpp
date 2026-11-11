// The sky family's engine side: the sky batch's hook, which lets the engine's batch run with its draws latched; the cloud group's
// hook, which latches the cloud layers drawn right after it and hands the draws to the anchor's pass; the family's DrawTriShape taker,
// which latches a sky draw in place of drawing it; and its BSSkyShader::SetupGeometry watcher, which has ConstantMirror watch the
// constant buffers the sky's draws read. Render thread.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/ConstantMirror.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/PublishedResources.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Logging.h"

#include <rhi_helpers.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace DCLF::Features::Sky
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		// The most draws one batch latches; past it a draw is the engine's (SkyDraws' latch has room for this many).
		constexpr std::size_t kMaxLatched = 128;

		const char* NameOf(View a_view) { return a_view == View::Main ? "main" : "face"; }

		/** @brief The refusals and fallbacks, each logged once per reason. */
		Util::ReasonLog& Refusals()
		{
			static Util::ReasonLog log("[DCLF] sky: the engine draws", spdlog::level::warn);
			return log;
		}

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
			Family::Passes passes;
			std::uint32_t lastTechnique = 0;   // the last pass's raw technique (State::currentPixelDescriptor & 0xFF)
			std::int32_t lastCloudLayer = -1;  // the face's last cloud layer latched in this batch
			Shape shape;                       // the batch's view and targets (CurrentShape)
			// Geometry group 13 when the capture spans the cloud layers too: the batch's draws are handed over after it (CloudGroup).
			const RE::BSBatchRenderer::GeometryGroup* cloudGroup = nullptr;
			bool deferred = false;      // the sky batch ran; the capture is finished after the cloud group
			bool inClouds = false;      // the cloud group is being drawn
			bool skyShort = false;      // the sky batch had a pass that did not draw through DrawTriShape
			bool skyFailed = false;     // a refusal in the sky batch
			bool cloudsFailed = false;  // a refusal in the cloud group
			// The cloud group's call did not follow a sky batch that spanned it (another hook at its site): never spanned again.
			bool deferralBroken = false;
			// A probe whose only refusals were cloud draws leaves the view's cloud layers to the engine for a while, so a cloud
			// technique the graph cannot draw never keeps the rest of the view's sky off the graph. The cause may pass (a program
			// still building, a streamed texture not loaded yet), so the clouds are probed again after cloudsBackoff game frames,
			// which doubles at each refusal up to kMaxCloudsBackoff.
			std::array<bool, static_cast<std::size_t>(View::Count)> cloudsRefused{};
			std::array<std::uint32_t, static_cast<std::size_t>(View::Count)> cloudsRetryFrame{};
			std::array<std::uint32_t, static_cast<std::size_t>(View::Count)> cloudsBackoff{};
		};

		// The first wait before a refused view's clouds are probed again, and the longest (game frames).
		constexpr std::uint32_t kFirstCloudsBackoff = 300;
		constexpr std::uint32_t kMaxCloudsBackoff = 9600;

		/** @brief Whether a_view's clouds are left to the engine now; a refusal whose wait is over is lifted (the next batch probes them). */
		bool CloudsRefused(Capture& a_capture, View a_view)
		{
			const auto index = static_cast<std::size_t>(a_view);
			if (!a_capture.cloudsRefused[index])
				return false;
			if (static_cast<std::int32_t>(Family::GameFrame() - a_capture.cloudsRetryFrame[index]) < 0)
				return true;
			// The wait is over: the view probes again, so the clouds are checked before any is withheld.
			a_capture.cloudsRefused[index] = false;
			a_capture.clean[index] = false;
			return false;
		}

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
		 * @brief The batch's view, targets and inputs from the shadow state and CS's bindings (what its draws would bind): a water reflection
		 * face, or the main camera's GBuffer pass. The published inputs go into a_shape, the others' heap indices into a_run. False for
		 * anything else (another accumulator, a cubemap of another kind, the forward pass, an input that cannot be read), which stays the
		 * engine's.
		 */
		bool CurrentShape(const void* a_accumulator, Shape& a_shape, Run& a_run, Capture& a_capture)
		{
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			auto* renderer = globals::game::renderer;
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			auto& cloudShadows = globals::features::cloudShadows;
			// The sky's pixel stages read Cloud Shadows' occlusion (t25), which the graph has only on its ORG path.
			if (cloudShadows.loaded && !ORGCloudShadows::Get().IsActive())
				return false;
			a_shape.cloudShadows = cloudShadows.loaded;
			// The cloud layers are geometry group 13, which the capture spans only through the cloud group's hook.
			a_shape.clouds = Address::kCloudGroup.Known();
			// The headers' textures the sky's pixel stage may read, as CS binds them for the pass.
			for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h) {
				Family::Header header;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				ID3D11ShaderResourceView* view = nullptr;
				const auto result = Family::ResolveHeader(kHeaderRegisters[h], kHeaderKinds[h], GpuTextures::Tag(GpuTextures::Source::FamilyHeader, static_cast<std::uint32_t>(h)),
					header, a_run.owners, &failure, &view);
				if (result != Family::HeaderResult::Ok) {
					Refusals().Report(std::format("a batch whose t{} cannot be read", kHeaderRegisters[h]),
						result == Family::HeaderResult::Buffer ? "a D3D11 buffer" : GpuTextures::DescribeFailure(view, kHeaderRegisters[h], failure));
					return false;
				}
				a_shape.headerIds[h] = header.publishedId;
				a_run.headerIndices[h] = header.index;
			}
			a_shape.depth = static_cast<std::int32_t>(rss.depthStencil);
			if (!IsDepth(a_shape.depth))
				return false;
			a_capture.colourFormats = {};
			a_capture.depthFormat = FormatOf(depths[a_shape.depth].views[0]);

			if (rss.cubeMapRenderTarget == static_cast<std::uint32_t>(RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS)) {
				a_shape.view = View::Face;
				a_run.face = rss.cubeMapRenderTargetView;
				const auto& cube = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];
				a_capture.colourFormats[0] = FormatOf(cube.cubeSideRTV[0]);
				a_shape.clouds = a_shape.clouds && TakesClouds(View::Face);
				return a_run.face < 6 && a_capture.colourFormats[0] && a_shape.depth == static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS);
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
			if (a_shape.sceneDepth == a_shape.depth) {
				const auto main = static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
				a_shape.sceneDepth = main != a_shape.depth ? main : -1;
			}
			// Neither (Terrain Blending's D3D11 depth): the view itself, read by heap index. Without it the sun's depth test read zero and
			// hid it.
			if (sceneDepth && a_shape.sceneDepthId.empty() && DepthOfView(sceneDepth) < 0) {
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				auto binding = GpuTextures::Get().ResolveBinding(sceneDepth, GpuTextures::Tag(GpuTextures::Source::FrameSlot, 17), &failure);
				if (binding.index == GpuTextures::kInvalid) {
					Refusals().Report("a main batch whose scene depth (t17) cannot be read", GpuTextures::DescribeFailure(sceneDepth, 17, failure));
					return false;
				}
				a_run.sceneDepthIndex = binding.index;
				a_run.owners.push_back(std::move(binding.owner));
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

		/** @brief Why a program is missing, for a refusal. */
		const char* ProgramReason(Family::ProgramState a_state)
		{
			return a_state == Family::ProgramState::Building ? "its ORG program is still building" : "its ORG program cannot be built";
		}

		/**
		 * @brief The draw the engine is about to make, latched: the shadow state SetDirtyStates would apply, the constants the shaders'
		 * SetupGeometry wrote (ConstantMirror), the textures and samplers, the geometry. Null when latched (or, in a probe, when it could
		 * be); otherwise why the graph cannot draw it, and the engine draws it.
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
			if (!vs || !ps || !state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Sky)
				return "the bound shader is not BSSkyShader";

			// Cloud Shadows' override (ModifySky, from SetupGeometry): on a face its layer, which this draw writes on the graph. Its flags
			// are taken only once the draw is latched; a draw the engine makes keeps them for SkyShaderHacks.
			auto& cloudShadows = globals::features::cloudShadows;
			const bool occlusion = a_capture.view == View::Face && cloudShadows.loaded && cloudShadows.overrideSky && cloudShadows.cloudShadowBlendState;

			const auto engine = Pipeline::EngineDrawState::Latch();
			auto* depthStencil = Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode);
			if (!depthStencil)
				return "no depth-stencil state";
			D3D11_DEPTH_STENCIL_DESC depthDesc{};
			depthStencil->GetDesc(&depthDesc);
			// The raster program has no stencil reference; the sky uses none.
			if (depthDesc.StencilEnable)
				return "its depth-stencil state uses stencil";

			// The PerGeometry groups as SetupGeometry wrote them. A technique's group holds only the rows its vanilla shader declares
			// (one may end before TexCoordOff, a pixel stage may have none): the rows past it are zero, which the technique's CS shader
			// does not read either. The matrices and EyePosition/VParams (c0-c12) are every technique's.
			auto& mirror = ConstantMirror::Get();
			const auto* vertexBuffer = reinterpret_cast<const ID3D11Buffer*>(vs->constantBuffers[2].buffer);
			const auto* pixelBuffer = reinterpret_cast<const ID3D11Buffer*>(ps->constantBuffers[2].buffer);
			const auto vertexBytes = vertexBuffer ? mirror.Contents(vertexBuffer) : std::span<const std::byte>{};
			const auto pixelBytes = pixelBuffer ? mirror.Contents(pixelBuffer) : std::span<const std::byte>{};
			if (vertexBytes.size() < 13 * 16)
				return "ConstantMirror has no copy of its vertex constants yet";

			Draw draw{};
			// The descriptors CS looks its shaders up by (State::ModifyShaderLookup): inside the deferred pass a sky pixel shader is
			// the DEFERRED one (descriptor | 256), whose clouds write no Cloud Shadows target and reject nothing by the scene depth.
			std::uint32_t vertexDescriptor = state->currentVertexDescriptor;
			std::uint32_t pixelDescriptor = state->currentPixelDescriptor;
			globals::state->ModifyShaderLookup(*state->currentShader, vertexDescriptor, pixelDescriptor);
			draw.key.descriptor = pixelDescriptor;
			draw.key.defineStage = Family::DefineStage::Pixel;
			draw.key.variant = a_capture.view == View::Face ? kVariantFaces : 0;
			draw.key.blend = occlusion ? static_cast<const void*>(cloudShadows.cloudShadowBlendState) :
			                             Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra);
			// Cloud Shadows binds no depth target for its occlusion draws (SkyShaderHacks).
			draw.key.depthStencil = occlusion ? nullptr : depthStencil;
			draw.key.raster = Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor);
			if (a_capture.view == View::Main) {
				std::copy(a_capture.colourFormats.begin(), a_capture.colourFormats.end(), draw.key.colourFormats.begin());
				draw.key.depthFormat = a_capture.depthFormat;
			} else {
				draw.key.colourFormats = { a_capture.colourFormats[0], occlusion ? static_cast<std::uint32_t>(rhi::helpers::ToRHI(DXGI_FORMAT_R8_UNORM)) : 0u, 0u, 0u };
				draw.key.depthFormat = occlusion ? 0u : a_capture.depthFormat;
			}
			if (!draw.key.blend || !draw.key.raster)
				return "no blend or rasterizer state object";
			Family::ProgramState programState{};
			draw.program = SkyDraws::Program(draw.key, *state->currentShader, programState);
			if (!draw.program)
				return ProgramReason(programState);
			if (occlusion) {
				// The face's layers in the engine's order, each once: a later layer then holds every earlier one's occlusion (FaceClouds).
				draw.occlusion = true;
				draw.layer = cloudShadows.currentLayerForDraw;
				if (draw.layer < 0 || draw.layer >= static_cast<std::int32_t>(kCloudLayers) || draw.layer <= a_capture.lastCloudLayer)
					return "its cloud layer is out of order";
				auto occlusionKey = draw.key;
				occlusionKey.maskedTargets = 1;
				draw.occlusionProgram = SkyDraws::Program(occlusionKey, *state->currentShader, programState);
				if (!draw.occlusionProgram)
					return ProgramReason(programState);
			}

			// The geometry, read by device address while its leases are held (the run keeps them until the GPU is done).
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
			auto& constants = draw.constants;
			constants.vertexAddress[0] = static_cast<std::uint32_t>(geometry.vertexAddress);
			constants.vertexAddress[1] = static_cast<std::uint32_t>(geometry.vertexAddress >> 32);
			constants.indexAddress[0] = static_cast<std::uint32_t>(geometry.indexAddress);
			constants.indexAddress[1] = static_cast<std::uint32_t>(geometry.indexAddress >> 32);
			constants.vertexLayout[0] = static_cast<std::uint32_t>(geometry.layout);
			constants.vertexLayout[1] = static_cast<std::uint32_t>(geometry.layout >> 32);
			constants.vertexStride = geometry.stride;
			constants.firstIndex = geometry.firstIndex;
			draw.indexCount = geometry.indexCount;

			// The textures the technique samples, and every slot's sampler (an unused slot reads a null view and takes the first sampler:
			// it is never sampled).
			const auto used = TexturesOf(state->currentPixelDescriptor & 0xFF);
			for (std::uint32_t t = 0; t < 3; ++t) {
				auto* view = used[t] ? Unwrap(rss.PSTexture[t]) : nullptr;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, GpuTextures::ViewKind::Texture2D, GpuTextures::Tag(GpuTextures::Source::FrameSlot, t),
					constants.textures[t], owners, &failure);
				if (slot == Family::SlotResult::Published)
					return "a texture slot holds a published graph texture";
				if (slot != Family::SlotResult::Ok) {
					Refusals().Report(std::format("a texture of technique {} could not be imported", state->currentPixelDescriptor & 0xFF),
						GpuTextures::DescribeFailure(view, t, failure));
					return "a texture it samples could not be imported";
				}
				constants.samplers[t] = used[t] ? Family::SlotSampler(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying(), owners) :
				                                  Family::SlotSampler(0, 0, owners);
				if (constants.samplers[t] == GpuTextures::kInvalid)
					return "its sampler modes have no graph sampler";
			}
			// Cloud self shadowing (t26): a main pass cloud layer reads the occlusion of the layers up to its own (SkyShaderHacks binds that
			// layer), anything else the occlusion cubemap.
			constants.textures[3] = kSelfShadowFromOcclusion;
			if (a_capture.view == View::Main && cloudShadows.loaded && cloudShadows.bindLayerSelfShadow) {
				const auto layer = cloudShadows.currentLayerForDraw;
				auto* layerTexture = layer >= 0 && layer < CloudShadows::kMaxCloudLayers ? cloudShadows.texCloudShadowLayers[layer] : nullptr;
				if (!layerTexture || !layerTexture->srv)
					return "its cloud layer has no occlusion texture";
				if (Family::SlotTexture(layerTexture->srv.get(), GpuTextures::ViewKind::Cube, GpuTextures::Tag(GpuTextures::Source::FrameSlot, 26), constants.textures[3],
						owners) != Family::SlotResult::Ok)
					return "its cloud layer's occlusion could not be imported";
			}

			// The constants: PerGeometry as the vertex stage reads it (c0 to c16.xy), the pixel stage's PParams, the alpha test reference
			// SetDirtyStates would write (PS b11: the reference while the test is on, else 0) and CS's extra descriptor for this draw
			// (State::UpdateSkyShaderPermutation ran in SetupGeometry).
			std::memcpy(&constants, vertexBytes.data(), (std::min)(vertexBytes.size(), kVertexConstantBytes));
			std::memcpy(constants.pParams, pixelBytes.data(), (std::min)(pixelBytes.size(), sizeof(constants.pParams)));
			constants.alphaTestRef = rss.alphaTestEnabled ? rss.alphaTestRef : 0.0f;
			constants.extraShaderDescriptor = state->permutationData.ExtraShaderDescriptor;
			// The camera's depth range as it is, with no depth bias offset (mode 0). The sky draws at the far plane: an offset taken off its
			// maximum depth wrote the dome and the moon masks short of 1, so the deferred pass lit and fogged the sky as geometry (grey
			// overcast, a darker exposure) and the clouds drew over the moons (sky-river-org1, 2026-11-11). The engine's sky leaves the
			// range whole; the shadow state's bias mode is not the table's index for these draws.
			draw.viewport = Family::LatchViewport(0);

			// The imports are kept, a probe's too (SkyDraws::Keep): the next batch finds them instead of importing again.
			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			// A probe only checks: the engine draws it, with Cloud Shadows' overrides.
			if (a_capture.probe)
				return nullptr;
			if (draw.occlusion) {
				// Cloud Shadows' bookkeeping for the face and the layer's clear, on D3D11 before the anchor's run draws into it.
				cloudShadows.NoteGraphCloudLayer(static_cast<int>(run.face), draw.layer);
				a_capture.lastCloudLayer = draw.layer;
			}
			run.draws.push_back(std::move(draw));
			// Its D3D11 overrides (SkyShaderHacks) run only at a draw's state flush, which a latched draw never reaches.
			cloudShadows.overrideSky = false;
			cloudShadows.bindLayerSelfShadow = false;
			return nullptr;
		}

		/** @brief A refusal noted: the batch fails, and the reason is logged once per technique and view. */
		void Refuse(Capture& a_capture, std::string_view a_reason, std::uint32_t a_technique)
		{
			a_capture.failed = true;
			(a_capture.inClouds ? a_capture.cloudsFailed : a_capture.skyFailed) = true;
			Refusals().Report(std::format("a {} draw of technique {}: {}", NameOf(a_capture.view), a_technique, a_reason), a_capture.probe ? "found by a probe" : "");
		}

		/**
		 * @brief A draw latched (true: the graph draws it) or the engine's. A draw that fails marks the batch: its later draws are the
		 * engine's too, so the engine's draws of the batch stay in order after the ones latched before (which the anchor draws after
		 * the batch: one batch drawn out of order, then the view probes again).
		 */
		bool LatchDraw(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			// A probe checks every draw, so one batch asks for every program the view needs; a batch the graph draws stops at the first
			// draw it cannot take.
			if (a_capture.failed && !a_capture.probe)
				return false;
			if (const char* reason = TryLatchDraw(a_capture, a_shape, a_start, a_triangles)) {
				const auto* state = globals::state;
				Refuse(a_capture, reason, state ? state->currentPixelDescriptor & 0xFF : 0xFF);
				return false;
			}
			return !a_capture.probe;
		}

		/**
		 * @brief The accumulator's geometry group 13 when the capture can span it: a pass list (FinishAccumulating draws it with
		 * RenderPersistentPassList) of BSSkyShader TriShapes only, the cloud layers. Null otherwise, logged once per reason and view: an
		 * empty group, or one with a sky-object effect (BSEffectShader, hint 0x12) or another geometry type, is drawn by the engine after
		 * the anchor's run, in the engine's order. FinishAccumulating calls the group exactly when it is non-null with flag 1 set (SE
		 * 99938 +0x260, AE 106583 +0x259), which a spanned group always is.
		 */
		const RE::BSBatchRenderer::GeometryGroup* CloudGroupOf(void* a_accumulator, Capture& a_capture)
		{
			// BSShaderAccumulator::batchRenderer (+0x130 on SE and AE: RenderBatches reads it there, SE 0x1412cce40, AE 0x1414b4fe0).
			const auto* batch = static_cast<RE::BSShaderAccumulator*>(a_accumulator)->GetRuntimeData()->batchRenderer;
			const auto* group = batch ? batch->geometryGroups[kCloudGeometryGroup] : nullptr;
			if (!group || !group->passList.head)
				return nullptr;
			const char* reason = nullptr;
			if (!(group->flags & 1))
				reason = "it is not a pass list";
			for (const auto* pass = group->passList.head; pass && !reason; pass = pass->passGroupNext) {
				if (!pass->geometry)
					continue;
				if (!pass->shader || pass->shader->shaderType.get() != RE::BSShader::Type::Sky)
					reason = "it holds a pass of another shader (a sky-object effect)";
				else if (pass->geometry->GetType().get() != RE::BSGeometry::Type::kTriShape)
					reason = "it holds a sky pass that is not a TriShape";
			}
			if (!reason)
				return group;
			static Util::ReasonLog log("[DCLF] sky: the engine draws the cloud layers this batch");
			log.Report(std::format("{} view: {}", NameOf(a_capture.shape.view), reason));
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
			const bool spannedClouds = a_capture.cloudGroup != nullptr;
			a_capture.cloudGroup = nullptr;
			auto run = std::move(a_capture.run);
			const View view = a_capture.shape.view;
			auto& clean = a_capture.clean[static_cast<std::size_t>(view)];
			// A shortfall the sky batch did not have is the cloud group's (CloudGroup noted the batch's).
			a_capture.inClouds = a_capture.inClouds && !a_capture.skyShort;
			if (!a_capture.passes.End()) {
				Refusals().Report(a_capture.passes.Describe(std::format("{} sky", NameOf(view))));
				Refuse(a_capture, "a pass of the batch drew through another draw function than DrawTriShape", a_capture.lastTechnique);
			}
			a_capture.inClouds = false;
			if (!run)
				return;
			if (a_capture.probe) {
				const auto index = static_cast<std::size_t>(view);
				if (a_capture.failed && a_capture.cloudsFailed && !a_capture.skyFailed && !a_capture.cloudsRefused[index]) {
					auto& backoff = a_capture.cloudsBackoff[index];
					backoff = backoff ? (std::min)(backoff * 2, kMaxCloudsBackoff) : kFirstCloudsBackoff;
					a_capture.cloudsRefused[index] = true;
					a_capture.cloudsRetryFrame[index] = Family::GameFrame() + backoff;
					logger::warn("[DCLF] sky: the {} cloud layers stay the engine's for {} frames: a cloud draw cannot be drawn by the graph (above); they are probed again then",
						NameOf(view), backoff);
				} else if (!a_capture.failed && spannedClouds) {
					// A clean probe of the clouds: a later refusal starts from the first wait again.
					a_capture.cloudsBackoff[index] = 0;
				}
				const bool wasClean = clean;
				clean = !a_capture.failed;
				if (clean && !wasClean)
					logger::info("[DCLF] sky: every {} sky draw can be drawn by the graph; its batches are the graph's from the next", NameOf(view));
				SkyDraws::Keep(std::move(run->owners));
				return;
			}
			if (a_capture.failed) {
				clean = false;
				Refusals().Report(std::format("a {} batch the graph had taken, from a draw it could not latch on", NameOf(view)), "the view probes again");
			}
			// A run that was not submitted lost its draws for this frame: the view probes again.
			if (!SkyDraws::Submit(std::move(run)))
				clean = false;
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
				// The cloud group's call always follows a batch that spanned it (CloudGroupOf). If another hook at its site kept it from
				// coming, the capture is finished now, late, and no batch spans the group again.
				if (capture.deferred) {
					if (!std::exchange(capture.deferralBroken, true))
						logger::error("[DCLF] sky: the cloud group was not drawn after a sky batch that spanned it (another hook at its call?); the engine draws the cloud layers from now on");
					Finish(capture);
				}
				if (capture.active || !ShellState::Running() || !FrameAnchors::IsRenderOwner() || !SkyDraws::Available()) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				Family::BeginBatch();
				Shape shape;
				auto run = std::make_shared<Run>();
				if (!CurrentShape(a_accumulator, shape, *run, capture)) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				run->view = shape.view;
				const auto index = static_cast<std::size_t>(shape.view);
				if (!SkyDraws::Ready(shape)) {
					// The view's client is joining the graph, or was made for another shape: the engine draws this batch, and the view
					// probes again once it is ready (the new targets may need other programs).
					capture.clean[index] = false;
					SkyDraws::Keep(std::move(run->owners));
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				const bool cloudsRefused = CloudsRefused(capture, shape.view);
				const bool probe = !capture.clean[index];
				// Draws are withheld only when the anchor's run will draw them (a probe withholds nothing).
				if (!probe && !SkyDraws::CanRun(shape.view)) {
					Refusals().Report(std::format("a {} batch whose anchor cannot run now", NameOf(shape.view)), "the ring is full or the client left the built graph");
					SkyDraws::Keep(std::move(run->owners));
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				capture.shape = std::move(shape);
				capture.view = capture.shape.view;
				capture.run = std::move(run);
				capture.probe = probe;
				capture.failed = false;
				capture.skyFailed = false;
				capture.cloudsFailed = false;
				capture.skyShort = false;
				capture.lastCloudLayer = -1;
				capture.passes.Begin();
				capture.cloudGroup = capture.shape.clouds && !cloudsRefused && !capture.deferralBroken ? CloudGroupOf(a_accumulator, capture) : nullptr;
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
				const auto& passes = capture.passes;
				capture.skyShort = passes.drawn < passes.passes || passes.otherTypes || passes.pendingType != Family::Passes::kNone;
				capture.active = true;
				capture.inClouds = true;
				func(a_group, a_flags);
				Finish(capture);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The family's DrawTriShape taker: during a latched sky batch, the draw is latched instead. */
		bool TakeDraw(const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& capture = GetCapture();
			if (!capture.active || !capture.run)
				return false;
			capture.passes.OnDraw();
			if (LatchDraw(capture, a_shape, a_start, a_triangles))
				return true;
			++capture.fallbacks;
			return false;
		}

		/**
		 * @brief After BSSkyShader::SetupGeometry (chained after Cloud Shadows' and the frame annotations'): during a latched batch,
		 * ConstantMirror watches the vertex and pixel stages' PerGeometry buffers, which the draw reads back. A buffer is mirrored from its
		 * next write on, so a batch's first sight of one draws on the engine.
		 */
		void OnSetupGeometry(RE::BSRenderPass* a_pass)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return;
			capture.passes.OnSetup(a_pass);
			if (const auto* state = globals::state)
				capture.lastTechnique = state->currentPixelDescriptor & 0xFF;
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
		const bool known = Address::kSkyBatch.Known() && Family::Address::kDrawTriShape.Known();
		HookSites::RequireKnown("the sky batch", known);
		if (!known)
			return;
		HookSites::RequireCall("the sky batch (FinishAccumulating's RenderBatches of the sky techniques)", REL::Offset(Address::kSkyBatch).address());
		if ((TakesClouds(View::Main) || TakesClouds(View::Face)) && Address::kCloudGroup.Known())
			HookSites::RequireCall("the cloud group (FinishAccumulating's RenderPersistentPassList of group 13)", REL::Offset(Address::kCloudGroup).address());
	}

	void InstallHooks()
	{
		if (!Address::kSkyBatch.Known() || !Family::AddDrawTaker("sky", &TakeDraw)) {
			logger::warn("[DCLF] sky: this runtime has no address for the sky batch; the sky stays the engine's");
			return;
		}
		stl::write_thunk_call<SkyBatch>(REL::Offset(Address::kSkyBatch).address());
		const bool clouds = (TakesClouds(View::Main) || TakesClouds(View::Face)) && Address::kCloudGroup.Known();
		if (clouds)
			stl::write_thunk_call<CloudGroup>(REL::Offset(Address::kCloudGroup).address());
		Family::WatchSetupGeometry<RE::BSShader::Type::Sky>(RE::VTABLE_BSSkyShader[0], &OnSetupGeometry);
		logger::info("[DCLF] sky: hooks installed (the sky batch{}, BSSkyShader::SetupGeometry)", clouds ? ", the cloud group" : "");
	}
}
