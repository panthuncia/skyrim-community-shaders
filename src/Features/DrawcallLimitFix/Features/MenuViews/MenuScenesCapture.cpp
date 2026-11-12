// The menu scenes family's engine side: the local map's two SubmitAccumulator calls, which run with their draws latched and hand
// them to the anchor's pass; the family's DrawTriShape taker, which latches a scene draw in place of drawing it; and its
// SetupGeometry watchers, which count a batch's Utility passes and end the latched part at a pass of a shader the family has no
// program for. Render thread.

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

#include <format>
#include <string>

namespace DCLF::Features::MenuViews::Scenes
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;
		using Stage = Family::Constants::Stage;
		using Flags = SIE::ShaderCache::UtilityShaderFlags;

		/** @brief The inherent refusals, each logged once per reason. */
		Util::ReasonLog& Refusals()
		{
			static Util::ReasonLog log("[DCLF] menu scenes: the engine draws", spdlog::level::warn);
			return log;
		}

		/** @brief The batch being latched (render thread). */
		struct Capture
		{
			bool active = false;
			bool failed = false;  // a draw was refused: the latched part ran, the rest of the batch is the engine's
			View view = View::LocalMap;
			std::shared_ptr<Run> run;
			Family::Passes passes;
		};

		Capture& GetCapture()
		{
			static Capture capture;
			return capture;
		}

		/** @brief Whether a Utility vertex descriptor is one of UTILITY_ORG_SCENE's techniques (Utility.hlsl's #error). */
		bool SceneTechnique(std::uint32_t a_descriptor)
		{
			constexpr auto kScene = static_cast<std::uint32_t>(Flags::RenderBaseTexture) | static_cast<std::uint32_t>(Flags::LocalMapFogOfWar);
			constexpr auto kNot = static_cast<std::uint32_t>(Flags::Skinned) | static_cast<std::uint32_t>(Flags::RenderDepth) |
			                      static_cast<std::uint32_t>(Flags::RenderShadowmap) | static_cast<std::uint32_t>(Flags::RenderShadowmask) |
			                      static_cast<std::uint32_t>(Flags::RenderShadowmaskSpot) | static_cast<std::uint32_t>(Flags::RenderShadowmaskPb) |
			                      static_cast<std::uint32_t>(Flags::RenderShadowmaskDpb);
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
		 * @brief The draw the engine is about to make, latched: the shadow state SetDirtyStates would apply, the constants the Utility
		 * shader's Setup functions wrote (the engine constants capture), the slots' textures and samplers, the geometry. Null when latched;
		 * otherwise why the graph cannot draw it (a_detail: what it was about), and the engine draws it.
		 */
		const char* TryLatchDraw(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles, std::string& a_detail)
		{
			auto& run = *a_capture.run;
			if (run.draws.size() >= kMaxDraws)
				return "more draws than the batch latches";
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* ps = rss.currentPixelShader;
			const auto* state = globals::state;
			if (!vs || !ps || !state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Utility)
				return "the bound shader is not BSUtilityShader (the family has no program for it)";
			if (!SceneTechnique(state->modifiedVertexDescriptor)) {
				a_detail = std::format("vertex descriptor {:#x}", state->modifiedVertexDescriptor);
				return "a Utility technique other than the base texture and the fog of war";
			}
			// LocalMapRenderer::Render binds kLOCAL_MAP_SWAP as RT0 and kMAIN's depth for both batches. A target the frame left bound at RT1
			// or later is not written: the pixel stage has one output.
			if (static_cast<std::uint32_t>(rss.renderTargets[0]) != kLocalMapSwapTarget ||
				rss.depthStencil != static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN) || rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return "its targets are not kLOCAL_MAP_SWAP and kMAIN's depth";

			const auto engine = Pipeline::EngineDrawState::Latch();
			Draw draw{};
			// The vertex descriptor CS looks its shader up by (State::ModifyShaderLookup): it holds the technique's bits and the geometry's
			// (TEXTURE, VC, NORMALS, LOD_LANDSCAPE, TREE_ANIM), and its defines serve both stages.
			draw.key.descriptor = state->modifiedVertexDescriptor;
			draw.key.defineStage = Family::DefineStage::Vertex;
			if (!Family::DescribeStates(draw.key, Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra),
					Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode), Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor)))
				return "a depth-stencil, blend or rasterizer state with no description";
			draw.key.colourFormats[0] = SceneDraws::ColourFormat(a_capture.view);
			draw.key.depthFormat = SceneDraws::DepthFormat(a_capture.view);
			if (!draw.key.colourFormats[0] || !draw.key.depthFormat)
				return "no imported target";
			draw.stencilRef = engine.stencilRef;

			// The constant groups as the Setup functions wrote them, and the alpha test reference SetDirtyStates would write into b11.
			auto& constants = draw.constants;
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

			draw.program = SceneDraws::Program(draw.key, *state->currentShader);
			if (!draw.program)
				return "its ORG program cannot be built";

			// The geometry, read by device address while its leases are held (the run keeps them until the GPU is done).
			Family::Owners owners;
			Family::Geometry geometry;
			if (const auto result = Family::LatchGeometry(a_shape, a_start, a_triangles, vs->vertexDesc, engine.topology, geometry, owners, &a_detail);
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

			// The slots the build reads as the shadow state holds them (an empty slot reads a null view of its declared kind) and their
			// samplers. Every other slot reads a null view and the first sampler, whatever is bound there.
			for (std::uint32_t t = 0; t < kSlots; ++t) {
				const bool read = SlotRead(t);
				auto* view = read ? Unwrap(rss.PSTexture[t]) : nullptr;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, SlotKind(t), GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), constants.textures[t], owners, &failure);
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

			// The viewport SetDirtyStates would set, with the depth bias mode's offset taken off its maximum depth.
			draw.viewport = Family::LatchViewport(engine.depthBias);

			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			run.draws.push_back(std::move(draw));
			return nullptr;
		}

		/**
		 * @brief One of the local map's SubmitAccumulator calls: with the family on, the batch runs with its draws latched, then the
		 * anchor's pass draws them at this point. Otherwise, or when the batch cannot be taken now, the engine draws it, vouched for.
		 */
		template <View kView>
		struct SceneSubmit
		{
			static void thunk(RE::NiCamera* a_camera, RE::BSShaderAccumulator* a_accumulator, std::uint32_t a_flags)
			{
				auto& capture = GetCapture();
				if (capture.active) {
					func(a_camera, a_accumulator, a_flags);
					return;
				}
				if (!Enabled() || !ShellState::Running() || !FrameAnchors::IsRenderOwner() || !SceneDraws::Available() || !SceneDraws::CanTake(kView)) {
					func(a_camera, a_accumulator, a_flags);
					return;
				}
				Family::BeginBatch();
				capture.view = kView;
				capture.run = std::make_shared<Run>();
				capture.run->view = kView;
				capture.failed = false;
				capture.passes.Begin();
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
		 * @brief The family's DrawTriShape taker: during a latched batch, the draw is latched instead. A taken draw then applies the engine's
		 * pending state as the draw function's own SetDirtyStates would: the clears of kLOCAL_MAP_SWAP and kMAIN's depth the render asked
		 * for land before the anchor's run, and the dirty bits are consumed as before.
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

		/** @brief After BSUtilityShader::SetupGeometry: counts the pass; one that will not reach DrawTriShape ends the latched part. */
		void OnUtilitySetup(RE::BSRenderPass* a_pass)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return;
			if (!capture.passes.OnSetup(a_pass) && !capture.failed)
				Refuse(capture, "a pass the dispatcher draws through another function than DrawTriShape");
		}

		/**
		 * @brief After the SetupGeometry of a shader the family has no program for (Lighting: the landscape's passes; the effects; tree
		 * LOD): the latched part ends before the engine draws the pass, so the rest of the batch is the engine's in order.
		 */
		void OnOtherSetup(RE::BSRenderPass*)
		{
			auto& capture = GetCapture();
			if (capture.active && !capture.failed)
				Refuse(capture, "a pass of a shader the family has no program for (Lighting, Effect or DistantTree)");
		}
	}

	const char* NameOf(View a_view)
	{
		return a_view == View::LocalMapFog ? "local-map-fog" : "local-map";
	}

	bool Enabled()
	{
		return MapEnabled();
	}

	void PreflightHooks()
	{
		if (!Enabled() && !Family::Flush::LatchRequested())
			return;
		const bool known = Address::kLocalMapSubmit.Known() && Address::kLocalMapWaterSubmit.Known() && Family::Address::kDrawTriShape.Known();
		HookSites::RequireKnown("the local map's batches", known);
		if (!known)
			return;
		HookSites::RequireCall("the local map's land and objects (LocalMapRenderer::Render's first SubmitAccumulator)", REL::Offset(Address::kLocalMapSubmit).address());
		HookSites::RequireCall("the local map's fog of war (LocalMapRenderer::Render's second SubmitAccumulator)", REL::Offset(Address::kLocalMapWaterSubmit).address());
	}

	void InstallHooks()
	{
		// Under the flush latch the batches are installed whatever CS_ORG_MAP says: off, they are the engine's, vouched for.
		if (!Enabled() && !Family::Flush::LatchRequested())
			return;
		if (!Address::kLocalMapSubmit.Known() || !Address::kLocalMapWaterSubmit.Known()) {
			logger::warn("[DCLF] menu scenes: this runtime has no address for the local map's batches; the local map stays the engine's");
			return;
		}
		if (Enabled() && !Family::AddDrawTaker("menu scenes", &TakeDraw)) {
			logger::warn("[DCLF] menu scenes: no DrawTriShape detour; the local map stays the engine's");
			return;
		}
		stl::write_thunk_call<SceneSubmit<View::LocalMap>>(REL::Offset(Address::kLocalMapSubmit).address());
		stl::write_thunk_call<SceneSubmit<View::LocalMapFog>>(REL::Offset(Address::kLocalMapWaterSubmit).address());
		if (!Enabled())
			return;
		Family::WatchSetupGeometry<RE::BSShader::Type::Utility>(RE::VTABLE_BSUtilityShader[0], &OnUtilitySetup);
		Family::WatchSetupGeometry<RE::BSShader::Type::Lighting>(RE::VTABLE_BSLightingShader[0], &OnOtherSetup);
		Family::WatchSetupGeometry<RE::BSShader::Type::Effect>(RE::VTABLE_BSEffectShader[0], &OnOtherSetup);
		Family::WatchSetupGeometry<RE::BSShader::Type::DistantTree>(RE::VTABLE_BSDistantTreeShader[0], &OnOtherSetup);
		// Both clients are made on the main thread once the graph runs (the main menu), before any world frame.
		Family::AddSetup("menu scenes", [] { return SceneDraws::Setup(); });
		logger::info("[DCLF] menu scenes: hooks installed (the local map's land and objects and its fog of war)");
	}
}
