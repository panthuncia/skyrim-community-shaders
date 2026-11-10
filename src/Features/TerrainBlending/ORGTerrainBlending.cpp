#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/TerrainBlending/ORGTerrainBlending.h"

#include "RenderGraph/Switches.h"

bool ORGTerrainBlending::Requested()
{
	static const bool requested = Switches::On("CS_ORG_TERRAIN_BLENDING");
	return requested;
}

ORGTerrainBlending& ORGTerrainBlending::Get()
{
	static ORGTerrainBlending s_terrainBlending;
	return s_terrainBlending;
}

ORGTerrainBlending::~ORGTerrainBlending() = default;

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Features/TerrainBlending.h"
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
#	include <Resources/ExternalTextureResource.h>

#	include <array>
#	include <utility>

namespace
{
	constexpr const char* kClientId = "cs.terrain-blending";
	constexpr const char* kShader = "TerrainBlending/DepthBlend.hlsl";
	constexpr const char* kTerrainDepthId = "cs.terrain-blending.terrain-depth";
	// DepthBlend.hlsl's numthreads.
	constexpr std::uint32_t kGroupSize = 8;

	// DepthBlend.hlsl's push block with MASK_OUTPUT (DEPTH_BLEND_RESOURCES), in list order.
	struct DepthBlendBlock
	{
		ShaderBindings::Handle blendedDepth, blendedDepth16, mask, mainDepth, terrainDepth;
	};
}

struct ORGTerrainBlending::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	std::unique_ptr<GraphResources::Texture2D> blendedDepth;
	std::unique_ptr<GraphResources::Texture2D> blendedDepth16;
	std::unique_ptr<GraphResources::Texture2D> mask;
	// The engine's main depth and Terrain Blending's terrain depth, both D3D11 textures (imports), taken at the first depth pass.
	std::shared_ptr<org::Resource> mainDepth;
	std::shared_ptr<org::Resource> terrainDepth;
	// Whether the client is registered with the textures and imports as they are now.
	bool registered = false;

	/** @brief The three twins, in the order they are published. */
	std::array<std::pair<std::string_view, GraphResources::Texture2D*>, 3> Outputs() const
	{
		return { { { kBlendedDepthId, blendedDepth.get() }, { kBlendedDepth16Id, blendedDepth16.get() }, { kMaskId, mask.get() } } };
	}
};

namespace
{
	using Impl = ORGTerrainBlending::Impl;
	using RenderGraph::AnchorPasses::Bind;

	/** @brief The engine's main depth, as a graph texture. Called before the depth table points its SRV at the blended depth. */
	std::shared_ptr<org::Resource> MainDepth()
	{
		return FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
	}

	/** @brief Terrain Blending's terrain depth (its D3D11 texture, DSV and SRV), as a graph texture. */
	std::shared_ptr<org::Resource> TerrainDepth()
	{
		const auto& terrainBlending = globals::features::terrainBlending;
		if (!terrainBlending.terrainDepth.texture)
			return nullptr;
		return FrameAnchors::ImportDepth(terrainBlending.terrainDepth, kTerrainDepthId);
	}

	/** @brief The size the D3D11 path makes its blended depth (the main render target's). */
	bool MainSize(UINT& a_width, UINT& a_height)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer)
			return false;
		auto* main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture;
		if (!main)
			return false;
		D3D11_TEXTURE2D_DESC desc{};
		main->GetDesc(&desc);
		a_width = desc.Width;
		a_height = desc.Height;
		return true;
	}

	D3D11_TEXTURE2D_DESC OutputDesc(UINT a_width, UINT a_height, DXGI_FORMAT a_format)
	{
		return { .Width = a_width,
			.Height = a_height,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = a_format,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS };
	}

	void Withdraw()
	{
		for (const auto id : { ORGTerrainBlending::kBlendedDepthId, ORGTerrainBlending::kBlendedDepth16Id, ORGTerrainBlending::kMaskId })
			PublishedResources::Withdraw(id);
	}
}

void ORGTerrainBlending::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::DepthDone))
		logger::warn("[ORG] Terrain Blending stays on D3D11: the DepthDone anchor could not be reserved");
}

bool ORGTerrainBlending::Active() const
{
	return impl && !failed && RenderGraphRuntime::Get().IsActive();
}

void ORGTerrainBlending::ClearShaderCache()
{
	// A program that built reloads through the pipeline registry. Only a failed compile is tried again.
	if (failed && programFailed) {
		failed = false;
		programFailed = false;
		Setup(onFailed);
	}
}

ID3D11ShaderResourceView* ORGTerrainBlending::BlendedDepthSRV(bool a_16bit) const
{
	if (!Active())
		return nullptr;
	return a_16bit ? impl->blendedDepth16->SRV() : impl->blendedDepth->SRV();
}

ID3D11ShaderResourceView* ORGTerrainBlending::MaskSRV() const
{
	return Active() ? impl->mask->SRV() : nullptr;
}

std::shared_ptr<org::Resource> ORGTerrainBlending::BlendedDepthGraph() const
{
	return Active() ? impl->blendedDepth->Graph() : nullptr;
}

bool ORGTerrainBlending::RegisterClient()
{
	// One dispatch writes min(main depth, terrain depth) into both blended depths and the main depth into the mask, as the D3D11
	// path's dispatch and copy. The whole texture: DepthBlend skips the threads past its edge. A smaller dynamic-resolution
	// viewport only leaves pixels nothing reads.
	const auto& desc = impl->blendedDepth->Desc();
	RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::DepthDone, kClientId, 0);
	builder.Add(RenderGraph::AnchorPasses::Dispatch{ .name = "depth-blend",
		.program = impl->program,
		.bindings = { Bind::Uav(impl->blendedDepth->Graph()), Bind::Uav(impl->blendedDepth16->Graph()), Bind::Uav(impl->mask->Graph()), Bind::Srv(impl->mainDepth),
			Bind::Srv(impl->terrainDepth) },
		.groups = { (desc.Width + kGroupSize - 1) / kGroupSize, (desc.Height + kGroupSize - 1) / kGroupSize, 1 } });
	if (builder.Failed()) {
		logger::error("[ORG] Terrain Blending: {}", builder.Failure());
		return false;
	}

	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.passes = builder.Take();
	client.registerResources = [current = impl](org::RenderGraph& a_graph) {
		for (const auto& [id, texture] : current->Outputs())
			texture->Register(a_graph);
	};
	client.active = [] { return globals::features::terrainBlending.settings.Enabled != 0; };
	client.onFailed = [] { Get().Fail("the render graph failed"); };
	return FrameAnchors::Register(std::move(client));
}

void ORGTerrainBlending::Fail(const char* a_why)
{
	logger::error("[ORG] Terrain Blending returns to D3D11: {}", a_why);
	failed = true;
	FrameAnchors::Unregister(kClientId);
	Withdraw();
	// The exports stay alive until Terrain Blending's depth table no longer names them.
	auto old = std::move(impl);
	if (onFailed)
		onFailed();
	old.reset();
}

bool ORGTerrainBlending::Setup(std::function<void()> a_onFailed)
{
	if (!Requested() || failed || impl)
		return impl != nullptr;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::DepthDone)) {
		logger::warn("[ORG] Terrain Blending: CS_ORG_TERRAIN_BLENDING=1 but the render graph or its DepthDone anchor is not available; it stays on D3D11");
		return false;
	}
	onFailed = std::move(a_onFailed);
	UINT width = 0, height = 0;
	if (!MainSize(width, height)) {
		Fail("the main render target is missing");
		return false;
	}
	auto next = std::make_shared<Impl>();
	next->program = ComputeProgram::Load(host->Device(), { .source = kShader, .constantWords = ShaderBindings::Words<DepthBlendBlock>(), .defines = { { L"MASK_OUTPUT", L"" } } });
	if (!next->program) {
		Fail("DepthBlend.hlsl could not be compiled");
		programFailed = true;
		return false;
	}
	// Written in full by the blend every frame and read within it, but the engine's draws and the D3D11 features read them through
	// their exports: exported, so never aliased.
	const GraphResources::Options exported{ .lifetime = GraphResources::Lifetime::Transient, .d3d11 = GraphResources::Export::SRV };
	next->blendedDepth = std::make_unique<GraphResources::Texture2D>(OutputDesc(width, height, DXGI_FORMAT_R32_FLOAT), std::string(kBlendedDepthId), exported);
	next->blendedDepth16 = std::make_unique<GraphResources::Texture2D>(OutputDesc(width, height, DXGI_FORMAT_R16_UNORM), std::string(kBlendedDepth16Id), exported);
	next->mask = std::make_unique<GraphResources::Texture2D>(OutputDesc(width, height, DXGI_FORMAT_R32_FLOAT), std::string(kMaskId), exported);
	for (const auto& [id, texture] : next->Outputs()) {
		if (!*texture || !texture->SRV()) {
			Fail("its textures could not be created");
			return false;
		}
	}
	// Screen Space Shadows, Exponential Height Fog and Drawcall Limit Fix's colour passes read them by name, ordered after the blend.
	// The client registers at the first depth pass, which imports the depths.
	for (const auto& [id, texture] : next->Outputs()) {
		if (!PublishedResources::Publish(id, *texture)) {
			impl = std::move(next);
			Fail("its textures could not be published");
			return false;
		}
	}
	impl = std::move(next);
	logger::info("[ORG] Terrain Blending: the depth blend runs on the render graph ({}x{})", width, height);
	return true;
}

bool ORGTerrainBlending::BeforeDepth()
{
	if (!Active() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::DepthDone))
		return false;
	bool changed = !impl->registered;
	// A resolution change: new engine depth textures, and the blended depths follow the main target's size. The anchors and
	// PublishedResources follow the twins' resize; the pass is made again for the new textures and group counts.
	UINT width = 0, height = 0;
	if (MainSize(width, height) && (width != impl->blendedDepth->Desc().Width || height != impl->blendedDepth->Desc().Height)) {
		for (const auto& [id, texture] : impl->Outputs()) {
			if (!texture->Resize(width, height) || !*texture || !texture->SRV()) {
				Fail("its textures could not be resized");
				return false;
			}
		}
		logger::info("[ORG] Terrain Blending: the depth blend's textures are now {}x{}", width, height);
		changed = true;
	}
	auto mainDepth = MainDepth();
	auto terrainDepth = TerrainDepth();
	if (!mainDepth || !terrainDepth) {
		Fail("the main or the terrain depth could not be imported");
		return false;
	}
	if (mainDepth != impl->mainDepth || terrainDepth != impl->terrainDepth) {
		impl->mainDepth = std::move(mainDepth);
		impl->terrainDepth = std::move(terrainDepth);
		changed = true;
	}
	if (changed) {
		impl->registered = RegisterClient();
		if (!impl->registered) {
			Fail("its client could not be registered");
			return false;
		}
	}
	return true;
}

bool ORGTerrainBlending::Blend()
{
	if (!Active() || !impl->registered)
		return false;
	// A failed epoch calls the client's onFailed inside Run, which returns Terrain Blending to D3D11.
	return FrameAnchors::Run(FrameAnchors::Anchor::DepthDone) && Active();
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Terrain Blending always blends on D3D11.
struct ORGTerrainBlending::Impl
{};

void ORGTerrainBlending::Reserve() {}
bool ORGTerrainBlending::Setup(std::function<void()>) { return false; }
bool ORGTerrainBlending::BeforeDepth() { return false; }
bool ORGTerrainBlending::Blend() { return false; }
bool ORGTerrainBlending::Active() const { return false; }
void ORGTerrainBlending::ClearShaderCache() {}
ID3D11ShaderResourceView* ORGTerrainBlending::BlendedDepthSRV(bool) const { return nullptr; }
ID3D11ShaderResourceView* ORGTerrainBlending::MaskSRV() const { return nullptr; }
std::shared_ptr<org::Resource> ORGTerrainBlending::BlendedDepthGraph() const { return nullptr; }
void ORGTerrainBlending::Fail(const char*) {}
bool ORGTerrainBlending::RegisterClient() { return false; }

#endif
