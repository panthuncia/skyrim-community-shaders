// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/PhysicalSky/ORGPhysicalSky.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "Buffer.h"
#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DynamicCubemaps.h"
#include "Features/PhysicalSky.h"
#include "Features/PhysicalSky/ORGVolumetricClouds.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainShadows/ORGTerrainShadows.h"
#include "Features/ViewAtmosphere.h"
#include "Features/ViewAtmosphere/ORGViewAtmosphereReflection.h"
#include "Globals.h"
#include "ORGDeferredComposite.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/D3D11Wrapper.h"
#include "RenderGraph/DepthViewSource.h"
#include "RenderGraph/Engine/Targets/TargetRef.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameClock.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "State.h"
#include "Utils/Game.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

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

	// The Prepass gate's entries: the full and the half resolution AP shadow, then the sky tail.
	constexpr std::uint32_t kShadowFullEntry = 0;
	constexpr std::uint32_t kShadowHalfEntry = 1;
	constexpr std::uint32_t kSkyTailEntry = 2;
	constexpr std::uint32_t kPrepassEntries = 3;
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

	/**
	* @brief What the last Prepass found for the AP shadow that is no variable yet (B2's interim): its inputs without the shadow
	* map, and the shadow map as an engine depth target or another view. The Prepass client's key imports it.
	*/
	struct ShadowSeen
	{
		ShadowInputs inputs;                     // shadowMap left empty
		RenderGraph::DepthViewSource shadowMap;  // empty when the frame drew no shadow mask

		bool operator==(const ShadowSeen&) const = default;

		/** @brief The inputs, with the shadow map imported. The engine depth's slot never changes. */
		ShadowInputs Resolved() const
		{
			auto resolved = inputs;
			resolved.shadowMap = shadowMap.Resolve("cs.physical-sky.directional-shadow-map");
			return resolved;
		}
	};

	/**
	* @brief Whether this frame's readers bind the volumetric clouds' outputs: the adopted revision has the clouds on, and a run of
	* the cloud client wrote the bound versions. Their ids are published at setup, for the session.
	*/
	bool CloudsOnGraph(const PhysicalSky& a_feature)
	{
		return a_feature.VolumetricCloudsEnabled() && ORGVolumetricClouds::OutputsWritten();
	}



	using Lut = ORGPhysicalSky::Lut;
	constexpr auto kLutCount = static_cast<std::size_t>(Lut::Count);
	// Gate entries: the four LutGen dispatches in LUTGEN order, then the disk phase LUT.
	constexpr std::uint32_t kLutGenEntries = 4;
	constexpr std::uint32_t kDiskPhaseEntry = 4;
	constexpr std::uint32_t kLutEntries = 5;
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
	void PublishTwin(const Twin& a_twin, Lut a_lut)
	{
		if (!PublishedResources::Publish(IdOf(a_lut), a_twin))
			RenderGraph::Fatal(ORGPhysicalSky::kName, fmt::format("{} could not be published", IdOf(a_lut)));
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
	// Queued at setup.
	std::array<RenderGraph::Programs::Fixed<ComputeProgram>, kLutGenEntries> lutPrograms;
	RenderGraph::Programs::Fixed<ComputeProgram> diskPhaseProgram;
	// SampTr, SampSv and SampNoise, kept as long as the programs.
	std::array<org::OwnedDescriptorBinding, 3> samplers;

	// Render thread. Set at the feature's EarlyPrepass, taken by the next commit.
	bool luts = false;
	std::optional<std::array<float, 12>> diskPhase;

	// The AP shadow (cs.physical-sky) and the AP composite (cs.physical-sky.composite). Empty programs mean neither is on the graph.
	RenderGraph::Programs::Fixed<ComputeProgram> shadowFull, shadowHalf, applyAp, skyTail;
	std::shared_ptr<RenderGraph::RunGate> prepassGate, compositeGate;
	std::shared_ptr<org::LatchBlock> prepassLatch;
	// The null descriptor, for inputs D3D11 binds null: it reads zero and has zero size, as a null SRV does.
	std::uint32_t nullIndex = DCLF::GpuTextures::kInvalid;
	std::shared_ptr<const void> nullOwner;

	// The declared clients. The Prepass and composite keys add what the last Prepass found, which is not a variable yet:
	// a change is an event, and the next revision registers the client for it.
	RenderGraph::Frame::DeclaredClient lutClient;
	// Each registration keeps the inputs it was made from. The composite's commit reads the adopted one's (Adopted()).
	RenderGraph::Frame::DeclaredClientOf<const ShadowInputs> prepassClient;
	RenderGraph::Frame::DeclaredClientOf<const CompositeInputs> compositeClient;
	RenderGraph::Frame::Observed<ShadowSeen> shadowObserved{ "PhysicalSky.ApShadow" };
	RenderGraph::Frame::Observed<CompositeInputs> compositeObserved{ "PhysicalSky.Composite" };
	std::string inputNote;                // the last missing AP shadow input, logged when it changes
	std::uint64_t beganFrame = UINT64_MAX;  // the render frame (FrameClock) BeginFrame last ran for

	// Render thread. Set at the feature's Prepass, taken by the next commit.
	std::optional<ShadowLatch> shadow;
	std::uint32_t shadowEntry = kShadowFullEntry;
	std::array<std::uint32_t, 2> shadowGroups{};
	std::uint64_t compositeFrame = UINT64_MAX;  // the render frame (FrameClock) StageComposite staged
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
		const auto held = compositeClient.Adopted();
		if (compositeFrame != RenderGraph::FrameClock::BeginFrame() || !held)
			return;
		const auto frame = ORGDeferredComposite::Get().GraphComposite();
		// The D3D11 interior composite is built without PHYSICAL_SKY, so interiors get no AP there either.
		if (!frame || frame->interior || frame->main != held->main)
			return;
		// A composite whose schedule does not hold it yet writes nothing this frame.
		if (!FrameAnchors::Place(kDeferredClientId, FrameAnchors::Anchor::DeferredComposite).draws)
			return;
		const auto groups = Util::GetScreenDispatchCount(true);
		compositeGate->OpenDispatch(0, groups.x, groups.y, 1);
	}

	/** @brief The null descriptor, made once. */
	void MakeNull()
	{
		if (nullIndex != DCLF::GpuTextures::kInvalid)
			return;
		auto binding = DCLF::GpuTextures::Get().NullBinding();
		if (!binding)
			Fatal("the null descriptor could not be made");
		nullIndex = binding.index;
		nullOwner = std::move(binding.owner);
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
			.program = diskPhaseProgram.Get(),
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
			builder.Add(AnchorPasses::Dispatch{ .name = names[i], .program = lutPrograms[i].Get(), .bindings = std::move(bindings), .gate = i });
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
		MakeNull();
		const auto depth = FrameAnchors::EngineTexture(FrameAnchors::Depth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY));
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
		// Every version is born zero (no shadow), for readers on frames no AP shadow ran.
		builder.Add(AnchorPasses::Dispatch{ .name = "ap-shadow", .program = shadowFull.Get(), .bindings = bindings, .gate = kShadowFullEntry });
		builder.Add(AnchorPasses::Dispatch{ .name = "ap-shadow.half", .program = shadowHalf.Get(), .bindings = bindings, .gate = kShadowHalfEntry });
		// The sky tail, in LutGen's list order. The LUTs are read by id, after the pre-depth stage wrote them. The cloud diffuse
		// transport is the second row of ViewAtmosphere's reflection sky SH, by id. The shader reads it only with the clouds on. It
		// is born zero and cleared when the reflection medium stops, which reads as no transport, as D3D11's null view.
		const bool transport = a_inputs.clouds && ORGViewAtmosphereReflection::Get().TransportPublished();
		const auto& tailOutput = *SkyTailTwin();
		builder.Add(AnchorPasses::Dispatch{ .name = "sky-tail",
			.program = skyTail.Get(),
			.bindings = { Bind::Shared(), Bind::Published(ORGPhysicalSky::kTransmittanceLutId), Bind::Published(ORGPhysicalSky::kMultiscatterLutId),
				Bind::Published(ORGPhysicalSky::kDiskPhaseLutId), Bind::Uav(tailOutput), Bind::Handle(samplers[0].Index()), Bind::Handle(samplers[1].Index()),
				Bind::Handle(samplers[2].Index()), a_inputs.clouds ? Bind::Published(ORGVolumetricClouds::kShadowVolumeId) : null,
				a_inputs.clouds ? Bind::Published(ORGVolumetricClouds::kCloudShadowMeanId) : null,
				transport ? Bind::Published(ORGViewAtmosphereReflection::kSkySHId) : null },
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
		// Every registration commits the same run, so the one the schedule holds keeps running until a newer one is adopted (by id).
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
		MakeNull();
		const auto main = FrameAnchors::EngineTexture(FrameAnchors::Colour(a_inputs.main));
		const auto depth = a_inputs.depth ? FrameAnchors::EngineTexture(FrameAnchors::Depth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY)) : FrameAnchors::Texture{};
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
		builder.Add(AnchorPasses::Dispatch{ .name = "apply", .program = applyAp.Get(), .bindings = std::move(bindings), .gate = 0 });
		if (builder.Failed()) {
			logger::error("[ORG] Physical Sky: the aerial perspective's pass: {}", builder.Failure());
			return false;
		}
		a_client.id = kCompositeClientId;
		a_client.sharedConstants = builder.ReadsSharedConstants();
		a_client.passes = builder.Take();
		a_client.gates = { compositeGate };
		a_client.active = [state = a_self] { return state->compositeFrame == RenderGraph::FrameClock::BeginFrame(); };
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
		if (!shadowFull || !applyAp)
			return;
		// While the sky is on, with what the last Prepass found.
		prepassClient = DeclareClient<ShadowInputs, const ShadowInputs>(
			kPrepassClientId, DependsOn(settings.enabled, shadowObserved.Var()),
			[weak](const Inputs& a_in) -> std::optional<ShadowInputs> {
				const auto state = weak.lock();
				if (!state || !a_in.Get(globals::features::physicalSky.settings.enabled))
					return std::nullopt;
				const auto* seen = state->shadowObserved.Get(a_in);
				if (!seen)
					return std::nullopt;
				return seen->Resolved();
			},
			[weak](const ShadowInputs& a_inputs, FrameAnchors::ClientDesc& a_client) -> std::shared_ptr<const ShadowInputs> {
				const auto state = weak.lock();
				if (!state || !state->MakePrepass(a_inputs, a_client, state))
					return nullptr;
				return std::make_shared<const ShadowInputs>(a_inputs);
			});
		// The clouds' screen targets are one texel while the clouds are off, so the composite reads them only with the clouds on in
		// the same revision. Dynamic Cubemaps is loaded or not for the session.
		compositeClient = DeclareClient<CompositeInputs, const CompositeInputs>(
			kCompositeClientId, DependsOn(settings.enabled, settings.enableVolumetricClouds, compositeObserved.Var()),
			[weak](const Inputs& a_in) -> std::optional<CompositeInputs> {
				const auto state = weak.lock();
				const auto& sky = globals::features::physicalSky.settings;
				if (!state || !a_in.Get(sky.enabled))
					return std::nullopt;
				const auto* seen = state->compositeObserved.Get(a_in);
				if (!seen)
					return std::nullopt;
				auto inputs = *seen;
				inputs.depth = globals::features::dynamicCubemaps.loaded;
				inputs.clouds = inputs.clouds && a_in.Get(sky.enableVolumetricClouds);
				return inputs;
			},
			[weak](const CompositeInputs& a_inputs, FrameAnchors::ClientDesc& a_client) -> std::shared_ptr<const CompositeInputs> {
				const auto state = weak.lock();
				if (!state || !state->MakeComposite(a_inputs, a_client, state))
					return nullptr;
				return std::make_shared<const CompositeInputs>(a_inputs);
			});
	}
};

ORGPhysicalSky::ORGPhysicalSky() = default;
ORGPhysicalSky::~ORGPhysicalSky() = default;

bool ORGPhysicalSky::FrameValue(const CS::Setting<bool>& a_setting)
{
	if (!On() || !RenderGraph::Frame::Driver::Installed())
		return a_setting.Get();
	return a_setting.Adopted();
}

namespace
{
	/** @brief The wrappers' registrations, kept for the session. */
	template <class Wrapper>
	std::vector<GraphResources::D3D11Wrapper<Wrapper>>& PhysSkyWrappers()
	{
		static std::vector<GraphResources::D3D11Wrapper<Wrapper>> s_wrappers;
		return s_wrappers;
	}

	/**
	* @brief A D3D11 wrapper of a twin's export, which the feature owns. The twin keeps the image. The wrapper keeps the D3D11
	* texture and its views, and follows the twin's version (RetargetWrappers).
	*/
	template <class Twin>
	auto* PhysSkyWrap(const Twin& a_twin, const char* a_name)
	{
		auto wrapper = GraphResources::MakeWrapper(a_twin, a_name);
		auto* object = wrapper.Get();
		PhysSkyWrappers<std::remove_pointer_t<decltype(object)>>().push_back(std::move(wrapper));
		return object;
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
	if (a_lut == Lut::AerialPerspective || a_lut >= Lut::Count || !On())
		return nullptr;
	auto& twin = Twins2D()[static_cast<std::size_t>(a_lut)];
	if (twin)
		return nullptr;
	const auto desc = PhysSkyTwinDesc(a_desc);
	// A fixed shape: declared with no dependencies, so it has one version for the session.
	auto declared = RenderGraph::Frame::DeclareTexture2D(
		std::string(IdOf(a_lut)), RenderGraph::Frame::Dependencies{}, [desc](const RenderGraph::Frame::Inputs&) { return desc; }, LutOptions(a_lut));
	if (!declared->D3D11() || !declared->SRV() || !declared->UAV())
		Fatal(fmt::format("{} could not be made on the render graph", IdOf(a_lut)));
	PublishTwin(*declared, a_lut);
	twin = std::move(declared);
	return PhysSkyWrap(*twin, IdOf(a_lut).data());
}

Texture3D* ORGPhysicalSky::MakeLut3D(Lut a_lut, const D3D11_TEXTURE3D_DESC& a_desc)
{
	if (a_lut != Lut::AerialPerspective || !On())
		return nullptr;
	auto& twin = ApTwin();
	if (twin)
		return nullptr;
	const auto desc = PhysSkyTwinDesc(a_desc);
	auto declared = RenderGraph::Frame::DeclareTexture3D(
		std::string(IdOf(a_lut)), RenderGraph::Frame::Dependencies{}, [desc](const RenderGraph::Frame::Inputs&) { return desc; }, LutOptions(a_lut));
	if (!declared->D3D11() || !declared->SRV() || !declared->UAV())
		Fatal(fmt::format("{} could not be made on the render graph", IdOf(a_lut)));
	PublishTwin(*declared, a_lut);
	twin = std::move(declared);
	return PhysSkyWrap(*twin, nullptr);
}

Texture2D* ORGPhysicalSky::MakeSkyTail(const D3D11_TEXTURE2D_DESC& a_desc)
{
	if (!On())
		return nullptr;
	auto& twin = SkyTailTwin();
	if (twin)
		return nullptr;
	// D3D11 writes it on frames the graph does not, and the next frame's reflection faces read it.
	const GraphResources::Options options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::UAV };
	auto texture = std::make_unique<GraphResources::Texture2D>(PhysSkyTwinDesc(a_desc), std::string(kSkyTailId), options);
	if (!texture->D3D11() || !texture->SRV() || !texture->UAV())
		Fatal(fmt::format("{} could not be made on the render graph", kSkyTailId));
	if (!PublishedResources::Publish(kSkyTailId, *texture))
		Fatal(fmt::format("{} could not be published", kSkyTailId));
	twin = std::move(texture);
	return PhysSkyWrap(*twin, kSkyTailId.data());
}

Texture2D* ORGPhysicalSky::MakeApShadow(const D3D11_TEXTURE2D_DESC& a_desc)
{
	if (!On())
		return nullptr;
	auto& twin = ApShadowTwin();
	if (twin)
		return nullptr;
	using namespace RenderGraph::Frame;
	// Sized for the display (decision 6a): the pass covers the dynamic-resolution region.
	// D3D11 writes it on frames the graph does not, every reader reads it, and the next frame's reflection faces read it.
	// Every version is born zero, which is no shadow, so nothing in the frame clears a new one.
	const GraphResources::Options options{ .lifetime = GraphResources::Lifetime::History,
		.d3d11 = GraphResources::Export::SRV | GraphResources::Export::UAV,
		.birth = Birth::Zero() };
	auto declared = DeclareTexture2D(std::string(kApShadowId), DisplaySized(PhysSkyTwinDesc(a_desc)), options);
	if (!declared->D3D11() || !declared->SRV() || !declared->UAV())
		Fatal(fmt::format("{} could not be made on the render graph", kApShadowId));
	// Published once, for the session. Readers follow the versions.
	if (!PublishedResources::Publish(kApShadowId, *declared))
		Fatal(fmt::format("{} could not be published", kApShadowId));
	twin = std::move(declared);
	return PhysSkyWrap(*twin, kApShadowId.data());
}

void ORGPhysicalSky::Setup(const PhysicalSky& a_feature)
{
	// Once a session. The LUTs are published, so they are never torn down.
	if (!BeginSetup())
		return;
	for (std::size_t i = 0; i < kLutCount; ++i)
		if (!TwinOf(static_cast<Lut>(i)))
			Fatal(fmt::format("{} is a D3D11 texture", IdOf(static_cast<Lut>(i))));

	auto next = std::make_shared<Impl>();
	for (std::uint32_t i = 0; i < kLutGenEntries; ++i) {
		const auto value = std::to_wstring(i);
		next->lutPrograms[i] = RenderGraph::Programs::Queue("PhysicalSky", ComputeProgram::Desc{ .source = kLutShader, .constantWords = kLutGenWords, .defines = { { L"LUTGEN", value } } });
	}
	next->diskPhaseProgram = RenderGraph::Programs::Queue("PhysicalSky", ComputeProgram::Desc{ .source = kDiskPhaseShader, .constantWords = kDiskPhaseWords });
	next->samplers = { SamplerOf(a_feature.sampTr.get()), SamplerOf(a_feature.sampSv.get()), SamplerOf(a_feature.sampNoise.get()) };
	next->gate = RenderGraph::RunGate::Create("cs.physical-sky.luts.gate", kLutEntries);
	next->latch = std::make_shared<org::LatchBlock>("cs.physical-sky.luts.latch", kDiskPhaseBytes);
	const bool samplers = std::ranges::all_of(next->samplers, [](const auto& a_sampler) { return static_cast<bool>(a_sampler); });
	if (!samplers)
		Fatal("the LUT samplers could not be made");

	// The AP shadow, the sky tail and the AP composite.
	if (!ApShadowTwin() || !SkyTailTwin())
		Fatal(fmt::format("{} or {} is a D3D11 texture", kApShadowId, kSkyTailId));
	next->shadowFull = RenderGraph::Programs::Queue("PhysicalSky", ComputeProgram::Desc{ .source = kShadowAccumShader, .constantWords = kShadowAccumWords });
	next->shadowHalf = RenderGraph::Programs::Queue("PhysicalSky",
		ComputeProgram::Desc{ .source = kShadowAccumShader, .constantWords = kShadowAccumWords, .defines = { { L"HALF_RES", L"" } } });
	next->applyAp = RenderGraph::Programs::Queue("PhysicalSky", ComputeProgram::Desc{ .source = kApplyApShader, .constantWords = kApplyApWords });
	next->skyTail = RenderGraph::Programs::Queue("PhysicalSky",
		ComputeProgram::Desc{ .source = kLutShader, .constantWords = kSkyTailWords, .defines = { { L"LUTGEN", L"2" }, { L"SKY_TAIL", L"" } } });
	next->prepassGate = RenderGraph::RunGate::Create("cs.physical-sky.gate", kPrepassEntries);
	next->compositeGate = RenderGraph::RunGate::Create("cs.physical-sky.composite.gate", 1);
	next->prepassLatch = std::make_shared<org::LatchBlock>("cs.physical-sky.latch", static_cast<std::uint32_t>(sizeof(ShadowLatch)));
	impl = next;
	// Every client follows the frame revisions from the first one on.
	next->DeclareClients(next);
}

void ORGPhysicalSky::BeginFrame(PhysicalSky& a_feature)
{
	if (!IsActive() || impl->beganFrame == RenderGraph::FrameClock::BeginFrame())
		return;
	impl->beganFrame = RenderGraph::FrameClock::BeginFrame();
	// A new version of the AP shadow switched in at this frame's Begin, and its wrapper with it (RetargetWrappers). It was born zero.
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
	ShadowSeen seen;
	auto& inputs = seen.inputs;
	ShadowLatch latch{};
	if (!globals::state->isMapMenuOpen) {
		const bool hasLight = globals::deferred && globals::deferred->GetDirectionalShadowLightData(latch.light);
		latch.lightCount = hasLight ? 1u : 0u;
		if (auto* shadowMap = globals::deferred ? globals::deferred->GetDirectionalShadowMap() : nullptr; shadowMap && hasLight) {
			// The key imports it: an engine depth target's slot, or the view as its own.
			seen.shadowMap = RenderGraph::DepthViewSource::Of(shadowMap);
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
		if (const auto* last = state.shadowObserved.Latest())
			seen = *last;
	}
	// What the AP shadow reads is not a variable yet. A change is an event: the next revision registers the client again.
	state.shadowObserved.Observe(seen);

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
	if (!state.shadowObserved.Latest()) {
		ShadowSeen seen;
		seen.inputs.clouds = CloudsOnGraph(a_feature);
		state.shadowObserved.Observe(seen);
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
	// Whether the composite binds the depth copy is the key's: Dynamic Cubemaps is loaded or not for the session.
	inputs.clouds = CloudsOnGraph(a_feature);
	// Not variables yet: a change is an event, and the next revision registers the client again.
	state.compositeObserved.Observe(inputs);
	state.compositeFrame = RenderGraph::FrameClock::BeginFrame();
}

