// The water family's engine side: the two water batches' hooks, which let the engine's batch run with its draws latched and hand them
// to the anchor's pass; the family's DrawTriShape taker, which latches a water draw in place of drawing it; and its
// BSWaterShader::SetupGeometry watcher, which has ConstantMirror watch the constant buffers the water's draws read. Render thread.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/Capture/ConstantMirror.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"

#include "Features/LightLimitFix.h"
#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/PublishedResources.h"
#include "State.h"
#include "Utils/Logging.h"

#include <rhi_helpers.h>

#include <algorithm>
#include <format>

namespace DCLF::Features::Water
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		// The most draws one batch latches; past it a draw is the engine's (WaterDraws' latch has room for this many).
		constexpr std::size_t kMaxLatched = 64;
		// Batches a view's probes run before its client is made: its programs and ConstantMirror's copies are asked for first.
		constexpr std::uint32_t kSettleBatches = 30;

		const char* NameOf(View a_view) { return a_view == View::Mask ? "mask" : "surface"; }

		/** @brief The refusals and fallbacks, each logged once per reason. */
		Util::ReasonLog& Refusals()
		{
			static Util::ReasonLog log("[DCLF] water: the engine draws", spdlog::level::warn);
			return log;
		}

		/**
		 * @brief The batch being latched (render thread).
		 *
		 * A view's batches are the graph's only after a probe: a batch the engine draws while every draw is checked as the graph would
		 * take it (its program built, its constants mirrored, its textures and buffers imported). A probe with every draw latchable
		 * makes the view's next batches the graph's; a draw a later batch cannot latch makes that batch's remaining draws the engine's
		 * and the view probes again. So nothing is withheld until its program exists.
		 */
		struct Capture
		{
			bool active = false;
			bool probe = false;                                               // the engine draws the batch; the draws are only checked
			bool failed = false;                                              // a draw of the batch could not be latched: the rest are the engine's
			bool ready = false;                                               // the view's client is in the built graph: its published inputs are read
			std::array<bool, static_cast<std::size_t>(View::Count)> clean{};  // the view's last probe latched every draw
			std::array<std::uint32_t, static_cast<std::size_t>(View::Count)> settled{};
			View view = View::Surface;
			std::shared_ptr<Run> run;
			std::uint32_t depthFormat = 0;
			std::uint64_t fallbacks = 0;      // draws the engine drew during a latched batch
			Family::Passes passes;
			std::uint32_t lastTechnique = 0;  // the last pass's technique (State::currentPixelDescriptor >> 11 & 0xF)
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

		std::uint32_t TechniqueOf(std::uint32_t a_descriptor) { return (a_descriptor >> 11) & 0xF; }

		/** @brief The published ids a batch's headers export, by header (empty: none), read once the view's client is ready. */
		using PublishedHeaders = std::array<std::string, kHeaderRegisters.size()>;

		/**
		 * @brief The batch's view and its headers' inputs: the main camera's accumulator with kMAIN's depth bound (the only view the
		 * family takes), the headers' heap indices (a_run) and the ids of those that are published exports (a_published, resolved
		 * once the view's client is ready: ReadHeaders). A view that reads no header latches null views. False for anything else
		 * (another accumulator, a cube face, a header buffer the graph cannot read), which stays the engine's.
		 */
		bool CurrentShape(const void* a_accumulator, View a_view, Shape& a_shape, Run& a_run, PublishedHeaders& a_published, Capture& a_capture)
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

			a_shape.view = a_view;
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
				a_published[h] = std::move(header.publishedId);
				a_run.headerIndices[h] = header.index;
			}
			return true;
		}

		/**
		 * @brief The published headers of a batch whose view's client is ready: each read by its id's index (WaterDraws::ReadPublished).
		 * False, logged once per id and reason, when the client cannot read one yet: the batch is the engine's.
		 */
		bool ReadHeaders(View a_view, const PublishedHeaders& a_published, Run& a_run)
		{
			for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h) {
				if (a_published[h].empty())
					continue;
				const auto result = WaterDraws::ReadPublished(a_view, a_published[h], a_run.headerIndices[h]);
				if (result != Family::InputResult::Graph) {
					Refusals().Report(std::format("a {} batch whose t{} holds '{}'", NameOf(a_view), kHeaderRegisters[h], a_published[h]), Family::ReasonOf(result));
					return false;
				}
			}
			return true;
		}

		/** @brief The buffer behind the view bound at PS t98 (Deferred's directional shadow record), or null. Not kept. */
		ID3D11Buffer* DirectionalShadowBuffer()
		{
			ID3D11ShaderResourceView* view = nullptr;
			globals::d3d::context->PSGetShaderResources(kDirectionalShadowRegister, 1, &view);
			if (!view)
				return nullptr;
			winrt::com_ptr<ID3D11Resource> resource;
			view->GetResource(resource.put());
			view->Release();
			const auto buffer = resource.try_as<ID3D11Buffer>();
			// The binding keeps it alive for the draw.
			return buffer.get();
		}

		/** @brief Light Limit Fix's strict light buffer (PS b3), or null without the feature. */
		ID3D11Buffer* StrictLightBuffer()
		{
			auto& lightLimitFix = globals::features::lightLimitFix;
			return lightLimitFix.loaded && lightLimitFix.strictLightDataCB ? lightLimitFix.strictLightDataCB->CB() : nullptr;
		}

		/**
		 * @brief The draw the engine is about to make, latched: the shadow state SetDirtyStates would apply, the constants the shaders'
		 * Setup functions wrote (ConstantMirror), the textures and samplers, the geometry. Null when latched (or, in a probe, when it could
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
			if (!vs || !ps || !state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Water)
				return "the bound shader is not BSWaterShader";

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
			draw.key.depthStencil = Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode);
			draw.key.blend = Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra);
			draw.key.raster = Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor);
			draw.stencilRef = engine.stencilRef;
			if (!draw.key.depthStencil || !draw.key.blend || !draw.key.raster)
				return "no depth-stencil, blend or rasterizer state object";
			// The vertex descriptor's defines serve both stages: the vertex stage reads Wading, which State strips from the pixel descriptor
			// (the pixel stage reads it at run time, as Water.hlsl's pixel build undefines it).
			draw.key.descriptor = state->currentVertexDescriptor;
			draw.key.defineStage = Family::DefineStage::Vertex;

			// The six constant groups as the Setup functions wrote them, Light Limit Fix's strict lights and the directional shadow record.
			auto& constants = draw.latch.constants;
			if (!Family::CopyRows(vs->constantBuffers[0].buffer, constants.vsTechnique, kGroupRows[0]) ||
				!Family::CopyRows(vs->constantBuffers[1].buffer, constants.vsMaterial, kGroupRows[1]) ||
				!Family::CopyRows(vs->constantBuffers[2].buffer, constants.vsGeometry, kGroupRows[2]) ||
				!Family::CopyRows(ps->constantBuffers[0].buffer, constants.psTechnique, kGroupRows[3]) ||
				!Family::CopyRows(ps->constantBuffers[1].buffer, constants.psMaterial, kGroupRows[4]) ||
				!Family::CopyRows(ps->constantBuffers[2].buffer, constants.psGeometry, kGroupRows[5]))
				return "ConstantMirror has no copy of its constants yet";
			// The vertex stage's matrices and PerGeometry are every technique's: a draw without them is not one the engine made.
			if (!vs->constantBuffers[2].buffer)
				return "no vertex PerGeometry";
			if (!Family::CopyConstants(StrictLightBuffer(), draw.latch.strictLights) || !Family::CopyConstants(DirectionalShadowBuffer(), draw.latch.directionalShadow))
				return "ConstantMirror has no copy of its strict lights or shadow record yet";
			// CS's permutation block for the draw (State's SetupGeometry hooks filled it): the pixel stage branches on its descriptors.
			const auto& permutation = state->permutationData;
			constants.descriptors[0] = permutation.VertexShaderDescriptor;
			constants.descriptors[1] = permutation.PixelShaderDescriptor;
			constants.descriptors[2] = permutation.ExtraShaderDescriptor;
			constants.descriptors[3] = permutation.ExtraFeatureDescriptor;
			constants.effectRadius = permutation.EffectRadius;

			Family::ProgramState programState{};
			draw.program = WaterDraws::Program(draw.key, *state->currentShader, programState);
			if (!draw.program)
				return programState == Family::ProgramState::Building ? "its ORG program is still building" : "its ORG program cannot be built";

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
					// declares, so the graph orders the write before the water. Before the client is ready, a probe only learns programs.
					const auto id = PublishedResources::ExportedId(view);
					const auto result = WaterDraws::ReadPublished(a_capture.view, id, constants.textures[t]);
					if (result != Family::InputResult::Graph && a_capture.ready) {
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

			// The imports are kept, a probe's too (WaterDraws::Keep): the next batch finds them instead of importing again.
			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			// A probe only checks: the engine draws it.
			if (a_capture.probe)
				return nullptr;
			run.draws.push_back(std::move(draw));
			return nullptr;
		}

		/** @brief A refusal noted: the batch fails, and the reason is logged once per technique and view. */
		void Refuse(Capture& a_capture, std::string_view a_reason, std::uint32_t a_technique)
		{
			a_capture.failed = true;
			Refusals().Report(std::format("a {} draw of technique {}: {}", NameOf(a_capture.view), a_technique, a_reason), a_capture.probe ? "found by a probe" : "");
		}

		/**
		 * @brief A draw latched (true: the graph draws it) or the engine's. A draw that fails marks the batch: its later draws are the
		 * engine's too, so the engine's draws of the batch stay in order after the ones latched before (which the anchor draws after the
		 * batch: one batch drawn out of order, then the view probes again).
		 */
		bool LatchDraw(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			// A probe checks every draw, so one batch asks for every program the view needs; a batch the graph draws stops at the first
			// draw it cannot take.
			if (a_capture.failed && !a_capture.probe)
				return false;
			if (const char* reason = TryLatchDraw(a_capture, a_shape, a_start, a_triangles)) {
				const auto* state = globals::state;
				Refuse(a_capture, reason, state ? TechniqueOf(state->currentPixelDescriptor) : 0xFF);
				return false;
			}
			return !a_capture.probe;
		}

		/**
		 * @brief A water batch: for the main camera's accumulator, the batch runs with its draws latched, then the anchor's pass draws them
		 * at this point.
		 */
		template <View kView>
		struct WaterBatch
		{
			static void thunk(void* a_accumulator, std::uint32_t a_first, std::uint32_t a_last, std::uint32_t a_flags, std::int32_t a_group)
			{
				auto& capture = GetCapture();
				if (capture.active || !TakesView(kView) || !ShellState::Running() || !FrameAnchors::IsRenderOwner() || !WaterDraws::Available()) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				Family::BeginBatch();
				Shape shape;
				auto run = std::make_shared<Run>();
				run->view = kView;
				PublishedHeaders published;
				if (!CurrentShape(a_accumulator, kView, shape, *run, published, capture)) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				const auto index = static_cast<std::size_t>(kView);
				auto& clean = capture.clean[index];
				// A view's client is made once, after kSettleBatches batches.
				auto& settled = capture.settled[index];
				settled = (std::min)(settled + 1, kSettleBatches);
				// Until the view's client is made and in the built graph, the engine draws its batches as probes: they ask for the programs
				// and ConstantMirror's copies.
				const bool ready = settled >= kSettleBatches && WaterDraws::Ready(shape);
				// A published header the client cannot read yet (its first Publish's rebuild has not come): the engine draws the batch.
				if (ready && !ReadHeaders(kView, published, *run)) {
					WaterDraws::Keep(std::move(run->owners));
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				const bool probe = !ready || !clean;
				// Draws are withheld only when the anchor's run will draw them (a probe withholds nothing).
				if (!probe && !WaterDraws::CanRun(kView)) {
					Refusals().Report(std::format("a {} batch whose anchor cannot run now", NameOf(kView)), "the ring is full or the client left the built graph");
					WaterDraws::Keep(std::move(run->owners));
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				capture.view = kView;
				capture.run = run;
				capture.ready = ready;
				capture.probe = probe;
				capture.failed = false;
				capture.passes.Begin();
				capture.active = true;
				func(a_accumulator, a_first, a_last, a_flags, a_group);
				capture.active = false;
				capture.run.reset();
				if (!capture.passes.End()) {
					Refusals().Report(capture.passes.Describe(std::format("{} water", NameOf(kView))));
					Refuse(capture, "a pass of the batch drew through another draw function than DrawTriShape", capture.lastTechnique);
				}
				if (!ready) {
					clean = false;
					WaterDraws::Keep(std::move(run->owners));
					return;
				}
				if (probe) {
					clean = !capture.failed;
					if (clean)
						logger::info("[DCLF] water: every {} water draw can be drawn by the graph; its batches are the graph's from the next", NameOf(kView));
					WaterDraws::Keep(std::move(run->owners));
					return;
				}
				if (capture.failed) {
					clean = false;
					Refusals().Report(std::format("a {} batch the graph had taken, from a draw it could not latch on", NameOf(kView)), "the view probes again");
				}
				// A run that was not submitted lost its draws for this frame: the view probes again.
				if (!WaterDraws::Submit(std::move(run)))
					clean = false;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The family's DrawTriShape taker: during a latched water batch, the draw is latched instead. */
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
		 * @brief After BSWaterShader::SetupGeometry (chained after Unified Water's, Light Limit Fix's, the cubemap fix's and the frame
		 * annotations'): during a latched batch, ConstantMirror watches the buffers the draw reads back (both stages' three groups, Light
		 * Limit Fix's strict lights, the directional shadow record). A buffer is mirrored from its next write on, so a batch's first sight
		 * of one draws on the engine.
		 */
		void OnSetupGeometry(RE::BSRenderPass* a_pass)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return;
			capture.passes.OnSetup(a_pass);
			if (const auto* state = globals::state)
				capture.lastTechnique = TechniqueOf(state->currentPixelDescriptor);
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			auto& mirror = ConstantMirror::Get();
			for (std::uint32_t g = 0; g < 3; ++g) {
				if (const auto* vs = rss.currentVertexShader; vs && vs->constantBuffers[g].buffer)
					mirror.Watch(reinterpret_cast<ID3D11Buffer*>(vs->constantBuffers[g].buffer));
				if (const auto* ps = rss.currentPixelShader; ps && ps->constantBuffers[g].buffer)
					mirror.Watch(reinterpret_cast<ID3D11Buffer*>(ps->constantBuffers[g].buffer));
			}
			if (auto* strictLights = StrictLightBuffer())
				mirror.Watch(strictLights);
			if (auto* directionalShadow = DirectionalShadowBuffer())
				mirror.Watch(directionalShadow);
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
	}

	void InstallHooks()
	{
		if (!Address::kMaskBatch.Known() || !Address::kSurfaceBatch.Known() || !Family::AddDrawTaker("water", &TakeDraw)) {
			logger::warn("[DCLF] water: this runtime has no address for the water batches; the water stays the engine's");
			return;
		}
		stl::write_thunk_call<WaterBatch<View::Mask>>(REL::Offset(Address::kMaskBatch).address());
		stl::write_thunk_call<WaterBatch<View::Surface>>(REL::Offset(Address::kSurfaceBatch).address());
		Family::WatchSetupGeometry<RE::BSShader::Type::Water>(RE::VTABLE_BSWaterShader[0], &OnSetupGeometry);
		logger::info("[DCLF] water: hooks installed (the water mask and water batches, BSWaterShader::SetupGeometry)");
	}
}
