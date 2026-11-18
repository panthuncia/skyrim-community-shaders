// The blood splatter family's render graph side: one client at the EngineDraws anchor, made once at setup (the first main-thread tick
// the graph runs), whose pass draws a batch's latched draws into the back buffer with CS's BloodSplatter.hlsl built for ORG. Native
// mode: the client is the UI stage's first step (Main::RenderScreenSplatter comes after the post stage's end and before the displays),
// and its pass records the batch at execution. Guest mode: the anchor's own epoch.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/BloodSplatter/BloodSplatter.h"

#include "Globals.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "Utils/Logging.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace DCLF::Features::BloodSplatter::BloodSplatterDraws
{
	namespace
	{
		constexpr const char* kShader = "BloodSplatter.hlsl";
		constexpr const char* kClientId = "cs.blood-splatter";
		constexpr std::uint32_t kMaxDraws = 32;
		constexpr std::uint32_t kDrawStride = static_cast<std::uint32_t>(sizeof(DrawConstants));

		/** @brief BloodSplatter.hlsl's push block (BLOOD_SPLATTER_ORG_RESOURCES). */
		struct BloodBlock
		{
			ShaderBindings::Latch draw;
		};
		static_assert(sizeof(BloodBlock) == 8, "BloodSplatter.hlsl's push block is 8 bytes");

		struct Client
		{
			std::shared_ptr<org::LatchBlock> latch;
			// One draw entry per latched draw. The anchor closes it at every run, and only this client's commit opens it, so a run of
			// the anchor that another client keeps draws nothing of this client's older run.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			// The back buffer as last imported (Target). A swap chain made again with the same format is the slot's next version; another
			// format is a new slot, taken at the next batch (CanTake) and declared from the next build.
			FrameAnchors::Texture target;
			mutable std::mutex targetMutex;  // the render thread replaces target, a build reads it
			std::shared_ptr<const Run> run;
			// The run each commit drew, by the epoch's run: what the pass recorded at execution reads. It may record on the submission
			// thread after the render thread published the next run.
			::RenderGraph::RunSlots<Run> runs;
			bool pending = false;

			FrameAnchors::Texture Target() const
			{
				std::scoped_lock lock(targetMutex);
				return target;
			}
			std::atomic<std::uint64_t> executed{ 0 };  // runs submitted with this client's pass (its executed callback)
		};

		struct Bindings
		{
			org::DeclaredViewToken target{};
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;  // null when the pass records at execution: Record reads the client's run then
			std::shared_ptr<const Client> client;
		};

		/** @brief A run's draws, in the engine's order: a render pass and a draw each. */
		class BloodPass final : public org::TypedRenderGraphPass<BloodPass, Frame, Bindings>
		{
		public:
			BloodPass(std::shared_ptr<Client> a_client, bool a_atExecution) :
				client(std::move(a_client)), atExecution(a_atExecution) {}

			Bindings Declare(org::PassBuilder& a_builder) { return { a_builder.RenderTarget(client->Target()).View() }; }

			// Recorded at execution, the pass reads the run its commit published when it records: the run is not part of it.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				if (atExecution)
					return;
				const auto run = std::atomic_load(&client->run);
				a_out.push_back(run ? run->revision : 0);
			}

			Frame Prepare(const Bindings&, const org::PassPrepareContext&) const
			{
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
				const auto run = a_frame.run ? a_frame.run : a_frame.client->runs.Find(a_recording.Run());
				if (!run)
					return;
				const auto& c = *a_frame.client;
				const org::RunRef latchRun = a_recording.Run();
				for (std::uint32_t i = 0; i < run->draws.size(); ++i) {
					const auto& draw = run->draws[i];
					if (!draw.program)
						continue;
					const auto& program = *static_cast<const RasterProgram*>(draw.program.get());
					const BloodBlock block{ ShaderBindings::LatchOf(*c.latch, latchRun, i * kDrawStride) };
					const RasterProgram::Colour colour{ .view = &a_bindings.target };
					program.Draw(a_recording, { .colours = std::span<const RasterProgram::Colour>(&colour, 1),
												  .viewport = { .x = draw.viewport.rect[0], .y = draw.viewport.rect[1], .width = draw.viewport.rect[2], .height = draw.viewport.rect[3] },
												  .constants = ShaderBindings::Data(block),
												  .debugName = "BloodSplatter",
												  // The counts come from the draw's gate entry, which the client's commit opens for this run only.
												  .indirect = &c.gate->Latch(),
												  .indirectOffset = c.gate->Offset(i) });
				}
			}

		private:
			std::shared_ptr<Client> client;
			bool atExecution = false;  // FrameAnchors::RecordsAtExecution at the build
		};

		/** @brief Every pixel descriptor of BSBloodSplatterShader CS loads is a program's (the splatter and the flare). */
		void StagesOf(bool a_pixel, std::uint32_t a_descriptor, std::vector<Family::StageRequest>& a_out)
		{
			if (a_pixel)
				a_out.push_back({ .descriptor = a_descriptor, .stage = Family::DefineStage::Pixel });
		}

		struct DrawsState
		{
			std::shared_ptr<Client> client;
			bool tried = false;  // the client is made once
			Family::EngineDrawPrograms programs{ "blood splatter", { .source = kShader,
																	   .constantWords = ShaderBindings::Words<BloodBlock>(),
																	   .shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::BloodSplatter),
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

		void Fail(const char* a_why)
		{
			auto& state = GetState();
			if (!state.failed)
				logger::error("[DCLF] blood splatter: back to the engine's draws for the session: {}", a_why);
			state.failed = true;
			FrameAnchors::Unregister(kClientId);
		}

		std::uint64_t RevisionOf(const Run& a_run)
		{
			std::uint64_t hash = 1469598103934665603ull;
			const auto mix = [&](std::uint64_t a_value) { hash = (hash ^ a_value) * 1099511628211ull; };
			mix(a_run.draws.size());
			for (const auto& draw : a_run.draws) {
				mix(reinterpret_cast<std::uintptr_t>(draw.program.get()));
				for (const auto v : draw.viewport.rect)
					mix(v);
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
			logger::warn("[DCLF] blood splatter: the EngineDraws anchor could not be reserved; the screen blood stays the engine's");
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
			logger::warn("[DCLF] blood splatter: needs the engine constants capture (CS_ORG_ENGINE_CONSTANTS=1); the screen blood stays the engine's");
			return true;
		}
		auto client = std::make_shared<Client>();
		client->latch = std::make_shared<org::LatchBlock>(std::string(kClientId) + ".latch", kMaxDraws * kDrawStride);
		client->gate = ::RenderGraph::RunGate::Create(std::string(kClientId) + ".gate", kMaxDraws);
		if (!client->gate) {
			logger::warn("[DCLF] blood splatter: the run gate could not be made; the screen blood stays the engine's");
			return true;
		}
		client->target = FrameAnchors::EngineTarget(0);
		if (!client->target) {
			logger::warn("[DCLF] blood splatter: the back buffer cannot be imported; the screen blood stays the engine's");
			return true;
		}
		FrameAnchors::ClientDesc desc{};
		desc.id = kClientId;
		desc.gates = { client->gate };
		desc.passes.push_back({ .name = std::string(kClientId) + ".draws",
			.anchor = FrameAnchors::Anchor::EngineDraws,
			.order = 2,
			.kind = FrameAnchors::PassKind::Render,
			.make = [client]() -> std::shared_ptr<org::RenderPass> {
				return std::make_shared<BloodPass>(client, FrameAnchors::RecordsAtExecution(FrameAnchors::Anchor::EngineDraws));
			},
			// The run's draws are known after its commit (a frame input of the UI stage): recorded then.
			.atExecution = true });
		desc.commit = [client](const FrameAnchors::Point&) {
			const auto run = std::atomic_load(&client->run);
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return;
			const org::RunRef latchRun = host->CurrentRun();
			// For the pass recorded at execution, by this run (null: nothing drawn).
			client->runs.Store(latchRun, run);
			if (!run)
				return;
			for (std::uint32_t i = 0; i < run->draws.size(); ++i) {
				const auto& draw = run->draws[i];
				client->latch->WriteValue(latchRun, i * kDrawStride, draw.constants);
				if (draw.program)
					client->gate->OpenDraw(i, draw.indexCount, 1);
			}
		};
		desc.executed = [client](const FrameAnchors::Point&) {
			if (client->executed.fetch_add(1, std::memory_order_relaxed) == 0)
				logger::info("[DCLF] blood splatter: the first screen blood run drawn by the graph");
		};
		desc.active = [client] { return client->pending; };
		// Native mode: a step of the UI stage only (the anchor has no epoch there).
		desc.segmentOnly = FrameAnchors::NamedByStage(desc.id);
		desc.onFailed = [] { Fail("the render graph failed with the screen blood's pass in it"); };
		if (!FrameAnchors::Register(std::move(desc))) {
			logger::warn("[DCLF] blood splatter: the {} client could not be registered; the screen blood stays the engine's", kClientId);
			return true;
		}
		state.client = std::move(client);
		// Register asked for the builds that hold the client: the UI stage's in native mode, the anchor's own graph in guest mode.
		logger::info("[DCLF] blood splatter: the {} client registered at setup", kClientId);
		return true;
	}

	bool CanTake()
	{
		auto& state = GetState();
		const auto& client = state.client;
		if (state.failed || !client)
			return false;
		// A swap chain made again keeps the back buffer's slot (its next version). One of another format is a new slot: the client takes
		// it and is built again with it (the import already asked every graph holding the old slot to be built again), and its batches
		// wait for that build's attach below.
		if (auto current = FrameAnchors::EngineTarget(0); current && current != client->Target()) {
			{
				std::scoped_lock lock(client->targetMutex);
				client->target = std::move(current);
			}
			FrameAnchors::Refresh(kClientId);
			logger::info("[DCLF] blood splatter: the back buffer is another graph resource (a format change); the client is built again with it");
		}
		return Family::HeldForBatch("blood splatter", kClientId, static_cast<std::uint32_t>(::RenderGraph::Frame::SegmentId::Ui));
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
		return Family::RunWithheld("blood splatter", kClientId, FrameAnchors::Anchor::EngineDraws, client.pending);
	}

	std::string Status()
	{
		const auto& state = GetState();
		if (!Enabled())
			return "off (CS_ORG_BLOOD_SPLATTER)";
		if (state.failed)
			return "failed: the engine draws the screen blood";
		return fmt::format("{} runs ({} drawn), {} draws, {} programs", state.runs, state.client ? state.client->executed.load(std::memory_order_relaxed) : 0,
			state.draws, state.programs.Built());
	}
}
