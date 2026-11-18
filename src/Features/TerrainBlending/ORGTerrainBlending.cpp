#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/TerrainBlending/ORGTerrainBlending.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

bool ORGTerrainBlending::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
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
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "State.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>

#	include <array>
#	include <utility>

namespace
{
	constexpr const char* kClientId = "cs.terrain-blending";
	constexpr const char* kShader = "TerrainBlending/DepthBlend.hlsl";
	// The terrain depth's import, when Terrain Blending made a D3D11 one (the twin could not be made).
	constexpr const char* kTerrainDepthImportId = "cs.terrain-blending.terrain-depth";
	constexpr const char* kClearClientId = "cs.terrain-blending.terrain-depth-clear";
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
	FrameAnchors::Texture mainDepth;
	FrameAnchors::Texture terrainDepth;
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
	FrameAnchors::Texture MainDepth()
	{
		return FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
	}

	/** @brief The terrain depth twin (MakeTerrainDepth); null when Terrain Blending made a D3D11 one. */
	std::unique_ptr<GraphResources::Texture2D>& TerrainDepthTwin()
	{
		static std::unique_ptr<GraphResources::Texture2D> s_twin;
		return s_twin;
	}

	/** @brief The frame (State::frameCount) whose EarlyPrepass run had the terrain depth's clear pass; render thread. */
	std::uint64_t& ClearedFrame()
	{
		static std::uint64_t s_frame = UINT64_MAX;
		return s_frame;
	}

	/** @brief Terrain Blending's terrain depth: the twin, or its D3D11 texture imported. */
	FrameAnchors::Texture TerrainDepth()
	{
		if (auto owned = ORGTerrainBlending::Get().TerrainDepthGraph())
			return owned;
		const auto& terrainBlending = globals::features::terrainBlending;
		if (!terrainBlending.terrainDepth.texture)
			return nullptr;
		return FrameAnchors::ImportDepth(terrainBlending.terrainDepth, kTerrainDepthImportId);
	}

	struct DepthClearFrame
	{
		std::uint32_t width = 0, height = 0;
		rhi::Format format = rhi::Format::Unknown;
		// One entry, opened by the client's commit: the clear is recorded at execution and skips a run that left it closed (gated).
		std::shared_ptr<const RenderGraph::RunGate> gate;
	};

	struct DepthClearBindings
	{
		org::DeclaredViewToken depth;
	};

	/** @brief The terrain depth cleared to 1 (its stencil kept), as TerrainBlending::ResetDepth's D3D11 clear: an empty render pass. */
	class TerrainDepthClearPass final : public org::TypedRenderGraphPass<TerrainDepthClearPass, DepthClearFrame, DepthClearBindings>
	{
	public:
		TerrainDepthClearPass(std::shared_ptr<org::ExternalTextureResource> a_depth, std::uint32_t a_width, std::uint32_t a_height,
			std::shared_ptr<const RenderGraph::RunGate> a_gate) :
			depth(std::move(a_depth)), frame{ a_width, a_height, depth->GetFormat(), std::move(a_gate) } {}

		DepthClearBindings Declare(org::PassBuilder& a_builder) { return { a_builder.DepthReadWrite(depth).View() }; }

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(frame.width);
			a_out.push_back(frame.height);
			a_out.push_back(static_cast<std::uint64_t>(frame.format));
		}

		DepthClearFrame Prepare(const DepthClearBindings&, const org::PassPrepareContext&) const { return frame; }

		// Its commands depend on the run only through SkipsAtExecution: a kept late batch keeps its list while the answer holds.
		static constexpr bool kRecordKeyedBySkip = true;

		// Recorded at execution: a run whose commit left the gate closed records only the pass's barriers.
		static bool SkipsAtExecution(const DepthClearBindings&, const DepthClearFrame& a_frame, const org::PassRecordContext& a_recording)
		{
			return a_frame.gate && !a_frame.gate->IsOpen(a_recording.Run(), 0);
		}

		static void Record(const DepthClearBindings& a_bindings, const DepthClearFrame& a_frame, org::PassRecordContext& a_recording)
		{
			rhi::DepthAttachment attachment{};
			attachment.dsv = a_recording.View(a_bindings.depth);
			attachment.resource = a_recording.Resolve(a_bindings.depth).GetHandle();
			attachment.depthLoad = rhi::LoadOp::Clear;
			attachment.depthStore = rhi::StoreOp::Store;
			attachment.stencilLoad = rhi::LoadOp::Load;
			attachment.stencilStore = rhi::StoreOp::Store;
			attachment.clear.type = rhi::ClearValueType::DepthStencil;
			attachment.clear.format = a_frame.format;
			attachment.clear.depthStencil = { 1.0f, 0 };
			rhi::PassBeginInfo begin{};
			begin.depth = &attachment;
			begin.width = a_frame.width;
			begin.height = a_frame.height;
			begin.debugName = "TerrainBlending::ClearDepth";
			auto& commands = a_recording.Commands();
			commands.BeginPass(begin);
			commands.EndPass();
		}

	private:
		std::shared_ptr<org::ExternalTextureResource> depth;
		DepthClearFrame frame;
	};

	/**
	 * @brief The terrain depth's clear at the EarlyPrepass anchor (Main::RenderShadowMaps returned, right before Main::RenderDepth):
	 * a client of its own, active while Terrain Blending is. Its commit notes the frame when the run's graph holds the pass, which
	 * TerrainBlending::ResetDepth reads to skip its D3D11 clear.
	 */
	bool RegisterTerrainDepthClear(std::shared_ptr<org::ExternalTextureResource> a_depth, std::uint32_t a_width, std::uint32_t a_height)
	{
		std::shared_ptr<RenderGraph::RunGate> gate = RenderGraph::RunGate::Create(kClearClientId, 1);
		if (!gate)
			return false;
		FrameAnchors::ClientDesc client{};
		client.id = kClearClientId;
		client.passes.push_back({ .name = kClearClientId,
			.anchor = FrameAnchors::Anchor::EarlyPrepass,
			.order = -100,
			.kind = FrameAnchors::PassKind::Render,
			.make = [a_depth, a_width, a_height, gate]() -> std::shared_ptr<org::RenderPass> {
				return std::make_shared<TerrainDepthClearPass>(a_depth, a_width, a_height, gate);
			},
			.atExecution = true });
		client.gates = { gate };
		client.active = [] { return globals::features::terrainBlending.settings.Enabled != 0; };
		// The declared frame: the clear is recorded ahead, so the setting turns it off in the description.
		client.declared = client.active;
		client.commit = [gate](const FrameAnchors::Point&) {
			gate->OpenDispatch(0, 1);
			if (FrameAnchors::InBuiltGraph(kClearClientId, FrameAnchors::Anchor::EarlyPrepass))
				ClearedFrame() = globals::state->frameCount;
		};
		client.onFailed = [] {
			logger::error("[ORG] Terrain Blending: the render graph failed; the terrain depth is cleared through its D3D11 export");
			ClearedFrame() = UINT64_MAX;
		};
		return FrameAnchors::Register(std::move(client));
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
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::EarlyPrepass))
		logger::warn("[ORG] Terrain Blending: the EarlyPrepass anchor could not be reserved; the terrain depth is cleared through D3D11");
}

bool ORGTerrainBlending::MakeTerrainDepth(RE::BSGraphics::DepthStencilData& a_target, const D3D11_TEXTURE2D_DESC& a_desc, DXGI_FORMAT a_srvFormat,
	DXGI_FORMAT a_dsvFormat)
{
	auto& twin = TerrainDepthTwin();
	if (twin || !Requested() || !RenderGraphRuntime::Get().IsActive())
		return false;
	// Drawn by D3D11 (the engine's terrain draws through its DSV) and read by D3D11 (the D3D11 blend, its mask fallback):
	// exported, never aliased.
	D3D11_TEXTURE2D_DESC desc = a_desc;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = 0;
	auto texture = std::make_unique<GraphResources::Texture2D>(desc, std::string(kTerrainDepthId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::DSV });
	auto graph = *texture ? std::dynamic_pointer_cast<org::ExternalTextureResource>(texture->Graph()) : nullptr;
	if (!graph || !texture->D3D11() || !texture->SRV() || !texture->DSV()) {
		logger::warn("[ORG] Terrain Blending: the terrain depth could not be made on the render graph (format {}); it is a D3D11 texture",
			static_cast<int>(a_desc.Format));
		return false;
	}
	// The engine's draws and the D3D11 blend use the views in the main depth's formats.
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	texture->SRV()->GetDesc(&srvDesc);
	texture->DSV()->GetDesc(&dsvDesc);
	if (srvDesc.Format != a_srvFormat || dsvDesc.Format != a_dsvFormat) {
		logger::warn("[ORG] Terrain Blending: the graph's terrain depth has views in formats {}/{}, the main depth's are {}/{}; it is a D3D11 texture",
			static_cast<int>(srvDesc.Format), static_cast<int>(dsvDesc.Format), static_cast<int>(a_srvFormat), static_cast<int>(a_dsvFormat));
		return false;
	}
	// The blend at DepthDone and Drawcall Limit Fix's Z-prepass declare it in their graphs: one resource in every graph.
	if (!PublishedResources::Publish(*texture)) {
		logger::warn("[ORG] Terrain Blending: the terrain depth could not be published; it is a D3D11 texture");
		return false;
	}
	if (!FrameAnchors::HasEpoch(FrameAnchors::Anchor::EarlyPrepass) || !RegisterTerrainDepthClear(graph, desc.Width, desc.Height))
		logger::warn("[ORG] Terrain Blending: the terrain depth's clear could not be put on the EarlyPrepass anchor; it is cleared through D3D11");
	// One reference each, as the D3D11 path's Create calls leave in the fields.
	a_target.texture = texture->D3D11();
	a_target.texture->AddRef();
	a_target.depthSRV = texture->SRV();
	a_target.depthSRV->AddRef();
	a_target.views[0] = texture->DSV();
	a_target.views[0]->AddRef();
	twin = std::move(texture);
	logger::info("[ORG] Terrain Blending: the terrain depth is a graph texture ({}x{}), published as {}", desc.Width, desc.Height, kTerrainDepthId);
	return true;
}

std::shared_ptr<org::ExternalTextureResource> ORGTerrainBlending::TerrainDepthGraph() const
{
	const auto& twin = TerrainDepthTwin();
	return twin && RenderGraphRuntime::Get().IsActive() ? std::dynamic_pointer_cast<org::ExternalTextureResource>(twin->Graph()) : nullptr;
}

bool ORGTerrainBlending::TerrainDepthCleared() const
{
	return TerrainDepthTwin() && ClearedFrame() == globals::state->frameCount;
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

bool ORGTerrainBlending::Holds() const
{
	return Active() && (frame == FrameBlend::Planned || frame == FrameBlend::Written);
}

ID3D11ShaderResourceView* ORGTerrainBlending::BlendedDepthSRV(bool a_16bit) const
{
	if (!Holds())
		return nullptr;
	return a_16bit ? impl->blendedDepth16->SRV() : impl->blendedDepth->SRV();
}

ID3D11ShaderResourceView* ORGTerrainBlending::MaskSRV() const
{
	return Holds() ? impl->mask->SRV() : nullptr;
}

bool ORGTerrainBlending::RegisterClient()
{
	// One dispatch writes min(main depth, terrain depth) into both blended depths and the main depth into the mask, as the D3D11
	// path's dispatch and copy. The whole texture: DepthBlend skips the threads past its edge. A smaller dynamic-resolution
	// viewport only leaves pixels nothing reads.
	const auto& desc = impl->blendedDepth->Desc();
	// The dispatch's counts come from a run gate entry the commit opens: gated, so the anchor can run in a frame phase.
	std::shared_ptr<RenderGraph::RunGate> gate = RenderGraph::RunGate::Create(kClientId, 1);
	if (!gate) {
		logger::error("[ORG] Terrain Blending: its run gate could not be created");
		return false;
	}
	const std::uint32_t groupsX = (desc.Width + kGroupSize - 1) / kGroupSize;
	const std::uint32_t groupsY = (desc.Height + kGroupSize - 1) / kGroupSize;
	RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::DepthDone, kClientId, 0, {}, gate);
	builder.Add(RenderGraph::AnchorPasses::Dispatch{ .name = "depth-blend",
		.program = impl->program,
		.bindings = { Bind::Uav(*impl->blendedDepth), Bind::Uav(*impl->blendedDepth16), Bind::Uav(*impl->mask), Bind::Srv(impl->mainDepth),
			Bind::Srv(impl->terrainDepth) },
		.gate = 0u });
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
	client.gates = { gate };
	client.commit = [gate, groupsX, groupsY](const FrameAnchors::Point&) { gate->OpenDispatch(0, groupsX, groupsY, 1); };
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
	// their exports: exported, so never aliased. Versioned: a resolution change resizes them as new versions, which every reader of
	// their published ids follows without a graph build; the blend declares their slots (Bind::Uav of the twin).
	const GraphResources::Options exported{ .lifetime = GraphResources::Lifetime::Transient, .d3d11 = GraphResources::Export::SRV, .versioned = true };
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
	frame = FrameBlend::None;
	if (!Active() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::DepthDone))
		return false;
	bool changed = !impl->registered;
	// A resolution change: new engine depth textures, and the blended depths follow the main target's size. Each twin's resize is a
	// new version of its slot, which the readers of the published ids follow with no build. The blend is made again for its group
	// counts and the new engine depths (its own anchor only).
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
	frame = FrameBlend::Planned;
	return true;
}

bool ORGTerrainBlending::Blend()
{
	if (frame != FrameBlend::Planned)
		return false;
	// A failed epoch calls the client's onFailed inside Run, which returns Terrain Blending to D3D11. An anchor that submitted
	// nothing leaves the frame to D3D11 too: the views then name the D3D11 textures Terrain Blending blends into.
	const bool written = Active() && impl->registered && FrameAnchors::Run(FrameAnchors::Anchor::DepthDone) && Active();
	frame = written ? FrameBlend::Written : FrameBlend::Missed;
	return written;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Terrain Blending always blends on D3D11.
struct ORGTerrainBlending::Impl
{};

void ORGTerrainBlending::Reserve() {}
bool ORGTerrainBlending::Setup(std::function<void()>) { return false; }
bool ORGTerrainBlending::MakeTerrainDepth(RE::BSGraphics::DepthStencilData&, const D3D11_TEXTURE2D_DESC&, DXGI_FORMAT, DXGI_FORMAT) { return false; }
std::shared_ptr<org::ExternalTextureResource> ORGTerrainBlending::TerrainDepthGraph() const { return nullptr; }
bool ORGTerrainBlending::TerrainDepthCleared() const { return false; }
bool ORGTerrainBlending::BeforeDepth() { return false; }
bool ORGTerrainBlending::Blend() { return false; }
bool ORGTerrainBlending::Active() const { return false; }
bool ORGTerrainBlending::Holds() const { return false; }
void ORGTerrainBlending::ClearShaderCache() {}
ID3D11ShaderResourceView* ORGTerrainBlending::BlendedDepthSRV(bool) const { return nullptr; }
ID3D11ShaderResourceView* ORGTerrainBlending::MaskSRV() const { return nullptr; }
void ORGTerrainBlending::Fail(const char*) {}
bool ORGTerrainBlending::RegisterClient() { return false; }

#endif
