// The menu scenes family's render graph side. One client per batch at the EngineDraws anchor draws the latched draws in engine order.
// The clients are steps of the UI stage in every frame kind. The draw list is a frame input stored per run, so the stage's kept
// recording never changes with the draws.
// The clients are declared at setup and registered by the next frame revision (frame revision plan, 6.19).

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/Engine/Engine.h"
#include "Features/DrawcallLimitFix/Features/MenuViews/MenuScenes.h"

#include "Features/DrawcallLimitFix/Core/Engine/Family/FamilyClient.h"

#include "Globals.h"
#include "RenderGraph/Engine/Buffers/EngineBuffers.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"
#include "ShaderCache.h"
#include "Utils/Hash.h"
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
		constexpr std::size_t kMaxImports = 5;

		/** @brief The push block of both builds, in list order. Lighting's UNREAD entries take no words. */
		struct UtilityBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Latch draw;
			ShaderBindings::Latch bones;
			ShaderBindings::Latch frame;  // the draw's FrameBuffer rows (ORG_SHARED_FRAMEBUFFER_AT)
		};
		static_assert(sizeof(UtilityBlock) == 40, "the scene builds' push block is 40 bytes");

		struct Client : Family::ClientState<Run>
		{
			View view = View::LocalMap;
			std::shared_ptr<org::LatchBlock> latch;
			// Only this client's commit opens the gate.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			std::array<FrameAnchors::Texture, kMaxImports> colours;
			std::array<std::uint32_t, kMaxImports> colourFormats{};
			std::uint32_t colourCount = 0;
			FrameAnchors::Texture depth;
			std::uint32_t depthFormat = 0;
			std::uint32_t depthTarget = 0;  // RE::RENDER_TARGETS_DEPTHSTENCIL: kMAIN, or the shadow maps for the shadow batch
			std::uint32_t depthSlices = 1;  // the slices its passes draw into
			// Fixed at setup, so a new Publish needs no new client.
			Family::PublishedInputs inputs;
			::RenderGraph::RunSlots<Run> runs;
			bool targetLogged = false;
			// A committed run drew into the back buffer. Its first execution is logged once.
			std::atomic<bool> backBufferCommitted{ false };
			std::atomic<bool> backBufferLogged{ false };
		};

		/** @brief Whether a run has a draw into the back buffer, such as an item menu's model. */
		bool DrawsIntoBackBuffer(const Run& a_run)
		{
			const auto targets = TargetsOf(a_run.view);
			for (const auto& draw : a_run.draws)
				for (std::uint32_t k = 0; k < draw.colourCount; ++k)
					if (draw.program && draw.colours[k] < targets.size() && targets[draw.colours[k]] == kFramebufferTarget)
						return true;
			return false;
		}

		struct Bindings
		{
			SharedConstants::Bindings shared;
			Extensions::DeclaredReads inputs;  // the published inputs (Client::inputs)
			std::array<org::DeclaredViewToken, kMaxImports> colours{};
			std::array<org::DeclaredViewToken, kMaxShadowSlices> depth{};  // a view per slice
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
			case View::MenuShadow:
				return "MenuShadow";
			default:
				return "MenuColour";
			}
		}

		/** @brief Records a run's draws in engine order. */
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
				std::array<org::DsvView, kMaxShadowSlices> slices{};
				for (std::uint32_t slice = 0; slice < client->depthSlices; ++slice)
					slices[slice].slice = slice;
				const auto declared = a_builder.DepthReadWrite(client->depth, std::span<const org::DsvView>(slices.data(), client->depthSlices));
				for (std::uint32_t slice = 0; slice < client->depthSlices; ++slice)
					bindings.depth[slice] = declared.View(slice);
				bindings.inputs = client->inputs.Declare(a_builder);
				// A particles draw pulls its positions from the engine's dynamic ring (EffectSceneConstants::streamRef).
				::RenderGraph::Engine::Buffers::DeclareRingRead(a_builder);
				return bindings;
			}

			// The pass reads the run at execution, so the run is not part of the revision and a kept recording stays current.
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
				for (std::uint32_t i = 0; i < runRef->draws.size(); ++i) {
					const auto& draw = runRef->draws[i];
					if (!draw.program || draw.depthSlice >= c.depthSlices)
						continue;
					const RasterProgram::Depth depth{ .view = &a_bindings.depth[draw.depthSlice], .readOnly = false };
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
												  .indirect = &c.gate->Latch(),
												  .indirectOffset = c.gate->Offset(i) });
				}
			}

		private:
			std::shared_ptr<Client> client;
			bool atExecution = false;  // FrameAnchors::RecordsAtExecution at the build
		};

		/** @brief The programs a Utility vertex descriptor can be drawn with. Programs are keyed by the vertex descriptor. */
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

		/** @brief Render-thread state. */
		struct DrawsState
		{
			Family::Client<Run, Client, kViews> clients{ "menu scenes" };
			Family::EngineDrawPrograms utility{ "menu scenes (Utility)", { .source = "Utility.hlsl",
																			 .constantWords = ShaderBindings::Words<UtilityBlock>(),
																			 .shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::Utility),
																			 .stagesOf = &UtilityStagesOf } };
			// Lighting has thousands of descriptors, so a key is built at its first draw and prebuilt from the manifest next session.
			Family::EngineDrawPrograms lighting{ "menu scenes (Lighting)", { .source = "Lighting.hlsl",
																			   .languageVersion = L"2018",  // FXC's semantics, as DCLF compiles it
																			   .constantWords = ShaderBindings::Words<UtilityBlock>(),
																			   .shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::Lighting) } };
			Family::EngineDrawPrograms effect{ "menu scenes (Effect)", { .source = "Effect.hlsl",
																		   .languageVersion = L"2018",
																		   .constantWords = ShaderBindings::Words<UtilityBlock>(),
																		   .shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::Effect) } };
		};

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

		std::uint32_t FormatOf(ID3D11RenderTargetView* a_rtv)
		{
			if (!a_rtv)
				return 0;
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			a_rtv->GetDesc(&desc);
			return static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
		}

		std::uint32_t FormatOf(ID3D11DepthStencilView* a_dsv)
		{
			if (!a_dsv)
				return 0;
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			a_dsv->GetDesc(&desc);
			return static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
		}

		/**
		 * @brief Every published id a batch's draws can read. Material textures are imports, so only engine targets are listed.
		 * Exporting an id not listed here is refused and logged once. Add the id here.
		 */
		void ListInputs(Family::PublishedInputs& a_inputs)
		{
			a_inputs.AddEngine(RE::RENDER_TARGET::kSHADOW_MASK);
			// An Effect draw's soft depth (t3): the depth copy, or kMAIN's.
			a_inputs.AddEngine(RE::RENDER_TARGET_DEPTHSTENCIL::kPOST_ZPREPASS_COPY);
			a_inputs.AddEngine(RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN_COPY);
		}

		/** @brief Makes the client of a_view. Null, logged, when a target cannot be imported. */
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
			for (const auto target : TargetsOf(a_view)) {
				if (client->colourCount >= kMaxImports)
					break;
				const auto index = client->colourCount++;
				client->colours[index] = FrameAnchors::EngineTexture(FrameAnchors::Colour(static_cast<std::uint32_t>(target)));
				client->colourFormats[index] = FormatOf(renderer->GetRuntimeData().renderTargets[target].RTV);
				if (!client->colours[index] || !client->colourFormats[index]) {
					logger::warn("[DCLF] menu scenes: render target {} cannot be imported; the {} batch stays the engine's", target, client->id);
					return nullptr;
				}
			}
			client->depthTarget = static_cast<std::uint32_t>(a_view == View::MenuShadow ? RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS : RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
			const auto& depthData = renderer->GetDepthStencilData().depthStencils[client->depthTarget];
			client->depth = FrameAnchors::EngineTexture(FrameAnchors::Depth(client->depthTarget));
			client->depthFormat = FormatOf(depthData.views[0]);
			if (a_view == View::MenuShadow && depthData.texture) {
				D3D11_TEXTURE2D_DESC desc{};
				depthData.texture->GetDesc(&desc);
				client->depthSlices = (std::min)(desc.ArraySize, kMaxShadowSlices);
			}
			if (!client->depth || !client->depthFormat || !client->depthSlices) {
				logger::warn("[DCLF] menu scenes: depth target {} cannot be imported; the {} batch stays the engine's", client->depthTarget, client->id);
				return nullptr;
			}
			ListInputs(client->inputs);
			return client;
		}

		/** @brief The registration of a_client. Its targets were imported at setup. */
		bool MakeDesc(const std::shared_ptr<Client>& a_client, FrameAnchors::ClientDesc& a_desc)
		{
			a_desc.id = a_client->id;
			a_desc.sharedConstants = true;
			a_desc.gates = { a_client->gate };
			a_desc.passes.push_back({ .name = a_client->id + ".draws",
				.anchor = FrameAnchors::Anchor::EngineDraws,
				.order = 5 + static_cast<int>(a_client->view),
				.kind = FrameAnchors::PassKind::Render,
				.make = [client = a_client]() -> std::shared_ptr<org::RenderPass> {
					return std::make_shared<ScenePass>(client, FrameAnchors::RecordsAtExecution(FrameAnchors::Anchor::EngineDraws));
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
				if (!run->latch.empty())
					client->latch->Write(latchRun, 0, std::span<const std::byte>(run->latch));
				for (std::uint32_t i = 0; i < run->draws.size(); ++i)
					if (run->draws[i].program)
						client->gate->OpenDraw(i, run->draws[i].indexCount, 1);
				if (!client->backBufferLogged.load(std::memory_order_relaxed) && DrawsIntoBackBuffer(*run))
					client->backBufferCommitted.store(true, std::memory_order_relaxed);
			};
			a_desc.executed = [client = a_client](const FrameAnchors::Point&) {
				if (client->CountExecuted())
					logger::info("[DCLF] menu scenes: the first {} run drawn by the graph", client->id);
				// The model lands in the back buffer after its menu's movie and before TAA's UI form reads it.
				if (client->backBufferCommitted.exchange(false, std::memory_order_relaxed) && !client->backBufferLogged.exchange(true, std::memory_order_relaxed))
					logger::info("[DCLF] menu scenes: the first {} run into the back buffer (rt0) drawn by the graph, such as an item menu's model", client->id);
			};
			a_desc.active = [client = a_client] { return client->pending; };
			// UI stage passes only. A batch while no open stage holds the client is a mismatch.
			a_desc.segmentOnly = FrameAnchors::NamedByStage(a_desc.id);
			logger::info("[DCLF] menu scenes: the {} client is made", a_client->id);
			return true;
		}

		const std::shared_ptr<Client>& ClientOf(View a_view)
		{
			return GetState().clients.At(static_cast<std::size_t>(a_view));
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
		state.utility.Prebuild();
		state.lighting.Prebuild();
		state.effect.Prebuild();
		if (!::RenderGraph::Engine::GraphMode()) {
			logger::warn("[DCLF] menu scenes: needs graph mode; the menu scenes stay the engine's");
			return true;
		}
		namespace RF = ::RenderGraph::Frame;
		for (std::size_t v = 0; v < kViews; ++v) {
			if (!ViewEnabled(static_cast<View>(v)))
				continue;
			auto client = MakeClient(static_cast<View>(v));
			if (!client)
				continue;
			// The targets are imported above. A target made again later is another import, which CanTake refuses.
			state.clients.Declare<Family::NoKey>(
				v, client, RF::DependsOn(), [](const RF::Inputs&) -> std::optional<Family::NoKey> { return Family::NoKey{}; },
				[client](const Family::NoKey&, FrameAnchors::ClientDesc& a_desc) { return MakeDesc(client, a_desc); }, "that batch stays the engine's");
		}
		return true;
	}

	bool CanTake(View a_view)
	{
		const auto& state = GetState();
		// Until a revision registers the client, the engine draws the batch.
		const auto& client = state.clients.Registered(static_cast<std::size_t>(a_view));
		if (!client)
			return false;
		// A target made again after a resolution change is another import, so the batch stays the engine's until the next start.
		bool same = FrameAnchors::EngineTexture(FrameAnchors::Depth(client->depthTarget)) == client->depth;
		const auto targets = TargetsOf(a_view);
		for (std::uint32_t i = 0; i < client->colourCount && same; ++i)
			same = FrameAnchors::EngineTexture(FrameAnchors::Colour(static_cast<std::uint32_t>(targets[i]))) == client->colours[i];
		if (!same) {
			if (!std::exchange(client->targetLogged, true))
				logger::warn("[DCLF] menu scenes: a target changed since the {} client registered; that batch stays the engine's", client->id);
			return false;
		}
		return state.clients.Held(*client, static_cast<std::uint32_t>(::RenderGraph::Frame::SegmentId::Ui));
	}

	Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index)
	{
		const auto& client = ClientOf(a_view);
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
		const auto& client = ClientOf(a_view);
		return client && a_index < client->colourCount ? client->colourFormats[a_index] : 0;
	}

	std::uint32_t DepthFormat(View a_view)
	{
		const auto& client = ClientOf(a_view);
		return client ? client->depthFormat : 0;
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
		if (!AnyEnabled())
			return "off (not graph mode)";
		return fmt::format("{} runs ({} drawn), {} draws, {} Utility, {} Lighting and {} Effect programs", state.clients.Runs(), state.clients.Executed(),
			state.clients.Draws(), state.utility.Built(), state.lighting.Built(), state.effect.Built());
	}
}

namespace DCLF::Features::MenuViews::Scenes
{
	std::uint64_t Run::Hash() const
	{
		Util::Fnv1a hash;
		hash.Mix(static_cast<std::uint64_t>(view));
		hash.Mix(draws.size());
		for (const auto& draw : draws) {
			hash.Mix(draw.program.get());
			hash.Mix(draw.stencilRef);
			hash.Mix((std::uint64_t(draw.latchOffset) << 32) | draw.bonesOffset);
			hash.Mix(draw.frameOffset);
			hash.Mix(draw.depthSlice);
			hash.Mix(draw.colourCount);
			for (std::uint32_t k = 0; k < draw.colourCount; ++k)
				hash.Mix(draw.colours[k]);
			for (const auto v : draw.viewport.rect)
				hash.Mix(v);
			std::uint32_t range[2];
			std::memcpy(range, draw.viewport.depthRange.data(), sizeof(range));
			hash.Mix((std::uint64_t(range[0]) << 32) | range[1]);
		}
		return hash.Value();
	}
}
