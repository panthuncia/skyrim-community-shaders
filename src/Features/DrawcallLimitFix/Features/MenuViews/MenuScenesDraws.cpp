// The menu scenes family's render graph side: a client per batch at the EngineDraws anchor, made once at setup, whose pass draws a
// batch's latched draws into kLOCAL_MAP_SWAP with kMAIN's depth, in the engine's order, with CS's Utility.hlsl built for ORG
// (UTILITY_ORG_SCENE: the base texture and the fog of war techniques).

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/MenuViews/MenuScenes.h"

#include "Globals.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"
#include "ShaderCache.h"
#include "Utils/Logging.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>

#include <rhi_helpers.h>

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <string_view>
#include <utility>

namespace DCLF::Features::MenuViews::Scenes::SceneDraws
{
	namespace
	{
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		constexpr const char* kShader = "Utility.hlsl";
		constexpr std::uint32_t kDrawStride = static_cast<std::uint32_t>(sizeof(SceneDrawConstants));
		constexpr std::size_t kViews = static_cast<std::size_t>(View::Count);

		/** @brief Utility.hlsl's push block (UTILITY_ORG_RESOURCES under UTILITY_ORG_SCENE), in list order. */
		struct SceneBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Latch draw;
		};
		static_assert(sizeof(SceneBlock) == 24, "Utility.hlsl's scene push block is 24 bytes");

		/** @brief A client's state, which its pass shares. */
		struct Client
		{
			View view = View::LocalMap;
			std::string id;
			std::shared_ptr<org::LatchBlock> latch;
			// One draw entry per latched draw. The anchor closes it at every run, and only this client's commit opens it.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			// kLOCAL_MAP_SWAP and kMAIN's depth, as imported at the registration, and their formats.
			std::shared_ptr<org::ExternalTextureResource> colour, depth;
			std::uint32_t colourFormat = 0, depthFormat = 0;
			// Every published id the batch's draws can read, fixed here (ListInputs). Each build declares the registered ones.
			Family::PublishedInputs inputs;
			// Render thread writes, the host's thread reads.
			std::shared_ptr<const Run> run;
			bool pending = false;
			bool targetLogged = false;
			std::atomic<std::uint64_t> executed{ 0 };
		};

		struct Bindings
		{
			SharedConstants::Bindings shared;
			org::DeclaredViewToken colour{}, depth{};
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;
			std::shared_ptr<const Client> client;
		};

		/** @brief A run's draws, recorded in the engine's order: a render pass and a draw each (RasterProgram::Draw). */
		class ScenePass final : public org::TypedRenderGraphPass<ScenePass, Frame, Bindings>
		{
		public:
			explicit ScenePass(std::shared_ptr<Client> a_client) :
				client(std::move(a_client)) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				Bindings bindings{};
				bindings.shared = SharedConstants::Declare(a_builder);
				bindings.colour = a_builder.RenderTarget(client->colour).View();
				bindings.depth = a_builder.DepthReadWrite(client->depth).View();
				client->inputs.Declare(a_builder);
				return bindings;
			}

			// The run's draws (their programs, stencil references and viewports).
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
				const org::RunRef latchRun = a_recording.Run();
				const RasterProgram::Colour colour{ .view = &a_bindings.colour };
				const RasterProgram::Depth depth{ .view = &a_bindings.depth, .readOnly = false };
				const char* name = c.view == View::LocalMapFog ? "LocalMapFog" : "LocalMap";
				for (std::uint32_t i = 0; i < a_frame.run->draws.size(); ++i) {
					const auto& draw = a_frame.run->draws[i];
					if (!draw.program)
						continue;
					const auto& program = *static_cast<const RasterProgram*>(draw.program.get());
					const SceneBlock block{ shared, ShaderBindings::LatchOf(*c.latch, latchRun, i * kDrawStride) };
					program.Draw(a_recording, { .colours = std::span<const RasterProgram::Colour>(&colour, 1),
												  .depth = &depth,
												  .viewport = { .x = draw.viewport.rect[0], .y = draw.viewport.rect[1], .width = draw.viewport.rect[2], .height = draw.viewport.rect[3],
													  .minDepth = draw.viewport.depthRange[0], .maxDepth = draw.viewport.depthRange[1] },
												  .constants = ShaderBindings::Data(block),
												  .debugName = name,
												  .stencilReference = draw.stencilRef,
												  // The counts come from the draw's gate entry, which the client's commit opens for this run only.
												  .indirect = &c.gate->Latch(),
												  .indirectOffset = c.gate->Offset(i) });
				}
			}

		private:
			std::shared_ptr<Client> client;
		};

		/**
		 * @brief The programs a Utility vertex descriptor CS loads can be drawn with: the base texture and the fog of war techniques,
		 * not skinned (Utility.hlsl's UTILITY_ORG_SCENE). The family keys its programs by the vertex descriptor (both stages take its
		 * defines), as the water family does.
		 */
		void StagesOf(bool a_pixel, std::uint32_t a_descriptor, std::vector<Family::StageRequest>& a_out)
		{
			using Flags = SIE::ShaderCache::UtilityShaderFlags;
			constexpr auto kScene = static_cast<std::uint32_t>(Flags::RenderBaseTexture) | static_cast<std::uint32_t>(Flags::LocalMapFogOfWar);
			constexpr auto kNot = static_cast<std::uint32_t>(Flags::Skinned) | static_cast<std::uint32_t>(Flags::RenderDepth) |
			                      static_cast<std::uint32_t>(Flags::RenderShadowmap) | static_cast<std::uint32_t>(Flags::RenderShadowmask) |
			                      static_cast<std::uint32_t>(Flags::RenderShadowmaskSpot) | static_cast<std::uint32_t>(Flags::RenderShadowmaskPb) |
			                      static_cast<std::uint32_t>(Flags::RenderShadowmaskDpb);
			if (!a_pixel && (a_descriptor & kScene) && !(a_descriptor & kNot))
				a_out.push_back({ .descriptor = a_descriptor, .stage = Family::DefineStage::Vertex });
		}

		/** @brief Everything here: the render thread's. */
		struct DrawsState
		{
			std::array<std::shared_ptr<Client>, kViews> clients;
			Family::EngineDrawPrograms programs{ "menu scenes", { .source = kShader,
																	.constantWords = ShaderBindings::Words<SceneBlock>(),
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

		/** @brief Stops the family for good: every menu scene draw is the engine's again. */
		void Fail(const char* a_why)
		{
			auto& state = GetState();
			if (!state.failed)
				logger::error("[DCLF] menu scenes: back to the engine's draws for the session: {}", a_why);
			state.failed = true;
			for (auto& client : state.clients)
				if (client)
					FrameAnchors::Unregister(client->id);
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

		/**
		 * @brief Every published id a batch's draws can read: the base textures are material textures (imports), so only what a ported
		 * feature may have bound at t0 or t7 in a menu scene. None is known; an export of an id not listed here is an inherent refusal,
		 * logged once with the id: add the id here.
		 */
		void ListInputs(Family::PublishedInputs&)
		{
		}

		/** @brief The client of a_view, registered with the anchor. Null, logged, when a target cannot be imported. */
		std::shared_ptr<Client> MakeClient(View a_view)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return nullptr;
			auto client = std::make_shared<Client>();
			client->view = a_view;
			client->id = std::string("cs.menu-scenes.") + NameOf(a_view);
			client->latch = std::make_shared<org::LatchBlock>(client->id + ".latch", kMaxDraws * kDrawStride);
			client->gate = ::RenderGraph::RunGate::Create(client->id + ".gate", kMaxDraws);
			if (!client->gate) {
				logger::warn("[DCLF] menu scenes: the {} run gate could not be made; that batch stays the engine's", client->id);
				return nullptr;
			}
			const auto mainDepth = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
			client->colour = FrameAnchors::EngineTarget(kLocalMapSwapTarget);
			client->depth = FrameAnchors::EngineDepth(mainDepth);
			auto* renderer = globals::game::renderer;
			if (renderer) {
				client->colourFormat = FormatOf(renderer->GetRuntimeData().renderTargets[kLocalMapSwapTarget].RTV);
				client->depthFormat = FormatOf(renderer->GetDepthStencilData().depthStencils[mainDepth].views[0]);
			}
			if (!client->colour || !client->depth || !client->colourFormat || !client->depthFormat) {
				logger::warn("[DCLF] menu scenes: kLOCAL_MAP_SWAP or kMAIN's depth cannot be imported; the {} batch stays the engine's", client->id);
				return nullptr;
			}
			ListInputs(client->inputs);

			FrameAnchors::ClientDesc desc{};
			desc.id = client->id;
			desc.sharedConstants = true;
			desc.gates = { client->gate };
			desc.passes.push_back({ .name = client->id + ".draws",
				.anchor = FrameAnchors::Anchor::EngineDraws,
				.order = a_view == View::LocalMap ? 5 : 6,
				.kind = FrameAnchors::PassKind::Render,
				.make = [client]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ScenePass>(client); } });
			desc.commit = [client](const FrameAnchors::Point&) {
				const auto run = std::atomic_load(&client->run);
				auto* host = RenderGraphRuntime::Get().Host();
				if (!run || !host)
					return;
				const org::RunRef latchRun = host->CurrentRun();
				for (std::uint32_t i = 0; i < run->draws.size(); ++i) {
					const auto& draw = run->draws[i];
					client->latch->WriteValue(latchRun, i * kDrawStride, draw.constants);
					if (draw.program)
						client->gate->OpenDraw(i, draw.indexCount, 1);
				}
			};
			desc.executed = [client](const FrameAnchors::Point&) {
				if (client->executed.fetch_add(1, std::memory_order_relaxed) == 0)
					logger::info("[DCLF] menu scenes: the first {} run drawn by the graph", client->id);
			};
			desc.active = [client] { return client->pending; };
			desc.onFailed = [] { Fail("the render graph failed with the menu scenes' passes in it"); };
			if (!FrameAnchors::Register(std::move(desc))) {
				logger::warn("[DCLF] menu scenes: the {} client could not be registered; that batch stays the engine's", client->id);
				return nullptr;
			}
			logger::info("[DCLF] menu scenes: the {} client registered at setup", client->id);
			// The one build request: the next build point (a menu frame's end) builds the graph with the client, before any world frame.
			FrameAnchors::RequestJoin(client->id, FrameAnchors::Anchor::EngineDraws);
			return client;
		}

		/** @brief What a run's recording depends on: its draws' programs, stencil references and viewports. */
		std::uint64_t RevisionOf(const Run& a_run)
		{
			std::uint64_t hash = 1469598103934665603ull;
			const auto mix = [&](std::uint64_t a_value) { hash = (hash ^ a_value) * 1099511628211ull; };
			mix(static_cast<std::uint64_t>(a_run.view));
			mix(a_run.draws.size());
			for (const auto& draw : a_run.draws) {
				mix(reinterpret_cast<std::uintptr_t>(draw.program.get()));
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
			logger::warn("[DCLF] menu scenes: the EngineDraws anchor could not be reserved; the menu scenes stay the engine's");
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
			logger::warn("[DCLF] menu scenes: needs the engine constants capture (CS_ORG_ENGINE_CONSTANTS=1); the menu scenes stay the engine's");
			return true;
		}
		for (const auto view : { View::LocalMap, View::LocalMapFog })
			state.clients[static_cast<std::size_t>(view)] = MakeClient(view);
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
		// A target made again (a resolution change) is another import: the batch stays the engine's until the next start.
		if (FrameAnchors::EngineTarget(kLocalMapSwapTarget) != client->colour ||
			FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN)) != client->depth) {
			if (!std::exchange(client->targetLogged, true))
				logger::warn("[DCLF] menu scenes: kLOCAL_MAP_SWAP or kMAIN's depth changed since the {} client registered; that batch stays the engine's", client->id);
			return false;
		}
		if (!FrameAnchors::CanRun(client->id, FrameAnchors::Anchor::EngineDraws)) {
			static Util::ReasonLog log("[DCLF] menu scenes: the engine draws a batch", spdlog::level::err);
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

	std::uint32_t ColourFormat(View a_view)
	{
		const auto& client = a_view < View::Count ? GetState().clients[static_cast<std::size_t>(a_view)] : nullptr;
		return client ? client->colourFormat : 0;
	}

	std::uint32_t DepthFormat(View a_view)
	{
		const auto& client = a_view < View::Count ? GetState().clients[static_cast<std::size_t>(a_view)] : nullptr;
		return client ? client->depthFormat : 0;
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
		return Family::RunWithheld("menu scenes", client->id, FrameAnchors::Anchor::EngineDraws, client->pending);
	}

	std::string Status()
	{
		const auto& state = GetState();
		if (!Enabled())
			return "off (CS_ORG_MAP)";
		if (state.failed)
			return "failed: the engine draws the menu scenes";
		std::uint64_t executed = 0;
		for (const auto& client : state.clients)
			if (client)
				executed += client->executed.load(std::memory_order_relaxed);
		return fmt::format("{} runs ({} drawn), {} draws, {} programs", state.runs, executed, state.draws, state.programs.Built());
	}
}
