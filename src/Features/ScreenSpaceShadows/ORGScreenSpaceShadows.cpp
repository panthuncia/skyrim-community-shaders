#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGScreenSpaceShadows.h"

#	include "Features/ScreenSpaceShadows.h"
#	include "Features/TerrainBlending.h"
#	include "Features/TerrainBlending/ORGTerrainBlending.h"
#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/Switches.h"
#	include "RenderGraph/UavClearPass.h"
#	include "Utils/Game.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>

#	include <cstddef>
#	include <map>
#	include <set>

namespace
{
	constexpr const char* kClientId = "cs.screen-space-shadows";
	constexpr const char* kShader = "ScreenSpaceShadows/RaymarchCS.hlsl";
	// The shadow texture's name, which it is published under: Drawcall Limit Fix's t45.
	constexpr std::string_view kTextureName = ORGScreenSpaceShadows::kShadowTextureId;
	// Terrain Blending's D3D11 blended depth, imported while its graph path is off (FrameAnchors::D3D11Texture adds "cs.d3d11.").
	constexpr std::string_view kBlendedDepthName = "terrain-blending.blended-depth";
	constexpr std::string_view kBlendedDepthImportId = "cs.d3d11.terrain-blending.blended-depth";
	constexpr std::uint32_t kMaxDispatches = ScreenSpaceShadows::Dispatches::kMax;

	// RaymarchCS's push block under ORG, in list order: three handles, then PerFrame's latch region.
	struct RaymarchBlock
	{
		ShaderBindings::Handle depthTexture, outputTexture, pointBorderSampler;
		ShaderBindings::Latch perFrame;
	};

	// One execution's values, in its slot of the latch block: each dispatch's constants (RaymarchConstants, read at their
	// offset by the shader). The group counts are in the run gate.
	struct RaymarchLatchData
	{
		ScreenSpaceShadows::RaymarchCB constants[kMaxDispatches];
	};
	static_assert(sizeof(ScreenSpaceShadows::RaymarchCB) % 16 == 0);

	/** @brief The raymarch program for one sample count. */
	struct Program
	{
		std::shared_ptr<const ComputeProgram> compute;
		std::uint32_t sampleCount = 0;
	};

	/** @brief The raymarch programs by sample count. */
	using ProgramSet = std::map<std::uint32_t, std::shared_ptr<const Program>>;

	/** @brief The depth the raymarch reads: an import, or Terrain Blending's published blended depth by its id. */
	struct DepthSource
	{
		std::shared_ptr<org::Resource> resource;
		std::string_view published;
		bool blendImport = false;  // the import of Terrain Blending's D3D11 blended depth

		explicit operator bool() const { return resource || !published.empty(); }
		bool operator==(const DepthSource&) const = default;
	};

	/** @brief What the passes are made from; changed on the render thread only, between builds (then FrameAnchors::Refresh). */
	struct ShadowState
	{
		std::shared_ptr<const Program> program;
		ProgramSet programs;  // built at setup; a count outside it is added at first sight
		std::unique_ptr<GraphResources::Texture2D> output;
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
		org::OwnedDescriptorBinding sampler;
		DepthSource depth;
		std::function<void()> onFailed;
	};

	std::shared_ptr<const Program> LoadProgram(rhi::Device a_device, std::uint32_t a_sampleCount)
	{
		std::vector<std::pair<std::wstring, std::wstring>> defines{ { L"SAMPLE_COUNT", std::to_wstring(a_sampleCount) } };
		// As the D3D11 build: the depth texture's HLSL type follows Terrain Blending.
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ L"TERRAIN_BLENDING", L"" });
		auto program = std::make_shared<Program>();
		program->sampleCount = a_sampleCount;
		program->compute = ComputeProgram::Load(a_device, { .source = kShader, .constantWords = ShaderBindings::Words<RaymarchBlock>(), .defines = defines });
		return program->compute ? program : nullptr;
	}

	/**
	 * @brief Every sample count the settings can ask for at the screen size: each multiplier (1-4) at the full render size and
	 * at the dynamic one now. A dynamic size seen later can ask for another; ProgramFor builds that one at first sight.
	 */
	std::set<std::uint32_t> SettingsSampleCounts()
	{
		const auto* graphics = globals::game::graphicsState;
		const float2 full{ static_cast<float>(graphics->screenWidth), static_cast<float>(graphics->screenHeight) };
		const float2 dynamic = Util::ConvertToDynamic(full);
		std::set<std::uint32_t> counts;
		for (std::uint32_t multiplier = 1; multiplier <= 4; ++multiplier) {
			counts.insert(ScreenSpaceShadows::ScaleSampleCount(multiplier, full));
			counts.insert(ScreenSpaceShadows::ScaleSampleCount(multiplier, dynamic));
		}
		return counts;
	}

	/**
	 * @brief The depth the D3D11 path reads at Prepass (Util::GetCurrentSceneDepthSRV(false)), as a graph texture: on Terrain
	 * Blending's ORG path its published blended depth, which the graph orders after the blend.
	 */
	DepthSource CurrentDepth()
	{
		auto& terrainBlending = globals::features::terrainBlending;
		if (terrainBlending.loaded && terrainBlending.settings.Enabled) {
			if (PublishedResources::IsPublished(ORGTerrainBlending::kBlendedDepthId))
				return { nullptr, ORGTerrainBlending::kBlendedDepthId };
			if (terrainBlending.blendedDepthTexture)
				return { FrameAnchors::D3D11Texture(kBlendedDepthName, terrainBlending.blendedDepthTexture->resource.get(),
							 terrainBlending.blendedDepthTexture->srv.get(), nullptr),
					{}, true };
		}
		return { FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY), {}, false };
	}

	const char* DepthLabel(const DepthSource& a_depth)
	{
		auto& terrainBlending = globals::features::terrainBlending;
		if (!terrainBlending.loaded || !terrainBlending.settings.Enabled)
			return "the z-prepass copy";
		return a_depth.published.empty() ? "Terrain Blending" : "Terrain Blending's graph texture";
	}

	// Every dispatch of the list's capacity, each reading its constants from the execution's latch region and its group counts
	// from its run gate entry.
	struct RaymarchBindings
	{
		org::DeclaredViewToken depth, output;
	};

	struct RaymarchFrame
	{
		std::shared_ptr<const Program> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t sampler = 0;
	};

	class RaymarchPass final : public org::TypedRenderGraphPass<RaymarchPass, RaymarchFrame, RaymarchBindings>
	{
	public:
		// At the build, on the render thread: the state as it is now.
		explicit RaymarchPass(const ShadowState& a_state) :
			program(a_state.program), latch(a_state.latch), gate(a_state.gate), sampler(a_state.sampler.Index()), depth(a_state.depth), output(a_state.output->Graph()) {}

		RaymarchBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto in = depth.resource ? a_builder.ShaderResource(depth.resource).View() :
			                                 a_builder.ShaderResource(org::ResourceIdentifier(depth.published)).View();
			return { in, a_builder.UnorderedAccess(output).View() };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(latch.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(gate.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(depth.resource.get()));
			a_out.push_back(std::hash<std::string_view>{}(depth.published));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(output.get()));
		}

		RaymarchFrame Prepare(const RaymarchBindings&, const org::PassPrepareContext&) const { return { program, latch, gate, sampler }; }

		static void Record(const RaymarchBindings& a_bindings, const RaymarchFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch || !a_frame.gate)
				return;
			const org::RunRef run = a_recording.Run();
			RaymarchBlock block{ a_recording.Index(a_bindings.depth), a_recording.Index(a_bindings.output), a_frame.sampler, {} };
			// Bend's dispatches write separate pixels, so they need no barrier between them (as on D3D11).
			for (std::uint32_t i = 0; i < kMaxDispatches; ++i) {
				block.perFrame = ShaderBindings::LatchOf(*a_frame.latch, run,
					static_cast<std::uint32_t>(offsetof(RaymarchLatchData, constants) + i * sizeof(ScreenSpaceShadows::RaymarchCB)));
				a_frame.gate->Dispatch(a_recording, *a_frame.program->compute, ShaderBindings::Data(block), i);
			}
		}

	private:
		std::shared_ptr<const Program> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t sampler;
		DepthSource depth;
		std::shared_ptr<org::Resource> output;
	};

	/** @brief a_sampleCount's program: from the set built at setup, else built now (logged) and kept. Null when it fails. */
	std::shared_ptr<const Program> ProgramFor(ShadowState& a_state, std::uint32_t a_sampleCount)
	{
		if (const auto found = a_state.programs.find(a_sampleCount); found != a_state.programs.end())
			return found->second;
		logger::info("[ORG] Screen Space Shadows: the {}-sample program was not built at setup; building it now", a_sampleCount);
		auto program = LoadProgram(RenderGraphRuntime::Get().Host()->Device(), a_sampleCount);
		if (program)
			a_state.programs.emplace(a_sampleCount, program);
		return program;
	}

	/** @brief Drops the import of Terrain Blending's D3D11 blended depth when the raymarch stops reading it. */
	void ReleaseBlendImport(const DepthSource& a_previous, const DepthSource& a_next)
	{
		if (a_previous.blendImport && !a_next.blendImport)
			FrameAnchors::ReleaseImport(kBlendedDepthImportId);
	}
}

struct ORGScreenSpaceShadows::Impl
{
	std::shared_ptr<ShadowState> state;
};

bool ORGScreenSpaceShadows::Requested()
{
	static const bool requested = Switches::On("CS_ORG_SCREEN_SPACE_SHADOWS");
	return requested;
}

void ORGScreenSpaceShadows::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Prepass))
		logger::warn("[ORG] Screen Space Shadows stays on D3D11: the Prepass anchor could not be reserved");
}

ORGScreenSpaceShadows& ORGScreenSpaceShadows::Get()
{
	static ORGScreenSpaceShadows s_shadows;
	return s_shadows;
}

ORGScreenSpaceShadows::~ORGScreenSpaceShadows() = default;

bool ORGScreenSpaceShadows::Setup(const D3D11_TEXTURE2D_DESC& a_desc, std::function<void()> a_onFailed)
{
	if (impl)
		Stop();
	if (!Requested())
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Prepass)) {
		logger::warn("[ORG] Screen Space Shadows stays on D3D11: the render graph or its Prepass anchor is not available");
		return false;
	}
	try {
		auto state = std::make_shared<ShadowState>();
		state->onFailed = std::move(a_onFailed);
		// Every sample count the settings can ask for at this size, so a settings change or the upscaler's scale finds its program.
		for (const auto count : SettingsSampleCounts()) {
			auto program = LoadProgram(host->Device(), count);
			if (!program) {
				logger::warn("[ORG] Screen Space Shadows stays on D3D11: the {}-sample program could not be made", count);
				return false;
			}
			state->programs.emplace(count, std::move(program));
		}
		state->program = ProgramFor(*state, globals::features::screenSpaceShadows.GetScaledSampleCount());
		// The D3D11 sampler's: point, border white (beyond the screen is far depth, no shadow).
		state->sampler = ShaderBindings::CreateSampler({ .minFilter = rhi::Filter::Nearest,
			.magFilter = rhi::Filter::Nearest,
			.mipFilter = rhi::MipFilter::Nearest,
			.addressU = rhi::AddressMode::Border,
			.addressV = rhi::AddressMode::Border,
			.addressW = rhi::AddressMode::Border,
			.borderPreset = rhi::BorderPreset::OpaqueWhite });
		// Written in full every frame, but the engine's draws read it through D3D11: exported, so never aliased.
		state->output = std::make_unique<GraphResources::Texture2D>(a_desc, std::string(kTextureName),
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Transient, .d3d11 = GraphResources::Export::SRV });
		state->latch = std::make_shared<org::LatchBlock>("cs.screen-space-shadows.latch", static_cast<std::uint32_t>(sizeof(RaymarchLatchData)));
		// One entry per dispatch of Bend's list: the commit opens those this frame uses.
		state->gate = RenderGraph::RunGate::Create("cs.screen-space-shadows.dispatches", kMaxDispatches);
		state->depth = CurrentDepth();
		if (!state->program || !state->sampler || !*state->output || !state->output->SRV() || !state->gate || !state->depth) {
			logger::warn("[ORG] Screen Space Shadows stays on D3D11: its program, sampler, texture, gate or depth input could not be made");
			ReleaseBlendImport(state->depth, {});
			return false;
		}

		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.passes.push_back({ .name = "cs.screen-space-shadows.clear",
			.anchor = FrameAnchors::Anchor::Prepass,
			.order = 100,
			.kind = FrameAnchors::PassKind::Compute,
			// The clear to white (no shadow), as the D3D11 path clears before its dispatches.
			.make = [state] { return GraphPasses::MakeUavClear(state->output->Graph(), { 1.0f, 1.0f, 1.0f, 1.0f }); } });
		client.passes.push_back({ .name = "cs.screen-space-shadows.raymarch",
			.anchor = FrameAnchors::Anchor::Prepass,
			.order = 101,
			.kind = FrameAnchors::PassKind::Compute,
			.make = [state]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<RaymarchPass>(*state); } });
		client.registerResources = [state](org::RenderGraph& a_graph) { state->output->Register(a_graph); };
		client.gates = { state->gate };
		client.commit = [state](const FrameAnchors::Point&) {
			auto& feature = globals::features::screenSpaceShadows;
			// The gate is closed for this run: a frame without the full sky dispatches nothing.
			if (!feature.ShouldDraw())
				return;
			const auto dispatches = feature.BuildDispatches();
			RaymarchLatchData data{};
			for (std::uint32_t i = 0; i < dispatches.count; ++i)
				data.constants[i] = dispatches.constants[i];
			state->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, data);
			for (std::uint32_t i = 0; i < dispatches.count; ++i)
				state->gate->OpenDispatch(i, dispatches.groups[i][0], dispatches.groups[i][1], dispatches.groups[i][2]);
		};
		client.onFailed = [] {
			logger::error("[ORG] Screen Space Shadows returns to D3D11: the render graph failed");
			auto& self = Get();
			PublishedResources::Withdraw(kTextureName);
			auto onFailed = self.impl ? std::move(self.impl->state->onFailed) : std::function<void()>{};
			self.impl.reset();
			if (onFailed)
				onFailed();
		};
		if (!FrameAnchors::Register(std::move(client))) {
			ReleaseBlendImport(state->depth, {});
			return false;
		}
		// Drawcall Limit Fix's colour passes read it by name, ordered after the raymarch (Integrations/ScreenSpaceShadows).
		if (!PublishedResources::Publish(*state->output)) {
			logger::warn("[ORG] Screen Space Shadows stays on D3D11: the shadow texture could not be published");
			FrameAnchors::Unregister(kClientId);
			ReleaseBlendImport(state->depth, {});
			return false;
		}
		impl = std::make_unique<Impl>();
		impl->state = std::move(state);
		logger::info("[ORG] Screen Space Shadows runs on the render graph ({}x{}, {} samples, {} sample-count programs, depth from {})",
			a_desc.Width, a_desc.Height, impl->state->program->sampleCount, impl->state->programs.size(), DepthLabel(impl->state->depth));
		return true;
	} catch (const std::exception& e) {
		logger::error("[ORG] Screen Space Shadows setup failed, staying on D3D11: {}", e.what());
		Stop();
		return false;
	}
}

bool ORGScreenSpaceShadows::IsActive() const
{
	return impl && RenderGraphRuntime::Get().IsActive();
}

void ORGScreenSpaceShadows::Stop()
{
	FrameAnchors::Unregister(kClientId);
	PublishedResources::Withdraw(kTextureName);
	if (impl)
		ReleaseBlendImport(impl->state->depth, {});
	impl.reset();
}

ID3D11ShaderResourceView* ORGScreenSpaceShadows::Prepass()
{
	if (!IsActive())
		return nullptr;
	auto& state = *impl->state;
	bool changed = false;
	// The sample count follows the settings and the render size (as GetComputeRaymarch's recompile does). The passes are made
	// again with the new program: a graph rebuild, only when the count changes.
	if (const auto sampleCount = globals::features::screenSpaceShadows.GetScaledSampleCount(); sampleCount != state.program->sampleCount) {
		auto program = ProgramFor(state, sampleCount);
		if (!program) {
			logger::error("[ORG] Screen Space Shadows returns to D3D11: the {}-sample program could not be made", sampleCount);
			Stop();
			return nullptr;
		}
		state.program = std::move(program);
		changed = true;
	}
	// Terrain Blending on or off, its graph path started or stopped, or a new depth texture.
	if (auto depth = CurrentDepth(); !(depth == state.depth)) {
		if (!depth) {
			Stop();
			return nullptr;
		}
		ReleaseBlendImport(state.depth, depth);
		state.depth = std::move(depth);
		logger::info("[ORG] Screen Space Shadows: depth now from {}", DepthLabel(state.depth));
		changed = true;
	}
	if (changed)
		FrameAnchors::Refresh(kClientId);
	return state.output->SRV();
}

ID3D11ShaderResourceView* ORGScreenSpaceShadows::ShadowSRV() const
{
	return IsActive() ? impl->state->output->SRV() : nullptr;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Screen Space Shadows always draws with D3D11.
#	include "ORGScreenSpaceShadows.h"

struct ORGScreenSpaceShadows::Impl
{};

bool ORGScreenSpaceShadows::Requested() { return false; }
void ORGScreenSpaceShadows::Reserve() {}

ORGScreenSpaceShadows& ORGScreenSpaceShadows::Get()
{
	static ORGScreenSpaceShadows s_shadows;
	return s_shadows;
}

ORGScreenSpaceShadows::~ORGScreenSpaceShadows() = default;
bool ORGScreenSpaceShadows::Setup(const D3D11_TEXTURE2D_DESC&, std::function<void()>) { return false; }
bool ORGScreenSpaceShadows::IsActive() const { return false; }
ID3D11ShaderResourceView* ORGScreenSpaceShadows::Prepass() { return nullptr; }
ID3D11ShaderResourceView* ORGScreenSpaceShadows::ShadowSRV() const { return nullptr; }
void ORGScreenSpaceShadows::Stop() {}

#endif
