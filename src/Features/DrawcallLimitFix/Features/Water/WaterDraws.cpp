// The water family's render graph side. One client per view at the EngineDraws anchor, made once, draws a batch's latched draws in
// engine order with the engine's stencil state and reference per draw.
// The clients are declared at setup and registered by the next frame revision (frame revision plan, 6.19).

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/Engine/Engine.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/FamilyClient.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DeferredRanges.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"

#include "Globals.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "Utils/Hash.h"
#include "Utils/Logging.h"
#include "RenderGraph/Frame/Declare.h"
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
		constexpr std::uint32_t kMaxDraws = 64;
		// A declared segment commits all of a view's runs of a frame into one run, so one execution holds several runs' draws.
		// Unified Water's distant water reached 301 at Riverwood's river (2026-11-20).
		constexpr std::uint32_t kExecutionDraws = 8 * kMaxDraws;
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

		/** @brief A commit's DCLF water draws (Draws::TakeCpuRange) and their target set. */
		struct CpuWater
		{
			void* run = nullptr;
			std::uint32_t range = 0, variant = 0;
		};

		const std::array<std::int32_t, 3>& TargetListOf(const Shape& a_shape) { return a_shape.view == View::Mask ? kMaskTargets : kSurfaceTargets; }

		struct Client : Family::ClientState<Run>
		{
			Shape shape;
			std::shared_ptr<org::LatchBlock> latch;
			// Only this client's commit opens the gate, so a run another client keeps draws nothing of this client's older run.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			std::array<FrameAnchors::Texture, 3> colours;
			FrameAnchors::Texture depth;
			// Fixed at setup, so a new Publish needs no new client.
			Family::PublishedInputs inputs;
			// The run each commit drew. The pass may record on the submission thread after the render thread published the next run.
			::RenderGraph::RunSlots<Run> runs;
			// The DCLF water draws each commit took (Run::cpuWater), by the same runs.
			::RenderGraph::RunSlots<CpuWater> cpuRuns;
			// The draws committed into the declared segment's run so far this frame.
			std::shared_ptr<const Run> committed;
			std::uint64_t committedFrame = UINT64_MAX;
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

		/** @brief Records a run's draws in engine order. */
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
				// Every registered input is declared, read this frame or not, so a new Publish needs no new client.
				bindings.inputs = client->inputs.Declare(a_builder);
				return bindings;
			}

			// The pass reads the run at execution, so the run is not part of the revision and a prepared ticket stays current.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				if (atExecution)
					return;
				const auto run = client->Current();
				a_out.push_back(run ? run->revision : 0);
			}

			Frame Prepare(const Bindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				Extensions::CheckDeclaredReads(a_bindings.inputs, a_preparation, client->id);
				return { atExecution ? nullptr : client->Current(), client };
			}

			/** @brief A run whose commit opened none of the gate entries records only the pass's barriers. */
			static bool SkipsAtExecution(const Bindings&, const Frame& a_frame, const org::PassRecordContext& a_recording)
			{
				if (!a_frame.client)
					return true;
				if (a_frame.client->cpuRuns.Find(a_recording.Run()))
					return false;
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
				// DCLF's claimed water first, as the GPU-driven frame's epoch draws it before the engine's batch.
				// Taken: its site run is drawn once, and its frame slot may retire after this run's submission.
				if (const auto cpu = a_frame.client->cpuRuns.Take(a_recording.Run()))
					RecordCpuWater(a_bindings, *a_frame.client, *cpu, a_recording);
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
												  .indirect = &c.gate->Latch(),
												  .indirectOffset = c.gate->Offset(i) });
				}
			}

		private:
			/** @brief The CPU-culled draws of a_cpu's range, into its target set: each target found among the pass's declared ones. */
			static void RecordCpuWater(const Bindings& a_bindings, const Client& a_client, const CpuWater& a_cpu, org::PassRecordContext& a_recording)
			{
				const auto& entry = Extensions::DeferredRanges()[a_cpu.range];
				if (!entry.taken || !entry.contribution.target)
					return;
				std::array<Direct::WaterTarget, Api::kDeferredRangeTargets> colours{};
				std::uint32_t count = 0;
				for (std::uint32_t i = 0; i < Api::kDeferredRangeTargets; ++i) {
					const auto target = entry.contribution.target(a_cpu.variant, i);
					if (!target)
						break;
					std::size_t k = 0;
					while (k < a_client.colours.size() && !(a_bindings.hasColours[k] && a_client.colours[k] == target))
						++k;
					if (k == a_client.colours.size()) {
						static std::atomic<bool> logged{ false };
						if (!logged.exchange(true))
							logger::error("[DCLF] water: the {} pass does not declare target {} of deferred range {}'s set {}; its DCLF water draws nothing <- WATER TARGETS",
								a_client.id, i, entry.name, a_cpu.variant);
						return;
					}
					colours[count++] = { a_recording.View(a_bindings.colours[k]), a_recording.Resolve(a_bindings.colours[k].Resource()).GetHandle() };
				}
				const Direct::WaterTarget depth{ a_recording.View(a_bindings.depth), a_recording.Resolve(a_bindings.depth.Resource()).GetHandle() };
				(void)Direct::RecordWaterDraws(a_cpu.run, a_recording.Commands(), std::span(colours.data(), count), depth);
			}

			std::shared_ptr<Client> client;
			bool atExecution = false;  // FrameAnchors::RecordsAtExecution at the build
		};

		/** @brief The programs a water vertex descriptor can be drawn with. Programs are keyed by the vertex descriptor. */
		void StagesOf(bool a_pixel, std::uint32_t a_descriptor, std::vector<Family::StageRequest>& a_out)
		{
			if (!a_pixel)
				a_out.push_back({ .descriptor = a_descriptor, .stage = Family::DefineStage::Vertex });
		}

		/** @brief Render-thread state. */
		struct DrawsState
		{
			Family::Client<Run, Client, kViews> clients{ "water" };
			Family::EngineDrawPrograms programs{ "water", { .source = kShader,
															  .constantWords = ShaderBindings::Words<WaterBlock>(),
															  .shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::Water),
															  .stagesOf = &StagesOf } };
		};

		/**
		 * @brief Every published id a_view's draws can read, listed at setup. The mask reads none.
		 * Exporting an id not listed here is refused and logged once. Add the id here.
		 */
		void ListInputs(View a_view, Family::PublishedInputs& a_inputs)
		{
			if (!HeadersRead(a_view))
				return;
			using Colour = RE::RENDER_TARGET;
			using Depth = RE::RENDER_TARGET_DEPTHSTENCIL;
			a_inputs.Add(Family::RegisteredIdsAt(kHeaderRegisters));
			a_inputs.Add(Family::RegisteredIdsAt(Integrations::PhysicalSky::kOrgTextureRegisters));
			for (const auto depth : { Depth::kPOST_ZPREPASS_COPY, Depth::kMAIN_COPY, Depth::kPRECIPITATION_OCCLUSION_MAP })
				a_inputs.AddEngine(depth);
			for (const auto colour : { Colour::kMAIN_COPY, Colour::kREFRACTION_NORMALS, Colour::kWATER_DISPLACEMENT, Colour::kWATER_DISPLACEMENT_SWAP,
					 Colour::kWATER_FLOW, Colour::kSSR, Colour::kSSR_RAW, Colour::kSSR_BLURRED0 })
				a_inputs.AddEngine(colour);
			a_inputs.AddEngine(RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS);
			// The refraction at t1, kMAIN's copy, which the engine fills before the water draws.
			a_inputs.AddEngineCopy(Colour::kMAIN);
			// The flow map at t8, written by the ImageSpace port's flow copies. Absent without the port.
			a_inputs.Add(::RenderGraph::Engine::ImageSpace::kFlowMapId);
		}

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

		/** @brief Makes the client for a_shape's view. Null, logged, when a target cannot be imported. */
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
			const auto& targets = TargetListOf(a_shape);
			for (std::size_t i = 0; i < targets.size(); ++i) {
				if (targets[i] < 0)
					continue;
				client->colours[i] = FrameAnchors::EngineTexture(FrameAnchors::Colour(static_cast<std::uint32_t>(targets[i])));
				if (!client->colours[i]) {
					logger::warn("[DCLF] water: the engine target {} cannot be imported; the {} stays the engine's", targets[i], client->id);
					return nullptr;
				}
			}
			client->depth = FrameAnchors::EngineTexture(FrameAnchors::Depth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN)));
			if (!client->depth) {
				logger::warn("[DCLF] water: the main depth cannot be imported; the {} stays the engine's", client->id);
				return nullptr;
			}
			ListInputs(a_shape.view, client->inputs);
			return client;
		}

		/** @brief The registration of a_client. Its targets were imported at setup. */
		bool MakeDesc(const std::shared_ptr<Client>& a_client, FrameAnchors::ClientDesc& a_desc)
		{
			a_desc.id = a_client->id;
			a_desc.sharedConstants = true;
			a_desc.gates = { a_client->gate };
			a_desc.passes.push_back({ .name = a_client->id + ".draws",
				.anchor = FrameAnchors::Anchor::EngineDraws,
				.order = a_client->shape.view == View::Mask ? 0 : 1,
				.kind = FrameAnchors::PassKind::Render,
				.make = [client = a_client]() -> std::shared_ptr<org::RenderPass> {
					return std::make_shared<WaterPass>(client, FrameAnchors::RecordsAtExecution(FrameAnchors::Anchor::EngineDraws));
				},
				.atExecution = true });
			a_desc.commit = [client = a_client](const FrameAnchors::Point&) {
				const auto run = client->Current();
				auto* host = RenderGraphRuntime::Get().Host();
				if (!host)
					return;
				const org::RunRef latchRun = host->CurrentRun();
				// A batch longer than kMaxDraws is several runs in a frame. Later runs follow the draws committed before.
				const auto frame = ::RenderGraph::Frame::Driver::Current().frame;
				std::uint32_t first = 0;
				if (run && FrameAnchors::TapInstance() > 0 && client->committedFrame == frame && client->committed) {
					const auto& earlier = *client->committed;
					if (earlier.headerIndices != run->headerIndices || earlier.draws.size() + run->draws.size() > kExecutionDraws) {
						logger::error("[DCLF] water: a second {} run in one segment run {}; it draws nothing", client->id,
							earlier.headerIndices != run->headerIndices ? std::string("reads other headers") :
																		  fmt::format("has too many draws ({} after {}, {} at most)", run->draws.size(), earlier.draws.size(), kExecutionDraws));
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
				client->runs.Store(latchRun, client->committed);
				if (run && run->cpuWater && !run->cpuHanded.exchange(true, std::memory_order_acq_rel))
					client->cpuRuns.Store(latchRun, std::make_shared<const CpuWater>(CpuWater{ run->cpuWater, client->shape.view == View::Mask ? 0u : 1u, run->cpuVariant }));
				if (!run)
					return;
				for (std::uint32_t i = 0; i < run->draws.size(); ++i) {
					const auto& draw = run->draws[i];
					client->latch->WriteValue(latchRun, (first + i) * kDrawStride, draw.latch);
					if (draw.program)
						client->gate->OpenDraw(first + i, draw.indexCount, 1);
				}
			};
			a_desc.repeats = true;
			a_desc.executed = [client = a_client](const FrameAnchors::Point&) {
				if (client->CountExecuted())
					logger::info("[DCLF] water: the first {} run drawn by the graph", client->id);
			};
			a_desc.active = [client = a_client] { return client->pending; };
			a_desc.segmentOnly = FrameAnchors::NamedByStage(a_desc.id);
			const auto late = a_client->inputs.Unregistered();
			logger::info("[DCLF] water: the {} client is made (published inputs it may read: {}{})", a_client->id, a_client->inputs.Describe(),
				late.empty() ? std::string() : std::format("; not published yet: {}", late));
			return true;
		}
	}

	bool Available()
	{
		return GetState().clients.Available();
	}

	bool Setup()
	{
		auto& state = GetState();
		if (const auto result = state.clients.BeginSetup())
			return *result;
		Family::StartStagePrecompile();
		state.programs.Prebuild();
		if (!::RenderGraph::Engine::GraphMode()) {
			logger::warn("[DCLF] water: needs graph mode; the water stays the engine's");
			return true;
		}
		namespace RF = ::RenderGraph::Frame;
		for (const auto view : { View::Mask, View::Surface }) {
			auto client = MakeClient({ .view = view });
			if (!client)
				continue;
			// The targets are the engine's, made once with its other targets, and imported above. Nothing changes the registration.
			state.clients.Declare<Family::NoKey>(
				static_cast<std::size_t>(view), client, RF::DependsOn(), [](const RF::Inputs&) -> std::optional<Family::NoKey> { return Family::NoKey{}; },
				[client](const Family::NoKey&, FrameAnchors::ClientDesc& a_desc) { return MakeDesc(client, a_desc); }, "the water stays the engine's");
		}
		return true;
	}

	bool CanTake(View a_view)
	{
		// Until a revision registers the client, the engine draws the batch.
		// The mask and the above-water surface are the deferred+water stage's. Under water the surface is the sun stage's.
		return GetState().clients.CanTake(static_cast<std::size_t>(a_view), static_cast<std::uint32_t>(::RenderGraph::Frame::SegmentId::Deferred));
	}

	Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index)
	{
		const auto& client = GetState().clients.At(static_cast<std::size_t>(a_view));
		return client ? client->inputs.Resolve(a_id, a_index) : Family::InputResult::NotListed;
	}

	std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader)
	{
		DCLF_ZONE("CS.DCLF.Water.Program");
		return GetState().programs.Get(a_key, a_shader);
	}

	void Keep(Family::Owners&& a_owners)
	{
		GetState().clients.Keep(std::move(a_owners));
	}

	bool Submit(std::shared_ptr<Run> a_run)
	{
		if (!a_run)
			return false;
		const auto view = static_cast<std::size_t>(a_run->view);
		return GetState().clients.Submit(view, std::move(a_run));
	}

	std::string Status()
	{
		const auto& state = GetState();
		if (!Enabled())
			return "off (not graph mode)";
		return fmt::format("{} runs ({} drawn), {} draws, {} programs", state.clients.Runs(), state.clients.Executed(), state.clients.Draws(), state.programs.Built());
	}
}

namespace DCLF::Features::Water
{
	std::uint64_t Run::Hash() const
	{
		Util::Fnv1a hash;
		hash.Mix(static_cast<std::uint64_t>(view));
		hash.Mix(draws.size());
		for (const auto index : headerIndices)
			hash.Mix(index);
		for (const auto& draw : draws) {
			hash.Mix(draw.program.get());
			hash.Mix((std::uint64_t(draw.colours[0]) << 8) | draw.colours[1]);
			hash.Mix(draw.stencilRef);
			for (const auto v : draw.viewport.rect)
				hash.Mix(v);
			std::uint32_t range[2];
			std::memcpy(range, draw.viewport.depthRange.data(), sizeof(range));
			hash.Mix((std::uint64_t(range[0]) << 32) | range[1]);
		}
		hash.Mix(cpuWater ? 1u + cpuVariant : 0u);
		return hash.Value();
	}
}
