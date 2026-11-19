#include "ORGSkylighting.h"

#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Switches.h"

bool ORGSkylighting::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
	return requested;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Buffer.h"
#	include "Deferred.h"
#	include "Features/Skylighting.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <array>
#	include <cmath>
#	include <cstring>
#	include <numbers>
#	include <optional>
#	include <type_traits>
#	include <utility>

namespace
{
	constexpr const char* kClientId = "cs.skylighting";
	constexpr const char* kShader = "Skylighting/UpdateProbesCS.hlsl";
	// The engine skeleton's clear shader: depth 1.0 from the vertex stage and an Always test.
	constexpr const char* kClearShader = "RenderGraph/EngineClear.hlsl";
	constexpr auto kVolumes = static_cast<std::size_t>(ORGSkylighting::Volume::Count);
	// The probe update, then the occlusion map's clear.
	constexpr std::uint32_t kUpdateEntry = 0;
	constexpr std::uint32_t kClearEntry = 1;

	/** @brief Null when Skylighting made a D3D11 one. */
	std::unique_ptr<GraphResources::Texture2D>& OcclusionTwin()
	{
		static std::unique_ptr<GraphResources::Texture2D> s_twin;
		return s_twin;
	}

	/** @brief For the clear program. */
	DXGI_FORMAT& OcclusionDsvFormat()
	{
		static DXGI_FORMAT s_format = DXGI_FORMAT_UNKNOWN;
		return s_format;
	}

	/** @brief The published volumes' readers return to D3D11. */
	void WithdrawVolumes()
	{
		PublishedResources::Withdraw(ORGSkylighting::kProbeArrayId);
		PublishedResources::Withdraw(ORGSkylighting::kShadowVisibilityId);
	}

	/** @brief As UpdateProbesShadows in UpdateProbesCS.hlsl: float4 rows, tightly packed. */
	struct Shadows
	{
		float shadowProj[32];     // column_major, as stored
		float splitDistances[4];  // end xy, start zw
	};
	static_assert(sizeof(Shadows) == 9 * 16);

	/** @brief As D3D11 gives them. */
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

	/** @brief What the D3D11 dispatch reads at t2. Zero without shadow maps, as D3D11's null SRV reads. */
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

	// In list order.
	struct UpdateProbesBlock
	{
		SharedConstants::Handles shared;
		ShaderBindings::Handle occlusion, shadowCascades, esramShadows;
		ShaderBindings::Handle volumes[kVolumes];
		ShaderBindings::Handle comparisonSampler;
		ShaderBindings::Latch shadows;
	};

	constexpr std::uint32_t kGroupX = 8, kGroupY = 8;

	/** @brief Built on the render thread. The pass copies what it uses when it is made. */
	struct Resources
	{
		std::uint32_t groups[3]{};
		std::shared_ptr<const ComputeProgram> program;
		org::OwnedDescriptorBinding sampler;
		std::array<std::unique_ptr<GraphResources::Texture3D>, kVolumes> volumes;
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
		// Null until the first frame that updates. Replaced, refreshing the client, when D3D11 gives others.
		FrameAnchors::Texture occlusion, shadowCascades, esramShadows;
		// Only for the twin. Null keeps the D3D11 clear.
		std::shared_ptr<const RasterProgram> clearProgram;
	};

	/** @brief This frame's shadow data, on a frame that updates. */
	struct ClientState
	{
		std::optional<Shadows> pending;
		// Render thread. DCLF drew the map since its last graph clear.
		bool occlusionDrawn = false;
		// The next run clears the map after the update read it.
		bool clearOcclusion = false;
		bool occlusionCleared = false;
		// Native mode: a run with the clear executed, so the map holds no unwritten memory and the update may read it.
		bool mapCleared = false;
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

	/** @brief The commit opens it on frames that update. */
	class UpdateProbesPass final : public org::TypedRenderGraphPass<UpdateProbesPass, ProbeFrame, ProbeBindings>
	{
	public:
		// At the build, on the render thread, so it sees the resources as they are now.
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
			// The shader reads the cascades as one Texture2DArray, so use the whole-array view.
			const org::SrvView wholeArray{ .variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull) };
			bindings.shadowCascades = a_builder.ShaderResource(shadowCascades, wholeArray).View();
			bindings.esramShadows = a_builder.ShaderResource(esramShadows, wholeArray).View();
			for (std::size_t i = 0; i < kVolumes; ++i)
				bindings.volumes[i] = a_builder.UnorderedAccess(volumes[i]).View();
			return bindings;
		}

		// Per-frame values are latched and gated, so the recording depends only on these.
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
			a_frame.gate->Dispatch(a_recording, *a_frame.program, ShaderBindings::Data(block), kUpdateEntry);
		}

	private:
		FrameAnchors::Texture occlusion, shadowCascades, esramShadows;
		std::array<std::shared_ptr<org::Resource>, kVolumes> volumes;
		ProbeFrame frame;
	};

	struct ClearBindings
	{
		org::DeclaredViewToken depth;
	};

	struct ClearFrame
	{
		std::shared_ptr<const RasterProgram> program;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t width = 0, height = 0;
	};

	/**
	* @brief Clears the map to 1.0 after the update read it, so DCLF's next shadow epoch draws into an empty map.
	* D3D11 cleared it just before that epoch. The commit opens it on the runs Update queued.
	*/
	class OcclusionClearPass final : public org::TypedRenderGraphPass<OcclusionClearPass, ClearFrame, ClearBindings>
	{
	public:
		OcclusionClearPass(std::shared_ptr<org::ExternalTextureResource> a_map, const Resources& a_resources) :
			map(std::move(a_map))
		{
			frame.program = a_resources.clearProgram;
			frame.gate = a_resources.gate;
			frame.width = static_cast<std::uint32_t>(map->GetWidth());
			frame.height = static_cast<std::uint32_t>(map->GetHeight());
		}

		ClearBindings Declare(org::PassBuilder& a_builder) { return { a_builder.DepthReadWrite(map).View() }; }

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.gate.get()));
		}

		ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const { return frame; }

		static void Record(const ClearBindings& a_bindings, const ClearFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.gate)
				return;
			// Native mode: the map is Skylighting's alone and its render pass clears it with a load op every run.
			// Guest mode: the draw clears it when the gate entry is open.
			const bool native = RenderGraph::Engine::Native::Active();
			const RasterProgram::Depth depth{ .view = &a_bindings.depth, .readOnly = false, .load = native ? rhi::LoadOp::Clear : rhi::LoadOp::Load, .clear = 1.0f };
			RasterProgram::DrawDesc draw{};
			draw.depth = &depth;
			draw.viewport = { .x = 0, .y = 0, .width = a_frame.width, .height = a_frame.height };
			draw.debugName = "Skylighting::OcclusionClear";
			draw.indirect = &a_frame.gate->Latch();
			draw.indirectOffset = a_frame.gate->Offset(kClearEntry);
			a_frame.program->Draw(a_recording, draw);
		}

	private:
		std::shared_ptr<org::ExternalTextureResource> map;
		ClearFrame frame;
	};

	/** @brief The engine's depth-stencil import when it is one of the engine's, else its own. */
	FrameAnchors::Texture ImportShadowMap(ID3D11ShaderResourceView* a_srv, const char* a_id)
	{
		if (!a_srv)
			return nullptr;
		winrt::com_ptr<ID3D11Resource> resource;
		a_srv->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture)
			return nullptr;
		// Either side may be a tripwire handle or DXVK's object, so compare what they wrap.
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

	/** @brief False when one is missing. Refreshes the client when one changed. */
	bool ResolveInputs(const Inputs& a_inputs)
	{
		FrameAnchors::Texture occlusion = ORGSkylighting::Get().OcclusionMap();
		if (!occlusion) {
			RE::BSGraphics::DepthStencilData occlusionTarget{};
			occlusionTarget.texture = a_inputs.occlusion;
			occlusionTarget.views[0] = a_inputs.occlusionDSV;
			occlusionTarget.depthSRV = a_inputs.occlusionSRV;
			occlusion = FrameAnchors::ImportDepth(occlusionTarget, "cs.skylighting.occlusion");
		}
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

	/** @brief One-shot volume resets before the gated probe update. A step that cannot be made fails a_builder. */
	FrameAnchors::ClientDesc MakeClient(RenderGraph::AnchorPasses::Builder& a_builder) const
	{
		namespace AnchorPasses = RenderGraph::AnchorPasses;
		const auto volume = [&](ORGSkylighting::Volume a_volume) { return resources->volumes[static_cast<std::size_t>(a_volume)]->Graph(); };
		// As ResetSkylighting: unit SH (fully unoccluded), no accumulated frames, all 32 history bits lit so a reset does not fade in, full visibility.
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
		});  // counts from the gate's update entry
		// Left out without a twin or program.
		a_builder.Add("clear-occlusion", FrameAnchors::PassKind::Render, [resources = resources]() -> std::shared_ptr<org::RenderPass> {
			auto map = ORGSkylighting::Get().OcclusionMap();
			if (!map || !resources->clearProgram)
				return nullptr;
			return std::make_shared<OcclusionClearPass>(std::move(map), *resources);
		});  // count from the gate's clear entry, depth loaded

		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.passes = a_builder.Take();
		client.sharedConstants = true;
		client.gates = { resources->gate };
		client.registerResources = [resources = resources](org::RenderGraph& a_graph) {
			for (const auto& volume : resources->volumes)
				volume->Register(a_graph);
		};
		// Every run closes the gate first, so other frames dispatch and draw nothing.
		client.active = [state = state] { return state->pending.has_value() || state->clearOcclusion; };
		client.commit = [state = state, resources = resources](const FrameAnchors::Point&) {
			if (std::exchange(state->clearOcclusion, false)) {
				resources->gate->OpenDraw(kClearEntry);
				state->occlusionCleared = true;
				state->mapCleared = true;
			}
			if (!state->pending)
				return;
			resources->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, *state->pending);
			resources->gate->OpenDispatch(kUpdateEntry, resources->groups[0], resources->groups[1], resources->groups[2]);
			state->pending.reset();
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

bool ORGSkylighting::Setup(const Skylighting& a_feature)
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
	resources->gate = RenderGraph::RunGate::Create("cs.skylighting.gate", 2);
	if (!resources->program || !resources->sampler || !resources->gate) {
		logger::warn("[ORG] Skylighting: the probe update's program, sampler or gate could not be made; it stays on D3D11");
		return false;
	}
	// A D3D11 map keeps Skylighting's D3D11 clear.
	if (OcclusionTwin() && OcclusionDsvFormat() != DXGI_FORMAT_UNKNOWN) {
		RasterProgram::Desc clearDesc{ .pixelSource = kClearShader, .vertexSource = kClearShader, .vertexEntry = L"VSMain", .commonDefines = false };
		clearDesc.pixelEntry = L"PSDepth";
		clearDesc.depthFormat = rhi::helpers::ToRHI(OcclusionDsvFormat());
		clearDesc.depth = { .depthEnable = true, .depthWrite = true, .depthFunc = rhi::CompareOp::Always };
		resources->clearProgram = RasterProgram::Load(host->Device(), clearDesc);
		if (!resources->clearProgram)
			logger::warn("[ORG] Skylighting: the occlusion map's clear program could not be made; the map is cleared on D3D11");
	}
	// D3D11 reads them, so the SRV is exported.
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

	// Four reset clears, then the update at -1, before IBL. The clear at 0 touches nothing IBL uses.
	RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::Prepass, kClientId, -5);
	auto client = next->MakeClient(builder);
	if (builder.Failed()) {
		logger::warn("[ORG] Skylighting: {}; the probe update stays on D3D11", builder.Failure());
		return false;
	}
	if (!FrameAnchors::Register(std::move(client)))
		return false;
	// DCLF's colour passes read them by name, ordered after the update.
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

Texture2D* ORGSkylighting::MakeOcclusionMap(const D3D11_TEXTURE2D_DESC& a_desc, DXGI_FORMAT a_srvFormat, DXGI_FORMAT a_dsvFormat)
{
	auto& twin = OcclusionTwin();
	if (twin || !Requested() || !RenderGraphRuntime::Get().IsActive())
		return nullptr;
	// Written and read by D3D11, so exported and never aliased. Read in the frame after one that renders no map.
	D3D11_TEXTURE2D_DESC desc = a_desc;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = 0;
	auto texture = std::make_unique<GraphResources::Texture2D>(desc, std::string(kOcclusionId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::DSV });
	if (!*texture || !texture->D3D11() || !texture->SRV() || !texture->DSV() ||
		!std::dynamic_pointer_cast<org::ExternalTextureResource>(texture->Graph())) {
		logger::warn("[ORG] Skylighting: the sky occlusion map could not be made on the render graph (format {}); it is a D3D11 texture",
			static_cast<int>(a_desc.Format));
		return nullptr;
	}
	// The engine's map draws and the precipitation shaders use views in the target's formats.
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	texture->SRV()->GetDesc(&srvDesc);
	texture->DSV()->GetDesc(&dsvDesc);
	if (srvDesc.Format != a_srvFormat || dsvDesc.Format != a_dsvFormat) {
		logger::warn("[ORG] Skylighting: the graph's sky occlusion map has views in formats {}/{}, the target's are {}/{}; it is a D3D11 texture",
			static_cast<int>(srvDesc.Format), static_cast<int>(dsvDesc.Format), static_cast<int>(a_srvFormat), static_cast<int>(a_dsvFormat));
		return nullptr;
	}
	// Published so DCLF's shadow epoch and the probe update share one resource.
	if (!PublishedResources::Publish(*texture)) {
		logger::warn("[ORG] Skylighting: the sky occlusion map could not be published; it is a D3D11 texture");
		return nullptr;
	}
	// The twin keeps the image, the wrapper keeps the D3D11 texture and its views.
	auto* d3d11 = texture->D3D11();
	d3d11->AddRef();
	auto* wrapper = new Texture2D(d3d11, kOcclusionId.data());
	wrapper->srv.copy_from(texture->SRV());
	wrapper->dsv.copy_from(texture->DSV());
	twin = std::move(texture);
	OcclusionDsvFormat() = a_dsvFormat;
	logger::info("[ORG] Skylighting: the sky occlusion map is a graph texture ({}x{}), published as {}", desc.Width, desc.Height, kOcclusionId);
	return wrapper;
}

std::shared_ptr<org::ExternalTextureResource> ORGSkylighting::OcclusionMap() const
{
	const auto& twin = OcclusionTwin();
	return twin && RenderGraphRuntime::Get().IsActive() ? std::dynamic_pointer_cast<org::ExternalTextureResource>(twin->Graph()) : nullptr;
}

bool ORGSkylighting::Update(const Skylighting& a_feature)
{
	if (!IsActive())
		return false;
	auto& state = *impl->state;
	// Native mode: until a run with the clear has executed, the map holds unwritten memory, so the update waits.
	const bool native = RenderGraph::Engine::Native::Active();
	// Without an input the gate stays closed and the volumes hold this frame.
	if (impl->ResolveInputs(MakeInputs(a_feature)) && (!native || state.mapCleared))
		state.pending = MakeShadows(a_feature);
	if (native) {
		state.clearOcclusion = impl->resources->clearProgram != nullptr;
		return true;
	}
	// Clear the map DCLF drew after the update reads it, when the schedule holds the clear.
	if (state.occlusionDrawn && impl->resources->clearProgram && FrameAnchors::Scheduled(kClientId, FrameAnchors::Anchor::Prepass)) {
		state.occlusionDrawn = false;
		state.occlusionCleared = false;
		state.clearOcclusion = true;
	}
	return true;
}

void ORGSkylighting::NoteOcclusionDrawn()
{
	if (IsActive())
		impl->state->occlusionDrawn = true;
}

bool ORGSkylighting::TakeOcclusionCleared()
{
	return IsActive() && std::exchange(impl->state->occlusionCleared, false);
}

bool ORGSkylighting::IsActive() const
{
	return impl && RenderGraphRuntime::Get().IsActive();
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

// Without OpenRenderGraph the probe update always runs on D3D11.
struct ORGSkylighting::Impl
{};

ORGSkylighting& ORGSkylighting::Get()
{
	static ORGSkylighting s_skylighting;
	return s_skylighting;
}

ORGSkylighting::~ORGSkylighting() = default;
void ORGSkylighting::Reserve() {}
bool ORGSkylighting::Setup(const Skylighting&) { return false; }
Texture2D* ORGSkylighting::MakeOcclusionMap(const D3D11_TEXTURE2D_DESC&, DXGI_FORMAT, DXGI_FORMAT) { return nullptr; }
std::shared_ptr<org::ExternalTextureResource> ORGSkylighting::OcclusionMap() const { return nullptr; }
bool ORGSkylighting::Update(const Skylighting&) { return false; }
void ORGSkylighting::NoteOcclusionDrawn() {}
bool ORGSkylighting::TakeOcclusionCleared() { return false; }
bool ORGSkylighting::IsActive() const { return false; }
ID3D11ShaderResourceView* ORGSkylighting::SRV(Volume) const { return nullptr; }
bool ORGSkylighting::QueueReset() { return false; }

#endif
