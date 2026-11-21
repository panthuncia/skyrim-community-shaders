// Engine-side hooks for the menu scenes family. Each batch call runs with its draws latched, DrawTriShape latches a scene draw
// instead of drawing it, and a pass of a shader with no ORG program flushes the latched part. Render thread.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/MenuViews/MenuScenes.h"
#include "Features/DrawcallLimitFix/Features/MenuViews/MenuViews.h"

#include "Features/LightLimitFix.h"
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
		Family::RefusalLog& Refusals()
		{
			static Family::RefusalLog log("[DCLF] menu scenes: the engine draws");
			return log;
		}

		/** @brief Whether the DrawData copy's taker is installed. Only then are the particles' passes the family's. */
		bool& DynamicTakerInstalled()
		{
			static bool installed = false;
			return installed;
		}

		/** @brief The batch being latched, on the render thread. */
		struct Capture
		{
			bool active = false;
			bool failed = false;  // a draw was refused: the latched part ran, the rest of the batch is the engine's
			View view = View::LocalMap;
			std::shared_ptr<Run> run;
			Family::Passes passes;
			// The DrawData copy's draw being latched, which has two streams. Null for a DrawTriShape draw.
			const Family::DynamicDrawData* dynamic = nullptr;
		};

		Capture& GetCapture()
		{
			static Capture capture;
			return capture;
		}

		/** @brief Whether a Utility vertex descriptor is one of UTILITY_ORG_SCENE's techniques. */
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

		/** @brief Runs the latched part of the batch now, before anything the engine draws after it. */
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

		/**
		 * @brief Whether the draw's vertex PerGeometry (b2) is bound or not declared at all.
		 * A skinned depth draw has none and takes its transforms from the bones. An undeclared group copies as zeros.
		 */
		bool HasVertexPerGeometry(Family::Constants::UseId a_use, const RE::BSGraphics::VertexShader* a_vs)
		{
			return Family::ConstantBytes(a_use, Stage::Vertex, 2) != 0 || !a_vs || !a_vs->constantBuffers[2].buffer;
		}

		/** @brief Runs the latched part and leaves the rest of the batch to the engine. Logged once per reason and view. */
		void Refuse(Capture& a_capture, std::string_view a_reason, std::string_view a_detail = {})
		{
			a_capture.failed = true;
			Refusals().Report(std::format("a {} draw: {}", NameOf(a_capture.view), a_reason), a_detail);
			FlushLatched(a_capture);
		}

		/** @brief Maps the draw's colour targets to the view's imports. Returns null when mapped, else why not. */
		const char* MapTargets(View a_view, std::uint32_t a_outputs, Draw& a_draw)
		{
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			if (rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return "a cube face is bound";
			// The shadow batch draws into a slice of the shadow maps, every other batch into kMAIN's depth.
			const bool shadow = a_view == View::MenuShadow;
			const auto depthTarget = shadow ? RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS : RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN;
			if (rss.depthStencil != static_cast<std::uint32_t>(depthTarget))
				return shadow ? "its depth target is not the shadow maps" : "its depth target is not kMAIN's";
			a_draw.depthSlice = shadow ? rss.depthStencilSlice : 0;
			if (a_draw.depthSlice >= kMaxShadowSlices)
				return "a shadow map slice past the batch's";
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

		/** @brief Appends a skinned draw's palettes (VS b9, b10) to the run's latch. False when they do not fit. */
		bool LatchBones(Family::Constants::UseId a_use, Run& a_run, std::uint32_t& a_offset)
		{
			std::array<std::byte, kBonesBytes> bones{};
			const std::span<std::byte> all(bones);
			Family::CopyConstants(a_use, Stage::Vertex, 9, all.subspan(0, kBonesBytes / 2));
			Family::CopyConstants(a_use, Stage::Vertex, 10, all.subspan(kBonesBytes / 2));
			a_offset = a_run.Append(bones);
			return a_offset != ~0u;
		}

		/** @brief Appends the draw's FrameBuffer (b12) to the latch with its matrices transposed. False when unbound or full. */
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

		/** @brief Latches a Utility draw. Returns null when latched, else why the engine must draw it. */
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
			draw.key.descriptor = descriptor;
			draw.key.defineStage = Family::DefineStage::Vertex;
			if (const char* why = MapTargets(a_capture.view, 1, draw))
				return why;
			if (!Family::DescribeStates(draw.key, Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra),
					Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode), Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor)))
				return "a depth-stencil, blend or rasterizer state with no description";
			draw.stencilRef = engine.stencilRef;

			UtilitySceneConstants constants{};
			const auto use = Family::CaptureConstants();
			if (!use.Valid())
				return "the engine constants capture is not active";
			if (!HasVertexPerGeometry(use, vs))
				return "its vertex shader declares a PerGeometry that is not bound";
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

			// The geometry is read by device address. The run holds its leases until the GPU is done.
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

			// Slots the build does not read get a null view and the first sampler, whatever is bound there.
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

			draw.viewport = Family::LatchViewport(engine.depthBias);

			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			run.draws.push_back(std::move(draw));
			return nullptr;
		}

		/**
		 * @brief Writes the pass's strict lights into the PerGeometry point light rows (c15, c22, c29.x).
		 * Light Limit Fix leaves those rows empty, and the scene build has no Light Limit Fix to read its strict lights instead.
		 */
		void LatchPointLights(LightingSceneConstants& a_constants)
		{
			auto& llf = globals::features::lightLimitFix;
			if (!llf.loaded)
				return;
			constexpr std::uint32_t kPositions = 15, kColours = 22, kCount = 29, kMaxLights = 7;
			const auto& strict = llf.strictLightDataTemp;
			std::uint32_t count = 0;
			for (std::uint32_t i = 0; i < strict.NumStrictLights && count < kMaxLights; ++i) {
				const auto& light = strict.StrictLights[i];
				if (light.lightFlags.any(LightLimitFix::LightFlags::Disabled))
					continue;
				const auto& position = light.positionWS.data;
				std::array<float, 4> row{ position.x, position.y, position.z, light.radius };
				std::memcpy(a_constants.psGeometry[kPositions + count], row.data(), sizeof(row));
				row = { light.color.x * light.fade, light.color.y * light.fade, light.color.z * light.fade, 0.0f };
				std::memcpy(a_constants.psGeometry[kColours + count], row.data(), sizeof(row));
				++count;
			}
			if (count)
				a_constants.psGeometry[kCount][0] = static_cast<float>(count);
		}

		/** @brief Latches a Lighting draw. Returns null when latched, else why not. */
		const char* TryLatchLighting(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles, std::string& a_detail)
		{
			auto& run = *a_capture.run;
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* state = globals::state;
			const auto engine = Pipeline::EngineDrawState::Latch();
			Draw draw{};
			draw.shader = Shader::Lighting;
			// VC and SKINNED describe the vertex inputs, and the engine's pixel descriptor does not follow them. Take them from the vertex descriptor.
			using LightingFlags = SIE::ShaderCache::LightingShaderFlags;
			constexpr auto kVertexInputs = static_cast<std::uint32_t>(LightingFlags::VC) | static_cast<std::uint32_t>(LightingFlags::Skinned);
			draw.key.descriptor = (state->modifiedPixelDescriptor & ~kVertexInputs) | (state->modifiedVertexDescriptor & kVertexInputs);
			draw.key.defineStage = Family::DefineStage::Pixel;
			// The deferred stage's fourth target is never bound in a menu scene.
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
			if (!HasVertexPerGeometry(use, vs))
				return "its vertex shader declares a PerGeometry that is not bound";
			Family::CopyRows(use, Stage::Vertex, 0, constants.vsTechnique, kLightingVSTechniqueRows);
			Family::CopyRows(use, Stage::Vertex, 1, constants.vsMaterial, kLightingVSMaterialRows);
			Family::CopyRows(use, Stage::Vertex, 2, constants.vsGeometry, kLightingVSGeometryRows);
			Family::CopyRows(use, Stage::Pixel, 0, constants.psTechnique, kLightingPSTechniqueRows);
			Family::CopyRows(use, Stage::Pixel, 1, constants.psMaterial, kLightingPSMaterialRows);
			Family::CopyRows(use, Stage::Pixel, 2, constants.psGeometry, kLightingPSGeometryRows);
			LatchPointLights(constants);
			constants.alphaTestRef = rss.alphaTestEnabled ? rss.alphaTestRef : 0.0f;
			const auto& permutation = state->permutationData;
			constants.descriptors[0] = permutation.VertexShaderDescriptor;
			constants.descriptors[1] = permutation.PixelShaderDescriptor;
			constants.descriptors[2] = permutation.ExtraShaderDescriptor;
			constants.descriptors[3] = permutation.ExtraFeatureDescriptor;
			constants.effectRadius = permutation.EffectRadius;
			constants.baseTextureIsWorking = permutation.BaseTextureIsWorking;
			constants.renderToUI = permutation.RenderToUI;
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

			auto null = GpuTextures::Get().NullOf(Kind::Texture2D);
			if (null.index == GpuTextures::kInvalid)
				return "no null view";
			constants.nullTexture = null.index;
			if (null.owner)
				owners.push_back(std::move(null.owner));
			for (std::uint32_t t = 0; t < kLightingSlots; ++t) {
				auto* view = LightingSlotRead(draw.key.descriptor, t) ? Unwrap(rss.PSTexture[t]) : nullptr;
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

		/** @brief Latches an Effect draw. Indexed texture coordinates are refused. Returns null when latched, else why not. */
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
			// SKINNED comes from the vertex descriptor, or a skinned draw's vertex stage could be built without its bones.
			constexpr auto kSkinned = static_cast<std::uint32_t>(EffectFlags::Skinned);
			draw.key.descriptor = state->modifiedPixelDescriptor | (state->modifiedVertexDescriptor & kSkinned);
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
			if (!HasVertexPerGeometry(use, vs))
				return "its vertex shader declares a PerGeometry that is not bound";
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

		/** @brief Latches the draw by its shader's kind. Returns null when latched, else why not. */
		const char* TryLatchDraw(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles, std::string& a_detail)
		{
			if (a_capture.run->draws.size() >= kMaxDraws)
				return "more draws than the batch latches";
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* state = globals::state;
			if (!rss.currentVertexShader || !state || !state->currentShader)
				return "no shader bound";
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
		 * @brief One batch's engine call. With the view's switch on, the draws are latched and the anchor draws them here.
		 * Otherwise, or when the batch cannot be taken now, the engine draws it.
		 */
		template <View kView, class Call>
		void RunBatch(Call&& a_call)
		{
			auto& capture = GetCapture();
			if (capture.active || !ViewEnabled(kView) || !FrameAnchors::IsRenderOwner() || !SceneDraws::Available() ||
				!SceneDraws::CanTake(kView)) {
				a_call();
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
			a_call();
			capture.active = false;
			if (!capture.passes.End())
				Refusals().Report(capture.passes.Describe(std::format("{} batch", NameOf(kView))));
			FlushLatched(capture);
		}

		/** @brief A SubmitAccumulator or StartAccumulating batch. They take the same arguments. */
		template <View kView>
		struct SceneBatch
		{
			static void thunk(RE::NiCamera* a_camera, RE::BSShaderAccumulator* a_accumulator, std::uint32_t a_flags)
			{
				RunBatch<kView>([&] { func(a_camera, a_accumulator, a_flags); });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The loading screen model's shadow lights as one batch, each light drawing its own shadow map slice. */
		struct ShadowBatch
		{
			static void thunk(void* a_manager, RE::NiCamera* a_camera)
			{
				RunBatch<View::MenuShadow>([&] { func(a_manager, a_camera); });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Latches a draw during a latched batch. Applies the engine's pending state as SetDirtyStates would.
		 * That way the batch's clears land before the anchor's run and the dirty bits are consumed as before.
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

		/** @brief TakeDraw for DrawDynamicTriShape's DrawData copy, latched as a two-stream draw. */
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

		/** @brief Counts the pass after SetupGeometry. A pass that will not reach DrawTriShape flushes the latched part. */
		void OnProgramSetup(RE::BSRenderPass* a_pass)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return;
			if (!capture.passes.OnSetup(a_pass) && !capture.failed)
				Refuse(capture, "a pass the dispatcher draws through another function than DrawTriShape");
		}

		/** @brief Flushes the latched part before a pass with no ORG program, such as tree LOD, so the engine draws the rest in order. */
		void OnOtherSetup(RE::BSRenderPass*)
		{
			auto& capture = GetCapture();
			if (capture.active && !capture.failed)
				Refuse(capture, "a pass of a shader the family has no program for (DistantTree)");
		}

		bool LocalMapKnown() { return Address::kLocalMapSubmit.Known() && Address::kLocalMapWaterSubmit.Known(); }
		bool MenuKnown() { return Address::kMenu3DSubmit.Known() && Address::kMenu3DDepthSubmit.Known() && Address::kMenu3DShadowRender.Known(); }
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
		case View::MenuShadow:
			return "menu-shadow";
		default:
			return "?";
		}
	}

	std::span<const std::int32_t> TargetsOf(View a_view)
	{
		if (a_view == View::LocalMap || a_view == View::LocalMapFog)
			return kLocalMapTargets;
		if (a_view == View::MenuShadow)
			return {};
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
			HookSites::RequireCall("the loading screen model's shadow lights (UI3DSceneManager::Render's call)", REL::Offset(Address::kMenu3DShadowRender).address());
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
				stl::write_thunk_call<ShadowBatch>(REL::Offset(Address::kMenu3DShadowRender).address());
			} else
				logger::warn("[DCLF] menu scenes: this runtime has no address for a menu model's batches; the menu models stay the engine's");
		}
		Family::WatchSetupGeometry<RE::BSShader::Type::Utility>(RE::VTABLE_BSUtilityShader[0], &OnProgramSetup);
		Family::WatchSetupGeometry<RE::BSShader::Type::Lighting>(RE::VTABLE_BSLightingShader[0], &OnProgramSetup);
		Family::WatchSetupGeometry<RE::BSShader::Type::Effect>(RE::VTABLE_BSEffectShader[0], &OnProgramSetup);
		Family::WatchSetupGeometry<RE::BSShader::Type::DistantTree>(RE::VTABLE_BSDistantTreeShader[0], &OnOtherSetup);
		Family::AddSetup("menu scenes", [] { return SceneDraws::Setup(); });
		logger::info("[DCLF] menu scenes: hooks installed (local map {}, menu models {})", MapEnabled() ? "on" : "off", Menu3DEnabled() ? "on" : "off");
	}
}
