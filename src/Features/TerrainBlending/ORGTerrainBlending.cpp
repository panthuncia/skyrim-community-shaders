// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/TerrainBlending/ORGTerrainBlending.h"

ORGTerrainBlending::ORGTerrainBlending() = default;
ORGTerrainBlending::~ORGTerrainBlending() = default;

#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/Frame/Revision.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/ShaderBindings.h"
#include "State.h"
#include "Utils/Logging.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>

#include <array>
#include <utility>

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
	RenderGraph::Programs::Fixed<ComputeProgram> program;
	// Declared at Setup on the display size (decision 6a). A new size is a new version, adopted with a frame revision.
	RenderGraph::Frame::DeclaredTexture2D blendedDepth;
	RenderGraph::Frame::DeclaredTexture2D blendedDepth16;
	RenderGraph::Frame::DeclaredTexture2D mask;
	// Registered whatever Enabled is. Its key imports the depth textures the last depth pass, or setup's build point, found.
	RenderGraph::Frame::DeclaredClient client;
	// The terrain depth texture Observe saw last, the graph's or Terrain Blending's D3D11 one (B2's interim). Zero before the first
	// observation. The main depth is an engine target, whose slot never changes.
	RenderGraph::Frame::Observed<std::uintptr_t> observedTerrainDepth{ "TerrainBlending.TerrainDepth" };
	// Why a frame blended nothing, each reason logged once.
	Util::ReasonLog lagReasons{ "[ORG] Terrain Blending" };

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
		return FrameAnchors::EngineTexture(FrameAnchors::Depth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
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

	/** @brief The terrain depth's texture, the graph's or Terrain Blending's D3D11 one. Null when there is none. */
	const void* TerrainDepthTexture()
	{
		if (auto owned = ORGTerrainBlending::Get().TerrainDepthGraph())
			return owned.get();
		return globals::features::terrainBlending.terrainDepth.texture;
	}

	FrameAnchors::Texture TerrainDepth()
	{
		if (auto owned = ORGTerrainBlending::Get().TerrainDepthGraph())
			return owned;
		const auto& terrainBlending = globals::features::terrainBlending;
		if (!terrainBlending.terrainDepth.texture)
			return nullptr;
		return FrameAnchors::ImportTexture(kTerrainDepthImportId, { .depth = &terrainBlending.terrainDepth });
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
			if (FrameAnchors::Place(kClearClientId, FrameAnchors::Anchor::EarlyPrepass).built)
				ClearedFrame() = globals::state->frameCount;
		};
		return true;
	}

	/**
	* @brief Right before Main::RenderDepth. The clear is registered for the session, whatever Enabled is: a switch of Enabled then
	* never leaves a frame without it. A frame with Terrain Blending off never draws terrain depth, so its clear is only wasted.
	*/
	bool DeclareTerrainDepthClear(std::shared_ptr<org::ExternalTextureResource> a_depth, std::uint32_t a_width, std::uint32_t a_height)
	{
		using namespace RenderGraph::Frame;
		TerrainDepthClear() = DeclareClient<bool>(
			kClearClientId, DependsOn(globals::features::terrainBlending.settings.Enabled),
			[](const Inputs&) -> std::optional<bool> { return true; },
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
			.program = a_impl->program.Get(),
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
}

bool ORGTerrainBlending::MakeTerrainDepth(RE::BSGraphics::DepthStencilData& a_target, const D3D11_TEXTURE2D_DESC& a_desc, DXGI_FORMAT a_srvFormat,
	DXGI_FORMAT a_dsvFormat)
{
	auto& twin = TerrainDepthTwin();
	if (twin || !On())
		return false;
	if (!RenderGraphRuntime::Get().IsActive())
		Fatal("the render graph is not available for the terrain depth");
	// Drawn and read through D3D11, so exported and never aliased.
	D3D11_TEXTURE2D_DESC desc = a_desc;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = 0;
	auto texture = std::make_unique<GraphResources::Texture2D>(desc, std::string(kTerrainDepthId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::DSV });
	auto graph = *texture ? std::dynamic_pointer_cast<org::ExternalTextureResource>(texture->Graph()) : nullptr;
	if (!graph || !texture->D3D11() || !texture->SRV() || !texture->DSV())
		Fatal(fmt::format("the terrain depth could not be made on the render graph (format {})", static_cast<int>(a_desc.Format)));
	// The engine's draws and the D3D11 blend use views in the main depth's formats.
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	texture->SRV()->GetDesc(&srvDesc);
	texture->DSV()->GetDesc(&dsvDesc);
	if (srvDesc.Format != a_srvFormat || dsvDesc.Format != a_dsvFormat)
		Fatal(fmt::format("the graph's terrain depth has views in formats {}/{}, the main depth's are {}/{}", static_cast<int>(srvDesc.Format),
			static_cast<int>(dsvDesc.Format), static_cast<int>(a_srvFormat), static_cast<int>(a_dsvFormat)));
	// Published so every graph that declares it gets one resource.
	if (!PublishedResources::Publish(*texture))
		Fatal("the terrain depth could not be published");
	if (!DeclareTerrainDepthClear(graph, desc.Width, desc.Height))
		Fatal("the terrain depth's clear could not be put on the EarlyPrepass anchor");
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
	return twin ? std::dynamic_pointer_cast<org::ExternalTextureResource>(twin->Graph()) : nullptr;
}

bool ORGTerrainBlending::TerrainDepthCleared() const
{
	// Graph mode: the EarlyPrepass client clears it, or the frame lags and keeps the last depth. D3D11 never clears it.
	return IsActive() && TerrainDepthTwin();
}

bool ORGTerrainBlending::Holds() const
{
	return IsActive() && frame != FrameBlend::None;
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

bool ORGTerrainBlending::Setup()
{
	if (!BeginSetup())
		return IsActive();
	auto next = std::make_shared<Impl>();
	next->program = RenderGraph::Programs::Queue("TerrainBlending",
		ComputeProgram::Desc{ .source = kShader, .constantWords = ShaderBindings::Words<DepthBlendBlock>(), .defines = { { L"MASK_OUTPUT", L"" } } });
	using namespace RenderGraph::Frame;
	// The engine and D3D11 features read them through exports, so never aliased. Declared on the display size (decision 6a):
	// a new size is a new version that readers follow without a graph build. The engine target set writes it before this.
	const GraphResources::Options exported{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV };
	const auto declare = [&](std::string_view a_id, DXGI_FORMAT a_format) {
		return DeclareTexture2D(std::string(a_id), DependsOn(Vars::DisplaySize()),
			[a_format](const Inputs& a_in) {
				const auto display = a_in.Get(Vars::DisplaySize());
				return OutputDesc(display.width, display.height, a_format);
			},
			exported);
	};
	next->blendedDepth = declare(kBlendedDepthId, DXGI_FORMAT_R32_FLOAT);
	next->blendedDepth16 = declare(kBlendedDepth16Id, DXGI_FORMAT_R16_UNORM);
	next->mask = declare(kMaskId, DXGI_FORMAT_R32_FLOAT);
	for (const auto& [id, texture] : next->Outputs()) {
		if (!texture || !*texture || !texture->SRV())
			Fatal("its textures could not be created");
	}
	// Published once, for the session. Readers are ordered after the blend and follow the versions.
	for (const auto& [id, texture] : next->Outputs()) {
		if (!PublishedResources::Publish(id, *texture))
			Fatal("its textures could not be published");
	}
	// The client follows the observed inputs, whatever Enabled is, so a switch of Enabled never leaves a frame without it. Only a
	// frame with the adopted Enabled on runs the anchor. Setup's build point observes the inputs, so the setup build holds it.
	const std::weak_ptr<Impl> weak = next;
	next->client = DeclareClient<BlendInputs>(
		kClientId, DependsOn(globals::features::terrainBlending.settings.Enabled, next->observedTerrainDepth.Var()),
		[weak](const Inputs& a_in) -> std::optional<BlendInputs> {
			const auto current = weak.lock();
			if (!current)
				return std::nullopt;
			const auto* terrainDepth = current->observedTerrainDepth.Get(a_in);
			if (!terrainDepth || !*terrainDepth)
				return std::nullopt;
			// Imported here, when the revision starts, so a new texture joins the revision.
			BlendInputs inputs{ MainDepth(), TerrainDepth() };
			if (!inputs.mainDepth || !inputs.terrainDepth)
				Fatal("the main or terrain depth cannot be imported into the render graph");
			return inputs;
		},
		[weak](const BlendInputs& a_inputs, FrameAnchors::ClientDesc& a_client) {
			const auto current = weak.lock();
			return current && MakeBlendClient(current, a_inputs, a_client);
		});
	// Before the setup build: the engine's depth and the terrain depth exist by then.
	RenderGraph::Frame::Revision::OnSetupBuildPoint([] { Get().ObserveAtSetup(); });
	impl = std::move(next);
	logger::info("[ORG] Terrain Blending: the depth blend runs on the render graph; its textures and client follow the frame revisions");
	return true;
}

void ORGTerrainBlending::ObserveAtSetup()
{
	if (!IsActive())
		return;
	const void* mainDepth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture;
	if (!mainDepth || !TerrainDepthTexture()) {
		logger::warn("[ORG] Terrain Blending: the main or the terrain depth is missing at setup; the first depth pass observes them");
		return;
	}
	Observe();
}

void ORGTerrainBlending::Observe()
{
	if (!IsActive())
		return;
	// No import here: the client's key imports them when the next revision starts.
	const void* mainDepth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture;
	const void* terrainDepth = TerrainDepthTexture();
	if (!mainDepth || !terrainDepth)
		Fatal("the main or the terrain depth is missing");
	// Not a variable yet: a new terrain depth is an event, and the next revision makes the client for it.
	impl->observedTerrainDepth.Observe(reinterpret_cast<std::uintptr_t>(terrainDepth));
}

bool ORGTerrainBlending::BeforeDepth()
{
	frame = FrameBlend::None;
	if (!IsActive())
		return false;
	// Graph mode: the depth table names the graph's blended depth every frame. D3D11 never blends.
	frame = FrameBlend::Planned;
	const auto place = FrameAnchors::Place(kClientId, FrameAnchors::Anchor::DepthDone);
	if (!place.draws) {
		// The client is registered from setup's build, so only a stage that runs an older schedule lags. One that never will is fatal.
		if (!place.MayLag())
			Fatal(fmt::format("the depth stage will never hold the blend ({})", FrameAnchors::ReasonName(place.reason)));
		impl->lagReasons.Report(fmt::format("this frame blends nothing, and the blended depth keeps the last frame's ({})", FrameAnchors::ReasonName(place.reason)));
	}
	return true;
}

bool ORGTerrainBlending::Blend()
{
	if (frame != FrameBlend::Planned)
		return false;
	const auto ran = FrameAnchors::Run(FrameAnchors::Anchor::DepthDone);
	// Idle: the schedule lags (BeforeDepth logged it), so the blended depth keeps the last frame's. A failed run is fatal.
	if (ran.status == FrameAnchors::RunStatus::Failed || ran.status == FrameAnchors::RunStatus::Refused)
		Fatal(fmt::format("the DepthDone anchor did not run (status {})", static_cast<int>(ran.status)));
	frame = ran ? FrameBlend::Written : FrameBlend::Missed;
	return true;
}

