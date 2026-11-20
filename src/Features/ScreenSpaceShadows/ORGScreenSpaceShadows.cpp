#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGScreenSpaceShadows.h"
#	include "RenderGraph/Engine/Native/Native.h"

#	include "Features/ScreenSpaceShadows.h"
#	include "Features/TerrainBlending.h"
#	include "Features/TerrainBlending/ORGTerrainBlending.h"
#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Frame/Declare.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/Switches.h"
#	include "RenderGraph/UavClearPass.h"
#	include "Utils/Game.h"
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#		include "RenderGraph/ProgramBuild.h"
#	endif

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/VersionedTexture.h>

#	include <cstddef>
#	include <optional>
#	include <string>
#	include <utility>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.screen-space-shadows";
	constexpr const char* kShader = "ScreenSpaceShadows/RaymarchCS.hlsl";
	// Published under this name for DCLF's t45.
	constexpr std::string_view kTextureName = ORGScreenSpaceShadows::kShadowTextureId;
	constexpr std::uint32_t kMaxDispatches = ScreenSpaceShadows::Dispatches::kMax;
	// After the dispatches' entries. Every commit opens it, as the D3D11 path clears on every run.
	constexpr std::uint32_t kClearEntry = kMaxDispatches;

	// Three handles, then PerFrame's latch region.
	struct RaymarchBlock
	{
		ShaderBindings::Handle depthTexture, outputTexture, pointBorderSampler;
		ShaderBindings::Latch perFrame;
	};

	// Each dispatch's RaymarchConstants. The group counts are in the run gate.
	struct RaymarchLatchData
	{
		ScreenSpaceShadows::RaymarchCB constants[kMaxDispatches];
	};
	static_assert(sizeof(ScreenSpaceShadows::RaymarchCB) % 16 == 0);

	struct Program
	{
		std::shared_ptr<const ComputeProgram> compute;
		std::uint32_t sampleCount = 0;
	};

	using Defines = std::vector<std::pair<std::wstring, std::wstring>>;

	/**
	* @brief Render thread, at setup. Every sample count's defines, SAMPLE_COUNT's value set per build. The common defines read
	* State, so they are resolved here and not on the programs lane.
	*/
	Defines ResolveDefines()
	{
		Defines defines{ { L"SAMPLE_COUNT", L"0" } };
		// As the D3D11 build, the depth texture's HLSL type follows Terrain Blending.
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ L"TERRAIN_BLENDING", L"" });
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
		return RenderGraph::ProgramBuild::WithCommonDefines(defines, L"cs_6_6");
#	else
		return defines;
#	endif
	}

	/** @brief Any thread. Null, logged, on failure. */
	std::shared_ptr<const Program> LoadProgram(rhi::Device a_device, std::uint32_t a_sampleCount, Defines a_defines)
	{
		for (auto& [name, value] : a_defines)
			if (name == L"SAMPLE_COUNT")
				value = std::to_wstring(a_sampleCount);
		auto program = std::make_shared<Program>();
		program->sampleCount = a_sampleCount;
		program->compute = ComputeProgram::Load(a_device,
			{ .source = kShader, .constantWords = ShaderBindings::Words<RaymarchBlock>(), .defines = std::move(a_defines), .commonDefines = false });
		return program->compute ? program : nullptr;
	}

	/**
	* @brief The sample count a revision's values give: the multiplier at the render size of the display size, as
	* ScreenSpaceShadows::GetScaledSampleCount gives it on D3D11. Before the display size is known, the engine's screen size.
	*/
	std::uint32_t SampleCountFor(const RenderGraph::Frame::Inputs& a_in)
	{
		const auto display = a_in.Get(RenderGraph::Frame::Vars::DisplaySize());
		const auto* graphics = globals::game::graphicsState;
		const float2 screen{ static_cast<float>(display.width ? display.width : graphics->screenWidth),
			static_cast<float>(display.height ? display.height : graphics->screenHeight) };
		return ScreenSpaceShadows::ScaleSampleCount(a_in.Get(globals::features::screenSpaceShadows.bendSettings.SampleCount), Util::ConvertToDynamic(screen));
	}

	/** @brief The z-prepass copy, or Terrain Blending's published blended depth. */
	struct DepthSource
	{
		FrameAnchors::Texture resource;
		std::string_view published;

		explicit operator bool() const { return resource || !published.empty(); }
		bool operator==(const DepthSource&) const = default;
	};

	/** @brief What the last Prepass found that is not a variable yet. The client's key reads it. */
	struct Observed
	{
		FrameAnchors::Texture prepassCopy;
		bool terrainBlendingGraph = false;  // Terrain Blending's blend runs on the graph and its blended depth is published

		bool operator==(const Observed&) const = default;
	};

	Observed Observe()
	{
		Observed observed;
		observed.prepassCopy = FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY);
		observed.terrainBlendingGraph = globals::features::terrainBlending.loaded && ORGTerrainBlending::Get().Active() &&
		                                PublishedResources::IsPublished(ORGTerrainBlending::kBlendedDepthId);
		return observed;
	}

	/**
	* @brief Terrain Blending's published blended depth while its graph path blends in the revision, else the z-prepass copy,
	* since a D3D11 import is not ordered after its writer. Terrain Blending's Enabled switches its blend and this in one revision.
	*/
	DepthSource DepthFor(const RenderGraph::Frame::Inputs& a_in, const Observed& a_observed)
	{
		if (a_observed.terrainBlendingGraph && a_in.Get(globals::features::terrainBlending.settings.Enabled))
			return { nullptr, ORGTerrainBlending::kBlendedDepthId };
		return { a_observed.prepassCopy, {} };
	}

	const char* DepthLabel(const DepthSource& a_depth)
	{
		return a_depth.published.empty() ? "the z-prepass copy" : "Terrain Blending's graph texture";
	}

	/** @brief What the client is made for. A disabled feature keeps the clear, since readers Load t45 by pixel. */
	struct Shape
	{
		std::shared_ptr<const Program> program;  // null while disabled: no raymarch
		DepthSource depth;

		bool operator==(const Shape&) const = default;
	};

	/** @brief Made at setup and kept. */
	struct Fixed
	{
		RenderGraph::Frame::DeclaredTexture2D output;
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
		org::OwnedDescriptorBinding sampler;
	};

	// Every dispatch of the list's capacity, with constants from the latch and counts from the gate.
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
		RaymarchPass(const Fixed& a_fixed, const Shape& a_shape) :
			program(a_shape.program), latch(a_fixed.latch), gate(a_fixed.gate), sampler(a_fixed.sampler.Index()), depth(a_shape.depth),
			output(a_fixed.output->Versioned())
		{}

		RaymarchBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto in = depth.resource ? a_builder.ShaderResource(depth.resource).View() :
			                                 a_builder.ShaderResource(org::ResourceIdentifier(depth.published)).View();
			// A resize is a new version of the slot, written with no build.
			return { in, a_builder.UnorderedAccess(org::ResourceResolverAndRange(*output)).View() };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(latch.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(gate.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(depth.resource.Key()));
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
			// Bend's dispatches write separate pixels, so they need no barrier between them, as on D3D11.
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
		std::shared_ptr<org::VersionedTexture> output;
	};

	/** @brief The registration for a new shape, when a revision starts. */
	bool MakeClient(const std::shared_ptr<const Fixed>& a_fixed, const Shape& a_shape, FrameAnchors::ClientDesc& a_client)
	{
		a_client.id = kClientId;
		a_client.passes.push_back({ .name = "cs.screen-space-shadows.clear",
			.anchor = FrameAnchors::Anchor::Prepass,
			.order = 100,
			.kind = FrameAnchors::PassKind::Compute,
			// Clear to white, as the D3D11 path does before its dispatches. Skips a run that left its entry closed.
			.make = [a_fixed] { return GraphPasses::MakeUavClear(*a_fixed->output, { 1.0f, 1.0f, 1.0f, 1.0f }, { a_fixed->gate, kClearEntry }); },
			.atExecution = true });
		if (a_shape.program)
			a_client.passes.push_back({ .name = "cs.screen-space-shadows.raymarch",
				.anchor = FrameAnchors::Anchor::Prepass,
				.order = 101,
				.kind = FrameAnchors::PassKind::Compute,
				.make = [a_fixed, a_shape]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<RaymarchPass>(*a_fixed, a_shape); } });
		a_client.registerResources = [a_fixed](org::RenderGraph& a_graph) { a_fixed->output->Register(a_graph); };
		a_client.gates = { a_fixed->gate };
		const bool raymarch = static_cast<bool>(a_shape.program);
		a_client.commit = [a_fixed, raymarch](const FrameAnchors::Point&) {
			a_fixed->gate->OpenDispatch(kClearEntry, 1);
			// A frame without the full sky dispatches nothing.
			if (!raymarch || !ScreenSpaceShadows::SkyFull())
				return;
			// The bound output's extent is the adopted display size.
			const auto extent = RenderGraph::Frame::Revision::Adopted().Extent(a_fixed->output);
			const auto dispatches = globals::features::screenSpaceShadows.BuildDispatches(float2{ static_cast<float>(extent[0]), static_cast<float>(extent[1]) });
			RaymarchLatchData data{};
			for (std::uint32_t i = 0; i < dispatches.count; ++i)
				data.constants[i] = dispatches.constants[i];
			a_fixed->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, data);
			for (std::uint32_t i = 0; i < dispatches.count; ++i)
				a_fixed->gate->OpenDispatch(i, dispatches.groups[i][0], dispatches.groups[i][1], dispatches.groups[i][2]);
		};
		logger::info("[ORG] Screen Space Shadows: a client for the next revision ({}, depth from {})",
			a_shape.program ? fmt::format("{} samples", a_shape.program->sampleCount) : std::string("off, clear only"), DepthLabel(a_shape.depth));
		return true;
	}
}

struct ORGScreenSpaceShadows::Impl
{
	std::shared_ptr<const Fixed> fixed;
	// Declared at Setup and kept. A new sample count is a program built off the render thread, a new shape a new client.
	RenderGraph::Frame::DeclaredProgram<Program> program;
	RenderGraph::Frame::DeclaredClient client;
	Observed observed;
};

bool ORGScreenSpaceShadows::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
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

bool ORGScreenSpaceShadows::Setup(const D3D11_TEXTURE2D_DESC& a_desc)
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
		using namespace RenderGraph::Frame;
		const auto& bend = globals::features::screenSpaceShadows.bendSettings;
		const auto& terrainEnabled = globals::features::terrainBlending.settings.Enabled;
		auto fixed = std::make_shared<Fixed>();
		// Point, border white, since beyond the screen is far depth with no shadow.
		fixed->sampler = ShaderBindings::CreateSampler({ .minFilter = rhi::Filter::Nearest,
			.magFilter = rhi::Filter::Nearest,
			.mipFilter = rhi::MipFilter::Nearest,
			.addressU = rhi::AddressMode::Border,
			.addressV = rhi::AddressMode::Border,
			.addressW = rhi::AddressMode::Border,
			.borderPreset = rhi::BorderPreset::OpaqueWhite });
		// The engine reads it through D3D11, so exported and never aliased. Declared on the display size (decision 6a): a new size
		// is a new version, adopted with a frame revision, with no graph build. Before the display size is known, the shadow mask's.
		fixed->output = DeclareTexture2D(std::string(kTextureName), DependsOn(Vars::DisplaySize()),
			[a_desc](const Inputs& a_in) {
				const auto display = a_in.Get(Vars::DisplaySize());
				D3D11_TEXTURE2D_DESC desc = a_desc;
				desc.Width = display.width ? display.width : a_desc.Width;
				desc.Height = display.height ? display.height : a_desc.Height;
				return desc;
			},
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Transient, .d3d11 = GraphResources::Export::SRV });
		fixed->latch = std::make_shared<org::LatchBlock>("cs.screen-space-shadows.latch", static_cast<std::uint32_t>(sizeof(RaymarchLatchData)));
		// One entry per dispatch of Bend's list, plus the clear.
		fixed->gate = RenderGraph::RunGate::Create("cs.screen-space-shadows.dispatches", kMaxDispatches + 1);
		if (!fixed->sampler || !fixed->output || !*fixed->output || !fixed->output->SRV() || !fixed->output->Versioned() || !fixed->gate) {
			logger::warn("[ORG] Screen Space Shadows stays on D3D11: its sampler, texture or gate could not be made");
			return false;
		}
		auto made = std::make_unique<Impl>();
		made->observed = Observe();
		if (!made->observed.prepassCopy) {
			logger::warn("[ORG] Screen Space Shadows stays on D3D11: the z-prepass copy could not be imported");
			return false;
		}
		// The sample count follows the multiplier and the render size. Its first program is built here; others on the programs
		// lane, before the revision that needs them starts.
		made->program = DeclareProgram("ScreenSpaceShadows::Raymarch", DependsOn(Vars::DisplaySize(), Vars::DynamicRatios(), bend.SampleCount),
			[](const Inputs& a_in) { return SampleCountFor(a_in); },
			[device = host->Device(), defines = ResolveDefines()](const std::uint32_t& a_sampleCount) { return LoadProgram(device, a_sampleCount, defines); });
		if (!made->program) {
			logger::warn("[ORG] Screen Space Shadows stays on D3D11: its program could not be made");
			return false;
		}
		const std::shared_ptr<const Fixed> shared = fixed;
		auto* self = made.get();
		made->client = DeclareClient<Shape>(
			kClientId, DependsOn(bend.Enable, bend.SampleCount, Vars::DisplaySize(), Vars::DynamicRatios(), terrainEnabled),
			[self](const Inputs& a_in) -> std::optional<Shape> {
				Shape shape;
				if (a_in.Get(globals::features::screenSpaceShadows.bendSettings.Enable))
					shape.program = self->program.For(a_in);
				shape.depth = DepthFor(a_in, self->observed);
				if (!shape.depth)
					return std::nullopt;
				return shape;
			},
			[shared](const Shape& a_shape, FrameAnchors::ClientDesc& a_client) { return MakeClient(shared, a_shape, a_client); });
		if (!made->client) {
			logger::warn("[ORG] Screen Space Shadows stays on D3D11: its client could not be declared");
			return false;
		}
		// DCLF's colour passes read it by name, ordered after the raymarch. Published once: readers follow its versions.
		if (!PublishedResources::Publish(*fixed->output)) {
			logger::warn("[ORG] Screen Space Shadows stays on D3D11: the shadow texture could not be published");
			return false;
		}
		made->fixed = shared;
		impl = std::move(made);
		logger::info("[ORG] Screen Space Shadows runs on the render graph ({}x{}, {} samples, depth from {}); its texture, program and client follow "
					 "the frame revisions",
			a_desc.Width, a_desc.Height, RenderGraph::Frame::Revision::Adopted().Program(impl->program)->sampleCount,
			DepthLabel(DepthFor(Inputs(nullptr), impl->observed)));
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
	impl.reset();
}

ID3D11ShaderResourceView* ORGScreenSpaceShadows::Prepass()
{
	if (!IsActive())
		return nullptr;
	// Terrain Blending's graph path started or stopped, or a new z-prepass copy: not variables yet, so the next revision
	// makes the client for them.
	if (auto observed = Observe(); !(observed == impl->observed)) {
		if (!observed.prepassCopy) {
			logger::error("[ORG] Screen Space Shadows returns to D3D11: the z-prepass copy could not be imported");
			Stop();
			return nullptr;
		}
		impl->observed = std::move(observed);
		impl->client.Invalidate();
	}
	return impl->fixed->output->SRV();
}

ID3D11ShaderResourceView* ORGScreenSpaceShadows::ShadowSRV() const
{
	return IsActive() ? impl->fixed->output->SRV() : nullptr;
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph Screen Space Shadows always draws with D3D11.
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
bool ORGScreenSpaceShadows::Setup(const D3D11_TEXTURE2D_DESC&) { return false; }
bool ORGScreenSpaceShadows::IsActive() const { return false; }
ID3D11ShaderResourceView* ORGScreenSpaceShadows::Prepass() { return nullptr; }
ID3D11ShaderResourceView* ORGScreenSpaceShadows::ShadowSRV() const { return nullptr; }
void ORGScreenSpaceShadows::Stop() {}

#endif
