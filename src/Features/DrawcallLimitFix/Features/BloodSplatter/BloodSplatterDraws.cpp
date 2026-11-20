// The blood splatter render graph side. One client at the EngineDraws anchor draws the latched draws into the back buffer.
// It is the UI stage's first step, and its pass records at execution because the draws are only known then.
// The client is declared on the display size and the presenter's format. A new back buffer slot registers it again with the
// next frame revision (frame revision plan, 6.19).

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/BloodSplatter/BloodSplatter.h"

#include "Globals.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Frame/Declare.h"
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
#include <optional>

namespace DCLF::Features::BloodSplatter::BloodSplatterDraws
{
	namespace
	{
		constexpr const char* kShader = "BloodSplatter.hlsl";
		constexpr const char* kClientId = "cs.blood-splatter";
		constexpr std::uint32_t kMaxDraws = 32;
		constexpr std::uint32_t kDrawStride = static_cast<std::uint32_t>(sizeof(DrawConstants));

		/** @brief Must match BLOOD_SPLATTER_ORG_RESOURCES in BloodSplatter.hlsl. */
		struct BloodBlock
		{
			ShaderBindings::Latch draw;
		};
		static_assert(sizeof(BloodBlock) == 8, "BloodSplatter.hlsl's push block is 8 bytes");

		struct Client
		{
			std::shared_ptr<org::LatchBlock> latch;
			// Only this client's commit opens the gate, so another client's run of the shared anchor
			// cannot redraw this client's older run.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			std::shared_ptr<const Run> run;
			// Keyed by run, because the pass may record on the submission thread after the render thread published the next run.
			::RenderGraph::RunSlots<Run> runs;
			bool pending = false;
			std::atomic<std::uint64_t> executed{ 0 };
		};

		/**
		* @brief What a registration is made for: the back buffer's graph slot. A new swap chain image of the same format is the
		* slot's next version and keeps the key. Another format, or a native back buffer made again, is a new slot.
		*/
		struct TargetKey
		{
			FrameAnchors::Texture target;
			bool operator==(const TargetKey&) const = default;
		};

		struct Bindings
		{
			org::DeclaredViewToken target{};
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;  // null when the pass records at execution
			std::shared_ptr<const Client> client;
		};

		/** @brief One render pass and one draw per latched draw, in the engine's order. */
		class BloodPass final : public org::TypedRenderGraphPass<BloodPass, Frame, Bindings>
		{
		public:
			BloodPass(std::shared_ptr<Client> a_client, FrameAnchors::Texture a_target, bool a_atExecution) :
				client(std::move(a_client)), target(std::move(a_target)), atExecution(a_atExecution) {}

			Bindings Declare(org::PassBuilder& a_builder) { return { a_builder.RenderTarget(target).View() }; }

			// Recorded at execution, so the run is not part of the revision.
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

			/** @brief A run whose commit opened no gate entries records only the pass's barriers. */
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
												  // The counts come from the gate, which this client's commit opens for this run only.
												  .indirect = &c.gate->Latch(),
												  .indirectOffset = c.gate->Offset(i) });
				}
			}

		private:
			std::shared_ptr<Client> client;
			FrameAnchors::Texture target;  // the registration's back buffer slot
			bool atExecution = false;
		};

		// Every BSBloodSplatterShader pixel descriptor CS loads gets a program.
		void StagesOf(bool a_pixel, std::uint32_t a_descriptor, std::vector<Family::StageRequest>& a_out)
		{
			if (a_pixel)
				a_out.push_back({ .descriptor = a_descriptor, .stage = Family::DefineStage::Pixel });
		}

		struct DrawsState
		{
			std::shared_ptr<Client> client;
			::RenderGraph::Frame::DeclaredClient declared;
			bool tried = false;
			Family::EngineDrawPrograms programs{ "blood splatter", { .source = kShader,
																	   .constantWords = ShaderBindings::Words<BloodBlock>(),
																	   .shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::BloodSplatter),
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

		/** @brief The registration for a_target. False, logged, when the back buffer was not imported. */
		bool MakeDesc(const std::shared_ptr<Client>& a_client, const FrameAnchors::Texture& a_target, FrameAnchors::ClientDesc& a_desc)
		{
			if (!a_target) {
				logger::error("[DCLF] blood splatter: the back buffer cannot be imported for the {} client", kClientId);
				return false;
			}
			a_desc.id = kClientId;
			a_desc.gates = { a_client->gate };
			a_desc.passes.push_back({ .name = std::string(kClientId) + ".draws",
				.anchor = FrameAnchors::Anchor::EngineDraws,
				.order = 2,
				.kind = FrameAnchors::PassKind::Render,
				.make = [client = a_client, target = a_target]() -> std::shared_ptr<org::RenderPass> {
					return std::make_shared<BloodPass>(client, target, FrameAnchors::RecordsAtExecution(FrameAnchors::Anchor::EngineDraws));
				},
				// The draws are only known after the commit.
				.atExecution = true });
			a_desc.commit = [client = a_client](const FrameAnchors::Point&) {
				const auto run = std::atomic_load(&client->run);
				auto* host = RenderGraphRuntime::Get().Host();
				if (!host)
					return;
				const org::RunRef latchRun = host->CurrentRun();
				// Null means nothing is drawn this run.
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
			a_desc.executed = [client = a_client](const FrameAnchors::Point&) {
				if (client->executed.fetch_add(1, std::memory_order_relaxed) == 0)
					logger::info("[DCLF] blood splatter: the first screen blood run drawn by the graph");
			};
			a_desc.active = [client = a_client] { return client->pending; };
			// The anchor has no epoch of its own.
			a_desc.segmentOnly = FrameAnchors::NamedByStage(kClientId);
			logger::info("[DCLF] blood splatter: the {} client is made for the back buffer {}", kClientId, a_target.Id());
			return true;
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
			logger::warn("[DCLF] blood splatter: the EngineDraws anchor could not be reserved; the screen blood stays the engine's");
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
		if (!FrameAnchors::EngineTarget(0)) {
			logger::warn("[DCLF] blood splatter: the back buffer cannot be imported; the screen blood stays the engine's");
			return true;
		}
		namespace RF = ::RenderGraph::Frame;
		// The key imports the back buffer when a revision starts. A revision that carries a new slot registers the client again
		// and waits for the UI stage's build, so a batch never meets a graph built for another back buffer.
		state.declared = RF::DeclareClient<TargetKey>(
			kClientId, RF::DependsOn(RF::Vars::DisplaySize(), RF::Vars::PresenterFormat()),
			[](const RF::Inputs&) -> std::optional<TargetKey> { return TargetKey{ FrameAnchors::EngineTarget(0) }; },
			[client](const TargetKey& a_key, FrameAnchors::ClientDesc& a_desc) { return MakeDesc(client, a_key.target, a_desc); });
		if (!state.declared) {
			logger::warn("[DCLF] blood splatter: the {} client could not be declared; the screen blood stays the engine's", kClientId);
			return true;
		}
		state.client = std::move(client);
		logger::info("[DCLF] blood splatter: the {} client declared at setup; it registers with the next frame revision", kClientId);
		return true;
	}

	bool CanTake()
	{
		const auto& state = GetState();
		// Until a revision registers the client, the engine draws the batch.
		if (!state.client || !state.declared.Registered())
			return false;
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
		if (!a_run || !state.client)
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
		return fmt::format("{} runs ({} drawn), {} draws, {} programs", state.runs, state.client ? state.client->executed.load(std::memory_order_relaxed) : 0,
			state.draws, state.programs.Built());
	}
}
