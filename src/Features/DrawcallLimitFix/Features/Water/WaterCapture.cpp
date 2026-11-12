// The water family's engine side: the water batches' hooks (the mask, geometry group 10 and the surface), which let the engine's batch
// run with its draws latched and hand them to the anchor's pass; the family's DrawTriShape taker, which latches a water draw in place of
// drawing it; and its BSWaterShader::SetupGeometry watcher, which counts a batch's passes. Render thread.
//
// A batch the family cannot take, or the rest of one after a refused draw, is drawn by the engine with nobody vouching for it: under the
// flush latch it is skipped and the residue census counts it, since every such case is a defect of the family.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"

#include "Deferred.h"
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

namespace DCLF::Features::Water
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;
		using Stage = Family::Constants::Stage;

		static_assert(sizeof(Deferred::DirectionalShadowLightData) == kDirectionalShadowBytes, "t98's record");

		// The most draws one batch latches; past it a draw is the engine's (WaterDraws' latch has room for this many).
		constexpr std::size_t kMaxLatched = 64;
		// Light Limit Fix's strict lights (PS b3: LightLimitFix::StrictLightDataCB, bound once a frame by CS).
		constexpr std::uint32_t kStrictLightSlot = 3;

		const char* NameOf(View a_view) { return a_view == View::Mask ? "mask" : "surface"; }

		/** @brief The inherent refusals, each logged once per reason. */
		Util::ReasonLog& Refusals()
		{
			static Util::ReasonLog log("[DCLF] water: the engine draws", spdlog::level::warn);
			return log;
		}

		/** @brief The batch being latched (render thread). */
		// The shadow record's alignment pads the struct, as intended.
#pragma warning(push)
#pragma warning(disable: 4324)
		struct Capture
		{
			bool active = false;
			bool failed = false;  // a draw was refused: the latched part ran, the rest of the batch is the engine's
			View view = View::Surface;
			std::shared_ptr<Run> run;
			// The batch's header indices (CurrentShape), for a run begun again after another shader's draw (Interrupt).
			std::array<std::uint32_t, kHeaderRegisters.size()> headerIndices{};
			std::uint32_t depthFormat = 0;
			Family::Passes passes;
			std::uint32_t lastTechnique = 0;  // the last pass's technique (State::currentPixelDescriptor >> 11 & 0xF)
			// Deferred's directional shadow record as last uploaded: the frames with no sun shadow light keep it, as t98 keeps its buffer.
			Deferred::DirectionalShadowLightData directionalShadow{};
		};
#pragma warning(pop)

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

		std::uint32_t TechniqueOf(std::uint32_t a_descriptor) { return (a_descriptor >> 11) & 0xF; }

		/** @brief The latched part of the batch, run now: in the engine's order, before anything the engine draws after it. */
		void FlushLatched(Capture& a_capture)
		{
			auto run = std::move(a_capture.run);
			if (!run)
				return;
			if (run->draws.empty()) {
				WaterDraws::Keep(std::move(run->owners));
				return;
			}
			WaterDraws::Submit(std::move(run));
		}

		/** @brief A refusal: the latched part runs now and the rest of the batch is the engine's; logged once per reason, technique and view. */
		void Refuse(Capture& a_capture, std::string_view a_reason, std::uint32_t a_technique)
		{
			a_capture.failed = true;
			Refusals().Report(std::format("a {} draw of technique {}: {}", NameOf(a_capture.view), a_technique, a_reason));
			FlushLatched(a_capture);
		}

		/**
		 * @brief A draw of another shader inside a latched batch: not the family's. Geometry group 10 holds the Lighting cross-fade copies
		 * as well as water, and DCLF draws those or vouches for them. The water latched so far runs first, so the engine's order holds,
		 * and the water draws after it go into a new run with the batch's headers.
		 */
		void Interrupt(Capture& a_capture)
		{
			if (!a_capture.run || a_capture.run->draws.empty())
				return;
			FlushLatched(a_capture);
			a_capture.run = std::make_shared<Run>();
			a_capture.run->view = a_capture.view;
			a_capture.run->headerIndices = a_capture.headerIndices;
		}

		/**
		 * @brief The batch's view and its headers' inputs: the main camera's accumulator with kMAIN's depth bound (the only view the
		 * family takes), and each header's heap index (an import, a published id's index, or a null view) into a_run. A view that reads no
		 * header latches null views. False, logged once per reason, for anything else (another accumulator, a cube face, a header buffer
		 * the graph cannot read, a published header its client cannot read), which stays the engine's.
		 */
		bool CurrentShape(const void* a_accumulator, View a_view, Run& a_run, Capture& a_capture)
		{
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			static REL::Relocation<void**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
			if (a_accumulator != *mainAccumulator || rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return false;
			const auto main = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
			if (rss.depthStencil != main) {
				Refusals().Report(std::format("a main camera {} batch with depth target {} bound, not kMAIN's", NameOf(a_view), rss.depthStencil));
				return false;
			}
			const auto& depths = globals::game::renderer->GetDepthStencilData().depthStencils;
			a_capture.depthFormat = FormatOf(depths[main].views[0]);
			if (!a_capture.depthFormat)
				return false;

			for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h) {
				// The mask's pixel stage reads no header: whatever CS binds there is no input of its draws.
				if (!HeadersRead(a_view)) {
					auto binding = GpuTextures::Get().NullOf(kHeaderKinds[h]);
					if (binding.index == GpuTextures::kInvalid)
						return false;
					a_run.headerIndices[h] = binding.index;
					if (binding.owner)
						a_run.owners.push_back(std::move(binding.owner));
					continue;
				}
				Family::Header header;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				ID3D11ShaderResourceView* view = nullptr;
				const auto result = Family::ResolveHeader(kHeaderRegisters[h], kHeaderKinds[h], GpuTextures::Tag(GpuTextures::Source::FamilyHeader, static_cast<std::uint32_t>(h)),
					header, a_run.owners, &failure, &view);
				if (result == Family::HeaderResult::Buffer) {
					// A D3D11 buffer cannot be imported (Light Limit Fix's D3D11 path): the water stays the engine's.
					Refusals().Report(std::format("the water: t{} is a D3D11 buffer the graph cannot read", kHeaderRegisters[h]), "turn on its feature's ORG path");
					return false;
				}
				if (result != Family::HeaderResult::Ok) {
					Refusals().Report(std::format("a {} batch whose t{} could not be imported", NameOf(a_view), kHeaderRegisters[h]),
						GpuTextures::DescribeFailure(view, kHeaderRegisters[h], failure));
					return false;
				}
				a_run.headerIndices[h] = header.index;
				if (header.publishedId.empty())
					continue;
				// A published header: read by its id's index, which the view's client declares.
				const auto read = WaterDraws::ReadPublished(a_view, header.publishedId, a_run.headerIndices[h]);
				if (read != Family::InputResult::Graph) {
					Refusals().Report(std::format("a {} batch whose t{} holds '{}'", NameOf(a_view), kHeaderRegisters[h], header.publishedId), Family::ReasonOf(read));
					return false;
				}
			}
			return true;
		}

		/**
		 * @brief The draw the engine is about to make, latched: the shadow state SetDirtyStates would apply, the constants the shaders'
		 * Setup functions wrote (the engine constants capture), the textures and samplers, the geometry. Null when latched; otherwise why
		 * the graph cannot draw it, and the engine draws it.
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
			if (!vs || !ps || !state || !state->currentShader)
				return "no bound shader";

			const auto engine = Pipeline::EngineDrawState::Latch();
			Draw draw{};
			// The colour targets the view's pixel stage writes, looked up in its client's imports.
			const auto& targets = a_capture.view == View::Mask ? kMaskTargets : kSurfaceTargets;
			const auto& engineTargets = globals::game::renderer->GetRuntimeData().renderTargets;
			for (std::uint32_t i = 0; i < TargetsOf(a_capture.view); ++i) {
				const auto target = static_cast<std::int32_t>(rss.renderTargets[i]);
				const auto found = std::ranges::find(targets, target);
				if (target < 0 || found == targets.end())
					return "a colour target its view's client does not import";
				draw.colours[i] = static_cast<std::uint8_t>(found - targets.begin());
				draw.key.colourFormats[i] = FormatOf(engineTargets[target].RTV);
				if (!draw.key.colourFormats[i])
					return "a colour target with no format";
			}
			draw.key.depthFormat = a_capture.depthFormat;

			// The depth-stencil state: the water's per-type stencil bits, tested or written with the shadow state's reference.
			if (!Family::DescribeStates(draw.key, Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra),
					Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode), Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor)))
				return "a depth-stencil, blend or rasterizer state with no description";
			draw.stencilRef = engine.stencilRef;
			// The vertex descriptor CS looks its shader up by (State::ModifyShaderLookup), which ShaderCompilationTask compiled the stages for;
			// its defines serve both stages: the vertex stage reads Wading, which State strips from the pixel descriptor (the pixel stage reads
			// it at run time, as Water.hlsl's pixel build undefines it).
			draw.key.descriptor = state->modifiedVertexDescriptor;
			draw.key.defineStage = Family::DefineStage::Vertex;

			// The six constant groups as the Setup functions wrote them and Light Limit Fix's strict lights (the engine constants capture), and
			// the directional shadow record Deferred uploads to t98, from the same CPU data.
			auto& constants = draw.latch.constants;
			const auto use = Family::CaptureConstants();
			if (!use.Valid())
				return "the engine constants capture is not active";
			// The vertex stage's matrices and PerGeometry are every technique's: a draw without them is not one the engine made.
			if (!Family::ConstantBytes(use, Stage::Vertex, 2))
				return "no vertex PerGeometry";
			Family::CopyRows(use, Stage::Vertex, 0, constants.vsTechnique, kGroupRows[0]);
			Family::CopyRows(use, Stage::Vertex, 1, constants.vsMaterial, kGroupRows[1]);
			Family::CopyRows(use, Stage::Vertex, 2, constants.vsGeometry, kGroupRows[2]);
			Family::CopyRows(use, Stage::Pixel, 0, constants.psTechnique, kGroupRows[3]);
			Family::CopyRows(use, Stage::Pixel, 1, constants.psMaterial, kGroupRows[4]);
			Family::CopyRows(use, Stage::Pixel, 2, constants.psGeometry, kGroupRows[5]);
			Family::CopyConstants(use, Stage::Pixel, kStrictLightSlot, draw.latch.strictLights);
			if (globals::deferred)
				globals::deferred->GetDirectionalShadowLightData(a_capture.directionalShadow);
			std::memcpy(draw.latch.directionalShadow, &a_capture.directionalShadow, kDirectionalShadowBytes);
			// CS's permutation block for the draw (State's SetupGeometry hooks filled it): the pixel stage branches on its descriptors.
			const auto& permutation = state->permutationData;
			constants.descriptors[0] = permutation.VertexShaderDescriptor;
			constants.descriptors[1] = permutation.PixelShaderDescriptor;
			constants.descriptors[2] = permutation.ExtraShaderDescriptor;
			constants.descriptors[3] = permutation.ExtraFeatureDescriptor;
			constants.effectRadius = permutation.EffectRadius;

			draw.program = WaterDraws::Program(draw.key, *state->currentShader);
			if (!draw.program)
				return "its ORG program cannot be built";

			// The geometry, read by device address while its leases are held (the run keeps them until the GPU is done).
			Family::Owners owners;
			Family::Geometry geometry;
			std::string geometryDetail;
			if (const auto result = Family::LatchGeometry(a_shape, a_start, a_triangles, vs->vertexDesc, engine.topology, geometry, owners, &geometryDetail);
				result != Family::GeometryResult::Ok) {
				if (!geometryDetail.empty())
					Refusals().Report(std::format("a draw of technique {} whose geometry is refused: {}", TechniqueOf(state->currentPixelDescriptor), Family::ReasonOf(result)),
						geometryDetail);
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

			// The twelve texture slots as the shadow state holds them (an empty slot reads zero, as on D3D11) and every slot's sampler. A
			// slot the view never samples reads a null view and the first sampler, whatever the engine left bound there.
			for (std::uint32_t t = 0; t < kMaterialSlots; ++t) {
				const bool read = SlotRead(a_capture.view, t);
				auto* view = read ? Unwrap(rss.PSTexture[t]) : nullptr;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, MaterialKind(t), GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), constants.textures[t], owners, &failure);
				if (slot == Family::SlotResult::Published) {
					// An engine target a graph pass writes (the refraction copy, the SSR targets): read by its id, which the view's client
					// declares, so the graph orders the write before the water.
					const auto id = PublishedResources::ExportedId(view);
					const auto result = WaterDraws::ReadPublished(a_capture.view, id, constants.textures[t]);
					if (result != Family::InputResult::Graph) {
						Refusals().Report(std::format("a {} draw's t{} holds '{}'", NameOf(a_capture.view), t, id), Family::ReasonOf(result));
						return "a texture slot holds a published graph texture its client cannot read";
					}
				} else if (slot != Family::SlotResult::Ok) {
					Refusals().Report(std::format("a {} draw's texture t{} could not be imported", NameOf(a_capture.view), t), GpuTextures::DescribeFailure(view, t, failure));
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
		 * @brief A water batch: for the main camera's accumulator, the batch runs with its draws latched, then the anchor's pass draws them
		 * at this point. kSite tells the hooked calls apart (each keeps its own original): the mask, the surface, and geometry group 10's
		 * batch just before the surface's, whose water passes (hint 10: a water shape's fade, drawn into kRAW_WATER or kMAIN) are the
		 * surface view's too.
		 */
		template <View kView, std::uint32_t kSite>
		struct WaterBatch
		{
			static void thunk(void* a_accumulator, std::uint32_t a_first, std::uint32_t a_last, std::uint32_t a_flags, std::int32_t a_group)
			{
				auto& capture = GetCapture();
				// A batch inside a latched one: its draws reach TakeDraw as the outer batch's. A view CS_ORG_WATER_PARTS leaves out, or a
				// frame the family cannot run: the engine draws the batch.
				if (capture.active || !TakesView(kView) || !ShellState::Running() || !FrameAnchors::IsRenderOwner() || !WaterDraws::Available()) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				Family::BeginBatch();
				auto run = std::make_shared<Run>();
				run->view = kView;
				// The view's client and an anchor that can run now, then the batch's shape: otherwise the engine draws it (logged once each).
				if (!WaterDraws::CanTake(kView) || !CurrentShape(a_accumulator, kView, *run, capture)) {
					WaterDraws::Keep(std::move(run->owners));
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				capture.view = kView;
				capture.headerIndices = run->headerIndices;
				capture.run = std::move(run);
				capture.failed = false;
				capture.passes.Begin();
				capture.active = true;
				func(a_accumulator, a_first, a_last, a_flags, a_group);
				capture.active = false;
				if (!capture.passes.End())
					Refusals().Report(capture.passes.Describe(std::format("{} water", NameOf(kView))));
				FlushLatched(capture);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The hooked RenderBatches calls, in the frame's order. */
		enum Site : std::uint32_t
		{
			kMaskSite,
			kGroup10Site,
			kSurfaceSite,
		};

		/** @brief The family's DrawTriShape taker: during a latched water batch, the draw is latched instead. */
		bool TakeDraw(const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return false;
			const auto* bound = globals::state ? globals::state->currentShader : nullptr;
			if (!bound || bound->shaderType.get() != RE::BSShader::Type::Water) {
				Interrupt(capture);
				return false;
			}
			capture.passes.OnDraw();
			if (capture.failed || !capture.run)
				return false;
			if (const char* reason = TryLatchDraw(capture, a_shape, a_start, a_triangles)) {
				const auto* state = globals::state;
				Refuse(capture, reason, state ? TechniqueOf(state->currentPixelDescriptor) : 0xFF);
				return false;
			}
			return true;
		}

		/**
		 * @brief After BSWaterShader::SetupGeometry (chained after Unified Water's, Light Limit Fix's, the cubemap fix's and the frame
		 * annotations'): counts the pass; one that will not reach DrawTriShape ends the latched part before the engine draws it.
		 */
		void OnSetupGeometry(RE::BSRenderPass* a_pass)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return;
			if (const auto* state = globals::state)
				capture.lastTechnique = TechniqueOf(state->currentPixelDescriptor);
			if (!capture.passes.OnSetup(a_pass) && !capture.failed)
				Refuse(capture, "a pass the dispatcher draws through another function than DrawTriShape", capture.lastTechnique);
		}
	}

	void PreflightHooks()
	{
		if (!Enabled())
			return;
		const bool known = Address::kMaskBatch.Known() && Address::kSurfaceBatch.Known() && Family::Address::kDrawTriShape.Known();
		HookSites::RequireKnown("the water batches", known);
		if (!known)
			return;
		HookSites::RequireCall("the water mask batch (FinishAccumulating's RenderBatches of the Stencil technique)", REL::Offset(Address::kMaskBatch).address());
		HookSites::RequireCall("the water batch (FinishAccumulatingPostResolveDepth's RenderBatches of the water techniques)",
			REL::Offset(Address::kSurfaceBatch).address());
		if (Address::kGroup10Batch.Known())
			HookSites::RequireCall("geometry group 10's batch (FinishAccumulatingPostResolveDepth, before the water)", REL::Offset(Address::kGroup10Batch).address());
	}

	void InstallHooks()
	{
		if (!Address::kMaskBatch.Known() || !Address::kSurfaceBatch.Known() || !Family::AddDrawTaker("water", &TakeDraw)) {
			logger::warn("[DCLF] water: this runtime has no address for the water batches; the water stays the engine's");
			return;
		}
		stl::write_thunk_call<WaterBatch<View::Mask, kMaskSite>>(REL::Offset(Address::kMaskBatch).address());
		stl::write_thunk_call<WaterBatch<View::Surface, kSurfaceSite>>(REL::Offset(Address::kSurfaceBatch).address());
		if (Address::kGroup10Batch.Known())
			stl::write_thunk_call<WaterBatch<View::Surface, kGroup10Site>>(REL::Offset(Address::kGroup10Batch).address());
		Family::WatchSetupGeometry<RE::BSShader::Type::Water>(RE::VTABLE_BSWaterShader[0], &OnSetupGeometry);
		// Both views' clients are made on the main thread once the graph runs (the main menu), before any world frame.
		Family::AddSetup("water", [] { return WaterDraws::Setup(); });
		logger::info("[DCLF] water: hooks installed (the water mask, geometry group 10 and water batches, BSWaterShader::SetupGeometry)");
	}
}
