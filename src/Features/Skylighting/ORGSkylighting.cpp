#include "ORGSkylighting.h"

#include "RenderGraph/Switches.h"

bool ORGSkylighting::Requested()
{
	static const bool requested = Switches::On("CS_ORG_SKYLIGHTING");
	return requested;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Deferred.h"
#	include "Features/Skylighting.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>

#	include <array>
#	include <cmath>
#	include <cstring>
#	include <numbers>
#	include <optional>
#	include <type_traits>

namespace
{
	constexpr const char* kClientId = "cs.skylighting";
	constexpr const char* kShader = "Skylighting/UpdateProbesCS.hlsl";
	constexpr auto kVolumes = static_cast<std::size_t>(ORGSkylighting::Volume::Count);

	/** @brief The graph no longer updates the probes: the published volumes' readers return to D3D11. */
	void WithdrawVolumes()
	{
		PublishedResources::Withdraw(ORGSkylighting::kProbeArrayId);
		PublishedResources::Withdraw(ORGSkylighting::kShadowVisibilityId);
	}

	/** @brief The directional shadow data, as UpdateProbesShadows in UpdateProbesCS.hlsl: float4 rows, tightly packed. */
	struct Shadows
	{
		float shadowProj[32];     // DirectionalShadowLightData::ShadowProj, as stored (column_major)
		float splitDistances[4];  // end xy, start zw
	};
	static_assert(sizeof(Shadows) == 9 * 16);

	/** @brief The textures the pass reads, as D3D11 gives them. */
	struct Inputs
	{
		ID3D11Texture2D* occlusion = nullptr;
		ID3D11ShaderResourceView* occlusionSRV = nullptr;
		ID3D11DepthStencilView* occlusionDSV = nullptr;
		ID3D11ShaderResourceView* shadowCascades = nullptr;  // PS t4 at the shadow mask draw
		ID3D11ShaderResourceView* esramShadows = nullptr;
	};

	template <class T>
	void CopyBytes(T& a_out, const void* a_in)
	{
		static_assert(std::is_trivially_copyable_v<T>);
		std::memcpy(&a_out, a_in, sizeof(T));
	}

	/**
	 * @brief What the D3D11 dispatch reads from Deferred's structured buffer at t2: the directional shadow data (zero without shadow
	 * maps, as D3D11's null SRV reads). The rest it reads from the shared constants, which the anchor latches for the pass.
	 */
	Shadows MakeShadows(const Skylighting& a_feature)
	{
		Shadows shadows{};
		Deferred::DirectionalShadowLightData light{};
		if (a_feature.shadowCascadeSRV && globals::deferred->GetDirectionalShadowLightData(light)) {
			CopyBytes(shadows.shadowProj, &light.ShadowProj);
			shadows.splitDistances[0] = light.EndSplitDistances.x;
			shadows.splitDistances[1] = light.EndSplitDistances.y;
			shadows.splitDistances[2] = light.StartSplitDistances.x;
			shadows.splitDistances[3] = light.StartSplitDistances.y;
		}
		return shadows;
	}

	Inputs MakeInputs(const Skylighting& a_feature)
	{
		Inputs inputs{};
		if (const auto* occlusion = a_feature.texOcclusion) {
			inputs.occlusion = occlusion->resource.get();
			inputs.occlusionSRV = occlusion->srv.get();
			inputs.occlusionDSV = occlusion->dsv.get();
		}
		inputs.shadowCascades = a_feature.shadowCascadeSRV;
		inputs.esramShadows = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM].depthSRV;
		return inputs;
	}

	// UpdateProbesCS's push block, in its list order.
	struct UpdateProbesBlock
	{
		SharedConstants::Handles shared;
		ShaderBindings::Handle occlusion, shadowCascades, esramShadows;
		ShaderBindings::Handle volumes[kVolumes];
		ShaderBindings::Handle comparisonSampler;
		ShaderBindings::Latch shadows;
	};

	// UpdateProbesCS runs 8x8x1 groups.
	constexpr std::uint32_t kGroupX = 8, kGroupY = 8;

	/** @brief The probe update's graph state. Built on the render thread; the pass copies what it uses when it is made. */
	struct Resources
	{
		std::uint32_t groups[3]{};
		std::shared_ptr<const ComputeProgram> program;
		org::OwnedDescriptorBinding sampler;
		std::array<std::unique_ptr<GraphResources::Texture3D>, kVolumes> volumes;
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
		// The imported inputs, null until the first frame that updates; replaced (and the client refreshed) when D3D11 gives others.
		std::shared_ptr<org::ExternalTextureResource> occlusion, shadowCascades, esramShadows;
	};

	/** @brief What the render thread hands the anchor's commit: this frame's shadow data, on a frame that updates. */
	struct ClientState
	{
		std::optional<Shadows> pending;
		bool failed = false;
	};

	struct ProbeBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken occlusion, shadowCascades, esramShadows;
		std::array<org::DeclaredViewToken, kVolumes> volumes;
	};

	struct ProbeFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		ShaderBindings::Handle sampler = 0;
	};

	/** @brief The probe update, through the client's run gate: the commit opens it on the frames that update. */
	class UpdateProbesPass final : public org::TypedRenderGraphPass<UpdateProbesPass, ProbeFrame, ProbeBindings>
	{
	public:
		// At the build, on the render thread: the resources as they are now (GraphResources' rule).
		explicit UpdateProbesPass(const Resources& a_resources) :
			occlusion(a_resources.occlusion), shadowCascades(a_resources.shadowCascades), esramShadows(a_resources.esramShadows)
		{
			for (std::size_t i = 0; i < kVolumes; ++i)
				volumes[i] = a_resources.volumes[i]->Graph();
			frame.program = a_resources.program;
			frame.latch = a_resources.latch;
			frame.gate = a_resources.gate;
			frame.sampler = a_resources.sampler.Index();
		}

		ProbeBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			ProbeBindings bindings{};
			bindings.shared = SharedConstants::Declare(a_builder);
			bindings.occlusion = a_builder.ShaderResource(occlusion).View();
			// The shader reads the cascades as one Texture2DArray: the whole-array view, not the default (first slice).
			const org::SrvView wholeArray{ .variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull) };
			bindings.shadowCascades = a_builder.ShaderResource(shadowCascades, wholeArray).View();
			bindings.esramShadows = a_builder.ShaderResource(esramShadows, wholeArray).View();
			for (std::size_t i = 0; i < kVolumes; ++i)
				bindings.volumes[i] = a_builder.UnorderedAccess(volumes[i]).View();
			return bindings;
		}

		// The values that change per frame are latched and gated, so the recording depends only on these.
		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.latch.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.gate.get()));
			a_out.push_back(frame.sampler);
		}

		ProbeFrame Prepare(const ProbeBindings&, const org::PassPrepareContext&) const { return frame; }

		static void Record(const ProbeBindings& a_bindings, const ProbeFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch || !a_frame.gate)
				return;
			UpdateProbesBlock block{};
			block.shared = SharedConstants::Index(a_recording, a_bindings.shared);
			block.occlusion = a_recording.Index(a_bindings.occlusion);
			block.shadowCascades = a_recording.Index(a_bindings.shadowCascades);
			block.esramShadows = a_recording.Index(a_bindings.esramShadows);
			for (std::size_t i = 0; i < kVolumes; ++i)
				block.volumes[i] = a_recording.Index(a_bindings.volumes[i]);
			block.comparisonSampler = a_frame.sampler;
			block.shadows = ShaderBindings::LatchOf(*a_frame.latch, a_recording.Run());
			a_frame.gate->Dispatch(a_recording, *a_frame.program, ShaderBindings::Data(block), 0);
		}

	private:
		std::shared_ptr<org::Resource> occlusion, shadowCascades, esramShadows;
		std::array<std::shared_ptr<org::Resource>, kVolumes> volumes;
		ProbeFrame frame;
	};

	/** @brief A shadow map's texture behind a_srv: the engine's depth-stencil import when it is one of the engine's, else its own. */
	std::shared_ptr<org::ExternalTextureResource> ImportShadowMap(ID3D11ShaderResourceView* a_srv, const char* a_id)
	{
		if (!a_srv)
			return nullptr;
		winrt::com_ptr<ID3D11Resource> resource;
		a_srv->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture)
			return nullptr;
		// The view and the depth fields can be tripwire handles or DXVK's objects: compare what they wrap.
		using RenderGraph::Engine::Tripwire::Unwrap;
		const auto& depthStencils = globals::game::renderer->GetDepthStencilData().depthStencils;
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); ++i)
			if (depthStencils[i].texture && Unwrap(depthStencils[i].texture) == Unwrap(texture.get()))
				return FrameAnchors::EngineDepth(i);
		RE::BSGraphics::DepthStencilData target{};
		target.texture = texture.get();
		target.depthSRV = a_srv;
		return FrameAnchors::ImportDepth(target, a_id);
	}

	D3D11_TEXTURE3D_DESC VolumeDesc(const std::uint32_t (&a_dims)[3], DXGI_FORMAT a_format)
	{
		return { .Width = a_dims[0],
			.Height = a_dims[1],
			.Depth = a_dims[2],
			.MipLevels = 1,
			.Format = a_format,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0 };
	}
}

struct ORGSkylighting::Impl
{
	std::shared_ptr<Resources> resources;
	std::shared_ptr<ClientState> state;

	/** @brief The inputs' imports for this frame; false when one is missing. Refreshes the client when one changed. */
	bool ResolveInputs(const Inputs& a_inputs)
	{
		RE::BSGraphics::DepthStencilData occlusionTarget{};
		occlusionTarget.texture = a_inputs.occlusion;
		occlusionTarget.views[0] = a_inputs.occlusionDSV;
		occlusionTarget.depthSRV = a_inputs.occlusionSRV;
		auto occlusion = FrameAnchors::ImportDepth(occlusionTarget, "cs.skylighting.occlusion");
		auto shadowCascades = ImportShadowMap(a_inputs.shadowCascades, "cs.skylighting.shadow-cascades");
		auto esramShadows = ImportShadowMap(a_inputs.esramShadows, "cs.skylighting.esram-shadows");
		if (!occlusion || !shadowCascades || !esramShadows)
			return false;
		auto& r = *resources;
		if (r.occlusion != occlusion || r.shadowCascades != shadowCascades || r.esramShadows != esramShadows) {
			r.occlusion = std::move(occlusion);
			r.shadowCascades = std::move(shadowCascades);
			r.esramShadows = std::move(esramShadows);
			FrameAnchors::Refresh(kClientId);
		}
		return true;
	}

	/**
	 * @brief The client: the volumes' reset (one-shot clears, armed at registration and by QueueReset) before the gated probe update.
	 * A step that cannot be made fails a_builder.
	 */
	FrameAnchors::ClientDesc MakeClient(RenderGraph::AnchorPasses::Builder& a_builder, std::function<void()> a_onFailed) const
	{
		namespace AnchorPasses = RenderGraph::AnchorPasses;
		const auto volume = [&](ORGSkylighting::Volume a_volume) { return resources->volumes[static_cast<std::size_t>(a_volume)]->Graph(); };
		// The values the D3D11 path's ResetSkylighting clears to: unit SH (fully unoccluded, matching Skylighting::UNIT_SH), no
		// accumulated frames, all 32 history bits lit (a reset does not fade in from black), full visibility.
		const float unitSH = std::sqrt(4.0f * std::numbers::pi_v<float>);
		constexpr std::array<std::uint32_t, 4> none{ 0, 0, 0, 0 };
		constexpr std::array<std::uint32_t, 4> lit{ 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
		a_builder.Add(AnchorPasses::Clear{ .name = "reset-probes", .target = volume(Volume::ProbeArray), .value = { unitSH, 0.0f, 0.0f, 0.0f } });
		a_builder.Add(AnchorPasses::Clear{ .name = "reset-accum-frames", .target = volume(Volume::AccumFrames), .uintValue = none });
		a_builder.Add(AnchorPasses::Clear{ .name = "reset-shadow-bitmask", .target = volume(Volume::ShadowBitmask), .uintValue = lit });
		a_builder.Add(AnchorPasses::Clear{ .name = "reset-shadow-visibility", .target = volume(Volume::ShadowVisibility), .value = { 1.0f, 1.0f, 1.0f, 1.0f } });
		// Left out of a build until the first frame's inputs are imported.
		a_builder.Add("update-probes", FrameAnchors::PassKind::Compute, [resources = resources]() -> std::shared_ptr<org::RenderPass> {
			if (!resources->occlusion || !resources->shadowCascades || !resources->esramShadows)
				return nullptr;
			return std::make_shared<UpdateProbesPass>(*resources);
		});

		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.passes = a_builder.Take();
		client.sharedConstants = true;
		client.gates = { resources->gate };
		client.registerResources = [resources = resources](org::RenderGraph& a_graph) {
			for (const auto& volume : resources->volumes)
				volume->Register(a_graph);
		};
		// Active on frames that update; every run closes the gate first, so the others dispatch nothing.
		client.active = [state = state] { return state->pending.has_value(); };
		client.commit = [state = state, resources = resources](const FrameAnchors::Point&) {
			if (!state->pending)
				return;
			resources->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, *state->pending);
			resources->gate->OpenDispatch(0, resources->groups[0], resources->groups[1], resources->groups[2]);
			state->pending.reset();
		};
		client.onFailed = [state = state, onFailed = std::move(a_onFailed)] {
			logger::error("[ORG] Skylighting: the render graph failed; the probe update returns to D3D11");
			state->failed = true;
			WithdrawVolumes();
			if (onFailed)
				onFailed();
		};
		return client;
	}
};

ORGSkylighting& ORGSkylighting::Get()
{
	static ORGSkylighting s_skylighting;
	return s_skylighting;
}

ORGSkylighting::~ORGSkylighting() = default;

void ORGSkylighting::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Prepass))
		logger::warn("[ORG] Skylighting: the Prepass anchor could not be reserved; the probe update stays on D3D11");
}

bool ORGSkylighting::Setup(const Skylighting& a_feature, std::function<void()> a_onFailed)
{
	impl.reset();
	if (!Requested())
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!runtime.IsActive() || !host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Prepass)) {
		logger::warn("[ORG] Skylighting: the render graph or its Prepass anchor is not available; the probe update stays on D3D11");
		return false;
	}

	const auto& dims = a_feature.probeArrayDims;
	auto next = std::make_unique<Impl>();
	next->state = std::make_shared<ClientState>();
	auto resources = std::make_shared<Resources>();
	resources->groups[0] = (dims[0] + kGroupX - 1) / kGroupX;
	resources->groups[1] = (dims[1] + kGroupY - 1) / kGroupY;
	resources->groups[2] = dims[2];
	resources->program = ComputeProgram::Load(host->Device(), { .source = kShader, .constantWords = ShaderBindings::Words<UpdateProbesBlock>() });
	resources->sampler = ShaderBindings::CreateSampler({ .compareEnable = true, .compareOp = rhi::CompareOp::LessEqual, .reduction = rhi::ReductionMode::Comparison });
	resources->latch = std::make_shared<org::LatchBlock>("cs.skylighting.latch", static_cast<std::uint32_t>(sizeof(Shadows)));
	resources->gate = RenderGraph::RunGate::Create("cs.skylighting.gate");
	if (!resources->program || !resources->sampler || !resources->gate) {
		logger::warn("[ORG] Skylighting: the probe update's program, sampler or gate could not be made; it stays on D3D11");
		return false;
	}
	// Read by D3D11 (the engine's draws, the composite): exported SRV. ResetSkylighting clears them on the graph (QueueReset).
	constexpr GraphResources::Options volumeOptions{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV };
	constexpr std::pair<DXGI_FORMAT, std::string_view> volumeInfo[kVolumes] = {
		{ DXGI_FORMAT_R16G16B16A16_FLOAT, kProbeArrayId },
		{ DXGI_FORMAT_R8_UINT, kAccumFramesId },
		{ DXGI_FORMAT_R32_UINT, kShadowBitmaskId },
		{ DXGI_FORMAT_R8_UNORM, kShadowVisibilityId },
	};
	for (std::size_t i = 0; i < kVolumes; ++i) {
		auto& volume = resources->volumes[i];
		volume = std::make_unique<GraphResources::Texture3D>(VolumeDesc(dims, volumeInfo[i].first), std::string(volumeInfo[i].second), volumeOptions);
		if (!*volume || !volume->SRV()) {
			logger::warn("[ORG] Skylighting: {} could not be made and exported; the probe update stays on D3D11", volumeInfo[i].second);
			return false;
		}
	}
	next->resources = resources;

	// The four reset clears, then the update at -1, before IBL (0 and 1).
	RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::Prepass, kClientId, -5);
	auto client = next->MakeClient(builder, std::move(a_onFailed));
	if (builder.Failed()) {
		logger::warn("[ORG] Skylighting: {}; the probe update stays on D3D11", builder.Failure());
		return false;
	}
	if (!FrameAnchors::Register(std::move(client)))
		return false;
	// Drawcall Limit Fix's colour passes read them by name, ordered after the update (Integrations/Skylighting).
	if (!PublishedResources::Publish(*resources->volumes[static_cast<std::size_t>(Volume::ProbeArray)]) ||
		!PublishedResources::Publish(*resources->volumes[static_cast<std::size_t>(Volume::ShadowVisibility)])) {
		logger::warn("[ORG] Skylighting: the probe volumes could not be published; the probe update stays on D3D11");
		FrameAnchors::Unregister(kClientId);
		WithdrawVolumes();
		return false;
	}
	impl = std::move(next);
	logger::info("[ORG] Skylighting: the probe update runs on the render graph at the Prepass anchor ({}x{}x{})", dims[0], dims[1], dims[2]);
	return true;
}

bool ORGSkylighting::Update(const Skylighting& a_feature)
{
	if (!IsActive())
		return false;
	// Without an input the gate stays closed: the graph's volumes hold this frame as they are.
	if (impl->ResolveInputs(MakeInputs(a_feature)))
		impl->state->pending = MakeShadows(a_feature);
	return true;
}

bool ORGSkylighting::IsActive() const
{
	return impl && !impl->state->failed && RenderGraphRuntime::Get().IsActive();
}

ID3D11ShaderResourceView* ORGSkylighting::SRV(Volume a_volume) const
{
	return IsActive() && a_volume < Volume::Count ? impl->resources->volumes[static_cast<std::size_t>(a_volume)]->SRV() : nullptr;
}

bool ORGSkylighting::QueueReset()
{
	if (!IsActive())
		return false;
	FrameAnchors::Rearm(kClientId);
	logger::info("[ORG] Skylighting: the probe volumes reset on the render graph (cs.skylighting.reset-* at the next Prepass)");
	return true;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: the probe update always runs on D3D11.
struct ORGSkylighting::Impl
{};

ORGSkylighting& ORGSkylighting::Get()
{
	static ORGSkylighting s_skylighting;
	return s_skylighting;
}

ORGSkylighting::~ORGSkylighting() = default;
void ORGSkylighting::Reserve() {}
bool ORGSkylighting::Setup(const Skylighting&, std::function<void()>) { return false; }
bool ORGSkylighting::Update(const Skylighting&) { return false; }
bool ORGSkylighting::IsActive() const { return false; }
ID3D11ShaderResourceView* ORGSkylighting::SRV(Volume) const { return nullptr; }
bool ORGSkylighting::QueueReset() { return false; }

#endif
