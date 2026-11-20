#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Features/PhysicalSky/ORGPhysicalSky.h"
#include "RenderGraph/Engine/Native/Native.h"

bool ORGPhysicalSky::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
	return requested;
}

ORGPhysicalSky& ORGPhysicalSky::Get()
{
	static ORGPhysicalSky s_physicalSky;
	return s_physicalSky;
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Buffer.h"
#	include "Deferred.h"
#	include "Features/CloudShadows.h"
#	include "Features/CloudShadows/ORGCloudShadows.h"
#	include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#	include "Features/DynamicCubemaps.h"
#	include "Features/PhysicalSky.h"
#	include "Features/PhysicalSky/ORGVolumetricClouds.h"
#	include "Features/TerrainShadows.h"
#	include "Features/TerrainShadows/ORGTerrainShadows.h"
#	include "Features/ViewAtmosphere.h"
#	include "Globals.h"
#	include "ORGDeferredComposite.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/Frame/Declare.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/UavClearPass.h"
#	include "State.h"
#	include "Utils/Game.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>

#	include <algorithm>
#	include <cstddef>
#	include <format>
#	include <optional>
#	include <span>
#	include <string>
#	include <type_traits>
#	include <utility>

namespace
{
	constexpr const char* kLutsClientId = "cs.physical-sky.luts";
	constexpr const char* kPrepassClientId = "cs.physical-sky";
	constexpr const char* kCompositeClientId = "cs.physical-sky.composite";
	constexpr const char* kDeferredClientId = "cs.deferred";
	constexpr const char* kLutShader = "PhysicalSky/LutGen.cs.hlsl";
	constexpr const char* kDiskPhaseShader = "PhysicalSky/DiskPhaseLut.cs.hlsl";
	constexpr const char* kShadowAccumShader = "PhysicalSky/ShadowAccum.cs.hlsl";
	constexpr const char* kApplyApShader = "PhysicalSky/ApplyAerialPerspectiveCS.hlsl";

	// The Prepass gate's entries: the full and the half resolution AP shadow, the sky tail, then the AP shadow's clear.
	constexpr std::uint32_t kShadowFullEntry = 0;
	constexpr std::uint32_t kShadowHalfEntry = 1;
	constexpr std::uint32_t kSkyTailEntry = 2;
	constexpr std::uint32_t kShadowClearEntry = 3;
	constexpr std::uint32_t kPrepassEntries = 4;
	// ShadowAccum's push block: the shared handles, depth, shadow map, light latch (two), heights, cloud shadow, shadow volume,
	// output and sampler.
	constexpr std::uint32_t kShadowAccumWords = 4 + 1 + 1 + 2 + 1 + 1 + 1 + 1 + 1;
	// LutGen's SKY_TAIL: LutGen's, then the cloud shadow volume, its mean and the cloud diffuse transport.
	constexpr std::uint32_t kSkyTailWords = 4 + 3 + 1 + 3 + 3;
	// ApplyAerialPerspectiveCS's: the shared handles, kMAIN, depth, five Physical Sky textures and SampSv.
	constexpr std::uint32_t kApplyApWords = 4 + 1 + 1 + 5 + 1;
	// Before the fog, which reads none of it, so only the order key decides.
	constexpr std::int32_t kPrepassOrder = 200;
	// After cs.deferred's composite (order 0). Both write kMAIN, so the graph orders them anyway.
	constexpr std::int32_t kCompositeOrder = 10;

	/**
	* @brief The AP shadow pass's latch (PhysicalSky/ApShadow.hlsli). The light's bytes are those D3D11's structured buffer holds,
	* then the light count and the clear flag ShadowAccum's ORG build reads.
	*/
	struct ShadowLatch
	{
		Deferred::DirectionalShadowLightData light{};
		std::uint32_t lightCount = 0;
		std::uint32_t clear = 0;
		std::uint32_t pad[2]{};
	};
	static_assert(sizeof(Deferred::DirectionalShadowLightData) == 272 && offsetof(ShadowLatch, lightCount) == 272 && sizeof(ShadowLatch) == 288,
		"PhysicalSky/ApShadow.hlsli's latch layout");

	/** @brief What the AP shadow and the sky tail read besides their own textures: the Prepass client's key. */
	struct ShadowInputs
	{
		FrameAnchors::Texture shadowMap;  // null when the frame drew no shadow mask
		std::string terrain;              // Terrain Shadows' published heights, empty for none
		std::string cloud;                // Cloud Shadows' last layer, published, empty for none
		bool clouds = false;              // the volumetric clouds' shadow volume and mean, read by their ids

		bool operator==(const ShadowInputs&) const = default;
	};

	/** @brief What the AP composite reads: the composite client's key. */
	struct CompositeInputs
	{
		std::uint32_t main = 0;  // the engine target the graph composite writes
		bool depth = false;      // with Dynamic Cubemaps only, as the D3D11 composite binds it
		bool clouds = false;     // the clouds' reprojected targets and lightning channels, read by their ids

		bool operator==(const CompositeInputs&) const = default;
	};

	/** @brief The engine's depth-stencil import when it is one of the engine's, else its own. As the fog imports its shadow map. */
	FrameAnchors::Texture PhysSkyImportShadowMap(ID3D11ShaderResourceView* a_view, const char* a_id)
	{
		if (!a_view || !globals::game::renderer)
			return nullptr;
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
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
		target.depthSRV = a_view;
		return FrameAnchors::ImportDepth(target, a_id);
	}

	/**
	* @brief Whether this frame's readers bind the volumetric clouds' outputs: the clouds are on, and their targets are the
	* published twins. The cloud client clears the bound versions to neutral before they count as written.
	*/
	bool CloudsOnGraph(const PhysicalSky& a_feature)
	{
		// Nothing may read them before a run of the cloud client wrote the bound versions.
		if (!a_feature.VolumetricCloudsEnabled() || !a_feature.texShadowVolume || !a_feature.texVolTr || !ORGVolumetricClouds::OutputsWritten())
			return false;
		const bool published = PublishedResources::IsExportOf(a_feature.texShadowVolume->srv.get(), ORGVolumetricClouds::kShadowVolumeId) &&
		                       PublishedResources::IsExportOf(a_feature.texVolTr->srv.get(), ORGVolumetricClouds::kVolTrCurrentId);
		if (!published) {
			static bool s_logged = false;
			if (!std::exchange(s_logged, true))
				logger::error("[ORG] Physical Sky: the volumetric clouds' targets are not the graph's; their readers see no clouds");
		}
		return published;
	}

	/** @brief Fatal: a graph input of Physical Sky is missing. */
	void PhysSkyFail(std::string_view a_why)
	{
		const auto message = std::format("Community Shaders: Physical Sky on the render graph: {}", a_why);
		logger::critical("[ORG] {}", message);
		stl::report_and_fail(message);
	}

	/** @brief Points a D3D11 wrapper at the twin's bound version. True when it was another one. */
	bool PhysSkyFollowTwin(::Texture2D& a_wrapper, const GraphResources::Texture2D& a_twin)
	{
		auto* d3d11 = a_twin.D3D11();
		if (!d3d11 || a_wrapper.resource.get() == d3d11)
			return false;
		a_wrapper.resource.copy_from(d3d11);
		d3d11->GetDesc(&a_wrapper.desc);
		a_wrapper.srv.copy_from(a_twin.SRV());
		a_wrapper.uav.copy_from(a_twin.UAV());
		a_wrapper.rtv.copy_from(a_twin.RTV());
		return true;
	}

	using Lut = ORGPhysicalSky::Lut;
	constexpr auto kLutCount = static_cast<std::size_t>(Lut::Count);
	// Gate entries: the four LutGen dispatches in LUTGEN order, then the disk phase LUT.
	constexpr std::uint32_t kLutGenEntries = 4;
	constexpr std::uint32_t kDiskPhaseEntry = 4;
	// The disk phase LUT's 12 floats (DiskPhaseFrame), at the latch's start.
	constexpr std::uint32_t kDiskPhaseBytes = 12 * sizeof(float);
	// LutGen's push block: the shared handles, three SRVs, the output and three samplers.
	constexpr std::uint32_t kLutGenWords = 4 + 3 + 1 + 3;
	constexpr std::uint32_t kDiskPhaseWords = 1 + 2;

	constexpr std::string_view IdOf(Lut a_lut)
	{
		switch (a_lut) {
		case Lut::Transmittance:
			return ORGPhysicalSky::kTransmittanceLutId;
		case Lut::Multiscatter:
			return ORGPhysicalSky::kMultiscatterLutId;
		case Lut::SkyView:
			return ORGPhysicalSky::kSkyViewLutId;
		case Lut::AerialPerspective:
			return ORGPhysicalSky::kApLutId;
		case Lut::DiskPhase:
			return ORGPhysicalSky::kDiskPhaseLutId;
		default:
			return {};
		}
	}

	/** @brief The 2D LUTs by Lut, empty for the 3D AP LUT or a D3D11 texture. Declared with no dependencies: one version each. */
	std::array<RenderGraph::Frame::DeclaredTexture2D, kLutCount>& Twins2D()
	{
		static std::array<RenderGraph::Frame::DeclaredTexture2D, kLutCount> s_twins;
		return s_twins;
	}

	RenderGraph::Frame::DeclaredTexture3D& ApTwin()
	{
		static RenderGraph::Frame::DeclaredTexture3D s_twin;
		return s_twin;
	}

	/** @brief The AP shadow, declared on the display size. History: the next frame's reflection faces read it. */
	RenderGraph::Frame::DeclaredTexture2D& ApShadowTwin()
	{
		static RenderGraph::Frame::DeclaredTexture2D s_twin;
		return s_twin;
	}

	/** @brief The sky tail's twin, the sky-view LUT's fixed shape. History: the next frame's reflection faces read it. */
	std::unique_ptr<GraphResources::Texture2D>& SkyTailTwin()
	{
		static std::unique_ptr<GraphResources::Texture2D> s_twin;
		return s_twin;
	}

	const GraphResources::Resource* TwinOf(Lut a_lut)
	{
		if (a_lut == Lut::AerialPerspective)
			return ApTwin().Get();
		return Twins2D()[static_cast<std::size_t>(a_lut)].Get();
	}

	// D3D11 writes them only on frames the graph does not, read them always, and the next frame's reflection faces read them.
	GraphResources::Options LutOptions(Lut a_lut)
	{
		return { .lifetime = a_lut == Lut::DiskPhase ? GraphResources::Lifetime::Persistent : GraphResources::Lifetime::History,
			.d3d11 = GraphResources::Export::SRV | GraphResources::Export::UAV };
	}

	/** @brief LutGen's group counts, as PhysicalSky::GenerateLuts dispatches them. */
	std::array<std::uint32_t, 3> LutGroups(Lut a_lut)
	{
		switch (a_lut) {
		case Lut::Transmittance:
			return { (PhysicalSky::kTrLutW + 7u) >> 3, (PhysicalSky::kTrLutH + 7u) >> 3, 1 };
		case Lut::Multiscatter:
			// Each group integrates sixteen directions for four LUT texels.
			return { (PhysicalSky::kMsLutW + 1u) >> 1, (PhysicalSky::kMsLutH + 1u) >> 1, 1 };
		case Lut::SkyView:
			return { (PhysicalSky::kSvLutW + 7u) >> 3, (PhysicalSky::kSvLutH + 7u) >> 3, 1 };
		case Lut::AerialPerspective:
			// Two rays per group; all depth intervals are evaluated in parallel.
			return { (PhysicalSky::kApLutW + 1u) >> 1, PhysicalSky::kApLutH, 1 };
		default:
			return { 1, 1, 1 };
		}
	}

	// A template, so a 2D twin takes Publish's 2D overload.
	template <class Twin>
	bool PublishTwin(const Twin& a_twin, Lut a_lut)
	{
		if (PublishedResources::Publish(IdOf(a_lut), a_twin))
			return true;
		logger::error("[ORG] Physical Sky: {} could not be published; it is a D3D11 texture", IdOf(a_lut));
		return false;
	}

	/** @brief The D3D11 sampler's description, for the graph's twin of it. */
	org::OwnedDescriptorBinding SamplerOf(ID3D11SamplerState* a_sampler)
	{
		if (!a_sampler)
			return {};
		D3D11_SAMPLER_DESC desc{};
		a_sampler->GetDesc(&desc);
		return ShaderBindings::CreateSamplerFromD3D11(desc);
	}
}

struct ORGPhysicalSky::Impl
{
	std::shared_ptr<RenderGraph::RunGate> gate;
	std::shared_ptr<org::LatchBlock> latch;
	std::array<std::shared_ptr<const ComputeProgram>, kLutGenEntries> lutPrograms;
	std::shared_ptr<const ComputeProgram> diskPhaseProgram;
	// SampTr, SampSv and SampNoise, kept as long as the programs.
	std::array<org::OwnedDescriptorBinding, 3> samplers;

	// Render thread. Set at the feature's EarlyPrepass, taken by the next commit.
	bool luts = false;
	std::optional<std::array<float, 12>> diskPhase;

	// The AP shadow (cs.physical-sky) and the AP composite (cs.physical-sky.composite). Null programs mean neither is on the graph.
	std::shared_ptr<const ComputeProgram> shadowFull, shadowHalf, applyAp, skyTail;
	std::shared_ptr<RenderGraph::RunGate> prepassGate, compositeGate;
	std::shared_ptr<org::LatchBlock> prepassLatch;
	// The null descriptor, for inputs D3D11 binds null: it reads zero and has zero size, as a null SRV does.
	std::uint32_t nullIndex = DCLF::GpuTextures::kInvalid;
	std::shared_ptr<const void> nullOwner;

	// The declared clients. The Prepass and composite keys add what the last Prepass found, which is not a variable yet:
	// a change invalidates the key, and the next revision registers the client for it.
	RenderGraph::Frame::DeclaredClient lutClient, prepassClient, compositeClient;
	std::optional<ShadowInputs> shadowObserved;
	std::optional<CompositeInputs> compositeObserved;
	// The composite registration the adopted revision holds. Its commit opens the pass only for that one's target.
	std::optional<CompositeInputs> compositeHeld;
	const void* shadowCleared = nullptr;  // the AP shadow version the last clear was opened for
	std::string inputNote;                // the last missing AP shadow input, logged when it changes
	std::uint32_t beganFrame = UINT32_MAX;

	// Render thread. Set at the feature's Prepass, taken by the next commit.
	std::optional<ShadowLatch> shadow;
	std::uint32_t shadowEntry = kShadowFullEntry;
	std::array<std::uint32_t, 2> shadowGroups{};
	std::uint32_t compositeFrame = UINT32_MAX;  // the frame StageComposite staged
	bool tail = false;                          // the sky tail

	void Commit()
	{
		if (std::exchange(luts, false))
			for (std::uint32_t i = 0; i < kLutGenEntries; ++i) {
				const auto groups = LutGroups(static_cast<Lut>(i));
				gate->OpenDispatch(i, groups[0], groups[1], groups[2]);
			}
		if (diskPhase) {
			latch->Write(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, std::as_bytes(std::span(*diskPhase)));
			gate->OpenDispatch(kDiskPhaseEntry, (PhysicalSky::kDiskPhaseLutSize + 63u) / 64u, 3, 1);
			diskPhase.reset();
		}
	}

	void CommitPrepass()
	{
		// A new version of the AP shadow is not born cleared (it is exported), so the run clears it first: no shadow.
		if (const void* version = ApShadowTwin()->Graph().get(); version != shadowCleared) {
			prepassGate->OpenDispatch(kShadowClearEntry, 1);
			shadowCleared = version;
		}
		if (std::exchange(tail, false))
			prepassGate->OpenDispatch(kSkyTailEntry, (PhysicalSky::kSvLutW + 7u) >> 3, (PhysicalSky::kSvLutH + 7u) >> 3, 1);
		if (!shadow)
			return;
		prepassLatch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, *shadow);
		prepassGate->OpenDispatch(shadowEntry, shadowGroups[0], shadowGroups[1], 1);
		shadow.reset();
	}

	/** @brief Opens the AP composite only when the graph composite writes kMAIN this frame, into the target the pass declares. */
	void CommitComposite()
	{
		if (compositeFrame != globals::state->frameCount || !compositeHeld)
			return;
		const auto frame = ORGDeferredComposite::Get().GraphComposite();
		// The D3D11 interior composite is built without PHYSICAL_SKY, so interiors get no AP there either.
		if (!frame || frame->interior || frame->main != compositeHeld->main)
			return;
		// A composite whose schedule does not hold it yet writes nothing this frame.
		if (!FrameAnchors::HeldNow(kDeferredClientId, FrameAnchors::Anchor::DeferredComposite).scheduled)
			return;
		const auto groups = Util::GetScreenDispatchCount(true);
		compositeGate->OpenDispatch(0, groups.x, groups.y, 1);
	}

	/** @brief The null descriptor. False, logged, when the device has none. */
	bool MakeNull()
	{
		if (nullIndex != DCLF::GpuTextures::kInvalid)
			return true;
		auto binding = DCLF::GpuTextures::Get().NullBinding();
		if (binding.index == DCLF::GpuTextures::kInvalid) {
			logger::error("[ORG] Physical Sky: no null descriptor for the inputs D3D11 binds null");
			return false;
		}
		nullIndex = binding.index;
		nullOwner = std::move(binding.owner);
		return true;
	}

	/** @brief The LUT client's registration. It has no dependencies, so it is made once, at the first revision. */
	bool MakeLutClient(FrameAnchors::ClientDesc& a_client, const std::shared_ptr<Impl>& a_self)
	{
		namespace AnchorPasses = RenderGraph::AnchorPasses;
		using Bind = AnchorPasses::Bind;
		const auto& tr = *TwinOf(Lut::Transmittance);
		const auto& ms = *TwinOf(Lut::Multiscatter);
		const auto& diskPhaseLut = *TwinOf(Lut::DiskPhase);
		const std::array samplerBinds{ Bind::Handle(samplers[0].Index()), Bind::Handle(samplers[1].Index()), Bind::Handle(samplers[2].Index()) };
		// Before IBL's and other EarlyPrepass clients, which read nothing of these.
		AnchorPasses::Builder builder(FrameAnchors::Anchor::EarlyPrepass, kLutsClientId, -20, latch, gate);
		builder.Add(AnchorPasses::Dispatch{ .name = "disk-phase",
			.program = diskPhaseProgram,
			.bindings = { Bind::Uav(diskPhaseLut), Bind::Latch(0) },
			.gate = kDiskPhaseEntry });
		constexpr std::array<const char*, kLutGenEntries> names{ "transmittance", "multiscatter", "sky-view", "aerial-perspective" };
		for (std::uint32_t i = 0; i < kLutGenEntries; ++i) {
			const auto lut = static_cast<Lut>(i);
			// The transmittance LUT reads none, the multiscatter LUT only the transmittance one.
			const bool readsTr = lut != Lut::Transmittance;
			const bool readsMs = lut == Lut::SkyView || lut == Lut::AerialPerspective;
			std::vector<Bind> bindings{ Bind::Shared(), readsTr ? Bind::Srv(tr) : Bind::Unused(), readsMs ? Bind::Srv(ms) : Bind::Unused(),
				readsMs ? Bind::Srv(diskPhaseLut) : Bind::Unused(), Bind::Uav(*TwinOf(lut)) };
			bindings.insert(bindings.end(), samplerBinds.begin(), samplerBinds.end());
			builder.Add(AnchorPasses::Dispatch{ .name = names[i], .program = lutPrograms[i], .bindings = std::move(bindings), .gate = i });
		}
		if (builder.Failed()) {
			logger::error("[ORG] Physical Sky: the LUT passes: {}", builder.Failure());
			return false;
		}
		a_client.id = kLutsClientId;
		a_client.sharedConstants = builder.ReadsSharedConstants();
		a_client.passes = builder.Take();
		a_client.gates = { gate };
		a_client.registerResources = [](org::RenderGraph& a_graph) {
			for (std::size_t i = 0; i < kLutCount; ++i)
				TwinOf(static_cast<Lut>(i))->Register(a_graph);
		};
		// Every run closes the gate first, so a frame without the feature dispatches nothing.
		a_client.active = [state = a_self] { return state->luts || state->diskPhase.has_value(); };
		a_client.commit = [state = a_self](const FrameAnchors::Point&) { state->Commit(); };
		logger::info("[ORG] Physical Sky: the LUTs run on the render graph at the EarlyPrepass anchor ({})", kLutsClientId);
		return true;
	}

	/** @brief The Prepass client's registration for a key, when a revision starts. */
	bool MakePrepass(const ShadowInputs& a_inputs, FrameAnchors::ClientDesc& a_client, const std::shared_ptr<Impl>& a_self)
	{
		namespace AnchorPasses = RenderGraph::AnchorPasses;
		using Bind = AnchorPasses::Bind;
		if (!MakeNull())
			return false;
		const auto depth = FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY);
		if (!depth) {
			logger::error("[ORG] Physical Sky: the depth copy cannot be imported for the AP shadow");
			return false;
		}
		const auto& output = *ApShadowTwin();
		const auto null = Bind::Handle(nullIndex);
		// In ShadowAccum's resource list order.
		const std::vector<Bind> bindings{ Bind::Shared(), Bind::Srv(depth), a_inputs.shadowMap ? Bind::SrvArray(a_inputs.shadowMap) : null, Bind::Latch(0),
			a_inputs.terrain.empty() ? null : Bind::Published(a_inputs.terrain),
			a_inputs.cloud.empty() ? null : Bind::Published(a_inputs.cloud, static_cast<std::uint32_t>(org::SRVViewType::TextureCube)),
			a_inputs.clouds ? Bind::Published(ORGVolumetricClouds::kShadowVolumeId) : null, Bind::Uav(output), Bind::Handle(samplers[0].Index()) };
		AnchorPasses::Builder builder(FrameAnchors::Anchor::Prepass, kPrepassClientId, kPrepassOrder, prepassLatch, prepassGate);
		// A new version starts out as no shadow, for readers on frames no AP shadow ran. The commit opens it once per version.
		const auto gateRef = std::shared_ptr<const RenderGraph::RunGate>(prepassGate);
		builder.Add("ap-shadow.clear", FrameAnchors::PassKind::Compute,
			[twin = &output, gateRef] { return GraphPasses::MakeUavClear(*twin, std::array<float, 4>{}, GraphPasses::ClearGate{ gateRef, kShadowClearEntry }); }, false,
			true);
		builder.Add(AnchorPasses::Dispatch{ .name = "ap-shadow", .program = shadowFull, .bindings = bindings, .gate = kShadowFullEntry });
		builder.Add(AnchorPasses::Dispatch{ .name = "ap-shadow.half", .program = shadowHalf, .bindings = bindings, .gate = kShadowHalfEntry });
		// The sky tail, in LutGen's list order. The LUTs are read by id, after the pre-depth stage wrote them. ViewAtmosphere's cloud
		// diffuse transport is not published, so it is null, which the shader reads only with the clouds on.
		const auto& tailOutput = *SkyTailTwin();
		builder.Add(AnchorPasses::Dispatch{ .name = "sky-tail",
			.program = skyTail,
			.bindings = { Bind::Shared(), Bind::Published(ORGPhysicalSky::kTransmittanceLutId), Bind::Published(ORGPhysicalSky::kMultiscatterLutId),
				Bind::Published(ORGPhysicalSky::kDiskPhaseLutId), Bind::Uav(tailOutput), Bind::Handle(samplers[0].Index()), Bind::Handle(samplers[1].Index()),
				Bind::Handle(samplers[2].Index()), a_inputs.clouds ? Bind::Published(ORGVolumetricClouds::kShadowVolumeId) : null,
				a_inputs.clouds ? Bind::Published(ORGVolumetricClouds::kCloudShadowMeanId) : null, null },
			.gate = kSkyTailEntry });
		if (builder.Failed()) {
			logger::error("[ORG] Physical Sky: the AP shadow's passes: {}", builder.Failure());
			return false;
		}
		a_client.id = kPrepassClientId;
		a_client.sharedConstants = builder.ReadsSharedConstants();
		a_client.passes = builder.Take();
		a_client.gates = { prepassGate };
		a_client.registerResources = [](org::RenderGraph& a_graph) {
			ApShadowTwin()->Register(a_graph);
			SkyTailTwin()->Register(a_graph);
		};
		a_client.active = [state = a_self] { return state->shadow.has_value() || state->tail; };
		a_client.commit = [state = a_self](const FrameAnchors::Point&) { state->CommitPrepass(); };
		// Every registration commits the same run, so the one the adopted revision holds keeps running until a newer one is adopted.
		a_client.matchesById = true;
		logger::info("[ORG] Physical Sky: an AP shadow for the next revision ({}; shadow map {}, terrain heights {}, cloud layer {}, clouds {})", kPrepassClientId,
			a_inputs.shadowMap ? "imported" : "none", a_inputs.terrain.empty() ? "none" : a_inputs.terrain, a_inputs.cloud.empty() ? "none" : a_inputs.cloud,
			a_inputs.clouds);
		return true;
	}

	/** @brief The composite client's registration for a key, when a revision starts. */
	bool MakeComposite(const CompositeInputs& a_inputs, FrameAnchors::ClientDesc& a_client, const std::shared_ptr<Impl>& a_self)
	{
		namespace AnchorPasses = RenderGraph::AnchorPasses;
		using Bind = AnchorPasses::Bind;
		if (!MakeNull())
			return false;
		const auto main = FrameAnchors::EngineTarget(a_inputs.main);
		const auto depth = a_inputs.depth ? FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY) : FrameAnchors::Texture{};
		if (!main || (a_inputs.depth && !depth)) {
			logger::error("[ORG] Physical Sky: the main target {} or the depth copy cannot be imported for the aerial perspective", a_inputs.main);
			return false;
		}
		const auto null = Bind::Handle(nullIndex);
		const auto srvOr = [&](const FrameAnchors::Texture& a_texture) { return a_texture ? Bind::Srv(a_texture) : null; };
		// In ApplyAerialPerspectiveCS's resource list order. A null depth reads zero, as the D3D11 composite's unbound t4 does.
		std::vector<Bind> bindings{ Bind::Shared(), Bind::Uav(main), srvOr(depth), Bind::Published(ORGPhysicalSky::kApLutId),
			Bind::Published(ORGPhysicalSky::kApShadowId), a_inputs.clouds ? Bind::Published(ORGVolumetricClouds::kVolTrCurrentId) : null,
			a_inputs.clouds ? Bind::Published(ORGVolumetricClouds::kVolLumCurrentId) : null,
			a_inputs.clouds ? Bind::Published(ORGVolumetricClouds::kLightningChannelsId) : null, Bind::Handle(samplers[1].Index()) };
		AnchorPasses::Builder builder(FrameAnchors::Anchor::DeferredComposite, kCompositeClientId, kCompositeOrder, {}, compositeGate);
		builder.Add(AnchorPasses::Dispatch{ .name = "apply", .program = applyAp, .bindings = std::move(bindings), .gate = 0 });
		if (builder.Failed()) {
			logger::error("[ORG] Physical Sky: the aerial perspective's pass: {}", builder.Failure());
			return false;
		}
		a_client.id = kCompositeClientId;
		a_client.sharedConstants = builder.ReadsSharedConstants();
		a_client.passes = builder.Take();
		a_client.gates = { compositeGate };
		a_client.active = [state = a_self] { return state->compositeFrame == globals::state->frameCount; };
		a_client.commit = [state = a_self](const FrameAnchors::Point&) { state->CommitComposite(); };
		logger::info("[ORG] Physical Sky: an aerial perspective after the graph composite for the next revision ({}; main target {}, depth {}, clouds {})",
			kCompositeClientId, a_inputs.main, a_inputs.depth ? "the depth copy" : "none (no Dynamic Cubemaps)", a_inputs.clouds ? "the graph's" : "none");
		return true;
	}

	/** @brief The three clients, declared at Setup. */
	void DeclareClients(const std::shared_ptr<Impl>& a_self)
	{
		using namespace RenderGraph::Frame;
		const std::weak_ptr<Impl> weak = a_self;
		const auto& settings = globals::features::physicalSky.settings;
		lutClient = DeclareClient<bool>(
			kLutsClientId, Dependencies{}, [](const Inputs&) -> std::optional<bool> { return true; },
			[weak](const bool&, FrameAnchors::ClientDesc& a_client) {
				const auto state = weak.lock();
				return state && state->MakeLutClient(a_client, state);
			});
		if (!lutClient)
			PhysSkyFail("the LUT client could not be declared");
		if (!shadowFull || !applyAp)
			return;
		// While the sky is on, with what the last Prepass found.
		prepassClient = DeclareClient<ShadowInputs>(
			kPrepassClientId, DependsOn(settings.enabled),
			[weak](const Inputs& a_in) -> std::optional<ShadowInputs> {
				const auto state = weak.lock();
				if (!state || !a_in.Get(globals::features::physicalSky.settings.enabled))
					return std::nullopt;
				return state->shadowObserved;
			},
			[weak](const ShadowInputs& a_inputs, FrameAnchors::ClientDesc& a_client) {
				const auto state = weak.lock();
				return state && state->MakePrepass(a_inputs, a_client, state);
			});
		// The clouds' screen targets are one texel while the clouds are off, so the composite reads them only with the clouds on in
		// the same revision. Dynamic Cubemaps is loaded or not for the session.
		compositeClient = DeclareClient<CompositeInputs>(
			kCompositeClientId, DependsOn(settings.enabled, settings.enableVolumetricClouds),
			[weak](const Inputs& a_in) -> std::optional<CompositeInputs> {
				const auto state = weak.lock();
				const auto& sky = globals::features::physicalSky.settings;
				if (!state || !state->compositeObserved || !a_in.Get(sky.enabled))
					return std::nullopt;
				auto inputs = *state->compositeObserved;
				inputs.depth = globals::features::dynamicCubemaps.loaded;
				inputs.clouds = inputs.clouds && a_in.Get(sky.enableVolumetricClouds);
				return inputs;
			},
			[weak](const CompositeInputs& a_inputs, FrameAnchors::ClientDesc& a_client) {
				const auto state = weak.lock();
				return state && state->MakeComposite(a_inputs, a_client, state);
			},
			[weak](const CompositeInputs& a_inputs) {
				if (const auto state = weak.lock())
					state->compositeHeld = a_inputs;
			});
		if (!prepassClient || !compositeClient)
			PhysSkyFail("the AP shadow's or the aerial perspective's client could not be declared");
	}
};

ORGPhysicalSky::~ORGPhysicalSky() = default;

bool ORGPhysicalSky::FrameValue(const CS::Setting<bool>& a_setting)
{
	if (!Requested() || !RenderGraph::Frame::Driver::Installed() || !RenderGraphRuntime::Get().IsActive())
		return a_setting.Get();
	return RenderGraph::Frame::Revision::Adopted().Get(a_setting);
}

void ORGPhysicalSky::Reserve()
{
	if (!Requested())
		return;
	if (!FrameAnchors::Reserve(FrameAnchors::Anchor::EarlyPrepass))
		logger::error("[ORG] Physical Sky: the EarlyPrepass anchor could not be reserved; the LUTs stay on D3D11");
	if (!FrameAnchors::Reserve(FrameAnchors::Anchor::Prepass) || !FrameAnchors::Reserve(FrameAnchors::Anchor::DeferredComposite))
		logger::error("[ORG] Physical Sky: the Prepass or DeferredComposite anchor could not be reserved; the AP shadow stays on D3D11");
}

namespace
{
	/** @brief A D3D11 wrapper of a twin's export. The twin keeps the image, the wrapper the D3D11 texture and its views. */
	template <class Wrapper, class Twin>
	Wrapper* PhysSkyWrap(const Twin& a_twin, const char* a_name)
	{
		auto* d3d11 = a_twin.D3D11();
		d3d11->AddRef();
		Wrapper* wrapper = nullptr;
		if constexpr (std::is_same_v<Wrapper, ::Texture3D>)
			wrapper = new ::Texture3D(d3d11);
		else
			wrapper = new ::Texture2D(d3d11, a_name);
		wrapper->srv.copy_from(a_twin.SRV());
		wrapper->uav.copy_from(a_twin.UAV());
		return wrapper;
	}

	/** @brief The description a twin exported for D3D11 SRV and UAV takes. */
	template <class Desc>
	Desc PhysSkyTwinDesc(const Desc& a_desc)
	{
		Desc desc = a_desc;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = 0;
		desc.Usage = D3D11_USAGE_DEFAULT;
		return desc;
	}
}

Texture2D* ORGPhysicalSky::MakeLut2D(Lut a_lut, const D3D11_TEXTURE2D_DESC& a_desc)
{
	if (a_lut == Lut::AerialPerspective || a_lut >= Lut::Count || !Requested() || !RenderGraphRuntime::Get().IsActive())
		return nullptr;
	auto& twin = Twins2D()[static_cast<std::size_t>(a_lut)];
	if (twin)
		return nullptr;
	const auto desc = PhysSkyTwinDesc(a_desc);
	// A fixed shape: declared with no dependencies, so it has one version for the session.
	auto declared = RenderGraph::Frame::DeclareTexture2D(
		std::string(IdOf(a_lut)), RenderGraph::Frame::Dependencies{}, [desc](const RenderGraph::Frame::Inputs&) { return desc; }, LutOptions(a_lut));
	if (!declared || !*declared || !declared->D3D11() || !declared->SRV() || !declared->UAV()) {
		logger::error("[ORG] Physical Sky: {} could not be made on the render graph; it is a D3D11 texture", IdOf(a_lut));
		return nullptr;
	}
	if (!PublishTwin(*declared, a_lut))
		return nullptr;
	twin = std::move(declared);
	return PhysSkyWrap<::Texture2D>(*twin, IdOf(a_lut).data());
}

Texture3D* ORGPhysicalSky::MakeLut3D(Lut a_lut, const D3D11_TEXTURE3D_DESC& a_desc)
{
	if (a_lut != Lut::AerialPerspective || !Requested() || !RenderGraphRuntime::Get().IsActive())
		return nullptr;
	auto& twin = ApTwin();
	if (twin)
		return nullptr;
	const auto desc = PhysSkyTwinDesc(a_desc);
	auto declared = RenderGraph::Frame::DeclareTexture3D(
		std::string(IdOf(a_lut)), RenderGraph::Frame::Dependencies{}, [desc](const RenderGraph::Frame::Inputs&) { return desc; }, LutOptions(a_lut));
	if (!declared || !*declared || !declared->D3D11() || !declared->SRV() || !declared->UAV()) {
		logger::error("[ORG] Physical Sky: {} could not be made on the render graph; it is a D3D11 texture", IdOf(a_lut));
		return nullptr;
	}
	if (!PublishTwin(*declared, a_lut))
		return nullptr;
	twin = std::move(declared);
	return PhysSkyWrap<::Texture3D>(*twin, nullptr);
}

Texture2D* ORGPhysicalSky::MakeSkyTail(const D3D11_TEXTURE2D_DESC& a_desc)
{
	if (!Requested() || !RenderGraphRuntime::Get().IsActive())
		return nullptr;
	auto& twin = SkyTailTwin();
	if (twin)
		return nullptr;
	// D3D11 writes it on frames the graph does not, and the next frame's reflection faces read it.
	const GraphResources::Options options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::UAV };
	auto texture = std::make_unique<GraphResources::Texture2D>(PhysSkyTwinDesc(a_desc), std::string(kSkyTailId), options);
	if (!*texture || !texture->D3D11() || !texture->SRV() || !texture->UAV()) {
		logger::error("[ORG] Physical Sky: {} could not be made on the render graph; it is a D3D11 texture", kSkyTailId);
		return nullptr;
	}
	if (!PublishedResources::Publish(kSkyTailId, *texture)) {
		logger::error("[ORG] Physical Sky: {} could not be published; it is a D3D11 texture", kSkyTailId);
		return nullptr;
	}
	twin = std::move(texture);
	return PhysSkyWrap<::Texture2D>(*twin, kSkyTailId.data());
}

Texture2D* ORGPhysicalSky::MakeApShadow(const D3D11_TEXTURE2D_DESC& a_desc)
{
	if (!Requested() || !RenderGraphRuntime::Get().IsActive())
		return nullptr;
	auto& twin = ApShadowTwin();
	if (twin)
		return nullptr;
	using namespace RenderGraph::Frame;
	// Sized for the display (decision 6a): the pass covers the dynamic-resolution region. Before the display size is known,
	// the main target's, which D3D11 sizes it by.
	const auto desc = PhysSkyTwinDesc(a_desc);
	// D3D11 writes it on frames the graph does not, every reader reads it, and the next frame's reflection faces read it.
	const GraphResources::Options options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::UAV };
	auto declared = DeclareTexture2D(
		std::string(kApShadowId), DependsOn(Vars::DisplaySize()),
		[desc](const Inputs& a_in) {
			auto next = desc;
			if (const auto display = a_in.Get(Vars::DisplaySize()); display.width && display.height) {
				next.Width = display.width;
				next.Height = display.height;
			}
			return next;
		},
		options);
	if (!declared || !*declared || !declared->D3D11() || !declared->SRV() || !declared->UAV()) {
		logger::error("[ORG] Physical Sky: {} could not be made on the render graph; it is a D3D11 texture", kApShadowId);
		return nullptr;
	}
	// Published once, for the session. Readers follow the versions.
	if (!PublishedResources::Publish(kApShadowId, *declared)) {
		logger::error("[ORG] Physical Sky: {} could not be published; it is a D3D11 texture", kApShadowId);
		return nullptr;
	}
	twin = std::move(declared);
	return PhysSkyWrap<::Texture2D>(*twin, kApShadowId.data());
}

bool ORGPhysicalSky::Setup(const PhysicalSky& a_feature)
{
	impl.reset();
	if (!Requested())
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!runtime.IsActive() || !host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::EarlyPrepass)) {
		logger::error("[ORG] Physical Sky: the render graph or its EarlyPrepass anchor is not available; the LUTs stay on D3D11");
		return false;
	}
	for (std::size_t i = 0; i < kLutCount; ++i)
		if (!TwinOf(static_cast<Lut>(i))) {
			logger::error("[ORG] Physical Sky: {} is a D3D11 texture; the LUTs stay on D3D11", IdOf(static_cast<Lut>(i)));
			return false;
		}

	auto next = std::make_shared<Impl>();
	for (std::uint32_t i = 0; i < kLutGenEntries; ++i) {
		const auto value = std::to_wstring(i);
		next->lutPrograms[i] = ComputeProgram::Load(host->Device(), { .source = kLutShader, .constantWords = kLutGenWords, .defines = { { L"LUTGEN", value } } });
	}
	next->diskPhaseProgram = ComputeProgram::Load(host->Device(), { .source = kDiskPhaseShader, .constantWords = kDiskPhaseWords });
	next->samplers = { SamplerOf(a_feature.sampTr.get()), SamplerOf(a_feature.sampSv.get()), SamplerOf(a_feature.sampNoise.get()) };
	next->gate = RenderGraph::RunGate::Create("cs.physical-sky.luts.gate", kLutGenEntries + 1);
	next->latch = std::make_shared<org::LatchBlock>("cs.physical-sky.luts.latch", kDiskPhaseBytes);
	const bool programs = std::ranges::all_of(next->lutPrograms, [](const auto& a_program) { return a_program != nullptr; }) && next->diskPhaseProgram;
	const bool samplers = std::ranges::all_of(next->samplers, [](const auto& a_sampler) { return static_cast<bool>(a_sampler); });
	if (!programs || !samplers || !next->gate) {
		logger::error("[ORG] Physical Sky: the LUT programs, samplers or run gate could not be made; the LUTs stay on D3D11");
		return false;
	}

	// The AP shadow, the sky tail and the AP composite. A program build blocks, so here.
	if (!ApShadowTwin() || !SkyTailTwin()) {
		logger::error("[ORG] Physical Sky: {} or {} is a D3D11 texture; the AP shadow and sky tail stay on D3D11 (no graph AP)", kApShadowId, kSkyTailId);
	} else if (!FrameAnchors::HasEpoch(FrameAnchors::Anchor::Prepass) || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::DeferredComposite)) {
		logger::error("[ORG] Physical Sky: the Prepass or DeferredComposite anchor has no epoch; the AP shadow stays on D3D11");
	} else {
		next->shadowFull = ComputeProgram::Load(host->Device(), { .source = kShadowAccumShader, .constantWords = kShadowAccumWords });
		next->shadowHalf = ComputeProgram::Load(host->Device(), { .source = kShadowAccumShader, .constantWords = kShadowAccumWords, .defines = { { L"HALF_RES", L"" } } });
		next->applyAp = ComputeProgram::Load(host->Device(), { .source = kApplyApShader, .constantWords = kApplyApWords });
		next->skyTail = ComputeProgram::Load(host->Device(),
			{ .source = kLutShader, .constantWords = kSkyTailWords, .defines = { { L"LUTGEN", L"2" }, { L"SKY_TAIL", L"" } } });
		next->prepassGate = RenderGraph::RunGate::Create("cs.physical-sky.gate", kPrepassEntries);
		next->compositeGate = RenderGraph::RunGate::Create("cs.physical-sky.composite.gate", 1);
		next->prepassLatch = std::make_shared<org::LatchBlock>("cs.physical-sky.latch", static_cast<std::uint32_t>(sizeof(ShadowLatch)));
		if (!next->shadowFull || !next->shadowHalf || !next->applyAp || !next->skyTail || !next->prepassGate || !next->compositeGate) {
			logger::error("[ORG] Physical Sky: the AP shadow, sky tail or AP composite programs or gates could not be made; they stay on D3D11");
			next->shadowFull = next->shadowHalf = next->applyAp = next->skyTail = nullptr;
		}
	}
	impl = next;
	// Every client follows the frame revisions from the first one on.
	next->DeclareClients(next);
	return true;
}

bool ORGPhysicalSky::IsActive() const
{
	return impl && RenderGraphRuntime::Get().IsActive();
}

void ORGPhysicalSky::BeginFrame(PhysicalSky& a_feature)
{
	if (!IsActive() || impl->beganFrame == globals::state->frameCount)
		return;
	impl->beganFrame = globals::state->frameCount;
	a_feature.FollowFrameToggles();
	// A new version of the AP shadow switched in at this frame's Begin. The commit clears it.
	if (a_feature.texApShadow && ApShadowTwin())
		PhysSkyFollowTwin(*a_feature.texApShadow, *ApShadowTwin());
	ORGVolumetricClouds::Get().FollowVersions(a_feature);
	// Reset ran before the Begin that adopted the newest revision. An off switch in it applies to this frame already, since
	// its clients and textures went with it. An on switch waits for the next Reset.
	if (!a_feature.IsActive())
		a_feature.cbData = {};
	else if (!a_feature.VolumetricCloudsEnabled())
		a_feature.cbData.enableVolumetricClouds = 0;
}

std::uint32_t ORGPhysicalSky::SamplerIndex(Sampler a_sampler, std::shared_ptr<const void>* a_owner) const
{
	const auto at = static_cast<std::size_t>(a_sampler);
	if (!IsActive() || at >= impl->samplers.size() || !impl->samplers[at])
		return kNoSampler;
	if (a_owner)
		*a_owner = impl->samplers[at].Owner();
	return impl->samplers[at].Index();
}

bool ORGPhysicalSky::GenerateLuts(PhysicalSky& a_feature)
{
	// A run of the client takes them. Before the first revision holds it the LUTs are not written, which is before the world.
	if (!IsActive())
		return false;
	impl->luts = true;
	// As UpdateDiskPhaseLut: only when the constants change. Kept until a run takes it.
	const auto parameters = a_feature.DiskPhaseParameters();
	if (!a_feature.diskPhaseValid || parameters != a_feature.diskPhaseParameters) {
		impl->diskPhase = parameters;
		a_feature.diskPhaseParameters = parameters;
		a_feature.diskPhaseValid = true;
	}
	return true;
}

bool ORGPhysicalSky::AccumShadow(const PhysicalSky& a_feature)
{
	if (!IsActive() || !impl->shadowFull)
		return false;
	auto& state = *impl;
	state.shadow.reset();
	const auto note = [&](const std::string& a_missing) {
		if (state.inputNote != a_missing) {
			if (!a_missing.empty())
				logger::warn("[ORG] Physical Sky: the AP shadow reads no {}: it is not on the render graph", a_missing);
			state.inputNote = a_missing;
		}
	};

	// As PhysicalSky::AccumShadow binds them.
	ShadowInputs inputs;
	ShadowLatch latch{};
	if (!globals::state->isMapMenuOpen) {
		const bool hasLight = globals::deferred && globals::deferred->GetDirectionalShadowLightData(latch.light);
		latch.lightCount = hasLight ? 1u : 0u;
		if (auto* shadowMap = globals::deferred ? globals::deferred->GetDirectionalShadowMap() : nullptr; shadowMap && hasLight) {
			inputs.shadowMap = PhysSkyImportShadowMap(shadowMap, "cs.physical-sky.directional-shadow-map");
			if (!inputs.shadowMap)
				PhysSkyFail("the directional shadow map cannot be imported for the AP shadow");
		}
		std::string missing;
		auto& terrainShadows = globals::features::terrainShadows;
		if (terrainShadows.IsShadowMapReady()) {
			if (terrainShadows.texShadowHeight && PublishedResources::IsExportOf(terrainShadows.texShadowHeight->srv.get(), ORGTerrainShadows::kShadowHeightId))
				inputs.terrain = ORGTerrainShadows::kShadowHeightId;
			else
				missing = "Terrain Shadows heights";
		}
		if (globals::features::cloudShadows.loaded) {
			inputs.cloud = ORGCloudShadows::PublishedLayerId(CloudShadows::kMaxCloudLayers - 1);
			if (inputs.cloud.empty())
				missing = "Cloud Shadows layer";
		}
		note(missing);
		inputs.clouds = CloudsOnGraph(a_feature);
	} else {
		// As D3D11 clears it in the map menu.
		latch.clear = 1;
		if (state.shadowObserved)
			inputs = *state.shadowObserved;
	}
	// What the AP shadow reads is not a variable yet. A change registers the client again with the next revision.
	if (!state.shadowObserved || !(*state.shadowObserved == inputs)) {
		state.shadowObserved = inputs;
		state.prepassClient.Invalidate();
	}

	const bool half = a_feature.settings.halfResApShadow && !latch.clear;
	// The dynamic-resolution region, inside the bound version.
	const auto extent = RenderGraph::Frame::Revision::Adopted().Extent(ApShadowTwin());
	std::uint32_t width = (std::min)(static_cast<std::uint32_t>(a_feature.cbData.frameDim.x), extent[0]);
	std::uint32_t height = (std::min)(static_cast<std::uint32_t>(a_feature.cbData.frameDim.y), extent[1]);
	if (half) {
		width = (width + 1u) / 2u;
		height = (height + 1u) / 2u;
	}
	state.shadowEntry = half ? kShadowHalfEntry : kShadowFullEntry;
	state.shadowGroups = { (width + 7u) >> 3, (height + 7u) >> 3 };
	// A run of the registration the frame holds takes it. Before the first one is adopted, nothing does.
	state.shadow = latch;
	return true;
}

bool ORGPhysicalSky::GenerateSkyTail(const PhysicalSky& a_feature)
{
	if (!IsActive() || !impl->skyTail)
		return false;
	auto& state = *impl;
	// The tail shares the AP shadow's client. Without an AP shadow seen yet, it registers with no shadow inputs.
	if (!state.shadowObserved) {
		state.shadowObserved = ShadowInputs{ .clouds = CloudsOnGraph(a_feature) };
		state.prepassClient.Invalidate();
	}
	state.tail = true;
	return true;
}

void ORGPhysicalSky::StageComposite(const PhysicalSky& a_feature)
{
	if (!IsActive() || !impl->applyAp)
		return;
	auto& state = *impl;
	// SetFrame of this frame comes later, so this is the last frame's target. The commit checks this frame's.
	const auto frame = ORGDeferredComposite::Get().GraphComposite();
	if (!frame)
		return;
	// ViewAtmosphere's medium replaces the aerial perspective (ViewAtmosphere::Enabled() in the shader), and this pass has no medium.
	if (globals::features::viewAtmosphere.IsPrepared())
		return;
	CompositeInputs inputs;
	inputs.main = frame->main;
	inputs.depth = globals::features::dynamicCubemaps.loaded;
	inputs.clouds = CloudsOnGraph(a_feature);
	// Not variables yet: a change registers the client again with the next revision.
	if (!state.compositeObserved || !(*state.compositeObserved == inputs)) {
		state.compositeObserved = inputs;
		state.compositeClient.Invalidate();
	}
	if (!state.compositeClient.Registered())
		state.compositeHeld.reset();
	state.compositeFrame = globals::state->frameCount;
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph the LUTs and the AP shadow are always D3D11's.
struct ORGPhysicalSky::Impl
{};

ORGPhysicalSky::~ORGPhysicalSky() = default;
void ORGPhysicalSky::Reserve() {}
Texture2D* ORGPhysicalSky::MakeLut2D(Lut, const D3D11_TEXTURE2D_DESC&) { return nullptr; }
Texture3D* ORGPhysicalSky::MakeLut3D(Lut, const D3D11_TEXTURE3D_DESC&) { return nullptr; }
bool ORGPhysicalSky::FrameValue(const CS::Setting<bool>& a_setting) { return a_setting.Get(); }
bool ORGPhysicalSky::Setup(const PhysicalSky&) { return false; }
bool ORGPhysicalSky::IsActive() const { return false; }
void ORGPhysicalSky::BeginFrame(PhysicalSky&) {}
std::uint32_t ORGPhysicalSky::SamplerIndex(Sampler, std::shared_ptr<const void>*) const { return kNoSampler; }
bool ORGPhysicalSky::GenerateLuts(PhysicalSky&) { return false; }
Texture2D* ORGPhysicalSky::MakeApShadow(const D3D11_TEXTURE2D_DESC&) { return nullptr; }
bool ORGPhysicalSky::AccumShadow(const PhysicalSky&) { return false; }
void ORGPhysicalSky::StageComposite(const PhysicalSky&) {}
Texture2D* ORGPhysicalSky::MakeSkyTail(const D3D11_TEXTURE2D_DESC&) { return nullptr; }
bool ORGPhysicalSky::GenerateSkyTail(const PhysicalSky&) { return false; }

#endif
