// The precipitation family's render graph side. One client at the EngineDraws anchor draws each latched draw as its own run.
// The client is the sun stage's first step. Each draw of a frame is a tap appended to the earlier ones, and the pass records them
// all at execution.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/Precipitation/Precipitation.h"

#include "Globals.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"
#include "Utils/Logging.h"

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

namespace DCLF::Features::Precipitation::PrecipitationDraws
{
	namespace
	{
		constexpr const char* kShader = "Particle.hlsl";
		constexpr const char* kClientId = "cs.precipitation";
		// Each draw is a run, so a stage run holds a frame's draws with room for a few.
		constexpr std::uint32_t kMaxDraws = 8;
		constexpr std::uint32_t kDrawStride = static_cast<std::uint32_t>(sizeof(DrawLatch));

		/** @brief Particle.hlsl's push block (PARTICLE_ORG_RESOURCES), laid out as Water.hlsl's. */
		struct ParticleBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Latch draw, strictLights;
			std::array<ShaderBindings::Handle, Water::kHeadersBeforeShadow> headers;                                       // t17 to t77
			ShaderBindings::Latch directionalShadow;                                                                       // t98
			std::array<ShaderBindings::Handle, Water::kHeaderRegisters.size() - Water::kHeadersBeforeShadow> lateHeaders;  // t102, t103
		};
		static_assert(sizeof(ParticleBlock) == 112, "Particle.hlsl's push block is 112 bytes");

		struct Client
		{
			std::shared_ptr<org::LatchBlock> latch;
			// Only this client's commit opens the gate, so a run another client keeps draws nothing of this client's older run.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			// Null where a second target could not be imported.
			std::array<FrameAnchors::Texture, kTargets.size()> colours;
			FrameAnchors::Texture depth;
			// Fixed at setup, so a new Publish needs no new client.
			Family::PublishedInputs inputs;
			// Written by the render thread, read by the host's thread.
			std::shared_ptr<const Run> run;
			// The draws each commit committed. The pass may record on the submission thread after the render thread published the next run.
			::RenderGraph::RunSlots<Run> runs;
			std::shared_ptr<const Run> committed;
			std::uint64_t committedFrame = UINT64_MAX;
			bool pending = false;
			std::atomic<std::uint64_t> executed{ 0 };  // runs submitted with this client's pass (its executed callback)
		};

		struct Bindings
		{
			SharedConstants::Bindings shared;
			Extensions::DeclaredReads inputs;  // the published inputs (Client::inputs)
			std::array<org::DeclaredViewToken, kTargets.size()> colours{};
			std::array<bool, kTargets.size()> hasColours{};
			org::DeclaredViewToken depth{};
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;  // null when the pass records at execution: Record reads the client's run then
			std::shared_ptr<const Client> client;
		};

		/** @brief Records a run's draws in engine order. */
		class PrecipitationPass final : public org::TypedRenderGraphPass<PrecipitationPass, Frame, Bindings>
		{
		public:
			PrecipitationPass(std::shared_ptr<Client> a_client, bool a_atExecution) :
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
				bindings.inputs = client->inputs.Declare(a_builder);
				return bindings;
			}

			// The pass reads the run at execution, so the run is not part of the revision.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				if (atExecution)
					return;
				const auto run = std::atomic_load(&client->run);
				a_out.push_back(run ? run->revision : 0);
			}

			Frame Prepare(const Bindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				Extensions::CheckDeclaredReads(a_bindings.inputs, a_preparation, kClientId);
				return { atExecution ? nullptr : std::atomic_load(&client->run), client };
			}

			/** @brief A run whose commits opened none of the gate entries records only the pass's barriers. */
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
					ParticleBlock block{};
					block.shared = shared;
					block.draw = ShaderBindings::LatchOf(*c.latch, latchRun, offset);
					block.strictLights = ShaderBindings::LatchOf(*c.latch, latchRun, offset + static_cast<std::uint32_t>(offsetof(DrawLatch, strictLights)));
					block.directionalShadow = ShaderBindings::LatchOf(*c.latch, latchRun, offset + static_cast<std::uint32_t>(offsetof(DrawLatch, directionalShadow)));
					std::copy_n(headers.begin(), Water::kHeadersBeforeShadow, block.headers.begin());
					std::copy(headers.begin() + Water::kHeadersBeforeShadow, headers.end(), block.lateHeaders.begin());

					std::array<RasterProgram::Colour, kMaxColours> colours{};
					bool targetsOk = draw.colourCount > 0 && draw.colourCount <= kMaxColours;
					for (std::uint32_t k = 0; targetsOk && k < draw.colourCount; ++k) {
						const auto target = draw.colours[k];
						targetsOk = target < a_bindings.colours.size() && a_bindings.hasColours[target];
						if (targetsOk)
							colours[k] = { .view = &a_bindings.colours[target] };
					}
					if (!targetsOk)
						continue;
					const RasterProgram::Depth depth{ .view = &a_bindings.depth, .readOnly = false };
					program.Draw(a_recording, { .colours = std::span<const RasterProgram::Colour>(colours.data(), draw.colourCount),
												  .depth = &depth,
												  .viewport = { .x = draw.viewport.rect[0], .y = draw.viewport.rect[1], .width = draw.viewport.rect[2], .height = draw.viewport.rect[3],
													  .minDepth = draw.viewport.depthRange[0], .maxDepth = draw.viewport.depthRange[1] },
												  .constants = ShaderBindings::Data(block),
												  .debugName = "Precipitation",
												  .stencilReference = draw.stencilRef,
												  .indirect = &c.gate->Latch(),
												  .indirectOffset = c.gate->Offset(i) });
				}
			}

		private:
			std::shared_ptr<Client> client;
			bool atExecution = false;  // FrameAnchors::RecordsAtExecution at the build
		};

		/** @brief The ENVCUBE pixel descriptors are programs. Both stages take their defines. */
		void StagesOf(bool a_pixel, std::uint32_t a_descriptor, std::vector<Family::StageRequest>& a_out)
		{
			if (a_pixel && PrecipitationTechnique(a_descriptor))
				a_out.push_back({ .descriptor = a_descriptor, .stage = Family::DefineStage::Pixel });
		}

		/** @brief Render-thread state. */
		struct DrawsState
		{
			std::shared_ptr<Client> client;
			bool tried = false;  // the client is made once
			Family::EngineDrawPrograms programs{ "precipitation", { .source = kShader,
																	  .languageVersion = L"2018",  // FXC's semantics: Particle.hlsl selects with vector ?: as FXC does
																	  .constantWords = ShaderBindings::Words<ParticleBlock>(),
																	  .shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::Particle),
																	  .stagesOf = &StagesOf } };
			bool reserved = false;
			std::uint64_t runs = 0, draws = 0;
			Family::KeptOwners kept;
		};

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

		/** @brief Every published id the draws can read, listed at setup. Exporting an unlisted id is refused and logged once. */
		void ListInputs(Family::PublishedInputs& a_inputs)
		{
			using Colour = RE::RENDER_TARGET;
			using Depth = RE::RENDER_TARGET_DEPTHSTENCIL;
			a_inputs.Add(Family::RegisteredIdsAt(Water::kHeaderRegisters));
			const std::array<std::uint32_t, 1> raindrop{ kRaindropRegister };
			a_inputs.Add(Family::RegisteredIdsAt(raindrop));
			for (const auto depth : { Depth::kPOST_ZPREPASS_COPY, Depth::kMAIN_COPY, Depth::kPRECIPITATION_OCCLUSION_MAP })
				a_inputs.AddEngine(depth);
			a_inputs.AddEngine(Colour::kUNDERWATER_MASK);
		}

		/** @brief What a run's recording depends on: everything but the latched values and counts. */
		std::uint64_t RevisionOf(const Run& a_run)
		{
			std::uint64_t hash = 1469598103934665603ull;
			const auto mix = [&](std::uint64_t a_value) { hash = (hash ^ a_value) * 1099511628211ull; };
			mix(a_run.draws.size());
			for (const auto index : a_run.headerIndices)
				mix(index);
			for (const auto& draw : a_run.draws) {
				mix(reinterpret_cast<std::uintptr_t>(draw.program.get()));
				mix((std::uint64_t(draw.colourCount) << 16) | (std::uint64_t(draw.colours[0]) << 8) | draw.colours[1]);
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
			logger::warn("[DCLF] precipitation: the EngineDraws anchor could not be reserved; the precipitation stays the engine's");
	}

	bool Setup()
	{
		auto& state = GetState();
		if (state.tried)
			return true;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !FrameAnchors::IsRenderOwner() || !Available())
			return false;
		state.tried = true;
		Family::StartStagePrecompile();
		state.programs.Prebuild();
		if (!::RenderGraph::Engine::Constants::Requested()) {
			logger::warn("[DCLF] precipitation: needs the engine constants capture (CS_ORG_ENGINE_CONSTANTS=1); the precipitation stays the engine's");
			return true;
		}
		auto client = std::make_shared<Client>();
		client->latch = std::make_shared<org::LatchBlock>(std::string(kClientId) + ".latch", kMaxDraws * kDrawStride);
		client->gate = ::RenderGraph::RunGate::Create(std::string(kClientId) + ".gate", kMaxDraws);
		if (!client->gate) {
			logger::warn("[DCLF] precipitation: the run gate could not be made; the precipitation stays the engine's");
			return true;
		}
		for (std::size_t i = 0; i < kTargets.size(); ++i) {
			client->colours[i] = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(kTargets[i]));
			if (!client->colours[i] && i == 0) {
				logger::warn("[DCLF] precipitation: kMAIN cannot be imported; the precipitation stays the engine's");
				return true;
			}
		}
		client->depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
		if (!client->depth) {
			logger::warn("[DCLF] precipitation: the main depth cannot be imported; the precipitation stays the engine's");
			return true;
		}
		ListInputs(client->inputs);

		FrameAnchors::ClientDesc desc{};
		desc.id = kClientId;
		desc.sharedConstants = true;
		desc.gates = { client->gate };
		desc.passes.push_back({ .name = std::string(kClientId) + ".draws",
			.anchor = FrameAnchors::Anchor::EngineDraws,
			.order = 3,
			.kind = FrameAnchors::PassKind::Render,
			.make = [client]() -> std::shared_ptr<org::RenderPass> {
				return std::make_shared<PrecipitationPass>(client, FrameAnchors::RecordsAtExecution(FrameAnchors::Anchor::EngineDraws));
			},
			.atExecution = true });
		desc.commit = [client](const FrameAnchors::Point&) {
			const auto run = std::atomic_load(&client->run);
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return;
			const org::RunRef latchRun = host->CurrentRun();
			// Each draw is a run, so a later draw this frame follows the ones committed before, and the pass records them all.
			const auto frame = ::RenderGraph::Frame::Driver::Current().frame;
			std::uint32_t first = 0;
			if (run && FrameAnchors::TapInstance() > 0 && client->committedFrame == frame && client->committed) {
				const auto& earlier = *client->committed;
				if (earlier.headerIndices != run->headerIndices || earlier.draws.size() + run->draws.size() > kMaxDraws) {
					logger::error("[DCLF] precipitation: a draw in one stage run {}; it draws nothing",
						earlier.headerIndices != run->headerIndices ? "reads other headers than the frame's earlier ones" : "is past the draws a run holds");
					return;
				}
				auto merged = std::make_shared<Run>();
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
			if (!run)
				return;
			for (std::uint32_t i = 0; i < run->draws.size(); ++i) {
				const auto& draw = run->draws[i];
				client->latch->WriteValue(latchRun, (first + i) * kDrawStride, draw.latch);
				if (draw.program)
					client->gate->OpenDraw(first + i, draw.indexCount, 1);
			}
		};
		desc.repeats = true;
		desc.segmentOnly = FrameAnchors::NamedByStage(desc.id);
		desc.executed = [client](const FrameAnchors::Point&) {
			if (client->executed.fetch_add(1, std::memory_order_relaxed) == 0)
				logger::info("[DCLF] precipitation: the first precipitation run drawn by the graph");
		};
		desc.active = [client] { return client->pending; };
		if (!FrameAnchors::Register(std::move(desc))) {
			logger::warn("[DCLF] precipitation: the {} client could not be registered; the precipitation stays the engine's", kClientId);
			return true;
		}
		const auto late = client->inputs.Unregistered();
		logger::info("[DCLF] precipitation: the {} client registered at setup (published inputs it may read: {}{})", kClientId, client->inputs.Describe(),
			late.empty() ? std::string() : std::format("; not published yet: {}", late));
		state.client = std::move(client);
		return true;
	}

	bool CanTake()
	{
		auto& state = GetState();
		if (!state.client)
			return false;
		return Family::HeldForBatch("precipitation", kClientId, static_cast<std::uint32_t>(::RenderGraph::Frame::SegmentId::Sun));
	}

	Family::InputResult ReadPublished(std::string_view a_id, std::uint32_t& a_index)
	{
		const auto& client = GetState().client;
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
		if (!a_run || !state.client)
			return false;
		a_run->revision = RevisionOf(*a_run);
		state.kept.Keep(std::move(a_run->owners));
		++state.runs;
		state.draws += a_run->draws.size();
		auto& client = *state.client;
		std::atomic_store(&client.run, std::shared_ptr<const Run>(std::move(a_run)));
		return Family::RunWithheld("precipitation", kClientId, FrameAnchors::Anchor::EngineDraws, client.pending);
	}

	std::string Status()
	{
		const auto& state = GetState();
		if (!Enabled())
			return "off (CS_ORG_PRECIPITATION)";
		return fmt::format("{} runs ({} drawn), {} draws, {} programs", state.runs, state.client ? state.client->executed.load(std::memory_order_relaxed) : 0,
			state.draws, state.programs.Built());
	}
}
