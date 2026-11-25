// Engine-side hooks for the sky family. They let the engine's sky batch, cloud layers and sun occlusion draw run
// with their draws latched, so the graph can draw them in the engine's order. In native mode (CS_DCLF_SKY_NATIVE=2) the draws are
// computed instead, and the sun query's engine call is not made. Render thread only.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/Engine/Engine.h"
#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"
#include "Features/DrawcallLimitFix/Features/Sky/SkyNative.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Queries/OcclusionQueries.h"
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
		using Stage = Family::Constants::Stage;

		// The most draws one run latches. A longer batch flushes what it has and continues in a new run.
		constexpr std::size_t kMaxLatched = 128;

		const char* NameOf(View a_view) { return a_view == View::Main ? "main" : a_view == View::Face ? "face" : "sun occlusion"; }

		namespace Queries = ::RenderGraph::Engine::OcclusionQueries;

		/** @brief Why a sun occlusion draw was skipped, each logged once. */
		Family::RefusalLog& SunSkips()
		{
			static Family::RefusalLog log("[DCLF] sky: a sun occlusion draw is skipped, its query answers its last count");
			return log;
		}

		Family::RefusalLog& Refusals()
		{
			static Family::RefusalLog log("[DCLF] sky: the engine draws");
			return log;
		}

		// TryLatchDraw's reason for a draw whose program is still building. Compared by address.
		constexpr char kProgramBuilding[] = "its ORG program is still building";

		/** @brief Draws left out while their program builds, each logged once by view and technique. */
		Family::RefusalLog& LeftOut()
		{
			static Family::RefusalLog log("[DCLF] sky: a draw is left out until its program is built (the engine's draw would bypass the graph)");
			return log;
		}

		/**
		 * @brief The batch being latched, on the render thread.
		 * After a refused draw the latched part runs, and the rest of the batch goes to D3D11 where the census counts it.
		 */
		struct Capture
		{
			bool active = false;
			bool failed = false;  // a draw was refused: the latched part ran, the rest is not the graph's
			View view = View::Main;
			std::shared_ptr<Run> run;
			std::array<std::uint32_t, 3> colourFormats{};  // the main pass's targets, or the face's (one)
			std::uint32_t depthFormat = 0;
			Family::Passes passes;
			std::uint32_t lastTechnique = 0;   // the last pass's raw technique (State::currentPixelDescriptor & 0xFF)
			std::int32_t lastCloudLayer = -1;  // the face's last cloud layer latched in this batch
			Shape shape;                       // the batch's view and targets (CurrentShape)
			// Geometry group 13 when the capture also spans the cloud layers.
			const RE::BSBatchRenderer::GeometryGroup* cloudGroup = nullptr;
			bool deferred = false;  // the sky batch ran; the capture is finished after the cloud group
			// Set when the cloud group's call did not follow a spanning sky batch, for example another hook at that site.
			bool deferralBroken = false;
			bool inCloudGroup = false;  // the capture spans the cloud group's call now
			void* accumulator = nullptr;  // the sky batch's, for the cloud group's native draws
			std::uint32_t renderFlags = 0;  // the sky batch's RenderBatches flags
			bool cloudLatched = false;  // a cloud layer of it was latched (sky-object effects before it are the engine's)
		};

		Capture& GetCapture()
		{
			static Capture capture;
			return capture;
		}

		// Observe mode: the face the engine renders now (ReflectionFaces), or null. Render thread.
		const FaceView* s_observedFace = nullptr;

		/** @brief The face draws that could not be submitted, each reason logged once. */
		Family::RefusalLog& FaceFailures()
		{
			static Family::RefusalLog log("[DCLF] sky: a native reflection face's sky is not drawn");
			return log;
		}

		std::uint32_t FormatOf(ID3D11RenderTargetView* a_rtv)
		{
			if (!a_rtv)
				return 0;
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			a_rtv->GetDesc(&desc);
			return static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
		}

		std::uint32_t FormatOf(ID3D11DepthStencilView* a_dsv)
		{
			if (!a_dsv)
				return 0;
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			a_dsv->GetDesc(&desc);
			return static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
		}

		bool IsTarget(std::int32_t a_target) { return a_target >= 0 && a_target < static_cast<std::int32_t>(RE::RENDER_TARGETS::kTOTAL); }
		bool IsDepth(std::int32_t a_depth) { return a_depth >= 0 && a_depth < static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); }

		/** @brief The engine depth target whose depth SRV is a_view, or -1. */
		std::int32_t DepthOfView(ID3D11ShaderResourceView* a_view)
		{
			const auto& depths = globals::game::renderer->GetDepthStencilData().depthStencils;
			for (std::int32_t d = 0; d < static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); ++d)
				if (a_view && Unwrap(depths[d].depthSRV) == a_view)
					return d;
			return -1;
		}

		/** @brief The published ids a batch's headers and t17 export. */
		struct PublishedInputIds
		{
			std::array<std::string, kHeaderRegisters.size()> headers;
			std::string sceneDepth;
		};

		/** @brief The header textures bound now (kHeaderRegisters), as imports or published ids. False when one cannot be read. */
		bool ResolveHeaders(Run& a_run, PublishedInputIds& a_published)
		{
			DCLF_ZONE("CS.DCLF.Sky.ResolveHeaders");
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
				a_published.headers[h] = std::move(header.publishedId);
				a_run.headerIndices[h] = header.index;
			}
			return true;
		}

		/**
		 * @brief Reads the batch's view, targets and inputs, for a water reflection face or the main GBuffer pass.
		 * Returns false for anything else, which stays the engine's.
		 */
		bool CurrentShape(const void* a_accumulator, Shape& a_shape, Run& a_run, PublishedInputIds& a_published, Capture& a_capture)
		{
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			auto* renderer = globals::game::renderer;
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			auto& cloudShadows = globals::features::cloudShadows;
			// The sky's pixel stages read Cloud Shadows' occlusion (t25), which the graph has only on its ORG path.
			if (cloudShadows.loaded && !ORGCloudShadows::Get().IsActive()) {
				Refusals().Report("a batch while Cloud Shadows runs on D3D11", "its occlusion (t25) is not on the graph");
				return false;
			}
			a_shape.cloudShadows = cloudShadows.loaded;
			a_shape.clouds = Address::kCloudGroup.Known();
			if (!ResolveHeaders(a_run, a_published))
				return false;
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
				return a_run.face < 6 && a_capture.colourFormats[0] && a_shape.depth == static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS);
			}
			if (rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return false;
			// The main camera's accumulator, inside CS's deferred pass.
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
			// t17 is the scene depth CS binds. Any engine depth other than the prepass copy, the depth target or Terrain Blending's is refused.
			auto* sceneDepth = Util::GetCurrentSceneDepthSRV(true);
			const auto engineDepth = DepthOfView(sceneDepth);
			if (engineDepth == static_cast<std::int32_t>(kSceneDepth))
				return a_capture.depthFormat != 0;
			if (engineDepth >= 0) {
				if (engineDepth != a_shape.depth) {
					Refusals().Report(std::format("a main batch whose scene depth (t17) is engine depth {}", engineDepth), "the client imports the Z-prepass copy");
					return false;
				}
				a_run.sceneDepthNull = true;
				return a_capture.depthFormat != 0;
			}
			if (!sceneDepth) {
				a_run.sceneDepthNull = true;
				return a_capture.depthFormat != 0;
			}
			a_published.sceneDepth = PublishedResources::ExportedId(sceneDepth);
			// Terrain Blending's D3D11 depth is read by heap index. Without it the sun's depth test read zero and hid the sun.
			if (a_published.sceneDepth.empty()) {
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

		/** @brief Reads a batch's published inputs by id. False when the client cannot read one, and the batch stays the engine's. */
		bool ReadInputs(View a_view, const PublishedInputIds& a_published, Run& a_run)
		{
			DCLF_ZONE("CS.DCLF.Sky.ReadInputs");
			const auto read = [&](std::uint32_t a_register, const std::string& a_id, std::uint32_t& a_index) {
				if (a_id.empty())
					return true;
				const auto result = SkyDraws::ReadPublished(a_view, a_id, a_index);
				if (result == Family::InputResult::Graph)
					return true;
				Refusals().Report(std::format("a {} batch whose t{} holds '{}'", NameOf(a_view), a_register, a_id), Family::ReasonOf(result));
				return false;
			};
			for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h)
				if (!read(kHeaderRegisters[h], a_published.headers[h], a_run.headerIndices[h]))
					return false;
			if (!read(17, a_published.sceneDepth, a_run.sceneDepthIndex))
				return false;
			// Physical Sky's resources, which the sky's pixel stages read through the draw's latch.
			const auto readPhysSky = [a_view](std::string_view a_id, std::uint32_t& a_index, std::string& a_why) {
				const auto result = SkyDraws::ReadPublished(a_view, a_id, a_index);
				a_why = Family::ReasonOf(result);
				return result == Family::InputResult::Graph;
			};
			std::string why;
			if (!Integrations::PhysicalSky::ResolveOrgHandles(a_run.physSky, a_run.owners, readPhysSky, why, a_view == View::Face)) {
				Refusals().Report(std::format("a {} batch", NameOf(a_view)), why);
				return false;
			}
			return true;
		}

		/** @brief Which of t0-t2 a sky technique samples. */
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

		/** @brief Runs the latched part of the batch now, before anything the engine draws after it. */
		void FlushLatched(Capture& a_capture)
		{
			auto run = std::move(a_capture.run);
			if (!run)
				return;
			if (run->draws.empty()) {
				SkyDraws::Keep(std::move(run->owners));
				return;
			}
			SkyDraws::Submit(std::move(run));
		}

		/**
		 * @brief Runs the full run and continues the batch in a new one with the same view and inputs.
		 * False for a face's cloud draws, since a later layer's copies draw every earlier layer of the same run.
		 */
		bool Continue(Capture& a_capture)
		{
			const auto& full = *a_capture.run;
			if (std::ranges::any_of(full.draws, [](const Draw& a_draw) { return a_draw.occlusion; }))
				return false;
			auto next = std::make_shared<Run>();
			next->view = full.view;
			next->face = full.face;
			next->sceneDepthIndex = full.sceneDepthIndex;
			next->sceneDepthNull = full.sceneDepthNull;
			next->headerIndices = full.headerIndices;
			next->physSky = full.physSky;
			FlushLatched(a_capture);
			a_capture.run = std::move(next);
			return true;
		}

		/** @brief Runs the latched part and leaves the rest of the batch to the engine. Logged once per reason, technique and view. */
		void Refuse(Capture& a_capture, std::string_view a_reason, std::uint32_t a_technique)
		{
			a_capture.failed = true;
			Refusals().Report(std::format("a {} draw of technique {}: {}", NameOf(a_capture.view), a_technique, a_reason));
			FlushLatched(a_capture);
		}

		/** @brief Latches the draw the engine is about to make. Returns null when latched, else the reason it was refused. */
		const char* TryLatchDraw(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			if (a_capture.run->draws.size() >= kMaxLatched && !Continue(a_capture))
				return "more draws than one run latches, with a face's cloud layers among them";
			auto& run = *a_capture.run;
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* ps = rss.currentPixelShader;
			const auto* state = globals::state;
			if (!vs || !ps || !state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Sky)
				return "the bound shader is not BSSkyShader";

			// Cloud Shadows' flags are taken only once the draw is latched. A draw the engine makes keeps them for SkyShaderHacks.
			auto& cloudShadows = globals::features::cloudShadows;
			const bool occlusion = a_capture.view == View::Face && cloudShadows.loaded && cloudShadows.overrideSky && cloudShadows.cloudShadowBlendState;

			const auto engine = Pipeline::EngineDrawState::Latch();
			auto* depthStencil = Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode);
			if (!depthStencil)
				return "no depth-stencil state";

			// A technique's group holds only the rows its vanilla shader declares. The rest are zero, and its CS shader does not read them.
			const auto use = Family::CaptureConstants();
			if (!use.Valid())
				return "the engine constants capture is not active";
			if (Family::ConstantBytes(use, Stage::Vertex, 2) < 13 * 16)
				return "its vertex PerGeometry is smaller than its matrices";

			Draw draw{};
			// Inside the deferred pass the sky pixel shader is the DEFERRED one (descriptor | 256), as State::ModifyShaderLookup picks it.
			std::uint32_t vertexDescriptor = state->currentVertexDescriptor;
			std::uint32_t pixelDescriptor = state->currentPixelDescriptor;
			globals::state->ModifyShaderLookup(*state->currentShader, vertexDescriptor, pixelDescriptor);
			draw.key.descriptor = pixelDescriptor;
			// A stencil test or write (a face's technique 8) uses the shadow state's reference, as the engine's draw would.
			draw.stencilRef = engine.stencilRef;
			draw.key.defineStage = Family::DefineStage::Pixel;
			draw.key.variant = a_capture.view != View::Main ? kVariantFaces : 0;
			auto* blend = occlusion ? cloudShadows.cloudShadowBlendState :
			                          Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra);
			// Cloud Shadows binds no depth target for its occlusion draws (SkyShaderHacks).
			if (!Family::DescribeStates(draw.key, blend, occlusion ? nullptr : depthStencil, Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor)))
				return "a blend, depth-stencil or rasterizer state with no description";
			if (a_capture.view == View::Main) {
				std::copy(a_capture.colourFormats.begin(), a_capture.colourFormats.end(), draw.key.colourFormats.begin());
				draw.key.depthFormat = a_capture.depthFormat;
			} else {
				draw.key.colourFormats = { a_capture.colourFormats[0], occlusion ? static_cast<std::uint32_t>(rhi::helpers::ToRHI(DXGI_FORMAT_R8_UNORM)) : 0u, 0u, 0u };
				draw.key.depthFormat = occlusion ? 0u : a_capture.depthFormat;
			}
			bool building = false;
			draw.program = SkyDraws::Program(draw.key, *state->currentShader, &building);
			if (!draw.program)
				return building ? kProgramBuilding : "its ORG program cannot be built";
			if (occlusion) {
				// Each face layer once, in engine order, so a later layer holds every earlier layer's occlusion.
				draw.occlusion = true;
				draw.layer = cloudShadows.currentLayerForDraw;
				if (draw.layer < 0 || draw.layer >= static_cast<std::int32_t>(kCloudLayers) || draw.layer <= a_capture.lastCloudLayer)
					return "its cloud layer is out of order";
				auto occlusionKey = draw.key;
				occlusionKey.maskedTargets = 1;
				draw.occlusionProgram = SkyDraws::Program(occlusionKey, *state->currentShader, &building);
				if (!draw.occlusionProgram)
					return building ? kProgramBuilding : "its occlusion program cannot be built";
			}

			// The geometry is read by device address. The run holds its leases until the GPU is done.
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
			Family::CopyConstants(use, Stage::Vertex, 2, { reinterpret_cast<std::byte*>(&constants), kVertexConstantBytes });
			Family::CopyConstants(use, Stage::Pixel, 2, { reinterpret_cast<std::byte*>(constants.pParams), sizeof(constants.pParams) });
			constants.vertexRef = geometry.vertexRef;
			constants.indexRef = geometry.indexRef;
			constants.vertexLayout[0] = static_cast<std::uint32_t>(geometry.layout);
			constants.vertexLayout[1] = static_cast<std::uint32_t>(geometry.layout >> 32);
			constants.vertexStride = geometry.stride;
			constants.firstIndex = geometry.firstIndex;
			draw.indexCount = geometry.indexCount;

			// An unused slot reads a null view with the first sampler. It is never sampled.
			const auto used = TexturesOf(state->currentPixelDescriptor & 0xFF);
			for (std::uint32_t t = 0; t < 3; ++t) {
				auto* view = used[t] ? Unwrap(rss.PSTexture[t]) : nullptr;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, GpuTextures::ViewKind::Texture2D, GpuTextures::Tag(GpuTextures::Source::FrameSlot, t),
					constants.textures[t], owners, &failure);
				if (slot == Family::SlotResult::Published) {
					const auto id = PublishedResources::ExportedId(view);
					const auto result = SkyDraws::ReadPublished(a_capture.view, id, constants.textures[t]);
					if (result != Family::InputResult::Graph) {
						Refusals().Report(std::format("a {} draw's t{} holds '{}'", NameOf(a_capture.view), t, id), Family::ReasonOf(result));
						return "a texture slot holds a published graph texture its client cannot read";
					}
				} else if (slot != Family::SlotResult::Ok) {
					Refusals().Report(std::format("a texture of technique {} could not be imported", state->currentPixelDescriptor & 0xFF),
						GpuTextures::DescribeFailure(view, t, failure));
					return "a texture it samples could not be imported";
				}
				constants.samplers[t] = used[t] ? Family::SlotSampler(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying(), owners) :
				                                  Family::SlotSampler(0, 0, owners);
				if (constants.samplers[t] == GpuTextures::kInvalid)
					return "its sampler modes have no graph sampler";
			}
			// Cloud self shadowing (t26). A main pass cloud layer reads its own layers' occlusion, anything else the occlusion cubemap.
			constants.textures[3] = kSelfShadowFromOcclusion;
			if (a_capture.view == View::Main && cloudShadows.loaded && cloudShadows.bindLayerSelfShadow) {
				const auto layer = cloudShadows.currentLayerForDraw;
				auto* layerTexture = layer >= 0 && layer < CloudShadows::kMaxCloudLayers ? cloudShadows.texCloudShadowLayers[layer] : nullptr;
				if (!layerTexture || !layerTexture->srv)
					return "its cloud layer has no occlusion texture";
				// A layer twin is read by published id, so the graph orders the read after the layer's writes. Only a D3D11 layer is imported.
				if (const auto id = ORGCloudShadows::PublishedLayerId(layer); !id.empty()) {
					const auto result = SkyDraws::ReadPublished(View::Main, id, constants.textures[3]);
					if (result != Family::InputResult::Graph) {
						Refusals().Report(std::format("a main cloud draw whose t26 holds '{}'", id), Family::ReasonOf(result));
						return "its cloud layer's occlusion is a published graph texture its client cannot read";
					}
				} else if (Family::SlotTexture(layerTexture->srv.get(), GpuTextures::ViewKind::Cube, GpuTextures::Tag(GpuTextures::Source::FrameSlot, 26),
							   constants.textures[3], owners) != Family::SlotResult::Ok) {
					return "its cloud layer's occlusion could not be imported";
				}
			}

			// The alpha test reference SetDirtyStates would write to PS b11.
			constants.alphaTestRef = rss.alphaTestEnabled ? rss.alphaTestRef : 0.0f;
			// A reflection face's draw is a reflection draw, as CS's RenderCubemap hook marks it on D3D11 (IsReflections, Sky.hlsl's
			// InReflection). The capture can run outside that hook, so the face's view sets the bit here.
			constants.extraShaderDescriptor = state->permutationData.ExtraShaderDescriptor |
			                                  (a_capture.view == View::Face ? static_cast<std::uint32_t>(State::ExtraShaderDescriptors::IsReflections) : 0u);
			constants.physSky = run.physSky;
			// No depth bias offset. An offset left the sky short of depth 1, so the deferred pass lit and fogged it as geometry.
			// The engine's sky also leaves the range whole.
			draw.viewport = Family::LatchViewport(0);

			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			if (draw.occlusion) {
				cloudShadows.NoteGraphCloudLayer(static_cast<int>(run.face), draw.layer);
				a_capture.lastCloudLayer = draw.layer;
			}
			run.draws.push_back(std::move(draw));
			// SkyShaderHacks runs only at a draw's state flush, which a latched draw never reaches.
			cloudShadows.overrideSky = false;
			cloudShadows.bindLayerSelfShadow = false;
			return nullptr;
		}

		/**
		 * @brief Geometry group 13 when the capture can span it, which needs it to hold only BSSkyShader TriShapes.
		 * FinishAccumulating calls the group exactly when it is non-null with flag 1 set (SE 99938 +0x260, AE 106583 +0x259).
		 */
		const RE::BSBatchRenderer::GeometryGroup* CloudGroupOf(void* a_accumulator, Capture& a_capture)
		{
			// BSShaderAccumulator::batchRenderer is at +0x130 on SE and AE.
			const auto* batch = static_cast<RE::BSShaderAccumulator*>(a_accumulator)->GetRuntimeData()->batchRenderer;
			const auto* group = batch ? batch->geometryGroups[kCloudGeometryGroup] : nullptr;
			if (!group || !group->passList.head)
				return nullptr;
			const char* reason = nullptr;
			if (!(group->flags & 1))
				reason = "it is not a pass list";
			// Sky-object effects (accumulation hint 0x12, BSEffectShader) are registered at the group's head, before the cloud layers:
			// the engine draws them first, and the cloud layers are latched after them. One after a cloud layer would break that order.
			bool sky = false;
			for (const auto* pass = group->passList.head; pass && !reason; pass = pass->passGroupNext) {
				if (!pass->geometry)
					continue;
				if (!pass->shader || pass->shader->shaderType.get() != RE::BSShader::Type::Sky) {
					if (sky)
						reason = "it holds a pass of another shader after a cloud layer";
					continue;
				}
				sky = true;
				if (pass->geometry->GetType().get() != RE::BSGeometry::Type::kTriShape)
					reason = "it holds a sky pass that is not a TriShape";
			}
			if (!reason)
				return group;
			static Family::RefusalLog log("[DCLF] sky: the engine draws the cloud layers this batch");
			log.Report(std::format("{} view: {}", NameOf(a_capture.shape.view), reason));
			return nullptr;
		}

		/** @brief Ends a capture and runs the latched part. */
		void Finish(Capture& a_capture)
		{
			a_capture.active = false;
			a_capture.deferred = false;
			a_capture.cloudGroup = nullptr;
			if (!a_capture.passes.End())
				Refusals().Report(a_capture.passes.Describe(std::format("{} sky", NameOf(a_capture.shape.view))));
			FlushLatched(a_capture);
		}

		/**
		 * @brief Hook on FinishAccumulating's RenderBatches of the sky techniques (SE 99938 +0x247, AE 106583 +0x237).
		 * The batch runs with its draws latched, for the main GBuffer pass and reflection faces.
		 */
		struct SkyBatch
		{
			static void thunk(void* a_accumulator, std::uint32_t a_first, std::uint32_t a_last, std::uint32_t a_flags, std::int32_t a_group)
			{
				auto& capture = GetCapture();
				// If another hook kept the cloud group's call from coming, finish late and never span the group again.
				if (capture.deferred) {
					if (!std::exchange(capture.deferralBroken, true))
						logger::error("[DCLF] sky: the cloud group was not drawn after a sky batch that spanned it (another hook at its call?); the engine draws the cloud layers from now on");
					Finish(capture);
				}
				if (capture.active) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				const auto engineDraws = [&] { func(a_accumulator, a_first, a_last, a_flags, a_group); };
				if (!FrameAnchors::IsRenderOwner() || !SkyDraws::Available())
					return engineDraws();
				Family::BeginBatch();
				Shape shape;
				auto run = std::make_shared<Run>();
				PublishedInputIds published;
				if (!CurrentShape(a_accumulator, shape, *run, published, capture) || !SkyDraws::CanTake(shape) || !ReadInputs(shape.view, published, *run)) {
					SkyDraws::Keep(std::move(run->owners));
					return engineDraws();
				}
				run->view = shape.view;
				capture.shape = std::move(shape);
				capture.view = capture.shape.view;
				capture.run = std::move(run);
				capture.failed = false;
				capture.lastCloudLayer = -1;
				capture.passes.Begin();
				capture.cloudGroup = capture.shape.clouds && !capture.deferralBroken ? CloudGroupOf(a_accumulator, capture) : nullptr;
				capture.accumulator = a_accumulator;
				capture.renderFlags = a_flags;
				if (capture.view == View::Face && s_observedFace && s_observedFace->face == capture.run->face)
					Native::CompareFaceBatch(*s_observedFace, static_cast<RE::BSShaderAccumulator*>(a_accumulator), a_flags);
				// The draws computed from the registered passes (SkyNative.cpp), when CS_DCLF_SKY_NATIVE asks for them.
				const auto nativeMode = Native::GetMode();
				std::vector<Draw> native;
				bool nativeBuilt = false;
				if (nativeMode != Native::Mode::Off) {
					auto* accumulator = static_cast<RE::BSShaderAccumulator*>(a_accumulator);
					const Native::BatchInputs inputs{ capture.view, capture.colourFormats, capture.depthFormat, a_flags, capture.run->physSky };
					// The leases go straight to the run: a failed build's leases are only held a frame longer.
					std::string why;
					nativeBuilt = Native::BuildBatch(accumulator, inputs, native, capture.run->owners, why) && native.size() <= kMaxLatched;
					if (!nativeBuilt) {
						static Family::RefusalLog log("[DCLF] sky native: the batch stays the capture's");
						log.Report(std::format("a {} batch", NameOf(capture.view)), why.empty() ? std::string("more draws than one run latches") : why);
					}
					// Drawn natively: the engine's call finds no sky pass to draw, and nothing is latched.
					if (nativeBuilt && nativeMode == Native::Mode::Draw) {
						capture.run->draws = std::move(native);
						auto detached = Native::DetachBatch(accumulator);
						func(a_accumulator, a_first, a_last, a_flags, a_group);
						Native::Reattach(detached);
						if (capture.cloudGroup) {
							capture.deferred = true;
							return;
						}
						Finish(capture);
						return;
					}
				}
				capture.active = true;
				func(a_accumulator, a_first, a_last, a_flags, a_group);
				capture.active = false;
				if (nativeBuilt && !capture.failed && capture.run)
					Native::Compare(capture.view, native, capture.run->draws);
				if (capture.cloudGroup && !capture.failed) {
					capture.deferred = true;
					return;
				}
				Finish(capture);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Hook on the RenderPersistentPassList of geometry group 13 (SE 99938 +0x284, AE 106583 +0x274).
		 * Latches the cloud layers after a spanning sky batch, so sky and clouds draw in engine order.
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
				if (capture.view == View::Face && s_observedFace && capture.run && s_observedFace->face == capture.run->face)
					Native::CompareFaceClouds(*s_observedFace, a_group);
				// The cloud layers computed from the group's passes (SkyNative.cpp), when CS_DCLF_SKY_NATIVE asks for them.
				const auto nativeMode = Native::GetMode();
				std::vector<Draw> native;
				bool nativeBuilt = false;
				if (nativeMode != Native::Mode::Off && capture.run && capture.accumulator) {
					const Native::BatchInputs inputs{ capture.view, capture.colourFormats, capture.depthFormat, a_flags, capture.run->physSky };
					std::string why;
					std::int32_t lastLayer = capture.lastCloudLayer;
					nativeBuilt = Native::BuildClouds(static_cast<RE::BSShaderAccumulator*>(capture.accumulator), a_group, inputs, lastLayer, native,
									  capture.run->owners, why) &&
					              capture.run->draws.size() + native.size() <= kMaxLatched;
					if (!nativeBuilt) {
						static Family::RefusalLog log("[DCLF] sky native: the cloud layers stay the capture's");
						log.Report(std::format("a {} cloud group", NameOf(capture.view)), why.empty() ? std::string("more draws than one run latches") : why);
					}
					// Drawn natively: the engine's call draws only the sky-object effects at the group's head, and nothing is latched.
					if (nativeBuilt && nativeMode == Native::Mode::Draw) {
						auto& cloudShadows = globals::features::cloudShadows;
						capture.run->draws.reserve(capture.run->draws.size() + native.size());
						for (auto& draw : native) {
							if (draw.occlusion)
								cloudShadows.NoteGraphCloudLayer(static_cast<int>(capture.run->face), draw.layer);
							capture.run->draws.push_back(std::move(draw));
						}
						capture.lastCloudLayer = lastLayer;
						Native::DetachClouds(a_group);
						func(a_group, a_flags);
						Finish(capture);
						return;
					}
				}
				const std::size_t before = capture.run ? capture.run->draws.size() : 0;
				capture.active = true;
				capture.inCloudGroup = true;
				capture.cloudLatched = false;
				func(a_group, a_flags);
				capture.inCloudGroup = false;
				if (nativeBuilt && !capture.failed && capture.run && capture.run->draws.size() >= before)
					Native::Compare(capture.view, native, std::span<const Draw>(capture.run->draws).subspan(before));
				Finish(capture);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The sun occlusion draw's view from the shadow state. Returns null when taken, else why not. */
		const char* SunShape(Capture& a_capture, Run& a_run)
		{
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			auto* renderer = globals::game::renderer;
			if (rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return "it draws into a cubemap";
			constexpr auto kMain = static_cast<std::int32_t>(RE::RENDER_TARGETS::kMAIN);
			if (static_cast<std::int32_t>(rss.renderTargets[0]) != kMain)
				return "its first target is not kMAIN";
			const auto depth = static_cast<std::int32_t>(rss.depthStencil);
			if (depth == static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN))
				a_run.sunCopyDepth = false;
			else if (depth == static_cast<std::int32_t>(kSceneDepth))
				a_run.sunCopyDepth = true;
			else
				return "its depth target is neither kMAIN's nor the Z-prepass copy";
			a_capture.colourFormats = { FormatOf(renderer->GetRuntimeData().renderTargets[kMain].RTV), 0, 0 };
			a_capture.depthFormat = FormatOf(renderer->GetDepthStencilData().depthStencils[depth].views[0]);
			return a_capture.colourFormats[0] && a_capture.depthFormat ? nullptr : "a target format cannot be read";
		}

		/** @brief The sun query's view, or why the graph cannot take its draw now (logged once each). */
		const char* SunRefusal(Capture& a_capture, Run& a_run)
		{
			const char* refusal = SunShape(a_capture, a_run);
			if (!refusal && (!FrameAnchors::IsRenderOwner() || !SkyDraws::Available()))
				refusal = "the graph cannot draw now";
			if (!refusal && !SkyDraws::CanTakeSun())
				refusal = "its anchor cannot run now";
			if (refusal)
				SunSkips().Report(refusal);
			return refusal;
		}

		Native::BatchInputs SunInputs(const Capture& a_capture, const Run& a_run)
		{
			return { View::Sun, a_capture.colourFormats, a_capture.depthFormat, 0u, a_run.physSky };
		}

		/**
		 * @brief The engine's sun query (FUN_1414b4c00) without its draw: the query's bookkeeping as the engine keeps it, and the sun's
		 * occlusion draw computed natively for the graph's query. The engine's call is not made.
		 */
		void DrawNative(Capture& a_capture, RE::BSShaderAccumulator* a_accumulator, RE::BSGeometry* a_geometry, std::uint32_t a_query, RE::NiCamera* a_camera)
		{
			// The accumulator's three query entries at +0x70, 0x18 apart: the ring entry, two counts and the pending flag (+0x14).
			// The engine skips a disabled accumulator (+0x64) and an entry whose last result is still pending.
			auto* bytes = reinterpret_cast<std::byte*>(a_accumulator);
			if (!a_geometry || static_cast<bool>(bytes[0x64]) || a_query >= 3)
				return;
			auto* entry = bytes + 0x70 + std::size_t(a_query) * 0x18;
			auto& pending = *reinterpret_cast<bool*>(entry + 0x14);
			if (pending)
				return;
			auto& query = *reinterpret_cast<void**>(entry);
			// The engine's occlusion query entries (the ring's), as FUN_1414b4c00 calls them.
			static const REL::Relocation<void* (*)(void*)> createQuery{ REL::RelocationID(75565, 77366) };
			static const REL::Relocation<void (*)(void*, void*)> beginQuery{ REL::RelocationID(75567, 77368) };
			static const REL::Relocation<void (*)(void*, void*)> endQuery{ REL::RelocationID(75568, 77369) };
			auto* renderer = globals::game::renderer;
			if (!query) {
				query = createQuery(renderer);
				if (!query) {
					pending = false;
					*reinterpret_cast<std::uint32_t*>(entry + 0x8) = 0;
					*reinterpret_cast<std::uint32_t*>(entry + 0x10) = 0;
					return;
				}
			}
			Queries::Bracket bracket;
			Family::BeginBatch();
			auto run = std::make_shared<Run>();
			run->view = View::Sun;
			beginQuery(renderer, query);
			// The engine's call sets the query's camera before its draw, and later code keeps it.
			RE::BSGraphics::State::GetSingleton()->SetCameraData(a_camera, 0);
			std::string why;
			const char* skip = SunRefusal(a_capture, *run);
			if (!skip && !Native::BuildSun(a_accumulator, a_geometry, SunInputs(a_capture, *run), run->draws.emplace_back(), run->owners, why)) {
				SunSkips().Report("its draw cannot be computed", why);
				skip = "its draw cannot be computed";
			}
			endQuery(renderer, query);
			pending = true;
			*reinterpret_cast<std::uint32_t*>(entry + 0x10) = 0;
			if (!skip && bracket.Slot() == Queries::kNoSlot)
				skip = "a draw with no query begun";
			if (skip) {
				SkyDraws::Keep(std::move(run->owners));
				bracket.Skip(skip);
				return;
			}
			run->query = bracket.Slot();
			if (!SkyDraws::Submit(std::move(run)))
				bracket.Skip("its run was not submitted");
		}

		/**
		 * @brief Hook on BSShaderAccumulator's occlusion query draw (SE 99958, AE 106604). Only in graph mode.
		 * An ORG query answers the engine's GetQueryData. A draw the graph cannot take is skipped.
		 */
		struct SunOcclusionQuery
		{
			static void thunk(void* a_accumulator, RE::BSGeometry* a_geometry, std::uint32_t a_query, RE::NiCamera* a_camera)
			{
				if (!Queries::Active() || !SkyDraws::HasSunClient()) {
					func(a_accumulator, a_geometry, a_query, a_camera);
					return;
				}
				auto& capture = GetCapture();
				if (capture.active || capture.deferred) {
					SunSkips().Report("a sun query inside a sky batch's capture: the engine's call is left out");
					return;
				}
				if (Native::GetMode() == Native::Mode::Draw) {
					DrawNative(capture, static_cast<RE::BSShaderAccumulator*>(a_accumulator), a_geometry, a_query, a_camera);
					return;
				}
				Queries::Bracket bracket;
				Family::BeginBatch();
				auto run = std::make_shared<Run>();
				run->view = View::Sun;
				const char* refusal = SunRefusal(capture, *run);
				// Observe: the native draw from the same inputs, compared with the latched one after the engine's call.
				Draw native;
				bool nativeBuilt = false;
				if (!refusal && Native::GetMode() == Native::Mode::Observe) {
					RE::BSGraphics::State::GetSingleton()->SetCameraData(a_camera, 0);
					std::string why;
					nativeBuilt = Native::BuildSun(static_cast<RE::BSShaderAccumulator*>(a_accumulator), a_geometry, SunInputs(capture, *run), native, run->owners, why);
					if (!nativeBuilt)
						SunSkips().Report("observe: the native draw cannot be computed", why);
				}
				capture.view = View::Sun;
				capture.shape = {};
				capture.shape.view = View::Sun;
				capture.run = run;
				capture.failed = refusal != nullptr;
				capture.lastCloudLayer = -1;
				capture.cloudGroup = nullptr;
				capture.passes.Begin();
				capture.active = true;
				func(a_accumulator, a_geometry, a_query, a_camera);
				capture.active = false;
				capture.run.reset();
				if (!capture.passes.End() && !capture.failed) {
					capture.failed = true;
					SunSkips().Report(capture.passes.Describe("sun occlusion"));
				}
				if (nativeBuilt && !capture.failed && !run->draws.empty())
					Native::Compare(View::Sun, std::span<const Draw>(&native, 1), run->draws);
				// Nothing begun and nothing drawn, because a result was pending or there was no query.
				if (bracket.Slot() == Queries::kNoSlot && run->draws.empty() && !capture.failed) {
					SkyDraws::Keep(std::move(run->owners));
					return;
				}
				const char* skip = capture.failed                     ? "its draw was refused" :
				                   bracket.Slot() == Queries::kNoSlot ? "a draw with no query begun" :
				                   run->draws.size() != 1             ? "not exactly one draw between its BeginQuery and EndQuery" :
				                                                        nullptr;
				if (skip) {
					SkyDraws::Keep(std::move(run->owners));
					bracket.Skip(skip);
					return;
				}
				run->query = bracket.Slot();
				if (!SkyDraws::Submit(std::move(run)))
					bracket.Skip("its run was not submitted");
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Latches a draw during a latched sky batch instead of drawing it. */
		bool TakeDraw(const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return false;
			capture.passes.OnDraw();
			if (capture.view == View::Sun) {
				if (capture.failed || !capture.run)
					return true;
				const char* reason = capture.run->draws.empty() ? TryLatchDraw(capture, a_shape, a_start, a_triangles) : "a second draw in its bracket";
				if (reason) {
					capture.failed = true;
					SunSkips().Report(reason, std::format("technique {}", globals::state ? globals::state->currentPixelDescriptor & 0xFF : 0xFF));
				}
				return true;
			}
			if (capture.failed || !capture.run)
				return false;
			// A sky-object effect at the cloud group's head (CloudGroupOf): the engine's draw, before the cloud layers it latches.
			if (capture.inCloudGroup && !capture.cloudLatched) {
				const auto* state = globals::state;
				if (!state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Sky)
					return false;
			}
			if (const char* reason = TryLatchDraw(capture, a_shape, a_start, a_triangles)) {
				const auto* state = globals::state;
				const std::uint32_t technique = state ? state->currentPixelDescriptor & 0xFF : 0xFF;
				// A program first seen in this draw builds on the program service. Until then the draw is left out and the batch
				// stays the graph's. Handed to the engine, the draw and the rest of its batch would reach D3D11, where Cloud Shadows'
				// layer draws are fatal in graph mode.
				if (reason == kProgramBuilding) {
					LeftOut().Report(std::format("a {} draw of technique {}", NameOf(capture.view), technique));
					// SkyShaderHacks never runs for a draw the engine does not make, so its flags are cleared as for a latched draw.
					auto& cloudShadows = globals::features::cloudShadows;
					cloudShadows.overrideSky = false;
					cloudShadows.bindLayerSelfShadow = false;
					return true;
				}
				Refuse(capture, reason, technique);
				return false;
			}
			if (capture.inCloudGroup)
				capture.cloudLatched = true;
			return true;
		}

		/** @brief Counts the pass after BSSkyShader::SetupGeometry. A pass that will not reach DrawTriShape flushes first. */
		void OnSetupGeometry(RE::BSRenderPass* a_pass)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return;
			if (const auto* state = globals::state)
				capture.lastTechnique = state->currentPixelDescriptor & 0xFF;
			if (capture.passes.OnSetup(a_pass) || capture.failed)
				return;
			if (capture.view == View::Sun) {
				capture.failed = true;
				SunSkips().Report("a pass the dispatcher draws through another function than DrawTriShape", "that draw reaches D3D11");
				return;
			}
			Refuse(capture, "a pass the dispatcher draws through another function than DrawTriShape", capture.lastTechnique);
		}
	}

	std::shared_ptr<Run> PrepareFace(const FaceView& a_view, std::string& a_why)
	{
		DCLF_ZONE("CS.DCLF.Sky.PrepareFace");
		if (!FrameAnchors::IsRenderOwner() || !SkyDraws::Available()) {
			a_why = "the sky family does not draw now";
			return nullptr;
		}
		if (Native::GetMode() != Native::Mode::Draw) {
			a_why = "the sky is not native (CS_DCLF_SKY_NATIVE)";
			return nullptr;
		}
		auto& cloudShadows = globals::features::cloudShadows;
		if (cloudShadows.loaded && !ORGCloudShadows::Get().IsActive()) {
			a_why = "Cloud Shadows runs on D3D11";
			return nullptr;
		}
		Family::BeginBatch();
		// The face's shape, as CurrentShape reads it from the shadow state inside the engine's face render.
		Shape shape;
		shape.view = View::Face;
		shape.depth = static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS);
		shape.cloudShadows = cloudShadows.loaded;
		shape.clouds = Address::kCloudGroup.Known();
		if (!SkyDraws::CanTake(shape)) {
			a_why = "the faces' client does not take a face now";
			return nullptr;
		}
		auto* renderer = globals::game::renderer;
		const auto& cube = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];
		const auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS];
		const Native::BatchInputs inputs{ View::Face, { FormatOf(cube.cubeSideRTV[0]), 0u, 0u }, FormatOf(depth.views[0]) };
		if (!inputs.colourFormats[0] || !inputs.depthFormat) {
			a_why = "the reflection cube or its depth has no view";
			return nullptr;
		}
		auto run = std::make_shared<Run>();
		run->view = View::Face;
		run->face = a_view.face;
		if (!Native::BuildFace(a_view, inputs, run->draws, run->owners, a_why) || run->draws.size() > kMaxLatched) {
			if (a_why.empty())
				a_why = "more draws than one run latches";
			SkyDraws::Keep(std::move(run->owners));
			return nullptr;
		}
		// The sky pass's first draw clears the face and its depth, so a face needs one.
		if (run->draws.empty()) {
			a_why = "a face without sky draws";
			SkyDraws::Keep(std::move(run->owners));
			return nullptr;
		}
		return run;
	}

	bool SubmitFace(std::shared_ptr<Run> a_run)
	{
		// What CS's reflection prepasses bound: the header textures, the published inputs and Physical Sky's resources.
		PublishedInputIds published;
		DCLF_ZONE("CS.DCLF.Sky.SubmitFace");
		if (!ResolveHeaders(*a_run, published) || !ReadInputs(View::Face, published, *a_run)) {
			FaceFailures().Report("its inputs cannot be read", "see the sky's refusals");
			SkyDraws::Keep(std::move(a_run->owners));
			return false;
		}
		auto& cloudShadows = globals::features::cloudShadows;
		for (auto& draw : a_run->draws) {
			draw.constants.physSky = a_run->physSky;
			if (draw.occlusion)
				cloudShadows.NoteGraphCloudLayer(static_cast<int>(a_run->face), draw.layer);
		}
		return SkyDraws::Submit(std::move(a_run));
	}

	void ObserveFace(const FaceView* a_view) { s_observedFace = a_view; }

	void PreflightHooks()
	{
		if (!Enabled())
			return;
		const bool known = Address::kSkyBatch.Known() && Family::Address::kDrawTriShape.Known();
		HookSites::RequireKnown("the sky batch", known);
		if (!known)
			return;
		HookSites::RequireCall("the sky batch (FinishAccumulating's RenderBatches of the sky techniques)", REL::Offset(Address::kSkyBatch).address());
		if (Address::kCloudGroup.Known())
			HookSites::RequireCall("the cloud group (FinishAccumulating's RenderPersistentPassList of group 13)", REL::Offset(Address::kCloudGroup).address());
	}

	void InstallHooks()
	{
		if (!Address::kSkyBatch.Known() || !Family::AddDrawTaker("sky", &TakeDraw)) {
			logger::warn("[DCLF] sky: this runtime has no address for the sky batch; the sky stays the engine's");
			return;
		}
		stl::write_thunk_call<SkyBatch>(REL::Offset(Address::kSkyBatch).address());
		// Hooked even when the graph does not take the clouds, because the engine's clouds are vouched for there.
		const bool clouds = Address::kCloudGroup.Known();
		if (clouds)
			stl::write_thunk_call<CloudGroup>(REL::Offset(Address::kCloudGroup).address());
		Family::WatchSetupGeometry<RE::BSShader::Type::Sky>(RE::VTABLE_BSSkyShader[0], &OnSetupGeometry);
		if (::RenderGraph::Engine::GraphMode() && Address::kSunOcclusionQuery.Known()) {
			stl::detour_thunk<SunOcclusionQuery>(REL::Offset(Address::kSunOcclusionQuery).address());
			HookSites::NoteDetour("BSShaderAccumulator's occlusion query draw (the sun's, on the graph)", SunOcclusionQuery::func.address());
		}
		Family::AddSetup("sky", [] { return SkyDraws::Setup(); });
		logger::info("[DCLF] sky: hooks installed (the sky batch{}, BSSkyShader::SetupGeometry)", clouds ? ", the cloud group" : "");
	}
}
