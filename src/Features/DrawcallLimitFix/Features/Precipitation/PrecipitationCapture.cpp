// The precipitation family's engine side: its DrawTriShape taker, which latches a precipitation draw in place of drawing it and runs the
// anchor's pass for it at once, at its place in the grouped alphas. Render thread.
//
// A draw the family cannot take is drawn by the engine with nobody vouching for it: under the flush latch it is skipped and the residue
// census counts it, since every such case is a defect of the family.

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

		// Light Limit Fix's strict lights (PS b3: LightLimitFix::StrictLightDataCB, bound once a frame by CS).
		constexpr std::uint32_t kStrictLightSlot = 3;

		/** @brief The inherent refusals, each logged once per reason. */
		Util::ReasonLog& Refusals()
		{
			static Util::ReasonLog log("[DCLF] precipitation: the engine draws", spdlog::level::warn);
			return log;
		}

		/** @brief Deferred's directional shadow record as last uploaded: a frame with no sun shadow light keeps it, as t98 keeps its buffer. */
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

		/**
		 * @brief The headers' inputs (the water's registers: Particle.hlsl's push block is laid out as Water.hlsl's) and t80, each a heap
		 * index: an import, a published id's index, or a null view. Null when resolved; otherwise why the draw stays the engine's.
		 */
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
					// A D3D11 buffer cannot be imported (Light Limit Fix's D3D11 path): the precipitation stays the engine's.
					Refusals().Report(std::format("t{} is a D3D11 buffer the graph cannot read", reg), "turn on its feature's ORG path");
					return "a header is a D3D11 buffer";
				}
				if (result != Family::HeaderResult::Ok) {
					Refusals().Report(std::format("t{} could not be imported", reg), GpuTextures::DescribeFailure(view, reg, failure));
					return "a header could not be imported";
				}
				std::uint32_t index = header.index;
				if (!header.publishedId.empty()) {
					// A published header: read by its id's index, which the client declares.
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
		 * @brief The draw the engine is about to make, latched into a_run: the shadow state SetDirtyStates would apply (Rain Refraction's
		 * blend state for refractive rain, which it binds itself), the constants the Setup functions wrote (the engine constants capture),
		 * the slots, the headers, the geometry. Null when latched; otherwise why the graph cannot draw it, and the engine draws it.
		 */
		const char* TryLatchDraw(Run& a_run, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* ps = rss.currentPixelShader;
			const auto* state = globals::state;
			if (!vs || !ps || !state || !state->currentShader)
				return "no bound shader";
			// The main view only: kMAIN's depth bound, no cube face.
			const auto main = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
			if (rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return "a cube face's draw";
			if (rss.depthStencil != main)
				return "a depth target other than kMAIN's";

			const auto engine = Pipeline::EngineDrawState::Latch();
			Draw draw{};
			// The colour targets bound, looked up in the client's imports.
			const auto& engineTargets = globals::game::renderer->GetRuntimeData().renderTargets;
			for (std::uint32_t i = 0; i < kMaxColours; ++i) {
				const auto target = static_cast<std::int32_t>(rss.renderTargets[i]);
				if (target < 0)
					break;
				const auto found = std::ranges::find(kTargets, target);
				if (found == kTargets.end() || (i == 0 && target != kTargets[0]))
					return "a colour target its client does not import";
				draw.colours[i] = static_cast<std::uint8_t>(found - kTargets.begin());
				draw.key.colourFormats[i] = FormatOf(engineTargets[target].RTV);
				if (!draw.key.colourFormats[i])
					return "a colour target with no format";
				draw.colourCount = i + 1;
			}
			if (draw.colourCount == 0)
				return "no colour target";
			if (static_cast<std::int32_t>(rss.renderTargets[kMaxColours]) >= 0)
				return "more colour targets than the pixel stage writes";
			const auto& depths = globals::game::renderer->GetDepthStencilData().depthStencils;
			draw.key.depthFormat = FormatOf(depths[main].views[0]);
			if (!draw.key.depthFormat)
				return "no depth format";

			// The blend state: the engine's, or Rain Refraction's for its refractive rain (it binds its own on the context: ParticleShaderHacks).
			ID3D11BlendState* blend = Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra);
			if (auto* refractive = globals::features::rainRefraction.RefractiveRainBlendState())
				blend = refractive;
			if (!Family::DescribeStates(draw.key, blend, Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode),
					Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor)))
				return "a depth-stencil, blend or rasterizer state with no description";
			draw.stencilRef = engine.stencilRef;
			// The descriptor CS looks its shader up by (State::ModifyShaderLookup), which ShaderCompilationTask compiled the stages for.
			draw.key.descriptor = state->modifiedPixelDescriptor;
			draw.key.defineStage = Family::DefineStage::Pixel;

			// The constant groups as the Setup functions wrote them, Light Limit Fix's strict lights and the directional shadow record.
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
			// What the vertex stage reads of CS's shared constants (Particle.hlsl, ParticleOrgDraw): from their CPU sources.
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
			// CS's permutation block for the draw: the pixel stage branches on its descriptors.
			const auto& permutation = state->permutationData;
			constants.descriptors[0] = permutation.VertexShaderDescriptor;
			constants.descriptors[1] = permutation.PixelShaderDescriptor;
			constants.descriptors[2] = permutation.ExtraShaderDescriptor;
			constants.descriptors[3] = permutation.ExtraFeatureDescriptor;
			constants.effectRadius = permutation.EffectRadius;

			draw.program = PrecipitationDraws::Program(draw.key, *state->currentShader);
			if (!draw.program)
				return "its ORG program cannot be built";

			// The geometry, read by device address while its leases are held (the run keeps them until the GPU is done).
			Family::Owners owners;
			Family::Geometry geometry;
			std::string geometryDetail;
			if (const auto result = Family::LatchGeometry(a_shape, a_start, a_triangles, vs->vertexDesc, engine.topology, geometry, owners, &geometryDetail);
				result != Family::GeometryResult::Ok) {
				if (!geometryDetail.empty())
					Refusals().Report(std::format("a draw whose geometry is refused: {}", Family::ReasonOf(result)), geometryDetail);
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

			// The four slots as the shadow state holds them (an empty slot reads zero, as on D3D11) and their samplers. A slot the
			// techniques never sample reads a null view and the first sampler.
			for (std::uint32_t t = 0; t < kMaterialSlots; ++t) {
				const bool read = SlotRead(t);
				auto* view = read ? Unwrap(rss.PSTexture[t]) : nullptr;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, Kind::Texture2D, GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), constants.textures[t], owners, &failure);
				if (slot == Family::SlotResult::Published) {
					// An engine target a graph pass writes (the precipitation occlusion map, the underwater mask): read by its id, which
					// the client declares, so the graph orders the write before the draw.
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

			// The viewport SetDirtyStates would set, with the depth bias mode's offset taken off its maximum depth.
			draw.viewport = Family::LatchViewport(engine.depthBias);

			for (auto& owner : owners)
				if (owner)
					a_run.owners.push_back(std::move(owner));
			a_run.draws.push_back(std::move(draw));
			return nullptr;
		}

		/**
		 * @brief The family's DrawTriShape taker: a BSParticleShader draw of an ENVCUBE technique is latched and drawn by the anchor's pass
		 * now. Any other draw is not the family's.
		 */
		bool TakeDraw(const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			const auto* state = globals::state;
			if (!state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Particle ||
				!PrecipitationTechnique(state->currentPixelDescriptor))
				return false;
			if (!ShellState::Running() || !FrameAnchors::IsRenderOwner() || !PrecipitationDraws::Available())
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
			// A run that did not submit is logged; the engine draws the draw then.
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
		// The client is made on the main thread once the graph runs (the main menu), before any world frame.
		Family::AddSetup("precipitation", [] { return PrecipitationDraws::Setup(); });
		logger::info("[DCLF] precipitation: hooks installed (the engine draw families' DrawTriShape taker)");
	}
}
