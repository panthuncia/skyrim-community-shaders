#include "ORGViewAtmosphereVolume.h"

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DynamicCubemaps.h"
#include "Features/DynamicCubemaps/ORGDynamicCubemaps.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/IBL.h"
#include "Features/IBL/ORGIBL.h"
#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/ORGLightCulling.h"
#include "Features/LightLimitFix/ORGLocalShadows.h"
#include "Features/PhysicalSky.h"
#include "Features/PhysicalSky/ORGPhysicalSky.h"
#include "Features/PhysicalSky/ORGVolumetricClouds.h"
#include "Features/Skylighting.h"
#include "Features/Skylighting/ORGSkylighting.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainShadows/ORGTerrainShadows.h"
#include "Features/ViewAtmosphere/ORGViewAtmosphereReflection.h"
#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/DepthViewSource.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/FrameClock.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "State.h"
#include "Utils/Game.h"

// The handle tables' names, in the shaders' order (view-atmosphere-port.md 3.6).
#include "../../../features/Atmosphere and Fog/Shaders/ViewAtmosphere/MediumTableList.hlsli"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <format>

namespace
{
	using Bind = RenderGraph::AnchorPasses::Bind;
	constexpr std::string_view kName = "Atmosphere and Fog";
	constexpr const char* kInjectShader = "ViewAtmosphere/LightScattering.cs.hlsl";
	constexpr const char* kIntegrateShader = "ViewAtmosphere/Integrate.cs.hlsl";
	// LightScattering.cs.hlsl's push block: the shared handles, the table, the engine local shadows' latch.
	constexpr std::uint32_t kInjectWords = 4 + 2 + 2;
	// Integrate.cs.hlsl's: the shared handles and the table.
	constexpr std::uint32_t kIntegrateWords = 4 + 2;
	constexpr std::uint32_t kGrids = 3;

	namespace InjectSlot
	{
		enum Value : std::uint32_t
		{
#define VIEW_ATMOSPHERE_SLOT(N) N,
			VIEW_ATMOSPHERE_INJECT_TABLE(VIEW_ATMOSPHERE_SLOT)
#undef VIEW_ATMOSPHERE_SLOT
				Count
		};
	}

	namespace IntegrateSlot
	{
		enum Value : std::uint32_t
		{
#define VIEW_ATMOSPHERE_SLOT(N) N,
			VIEW_ATMOSPHERE_INTEGRATE_TABLE(VIEW_ATMOSPHERE_SLOT)
#undef VIEW_ATMOSPHERE_SLOT
				Count
		};
	}

	/** @brief The latch: the directional shadow light (copied into the t98 buffer), then the engine local shadows (b1). */
	struct LatchData
	{
		Deferred::DirectionalShadowLightData light{};
		ExponentialHeightFog::EngineLocalShadows engineShadows{};
	};
	constexpr std::uint32_t kEngineShadowsOffset = static_cast<std::uint32_t>(offsetof(LatchData, engineShadows));
	static_assert(sizeof(Deferred::DirectionalShadowLightData) == ORGViewAtmosphereVolume::kLightBytes && kEngineShadowsOffset % 16 == 0 &&
					  sizeof(ExponentialHeightFog::EngineLocalShadows) == 4 * 112,
		"LightScattering.cs.hlsl's latch layout");

	/** @brief Gate entries: per direction (which texture of the pair is the history), the three injects, then the three integrates. */
	constexpr std::uint32_t Entry(std::uint32_t a_history, bool a_integrate, std::uint32_t a_grid)
	{
		return a_history * 2 * kGrids + (a_integrate ? kGrids : 0) + a_grid;
	}
	constexpr std::uint32_t kEntries = 2 * 2 * kGrids;

	/** @brief What the last Prepass found that is no variable yet (B2's interim). The key imports the depth views. */
	struct Seen
	{
		RenderGraph::DepthViewSource shadowMap;
		RenderGraph::DepthViewSource engineShadows;
		std::string terrain;
		std::string cloudLayer;
		bool clouds = false;
		bool localShadows = false;

		bool operator==(const Seen&) const = default;
	};

	/** @brief What Stage hands to the commit. */
	struct Staged
	{
		ViewAtmosphere::Parameters parameters{};
		LatchData latch{};
		std::uint32_t history = 0;  // the texture of the pair the run reads
	};
}

std::uint32_t ORGViewAtmosphereVolume::Inputs::Bound() const
{
	return ViewAtmosphere::InputFlags{ .directionalShadow = static_cast<bool>(shadowMap),
		.ibl = ibl,
		.probes = probes,
		.localLights = lights,
		.localShadows = lights && localShadows,
		.engineShadows = static_cast<bool>(engineShadows),
		.terrain = !terrain.empty(),
		.cloudLayer = !cloudLayer.empty(),
		.environment = environment }
	    .Bits();
}

struct ORGViewAtmosphereVolume::Impl
{
	RenderGraph::Programs::Fixed<ComputeProgram> inject, integrate;
	org::OwnedDescriptorBinding linearSampler, shadowSampler;
	// The null descriptor, for what D3D11 leaves unbound: it reads zero, as an unbound register does.
	std::uint32_t nullIndex = DCLF::GpuTextures::kInvalid;
	std::shared_ptr<const void> nullOwner;
	std::shared_ptr<org::LatchBlock> latch;
	std::shared_ptr<RenderGraph::RunGate> gate;
	std::unique_ptr<GraphResources::StructuredBuffer> lightBuffer;
	// The ping-pong pair, sized as the atlas. Each version is born zero, which Written keeps from being read as history.
	std::array<RenderGraph::Frame::DeclaredTexture3D, 2> pair;

	RenderGraph::Frame::Observed<Seen> observed{ "ViewAtmosphere.Volume" };
	std::optional<Staged> staged;
	// The last committed run: the texture it wrote, its version, its frame, and its grids and fog flag (D3D11's "changed").
	std::uint32_t written = 0;
	const void* committedVersion = nullptr;
	std::optional<std::uint32_t> committedWritten;
	std::uint64_t lastFrame = UINT64_MAX;  // the render frame (FrameClock) of the last run
	ViewAtmosphere::Parameters last{};
	std::uint32_t lastShadowBits = 0;
	bool reported = false;

	[[noreturn]] static void Fatal(std::string_view a_reason) { RenderGraph::Fatal(kName, a_reason); }
};

ORGViewAtmosphereVolume::ORGViewAtmosphereVolume() = default;
ORGViewAtmosphereVolume::~ORGViewAtmosphereVolume() = default;

void ORGViewAtmosphereVolume::Setup(const AtlasDeclaration& a_atlas)
{
	auto state = std::make_unique<Impl>();
	// As ViewAtmosphere::Prepare compiles them.
	std::vector<std::pair<std::string, std::string>> injectDefines{ { "PHYSICAL_SKY", "" } };
	if (globals::features::exponentialHeightFog.loaded)
		injectDefines.emplace_back("EXP_HEIGHT_FOG", "");
	if (globals::features::lightLimitFix.loaded)
		injectDefines.emplace_back("LIGHT_LIMIT_FIX", "");
	if (globals::features::terrainShadows.loaded)
		injectDefines.emplace_back("TERRAIN_SHADOWS", "");
	if (globals::features::cloudShadows.loaded)
		injectDefines.emplace_back("CLOUD_SHADOWS", "");
	state->inject = RenderGraph::Programs::Queue("ViewAtmosphere",
		ComputeProgram::Desc{ .source = kInjectShader, .constantWords = kInjectWords, .defines = RenderGraph::AnchorPasses::Defines(injectDefines) });
	state->integrate = RenderGraph::Programs::Queue("ViewAtmosphere",
		ComputeProgram::Desc{ .source = kIntegrateShader, .constantWords = kIntegrateWords, .defines = RenderGraph::AnchorPasses::Defines({ { "PHYSICAL_SKY", "" } }) });
	state->linearSampler = ShaderBindings::CreateSamplerFromD3D11(ViewAtmosphere::SamplerDesc(false));
	state->shadowSampler = ShaderBindings::CreateSamplerFromD3D11(ViewAtmosphere::SamplerDesc(true));
	if (!state->linearSampler || !state->shadowSampler)
		Impl::Fatal("the shared volume's samplers could not be made");
	state->latch = std::make_shared<org::LatchBlock>("cs.view-atmosphere.volume.latch", static_cast<std::uint32_t>(sizeof(LatchData)));
	state->gate = RenderGraph::RunGate::Create("cs.view-atmosphere.volume.gate", kEntries);

	D3D11_BUFFER_DESC bufferDesc{};
	bufferDesc.Usage = D3D11_USAGE_DEFAULT;
	bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	bufferDesc.StructureByteStride = kLightBytes;
	bufferDesc.ByteWidth = kLightBytes;
	state->lightBuffer = std::make_unique<GraphResources::StructuredBuffer>(bufferDesc, 1, "ViewAtmosphere::DirectionalShadowLights",
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::Transient });
	if (!*state->lightBuffer)
		Impl::Fatal("the shadow light buffer could not be made");

	// Graph only, never published: the inject pass alone reads them.
	const GraphResources::Options history{ .lifetime = GraphResources::Lifetime::History, .versioned = true };
	for (std::uint32_t i = 0; i < 2; ++i) {
		state->pair[i] = RenderGraph::Frame::DeclareTexture3D(std::format("ViewAtmosphere::Source{}", i == 0 ? 'A' : 'B'), a_atlas.dependencies, a_atlas.desc,
			history, a_atlas.exists);
		if (!state->pair[i] || !*state->pair[i])
			Impl::Fatal("the shared volume's source textures could not be made");
	}
	impl = std::move(state);
}

RenderGraph::Frame::Dependencies ORGViewAtmosphereVolume::Dependencies() const
{
	return RenderGraph::Frame::DependsOn(impl->observed.Var(), globals::features::ibl.settings.EnableIBL);
}

ORGViewAtmosphereVolume::Inputs ORGViewAtmosphereVolume::KeyAt(const RenderGraph::Frame::Inputs& a_in) const
{
	Inputs inputs;
	if (const auto* seen = impl->observed.Get(a_in)) {
		inputs.shadowMap = seen->shadowMap.Resolve("cs.view-atmosphere.directional-shadow-map");
		inputs.engineShadows = seen->engineShadows.Resolve("cs.view-atmosphere.engine-shadow-maps");
		inputs.terrain = seen->terrain;
		inputs.cloudLayer = seen->cloudLayer;
		inputs.clouds = seen->clouds;
		inputs.localShadows = seen->localShadows;
	}
	// Published at setup, for the session, while their features are loaded.
	inputs.lights = globals::features::lightLimitFix.loaded;
	const auto& ibl = globals::features::ibl;
	inputs.ibl = ibl.loaded && a_in.Get(ibl.settings.EnableIBL) != 0;
	inputs.probes = globals::features::skylighting.loaded;
	inputs.environment = globals::features::dynamicCubemaps.loaded && PublishedResources::IsDeclarable(ORGDynamicCubemaps::kEnvBC6HId);
	return inputs;
}

void ORGViewAtmosphereVolume::AddPasses(const Inputs& a_inputs, RenderGraph::AnchorPasses::Builder& a_builder, const GraphResources::Resource& a_luminance,
	const GraphResources::Resource& a_transmittance, const GraphResources::Resource& a_parameters)
{
	// Made once, at the first registration, as Physical Sky's passes make it.
	if (impl->nullIndex == DCLF::GpuTextures::kInvalid) {
		auto binding = DCLF::GpuTextures::Get().NullBinding();
		if (!binding)
			Impl::Fatal("the null descriptor could not be made");
		impl->nullIndex = binding.index;
		impl->nullOwner = std::move(binding.owner);
	}
	const auto null = Bind::Handle(impl->nullIndex);
	const auto published = [&](bool a_bound, std::string_view a_id) { return a_bound ? Bind::Published(a_id) : null; };

	// The inject table, as ViewAtmosphere::Render binds it. The pair's entries and the grid are set per dispatch.
	std::vector<Bind> injectTable(InjectSlot::Count, null);
	injectTable[InjectSlot::LinearSampler] = Bind::Handle(impl->linearSampler.Index());
	injectTable[InjectSlot::ShadowSampler] = Bind::Handle(impl->shadowSampler.Index());
	injectTable[InjectSlot::DirectionalShadowMap] = a_inputs.shadowMap ? Bind::SrvArray(a_inputs.shadowMap) : null;
	injectTable[InjectSlot::MediumMsLut] = Bind::Published(ORGPhysicalSky::kMultiscatterLutId);
	injectTable[InjectSlot::DirectionalShadowLights] = Bind::Srv(*impl->lightBuffer);
	injectTable[InjectSlot::ViewAtmosphereData] = Bind::Srv(a_parameters);
	injectTable[InjectSlot::TexTrLut] = Bind::Published(ORGPhysicalSky::kTransmittanceLutId);
	injectTable[InjectSlot::TexDiskPhaseLut] = Bind::Published(ORGPhysicalSky::kDiskPhaseLutId);
	// The clouds' outputs, once a cloud run wrote them (physical-sky-port.md open question 24).
	injectTable[InjectSlot::TexShadowVolume] = published(a_inputs.clouds, ORGVolumetricClouds::kShadowVolumeId);
	injectTable[InjectSlot::MeanProfile] = published(a_inputs.clouds, ORGVolumetricClouds::kCloudShadowMeanId);
	// The cloud diffuse transport (D3D11's CloudDiffuseTransport(), t70) is the reflection sky SH's second row. It is born zero
	// and cleared when the reflection medium stops, which reads as no transport.
	injectTable[InjectSlot::TexCloudDiffuseTransport] =
		ORGViewAtmosphereReflection::Get().TransportPublished() ? Bind::Published(ORGViewAtmosphereReflection::kSkySHId) : null;
	injectTable[InjectSlot::ShadowHeightTexture] = a_inputs.terrain.empty() ? null : Bind::Published(a_inputs.terrain);
	injectTable[InjectSlot::CloudShadowsTexture] =
		a_inputs.cloudLayer.empty() ? null : Bind::Published(a_inputs.cloudLayer, static_cast<std::uint32_t>(org::SRVViewType::TextureCube));
	injectTable[InjectSlot::EnvIBLTexture] = published(a_inputs.ibl, ORGIBL::kEnvId);
	injectTable[InjectSlot::SkyIBLTexture] = published(a_inputs.ibl, ORGIBL::kSkyId);
	injectTable[InjectSlot::MediumEnvironment] = published(a_inputs.environment, ORGDynamicCubemaps::kEnvBC6HId);
	injectTable[InjectSlot::SkylightingProbeArray] = published(a_inputs.probes, ORGSkylighting::kProbeArrayId);
	injectTable[InjectSlot::lights] = published(a_inputs.lights, ORGLightCulling::kLightsId);
	injectTable[InjectSlot::lightList] = published(a_inputs.lights, ORGLightCulling::kLightIndexListId);
	injectTable[InjectSlot::lightGrid] = published(a_inputs.lights, ORGLightCulling::kLightGridId);
	injectTable[InjectSlot::LocalShadows] = published(a_inputs.lights && a_inputs.localShadows, ORGLocalShadows::kDataId);
	injectTable[InjectSlot::LocalShadowMaps] = a_inputs.lights && a_inputs.localShadows ?
	                                               Bind::Published(ORGLocalShadows::kCacheId, static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull)) :
	                                               null;
	injectTable[InjectSlot::EngineLocalShadowMaps] = a_inputs.engineShadows ? Bind::SrvArray(a_inputs.engineShadows) : null;

	std::vector<Bind> integrateTable(IntegrateSlot::Count, null);
	integrateTable[IntegrateSlot::IntegratedLuminance] = Bind::Uav(a_luminance);
	integrateTable[IntegrateSlot::IntegratedTransmittance] = Bind::Uav(a_transmittance);
	integrateTable[IntegrateSlot::ViewAtmosphereData] = Bind::Srv(a_parameters);
	integrateTable[IntegrateSlot::SkylightingProbeArray] = published(a_inputs.probes, ORGSkylighting::kProbeArrayId);

	const std::vector<Bind> injectPush{ Bind::Shared(), Bind::Table(), Bind::Latch(kEngineShadowsOffset) };
	const std::vector<Bind> integratePush{ Bind::Shared(), Bind::Table() };
	const auto injectProgram = impl->inject.Get();
	const auto integrateProgram = impl->integrate.Get();
	// Both directions: the gate opens the one whose history the last run wrote. Inject all grids, then integrate them, as D3D11 does.
	for (std::uint32_t history = 0; history < 2; ++history) {
		const auto& from = *impl->pair[history];
		const auto& to = *impl->pair[1 - history];
		const char direction = history == 0 ? 'a' : 'b';
		for (std::uint32_t grid = 0; grid < kGrids; ++grid) {
			auto table = injectTable;
			table[InjectSlot::MediumLevel] = Bind::Handle(grid);
			table[InjectSlot::MediumHistory] = Bind::Srv(from);
			table[InjectSlot::MediumSource] = Bind::Uav(to);
			a_builder.Add(RenderGraph::AnchorPasses::Dispatch{
				.name = std::format("{}.inject.{}", direction, grid), .program = injectProgram, .bindings = injectPush, .gate = Entry(history, false, grid), .table = std::move(table) });
		}
		for (std::uint32_t grid = 0; grid < kGrids; ++grid) {
			auto table = integrateTable;
			table[IntegrateSlot::MediumLevel] = Bind::Handle(grid);
			table[IntegrateSlot::MediumSource] = Bind::Srv(to);
			a_builder.Add(RenderGraph::AnchorPasses::Dispatch{ .name = std::format("{}.integrate.{}", direction, grid),
				.program = integrateProgram,
				.bindings = integratePush,
				.gate = Entry(history, true, grid),
				.table = std::move(table) });
		}
	}
}

std::shared_ptr<const org::LatchBlock> ORGViewAtmosphereVolume::Latch() const
{
	return impl->latch;
}

const GraphResources::StructuredBuffer& ORGViewAtmosphereVolume::LightBuffer() const
{
	return *impl->lightBuffer;
}

std::shared_ptr<RenderGraph::RunGate> ORGViewAtmosphereVolume::Gate() const
{
	return impl->gate;
}

void ORGViewAtmosphereVolume::RegisterResources(org::RenderGraph& a_graph) const
{
	impl->lightBuffer->Register(a_graph);
	impl->pair[0]->Register(a_graph);
	impl->pair[1]->Register(a_graph);
}

void ORGViewAtmosphereVolume::Observe()
{
	// As ViewAtmosphere::Render binds them.
	Seen seen;
	Deferred::DirectionalShadowLightData light{};
	if (globals::deferred && globals::deferred->GetDirectionalShadowLightData(light))
		seen.shadowMap = RenderGraph::DepthViewSource::Of(globals::deferred->GetDirectionalShadowMap());
	auto& lights = globals::features::lightLimitFix;
	if (lights.loaded) {
		if (lights.IsLocalShadowCacheActive() && lights.GetLocalShadowDataSRV()) {
			if (!PublishedResources::IsExportOf(lights.GetLocalShadowDataSRV(), ORGLocalShadows::kDataId) ||
				!PublishedResources::IsExportOf(lights.GetLocalShadowCacheSRV(), ORGLocalShadows::kCacheId))
				Impl::Fatal("Light Limit Fix's local shadows are on D3D11");
			seen.localShadows = true;
		}
		ExponentialHeightFog::EngineLocalShadows shadows{};
		seen.engineShadows = RenderGraph::DepthViewSource::Of(globals::features::exponentialHeightFog.BuildEngineLocalShadowData(shadows));
	}
	auto& terrain = globals::features::terrainShadows;
	if (terrain.IsShadowMapReady()) {
		if (!PublishedResources::IsExportOf(terrain.GetShadowHeightSRV(), ORGTerrainShadows::kShadowHeightId))
			Impl::Fatal("Terrain Shadows' heights are on D3D11");
		seen.terrain = ORGTerrainShadows::kShadowHeightId;
	}
	if (globals::features::cloudShadows.loaded)
		seen.cloudLayer = ORGCloudShadows::PublishedLayerId(CloudShadows::kMaxCloudLayers - 1);
	auto& sky = globals::features::physicalSky;
	seen.clouds = sky.loaded && sky.VolumetricCloudsEnabled() && ORGVolumetricClouds::OutputsWritten();
	impl->observed.Observe(seen);
}

ViewAtmosphere::Parameters ORGViewAtmosphereVolume::Stage()
{
	auto& state = *impl;
	auto& atmosphere = globals::features::viewAtmosphere;
	const auto& adopted = RenderGraph::Frame::Revision::Adopted();
	const auto& s = atmosphere.settings;
	// The atlas of the adopted revision (ORGViewAtmosphere's AtlasDesc), so the grids fit the bound versions. Sized for the display;
	// the grids cover the whole view, so dynamic resolution needs no other size.
	const auto display = adopted.Get(RenderGraph::Frame::Vars::DisplaySize());
	const auto layout = ViewAtmosphere::LayoutFor(float2{ static_cast<float>(display.width), static_cast<float>(display.height) }, adopted.Get(s.nearPixelSize),
		adopted.Get(s.middlePixelSize), adopted.Get(s.distantPixelSize), adopted.Get(s.nearSlices), adopted.Get(s.middleSlices), adopted.Get(s.distantSlices));
	auto& fog = globals::features::exponentialHeightFog;
	const bool useFog = fog.IsActive();

	Staged staged;
	// The run reads the texture the last committed run wrote. Until a submitted run wrote it, it holds its birth only.
	staged.history = state.written;
	auto parameters = atmosphere.ParametersFor(layout, useFog, false);
	const bool changed = std::memcmp(state.last.grids, parameters.grids, sizeof(parameters.grids)) != 0 ||
	                     std::memcmp(&state.last.atlas, &parameters.atlas, sizeof(parameters.atlas)) != 0 || state.last.flags.y != parameters.flags.y;
	const auto frame = RenderGraph::FrameClock::BeginFrame();
	const bool continuous = !changed && state.lastFrame != UINT64_MAX && state.lastFrame + 1u == frame && adopted.Written(state.pair[staged.history]);
	parameters.history = atmosphere.ParametersFor(layout, useFog, continuous).history;

	// flags.z as Render finds them. The commit clears what the registration it runs does not bind.
	auto& lights = globals::features::lightLimitFix;
	auto& ibl = globals::features::ibl;
	auto& cubemaps = globals::features::dynamicCubemaps;
	const bool hasLight = globals::deferred && globals::deferred->GetDirectionalShadowLightData(staged.latch.light);
	const bool localLights = useFog && fog.settings.volumetricFogEnabled && lights.loaded;
	const bool engineShadows = localLights && fog.BuildEngineLocalShadowData(staged.latch.engineShadows) != nullptr;
	parameters.flags.z = ViewAtmosphere::InputFlags{ .directionalShadow = hasLight && globals::deferred->GetDirectionalShadowMap(),
		.ibl = useFog && ibl.loaded && ibl.settings.EnableIBL && !ibl.IsDisabledForCurrentScene(),
		.probes = globals::features::skylighting.loaded,
		.localLights = localLights,
		.localShadows = localLights && lights.IsLocalShadowCacheActive() && lights.GetLocalShadowDataSRV(),
		.engineShadows = engineShadows,
		.terrain = globals::features::terrainShadows.IsShadowMapReady(),
		.cloudLayer = globals::features::cloudShadows.loaded,
		.environment = cubemaps.loaded && cubemaps.cubemapValid[0] }
	                         .Bits();
	staged.parameters = parameters;
	state.staged = staged;
	return parameters;
}

void ORGViewAtmosphereVolume::Commit(const Inputs* a_held, ViewAtmosphere::Parameters& a_parameters)
{
	auto& state = *impl;
	const auto staged = std::exchange(state.staged, std::nullopt);
	if (!a_held || !staged) {
		a_parameters = {};
		return;
	}
	a_parameters = staged->parameters;
	a_parameters.flags.z &= a_held->Bound();
	// As Render: a change of the shadow inputs drops the history.
	const auto shadowBits = a_parameters.flags.z & ViewAtmosphere::kShadowInputBits;
	if (shadowBits != state.lastShadowBits)
		a_parameters.history.x = 0.0f;
	state.lastShadowBits = shadowBits;
	auto* host = RenderGraphRuntime::Get().Host();
	if (!host)
		return;
	state.latch->WriteValue(host->CurrentRun(), 0, staged->latch);
	for (std::uint32_t grid = 0; grid < kGrids; ++grid) {
		const auto size = a_parameters.grids[grid].size;
		state.gate->OpenDispatch(Entry(staged->history, false, grid), (size.x + 7u) / 8u, (size.y + 7u) / 8u, (size.z + 3u) / 4u);
		state.gate->OpenDispatch(Entry(staged->history, true, grid), (size.x + 7u) / 8u, (size.y + 7u) / 8u, 1);
	}
	// The run writes the other texture of the pair. The next frame reads it once this run is submitted (Executed).
	const auto source = 1u - staged->history;
	state.committedWritten = source;
	state.committedVersion = RenderGraph::Frame::Revision::Adopted().Version(state.pair[source]);
	state.written = source;
	state.lastFrame = RenderGraph::FrameClock::BeginFrame();
	state.last = a_parameters;
	if (!std::exchange(state.reported, true)) {
		const auto& g = a_parameters.grids;
		logger::info("[ORG] Atmosphere and Fog: the first shared volume run: atlas {}x{}x{}, grids {}x{}x{}, {}x{}x{}, {}x{}x{}; flags z {:#x} (bound {:#x}), fog {}",
			a_parameters.atlas.x, a_parameters.atlas.y, a_parameters.atlas.z, g[0].size.x, g[0].size.y, g[0].size.z, g[1].size.x, g[1].size.y, g[1].size.z,
			g[2].size.x, g[2].size.y, g[2].size.z, a_parameters.flags.z, a_held->Bound(), a_parameters.flags.y);
	}
}

void ORGViewAtmosphereVolume::Executed()
{
	auto& state = *impl;
	if (const auto source = std::exchange(state.committedWritten, std::nullopt))
		RenderGraph::Frame::NoteWritten(state.pair[*source], state.committedVersion);
}
