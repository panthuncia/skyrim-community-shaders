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
#	include "RenderGraph/Frame/Declare.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/Switches.h"
#	include "RenderGraph/UavClearPass.h"
#	include "Utils/Game.h"
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#		include "RenderGraph/ProgramBuild.h"
#	endif

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/OwnedDescriptorBinding.h>
#	include <Resources/ExternalTextureResource.h>

#	include <algorithm>
#	include <array>
#	include <format>
#	include <functional>
#	include <map>
#	include <optional>
#	include <string>
#	include <string_view>
#	include <utility>
#	include <vector>

namespace
{
	namespace AnchorPasses = RenderGraph::AnchorPasses;
	namespace Frame = RenderGraph::Frame;
	namespace Vars = RenderGraph::Frame::Vars;
	using AnchorPasses::Bind;

	constexpr const char* kClientId = "cs.ssgi";
	constexpr const char* kNoiseName = "ssgi.noise";
	constexpr std::uint32_t kMips = 5;
	// Below Subsurface Scattering's keys (199-202), since the D3D11 frame runs SSGI first.
	constexpr std::int32_t kOrderFirst = 100;

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

	// After the steps' entries. The history clears run on runs whose commit opened it.
	constexpr std::uint32_t kClearEntry = kSteps;

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

	/** @brief One revision's shape settings, with the debug overrides. */
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

		/** @brief From a revision's values (or the newest written ones, with Inputs(nullptr)). */
		static Config For(const Frame::Inputs& a_in)
		{
			const auto& settings = globals::features::screenSpaceGI.settings;
			Config config{ std::clamp(a_in.Get(settings.ResolutionMode), 0, 2), a_in.Get(settings.Enabled), a_in.Get(settings.EnableGI),
				a_in.Get(settings.EnableExperimentalSpecularGI), a_in.Get(settings.EnableTemporalDenoiser), a_in.Get(settings.EnableBlur) };
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

	/** @brief What the programs' defines come from. Enabled and blur add none. */
	struct ProgramConfig
	{
		int resolutionMode = 0;
		bool temporal = false;
		bool gi = false;
		bool specular = false;

		bool operator==(const ProgramConfig&) const = default;

		static ProgramConfig Of(const Config& a_config) { return { a_config.resolutionMode, a_config.temporal, a_config.gi, a_config.specular }; }
	};

	/** @brief As CompileComputeShaders gives them. The common defines are added at setup (ResolveDefines). */
	std::vector<std::pair<std::string, std::string>> StepDefines(Step a_step, const ProgramConfig& a_config)
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

	std::uint32_t ProgramKey(const ProgramConfig& a_config, std::uint32_t a_step)
	{
		return a_step | (static_cast<std::uint32_t>(a_config.resolutionMode) << 4) | (static_cast<std::uint32_t>(a_config.temporal) << 6) |
		       (static_cast<std::uint32_t>(a_config.gi) << 7) | (static_cast<std::uint32_t>(a_config.specular) << 8);
	}

	/** @brief The upsample only below full resolution. The blur is built always, since blur adds no define. */
	bool BuildsStep(const ProgramConfig& a_config, std::uint32_t a_step)
	{
		return !(a_step == kUpsample && a_config.resolutionMode == 0);
	}

	bool UsesStep(const Config& a_config, std::uint32_t a_step)
	{
		return !(a_step == kBlur && !a_config.blur) && !(a_step == kUpsample && a_config.FullRes());
	}

	using Defines = std::vector<std::pair<std::wstring, std::wstring>>;
	using DefineTable = std::map<std::uint32_t, Defines>;

	/**
	* @brief Render thread, at setup. Every step's defines for every resolution mode and define option, with the common
	* defines. Those read State, so they are resolved here and not on the programs lane.
	*/
	std::shared_ptr<const DefineTable> ResolveDefines()
	{
		auto table = std::make_shared<DefineTable>();
		for (int mode = 0; mode <= 2; ++mode) {
			for (std::uint32_t bits = 0; bits < 8; ++bits) {
				const ProgramConfig config{ mode, (bits & 4) != 0, (bits & 1) != 0, (bits & 2) != 0 };
				for (std::uint32_t step = 0; step < kSteps; ++step) {
					auto own = AnchorPasses::Defines(StepDefines(static_cast<Step>(step), config));
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
					(*table)[ProgramKey(config, step)] = RenderGraph::ProgramBuild::WithCommonDefines(own, L"cs_6_6");
#	else
					(*table)[ProgramKey(config, step)] = std::move(own);
#	endif
				}
			}
		}
		return table;
	}

	/** @brief The chain's programs for one define set. Null where a step is not built. */
	struct ChainPrograms
	{
		ProgramConfig config;
		std::array<std::shared_ptr<const ComputeProgram>, kSteps> steps;
	};

	/** @brief Any thread. Null, logged, on failure. */
	std::shared_ptr<const ChainPrograms> LoadPrograms(rhi::Device a_device, const DefineTable& a_defines, const ProgramConfig& a_config)
	{
		auto programs = std::make_shared<ChainPrograms>();
		programs->config = a_config;
		for (std::uint32_t step = 0; step < kSteps; ++step) {
			if (!BuildsStep(a_config, step))
				continue;
			const auto found = a_defines.find(ProgramKey(a_config, step));
			if (found == a_defines.end()) {
				logger::error("[ORG] Screen Space GI: no defines for {} (resolution mode {})", kStepInfo[step].source, a_config.resolutionMode);
				return nullptr;
			}
			const ComputeProgram::Desc desc{ .source = kStepInfo[step].source,
				.constantWords = kFixedWords + kStepInfo[step].views,
				.defines = found->second,
				.commonDefines = false };
			programs->steps[step] = ComputeProgram::Load(a_device, desc);
			if (!programs->steps[step]) {
				logger::error("[ORG] Screen Space GI: {} could not be built", kStepInfo[step].source);
				return nullptr;
			}
		}
		return programs;
	}

	/** @brief What the last Prepass found that is not a variable yet. The client's key reads it. */
	struct EngineInputs
	{
		FrameAnchors::Texture depth, main, normalRoughness, motionVectors, noise;

		bool operator==(const EngineInputs&) const = default;
		explicit operator bool() const { return depth && main && normalRoughness && motionVectors && noise; }
	};

	EngineInputs ObserveInputs()
	{
		auto& feature = globals::features::screenSpaceGI;
		EngineInputs inputs;
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

	/** @brief The textures' extent: the display size (decision 6a), or kMAIN's at setup before the display size is known. */
	Vars::Extent2D SizeFor(const Frame::Inputs& a_in, const Vars::Extent2D& a_setup)
	{
		const auto display = a_in.Get(Vars::DisplaySize());
		return { display.width ? display.width : a_setup.width, display.height ? display.height : a_setup.height };
	}

	D3D11_TEXTURE2D_DESC TextureDesc(DXGI_FORMAT a_format, UINT a_mips, const Vars::Extent2D& a_size)
	{
		return { .Width = a_size.width,
			.Height = a_size.height,
			.MipLevels = a_mips,
			.ArraySize = 1,
			.Format = a_format,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0 };
	}

	/** @brief The composite's inputs, and the order of their published ids. */
	enum Output : std::uint32_t
	{
		kAo,
		kY,
		kCoCg,
		kSpecular,
		kOutputs
	};

	struct OutputInfo
	{
		const char* name;         // the published, exported texture
		const char* partnerName;  // its partner, not exported
		DXGI_FORMAT format;
		std::string_view id;
	};

	constexpr std::array<OutputInfo, kOutputs> kOutputInfo{ {
		// 8-bit history stalls the temporal AO blend.
		{ "SSGI::AO", "SSGI::AOPartner", DXGI_FORMAT_R16_UNORM, ORGScreenSpaceGI::kAoId },
		{ "SSGI::IlY", "SSGI::IlYPartner", DXGI_FORMAT_R16G16B16A16_FLOAT, ORGScreenSpaceGI::kYId },
		{ "SSGI::IlCoCg", "SSGI::IlCoCgPartner", DXGI_FORMAT_R16G16_FLOAT, ORGScreenSpaceGI::kCoCgId },
		{ "SSGI::GiSpecular", "SSGI::GiSpecularPartner", DXGI_FORMAT_R16G16B16A16_FLOAT, ORGScreenSpaceGI::kSpecularId },
	} };

	/**
	* @brief Declared at setup and kept. The textures the next frame reads are declared on the display size and the settings
	* that make them exist; each is one texel while that is off (the existence rule), so no slot ever goes away.
	* Each composite input has one published slot. At full resolution it is also the history and its partner the scratch the
	* chain reprojects into; below full resolution the partner is the history and the upsample writes the output. So the
	* resolution mode switches roles in the client, never textures.
	*/
	struct Fixed
	{
		Frame::DeclaredTexture2D accum, prevGeo;
		std::array<Frame::DeclaredTexture2D, kOutputs> outputs, partners;
		std::shared_ptr<org::LatchBlock> latch;  // SSGICB, written by the commit each run
		std::shared_ptr<RenderGraph::RunGate> gate;  // one entry per step, then the history clears
		org::OwnedDescriptorBinding pointSampler, linearSampler;

		std::vector<const Frame::DeclaredTexture2D*> All() const
		{
			std::vector<const Frame::DeclaredTexture2D*> all{ &accum, &prevGeo };
			for (std::uint32_t i = 0; i < kOutputs; ++i) {
				all.push_back(&outputs[i]);
				all.push_back(&partners[i]);
			}
			return all;
		}
	};

	/** @brief What the client is made for. While SSGI is off (or before its inputs are found) only the history clears. */
	struct Shape
	{
		Config config;  // default (off) while off
		Vars::Extent2D size{};
		std::shared_ptr<const ChainPrograms> programs;  // null while off
		EngineInputs inputs;

		bool operator==(const Shape&) const = default;
	};

	/** @brief What a history reset follows: a new configuration or size clears the history in the first run of its client. */
	struct ResetKey
	{
		Config config;
		Vars::Extent2D size{};

		bool operator==(const ResetKey&) const = default;
	};

	using Texture = std::shared_ptr<GraphResources::Texture2D>;

	/** @brief One registration's textures: transients made at its size, and the roles of the declared ones. Null where its permutation has none. */
	struct GiState
	{
		Shape shape;
		std::shared_ptr<const Fixed> fixed;

		// Aliasable, made for this registration.
		Texture workingDepth, normal, radiance, radianceTemp, accumScratch, yBlurInput, coCgBlurInput;
		// Read by the next frame's disocclusion pass.
		const GraphResources::Texture2D *accum = nullptr, *prevGeo = nullptr, *ao = nullptr, *y = nullptr, *coCg = nullptr, *spec = nullptr;
		// Written by the disocclusion pass and, below full resolution, by the upsample.
		const GraphResources::Texture2D *aoScratch = nullptr, *yScratch = nullptr, *coCgScratch = nullptr, *specScratch = nullptr;

		std::vector<const Texture*> Transients() const
		{
			return { &workingDepth, &normal, &radiance, &radianceTemp, &accumScratch, &yBlurInput, &coCgBlurInput };
		}

		/** @brief The published outputs the composite reads. */
		const GraphResources::Texture2D* Out(Output a_output) const { return fixed->outputs[a_output].Get(); }
	};

	/** @brief Handle 0 for an input the permutation does not touch. */
	Bind Srv(const FrameAnchors::Texture& a_texture) { return a_texture ? Bind::Srv(a_texture) : Bind::Unused(); }

	/** @brief Uses the slot when versioned, so a new size is a new version. Handle 0 for a texture the permutation lacks. */
	Bind Srv(const GraphResources::Texture2D* a_texture) { return a_texture ? Bind::Srv(*a_texture) : Bind::Unused(); }
	Bind Uav(const GraphResources::Texture2D* a_texture) { return a_texture ? Bind::Uav(*a_texture) : Bind::Unused(); }

	std::vector<Bind> UavMips(const GraphResources::Texture2D* a_texture)
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
		const auto& in = s.shape.inputs;
		// With the blur, GI writes scratch that the blur reads, and the blur writes the history.
		const bool blur = s.shape.config.blur;
		const auto* yBlurInput = s.yBlurInput.get();
		const auto* coCgBlurInput = s.coCgBlurInput.get();
		std::vector<Bind> binds;
		const auto add = [&](std::vector<Bind> a_more) { binds.insert(binds.end(), a_more.begin(), a_more.end()); };
		switch (a_step) {
		case kPrefilterDepths:
			add({ Srv(in.depth) });
			add(UavMips(s.workingDepth.get()));
			break;
		case kRadianceDisocc:
			add({ Srv(in.main), Srv(s.workingDepth.get()), Srv(in.normalRoughness), Srv(s.prevGeo), Srv(in.motionVectors),
				Srv(s.accum), Srv(s.ao), Srv(s.y), Srv(s.coCg), Srv(s.spec),
				Uav(s.radianceTemp.get()), Uav(s.accumScratch.get()), Uav(s.aoScratch), Uav(s.yScratch),
				Uav(s.coCgScratch), Uav(s.specScratch) });
			break;
		case kPrefilterRadiance:
			add({ Srv(s.radianceTemp.get()) });
			add(UavMips(s.radiance.get()));
			break;
		case kPrefilterNormal:
			add({ Srv(in.normalRoughness) });
			add(UavMips(s.normal.get()));
			break;
		case kGi:
			add({ Srv(s.workingDepth.get()), Srv(in.normalRoughness), Srv(s.radiance.get()), Srv(in.noise), Srv(s.accumScratch.get()),
				Srv(s.yScratch), Srv(s.coCgScratch), Srv(s.specScratch), Srv(s.normal.get()), Srv(s.aoScratch),
				Uav(s.ao), Uav(blur ? yBlurInput : s.y), Uav(blur ? coCgBlurInput : s.coCg), Uav(s.spec),
				Uav(s.prevGeo) });
			break;
		case kBlur:
			add({ Srv(s.workingDepth.get()), Srv(in.normalRoughness), Srv(s.accumScratch.get()), Srv(yBlurInput),
				Srv(coCgBlurInput), Uav(s.accum), Uav(s.y), Uav(s.coCg) });
			break;
		case kUpsample:
			add({ Srv(s.workingDepth.get()), Srv(s.ao), Srv(s.y), Srv(s.coCg), Srv(s.spec),
				Uav(s.aoScratch), Uav(s.yScratch), Uav(s.coCgScratch), Uav(s.specScratch) });
			break;
		default:
			break;
		}
		return binds;
	}

	/** @brief A display-size transient, aliasable unless a debug mode keeps it apart. Made at each registration. */
	Texture MakeTransient(DXGI_FORMAT a_format, UINT a_mips, const char* a_name, const Vars::Extent2D& a_size)
	{
		GraphResources::Options options{ .lifetime = KeptApart(a_name) ? GraphResources::Lifetime::History : GraphResources::Lifetime::Transient };
		if (DebugMode() == Debug::PrivatePool && options.lifetime == GraphResources::Lifetime::Transient)
			options.aliasPool = kPrivatePool;
		return std::make_shared<GraphResources::Texture2D>(TextureDesc(a_format, a_mips, a_size), a_name, options);
	}

	/** @brief Registered with each build of the anchor's graph: the declared textures, then this registration's transients. */
	std::function<void(org::RenderGraph&)> Registrar(std::shared_ptr<const GiState> a_state)
	{
		return [a_state](org::RenderGraph& a_graph) {
			for (const auto* texture : a_state->fixed->All())
				(*texture)->Register(a_graph);
			for (const auto* texture : a_state->Transients())
				if (*texture)
					(*texture)->Register(a_graph);
		};
	}

	[[noreturn]] void Fail(std::string_view a_why)
	{
		const auto message = fmt::format("Community Shaders: Screen Space GI on the render graph failed: {}", a_why);
		logger::critical("[ORG] {}", message);
		spdlog::default_logger()->flush();
		stl::report_and_fail(message);
	}
}

struct ORGScreenSpaceGI::Impl
{
	std::shared_ptr<const Fixed> fixed;
	// Declared at setup and kept. New define settings are programs built off the render thread, a new shape a new client.
	Frame::DeclaredProgram<ChainPrograms> programs;
	Frame::DeclaredClient client;
	Vars::Extent2D setupSize{};
	EngineInputs observed;
	// The configuration and size the history was last cleared for. Render thread, in the commits.
	std::optional<ResetKey> lastReset;
	// Set by the first Prepass.
	bool frameReady = false;

	/** @brief The client's key for a revision's values. */
	std::optional<Shape> KeyFor(const Frame::Inputs& a_in) const
	{
		Shape shape;
		const auto config = Config::For(a_in);
		// Off, or no inputs yet: the client only clears, so the outputs read zero.
		if (!config.enabled || !observed)
			return shape;
		shape.config = config;
		shape.size = SizeFor(a_in, setupSize);
		shape.programs = programs.For(a_in);
		shape.inputs = observed;
		return shape;
	}

	/** @brief The registration for a new shape, when a revision starts. */
	bool MakeClient(const Shape& a_shape, FrameAnchors::ClientDesc& a_client)
	{
		auto state = std::make_shared<GiState>();
		state->shape = a_shape;
		state->fixed = fixed;
		const auto& config = a_shape.config;
		const bool on = config.enabled;

		if (on) {
			// At full resolution the published outputs are the history; below it the upsample writes them.
			const auto& history = config.FullRes() ? fixed->outputs : fixed->partners;
			const auto& scratch = config.FullRes() ? fixed->partners : fixed->outputs;
			state->accum = fixed->accum.Get();
			state->prevGeo = fixed->prevGeo.Get();
			state->ao = history[kAo].Get();
			state->y = history[kY].Get();
			state->coCg = history[kCoCg].Get();
			state->aoScratch = scratch[kAo].Get();
			state->yScratch = scratch[kY].Get();
			state->coCgScratch = scratch[kCoCg].Get();
			if (config.SpecularTextures()) {
				state->spec = history[kSpecular].Get();
				state->specScratch = scratch[kSpecular].Get();
			}
			const auto& size = a_shape.size;
			state->workingDepth = MakeTransient(DXGI_FORMAT_R16_FLOAT, kMips, "SSGI::WorkingDepth", size);
			state->normal = MakeTransient(DXGI_FORMAT_R8G8_UNORM, kMips, "SSGI::Normal", size);
			state->radiance = MakeTransient(DXGI_FORMAT_R11G11B10_FLOAT, kMips, "SSGI::Radiance", size);
			state->radianceTemp = MakeTransient(DXGI_FORMAT_R11G11B10_FLOAT, 1, "SSGI::RadianceTemp", size);
			state->accumScratch = MakeTransient(DXGI_FORMAT_R8_UNORM, 1, "SSGI::AccumFramesScratch", size);
			if (config.blur) {
				state->yBlurInput = MakeTransient(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::IlYBlurInput", size);
				state->coCgBlurInput = MakeTransient(DXGI_FORMAT_R16G16_FLOAT, 1, "SSGI::IlCoCgBlurInput", size);
			}
			for (const auto* texture : state->Transients()) {
				if (*texture && !**texture) {
					logger::error("[ORG] Screen Space GI: {} could not be made", (*texture)->Name());
					return false;
				}
			}
		}

		AnchorPasses::Builder builder(FrameAnchors::Anchor::DeferredPasses, kClientId, kOrderFirst, fixed->latch, fixed->gate);
		// The history reset: every declared texture cleared on runs whose commit opened the entry.
		{
			std::uint32_t index = 0;
			for (const auto* texture : fixed->All()) {
				const GraphResources::Texture2D* twin = texture->Get();
				builder.Add(
					std::format("reset-{}", index++), FrameAnchors::PassKind::Compute,
					[shared = fixed, twin] { return GraphPasses::MakeUavClear(*twin, { 0.0f, 0.0f, 0.0f, 0.0f }, { shared->gate, kClearEntry }); },
					true);
			}
		}
		if (on) {
			// Debug clears run every run.
			const auto clearEachRun = [&](const char* a_name, const GraphResources::Texture2D* a_texture) {
				if (a_texture)
					builder.Add(AnchorPasses::ClearOf(a_name, *a_texture));
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
				if (!UsesStep(config, step))
					continue;
				const auto& program = a_shape.programs ? a_shape.programs->steps[step] : nullptr;
				if (!program) {
					logger::error("[ORG] Screen Space GI: {} has no program for its revision", kStepInfo[step].source);
					return false;
				}
				std::vector<Bind> binds{ Bind::Shared(), Bind::Latch(0), Bind::Handle(fixed->pointSampler.Index()), Bind::Handle(fixed->linearSampler.Index()) };
				for (auto& bind : StepBinds(*state, static_cast<Step>(step)))
					binds.push_back(std::move(bind));
				builder.Add(AnchorPasses::Dispatch{ .name = kStepInfo[step].name, .program = program, .bindings = std::move(binds), .gate = step });
			}
			// Without the blur, the next frame reads counts the disocclusion pass cannot write in place. The size is the revision's.
			if (!config.blur)
				builder.Add(AnchorPasses::CopyOf("keep-accumulation", *state->accumScratch, *state->accum, a_shape.size.width, a_shape.size.height));
			if (DebugMode() == Debug::NoChain) {
				clearEachRun("debug-clear-out-ao", state->Out(kAo));
				clearEachRun("debug-clear-out-y", state->Out(kY));
				clearEachRun("debug-clear-out-cocg", state->Out(kCoCg));
				clearEachRun("debug-clear-out-spec", state->spec ? state->Out(kSpecular) : nullptr);
			}
			if (DebugMode() == Debug::ClearCoCg)
				clearEachRun("debug-clear-out-cocg", state->Out(kCoCg));
		}
		if (builder.Failed()) {
			logger::error("[ORG] Screen Space GI: {}", builder.Failure());
			return false;
		}

		a_client.id = kClientId;
		a_client.passes = builder.Take();
		a_client.sharedConstants = builder.ReadsSharedConstants();
		a_client.gates = { fixed->gate };
		a_client.registerResources = Registrar(state);
		a_client.commit = [this, state](const FrameAnchors::Point&) { Commit(*state); };
		a_client.active = [this] { return frameReady; };
		if (on)
			logger::info("[ORG] Screen Space GI: a client for the next revision ({}x{}, resolution mode {}, GI {}, specular {}, temporal {}, blur {})",
				a_shape.size.width, a_shape.size.height, config.resolutionMode, config.gi, config.specular, config.temporal, config.blur);
		else
			logger::info("[ORG] Screen Space GI: a client for the next revision (off{}, history clears only)", observed ? "" : ", no inputs yet");
		return true;
	}

	/** @brief Each run: the history reset, then this frame's constants and group counts. */
	void Commit(const GiState& a_state)
	{
		const auto& adopted = Frame::Revision::Adopted();
		auto& feature = globals::features::screenSpaceGI;
		// A loading screen or a shader reload asked for it, a new version is bound, or the client is new for its configuration or size.
		bool clear = feature.queuedResetHistory.exchange(false);
		for (const auto* texture : fixed->All())
			clear |= adopted.FirstFrame(*texture);
		const ResetKey key{ a_state.shape.config, a_state.shape.size };
		if (!lastReset || !(*lastReset == key)) {
			lastReset = key;
			clear = true;
		}
		if (clear)
			fixed->gate->OpenDispatch(kClearEntry, 1);
		if (!a_state.shape.config.enabled)
			return;

		// The bound version's extent is the adopted display size, which this registration was made for.
		const auto extent = adopted.Extent(fixed->accum);
		const float2 texDim{ static_cast<float>(extent[0]), static_cast<float>(extent[1]) };
		const auto constants = feature.BuildConstants(texDim);
		fixed->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, constants);
		if (DebugMode() == Debug::NoChain)
			return;
		// The dynamic render size, never past the bound textures.
		const auto dynamic = Util::ConvertToDynamic(texDim);
		const std::array<uint, 2> full{ (std::min)(static_cast<uint>(dynamic.x), static_cast<uint>(extent[0])),
			(std::min)(static_cast<uint>(dynamic.y), static_cast<uint>(extent[1])) };
		// The registration's resolution mode, which its programs were made for.
		const auto shift = static_cast<uint>(a_state.shape.config.resolutionMode);
		const std::array<uint, 2> internal{ full[0] >> shift, full[1] >> shift };
		const auto open = [&](Step a_step, const std::array<uint, 2>& a_size, uint a_tile) {
			if (UsesStep(a_state.shape.config, a_step))
				fixed->gate->OpenDispatch(a_step, (a_size[0] + a_tile - 1) / a_tile, (a_size[1] + a_tile - 1) / a_tile);
		};
		open(kPrefilterDepths, full, 16);
		open(kRadianceDisocc, internal, 8);
		open(kPrefilterRadiance, internal, 16);
		open(kPrefilterNormal, internal, 16);
		open(kGi, internal, 8);
		open(kBlur, internal, 8);
		open(kUpsample, full, 8);
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

void ORGScreenSpaceGI::Stop()
{
	FrameAnchors::Unregister(kClientId);
	for (const auto& info : kOutputInfo)
		PublishedResources::Withdraw(info.id);
	impl.reset();
}

bool ORGScreenSpaceGI::Setup(const D3D11_TEXTURE2D_DESC& a_main)
{
	if (impl)
		Stop();
	if (!Requested())
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::DeferredPasses)) {
		logger::warn("[ORG] Screen Space GI stays on D3D11: the render graph or its DeferredPasses anchor is not available");
		return false;
	}
	try {
		using Frame::DependsOn;
		const auto& s = globals::features::screenSpaceGI.settings;
		const Vars::Extent2D setupSize{ a_main.Width, a_main.Height };
		auto fixed = std::make_shared<Fixed>();
		fixed->pointSampler = ShaderBindings::CreateSampler({ .minFilter = rhi::Filter::Nearest,
			.magFilter = rhi::Filter::Nearest,
			.mipFilter = rhi::MipFilter::Nearest });
		fixed->linearSampler = ShaderBindings::CreateSampler({});
		fixed->latch = std::make_shared<org::LatchBlock>("cs.ssgi.latch", static_cast<std::uint32_t>(sizeof(ScreenSpaceGI::SSGICB)));
		fixed->gate = RenderGraph::RunGate::Create("cs.ssgi.gate", kSteps + 1);

		// History, declared on the display size and the settings that make each exist. A new size is a new version, born
		// cleared, adopted with a frame revision and no graph build. One texel while it does not exist.
		const auto declare = [&](const char* a_name, DXGI_FORMAT a_format, bool a_exported, bool a_specular) {
			auto dependencies = a_specular ? DependsOn(Vars::DisplaySize(), s.Enabled, s.EnableExperimentalSpecularGI, s.EnableTemporalDenoiser) :
			                                 DependsOn(Vars::DisplaySize(), s.Enabled);
			return Frame::DeclareTexture2D(a_name, std::move(dependencies),
				[setupSize, a_format, a_specular](const Frame::Inputs& a_in) {
					const auto config = Config::For(a_in);
					const bool exists = config.enabled && (!a_specular || config.SpecularTextures());
					return TextureDesc(a_format, 1, exists ? SizeFor(a_in, setupSize) : Vars::Extent2D{ 1, 1 });
				},
				GraphResources::Options{ .lifetime = GraphResources::Lifetime::History,
					.d3d11 = a_exported ? GraphResources::Export::SRV : GraphResources::Export::None });
		};
		fixed->accum = declare("SSGI::AccumFrames", DXGI_FORMAT_R8_UNORM, false, false);
		fixed->prevGeo = declare("SSGI::PrevGeo", DXGI_FORMAT_R11G11B10_FLOAT, false, false);
		for (std::uint32_t i = 0; i < kOutputs; ++i) {
			const auto& info = kOutputInfo[i];
			// The composite's inputs are exported to D3D11, so they are never aliased while D3D11 reads them.
			fixed->outputs[i] = declare(info.name, info.format, true, i == kSpecular);
			fixed->partners[i] = declare(info.partnerName, info.format, false, i == kSpecular);
		}
		bool made = fixed->pointSampler && fixed->linearSampler && fixed->latch && fixed->gate;
		for (const auto* texture : fixed->All())
			made = made && *texture && **texture && (*texture)->Versioned();
		for (const auto& output : fixed->outputs)
			made = made && output && output->SRV();
		if (!made) {
			logger::warn("[ORG] Screen Space GI stays on D3D11: its samplers, gate or textures could not be made");
			return false;
		}

		auto next = std::make_unique<Impl>();
		next->fixed = fixed;
		next->setupSize = setupSize;
		// Found again at each Prepass. Missing inputs here only delay the chain to the revision after the first Prepass.
		next->observed = ObserveInputs();
		// The define settings choose the programs. The first is built here, the others on the programs lane, before the
		// revision that needs them starts.
		next->programs = Frame::DeclareProgram("ScreenSpaceGI::Chain",
			DependsOn(s.ResolutionMode, s.EnableTemporalDenoiser, s.EnableGI, s.EnableExperimentalSpecularGI),
			[](const Frame::Inputs& a_in) { return ProgramConfig::Of(Config::For(a_in)); },
			[device = host->Device(), defines = ResolveDefines()](const ProgramConfig& a_config) { return LoadPrograms(device, *defines, a_config); });
		if (!next->programs) {
			logger::warn("[ORG] Screen Space GI stays on D3D11: its programs could not be built");
			return false;
		}
		auto* self = next.get();
		next->client = Frame::DeclareClient<Shape>(kClientId,
			DependsOn(Vars::DisplaySize(), s.Enabled, s.EnableGI, s.EnableExperimentalSpecularGI, s.ResolutionMode, s.EnableTemporalDenoiser, s.EnableBlur),
			[self](const Frame::Inputs& a_in) { return self->KeyFor(a_in); },
			[self](const Shape& a_shape, FrameAnchors::ClientDesc& a_client) { return self->MakeClient(a_shape, a_client); });
		if (!next->client) {
			logger::warn("[ORG] Screen Space GI stays on D3D11: its client could not be declared");
			return false;
		}
		// The composite reads them by id, ordered after the chain. Published once: readers follow their versions.
		for (std::uint32_t i = 0; i < kOutputs; ++i) {
			if (!PublishedResources::Publish(kOutputInfo[i].id, *fixed->outputs[i])) {
				logger::warn("[ORG] Screen Space GI stays on D3D11: the composite's inputs could not be published");
				FrameAnchors::Unregister(kClientId);
				for (const auto& info : kOutputInfo)
					PublishedResources::Withdraw(info.id);
				return false;
			}
		}
		impl = std::move(next);
		const auto config = Config::For(Frame::Inputs(nullptr));
		logger::info("[ORG] Screen Space GI runs on the render graph ({}x{}, {}, resolution mode {}, blur {}); its textures, programs and client "
					 "follow the frame revisions",
			a_main.Width, a_main.Height, config.enabled ? "on" : "off", config.resolutionMode, config.blur);
		if (const auto mode = DebugMode(); mode != Debug::Off)
			logger::warn("[ORG] Screen Space GI debug: CS_ORG_SSGI_DEBUG={} ({}); temporal {}, GI {}, specular {}", static_cast<std::uint32_t>(mode),
				kDebugNames[static_cast<std::size_t>(mode)], config.temporal, config.gi, config.specular);
		return true;
	} catch (const std::exception& e) {
		logger::error("[ORG] Screen Space GI setup failed, staying on D3D11: {}", e.what());
		Stop();
		return false;
	}
}

void ORGScreenSpaceGI::Prepass()
{
	if (!IsActive())
		return;
	// A new engine texture or noise texture: not variables yet, so the next revision makes the client for them.
	if (auto observed = ObserveInputs(); !(observed == impl->observed)) {
		if (!observed)
			Fail("an input could not be imported");
		impl->observed = std::move(observed);
		impl->client.Invalidate();
	}
	impl->frameReady = true;
}

bool ORGScreenSpaceGI::IsActive() const
{
	return impl && RenderGraphRuntime::Get().IsActive();
}

ORGScreenSpaceGI::Outputs ORGScreenSpaceGI::GetOutputs() const
{
	if (!IsActive() || DebugMode() == Debug::NoOutputs)
		return {};
	const auto& outputs = impl->fixed->outputs;
	return { outputs[kAo]->SRV(), outputs[kY]->SRV(), outputs[kCoCg]->SRV(), outputs[kSpecular]->SRV() };
}

bool ORGScreenSpaceGI::AdoptedSpecularGI() const
{
	return RenderGraph::Frame::Revision::Adopted().Get(globals::features::screenSpaceGI.settings.EnableExperimentalSpecularGI);
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
void ORGScreenSpaceGI::Stop() {}
bool ORGScreenSpaceGI::Setup(const D3D11_TEXTURE2D_DESC&) { return false; }
void ORGScreenSpaceGI::Prepass() {}
bool ORGScreenSpaceGI::IsActive() const { return false; }
ORGScreenSpaceGI::Outputs ORGScreenSpaceGI::GetOutputs() const { return {}; }
bool ORGScreenSpaceGI::AdoptedSpecularGI() const { return false; }

#endif
