#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGScreenSpaceGI.h"

#	include "Deferred.h"
#	include "Features/ScreenSpaceGI.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ProgramCache.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/Switches.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/OwnedDescriptorBinding.h>
#	include <Resources/ExternalTextureResource.h>

#	include <array>
#	include <format>
#	include <map>
#	include <vector>

namespace
{
	namespace AnchorPasses = RenderGraph::AnchorPasses;
	using AnchorPasses::Bind;

	constexpr const char* kClientId = "cs.ssgi";
	// The history clears, a client of their own: new inputs register the chain again without clearing the history.
	constexpr const char* kResetClientId = "cs.ssgi.reset";
	constexpr const char* kNoiseName = "ssgi.noise";
	constexpr std::uint32_t kMips = 5;
	// Below Subsurface Scattering's keys (199-202): the D3D11 frame runs SSGI first.
	constexpr std::int32_t kOrderReset = 100;
	constexpr std::int32_t kOrderFirstStep = 110;
	constexpr std::size_t kHistoryTextures = 6;

	/** @brief The chain's dispatches, in their D3D11 order; also each one's run gate entry. */
	enum Step : std::uint32_t
	{
		kPrefilterDepths,
		kRadianceDisocc,
		kPrefilterRadiance,
		kPrefilterNormal,
		kGi,
		kBlur,
		kUpsample,
		kSteps
	};

	struct StepInfo
	{
		const char* name;
		const char* source;
		std::uint32_t views;  // the shader's SRV and UAV entries, after its SHARED, latch and sampler entries
	};

	constexpr std::array<StepInfo, kSteps> kStepInfo{ {
		{ "prefilter-depths", "ScreenSpaceGI/prefilterDepths.cs.hlsl", 1 + kMips },
		{ "radiance-disocc", "ScreenSpaceGI/radianceDisocc.cs.hlsl", 10 + 6 },
		{ "prefilter-radiance", "ScreenSpaceGI/prefilterRadiance.cs.hlsl", 1 + kMips },
		{ "prefilter-normal", "ScreenSpaceGI/prefilterNormal.cs.hlsl", 1 + kMips },
		{ "gi", "ScreenSpaceGI/gi.cs.hlsl", 10 + 5 },
		{ "blur", "ScreenSpaceGI/blur.cs.hlsl", 5 + 3 },
		{ "upsample", "ScreenSpaceGI/upsample.cs.hlsl", 5 + 4 },
	} };

	// Each push block: the four shared constant handles, the latch (handle, offset), the point and linear samplers, then the views.
	constexpr std::uint32_t kFixedWords = 4 + 2 + 2;

	static_assert(sizeof(ScreenSpaceGI::SSGICB) % 16 == 0);

	/**
	 * @brief CS_ORG_SSGI_DEBUG=<n>: a diagnostic that changes one thing about the graph path, to find which part of it goes wrong
	 * with one run per mode. Unset or 0: off. Read once, logged at setup.
	 */
	enum class Debug : std::uint32_t
	{
		Off,
		NoAliasing,    // 1: every transient texture is made a history one, so nothing is aliased
		NoOutputs,     // 2: the chain runs, but the composite gets no SSGI (null inputs)
		NoChain,       // 3: no dispatch runs (gates stay closed) and the outputs are cleared each run
		ClearCoCg,     // 4: the CoCg output is cleared each run, after the chain (luma only)
		ClearHistory,  // 5: every history texture is cleared each run, before the chain (no temporal reuse)
		NoBlur,        // 6: the blur is off, whatever the setting
		NoTemporal,    // 7: the temporal denoiser is off, whatever the setting
		FullRes,       // 8: full resolution, whatever the setting (no upsample)
		MainDepth,     // 9: the depth input is the engine's kMAIN, not the final depth copy
		Count
	};

	constexpr std::array<const char*, static_cast<std::size_t>(Debug::Count)> kDebugNames{ "off", "no aliasing", "no outputs to the composite",
		"no dispatches, outputs cleared", "CoCg output cleared", "history cleared each run", "blur off", "temporal denoiser off",
		"full resolution", "depth from kMAIN" };

	Debug DebugMode()
	{
		static const Debug mode = [] {
			const auto value = Switches::Number("CS_ORG_SSGI_DEBUG", 0);
			return value < static_cast<std::uint64_t>(Debug::Count) ? static_cast<Debug>(value) : Debug::Off;
		}();
		return mode;
	}

	/** @brief The settings the programs and the textures are made from: a change makes them again. */
	struct Config
	{
		int resolutionMode = 0;
		bool enabled = false;
		bool gi = false;
		bool specular = false;
		bool temporal = false;
		bool blur = false;

		bool operator==(const Config&) const = default;

		bool FullRes() const { return resolutionMode == 0; }
		// The shaders touch the specular textures with the specular GI or the temporal denoiser on.
		bool SpecularTextures() const { return specular || temporal; }

		static Config Current()
		{
			const auto& settings = globals::features::screenSpaceGI.settings;
			Config config{ settings.ResolutionMode, settings.Enabled, settings.EnableGI, settings.EnableExperimentalSpecularGI,
				settings.EnableTemporalDenoiser, settings.EnableBlur };
			// CS_ORG_SSGI_DEBUG's setting overrides.
			switch (DebugMode()) {
			case Debug::NoBlur:
				config.blur = false;
				break;
			case Debug::NoTemporal:
				config.temporal = false;
				break;
			case Debug::FullRes:
				config.resolutionMode = 0;
				break;
			default:
				break;
			}
			return config;
		}
	};

	/** @brief A step's own defines, as CompileComputeShaders gives them. The program adds CS's common ones itself. */
	std::vector<std::pair<std::string, std::string>> StepDefines(Step a_step, const Config& a_config)
	{
		std::vector<std::pair<std::string, std::string>> defines;
		if (a_step == kPrefilterDepths)
			defines.emplace_back("LINEAR_FILTER", "");
		if (a_config.resolutionMode == 1)
			defines.emplace_back("HALF_RES", "");
		if (a_config.resolutionMode == 2)
			defines.emplace_back("QUARTER_RES", "");
		if (a_config.temporal)
			defines.emplace_back("TEMPORAL_DENOISER", "");
		if (a_config.gi)
			defines.emplace_back("GI", "");
		if (a_config.specular)
			defines.emplace_back("GI_SPECULAR", "");
		return defines;
	}

	/** @brief A step's program key: the step and the settings its defines come from (not enabled or blur, which add none). */
	std::uint32_t ProgramKey(const Config& a_config, std::uint32_t a_step)
	{
		return a_step | (static_cast<std::uint32_t>(a_config.resolutionMode) << 4) | (static_cast<std::uint32_t>(a_config.temporal) << 6) |
		       (static_cast<std::uint32_t>(a_config.gi) << 7) | (static_cast<std::uint32_t>(a_config.specular) << 8);
	}

	/** @brief A step's program for a configuration. */
	ComputeProgram::Desc StepDesc(const Config& a_config, std::uint32_t a_step)
	{
		return { .source = kStepInfo[a_step].source,
			.constantWords = kFixedWords + kStepInfo[a_step].views,
			.defines = AnchorPasses::Defines(StepDefines(static_cast<Step>(a_step), a_config)) };
	}

	/** @brief Whether a configuration dispatches a step. */
	bool UsesStep(const Config& a_config, std::uint32_t a_step)
	{
		return !(a_step == kBlur && !a_config.blur) && !(a_step == kUpsample && a_config.FullRes());
	}

	/** @brief Every configuration the settings can select, for the programs: each resolution mode and define option, blur on. */
	std::vector<Config> SettingsConfigs()
	{
		std::vector<Config> configs;
		for (int mode = 0; mode <= 2; ++mode)
			for (std::uint32_t bits = 0; bits < 8; ++bits)
				configs.push_back({ .resolutionMode = mode, .enabled = true, .gi = (bits & 1) != 0, .specular = (bits & 2) != 0, .temporal = (bits & 4) != 0, .blur = true });
		return configs;
	}

	/** @brief The engine and feature textures the chain reads, as graph textures. */
	struct Inputs
	{
		std::shared_ptr<org::ExternalTextureResource> depth, main, normalRoughness, motionVectors, noise;

		bool operator==(const Inputs&) const = default;
		explicit operator bool() const { return depth && main && normalRoughness && motionVectors && noise; }
	};

	using Texture = std::unique_ptr<GraphResources::Texture2D>;

	/** @brief What one configuration's passes are made from. Made on the render thread; a new one replaces it on a settings change. */
	struct GiState
	{
		Config config;
		std::array<std::shared_ptr<const ComputeProgram>, kSteps> programs;
		Inputs inputs;

		// Transient within the chain: aliasable unless exported.
		Texture workingDepth, normal, radiance, radianceTemp, accumScratch, aoScratch, yScratch, coCgScratch, specScratch, yBlurInput,
			coCgBlurInput;
		// Read by the next frame's disocclusion pass.
		Texture accum, ao, y, coCg, spec, prevGeo;

		/** @brief The textures the composite reads: the history at full resolution, the upsampled scratch otherwise. */
		const Texture& OutAo() const { return config.FullRes() ? ao : aoScratch; }
		const Texture& OutY() const { return config.FullRes() ? y : yScratch; }
		const Texture& OutCoCg() const { return config.FullRes() ? coCg : coCgScratch; }
		const Texture& OutSpec() const { return config.FullRes() ? spec : specScratch; }

		std::vector<const Texture*> All() const
		{
			return { &workingDepth, &normal, &radiance, &radianceTemp, &accumScratch, &aoScratch, &yScratch, &coCgScratch, &specScratch,
				&yBlurInput, &coCgBlurInput, &accum, &ao, &y, &coCg, &spec, &prevGeo };
		}

		std::array<const Texture*, kHistoryTextures> History() const { return { &accum, &ao, &y, &coCg, &spec, &prevGeo }; }
	};

	std::shared_ptr<org::Resource> GraphOf(const Texture& a_texture) { return a_texture ? a_texture->Graph() : nullptr; }

	/** @brief A shader resource entry, or handle 0 for a texture the permutation does not touch. */
	Bind Srv(std::shared_ptr<org::Resource> a_resource) { return a_resource ? Bind::Srv(std::move(a_resource)) : Bind::Unused(); }
	Bind Uav(std::shared_ptr<org::Resource> a_resource) { return a_resource ? Bind::Uav(std::move(a_resource)) : Bind::Unused(); }

	/** @brief A UAV entry for each of the kMips mips. */
	std::vector<Bind> UavMips(const std::shared_ptr<org::Resource>& a_resource)
	{
		std::vector<Bind> mips;
		for (std::uint32_t mip = 0; mip < kMips; ++mip)
			mips.push_back(a_resource ? Bind::Uav(a_resource, mip) : Bind::Unused());
		return mips;
	}

	/** @brief A step's resource entries, in its shader's list order after the fixed entries. */
	std::vector<Bind> StepBinds(const GiState& a_state, Step a_step)
	{
		const auto& s = a_state;
		const auto& in = s.inputs;
		// With the blur, GI writes scratch textures that the blur reads, and the blur writes the history.
		const bool blur = s.config.blur;
		std::vector<Bind> binds;
		const auto add = [&](std::vector<Bind> a_more) { binds.insert(binds.end(), a_more.begin(), a_more.end()); };
		switch (a_step) {
		case kPrefilterDepths:
			add({ Srv(in.depth) });
			add(UavMips(GraphOf(s.workingDepth)));
			break;
		case kRadianceDisocc:
			add({ Srv(in.main), Srv(GraphOf(s.workingDepth)), Srv(in.normalRoughness), Srv(GraphOf(s.prevGeo)), Srv(in.motionVectors),
				Srv(GraphOf(s.accum)), Srv(GraphOf(s.ao)), Srv(GraphOf(s.y)), Srv(GraphOf(s.coCg)), Srv(GraphOf(s.spec)),
				Uav(GraphOf(s.radianceTemp)), Uav(GraphOf(s.accumScratch)), Uav(GraphOf(s.aoScratch)), Uav(GraphOf(s.yScratch)),
				Uav(GraphOf(s.coCgScratch)), Uav(GraphOf(s.specScratch)) });
			break;
		case kPrefilterRadiance:
			add({ Srv(GraphOf(s.radianceTemp)) });
			add(UavMips(GraphOf(s.radiance)));
			break;
		case kPrefilterNormal:
			add({ Srv(in.normalRoughness) });
			add(UavMips(GraphOf(s.normal)));
			break;
		case kGi:
			add({ Srv(GraphOf(s.workingDepth)), Srv(in.normalRoughness), Srv(GraphOf(s.radiance)), Srv(in.noise), Srv(GraphOf(s.accumScratch)),
				Srv(GraphOf(s.yScratch)), Srv(GraphOf(s.coCgScratch)), Srv(GraphOf(s.specScratch)), Srv(GraphOf(s.normal)), Srv(GraphOf(s.aoScratch)),
				Uav(GraphOf(s.ao)), Uav(GraphOf(blur ? s.yBlurInput : s.y)), Uav(GraphOf(blur ? s.coCgBlurInput : s.coCg)), Uav(GraphOf(s.spec)),
				Uav(GraphOf(s.prevGeo)) });
			break;
		case kBlur:
			add({ Srv(GraphOf(s.workingDepth)), Srv(in.normalRoughness), Srv(GraphOf(s.accumScratch)), Srv(GraphOf(s.yBlurInput)),
				Srv(GraphOf(s.coCgBlurInput)), Uav(GraphOf(s.accum)), Uav(GraphOf(s.y)), Uav(GraphOf(s.coCg)) });
			break;
		case kUpsample:
			add({ Srv(GraphOf(s.workingDepth)), Srv(GraphOf(s.ao)), Srv(GraphOf(s.y)), Srv(GraphOf(s.coCg)), Srv(GraphOf(s.spec)),
				Uav(GraphOf(s.aoScratch)), Uav(GraphOf(s.yScratch)), Uav(GraphOf(s.coCgScratch)), Uav(GraphOf(s.specScratch)) });
			break;
		default:
			break;
		}
		return binds;
	}

	/** @brief The inputs as they are now: the final depth copy, the main target, the GBuffer normals, motion vectors and noise. */
	Inputs CurrentInputs()
	{
		auto& feature = globals::features::screenSpaceGI;
		Inputs inputs;
		// At DeferredPasses the D3D11 path reads Util::GetCurrentSceneDepthSRV(), which is the final depth copy by then.
		inputs.depth = FrameAnchors::EngineDepth(DebugMode() == Debug::MainDepth ? RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN :
		                                                                           RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY);
		if (globals::deferred)
			inputs.main = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(globals::deferred->forwardRenderTargets[0]));
		inputs.normalRoughness = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(NORMALROUGHNESS));
		inputs.motionVectors = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kMOTION_VECTOR));
		if (feature.texNoise)
			inputs.noise = FrameAnchors::D3D11Texture(kNoiseName, feature.texNoise->resource.get(), feature.texNoise->srv.get(), nullptr);
		return inputs;
	}
}

struct ORGScreenSpaceGI::Impl
{
	std::shared_ptr<GiState> state;
	org::OwnedDescriptorBinding pointSampler, linearSampler;
	// SSGICB, the shaders' latch entry, written by the chain's commit each run.
	std::shared_ptr<org::LatchBlock> latch;
	// One entry per step: a run does only the steps its commit opened.
	std::shared_ptr<RenderGraph::RunGate> gate;
	D3D11_TEXTURE2D_DESC main{};
	std::function<void()> onFailed;
	// The programs by ProgramKey: the current configuration's built at setup, every other one queued then on the program build
	// thread (prebuilt) so a settings change finds it. Taken into programs at first use.
	std::map<std::uint32_t, std::shared_ptr<const ComputeProgram>> programs;
	RenderGraph::ProgramCache<ComputeProgram, std::uint32_t> prebuilt{ "Screen Space GI" };
	bool registered = false;
	// Set by Prepass on frames the chain has its inputs: the client's active.
	bool frameReady = false;
	bool failed = false;

	/** @brief A texture of the chain, the size of the main target. */
	Texture MakeTexture(DXGI_FORMAT a_format, UINT a_mips, const char* a_name, GraphResources::Lifetime a_lifetime, bool a_exported) const
	{
		if (DebugMode() == Debug::NoAliasing && a_lifetime == GraphResources::Lifetime::Transient)
			a_lifetime = GraphResources::Lifetime::History;
		D3D11_TEXTURE2D_DESC desc{
			.Width = main.Width,
			.Height = main.Height,
			.MipLevels = a_mips,
			.ArraySize = 1,
			.Format = a_format,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		return std::make_unique<GraphResources::Texture2D>(desc, a_name,
			GraphResources::Options{ .lifetime = a_lifetime, .d3d11 = a_exported ? GraphResources::Export::SRV : GraphResources::Export::None });
	}

	/** @brief Queues every configuration's programs on the program build thread, except those already built. */
	void Prebuild()
	{
		for (const auto& config : SettingsConfigs())
			for (std::uint32_t step = 0; step < kSteps; ++step)
				if (UsesStep(config, step) && !programs.contains(ProgramKey(config, step)))
					(void)prebuilt.Get(ProgramKey(config, step), [&] { return StepDesc(config, step); });
	}

	/**
	 * @brief A step's program: built before, or by Prebuild's queue, or built now on this thread. a_expected: setup or a shader
	 * reload, where building now is the plan; otherwise it is logged (a program setup did not prebuild in time).
	 */
	std::shared_ptr<const ComputeProgram> ProgramFor(const Config& a_config, std::uint32_t a_step, bool a_expected)
	{
		const auto key = ProgramKey(a_config, a_step);
		if (const auto found = programs.find(key); found != programs.end())
			return found->second;
		auto program = prebuilt.StatusOf(key) == RenderGraph::ProgramStatus::Ready ? prebuilt.Get(key, [&] { return StepDesc(a_config, a_step); }) : nullptr;
		if (!program) {
			if (!a_expected)
				logger::info("[ORG] Screen Space GI: {} for these settings was not prebuilt yet; building it now", kStepInfo[a_step].source);
			auto* host = RenderGraphRuntime::Get().Host();
			program = host ? ComputeProgram::Load(host->Device(), StepDesc(a_config, a_step)) : nullptr;
		}
		if (program)
			programs.emplace(key, program);
		return program;
	}

	/** @brief The programs and textures of a configuration; null, logged, when one cannot be made. a_expected: as ProgramFor. */
	std::shared_ptr<GiState> Build(const Config& a_config, bool a_expected)
	{
		auto gi = std::make_shared<GiState>();
		gi->config = a_config;
		if (!a_config.enabled)
			return gi;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return nullptr;
		for (std::uint32_t step = 0; step < kSteps; ++step) {
			if (!UsesStep(a_config, step))
				continue;
			gi->programs[step] = ProgramFor(a_config, step, a_expected);
			if (!gi->programs[step]) {
				logger::error("[ORG] Screen Space GI: {} could not be built", kStepInfo[step].source);
				return nullptr;
			}
		}

		using enum GraphResources::Lifetime;
		const bool full = a_config.FullRes();
		gi->workingDepth = MakeTexture(DXGI_FORMAT_R16_FLOAT, kMips, "SSGI::WorkingDepth", Transient, false);
		gi->normal = MakeTexture(DXGI_FORMAT_R8G8_UNORM, kMips, "SSGI::Normal", Transient, false);
		gi->radiance = MakeTexture(DXGI_FORMAT_R11G11B10_FLOAT, kMips, "SSGI::Radiance", Transient, false);
		gi->radianceTemp = MakeTexture(DXGI_FORMAT_R11G11B10_FLOAT, 1, "SSGI::RadianceTemp", Transient, false);
		gi->accumScratch = MakeTexture(DXGI_FORMAT_R8_UNORM, 1, "SSGI::AccumFramesScratch", Transient, false);
		gi->accum = MakeTexture(DXGI_FORMAT_R8_UNORM, 1, "SSGI::AccumFrames", History, false);
		// 8-bit history stalls the temporal AO blend.
		gi->ao = MakeTexture(DXGI_FORMAT_R16_UNORM, 1, "SSGI::AO", History, full);
		gi->aoScratch = MakeTexture(DXGI_FORMAT_R16_UNORM, 1, "SSGI::AOScratch", Transient, !full);
		gi->y = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::IlY", History, full);
		gi->yScratch = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::IlYScratch", Transient, !full);
		gi->coCg = MakeTexture(DXGI_FORMAT_R16G16_FLOAT, 1, "SSGI::IlCoCg", History, full);
		gi->coCgScratch = MakeTexture(DXGI_FORMAT_R16G16_FLOAT, 1, "SSGI::IlCoCgScratch", Transient, !full);
		if (a_config.blur) {
			gi->yBlurInput = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::IlYBlurInput", Transient, false);
			gi->coCgBlurInput = MakeTexture(DXGI_FORMAT_R16G16_FLOAT, 1, "SSGI::IlCoCgBlurInput", Transient, false);
		}
		if (a_config.SpecularTextures()) {
			gi->spec = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::GiSpecular", History, full);
			gi->specScratch = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::GiSpecularScratch", Transient, !full);
		}
		gi->prevGeo = MakeTexture(DXGI_FORMAT_R11G11B10_FLOAT, 1, "SSGI::PrevGeo", History, false);
		for (const auto* texture : gi->All()) {
			// Only the optional ones may be missing, and only when their option is off.
			if (*texture && !**texture) {
				logger::error("[ORG] Screen Space GI: {} could not be made", (*texture)->Name());
				return nullptr;
			}
		}
		for (const Texture* exported : { &gi->OutAo(), &gi->OutY(), &gi->OutCoCg() }) {
			if (!*exported || !(*exported)->SRV()) {
				logger::error("[ORG] Screen Space GI: the composite's inputs could not be exported to D3D11");
				return nullptr;
			}
		}
		return gi;
	}

	/** @brief Every texture of a state, registered with each build of the anchor's graph. */
	static std::function<void(org::RenderGraph&)> Registrar(std::shared_ptr<const GiState> a_state)
	{
		return [a_state](org::RenderGraph& a_graph) {
			for (const auto* texture : a_state->All())
				if (*texture)
					(*texture)->Register(a_graph);
		};
	}

	/** @brief Both clients' onFailed: the graph failed, so the feature returns to D3D11 for good. */
	std::function<void()> FailedCallback()
	{
		return [this] {
			if (failed)
				return;
			logger::error("[ORG] Screen Space GI returns to D3D11: the render graph failed");
			Stop();
		};
	}

	/**
	 * @brief The chain's client for the current state and inputs: every step behind its gate entry, then the accumulation copy.
	 * Registering again replaces it, so the anchor builds its graph with the new textures.
	 */
	bool RegisterChain()
	{
		AnchorPasses::Builder builder(FrameAnchors::Anchor::DeferredPasses, kClientId, kOrderFirstStep, latch, gate);
		// CS_ORG_SSGI_DEBUG's clears: every run, not one-shot.
		const auto clearEachRun = [&](const char* a_name, const Texture& a_texture) {
			if (a_texture)
				builder.Add(AnchorPasses::Clear{ .name = a_name, .target = a_texture->Graph() }, false);
		};
		if (DebugMode() == Debug::ClearHistory) {
			clearEachRun("debug-clear-accum", state->accum);
			clearEachRun("debug-clear-ao", state->ao);
			clearEachRun("debug-clear-y", state->y);
			clearEachRun("debug-clear-cocg", state->coCg);
			clearEachRun("debug-clear-spec", state->spec);
			clearEachRun("debug-clear-prev-geo", state->prevGeo);
		}
		for (std::uint32_t step = 0; step < kSteps; ++step) {
			if (!state->programs[step])
				continue;
			std::vector<Bind> binds{ Bind::Shared(), Bind::Latch(0), Bind::Handle(pointSampler.Index()), Bind::Handle(linearSampler.Index()) };
			for (auto& bind : StepBinds(*state, static_cast<Step>(step)))
				binds.push_back(std::move(bind));
			builder.Add(AnchorPasses::Dispatch{ .name = kStepInfo[step].name, .program = state->programs[step], .bindings = std::move(binds), .gate = step });
		}
		// Without the blur, the next frame reads the counts the disocclusion pass wrote, which it cannot write in place.
		if (!state->config.blur)
			builder.Add(AnchorPasses::Copy{ .name = "keep-accumulation",
				.source = state->accumScratch->Graph(),
				.target = state->accum->Graph(),
				.width = state->accum->Desc().Width,
				.height = state->accum->Desc().Height });
		if (DebugMode() == Debug::NoChain) {
			clearEachRun("debug-clear-out-ao", state->OutAo());
			clearEachRun("debug-clear-out-y", state->OutY());
			clearEachRun("debug-clear-out-cocg", state->OutCoCg());
			clearEachRun("debug-clear-out-spec", state->OutSpec());
		}
		if (DebugMode() == Debug::ClearCoCg)
			clearEachRun("debug-clear-out-cocg", state->OutCoCg());
		if (builder.Failed()) {
			logger::error("[ORG] Screen Space GI: {}", builder.Failure());
			return false;
		}

		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.passes = builder.Take();
		client.sharedConstants = builder.ReadsSharedConstants();
		client.gates = { gate };
		client.registerResources = Registrar(state);
		client.commit = [this](const FrameAnchors::Point&) { Commit(); };
		client.active = [this] { return frameReady; };
		client.onFailed = FailedCallback();
		registered = FrameAnchors::Register(std::move(client));
		return registered;
	}

	/**
	 * @brief The history clears as one-shot passes: they run at the anchor's next run after this registration (a new state) or
	 * after FrameAnchors::Rearm (a history reset). The client is never active itself, so it keeps no epoch running.
	 */
	bool RegisterReset()
	{
		AnchorPasses::Builder builder(FrameAnchors::Anchor::DeferredPasses, kResetClientId, kOrderReset);
		for (std::size_t i = 0; i < kHistoryTextures; ++i)
			if (const auto& texture = *state->History()[i])
				builder.Add(AnchorPasses::Clear{ .name = std::format("{}", i), .target = texture->Graph() });
		FrameAnchors::ClientDesc client{};
		client.id = kResetClientId;
		client.passes = builder.Take();
		client.registerResources = Registrar(state);
		client.active = [] { return false; };
		client.onFailed = FailedCallback();
		return FrameAnchors::Register(std::move(client));
	}

	/** @brief This run's constants, and the steps it does with their group counts. */
	void Commit() const
	{
		if (!state || !state->config.enabled)
			return;
		auto& feature = globals::features::screenSpaceGI;
		const auto constants = feature.BuildConstants({ static_cast<float>(state->ao->Desc().Width), static_cast<float>(state->ao->Desc().Height) });
		latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, constants);
		if (DebugMode() == Debug::NoChain)
			return;
		// The internal size from the built state's resolution mode, which the programs were made for (and the debug switch may
		// override), as GetDispatchSizes gives it from the setting.
		const auto full = feature.GetDispatchSizes().full;
		const auto shift = static_cast<uint>(state->config.resolutionMode);
		const std::array<uint, 2> internal{ full[0] >> shift, full[1] >> shift };
		const auto open = [&](Step a_step, const std::array<uint, 2>& a_size, uint a_tile) {
			if (state->programs[a_step])
				gate->OpenDispatch(a_step, (a_size[0] + a_tile - 1) / a_tile, (a_size[1] + a_tile - 1) / a_tile);
		};
		open(kPrefilterDepths, full, 16);
		open(kRadianceDisocc, internal, 8);
		open(kPrefilterRadiance, internal, 16);
		open(kPrefilterNormal, internal, 16);
		open(kGi, internal, 8);
		open(kBlur, internal, 8);
		open(kUpsample, full, 8);
	}

	/** @brief Takes both clients out of the anchor's graph. */
	void Unregister()
	{
		registered = false;
		FrameAnchors::Unregister(kClientId);
		FrameAnchors::Unregister(kResetClientId);
	}

	/** @brief Leaves the graph path for good: the feature's D3D11 path takes over. */
	void Stop()
	{
		failed = true;
		frameReady = false;
		Unregister();
		if (onFailed)
			onFailed();
	}
};

bool ORGScreenSpaceGI::Requested()
{
	static const bool requested = Switches::On("CS_ORG_SCREEN_SPACE_GI");
	return requested;
}

ORGScreenSpaceGI& ORGScreenSpaceGI::Get()
{
	static ORGScreenSpaceGI s_gi;
	return s_gi;
}

ORGScreenSpaceGI::~ORGScreenSpaceGI() = default;

void ORGScreenSpaceGI::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::DeferredPasses))
		logger::warn("[ORG] Screen Space GI stays on D3D11: the DeferredPasses anchor could not be reserved");
}

bool ORGScreenSpaceGI::Setup(const D3D11_TEXTURE2D_DESC& a_main, std::function<void()> a_onFailed)
{
	// The clients' callbacks point at the Impl: they go with it.
	if (impl)
		impl->Unregister();
	impl.reset();
	if (!Requested())
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.Host() || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::DeferredPasses)) {
		logger::warn("[ORG] Screen Space GI stays on D3D11: the render graph or its DeferredPasses anchor is not available");
		return false;
	}
	try {
		auto next = std::make_unique<Impl>();
		next->main = a_main;
		next->onFailed = std::move(a_onFailed);
		next->pointSampler = ShaderBindings::CreateSampler({ .minFilter = rhi::Filter::Nearest,
			.magFilter = rhi::Filter::Nearest,
			.mipFilter = rhi::MipFilter::Nearest });
		next->linearSampler = ShaderBindings::CreateSampler({});
		next->latch = std::make_shared<org::LatchBlock>("cs.ssgi.latch", static_cast<std::uint32_t>(sizeof(ScreenSpaceGI::SSGICB)));
		next->gate = RenderGraph::RunGate::Create("cs.ssgi.gate", kSteps);
		next->state = next->Build(Config::Current(), true);
		// The other configurations' programs, off the render thread: a settings change then makes no compile.
		next->Prebuild();
		if (!next->pointSampler || !next->linearSampler || !next->gate || !next->state) {
			logger::warn("[ORG] Screen Space GI stays on D3D11: its samplers, programs or textures could not be made");
			return false;
		}
		impl = std::move(next);
		logger::info("[ORG] Screen Space GI runs on the render graph ({}x{}, resolution mode {}, blur {})", a_main.Width, a_main.Height,
			impl->state->config.resolutionMode, impl->state->config.blur);
		if (const auto mode = DebugMode(); mode != Debug::Off)
			logger::warn("[ORG] Screen Space GI debug: CS_ORG_SSGI_DEBUG={} ({}); temporal {}, GI {}, specular {}", static_cast<std::uint32_t>(mode),
				kDebugNames[static_cast<std::size_t>(mode)], impl->state->config.temporal, impl->state->config.gi, impl->state->config.specular);
		return true;
	} catch (const std::exception& e) {
		logger::error("[ORG] Screen Space GI setup failed, staying on D3D11: {}", e.what());
		impl.reset();
		return false;
	}
}

void ORGScreenSpaceGI::Prepass()
{
	if (!IsActive())
		return;
	auto& self = *impl;
	self.frameReady = false;
	auto& feature = globals::features::screenSpaceGI;
	const auto fail = [&](const char* a_reason) {
		logger::error("[ORG] Screen Space GI returns to D3D11: {}", a_reason);
		self.Stop();
	};

	// A setting the programs or textures depend on, or a recompile from the menu: both made again, and the history with them.
	bool newState = false;
	if (feature.recompileFlag) {
		// The D3D11 shaders too, so a later fallback finds them current. It also queues the history reset. The graph's programs are
		// built again from the reloaded sources: the current ones now, the others queued again.
		feature.ClearShaderCache();
		self.programs.clear();
		self.prebuilt.Clear();
		newState = true;
	}
	const auto config = Config::Current();
	if (newState || config != self.state->config) {
		const bool reload = newState;
		auto state = self.Build(config, reload);
		if (!state)
			return fail("the new settings' programs or textures could not be made");
		if (reload)
			self.Prebuild();
		state->inputs = self.state->inputs;
		self.state = std::move(state);
		newState = true;
	}
	if (!self.state->config.enabled) {
		if (self.registered)
			self.Unregister();
		return;
	}

	// A new engine texture (a resolution change) or a new noise texture: the chain's passes are made again.
	bool newInputs = false;
	if (auto inputs = CurrentInputs(); !(inputs == self.state->inputs)) {
		if (!inputs)
			return fail("an input could not be imported");
		self.state->inputs = std::move(inputs);
		newInputs = true;
	}

	const bool reset = feature.queuedResetHistory.exchange(false);
	if (!self.registered || newState) {
		// Registered at the first frame, with the inputs imported on the render thread. A new registration of the clears runs
		// them once, so a new state starts from a cleared history.
		if (!self.RegisterReset() || !self.RegisterChain())
			return fail("its anchor clients could not be registered");
	} else {
		if (newInputs && !self.RegisterChain())
			return fail("its anchor client could not be registered");
		if (reset)
			FrameAnchors::Rearm(kResetClientId);
	}
	self.frameReady = true;
}

bool ORGScreenSpaceGI::IsActive() const
{
	return impl && !impl->failed && RenderGraphRuntime::Get().IsActive();
}

ORGScreenSpaceGI::Outputs ORGScreenSpaceGI::GetOutputs() const
{
	if (!IsActive() || !impl->state->config.enabled || DebugMode() == Debug::NoOutputs)
		return {};
	const auto& state = *impl->state;
	const auto srv = [](const Texture& a_texture) { return a_texture ? a_texture->SRV() : nullptr; };
	return { srv(state.OutAo()), srv(state.OutY()), srv(state.OutCoCg()), srv(state.OutSpec()) };
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Screen Space GI always draws with D3D11.
#	include "ORGScreenSpaceGI.h"

struct ORGScreenSpaceGI::Impl
{};

bool ORGScreenSpaceGI::Requested() { return false; }

ORGScreenSpaceGI& ORGScreenSpaceGI::Get()
{
	static ORGScreenSpaceGI s_gi;
	return s_gi;
}

ORGScreenSpaceGI::~ORGScreenSpaceGI() = default;
void ORGScreenSpaceGI::Reserve() {}
bool ORGScreenSpaceGI::Setup(const D3D11_TEXTURE2D_DESC&, std::function<void()>) { return false; }
void ORGScreenSpaceGI::Prepass() {}
bool ORGScreenSpaceGI::IsActive() const { return false; }
ORGScreenSpaceGI::Outputs ORGScreenSpaceGI::GetOutputs() const { return {}; }

#endif
