// Engine-side hooks for the water family. The water batches run with their draws latched, DrawTriShape latches a water draw
// instead of drawing it, and SetupGeometry counts a batch's passes. Render thread.
// A batch the family cannot take reaches the engine with nobody vouching for it. Under the flush latch it is skipped.
// Each such case is a family defect, and its reason is logged once.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/Water/Draw/WaterDraw.h"
#include "Features/DrawcallLimitFix/Features/Water/Port/WaterParity.h"
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

		// The most draws one run latches. A longer batch runs what it has and continues in a new run, so no draw is refused for the count.
		constexpr std::size_t kMaxLatched = 64;
		// Light Limit Fix's strict lights at PS b3, bound once a frame by CS.
		constexpr std::uint32_t kStrictLightSlot = 3;

		const char* NameOf(View a_view) { return a_view == View::Mask ? "mask" : "surface"; }

		/** @brief The inherent refusals, each logged once per reason. */
		Family::RefusalLog& Refusals()
		{
			static Family::RefusalLog log("[DCLF] water: the engine draws");
			return log;
		}

		/**
		 * @brief A material slot as the run's last latched draw resolved it. A run holds every owner its draws named, so a later draw of
		 * the same run with the same view and sampler modes reuses the indices without resolving or holding them again.
		 */
		struct SlotMemo
		{
			ID3D11ShaderResourceView* view = nullptr;
			std::uint32_t address = 0, filter = 0;
			std::uint32_t texture = GpuTextures::kInvalid, sampler = GpuTextures::kInvalid;
			bool valid = false;
		};

		/** @brief The batch being latched, on the render thread. */
		// The shadow record's alignment pads the struct, as intended.
#pragma warning(push)
#pragma warning(disable: 4324)
		struct Capture
		{
			bool active = false;
			bool failed = false;  // a draw was refused: the latched part ran, the rest of the batch is the engine's
			View view = View::Surface;
			std::shared_ptr<Run> run;
			// Kept so a run restarted after another shader's draw (Interrupt) has the same headers.
			std::array<std::uint32_t, kHeaderRegisters.size()> headerIndices{};
			Integrations::PhysicalSky::OrgHandles physSky{};
			std::uint32_t depthFormat = 0;
			Family::Passes passes;
			std::uint32_t lastTechnique = 0;  // the last pass's technique (State::currentPixelDescriptor >> 11 & 0xF)
			const RE::BSRenderPass* lastPass = nullptr;  // the pass whose SetupGeometry ran last, for the water port's parity observer
			// Frames with no sun shadow light keep the last record, as t98 keeps its buffer.
			Deferred::DirectionalShadowLightData directionalShadow{};
			// The current run's slots. Cleared whenever the run is handed off, so it never names owners another run holds.
			std::array<SlotMemo, kMaterialSlots> slotMemo{};
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

		std::uint32_t TechniqueOf(std::uint32_t a_descriptor) { return (a_descriptor >> 11) & 0xF; }

		/** @brief Runs the latched part of the batch now, before anything the engine draws after it. */
		void FlushLatched(Capture& a_capture)
		{
			a_capture.slotMemo = {};
			auto run = std::move(a_capture.run);
			if (!run)
				return;
			if (run->draws.empty()) {
				WaterDraws::Keep(std::move(run->owners));
				return;
			}
			WaterDraws::Submit(std::move(run));
		}

		/** @brief Runs the latched part and leaves the rest of the batch to the engine. Logged once per reason, technique and view. */
		void Refuse(Capture& a_capture, std::string_view a_reason, std::uint32_t a_technique)
		{
			a_capture.failed = true;
			Refusals().Report(std::format("a {} draw of technique {}: {}", NameOf(a_capture.view), a_technique, a_reason));
			FlushLatched(a_capture);
		}

		/**
		 * @brief Handles another shader's draw inside a latched batch, such as group 10's Lighting cross-fade copies.
		 * The water latched so far runs first to keep engine order, and later water draws go into a new run.
		 */
		void Interrupt(Capture& a_capture)
		{
			if (!a_capture.run || a_capture.run->draws.empty())
				return;
			FlushLatched(a_capture);
			a_capture.run = std::make_shared<Run>();
			a_capture.run->view = a_capture.view;
			a_capture.run->headerIndices = a_capture.headerIndices;
			a_capture.run->physSky = a_capture.physSky;
		}

		/**
		 * @brief Reads the batch's view and header inputs. Only the main camera's accumulator with kMAIN's depth is taken.
		 * Returns false for anything else, which stays the engine's.
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
					// A D3D11 buffer, as on Light Limit Fix's D3D11 path, cannot be imported, so the water stays the engine's.
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
				const auto read = WaterDraws::ReadPublished(a_view, header.publishedId, a_run.headerIndices[h]);
				if (read != Family::InputResult::Graph) {
					Refusals().Report(std::format("a {} batch whose t{} holds '{}'", NameOf(a_view), kHeaderRegisters[h], header.publishedId), Family::ReasonOf(read));
					return false;
				}
			}
			// Physical Sky's resources, which the pixel stages read through the draw's latch. A view that reads no headers reads none of them.
			if (!HeadersRead(a_view))
				return true;
			const auto readPhysSky = [a_view](std::string_view a_id, std::uint32_t& a_index, std::string& a_why) {
				const auto result = WaterDraws::ReadPublished(a_view, a_id, a_index);
				a_why = Family::ReasonOf(result);
				return result == Family::InputResult::Graph;
			};
			std::string why;
			if (!Integrations::PhysicalSky::ResolveOrgHandles(a_run.physSky, a_run.owners, readPhysSky, why)) {
				Refusals().Report(std::format("a {} batch", NameOf(a_view)), why);
				return false;
			}
			return true;
		}

		/** @brief Latches the draw the engine is about to make. Returns null when latched, else the reason it was refused. */
		const char* TryLatchDraw(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			DCLF_ZONE("CS.DCLF.Water.TryLatchDraw");
			if (a_capture.run->draws.size() >= kMaxLatched)
				Interrupt(a_capture);
			auto& run = *a_capture.run;
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* ps = rss.currentPixelShader;
			const auto* state = globals::state;
			if (!vs || !ps || !state || !state->currentShader)
				return "no bound shader";

			const auto engine = Pipeline::EngineDrawState::Latch();
			Draw draw{};
			{
				DCLF_ZONE("CS.DCLF.Water.Setup");
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

				if (!Family::DescribeStates(draw.key, Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra),
						Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode), Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor)))
					return "a depth-stencil, blend or rasterizer state with no description";
			}
			draw.stencilRef = engine.stencilRef;
			// The vertex stage reads Wading, which State strips from the pixel descriptor, so key on the vertex descriptor.
			draw.key.descriptor = state->modifiedVertexDescriptor;
			draw.key.defineStage = Family::DefineStage::Vertex;

			auto& constants = draw.latch.constants;
			{
				DCLF_ZONE("CS.DCLF.Water.LatchConstants");
				const auto use = Family::CaptureConstants();
				if (!use.Valid())
					return "the engine constants capture is not active";
				// A draw without the vertex matrices and PerGeometry is not one the engine made.
				if (!Family::ConstantBytes(use, Stage::Vertex, 2))
					return "no vertex PerGeometry";
				Family::CopyRows(use, Stage::Vertex, 0, constants.vsTechnique, kGroupRows[0]);
				Family::CopyRows(use, Stage::Vertex, 1, constants.vsMaterial, kGroupRows[1]);
				Family::CopyRows(use, Stage::Vertex, 2, constants.vsGeometry, kGroupRows[2]);
				Family::CopyRows(use, Stage::Pixel, 0, constants.psTechnique, kGroupRows[3]);
				Family::CopyRows(use, Stage::Pixel, 1, constants.psMaterial, kGroupRows[4]);
				Family::CopyRows(use, Stage::Pixel, 2, constants.psGeometry, kGroupRows[5]);
				Family::CopyConstants(use, Stage::Pixel, kStrictLightSlot, draw.latch.strictLights);
				std::memcpy(draw.latch.directionalShadow, &a_capture.directionalShadow, kDirectionalShadowBytes);
			}
			const auto& permutation = state->permutationData;
			constants.descriptors[0] = permutation.VertexShaderDescriptor;
			constants.descriptors[1] = permutation.PixelShaderDescriptor;
			constants.descriptors[2] = permutation.ExtraShaderDescriptor;
			constants.descriptors[3] = permutation.ExtraFeatureDescriptor;
			constants.physSky = run.physSky;
			constants.effectRadius = permutation.EffectRadius;

			draw.program = WaterDraws::Program(draw.key, *state->currentShader);
			if (!draw.program)
				return "its ORG program cannot be built";

			// The geometry is read by device address. The run holds its leases until the GPU is done.
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
			constants.vertexRef = geometry.vertexRef;
			constants.indexRef = geometry.indexRef;
			constants.vertexLayout[0] = static_cast<std::uint32_t>(geometry.layout);
			constants.vertexLayout[1] = static_cast<std::uint32_t>(geometry.layout >> 32);
			constants.vertexStride = geometry.stride;
			constants.firstIndex = geometry.firstIndex;
			draw.indexCount = geometry.indexCount;

			// An empty slot reads zero, as on D3D11. A slot the view never samples gets a null view and the first sampler.
			// Committed to the capture only once the draw is appended, since a refused draw's owners never reach the run.
			auto memo = a_capture.slotMemo;
			for (std::uint32_t t = 0; t < kMaterialSlots; ++t) {
				const bool read = SlotRead(a_capture.view, t);
				auto* view = read ? Unwrap(rss.PSTexture[t]) : nullptr;
				const std::uint32_t address = read ? rss.PSTextureAddressMode[t].underlying() : 0u;
				const std::uint32_t filter = read ? rss.PSTextureFilterMode[t].underlying() : 0u;
				auto& known = memo[t];
				if (known.valid && known.view == view && known.address == address && known.filter == filter) {
					constants.textures[t] = known.texture;
					constants.samplers[t] = known.sampler;
					continue;
				}
				DCLF_ZONE("CS.DCLF.Water.Slot");
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, MaterialKind(t), GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), constants.textures[t], owners, &failure);
				if (slot == Family::SlotResult::Published) {
					// An engine target a graph pass writes is read by published id, so the graph orders the write before the water.
					const auto id = PublishedResources::ExportedId(view);
					const auto result = WaterDraws::ReadPublished(a_capture.view, id, constants.textures[t]);
					if (result != Family::InputResult::Graph) {
						Refusals().Report(std::format("a {} draw's t{} holds '{}'", NameOf(a_capture.view), t, id),
							result == Family::InputResult::NoView ? std::format("{} ({})", Family::ReasonOf(result), PublishedResources::DescribeIndex(id)) :
																	Family::ReasonOf(result));
						return "a texture slot holds a published graph texture its client cannot read";
					}
				} else if (slot != Family::SlotResult::Ok) {
					Refusals().Report(std::format("a {} draw's texture t{} could not be imported", NameOf(a_capture.view), t), GpuTextures::DescribeFailure(view, t, failure));
					return "a texture it samples could not be imported";
				}
				constants.samplers[t] = Family::SlotSampler(address, filter, owners);
				if (constants.samplers[t] == GpuTextures::kInvalid)
					return "its sampler modes have no graph sampler";
				known = { view, address, filter, constants.textures[t], constants.samplers[t], true };
			}

			draw.viewport = Family::LatchViewport(engine.depthBias);

			{
				DCLF_ZONE("CS.DCLF.Water.Append");
				for (auto& owner : owners)
					if (owner)
						run.owners.push_back(std::move(owner));
				run.draws.push_back(std::move(draw));
			}
			a_capture.slotMemo = memo;
			if (Port::ParityEnabled())
				Port::ObserveDraw(run.draws.back().latch.constants, a_capture.lastPass, a_shape, a_capture.view);
			return nullptr;
		}

		enum Site : std::uint32_t
		{
			kMaskSite,
			kGroup10Site,
			kSurfaceSite,
		};

		/**
		 * @brief A water batch hook. The draws are latched, then the anchor's pass draws them here.
		 * kSite tells apart the mask, the surface, and the geometry group 10 batch just before the surface.
		 * The mask and surface batches first submit DCLF's water epoch for what it claims. The engine's batch then draws the rest.
		 */
		template <View kView, std::uint32_t kSite>
		struct WaterBatch
		{
			static void thunk(void* a_accumulator, std::uint32_t a_first, std::uint32_t a_last, std::uint32_t a_flags, std::int32_t a_group)
			{
				auto& capture = GetCapture();
				if constexpr (kSite != kGroup10Site)
					if (!capture.active)
						(void)Indirect::ReachBatch(kView, a_accumulator);
				// A batch nested in a latched one feeds the outer batch. Otherwise the engine draws batches the family cannot run.
				if (capture.active || !FrameAnchors::IsRenderOwner() || !WaterDraws::Available()) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				Family::BeginBatch();
				auto run = std::make_shared<Run>();
				run->view = kView;
				if (!CurrentShape(a_accumulator, kView, *run, capture) || !WaterDraws::CanTake(kView)) {
					WaterDraws::Keep(std::move(run->owners));
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				capture.view = kView;
				// The sun's shadow record is the frame's, so it is read once per batch rather than per draw.
				if (globals::deferred)
					globals::deferred->GetDirectionalShadowLightData(capture.directionalShadow);
				capture.headerIndices = run->headerIndices;
				capture.physSky = run->physSky;
				capture.run = std::move(run);
				capture.failed = false;
				capture.lastPass = nullptr;
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

		/** @brief Latches a draw during a latched water batch instead of drawing it. */
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

		/** @brief Counts the pass after BSWaterShader::SetupGeometry. A pass that will not reach DrawTriShape flushes first. */
		void OnSetupGeometry(RE::BSRenderPass* a_pass)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return;
			if (const auto* state = globals::state)
				capture.lastTechnique = TechniqueOf(state->currentPixelDescriptor);
			capture.lastPass = a_pass;
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

	bool InstallHooks()
	{
		if (!Address::kMaskBatch.Known() || !Address::kSurfaceBatch.Known() || !Family::AddDrawTaker("water", &TakeDraw)) {
			logger::warn("[DCLF] water: this runtime has no address for the water batches; the water stays the engine's");
			return false;
		}
		stl::write_thunk_call<WaterBatch<View::Mask, kMaskSite>>(REL::Offset(Address::kMaskBatch).address());
		stl::write_thunk_call<WaterBatch<View::Surface, kSurfaceSite>>(REL::Offset(Address::kSurfaceBatch).address());
		if (Address::kGroup10Batch.Known())
			stl::write_thunk_call<WaterBatch<View::Surface, kGroup10Site>>(REL::Offset(Address::kGroup10Batch).address());
		Family::WatchSetupGeometry<RE::BSShader::Type::Water>(RE::VTABLE_BSWaterShader[0], &OnSetupGeometry);
		Family::AddSetup("water", [] { return WaterDraws::Setup(); });
		logger::info("[DCLF] water: hooks installed (the water mask, geometry group 10 and water batches, BSWaterShader::SetupGeometry)");
		return true;
	}
}
