// The water family's render graph side: a client per view at the EngineDraws anchor, made once, whose pass draws a batch's latched draws
// in the engine's order with CS's Water.hlsl built for ORG, with the engine's stencil state and reference per draw.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"

#include "Globals.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "Utils/Logging.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <format>
#include <string_view>

namespace DCLF::Features::Water::WaterDraws
{
	namespace
	{
		constexpr const char* kShader = "Water.hlsl";
		// The most draws a run latches (WaterCapture's kMaxLatched); a batch with more draws in several runs.
		constexpr std::uint32_t kMaxDraws = 64;
		// The most draws one execution holds: a declared segment commits all of a view's runs of a frame into
		// its one run (ClientDesc::repeats), each after the earlier ones' entries.
		constexpr std::uint32_t kExecutionDraws = 4 * kMaxDraws;
		constexpr std::uint32_t kDrawStride = static_cast<std::uint32_t>(sizeof(DrawLatch));
		constexpr std::size_t kViews = static_cast<std::size_t>(View::Count);

		/** @brief Water.hlsl's push block (WATER_ORG_RESOURCES), in list order. */
		struct WaterBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Latch draw, strictLights;
			std::array<ShaderBindings::Handle, kHeadersBeforeShadow> headers;                                // t17 to t77
			ShaderBindings::Latch directionalShadow;                                                         // t98
			std::array<ShaderBindings::Handle, kHeaderRegisters.size() - kHeadersBeforeShadow> lateHeaders;  // t102, t103
		};
		static_assert(sizeof(WaterBlock) == 112, "Water.hlsl's push block is 112 bytes");

		const std::array<std::int32_t, 3>& TargetListOf(const Shape& a_shape) { return a_shape.view == View::Mask ? kMaskTargets : kSurfaceTargets; }

		/** @brief A client's state, which its pass shares. */
		struct Client
		{
			Shape shape;
			std::string id;
			std::shared_ptr<org::LatchBlock> latch;
			// One draw entry per latched draw. The anchor closes it at every run, and only this client's commit opens it, so a run of
			// the anchor that another client keeps draws nothing of this client's older run.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			// The targets as imported at the registration (FrameAnchors keeps each while the engine's texture stays the same).
			std::array<FrameAnchors::Texture, 3> colours;
			FrameAnchors::Texture depth;
			// Every published id the view's draws can read, fixed here (ListInputs). Each build declares the registered ones.
			Family::PublishedInputs inputs;
			// Render thread writes, the host's thread reads.
			std::shared_ptr<const Run> run;
			// The run each commit drew, by the epoch's run: what the pass recorded at execution reads. It may record on the submission
			// thread after the render thread published the next run.
			::RenderGraph::RunSlots<Run> runs;
			// The draws committed into the declared segment's run this frame so far (a later run of the same frame follows them), and its frame.
			std::shared_ptr<const Run> committed;
			std::uint64_t committedFrame = UINT64_MAX;
			bool pending = false;
			std::atomic<std::uint64_t> executed{ 0 };  // runs submitted with this client's pass (its executed callback)
		};

		struct Bindings
		{
			SharedConstants::Bindings shared;
			Extensions::DeclaredReads inputs;  // the published inputs (Client::inputs)
			std::array<org::DeclaredViewToken, 3> colours{};
			std::array<bool, 3> hasColours{};
			org::DeclaredViewToken depth{};
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;  // null when the pass records at execution: Record reads the client's run then
			std::shared_ptr<const Client> client;
		};

		/** @brief A run's draws, recorded in the engine's order: a render pass and a draw each (RasterProgram::Draw). */
		class WaterPass final : public org::TypedRenderGraphPass<WaterPass, Frame, Bindings>
		{
		public:
			WaterPass(std::shared_ptr<Client> a_client, bool a_atExecution) :
				client(std::move(a_client)), atExecution(a_atExecution) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				Bindings bindings{};
				bindings.shared = SharedConstants::Declare(a_builder);
				const auto& c = *client;
				for (std::size_t i = 0; i < c.colours.size(); ++i) {
					if (!c.colours[i])
						continue;
					bindings.colours[i] = a_builder.RenderTarget(c.colours[i]).View();
					bindings.hasColours[i] = true;
				}
				bindings.depth = a_builder.DepthReadWrite(c.depth).View();
				// The published inputs, each with the view whose index the run's header indices and the draws' latches carry. Every
				// registered one is declared, read this frame or not, so a new Publish needs no new client.
				bindings.inputs = client->inputs.Declare(a_builder);
				return bindings;
			}

			// The run's draws (their programs, targets, stencil references, counts and viewports) and the headers' indices.
			// Recorded at execution, the pass reads the run its commit published when it records: the run is not part of it, so a
			// ticket prepared ahead stays current when the draws change.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				if (atExecution)
					return;
				const auto run = std::atomic_load(&client->run);
				a_out.push_back(run ? run->revision : 0);
			}

			Frame Prepare(const Bindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				Extensions::CheckDeclaredReads(a_bindings.inputs, a_preparation, client->id);
				return { atExecution ? nullptr : std::atomic_load(&client->run), client };
			}

			/** @brief Recorded at execution: a run whose commit opened none of the draws' gate entries records only the pass's barriers. */
			static bool SkipsAtExecution(const Bindings&, const Frame& a_frame, const org::PassRecordContext& a_recording)
			{
				if (!a_frame.client)
					return true;
				const auto run = a_frame.run ? a_frame.run : a_frame.client->runs.Find(a_recording.Run());
				if (!run)
					return true;
				for (std::uint32_t i = 0; i < run->draws.size(); ++i)
					if (a_frame.client->gate->IsOpen(a_recording.Run(), i))
						return false;
				return true;
			}

			static void Record(const Bindings& a_bindings, const Frame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.client)
					return;
				const auto runRef = a_frame.run ? a_frame.run : a_frame.client->runs.Find(a_recording.Run());
				if (!runRef)
					return;
				const auto& run = *runRef;
				const auto& c = *a_frame.client;
				const auto shared = SharedConstants::Index(a_recording, a_bindings.shared);
				const auto& headers = run.headerIndices;
				const org::RunRef latchRun = a_recording.Run();
				for (std::uint32_t i = 0; i < run.draws.size(); ++i) {
					const auto& draw = run.draws[i];
					if (!draw.program)
						continue;
					const auto& program = *static_cast<const RasterProgram*>(draw.program.get());
					const std::uint32_t offset = i * kDrawStride;
					WaterBlock block{};
					block.shared = shared;
					block.draw = ShaderBindings::LatchOf(*c.latch, latchRun, offset);
					block.strictLights = ShaderBindings::LatchOf(*c.latch, latchRun, offset + static_cast<std::uint32_t>(offsetof(DrawLatch, strictLights)));
					block.directionalShadow = ShaderBindings::LatchOf(*c.latch, latchRun, offset + static_cast<std::uint32_t>(offsetof(DrawLatch, directionalShadow)));
					std::copy_n(headers.begin(), kHeadersBeforeShadow, block.headers.begin());
					std::copy(headers.begin() + kHeadersBeforeShadow, headers.end(), block.lateHeaders.begin());

					std::array<RasterProgram::Colour, 2> colours{};
					const std::uint32_t colourCount = Water::TargetsOf(run.view);
					bool targetsOk = true;
					for (std::uint32_t k = 0; k < colourCount; ++k) {
						const auto target = draw.colours[k];
						targetsOk = targetsOk && target < a_bindings.colours.size() && a_bindings.hasColours[target];
						if (targetsOk)
							colours[k] = { .view = &a_bindings.colours[target] };
					}
					if (!targetsOk)
						continue;
					const RasterProgram::Depth depth{ .view = &a_bindings.depth, .readOnly = false };
					program.Draw(a_recording, { .colours = std::span<const RasterProgram::Colour>(colours.data(), colourCount),
												  .depth = &depth,
												  .viewport = { .x = draw.viewport.rect[0], .y = draw.viewport.rect[1], .width = draw.viewport.rect[2], .height = draw.viewport.rect[3],
													  .minDepth = draw.viewport.depthRange[0], .maxDepth = draw.viewport.depthRange[1] },
												  .constants = ShaderBindings::Data(block),
												  .debugName = run.view == View::Mask ? "WaterMask" : "Water",
												  .stencilReference = draw.stencilRef,
												  // The counts come from the draw's gate entry, which the client's commit opens for this run only.
												  .indirect = &c.gate->Latch(),
												  .indirectOffset = c.gate->Offset(i) });
				}
			}

		private:
			std::shared_ptr<Client> client;
			bool atExecution = false;  // FrameAnchors::RecordsAtExecution at the build
		};

		/**
		 * @brief The programs a BSWaterShader vertex descriptor CS loads can be drawn with: the family keys its programs by the vertex
		 * descriptor (both stages take its defines), so every one is a program's, for the mask (the Stencil technique) and the surface alike.
		 */
		void StagesOf(bool a_pixel, std::uint32_t a_descriptor, std::vector<Family::StageRequest>& a_out)
		{
			if (!a_pixel)
				a_out.push_back({ .descriptor = a_descriptor, .stage = Family::DefineStage::Vertex });
		}

		/** @brief Everything here: the render thread's. */
		struct DrawsState
		{
			std::array<std::shared_ptr<Client>, kViews> clients;
			Family::EngineDrawPrograms programs{ "water", { .source = kShader,
															  .constantWords = ShaderBindings::Words<WaterBlock>(),
															  .shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::Water),
															  .stagesOf = &StagesOf } };
			bool reserved = false;
			bool setUp = false;
			std::uint64_t runs = 0, draws = 0;
			Family::KeptOwners kept;
		};

		/**
		 * @brief Every published id a_view's draws can read, listed when its client is made at setup. That is what the ported features
		 * publish at the headers' registers (DCLF's registry), the engine depths CS binds at a header (the scene depth at t17, Wetness
		 * Effects' precipitation occlusion at t70), and the engine targets the surface's material slots hold (water.md, "The shader": the
		 * refraction copy at t1, the displacement at t2, the reflection cubemap at t3, the depth copy at t7, the flow at t8, the SSR
		 * targets at t10 and t11), imported now so the first build declares them, and the flow map at t8 (the ImageSpace port's
		 * published import). The mask reads no header and no slot (HeadersRead,
		 * SlotRead). An export of an id not listed here is an inherent refusal, logged once with the id: add the id here.
		 */
		void ListInputs(View a_view, Family::PublishedInputs& a_inputs)
		{
			if (!HeadersRead(a_view))
				return;
			using Colour = RE::RENDER_TARGET;
			using Depth = RE::RENDER_TARGET_DEPTHSTENCIL;
			a_inputs.Add(Family::RegisteredIdsAt(kHeaderRegisters));
			for (const auto depth : { Depth::kPOST_ZPREPASS_COPY, Depth::kMAIN_COPY, Depth::kPRECIPITATION_OCCLUSION_MAP })
				a_inputs.AddEngine(depth);
			for (const auto colour : { Colour::kMAIN_COPY, Colour::kREFRACTION_NORMALS, Colour::kWATER_DISPLACEMENT, Colour::kWATER_DISPLACEMENT_SWAP,
					 Colour::kWATER_FLOW, Colour::kSSR, Colour::kSSR_RAW, Colour::kSSR_BLURRED0 })
				a_inputs.AddEngine(colour);
			a_inputs.AddEngine(RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS);
			// The refraction at t1: kMAIN's copy texture, which the engine fills from kMAIN before the water draws.
			a_inputs.AddEngineCopy(Colour::kMAIN);
			// The flow map at t8, which the ImageSpace port's flow copies write (published at its Setup; not without the port).
			a_inputs.Add(::RenderGraph::Engine::ImageSpace::kFlowMapId);
		}

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

		/** @brief The client of a_shape's view, registered with the anchor for a_shape. Null, logged, when a target cannot be imported. */
		std::shared_ptr<Client> MakeClient(const Shape& a_shape)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return nullptr;
			auto client = std::make_shared<Client>();
			client->shape = a_shape;
			client->id = a_shape.view == View::Mask ? "cs.water.mask" : "cs.water.surface";
			client->latch = std::make_shared<org::LatchBlock>(client->id + ".latch", kExecutionDraws * kDrawStride);
			client->gate = ::RenderGraph::RunGate::Create(client->id + ".gate", kExecutionDraws);
			if (!client->gate) {
				logger::warn("[DCLF] water: the {} run gate could not be made; the water stays the engine's", client->id);
				return nullptr;
			}
			const auto& targets = TargetListOf(a_shape);
			for (std::size_t i = 0; i < targets.size(); ++i) {
				if (targets[i] < 0)
					continue;
				client->colours[i] = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(targets[i]));
				if (!client->colours[i]) {
					logger::warn("[DCLF] water: the engine target {} cannot be imported; the {} stays the engine's", targets[i], client->id);
					return nullptr;
				}
			}
			client->depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
			if (!client->depth) {
				logger::warn("[DCLF] water: the main depth cannot be imported; the {} stays the engine's", client->id);
				return nullptr;
			}
			ListInputs(a_shape.view, client->inputs);

			FrameAnchors::ClientDesc desc{};
			desc.id = client->id;
			desc.sharedConstants = true;
			desc.gates = { client->gate };
			desc.passes.push_back({ .name = client->id + ".draws",
				.anchor = FrameAnchors::Anchor::EngineDraws,
				.order = a_shape.view == View::Mask ? 0 : 1,
				.kind = FrameAnchors::PassKind::Render,
				.make = [client]() -> std::shared_ptr<org::RenderPass> {
					return std::make_shared<WaterPass>(client, FrameAnchors::RecordsAtExecution(FrameAnchors::Anchor::EngineDraws));
				},
				// The run's draws are known after its commit: recorded then, the pass reads them without a ticket prepared again.
				.atExecution = true });
			desc.commit = [client](const FrameAnchors::Point&) {
				const auto run = std::atomic_load(&client->run);
				auto* host = RenderGraphRuntime::Get().Host();
				if (!host)
					return;
				const org::RunRef latchRun = host->CurrentRun();
				// A later tap of a declared segment's run this frame (a batch of more than kMaxDraws draws is several runs): its draws follow
				// the ones committed before, and the pass records them all.
				const auto frame = ::RenderGraph::Frame::Driver::Current().frame;
				std::uint32_t first = 0;
				if (run && FrameAnchors::TapInstance() > 0 && client->committedFrame == frame && client->committed) {
					const auto& earlier = *client->committed;
					if (earlier.headerIndices != run->headerIndices || earlier.draws.size() + run->draws.size() > kExecutionDraws) {
						logger::error("[DCLF] water: a second {} run in one segment run {}; it draws nothing", client->id,
							earlier.headerIndices != run->headerIndices ? "reads other headers" : "has too many draws");
						return;
					}
					auto merged = std::make_shared<Run>();
					merged->view = run->view;
					merged->headerIndices = run->headerIndices;
					merged->revision = run->revision;
					merged->draws = earlier.draws;
					merged->draws.insert(merged->draws.end(), run->draws.begin(), run->draws.end());
					first = static_cast<std::uint32_t>(earlier.draws.size());
					client->committed = merged;
				} else {
					client->committed = run;
				}
				client->committedFrame = frame;
				// For the pass recorded at execution, by this run (null: nothing drawn).
				client->runs.Store(latchRun, client->committed);
				if (!run)
					return;
				for (std::uint32_t i = 0; i < run->draws.size(); ++i) {
					const auto& draw = run->draws[i];
					client->latch->WriteValue(latchRun, (first + i) * kDrawStride, draw.latch);
					if (draw.program)
						client->gate->OpenDraw(first + i, draw.indexCount, 1);
				}
			};
			// In a declared segment both views may commit more than once into its run (above).
			desc.repeats = true;
			desc.executed = [client](const FrameAnchors::Point&) {
				if (client->executed.fetch_add(1, std::memory_order_relaxed) == 0)
					logger::info("[DCLF] water: the first {} run drawn by the graph", client->id);
			};
			desc.active = [client] { return client->pending; };
			// Passes of the deferred+water stage, or of the sun stage under water, only.
			desc.segmentOnly = FrameAnchors::NamedByStage(desc.id);
			if (!FrameAnchors::Register(std::move(desc))) {
				logger::warn("[DCLF] water: the {} client could not be registered; the water stays the engine's", client->id);
				return nullptr;
			}
			const auto late = client->inputs.Unregistered();
			logger::info("[DCLF] water: the {} client registered at setup (published inputs it may read: {}{})", client->id, client->inputs.Describe(),
				late.empty() ? std::string() : std::format("; not published yet: {}", late));
			return client;
		}

		/** @brief What a run's recording depends on: everything of its draws but the latched values and counts, and the headers' indices. */
		std::uint64_t RevisionOf(const Run& a_run)
		{
			std::uint64_t hash = 1469598103934665603ull;
			const auto mix = [&](std::uint64_t a_value) { hash = (hash ^ a_value) * 1099511628211ull; };
			mix(static_cast<std::uint64_t>(a_run.view));
			mix(a_run.draws.size());
			for (const auto index : a_run.headerIndices)
				mix(index);
			for (const auto& draw : a_run.draws) {
				mix(reinterpret_cast<std::uintptr_t>(draw.program.get()));
				mix((std::uint64_t(draw.colours[0]) << 8) | draw.colours[1]);
				mix(draw.stencilRef);
				for (const auto v : draw.viewport.rect)
					mix(v);
				std::uint32_t range[2];
				std::memcpy(range, draw.viewport.depthRange.data(), sizeof(range));
				mix((std::uint64_t(range[0]) << 32) | range[1]);
			}
			return hash;
		}
	}

	bool Available()
	{
		const auto& state = GetState();
		return state.reserved && RenderGraphRuntime::Get().IsActive() && FrameAnchors::HasEpoch(FrameAnchors::Anchor::EngineDraws);
	}

	void Reserve()
	{
		auto& state = GetState();
		state.reserved = FrameAnchors::Reserve(FrameAnchors::Anchor::EngineDraws);
		if (!state.reserved)
			logger::warn("[DCLF] water: the EngineDraws anchor could not be reserved; the water stays the engine's");
	}

	bool Setup()
	{
		auto& state = GetState();
		if (state.setUp)
			return true;
		if (!RenderGraphRuntime::Get().Host() || !FrameAnchors::IsRenderOwner() || !Available())
			return false;
		state.setUp = true;
		Family::StartStagePrecompile();
		state.programs.Prebuild();
		if (!::RenderGraph::Engine::Constants::Requested()) {
			logger::warn("[DCLF] water: needs the engine constants capture (CS_ORG_ENGINE_CONSTANTS=1); the water stays the engine's");
			return true;
		}
		// A client is made once for its view. Its targets and published inputs are fixed lists, so no later batch needs another.
		for (const auto view : { View::Mask, View::Surface })
			if (TakesView(view))
				state.clients[static_cast<std::size_t>(view)] = MakeClient({ .view = view });
		return true;
	}

	bool CanTake(View a_view)
	{
		auto& state = GetState();
		if (a_view >= View::Count)
			return false;
		const auto& client = state.clients[static_cast<std::size_t>(a_view)];
		if (!client)
			return false;
		// The water mask and above water the surface are the deferred+water stage's; under water the surface is the sun stage's.
		return Family::HeldForBatch("water", client->id, static_cast<std::uint32_t>(::RenderGraph::Frame::SegmentId::Deferred));
	}

	Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index)
	{
		if (a_view >= View::Count)
			return Family::InputResult::NotListed;
		const auto& client = GetState().clients[static_cast<std::size_t>(a_view)];
		return client ? client->inputs.Resolve(a_id, a_index) : Family::InputResult::NotListed;
	}

	std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader)
	{
		return GetState().programs.Get(a_key, a_shader);
	}

	void Keep(Family::Owners&& a_owners)
	{
		GetState().kept.Keep(std::move(a_owners));
	}

	bool Submit(std::shared_ptr<Run> a_run)
	{
		auto& state = GetState();
		if (!a_run)
			return false;
		const auto& client = state.clients[static_cast<std::size_t>(a_run->view)];
		if (!client)
			return false;
		a_run->revision = RevisionOf(*a_run);
		state.kept.Keep(std::move(a_run->owners));
		++state.runs;
		state.draws += a_run->draws.size();
		std::atomic_store(&client->run, std::shared_ptr<const Run>(std::move(a_run)));
		return Family::RunWithheld("water", client->id, FrameAnchors::Anchor::EngineDraws, client->pending);
	}

	std::string Status()
	{
		const auto& state = GetState();
		if (!Enabled())
			return "off (CS_ORG_WATER)";
		std::uint64_t executed = 0;
		for (const auto& client : state.clients)
			if (client)
				executed += client->executed.load(std::memory_order_relaxed);
		return fmt::format("{} runs ({} drawn), {} draws, {} programs", state.runs, executed, state.draws, state.programs.Built());
	}
}
