// The menu scenes family's engine side: the batches' calls (the local map's two SubmitAccumulator calls, a menu model's depth
// StartAccumulating and colour SubmitAccumulator), which run with their draws latched and hand them to the anchor's pass; the
// family's DrawTriShape taker, which latches a scene draw in place of drawing it; and its SetupGeometry watchers, which count a
// batch's passes and end the latched part at a pass of a shader the family has no program for. Render thread.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/MenuViews/MenuScenes.h"
#include "Features/DrawcallLimitFix/Features/MenuViews/MenuViews.h"

#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/PublishedResources.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/Logging.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace DCLF::Features::MenuViews::Scenes
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;
		using Stage = Family::Constants::Stage;
		using Flags = SIE::ShaderCache::UtilityShaderFlags;

		constexpr std::array<std::int32_t, 1> kLocalMapTargets{ kLocalMapSwapTarget };
		constexpr std::array<std::int32_t, 4> kMenuTargets{ kMainTarget, kMotionVectorTarget, kNormalTaaMaskTarget, kNormalTaaMaskSwapTarget };

		/** @brief The inherent refusals, each logged once per reason. */
		Util::ReasonLog& Refusals()
		{
			static Util::ReasonLog log("[DCLF] menu scenes: the engine draws", spdlog::level::warn);
			return log;
		}

		/** @brief Whether the DrawData copy's taker is installed (InstallHooks): the particles' passes are then the family's too. */
		bool& DynamicTakerInstalled()
		{
			static bool installed = false;
			return installed;
		}

		/** @brief The batch being latched (render thread). */
		struct Capture
		{
			bool active = false;
			bool failed = false;  // a draw was refused: the latched part ran, the rest of the batch is the engine's
			View view = View::LocalMap;
			std::shared_ptr<Run> run;
			Family::Passes passes;
			// The DrawData copy's draw being latched (TakeDynamicDraw): its geometry is two streams. Null for a DrawTriShape draw.
			const Family::DynamicDrawData* dynamic = nullptr;
		};

		Capture& GetCapture()
		{
			static Capture capture;
			return capture;
		}

		/** @brief Whether a Utility vertex descriptor is one of UTILITY_ORG_SCENE's techniques (Utility.hlsl's #error). */
		bool SceneTechnique(std::uint32_t a_descriptor)
		{
			constexpr auto kScene = static_cast<std::uint32_t>(Flags::RenderBaseTexture) | static_cast<std::uint32_t>(Flags::LocalMapFogOfWar) |
			                        static_cast<std::uint32_t>(Flags::RenderDepth) | static_cast<std::uint32_t>(Flags::RenderShadowmap);
			constexpr auto kNot = static_cast<std::uint32_t>(Flags::RenderShadowmask) | static_cast<std::uint32_t>(Flags::RenderShadowmaskSpot) |
			                      static_cast<std::uint32_t>(Flags::RenderShadowmaskPb) | static_cast<std::uint32_t>(Flags::RenderShadowmaskDpb) |
			                      static_cast<std::uint32_t>(Flags::RenderNormal) | static_cast<std::uint32_t>(Flags::RenderNormalClear) |
			                      static_cast<std::uint32_t>(Flags::DebugShadowSplit) | static_cast<std::uint32_t>(Flags::DebugColor);
			return (a_descriptor & kScene) && !(a_descriptor & kNot);
		}

		/** @brief The latched part of the batch, run now: in the engine's order, before anything the engine draws after it. */
		void FlushLatched(Capture& a_capture)
		{
			auto run = std::move(a_capture.run);
			if (!run)
				return;
			if (run->draws.empty()) {
				SceneDraws::Keep(std::move(run->owners));
				return;
			}
			SceneDraws::Submit(std::move(run));
		}

		/** @brief A refusal: the latched part runs now and the rest of the batch is the engine's; logged once per reason and view. */
		void Refuse(Capture& a_capture, std::string_view a_reason, std::string_view a_detail = {})
		{
			a_capture.failed = true;
			Refusals().Report(std::format("a {} draw: {}", NameOf(a_capture.view), a_reason), a_detail);
			FlushLatched(a_capture);
		}

		/**
		 * @brief The draw's colour targets as SetDirtyStates would bind them, looked up in the view's imports: the first a_outputs render
		 * targets the pixel stage writes, up to the first unbound one. Null when mapped, else why not.
		 */
		const char* MapTargets(View a_view, std::uint32_t a_outputs, Draw& a_draw)
		{
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			if (rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return "a cube face is bound";
			if (rss.depthStencil != static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN))
				return "its depth target is not kMAIN's";
			const auto imports = TargetsOf(a_view);
			a_draw.colourCount = 0;
			for (std::uint32_t i = 0; i < std::min(a_outputs, kMaxColours); ++i) {
				const auto target = static_cast<std::int32_t>(rss.renderTargets[i]);
				if (target < 0)
					break;
				const auto found = std::ranges::find(imports, target);
				if (found == imports.end())
					return "a colour target its view's client does not import";
				const auto index = static_cast<std::uint32_t>(found - imports.begin());
				a_draw.colours[i] = static_cast<std::uint8_t>(index);
				a_draw.key.colourFormats[i] = SceneDraws::ColourFormat(a_view, index);
				if (!a_draw.key.colourFormats[i])
					return "a colour target with no format";
				++a_draw.colourCount;
			}
			a_draw.key.depthFormat = SceneDraws::DepthFormat(a_view);
			return a_draw.key.depthFormat ? nullptr : "no depth import";
		}

		/** @brief A skinned draw's palettes (VS b9, b10) appended to the run's latch; its offset in a_offset. False when they do not fit. */
		bool LatchBones(Family::Constants::UseId a_use, Run& a_run, std::uint32_t& a_offset)
		{
			std::array<std::byte, kBonesBytes> bones{};
			const std::span<std::byte> all(bones);
			Family::CopyConstants(a_use, Stage::Vertex, 9, all.subspan(0, kBonesBytes / 2));
			Family::CopyConstants(a_use, Stage::Vertex, 10, all.subspan(kBonesBytes / 2));
			a_offset = a_run.Append(bones);
			return a_offset != ~0u;
		}

		/**
		 * @brief The draw's FrameBuffer (b12: the camera SetCameraData wrote for it) appended to the run's latch, its matrices transposed
		 * (kFrameMatrices); its offset in a_offset. False when it is not bound or does not fit.
		 */
		bool LatchFrame(Family::Constants::UseId a_use, Run& a_run, std::uint32_t& a_offset)
		{
			const auto stage = Family::ConstantBytes(a_use, Stage::Vertex, 12) ? Stage::Vertex : Stage::Pixel;
			if (!Family::ConstantBytes(a_use, stage, 12))
				return false;
			float rows[kFrameRows][4]{};
			Family::CopyRows(a_use, stage, 12, rows, kFrameRows);
			for (std::uint32_t m = 0; m < kFrameMatrices; ++m) {
				float(*matrix)[4] = rows + m * 4;
				for (std::uint32_t r = 0; r < 4; ++r)
					for (std::uint32_t c = r + 1; c < 4; ++c)
						std::swap(matrix[r][c], matrix[c][r]);
			}
			a_offset = a_run.Append(std::as_bytes(std::span<const float[4]>(rows, kFrameRows)));
			return a_offset != ~0u;
		}

		/**
		 * @brief A Utility draw, latched: the shadow state SetDirtyStates would apply, the constants the Utility shader's Setup functions
		 * wrote (the engine constants capture), the palettes of a skinned one, the slots' textures and samplers, the geometry. Null when
		 * latched; otherwise why the graph cannot draw it (a_detail: what it was about), and the engine draws it.
		 */
		const char* TryLatchUtility(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles, std::string& a_detail)
		{
			auto& run = *a_capture.run;
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* state = globals::state;
			const std::uint32_t descriptor = state->modifiedVertexDescriptor;
			if (!SceneTechnique(descriptor)) {
				a_detail = std::format("vertex descriptor {:#x}", descriptor);
				return "a Utility technique the scene build does not draw";
			}
			const auto engine = Pipeline::EngineDrawState::Latch();
			Draw draw{};
			draw.shader = Shader::Utility;
			// The vertex descriptor CS looks its shader up by (State::ModifyShaderLookup): it holds the technique's bits and the
			// geometry's (TEXTURE, VC, NORMALS, SKINNED, LOD_LANDSCAPE, TREE_ANIM), and its defines serve both stages.
			draw.key.descriptor = descriptor;
			draw.key.defineStage = Family::DefineStage::Vertex;
			if (const char* why = MapTargets(a_capture.view, 1, draw))
				return why;
			if (!Family::DescribeStates(draw.key, Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra),
					Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode), Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor)))
				return "a depth-stencil, blend or rasterizer state with no description";
			draw.stencilRef = engine.stencilRef;

			// The constant groups as the Setup functions wrote them, and the alpha test reference SetDirtyStates would write into b11.
			UtilitySceneConstants constants{};
			const auto use = Family::CaptureConstants();
			if (!use.Valid())
				return "the engine constants capture is not active";
			if (!Family::ConstantBytes(use, Stage::Vertex, 2))
				return "no vertex PerGeometry";
			Family::CopyRows(use, Stage::Vertex, 0, constants.vsTechnique, kVSTechniqueRows);
			Family::CopyRows(use, Stage::Vertex, 1, constants.vsMaterial, kVSMaterialRows);
			Family::CopyRows(use, Stage::Vertex, 2, constants.vsGeometry, kVSGeometryRows);
			Family::CopyRows(use, Stage::Pixel, 1, constants.psMaterial, kPSMaterialRows);
			Family::CopyRows(use, Stage::Pixel, 2, constants.psGeometry, kPSGeometryRows);
			constants.alphaTestRef = rss.alphaTestEnabled ? rss.alphaTestRef : 0.0f;
			if ((descriptor & static_cast<std::uint32_t>(Flags::Skinned)) && !LatchBones(use, run, draw.bonesOffset))
				return "more palettes than the batch's latch holds";
			if (!LatchFrame(use, run, draw.frameOffset))
				return "no FrameBuffer bound, or more than the batch's latch holds";

			draw.program = SceneDraws::UtilityProgram(draw.key, *state->currentShader);
			if (!draw.program)
				return "its ORG program cannot be built";

			// The geometry, read by device address while its leases are held (the run keeps them until the GPU is done).
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

			// The slots the build reads as the shadow state holds them (an empty slot reads a null view of its declared kind) and their
			// samplers. Every other slot reads a null view and the first sampler, whatever is bound there.
			for (std::uint32_t t = 0; t < kUtilitySlots; ++t) {
				const bool read = UtilitySlotRead(t);
				auto* view = read ? Unwrap(rss.PSTexture[t]) : nullptr;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, UtilitySlotKind(t), GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), constants.textures[t], owners, &failure);
				if (slot == Family::SlotResult::Published) {
					const auto id = PublishedResources::ExportedId(view);
					const auto result = SceneDraws::ReadPublished(a_capture.view, id, constants.textures[t]);
					if (result != Family::InputResult::Graph) {
						a_detail = std::format("t{} holds '{}': {}", t, id, Family::ReasonOf(result));
						return "a slot holds a published texture its client does not read";
					}
				} else if (slot != Family::SlotResult::Ok) {
					a_detail = GpuTextures::DescribeFailure(view, t, failure);
					return "a texture it samples could not be imported";
				}
				constants.samplers[t] = read ? Family::SlotSampler(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying(), owners) :
				                               Family::SlotSampler(0, 0, owners);
				if (constants.samplers[t] == GpuTextures::kInvalid)
					return "its sampler modes have no graph sampler";
			}

			draw.latchOffset = run.Append(std::as_bytes(std::span<const UtilitySceneConstants, 1>(&constants, 1)));
			if (draw.latchOffset == ~0u)
				return "more constants than the batch's latch holds";

			// The viewport SetDirtyStates would set, with the depth bias mode's offset taken off its maximum depth.
			draw.viewport = Family::LatchViewport(engine.depthBias);

			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			run.draws.push_back(std::move(draw));
			return nullptr;
		}

		/**
		 * @brief A Lighting draw, latched: the shadow state SetDirtyStates would apply, both stages' constants as the Lighting shader's
		 * Setup functions wrote them, CS's permutation block for the draw (State's SetupGeometry hooks filled it), the palettes of a
		 * skinned one, t0-t15 and s0-s15 as the shadow state holds them, the geometry. Null when latched; otherwise why not.
		 */
		const char* TryLatchLighting(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles, std::string& a_detail)
		{
			auto& run = *a_capture.run;
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* state = globals::state;
			const auto engine = Pipeline::EngineDrawState::Latch();
			Draw draw{};
			draw.shader = Shader::Lighting;
			// The pixel descriptor CS looks its shader up by: it holds the vertex descriptor's bits (the technique, VC, SKINNED,
			// MODELSPACENORMALS) and the pixel stage's flags, so its defines serve both stages.
			draw.key.descriptor = state->modifiedPixelDescriptor;
			draw.key.defineStage = Family::DefineStage::Pixel;
			// The forward stage writes three targets (the colour, the motion vectors, the normals and masks); the deferred one more, which
			// a menu scene does not bind.
			if (const char* why = MapTargets(a_capture.view, kMaxColours, draw))
				return why;
			if (!Family::DescribeStates(draw.key, Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra),
					Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode), Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor)))
				return "a depth-stencil, blend or rasterizer state with no description";
			draw.stencilRef = engine.stencilRef;

			LightingSceneConstants constants{};
			const auto use = Family::CaptureConstants();
			if (!use.Valid())
				return "the engine constants capture is not active";
			if (!Family::ConstantBytes(use, Stage::Vertex, 2))
				return "no vertex PerGeometry";
			Family::CopyRows(use, Stage::Vertex, 0, constants.vsTechnique, kLightingVSTechniqueRows);
			Family::CopyRows(use, Stage::Vertex, 1, constants.vsMaterial, kLightingVSMaterialRows);
			Family::CopyRows(use, Stage::Vertex, 2, constants.vsGeometry, kLightingVSGeometryRows);
			Family::CopyRows(use, Stage::Pixel, 0, constants.psTechnique, kLightingPSTechniqueRows);
			Family::CopyRows(use, Stage::Pixel, 1, constants.psMaterial, kLightingPSMaterialRows);
			Family::CopyRows(use, Stage::Pixel, 2, constants.psGeometry, kLightingPSGeometryRows);
			constants.alphaTestRef = rss.alphaTestEnabled ? rss.alphaTestRef : 0.0f;
			const auto& permutation = state->permutationData;
			constants.descriptors[0] = permutation.VertexShaderDescriptor;
			constants.descriptors[1] = permutation.PixelShaderDescriptor;
			constants.descriptors[2] = permutation.ExtraShaderDescriptor;
			constants.descriptors[3] = permutation.ExtraFeatureDescriptor;
			constants.effectRadius = permutation.EffectRadius;
			constants.baseTextureIsWorking = permutation.BaseTextureIsWorking;
			constants.renderToUI = permutation.RenderToUI;
			// Lighting's vertex flag 1 << 1: SKINNED (Permutation::LightingFlags::Skinned).
			if ((state->modifiedVertexDescriptor & 0x2u) && !LatchBones(use, run, draw.bonesOffset))
				return "more palettes than the batch's latch holds";
			if (!LatchFrame(use, run, draw.frameOffset))
				return "no FrameBuffer bound, or more than the batch's latch holds";

			draw.program = SceneDraws::LightingProgram(draw.key, *state->currentShader);
			if (!draw.program)
				return "its ORG program cannot be built";

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

			// The null 2D view: what the build reads at a register past t15 (the features it turns off).
			auto null = GpuTextures::Get().NullOf(Kind::Texture2D);
			if (null.index == GpuTextures::kInvalid)
				return "no null view";
			constants.nullTexture = null.index;
			if (null.owner)
				owners.push_back(std::move(null.owner));
			// Every slot as the shadow state holds it (a bound view's import, in its own dimension; an empty slot a null 2D view) and
			// its sampler.
			for (std::uint32_t t = 0; t < kLightingSlots; ++t) {
				auto* view = Unwrap(rss.PSTexture[t]);
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, Kind::Texture2D, GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), constants.textures[t], owners, &failure);
				if (slot == Family::SlotResult::Published) {
					const auto id = PublishedResources::ExportedId(view);
					const auto result = SceneDraws::ReadPublished(a_capture.view, id, constants.textures[t]);
					if (result != Family::InputResult::Graph) {
						a_detail = std::format("t{} holds '{}': {}", t, id, Family::ReasonOf(result));
						return "a slot holds a published texture its client does not read";
					}
				} else if (slot != Family::SlotResult::Ok) {
					a_detail = GpuTextures::DescribeFailure(view, t, failure);
					return "a texture it samples could not be imported";
				}
				constants.samplers[t] = Family::SlotSampler(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying(), owners);
				if (constants.samplers[t] == GpuTextures::kInvalid)
					return "its sampler modes have no graph sampler";
			}

			draw.latchOffset = run.Append(std::as_bytes(std::span<const LightingSceneConstants, 1>(&constants, 1)));
			if (draw.latchOffset == ~0u)
				return "more constants than the batch's latch holds";
			draw.viewport = Family::LatchViewport(engine.depthBias);

			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			run.draws.push_back(std::move(draw));
			return nullptr;
		}

		/**
		 * @brief An Effect draw, latched as a Lighting one is: both stages' constants, the permutation block, the palettes of a skinned
		 * one, t0-t4 and s0-s4, the geometry. A draw with indexed texture coordinates (particles' subtextures: b11, not latched) is
		 * refused. Null when latched; otherwise why not.
		 */
		const char* TryLatchEffect(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles, std::string& a_detail)
		{
			auto& run = *a_capture.run;
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* state = globals::state;
			using EffectFlags = SIE::ShaderCache::EffectShaderFlags;
			const bool texCoordIndex = (state->modifiedVertexDescriptor & static_cast<std::uint32_t>(EffectFlags::TexCoordIndex)) != 0;
			const auto engine = Pipeline::EngineDrawState::Latch();
			Draw draw{};
			draw.shader = Shader::Effect;
			// The pixel descriptor holds the vertex descriptor's bits and the pixel stage's, so its defines serve both stages.
			draw.key.descriptor = state->modifiedPixelDescriptor;
			draw.key.defineStage = Family::DefineStage::Pixel;
			if (const char* why = MapTargets(a_capture.view, kMaxColours, draw))
				return why;
			if (!Family::DescribeStates(draw.key, Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra),
					Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode), Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor)))
				return "a depth-stencil, blend or rasterizer state with no description";
			draw.stencilRef = engine.stencilRef;

			EffectSceneConstants constants{};
			const auto use = Family::CaptureConstants();
			if (!use.Valid())
				return "the engine constants capture is not active";
			if (!Family::ConstantBytes(use, Stage::Vertex, 2))
				return "no vertex PerGeometry";
			Family::CopyRows(use, Stage::Vertex, 0, constants.vsTechnique, kEffectVSTechniqueRows);
			Family::CopyRows(use, Stage::Vertex, 1, constants.vsMaterial, kEffectVSMaterialRows);
			Family::CopyRows(use, Stage::Vertex, 2, constants.vsGeometry, kEffectVSGeometryRows);
			Family::CopyRows(use, Stage::Pixel, 0, constants.psTechnique, kEffectPSTechniqueRows);
			Family::CopyRows(use, Stage::Pixel, 1, constants.psMaterial, kEffectPSMaterialRows);
			Family::CopyRows(use, Stage::Pixel, 2, constants.psGeometry, kEffectPSGeometryRows);
			constants.alphaTestRef = rss.alphaTestEnabled ? rss.alphaTestRef : 0.0f;
			const auto& permutation = state->permutationData;
			constants.descriptors[0] = permutation.VertexShaderDescriptor;
			constants.descriptors[1] = permutation.PixelShaderDescriptor;
			constants.descriptors[2] = permutation.ExtraShaderDescriptor;
			constants.descriptors[3] = permutation.ExtraFeatureDescriptor;
			constants.effectRadius = permutation.EffectRadius;
			constants.baseTextureIsWorking = permutation.BaseTextureIsWorking;
			constants.renderToUI = permutation.RenderToUI;
			if ((state->modifiedVertexDescriptor & static_cast<std::uint32_t>(EffectFlags::Skinned)) && !LatchBones(use, run, draw.bonesOffset))
				return "more palettes than the batch's latch holds";
			if (!LatchFrame(use, run, draw.frameOffset))
				return "no FrameBuffer bound, or more than the batch's latch holds";

			draw.program = SceneDraws::EffectProgram(draw.key, *state->currentShader);
			if (!draw.program)
				return "its ORG program cannot be built";

			Family::Owners owners;
			Family::Geometry geometry;
			// A DrawData copy's draw (the particles) pulls its position from stream 1, the dynamic ring.
			const auto latched = a_capture.dynamic ?
			                         Family::LatchDynamicGeometry(a_capture.dynamic, a_start, a_triangles, vs->vertexDesc, engine.topology, geometry, owners, &a_detail) :
			                         Family::LatchGeometry(a_shape, a_start, a_triangles, vs->vertexDesc, engine.topology, geometry, owners, &a_detail);
			if (latched != Family::GeometryResult::Ok)
				return Family::ReasonOf(latched);
			constants.streamRef = geometry.streamRef;
			constants.streamStride = geometry.streamStride;
			constants.vertexRef = geometry.vertexRef;
			constants.indexRef = geometry.indexRef;
			constants.vertexLayout[0] = static_cast<std::uint32_t>(geometry.layout);
			constants.vertexLayout[1] = static_cast<std::uint32_t>(geometry.layout >> 32);
			constants.vertexStride = geometry.stride;
			constants.firstIndex = geometry.firstIndex;
			draw.indexCount = geometry.indexCount;

			for (std::uint32_t t = 0; t < kEffectSlots; ++t) {
				const bool read = t < kEffectSlotsRead;
				auto* view = read ? Unwrap(rss.PSTexture[t]) : nullptr;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, Kind::Texture2D, GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), constants.textures[t], owners, &failure);
				if (slot == Family::SlotResult::Published) {
					const auto id = PublishedResources::ExportedId(view);
					const auto result = SceneDraws::ReadPublished(a_capture.view, id, constants.textures[t]);
					if (result != Family::InputResult::Graph) {
						a_detail = std::format("t{} holds '{}': {}", t, id, Family::ReasonOf(result));
						return "a slot holds a published texture its client does not read";
					}
				} else if (slot != Family::SlotResult::Ok) {
					a_detail = GpuTextures::DescribeFailure(view, t, failure);
					return "a texture it samples could not be imported";
				}
				constants.samplers[t] = read ? Family::SlotSampler(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying(), owners) :
				                               Family::SlotSampler(0, 0, owners);
				if (constants.samplers[t] == GpuTextures::kInvalid)
					return "its sampler modes have no graph sampler";
			}

			// A particle draw's indexed texture coordinates (VS b11: each particle's subtexture rectangle) follow its constants in the latch
			// (Effect.hlsl's ORG build reads the row a vertex names).
			std::vector<std::byte> bytes(sizeof(EffectSceneConstants) + (texCoordIndex ? kEffectIndexedTexCoordBytes : 0));
			std::memcpy(bytes.data(), &constants, sizeof(EffectSceneConstants));
			if (texCoordIndex) {
				float rows[kEffectIndexedTexCoordRows][4]{};
				if (!Family::CopyRows(use, Stage::Vertex, 11, rows, kEffectIndexedTexCoordRows))
					return "its indexed texture coordinates (VS b11) could not be read";
				std::memcpy(bytes.data() + sizeof(EffectSceneConstants), rows, kEffectIndexedTexCoordBytes);
			}
			draw.latchOffset = run.Append(bytes);
			if (draw.latchOffset == ~0u)
				return "more constants than the batch's latch holds";
			draw.viewport = Family::LatchViewport(engine.depthBias);

			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			run.draws.push_back(std::move(draw));
			return nullptr;
		}

		/** @brief The draw the engine is about to make, latched by its shader's kind. Null when latched; otherwise why not. */
		const char* TryLatchDraw(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles, std::string& a_detail)
		{
			if (a_capture.run->draws.size() >= kMaxDraws)
				return "more draws than the batch latches";
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* state = globals::state;
			if (!rss.currentVertexShader || !state || !state->currentShader)
				return "no shader bound";
			// A depth-only Utility draw (the menu model's depth batch) binds no pixel shader: its scene build draws the depth alone.
			if (!rss.currentPixelShader && state->currentShader->shaderType.get() != RE::BSShader::Type::Utility)
				return "no pixel shader bound";
			if (a_capture.dynamic && state->currentShader->shaderType.get() != RE::BSShader::Type::Effect)
				return "a DrawData copy's draw of a shader whose scene build pulls one stream";
			switch (state->currentShader->shaderType.get()) {
			case RE::BSShader::Type::Utility:
				return TryLatchUtility(a_capture, a_shape, a_start, a_triangles, a_detail);
			case RE::BSShader::Type::Lighting:
				return TryLatchLighting(a_capture, a_shape, a_start, a_triangles, a_detail);
			case RE::BSShader::Type::Effect:
				return TryLatchEffect(a_capture, a_shape, a_start, a_triangles, a_detail);
			default:
				return "a shader the family has no program for";
			}
		}

		/**
		 * @brief One batch's engine call (SubmitAccumulator or StartAccumulating, the same arguments): with the view's switch on, the batch
		 * runs with its draws latched, then the anchor's pass draws them at this point. Otherwise, or when the batch cannot be taken now,
		 * the engine draws it.
		 */
		template <View kView>
		struct SceneBatch
		{
			static void thunk(RE::NiCamera* a_camera, RE::BSShaderAccumulator* a_accumulator, std::uint32_t a_flags)
			{
				auto& capture = GetCapture();
				if (capture.active || !ViewEnabled(kView) || !ShellState::Running() || !FrameAnchors::IsRenderOwner() || !SceneDraws::Available() ||
					!SceneDraws::CanTake(kView)) {
					func(a_camera, a_accumulator, a_flags);
					return;
				}
				Family::BeginBatch();
				capture.view = kView;
				capture.run = std::make_shared<Run>();
				capture.run->view = kView;
				capture.failed = false;
				capture.passes.Begin();
				capture.passes.dynamicDraws = DynamicTakerInstalled();
				capture.active = true;
				func(a_camera, a_accumulator, a_flags);
				capture.active = false;
				if (!capture.passes.End())
					Refusals().Report(capture.passes.Describe(std::format("{} batch", NameOf(kView))));
				FlushLatched(capture);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief The family's DrawTriShape taker: during a latched batch, the draw is latched instead. A taken draw then applies the
		 * engine's pending state as the draw function's own SetDirtyStates would: the clears the batch's setup asked for land before the
		 * anchor's run, and the dirty bits are consumed as before.
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
			return true;
		}

		/** @brief TakeDraw for DrawDynamicTriShape's DrawData copy (the particles): latched as a two-stream draw. */
		bool TakeDynamicDraw(const Family::DynamicDrawData* a_data, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return false;
			capture.dynamic = a_data;
			const bool taken = TakeDraw(reinterpret_cast<const Family::TriShape*>(a_data), a_start, a_triangles);
			capture.dynamic = nullptr;
			return taken;
		}

		/** @brief After the SetupGeometry of a shader the family draws: counts the pass; one that will not reach DrawTriShape ends the latched part. */
		void OnProgramSetup(RE::BSRenderPass* a_pass)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return;
			if (!capture.passes.OnSetup(a_pass) && !capture.failed)
				Refuse(capture, "a pass the dispatcher draws through another function than DrawTriShape");
		}

		/**
		 * @brief After the SetupGeometry of a shader the family has no program for (tree LOD): the latched part ends before the engine
		 * draws the pass, so the rest of the batch is the engine's in order.
		 */
		void OnOtherSetup(RE::BSRenderPass*)
		{
			auto& capture = GetCapture();
			if (capture.active && !capture.failed)
				Refuse(capture, "a pass of a shader the family has no program for (DistantTree)");
		}

		bool LocalMapKnown() { return Address::kLocalMapSubmit.Known() && Address::kLocalMapWaterSubmit.Known(); }
		bool MenuKnown() { return Address::kMenu3DSubmit.Known() && Address::kMenu3DDepthSubmit.Known(); }
	}

	const char* NameOf(View a_view)
	{
		switch (a_view) {
		case View::LocalMap:
			return "local-map";
		case View::LocalMapFog:
			return "local-map-fog";
		case View::MenuDepth:
			return "menu-depth";
		case View::MenuColour:
			return "menu-colour";
		default:
			return "?";
		}
	}

	std::span<const std::int32_t> TargetsOf(View a_view)
	{
		if (a_view == View::LocalMap || a_view == View::LocalMapFog)
			return kLocalMapTargets;
		return kMenuTargets;
	}

	bool Enabled()
	{
		return MapEnabled();
	}

	bool ViewEnabled(View a_view)
	{
		return a_view == View::LocalMap || a_view == View::LocalMapFog ? MapEnabled() : Menu3DEnabled();
	}

	bool AnyEnabled()
	{
		return MapEnabled() || Menu3DEnabled();
	}

	void PreflightHooks()
	{
		if (!AnyEnabled())
			return;
		HookSites::RequireKnown("the menu scenes' batches",
			Family::Address::kDrawTriShape.Known() && (!MapEnabled() || LocalMapKnown()) && (!Menu3DEnabled() || MenuKnown()));
		if (MapEnabled() && LocalMapKnown()) {
			HookSites::RequireCall("the local map's land and objects (LocalMapRenderer::Render's first SubmitAccumulator)", REL::Offset(Address::kLocalMapSubmit).address());
			HookSites::RequireCall("the local map's fog of war (LocalMapRenderer::Render's second SubmitAccumulator)", REL::Offset(Address::kLocalMapWaterSubmit).address());
		}
		if (Menu3DEnabled() && MenuKnown()) {
			HookSites::RequireCall("a menu model's depth pass (UI3DSceneManager::Render's StartAccumulating)", REL::Offset(Address::kMenu3DDepthSubmit).address());
			HookSites::RequireCall("a menu model's colour pass (UI3DSceneManager::Render's SubmitAccumulator)", REL::Offset(Address::kMenu3DSubmit).address());
		}
	}

	void InstallHooks()
	{
		if (!AnyEnabled())
			return;
		if (!Family::AddDrawTaker("menu scenes", &TakeDraw)) {
			logger::warn("[DCLF] menu scenes: no DrawTriShape detour; the menu scenes stay the engine's");
			return;
		}
		// The particles (a loading-screen model's) draw through the DrawData copy; without it they stay the engine's.
		DynamicTakerInstalled() = Family::AddDynamicDrawTaker("menu scenes", &TakeDynamicDraw);
		if (MapEnabled()) {
			if (LocalMapKnown()) {
				stl::write_thunk_call<SceneBatch<View::LocalMap>>(REL::Offset(Address::kLocalMapSubmit).address());
				stl::write_thunk_call<SceneBatch<View::LocalMapFog>>(REL::Offset(Address::kLocalMapWaterSubmit).address());
			} else
				logger::warn("[DCLF] menu scenes: this runtime has no address for the local map's batches; the local map stays the engine's");
		}
		if (Menu3DEnabled()) {
			if (MenuKnown()) {
				stl::write_thunk_call<SceneBatch<View::MenuDepth>>(REL::Offset(Address::kMenu3DDepthSubmit).address());
				stl::write_thunk_call<SceneBatch<View::MenuColour>>(REL::Offset(Address::kMenu3DSubmit).address());
			} else
				logger::warn("[DCLF] menu scenes: this runtime has no address for a menu model's batches; the menu models stay the engine's");
		}
		Family::WatchSetupGeometry<RE::BSShader::Type::Utility>(RE::VTABLE_BSUtilityShader[0], &OnProgramSetup);
		Family::WatchSetupGeometry<RE::BSShader::Type::Lighting>(RE::VTABLE_BSLightingShader[0], &OnProgramSetup);
		Family::WatchSetupGeometry<RE::BSShader::Type::Effect>(RE::VTABLE_BSEffectShader[0], &OnProgramSetup);
		Family::WatchSetupGeometry<RE::BSShader::Type::DistantTree>(RE::VTABLE_BSDistantTreeShader[0], &OnOtherSetup);
		// The clients are made on the main thread once the graph runs (the main menu), before any world frame.
		Family::AddSetup("menu scenes", [] { return SceneDraws::Setup(); });
		logger::info("[DCLF] menu scenes: hooks installed (local map {}, menu models {})", MapEnabled() ? "on" : "off", Menu3DEnabled() ? "on" : "off");
	}
}
