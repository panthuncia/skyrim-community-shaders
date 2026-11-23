#include "ORGViewAtmosphereReflection.h"

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/IBL.h"
#include "Features/IBL/ORGIBL.h"
#include "Features/PhysicalSky.h"
#include "Features/PhysicalSky/ORGPhysicalSky.h"
#include "Features/PhysicalSky/ORGVolumetricClouds.h"
#include "Features/Skylighting.h"
#include "Features/Skylighting/ORGSkylighting.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainShadows/ORGTerrainShadows.h"
#include "Features/ViewAtmosphere.h"
#include "Features/ViewAtmosphere/ORGViewAtmosphere.h"
#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "State.h"
#include "Utils/Logging.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using Anchor = FrameAnchors::Anchor;
	using Bind = RenderGraph::AnchorPasses::Bind;

	constexpr const char* kClientId = "cs.view-atmosphere.reflection";
	constexpr const char* kShader = "ViewAtmosphere/Reflection.cs.hlsl";
	// After Cloud Shadows' clients in the same step, whose last layer it reads.
	constexpr std::int32_t kOrder = 100;

	// The gate's entries: the sky SH, the medium, and the clear of the sky SH when the medium stops.
	constexpr std::uint32_t kSkyEntry = 0;
	constexpr std::uint32_t kMediumEntry = 1;
	constexpr std::uint32_t kClearEntry = 2;
	constexpr std::uint32_t kEntries = 3;

	// Reflection.cs.hlsl's push block: the shared handles, the sampler, the multiscatter LUT, the headers' 16 EXTERN entries,
	// then the outputs (one for the sky SH, two for the medium).
	constexpr std::uint32_t kCommonWords = 4 + 1 + 1 + 16;
	constexpr std::uint32_t kSkyWords = kCommonWords + 1;
	constexpr std::uint32_t kMediumWords = kCommonWords + 2;

	// The medium's size and groups, as RenderReflection makes and dispatches it: two angular rays a group.
	constexpr UINT kMediumSize = 32;
	constexpr std::array<std::uint32_t, 3> kMediumGroups{ kMediumSize / 2, kMediumSize, 1 };

	using Parameters = std::array<std::uint32_t, 64>;
	constexpr std::uint32_t kParametersBytes = sizeof(Parameters);
	// Where flags sits in the parameters: after three 12-word grids and the atlas.
	constexpr std::size_t kFlagsWord = 3 * 12 + 4;

	/** @brief What the passes read besides their own resources: the client's key. Empty strings and false mean null. */
	struct ReflectionInputs
	{
		bool clouds = false;     // the clouds' shadow volume, its mean and the cloud cubes, by id
		bool ibl = false;        // IBL's environment SH, by id
		bool probes = false;     // Skylighting's probes, by id
		bool terrain = false;    // Terrain Shadows' heights, by id
		std::string cloudLayer;  // Cloud Shadows' last layer, by id

		bool operator==(const ReflectionInputs&) const = default;
	};

	/** @brief Whether this frame's readers bind the clouds' outputs: the adopted revision has them on and a cloud run wrote them. */
	bool CloudsOnGraph()
	{
		return globals::features::physicalSky.VolumetricCloudsEnabled() && ORGVolumetricClouds::OutputsWritten();
	}

	/** @brief The flags RenderReflection computes, from what the registration binds and this frame's state. */
	ViewAtmosphere::ReflectionFlags FlagsOf(const ReflectionInputs& a_inputs)
	{
		ViewAtmosphere::ReflectionFlags flags;
		flags.fog = globals::features::exponentialHeightFog.IsActive();
		flags.ibl = flags.fog && a_inputs.ibl && !globals::features::ibl.IsDisabledForCurrentScene();
		flags.probes = a_inputs.probes;
		flags.terrain = a_inputs.terrain && globals::features::terrainShadows.IsShadowMapReady();
		flags.cloudLayer = !a_inputs.cloudLayer.empty();
		return flags;
	}

	D3D11_TEXTURE3D_DESC MediumDesc()
	{
		D3D11_TEXTURE3D_DESC desc{};
		desc.Width = desc.Height = desc.Depth = kMediumSize;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		return desc;
	}

	D3D11_TEXTURE2D_DESC SkySHDesc()
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = 3;
		desc.Height = 2;  // the sky SH, then the cloud/clear-sky diffuse transport
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		return desc;
	}

	// Copies the run's parameters from the latch into the published buffer. A run without the medium copies the zeros of no medium.
	struct ParametersBindings
	{
		org::ResourceBindingToken buffer;
	};

	struct ParametersFrame
	{
		std::shared_ptr<const org::LatchBlock> latch;
	};

	class ParametersPass final : public org::TypedRenderGraphPass<ParametersPass, ParametersFrame, ParametersBindings>
	{
	public:
		ParametersPass(std::shared_ptr<org::Resource> a_buffer, std::shared_ptr<const org::LatchBlock> a_latch) :
			buffer(std::move(a_buffer)), latch(std::move(a_latch)) {}

		ParametersBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.CopyDestination(buffer) };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(latch.get())); }

		ParametersFrame Prepare(const ParametersBindings&, const org::PassPrepareContext&) const { return { latch }; }

		static void Record(const ParametersBindings& a_bindings, const ParametersFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto region = a_frame.latch->At(a_recording.Run());
			a_recording.Commands().CopyBufferRegion(a_recording.Resolve(a_bindings.buffer).GetHandle(), 0, region.buffer->GetAPIResource().GetHandle(),
				region.offset, kParametersBytes);
		}

	private:
		std::shared_ptr<org::Resource> buffer;
		std::shared_ptr<const org::LatchBlock> latch;
	};
}

struct ORGViewAtmosphereReflection::Impl
{
	RenderGraph::Programs::Fixed<ComputeProgram> skyProgram, mediumProgram;
	// RenderReflection's sampler: linear, wrapping in azimuth.
	org::OwnedDescriptorBinding sampler;
	std::shared_ptr<RenderGraph::RunGate> gate;
	std::shared_ptr<org::LatchBlock> latch;
	// The null descriptor, for inputs D3D11 binds null: it reads zero and has zero size, as a null SRV does.
	std::uint32_t nullIndex = DCLF::GpuTextures::kInvalid;
	std::shared_ptr<const void> nullOwner;

	// Made at Setup and published for the session. One version each.
	RenderGraph::Frame::DeclaredTexture3D luminance, transmittance;
	RenderGraph::Frame::DeclaredTexture2D skySH;
	std::unique_ptr<GraphResources::StructuredBuffer> parameters;

	// Each registration keeps the inputs it binds. The commit reads the held one's, so its flags name only what it binds.
	RenderGraph::Frame::DeclaredClientOf<const ReflectionInputs> client;
	RenderGraph::Frame::Observed<ReflectionInputs> observed{ "ViewAtmosphere.Reflection" };

	bool transportWritten = false;  // the sky SH holds a run's output, so it is cleared when the medium stops
	bool reportedRun = false;       // the first run is logged once
	std::uint32_t ranFrame = UINT32_MAX;  // the frame whose first cubemap render ran the client

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

	void MakeOutputs()
	{
		using namespace RenderGraph::Frame;
		// Graph readers only (DCLF and the passes read the ids), so nothing is exported to D3D11. Each is born as "no medium".
		const auto persistent = [](Birth a_birth) {
			return GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .birth = a_birth };
		};
		luminance = DeclareTexture3D(std::string(kLuminanceId), Dependencies{}, [](const Inputs&) { return MediumDesc(); }, persistent(Birth::Zero()));
		transmittance = DeclareTexture3D(std::string(kTransmittanceId), Dependencies{}, [](const Inputs&) { return MediumDesc(); }, persistent(Birth::Value({ 1.0f, 1.0f, 1.0f, 1.0f })));
		skySH = DeclareTexture2D(std::string(kSkySHId), Dependencies{}, [](const Inputs&) { return SkySHDesc(); }, persistent(Birth::Zero()));
		D3D11_BUFFER_DESC bufferDesc{};
		bufferDesc.Usage = D3D11_USAGE_DEFAULT;
		bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bufferDesc.StructureByteStride = kParametersBytes;
		bufferDesc.ByteWidth = kParametersBytes;
		parameters = std::make_unique<GraphResources::StructuredBuffer>(bufferDesc, 1, std::string(kParametersId),
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
		if (!*parameters || !luminance || !*luminance || !transmittance || !*transmittance || !skySH || !*skySH)
			Fatal("the reflection medium's resources could not be made");
		if (!PublishedResources::Publish(*luminance) || !PublishedResources::Publish(*transmittance) || !PublishedResources::Publish(*parameters) ||
			!PublishedResources::Publish(*skySH))
			Fatal("the reflection medium could not be published");
	}

	/** @brief What the passes can read now. */
	static ReflectionInputs InputsNow()
	{
		ReflectionInputs inputs;
		inputs.clouds = CloudsOnGraph();
		const auto& ibl = globals::features::ibl;
		inputs.ibl = ibl.loaded && ibl.settings.EnableIBL.Get() != 0 && PublishedResources::IsDeclarable(ORGIBL::kEnvId);
		inputs.probes = globals::features::skylighting.loaded && PublishedResources::IsDeclarable(ORGSkylighting::kProbeArrayId);
		auto& terrain = globals::features::terrainShadows;
		inputs.terrain = terrain.IsShadowMapReady() && terrain.texShadowHeight &&
		                 PublishedResources::IsExportOf(terrain.texShadowHeight->srv.get(), ORGTerrainShadows::kShadowHeightId);
		if (globals::features::cloudShadows.loaded)
			inputs.cloudLayer = ORGCloudShadows::PublishedLayerId(CloudShadows::kMaxCloudLayers - 1);
		return inputs;
	}

	/** @brief D3D11's conditions in RenderReflection, for this frame. The registration must bind the clouds exactly when they are on. */
	static bool RunsNow(const ReflectionInputs& a_inputs)
	{
		const auto& atmosphere = globals::features::viewAtmosphere;
		const auto& graph = ORGViewAtmosphere::Get();
		// D3D11's ready: the shared volume ran. Its composite must be on, as CompositeEnabled() asks.
		if (!atmosphere.loaded || atmosphere.settings.enabled.Adopted() == 0 || !graph.SharedVolumeActive() || !graph.CompositeActive())
			return false;
		if (!globals::features::physicalSky.cbData.enabled || globals::state->isMapMenuOpen)
			return false;
		// Readers wait for the cloud client (physical-sky-port.md open question 24): with the clouds on, only a registration that
		// binds their outputs runs, once they are written.
		return a_inputs.clouds == CloudsOnGraph();
	}

	void Commit()
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		ranFrame = globals::state->frameCount;
		const auto held = client.Held(Anchor::ReflectionsPrepass);
		Parameters words{};
		if (held && RunsNow(*held)) {
			words = ViewAtmosphere::ReflectionParameters(FlagsOf(*held));
			gate->OpenDispatch(kSkyEntry, 1, 1, 1);
			gate->OpenDispatch(kMediumEntry, kMediumGroups[0], kMediumGroups[1], kMediumGroups[2]);
			transportWritten = true;
			if (!std::exchange(reportedRun, true))
				logger::info("[ORG] Atmosphere and Fog: the first reflection medium runs at frame {} (flags.y {}, flags.z {}, clouds {})", globals::state->frameCount,
					words[kFlagsWord + 1], words[kFlagsWord + 2], held->clouds);
		} else if (std::exchange(transportWritten, false)) {
			// D3D11's CloudDiffuseTransport() is null without the medium. Zero reads as no transport.
			gate->OpenDispatch(kClearEntry, 1, 1, 1);
		}
		// Every run copies the parameters, so the faces read no medium on runs without one.
		latch->WriteValue(host->CurrentRun(), 0, words);
	}

	/** @brief The registration for a key, when a revision starts. */
	bool Make(const ReflectionInputs& a_inputs, FrameAnchors::ClientDesc& a_client, const std::shared_ptr<Impl>& a_self)
	{
		namespace AnchorPasses = RenderGraph::AnchorPasses;
		MakeNull();
		const auto null = Bind::Handle(nullIndex);
		const auto cloud = [&](std::string_view a_id) { return a_inputs.clouds ? Bind::Published(a_id) : null; };
		// In Reflection.cs.hlsl's list order: its own resources, then the headers'. The transport (t70), the environment SH (t76) and
		// the sky SH (t77) follow, then the rest of the headers' and the outputs.
		const std::vector<Bind> common{ Bind::Shared(), Bind::Handle(sampler.Index()), Bind::Published(ORGPhysicalSky::kMultiscatterLutId), Bind::Published(kParametersId),
			Bind::Published(ORGPhysicalSky::kTransmittanceLutId), Bind::Published(ORGPhysicalSky::kSkyViewLutId), Bind::Published(ORGPhysicalSky::kApLutId),
			Bind::Published(ORGPhysicalSky::kDiskPhaseLutId), cloud(ORGVolumetricClouds::kShadowVolumeId), cloud(ORGVolumetricClouds::kCloudShadowMeanId),
			cloud(ORGVolumetricClouds::kVolCubeTrId), cloud(ORGVolumetricClouds::kVolCubeLumId), cloud(ORGVolumetricClouds::kVolCubeDepthId) };
		const auto environment = a_inputs.ibl ? Bind::Published(ORGIBL::kEnvId) : null;
		const std::vector<Bind> tail{ a_inputs.probes ? Bind::Published(ORGSkylighting::kProbeArrayId) : null,
			a_inputs.terrain ? Bind::Published(ORGTerrainShadows::kShadowHeightId) : null,
			a_inputs.cloudLayer.empty() ? null : Bind::Published(a_inputs.cloudLayer, static_cast<std::uint32_t>(org::SRVViewType::TextureCube)) };
		// The sky SH pass reads neither the transport nor a sky SH, as RenderReflection binds them null.
		auto skyBindings = common;
		skyBindings.insert(skyBindings.end(), { null, environment, null });
		skyBindings.insert(skyBindings.end(), tail.begin(), tail.end());
		skyBindings.push_back(Bind::Uav(*skySH));
		// The medium reads the sky SH as IBL's sky SH, and its second row as the cloud diffuse transport.
		auto mediumBindings = common;
		mediumBindings.insert(mediumBindings.end(), { Bind::Srv(*skySH), environment, Bind::Srv(*skySH) });
		mediumBindings.insert(mediumBindings.end(), tail.begin(), tail.end());
		mediumBindings.push_back(Bind::Uav(*luminance));
		mediumBindings.push_back(Bind::Uav(*transmittance));

		AnchorPasses::Builder builder(Anchor::ReflectionsPrepass, kClientId, kOrder, {}, gate);
		builder.Add("parameters", FrameAnchors::PassKind::Copy, [buffer = parameters->Graph(), latch = std::shared_ptr<const org::LatchBlock>(latch)] {
			return std::make_shared<ParametersPass>(buffer, latch);
		});
		builder.Add(AnchorPasses::ClearOf("sky-sh.clear", *skySH, {}, kClearEntry));
		builder.Add(AnchorPasses::Dispatch{ .name = "sky-sh", .program = skyProgram.Get(), .bindings = std::move(skyBindings), .gate = kSkyEntry });
		builder.Add(AnchorPasses::Dispatch{ .name = "medium", .program = mediumProgram.Get(), .bindings = std::move(mediumBindings), .gate = kMediumEntry });
		if (builder.Failed()) {
			logger::error("[ORG] Atmosphere and Fog: the reflection medium's passes: {}", builder.Failure());
			return false;
		}
		a_client.id = kClientId;
		a_client.sharedConstants = builder.ReadsSharedConstants();
		a_client.passes = builder.Take();
		a_client.gates = { gate };
		a_client.registerResources = [state = a_self](org::RenderGraph& a_graph) {
			state->luminance->Register(a_graph);
			state->transmittance->Register(a_graph);
			state->skySH->Register(a_graph);
			state->parameters->Register(a_graph);
		};
		// The anchor runs at every cubemap render, and the recipe holds one instance a frame. So only the frame's first render runs
		// the client: the medium's inputs do not change within a frame. D3D11 builds the same medium again at each render.
		a_client.active = [state = a_self] { return state->ranFrame != globals::state->frameCount; };
		a_client.commit = [state = a_self](const FrameAnchors::Point&) { state->Commit(); };
		// Every registration commits the same run, so the one the schedule holds keeps running until a newer one is adopted (by id).
		logger::info("[ORG] Atmosphere and Fog: a reflection medium for the next revision ({}; clouds {}, IBL {}, probes {}, terrain heights {}, cloud layer {})",
			kClientId, a_inputs.clouds, a_inputs.ibl, a_inputs.probes, a_inputs.terrain, a_inputs.cloudLayer.empty() ? "none" : a_inputs.cloudLayer);
		return true;
	}

	/**
	* @brief The client, declared on the cloud toggle and on what the last cubemap render found. It is always registered, even with
	* the features off: its copy then gives the faces the zeros of no medium from the first cubemap render on, since the parameters
	* buffer is born undefined. The commit checks the toggles each run.
	*/
	void DeclareReflectionClient(const std::shared_ptr<Impl>& a_self)
	{
		using namespace RenderGraph::Frame;
		const std::weak_ptr<Impl> weak = a_self;
		client = DeclareClient<ReflectionInputs, const ReflectionInputs>(
			kClientId, DependsOn(globals::features::physicalSky.settings.enableVolumetricClouds, observed.Var()),
			[weak](const Inputs& a_in) -> std::optional<ReflectionInputs> {
				const auto state = weak.lock();
				if (!state)
					return std::nullopt;
				// Before the first cubemap render, a registration that binds no optional input.
				const auto* seen = state->observed.Get(a_in);
				auto inputs = seen ? *seen : ReflectionInputs{};
				// The clouds' outputs only with the clouds on in the same revision.
				inputs.clouds = inputs.clouds && a_in.Get(globals::features::physicalSky.settings.enableVolumetricClouds);
				return inputs;
			},
			[weak](const ReflectionInputs& a_inputs, FrameAnchors::ClientDesc& a_client) -> std::shared_ptr<const ReflectionInputs> {
				const auto state = weak.lock();
				if (!state || !state->Make(a_inputs, a_client, state))
					return nullptr;
				return std::make_shared<const ReflectionInputs>(a_inputs);
			});
	}
};

ORGViewAtmosphereReflection::ORGViewAtmosphereReflection() = default;
ORGViewAtmosphereReflection::~ORGViewAtmosphereReflection() = default;

void ORGViewAtmosphereReflection::Setup()
{
	// The medium needs Physical Sky's LUTs, as RenderReflection does.
	if (!globals::features::physicalSky.loaded || !BeginSetup())
		return;
	auto state = std::make_shared<Impl>();
	// The defines RenderReflection compiles with. Whether a feature is loaded is fixed for the session.
	std::vector<std::pair<std::wstring, std::wstring>> defines{ { L"PHYSICAL_SKY", L"" } };
	if (globals::features::exponentialHeightFog.loaded)
		defines.emplace_back(L"EXP_HEIGHT_FOG", L"");
	if (globals::features::terrainShadows.loaded)
		defines.emplace_back(L"TERRAIN_SHADOWS", L"");
	if (globals::features::cloudShadows.loaded)
		defines.emplace_back(L"CLOUD_SHADOWS", L"");
	state->mediumProgram = RenderGraph::Programs::Queue("ViewAtmosphere", ComputeProgram::Desc{ .source = kShader, .constantWords = kMediumWords, .defines = defines });
	defines.emplace_back(L"REFLECTION_SKY_SH", L"");
	state->skyProgram = RenderGraph::Programs::Queue("ViewAtmosphere", ComputeProgram::Desc{ .source = kShader, .constantWords = kSkyWords, .defines = defines });

	D3D11_SAMPLER_DESC sampler{};
	sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	state->sampler = ShaderBindings::CreateSamplerFromD3D11(sampler);
	if (!state->sampler)
		Fatal("the reflection sampler could not be made");
	state->gate = RenderGraph::RunGate::Create("cs.view-atmosphere.reflection.gate", kEntries);
	state->latch = std::make_shared<org::LatchBlock>("cs.view-atmosphere.reflection.latch", kParametersBytes);
	// Before the first frame, since DCLF's faces read the ids from their first frame on.
	state->MakeOutputs();
	impl = state;
	state->DeclareReflectionClient(state);
	logger::info("[ORG] Atmosphere and Fog: the reflection medium is on the render graph ({} at the ReflectionsPrepass anchor)", kClientId);
}

void ORGViewAtmosphereReflection::Observe()
{
	if (!impl)
		return;
	// Not a variable yet. A change is an event, and the next revision registers the client for it.
	impl->observed.Observe(Impl::InputsNow());
}

bool ORGViewAtmosphereReflection::TransportPublished() const
{
	return impl && impl->skySH;
}
