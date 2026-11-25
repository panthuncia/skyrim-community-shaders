// The shadow mask family's render graph side. One client per view at the EngineDraws anchor draws a batch's latched draws into
// kSHADOW_MASK in engine order with CS's Utility.hlsl. Each client is declared on the display size, so a new kSHADOW_MASK slot
// registers it again with the next frame revision (frame revision plan, 6.19).

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/Engine/Engine.h"
#include "Features/DrawcallLimitFix/Features/ShadowMask/ShadowMask.h"

#include "Features/DrawcallLimitFix/Core/Engine/Family/FamilyClient.h"

#include "Features/TerrainBlending/ORGTerrainBlending.h"
#include "Globals.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "ShaderCache.h"
#include "Utils/Hash.h"
#include "Utils/Logging.h"
#include "RenderGraph/Frame/Declare.h"
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
#include <optional>
#include <string_view>

namespace DCLF::Features::ShadowMask::ShadowMaskDraws
{
	namespace
	{
		constexpr const char* kShader = "Utility.hlsl";
		constexpr std::uint32_t kDrawStride = static_cast<std::uint32_t>(sizeof(DrawConstants));
		constexpr std::size_t kViews = static_cast<std::size_t>(View::Count);

		/** @brief Utility.hlsl's push block (UTILITY_ORG_RESOURCES), in list order. */
		struct ShadowMaskBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Latch draw;
		};
		static_assert(sizeof(ShadowMaskBlock) == 24, "Utility.hlsl's ORG push block is 24 bytes");

		struct Client : Family::ClientState<Run>
		{
			Shape shape;
			std::shared_ptr<org::LatchBlock> latch;
			// Only this client's commit opens the gate, so a run another client keeps draws nothing of this client's older run.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			// Fixed at setup, so a new Publish needs no new client.
			Family::PublishedInputs inputs;
			::RenderGraph::RunSlots<Run> runs;
		};

		/**
		* @brief What a registration is made for: kSHADOW_MASK's graph slot. A new texture of the same format is the slot's next
		* version and keeps the key. Another format is a new slot.
		*/
		struct TargetKey
		{
			FrameAnchors::Texture target;
			bool operator==(const TargetKey&) const = default;
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

		/** @brief Records a run's draws in engine order. */
		class ShadowMaskPass final : public org::TypedRenderGraphPass<ShadowMaskPass, Frame, Bindings>
		{
		public:
			ShadowMaskPass(std::shared_ptr<Client> a_client, FrameAnchors::Texture a_target, bool a_atExecution) :
				client(std::move(a_client)), target(std::move(a_target)), atExecution(a_atExecution) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				Bindings bindings{};
				bindings.shared = SharedConstants::Declare(a_builder);
				bindings.target = a_builder.RenderTarget(target).View();
				// Every registered input is declared, read this frame or not, so a new Publish needs no new client.
				bindings.inputs = client->inputs.Declare(a_builder);
				return bindings;
			}

			// The pass reads the run at execution, so a kept recording stays current when the draws change.
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
												  .indirect = &c.gate->Latch(),
												  .indirectOffset = c.gate->Offset(i) });
				}
			}

		private:
			std::shared_ptr<Client> client;
			FrameAnchors::Texture target;  // the registration's kSHADOW_MASK slot
			bool atExecution = false;      // FrameAnchors::RecordsAtExecution at the build
		};

		/** @brief The shadow mask programs a Utility pixel descriptor can be drawn with: a RenderShadowmask* bit plus RenderDepth. */
		void StagesOf(bool a_pixel, std::uint32_t a_descriptor, std::vector<Family::StageRequest>& a_out)
		{
			using Flags = SIE::ShaderCache::UtilityShaderFlags;
			constexpr auto kMasks = static_cast<std::uint32_t>(Flags::RenderShadowmask) | static_cast<std::uint32_t>(Flags::RenderShadowmaskSpot) |
			                        static_cast<std::uint32_t>(Flags::RenderShadowmaskPb) | static_cast<std::uint32_t>(Flags::RenderShadowmaskDpb);
			// A mask bit without RenderDepth is another technique whose bits overlap in the legacy layouts. The family never draws it.
			if (a_pixel && (a_descriptor & kMasks) && (a_descriptor & static_cast<std::uint32_t>(Flags::RenderDepth)))
				a_out.push_back({ .descriptor = a_descriptor, .stage = Family::DefineStage::Pixel });
		}

		/** @brief Render-thread state. */
		struct DrawsState
		{
			Family::Client<Run, Client, kViews> clients{ "shadow mask" };
			Family::EngineDrawPrograms programs{ "shadow mask", { .source = kShader,
																	.constantWords = ShaderBindings::Words<ShadowMaskBlock>(),
																	.shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::Utility),
																	.stagesOf = &StagesOf } };
		};

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

		/**
		 * @brief Every published id a view's draws can read, listed at setup. Terrain Blending binds its R32 blended depth at t2.
		 * Exporting an id not listed here is refused and logged once. Add the id here.
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

		/** @brief Makes the client state for a_shape's view. Null, logged, when its run gate cannot be made. */
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
			ListInputs(client->inputs);
			return client;
		}

		/** @brief The registration of a_client for a_target. False, logged, when kSHADOW_MASK was not imported. */
		bool MakeDesc(const std::shared_ptr<Client>& a_client, const FrameAnchors::Texture& a_target, FrameAnchors::ClientDesc& a_desc)
		{
			if (!a_target) {
				logger::error("[DCLF] shadow mask: kSHADOW_MASK cannot be imported for the {} client", a_client->id);
				return false;
			}
			a_desc.id = a_client->id;
			a_desc.sharedConstants = true;
			a_desc.gates = { a_client->gate };
			a_desc.passes.push_back({ .name = a_client->id + ".draws",
				.anchor = FrameAnchors::Anchor::EngineDraws,
				.order = a_client->shape.view == View::World ? 3 : 4,
				.kind = FrameAnchors::PassKind::Render,
				.make = [client = a_client, target = a_target]() -> std::shared_ptr<org::RenderPass> {
					return std::make_shared<ShadowMaskPass>(client, target, FrameAnchors::RecordsAtExecution(FrameAnchors::Anchor::EngineDraws));
				},
				.atExecution = true });
			a_desc.commit = [client = a_client](const FrameAnchors::Point&) {
				const auto run = client->Current();
				auto* host = RenderGraphRuntime::Get().Host();
				if (!host)
					return;
				const org::RunRef latchRun = host->CurrentRun();
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
				if (client->CountExecuted())
					logger::info("[DCLF] shadow mask: the first {} run drawn by the graph", client->id);
			};
			a_desc.active = [client = a_client] { return client->pending; };
			// A step of its stage only. A batch outside the stage's window is a tap error.
			a_desc.segmentOnly = FrameAnchors::NamedByStage(a_client->id);
			const auto late = a_client->inputs.Unregistered();
			logger::info("[DCLF] shadow mask: the {} client is made for {} (published inputs it may read: {}{})", a_client->id, a_target.Id(),
				a_client->inputs.Describe(), late.empty() ? std::string() : std::format("; not published yet: {}", late));
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
			logger::warn("[DCLF] shadow mask: needs graph mode; the shadow masks stay the engine's");
			return true;
		}
		if (!FrameAnchors::EngineTexture(FrameAnchors::Colour(kShadowMaskTarget))) {
			logger::warn("[DCLF] shadow mask: kSHADOW_MASK cannot be imported; the shadow masks stay the engine's");
			return true;
		}
		namespace RF = ::RenderGraph::Frame;
		for (const auto view : { View::World, View::Menu }) {
			auto client = MakeClient({ .view = view });
			if (!client)
				continue;
			// The engine makes kSHADOW_MASK with its other targets, which raises the display size. The key imports it when a
			// revision starts, so a new slot registers the client again in that revision.
			state.clients.Declare<TargetKey>(
				static_cast<std::size_t>(view), client, RF::DependsOn(RF::Vars::DisplaySize()),
				[](const RF::Inputs&) -> std::optional<TargetKey> { return TargetKey{ FrameAnchors::EngineTexture(FrameAnchors::Colour(kShadowMaskTarget)) }; },
				[client](const TargetKey& a_key, FrameAnchors::ClientDesc& a_desc) { return MakeDesc(client, a_key.target, a_desc); },
				"those shadow masks stay the engine's");
		}
		return true;
	}

	bool CanTake(View a_view)
	{
		// Until a revision registers the client, the engine draws the batch.
		const auto segment = a_view == View::World ? ::RenderGraph::Frame::SegmentId::PreMain : ::RenderGraph::Frame::SegmentId::Ui;
		return GetState().clients.CanTake(static_cast<std::size_t>(a_view), static_cast<std::uint32_t>(segment));
	}

	Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index)
	{
		const auto& client = GetState().clients.At(static_cast<std::size_t>(a_view));
		return client ? client->inputs.Resolve(a_id, a_index) : Family::InputResult::NotListed;
	}

	std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader, bool* a_building)
	{
		return GetState().programs.Get(a_key, a_shader, a_building);
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

namespace DCLF::Features::ShadowMask
{
	std::uint64_t Run::Hash() const
	{
		Util::Fnv1a hash;
		hash.Mix(static_cast<std::uint64_t>(view));
		hash.Mix(draws.size());
		for (const auto& draw : draws) {
			hash.Mix(draw.program.get());
			for (const auto v : draw.viewport.rect)
				hash.Mix(v);
			std::uint32_t range[2];
			std::memcpy(range, draw.viewport.depthRange.data(), sizeof(range));
			hash.Mix((std::uint64_t(range[0]) << 32) | range[1]);
		}
		return hash.Value();
	}
}
