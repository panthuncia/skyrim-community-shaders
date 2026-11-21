// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "TruePBR/ORGGlintsNoise.h"

#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/RenderGraph/RenderGraph.h>

#include <optional>
#include <utility>

namespace
{
	constexpr const char* kClientId = "cs.true-pbr.glints";
	constexpr const char* kShader = "Common/Glints/noisegen.cs.hlsl";
	// TruePBR::SetupGlintsTexture's size, and noisegen.cs.hlsl's group size.
	constexpr UINT kNoiseSize = 128;
	constexpr std::uint32_t kGroupSize = 32;
	// The fill's gate entry, a first-run gate.
	constexpr std::uint32_t kFillEntry = 0;

	// In NOISEGEN_RESOURCES order.
	struct NoiseGenBlock
	{
		ShaderBindings::Handle noise;
	};

	/** @brief What the fill's registration depends on: the program it dispatches. */
	struct GlintsKey
	{
		std::uint64_t program = 0;
		bool operator==(const GlintsKey&) const = default;
	};

	/** @brief Declared once for the session. */
	RenderGraph::Frame::DeclaredClient& GlintsClient()
	{
		static RenderGraph::Frame::DeclaredClient client;
		return client;
	}

	/** @brief The program's serial, raised when a reload replaces it. The client's key reads it. */
	RenderGraph::Frame::Observed<std::uint64_t>& GlintsProgramSerial()
	{
		static RenderGraph::Frame::Observed<std::uint64_t> serial{ "TruePBR.Glints.Program" };
		return serial;
	}
}

struct ORGGlintsNoise::Impl
{
	RenderGraph::Programs::Fixed<ComputeProgram> program;
	std::unique_ptr<GraphResources::Texture2D> noise;
	// The fill's group counts. Each registration's commit opens it until a run with the fill submits.
	std::shared_ptr<RenderGraph::RunGate> gate;
	// Set by the executed signal of the run that submitted the fill. A new registration clears it.
	bool filled = false;
};

namespace
{
	using Impl = ORGGlintsNoise::Impl;
	using RenderGraph::AnchorPasses::Bind;

	/** @brief Render thread. A later queue gates the next revision, so the client's make sees it built. */
	RenderGraph::Programs::Fixed<ComputeProgram> QueueFillProgram()
	{
		return RenderGraph::Programs::Queue("TruePBR", ComputeProgram::Desc{ .source = kShader, .constantWords = ShaderBindings::Words<NoiseGenBlock>() });
	}
}

bool ORGGlintsNoise::Filled() const
{
	return impl && impl->filled;
}

ID3D11ShaderResourceView* ORGGlintsNoise::NoiseSRV() const
{
	return Filled() ? impl->noise->SRV() : nullptr;
}

void ORGGlintsNoise::DeclareClient()
{
	GlintsClient() = RenderGraph::Frame::DeclareClient<GlintsKey>(
		kClientId, RenderGraph::Frame::DependsOn(GlintsProgramSerial().Var()),
		[](const RenderGraph::Frame::Inputs& a_in) -> std::optional<GlintsKey> {
			const auto& self = Get();
			if (!self.impl)
				return std::nullopt;
			// Null before the first reload: the setup program.
			const auto* serial = GlintsProgramSerial().Get(a_in);
			return GlintsKey{ serial ? *serial : 0 };
		},
		[](const GlintsKey&, FrameAnchors::ClientDesc& a_client) {
			auto& self = Get();
			return self.impl && self.MakeClient(self.impl, a_client);
		});
}

bool ORGGlintsNoise::MakeClient(const std::shared_ptr<Impl>& a_impl, FrameAnchors::ClientDesc& a_client)
{
	// The dispatch stays in the graph and takes its group counts from the gate, which this registration's commit opens
	// until a run with the fill submits. Other runs dispatch nothing.
	RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::EarlyPrepass, kClientId, 0, {}, a_impl->gate);
	builder.Add(RenderGraph::AnchorPasses::Dispatch{
		.name = "noise-fill",
		.program = a_impl->program.Get(),
		.bindings = { Bind::Uav(a_impl->noise->Graph()) },
		.gate = kFillEntry });
	if (builder.Failed())
		Fatal(builder.Failure());

	auto& client = a_client;
	client.id = kClientId;
	client.passes = builder.Take();
	client.gates = { a_impl->gate };
	client.registerResources = [a_impl](org::RenderGraph& a_graph) { a_impl->noise->Register(a_graph); };
	const auto fill = std::make_shared<RenderGraph::FirstRun>();
	// Active until a run whose commit opened the fill was submitted.
	client.active = [fill] { return fill->Pending(); };
	client.commit = [a_impl, fill](const FrameAnchors::Point&) {
		constexpr std::uint32_t groups = (kNoiseSize + kGroupSize - 1) / kGroupSize;
		fill->Open(*a_impl->gate, kFillEntry, groups, groups);
	};
	client.executed = [a_impl, fill](const FrameAnchors::Point&) {
		if (!fill->Executed())
			return;
		a_impl->filled = true;
		logger::info("[ORG] TruePBR glints: the noise was filled; its gate stays closed from now on");
	};
	a_impl->filled = false;
	logger::info("[ORG] TruePBR glints: a fill client for the next revision");
	return true;
}

void ORGGlintsNoise::Setup()
{
	if (!BeginSetup())
		return;
	auto next = std::make_shared<Impl>();
	next->gate = RenderGraph::RunGate::Create("TruePBR::GlintsNoiseGate");
	next->program = QueueFillProgram();

	// TruePBR::SetupGlintsTexture's texture.
	const D3D11_TEXTURE2D_DESC desc{ .Width = kNoiseSize,
		.Height = kNoiseSize,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS };
	// Engine draws read it at t20 until they are ported. Filled once, so it lives for good.
	next->noise = std::make_unique<GraphResources::Texture2D>(desc, std::string(kNoiseId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
	if (!next->noise->SRV())
		Fatal(fmt::format("the {}x{} noise texture could not be created", kNoiseSize, kNoiseSize));

	// DCLF's colour passes read it by name, ordered after the fill.
	if (!PublishedResources::Publish(*next->noise))
		Fatal("the noise texture could not be published");
	impl = std::move(next);
	// The fill registers when the next revision starts, inside the frame's adoption point.
	DeclareClient();
	logger::info("[ORG] TruePBR glints: the {}x{} noise is filled on the render graph at EarlyPrepass", kNoiseSize, kNoiseSize);
}

void ORGGlintsNoise::ClearShaderCache()
{
	if (!impl)
		return;
	// A reload refills with the program as the file is now. The next revision starts once it is built.
	// The fill's spec holds its program, so that revision registers it again, which fills again.
	impl->program = QueueFillProgram();
	GlintsProgramSerial().Observe(++programSerial);
	logger::info("[ORG] TruePBR glints: the noise is filled again after a shader reload, from the next revision");
}

ORGGlintsNoise::ORGGlintsNoise() = default;
ORGGlintsNoise::~ORGGlintsNoise() = default;

