#include "ORGSkylighting.h"

#include <cstring>

bool ORGSkylighting::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA("CS_ORG_SKYLIGHTING", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Deferred.h"
#	include "Features/Skylighting.h"
#	include "Globals.h"
#	include "State.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>

#	include <array>
#	include <atomic>
#	include <cmath>
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

	/** @brief One frame's values, as UpdateProbesFrame in UpdateProbesCS.hlsl: float4 rows, tightly packed. */
	struct Frame
	{
		float occlusionViewProj[16];  // SkylightingSettings' row_major matrix, as stored
		float occlusionSHBasis4Pi[4];
		float posOffset[4];
		std::uint32_t arrayOrigin[4];
		std::int32_t validMargin[4];
		float cameraViewProj[16];  // FrameBuffer's row_major matrix, as stored
		float cameraPosAdjust[4];
		float cameraData[4];          // SharedData::CameraData
		float shadowProj[32];         // DirectionalShadowLightData::ShadowProj, as stored (column_major)
		float splitDistances[4];      // end xy, start zw
		std::uint32_t frameCount[4];  // x: FrameCountAlwaysActive, y: whether the probes update
	};
	static_assert(sizeof(Frame) == 24 * 16);

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
	 * @brief What the D3D11 dispatch reads from the shared constant buffers at this point: the skylighting settings this frame's
	 * feature buffer took (Skylighting::frameSettings), the main camera's per-frame buffer, SharedData's camera data and frame count,
	 * and the directional shadow data Deferred uploads at t98 (zero without shadow maps, as D3D11's null SRV reads).
	 */
	Frame MakeFrame(const Skylighting& a_feature)
	{
		Frame frame{};
		const auto& settings = a_feature.frameSettings;
		CopyBytes(frame.occlusionViewProj, &settings.OcclusionViewProj);
		CopyBytes(frame.occlusionSHBasis4Pi, &settings.OcclusionSHBasis4Pi);
		CopyBytes(frame.posOffset, &settings.PosOffset);
		std::copy(std::begin(settings.ArrayOrigin), std::end(settings.ArrayOrigin), frame.arrayOrigin);
		std::copy(std::begin(settings.ValidMargin), std::end(settings.ValidMargin), frame.validMargin);
		const auto& camera = globals::game::frameBufferCached;
		CopyBytes(frame.cameraViewProj, &camera.GetCameraViewProj());
		CopyBytes(frame.cameraPosAdjust, &camera.GetCameraPosAdjust());
		const auto cameraData = Util::GetCameraData();
		CopyBytes(frame.cameraData, &cameraData);
		Deferred::DirectionalShadowLightData shadows{};
		if (a_feature.shadowCascadeSRV && globals::deferred->GetDirectionalShadowLightData(shadows)) {
			CopyBytes(frame.shadowProj, &shadows.ShadowProj);
			frame.splitDistances[0] = shadows.EndSplitDistances.x;
			frame.splitDistances[1] = shadows.EndSplitDistances.y;
			frame.splitDistances[2] = shadows.StartSplitDistances.x;
			frame.splitDistances[3] = shadows.StartSplitDistances.y;
		}
		frame.frameCount[0] = globals::state->frameCount;
		return frame;
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
		ShaderBindings::Handle occlusion, shadowCascades, esramShadows;
		ShaderBindings::Handle volumes[kVolumes];
		ShaderBindings::Handle comparisonSampler;
		ShaderBindings::Latch frame;
	};

	// UpdateProbesCS runs 8x8x1 groups.
	constexpr std::uint32_t kGroupX = 8, kGroupY = 8;

	/** @brief The probe update's graph state. Built on the render thread; the pass copies what it uses when it is made. */
	struct Resources
	{
		std::uint32_t dims[3]{};
		std::shared_ptr<const ComputeProgram> program;
		org::OwnedDescriptorBinding sampler;
		std::array<std::unique_ptr<GraphResources::Texture3D>, kVolumes> volumes;
		std::shared_ptr<org::LatchBlock> latch;
		// The imported inputs; replaced (and the client refreshed) when D3D11 gives other textures.
		std::shared_ptr<org::ExternalTextureResource> occlusion, shadowCascades, esramShadows;
	};

	/**
	 * @brief What the render thread hands the anchor's commit: this frame's values, how many idle frames were latched since, and a queued
	 * reset of the volumes (ResetSkylighting).
	 */
	struct ClientState
	{
		std::optional<Frame> pending;
		std::uint32_t idleCommits = ~0u;
		bool failed = false;
		// Render thread: a reset waits for a build with the reset pass, then for that pass to be prepared; the next build drops it.
		bool resetQueued = false;
		std::atomic<bool> resetPrepared{ false };  // set where the reset pass prepares (possibly the host's thread)
	};

	struct ResetBindings
	{
		std::array<org::ResourceBindingToken, kVolumes> resources;
		std::array<org::DeclaredViewToken, kVolumes> shaderVisible, cpuVisible;
	};

	struct ResetFrame
	{};

	/** @brief The volumes cleared to their reset values: in builds after a reset only (ClientState::resetQueued). */
	class ResetVolumesPass final : public org::TypedRenderGraphPass<ResetVolumesPass, ResetFrame, ResetBindings>
	{
	public:
		ResetVolumesPass(const std::array<std::shared_ptr<org::Resource>, kVolumes>& a_volumes, std::shared_ptr<ClientState> a_state) :
			volumes(a_volumes), state(std::move(a_state)) {}

		ResetBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			ResetBindings bindings{};
			for (std::size_t i = 0; i < kVolumes; ++i) {
				const auto use = a_builder.UnorderedAccessClear(volumes[i]);
				bindings.resources[i] = use.resources.front();
				bindings.shaderVisible[i] = use.views[0];
				bindings.cpuVisible[i] = use.views[1];
			}
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			for (const auto& volume : volumes)
				a_out.push_back(reinterpret_cast<std::uintptr_t>(volume.get()));
		}

		// The reset counts as done once this pass was prepared: a build made too late for this run's epoch has not cleared anything.
		ResetFrame Prepare(const ResetBindings&, const org::PassPrepareContext&) const
		{
			state->resetPrepared.store(true, std::memory_order_release);
			return {};
		}

		static void Record(const ResetBindings& a_bindings, const ResetFrame&, org::PassRecordContext& a_recording)
		{
			auto& commands = a_recording.Commands();
			const auto info = [&](ORGSkylighting::Volume a_volume) {
				const auto i = static_cast<std::size_t>(a_volume);
				rhi::UavClearInfo clear{};
				clear.shaderVisible = a_recording.View(a_bindings.shaderVisible[i]);
				clear.cpuVisible = a_recording.View(a_bindings.cpuVisible[i]);
				clear.resource = a_recording.Resolve(a_bindings.resources[i]);
				return clear;
			};
			// The values the D3D11 path's ResetSkylighting clears to: unit SH (fully unoccluded, matching Skylighting::UNIT_SH), no
			// accumulated frames, all 32 history bits lit (a reset does not fade in from black), full visibility.
			const float unitSH[4] = { std::sqrt(4.0f * std::numbers::pi_v<float>), 0.0f, 0.0f, 0.0f };
			const float visible[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			const std::uint32_t none[4] = { 0, 0, 0, 0 };
			const std::uint32_t lit[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
			commands.ClearUavFloat(info(ORGSkylighting::Volume::ProbeArray), rhi::UavClearFloat(unitSH));
			commands.ClearUavUint(info(ORGSkylighting::Volume::AccumFrames), rhi::UavClearUint(none));
			commands.ClearUavUint(info(ORGSkylighting::Volume::ShadowBitmask), rhi::UavClearUint(lit));
			commands.ClearUavFloat(info(ORGSkylighting::Volume::ShadowVisibility), rhi::UavClearFloat(visible));
		}

	private:
		std::array<std::shared_ptr<org::Resource>, kVolumes> volumes;
		std::shared_ptr<ClientState> state;
	};

	struct ProbeBindings
	{
		org::DeclaredViewToken occlusion, shadowCascades, esramShadows;
		std::array<org::DeclaredViewToken, kVolumes> volumes;
	};

	struct ProbeFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		ShaderBindings::Handle sampler = 0;
		std::uint32_t groups[3]{};
	};

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
			frame.sampler = a_resources.sampler.Index();
			frame.groups[0] = (a_resources.dims[0] + kGroupX - 1) / kGroupX;
			frame.groups[1] = (a_resources.dims[1] + kGroupY - 1) / kGroupY;
			frame.groups[2] = a_resources.dims[2];
		}

		ProbeBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			ProbeBindings bindings{};
			bindings.occlusion = a_builder.ShaderResource(occlusion).View();
			// The shader reads the cascades as one Texture2DArray: the whole-array view, not the default (first slice).
			const org::SrvView wholeArray{ .variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull) };
			bindings.shadowCascades = a_builder.ShaderResource(shadowCascades, wholeArray).View();
			bindings.esramShadows = a_builder.ShaderResource(esramShadows, wholeArray).View();
			for (std::size_t i = 0; i < kVolumes; ++i)
				bindings.volumes[i] = a_builder.UnorderedAccess(volumes[i]).View();
			return bindings;
		}

		// The values that change per frame are latched, so the recording depends only on these.
		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.latch.get()));
			a_out.push_back(frame.sampler);
		}

		ProbeFrame Prepare(const ProbeBindings&, const org::PassPrepareContext&) const { return frame; }

		static void Record(const ProbeBindings& a_bindings, const ProbeFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch)
				return;
			UpdateProbesBlock block{};
			block.occlusion = a_recording.Index(a_bindings.occlusion);
			block.shadowCascades = a_recording.Index(a_bindings.shadowCascades);
			block.esramShadows = a_recording.Index(a_bindings.esramShadows);
			for (std::size_t i = 0; i < kVolumes; ++i)
				block.volumes[i] = a_recording.Index(a_bindings.volumes[i]);
			block.comparisonSampler = a_frame.sampler;
			block.frame = ShaderBindings::LatchOf(*a_frame.latch, a_recording.FrameSlot());
			a_frame.program->Dispatch(a_recording, ShaderBindings::Data(block), a_frame.groups[0], a_frame.groups[1], a_frame.groups[2]);
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
		const auto& depthStencils = globals::game::renderer->GetDepthStencilData().depthStencils;
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); ++i)
			if (depthStencils[i].texture == texture.get())
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
	bool ResolveInputs(const Inputs& a_inputs, bool a_registered)
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
			if (a_registered)
				FrameAnchors::Refresh(kClientId);
		}
		return true;
	}

	FrameAnchors::ClientDesc MakeClient() const
	{
		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		// Before the update, which then reads the reset volumes.
		client.passes.push_back({ .name = std::string(kClientId) + ".reset",
			.anchor = FrameAnchors::Anchor::Prepass,
			.order = -1,
			.kind = FrameAnchors::PassKind::Compute,
			.make = [resources = resources, state = state]() -> std::shared_ptr<org::RenderPass> {
				if (!state->resetQueued)
					return nullptr;
				std::array<std::shared_ptr<org::Resource>, kVolumes> volumes;
				for (std::size_t i = 0; i < kVolumes; ++i)
					volumes[i] = resources->volumes[i]->Graph();
				return std::make_shared<ResetVolumesPass>(volumes, state);
			} });
		client.passes.push_back({ .name = std::string(kClientId) + ".update-probes",
			.anchor = FrameAnchors::Anchor::Prepass,
			.kind = FrameAnchors::PassKind::Compute,
			.make = [resources = resources]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<UpdateProbesPass>(*resources); } });
		client.registerResources = [resources = resources](org::RenderGraph& a_graph) {
			for (const auto& volume : resources->volumes)
				volume->Register(a_graph);
		};
		// Each frame slot holds its own latch: after the last update, every slot is latched idle once before the client rests, so a
		// slot submitted again by another client's epoch never replays an old update.
		client.active = [state = state, resources = resources] {
			return state->pending.has_value() || state->idleCommits < resources->latch->Slots() || state->resetQueued;
		};
		client.commit = [state = state, resources = resources](const FrameAnchors::Point&) {
			// A reset pass was prepared: the next build leaves it out.
			if (state->resetQueued && state->resetPrepared.exchange(false, std::memory_order_acq_rel)) {
				state->resetQueued = false;
				FrameAnchors::Refresh(kClientId);
			}
			Frame frame{};
			if (state->pending) {
				frame = *state->pending;
				frame.frameCount[1] = 1;
				state->idleCommits = 0;
			} else if (state->idleCommits != ~0u) {
				++state->idleCommits;
			}
			state->pending.reset();
			resources->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, frame);
		};
		client.onFailed = [state = state] {
			state->failed = true;
			WithdrawVolumes();
			logger::error("[ORG] Skylighting: the render graph failed; the probe update returns to D3D11");
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

bool ORGSkylighting::Update(const Skylighting& a_feature)
{
	const auto& dims = a_feature.probeArrayDims;
	const Inputs inputs = MakeInputs(a_feature);
	if (impl && impl->state->failed)
		impl.reset();  // the graph failed: its volumes go, Skylighting makes its D3D11 ones again
	if (!impl) {
		if (attempted || !Requested())
			return false;
		auto& runtime = RenderGraphRuntime::Get();
		auto* host = runtime.Host();
		// The shadow maps are known from the first shadow mask draw: set up once they are.
		if (!runtime.IsActive() || !host || !inputs.shadowCascades || !inputs.esramShadows || !inputs.occlusion)
			return false;
		attempted = true;
		if (!FrameAnchors::HasEpoch(FrameAnchors::Anchor::Prepass)) {
			logger::warn("[ORG] Skylighting: no Prepass epoch; the probe update stays on D3D11");
			return false;
		}
		auto state = std::make_unique<Impl>();
		state->state = std::make_shared<ClientState>();
		auto resources = std::make_shared<Resources>();
		std::copy(std::begin(dims), std::end(dims), resources->dims);
		resources->program = ComputeProgram::Load(host->Device(), { .source = kShader, .constantWords = ShaderBindings::Words<UpdateProbesBlock>() });
		resources->sampler = ShaderBindings::CreateSampler({ .compareEnable = true, .compareOp = rhi::CompareOp::LessEqual, .reduction = rhi::ReductionMode::Comparison });
		resources->latch = std::make_shared<org::LatchBlock>("cs.skylighting.latch", static_cast<std::uint32_t>(sizeof(Frame)), host->FrameSlots());
		if (!resources->program || !resources->sampler) {
			logger::warn("[ORG] Skylighting: the probe update's program or sampler could not be made; it stays on D3D11");
			return false;
		}
		// Read by D3D11 (the engine's draws, the composite): exported SRV. ResetSkylighting clears them on the graph (QueueReset).
		constexpr GraphResources::Options volumeOptions{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV };
		constexpr std::pair<DXGI_FORMAT, std::string_view> volumeInfo[kVolumes] = {
			{ DXGI_FORMAT_R16G16B16A16_FLOAT, kProbeArrayId },
			{ DXGI_FORMAT_R8_UINT, "Skylighting::AccumFramesArray" },
			{ DXGI_FORMAT_R32_UINT, "Skylighting::ShadowBitmask" },
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
		state->resources = resources;
		if (!state->ResolveInputs(inputs, false)) {
			logger::warn("[ORG] Skylighting: the occlusion or shadow maps could not be imported; the probe update stays on D3D11");
			return false;
		}
		if (!FrameAnchors::Register(state->MakeClient()))
			return false;
		// Drawcall Limit Fix's colour passes read them by name, ordered after the update (Integrations/Skylighting).
		if (!PublishedResources::Publish(*resources->volumes[static_cast<std::size_t>(Volume::ProbeArray)]) ||
			!PublishedResources::Publish(*resources->volumes[static_cast<std::size_t>(Volume::ShadowVisibility)])) {
			logger::warn("[ORG] Skylighting: the probe volumes could not be published; the probe update stays on D3D11");
			FrameAnchors::Unregister(kClientId);
			WithdrawVolumes();
			return false;
		}
		impl = std::move(state);
		logger::info("[ORG] Skylighting: the probe update runs on the render graph at the Prepass anchor ({}x{}x{})", dims[0], dims[1], dims[2]);
	} else if (!impl->ResolveInputs(inputs, true)) {
		// An input went missing: the graph's volumes hold this frame as they are.
		return true;
	}
	impl->state->pending = MakeFrame(a_feature);
	return true;
}

bool ORGSkylighting::IsActive() const
{
	return impl && !impl->state->failed && RenderGraphRuntime::Get().IsActive();
}

ID3D11ShaderResourceView* ORGSkylighting::SRV(Volume a_volume) const
{
	return impl && a_volume < Volume::Count ? impl->resources->volumes[static_cast<std::size_t>(a_volume)]->SRV() : nullptr;
}

bool ORGSkylighting::QueueReset()
{
	if (!IsActive())
		return false;
	auto& state = *impl->state;
	if (!state.resetQueued) {
		state.resetQueued = true;
		state.resetPrepared.store(false, std::memory_order_release);
		// The next build holds the reset pass.
		FrameAnchors::Refresh(kClientId);
		logger::info("[ORG] Skylighting: the probe volumes reset on the render graph (cs.skylighting.reset at the next Prepass)");
	}
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
bool ORGSkylighting::Update(const Skylighting&) { return false; }
bool ORGSkylighting::IsActive() const { return false; }
ID3D11ShaderResourceView* ORGSkylighting::SRV(Volume) const { return nullptr; }
bool ORGSkylighting::QueueReset() { return false; }

#endif
