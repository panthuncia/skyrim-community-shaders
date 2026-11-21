#include "ORGVolumetricLighting.h"

#include <winrt/base.h>

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Feature.h"
#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainShadows/ORGTerrainShadows.h"
#include "Features/VolumetricLighting.h"
#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"
#include "State.h"
#include "Utils/Game.h"
#include "Utils/Logging.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <Resources/ExternalTextureResource.h>
#include <rhi_helpers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace
{
	using Anchor = FrameAnchors::Anchor;
	using RenderTarget = RE::RENDER_TARGETS::RENDER_TARGET;
	using DepthTarget = RE::RENDER_TARGETS_DEPTHSTENCIL::RENDER_TARGET_DEPTHSTENCIL;
	namespace AnchorPasses = RenderGraph::AnchorPasses;
	namespace Pipeline = RenderGraph::Engine::Pipeline;

	constexpr const char* kClientId = "cs.volumetric-lighting";
	constexpr Anchor kAnchor = Anchor::VolumetricLighting;
	constexpr const char* kLogPrefix = "[ORG] Volumetric Lighting";
	// The size SetComputeShaderConstants uses for the density generation's PerTechnique block.
	constexpr std::size_t kGenerateBytes = 0x180;
	constexpr std::uint32_t kGenerateRows = static_cast<std::uint32_t>(kGenerateBytes / 16);

	// The apply writes one of the first two and reads the other, swapping by frame. The blurs go through the copy and end in the one the apply read.
	constexpr std::uint32_t kVolumetricTarget = RenderTarget::kIMAGESPACE_VOLUMETRIC_LIGHTING;
	constexpr std::uint32_t kPreviousTarget = RenderTarget::kIMAGESPACE_VOLUMETRIC_LIGHTING_PREVIOUS;
	constexpr std::uint32_t kSwapTarget = RenderTarget::kIMAGESPACE_VOLUMETRIC_LIGHTING_COPY;
	/** @brief Output 0 is kIMAGESPACE_VOLUMETRIC_LIGHTING. The apply reads the other one. */
	constexpr std::uint32_t OutputOf(std::uint32_t a_output) { return a_output == 0 ? kVolumetricTarget : kPreviousTarget; }
	constexpr std::uint32_t PreviousOf(std::uint32_t a_output) { return a_output == 0 ? kPreviousTarget : kVolumetricTarget; }

	// Gate entries: noise, generate, raymarch, the apply into either output, the blurs by output, then the depth copy.
	constexpr std::uint32_t kNoiseEntry = 0;
	constexpr std::uint32_t kGenerateEntry = 1;
	constexpr std::uint32_t kRaymarchEntry = 2;
	constexpr std::uint32_t ApplyEntry(std::uint32_t a_output) { return 3 + a_output; }
	constexpr std::uint32_t BlurHEntry(std::uint32_t a_output) { return 5 + a_output; }
	constexpr std::uint32_t BlurVEntry(std::uint32_t a_output) { return 7 + a_output; }
	constexpr std::uint32_t kDepthCopyEntry = 9;
	constexpr std::uint32_t kGateEntries = kDepthCopyEntry + 1;

	using Float4 = std::array<float, 4>;

	// One run's values in the latch, one per shader's LATCH entry.
	struct LatchData
	{
		Float4 noise;                                    // the noise volume's size
		Float4 raymarch;                                 // the volume's size. StepIndex is never read.
		std::array<std::int32_t, 4> blur;
		Float4 applyIntensityTemporal;                   // g_IntensityX_TemporalY
		Float4 applyRepartition;                         // x is the repartition exponent
		std::array<Float4, kGenerateRows + 1> generate;  // the engine's block, then x is the inverse exponent
	};
	static_assert(offsetof(LatchData, applyIntensityTemporal) % 16 == 0 && offsetof(LatchData, generate) % 16 == 0 && sizeof(LatchData) % 16 == 0);

	// Push blocks in each resource list's order.
	using ShaderBindings::Handle;
	struct NoiseBlock
	{
		Handle output;
		ShaderBindings::Latch constants;
	};
	struct GenerateBlock
	{
		SharedConstants::Handles shared;
		Handle shadowmapSampler, shadowmapVLSampler, noiseSampler;
		Handle shadowmap, shadowmapVL, noise;
		Handle density;
		ShaderBindings::Latch constants;
		Handle shadowHeights, cloudShadows;
	};
	struct RaymarchBlock
	{
		Handle density;
		ShaderBindings::Latch constants;
	};
	struct BlurBlock
	{
		SharedConstants::Handles shared;
		Handle input, depth, output;
		ShaderBindings::Latch constants;
	};
	struct ApplyBlock
	{
		SharedConstants::Handles shared;
		Handle depth, previous, volume, noiseGradient, motionVectors, previousDepth;
		Handle depthSampler, previousSampler, volumeSampler, noiseGradientSampler, motionVectorsSampler, previousDepthSampler;
		ShaderBindings::Latch constants;
	};
	static_assert(sizeof(NoiseBlock) == 12 && sizeof(GenerateBlock) == 60 && sizeof(RaymarchBlock) == 12 && sizeof(BlurBlock) == 36 && sizeof(ApplyBlock) == 72);

	/** @brief An engine sampler by its shadow state address and filter modes. */
	struct SamplerModes
	{
		std::uint32_t address = 0, filter = 0;
		auto operator<=>(const SamplerModes&) const = default;
	};
	// From the VL render (SE 100307) and the apply (SE 100475). The noise gradient is wrap point, the rest of the apply clamp linear.
	constexpr SamplerModes kClampPoint{ 0, 0 }, kClampLinear{ 0, 1 }, kNoiseModes{ 5, 1 }, kWrapPoint{ 3, 0 };

	/** @brief In texels, from the VL descriptor's +0x30, +0x34, +0x38. */
	struct VolumeSize
	{
		std::uint32_t width = 0, height = 0, depth = 0;
		auto operator<=>(const VolumeSize&) const = default;
		explicit operator bool() const { return width >= 32 && height >= 32 && depth > 0; }
	};

	/** @brief The density volume's description. The engine's accumulation volume format (AE 0x1414c5fa0). */
	D3D11_TEXTURE3D_DESC VolumeDesc(const VolumeSize& a_size)
	{
		D3D11_TEXTURE3D_DESC desc{};
		desc.Width = (std::max)(a_size.width, 1u);
		desc.Height = (std::max)(a_size.height, 1u);
		desc.Depth = (std::max)(a_size.depth, 1u);
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R16_FLOAT;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		return desc;
	}

	VolumeSize SizeOf(const VolumetricLighting::TextureSize& a_size)
	{
		return { static_cast<std::uint32_t>((std::max)(a_size.Width, 0)), static_cast<std::uint32_t>((std::max)(a_size.Height, 0)),
			static_cast<std::uint32_t>((std::max)(a_size.Depth, 0)) };
	}

	// ---- The engine's data ----

	/** @brief The VL descriptor (SE 100297, AE 107014). +0x00 intensity, +0x2C repartition range factor, +0x30 to +0x38 volume size. */
	const std::byte* Descriptor()
	{
		static REL::Relocation<const std::byte* (*)()> descriptor{ REL::RelocationID(100297, 107014) };
		return descriptor();
	}

	template <class T>
	T DescriptorField(std::size_t a_offset)
	{
		T value{};
		std::memcpy(&value, Descriptor() + a_offset, sizeof(T));
		return value;
	}

	VolumeSize DescriptorSize()
	{
		return { DescriptorField<std::uint32_t>(0x30), DescriptorField<std::uint32_t>(0x34), DescriptorField<std::uint32_t>(0x38) };
	}

	/** @brief SE 100298, AE 107015. The weather's value while it has one, else 0. */
	float TemporalFactor()
	{
		static REL::Relocation<float (*)()> temporal{ REL::RelocationID(100298, 107015) };
		return temporal();
	}

	/** @brief As the VL render's helper wrote it (SE 527948, AE 414886). */
	const std::byte* GenerateConstants()
	{
		return reinterpret_cast<const std::byte*>(REL::RelocationID(527948, 414886).address());
	}

	/** @brief The noise gradient the apply binds at t4 (SE 100475, AE 107193), the same one the sky binds. Null before it loads. */
	ID3D11ShaderResourceView* NoiseGradientView()
	{
		const auto* holder = *reinterpret_cast<const std::byte* const*>(REL::RelocationID(525022, 411504).address());
		if (!holder)
			return nullptr;
		const auto* texture = *reinterpret_cast<const std::byte* const*>(holder + 0x48);
		return texture ? *reinterpret_cast<ID3D11ShaderResourceView* const*>(texture + 0x10) : nullptr;
	}

	/** @brief From the INI settings the VL render's first call uses to create the volume. */
	VolumeSize NoiseSize()
	{
		const auto setting = [](const char* a_name) -> std::uint32_t {
			RE::Setting* found = globals::game::iniSettingCollection ? globals::game::iniSettingCollection->GetSetting(a_name) : nullptr;
			if (!found && globals::game::iniPrefSettingCollection)
				found = globals::game::iniPrefSettingCollection->GetSetting(a_name);
			return found ? static_cast<std::uint32_t>((std::max)(found->GetInteger(), 0)) : 0u;
		};
		return { setting("iVolumetricLightingNoiseTextureWidth:Display"), setting("iVolumetricLightingNoiseTextureHeight:Display"),
			setting("iVolumetricLightingNoiseTextureDepth:Display") };
	}

	/** @brief Off means the client draws the engine's own volumetric lighting. */
	bool FeatureOn()
	{
		return globals::features::volumetricLighting.loaded;
	}

	/** @brief In graph mode, with the feature on or off: the post stage holds the anchor, and the engine's VL has no D3D11 path inside a stage. */
	bool Wanted()
	{
		return ORGVolumetricLighting::On();
	}

	/** @brief The dynamic resolution part of the screen and its last pixel. A per-frame constant, never a declaration. */
	std::array<std::int32_t, 4> BlurArea()
	{
		const float2 size = RenderGraph::Frame::Vars::DynamicArea();
		const auto x = (std::max)(1, static_cast<std::int32_t>(size.x));
		const auto y = (std::max)(1, static_cast<std::int32_t>(size.y));
		return { x, y, x - 1, y - 1 };
	}

	// TG_DIM and WINDOW of ISVolumetricLightingBlurHCS/VCS.
	constexpr std::uint32_t kBlurGroup = 256;
	constexpr std::uint32_t kBlurWindow = 12;

	/** @brief As ShaderCache's GetImagespaceShaderDefines gives the D3D11 build. */
	std::vector<std::pair<std::string, std::string>> FeatureDefines()
	{
		std::vector<std::pair<std::string, std::string>> defines;
		for (auto* feature : Feature::GetFeatureList()) {
			if (!feature->loaded || !feature->HasShaderDefine(RE::BSShader::Type::ImageSpace))
				continue;
			defines.emplace_back(std::string(feature->GetShaderDefineName()), "");
			for (const auto& [name, value] : feature->GetShaderDefineOptions())
				defines.emplace_back(std::string(name), std::string(value));
		}
		return defines;
	}

	// ---- State ----

	/** @brief Made once at Setup and shared by every registration. */
	struct VLFixed
	{
		// Queued at Setup.
		RenderGraph::Programs::Fixed<ComputeProgram> noise, generate, raymarch, blurH, blurV;
		RenderGraph::Programs::Fixed<RasterProgram> apply;
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
		std::unique_ptr<GraphResources::Texture3D> noiseVolume;
		VolumeSize noiseSize;
		rhi::Format outputFormat = rhi::Format::Unknown;
		// The feature is off, so it is the engine's own VL: no feature define, no Terrain or Cloud Shadows input, the engine's volume size.
		bool vanilla = false;
		// Taken at a graph build, because the engine fills its table after CS's Setup.
		std::map<SamplerModes, org::OwnedDescriptorBinding> samplers;
	};

	/**
	* @brief What a registration reads that is not a variable yet: the sky's noise gradient and the published ids of Terrain and
	* Cloud Shadows. The VL render finds them. A change invalidates the client's key, and the next revision registers it again.
	*/
	struct VLInputs
	{
		ID3D11ShaderResourceView* noiseGradient = nullptr;
		std::string shadowHeights, cloudShadows;  // published ids, empty for none
		friend bool operator==(const VLInputs&, const VLInputs&) = default;
	};

	/** @brief One registration of the client. Immutable once made, but for the passes its builds made. */
	struct VLRegistration
	{
		FrameAnchors::Texture targets[3];
		FrameAnchors::Texture motionVectors, depth, previousDepth, shadowmap, shadowmapVL, noiseGradient;
		std::string shadowHeights, cloudShadows;
		// Written at its builds. A published input not declarable yet leaves the generate out.
		mutable std::array<std::atomic<bool>, kGateEntries> passMade{};
	};

	/** @brief Held until the blur's call runs it. */
	struct VLPending
	{
		bool generate = false;
		VolumeSize size;
		std::array<Float4, kGenerateRows + 1> generateConstants{};
		float rangeFactor = 1.0f;
		std::optional<std::uint32_t> output;
		float intensity = 0.0f, temporal = 0.0f;
	};

	struct VLRun
	{
		std::uint32_t output = 0;
		VolumeSize size;
		bool noise = false;
		LatchData data{};
		std::array<std::uint32_t, 2> blurHGroups{}, blurVGroups{};
		// The commit of the build this run executes opened the gates.
		bool opened = false;
	};
}

struct ORGVolumetricLighting::Impl
{
	std::shared_ptr<VLFixed> fixed;
	// The density volume, declared on the size settings and the Interior variable (the engine's size with the feature off).
	RenderGraph::Frame::DeclaredTexture3D volume;
	// The client, declared on the inputs the last VL render found (B2's interim). The engine targets it reads are stable slots.
	RenderGraph::Frame::DeclaredClient client;
	RenderGraph::Frame::Observed<VLInputs> observed{ "VolumetricLighting.Inputs" };
	VLPending pending;
	std::optional<VLRun> run;
	bool inRender = false;
	// The engine's volume size with the feature off, declared at setup.
	VolumeSize vanillaSize{};
	bool noiseDone = false;
	bool announced = false;
	static constexpr std::uint32_t kLateSkipsLogged = 16;
	std::uint32_t lateSkips = 0;
	Util::ReasonLog skipped{ fmt::format("{}: a frame has no volumetric lighting", kLogPrefix) };
	Util::ReasonLog engineDispatches{ fmt::format("{}: the VL render's dispatch is withheld", kLogPrefix) };

	/** @brief Made at a graph build. UINT32_MAX when the engine's table has none. */
	std::uint32_t Sampler(SamplerModes a_modes)
	{
		auto& samplers = fixed->samplers;
		if (const auto found = samplers.find(a_modes); found != samplers.end())
			return found->second ? found->second.Index() : UINT32_MAX;
		auto* sampler = Pipeline::EngineSamplerState(a_modes.address, a_modes.filter);
		if (!sampler)
			return UINT32_MAX;
		D3D11_SAMPLER_DESC desc{};
		sampler->GetDesc(&desc);
		auto binding = ShaderBindings::CreateSamplerFromD3D11(desc);
		const auto index = binding ? binding.Index() : UINT32_MAX;
		samplers.emplace(a_modes, std::move(binding));
		return index;
	}

	/**
	* @brief The feature's published id, when it is on the graph. Empty otherwise, since a D3D11 import is not ordered after
	* its producer and an id nobody publishes would hold the generate back for good.
	*/
	static std::string FeatureInput(bool a_loaded, bool a_orgPath, std::string_view a_id)
	{
		if (!a_loaded)
			return {};
		if (a_orgPath || PublishedResources::IsRegistered(a_id))
			return std::string(a_id);
		return {};
	}

	/** @brief Null, logged, when it cannot be made. */
	std::shared_ptr<org::RenderPass> MakeStep(const VLRegistration& a_registration, std::uint32_t a_entry, std::string_view a_name,
		const std::function<void(AnchorPasses::Builder&)>& a_add)
	{
		AnchorPasses::Builder builder(kAnchor, kClientId, 0, fixed->latch, fixed->gate);
		a_add(builder);
		auto passes = builder.Take();
		std::shared_ptr<org::RenderPass> pass;
		if (builder.Failed() || passes.empty() || !passes.front().make)
			logger::error("{}: the pass {} could not be made ({}); its work is skipped", kLogPrefix, a_name, builder.Failure());
		else
			pass = passes.front().make();
		if (a_entry < kGateEntries)
			a_registration.passMade[a_entry] = pass != nullptr;
		return pass;
	}

	/** @brief False, logged, when the engine's table lacks one at this build. */
	bool Samplers(std::span<const SamplerModes> a_modes, std::span<AnchorPasses::Bind> a_out, std::string_view a_pass)
	{
		for (std::size_t i = 0; i < a_modes.size(); ++i) {
			const auto index = Sampler(a_modes[i]);
			if (index == UINT32_MAX) {
				logger::error("{}: {}: the engine's sampler (address {}, filter {}) has no graph form at this build; the pass is left out", kLogPrefix, a_pass,
					a_modes[i].address, a_modes[i].filter);
				return false;
			}
			a_out[i] = AnchorPasses::Bind::Handle(index);
		}
		return true;
	}

	// ---- Declarations ----

	/** @brief With the feature on: the size the engine is given for the location, and one texel while its VL is off. */
	void DeclareFeatureVolume()
	{
		using namespace RenderGraph::Frame;
		const auto& s = globals::features::volumetricLighting.settings;
		volume = DeclareTexture3D(
			"VolumetricLighting::Volume",
			DependsOn(Vars::Interior(), s.ExteriorEnabled, s.ExteriorQuality, s.ExteriorCustomSize, s.InteriorEnabled, s.InteriorQuality,
				s.InteriorCustomSize),
			[](const Inputs& a_in) {
				const auto& feature = globals::features::volumetricLighting;
				const auto& f = feature.settings;
				const bool interior = a_in.Get(Vars::Interior());
				if (!(interior ? a_in.Get(f.InteriorEnabled) : a_in.Get(f.ExteriorEnabled)))
					return VolumeDesc({ 1, 1, 1 });
				const auto size = interior ? feature.VolumeSizeFor(a_in.Get(f.InteriorQuality), a_in.Get(f.InteriorCustomSize)) :
				                             feature.VolumeSizeFor(a_in.Get(f.ExteriorQuality), a_in.Get(f.ExteriorCustomSize));
				return VolumeDesc(SizeOf(size));
			},
			// Written by the generate and read by the apply in the same run. Not transient, so a new size is a new version.
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
	}

	/** @brief With the feature off: the engine's own size, which nothing changes in a session. Declared once it is known. */
	void DeclareVanillaVolume(const VolumeSize& a_size)
	{
		using namespace RenderGraph::Frame;
		volume = DeclareTexture3D(
			"VolumetricLighting::Volume", Dependencies{}, [a_size](const Inputs&) { return VolumeDesc(a_size); },
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
		logger::info("{}: the engine's volume is {}x{}x{}", kLogPrefix, a_size.width, a_size.height, a_size.depth);
	}

	void DeclareClient()
	{
		client = RenderGraph::Frame::DeclareClient<VLInputs>(
			kClientId, RenderGraph::Frame::DependsOn(observed.Var()),
			[this](const RenderGraph::Frame::Inputs& a_in) -> std::optional<VLInputs> {
				// Until a VL render found the sky's noise gradient, there is no client. The volume is declared before the client.
				const auto* seen = observed.Get(a_in);
				if (!seen || !seen->noiseGradient)
					return std::nullopt;
				return *seen;
			},
			[this](const VLInputs& a_inputs, FrameAnchors::ClientDesc& a_client) { return MakeClient(a_inputs, a_client); });
	}

	/** @brief In each VL render. A change is an event: the next revision registers a new client. */
	void Observe()
	{
		VLInputs seen;
		seen.noiseGradient = NoiseGradientView();
		auto& terrainShadows = globals::features::terrainShadows;
		auto& cloudShadows = globals::features::cloudShadows;
		seen.shadowHeights = FeatureInput(!fixed->vanilla && terrainShadows.loaded, ORGTerrainShadows::On(), ORGTerrainShadows::kShadowHeightId);
		seen.cloudShadows = FeatureInput(!fixed->vanilla && cloudShadows.loaded, ORGCloudShadows::On(), ORGCloudShadows::kOcclusionId);
		observed.Observe(seen);
	}

	/** @brief The registration for a new key, at the start of the revision that adopts it. A new volume size is a version, never this. */
	bool MakeClient(const VLInputs& a_inputs, FrameAnchors::ClientDesc& a_client)
	{
		auto made = std::make_shared<VLRegistration>();
		constexpr std::array kTargets{ kVolumetricTarget, kPreviousTarget, kSwapTarget };
		for (std::size_t i = 0; i < kTargets.size(); ++i)
			made->targets[i] = FrameAnchors::EngineTexture(FrameAnchors::Colour(kTargets[i]));
		made->motionVectors = FrameAnchors::EngineTexture(FrameAnchors::Colour(RenderTarget::kMOTION_VECTOR));
		made->depth = FrameAnchors::EngineTexture(FrameAnchors::Depth(DepthTarget::kMAIN));
		made->previousDepth = FrameAnchors::EngineTexture(FrameAnchors::Depth(DepthTarget::kMAIN_COPY));
		made->shadowmap = FrameAnchors::EngineTexture(FrameAnchors::Depth(DepthTarget::kSHADOWMAPS_ESRAM));
		made->shadowmapVL = FrameAnchors::EngineTexture(FrameAnchors::Depth(DepthTarget::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM));
		if (!made->targets[0] || !made->targets[1] || !made->targets[2] || !made->motionVectors || !made->depth || !made->previousDepth || !made->shadowmap ||
			!made->shadowmapVL) {
			logger::error("{}: an engine target or depth could not be imported", kLogPrefix);
			return false;
		}
		winrt::com_ptr<ID3D11Resource> resource;
		a_inputs.noiseGradient->GetResource(resource.put());
		const auto texture = resource.try_as<ID3D11Texture2D>();
		made->noiseGradient = texture ? FrameAnchors::ImportTexture("cs.d3d11.volumetric-lighting.noise-gradient", { .texture = texture.get(), .srv = a_inputs.noiseGradient, .immutable = true }) : nullptr;
		if (!made->noiseGradient) {
			logger::error("{}: the sky's noise gradient texture could not be imported", kLogPrefix);
			return false;
		}
		made->shadowHeights = a_inputs.shadowHeights;
		made->cloudShadows = a_inputs.cloudShadows;

		// Passes bind the volume's slot, so each frame binds the version its revision adopted.
		auto* twin = volume.Get();
		const std::shared_ptr<const VLRegistration> shared = made;
		a_client.id = kClientId;
		std::int32_t order = 0;
		const auto add = [&](std::string a_name, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make) {
			// Gated so the anchor can run in a frame phase. A copy skips at execution when its entry is closed.
			a_client.passes.push_back({ .name = fmt::format("{}.{}", kClientId, a_name),
				.anchor = kAnchor,
				.order = order++,
				.kind = a_kind,
				.make = std::move(a_make),
				.atExecution = a_kind == FrameAnchors::PassKind::Copy });
		};
		add("noise", FrameAnchors::PassKind::Compute, [this, shared] {
			return MakeStep(*shared, kNoiseEntry, "noise", [&](AnchorPasses::Builder& a_builder) {
				a_builder.Add(AnchorPasses::Dispatch{ .name = "noise",
					.program = fixed->noise.Get(),
					.bindings = { AnchorPasses::Bind::Uav(fixed->noiseVolume->Graph()), AnchorPasses::Bind::Latch(offsetof(LatchData, noise)) },
					.gate = kNoiseEntry });
			});
		});
		add("generate", FrameAnchors::PassKind::Compute, [this, shared, twin]() -> std::shared_ptr<org::RenderPass> {
			// A published input not declarable yet leaves the generate out of this build. Its first publish builds every graph again.
			for (const auto* input : { &shared->shadowHeights, &shared->cloudShadows })
				if (!input->empty() && !PublishedResources::IsDeclarable(*input)) {
					logger::info("{}: the generate waits for {} to be published; its frames have no volumetric lighting until the graph is built with it",
						kLogPrefix, *input);
					shared->passMade[kGenerateEntry] = false;
					return nullptr;
				}
			std::array<AnchorPasses::Bind, 3> samplers{};
			if (!Samplers(std::array{ kClampPoint, kClampLinear, kNoiseModes }, samplers, "generate")) {
				shared->passMade[kGenerateEntry] = false;
				return nullptr;
			}
			const auto feature = [](const std::string& a_id) { return a_id.empty() ? AnchorPasses::Bind::Unused() : AnchorPasses::Bind::Published(a_id); };
			return MakeStep(*shared, kGenerateEntry, "generate", [&](AnchorPasses::Builder& a_builder) {
				a_builder.Add(AnchorPasses::Dispatch{ .name = "generate",
					.program = fixed->generate.Get(),
					.bindings = { AnchorPasses::Bind::Shared(), samplers[0], samplers[1], samplers[2], AnchorPasses::Bind::SrvArray(shared->shadowmap),
						AnchorPasses::Bind::SrvArray(shared->shadowmapVL), AnchorPasses::Bind::Srv(fixed->noiseVolume->Graph()), AnchorPasses::Bind::Uav(*twin),
						AnchorPasses::Bind::Latch(offsetof(LatchData, generate)), feature(shared->shadowHeights), feature(shared->cloudShadows) },
					.gate = kGenerateEntry });
			});
		});
		add("raymarch", FrameAnchors::PassKind::Compute, [this, shared, twin] {
			return MakeStep(*shared, kRaymarchEntry, "raymarch", [&](AnchorPasses::Builder& a_builder) {
				a_builder.Add(AnchorPasses::Dispatch{ .name = "raymarch",
					.program = fixed->raymarch.Get(),
					.bindings = { AnchorPasses::Bind::Uav(*twin), AnchorPasses::Bind::Latch(offsetof(LatchData, raymarch)) },
					.gate = kRaymarchEntry });
			});
		});
		// The apply, then the blurs, so each frame's three steps are adjacent.
		for (std::uint32_t o = 0; o < 2; ++o) {
			const auto output = made->targets[o == 0 ? 0 : 1];
			const auto previous = made->targets[o == 0 ? 1 : 0];
			add(fmt::format("apply.{}", o), FrameAnchors::PassKind::Render, [this, shared, twin, output, previous, o]() -> std::shared_ptr<org::RenderPass> {
				const auto name = fmt::format("apply.{}", o);
				std::array<AnchorPasses::Bind, 6> samplers{};
				if (!Samplers(std::array{ kClampLinear, kClampLinear, kClampLinear, kWrapPoint, kClampLinear, kClampLinear }, samplers, name)) {
					shared->passMade[ApplyEntry(o)] = false;
					return nullptr;
				}
				return MakeStep(*shared, ApplyEntry(o), name, [&](AnchorPasses::Builder& a_builder) {
					AnchorPasses::Draw draw{};
					draw.name = name;
					draw.program = fixed->apply.Get();
					draw.bindings = { AnchorPasses::Bind::Shared(), AnchorPasses::Bind::Srv(shared->depth), AnchorPasses::Bind::Srv(previous),
						AnchorPasses::Bind::Srv(*twin), AnchorPasses::Bind::Srv(shared->noiseGradient), AnchorPasses::Bind::Srv(shared->motionVectors),
						AnchorPasses::Bind::Srv(shared->previousDepth), samplers[0], samplers[1], samplers[2], samplers[3], samplers[4], samplers[5],
						AnchorPasses::Bind::Latch(offsetof(LatchData, applyIntensityTemporal)) };
					draw.target = output;
					// The whole target, of the version each preparation binds. The shader keeps the engine's dynamic-resolution part.
					draw.viewport = {};
					draw.load = rhi::LoadOp::Load;
					draw.gate = ApplyEntry(o);
					a_builder.Add(std::move(draw));
				});
			});
			const auto swap = made->targets[2];
			add(fmt::format("blur-h.{}", o), FrameAnchors::PassKind::Compute, [this, shared, output, swap, o] {
				return MakeStep(*shared, BlurHEntry(o), fmt::format("blur-h.{}", o), [&](AnchorPasses::Builder& a_builder) {
					a_builder.Add(AnchorPasses::Dispatch{ .name = fmt::format("blur-h.{}", o),
						.program = fixed->blurH.Get(),
						.bindings = { AnchorPasses::Bind::Shared(), AnchorPasses::Bind::Srv(output), AnchorPasses::Bind::Srv(shared->depth), AnchorPasses::Bind::Uav(swap),
							AnchorPasses::Bind::Latch(offsetof(LatchData, blur)) },
						.gate = BlurHEntry(o) });
				});
			});
			add(fmt::format("blur-v.{}", o), FrameAnchors::PassKind::Compute, [this, shared, previous, swap, o] {
				return MakeStep(*shared, BlurVEntry(o), fmt::format("blur-v.{}", o), [&](AnchorPasses::Builder& a_builder) {
					a_builder.Add(AnchorPasses::Dispatch{ .name = fmt::format("blur-v.{}", o),
						.program = fixed->blurV.Get(),
						.bindings = { AnchorPasses::Bind::Shared(), AnchorPasses::Bind::Srv(swap), AnchorPasses::Bind::Srv(shared->depth), AnchorPasses::Bind::Uav(previous),
							AnchorPasses::Bind::Latch(offsetof(LatchData, blur)) },
						.gate = BlurVEntry(o) });
				});
			});
		}
		// The apply's last D3D11 step copies the main depth into kMAIN_COPY, which next frame's apply reads at t6.
		add("depth-copy", FrameAnchors::PassKind::Copy, [this, shared] {
			return MakeStep(*shared, kDepthCopyEntry, "depth-copy", [&](AnchorPasses::Builder& a_builder) {
				a_builder.Add(AnchorPasses::Copy{ .name = "depth-copy",
					.source = shared->depth,
					.target = shared->previousDepth,
					// The versions each preparation binds, so a resize needs no new pass.
					.width = 0,
					.height = 0,
					.gate = kDepthCopyEntry });
			});
		});
		a_client.registerResources = [fixedState = fixed, twin](org::RenderGraph& a_graph) {
			fixedState->noiseVolume->Register(a_graph);
			twin->Register(a_graph);
		};
		// The commit and the gates are this registration's, so a run of an older build commits for the passes it holds.
		a_client.commit = [this, shared](const FrameAnchors::Point&) { Commit(*shared); };
		a_client.active = [this] { return run.has_value(); };
		a_client.executed = [this](const FrameAnchors::Point&) { Executed(); };
		a_client.sharedConstants = true;
		a_client.gates = { fixed->gate };
		logger::info("{}: a client for the next revision (Terrain Shadows {}, Cloud Shadows {})", kLogPrefix,
			made->shadowHeights.empty() ? "off" : "by its published id", made->cloudShadows.empty() ? "off" : "by its published id");
		return true;
	}

	// ---- The engine's calls ----

	void BeginRender()
	{
		pending = {};
		inRender = true;
	}

	/** @brief When it generated, its constants and volume size are this frame's work. */
	void EndRender()
	{
		inRender = false;
		if (!pending.generate)
			return;
		pending.size = DescriptorSize();
		pending.rangeFactor = DescriptorField<float>(0x2C);
		std::memcpy(pending.generateConstants.data(), GenerateConstants(), kGenerateBytes);
		pending.generateConstants[kGenerateRows] = { pending.rangeFactor != 0.0f ? 1.0f / pending.rangeFactor : 0.0f, 0.0f, 0.0f, 0.0f };
		// The volume was declared at setup, so a render never declares anything (phase 3, design 3.5).
		if (fixed->vanilla && pending.size && !(pending.size == vanillaSize))
			Fatal(fmt::format("the engine renders its volume at {}x{}x{}, but setup declared {}x{}x{}", pending.size.width, pending.size.height,
				pending.size.depth, vanillaSize.width, vanillaSize.height, vanillaSize.depth));
	}

	std::uint32_t Apply(std::uint8_t* a_helper, std::uint32_t a_previous, std::uint32_t a_output)
	{
		// Helper +0x8 means the apply is on. +0x9 means no temporal blend this frame and is cleared by the apply.
		if (!a_helper[0x8])
			return a_previous;
		const bool noTemporal = a_helper[0x9] != 0;
		a_helper[0x9] = 0;
		if (a_output == kVolumetricTarget && a_previous == kPreviousTarget)
			pending.output = 0;
		else if (a_output == kPreviousTarget && a_previous == kVolumetricTarget)
			pending.output = 1;
		else
			skipped.Report(fmt::format("the apply's targets are {} and {}, not the VL pair", a_previous, a_output));
		const auto depth = DescriptorField<std::uint32_t>(0x38);
		pending.intensity = depth ? DescriptorField<float>(0x0) / static_cast<float>(depth) : 0.0f;
		pending.temporal = noTemporal ? 0.0f : TemporalFactor();
		return a_output;
	}

	/** @brief The frame's work runs on the graph now, with the client and the volume version of the adopted revision. */
	void Blur(std::uint32_t a_source, std::uint32_t a_destination, std::uint32_t a_swap)
	{
		const auto clear = [&] { pending = {}; };
		Observe();
		const auto skip = [&](std::string_view a_reason) {
			skipped.Report(a_reason);
			// Once a frame ran, a change switches in with its revision and the old one draws until then, so every frame has it.
			if (announced && lateSkips < kLateSkipsLogged)
				logger::warn("{}: frame {} has no volumetric lighting after the first one ran ({}){}", kLogPrefix, globals::state->frameCount, a_reason,
					++lateSkips == kLateSkipsLogged ? "; later ones are counted only" : "");
		};
		if (!pending.output) {
			skip("the blur ran without an apply on the VL pair");
			return clear();
		}
		const std::uint32_t output = *pending.output;
		if (a_source != OutputOf(output) || a_destination != PreviousOf(output) || a_swap != kSwapTarget) {
			skip(fmt::format("the blur's targets are {}, {} and {}, not the apply's", a_source, a_destination, a_swap));
			return clear();
		}
		if (!pending.generate) {
			skip("the apply ran without the VL render's generate this frame");
			return clear();
		}
		if (!volume) {
			skip("the engine gave no volume size yet");
			return clear();
		}
		if (!client.Registered()) {
			skip("the client is not registered yet (the first revision after a VL render found the sky's noise gradient registers it)");
			return clear();
		}
		// The engine was given the adopted revision's quality at EarlyPrepass, so its size is the bound version's.
		const auto extent = RenderGraph::Frame::Revision::Adopted().Extent(volume);
		const VolumeSize size{ extent[0], extent[1], extent[2] };
		if (size != pending.size)
			Fatal(fmt::format("the engine renders a {}x{}x{} volume, but the adopted revision's is {}x{}x{}", pending.size.width, pending.size.height,
				pending.size.depth, size.width, size.height, size.depth));

		VLRun next{ .output = output, .size = size, .noise = !noiseDone };
		auto& data = next.data;
		const auto& noiseSize = fixed->noiseSize;
		data.noise = { static_cast<float>(noiseSize.width), static_cast<float>(noiseSize.height), static_cast<float>(noiseSize.depth), 0.0f };
		data.raymarch = { static_cast<float>(size.width), static_cast<float>(size.height), static_cast<float>(size.depth), 0.0f };
		data.blur = BlurArea();
		data.applyIntensityTemporal = { pending.intensity, pending.temporal, 0.0f, 0.0f };
		data.applyRepartition = { pending.rangeFactor, 0.0f, 0.0f, 0.0f };
		data.generate = pending.generateConstants;
		// One group per row or column, each covering kBlurGroup - 2 * kBlurWindow pixels.
		const auto width = static_cast<std::uint32_t>(data.blur[0]), height = static_cast<std::uint32_t>(data.blur[1]);
		constexpr std::uint32_t span = kBlurGroup - 2 * kBlurWindow;
		next.blurHGroups = { (width + span - 1) / span, height };
		next.blurVGroups = { width, (height + span - 1) / span };
		run = next;
		const auto result = FrameAnchors::Run(kAnchor);
		const bool opened = run->opened;
		run.reset();
		if (!result)
			skip(fmt::format("the anchor's run did not submit (status {})", static_cast<std::uint32_t>(result.status)));
		else if (!opened)
			skip("the frame's graph does not hold the client yet (until the revision that registered it is adopted), or a pass was left out");
		clear();
	}

	/** @brief For the registration whose build this run executes. */
	void Commit(const VLRegistration& a_registration)
	{
		if (!run)
			return;
		// A pass this build left out (logged at the build) leaves the frame without volumetric lighting.
		for (const auto entry : { kGenerateEntry, kRaymarchEntry, ApplyEntry(run->output), BlurHEntry(run->output), BlurVEntry(run->output) })
			if (!a_registration.passMade[entry])
				return;
		if (run->noise && !a_registration.passMade[kNoiseEntry])
			return;
		run->opened = true;
		const auto& gate = *fixed->gate;
		fixed->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, run->data);
		const auto& size = run->size;
		if (run->noise) {
			const auto& noise = fixed->noiseSize;
			gate.OpenDispatch(kNoiseEntry, noise.width >> 5, noise.height >> 5, noise.depth);
		}
		// The engine uses 32x32 threads and one group per slice. The raymarch walks the slices itself.
		gate.OpenDispatch(kGenerateEntry, size.width >> 5, size.height >> 5, size.depth);
		gate.OpenDispatch(kRaymarchEntry, size.width >> 5, size.height >> 5, 1);
		gate.OpenDraw(ApplyEntry(run->output));
		gate.OpenDispatch(BlurHEntry(run->output), run->blurHGroups[0], run->blurHGroups[1], 1);
		gate.OpenDispatch(BlurVEntry(run->output), run->blurVGroups[0], run->blurVGroups[1], 1);
		gate.OpenDispatch(kDepthCopyEntry, 1);
	}

	/** @brief The noise volume counts as made once a run with it is submitted. */
	void Executed()
	{
		if (!run || !run->opened)
			return;
		if (run->noise && !noiseDone) {
			noiseDone = true;
			logger::info("{}: the noise volume ({}x{}x{}) is made on the graph", kLogPrefix, fixed->noiseSize.width, fixed->noiseSize.height, fixed->noiseSize.depth);
		}
		if (!std::exchange(announced, true))
			logger::info("{}: the first frame's volumetric lighting ran on the graph ({}x{}x{} volume)", kLogPrefix, run->size.width, run->size.height, run->size.depth);
	}
};

ORGVolumetricLighting::ORGVolumetricLighting() = default;
ORGVolumetricLighting::~ORGVolumetricLighting() = default;

struct ORGVolumetricLightingDetours
{
	static ORGVolumetricLighting::Impl* Taking()
	{
		auto& self = ORGVolumetricLighting::Get();
		return self.IsActive() && FrameAnchors::IsRenderOwner() ? self.impl.get() : nullptr;
	}

	/** @brief The VL render (SE 100306, AE 107023). Its CPU work runs and its dispatches are withheld. */
	struct Render
	{
		static void thunk(void* a_descriptor, void* a_camera, std::uintptr_t a_third, std::uintptr_t a_fourth)
		{
			auto* impl = Taking();
			if (!impl) {
				func(a_descriptor, a_camera, a_third, a_fourth);
				return;
			}
			impl->BeginRender();
			func(a_descriptor, a_camera, a_third, a_fourth);
			impl->EndRender();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief ApplyVL (SE 100475, AE 107193). The helper is at ImageSpaceManager+0x170. */
	struct Apply
	{
		static std::uint32_t thunk(std::uint8_t* a_helper, std::uint32_t a_previous, std::uint32_t a_output)
		{
			auto* impl = Taking();
			if (!impl || !a_helper)
				return func(a_helper, a_previous, a_output);
			return impl->Apply(a_helper, a_previous, a_output);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief The VL blur (SE 100486, AE 107206). The helper is at ImageSpaceManager+0x188. */
	struct Blur
	{
		static std::uint32_t thunk(void* a_helper, std::uint32_t a_source, std::uint32_t a_destination, std::uint32_t a_swap)
		{
			auto* impl = Taking();
			if (!impl)
				return func(a_helper, a_source, a_destination, a_swap);
			impl->Blur(a_source, a_destination, a_swap);
			return a_destination;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
};

void ORGVolumetricLighting::Install()
{
	if (!Wanted())
		return;
	stl::detour_thunk<ORGVolumetricLightingDetours::Render>(REL::RelocationID(100306, 107023));
	stl::detour_thunk<ORGVolumetricLightingDetours::Apply>(REL::RelocationID(100475, 107193));
	stl::detour_thunk<ORGVolumetricLightingDetours::Blur>(REL::RelocationID(100486, 107206));
	logger::info("{}: hooks installed (the VL render, ApplyVL, the VL blur)", kLogPrefix);
}

void ORGVolumetricLighting::Setup()
{
	if (!BeginSetup())
		return;
	if (!globals::game::renderer)
		Fatal("there is no renderer");
	try {
		auto fixed = std::make_shared<VLFixed>();
		const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
		const auto formatOf = [&](std::uint32_t a_target) {
			D3D11_RENDER_TARGET_VIEW_DESC view{};
			if (!targets[a_target].RTV)
				return rhi::Format::Unknown;
			targets[a_target].RTV->GetDesc(&view);
			return rhi::helpers::ToRHI(view.Format);
		};
		fixed->outputFormat = formatOf(kVolumetricTarget);
		if (fixed->outputFormat == rhi::Format::Unknown || fixed->outputFormat != formatOf(kPreviousTarget))
			Fatal("the VL targets have no render target view, or two formats");

		// Same defines as the D3D11 builds. With the feature off the engine's shaders get no feature define.
		fixed->vanilla = !FeatureOn();
		auto defines = fixed->vanilla ? std::vector<std::pair<std::string, std::string>>{} : FeatureDefines();
		auto computeDefines = defines;
		computeDefines.emplace_back("CSHADER", "");
		const auto compute = [&](const char* a_source, std::uint32_t a_words) {
			return RenderGraph::Programs::Queue("VolumetricLighting",
				ComputeProgram::Desc{ .source = a_source, .constantWords = a_words, .defines = AnchorPasses::Defines(computeDefines) });
		};
		fixed->noise = compute("ISPerlinNoiseCS.hlsl", ShaderBindings::Words<NoiseBlock>());
		fixed->generate = compute("ISVolumetricLightingGenerateCS.hlsl", ShaderBindings::Words<GenerateBlock>());
		fixed->raymarch = compute("ISVolumetricLightingRaymarchCS.hlsl", ShaderBindings::Words<RaymarchBlock>());
		fixed->blurH = compute("ISVolumetricLightingBlurHCS.hlsl", ShaderBindings::Words<BlurBlock>());
		fixed->blurV = compute("ISVolumetricLightingBlurVCS.hlsl", ShaderBindings::Words<BlurBlock>());
		RasterProgram::Desc apply{};
		apply.pixelSource = "ISApplyVolumetricLighting.hlsl";
		// An FXC-era source.
		apply.languageVersion = L"2018";
		apply.constantWords = ShaderBindings::Words<ApplyBlock>();
		apply.defines = AnchorPasses::Defines(defines);
		apply.targets = { { .format = fixed->outputFormat } };
		fixed->apply = RenderGraph::Programs::Queue("VolumetricLighting", std::move(apply));

		fixed->latch = std::make_shared<org::LatchBlock>("cs.volumetric-lighting.latch", static_cast<std::uint32_t>(sizeof(LatchData)));
		fixed->gate = RenderGraph::RunGate::Create("cs.volumetric-lighting.gate", kGateEntries);

		// Made as the engine makes its own (AE 107023, R8_UNORM at the INI's size), filled once by ISPerlinNoiseCS.
		fixed->noiseSize = NoiseSize();
		if (!fixed->noiseSize)
			Fatal("the noise volume's INI size (iVolumetricLightingNoiseTexture*:Display) is not readable");
		D3D11_TEXTURE3D_DESC noise{};
		noise.Width = fixed->noiseSize.width;
		noise.Height = fixed->noiseSize.height;
		noise.Depth = fixed->noiseSize.depth;
		noise.MipLevels = 1;
		noise.Format = DXGI_FORMAT_R8_UNORM;
		noise.Usage = D3D11_USAGE_DEFAULT;
		noise.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		fixed->noiseVolume = std::make_unique<GraphResources::Texture3D>(noise, "VolumetricLighting::Noise", GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });

		auto made = std::make_unique<Impl>();
		made->fixed = std::move(fixed);
		// The volume first, so the client's key never sees it missing. The volume follows the revisions.
		if (!made->fixed->vanilla) {
			made->DeclareFeatureVolume();
		} else if (const auto size = DescriptorSize()) {
			made->vanillaSize = size;
			made->DeclareVanillaVolume(size);
		} else {
			// Declared at setup only, never inside a frame. The descriptor is filled from the INI before the first frame.
			Fatal("the engine's volume size (the volumetric lighting descriptor) is not known at setup");
		}
		// The client exists from the first revision after a VL render found its inputs.
		made->DeclareClient();
		impl = std::move(made);
		logger::info("{}: on{}; noise volume {}x{}x{}; the volume and the client follow the frame revisions", kLogPrefix,
			impl->fixed->vanilla ? ", the feature off: the engine's volumetric lighting" : "", impl->fixed->noiseSize.width, impl->fixed->noiseSize.height,
			impl->fixed->noiseSize.depth);
	} catch (const std::exception& e) {
		Fatal(fmt::format("its setup threw ({})", e.what()));
	}
}

bool ORGVolumetricLighting::TakeEngineDispatch(std::string_view a_computeShader)
{
	if (!impl || !impl->inRender)
		return false;
	if (a_computeShader == "ISVolumetricLightingGenerateCS")
		impl->pending.generate = true;
	else if (a_computeShader != "ISVolumetricLightingRaymarchCS")
		impl->engineDispatches.Report(fmt::format("{} (the noise volume is made on the graph)", a_computeShader.empty() ? "an unnamed shader" : a_computeShader));
	return true;
}

