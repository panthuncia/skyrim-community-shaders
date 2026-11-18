// The menu scenes family's render graph side: a client per batch at the EngineDraws anchor, made once at setup, whose pass draws a
// batch's latched draws into the view's imported targets with kMAIN's depth, in the engine's order, with CS's shaders built for ORG
// (Utility.hlsl's UTILITY_ORG_SCENE, Lighting.hlsl's LIGHTING_ORG, Effect.hlsl's EFFECT_ORG). The clients are steps of the UI stage in every
// frame kind (a world frame's inventory item and local map too) and only there (segment-only); a run's draw list is a frame input: the
// commit stores it for its run (RunSlots) and the pass records it at execution, so the stage's kept recording never changes with the
// draws.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/MenuViews/MenuScenes.h"

#include "Globals.h"
#include "RenderGraph/Engine/Buffers/EngineBuffers.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameStaging.h"
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

		constexpr std::size_t kViews = static_cast<std::size_t>(View::Count);
		constexpr std::size_t kMaxImports = 4;

		/**
		 * @brief The push block of both builds, in list order: Utility.hlsl's (UTILITY_ORG_RESOURCES under UTILITY_ORG_SCENE) and
		 * Lighting.hlsl's (LIGHTING_ORG_RESOURCES: its UNREAD entries take no words).
		 */
		struct UtilityBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Latch draw;
			ShaderBindings::Latch bones;
			ShaderBindings::Latch frame;  // the draw's FrameBuffer rows (ORG_SHARED_FRAMEBUFFER_AT)
		};
		static_assert(sizeof(UtilityBlock) == 40, "the scene builds' push block is 40 bytes");

		/** @brief A client's state, which its pass shares. */
		struct Client
		{
			View view = View::LocalMap;
			std::string id;
			std::shared_ptr<org::LatchBlock> latch;
			// One draw entry per latched draw. The anchor closes it at every run, and only this client's commit opens it.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			// The view's colour targets (TargetsOf) and kMAIN's depth, as imported at the registration, and their formats.
			std::array<FrameAnchors::Texture, kMaxImports> colours;
			std::array<std::uint32_t, kMaxImports> colourFormats{};
			std::uint32_t colourCount = 0;
			FrameAnchors::Texture depth;
			std::uint32_t depthFormat = 0;
			// Every published id the batch's draws can read, fixed here (ListInputs). Each build declares the registered ones.
			Family::PublishedInputs inputs;
			// Render thread writes, the host's thread reads.
			std::shared_ptr<const Run> run;
			// Each run's draws, stored by the commit for the pass recorded at execution (a frame input).
			::RenderGraph::RunSlots<Run> runs;
			bool pending = false;
			bool targetLogged = false;
			std::atomic<std::uint64_t> executed{ 0 };
		};

		struct Bindings
		{
			SharedConstants::Bindings shared;
			Extensions::DeclaredReads inputs;  // the published inputs (Client::inputs)
			std::array<org::DeclaredViewToken, kMaxImports> colours{};
			org::DeclaredViewToken depth{};
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;
			std::shared_ptr<const Client> client;
		};

		const char* DebugName(View a_view)
		{
			switch (a_view) {
			case View::LocalMap:
				return "LocalMap";
			case View::LocalMapFog:
				return "LocalMapFog";
			case View::MenuDepth:
				return "MenuDepth";
			default:
				return "MenuColour";
			}
		}

		/** @brief A run's draws, recorded in the engine's order: a render pass and a draw each (RasterProgram::Draw). */
		class ScenePass final : public org::TypedRenderGraphPass<ScenePass, Frame, Bindings>
		{
		public:
			ScenePass(std::shared_ptr<Client> a_client, bool a_atExecution) :
				client(std::move(a_client)), atExecution(a_atExecution) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				Bindings bindings{};
				bindings.shared = SharedConstants::Declare(a_builder);
				for (std::uint32_t i = 0; i < client->colourCount; ++i)
					bindings.colours[i] = a_builder.RenderTarget(client->colours[i]).View();
				bindings.depth = a_builder.DepthReadWrite(client->depth).View();
				// The published inputs, each with the view whose index the draws' latches carry.
				bindings.inputs = client->inputs.Declare(a_builder);
				// A particles draw pulls its positions from the engine's dynamic ring (EffectSceneConstants::streamRef).
				::RenderGraph::Engine::Buffers::DeclareRingRead(a_builder);
				return bindings;
			}

			// The run's draws (their programs, targets, stencil references and viewports). Recorded at execution, the pass reads the run
			// its commit stored when it records: the run is not part of it, so a recording made ahead stays current when the draws change.
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
				const auto& c = *a_frame.client;
				const auto shared = SharedConstants::Index(a_recording, a_bindings.shared);
				const org::RunRef latchRun = a_recording.Run();
				const RasterProgram::Depth depth{ .view = &a_bindings.depth, .readOnly = false };
				for (std::uint32_t i = 0; i < runRef->draws.size(); ++i) {
					const auto& draw = runRef->draws[i];
					if (!draw.program)
						continue;
					const auto& program = *static_cast<const RasterProgram*>(draw.program.get());
					std::array<RasterProgram::Colour, kMaxColours> colours{};
					bool targetsOk = true;
					for (std::uint32_t k = 0; k < draw.colourCount; ++k) {
						targetsOk = targetsOk && draw.colours[k] < c.colourCount;
						if (targetsOk)
							colours[k] = { .view = &a_bindings.colours[draw.colours[k]] };
					}
					if (!targetsOk)
						continue;
					const UtilityBlock block{ shared, ShaderBindings::LatchOf(*c.latch, latchRun, draw.latchOffset), ShaderBindings::LatchOf(*c.latch, latchRun, draw.bonesOffset),
						ShaderBindings::LatchOf(*c.latch, latchRun, draw.frameOffset) };
					program.Draw(a_recording, { .colours = std::span<const RasterProgram::Colour>(colours.data(), draw.colourCount),
												  .depth = &depth,
												  .viewport = { .x = draw.viewport.rect[0], .y = draw.viewport.rect[1], .width = draw.viewport.rect[2], .height = draw.viewport.rect[3],
													  .minDepth = draw.viewport.depthRange[0], .maxDepth = draw.viewport.depthRange[1] },
												  .constants = ShaderBindings::Data(block),
												  .debugName = DebugName(c.view),
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
		 * @brief The programs a Utility vertex descriptor CS loads can be drawn with: the base texture, the fog of war, depth and the
		 * shadow maps (Utility.hlsl's UTILITY_ORG_SCENE). The family keys its programs by the vertex descriptor (both stages take its
		 * defines), as the water family does.
		 */
		void UtilityStagesOf(bool a_pixel, std::uint32_t a_descriptor, std::vector<Family::StageRequest>& a_out)
		{
			using Flags = SIE::ShaderCache::UtilityShaderFlags;
			constexpr auto kScene = static_cast<std::uint32_t>(Flags::RenderBaseTexture) | static_cast<std::uint32_t>(Flags::LocalMapFogOfWar) |
			                        static_cast<std::uint32_t>(Flags::RenderDepth) | static_cast<std::uint32_t>(Flags::RenderShadowmap);
			constexpr auto kNot = static_cast<std::uint32_t>(Flags::RenderShadowmask) | static_cast<std::uint32_t>(Flags::RenderShadowmaskSpot) |
			                      static_cast<std::uint32_t>(Flags::RenderShadowmaskPb) | static_cast<std::uint32_t>(Flags::RenderShadowmaskDpb) |
			                      static_cast<std::uint32_t>(Flags::RenderNormal) | static_cast<std::uint32_t>(Flags::RenderNormalClear) |
			                      static_cast<std::uint32_t>(Flags::DebugShadowSplit) | static_cast<std::uint32_t>(Flags::DebugColor);
			if (!a_pixel && (a_descriptor & kScene) && !(a_descriptor & kNot))
				a_out.push_back({ .descriptor = a_descriptor, .stage = Family::DefineStage::Vertex });
		}

		/** @brief Everything here: the render thread's. */
		struct DrawsState
		{
			std::array<std::shared_ptr<Client>, kViews> clients;
			Family::EngineDrawPrograms utility{ "menu scenes (Utility)", { .source = "Utility.hlsl",
																			 .constantWords = ShaderBindings::Words<UtilityBlock>(),
																			 .shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::Utility),
																			 .stagesOf = &UtilityStagesOf } };
			// Lighting's programs are not precompiled per descriptor (CS loads thousands): a key is built at its first draw, then
			// prebuilt from the manifest in the next session.
			Family::EngineDrawPrograms lighting{ "menu scenes (Lighting)", { .source = "Lighting.hlsl",
																			   .languageVersion = L"2018",  // FXC's semantics, as DCLF compiles it
																			   .constantWords = ShaderBindings::Words<UtilityBlock>(),
																			   .shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::Lighting) } };
			Family::EngineDrawPrograms effect{ "menu scenes (Effect)", { .source = "Effect.hlsl",
																		   .languageVersion = L"2018",
																		   .constantWords = ShaderBindings::Words<UtilityBlock>(),
																		   .shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::Effect) } };
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
		 * @brief Every published id a batch's draws can read: the material textures are imports, so only the engine targets a slot may
		 * hold: the shadow mask a Lighting draw reads at t14 (drawn by the shadow mask family's graph pass). An export of an id not listed
		 * here is an inherent refusal, logged once with the id: add the id here.
		 */
		void ListInputs(Family::PublishedInputs& a_inputs)
		{
			a_inputs.AddEngine(RE::RENDER_TARGET::kSHADOW_MASK);
			// An Effect draw's soft depth (t3): the depth copy, or kMAIN's.
			a_inputs.AddEngine(RE::RENDER_TARGET_DEPTHSTENCIL::kPOST_ZPREPASS_COPY);
			a_inputs.AddEngine(RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN_COPY);
		}

		/** @brief The client of a_view, registered with the anchor. Null, logged, when a target cannot be imported. */
		std::shared_ptr<Client> MakeClient(View a_view)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			auto* renderer = globals::game::renderer;
			if (!host || !renderer)
				return nullptr;
			auto client = std::make_shared<Client>();
			client->view = a_view;
			client->id = std::string("cs.menu-scenes.") + NameOf(a_view);
			client->latch = std::make_shared<org::LatchBlock>(client->id + ".latch", kLatchBytes);
			client->gate = ::RenderGraph::RunGate::Create(client->id + ".gate", kMaxDraws);
			if (!client->gate) {
				logger::warn("[DCLF] menu scenes: the {} run gate could not be made; that batch stays the engine's", client->id);
				return nullptr;
			}
			for (const auto target : TargetsOf(a_view)) {
				if (client->colourCount >= kMaxImports)
					break;
				const auto index = client->colourCount++;
				client->colours[index] = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(target));
				client->colourFormats[index] = FormatOf(renderer->GetRuntimeData().renderTargets[target].RTV);
				if (!client->colours[index] || !client->colourFormats[index]) {
					logger::warn("[DCLF] menu scenes: render target {} cannot be imported; the {} batch stays the engine's", target, client->id);
					return nullptr;
				}
			}
			const auto mainDepth = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
			client->depth = FrameAnchors::EngineDepth(mainDepth);
			client->depthFormat = FormatOf(renderer->GetDepthStencilData().depthStencils[mainDepth].views[0]);
			if (!client->depth || !client->depthFormat) {
				logger::warn("[DCLF] menu scenes: kMAIN's depth cannot be imported; the {} batch stays the engine's", client->id);
				return nullptr;
			}
			ListInputs(client->inputs);

			FrameAnchors::ClientDesc desc{};
			desc.id = client->id;
			desc.sharedConstants = true;
			desc.gates = { client->gate };
			desc.passes.push_back({ .name = client->id + ".draws",
				.anchor = FrameAnchors::Anchor::EngineDraws,
				.order = 5 + static_cast<int>(a_view),
				.kind = FrameAnchors::PassKind::Render,
				.make = [client]() -> std::shared_ptr<org::RenderPass> {
					return std::make_shared<ScenePass>(client, FrameAnchors::RecordsAtExecution(FrameAnchors::Anchor::EngineDraws));
				},
				// The run's draws are known after its commit: recorded then, the pass reads them without a recording made again.
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
				if (!run->latch.empty())
					client->latch->Write(latchRun, 0, std::span<const std::byte>(run->latch));
				for (std::uint32_t i = 0; i < run->draws.size(); ++i)
					if (run->draws[i].program)
						client->gate->OpenDraw(i, run->draws[i].indexCount, 1);
			};
			desc.executed = [client](const FrameAnchors::Point&) {
				if (client->executed.fetch_add(1, std::memory_order_relaxed) == 0)
					logger::info("[DCLF] menu scenes: the first {} run drawn by the graph", client->id);
			};
			desc.active = [client] { return client->pending; };
			// Passes of the UI stage only. A batch while no open stage holds the client is a mismatch (CanTake).
			desc.segmentOnly = FrameAnchors::NamedByStage(desc.id);
			desc.onFailed = [] { Fail("the render graph failed with the menu scenes' passes in it"); };
			if (!FrameAnchors::Register(std::move(desc))) {
				logger::warn("[DCLF] menu scenes: the {} client could not be registered; that batch stays the engine's", client->id);
				return nullptr;
			}
			// Register asked for the builds that hold the client: the UI stage's in native mode, the anchor's own graph in guest mode.
			logger::info("[DCLF] menu scenes: the {} client registered at setup", client->id);
			return client;
		}

		/** @brief What a run's recording depends on: its draws' programs, targets, stencil references, offsets and viewports. */
		std::uint64_t RevisionOf(const Run& a_run)
		{
			std::uint64_t hash = 1469598103934665603ull;
			const auto mix = [&](std::uint64_t a_value) { hash = (hash ^ a_value) * 1099511628211ull; };
			mix(static_cast<std::uint64_t>(a_run.view));
			mix(a_run.draws.size());
			for (const auto& draw : a_run.draws) {
				mix(reinterpret_cast<std::uintptr_t>(draw.program.get()));
				mix(draw.stencilRef);
				mix((std::uint64_t(draw.latchOffset) << 32) | draw.bonesOffset);
				mix(draw.frameOffset);
				mix(draw.colourCount);
				for (std::uint32_t k = 0; k < draw.colourCount; ++k)
					mix(draw.colours[k]);
				for (const auto v : draw.viewport.rect)
					mix(v);
				std::uint32_t range[2];
				std::memcpy(range, draw.viewport.depthRange.data(), sizeof(range));
				mix((std::uint64_t(range[0]) << 32) | range[1]);
			}
			return hash;
		}

		std::shared_ptr<Client> ClientOf(View a_view)
		{
			return a_view < View::Count ? GetState().clients[static_cast<std::size_t>(a_view)] : nullptr;
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
		state.utility.Prebuild();
		state.lighting.Prebuild();
		state.effect.Prebuild();
		if (!::RenderGraph::Engine::Constants::Requested()) {
			logger::warn("[DCLF] menu scenes: needs the engine constants capture (CS_ORG_ENGINE_CONSTANTS=1); the menu scenes stay the engine's");
			return true;
		}
		for (std::size_t v = 0; v < kViews; ++v)
			if (ViewEnabled(static_cast<View>(v)))
				state.clients[v] = MakeClient(static_cast<View>(v));
		return true;
	}

	bool CanTake(View a_view)
	{
		auto& state = GetState();
		if (state.failed)
			return false;
		const auto client = ClientOf(a_view);
		if (!client)
			return false;
		// A target made again (a resolution change) is another import: the batch stays the engine's until the next start.
		bool same = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN)) == client->depth;
		const auto targets = TargetsOf(a_view);
		for (std::uint32_t i = 0; i < client->colourCount && same; ++i)
			same = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(targets[i])) == client->colours[i];
		if (!same) {
			if (!std::exchange(client->targetLogged, true))
				logger::warn("[DCLF] menu scenes: a target changed since the {} client registered; that batch stays the engine's", client->id);
			return false;
		}
		// Native mode: the UI stage holds the batch in every frame kind.
		return Family::HeldForBatch("menu scenes", client->id, static_cast<std::uint32_t>(::RenderGraph::Frame::SegmentId::Ui));
	}

	Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index)
	{
		const auto client = ClientOf(a_view);
		return client ? client->inputs.Resolve(a_id, a_index) : Family::InputResult::NotListed;
	}

	std::shared_ptr<const void> UtilityProgram(const Family::ProgramKey& a_key, const RE::BSShader& a_shader)
	{
		return GetState().utility.Get(a_key, a_shader);
	}

	std::shared_ptr<const void> LightingProgram(const Family::ProgramKey& a_key, const RE::BSShader& a_shader)
	{
		return GetState().lighting.Get(a_key, a_shader);
	}

	std::shared_ptr<const void> EffectProgram(const Family::ProgramKey& a_key, const RE::BSShader& a_shader)
	{
		return GetState().effect.Get(a_key, a_shader);
	}

	std::uint32_t ColourFormat(View a_view, std::uint32_t a_index)
	{
		const auto client = ClientOf(a_view);
		return client && a_index < client->colourCount ? client->colourFormats[a_index] : 0;
	}

	std::uint32_t DepthFormat(View a_view)
	{
		const auto client = ClientOf(a_view);
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
		const auto client = ClientOf(a_run->view);
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
		if (!AnyEnabled())
			return "off (CS_ORG_MAP, CS_ORG_MENU3D)";
		if (state.failed)
			return "failed: the engine draws the menu scenes";
		std::uint64_t executed = 0;
		for (const auto& client : state.clients)
			if (client)
				executed += client->executed.load(std::memory_order_relaxed);
		return fmt::format("{} runs ({} drawn), {} draws, {} Utility, {} Lighting and {} Effect programs", state.runs, executed, state.draws, state.utility.Built(),
			state.lighting.Built(), state.effect.Built());
	}
}
