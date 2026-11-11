// The shadow mask family's render graph side: a client per view at the EngineDraws anchor, made once, whose pass draws a batch's latched
// draws into kSHADOW_MASK in the engine's order with CS's Utility.hlsl built for ORG (its shadow mask techniques).

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/ShadowMask/ShadowMask.h"

#include "Features/TerrainBlending/ORGTerrainBlending.h"
#include "Globals.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "ShaderCache.h"
#include "Utils/Logging.h"
#include "RenderGraph/FrameAnchors.h"
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

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <string_view>

namespace DCLF::Features::ShadowMask::ShadowMaskDraws
{
	namespace
	{
		constexpr const char* kShader = "Utility.hlsl";
		// The most draws a batch latches (ShadowMaskCapture's kMaxLatched).
		constexpr std::uint32_t kMaxDraws = 32;
		constexpr std::uint32_t kDrawStride = static_cast<std::uint32_t>(sizeof(DrawConstants));
		constexpr std::size_t kViews = static_cast<std::size_t>(View::Count);

		/** @brief Utility.hlsl's push block (UTILITY_ORG_RESOURCES), in list order. */
		struct ShadowMaskBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Latch draw;
		};
		static_assert(sizeof(ShadowMaskBlock) == 24, "Utility.hlsl's ORG push block is 24 bytes");

		/** @brief A client's state, which its pass shares. */
		struct Client
		{
			Shape shape;
			std::string id;
			std::shared_ptr<org::LatchBlock> latch;
			// One draw entry per latched draw. The anchor closes it at every run, and only this client's commit opens it, so a run of
			// the anchor that another client keeps draws nothing of this client's older run.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			std::shared_ptr<org::ExternalTextureResource> target;  // kSHADOW_MASK, as imported at the registration
			// Every published id the view's draws can read, fixed here (ListInputs). Each build declares the registered ones.
			Family::PublishedInputs inputs;
			// Render thread writes, the host's thread reads.
			std::shared_ptr<const Run> run;
			bool pending = false;
			bool targetLogged = false;
			std::atomic<std::uint64_t> executed{ 0 };  // runs submitted with this client's pass (its executed callback)
		};

		struct Bindings
		{
			SharedConstants::Bindings shared;
			org::DeclaredViewToken target{};
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;
			std::shared_ptr<const Client> client;
		};

		/** @brief A run's draws, recorded in the engine's order: a render pass and a draw each (RasterProgram::Draw). */
		class ShadowMaskPass final : public org::TypedRenderGraphPass<ShadowMaskPass, Frame, Bindings>
		{
		public:
			explicit ShadowMaskPass(std::shared_ptr<Client> a_client) :
				client(std::move(a_client)) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				Bindings bindings{};
				bindings.shared = SharedConstants::Declare(a_builder);
				bindings.target = a_builder.RenderTarget(client->target).View();
				// The published inputs, declared for their order only: the draws' latches carry their indices. Every registered one is
				// declared, read this frame or not, so a new Publish needs no new client.
				client->inputs.Declare(a_builder);
				return bindings;
			}

			// The run's draws (their programs and viewports).
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
				const auto shared = SharedConstants::Index(a_recording, a_bindings.shared);
				const std::uint32_t slot = a_recording.FrameSlot();
				const RasterProgram::Colour colour{ .view = &a_bindings.target };
				for (std::uint32_t i = 0; i < a_frame.run->draws.size(); ++i) {
					const auto& draw = a_frame.run->draws[i];
					if (!draw.program)
						continue;
					const auto& program = *static_cast<const RasterProgram*>(draw.program.get());
					const ShadowMaskBlock block{ shared, ShaderBindings::LatchOf(*c.latch, slot, i * kDrawStride) };
					program.Draw(a_recording, { .colours = std::span<const RasterProgram::Colour>(&colour, 1),
												  .viewport = { .x = draw.viewport.rect[0], .y = draw.viewport.rect[1], .width = draw.viewport.rect[2], .height = draw.viewport.rect[3], .minDepth = draw.viewport.depthRange[0], .maxDepth = draw.viewport.depthRange[1] },
												  .constants = ShaderBindings::Data(block),
												  .debugName = "ShadowMask",
												  // The counts come from the draw's gate entry, which the client's commit opens for this run only.
												  .indirect = &c.gate->Latch(),
												  .indirectOffset = c.gate->Offset(i) });
				}
			}

		private:
			std::shared_ptr<Client> client;
		};

		/**
		 * @brief The shadow mask programs a Utility pixel descriptor CS loads can be drawn with: those with a RenderShadowmask* technique
		 * bit (the world's and the menu scenes' masks: shadow-mask.md, "The shader"). Every other Utility descriptor is DCLF's or the
		 * engine's.
		 */
		void StagesOf(bool a_pixel, std::uint32_t a_descriptor, std::vector<Family::StageRequest>& a_out)
		{
			using Flags = SIE::ShaderCache::UtilityShaderFlags;
			constexpr auto kMasks = static_cast<std::uint32_t>(Flags::RenderShadowmask) | static_cast<std::uint32_t>(Flags::RenderShadowmaskSpot) |
			                        static_cast<std::uint32_t>(Flags::RenderShadowmaskPb) | static_cast<std::uint32_t>(Flags::RenderShadowmaskDpb);
			if (a_pixel && (a_descriptor & kMasks))
				a_out.push_back({ .descriptor = a_descriptor, .stage = Family::DefineStage::Pixel });
		}

		/** @brief Everything here: the render thread's. */
		struct DrawsState
		{
			std::array<std::shared_ptr<Client>, kViews> clients;
			Family::EngineDrawPrograms programs{ "shadow mask", { .source = kShader,
																	.constantWords = ShaderBindings::Words<ShadowMaskBlock>(),
																	.shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::Utility),
																	.stagesOf = &StagesOf } };
			bool reserved = false;
			bool setUp = false;
			bool failed = false;
			std::uint64_t runs = 0, draws = 0;
			Family::KeptOwners kept;
		};

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

		/** @brief Stops the family for good: every shadow mask is the engine's again. */
		void Fail(const char* a_why)
		{
			auto& state = GetState();
			if (!state.failed)
				logger::error("[DCLF] shadow mask: back to the engine's draws for the session: {}", a_why);
			state.failed = true;
			for (auto& client : state.clients)
				if (client)
					FrameAnchors::Unregister(client->id);
		}

		/**
		 * @brief Every published id a view's draws can read, listed when its client is made at setup: the engine depths the slots hold
		 * (shadow-mask.md, "The shader": the scene depth at t2, the light's shadow maps at t3 and t4), imported now so the first build
		 * declares them, and what the ported features publish as the scene depth. Terrain Blending binds its blended depth at t2
		 * ("TerrainBlending::BlendedDepth", R32) and lists its 16-bit copy in DCLF's registry at t17. An export of an id not listed here
		 * is an inherent refusal, logged once with the id: add the id here.
		 */
		void ListInputs(Family::PublishedInputs& a_inputs)
		{
			using Depth = RE::RENDER_TARGET_DEPTHSTENCIL;
			for (const auto depth : { Depth::kMAIN, Depth::kMAIN_COPY, Depth::kPOST_ZPREPASS_COPY, Depth::kSHADOWMAPS, Depth::kSHADOWMAPS_ESRAM,
					 Depth::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM, Depth::kFOCUS_NEO })
				a_inputs.AddEngine(depth);
			a_inputs.Add(ORGTerrainBlending::kBlendedDepthId);
			constexpr std::array<std::uint32_t, 1> kSceneDepthRegister{ 17 };
			a_inputs.Add(Family::RegisteredIdsAt(kSceneDepthRegister));
		}

		/** @brief The client of a_shape's view, registered with the anchor for a_shape. Null, logged, when the target cannot be imported. */
		std::shared_ptr<Client> MakeClient(const Shape& a_shape)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return nullptr;
			auto client = std::make_shared<Client>();
			client->shape = a_shape;
			client->id = std::string("cs.shadow-mask.") + NameOf(a_shape.view);
			client->latch = std::make_shared<org::LatchBlock>(client->id + ".latch", kMaxDraws * kDrawStride, host->FrameSlots());
			client->gate = ::RenderGraph::RunGate::Create(client->id + ".gate", kMaxDraws);
			if (!client->gate) {
				logger::warn("[DCLF] shadow mask: the {} run gate could not be made; those shadow masks stay the engine's", client->id);
				return nullptr;
			}
			client->target = FrameAnchors::EngineTarget(kShadowMaskTarget);
			if (!client->target) {
				logger::warn("[DCLF] shadow mask: kSHADOW_MASK cannot be imported; the {} stays the engine's", client->id);
				return nullptr;
			}
			ListInputs(client->inputs);

			FrameAnchors::ClientDesc desc{};
			desc.id = client->id;
			desc.sharedConstants = true;
			desc.gates = { client->gate };
			desc.passes.push_back({ .name = client->id + ".draws",
				.anchor = FrameAnchors::Anchor::EngineDraws,
				.order = a_shape.view == View::World ? 3 : 4,
				.kind = FrameAnchors::PassKind::Render,
				.make = [client]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ShadowMaskPass>(client); } });
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
					logger::info("[DCLF] shadow mask: the first {} run drawn by the graph", client->id);
			};
			desc.active = [client] { return client->pending; };
			desc.onFailed = [] { Fail("the render graph failed with the shadow masks' passes in it"); };
			if (!FrameAnchors::Register(std::move(desc))) {
				logger::warn("[DCLF] shadow mask: the {} client could not be registered; those shadow masks stay the engine's", client->id);
				return nullptr;
			}
			const auto late = client->inputs.Unregistered();
			logger::info("[DCLF] shadow mask: the {} client registered at setup (published inputs it may read: {}{})", client->id, client->inputs.Describe(),
				late.empty() ? std::string() : std::format("; not published yet: {}", late));
			// The one build request: the next build point (a menu frame's end) builds the graph with the client, before any world frame.
			FrameAnchors::RequestJoin(client->id, FrameAnchors::Anchor::EngineDraws);
			return client;
		}

		/** @brief What a run's recording depends on: its draws' programs and viewports (the latched values and counts are not). */
		std::uint64_t RevisionOf(const Run& a_run)
		{
			std::uint64_t hash = 1469598103934665603ull;
			const auto mix = [&](std::uint64_t a_value) { hash = (hash ^ a_value) * 1099511628211ull; };
			mix(static_cast<std::uint64_t>(a_run.view));
			mix(a_run.draws.size());
			for (const auto& draw : a_run.draws) {
				mix(reinterpret_cast<std::uintptr_t>(draw.program.get()));
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
			logger::warn("[DCLF] shadow mask: the EngineDraws anchor could not be reserved; the shadow masks stay the engine's");
	}

	bool Setup()
	{
		auto& state = GetState();
		if (state.setUp || state.failed)
			return true;
		if (!RenderGraphRuntime::Get().Host() || !FrameAnchors::IsRenderOwner() || !Available())
			return false;
		state.setUp = true;
		Family::StartStagePrecompile();
		state.programs.Prebuild();
		if (!::RenderGraph::Engine::Constants::Requested()) {
			logger::warn("[DCLF] shadow mask: needs the engine constants capture (CS_ORG_ENGINE_CONSTANTS=1); the shadow masks stay the engine's");
			return true;
		}
		for (const auto view : { View::World, View::Menu })
			if (TakesView(view))
				state.clients[static_cast<std::size_t>(view)] = MakeClient({ .view = view });
		return true;
	}

	bool CanTake(View a_view)
	{
		auto& state = GetState();
		if (state.failed || a_view >= View::Count)
			return false;
		const auto& client = state.clients[static_cast<std::size_t>(a_view)];
		if (!client)
			return false;
		// kSHADOW_MASK made again (a resolution change) is another import: those batches stay the engine's until the next start (no
		// client is registered again during play).
		if (FrameAnchors::EngineTarget(kShadowMaskTarget) != client->target) {
			if (!std::exchange(client->targetLogged, true))
				logger::warn("[DCLF] shadow mask: kSHADOW_MASK changed since the {} client registered; those batches stay the engine's", client->id);
			return false;
		}
		if (!FrameAnchors::CanRun(client->id, FrameAnchors::Anchor::EngineDraws)) {
			static Util::ReasonLog log("[DCLF] shadow mask: the engine draws a batch", spdlog::level::err);
			log.Report(std::format("{}: its anchor cannot run now (not in the built graph, or its ring is full)", client->id));
			return false;
		}
		return true;
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
		if (!a_run || state.failed)
			return false;
		const auto& client = state.clients[static_cast<std::size_t>(a_run->view)];
		if (!client)
			return false;
		a_run->revision = RevisionOf(*a_run);
		state.kept.Keep(std::move(a_run->owners));
		++state.runs;
		state.draws += a_run->draws.size();
		std::atomic_store(&client->run, std::shared_ptr<const Run>(std::move(a_run)));
		return Family::RunWithheld("shadow mask", client->id, FrameAnchors::Anchor::EngineDraws, client->pending);
	}

	std::string Status()
	{
		const auto& state = GetState();
		if (!Enabled())
			return "off (CS_ORG_SHADOW_MASK)";
		if (state.failed)
			return "failed: the engine draws the shadow masks";
		std::uint64_t executed = 0;
		for (const auto& client : state.clients)
			if (client)
				executed += client->executed.load(std::memory_order_relaxed);
		return fmt::format("{} runs ({} drawn), {} draws, {} programs", state.runs, executed, state.draws, state.programs.Built());
	}
}
