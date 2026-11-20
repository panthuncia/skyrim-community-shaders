#include "ORGViewAtmosphere.h"
#include "RenderGraph/Engine/Native/Native.h"

bool ORGViewAtmosphere::Requested()
{
	return RenderGraph::Engine::Native::GraphMode();
}

ORGViewAtmosphere& ORGViewAtmosphere::Get()
{
	static ORGViewAtmosphere s_atmosphere;
	return s_atmosphere;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/DynamicCubemaps.h"
#	include "Features/DynamicCubemaps/ORGDynamicCubemaps.h"
#	include "Features/ExponentialHeightFog.h"
#	include "Features/ExponentialHeightFog/ORGExponentialHeightFog.h"
#	include "Features/IBL.h"
#	include "Features/IBL/ORGIBL.h"
#	include "Features/PhysicalSky.h"
#	include "Features/ViewAtmosphere.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Frame/Declare.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ProgramCache.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/Switches.h"
#	include "State.h"
#	include "Utils/Logging.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>

#	include <array>
#	include <cmath>
#	include <cstdint>
#	include <cstring>
#	include <optional>
#	include <string>
#	include <vector>

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

	/** @brief Fatal: a graph mode has no D3D11 path for the medium. */
	[[noreturn]] void Fail(std::string_view a_what)
	{
		logger::critical("[ORG] Atmosphere and Fog: {}", a_what);
		stl::report_and_fail(std::format("Atmosphere and Fog on the render graph: {}", a_what));
	}

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

	/** @brief One RGBA16F texel. */
	std::array<std::uint16_t, 4> Texel(bool a_r, bool a_g, bool a_b, bool a_a)
	{
		constexpr std::uint16_t kOne = 0x3C00;
		return { a_r ? kOne : std::uint16_t(0), a_g ? kOne : std::uint16_t(0), a_b ? kOne : std::uint16_t(0), a_a ? kOne : std::uint16_t(0) };
	}

	std::shared_ptr<const GraphResources::StagedUpload> StageTexel(const GraphResources::Texture3D& a_texture, const std::array<std::uint16_t, 4>& a_texel)
	{
		const GraphResources::SubresourceData slice{ a_texel.data(), sizeof(a_texel), sizeof(a_texel) };
		return a_texture.Stage(std::span(&slice, 1));
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
	RenderGraph::ProgramCache<ComputeProgram, std::uint32_t> programs{ "Atmosphere and Fog" };
	std::uint32_t programGeneration = 0;
	std::shared_ptr<const ComputeProgram> program;  // the newest built

	org::OwnedDescriptorBinding sampler;
	std::shared_ptr<org::LatchBlock> latch;
	std::shared_ptr<RenderGraph::RunGate> gate;
	// The medium's first contents: a first-run gate the medium client's commit opens.
	std::shared_ptr<RenderGraph::RunGate> mediumGate;

	// Declared or made at Setup and kept for the session. The atlas follows the size settings and the display size.
	RenderGraph::Frame::DeclaredTexture3D luminance, transmittance;
	std::unique_ptr<GraphResources::Texture3D> noFog;
	std::unique_ptr<GraphResources::StructuredBuffer> parameters;
	std::vector<std::shared_ptr<const GraphResources::StagedUpload>> staged;
	RenderGraph::Frame::DeclaredClient medium, composite;
	// Whether the shared volume's inject and integrate passes are on the graph. They wait on Physical Sky's media
	// (view-atmosphere-port.md 3.6). Until then the atlas is one texel, as for a feature that is off.
	bool sharedVolumePasses = false;

	// What the last Prepass found the composite can read. Empty while it cannot run. The composite's key reads it.
	std::optional<CompositeKey> observed;
	// Whether the last Prepass found Physical Sky would render if it were on. The key reads it with Physical Sky's toggle.
	bool skyObserved = false;
	// A registration after none starts a new count. The frame composes once an adopted revision holds that count's registration.
	// A registration for new inputs keeps the count: the older one composes until the new one switches in (matchesById).
	std::uint64_t compositeCount = 0, heldCount = 0;
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

	/** @brief The atlas for a revision's values. One texel while the shared volume is off (the existence rule). */
	D3D11_TEXTURE3D_DESC AtlasDesc(const RenderGraph::Frame::Inputs& a_in) const
	{
		auto desc = VolumeDesc(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
		const auto& s = globals::features::viewAtmosphere.settings;
		if (!sharedVolumePasses || !a_in.Get(s.enabled))
			return desc;
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
		const auto dependencies = DependsOn(Vars::DisplaySize(), s.enabled, s.nearPixelSize, s.nearSlices, s.middlePixelSize, s.middleSlices, s.distantPixelSize,
			s.distantSlices);
		const GraphResources::Options exported{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV, .versioned = true };
		const auto atlas = [this](const Inputs& a_in) { return AtlasDesc(a_in); };
		luminance = DeclareTexture3D(std::string(kLuminanceId), dependencies, atlas, exported);
		transmittance = DeclareTexture3D(std::string(kTransmittanceId), dependencies, atlas, exported);
		noFog = std::make_unique<GraphResources::Texture3D>(VolumeDesc(D3D11_BIND_SHADER_RESOURCE), "ViewAtmosphere::NoFog",
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
		D3D11_BUFFER_DESC bufferDesc{};
		bufferDesc.Usage = D3D11_USAGE_DEFAULT;
		bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bufferDesc.StructureByteStride = kParametersBytes;
		bufferDesc.ByteWidth = kParametersBytes;
		parameters = std::make_unique<GraphResources::StructuredBuffer>(bufferDesc, 1, std::string(kParametersId),
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
		if (!luminance || !transmittance || !*luminance || !*transmittance || !*noFog || !*parameters || !luminance->SRV() || !transmittance->SRV() ||
			!parameters->SRV())
			Fail("the medium's textures could not be made");
		// No medium: nothing scattered, everything transmitted. The no-fog volume matches ImageSpace's, for a fog that is off.
		// The atlas is one texel while the shared volume is off, so these contents cover it.
		staged = { StageTexel(*luminance, Texel(false, false, false, false)), StageTexel(*transmittance, Texel(true, true, true, true)),
			StageTexel(*noFog, Texel(false, false, false, true)) };
		for (const auto& upload : staged)
			if (!upload)
				Fail("the medium's first contents could not be staged");
		// Published once, for the session. Readers follow the versions.
		if (!PublishedResources::Publish(*luminance) || !PublishedResources::Publish(*transmittance) || !PublishedResources::Publish(*parameters))
			Fail("the medium could not be published");
	}

	/** @brief The medium's registration. Its client has no dependencies, so it is registered once, at the first revision. */
	bool MakeMediumClient(FrameAnchors::ClientDesc& a_client)
	{
		RenderGraph::AnchorPasses::Builder builder(Anchor::Prepass, kMediumClientId, kPrepassOrder);
		// The first contents copy on runs whose commit opened them, until a run with them submits. The others skip at execution.
		const auto firstContents = std::make_shared<RenderGraph::FirstRun>();
		builder.Add(
			"first-contents", FrameAnchors::PassKind::Copy,
			[uploads = staged, gateRef = std::shared_ptr<const RenderGraph::RunGate>(mediumGate)] { return GraphResources::MakeUploadPass(uploads, { gateRef, 0 }); },
			true);
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
		a_client.gates = { mediumGate };
		// No active callback, so the commit runs every run.
		a_client.commit = [this, firstContents](const FrameAnchors::Point&) {
			if (auto* host = RenderGraphRuntime::Get().Host())
				latch->WriteValue(host->CurrentRun(), 0, frameParameters);
			firstContents->Open(*mediumGate, 0);
		};
		a_client.executed = [firstContents](const FrameAnchors::Point&) { firstContents->Executed(); };
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

	/**
	* @brief What the composite can read this frame, for its key. Empty while no program is built, or while an input is missing.
	* Physical Sky is the key's (its toggle). These are not variables yet, so a change invalidates the client (Prepass).
	*/
	std::optional<CompositeKey> Observe()
	{
		if (!globals::features::exponentialHeightFog.loaded)
			return std::nullopt;
		// The program builds off the render thread. A new build replaces the one in use once it is done.
		if (auto built = programs.Get(programGeneration, [this] { return CompositeDesc(); }))
			program = std::move(built);
		else if (programs.StatusOf(programGeneration) == RenderGraph::ProgramStatus::Failed)
			Fail("the post-water composite's program could not be built");
		if (!program)
			return std::nullopt;
		const auto inputs = InputsNow();
		if (!inputs)
			return std::nullopt;
		CompositeKey key{ program, FrameAnchors::EngineTarget(RE::RENDER_TARGETS::kMAIN), FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN),
			*inputs };
		if (!key.main || !key.depth)
			Fail("kMAIN or its depth could not be imported for the post-water composite");
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
		// compose until the revision that holds the new one is adopted. No frame goes without it.
		a_client.matchesById = true;
		if (!composite.Registered())
			++compositeCount;
		logger::info("[ORG] Atmosphere and Fog: a post-water composite for the next revision (fog {}/{}, IBL {}, cubemap {})", inputs.fogNear ? "near" : "-",
			inputs.fogFar ? "far" : "-", inputs.ibl, inputs.cubemap);
		return true;
	}

	/** @brief Inside the adoption point of the first Begin whose revision holds the registration. */
	void CompositeAdopted()
	{
		if (heldCount != compositeCount)
			logger::info("[ORG] Atmosphere and Fog: the post-water composite switches in with the revision adopted at frame {}; PostWaterComposite is on",
				globals::state->frameCount);
		heldCount = compositeCount;
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
		composite = RenderGraph::Frame::DeclareClient<CompositeKey>(
			kCompositeClientId, DependsOn(fog.enabled, sky.enabled),
			[this](const Inputs& a_in) -> std::optional<CompositeKey> {
				if (!a_in.Get(globals::features::exponentialHeightFog.settings.enabled))
					return std::nullopt;
				const auto& physicalSky = globals::features::physicalSky;
				if (physicalSky.loaded && skyObserved && a_in.Get(physicalSky.settings.enabled))
					return std::nullopt;
				return observed;
			},
			[this](const CompositeKey& a_key, FrameAnchors::ClientDesc& a_client) { return MakeComposite(a_key, a_client); },
			[this](const CompositeKey&) { CompositeAdopted(); });
		if (!medium || !composite)
			Fail("the medium's or the post-water composite's client could not be declared");
	}
};

ORGViewAtmosphere::~ORGViewAtmosphere() = default;

void ORGViewAtmosphere::Reserve()
{
	if (!Requested())
		return;
	if (!FrameAnchors::Reserve(Anchor::Prepass) || !FrameAnchors::Reserve(Anchor::EngineDraws))
		Fail("the Prepass or EngineDraws anchor could not be reserved");
}

void ORGViewAtmosphere::Setup()
{
	if (!Requested() || impl)
		return;
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.IsActive() || !runtime.Host() || !FrameAnchors::HasEpoch(Anchor::Prepass) || !FrameAnchors::HasEpoch(Anchor::EngineDraws))
		Fail("the render graph or its Prepass and EngineDraws anchors are not available");
	auto state = std::make_unique<Impl>();
	// ViewAtmosphere's linear clamp sampler.
	D3D11_SAMPLER_DESC desc{};
	desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	desc.MaxAnisotropy = 1;
	desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	desc.MaxLOD = D3D11_FLOAT32_MAX;
	state->sampler = ShaderBindings::CreateSamplerFromD3D11(desc);
	state->latch = std::make_shared<org::LatchBlock>("cs.view-atmosphere.latch", kParametersBytes);
	state->gate = RenderGraph::RunGate::Create("cs.view-atmosphere.composite.gate", 1);
	state->mediumGate = RenderGraph::RunGate::Create("cs.view-atmosphere.medium.gate", 1);
	if (!state->sampler || !state->gate || !state->mediumGate)
		Fail("the sampler or a run gate could not be made");
	// Before the first frame, since DCLF's draws read the exports from their first frame on.
	state->MakeMedium();
	// Both clients follow the frame revisions from the first one on.
	state->DeclareClients();
	impl = std::move(state);
}

void ORGViewAtmosphere::Prepass()
{
	if (!impl)
		return;
	// No shared volume yet, so every frame stages the parameters of no medium.
	impl->frameParameters = {};
	// What the composite reads is not a variable yet. A change makes its key again when the next revision starts.
	// Physical Sky's checks but its toggle (worldspace, interior, shaders), which the toggle joins in the key.
	const auto& sky = globals::features::physicalSky;
	const bool skyHere = sky.loaded && sky.RendersWhenEnabled();
	if (auto seen = impl->Observe(); seen != impl->observed || skyHere != impl->skyObserved) {
		impl->observed = std::move(seen);
		impl->skyObserved = skyHere;
		impl->composite.Invalidate();
	}
}

bool ORGViewAtmosphere::CompositeActive() const
{
	// The registration the adopted revision holds. A new one switches in with its revision, so PostWaterComposite and the
	// composite agree in every frame. A removed one stops at once: its gate closes, and the shaders do their own fog again.
	return impl && impl->composite.Registered() && impl->compositeCount != 0 && impl->heldCount == impl->compositeCount &&
	       RenderGraphRuntime::Get().IsActive();
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

ID3D11ShaderResourceView* ORGViewAtmosphere::LuminanceSRV() const
{
	return impl && impl->luminance ? impl->luminance->SRV() : nullptr;
}

ID3D11ShaderResourceView* ORGViewAtmosphere::TransmittanceSRV() const
{
	return impl && impl->transmittance ? impl->transmittance->SRV() : nullptr;
}

ID3D11ShaderResourceView* ORGViewAtmosphere::ParametersSRV() const
{
	return impl && impl->parameters ? impl->parameters->SRV() : nullptr;
}

void ORGViewAtmosphere::ClearShaderCache()
{
	// A new key builds the program again. The registered composite keeps composing until the new one is built and adopted.
	if (impl)
		++impl->programGeneration;
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph only the D3D11 path exists.
struct ORGViewAtmosphere::Impl
{};

ORGViewAtmosphere::~ORGViewAtmosphere() = default;
void ORGViewAtmosphere::Reserve() {}
void ORGViewAtmosphere::Setup() {}
void ORGViewAtmosphere::Prepass() {}
bool ORGViewAtmosphere::CompositeActive() const { return false; }
void ORGViewAtmosphere::CompositeAfterWater() {}
ID3D11ShaderResourceView* ORGViewAtmosphere::LuminanceSRV() const { return nullptr; }
ID3D11ShaderResourceView* ORGViewAtmosphere::TransmittanceSRV() const { return nullptr; }
ID3D11ShaderResourceView* ORGViewAtmosphere::ParametersSRV() const { return nullptr; }
void ORGViewAtmosphere::ClearShaderCache() {}

#endif
