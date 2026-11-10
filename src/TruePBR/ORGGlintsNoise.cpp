#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "TruePBR/ORGGlintsNoise.h"

#include "RenderGraph/Switches.h"

bool ORGGlintsNoise::Requested()
{
	static const bool requested = Switches::On("CS_ORG_GLINTS");
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
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>

#	include <utility>

namespace
{
	constexpr const char* kClientId = "cs.true-pbr.glints";
	constexpr const char* kShader = "Common/Glints/noisegen.cs.hlsl";
	// TruePBR::SetupGlintsTexture's size, and noisegen.cs.hlsl's group size.
	constexpr UINT kNoiseSize = 128;
	constexpr std::uint32_t kGroupSize = 32;
	// TruePBR binds the noise here for the engine's Lighting pixel shaders (Glints2023.hlsli's Glint2023NoiseMap).
	constexpr UINT kNoiseRegister = 20;

	// noisegen.cs.hlsl's push block (NOISEGEN_RESOURCES), in list order.
	struct NoiseGenBlock
	{
		ShaderBindings::Handle noise;
	};
}

struct ORGGlintsNoise::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	std::unique_ptr<GraphResources::Texture2D> noise;
	// Set by the executed signal of the run that submitted the fill. A new registration (a reload) clears it.
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

	/** @brief Unbinds t20 before the export goes: its texture is freed once D3D11 is past the commands issued until then. */
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
	FrameAnchors::Unregister(kClientId);
	PublishedResources::Withdraw(kNoiseId);
	if (impl)
		UnbindNoise();
	impl.reset();
}

bool ORGGlintsNoise::RegisterClient(const std::shared_ptr<Impl>& a_impl)
{
	// The fill is one-shot: it runs at the anchor's next run, and the graph is built again without it after that run.
	RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::EarlyPrepass, kClientId, 0);
	constexpr std::uint32_t groups = (kNoiseSize + kGroupSize - 1) / kGroupSize;
	builder.Add(RenderGraph::AnchorPasses::Dispatch{
		.name = "noise-fill", .program = a_impl->program, .bindings = { Bind::Uav(a_impl->noise->Graph()) }, .groups = { groups, groups, 1 } });
	if (builder.Failed()) {
		logger::error("[ORG] TruePBR glints: {}", builder.Failure());
		return false;
	}

	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.passes = builder.Take();
	// Builder::Add(Dispatch) makes a pass that runs every run: the fill is made one-shot here.
	for (auto& pass : client.passes)
		pass.once = true;
	client.registerResources = [a_impl](org::RenderGraph& a_graph) { a_impl->noise->Register(a_graph); };
	// Active until the run with the fill was submitted.
	client.active = [a_impl] { return !a_impl->filled; };
	client.executed = [a_impl](const FrameAnchors::Point&) {
		if (a_impl->filled)
			return;
		a_impl->filled = true;
		logger::info("[ORG] TruePBR glints: the noise was filled; the fill pass is dropped");
	};
	client.onFailed = [] {
		logger::error("[ORG] TruePBR glints: the render graph failed; back to D3D11");
		PublishedResources::Withdraw(kNoiseId);
		UnbindNoise();
		auto& self = Get();
		self.failed = true;
		self.impl.reset();
		if (self.onFailed)
			self.onFailed();
	};
	a_impl->filled = false;
	return FrameAnchors::Register(std::move(client));
}

bool ORGGlintsNoise::Setup(std::function<void()> a_onFailed)
{
	if (!Requested() || failed || impl)
		return impl != nullptr;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::EarlyPrepass)) {
		logger::warn("[ORG] TruePBR glints: CS_ORG_GLINTS=1 but the render graph or its EarlyPrepass anchor is not available; back to D3D11");
		return false;
	}
	onFailed = std::move(a_onFailed);
	auto next = std::make_shared<Impl>();
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
	// Engine draws read it at t20 until they are ported. It is filled once, so it lives for good.
	next->noise = std::make_unique<GraphResources::Texture2D>(desc, std::string(kNoiseId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
	if (!*next->noise || !next->noise->SRV()) {
		logger::error("[ORG] TruePBR glints: the {}x{} noise texture could not be created; back to D3D11", kNoiseSize, kNoiseSize);
		Fail();
		return false;
	}

	if (!RegisterClient(next)) {
		Fail();
		return false;
	}
	// Drawcall Limit Fix's colour passes read it by name, ordered after the fill (Integrations/TruePBR).
	if (!PublishedResources::Publish(*next->noise)) {
		logger::error("[ORG] TruePBR glints: the noise texture could not be published; back to D3D11");
		Fail();
		return false;
	}
	impl = std::move(next);
	logger::info("[ORG] TruePBR glints: the {}x{} noise is filled on the render graph at EarlyPrepass", kNoiseSize, kNoiseSize);
	return true;
}

void ORGGlintsNoise::ClearShaderCache()
{
	if (failed) {
		// Only a failed compile is tried again.
		if (programFailed) {
			failed = false;
			programFailed = false;
			Setup(onFailed);
		}
		return;
	}
	auto* host = RenderGraphRuntime::Get().Host();
	if (!impl || !host)
		return;
	// The noise is filled once, so a reload fills it again with the program as the file is now. Until then nothing is bound.
	auto program = LoadFillProgram(host->Device());
	if (!program) {
		logger::error("[ORG] TruePBR glints: {} could not be compiled; the noise keeps its last fill", kShader);
		return;
	}
	// The fill's spec holds its program, so the client is registered again (the fill pending again) instead of rearmed.
	impl->program = std::move(program);
	if (!RegisterClient(impl)) {
		Fail();
		return;
	}
	logger::info("[ORG] TruePBR glints: the noise is filled again after a shader reload");
}

ORGGlintsNoise::~ORGGlintsNoise() = default;

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: the glints noise is always made on D3D11.
struct ORGGlintsNoise::Impl
{};

void ORGGlintsNoise::Reserve() {}
bool ORGGlintsNoise::Setup(std::function<void()>) { return false; }
bool ORGGlintsNoise::Active() const { return false; }
bool ORGGlintsNoise::Filled() const { return false; }
ID3D11ShaderResourceView* ORGGlintsNoise::NoiseSRV() const { return nullptr; }
void ORGGlintsNoise::ClearShaderCache() {}
void ORGGlintsNoise::Fail() { failed = true; }
bool ORGGlintsNoise::RegisterClient(const std::shared_ptr<Impl>&) { return false; }
ORGGlintsNoise::~ORGGlintsNoise() = default;

#endif
