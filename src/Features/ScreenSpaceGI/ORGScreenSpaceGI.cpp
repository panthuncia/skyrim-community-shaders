#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGScreenSpaceGI.h"
#	include "RenderGraph/Engine/Native/Native.h"

#	include "Deferred.h"
#	include "Features/ScreenSpaceGI.h"
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

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/OwnedDescriptorBinding.h>
#	include <Resources/ExternalTextureResource.h>

#	include <array>
#	include <format>
#	include <map>
#	include <string_view>
#	include <vector>

namespace
{
	namespace AnchorPasses = RenderGraph::AnchorPasses;
	using AnchorPasses::Bind;

	constexpr const char* kClientId = "cs.ssgi";
	// A separate client, so new inputs can re-register the chain without clearing the history.
	constexpr const char* kResetClientId = "cs.ssgi.reset";
	constexpr const char* kNoiseName = "ssgi.noise";
	constexpr std::uint32_t kMips = 5;
	// Below Subsurface Scattering's keys (199-202), since the D3D11 frame runs SSGI first.
	constexpr std::int32_t kOrderReset = 100;
	constexpr std::int32_t kOrderFirstStep = 110;
	constexpr std::size_t kHistoryTextures = 6;

	/** @brief In D3D11 order. Also each step's run gate entry. */
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
		std::uint32_t views;  // SRV and UAV entries after the SHARED, latch and sampler entries
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

	// Four shared constant handles, the latch handle and offset, point and linear samplers, then the views.
	constexpr std::uint32_t kFixedWords = 4 + 2 + 2;

	static_assert(sizeof(ScreenSpaceGI::SSGICB) % 16 == 0);

	/** @brief CS_ORG_SSGI_DEBUG=<n> changes one thing about the graph path, to bisect a fault with one run per mode. Read once. */
	enum class Debug : std::uint32_t
	{
		Off,
		NoAliasing,    // 1: no transient is aliased
		NoOutputs,     // 2: the composite gets no SSGI
		NoChain,       // 3: no dispatch runs and the outputs are cleared each run
		ClearCoCg,     // 4: CoCg cleared after the chain, luma only
		ClearHistory,  // 5: histories cleared before the chain, no temporal reuse
		NoBlur,        // 6: blur off
		NoTemporal,    // 7: temporal denoiser off
		FullRes,       // 8: full resolution, no upsample
		MainDepth,     // 9: depth from kMAIN instead of the final depth copy
		// 10-15: one transient kept out of the alias pool.
		KeepWorkingDepth,
		KeepRadianceTemp,
		KeepNormal,
		KeepRadiance,
		KeepAccumScratch,
		KeepBlurInputs,
		KeepSssDiffuse,  // 16: ORGSubsurfaceScattering.cpp reads this switch
		PrivatePool,     // 17: SSGI's transients alias only with each other, in a private pool
		Count
	};

	constexpr std::array<const char*, static_cast<std::size_t>(Debug::Count)> kDebugNames{ "off", "no aliasing", "no outputs to the composite",
		"no dispatches, outputs cleared", "CoCg output cleared", "history cleared each run", "blur off", "temporal denoiser off",
		"full resolution", "depth from kMAIN", "WorkingDepth not aliased", "RadianceTemp not aliased", "Normal not aliased",
		"Radiance not aliased", "AccumFramesScratch not aliased", "blur inputs not aliased", "SSS::DiffuseNoAlbedo not aliased",
		"SSGI's own alias pool" };

	// Any value no other resource uses.
	constexpr std::uint64_t kPrivatePool = 0x5591'0000'0000'0017ull;

	Debug DebugMode()
	{
		static const Debug mode = [] {
			const auto value = Switches::Number("CS_ORG_SSGI_DEBUG", 0);
			return value < static_cast<std::uint64_t>(Debug::Count) ? static_cast<Debug>(value) : Debug::Off;
		}();
		return mode;
	}

	bool KeptApart(std::string_view a_name)
	{
		switch (DebugMode()) {
		case Debug::NoAliasing:
			return true;
		case Debug::KeepWorkingDepth:
			return a_name == "SSGI::WorkingDepth";
		case Debug::KeepRadianceTemp:
			return a_name == "SSGI::RadianceTemp";
		case Debug::KeepNormal:
			return a_name == "SSGI::Normal";
		case Debug::KeepRadiance:
			return a_name == "SSGI::Radiance";
		case Debug::KeepAccumScratch:
			return a_name == "SSGI::AccumFramesScratch";
		case Debug::KeepBlurInputs:
			return a_name == "SSGI::IlYBlurInput" || a_name == "SSGI::IlCoCgBlurInput";
		default:
			return false;
		}
	}

	/** @brief A change remakes the programs and textures. */
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
		// The shaders touch the specular textures with specular GI or the temporal denoiser on.
		bool SpecularTextures() const { return specular || temporal; }

		static Config Current()
		{
			const auto& settings = globals::features::screenSpaceGI.settings;
			Config config{ settings.ResolutionMode, settings.Enabled, settings.EnableGI, settings.EnableExperimentalSpecularGI,
				settings.EnableTemporalDenoiser, settings.EnableBlur };
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

	/** @brief As CompileComputeShaders gives them. The program adds CS's common defines itself. */
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

	/** @brief Enabled and blur add no defines, so they are not part of the key. */
	std::uint32_t ProgramKey(const Config& a_config, std::uint32_t a_step)
	{
		return a_step | (static_cast<std::uint32_t>(a_config.resolutionMode) << 4) | (static_cast<std::uint32_t>(a_config.temporal) << 6) |
		       (static_cast<std::uint32_t>(a_config.gi) << 7) | (static_cast<std::uint32_t>(a_config.specular) << 8);
	}

	ComputeProgram::Desc StepDesc(const Config& a_config, std::uint32_t a_step)
	{
		return { .source = kStepInfo[a_step].source,
			.constantWords = kFixedWords + kStepInfo[a_step].views,
			.defines = AnchorPasses::Defines(StepDefines(static_cast<Step>(a_step), a_config)) };
	}

	bool UsesStep(const Config& a_config, std::uint32_t a_step)
	{
		return !(a_step == kBlur && !a_config.blur) && !(a_step == kUpsample && a_config.FullRes());
	}

	/** @brief Each resolution mode and define option, with blur on. */
	std::vector<Config> SettingsConfigs()
	{
		std::vector<Config> configs;
		for (int mode = 0; mode <= 2; ++mode)
			for (std::uint32_t bits = 0; bits < 8; ++bits)
				configs.push_back({ .resolutionMode = mode, .enabled = true, .gi = (bits & 1) != 0, .specular = (bits & 2) != 0, .temporal = (bits & 4) != 0, .blur = true });
		return configs;
	}

	struct Inputs
	{
		FrameAnchors::Texture depth, main, normalRoughness, motionVectors, noise;

		bool operator==(const Inputs&) const = default;
		explicit operator bool() const { return depth && main && normalRoughness && motionVectors && noise; }
	};

	// Shared, so a new state can keep a texture from the old one while the old clients still name it.
	using Texture = std::shared_ptr<GraphResources::Texture2D>;

	/** @brief Made on the render thread. A settings change replaces it. */
	struct GiState
	{
		Config config;
		std::array<std::shared_ptr<const ComputeProgram>, kSteps> programs;
		Inputs inputs;

		// Aliasable unless exported.
		Texture workingDepth, normal, radiance, radianceTemp, accumScratch, aoScratch, yScratch, coCgScratch, specScratch, yBlurInput,
			coCgBlurInput;
		// Read by the next frame's disocclusion pass.
		Texture accum, ao, y, coCg, spec, prevGeo;

		/** @brief The history at full resolution, the upsampled scratch otherwise. */
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

	/** @brief Handle 0 for an input the permutation does not touch. */
	Bind Srv(const FrameAnchors::Texture& a_texture) { return a_texture ? Bind::Srv(a_texture) : Bind::Unused(); }

	/** @brief Uses the slot when versioned, so a resize is a new version. Handle 0 for a texture the permutation lacks. */
	Bind Srv(const Texture& a_texture) { return a_texture ? Bind::Srv(*a_texture) : Bind::Unused(); }
	Bind Uav(const Texture& a_texture) { return a_texture ? Bind::Uav(*a_texture) : Bind::Unused(); }

	std::vector<Bind> UavMips(const Texture& a_texture)
	{
		std::vector<Bind> mips;
		for (std::uint32_t mip = 0; mip < kMips; ++mip)
			mips.push_back(a_texture ? Bind::Uav(*a_texture, mip) : Bind::Unused());
		return mips;
	}

	/** @brief In the shader's list order after the fixed entries. */
	std::vector<Bind> StepBinds(const GiState& a_state, Step a_step)
	{
		const auto& s = a_state;
		const auto& in = s.inputs;
		// With the blur, GI writes scratch that the blur reads, and the blur writes the history.
		const bool blur = s.config.blur;
		std::vector<Bind> binds;
		const auto add = [&](std::vector<Bind> a_more) { binds.insert(binds.end(), a_more.begin(), a_more.end()); };
		switch (a_step) {
		case kPrefilterDepths:
			add({ Srv(in.depth) });
			add(UavMips(s.workingDepth));
			break;
		case kRadianceDisocc:
			add({ Srv(in.main), Srv(s.workingDepth), Srv(in.normalRoughness), Srv(s.prevGeo), Srv(in.motionVectors),
				Srv(s.accum), Srv(s.ao), Srv(s.y), Srv(s.coCg), Srv(s.spec),
				Uav(s.radianceTemp), Uav(s.accumScratch), Uav(s.aoScratch), Uav(s.yScratch),
				Uav(s.coCgScratch), Uav(s.specScratch) });
			break;
		case kPrefilterRadiance:
			add({ Srv(s.radianceTemp) });
			add(UavMips(s.radiance));
			break;
		case kPrefilterNormal:
			add({ Srv(in.normalRoughness) });
			add(UavMips(s.normal));
			break;
		case kGi:
			add({ Srv(s.workingDepth), Srv(in.normalRoughness), Srv(s.radiance), Srv(in.noise), Srv(s.accumScratch),
				Srv(s.yScratch), Srv(s.coCgScratch), Srv(s.specScratch), Srv(s.normal), Srv(s.aoScratch),
				Uav(s.ao), Uav(blur ? s.yBlurInput : s.y), Uav(blur ? s.coCgBlurInput : s.coCg), Uav(s.spec),
				Uav(s.prevGeo) });
			break;
		case kBlur:
			add({ Srv(s.workingDepth), Srv(in.normalRoughness), Srv(s.accumScratch), Srv(s.yBlurInput),
				Srv(s.coCgBlurInput), Uav(s.accum), Uav(s.y), Uav(s.coCg) });
			break;
		case kUpsample:
			add({ Srv(s.workingDepth), Srv(s.ao), Srv(s.y), Srv(s.coCg), Srv(s.spec),
				Uav(s.aoScratch), Uav(s.yScratch), Uav(s.coCgScratch), Uav(s.specScratch) });
			break;
		default:
			break;
		}
		return binds;
	}

	/** @brief The chain's textures follow it. False without it. */
	bool MainDesc(D3D11_TEXTURE2D_DESC& a_desc)
	{
		auto* renderer = globals::game::renderer;
		auto* main = renderer ? renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture : nullptr;
		if (!main)
			return false;
		main->GetDesc(&a_desc);
		return true;
	}

	Inputs CurrentInputs()
	{
		auto& feature = globals::features::screenSpaceGI;
		Inputs inputs;
		// At DeferredPasses the D3D11 path's scene depth is already the final depth copy.
		inputs.depth = FrameAnchors::EngineDepth(DebugMode() == Debug::MainDepth ? RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN :
		                                                                           RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY);
		if (globals::deferred)
			inputs.main = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(globals::deferred->forwardRenderTargets[0]));
		inputs.normalRoughness = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(NORMALROUGHNESS));
		inputs.motionVectors = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kMOTION_VECTOR));
		if (feature.texNoise)
			inputs.noise = FrameAnchors::ImmutableTexture(kNoiseName, feature.texNoise->resource.get(), feature.texNoise->srv.get());
		return inputs;
	}
}

struct ORGScreenSpaceGI::Impl
{
	std::shared_ptr<GiState> state;
	org::OwnedDescriptorBinding pointSampler, linearSampler;
	// SSGICB, written by the commit each run.
	std::shared_ptr<org::LatchBlock> latch;
	// One entry per step, so a run does only the steps its commit opened.
	std::shared_ptr<RenderGraph::RunGate> gate;
	D3D11_TEXTURE2D_DESC main{};
	std::function<void()> onFailed;
	// The current configuration is built at setup. The others are queued on the program build thread so a settings change finds them ready.
	std::map<std::uint32_t, std::shared_ptr<const ComputeProgram>> programs;
	RenderGraph::ProgramCache<ComputeProgram, std::uint32_t> prebuilt{ "Screen Space GI" };
	bool registered = false;
	// Set by Prepass on frames the chain has its inputs.
	bool frameReady = false;
	bool failed = false;

	/**
	* @brief A main-target-sized texture. Non-aliased ones are versioned, so a resize is a new version.
	* A matching texture from a_previous is kept, so a settings change rebuilds nothing outside the chain's anchor.
	*/
	Texture MakeTexture(DXGI_FORMAT a_format, UINT a_mips, const char* a_name, GraphResources::Lifetime a_lifetime, bool a_exported,
		const GiState* a_previous) const
	{
		if (a_lifetime == GraphResources::Lifetime::Transient && KeptApart(a_name))
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
		GraphResources::Options options{ .lifetime = a_lifetime, .d3d11 = a_exported ? GraphResources::Export::SRV : GraphResources::Export::None };
		options.versioned = a_lifetime != GraphResources::Lifetime::Transient || a_exported;
		if (DebugMode() == Debug::PrivatePool)
			options.aliasPool = kPrivatePool;
		if (a_previous) {
			for (const auto* texture : a_previous->All()) {
				const auto& kept = *texture;
				if (kept && *kept && kept->Name() == a_name && kept->GetLifetime() == a_lifetime && kept->Exported() == a_exported &&
					kept->Desc().Format == a_format && kept->Desc().MipLevels == a_mips && kept->Desc().Width == desc.Width &&
					kept->Desc().Height == desc.Height)
					return kept;
			}
		}
		return std::make_shared<GraphResources::Texture2D>(desc, a_name, options);
	}

	/** @brief Queues every other configuration on the program build thread. */
	void Prebuild()
	{
		for (const auto& config : SettingsConfigs())
			for (std::uint32_t step = 0; step < kSteps; ++step)
				if (UsesStep(config, step) && !programs.contains(ProgramKey(config, step)))
					(void)prebuilt.Get(ProgramKey(config, step), [&] { return StepDesc(config, step); });
	}

	/** @brief Building now is expected only at setup or a shader reload. Otherwise it is logged as a missed prebuild. */
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

	/** @brief Null, logged, when one cannot be made. Matching textures from a_previous are kept. */
	std::shared_ptr<GiState> Build(const Config& a_config, bool a_expected, const GiState* a_previous = nullptr)
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
		gi->workingDepth = MakeTexture(DXGI_FORMAT_R16_FLOAT, kMips, "SSGI::WorkingDepth", Transient, false, a_previous);
		gi->normal = MakeTexture(DXGI_FORMAT_R8G8_UNORM, kMips, "SSGI::Normal", Transient, false, a_previous);
		gi->radiance = MakeTexture(DXGI_FORMAT_R11G11B10_FLOAT, kMips, "SSGI::Radiance", Transient, false, a_previous);
		gi->radianceTemp = MakeTexture(DXGI_FORMAT_R11G11B10_FLOAT, 1, "SSGI::RadianceTemp", Transient, false, a_previous);
		gi->accumScratch = MakeTexture(DXGI_FORMAT_R8_UNORM, 1, "SSGI::AccumFramesScratch", Transient, false, a_previous);
		gi->accum = MakeTexture(DXGI_FORMAT_R8_UNORM, 1, "SSGI::AccumFrames", History, false, a_previous);
		// 8-bit history stalls the temporal AO blend.
		gi->ao = MakeTexture(DXGI_FORMAT_R16_UNORM, 1, "SSGI::AO", History, full, a_previous);
		gi->aoScratch = MakeTexture(DXGI_FORMAT_R16_UNORM, 1, "SSGI::AOScratch", Transient, !full, a_previous);
		gi->y = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::IlY", History, full, a_previous);
		gi->yScratch = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::IlYScratch", Transient, !full, a_previous);
		gi->coCg = MakeTexture(DXGI_FORMAT_R16G16_FLOAT, 1, "SSGI::IlCoCg", History, full, a_previous);
		gi->coCgScratch = MakeTexture(DXGI_FORMAT_R16G16_FLOAT, 1, "SSGI::IlCoCgScratch", Transient, !full, a_previous);
		if (a_config.blur) {
			gi->yBlurInput = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::IlYBlurInput", Transient, false, a_previous);
			gi->coCgBlurInput = MakeTexture(DXGI_FORMAT_R16G16_FLOAT, 1, "SSGI::IlCoCgBlurInput", Transient, false, a_previous);
		}
		if (a_config.SpecularTextures()) {
			gi->spec = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::GiSpecular", History, full, a_previous);
			gi->specScratch = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::GiSpecularScratch", Transient, !full, a_previous);
		}
		gi->prevGeo = MakeTexture(DXGI_FORMAT_R11G11B10_FLOAT, 1, "SSGI::PrevGeo", History, false, a_previous);
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

	/** @brief Registered with each build of the anchor's graph. */
	static std::function<void(org::RenderGraph&)> Registrar(std::shared_ptr<const GiState> a_state)
	{
		return [a_state](org::RenderGraph& a_graph) {
			for (const auto* texture : a_state->All())
				if (*texture)
					(*texture)->Register(a_graph);
		};
	}

	/** @brief Each step behind its gate entry, then the accumulation copy. Registering again replaces it. */
	bool RegisterChain()
	{
		AnchorPasses::Builder builder(FrameAnchors::Anchor::DeferredPasses, kClientId, kOrderFirstStep, latch, gate);
		// Debug clears run every run, not once.
		const auto clearEachRun = [&](const char* a_name, const Texture& a_texture) {
			if (a_texture)
				builder.Add(AnchorPasses::ClearOf(a_name, *a_texture), false);
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
		// Without the blur, the next frame reads counts the disocclusion pass cannot write in place.
		if (!state->config.blur)
			builder.Add(AnchorPasses::CopyOf("keep-accumulation", *state->accumScratch, *state->accum, state->accum->Desc().Width, state->accum->Desc().Height));
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
		registered = FrameAnchors::Register(std::move(client));
		return registered;
	}

	/**
	* @brief One-shot history clears, run at the anchor's next run after this registration or after FrameAnchors::Rearm.
	* The client is never active itself, so it keeps no epoch running.
	*/
	bool RegisterReset()
	{
		AnchorPasses::Builder builder(FrameAnchors::Anchor::DeferredPasses, kResetClientId, kOrderReset);
		for (std::size_t i = 0; i < kHistoryTextures; ++i)
			if (const auto& texture = *state->History()[i])
				builder.Add(AnchorPasses::ClearOf(std::format("{}", i), *texture));
		FrameAnchors::ClientDesc client{};
		client.id = kResetClientId;
		client.passes = builder.Take();
		client.registerResources = Registrar(state);
		client.active = [] { return false; };
		return FrameAnchors::Register(std::move(client));
	}

	void Commit() const
	{
		if (!state || !state->config.enabled)
			return;
		auto& feature = globals::features::screenSpaceGI;
		const auto constants = feature.BuildConstants({ static_cast<float>(state->ao->Desc().Width), static_cast<float>(state->ao->Desc().Height) });
		latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, constants);
		if (DebugMode() == Debug::NoChain)
			return;
		// Use the built state's resolution mode, which the programs were made for.
		auto full = feature.GetDispatchSizes().full;
		// Between a resolution change and the next frame boundary the textures still have their old size.
		full[0] = (std::min)(full[0], static_cast<uint>(state->ao->Desc().Width));
		full[1] = (std::min)(full[1], static_cast<uint>(state->ao->Desc().Height));
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

	/** @brief Null where the state has none. */
	std::array<std::pair<std::string_view, const GraphResources::Texture2D*>, 4> CompositeInputs() const
	{
		return { { { ORGScreenSpaceGI::kAoId, state->OutAo().get() }, { ORGScreenSpaceGI::kYId, state->OutY().get() },
			{ ORGScreenSpaceGI::kCoCgId, state->OutCoCg().get() }, { ORGScreenSpaceGI::kSpecularId, state->OutSpec().get() } } };
	}

	/** @brief Orders the composite's reads after the chain. An id with no texture is withdrawn. A new texture under an id rebuilds every graph once. */
	bool PublishOutputs() const
	{
		for (const auto& [id, texture] : CompositeInputs()) {
			if (!texture)
				PublishedResources::Withdraw(id);
			else if (!PublishedResources::Publish(id, *texture))
				return false;
		}
		return true;
	}

	/** @brief The composite returns to the D3D11 views. */
	static void WithdrawOutputs()
	{
		for (const auto id : { ORGScreenSpaceGI::kAoId, ORGScreenSpaceGI::kYId, ORGScreenSpaceGI::kCoCgId, ORGScreenSpaceGI::kSpecularId })
			PublishedResources::Withdraw(id);
	}

	void Unregister()
	{
		registered = false;
		WithdrawOutputs();
		FrameAnchors::Unregister(kClientId);
		FrameAnchors::Unregister(kResetClientId);
	}

	/** @brief For good. The D3D11 path takes over. */
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
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
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
	// The clients' callbacks point at the Impl, so they go with it.
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
		// Prebuild off the render thread so a settings change does not compile.
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

	// A setting the programs or textures depend on, or a menu recompile, remakes both and the history.
	bool newState = false;
	if (feature.recompileFlag) {
		// Also reload the D3D11 shaders so a later fallback finds them current. This queues the history reset too.
		feature.ClearShaderCache();
		self.programs.clear();
		self.prebuilt.Clear();
		newState = true;
	}
	const auto config = Config::Current();
	if (newState || config != self.state->config) {
		const bool reload = newState;
		auto state = self.Build(config, reload, self.state.get());
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

	// A resolution change. Textures take the new size at the next frame boundary, then the chain re-registers and clears its history.
	if (D3D11_TEXTURE2D_DESC main{}; MainDesc(main))
		for (const auto* texture : self.state->All())
			if (*texture)
				(*texture)->RequestSize(main.Width, main.Height);
	bool resized = false;
	if (const auto& desc = self.state->ao->Desc(); desc.Width != self.main.Width || desc.Height != self.main.Height) {
		self.main.Width = desc.Width;
		self.main.Height = desc.Height;
		logger::info("[ORG] Screen Space GI: its textures are now {}x{}", desc.Width, desc.Height);
		resized = true;
	}

	// A new engine texture or noise texture remakes the chain's passes.
	bool newInputs = resized;
	if (auto inputs = CurrentInputs(); !(inputs == self.state->inputs)) {
		if (!inputs)
			return fail("an input could not be imported");
		self.state->inputs = std::move(inputs);
		newInputs = true;
	}

	const bool reset = feature.queuedResetHistory.exchange(false);
	if (!self.registered || newState) {
		// Registered at the first frame, with the inputs imported on the render thread. Re-registering the clears runs them once.
		if (!self.RegisterReset() || !self.RegisterChain())
			return fail("its anchor clients could not be registered");
		if (!self.PublishOutputs())
			return fail("the composite's inputs could not be published");
	} else {
		if (newInputs && !self.RegisterChain())
			return fail("its anchor client could not be registered");
		// A resize leaves the new versions' history undefined, so clear it.
		if (reset || resized)
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

// Without OpenRenderGraph Screen Space GI always draws with D3D11.
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
