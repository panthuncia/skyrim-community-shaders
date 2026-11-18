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
#include "RenderGraph/Frame/FrameDescription.h"
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

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string_view>

namespace DCLF::Features::ShadowMask::ShadowMaskDraws
{
	namespace
	{
		constexpr const char* kShader = "Utility.hlsl";
		// The most draws a run latches (ShadowMaskCapture's kMaxLatched).
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
			// kSHADOW_MASK as last imported (Target). A new texture of the same format is the slot's next version; another format is a
			// new slot, taken at the next batch (CanTake) and declared from the next build.
			FrameAnchors::Texture target;
			mutable std::mutex targetMutex;  // the render thread replaces target, a build reads it
			// Every published id the view's draws can read, fixed here (ListInputs). Each build declares the registered ones.
			Family::PublishedInputs inputs;
			// Render thread writes, the host's thread reads.
			std::shared_ptr<const Run> run;
			// Each run's draws, stored by the commit for the pass recorded at execution (a frame input).
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
			SharedConstants::Bindings shared;
			Extensions::DeclaredReads inputs;  // the published inputs (Client::inputs)
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
			ShadowMaskPass(std::shared_ptr<Client> a_client, bool a_atExecution) :
				client(std::move(a_client)), atExecution(a_atExecution) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				Bindings bindings{};
				bindings.shared = SharedConstants::Declare(a_builder);
				bindings.target = a_builder.RenderTarget(client->Target()).View();
				// The published inputs, each with the view whose index the draws' latches carry. Every registered one is declared, read
				// this frame or not, so a new Publish needs no new client.
				bindings.inputs = client->inputs.Declare(a_builder);
				return bindings;
			}

			// The run's draws (their programs and viewports). Recorded at execution, the pass reads the run its commit stored when it records:
			// a recording made ahead stays current when the draws change.
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
				const RasterProgram::Colour colour{ .view = &a_bindings.target };
				for (std::uint32_t i = 0; i < runRef->draws.size(); ++i) {
					const auto& draw = runRef->draws[i];
					if (!draw.program)
						continue;
					const auto& program = *static_cast<const RasterProgram*>(draw.program.get());
					const ShadowMaskBlock block{ shared, ShaderBindings::LatchOf(*c.latch, latchRun, i * kDrawStride) };
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
			bool atExecution = false;  // FrameAnchors::RecordsAtExecution at the build
		};

		/**
		 * @brief The shadow mask programs a Utility pixel descriptor CS loads can be drawn with: those with a RenderShadowmask* technique
		 * bit and RenderDepth (the world's and the menu scenes' masks: shadow-mask.md, "The shader"). Every other Utility descriptor is DCLF's or the
		 * engine's.
		 */
		void StagesOf(bool a_pixel, std::uint32_t a_descriptor, std::vector<Family::StageRequest>& a_out)
		{
			using Flags = SIE::ShaderCache::UtilityShaderFlags;
			constexpr auto kMasks = static_cast<std::uint32_t>(Flags::RenderShadowmask) | static_cast<std::uint32_t>(Flags::RenderShadowmaskSpot) |
			                        static_cast<std::uint32_t>(Flags::RenderShadowmaskPb) | static_cast<std::uint32_t>(Flags::RenderShadowmaskDpb);
			// Utility.hlsl's ORG build is RENDER_DEPTH with a RENDER_SHADOWMASK* technique only (its #error): a descriptor with a mask bit
			// but no RenderDepth is another technique whose bits overlap (the legacy layouts), which the family never draws.
			if (a_pixel && (a_descriptor & kMasks) && (a_descriptor & static_cast<std::uint32_t>(Flags::RenderDepth)))
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
			client->latch = std::make_shared<org::LatchBlock>(client->id + ".latch", kMaxDraws * kDrawStride);
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
				.make = [client]() -> std::shared_ptr<org::RenderPass> {
					return std::make_shared<ShadowMaskPass>(client, FrameAnchors::RecordsAtExecution(FrameAnchors::Anchor::EngineDraws));
				},
				// The run's draws are known after its commit (a frame input): recorded then.
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
					logger::info("[DCLF] shadow mask: the first {} run drawn by the graph", client->id);
			};
			desc.active = [client] { return client->pending; };
			// A step of its stage only (pre-main for the world's, the UI stage for a menu scene's): a batch outside its stage's window is a
			// tap error (CanTake).
			desc.segmentOnly = FrameAnchors::NamedByStage(desc.id);
			desc.onFailed = [] { Fail("the render graph failed with the shadow masks' passes in it"); };
			if (!FrameAnchors::Register(std::move(desc))) {
				logger::warn("[DCLF] shadow mask: the {} client could not be registered; those shadow masks stay the engine's", client->id);
				return nullptr;
			}
			// Register asked for the builds that hold the client: its stage's in native mode, the anchor's own graph in guest mode.
			const auto late = client->inputs.Unregistered();
			logger::info("[DCLF] shadow mask: the {} client registered at setup (published inputs it may read: {}{})", client->id, client->inputs.Describe(),
				late.empty() ? std::string() : std::format("; not published yet: {}", late));
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
		// kSHADOW_MASK made again at another size is the slot's next version (FrameAnchors rebuilds what took its size). Another format
		// is a new slot: the client takes it and is built again with it (the import already asked every graph holding the old slot to be
		// built again), and its batches wait for that build's attach below.
		if (auto current = FrameAnchors::EngineTarget(kShadowMaskTarget); current && current != client->Target()) {
			{
				std::scoped_lock lock(client->targetMutex);
				client->target = std::move(current);
			}
			FrameAnchors::Refresh(client->id);
			logger::info("[DCLF] shadow mask: kSHADOW_MASK is another graph resource (a format change); the {} client is built again with it", client->id);
		}
		const auto segment = a_view == View::World ? ::RenderGraph::Frame::SegmentId::PreMain : ::RenderGraph::Frame::SegmentId::Ui;
		return Family::HeldForBatch("shadow mask", client->id, static_cast<std::uint32_t>(segment));
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
