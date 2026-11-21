#include "ORGViewAtmosphere.h"

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DynamicCubemaps.h"
#include "Features/DynamicCubemaps/ORGDynamicCubemaps.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/ExponentialHeightFog/ORGExponentialHeightFog.h"
#include "Features/IBL.h"
#include "Features/IBL/ORGIBL.h"
#include "Features/PhysicalSky.h"
#include "Features/ViewAtmosphere.h"
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
#include "RenderGraph/Switches.h"
#include "State.h"
#include "Utils/Logging.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace
{
	using Anchor = FrameAnchors::Anchor;
	using Bind = RenderGraph::AnchorPasses::Bind;

	constexpr const char* kMediumClientId = "cs.view-atmosphere";
	constexpr const char* kCompositeClientId = "cs.view-atmosphere.composite";
	constexpr const char* kCompositeShader = "ViewAtmosphere/Composite.cs.hlsl";
	// After Exponential Height Fog's passes at the Prepass anchor.
	constexpr std::int32_t kPrepassOrder = 400;

	// ViewAtmosphere::Parameters' size. All zero means no medium: Enabled() and ReflectionEnabled() read flags.x as 0.
	constexpr std::uint32_t kParametersBytes = 256;
	using Parameters = std::array<std::uint32_t, kParametersBytes / 4>;

	// Composite.cs.hlsl's push block: the shared constants, then one handle per entry.
	constexpr std::uint32_t kCompositeWords = 4 + 11;

	D3D11_TEXTURE3D_DESC VolumeDesc(UINT a_bind)
	{
		D3D11_TEXTURE3D_DESC desc{};
		desc.Width = desc.Height = desc.Depth = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = a_bind;
		return desc;
	}

	// Copies the run's parameters from the latch into the published buffer. A run without them copies the zeros of no medium.
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

	/** @brief What the composite reads besides its own resources. */
	struct CompositeInputs
	{
		bool fogNear = false, fogFar = false;  // the fog's published volumes, else the no-fog volume
		bool ibl = false;
		bool cubemap = false;

		bool operator==(const CompositeInputs&) const = default;
	};

	/** @brief What a composite registration is made from: the key of its declared client. A new one registers it again. */
	struct CompositeKey
	{
		std::shared_ptr<const ComputeProgram> program;
		FrameAnchors::Texture main, depth;
		CompositeInputs inputs;

		bool operator==(const CompositeKey&) const = default;
	};

	/** @brief What the last Prepass found that is no variable yet. The engine targets are stable slots, so the key adds them. */
	struct CompositeSeen
	{
		std::optional<CompositeInputs> inputs;  // empty while an input is missing
		bool sky = false;                       // Physical Sky would render if it were on

		bool operator==(const CompositeSeen&) const = default;
	};

	/** @brief What the composite's program is built from: the reload count and its description. Equal when the counts are. */
	struct CompositeProgramKey
	{
		std::uint32_t reloads = 0;
		ComputeProgram::Desc desc;

		bool operator==(const CompositeProgramKey& a_other) const { return reloads == a_other.reloads; }
	};

	/** @brief The composite's debug view, 1 to 13, from CS_ORG_VIEW_ATMOSPHERE_DEBUG (Composite.cs.hlsl lists them). 0 when off. */
	std::uint32_t DebugView()
	{
		static const std::uint32_t s_view = [] {
			const auto view = Switches::Number("CS_ORG_VIEW_ATMOSPHERE_DEBUG", 0);
			if (view > 13) {
				logger::warn("[ORG] Atmosphere and Fog: CS_ORG_VIEW_ATMOSPHERE_DEBUG={} is not a debug view (1 to 13); it is off", view);
				return 0u;
			}
			if (view)
				logger::info("[ORG] Atmosphere and Fog: the post-water composite draws debug view {}", view);
			return static_cast<std::uint32_t>(view);
		}();
		return s_view;
	}

	/** @brief The groups the hook asks for. Read by the commit of the same run. */
	struct CompositeRun
	{
		std::uint32_t groupsX = 0, groupsY = 0;
	};
}

struct ORGViewAtmosphere::Impl
{
	// The composite's program. A shader reload raises the count, which is its key, so it is built again.
	RenderGraph::Frame::Observed<std::uint32_t> programReloads{ "ViewAtmosphere.CompositeProgram" };
	std::uint32_t reloads = 0;
	RenderGraph::Frame::DeclaredProgram<ComputeProgram> program;

	org::OwnedDescriptorBinding sampler;
	std::shared_ptr<org::LatchBlock> latch;
	std::shared_ptr<RenderGraph::RunGate> gate;

	// Declared or made at Setup and kept for the session. The atlas follows the size settings and the display size. Each
	// version is born as no medium: nothing scattered, everything transmitted. The no-fog volume is born as a fog that is off.
	RenderGraph::Frame::DeclaredTexture3D luminance, transmittance, noFog;
	std::unique_ptr<GraphResources::StructuredBuffer> parameters;
	RenderGraph::Frame::DeclaredClient medium;
	// Each registration keeps its key. The frame composes while the adopted revision holds one (Adopted()).
	RenderGraph::Frame::DeclaredClientOf<const CompositeKey> composite;
	// Whether the shared volume's inject and integrate passes are on the graph. They wait on Physical Sky's media
	// (view-atmosphere-port.md 3.6). Until then the atlas is one texel, as for a feature that is off.
	bool sharedVolumePasses = false;

	// What the last Prepass found the composite can read, and whether Physical Sky would render if it were on (B2's interim).
	// The composite's key reads it, the sky flag with Physical Sky's toggle.
	RenderGraph::Frame::Observed<CompositeSeen> observed{ "ViewAtmosphere.Composite" };
	std::optional<CompositeRun> pendingRun;
	std::uint32_t compositeFrame = UINT32_MAX;
	bool reportedRun = false;  // the first run's inputs are logged once
	Parameters frameParameters{};

	/** @brief One line with what the first run reads, to tell its inputs apart between the modes. */
	void ReportFirstRun(const D3D11_TEXTURE2D_DESC& a_main, std::uint32_t a_width, std::uint32_t a_height) const
	{
		DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
		if (auto* depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture) {
			D3D11_TEXTURE2D_DESC depthDesc{};
			depth->GetDesc(&depthDesc);
			depthFormat = depthDesc.Format;
		}
		const auto& fog = globals::features::exponentialHeightFog.settings;
		const auto& atmosphere = globals::features::viewAtmosphere.settings;
		const auto& orgFog = ORGExponentialHeightFog::Get();
		const auto slices = orgFog.Slices();
		const auto& shared = globals::state->lastSharedData;
		const auto& camera = globals::game::frameBufferCached.GetCameraPosAdjust();
		logger::info(
			"[ORG] Atmosphere and Fog: the first post-water composite: kMAIN {}x{} format {}, groups for {}x{}, depth format {}; "
			"fog on {}, volumetric {}, density {:.4f}/{:.4f}, start {:.0f}, volumetric start {:.0f}, local range {:.0f}, near range {:.0f}; "
			"the fog's graph volumes {} (slices {}/{}); PostWaterComposite {}, near plane {:.2f}, camera ({:.0f}, {:.0f}, {:.0f}), debug view {}",
			a_main.Width, a_main.Height, static_cast<int>(a_main.Format), a_width, a_height, static_cast<int>(depthFormat), fog.enabled.Get(), fog.volumetricFogEnabled.Get(),
			fog.fogDensity.Get(), fog.fogDensity2.Get(), fog.startDistance, fog.volumetricFogStartDistance, atmosphere.localRange, atmosphere.nearRange,
			orgFog.NearSRV() ? "ran this frame" : "did not run this frame", slices ? slices->first : 0u, slices ? slices->second : 0u,
			shared.PostWaterComposite, shared.CameraData.y, camera.x, camera.y, camera.z, DebugView());
		ReportAnalyticInputs();
	}

	/**
	* @brief The analytic fog's inputs in the bytes the shared constants latch from, and the opacity the shader should get from them.
	* Finds the fog's settings block in the FeatureData bytes by content, so a block at the wrong place or stale shows as missing.
	*/
	static void ReportAnalyticInputs()
	{
		const auto expected = globals::features::exponentialHeightFog.GetCommonBufferData();
		const auto& bytes = globals::state->lastFeatureData;
		std::optional<std::size_t> offset;
		for (std::size_t at = 0; at + sizeof(expected) <= bytes.size(); at += 16)
			if (std::memcmp(bytes.data() + at, &expected, sizeof(expected)) == 0) {
				offset = at;
				break;
			}
		const auto& camera = globals::game::frameBufferCached.GetCameraPosAdjust();
		// The shader's HeightDensityIntegral for a level ray at the camera's height, at 20000 and 100000 units.
		const auto opacity = [&](float a_length) {
			const double height = static_cast<double>(camera.z) - expected.fogHeight;
			const double falloff = expected.fogHeightFalloff * 0.001;
			const double perUnit = height <= 0.0 ? 0.69314718056 : std::exp2(-falloff * height) * 0.69314718056;
			return 1.0 - std::exp2(-expected.fogDensity * 0.001 * perUnit * a_length);
		};
		logger::info(
			"[ORG] Atmosphere and Fog: the analytic fog's inputs: settings block {} in FeatureData ({} B); fog height {:.1f}, falloff {:.3f}, "
			"density {:.4f}, start {:.0f}, camera z {:.1f}; a level ray should get opacity {:.3f} at 20000 units and {:.3f} at 100000",
			offset ? fmt::format("at offset {}", *offset) : std::string("NOT FOUND"), bytes.size(), expected.fogHeight, expected.fogHeightFalloff,
			expected.fogDensity, expected.startDistance, camera.z, opacity(20000.0f), opacity(100000.0f));
	}

	/** @brief The atlas for a revision's values, while it exists (MakeMedium's existence rule). */
	static D3D11_TEXTURE3D_DESC AtlasDesc(const RenderGraph::Frame::Inputs& a_in)
	{
		auto desc = VolumeDesc(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
		const auto& s = globals::features::viewAtmosphere.settings;
		// Sized for the display (decision 6a). The shaders map the dynamic-resolution region.
		const auto display = a_in.Get(RenderGraph::Frame::Vars::DisplaySize());
		const auto layout = ViewAtmosphere::LayoutFor(float2{ static_cast<float>(display.width), static_cast<float>(display.height) }, a_in.Get(s.nearPixelSize),
			a_in.Get(s.middlePixelSize), a_in.Get(s.distantPixelSize), a_in.Get(s.nearSlices), a_in.Get(s.middleSlices), a_in.Get(s.distantSlices));
		desc.Width = layout.atlas.x;
		desc.Height = layout.atlas.y;
		desc.Depth = layout.atlas.z;
		return desc;
	}

	void MakeMedium()
	{
		using namespace RenderGraph::Frame;
		const auto& s = globals::features::viewAtmosphere.settings;
		const auto dependencies = DependsOn(Vars::DisplaySize(), s.nearPixelSize, s.nearSlices, s.middlePixelSize, s.middleSlices, s.distantPixelSize,
			s.distantSlices);
		// One texel while the shared volume is off (the existence rule). Its passes are not on the graph yet, so it is off.
		const auto exists = [&] {
			return ExistsWhen(DependsOn(s.enabled),
				[shared = sharedVolumePasses](const Inputs& a_in) { return shared && a_in.Get(globals::features::viewAtmosphere.settings.enabled); });
		};
		// Only graph readers: DCLF reads the published ids, so nothing is exported to D3D11.
		const auto published = [](std::array<float, 4> a_birth) {
			return GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .versioned = true, .birth = Birth::Value(a_birth) };
		};
		luminance = DeclareTexture3D(std::string(kLuminanceId), dependencies, AtlasDesc, published({ 0.0f, 0.0f, 0.0f, 0.0f }), exists());
		transmittance = DeclareTexture3D(std::string(kTransmittanceId), dependencies, AtlasDesc, published({ 1.0f, 1.0f, 1.0f, 1.0f }), exists());
		// The no-fog volume matches ImageSpace's, for a fog that is off. A declaration with no dependencies, for its birth.
		noFog = DeclareTexture3D(
			"ViewAtmosphere::NoFog", Dependencies{}, [](const Inputs&) { return VolumeDesc(D3D11_BIND_SHADER_RESOURCE); },
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .birth = Birth::Value({ 0.0f, 0.0f, 0.0f, 1.0f }) });
		D3D11_BUFFER_DESC bufferDesc{};
		bufferDesc.Usage = D3D11_USAGE_DEFAULT;
		bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bufferDesc.StructureByteStride = kParametersBytes;
		bufferDesc.ByteWidth = kParametersBytes;
		parameters = std::make_unique<GraphResources::StructuredBuffer>(bufferDesc, 1, std::string(kParametersId),
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
		if (!*parameters || !luminance || !*luminance || !transmittance || !*transmittance)
			Fatal("the medium's textures could not be made");
		// Published once, for the session. Readers follow the versions.
		if (!PublishedResources::Publish(*luminance) || !PublishedResources::Publish(*transmittance) || !PublishedResources::Publish(*parameters))
			Fatal("the medium could not be published");
	}

	/** @brief The medium's registration. Its client has no dependencies, so it is registered once, at the first revision. */
	bool MakeMediumClient(FrameAnchors::ClientDesc& a_client)
	{
		RenderGraph::AnchorPasses::Builder builder(Anchor::Prepass, kMediumClientId, kPrepassOrder);
		builder.Add("parameters", FrameAnchors::PassKind::Copy, [buffer = parameters->Graph(), latch = std::shared_ptr<const org::LatchBlock>(latch)] {
			return std::make_shared<ParametersPass>(buffer, latch);
		});
		if (builder.Failed()) {
			logger::error("[ORG] Atmosphere and Fog: the medium's passes: {}", builder.Failure());
			return false;
		}
		a_client.id = kMediumClientId;
		a_client.passes = builder.Take();
		a_client.registerResources = [this](org::RenderGraph& a_graph) {
			luminance->Register(a_graph);
			transmittance->Register(a_graph);
			noFog->Register(a_graph);
			parameters->Register(a_graph);
		};
		// No active callback, so the commit runs every run.
		a_client.commit = [this](const FrameAnchors::Point&) {
			if (auto* host = RenderGraphRuntime::Get().Host())
				latch->WriteValue(host->CurrentRun(), 0, frameParameters);
		};
		logger::info("[ORG] Atmosphere and Fog: the medium is on the render graph (no shared volume: Exponential Height Fog's volumes stay the fog)");
		return true;
	}

	/** @brief What the composite reads now. Empty while an input it needs is not on the graph yet. */
	static std::optional<CompositeInputs> InputsNow()
	{
		CompositeInputs inputs;
		const auto& fog = globals::features::exponentialHeightFog;
		if (fog.loaded) {
			inputs.fogNear = PublishedResources::IsDeclarable(ORGExponentialHeightFog::kNearIntegratedId);
			inputs.fogFar = PublishedResources::IsDeclarable(ORGExponentialHeightFog::kFarIntegratedId);
		}
		if (globals::features::ibl.loaded) {
			if (!PublishedResources::IsDeclarable(ORGIBL::kEnvId) || !PublishedResources::IsDeclarable(ORGIBL::kSkyId))
				return std::nullopt;
			inputs.ibl = true;
		}
		if (fog.loaded && globals::features::dynamicCubemaps.loaded) {
			if (!PublishedResources::IsDeclarable(ORGDynamicCubemaps::kEnvBC6HId))
				return std::nullopt;
			inputs.cubemap = true;
		}
		return inputs;
	}

	ComputeProgram::Desc CompositeDesc() const
	{
		std::vector<std::pair<std::string, std::string>> defines;
		// Physical Sky's media are not on the graph, so this build composes the fog alone.
		if (globals::features::exponentialHeightFog.loaded)
			defines.emplace_back("EXP_HEIGHT_FOG", "");
		if (globals::features::ibl.loaded)
			defines.emplace_back("IBL", "");
		if (globals::features::dynamicCubemaps.loaded)
			defines.emplace_back("DYNAMIC_CUBEMAPS", "");
		if (const auto debug = DebugView())
			defines.emplace_back("VIEW_ATMOSPHERE_COMPOSITE_DEBUG", std::to_string(debug));
		return { .source = kCompositeShader, .constantWords = kCompositeWords, .defines = RenderGraph::AnchorPasses::Defines(defines) };
	}

	/** @brief What the composite can read this frame, for its key. No inputs while one is missing. Physical Sky is the key's (its toggle). */
	CompositeSeen Observe()
	{
		CompositeSeen seen;
		if (!globals::features::exponentialHeightFog.loaded)
			return seen;
		seen.inputs = InputsNow();
		return seen;
	}

	/**
	* @brief Setup. The composite's program, built on the programs lane before the revision that needs it starts. Its description
	* is made on the render thread, when a revision starts. A failed build is fatal.
	*/
	void DeclareCompositeProgram()
	{
		using namespace RenderGraph::Frame;
		program = DeclareProgram(
			"ViewAtmosphere::Composite", DependsOn(programReloads.Var()),
			[this](const Inputs& a_in) {
				const auto* count = programReloads.Get(a_in);
				CompositeProgramKey key{ count ? *count : 0u, CompositeDesc() };
				RenderGraph::Programs::ResolveDefines(key.desc);
				return key;
			},
			[](const CompositeProgramKey& a_key) { return RenderGraph::Programs::Load(a_key.desc); });
	}

	/** @brief The composite's key for what a revision carries. kMAIN and its depth are slots that never change. */
	std::optional<CompositeKey> KeyOf(const RenderGraph::Frame::Inputs& a_in, const CompositeSeen& a_seen)
	{
		if (!a_seen.inputs || !program)
			return std::nullopt;
		CompositeKey key{ program.For(a_in), FrameAnchors::EngineTexture(FrameAnchors::Colour(RE::RENDER_TARGETS::kMAIN)),
			FrameAnchors::EngineTexture(FrameAnchors::Depth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN)), *a_seen.inputs };
		if (!key.main || !key.depth)
			Fatal("kMAIN or its depth could not be imported for the post-water composite");
		return key;
	}

	/** @brief The composite's registration for a key, when a revision starts. */
	bool MakeComposite(const CompositeKey& a_key, FrameAnchors::ClientDesc& a_client)
	{
		const auto fogVolume = [&](bool a_published, std::string_view a_id) { return a_published ? Bind::Published(a_id) : Bind::Srv(*noFog); };
		const auto& inputs = a_key.inputs;
		RenderGraph::AnchorPasses::Builder builder(Anchor::EngineDraws, kCompositeClientId, 0, nullptr, gate);
		builder.Add(RenderGraph::AnchorPasses::Dispatch{ .name = "dispatch",
			.program = a_key.program,
			.bindings = { Bind::Shared(), Bind::Handle(sampler.Index()), Bind::Srv(a_key.depth), Bind::Uav(a_key.main), Bind::Published(kLuminanceId),
				Bind::Published(kTransmittanceId), Bind::Published(kParametersId), fogVolume(inputs.fogNear, ORGExponentialHeightFog::kNearIntegratedId),
				fogVolume(inputs.fogFar, ORGExponentialHeightFog::kFarIntegratedId), inputs.ibl ? Bind::Published(ORGIBL::kEnvId) : Bind::Unused(),
				inputs.ibl ? Bind::Published(ORGIBL::kSkyId) : Bind::Unused(), inputs.cubemap ? Bind::Published(ORGDynamicCubemaps::kEnvBC6HId) : Bind::Unused() },
			.gate = 0u });
		if (builder.Failed()) {
			logger::error("[ORG] Atmosphere and Fog: the post-water composite's pass: {}", builder.Failure());
			return false;
		}
		const bool shared = builder.ReadsSharedConstants();
		a_client.id = kCompositeClientId;
		a_client.passes = builder.Take();
		a_client.commit = [this](const FrameAnchors::Point&) {
			if (pendingRun)
				gate->OpenDispatch(0, pendingRun->groupsX, pendingRun->groupsY, 1);
		};
		a_client.active = [this] { return pendingRun.has_value(); };
		a_client.sharedConstants = shared;
		a_client.gates = { gate };
		// Every registration keeps the gate and commits the same run, so a registration for new inputs lets the older one
		// compose until the revision that holds the new one is adopted (it taps by id). No frame goes without it.
		logger::info("[ORG] Atmosphere and Fog: a post-water composite for the next revision (fog {}/{}, IBL {}, cubemap {})", inputs.fogNear ? "near" : "-",
			inputs.fogFar ? "far" : "-", inputs.ibl, inputs.cubemap);
		return true;
	}

	/** @brief Inside the adoption point of the first Begin whose revision holds the registration. */
	void CompositeAdopted()
	{
		logger::info("[ORG] Atmosphere and Fog: a post-water composite switches in with the revision adopted at frame {}; PostWaterComposite is on",
			globals::state->frameCount);
	}

	/** @brief The two clients, declared on the settings their keys read. */
	void DeclareClients()
	{
		using namespace RenderGraph::Frame;
		medium = RenderGraph::Frame::DeclareClient<bool>(
			kMediumClientId, Dependencies{}, [](const Inputs&) -> std::optional<bool> { return true; },
			[this](const bool&, FrameAnchors::ClientDesc& a_client) { return MakeMediumClient(a_client); });
		// On while the adopted revision has the fog on and the last Prepass found what the composite reads. Off while Physical Sky
		// renders: its media are not on the graph, so its shaders keep their own aerial perspective and clouds. Its toggle is a
		// dependency, so a toggle switches the composite in the same revision as the sky's clients.
		const auto& fog = globals::features::exponentialHeightFog.settings;
		const auto& sky = globals::features::physicalSky.settings;
		composite = RenderGraph::Frame::DeclareClient<CompositeKey, const CompositeKey>(
			kCompositeClientId, DependsOn(fog.enabled, sky.enabled, observed.Var(), programReloads.Var()),
			[this](const Inputs& a_in) -> std::optional<CompositeKey> {
				if (!a_in.Get(globals::features::exponentialHeightFog.settings.enabled))
					return std::nullopt;
				const auto* seen = observed.Get(a_in);
				if (!seen)
					return std::nullopt;
				const auto& physicalSky = globals::features::physicalSky;
				if (physicalSky.loaded && seen->sky && a_in.Get(physicalSky.settings.enabled))
					return std::nullopt;
				return KeyOf(a_in, *seen);
			},
			[this](const CompositeKey& a_key, FrameAnchors::ClientDesc& a_client) -> std::shared_ptr<const CompositeKey> {
				if (!MakeComposite(a_key, a_client))
					return nullptr;
				return std::make_shared<const CompositeKey>(a_key);
			},
			[this](const CompositeKey&, const CompositeKey&) { CompositeAdopted(); });
	}
};

ORGViewAtmosphere::ORGViewAtmosphere() = default;
ORGViewAtmosphere::~ORGViewAtmosphere() = default;

void ORGViewAtmosphere::Setup()
{
	if (!BeginSetup())
		return;
	auto state = std::make_unique<Impl>();
	// ViewAtmosphere's linear clamp sampler.
	state->sampler = ShaderBindings::Sampler(ShaderBindings::SamplerPreset::LinearClamp);
	state->latch = std::make_shared<org::LatchBlock>("cs.view-atmosphere.latch", kParametersBytes);
	state->gate = RenderGraph::RunGate::Create("cs.view-atmosphere.composite.gate", 1);
	if (!state->sampler)
		Fatal("the sampler could not be made");
	// Before the first frame, since DCLF's draws read the exports from their first frame on.
	state->MakeMedium();
	// The program and both clients follow the frame revisions from the first one on. The composite needs the fog.
	if (globals::features::exponentialHeightFog.loaded)
		state->DeclareCompositeProgram();
	state->DeclareClients();
	impl = std::move(state);
}

void ORGViewAtmosphere::Prepass()
{
	if (!impl)
		return;
	// No shared volume yet, so every frame stages the parameters of no medium.
	impl->frameParameters = {};
	// What the composite reads is not a variable yet. A change is an event, so the next revision makes its key again.
	// Physical Sky's checks but its toggle (worldspace, interior, shaders), which the toggle joins in the key.
	const auto& sky = globals::features::physicalSky;
	auto seen = impl->Observe();
	seen.sky = sky.loaded && sky.RendersWhenEnabled();
	impl->observed.Observe(seen);
}

bool ORGViewAtmosphere::CompositeActive() const
{
	// The registration the adopted revision holds. A new one switches in with its revision, so PostWaterComposite and the
	// composite agree in every frame. A removed one stops at once: its gate closes, and the shaders do their own fog again.
	return impl && impl->composite.Adopted();
}

void ORGViewAtmosphere::CompositeAfterWater()
{
	if (!CompositeActive() || !globals::state->inWorld)
		return;
	// Not for the reflection faces, and once a frame, as on D3D11.
	const auto frame = globals::state->frameCount;
	if ((globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)) != 0 ||
		impl->compositeFrame == frame)
		return;
	impl->compositeFrame = frame;
	const auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!main.texture)
		return;
	D3D11_TEXTURE2D_DESC desc{};
	main.texture->GetDesc(&desc);
	const auto scale = globals::game::frameBufferCached.GetDynamicResolutionParams1();
	const auto width = static_cast<std::uint32_t>(std::clamp(desc.Width * scale.x, 1.f, float(desc.Width)));
	const auto height = static_cast<std::uint32_t>(std::clamp(desc.Height * scale.y, 1.f, float(desc.Height)));
	impl->pendingRun = CompositeRun{ (width + 7u) / 8u, (height + 7u) / 8u };
	const auto result = FrameAnchors::Run(Anchor::EngineDraws);
	impl->pendingRun.reset();
	if (result && !std::exchange(impl->reportedRun, true))
		impl->ReportFirstRun(desc, width, height);
	// PostWaterComposite is on for this frame, so the frame has no fog where this did not run.
	if (!result) {
		static Util::ReasonLog s_missed("[ORG] Atmosphere and Fog: the post-water composite did not run this frame, though PostWaterComposite is on");
		s_missed.Report(result.status == FrameAnchors::RunStatus::Idle ? "idle (the schedule does not hold it)" :
		                result.status == FrameAnchors::RunStatus::Refused ? "refused" :
		                                                                     "its epoch failed");
	}
}

bool ORGViewAtmosphere::MediumPublished() const
{
	return impl && impl->luminance && impl->transmittance && impl->parameters;
}

void ORGViewAtmosphere::ClearShaderCache()
{
	// A new key builds the program again. The registered composite keeps composing until the new one is built and adopted.
	if (impl)
		impl->programReloads.Observe(++impl->reloads);
}

