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
#	include "RenderGraph/Frame/Declare.h"
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
	// Used when the twin could not be made.
	constexpr const char* kTerrainDepthImportId = "cs.terrain-blending.terrain-depth";
	constexpr const char* kClearClientId = "cs.terrain-blending.terrain-depth-clear";
	// DepthBlend.hlsl's numthreads.
	constexpr std::uint32_t kGroupSize = 8;

	// With MASK_OUTPUT, in list order.
	struct DepthBlendBlock
	{
		ShaderBindings::Handle blendedDepth, blendedDepth16, mask, mainDepth, terrainDepth;
	};
}

struct ORGTerrainBlending::Impl
{
	std::shared_ptr<const ComputeProgram> program;
	// Declared at Setup on the display size (decision 6a). A new size is a new version, adopted with a frame revision.
	RenderGraph::Frame::DeclaredTexture2D blendedDepth;
	RenderGraph::Frame::DeclaredTexture2D blendedDepth16;
	RenderGraph::Frame::DeclaredTexture2D mask;
	// Declared on Enabled. Its key adds the inputs the last depth pass imported.
	RenderGraph::Frame::DeclaredClient client;
	FrameAnchors::Texture mainDepth;
	FrameAnchors::Texture terrainDepth;

	/** @brief In publish order. */
	std::array<std::pair<std::string_view, GraphResources::Texture2D*>, 3> Outputs() const
	{
		return { { { kBlendedDepthId, blendedDepth.Get() }, { kBlendedDepth16Id, blendedDepth16.Get() }, { kMaskId, mask.Get() } } };
	}
};

namespace
{
	using Impl = ORGTerrainBlending::Impl;
	using RenderGraph::AnchorPasses::Bind;

	/** @brief Call before the depth table points its SRV at the blended depth. */
	FrameAnchors::Texture MainDepth()
	{
		return FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
	}

	/** @brief Null when Terrain Blending made a D3D11 one. */
	std::unique_ptr<GraphResources::Texture2D>& TerrainDepthTwin()
	{
		static std::unique_ptr<GraphResources::Texture2D> s_twin;
		return s_twin;
	}

	/** @brief Render thread. */
	std::uint64_t& ClearedFrame()
	{
		static std::uint64_t s_frame = UINT64_MAX;
		return s_frame;
	}

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
		// Recorded at execution, so a run that left it closed skips.
		std::shared_ptr<const RenderGraph::RunGate> gate;
	};

	struct DepthClearBindings
	{
		org::DeclaredViewToken depth;
	};

	/** @brief Clears to 1 and keeps stencil, as TerrainBlending::ResetDepth does. */
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

		// A kept late batch keeps its list while SkipsAtExecution's answer holds.
		static constexpr bool kRecordKeyedBySkip = true;

		// A run that left the gate closed records only barriers.
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

	/** @brief The terrain depth's clear. Kept for the session, as the twin is. */
	RenderGraph::Frame::DeclaredClient& TerrainDepthClear()
	{
		static RenderGraph::Frame::DeclaredClient s_client;
		return s_client;
	}

	/** @brief The clear's registration while Terrain Blending is on. The commit notes the frame so ResetDepth can skip its D3D11 clear. */
	bool MakeTerrainDepthClear(FrameAnchors::ClientDesc& a_client, const std::shared_ptr<org::ExternalTextureResource>& a_depth, std::uint32_t a_width,
		std::uint32_t a_height)
	{
		std::shared_ptr<RenderGraph::RunGate> gate = RenderGraph::RunGate::Create(kClearClientId, 1);
		if (!gate)
			return false;
		a_client.id = kClearClientId;
		a_client.passes.push_back({ .name = kClearClientId,
			.anchor = FrameAnchors::Anchor::EarlyPrepass,
			.order = -100,
			.kind = FrameAnchors::PassKind::Render,
			.make = [a_depth, a_width, a_height, gate]() -> std::shared_ptr<org::RenderPass> {
				return std::make_shared<TerrainDepthClearPass>(a_depth, a_width, a_height, gate);
			},
			.atExecution = true });
		a_client.gates = { gate };
		a_client.commit = [gate](const FrameAnchors::Point&) {
			gate->OpenDispatch(0, 1);
			if (FrameAnchors::InBuiltGraph(kClearClientId, FrameAnchors::Anchor::EarlyPrepass))
				ClearedFrame() = globals::state->frameCount;
		};
		return true;
	}

	/**
	* @brief Right before Main::RenderDepth. The clear exists while the adopted Enabled is on, so it switches in the revision
	* that switches the blend.
	*/
	bool DeclareTerrainDepthClear(std::shared_ptr<org::ExternalTextureResource> a_depth, std::uint32_t a_width, std::uint32_t a_height)
	{
		using namespace RenderGraph::Frame;
		TerrainDepthClear() = DeclareClient<bool>(
			kClearClientId, DependsOn(globals::features::terrainBlending.settings.Enabled),
			[](const Inputs& a_in) -> std::optional<bool> {
				if (!a_in.Get(globals::features::terrainBlending.settings.Enabled))
					return std::nullopt;
				return true;
			},
			[a_depth = std::move(a_depth), a_width, a_height](const bool&, FrameAnchors::ClientDesc& a_client) {
				return MakeTerrainDepthClear(a_client, a_depth, a_width, a_height);
			});
		return static_cast<bool>(TerrainDepthClear());
	}

	/** @brief What the blend's client is made for: its depth inputs, imported at the depth pass. */
	struct BlendInputs
	{
		FrameAnchors::Texture mainDepth;
		FrameAnchors::Texture terrainDepth;

		bool operator==(const BlendInputs& a_other) const { return mainDepth == a_other.mainDepth && terrainDepth == a_other.terrainDepth; }
	};

	/** @brief The blend's registration for a_inputs, when a revision starts. */
	bool MakeBlendClient(const std::shared_ptr<Impl>& a_impl, const BlendInputs& a_inputs, FrameAnchors::ClientDesc& a_client)
	{
		// Gated so the anchor can run in a frame phase.
		std::shared_ptr<RenderGraph::RunGate> gate = RenderGraph::RunGate::Create(kClientId, 1);
		if (!gate) {
			logger::error("[ORG] Terrain Blending: its run gate could not be created");
			return false;
		}
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::DepthDone, kClientId, 0, {}, gate);
		builder.Add(RenderGraph::AnchorPasses::Dispatch{ .name = "depth-blend",
			.program = a_impl->program,
			.bindings = { Bind::Uav(*a_impl->blendedDepth), Bind::Uav(*a_impl->blendedDepth16), Bind::Uav(*a_impl->mask), Bind::Srv(a_inputs.mainDepth),
				Bind::Srv(a_inputs.terrainDepth) },
			.gate = 0u });
		if (builder.Failed()) {
			logger::error("[ORG] Terrain Blending: {}", builder.Failure());
			return false;
		}
		a_client.id = kClientId;
		a_client.passes = builder.Take();
		a_client.registerResources = [current = a_impl](org::RenderGraph& a_graph) {
			for (const auto& [id, texture] : current->Outputs())
				texture->Register(a_graph);
		};
		a_client.gates = { gate };
		// One dispatch over the bound version. DepthBlend skips threads past the edge, and pixels outside a dynamic-resolution
		// viewport are never read. The counts are per run, so a new size needs no new registration.
		a_client.commit = [gate, current = a_impl](const FrameAnchors::Point&) {
			const auto extent = RenderGraph::Frame::Revision::Adopted().Extent(current->blendedDepth);
			gate->OpenDispatch(0, (extent[0] + kGroupSize - 1) / kGroupSize, (extent[1] + kGroupSize - 1) / kGroupSize, 1);
		};
		logger::info("[ORG] Terrain Blending: the depth blend's client for the next revision");
		return true;
	}

	/** @brief The main render target's size, as the D3D11 path uses. */
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
	// Drawn and read through D3D11, so exported and never aliased.
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
	// The engine's draws and the D3D11 blend use views in the main depth's formats.
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	texture->SRV()->GetDesc(&srvDesc);
	texture->DSV()->GetDesc(&dsvDesc);
	if (srvDesc.Format != a_srvFormat || dsvDesc.Format != a_dsvFormat) {
		logger::warn("[ORG] Terrain Blending: the graph's terrain depth has views in formats {}/{}, the main depth's are {}/{}; it is a D3D11 texture",
			static_cast<int>(srvDesc.Format), static_cast<int>(dsvDesc.Format), static_cast<int>(a_srvFormat), static_cast<int>(a_dsvFormat));
		return false;
	}
	// Published so every graph that declares it gets one resource.
	if (!PublishedResources::Publish(*texture)) {
		logger::warn("[ORG] Terrain Blending: the terrain depth could not be published; it is a D3D11 texture");
		return false;
	}
	if (!FrameAnchors::HasEpoch(FrameAnchors::Anchor::EarlyPrepass) || !DeclareTerrainDepthClear(graph, desc.Width, desc.Height))
		logger::warn("[ORG] Terrain Blending: the terrain depth's clear could not be put on the EarlyPrepass anchor; it is cleared through D3D11");
	// One reference each, as D3D11's Create calls leave.
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
	// A built program reloads through the pipeline registry. Only a failed compile is retried.
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

void ORGTerrainBlending::Fail(const char* a_why)
{
	logger::error("[ORG] Terrain Blending returns to D3D11: {}", a_why);
	failed = true;
	FrameAnchors::Unregister(kClientId);
	Withdraw();
	// The exports stay alive until the depth table no longer names them. Dropping the declarations stops them following.
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
	using namespace RenderGraph::Frame;
	// The engine and D3D11 features read them through exports, so never aliased. Declared on the display size (decision 6a):
	// a new size is a new version that readers follow without a graph build. Before the display size is known, the main target's.
	const GraphResources::Options exported{ .lifetime = GraphResources::Lifetime::Transient, .d3d11 = GraphResources::Export::SRV };
	const auto declare = [&](std::string_view a_id, DXGI_FORMAT a_format) {
		return DeclareTexture2D(std::string(a_id), DependsOn(Vars::DisplaySize()),
			[width, height, a_format](const Inputs& a_in) {
				const auto display = a_in.Get(Vars::DisplaySize());
				return OutputDesc(display.width ? display.width : width, display.height ? display.height : height, a_format);
			},
			exported);
	};
	next->blendedDepth = declare(kBlendedDepthId, DXGI_FORMAT_R32_FLOAT);
	next->blendedDepth16 = declare(kBlendedDepth16Id, DXGI_FORMAT_R16_UNORM);
	next->mask = declare(kMaskId, DXGI_FORMAT_R32_FLOAT);
	for (const auto& [id, texture] : next->Outputs()) {
		if (!texture || !*texture || !texture->SRV()) {
			Fail("its textures could not be created");
			return false;
		}
	}
	// Published once, for the session. Readers are ordered after the blend and follow the versions.
	for (const auto& [id, texture] : next->Outputs()) {
		if (!PublishedResources::Publish(id, *texture)) {
			impl = std::move(next);
			Fail("its textures could not be published");
			return false;
		}
	}
	// The client follows Enabled from the first revision. Its key needs the inputs a depth pass imported (Observe).
	const std::weak_ptr<Impl> weak = next;
	next->client = DeclareClient<BlendInputs>(
		kClientId, DependsOn(globals::features::terrainBlending.settings.Enabled),
		[weak](const Inputs& a_in) -> std::optional<BlendInputs> {
			const auto current = weak.lock();
			if (!current || !a_in.Get(globals::features::terrainBlending.settings.Enabled) || !current->mainDepth || !current->terrainDepth)
				return std::nullopt;
			return BlendInputs{ current->mainDepth, current->terrainDepth };
		},
		[weak](const BlendInputs& a_inputs, FrameAnchors::ClientDesc& a_client) {
			const auto current = weak.lock();
			return current && MakeBlendClient(current, a_inputs, a_client);
		});
	if (!next->client) {
		impl = std::move(next);
		Fail("its client could not be declared");
		return false;
	}
	impl = std::move(next);
	logger::info("[ORG] Terrain Blending: the depth blend runs on the render graph ({}x{}); its textures and client follow the frame revisions", width, height);
	return true;
}

void ORGTerrainBlending::Observe()
{
	if (!Active())
		return;
	auto mainDepth = MainDepth();
	auto terrainDepth = TerrainDepth();
	if (!mainDepth || !terrainDepth) {
		Fail("the main or the terrain depth could not be imported");
		return;
	}
	if (mainDepth == impl->mainDepth && terrainDepth == impl->terrainDepth)
		return;
	impl->mainDepth = std::move(mainDepth);
	impl->terrainDepth = std::move(terrainDepth);
	// Not variables yet: the next revision makes the client for them.
	impl->client.Invalidate();
}

bool ORGTerrainBlending::BeforeDepth()
{
	frame = FrameBlend::None;
	// The client and the adopted Enabled switch in one revision. Until the first one registered it, D3D11 blends.
	if (!Active() || !impl->client.Registered())
		return false;
	frame = FrameBlend::Planned;
	return true;
}

bool ORGTerrainBlending::Blend()
{
	if (frame != FrameBlend::Planned)
		return false;
	// An anchor that submitted nothing leaves the frame to D3D11.
	const bool written = Active() && FrameAnchors::Run(FrameAnchors::Anchor::DepthDone);
	frame = written ? FrameBlend::Written : FrameBlend::Missed;
	return written;
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph Terrain Blending always blends on D3D11.
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
void ORGTerrainBlending::Observe() {}

#endif
