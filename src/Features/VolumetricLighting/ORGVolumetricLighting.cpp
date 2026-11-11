#include "ORGVolumetricLighting.h"

#include "RenderGraph/Switches.h"

bool ORGVolumetricLighting::Requested()
{
	static const bool requested = Switches::On("CS_ORG_VOLUMETRIC_LIGHTING");
	return requested;
}

ORGVolumetricLighting& ORGVolumetricLighting::Get()
{
	static ORGVolumetricLighting s_lighting;
	return s_lighting;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Feature.h"
#	include "Features/CloudShadows.h"
#	include "Features/CloudShadows/ORGCloudShadows.h"
#	include "Features/TerrainShadows.h"
#	include "Features/TerrainShadows/ORGTerrainShadows.h"
#	include "Features/VolumetricLighting.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Engine/Pipeline/Descriptions.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "Utils/Logging.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <array>
#	include <cstddef>
#	include <cstring>
#	include <functional>
#	include <map>
#	include <optional>
#	include <span>
#	include <string>
#	include <vector>

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
	// The volume sizes registered at once: the exterior's and the interior's (VolumetricLighting::VolumeSize).
	constexpr std::uint32_t kMaxVolumes = 2;
	// The engine's PerTechnique block of the density generation (SetComputeShaderConstants' size in the VL render).
	constexpr std::size_t kGenerateBytes = 0x180;
	constexpr std::uint32_t kGenerateRows = static_cast<std::uint32_t>(kGenerateBytes / 16);

	// The VL targets: the apply draws into one of the first two and reads the other (they swap by frame), the blurs go through the
	// copy and end in the one the apply read (RenderWorldPostProcess, SE 100430, AE 107148).
	constexpr std::uint32_t kVolumetricTarget = RenderTarget::kIMAGESPACE_VOLUMETRIC_LIGHTING;
	constexpr std::uint32_t kPreviousTarget = RenderTarget::kIMAGESPACE_VOLUMETRIC_LIGHTING_PREVIOUS;
	constexpr std::uint32_t kSwapTarget = RenderTarget::kIMAGESPACE_VOLUMETRIC_LIGHTING_COPY;
	/** @brief The apply's output by its index (0: kIMAGESPACE_VOLUMETRIC_LIGHTING), and the target it reads, the other one. */
	constexpr std::uint32_t OutputOf(std::uint32_t a_output) { return a_output == 0 ? kVolumetricTarget : kPreviousTarget; }
	constexpr std::uint32_t PreviousOf(std::uint32_t a_output) { return a_output == 0 ? kPreviousTarget : kVolumetricTarget; }

	// Run gate entries: the noise, then four per volume (generate, raymarch, the apply into either output), then the blurs by output.
	constexpr std::uint32_t kNoiseEntry = 0;
	constexpr std::uint32_t GenerateEntry(std::uint32_t a_volume) { return 1 + 4 * a_volume; }
	constexpr std::uint32_t RaymarchEntry(std::uint32_t a_volume) { return 2 + 4 * a_volume; }
	constexpr std::uint32_t ApplyEntry(std::uint32_t a_volume, std::uint32_t a_output) { return 3 + 4 * a_volume + a_output; }
	constexpr std::uint32_t BlurHEntry(std::uint32_t a_output) { return 1 + 4 * kMaxVolumes + a_output; }
	constexpr std::uint32_t BlurVEntry(std::uint32_t a_output) { return 3 + 4 * kMaxVolumes + a_output; }
	constexpr std::uint32_t kGateEntries = 5 + 4 * kMaxVolumes;

	using Float4 = std::array<float, 4>;

	// One run's values, in its frame slot of the latch block: each shader's LATCH entry.
	struct LatchData
	{
		Float4 noise;                     // ISPerlinNoiseCS's PerlinNoiseConstants: the noise volume's size
		Float4 raymarch;                  // VolumetricLightingRaymarchConstants: the volume's size (StepIndex is never read)
		std::array<std::int32_t, 4> blur;  // VolumetricLightingBlurConstants: VolumetricLighting::BlurData
		Float4 applyIntensityTemporal;    // VolumetricLightingApplyConstants: g_IntensityX_TemporalY
		Float4 applyRepartition;          // x: the repartition's exponent
		std::array<Float4, kGenerateRows + 1> generate;  // VolumetricLightingGenerateConstants: the engine's block, then x the inverse exponent
	};
	static_assert(offsetof(LatchData, applyIntensityTemporal) % 16 == 0 && offsetof(LatchData, generate) % 16 == 0 && sizeof(LatchData) % 16 == 0);

	// The shaders' push blocks under ORG, in their resource lists' order (check-dual-target-shaders reports the sizes).
	using ShaderBindings::Handle;
	struct NoiseBlock  // ISPerlinNoiseCS (3D)
	{
		Handle output;
		ShaderBindings::Latch constants;
	};
	struct GenerateBlock  // ISVolumetricLightingGenerateCS
	{
		SharedConstants::Handles shared;
		Handle shadowmapSampler, shadowmapVLSampler, noiseSampler;
		Handle shadowmap, shadowmapVL, noise;
		Handle density;
		ShaderBindings::Latch constants;
		Handle shadowHeights, cloudShadows;
	};
	struct RaymarchBlock  // ISVolumetricLightingRaymarchCS
	{
		Handle density;
		ShaderBindings::Latch constants;
	};
	struct BlurBlock  // ISVolumetricLightingBlurHCS / VCS
	{
		SharedConstants::Handles shared;
		Handle input, depth, output;
		ShaderBindings::Latch constants;
	};
	struct ApplyBlock  // ISApplyVolumetricLighting
	{
		SharedConstants::Handles shared;
		Handle depth, previous, volume, noiseGradient, motionVectors, previousDepth;
		Handle depthSampler, previousSampler, volumeSampler, noiseGradientSampler, motionVectorsSampler, previousDepthSampler;
		ShaderBindings::Latch constants;
	};
	static_assert(sizeof(NoiseBlock) == 12 && sizeof(GenerateBlock) == 60 && sizeof(RaymarchBlock) == 12 && sizeof(BlurBlock) == 36 && sizeof(ApplyBlock) == 72);

	/** @brief An engine sampler by its shadow state modes (address, filter), as the engine's calls set them. */
	struct SamplerModes
	{
		std::uint32_t address = 0, filter = 0;
		auto operator<=>(const SamplerModes&) const = default;
	};
	// The VL render's (SE 100307's disassembly: slot, address, filter): the shadow map point, the VL shadow map linear, the noise
	// with address mode 5, linear. The apply's (SE 100475): every slot clamp linear but the noise gradient's, wrap point.
	constexpr SamplerModes kClampPoint{ 0, 0 }, kClampLinear{ 0, 1 }, kNoiseModes{ 5, 1 }, kWrapPoint{ 3, 0 };

	/** @brief A volume's size in texels (the VL descriptor's +0x30, +0x34, +0x38). */
	struct VolumeSize
	{
		std::uint32_t width = 0, height = 0, depth = 0;
		auto operator<=>(const VolumeSize&) const = default;
		explicit operator bool() const { return width >= 32 && height >= 32 && depth > 0; }
	};

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	// The engine's data
	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	/**
	 * @brief The VL descriptor (VolumetricLighting::GetVLDescriptor, SE 100297, AE 107014). Its fields (Ghidra, SE 100306 and AE
	 * 107023): +0x00 the intensity and +0x2C the repartition's range factor (the first and last of the twelve render data floats),
	 * +0x30 to +0x38 the volume's size.
	 */
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

	/** @brief The apply's temporal factor (SE 100298, AE 107015): the weather's value while it has one, else 0. */
	float TemporalFactor()
	{
		static REL::Relocation<float (*)()> temporal{ REL::RelocationID(100298, 107015) };
		return temporal();
	}

	/** @brief The density generation's PerTechnique as the VL render's helper wrote it (SE 527948, AE 414886). */
	const std::byte* GenerateConstants()
	{
		return reinterpret_cast<const std::byte*>(REL::RelocationID(527948, 414886).address());
	}

	/**
	 * @brief The noise gradient texture's view the apply binds at t4 (SE 100475, AE 107193: the global's object, +0x48 its texture,
	 * +0x10 that one's SRV; the sky shader binds the same). Null before it is loaded.
	 */
	ID3D11ShaderResourceView* NoiseGradientView()
	{
		const auto* holder = *reinterpret_cast<const std::byte* const*>(REL::RelocationID(525022, 411504).address());
		if (!holder)
			return nullptr;
		const auto* texture = *reinterpret_cast<const std::byte* const*>(holder + 0x48);
		return texture ? *reinterpret_cast<ID3D11ShaderResourceView* const*>(texture + 0x10) : nullptr;
	}

	/** @brief The engine's noise volume's size: its INI settings, which the VL render's first call creates the volume with. */
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

	/** @brief The defines the D3D11 build of an ImageSpace shader gets from the features (ShaderCache's GetImagespaceShaderDefines). */
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

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	// State
	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	/** @brief What Setup makes once and every registration shares. */
	struct VLFixed
	{
		std::shared_ptr<const ComputeProgram> noise, generate, raymarch, blurH, blurV;
		std::shared_ptr<const RasterProgram> apply;
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
		std::unique_ptr<GraphResources::Texture3D> noiseVolume;
		VolumeSize noiseSize;
		rhi::Format outputFormat = rhi::Format::Unknown;
		// The engine's samplers as graph samplers, taken when a graph is built (the engine fills its table after CS's Setup).
		std::map<SamplerModes, org::OwnedDescriptorBinding> samplers;
	};

	/** @brief A graph resource a pass reads: an import, or a published id. */
	struct VLSource
	{
		std::shared_ptr<org::Resource> resource;
		std::string published;

		explicit operator bool() const { return resource || !published.empty(); }
		AnchorPasses::Bind ToBind() const { return resource ? AnchorPasses::Bind::Srv(resource) : AnchorPasses::Bind::Published(published); }
	};

	/** @brief One registration: its volumes and inputs, immutable once registered. */
	struct VLRegistration
	{
		std::vector<VolumeSize> sizes;
		std::vector<std::unique_ptr<GraphResources::Texture3D>> volumes;
		std::shared_ptr<org::ExternalTextureResource> targets[3];  // kVolumetricTarget, kPreviousTarget, kSwapTarget
		std::shared_ptr<org::ExternalTextureResource> motionVectors, depth, previousDepth, shadowmap, shadowmapVL, noiseGradient;
		VLSource shadowHeights, cloudShadows;
		// The engine objects the imports were made from: one that changes was made again (a resize), and the client registers again.
		std::vector<const void*> engineObjects;
		const ID3D11ShaderResourceView* noiseGradientView = nullptr;
	};

	/** @brief The frame's work, as the engine's calls gave it, until the blur's call runs it. */
	struct VLPending
	{
		bool generate = false;
		VolumeSize size;
		std::array<Float4, kGenerateRows + 1> generateConstants{};
		float rangeFactor = 1.0f;
		std::optional<std::uint32_t> output;  // the apply's output (OutputOf)
		float intensity = 0.0f, temporal = 0.0f;
	};

	/** @brief The run in progress: what its commit opens. */
	struct VLRun
	{
		std::uint32_t volume = 0;
		std::uint32_t output = 0;
		VolumeSize size;
		bool noise = false;
		LatchData data{};
		std::array<std::uint32_t, 2> blurHGroups{}, blurVGroups{};
	};
}

struct ORGVolumetricLighting::Impl
{
	std::shared_ptr<VLFixed> fixed;
	std::shared_ptr<const VLRegistration> registration;
	std::string lastMissing;  // why the last registration attempt could not be made, logged when it changes
	VLPending pending;
	std::optional<VLRun> run;
	bool failed = false;
	bool inRender = false;
	bool noiseDone = false;
	bool announced = false;
	// By gate entry: whether the last build made its pass (a published input not declarable yet leaves the generate out).
	std::array<bool, kGateEntries> passMade{};
	Util::ReasonLog skipped{ fmt::format("{}: a frame's volumetric lighting is skipped", kLogPrefix) };
	Util::ReasonLog engineDispatches{ fmt::format("{}: the VL render's dispatch is withheld", kLogPrefix) };

	/** @brief The graph sampler of an engine sampler (the engine's table entry), made at a graph build. UINT32_MAX when it has none. */
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

	/** @brief Every engine object the registration's imports come from now, in a fixed order, read from the renderer's tables. */
	static std::vector<const void*> EngineObjects()
	{
		auto* renderer = globals::game::renderer;
		const auto& targets = renderer->GetRuntimeData().renderTargets;
		const auto& depths = renderer->GetDepthStencilData().depthStencils;
		return { targets[kVolumetricTarget].texture, targets[kPreviousTarget].texture, targets[kSwapTarget].texture, targets[RenderTarget::kMOTION_VECTOR].texture,
			depths[DepthTarget::kMAIN].texture, depths[DepthTarget::kMAIN_COPY].texture, depths[DepthTarget::kSHADOWMAPS_ESRAM].texture,
			depths[DepthTarget::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM].texture };
	}

	/**
	 * @brief Terrain or Cloud Shadows' texture the generate reads through ShadowSampling: its published id on the feature's ORG path,
	 * else the D3D11 texture behind the view the feature binds, imported. Empty when the feature is off, or its texture missing.
	 */
	static VLSource FeatureInput(bool a_loaded, bool a_orgPath, std::string_view a_id, ID3D11ShaderResourceView* a_view, std::string_view a_importName, bool& a_missing)
	{
		if (!a_loaded)
			return {};
		if (a_orgPath || PublishedResources::IsRegistered(a_id))
			return { nullptr, std::string(a_id) };
		if (!a_view) {
			a_missing = true;
			return {};
		}
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		const auto texture = resource.try_as<ID3D11Texture2D>();
		auto imported = texture ? FrameAnchors::D3D11Texture(a_importName, texture.get(), a_view, nullptr) : nullptr;
		if (!imported)
			a_missing = true;
		return { imported, {} };
	}

	/** @brief A pass made at a graph build from the step a_add adds: the AnchorPasses pass, or null (logged) when it cannot be made. */
	std::shared_ptr<org::RenderPass> MakeStep(std::uint32_t a_entry, std::string_view a_name, const std::function<void(AnchorPasses::Builder&)>& a_add)
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
			passMade[a_entry] = pass != nullptr;
		return pass;
	}

	/** @brief The samplers a pass takes, or false (logged) when the engine's table has one missing at this build. */
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

	/**
	 * @brief Registers the client for a_sizes: a volume and its passes for each, the shared passes, every input imported. False with
	 * a_missing set when an input is not there yet (the registration is tried again at the next VL render).
	 */
	bool Register(std::vector<VolumeSize> a_sizes, std::string& a_missing)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer) {
			a_missing = "no renderer";
			return false;
		}
		// The sizes in their order (the one the engine has now first), each once, at most kMaxVolumes.
		std::vector<VolumeSize> sizes;
		for (const auto& size : a_sizes)
			if (size && std::ranges::find(sizes, size) == sizes.end() && sizes.size() < kMaxVolumes)
				sizes.push_back(size);
		a_sizes = std::move(sizes);
		if (a_sizes.empty()) {
			a_missing = "no volume size";
			return false;
		}
		auto made = std::make_shared<VLRegistration>();
		made->sizes = a_sizes;
		made->engineObjects = EngineObjects();
		for (std::uint32_t i = 0; i < 3; ++i)
			made->targets[i] = FrameAnchors::EngineTarget(i == 0 ? kVolumetricTarget : i == 1 ? kPreviousTarget : kSwapTarget);
		made->motionVectors = FrameAnchors::EngineTarget(RenderTarget::kMOTION_VECTOR);
		made->depth = FrameAnchors::EngineDepth(DepthTarget::kMAIN);
		made->previousDepth = FrameAnchors::EngineDepth(DepthTarget::kMAIN_COPY);
		made->shadowmap = FrameAnchors::EngineDepth(DepthTarget::kSHADOWMAPS_ESRAM);
		made->shadowmapVL = FrameAnchors::EngineDepth(DepthTarget::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM);
		if (!made->targets[0] || !made->targets[1] || !made->targets[2] || !made->motionVectors || !made->depth || !made->previousDepth || !made->shadowmap ||
			!made->shadowmapVL) {
			a_missing = "an engine target or depth could not be imported";
			return false;
		}
		auto* noiseGradient = NoiseGradientView();
		if (noiseGradient) {
			winrt::com_ptr<ID3D11Resource> resource;
			noiseGradient->GetResource(resource.put());
			const auto texture = resource.try_as<ID3D11Texture2D>();
			made->noiseGradient = texture ? FrameAnchors::D3D11Texture("volumetric-lighting.noise-gradient", texture.get(), noiseGradient, nullptr) : nullptr;
			made->noiseGradientView = noiseGradient;
		}
		if (!made->noiseGradient) {
			a_missing = "the sky's noise gradient texture is not loaded yet";
			return false;
		}
		bool missing = false;
		auto& terrainShadows = globals::features::terrainShadows;
		auto& cloudShadows = globals::features::cloudShadows;
		made->shadowHeights = FeatureInput(terrainShadows.loaded, ORGTerrainShadows::Requested(), ORGTerrainShadows::kShadowHeightId,
			terrainShadows.loaded ? terrainShadows.GetShadowHeightSRV() : nullptr, "volumetric-lighting.terrain-shadows", missing);
		made->cloudShadows = FeatureInput(cloudShadows.loaded, ORGCloudShadows::Requested(), ORGCloudShadows::kOcclusionId,
			cloudShadows.loaded ? cloudShadows.OcclusionSRV() : nullptr, "volumetric-lighting.cloud-shadows", missing);
		if (missing) {
			a_missing = "Terrain or Cloud Shadows' texture is not made yet";
			return false;
		}
		for (const auto& size : a_sizes) {
			D3D11_TEXTURE3D_DESC desc{};
			desc.Width = size.width;
			desc.Height = size.height;
			desc.Depth = size.depth;
			desc.MipLevels = 1;
			// The engine's accumulation volume's format (AE 0x1414c5fa0: DXGI_FORMAT_R16_FLOAT).
			desc.Format = DXGI_FORMAT_R16_FLOAT;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			// Written by the generate and read by the apply in the same run: transient, aliasable.
			auto volume = std::make_unique<GraphResources::Texture3D>(desc, fmt::format("VolumetricLighting::Volume{}x{}x{}", size.width, size.height, size.depth),
				GraphResources::Options{ .lifetime = GraphResources::Lifetime::Transient });
			if (!*volume) {
				a_missing = "a volume could not be made";
				return false;
			}
			made->volumes.push_back(std::move(volume));
		}

		const std::shared_ptr<const VLRegistration> shared = made;
		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		std::int32_t order = 0;
		const auto add = [&](std::string a_name, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make) {
			client.passes.push_back({ .name = fmt::format("{}.{}", kClientId, a_name), .anchor = kAnchor, .order = order++, .kind = a_kind, .make = std::move(a_make) });
		};
		add("noise", FrameAnchors::PassKind::Compute, [this] {
			return MakeStep(kNoiseEntry, "noise", [&](AnchorPasses::Builder& a_builder) {
				a_builder.Add(AnchorPasses::Dispatch{ .name = "noise",
					.program = fixed->noise,
					.bindings = { AnchorPasses::Bind::Uav(fixed->noiseVolume->Graph()), AnchorPasses::Bind::Latch(offsetof(LatchData, noise)) },
					.gate = kNoiseEntry });
			});
		});
		for (std::uint32_t v = 0; v < made->volumes.size(); ++v) {
			const auto volume = made->volumes[v]->Graph();
			add(fmt::format("generate.{}", v), FrameAnchors::PassKind::Compute, [this, shared, volume, v]() -> std::shared_ptr<org::RenderPass> {
				const auto name = fmt::format("generate.{}", v);
				// A published input the graph cannot declare yet (its producer publishes later) leaves the generate out of this build.
				for (const auto* input : { &shared->shadowHeights, &shared->cloudShadows })
					if (!input->published.empty() && !PublishedResources::IsDeclarable(input->published)) {
						logger::info("{}: {} waits for {} to be published; its frames are skipped until the graph is built with it", kLogPrefix, name, input->published);
						passMade[GenerateEntry(v)] = false;
						return nullptr;
					}
				std::array<AnchorPasses::Bind, 3> samplers{};
				if (!Samplers(std::array{ kClampPoint, kClampLinear, kNoiseModes }, samplers, name)) {
					passMade[GenerateEntry(v)] = false;
					return nullptr;
				}
				return MakeStep(GenerateEntry(v), name, [&](AnchorPasses::Builder& a_builder) {
					a_builder.Add(AnchorPasses::Dispatch{ .name = name,
						.program = fixed->generate,
						.bindings = { AnchorPasses::Bind::Shared(), samplers[0], samplers[1], samplers[2], AnchorPasses::Bind::SrvArray(shared->shadowmap),
							AnchorPasses::Bind::SrvArray(shared->shadowmapVL), AnchorPasses::Bind::Srv(fixed->noiseVolume->Graph()), AnchorPasses::Bind::Uav(volume),
							AnchorPasses::Bind::Latch(offsetof(LatchData, generate)),
							shared->shadowHeights ? shared->shadowHeights.ToBind() : AnchorPasses::Bind::Unused(),
							shared->cloudShadows ? shared->cloudShadows.ToBind() : AnchorPasses::Bind::Unused() },
						.gate = GenerateEntry(v) });
				});
			});
			add(fmt::format("raymarch.{}", v), FrameAnchors::PassKind::Compute, [this, volume, v] {
				return MakeStep(RaymarchEntry(v), fmt::format("raymarch.{}", v), [&](AnchorPasses::Builder& a_builder) {
					a_builder.Add(AnchorPasses::Dispatch{ .name = fmt::format("raymarch.{}", v),
						.program = fixed->raymarch,
						.bindings = { AnchorPasses::Bind::Uav(volume), AnchorPasses::Bind::Latch(offsetof(LatchData, raymarch)) },
						.gate = RaymarchEntry(v) });
				});
			});
		}
		// By output: the apply of each volume, then the blurs, so each frame's three steps follow each other.
		for (std::uint32_t o = 0; o < 2; ++o) {
			const auto output = made->targets[o == 0 ? 0 : 1];
			const auto previous = made->targets[o == 0 ? 1 : 0];
			for (std::uint32_t v = 0; v < made->volumes.size(); ++v) {
				const auto volume = made->volumes[v]->Graph();
				add(fmt::format("apply.{}.{}", v, o), FrameAnchors::PassKind::Render, [this, shared, volume, output, previous, v, o]() -> std::shared_ptr<org::RenderPass> {
					const auto name = fmt::format("apply.{}.{}", v, o);
					std::array<AnchorPasses::Bind, 6> samplers{};
					if (!Samplers(std::array{ kClampLinear, kClampLinear, kClampLinear, kWrapPoint, kClampLinear, kClampLinear }, samplers, name)) {
						passMade[ApplyEntry(v, o)] = false;
						return nullptr;
					}
					return MakeStep(ApplyEntry(v, o), name, [&](AnchorPasses::Builder& a_builder) {
						AnchorPasses::Draw draw{};
						draw.name = name;
						draw.program = fixed->apply;
						draw.bindings = { AnchorPasses::Bind::Shared(), AnchorPasses::Bind::Srv(shared->depth), AnchorPasses::Bind::Srv(previous),
							AnchorPasses::Bind::Srv(volume), AnchorPasses::Bind::Srv(shared->noiseGradient), AnchorPasses::Bind::Srv(shared->motionVectors),
							AnchorPasses::Bind::Srv(shared->previousDepth), samplers[0], samplers[1], samplers[2], samplers[3], samplers[4], samplers[5],
							AnchorPasses::Bind::Latch(offsetof(LatchData, applyIntensityTemporal)) };
						draw.target = output;
						// The whole target: the shader keeps the engine's dynamic-resolution part (ISApplyVolumetricLighting.hlsl).
						draw.viewport = { .x = 0, .y = 0, .width = output->GetWidth(), .height = output->GetHeight() };
						draw.load = rhi::LoadOp::Load;
						draw.gate = ApplyEntry(v, o);
						a_builder.Add(std::move(draw));
					});
				});
			}
			const auto swap = made->targets[2];
			add(fmt::format("blur-h.{}", o), FrameAnchors::PassKind::Compute, [this, shared, output, swap, o] {
				return MakeStep(BlurHEntry(o), fmt::format("blur-h.{}", o), [&](AnchorPasses::Builder& a_builder) {
					a_builder.Add(AnchorPasses::Dispatch{ .name = fmt::format("blur-h.{}", o),
						.program = fixed->blurH,
						.bindings = { AnchorPasses::Bind::Shared(), AnchorPasses::Bind::Srv(output), AnchorPasses::Bind::Srv(shared->depth), AnchorPasses::Bind::Uav(swap),
							AnchorPasses::Bind::Latch(offsetof(LatchData, blur)) },
						.gate = BlurHEntry(o) });
				});
			});
			add(fmt::format("blur-v.{}", o), FrameAnchors::PassKind::Compute, [this, shared, previous, swap, o] {
				return MakeStep(BlurVEntry(o), fmt::format("blur-v.{}", o), [&](AnchorPasses::Builder& a_builder) {
					a_builder.Add(AnchorPasses::Dispatch{ .name = fmt::format("blur-v.{}", o),
						.program = fixed->blurV,
						.bindings = { AnchorPasses::Bind::Shared(), AnchorPasses::Bind::Srv(swap), AnchorPasses::Bind::Srv(shared->depth), AnchorPasses::Bind::Uav(previous),
							AnchorPasses::Bind::Latch(offsetof(LatchData, blur)) },
						.gate = BlurVEntry(o) });
				});
			});
		}
		// The apply's last step on D3D11 (CopyResource of the main depth into kMAIN_COPY): next frame's apply reads it at t6. A copy
		// has no gate; the anchor runs only for a frame with the apply.
		add("depth-copy", FrameAnchors::PassKind::Copy, [this, shared] {
			return MakeStep(UINT32_MAX, "depth-copy", [&](AnchorPasses::Builder& a_builder) {
				a_builder.Add(AnchorPasses::Copy{ .name = "depth-copy",
					.source = shared->depth,
					.target = shared->previousDepth,
					.width = shared->depth->GetWidth(),
					.height = shared->depth->GetHeight() });
			});
		});
		client.registerResources = [shared, fixedState = fixed](org::RenderGraph& a_graph) {
			fixedState->noiseVolume->Register(a_graph);
			for (const auto& volume : shared->volumes)
				volume->Register(a_graph);
		};
		client.commit = [this](const FrameAnchors::Point&) { Commit(); };
		client.active = [this] { return run.has_value(); };
		client.executed = [this](const FrameAnchors::Point&) { Executed(); };
		client.onFailed = [this] {
			logger::error("{} returns to D3D11: the render graph failed", kLogPrefix);
			failed = true;
			inRender = false;
		};
		client.sharedConstants = true;
		client.gates = { fixed->gate };
		passMade.fill(false);
		if (!FrameAnchors::Register(std::move(client))) {
			a_missing = "the VolumetricLighting anchor refused the client";
			return false;
		}
		registration = std::move(made);
		std::string sizes;
		for (const auto& size : registration->sizes)
			sizes += fmt::format("{}{}x{}x{}", sizes.empty() ? "" : ", ", size.width, size.height, size.depth);
		logger::info("{}: registered with volumes {} (Terrain Shadows {}, Cloud Shadows {}); the anchor's graph holds it from its next build", kLogPrefix, sizes,
			registration->shadowHeights ? (registration->shadowHeights.published.empty() ? "imported" : "by its published id") : "off",
			registration->cloudShadows ? (registration->cloudShadows.published.empty() ? "imported" : "by its published id") : "off");
		return true;
	}

	/** @brief a_also (the descriptor's size now, when given), then the sizes SetupVL gives the engine for exteriors and interiors. */
	static std::vector<VolumeSize> SettingsSizes(std::optional<VolumeSize> a_also = std::nullopt)
	{
		auto& feature = globals::features::volumetricLighting;
		std::vector<VolumeSize> sizes;
		if (a_also)
			sizes.push_back(*a_also);
		for (const bool interior : { false, true }) {
			const auto size = feature.VolumeSize(interior);
			sizes.push_back({ static_cast<std::uint32_t>((std::max)(size.Width, 0)), static_cast<std::uint32_t>((std::max)(size.Height, 0)),
				static_cast<std::uint32_t>((std::max)(size.Depth, 0)) });
		}
		return sizes;
	}

	/** @brief Registers when not yet registered (an input that was missing at Setup), logging what is missing when it changes. */
	bool EnsureRegistered(std::optional<VolumeSize> a_also = std::nullopt)
	{
		if (registration)
			return true;
		std::string missing;
		if (Register(SettingsSizes(a_also), missing)) {
			lastMissing.clear();
			return true;
		}
		if (missing != lastMissing) {
			logger::info("{}: not registered yet ({}); the volumetric lighting is skipped until it is", kLogPrefix, missing);
			lastMissing = missing;
		}
		return false;
	}

	/** @brief Registers again (the settings' sizes and a_size, or new engine targets), logged with a_why. */
	void RegisterAgain(std::string_view a_why, std::optional<VolumeSize> a_size = std::nullopt)
	{
		logger::info("{}: {}; the client registers again, and its graph is built with it at the next build point", kLogPrefix, a_why);
		registration.reset();
		EnsureRegistered(a_size);
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	// The engine's calls
	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	/** @brief The VL render's dispatches are withheld while it runs. */
	void BeginRender()
	{
		pending = {};
		inRender = true;
	}

	/** @brief The VL render returned: when it generated, its constants and the volume's size are this frame's work. */
	void EndRender()
	{
		inRender = false;
		if (!pending.generate)
			return;
		pending.size = DescriptorSize();
		pending.rangeFactor = DescriptorField<float>(0x2C);
		std::memcpy(pending.generateConstants.data(), GenerateConstants(), kGenerateBytes);
		pending.generateConstants[kGenerateRows] = { pending.rangeFactor != 0.0f ? 1.0f / pending.rangeFactor : 0.0f, 0.0f, 0.0f, 0.0f };
		// A size outside the registration (a settings change): registered again now, its frames skipped until the graph has it.
		if (registration && std::ranges::find(registration->sizes, pending.size) == registration->sizes.end())
			RegisterAgain(fmt::format("the volume size changed to {}x{}x{}", pending.size.width, pending.size.height, pending.size.depth), pending.size);
	}

	/** @brief ApplyVL's call (helper, previous target, output target): its values for the run. */
	std::uint32_t Apply(std::uint8_t* a_helper, std::uint32_t a_previous, std::uint32_t a_output)
	{
		// The helper's +0x8: the apply is on (the VL render sets it); +0x9: no temporal blend this frame, cleared by the apply.
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

	/** @brief The VL blur's call (source, destination, swap): the frame's work runs on the graph now. */
	void Blur(std::uint32_t a_source, std::uint32_t a_destination, std::uint32_t a_swap)
	{
		const auto clear = [&] { pending = {}; };
		if (!pending.output) {
			skipped.Report("the blur ran without an apply on the VL pair");
			return clear();
		}
		const std::uint32_t output = *pending.output;
		if (a_source != OutputOf(output) || a_destination != PreviousOf(output) || a_swap != kSwapTarget) {
			skipped.Report(fmt::format("the blur's targets are {}, {} and {}, not the apply's", a_source, a_destination, a_swap));
			return clear();
		}
		if (!pending.generate) {
			skipped.Report("the apply ran without the VL render's generate this frame");
			return clear();
		}
		if (!EnsureRegistered(pending.size))
			return clear();
		if (registration->engineObjects != EngineObjects() || registration->noiseGradientView != NoiseGradientView()) {
			RegisterAgain("the engine made a target or the noise gradient texture again");
			return clear();
		}
		const auto found = std::ranges::find(registration->sizes, pending.size);
		if (found == registration->sizes.end()) {
			skipped.Report("its volume size is not registered yet");
			return clear();
		}
		const auto volume = static_cast<std::uint32_t>(found - registration->sizes.begin());
		if (!FrameAnchors::CanRun(kClientId, kAnchor)) {
			skipped.Report("the anchor's graph does not hold the client yet, or its ring is full");
			return clear();
		}
		const bool noise = !noiseDone;
		for (const auto entry : { GenerateEntry(volume), RaymarchEntry(volume), ApplyEntry(volume, output), BlurHEntry(output), BlurVEntry(output) })
			if (!passMade[entry]) {
				skipped.Report("a pass was left out of the graph's build (logged above)");
				return clear();
			}
		if (noise && !passMade[kNoiseEntry]) {
			skipped.Report("the noise volume's pass was left out of the graph's build (logged above)");
			return clear();
		}

		VLRun next{ .volume = volume, .output = output, .size = pending.size, .noise = noise };
		auto& data = next.data;
		const auto& noiseSize = fixed->noiseSize;
		data.noise = { static_cast<float>(noiseSize.width), static_cast<float>(noiseSize.height), static_cast<float>(noiseSize.depth), 0.0f };
		data.raymarch = { static_cast<float>(pending.size.width), static_cast<float>(pending.size.height), static_cast<float>(pending.size.depth), 0.0f };
		data.blur = globals::features::volumetricLighting.BlurData();
		data.applyIntensityTemporal = { pending.intensity, pending.temporal, 0.0f, 0.0f };
		data.applyRepartition = { pending.rangeFactor, 0.0f, 0.0f, 0.0f };
		data.generate = pending.generateConstants;
		globals::features::volumetricLighting.SetGroupCountsHCS(next.blurHGroups[0], next.blurHGroups[1]);
		globals::features::volumetricLighting.SetGroupCountsVCS(next.blurVGroups[0], next.blurVGroups[1]);
		run = next;
		const auto result = FrameAnchors::Run(kAnchor);
		run.reset();
		if (!result)
			skipped.Report(fmt::format("the anchor's run did not submit (status {})", static_cast<std::uint32_t>(result.status)));
		clear();
	}

	/** @brief The run's commit: its values into the latch, and the gate open for this frame's passes. */
	void Commit()
	{
		if (!run)
			return;
		const auto& gate = *fixed->gate;
		fixed->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, run->data);
		const auto& size = run->size;
		if (run->noise) {
			const auto& noise = fixed->noiseSize;
			gate.OpenDispatch(kNoiseEntry, noise.width >> 5, noise.height >> 5, noise.depth);
		}
		// The engine's group counts (the VL render: 32x32 threads, one group per slice; the raymarch walks the slices itself).
		gate.OpenDispatch(GenerateEntry(run->volume), size.width >> 5, size.height >> 5, size.depth);
		gate.OpenDispatch(RaymarchEntry(run->volume), size.width >> 5, size.height >> 5, 1);
		gate.OpenDraw(ApplyEntry(run->volume, run->output));
		gate.OpenDispatch(BlurHEntry(run->output), run->blurHGroups[0], run->blurHGroups[1], 1);
		gate.OpenDispatch(BlurVEntry(run->output), run->blurVGroups[0], run->blurVGroups[1], 1);
	}

	/** @brief A run reached submission: the noise volume is made when it held the noise. */
	void Executed()
	{
		if (!run)
			return;
		if (run->noise && !noiseDone) {
			noiseDone = true;
			logger::info("{}: the noise volume ({}x{}x{}) is made on the graph", kLogPrefix, fixed->noiseSize.width, fixed->noiseSize.height, fixed->noiseSize.depth);
		}
		if (!std::exchange(announced, true))
			logger::info("{}: the first frame's volumetric lighting ran on the graph", kLogPrefix);
	}
};

ORGVolumetricLighting::~ORGVolumetricLighting() = default;

/** @brief The engine's VL render, apply and blur, taken on the ORG path. */
struct ORGVolumetricLightingDetours
{
	static ORGVolumetricLighting::Impl* Taking()
	{
		auto& self = ORGVolumetricLighting::Get();
		return self.IsActive() && FrameAnchors::IsRenderOwner() ? self.impl.get() : nullptr;
	}

	/**
	 * @brief The VL render (VolumetricLightingDescriptor::Render: SE 100306, AE 107023 at 0x1414c4f40, called by Main::Draw): its
	 * CPU work runs, its dispatches are withheld (TakeEngineDispatch). Its arguments (Ghidra: the descriptor, the camera, a byte)
	 * are integers; four are passed on.
	 */
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

	/** @brief ApplyVL (SE 100475, AE 107193): (the helper at ImageSpaceManager+0x170, the previous target, the output). */
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

	/** @brief The VL blur (SE 100486, AE 107206): (the helper at ImageSpaceManager+0x188, the source, the destination, the swap). */
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

void ORGVolumetricLighting::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(Anchor::VolumetricLighting))
		logger::warn("{} stays on D3D11: the VolumetricLighting anchor could not be reserved", kLogPrefix);
}

void ORGVolumetricLighting::Install()
{
	if (!Requested())
		return;
	stl::detour_thunk<ORGVolumetricLightingDetours::Render>(REL::RelocationID(100306, 107023));
	stl::detour_thunk<ORGVolumetricLightingDetours::Apply>(REL::RelocationID(100475, 107193));
	stl::detour_thunk<ORGVolumetricLightingDetours::Blur>(REL::RelocationID(100486, 107206));
	logger::info("{}: hooks installed (the VL render, ApplyVL, the VL blur)", kLogPrefix);
}

bool ORGVolumetricLighting::Setup()
{
	if (!Requested() || impl)
		return impl != nullptr;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	const auto off = [](std::string_view a_why) {
		logger::warn("{}: CS_ORG_VOLUMETRIC_LIGHTING=1, but {}; it stays on D3D11", kLogPrefix, a_why);
		return false;
	};
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(kAnchor))
		return off("the render graph or its VolumetricLighting anchor is not available");
	if (!globals::game::renderer)
		return off("there is no renderer");
	try {
		auto fixed = std::make_shared<VLFixed>();
		// The VL targets' format, for the apply's program.
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
			return off("the VL targets have no render target view, or two formats");

		// The programs, with the defines the D3D11 builds get (CSHADER for the generate's guard; the features' ImageSpace defines).
		auto defines = FeatureDefines();
		auto computeDefines = defines;
		computeDefines.emplace_back("CSHADER", "");
		const auto compute = [&](const char* a_source, std::uint32_t a_words) {
			return ComputeProgram::Load(host->Device(), { .source = a_source, .constantWords = a_words, .defines = AnchorPasses::Defines(computeDefines) });
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
		fixed->apply = RasterProgram::Load(host->Device(), apply);
		if (!fixed->noise || !fixed->generate || !fixed->raymarch || !fixed->blurH || !fixed->blurV || !fixed->apply)
			return off("a program could not be built (logged above)");

		fixed->latch = std::make_shared<org::LatchBlock>("cs.volumetric-lighting.latch", static_cast<std::uint32_t>(sizeof(LatchData)), host->FrameSlots());
		fixed->gate = RenderGraph::RunGate::Create("cs.volumetric-lighting.gate", kGateEntries);
		if (!fixed->gate)
			return off("its run gate could not be made");

		// The noise volume, as the VL render's first call makes the engine's (AE 107023: R8_UNORM, the INI's size), filled once by
		// ISPerlinNoiseCS on the graph.
		fixed->noiseSize = NoiseSize();
		if (!fixed->noiseSize)
			return off("the noise volume's INI size (iVolumetricLightingNoiseTexture*:Display) is not readable");
		D3D11_TEXTURE3D_DESC noise{};
		noise.Width = fixed->noiseSize.width;
		noise.Height = fixed->noiseSize.height;
		noise.Depth = fixed->noiseSize.depth;
		noise.MipLevels = 1;
		noise.Format = DXGI_FORMAT_R8_UNORM;
		noise.Usage = D3D11_USAGE_DEFAULT;
		noise.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		fixed->noiseVolume = std::make_unique<GraphResources::Texture3D>(noise, "VolumetricLighting::Noise", GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
		if (!*fixed->noiseVolume)
			return off("the noise volume could not be made");

		impl = std::make_unique<Impl>();
		impl->fixed = std::move(fixed);
		logger::info("{}: on (CS_ORG_VOLUMETRIC_LIGHTING); noise volume {}x{}x{}", kLogPrefix, impl->fixed->noiseSize.width, impl->fixed->noiseSize.height,
			impl->fixed->noiseSize.depth);
		impl->EnsureRegistered();
		return true;
	} catch (const std::exception& e) {
		impl.reset();
		return off(fmt::format("its setup threw ({})", e.what()));
	}
}

bool ORGVolumetricLighting::IsActive() const
{
	return impl && !impl->failed && RenderGraphRuntime::Get().IsActive();
}

bool ORGVolumetricLighting::TakeEngineDispatch(std::string_view a_computeShader)
{
	if (!impl || !impl->inRender)
		return false;
	if (a_computeShader == "ISVolumetricLightingGenerateCS")
		impl->pending.generate = true;
	else if (a_computeShader != "ISVolumetricLightingRaymarchCS")
		// The first call's noise: the graph makes its own noise volume.
		impl->engineDispatches.Report(fmt::format("{} (the noise volume is made on the graph)", a_computeShader.empty() ? "an unnamed shader" : a_computeShader));
	return true;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Volumetric Lighting always runs on D3D11.
struct ORGVolumetricLighting::Impl
{};

ORGVolumetricLighting::~ORGVolumetricLighting() = default;
void ORGVolumetricLighting::Reserve() {}
void ORGVolumetricLighting::Install()
{
	if (Requested())
		logger::warn("[ORG] Volumetric Lighting: CS_ORG_VOLUMETRIC_LIGHTING=1, but this build has no render graph");
}
bool ORGVolumetricLighting::Setup() { return false; }
bool ORGVolumetricLighting::IsActive() const { return false; }
bool ORGVolumetricLighting::TakeEngineDispatch(std::string_view) { return false; }

#endif
