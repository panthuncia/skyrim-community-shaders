// Engine-side hook for the precipitation family. DrawTriShape latches a precipitation draw and runs the anchor's pass for it at
// once, at its place in the grouped alphas. Render thread.
// A draw the family cannot take reaches the engine with nobody vouching for it. Its reason is logged once.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/Precipitation/Precipitation.h"

#include "Deferred.h"
#include "Features/RainRefraction.h"
#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/PublishedResources.h"
#include "State.h"
#include "Utils/Logging.h"

#include <rhi_helpers.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace DCLF::Features::Precipitation
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;
		using Stage = Family::Constants::Stage;
		using Kind = GpuTextures::ViewKind;

		static_assert(sizeof(Deferred::DirectionalShadowLightData) == Water::kDirectionalShadowBytes, "t98's record");
		static_assert(sizeof(RainRefraction::PerFrame) == sizeof(DrawConstants::rainRefraction), "RainRefractionSettings' layout");

		// Light Limit Fix's strict lights at PS b3, bound once a frame by CS.
		constexpr std::uint32_t kStrictLightSlot = 3;

		/** @brief The inherent refusals, each logged once per reason. */
		Family::RefusalLog& Refusals()
		{
			static Family::RefusalLog log("[DCLF] precipitation: the engine draws");
			return log;
		}

		/** @brief The last uploaded directional shadow record. A frame with no sun shadow light keeps it, as t98 keeps its buffer. */
		Deferred::DirectionalShadowLightData& DirectionalShadow()
		{
			static Deferred::DirectionalShadowLightData record{};
			return record;
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

		/** @brief Resolves the headers and t80 to heap indices. Returns null when resolved, else why the draw stays the engine's. */
		const char* ResolveHeaders(Run& a_run, DrawConstants& a_constants)
		{
			for (std::size_t h = 0; h <= Water::kHeaderRegisters.size(); ++h) {
				const bool raindrop = h == Water::kHeaderRegisters.size();
				const std::uint32_t reg = raindrop ? kRaindropRegister : Water::kHeaderRegisters[h];
				const Kind kind = raindrop ? Kind::Texture2D : Water::kHeaderKinds[h];
				Family::Header header;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				ID3D11ShaderResourceView* view = nullptr;
				const auto result = Family::ResolveHeader(reg, kind, GpuTextures::Tag(GpuTextures::Source::FamilyHeader, static_cast<std::uint32_t>(h)), header, a_run.owners,
					&failure, &view);
				if (result == Family::HeaderResult::Buffer) {
					// A D3D11 buffer, as on Light Limit Fix's D3D11 path, cannot be imported, so the precipitation stays the engine's.
					Refusals().Report(std::format("t{} is a D3D11 buffer the graph cannot read", reg), "turn on its feature's ORG path");
					return "a header is a D3D11 buffer";
				}
				if (result != Family::HeaderResult::Ok) {
					Refusals().Report(std::format("t{} could not be imported", reg), GpuTextures::DescribeFailure(view, reg, failure));
					return "a header could not be imported";
				}
				std::uint32_t index = header.index;
				if (!header.publishedId.empty()) {
					const auto read = PrecipitationDraws::ReadPublished(header.publishedId, index);
					if (read != Family::InputResult::Graph) {
						Refusals().Report(std::format("t{} holds '{}'", reg, header.publishedId), Family::ReasonOf(read));
						return "a header holds a published resource its client cannot read";
					}
				}
				if (raindrop)
					a_constants.raindropNormals = index;
				else
					a_run.headerIndices[h] = index;
			}
			return nullptr;
		}

		/**
		 * @brief Latches the draw the engine is about to make into a_run. Refractive rain uses Rain Refraction's own blend state.
		 * Returns null when latched, else why the engine must draw it.
		 */
		const char* TryLatchDraw(Run& a_run, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* ps = rss.currentPixelShader;
			const auto* state = globals::state;
			if (!vs || !ps || !state || !state->currentShader)
				return "no bound shader";
			const auto main = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
			if (rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return "a cube face's draw";
			if (rss.depthStencil != main)
				return "a depth target other than kMAIN's";

			const auto engine = Pipeline::EngineDrawState::Latch();
			Draw draw{};
			const auto& engineTargets = globals::game::renderer->GetRuntimeData().renderTargets;
			for (std::uint32_t i = 0; i < kMaxColours; ++i) {
				const auto target = static_cast<std::int32_t>(rss.renderTargets[i]);
				if (target < 0)
					break;
				const auto found = std::ranges::find(kTargets, target);
				if (i == 0 && target != kTargets[0]) {
					Refusals().Report(std::format("first colour target {}", target), "not kMAIN, the one first target the client imports");
					return "a colour target its client does not import";
				}
				// A second target the client does not import: write kMAIN alone, since Particle.hlsl's colour is what shows.
				if (found == kTargets.end()) {
					Refusals().Report(std::format("second colour target {} left bound", target), "drawn into kMAIN alone, its normal output dropped");
					break;
				}
				draw.colours[i] = static_cast<std::uint8_t>(found - kTargets.begin());
				draw.key.colourFormats[i] = FormatOf(engineTargets[target].RTV);
				if (!draw.key.colourFormats[i])
					return "a colour target with no format";
				draw.colourCount = i + 1;
			}
			if (draw.colourCount == 0)
				return "no colour target";
			// Particle.hlsl writes two outputs, so the engine's draw leaves any third target as it was, as seen on the river's water in rain.
			if (draw.colourCount == kMaxColours && static_cast<std::int32_t>(rss.renderTargets[kMaxColours]) >= 0) {
				static Util::ReasonLog extra("[DCLF] precipitation: a draw's targets past the pixel stage's two outputs are left out (the engine's draw leaves them as they were)");
				extra.Report(std::format("colour target {}", static_cast<std::int32_t>(rss.renderTargets[kMaxColours])));
			}
			const auto& depths = globals::game::renderer->GetDepthStencilData().depthStencils;
			draw.key.depthFormat = FormatOf(depths[main].views[0]);
			if (!draw.key.depthFormat)
				return "no depth format";

			// Rain Refraction binds its own blend state on the context for refractive rain (ParticleShaderHacks).
			ID3D11BlendState* blend = Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra);
			if (auto* refractive = globals::features::rainRefraction.RefractiveRainBlendState())
				blend = refractive;
			if (!Family::DescribeStates(draw.key, blend, Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode),
					Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor)))
				return "a depth-stencil, blend or rasterizer state with no description";
			draw.stencilRef = engine.stencilRef;
			draw.key.descriptor = state->modifiedPixelDescriptor;
			draw.key.defineStage = Family::DefineStage::Pixel;

			auto& constants = draw.latch.constants;
			const auto use = Family::CaptureConstants();
			if (!use.Valid())
				return "the engine constants capture is not active";
			if (!Family::ConstantBytes(use, Stage::Vertex, 2))
				return "no vertex PerGeometry";
			Family::CopyRows(use, Stage::Vertex, 0, constants.vsTechnique, kVSTechniqueRows);
			Family::CopyRows(use, Stage::Vertex, 2, constants.vsGeometry, kVSGeometryRows);
			Family::CopyRows(use, Stage::Pixel, 2, constants.psGeometry, kPSGeometryRows);
			Family::CopyConstants(use, Stage::Pixel, kStrictLightSlot, draw.latch.strictLights);
			auto& directionalShadow = DirectionalShadow();
			if (globals::deferred)
				globals::deferred->GetDirectionalShadowLightData(directionalShadow);
			std::memcpy(draw.latch.directionalShadow, &directionalShadow, Water::kDirectionalShadowBytes);
			const auto& frame = globals::game::frameBufferCached.data;
			constants.projection[0] = frame.CameraProj.m[0][0];
			constants.projection[1] = frame.CameraProj.m[1][1];
			constants.projection[2] = frame.DynamicResolutionParams1.x;
			constants.projection[3] = frame.DynamicResolutionParams1.y;
			const auto& bufferDim = state->lastSharedData.BufferDim;
			constants.bufferDim[0] = bufferDim.x;
			constants.bufferDim[1] = bufferDim.y;
			constants.bufferDim[2] = bufferDim.z;
			constants.bufferDim[3] = bufferDim.w;
			const auto rain = globals::features::rainRefraction.GetCommonBufferData();
			std::memcpy(constants.rainRefraction, &rain, sizeof(constants.rainRefraction));
			const auto& permutation = state->permutationData;
			constants.descriptors[0] = permutation.VertexShaderDescriptor;
			constants.descriptors[1] = permutation.PixelShaderDescriptor;
			constants.descriptors[2] = permutation.ExtraShaderDescriptor;
			constants.descriptors[3] = permutation.ExtraFeatureDescriptor;
			constants.effectRadius = permutation.EffectRadius;

			draw.program = PrecipitationDraws::Program(draw.key, *state->currentShader);
			if (!draw.program)
				return "its ORG program cannot be built";

			// The geometry is read by device address. The run holds its leases until the GPU is done.
			Family::Owners owners;
			Family::Geometry geometry;
			std::string geometryDetail;
			if (const auto result = Family::LatchGeometry(a_shape, a_start, a_triangles, vs->vertexDesc, engine.topology, geometry, owners, &geometryDetail);
				result != Family::GeometryResult::Ok) {
				if (!geometryDetail.empty())
					Refusals().Report(std::format("a draw whose geometry is refused: {}", Family::ReasonOf(result)), geometryDetail);
				return Family::ReasonOf(result);
			}
			constants.vertexRef = geometry.vertexRef;
			constants.indexRef = geometry.indexRef;
			constants.vertexLayout[0] = static_cast<std::uint32_t>(geometry.layout);
			constants.vertexLayout[1] = static_cast<std::uint32_t>(geometry.layout >> 32);
			constants.vertexStride = geometry.stride;
			constants.firstIndex = geometry.firstIndex;
			draw.indexCount = geometry.indexCount;

			// An empty slot reads zero, as on D3D11. A slot the techniques never sample gets a null view and the first sampler.
			for (std::uint32_t t = 0; t < kMaterialSlots; ++t) {
				const bool read = SlotRead(t);
				auto* view = read ? Unwrap(rss.PSTexture[t]) : nullptr;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, Kind::Texture2D, GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), constants.textures[t], owners, &failure);
				if (slot == Family::SlotResult::Published) {
					// An engine target a graph pass writes is read by published id, so the graph orders the write before the draw.
					const auto id = PublishedResources::ExportedId(view);
					const auto result = PrecipitationDraws::ReadPublished(id, constants.textures[t]);
					if (result != Family::InputResult::Graph) {
						Refusals().Report(std::format("t{} holds '{}'", t, id), Family::ReasonOf(result));
						return "a texture slot holds a published graph texture its client cannot read";
					}
				} else if (slot != Family::SlotResult::Ok) {
					Refusals().Report(std::format("texture t{} could not be imported", t), GpuTextures::DescribeFailure(view, t, failure));
					return "a texture it samples could not be imported";
				}
				constants.samplers[t] = read ? Family::SlotSampler(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying(), owners) :
				                               Family::SlotSampler(0, 0, owners);
				if (constants.samplers[t] == GpuTextures::kInvalid)
					return "its sampler modes have no graph sampler";
			}
			if (const char* reason = ResolveHeaders(a_run, constants)) {
				for (auto& owner : owners)
					if (owner)
						a_run.owners.push_back(std::move(owner));
				return reason;
			}

			draw.viewport = Family::LatchViewport(engine.depthBias);

			for (auto& owner : owners)
				if (owner)
					a_run.owners.push_back(std::move(owner));
			a_run.draws.push_back(std::move(draw));
			return nullptr;
		}

		/** @brief Latches an ENVCUBE BSParticleShader draw and has the anchor's pass draw it now. */
		bool TakeDraw(const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			const auto* state = globals::state;
			if (!state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Particle ||
				!PrecipitationTechnique(state->currentPixelDescriptor))
				return false;
			if (!FrameAnchors::IsRenderOwner() || !PrecipitationDraws::Available())
				return false;
			Family::BeginBatch();
			if (!PrecipitationDraws::CanTake())
				return false;
			auto run = std::make_shared<Run>();
			if (const char* reason = TryLatchDraw(*run, a_shape, a_start, a_triangles)) {
				Refusals().Report(std::format("a draw of technique {}: {}", state->currentPixelDescriptor, reason));
				PrecipitationDraws::Keep(std::move(run->owners));
				return false;
			}
			return PrecipitationDraws::Submit(std::move(run));
		}
	}

	void PreflightHooks()
	{
		if (!Enabled())
			return;
		HookSites::RequireKnown("DrawTriShape (the precipitation's draw)", Family::Address::kDrawTriShape.Known());
	}

	void InstallHooks()
	{
		if (!Family::AddDrawTaker("precipitation", &TakeDraw)) {
			logger::warn("[DCLF] precipitation: this runtime has no address for DrawTriShape; the precipitation stays the engine's");
			return;
		}
		Family::AddSetup("precipitation", [] { return PrecipitationDraws::Setup(); });
		logger::info("[DCLF] precipitation: hooks installed (the engine draw families' DrawTriShape taker)");
	}
}
