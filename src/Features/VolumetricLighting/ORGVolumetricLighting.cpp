#include "ORGVolumetricLighting.h"

#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Switches.h"

bool ORGVolumetricLighting::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
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
#	include "Utils/Game.h"
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
	// The exterior's and the interior's volume sizes.
	constexpr std::uint32_t kMaxVolumes = 2;
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

	// Gate entries: noise, four per volume (generate, raymarch, apply into either output), the blurs by output, then the depth copy.
	constexpr std::uint32_t kNoiseEntry = 0;
	constexpr std::uint32_t GenerateEntry(std::uint32_t a_volume) { return 1 + 4 * a_volume; }
	constexpr std::uint32_t RaymarchEntry(std::uint32_t a_volume) { return 2 + 4 * a_volume; }
	constexpr std::uint32_t ApplyEntry(std::uint32_t a_volume, std::uint32_t a_output) { return 3 + 4 * a_volume + a_output; }
	constexpr std::uint32_t BlurHEntry(std::uint32_t a_output) { return 1 + 4 * kMaxVolumes + a_output; }
	constexpr std::uint32_t BlurVEntry(std::uint32_t a_output) { return 3 + 4 * kMaxVolumes + a_output; }
	constexpr std::uint32_t kDepthCopyEntry = 5 + 4 * kMaxVolumes;
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

	/** @brief With the switch and the feature on, or wherever a stage holds the anchor, since the engine's VL has no D3D11 path inside a stage. */
	bool Wanted()
	{
		return ORGVolumetricLighting::Requested() && (FeatureOn() || FrameAnchors::StageStepsAt(Anchor::VolumetricLighting));
	}

	/** @brief The dynamic resolution part of the screen and its last pixel. */
	std::array<std::int32_t, 4> BlurArea()
	{
		const float2 size = Util::ConvertToDynamic(float2{ static_cast<float>(globals::game::graphicsState->screenWidth),
			static_cast<float>(globals::game::graphicsState->screenHeight) });
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
		std::shared_ptr<const ComputeProgram> noise, generate, raymarch, blurH, blurV;
		std::shared_ptr<const RasterProgram> apply;
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

	struct VLSource
	{
		FrameAnchors::Texture resource;
		std::string published;

		explicit operator bool() const { return resource || !published.empty(); }
		AnchorPasses::Bind ToBind() const { return resource ? AnchorPasses::Bind::Srv(resource) : AnchorPasses::Bind::Published(published); }
	};

	/** @brief Immutable once registered. */
	struct VLRegistration
	{
		std::vector<VolumeSize> sizes;
		std::vector<std::unique_ptr<GraphResources::Texture3D>> volumes;
		FrameAnchors::Texture targets[3];
		FrameAnchors::Texture motionVectors, depth, previousDepth, shadowmap, shadowmapVL, noiseGradient;
		VLSource shadowHeights, cloudShadows;
		// A change means a resize, and the client registers again.
		std::vector<const void*> engineObjects;
		const ID3D11ShaderResourceView* noiseGradientView = nullptr;
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
	std::string lastMissing;  // logged when it changes
	VLPending pending;
	std::optional<VLRun> run;
	bool inRender = false;
	bool noiseDone = false;
	bool announced = false;
	// A published input not declarable yet leaves the generate out.
	std::array<bool, kGateEntries> passMade{};
	Util::ReasonLog skipped{ fmt::format("{}: a frame's volumetric lighting is skipped", kLogPrefix) };
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

	/** @brief In a fixed order, read from the renderer's tables. */
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
	* @brief The feature's published id, when it is on the graph. Empty otherwise, since a D3D11 import is not ordered after
	* its producer and an id nobody publishes would hold the generate back for good.
	*/
	static VLSource FeatureInput(bool a_loaded, bool a_orgPath, std::string_view a_id)
	{
		if (!a_loaded)
			return {};
		if (a_orgPath || PublishedResources::IsRegistered(a_id))
			return { nullptr, std::string(a_id) };
		return {};
	}

	/** @brief Null, logged, when it cannot be made. */
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

	/** @brief False with a_missing set when an input is not there yet. It is tried again at the next VL render. */
	bool Register(std::vector<VolumeSize> a_sizes, std::string& a_missing)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer) {
			a_missing = "no renderer";
			return false;
		}
		// The engine's current size first, each once, at most kMaxVolumes.
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
		constexpr std::array kTargets{ kVolumetricTarget, kPreviousTarget, kSwapTarget };
		for (std::size_t i = 0; i < kTargets.size(); ++i)
			made->targets[i] = FrameAnchors::EngineTarget(kTargets[i]);
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
			made->noiseGradient = texture ? FrameAnchors::ImmutableTexture("volumetric-lighting.noise-gradient", texture.get(), noiseGradient) : nullptr;
			made->noiseGradientView = noiseGradient;
		}
		if (!made->noiseGradient) {
			a_missing = "the sky's noise gradient texture is not loaded yet";
			return false;
		}
		auto& terrainShadows = globals::features::terrainShadows;
		auto& cloudShadows = globals::features::cloudShadows;
		made->shadowHeights = FeatureInput(!fixed->vanilla && terrainShadows.loaded, ORGTerrainShadows::Requested(), ORGTerrainShadows::kShadowHeightId);
		made->cloudShadows = FeatureInput(!fixed->vanilla && cloudShadows.loaded, ORGCloudShadows::Requested(), ORGCloudShadows::kOcclusionId);
		for (const auto& size : a_sizes) {
			D3D11_TEXTURE3D_DESC desc{};
			desc.Width = size.width;
			desc.Height = size.height;
			desc.Depth = size.depth;
			desc.MipLevels = 1;
			// The engine's accumulation volume format (AE 0x1414c5fa0).
			desc.Format = DXGI_FORMAT_R16_FLOAT;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			// Written by the generate and read by the apply in the same run, so transient.
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
			// Gated so the anchor can run in a frame phase. A copy skips at execution when its entry is closed.
			client.passes.push_back({ .name = fmt::format("{}.{}", kClientId, a_name),
				.anchor = kAnchor,
				.order = order++,
				.kind = a_kind,
				.make = std::move(a_make),
				.atExecution = a_kind == FrameAnchors::PassKind::Copy });
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
				// A published input not declarable yet leaves the generate out of this build.
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
		// Each volume's apply, then the blurs, so each frame's three steps are adjacent.
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
						// The whole target. The shader keeps the engine's dynamic-resolution part.
						draw.viewport = { .x = 0, .y = 0, .width = output.Width(), .height = output.Height() };
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
		// The apply's last D3D11 step copies the main depth into kMAIN_COPY, which next frame's apply reads at t6.
		add("depth-copy", FrameAnchors::PassKind::Copy, [this, shared] {
			return MakeStep(kDepthCopyEntry, "depth-copy", [&](AnchorPasses::Builder& a_builder) {
				a_builder.Add(AnchorPasses::Copy{ .name = "depth-copy",
					.source = shared->depth,
					.target = shared->previousDepth,
					.width = shared->depth.Width(),
					.height = shared->depth.Height(),
					.gate = kDepthCopyEntry });
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
		client.sharedConstants = true;
		client.gates = { fixed->gate };
		passMade.fill(false);
		if (!FrameAnchors::Register(std::move(client))) {
			a_missing = "the VolumetricLighting anchor refused the client";
			return false;
		}
		registration = std::move(made);
		std::string sizeList;
		for (const auto& size : registration->sizes)
			sizeList += fmt::format("{}{}x{}x{}", sizeList.empty() ? "" : ", ", size.width, size.height, size.depth);
		logger::info("{}: registered with volumes {} (Terrain Shadows {}, Cloud Shadows {}); the anchor's graph holds it from its next build", kLogPrefix, sizeList,
			registration->shadowHeights ? (registration->shadowHeights.published.empty() ? "imported" : "by its published id") : "off",
			registration->cloudShadows ? (registration->cloudShadows.published.empty() ? "imported" : "by its published id") : "off");
		return true;
	}

	/** @brief a_also first, then the exterior and interior sizes. With the feature off, the descriptor's size, which is the same everywhere. */
	std::vector<VolumeSize> SettingsSizes(std::optional<VolumeSize> a_also = std::nullopt) const
	{
		auto& feature = globals::features::volumetricLighting;
		std::vector<VolumeSize> sizes;
		if (a_also)
			sizes.push_back(*a_also);
		if (fixed->vanilla) {
			sizes.push_back(DescriptorSize());
			return sizes;
		}
		for (const bool interior : { false, true }) {
			const auto size = feature.VolumeSize(interior);
			sizes.push_back({ static_cast<std::uint32_t>((std::max)(size.Width, 0)), static_cast<std::uint32_t>((std::max)(size.Height, 0)),
				static_cast<std::uint32_t>((std::max)(size.Depth, 0)) });
		}
		return sizes;
	}

	/** @brief Logs what is missing when it changes. */
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

	void RegisterAgain(std::string_view a_why, std::optional<VolumeSize> a_size = std::nullopt)
	{
		logger::info("{}: {}; the client registers again, and its graph is built with it at the next build point", kLogPrefix, a_why);
		registration.reset();
		EnsureRegistered(a_size);
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
		// A new size registers again now, and its frames are skipped until the graph has it.
		if (registration && std::ranges::find(registration->sizes, pending.size) == registration->sizes.end())
			RegisterAgain(fmt::format("the volume size changed to {}x{}x{}", pending.size.width, pending.size.height, pending.size.depth), pending.size);
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

	/** @brief The frame's work runs on the graph now. */
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
		if (!FrameAnchors::Scheduled(kClientId, kAnchor)) {
			skipped.Report("the frame's schedule does not hold the client yet");
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
		run.reset();
		if (!result)
			skipped.Report(fmt::format("the anchor's run did not submit (status {})", static_cast<std::uint32_t>(result.status)));
		clear();
	}

	void Commit()
	{
		if (!run)
			return;
		const auto& gate = *fixed->gate;
		fixed->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, run->data);
		const auto& size = run->size;
		if (run->noise) {
			const auto& noise = fixed->noiseSize;
			gate.OpenDispatch(kNoiseEntry, noise.width >> 5, noise.height >> 5, noise.depth);
		}
		// The engine uses 32x32 threads and one group per slice. The raymarch walks the slices itself.
		gate.OpenDispatch(GenerateEntry(run->volume), size.width >> 5, size.height >> 5, size.depth);
		gate.OpenDispatch(RaymarchEntry(run->volume), size.width >> 5, size.height >> 5, 1);
		gate.OpenDraw(ApplyEntry(run->volume, run->output));
		gate.OpenDispatch(BlurHEntry(run->output), run->blurHGroups[0], run->blurHGroups[1], 1);
		gate.OpenDispatch(BlurVEntry(run->output), run->blurVGroups[0], run->blurVGroups[1], 1);
		gate.OpenDispatch(kDepthCopyEntry, 1);
	}

	/** @brief The noise volume counts as made once a run with it is submitted. */
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

void ORGVolumetricLighting::Reserve()
{
	if (Wanted() && !FrameAnchors::Reserve(Anchor::VolumetricLighting))
		logger::warn("{} stays on D3D11: the VolumetricLighting anchor could not be reserved", kLogPrefix);
}

void ORGVolumetricLighting::Install()
{
	if (!Wanted())
		return;
	stl::detour_thunk<ORGVolumetricLightingDetours::Render>(REL::RelocationID(100306, 107023));
	stl::detour_thunk<ORGVolumetricLightingDetours::Apply>(REL::RelocationID(100475, 107193));
	stl::detour_thunk<ORGVolumetricLightingDetours::Blur>(REL::RelocationID(100486, 107206));
	logger::info("{}: hooks installed (the VL render, ApplyVL, the VL blur)", kLogPrefix);
}

bool ORGVolumetricLighting::Setup()
{
	if (!Wanted() || impl)
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

		// Same defines as the D3D11 builds. With the feature off the engine's shaders get no feature define.
		fixed->vanilla = !FeatureOn();
		auto defines = fixed->vanilla ? std::vector<std::pair<std::string, std::string>>{} : FeatureDefines();
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

		fixed->latch = std::make_shared<org::LatchBlock>("cs.volumetric-lighting.latch", static_cast<std::uint32_t>(sizeof(LatchData)));
		fixed->gate = RenderGraph::RunGate::Create("cs.volumetric-lighting.gate", kGateEntries);
		if (!fixed->gate)
			return off("its run gate could not be made");

		// Made as the engine makes its own (AE 107023, R8_UNORM at the INI's size), filled once by ISPerlinNoiseCS.
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
		logger::info("{}: on (CS_ORG_VOLUMETRIC_LIGHTING{}); noise volume {}x{}x{}", kLogPrefix,
			impl->fixed->vanilla ? ", the feature off: the engine's volumetric lighting" : "", impl->fixed->noiseSize.width, impl->fixed->noiseSize.height,
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
	return impl && RenderGraphRuntime::Get().IsActive();
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

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph Volumetric Lighting always runs on D3D11.
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
