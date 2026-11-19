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

	/** @brief What the composite reads besides its own resources. A change registers the client again. */
	struct CompositeInputs
	{
		bool fogNear = false, fogFar = false;  // the fog's published volumes, else the no-fog volume
		bool ibl = false;
		bool cubemap = false;

		bool operator==(const CompositeInputs&) const = default;
	};

	/** @brief The composite's debug view, 1 to 12, from CS_ORG_VIEW_ATMOSPHERE_DEBUG (Composite.cs.hlsl lists them). 0 when off. */
	std::uint32_t DebugView()
	{
		static const std::uint32_t s_view = [] {
			const auto view = Switches::Number("CS_ORG_VIEW_ATMOSPHERE_DEBUG", 0);
			if (view > 12) {
				logger::warn("[ORG] Atmosphere and Fog: CS_ORG_VIEW_ATMOSPHERE_DEBUG={} is not a debug view (1 to 12); it is off", view);
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
	std::shared_ptr<const ComputeProgram> program;  // the one the registered composite uses

	org::OwnedDescriptorBinding sampler;
	std::shared_ptr<org::LatchBlock> latch;
	std::shared_ptr<RenderGraph::RunGate> gate;

	// Made at Setup and kept for the session.
	std::unique_ptr<GraphResources::Texture3D> luminance, transmittance, noFog;
	std::unique_ptr<GraphResources::StructuredBuffer> parameters;
	std::vector<std::shared_ptr<const GraphResources::StagedUpload>> staged;
	bool mediumRegistered = false;

	std::optional<CompositeInputs> compositeInputs;  // set while the composite is registered with these inputs
	bool compositeWanted = false;                    // this frame's settings ask for the composite
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
			a_main.Width, a_main.Height, static_cast<int>(a_main.Format), a_width, a_height, static_cast<int>(depthFormat), fog.enabled, fog.volumetricFogEnabled,
			fog.fogDensity, fog.fogDensity2, fog.startDistance, fog.volumetricFogStartDistance, atmosphere.localRange, atmosphere.nearRange,
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

	void MakeMedium()
	{
		const GraphResources::Options exported{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV, .versioned = true };
		const UINT volumeBind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		luminance = std::make_unique<GraphResources::Texture3D>(VolumeDesc(volumeBind), std::string(kLuminanceId), exported);
		transmittance = std::make_unique<GraphResources::Texture3D>(VolumeDesc(volumeBind), std::string(kTransmittanceId), exported);
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
		if (!*luminance || !*transmittance || !*noFog || !*parameters || !luminance->SRV() || !transmittance->SRV() || !parameters->SRV())
			Fail("the medium's textures could not be made");
		// No medium: nothing scattered, everything transmitted. The no-fog volume matches ImageSpace's, for a fog that is off.
		staged = { StageTexel(*luminance, Texel(false, false, false, false)), StageTexel(*transmittance, Texel(true, true, true, true)),
			StageTexel(*noFog, Texel(false, false, false, true)) };
		for (const auto& upload : staged)
			if (!upload)
				Fail("the medium's first contents could not be staged");
		if (!PublishedResources::Publish(*luminance) || !PublishedResources::Publish(*transmittance) || !PublishedResources::Publish(*parameters))
			Fail("the medium could not be published");
	}

	void RegisterMedium()
	{
		RenderGraph::AnchorPasses::Builder builder(Anchor::Prepass, kMediumClientId, kPrepassOrder);
		builder.Add("first-contents", FrameAnchors::PassKind::Copy, [uploads = staged] { return GraphResources::MakeUploadPass(uploads); }, true);
		builder.Add("parameters", FrameAnchors::PassKind::Copy, [buffer = parameters->Graph(), latch = std::shared_ptr<const org::LatchBlock>(latch)] {
			return std::make_shared<ParametersPass>(buffer, latch);
		});
		if (builder.Failed())
			Fail(builder.Failure());
		const bool registered = FrameAnchors::Register({
			.id = kMediumClientId,
			.passes = builder.Take(),
			.registerResources =
				[this](org::RenderGraph& a_graph) {
					luminance->Register(a_graph);
					transmittance->Register(a_graph);
					noFog->Register(a_graph);
					parameters->Register(a_graph);
				},
			.commit =
				[this](const FrameAnchors::Point&) {
					if (auto* host = RenderGraphRuntime::Get().Host())
						latch->WriteValue(host->CurrentRun(), 0, frameParameters);
				},
		});
		if (!registered)
			Fail("the medium's client could not be registered at the Prepass anchor");
		mediumRegistered = true;
		logger::info("[ORG] Atmosphere and Fog: the medium is on the render graph (no shared volume: Exponential Height Fog's volumes stay the fog)");
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

	void RegisterComposite(const std::shared_ptr<const ComputeProgram>& a_program, const CompositeInputs& a_inputs)
	{
		const auto main = FrameAnchors::EngineTarget(RE::RENDER_TARGETS::kMAIN);
		const auto depth = FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
		if (!main || !depth)
			Fail("kMAIN or its depth could not be imported for the post-water composite");
		const auto fogVolume = [&](bool a_published, std::string_view a_id) { return a_published ? Bind::Published(a_id) : Bind::Srv(*noFog); };
		RenderGraph::AnchorPasses::Builder builder(Anchor::EngineDraws, kCompositeClientId, 0, nullptr, gate);
		builder.Add(RenderGraph::AnchorPasses::Dispatch{ .name = "dispatch",
			.program = a_program,
			.bindings = { Bind::Shared(), Bind::Handle(sampler.Index()), Bind::Srv(depth), Bind::Uav(main), Bind::Published(kLuminanceId),
				Bind::Published(kTransmittanceId), Bind::Published(kParametersId), fogVolume(a_inputs.fogNear, ORGExponentialHeightFog::kNearIntegratedId),
				fogVolume(a_inputs.fogFar, ORGExponentialHeightFog::kFarIntegratedId), a_inputs.ibl ? Bind::Published(ORGIBL::kEnvId) : Bind::Unused(),
				a_inputs.ibl ? Bind::Published(ORGIBL::kSkyId) : Bind::Unused(), a_inputs.cubemap ? Bind::Published(ORGDynamicCubemaps::kEnvBC6HId) : Bind::Unused() },
			.gate = 0u });
		if (builder.Failed())
			Fail(builder.Failure());
		const bool shared = builder.ReadsSharedConstants();
		const bool registered = FrameAnchors::Register({
			.id = kCompositeClientId,
			.passes = builder.Take(),
			.commit =
				[this](const FrameAnchors::Point&) {
					if (pendingRun)
						gate->OpenDispatch(0, pendingRun->groupsX, pendingRun->groupsY, 1);
				},
			.active = [this] { return pendingRun.has_value(); },
			.sharedConstants = shared,
			.gates = { gate },
		});
		if (!registered)
			Fail("the post-water composite could not be registered at the EngineDraws anchor");
		program = a_program;
		compositeInputs = a_inputs;
		logger::info("[ORG] Atmosphere and Fog: the post-water composite runs on the render graph (fog {}/{}, IBL {}, cubemap {})",
			a_inputs.fogNear ? "near" : "-", a_inputs.fogFar ? "far" : "-", a_inputs.ibl, a_inputs.cubemap);
	}

	/** @brief As ViewAtmosphere::CompositeEnabled asks on D3D11, for the media the graph composite has. */
	static bool CompositeWanted()
	{
		const auto& sky = globals::features::physicalSky;
		const auto& fog = globals::features::exponentialHeightFog;
		// Physical Sky's media are not on the graph. Its shaders keep their own aerial perspective and clouds meanwhile.
		if (sky.loaded && sky.cbData.enabled)
			return false;
		return fog.loaded && fog.settings.enabled;
	}

	void UpdateComposite()
	{
		compositeWanted = CompositeWanted();
		if (!compositeWanted)
			return;
		auto built = programs.Get(programGeneration, [this] { return CompositeDesc(); });
		if (!built) {
			if (programs.StatusOf(programGeneration) == RenderGraph::ProgramStatus::Failed)
				Fail("the post-water composite's program could not be built");
			return;
		}
		const auto inputs = InputsNow();
		if (!inputs)
			return;
		if (built != program || !compositeInputs || !(*compositeInputs == *inputs))
			RegisterComposite(built, *inputs);
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
	if (!state->sampler || !state->gate)
		Fail("the sampler or the run gate could not be made");
	// Before the first frame, since DCLF's draws read the exports from their first frame on.
	state->MakeMedium();
	impl = std::move(state);
}

void ORGViewAtmosphere::Prepass()
{
	if (!impl)
		return;
	if (!impl->mediumRegistered)
		impl->RegisterMedium();
	// No shared volume yet, so every frame stages the parameters of no medium.
	impl->frameParameters = {};
	impl->UpdateComposite();
}

bool ORGViewAtmosphere::CompositeActive() const
{
	// On only in frames whose schedule draws the composite, so PostWaterComposite and the composite agree. The schedules are taken at
	// Renderer::Begin, before anything reads the flag, and do not change during the frame. Before this, the flag followed the
	// registration and frames whose build lagged had no fog at all. Migration 6.21 deletes this.
	return impl && impl->compositeWanted && impl->compositeInputs.has_value() && RenderGraphRuntime::Get().IsActive() &&
	       FrameAnchors::ScheduledThisFrame(kCompositeClientId, Anchor::EngineDraws);
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
	// CompositeActive already asked ScheduledThisFrame, the answer PostWaterComposite took for this frame. The open stage must take
	// the tap now, else the frame applies no fog: a disagreement, logged.
	if (!FrameAnchors::Scheduled(kCompositeClientId, Anchor::EngineDraws)) {
		static Util::ReasonLog s_lag("[ORG] Atmosphere and Fog: the post-water composite's stage does not take its tap, though the frame's schedule holds it");
		s_lag.Report("not scheduled");
		return;
	}
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
	if (!result) {
		static Util::ReasonLog s_missed("[ORG] Atmosphere and Fog: the post-water composite did not run this frame");
		s_missed.Report(result.status == FrameAnchors::RunStatus::Idle ? "idle (the schedule does not hold it yet)" :
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
	// A new key builds the program again. The registered one keeps compositing until it is built.
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
