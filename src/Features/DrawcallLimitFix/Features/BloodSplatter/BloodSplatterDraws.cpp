// The blood splatter family's render graph side: one client at the EngineDraws anchor, made once at the first main-thread tick the
// graph runs, whose pass draws a batch's latched draws into the back buffer with CS's BloodSplatter.hlsl built for ORG.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/BloodSplatter/BloodSplatter.h"

#include "Globals.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>

#include <atomic>
#include <cstring>

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
			std::shared_ptr<org::ExternalTextureResource> target;  // the back buffer, as imported at the registration
			std::shared_ptr<const Run> run;
			bool pending = false;
			bool targetLogged = false;
			std::atomic<std::uint64_t> executed{ 0 };  // runs submitted with this client's pass (its executed callback)
		};

		struct Bindings
		{
			org::DeclaredViewToken target{};
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;
			std::shared_ptr<const Client> client;
		};

		/** @brief A run's draws, in the engine's order: a render pass and a draw each. */
		class BloodPass final : public org::TypedRenderGraphPass<BloodPass, Frame, Bindings>
		{
		public:
			explicit BloodPass(std::shared_ptr<Client> a_client) :
				client(std::move(a_client)) {}

			Bindings Declare(org::PassBuilder& a_builder) { return { a_builder.RenderTarget(client->target).View() }; }

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
				const auto& c = *a_frame.client;
				const std::uint32_t slot = a_recording.FrameSlot();
				for (std::uint32_t i = 0; i < a_frame.run->draws.size(); ++i) {
					const auto& draw = a_frame.run->draws[i];
					if (!draw.program)
						continue;
					const auto& program = *static_cast<const RasterProgram*>(draw.program.get());
					const BloodBlock block{ ShaderBindings::LatchOf(*c.latch, slot, i * kDrawStride) };
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
		};

		struct DrawsState
		{
			std::shared_ptr<Client> client;
			bool tried = false;  // the client is made once
			Family::EngineDrawPrograms programs{ "blood splatter", { .source = kShader, .constantWords = ShaderBindings::Words<BloodBlock>() } };
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

	void EnsureClient()
	{
		auto& state = GetState();
		if (state.tried || state.failed)
			return;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		state.tried = true;
		auto client = std::make_shared<Client>();
		client->latch = std::make_shared<org::LatchBlock>(std::string(kClientId) + ".latch", kMaxDraws * kDrawStride, host->FrameSlots());
		client->gate = ::RenderGraph::RunGate::Create(std::string(kClientId) + ".gate", kMaxDraws);
		if (!client->gate) {
			logger::warn("[DCLF] blood splatter: the run gate could not be made; the screen blood stays the engine's");
			return;
		}
		client->target = FrameAnchors::EngineTarget(0);
		if (!client->target) {
			logger::warn("[DCLF] blood splatter: the back buffer cannot be imported; the screen blood stays the engine's");
			return;
		}
		FrameAnchors::ClientDesc desc{};
		desc.id = kClientId;
		desc.gates = { client->gate };
		desc.passes.push_back({ .name = std::string(kClientId) + ".draws",
			.anchor = FrameAnchors::Anchor::EngineDraws,
			.order = 2,
			.kind = FrameAnchors::PassKind::Render,
			.make = [client]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<BloodPass>(client); } });
		desc.commit = [client](const FrameAnchors::Point&) {
			const auto run = std::atomic_load(&client->run);
			auto* host = RenderGraphRuntime::Get().Host();
			if (!run || !host)
				return;
			const std::uint32_t slot = host->CurrentFrameSlot();
			for (std::uint32_t i = 0; i < run->draws.size(); ++i) {
				const auto& draw = run->draws[i];
				client->latch->WriteValue(slot, i * kDrawStride, draw.constants);
				if (draw.program)
					client->gate->OpenDraw(i, draw.indexCount, 1);
			}
		};
		desc.executed = [client](const FrameAnchors::Point&) {
			if (client->executed.fetch_add(1, std::memory_order_relaxed) == 0)
				logger::info("[DCLF] blood splatter: the first screen blood run drawn by the graph");
		};
		desc.active = [client] { return client->pending; };
		desc.onFailed = [] { Fail("the render graph failed with the screen blood's pass in it"); };
		if (!FrameAnchors::Register(std::move(desc))) {
			logger::warn("[DCLF] blood splatter: the {} client could not be registered; the screen blood stays the engine's", kClientId);
			return;
		}
		state.client = std::move(client);
		// Asks for the build now, so the client is in the graph before the first hit.
		FrameAnchors::RequestJoin(kClientId, FrameAnchors::Anchor::EngineDraws);
		logger::info("[DCLF] blood splatter: the {} client registered", kClientId);
	}

	bool Ready()
	{
		auto& state = GetState();
		const auto& client = state.client;
		if (state.failed || !client)
			return false;
		// The back buffer the engine binds now must be the one the client imported: a new swap chain keeps the screen blood the engine's
		// (no client is registered again during play).
		if (FrameAnchors::EngineTarget(0) != client->target) {
			if (!std::exchange(client->targetLogged, true))
				logger::warn("[DCLF] blood splatter: the back buffer changed since the client registered; the screen blood stays the engine's");
			return false;
		}
		return FrameAnchors::RequestJoin(kClientId, FrameAnchors::Anchor::EngineDraws);
	}

	bool CanRun()
	{
		return GetState().client && FrameAnchors::CanRun(kClientId, FrameAnchors::Anchor::EngineDraws);
	}

	std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader, Family::ProgramState& a_state)
	{
		return GetState().programs.Get(a_key, a_shader, {}, a_state);
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
