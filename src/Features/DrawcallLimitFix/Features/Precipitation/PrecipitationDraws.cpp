// The precipitation family's render graph side: one client at the EngineDraws anchor, made once at setup, whose pass draws a run's
// latched draws (one each: a run is a draw at its place in the grouped alphas) with CS's Particle.hlsl built for ORG, with the
// engine's depth-stencil state and reference per draw.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/Precipitation/Precipitation.h"

#include "Globals.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/FrameAnchors.h"
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
		// The most draws a run holds: one (a run is one draw), with room for a few.
		constexpr std::uint32_t kMaxDraws = 4;
		constexpr std::uint32_t kDrawStride = static_cast<std::uint32_t>(sizeof(DrawLatch));

		/** @brief Particle.hlsl's push block (PARTICLE_ORG_RESOURCES), in list order: Water.hlsl's layout. */
		struct ParticleBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Latch draw, strictLights;
			std::array<ShaderBindings::Handle, Water::kHeadersBeforeShadow> headers;                                       // t17 to t77
			ShaderBindings::Latch directionalShadow;                                                                       // t98
			std::array<ShaderBindings::Handle, Water::kHeaderRegisters.size() - Water::kHeadersBeforeShadow> lateHeaders;  // t102, t103
		};
		static_assert(sizeof(ParticleBlock) == 112, "Particle.hlsl's push block is 112 bytes");

		/** @brief The client's state, which its pass shares. */
		struct Client
		{
			std::shared_ptr<org::LatchBlock> latch;
			// One draw entry per latched draw. The anchor closes it at every run, and only this client's commit opens it, so a run of
			// the anchor that another client keeps draws nothing of this client's older run.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			// The targets as imported at the registration (kTargets; null where a second target could not be imported).
			std::array<std::shared_ptr<org::ExternalTextureResource>, kTargets.size()> colours;
			std::shared_ptr<org::ExternalTextureResource> depth;
			// Every published id the draws can read, fixed here (ListInputs). Each build declares the registered ones.
			Family::PublishedInputs inputs;
			// Render thread writes, the host's thread reads.
			std::shared_ptr<const Run> run;
			bool pending = false;
			std::atomic<std::uint64_t> executed{ 0 };  // runs submitted with this client's pass (its executed callback)
		};

		struct Bindings
		{
			SharedConstants::Bindings shared;
			std::array<org::DeclaredViewToken, kTargets.size()> colours{};
			std::array<bool, kTargets.size()> hasColours{};
			org::DeclaredViewToken depth{};
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;
			std::shared_ptr<const Client> client;
		};

		/** @brief A run's draws, in the engine's order: a render pass and a draw each (RasterProgram::Draw). */
		class PrecipitationPass final : public org::TypedRenderGraphPass<PrecipitationPass, Frame, Bindings>
		{
		public:
			explicit PrecipitationPass(std::shared_ptr<Client> a_client) :
				client(std::move(a_client)) {}

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
				// The published inputs, declared for their order only: the run's header indices and the draws' latches carry their indices.
				client->inputs.Declare(a_builder);
				return bindings;
			}

			// The run's draws (their programs, targets, stencil references and viewports) and the headers' indices.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				const auto run = std::atomic_load(&client->run);
				a_out.push_back(run ? run->revision : 0);
			}

			Frame Prepare(const Bindings&, const org::PassPrepareContext&) const { return { std::atomic_load(&client->run), client }; }

			static void Record(const Bindings& a_bindings, const Frame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.run || !a_frame.client)
					return;
				const auto& run = *a_frame.run;
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
												  // The counts come from the draw's gate entry, which the client's commit opens for this run only.
												  .indirect = &c.gate->Latch(),
												  .indirectOffset = c.gate->Offset(i) });
				}
			}

		private:
			std::shared_ptr<Client> client;
		};

		/** @brief The ENVCUBE pixel descriptors CS loads for BSParticleShader are programs' (both stages take their defines). */
		void StagesOf(bool a_pixel, std::uint32_t a_descriptor, std::vector<Family::StageRequest>& a_out)
		{
			if (a_pixel && PrecipitationTechnique(a_descriptor))
				a_out.push_back({ .descriptor = a_descriptor, .stage = Family::DefineStage::Pixel });
		}

		/** @brief Everything here: the render thread's. */
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
			bool failed = false;
			std::uint64_t runs = 0, draws = 0;
			Family::KeptOwners kept;
		};

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

		/** @brief Stops the family for good: every draw is the engine's again. */
		void Fail(const char* a_why)
		{
			auto& state = GetState();
			if (!state.failed)
				logger::error("[DCLF] precipitation: back to the engine's draws for the session: {}", a_why);
			state.failed = true;
			FrameAnchors::Unregister(kClientId);
		}

		/**
		 * @brief Every published id the draws can read, listed when the client is made at setup: what the ported features publish at the
		 * headers' registers and at t80 (DCLF's registry), the engine depths CS binds at a header (the scene depth at t17, Wetness Effects'
		 * occlusion at t70), and the engine targets the slots hold (the precipitation occlusion map at t2, the underwater mask at t3),
		 * imported now so the first build declares them. An export of an id not listed here is an inherent refusal, logged once with the id.
		 */
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

		/** @brief What a run's recording depends on: everything of its draws but the latched values and counts, and the headers' indices. */
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
		return state.reserved && !state.failed && RenderGraphRuntime::Get().IsActive() && FrameAnchors::HasEpoch(FrameAnchors::Anchor::EngineDraws);
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
		if (state.tried || state.failed)
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
			.make = [client]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<PrecipitationPass>(client); } });
		desc.commit = [client](const FrameAnchors::Point&) {
			const auto run = std::atomic_load(&client->run);
			auto* host = RenderGraphRuntime::Get().Host();
			if (!run || !host)
				return;
			const org::RunRef latchRun = host->CurrentRun();
			for (std::uint32_t i = 0; i < run->draws.size(); ++i) {
				const auto& draw = run->draws[i];
				client->latch->WriteValue(latchRun, i * kDrawStride, draw.latch);
				if (draw.program)
					client->gate->OpenDraw(i, draw.indexCount, 1);
			}
		};
		desc.executed = [client](const FrameAnchors::Point&) {
			if (client->executed.fetch_add(1, std::memory_order_relaxed) == 0)
				logger::info("[DCLF] precipitation: the first precipitation run drawn by the graph");
		};
		desc.active = [client] { return client->pending; };
		desc.onFailed = [] { Fail("the render graph failed with the precipitation's pass in it"); };
		if (!FrameAnchors::Register(std::move(desc))) {
			logger::warn("[DCLF] precipitation: the {} client could not be registered; the precipitation stays the engine's", kClientId);
			return true;
		}
		const auto late = client->inputs.Unregistered();
		logger::info("[DCLF] precipitation: the {} client registered at setup (published inputs it may read: {}{})", kClientId, client->inputs.Describe(),
			late.empty() ? std::string() : std::format("; not published yet: {}", late));
		state.client = std::move(client);
		// The one build request: the next build point (a menu frame's end) builds the graph with the client, before any world frame.
		FrameAnchors::RequestJoin(kClientId, FrameAnchors::Anchor::EngineDraws);
		return true;
	}

	bool CanTake()
	{
		auto& state = GetState();
		if (state.failed || !state.client)
			return false;
		if (!FrameAnchors::CanRun(kClientId, FrameAnchors::Anchor::EngineDraws)) {
			static Util::ReasonLog log("[DCLF] precipitation: the engine draws a draw", spdlog::level::err);
			log.Report("its anchor cannot run now (not in the built graph, or its ring is full)");
			return false;
		}
		return true;
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
		if (!a_run || state.failed || !state.client)
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
		if (state.failed)
			return "failed: the engine draws the precipitation";
		return fmt::format("{} runs ({} drawn), {} draws, {} programs", state.runs, state.client ? state.client->executed.load(std::memory_order_relaxed) : 0,
			state.draws, state.programs.Built());
	}
}
