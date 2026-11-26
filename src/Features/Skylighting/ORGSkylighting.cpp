#include "ORGSkylighting.h"
#include "Snapshot/EngineReads.h"

#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/FrameClock.h"

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Buffer.h"
#include "Deferred.h"
#include "Features/Skylighting.h"
#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/D3D11Wrapper.h"
#include "RenderGraph/Engine/Targets/TargetRef.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"
#include "State.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>
#include <rhi_helpers.h>

#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <optional>
#include <type_traits>
#include <utility>

namespace
{
	constexpr const char* kClientId = "cs.skylighting";
	constexpr const char* kShader = "Skylighting/UpdateProbesCS.hlsl";
	// The engine skeleton's clear shader: depth 1.0 from the vertex stage and an Always test.
	constexpr const char* kClearShader = "RenderGraph/EngineClear.hlsl";
	constexpr auto kVolumes = static_cast<std::size_t>(ORGSkylighting::Volume::Count);
	// The probe update, the occlusion map's clear and the zenith capture's copy. A reset is a new version of the volumes, born
	// in the reset state on the revision lane, so it has no pass.
	constexpr std::uint32_t kUpdateEntry = 0;
	constexpr std::uint32_t kClearEntry = 1;
	constexpr std::uint32_t kZenithEntry = 2;
	constexpr std::uint32_t kGateEntries = 3;

	/** @brief Null outside graph mode, where Skylighting makes a D3D11 one. */
	std::unique_ptr<GraphResources::Texture2D>& OcclusionTwin()
	{
		static std::unique_ptr<GraphResources::Texture2D> s_twin;
		return s_twin;
	}

	/** @brief The zenith capture, copied from the occlusion map on zenith frames. Null without the occlusion twin. */
	std::unique_ptr<GraphResources::Texture2D>& ZenithTwin()
	{
		static std::unique_ptr<GraphResources::Texture2D> s_twin;
		return s_twin;
	}

	/** @brief The feature's D3D11 wrappers of the occlusion and zenith twins. */
	std::array<GraphResources::D3D11Wrapper<::Texture2D>, 2>& MapWrappers()
	{
		static std::array<GraphResources::D3D11Wrapper<::Texture2D>, 2> s_wrappers;
		return s_wrappers;
	}

	/** @brief For the clear program. */
	DXGI_FORMAT& OcclusionDsvFormat()
	{
		static DXGI_FORMAT s_format = DXGI_FORMAT_UNKNOWN;
		return s_format;
	}

	/** @brief The probe update's imports: the client's key. Another texture registers the client again with the next revision. */
	struct Imports
	{
		FrameAnchors::Texture occlusion, shadowCascades, esramShadows;

		explicit operator bool() const { return occlusion && shadowCascades && esramShadows; }
		bool operator==(const Imports&) const = default;
	};

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
		return inputs;
	}

	// In list order.
	struct UpdateProbesBlock
	{
		SharedConstants::Handles shared;
		ShaderBindings::Handle occlusion, shadowCascades, esramShadows, zenith;
		ShaderBindings::Handle volumes[kVolumes];
		ShaderBindings::Handle comparisonSampler;
		ShaderBindings::Latch shadows;
	};

	constexpr std::uint32_t kGroupX = 8, kGroupY = 8;

	/** @brief Made at Setup and kept. The pass copies what it uses when it is made. */
	struct Resources
	{
		std::uint32_t groups[3]{};
		RenderGraph::Programs::Fixed<ComputeProgram> program;
		org::OwnedDescriptorBinding sampler;
		// Declared on the reset count: a reset is a new version of each, adopted with its revision.
		std::array<RenderGraph::Frame::DeclaredTexture3D, kVolumes> volumes;
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
		FrameAnchors::Texture zenith;
		// Only for the twin. Empty keeps the D3D11 clear.
		RenderGraph::Programs::Fixed<RasterProgram> clearProgram;
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
		// This frame's map is the zenith capture, so the next run copies it before the update.
		bool copyZenith = false;
		// Native mode: a run with the clear executed, so the map holds no unwritten memory and the update may read it.
		bool mapCleared = false;
		// A commit ran, so the frame's graph holds the client. The zenith copy and the map's clear are in it from then on.
		bool committed = false;
	};

	/**
	* @brief The shadow cascades' view the last shadow mask draw bound, as the last Update saw it (B2's interim): an engine depth
	* target, or a view that is none, which the key imports.
	*/
	struct CascadesSeen
	{
		std::uint32_t depth = UINT32_MAX;  // the engine depth target, UINT32_MAX for none
		winrt::com_ptr<ID3D11ShaderResourceView> other;

		bool operator==(const CascadesSeen& a_other) const { return depth == a_other.depth && other.get() == a_other.other.get(); }
	};

	struct ProbeBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken occlusion, shadowCascades, esramShadows, zenith;
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
		// At the build, on the render thread. The volumes are declared through their slots, so a new version needs no build.
		UpdateProbesPass(const Resources& a_resources, const Imports& a_imports) :
			occlusion(a_imports.occlusion), shadowCascades(a_imports.shadowCascades), esramShadows(a_imports.esramShadows), zenith(a_resources.zenith)
		{
			for (std::size_t i = 0; i < kVolumes; ++i)
				volumes[i] = RenderGraph::AnchorPasses::TextureOf(*a_resources.volumes[i]);
			frame.program = a_resources.program.Get();
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
			bindings.zenith = a_builder.ShaderResource(zenith).View();
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
			block.zenith = a_recording.Index(a_bindings.zenith);
			for (std::size_t i = 0; i < kVolumes; ++i)
				block.volumes[i] = a_recording.Index(a_bindings.volumes[i]);
			block.comparisonSampler = a_frame.sampler;
			block.shadows = ShaderBindings::LatchOf(*a_frame.latch, a_recording.Run());
			a_frame.gate->Dispatch(a_recording, *a_frame.program, ShaderBindings::Data(block), kUpdateEntry);
		}

	private:
		FrameAnchors::Texture occlusion, shadowCascades, esramShadows, zenith;
		std::array<FrameAnchors::Texture, kVolumes> volumes;
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
			frame.program = a_resources.clearProgram.Get();
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

	/**
	* @brief Which engine depth target a view is, or UINT32_MAX. A field handle is identified by the target set. A view the shadow
	* mask draw bound may be DXVK's own object, which the set cannot identify, so it is compared by what the fields wrap.
	*/
	std::uint32_t EngineDepthOf(ID3D11ShaderResourceView* a_srv)
	{
		namespace Tripwire = RenderGraph::Engine::Tripwire;
		namespace Targets = RenderGraph::Engine::Targets;
		if (!a_srv)
			return UINT32_MAX;
		if (Tripwire::IsHandle(a_srv)) {
			const auto target = Targets::Identify(static_cast<const ID3D11View*>(a_srv));
			return target && target->kind == Targets::Kind::Depth ? target->index : UINT32_MAX;
		}
		winrt::com_ptr<ID3D11Resource> resource;
		a_srv->GetResource(resource.put());
		if (!resource)
			return UINT32_MAX;
		const auto& depthStencils = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetDepthStencilData().depthStencils;
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); ++i)
			if (depthStencils[i].texture && Tripwire::Unwrap(depthStencils[i].texture) == Tripwire::Unwrap(static_cast<ID3D11Texture2D*>(resource.get())))
				return i;
		return UINT32_MAX;
	}

	/** @brief A shadow map view that is no engine target, imported as its own. */
	FrameAnchors::Texture ImportShadowMap(ID3D11ShaderResourceView* a_srv, const char* a_id)
	{
		if (!a_srv)
			return nullptr;
		winrt::com_ptr<ID3D11Resource> resource;
		a_srv->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture)
			return nullptr;
		RE::BSGraphics::DepthStencilData target{};
		target.texture = texture.get();
		target.depthSRV = a_srv;
		return FrameAnchors::ImportTexture(a_id, { .depth = &target });
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
	RenderGraph::Frame::DeclaredClient client;
	// The client's key reads it. The occlusion map is the graph's twin and the ESRAM shadows an engine target, both stable.
	RenderGraph::Frame::Observed<CascadesSeen> cascades{ "Skylighting.ShadowCascades" };

	/** @brief The imports for what a revision carries. Empty fields when one is missing. */
	static Imports ImportsFor(const CascadesSeen& a_cascades)
	{
		FrameAnchors::Texture occlusion = ORGSkylighting::Get().OcclusionMap();
		if (!occlusion) {
			// Outside graph mode's own map: the feature's D3D11 map, made once.
			const auto inputs = MakeInputs(globals::features::skylighting);
			RE::BSGraphics::DepthStencilData occlusionTarget{};
			occlusionTarget.texture = inputs.occlusion;
			occlusionTarget.views[0] = inputs.occlusionDSV;
			occlusionTarget.depthSRV = inputs.occlusionSRV;
			occlusion = FrameAnchors::ImportTexture("cs.skylighting.occlusion", { .depth = &occlusionTarget });
		}
		auto shadowCascades = a_cascades.depth != UINT32_MAX ? FrameAnchors::EngineTexture(FrameAnchors::Depth(a_cascades.depth)) :
		                                                        ImportShadowMap(a_cascades.other.get(), "cs.skylighting.shadow-cascades");
		auto esramShadows = FrameAnchors::EngineTexture(FrameAnchors::Depth(RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM));
		return Imports{ std::move(occlusion), std::move(shadowCascades), std::move(esramShadows) };
	}

	/** @brief Notes this frame's shadow cascades. False when an input is missing. A change registers the client again with the next revision. */
	bool ResolveInputs(const Inputs& a_inputs)
	{
		CascadesSeen seen;
		seen.depth = EngineDepthOf(a_inputs.shadowCascades);
		if (seen.depth == UINT32_MAX)
			seen.other.copy_from(a_inputs.shadowCascades);
		if (!a_inputs.shadowCascades)
			return false;
		cascades.Observe(seen);
		return static_cast<bool>(ImportsFor(seen));
	}

	/**
	* @brief The registration for a_imports: the zenith copy, the gated probe update and the occlusion map's clear. False, logged,
	* when a step cannot be made, which the declaration makes fatal.
	*/
	bool MakeClient(const Imports& a_imports, FrameAnchors::ClientDesc& a_client) const
	{
		namespace AnchorPasses = RenderGraph::AnchorPasses;
		// The zenith copy, then the update at -1, before IBL. The clear at 0 touches nothing IBL uses.
		AnchorPasses::Builder builder(FrameAnchors::Anchor::Prepass, kClientId, -2, {}, resources->gate);
		// The zenith frame's map, copied before the update reads it and before the clear empties it. A gate, not a revision.
		const auto& occlusionTwin = *OcclusionTwin();
		auto zenithCopy = AnchorPasses::CopyOf("copy-zenith", occlusionTwin, *ZenithTwin(), occlusionTwin.Desc().Width, occlusionTwin.Desc().Height);
		zenithCopy.gate = kZenithEntry;
		builder.Add(std::move(zenithCopy));
		builder.Add("update-probes", FrameAnchors::PassKind::Compute, [resources = resources, a_imports]() -> std::shared_ptr<org::RenderPass> {
			return std::make_shared<UpdateProbesPass>(*resources, a_imports);
		});  // counts from the gate's update entry
		// Left out without a twin or program.
		builder.Add("clear-occlusion", FrameAnchors::PassKind::Render, [resources = resources]() -> std::shared_ptr<org::RenderPass> {
			auto map = ORGSkylighting::Get().OcclusionMap();
			if (!map || !resources->clearProgram)
				return nullptr;
			return std::make_shared<OcclusionClearPass>(std::move(map), *resources);
		});  // count from the gate's clear entry, depth loaded
		if (builder.Failed()) {
			logger::critical("[ORG] Skylighting: its client could not be made: {}", builder.Failure());
			return false;
		}

		a_client.id = kClientId;
		a_client.passes = builder.Take();
		a_client.sharedConstants = true;
		a_client.gates = { resources->gate };
		a_client.registerResources = [resources = resources](org::RenderGraph& a_graph) {
			for (const auto& volume : resources->volumes)
				volume->Register(a_graph);
		};
		// Every run closes the gate first, so other frames dispatch and draw nothing.
		a_client.active = [state = state] { return state->pending.has_value() || state->clearOcclusion || state->copyZenith; };
		a_client.commit = [state = state, resources = resources](const FrameAnchors::Point&) {
			state->committed = true;
			if (std::exchange(state->copyZenith, false))
				resources->gate->OpenDraw(kZenithEntry);
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
		logger::info("[ORG] Skylighting: a client for the next revision (occlusion {}, shadow cascades {}, ESRAM shadows {})", a_imports.occlusion.Id(),
			a_imports.shadowCascades.Id(), a_imports.esramShadows.Id());
		return true;
	}
};

ORGSkylighting::ORGSkylighting() = default;
ORGSkylighting::~ORGSkylighting() = default;

bool ORGSkylighting::Setup(const Skylighting& a_feature)
{
	impl.reset();
	if (!On())
		return false;
	SetupHost();

	const auto& dims = a_feature.probeArrayDims;
	auto next = std::make_unique<Impl>();
	next->state = std::make_shared<ClientState>();
	auto resources = std::make_shared<Resources>();
	resources->groups[0] = (dims[0] + kGroupX - 1) / kGroupX;
	resources->groups[1] = (dims[1] + kGroupY - 1) / kGroupY;
	resources->groups[2] = dims[2];
	resources->program = RenderGraph::Programs::Queue("Skylighting", ComputeProgram::Desc{ .source = kShader, .constantWords = ShaderBindings::Words<UpdateProbesBlock>() });
	resources->sampler = ShaderBindings::CreateSampler({ .compareEnable = true, .compareOp = rhi::CompareOp::LessEqual, .reduction = rhi::ReductionMode::Comparison });
	resources->latch = std::make_shared<org::LatchBlock>("cs.skylighting.latch", static_cast<std::uint32_t>(sizeof(Shadows)));
	resources->gate = RenderGraph::RunGate::Create("cs.skylighting.gate", kGateEntries);
	if (!resources->sampler)
		Fatal("the probe update's sampler could not be made");
	// The zenith capture is copied on the graph, so both maps must be graph textures.
	if (!OcclusionTwin() || !ZenithTwin())
		Fatal("the occlusion or zenith map is not a graph texture");
	resources->zenith = RenderGraph::AnchorPasses::TextureOf(*ZenithTwin());
	if (OcclusionTwin() && OcclusionDsvFormat() != DXGI_FORMAT_UNKNOWN) {
		RasterProgram::Desc clearDesc{ .pixelSource = kClearShader, .vertexSource = kClearShader, .vertexEntry = L"VSMain", .commonDefines = false };
		clearDesc.pixelEntry = L"PSDepth";
		clearDesc.depthFormat = rhi::helpers::ToRHI(OcclusionDsvFormat());
		clearDesc.depth = { .depthEnable = true, .depthWrite = true, .depthFunc = rhi::CompareOp::Always };
		resources->clearProgram = RenderGraph::Programs::Queue("Skylighting", std::move(clearDesc));
	}
	// D3D11 reads them, so the SRV is exported. Every version, the first and each reset's, is born in ResetSkylighting's state on
	// the revision lane: unit SH (fully unoccluded), no accumulated frames, all 32 history bits lit so a reset does not fade in,
	// full visibility, unknown coverage.
	namespace RF = RenderGraph::Frame;
	const float unitSH = std::sqrt(4.0f * std::numbers::pi_v<float>);
	const RF::Birth allBitsLit = RF::Birth::Fill([](std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t a_rows, std::uint32_t a_depth,
													 std::uint32_t a_rowPitch, std::byte* a_texels) {
		std::memset(a_texels, 0xFF, static_cast<std::size_t>(a_rowPitch) * a_rows * a_depth);
		return true;
	});
	const std::pair<RF::Birth, std::string_view> volumeBirth[kVolumes] = {
		{ RF::Birth::Value({ unitSH, 0.0f, 0.0f, 0.0f }), "probes" },
		{ RF::Birth::Zero(), "accumulated frames" },
		{ allBitsLit, "shadow bitmask" },
		{ RF::Birth::Value({ 1.0f, 1.0f, 1.0f, 1.0f }), "shadow visibility" },
		{ RF::Birth::Value({ static_cast<float>(Skylighting::COVERAGE_UNKNOWN), 0.0f, 0.0f, 0.0f }), "coverage" },
	};
	constexpr std::pair<DXGI_FORMAT, std::string_view> volumeInfo[kVolumes] = {
		{ DXGI_FORMAT_R16G16B16A16_FLOAT, kProbeArrayId },
		{ DXGI_FORMAT_R8_UINT, kAccumFramesId },
		{ DXGI_FORMAT_R32_UINT, kShadowBitmaskId },
		{ DXGI_FORMAT_R8_UNORM, kShadowVisibilityId },
		{ DXGI_FORMAT_R8_UINT, kCoverageId },
	};
	// Declared on the reset count (Skylighting::resets): each reset is a new version of every volume, adopted with its revision.
	const auto& resets = a_feature.resets;
	const std::array<std::uint32_t, 3> extent{ dims[0], dims[1], dims[2] };
	for (std::size_t i = 0; i < kVolumes; ++i) {
		const DXGI_FORMAT format = volumeInfo[i].first;
		GraphResources::Options volumeOptions{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV };
		volumeOptions.birth = volumeBirth[i].first;
		resources->volumes[i] = RF::DeclareTexture3D(
			std::string(volumeInfo[i].second), RF::DependsOn(resets),
			[extent, format](const RF::Inputs&) {
				const std::uint32_t size[3]{ extent[0], extent[1], extent[2] };
				return VolumeDesc(size, format);
			},
			volumeOptions, [&resets](const RF::Inputs& a_in) -> std::uint64_t { return a_in.Get(resets); });
		const auto& volume = resources->volumes[i];
		if (!volume || !volume->SRV())
			Fatal(fmt::format("{} could not be made and exported", volumeInfo[i].second));
	}
	next->resources = resources;

	// DCLF's colour passes read them by name, ordered after the update. Published once, for the session: readers follow the versions.
	if (!PublishedResources::Publish(*resources->volumes[static_cast<std::size_t>(Volume::ProbeArray)]) ||
		!PublishedResources::Publish(*resources->volumes[static_cast<std::size_t>(Volume::ShadowVisibility)]) ||
		!PublishedResources::Publish(*resources->volumes[static_cast<std::size_t>(Volume::Coverage)]))
		Fatal("the probe volumes could not be published");
	// The client follows the shadow cascades the last Update found (B2's interim). Before the first, there is none.
	auto* made = next.get();
	next->client = RF::DeclareClient<Imports>(
		kClientId, RF::DependsOn(next->cascades.Var()),
		[made](const RF::Inputs& a_in) -> std::optional<Imports> {
			const auto* seen = made->cascades.Get(a_in);
			if (!seen)
				return std::nullopt;
			auto imports = Impl::ImportsFor(*seen);
			if (!imports)
				return std::nullopt;
			return imports;
		},
		[made](const Imports& a_imports, FrameAnchors::ClientDesc& a_client) { return made->MakeClient(a_imports, a_client); });
	impl = std::move(next);
	logger::info("[ORG] Skylighting: the probe update runs on the render graph at the Prepass anchor ({}x{}x{}); its client and volumes follow the frame revisions",
		dims[0], dims[1], dims[2]);
	return true;
}

Texture2D* ORGSkylighting::MakeOcclusionMap(const D3D11_TEXTURE2D_DESC& a_desc, DXGI_FORMAT a_srvFormat, DXGI_FORMAT a_dsvFormat)
{
	auto& twin = OcclusionTwin();
	if (twin || !On())
		return nullptr;
	if (!RenderGraphRuntime::Get().IsActive())
		Fatal("the render graph is not available for the sky occlusion map");
	// Written and read by D3D11, so exported and never aliased. Read in the frame after one that renders no map.
	D3D11_TEXTURE2D_DESC desc = a_desc;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = 0;
	auto texture = std::make_unique<GraphResources::Texture2D>(desc, std::string(kOcclusionId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::DSV });
	if (!texture->D3D11() || !texture->SRV() || !texture->DSV() || !std::dynamic_pointer_cast<org::ExternalTextureResource>(texture->Graph()))
		Fatal(fmt::format("the sky occlusion map could not be made on the render graph (format {})", static_cast<int>(a_desc.Format)));
	// The engine's map draws and the precipitation shaders use views in the target's formats.
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	texture->SRV()->GetDesc(&srvDesc);
	texture->DSV()->GetDesc(&dsvDesc);
	if (srvDesc.Format != a_srvFormat || dsvDesc.Format != a_dsvFormat)
		Fatal(fmt::format("the graph's sky occlusion map has views in formats {}/{}, the target's are {}/{}", static_cast<int>(srvDesc.Format),
			static_cast<int>(dsvDesc.Format), static_cast<int>(a_srvFormat), static_cast<int>(a_dsvFormat)));
	// Published so DCLF's shadow epoch and the probe update share one resource.
	if (!PublishedResources::Publish(*texture))
		Fatal("the sky occlusion map could not be published");
	// The twin keeps the image. The wrapper keeps the D3D11 texture and its views, and follows the twin.
	auto wrapper = GraphResources::MakeWrapper(*texture, kOcclusionId.data());
	MapWrappers()[0] = wrapper;
	twin = std::move(texture);
	OcclusionDsvFormat() = a_dsvFormat;
	logger::info("[ORG] Skylighting: the sky occlusion map is a graph texture ({}x{}), published as {}", desc.Width, desc.Height, kOcclusionId);
	return wrapper.Get();
}

Texture2D* ORGSkylighting::MakeZenithMap()
{
	const auto& occlusion = OcclusionTwin();
	auto& twin = ZenithTwin();
	if (twin || !occlusion)
		return nullptr;
	// The occlusion map's twin, so the copy keeps depth layouts. The DSV export serves the D3D11 probe update's own capture.
	auto texture = std::make_unique<GraphResources::Texture2D>(occlusion->Desc(), std::string(kZenithId),
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::DSV });
	if (!texture->D3D11() || !texture->SRV() || !texture->DSV())
		Fatal("the zenith map could not be made on the render graph");
	// Published so DCLF's colour passes and the composite read it after the copy.
	if (!PublishedResources::Publish(*texture))
		Fatal("the zenith map could not be published");
	auto wrapper = GraphResources::MakeWrapper(*texture, kZenithId.data());
	MapWrappers()[1] = wrapper;
	twin = std::move(texture);
	logger::info("[ORG] Skylighting: the zenith map is a graph texture, published as {}", kZenithId);
	return wrapper.Get();
}

bool ORGSkylighting::ZenithReady() const
{
	// Native mode: the map holds unwritten memory until a run with the clear has executed.
	// The copy is in the frame's graph once a commit of the client ran: an adopted client stays in it.
	return IsActive() && (!RenderGraph::Engine::Native::Active() || impl->state->mapCleared) && impl->state->committed;
}

std::shared_ptr<org::ExternalTextureResource> ORGSkylighting::OcclusionMap() const
{
	const auto& twin = OcclusionTwin();
	return twin ? std::dynamic_pointer_cast<org::ExternalTextureResource>(twin->Graph()) : nullptr;
}

bool ORGSkylighting::Update(const Skylighting& a_feature)
{
	if (!IsActive())
		return false;
	auto& state = *impl->state;
	// A reset is a new version of the volumes, adopted with the reset count (QueueReset) and born in the reset state.
	// Native mode: until a run with the clear has executed, the map holds unwritten memory, so the update waits.
	const bool native = RenderGraph::Engine::Native::Active();
	// Without an input the gate stays closed and the volumes hold this frame.
	if (impl->ResolveInputs(MakeInputs(a_feature)) && (!native || state.mapCleared))
		state.pending = MakeShadows(a_feature);
	// ZenithReady held when this frame's capture was taken, so the map holds it.
	state.copyZenith = a_feature.zenithFrame && a_feature.occlusionFrame == RenderGraph::FrameClock::BeginFrame();
	if (native) {
		state.clearOcclusion = static_cast<bool>(impl->resources->clearProgram);
		return true;
	}
	// Clear the map DCLF drew after the update reads it, once the frame's graph holds the clear.
	if (state.occlusionDrawn && impl->resources->clearProgram && state.committed) {
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

ID3D11ShaderResourceView* ORGSkylighting::SRV(Volume a_volume) const
{
	return IsActive() && a_volume < Volume::Count ? impl->resources->volumes[static_cast<std::size_t>(a_volume)]->SRV() : nullptr;
}

bool ORGSkylighting::QueueReset(Skylighting& a_feature)
{
	if (!IsActive())
		return false;
	// A new reset count: the next revision makes new versions of the volumes, and their first run clears them.
	a_feature.resets = a_feature.resets.Get() + 1u;
	logger::info("[ORG] Skylighting: the probe volumes reset on the render graph (reset {}: new versions with the next revision)", a_feature.resets.Get());
	return true;
}

