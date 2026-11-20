#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "TruePBR/ORGGlintsNoise.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

bool ORGGlintsNoise::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
	return requested;
}

ORGGlintsNoise& ORGGlintsNoise::Get()
{
	static ORGGlintsNoise s_noise;
	return s_noise;
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Frame/Declare.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>

#	include <optional>
#	include <utility>

namespace
{
	constexpr const char* kClientId = "cs.true-pbr.glints";
	constexpr const char* kShader = "Common/Glints/noisegen.cs.hlsl";
	// TruePBR::SetupGlintsTexture's size, and noisegen.cs.hlsl's group size.
	constexpr UINT kNoiseSize = 128;
	constexpr std::uint32_t kGroupSize = 32;
	// Glints2023.hlsli's Glint2023NoiseMap register.
	constexpr UINT kNoiseRegister = 20;
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

	/** @brief Declared once for the session; failures and reloads evaluate it again. */
	RenderGraph::Frame::DeclaredClient& GlintsClient()
	{
		static RenderGraph::Frame::DeclaredClient client;
		return client;
	}
}

struct ORGGlintsNoise::Impl
{
	std::shared_ptr<const ComputeProgram> program;
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

	std::shared_ptr<const ComputeProgram> LoadFillProgram(rhi::Device a_device)
	{
		return ComputeProgram::Load(a_device, { .source = kShader, .constantWords = ShaderBindings::Words<NoiseGenBlock>() });
	}

	/** @brief Unbinds t20 before the export goes. Its texture is freed once D3D11 is past the commands issued so far. */
	void UnbindNoise()
	{
		ID3D11ShaderResourceView* none = nullptr;
		globals::d3d::context->PSSetShaderResources(kNoiseRegister, 1, &none);
	}
}

void ORGGlintsNoise::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::EarlyPrepass))
		logger::warn("[ORG] TruePBR glints stay on D3D11: the EarlyPrepass anchor could not be reserved");
}

bool ORGGlintsNoise::Active() const
{
	return impl && !failed && RenderGraphRuntime::Get().IsActive();
}

bool ORGGlintsNoise::Filled() const
{
	return Active() && impl->filled;
}

ID3D11ShaderResourceView* ORGGlintsNoise::NoiseSRV() const
{
	return Filled() ? impl->noise->SRV() : nullptr;
}

void ORGGlintsNoise::Fail()
{
	failed = true;
	// The next revision removes the client (its key is empty once failed).
	GlintsClient().Invalidate();
	PublishedResources::Withdraw(kNoiseId);
	if (impl)
		UnbindNoise();
	impl.reset();
}

bool ORGGlintsNoise::DeclareClient()
{
	// Set up again after a failure: a new key, so the next revision registers the new texture even if no revision saw the failure.
	if (GlintsClient()) {
		++programSerial;
		GlintsClient().Invalidate();
		return true;
	}
	GlintsClient() = RenderGraph::Frame::DeclareClient<GlintsKey>(
		kClientId, {},
		[](const RenderGraph::Frame::Inputs&) -> std::optional<GlintsKey> {
			const auto& self = Get();
			if (!self.impl || self.failed)
				return std::nullopt;
			return GlintsKey{ self.programSerial };
		},
		[](const GlintsKey&, FrameAnchors::ClientDesc& a_client) {
			auto& self = Get();
			return self.impl && self.MakeClient(self.impl, a_client);
		});
	return static_cast<bool>(GlintsClient());
}

bool ORGGlintsNoise::MakeClient(const std::shared_ptr<Impl>& a_impl, FrameAnchors::ClientDesc& a_client)
{
	// The dispatch stays in the graph and takes its group counts from the gate, which this registration's commit opens
	// until a run with the fill submits. Other runs dispatch nothing.
	RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::EarlyPrepass, kClientId, 0, {}, a_impl->gate);
	builder.Add(RenderGraph::AnchorPasses::Dispatch{
		.name = "noise-fill", .program = a_impl->program, .bindings = { Bind::Uav(a_impl->noise->Graph()) }, .gate = kFillEntry });
	if (builder.Failed()) {
		logger::error("[ORG] TruePBR glints: {}", builder.Failure());
		return false;
	}

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

bool ORGGlintsNoise::Setup()
{
	if (!Requested() || failed || impl)
		return impl != nullptr;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::EarlyPrepass)) {
		logger::warn("[ORG] TruePBR glints: CS_ORG_GLINTS=1 but the render graph or its EarlyPrepass anchor is not available; back to D3D11");
		return false;
	}
	auto next = std::make_shared<Impl>();
	next->gate = RenderGraph::RunGate::Create("TruePBR::GlintsNoiseGate");
	if (!next->gate) {
		logger::error("[ORG] TruePBR glints: the fill's run gate could not be created; back to D3D11");
		Fail();
		return false;
	}
	next->program = LoadFillProgram(host->Device());
	if (!next->program) {
		logger::error("[ORG] TruePBR glints: {} could not be compiled; back to D3D11", kShader);
		Fail();
		programFailed = true;
		return false;
	}

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
	if (!*next->noise || !next->noise->SRV()) {
		logger::error("[ORG] TruePBR glints: the {}x{} noise texture could not be created; back to D3D11", kNoiseSize, kNoiseSize);
		Fail();
		return false;
	}

	// DCLF's colour passes read it by name, ordered after the fill.
	if (!PublishedResources::Publish(*next->noise)) {
		logger::error("[ORG] TruePBR glints: the noise texture could not be published; back to D3D11");
		Fail();
		return false;
	}
	impl = std::move(next);
	// The fill registers when the next revision starts, inside the frame's adoption point.
	if (!DeclareClient()) {
		logger::error("[ORG] TruePBR glints: the fill's client could not be declared; back to D3D11");
		Fail();
		return false;
	}
	logger::info("[ORG] TruePBR glints: the {}x{} noise is filled on the render graph at EarlyPrepass", kNoiseSize, kNoiseSize);
	return true;
}

void ORGGlintsNoise::ClearShaderCache()
{
	if (failed) {
		// Only a failed compile is retried.
		if (programFailed) {
			failed = false;
			programFailed = false;
			Setup();
		}
		return;
	}
	auto* host = RenderGraphRuntime::Get().Host();
	if (!impl || !host)
		return;
	// A reload refills with the program as the file is now. Nothing is bound until then.
	auto program = LoadFillProgram(host->Device());
	if (!program) {
		logger::error("[ORG] TruePBR glints: {} could not be compiled; the noise keeps its last fill", kShader);
		return;
	}
	// The fill's spec holds its program, so the next revision registers it again, which fills again.
	impl->program = std::move(program);
	++programSerial;
	GlintsClient().Invalidate();
	logger::info("[ORG] TruePBR glints: the noise is filled again after a shader reload, from the next revision");
}

ORGGlintsNoise::~ORGGlintsNoise() = default;

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph the glints noise is always made on D3D11.
struct ORGGlintsNoise::Impl
{};

void ORGGlintsNoise::Reserve() {}
bool ORGGlintsNoise::Setup() { return false; }
bool ORGGlintsNoise::Active() const { return false; }
bool ORGGlintsNoise::Filled() const { return false; }
ID3D11ShaderResourceView* ORGGlintsNoise::NoiseSRV() const { return nullptr; }
void ORGGlintsNoise::ClearShaderCache() {}
void ORGGlintsNoise::Fail() { failed = true; }
bool ORGGlintsNoise::MakeClient(const std::shared_ptr<Impl>&, FrameAnchors::ClientDesc&) { return false; }
bool ORGGlintsNoise::DeclareClient() { return false; }
ORGGlintsNoise::~ORGGlintsNoise() = default;

#endif
