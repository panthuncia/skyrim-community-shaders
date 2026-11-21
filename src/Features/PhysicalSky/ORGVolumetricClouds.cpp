// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/PhysicalSky/ORGVolumetricClouds.h"

#include "Features/PhysicalSky/ORGCloudInputs.h"
#include "RenderGraph/Engine/Native/Native.h"

void ORGVolumetricClouds::SetInputSource(InputSource a_source) { inputs = std::move(a_source); }

#include "Buffer.h"
#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Features/PhysicalSky/ORGPhysicalSky.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainShadows/ORGTerrainShadows.h"
#include "Features/ViewAtmosphere.h"
#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/D3D11Wrapper.h"
#include "RenderGraph/Engine/Targets/TargetRef.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include <Render/RunReadback.h>
#include "RenderGraph/ShaderBindings.h"
#include "State.h"

// The handle table's names, in the shader's order (physical-sky-port.md, 6a).
#include "../../../features/Physical Sky/Shaders/PhysicalSky/CloudTableList.hlsli"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
	constexpr const char* kCloudsClientId = "cs.physical-sky.clouds";
	// After cs.physical-sky.cloud-maps (150), before cs.physical-sky (200), whose AP shadow and sky tail read the shadow volume.
	constexpr std::int32_t kCloudsOrder = 160;
	constexpr const char* kVolumetricsShader = "PhysicalSky/Volumetrics.cs.hlsl";
	constexpr const char* kMiddleBoundsShader = "PhysicalSky/MidCloudBounds.cs.hlsl";
	constexpr const char* kBoundaryShader = "PhysicalSky/CloudBoundary.hlsl";
	// Volumetrics.cs.hlsl's push block: the shared handles, the table, then ten latches.
	constexpr std::uint32_t kCloudWords = 4 + 2 + 10 * 2;
	// MidCloudBounds.cs.hlsl's: the weather map, the finer mip and the output mip.
	constexpr std::uint32_t kMiddleBoundsWords = 3;
	// CloudBoundary.hlsl's: the shared handles, the NDF height and modeling maps, the sampler and the parameters' latch.
	constexpr std::uint32_t kBoundaryWords = 4 + 1 + 1 + 1 + 2;

	/** @brief The handle table's slots. */
	namespace PhysSkyCloudSlot
	{
		enum Value : std::uint32_t
		{
#define PHYSSKY_CLOUD_SLOT(N) N,
			PHYSSKY_CLOUD_TABLE(PHYSSKY_CLOUD_SLOT)
#undef PHYSSKY_CLOUD_SLOT
			Count
		};
	}
	using Slot = PhysSkyCloudSlot::Value;

	/**
	* @brief The AP shadow's latch, as ORGPhysicalSky.cpp's ShadowLatch (PhysicalSky/ApShadow.hlsli): the light's bytes as
	* D3D11's structured buffer holds them, the light count and the clear flag.
	*/
	struct CloudShadowLight
	{
		Deferred::DirectionalShadowLightData light{};
		std::uint32_t lightCount = 0;
		std::uint32_t clear = 0;
		std::uint32_t pad[2]{};
	};
	static_assert(sizeof(CloudShadowLight) == 288 && offsetof(CloudShadowLight, lightCount) == 272, "PhysicalSky/ApShadow.hlsli's latch layout");

	// The latch: every per-frame value the cloud programs read, at fixed offsets (multiples of 16).
	constexpr std::uint32_t kMaxLatchStorms = 16;
	constexpr std::uint32_t kMaxLatchWeights = 64;
	// The voxel lighting array holds 32 slices per placement, so D3D11 allows at most 64.
	constexpr std::uint32_t kMaxLatchVoxels = D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION / 32u;
	constexpr std::uint32_t kCloudOffset = 0;
	constexpr std::uint32_t kStormRectOffset = kCloudOffset + sizeof(PhysicalSky::VolumetricCloudCB);
	constexpr std::uint32_t kVoxelRectOffset = kStormRectOffset + 16;
	// Never written, so it reads zeros, as D3D11's unbound b3 does for the passes that leave it unbound.
	constexpr std::uint32_t kZeroRectOffset = kVoxelRectOffset + 16;
	constexpr std::uint32_t kAmbientOffset = kZeroRectOffset + 16;
	constexpr std::uint32_t kVoxelUpdateOffset = kAmbientOffset + sizeof(PhysicalSky::CloudAmbientVolumeCB);
	constexpr std::uint32_t kStampOffset = kVoxelUpdateOffset + 16;
	constexpr std::uint32_t kShadowLightOffset = kStampOffset + sizeof(org::RunReadback::Stamp);
	constexpr std::uint32_t kBoundaryOffset = kShadowLightOffset + sizeof(CloudShadowLight);
	constexpr std::uint32_t kSingleStormOffset = kBoundaryOffset + sizeof(PhysicalSky::CloudBoundaryCB);
	constexpr std::uint32_t kStormsOffset = kSingleStormOffset + sizeof(CloudStormData);
	constexpr std::uint32_t kWeightsOffset = kStormsOffset + kMaxLatchStorms * sizeof(CloudStormData);
	constexpr std::uint32_t kVoxelsOffset = kWeightsOffset + kMaxLatchWeights * sizeof(float4);
	constexpr std::uint32_t kSegmentsOffset = kVoxelsOffset + kMaxLatchVoxels * sizeof(VoxelCloudData);
	constexpr std::uint32_t kLatchBytes = kSegmentsOffset + SkyLightning::kMaxSegments * sizeof(SkyLightning::Segment);
	static_assert(sizeof(PhysicalSky::VolumetricCloudCB) == 1328 && sizeof(CloudStormData) == 1824 && sizeof(VoxelCloudData) == 96 &&
					  sizeof(SkyLightning::Segment) == 32 && sizeof(float4) == 16,
		"Volumetrics.cs.hlsl's latch strides");
	static_assert(kStormRectOffset % 16 == 0 && kAmbientOffset % 16 == 0 && kShadowLightOffset % 16 == 0 && kBoundaryOffset % 16 == 0 &&
					  kSingleStormOffset % 16 == 0 && kStormsOffset % 16 == 0 && kWeightsOffset % 16 == 0 && kVoxelsOffset % 16 == 0 &&
					  kSegmentsOffset % 16 == 0,
		"latch regions start on 16-byte rows");

	/** @brief The Volumetrics.cs.hlsl programs, as CompileVolumetricShaders builds them. */
	enum class CloudProgram : std::uint32_t
	{
		HeightBounds,
		HeightBoundsReduce,
		AmbientSH,
		AmbientReduce,
		AmbientLighting,
		AmbientDensity,
		AmbientBoundary,
		AmbientSweep,
		AmbientSweepFar,
		AmbientProject,
		AmbientLayout,
		AmbientView,
		MainView,
		UpperSunDepth,
		UpperView,
		UpperTiles,
		UpperViewTiles,
		StormTiles,
		MainWithoutStorm,
		MainWeather,
		MainVolcanic,
		MainSingleVolcanic,
		CubeVolcanic,
		ShadowVolcanic,
		AmbientDensityVolcanic,
		VoxelView,
		VoxelViewSingle,
		VoxelLighting,
		CubeVoxel,
		ShadowVoxel,
		AmbientDensityVoxel,
		Filter,
		FilterWide,
		Reproject,
		CubeReproject,
		ShadowVolume,
		ShadowWeather,
		ShadowColumns,
		ShadowResample,
		ShadowMean,
		ShadowMeanWeather,
		ShadowMeanVolcanic,
		ShadowMeanVoxel,
		Cube,
		LightningQuery,
		LightningComposite,
		LightningCubeComposite,
		Count
	};
	constexpr auto kCloudProgramCount = static_cast<std::size_t>(CloudProgram::Count);

	struct CloudProgramDesc
	{
		const wchar_t* entry;
		std::vector<const wchar_t*> defines;
	};

	/** @brief Each program's entry and defines, in CloudProgram's order. */
	const std::array<CloudProgramDesc, kCloudProgramCount>& CloudProgramDescs()
	{
		static const std::array<CloudProgramDesc, kCloudProgramCount> descs{ {
			{ L"buildCloudHeightBounds", {} },
			{ L"reduceCloudHeightBounds", {} },
			{ L"buildCloudAmbientSH", {} },
			{ L"reduceCloudAmbientSH", {} },
			{ L"buildCloudAmbientLighting", {} },
			{ L"buildCloudAmbientDensity", {} },
			{ L"buildCloudAmbientBoundary", {} },
			{ L"sweepCloudAmbient", {} },
			{ L"sweepCloudAmbient", { L"CLOUD_AMBIENT_SWEEP_THREADS=16" } },
			{ L"projectCloudAmbient", {} },
			{ L"updateCloudAmbientLayout", {} },
			{ L"resolveCloudAmbientView", {} },
			{ L"main", { L"CLOUD_AP_SHADOW", L"CLOUD_MAIN_DISPATCH" } },
			{ L"buildUpperSunDepth", { L"NO_CLOUD_STORMS" } },
			{ L"renderUpperView", { L"NO_CLOUD_STORMS" } },
			{ L"classifyUpperTiles", { L"NO_CLOUD_STORMS" } },
			{ L"renderUpperTiles", { L"NO_CLOUD_STORMS" } },
			{ L"classifyStormTiles", {} },
			{ L"main", { L"CLOUD_AP_SHADOW", L"CLOUD_MAIN_DISPATCH", L"NO_CLOUD_STORMS" } },
			{ L"main", { L"CLOUD_AP_SHADOW", L"CLOUD_MAIN_DISPATCH", L"NO_CLOUD_STORMS", L"NO_LOCAL_NDF" } },
			{ L"main", { L"CLOUD_AP_SHADOW", L"CLOUD_MAIN_DISPATCH", L"VOLCANIC_STORMS_ONLY" } },
			{ L"main", { L"CLOUD_AP_SHADOW", L"CLOUD_MAIN_DISPATCH", L"VOLCANIC_STORMS_ONLY", L"SINGLE_CLOUD_STORM" } },
			{ L"renderCubemap", { L"CLOUD_REFLECTION", L"VOLCANIC_STORMS_ONLY" } },
			{ L"renderShadowVolume", { L"VOLCANIC_STORMS_ONLY" } },
			{ L"buildCloudAmbientDensity", { L"VOLCANIC_STORMS_ONLY" } },
			{ L"renderVoxelView", { L"CLOUD_AP_SHADOW", L"CLOUD_VOXELS", L"CLOUD_VOXEL_VIEW", L"NO_CLOUD_STORMS" } },
			{ L"renderVoxelView", { L"CLOUD_AP_SHADOW", L"CLOUD_VOXELS", L"CLOUD_VOXEL_SINGLE", L"CLOUD_VOXEL_VIEW", L"NO_CLOUD_STORMS" } },
			{ L"buildVoxelLighting", { L"CLOUD_VOXELS" } },
			{ L"renderCubemap", { L"CLOUD_REFLECTION", L"CLOUD_VOXELS", L"CLOUD_VOXEL_COARSE" } },
			{ L"renderShadowVolume", { L"CLOUD_VOXELS" } },
			{ L"buildCloudAmbientDensity", { L"CLOUD_VOXELS" } },
			{ L"filterCloud", {} },
			{ L"filterCloud", { L"CLOUD_BLUR_RADIUS=2" } },
			{ L"reproject", {} },
			{ L"reprojectCubemap", {} },
			{ L"renderShadowVolume", {} },
			{ L"renderShadowVolume", { L"NO_CLOUD_STORMS" } },
			{ L"renderShadowColumns", { L"NO_CLOUD_STORMS" } },
			{ L"resampleShadowVolume", {} },
			{ L"buildCloudShadowMean", {} },
			{ L"buildCloudShadowMean", { L"NO_CLOUD_STORMS" } },
			{ L"buildCloudShadowMean", { L"VOLCANIC_STORMS_ONLY" } },
			{ L"buildCloudShadowMean", { L"CLOUD_VOXELS" } },
			{ L"renderCubemap", { L"CLOUD_REFLECTION" } },
			{ L"findLightningSource", { L"CLOUD_VOXELS" } },
			{ L"compositeLightning", { L"CLOUD_VOXELS" } },
			{ L"compositeLightningCube", { L"CLOUD_VOXELS" } },
		} };
		return descs;
	}

	/** @brief The run gate's entries: every dispatch, draw, copy group and clear group the D3D11 passes may issue. */
	enum CloudEntry : std::uint32_t
	{
		kNeutral,  // the neutral clears of a fresh start or of ClearVolumetricToNeutral
		// The shadow volume pass.
		kShadowVoxel,
		kShadowColumns,
		kShadowWeather,
		kShadowVolcanic,
		kShadowVolume,
		kShadowResample,
		kShadowMeanVoxel,
		kShadowMeanWeather,
		kShadowMeanVolcanic,
		kShadowMean,
		// The main view and cubemap pass.
		kVoxelLighting,
		kAmbientSH,
		kAmbientReduce,
		kAmbientLighting,
		kMiddleBounds,  // and the next kMiddleBoundsLevels - 1 entries, one per mip
		kUpperSunDepth = kMiddleBounds + PhysicalSky::kMiddleBoundsLevels,
		kHeightBounds,
		kHeightBoundsReduce,
		kAmbientReset,
		kAmbientBoundary,
		kAmbientDensityVoxel,
		kAmbientDensityVolcanic,
		kAmbientDensity,
		kAmbientSweep,
		kAmbientSweepFar,
		kAmbientProject,                                                                // and one more per cascade
		kAmbientSourceCopy = kAmbientProject + PhysicalSky::kCloudAmbientCascadeCount,  // the same
		kAmbientLayoutIn = kAmbientSourceCopy + PhysicalSky::kCloudAmbientCascadeCount,
		kAmbientLayout,
		kAmbientLayoutOut,
		kAmbientView,
		kLightningQuery,
		kBoundary,
		kStormTiles,
		kMainWeather,
		kMainWeatherTiles,
		kMainWithoutStorm,
		kMainWithoutStormTiles,
		kMainStorm,
		kMainVolcanic,
		kMainSingleVolcanic,
		kVoxelView,
		kVoxelViewSingle,
		kVoxelCopy,
		kUpperTiles,
		kUpperViewTiles,
		kUpperView,
		kUpperCopy,
		kReproject,
		kFilter,
		kFilterWide,
		kFilterCopy,
		kMapMenuClear,
		kLightningComposite,
		kCubeVoxel,
		kCubeVolcanic,
		kCube,
		kCubeReproject,
		kLightningCube,
		kHistories,
		kEntryCount
	};

	constexpr float KilometersToGameUnits(float a_value) { return a_value / Util::Units::GAME_UNIT_TO_KM; }

	/** @brief A full-resolution target: its declared twin and the feature's D3D11 wrapper, which follows the bound version. */
	struct CloudScreenTarget
	{
		RenderGraph::Frame::DeclaredTexture2D twin;
		GraphResources::D3D11Wrapper<::Texture2D> wrapper;  // the object is owned by the feature's member
	};

	/** @brief The 2D and 3D twins, their wrappers' registrations, and each twin by the feature's wrapper object. Render thread. */
	struct CloudTwins
	{
		std::map<std::string, std::unique_ptr<GraphResources::Texture2D>> textures2D;
		std::map<std::string, std::unique_ptr<GraphResources::Texture3D>> textures3D;
		std::map<std::string, GraphResources::D3D11Wrapper<::Texture2D>> wrappers2D;
		std::map<std::string, GraphResources::D3D11Wrapper<::Texture3D>> wrappers3D;
		std::map<std::string, CloudScreenTarget> screens;
		// The wrapper object stays the same across versions, so it names its twin for the session.
		std::map<const void*, const GraphResources::Resource*> byWrapper;
		std::uint64_t generation = 0;  // bumped by every new twin, which registers the client again
	};

	CloudTwins& Twins()
	{
		static CloudTwins s_twins;
		return s_twins;
	}

	/**
	* @brief Which engine depth target a view is, or UINT32_MAX. A field handle is identified by the target set. The view Deferred
	* captured may be DXVK's own object, which the set cannot identify, so it is compared by what the fields wrap.
	*/
	std::uint32_t CloudEngineDepthOf(ID3D11ShaderResourceView* a_view)
	{
		namespace Tripwire = RenderGraph::Engine::Tripwire;
		namespace Targets = RenderGraph::Engine::Targets;
		if (!a_view || !globals::game::renderer)
			return UINT32_MAX;
		if (Tripwire::IsHandle(a_view)) {
			const auto target = Targets::Identify(static_cast<const ID3D11View*>(a_view));
			return target && target->kind == Targets::Kind::Depth ? target->index : UINT32_MAX;
		}
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		if (!resource)
			return UINT32_MAX;
		const auto& depthStencils = globals::game::renderer->GetDepthStencilData().depthStencils;
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); ++i)
			if (depthStencils[i].texture && Tripwire::Unwrap(depthStencils[i].texture) == Tripwire::Unwrap(static_cast<ID3D11Texture2D*>(resource.get())))
				return i;
		return UINT32_MAX;
	}

	/** @brief A directional shadow map view that is no engine target, imported as its own. */
	FrameAnchors::Texture CloudImportShadowMap(ID3D11ShaderResourceView* a_view)
	{
		if (!a_view)
			return nullptr;
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture)
			return nullptr;
		RE::BSGraphics::DepthStencilData target{};
		target.texture = texture.get();
		target.depthSRV = a_view;
		return FrameAnchors::ImportTexture("cs.physical-sky.clouds.directional-shadow-map", { .depth = &target });
	}

	/** @brief What a registration is made from. A change registers the client again. */
	struct CloudRegistrationKey
	{
		ORGCloudInputs::Inputs inputs;
		FrameAnchors::Texture shadowMap;  // null when the frame has no directional shadow map
		std::string terrain;              // Terrain Shadows' published heights, empty for none
		std::string cloudLayer;           // Cloud Shadows' last layer, published, empty for none
		std::uint64_t twins = 0;

		bool operator==(const CloudRegistrationKey&) const = default;
	};

	/**
	* @brief What the last Stage found that is no variable yet (B2's interim): the registration key without its shadow map, and
	* the shadow map as an engine depth target or another view. The client's key imports the shadow map from it.
	*/
	struct CloudSeen
	{
		CloudRegistrationKey key;                               // shadowMap left empty
		std::uint32_t shadowDepth = UINT32_MAX;                 // the engine depth target, UINT32_MAX for none
		winrt::com_ptr<ID3D11ShaderResourceView> shadowOther;  // a view that is no engine target

		bool operator==(const CloudSeen& a_other) const
		{
			return key == a_other.key && shadowDepth == a_other.shadowDepth && shadowOther.get() == a_other.shadowOther.get();
		}

		/** @brief The registration key, with the shadow map imported. The engine depth's slot never changes. */
		CloudRegistrationKey Resolved() const
		{
			auto resolved = key;
			if (shadowDepth != UINT32_MAX)
				resolved.shadowMap = FrameAnchors::EngineTexture(FrameAnchors::Depth(shadowDepth));
			else if (shadowOther)
				resolved.shadowMap = CloudImportShadowMap(shadowOther.get());
			return resolved;
		}
	};

	/** @brief The client's key: what the last Stage found, and the screen targets' extent the copies bake in. */
	struct CloudClientKey
	{
		CloudRegistrationKey observed;
		std::array<std::uint32_t, 2> screen{};

		bool operator==(const CloudClientKey&) const = default;
	};

	/** @brief The screen targets' extent for a revision's values: the display size while the sky and its clouds are on, else one texel. */
	std::array<std::uint32_t, 2> CloudScreenExtent(const RenderGraph::Frame::Inputs& a_in)
	{
		const auto& settings = globals::features::physicalSky.settings;
		if (!a_in.Get(settings.enabled) || !a_in.Get(settings.enableVolumetricClouds))
			return { 1, 1 };
		const auto display = a_in.Get(RenderGraph::Frame::Vars::DisplaySize());
		return { display.width, display.height };
	}

	/** @brief The handle table as D3D11's bindings stand at one point of the passes: each slot's entry, or the null descriptor. */
	class CloudTable
	{
	public:
		using Bind = RenderGraph::AnchorPasses::Bind;

		explicit CloudTable(Bind a_null) :
			null(a_null) { entries.fill(a_null); }

		void Set(Slot a_slot, Bind a_bind) { entries[a_slot] = std::move(a_bind); }
		void Reset(Slot a_slot) { entries[a_slot] = null; }

		/**
		* @brief The table for one dispatch writing a_outputs. A read of a resource the dispatch writes is left out, as D3D11
		* unbinds an SRV whose resource is bound as a UAV.
		*/
		std::vector<Bind> With(std::initializer_list<std::pair<Slot, Bind>> a_outputs) const
		{
			std::vector<Bind> table(entries.begin(), entries.end());
			for (const auto& [slot, bind] : a_outputs)
				table[slot] = bind;
			for (auto& bind : table) {
				if (bind.kind != Bind::Kind::Srv)
					continue;
				const bool written = std::ranges::any_of(a_outputs, [&](const auto& a_output) {
					const auto& output = a_output.second;
					return (bind.resource && bind.resource == output.resource) || (bind.slot && bind.slot == output.slot);
				});
				if (written)
					bind = null;
			}
			return table;
		}

	private:
		Bind null;
		std::array<Bind, Slot::Count> entries;
	};
}

struct ORGVolumetricClouds::Impl
{
	// Queued at setup.
	std::array<RenderGraph::Programs::Fixed<ComputeProgram>, kCloudProgramCount> programs;
	RenderGraph::Programs::Fixed<ComputeProgram> middleBounds, middleBoundsReduce;
	RenderGraph::Programs::Fixed<RasterProgram> boundary;
	std::shared_ptr<RenderGraph::RunGate> gate;
	std::shared_ptr<org::LatchBlock> latch;
	org::OwnedDescriptorBinding tileable;
	std::shared_ptr<org::RunReadback> readback;
	std::unique_ptr<GraphResources::StructuredBuffer> candidates;

	// The null descriptor, for what D3D11 leaves unbound: it reads zero, as an unbound register does.
	std::uint32_t nullIndex = DCLF::GpuTextures::kInvalid;
	std::shared_ptr<const void> nullOwner;
	// SampTr and SampSv, ORGPhysicalSky's.
	std::uint32_t samplerTr = ORGPhysicalSky::kNoSampler, samplerSv = ORGPhysicalSky::kNoSampler;
	std::shared_ptr<const void> samplerTrOwner, samplerSvOwner;

	// The declared client, and what its key adds: the inputs the last Stage found. A change is an event, and the next revision
	// registers the client for it. Each registration keeps its key. While the adopted revision holds one (Adopted()), the frame
	// renders the clouds.
	RenderGraph::Frame::DeclaredClientOf<const CloudClientKey> client;
	RenderGraph::Frame::Observed<CloudSeen> observed{ "PhysicalSky.Clouds" };

	// Render thread. Staged by Stage, taken by the next commit.
	struct Pending
	{
		std::vector<std::byte> latch = std::vector<std::byte>(kLatchBytes);
		std::vector<std::pair<std::uint32_t, std::array<std::uint32_t, 3>>> dispatches;
		bool boundary = false;
		bool query = false;
	};
	std::optional<Pending> pending;
	std::uint32_t pendingFrame = UINT32_MAX;  // the frame Stage staged pending for
	// The next commit opens the neutral clears (ClearVolumetricToNeutral, a declared reset). On at first: the feature asks for
	// them at setup (EnsureVolumetricScreenResources), before Setup makes this. The plain twins get their first contents so.
	bool neutral = true;
	bool neutralCommitted = false;  // a commit opened them, and its run has not executed yet
	// The screen targets' versions the last commit read, in Twins().screens' order. Its run's executed callback marks them written.
	std::vector<const void*> committedScreens;
	std::optional<std::uint32_t> queryStamp;  // the stamp of the run that last dispatched the lightning query

	void Open(Pending& a_pending, std::uint32_t a_entry, std::uint32_t a_x, std::uint32_t a_y = 1, std::uint32_t a_z = 1)
	{
		a_pending.dispatches.push_back({ a_entry, { a_x, a_y, a_z } });
	}

	template <class T>
	static void Put(Pending& a_pending, std::uint32_t a_offset, const T& a_value)
	{
		std::memcpy(a_pending.latch.data() + a_offset, &a_value, sizeof(T));
	}

	/** @brief Whether this frame staged work: a frame that did not stage leaves the clouds as they are. */
	bool Active() const { return pending.has_value() && pendingFrame == globals::state->frameCount; }

	void Commit()
	{
		if (!Active()) {
			pending.reset();
			return;
		}
		auto& frame = *pending;
		const auto run = RenderGraphRuntime::Get().Host()->CurrentRun();
		const org::RunReadback::Stamp stamp{ .value = readback->Commit(run) };
		Put(frame, kStampOffset, stamp);
		latch->Write(run, 0, std::span<const std::byte>(frame.latch));
		if (neutral) {
			gate->OpenDispatch(kNeutral, 1);
			neutralCommitted = true;
		}
		// The bound screen versions this run writes. OutputsWritten reads them once it was submitted.
		const auto& adopted = RenderGraph::Frame::Revision::Adopted();
		committedScreens.clear();
		for (const auto& [_, screen] : Twins().screens)
			committedScreens.push_back(adopted.Version(screen.twin));
		for (const auto& [entry, groups] : frame.dispatches)
			gate->OpenDispatch(entry, groups[0], groups[1], groups[2]);
		if (frame.boundary)
			gate->OpenDraw(kBoundary, PhysicalSky::kCloudBoundaryCells * PhysicalSky::kCloudBoundaryCells * 6u, 2);
		if (frame.query)
			queryStamp = stamp.value;
		pending.reset();
	}

	void Executed()
	{
		readback->Executed();
		if (std::exchange(neutralCommitted, false))
			neutral = false;
		std::size_t i = 0;
		for (const auto& [_, screen] : Twins().screens) {
			if (i < committedScreens.size() && committedScreens[i])
				RenderGraph::Frame::NoteWritten(screen.twin, committedScreens[i]);
			++i;
		}
		committedScreens.clear();
	}

	/** @brief The null descriptor and ORGPhysicalSky's samplers. False, logged, when a sampler is missing. */
	bool MakeHandles()
	{
		if (nullIndex == DCLF::GpuTextures::kInvalid) {
			auto binding = DCLF::GpuTextures::Get().NullBinding();
			if (!binding)
				Fatal("the null descriptor could not be made");
			nullIndex = binding.index;
			nullOwner = std::move(binding.owner);
		}
		auto& sky = ORGPhysicalSky::Get();
		samplerTr = sky.SamplerIndex(ORGPhysicalSky::Sampler::Transmittance, &samplerTrOwner);
		samplerSv = sky.SamplerIndex(ORGPhysicalSky::Sampler::SkyView, &samplerSvOwner);
		if (samplerTr == ORGPhysicalSky::kNoSampler || samplerSv == ORGPhysicalSky::kNoSampler) {
			logger::error("[ORG] Physical Sky clouds: the LUTs are not on the render graph (no samplers); no clouds");
			return false;
		}
		return true;
	}

	/** @brief The client's registration for a key, when a revision starts. False, logged, when it cannot be made (fatal). */
	bool Make(const PhysicalSky& a_feature, const CloudClientKey& a_key, const std::shared_ptr<Impl>& a_self, FrameAnchors::ClientDesc& a_client);
};

namespace
{
	/** @brief The twin a D3D11 member names, or null when it is a D3D11 texture. */
	template <class Wrapper>
	const GraphResources::Resource* CloudTwinOf(const eastl::unique_ptr<Wrapper>& a_member)
	{
		if (!a_member)
			return nullptr;
		const auto& twins = Twins().byWrapper;
		const auto found = twins.find(static_cast<const void*>(a_member.get()));
		return found == twins.end() ? nullptr : found->second;
	}
}

bool ORGVolumetricClouds::Impl::Make(const PhysicalSky& a_feature, const CloudClientKey& a_clientKey, const std::shared_ptr<Impl>& a_self,
	FrameAnchors::ClientDesc& a_client)
{
	namespace AnchorPasses = RenderGraph::AnchorPasses;
	using Bind = AnchorPasses::Bind;
	if (!MakeHandles())
		return false;
	const auto& f = a_feature;
	const auto& a_key = a_clientKey.observed;
	const auto& in = a_key.inputs;
	AnchorPasses::Builder builder(FrameAnchors::Anchor::Prepass, kCloudsClientId, kCloudsOrder, latch, gate);
	const auto null = Bind::Handle(nullIndex);

	// A member's twin as a graph handle. A member without one fails the builder.
	const auto tex = [&](const auto& a_member, const char* a_name) -> FrameAnchors::Texture {
		const auto* twin = CloudTwinOf(a_member);
		if (!twin) {
			builder.Fail(std::format("{} is not a graph twin", a_name));
			return {};
		}
		return AnchorPasses::TextureOf(*twin);
	};
#define CLOUD_TEX(M) tex(f.M, #M)
	const auto srv = [&](const FrameAnchors::Texture& a_texture) { return a_texture ? Bind::Srv(a_texture) : null; };
	const auto srvCube = [&](const FrameAnchors::Texture& a_texture) { return a_texture ? Bind::SrvCube(a_texture) : null; };
	const auto uavArray = [&](const FrameAnchors::Texture& a_texture) { return a_texture ? Bind::UavArray(a_texture) : null; };
	const auto srvArray = [&](const FrameAnchors::Texture& a_texture) { return a_texture ? Bind::SrvArray(a_texture) : null; };

	// The cloud targets.
	const auto shadowVolume = CLOUD_TEX(texShadowVolume), shadowRaw = CLOUD_TEX(texShadowVolumeRaw), shadowMean = CLOUD_TEX(texCloudShadowMean);
	const auto ambientSH = CLOUD_TEX(texVolCloudAmbientSH), ambientPartial = CLOUD_TEX(texCloudAmbientPartial);
	const auto ambientLighting = CLOUD_TEX(texCloudAmbientLighting), ambientBoundary = CLOUD_TEX(texCloudAmbientBoundary);
	const auto ambientDensity = CLOUD_TEX(texCloudAmbientDensity), ambientSweep = CLOUD_TEX(texCloudAmbientSweep);
	const std::array ambientLayout{ CLOUD_TEX(texCloudAmbientLayout[0]), CLOUD_TEX(texCloudAmbientLayout[1]) };
	std::array<std::array<FrameAnchors::Texture, 2>, PhysicalSky::kCloudAmbientCascadeCount> ambientSource;
	std::array<FrameAnchors::Texture, PhysicalSky::kCloudAmbientCascadeCount> ambientView;
	for (std::uint32_t i = 0; i < PhysicalSky::kCloudAmbientCascadeCount; ++i) {
		ambientSource[i] = { tex(f.texCloudAmbientSource[i][0], "texCloudAmbientSource"), tex(f.texCloudAmbientSource[i][1], "texCloudAmbientSource") };
		ambientView[i] = tex(f.texCloudAmbientView[i], "texCloudAmbientView");
	}
	const auto heightBounds = CLOUD_TEX(texCloudHeightBounds), heightTiles = CLOUD_TEX(texCloudHeightBoundsTiles);
	const auto weatherBounds = CLOUD_TEX(texCloudWeatherBounds);
	const auto middleBoundsTex = CLOUD_TEX(texMiddleBounds), upperSunDepth = CLOUD_TEX(texUpperSunDepth);
	const auto boundaryTarget = CLOUD_TEX(texCloudBoundary), stormTiles = CLOUD_TEX(texStormTiles), upperTiles = CLOUD_TEX(texUpperTiles);
	const auto lowTr = CLOUD_TEX(texVolLowTr), lowLum = CLOUD_TEX(texVolLowLum), lowAux = CLOUD_TEX(texVolLowAux);
	const auto lightningTrace = CLOUD_TEX(texLightningTrace), stormMotion = CLOUD_TEX(texVolStormMotion);
	const auto voxelTr = CLOUD_TEX(texVoxelTraceTr), voxelLum = CLOUD_TEX(texVoxelTraceLum), voxelAux = CLOUD_TEX(texVoxelTraceAux);
	const auto voxelMotion = CLOUD_TEX(texVoxelTraceMotion), voxelLightning = CLOUD_TEX(texVoxelTraceLightning);
	const auto volTr = CLOUD_TEX(texVolTr), volLum = CLOUD_TEX(texVolLum), volAux = CLOUD_TEX(texVolAux);
	const auto historyTr = CLOUD_TEX(texVolHistoryTr), historyLum = CLOUD_TEX(texVolHistoryLum), historyAux = CLOUD_TEX(texVolHistoryAux);
	const auto filteredTr = CLOUD_TEX(texVolFilteredTr), filteredLum = CLOUD_TEX(texVolFilteredLum), filteredAux = CLOUD_TEX(texVolFilteredAux);
	const auto lightningMask = CLOUD_TEX(texLightningMask), lightningHistory = CLOUD_TEX(texLightningHistory);
	const auto lightningChannels = CLOUD_TEX(texLightningChannels);
	const auto cubeTr = CLOUD_TEX(texVolCubeTr), cubeLum = CLOUD_TEX(texVolCubeLum), cubeAux = CLOUD_TEX(texVolCubeAux);
	const auto cubeHistoryTr = CLOUD_TEX(texVolCubeHistoryTr), cubeHistoryLum = CLOUD_TEX(texVolCubeHistoryLum);
	const auto cubeHistoryAux = CLOUD_TEX(texVolCubeHistoryAux);
	const auto cubeTraceTr = CLOUD_TEX(texVolCubeTraceTr), cubeTraceLum = CLOUD_TEX(texVolCubeTraceLum), cubeTraceAux = CLOUD_TEX(texVolCubeTraceAux);
	const auto lightningCubeTrace = CLOUD_TEX(texLightningCubeTrace), lightningCubeMask = CLOUD_TEX(texLightningCubeMask);
	const auto lightningCubeHistory = CLOUD_TEX(texLightningCubeHistory), lightningCubeLum = CLOUD_TEX(texLightningCubeLum);
	const auto cubeStormMotion = CLOUD_TEX(texVolCubeStormMotion);
#undef CLOUD_TEX
	const auto depth = FrameAnchors::EngineTexture(FrameAnchors::Depth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY));
	const auto groundReflection = FrameAnchors::EngineTexture(FrameAnchors::Cubemap(RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS));
	if (!depth || !groundReflection)
		builder.Fail("the depth copy or the reflection cubemap cannot be imported");
	if (builder.Failed()) {
		logger::error("[ORG] Physical Sky clouds: {}; no clouds", builder.Failure());
		return false;
	}

	// The push block: the shared handles, the table, then the latches in Volumetrics.cs.hlsl's order.
	const auto push = [](std::uint32_t a_dispatchRect) {
		return std::vector<Bind>{ Bind::Shared(), Bind::Table(), Bind::Latch(kCloudOffset), Bind::Latch(a_dispatchRect), Bind::Latch(kAmbientOffset),
			Bind::Latch(kSingleStormOffset), Bind::Latch(kVoxelUpdateOffset), Bind::Latch(kShadowLightOffset), Bind::Latch(kStormsOffset),
			Bind::Latch(kWeightsOffset), Bind::Latch(kVoxelsOffset), Bind::Latch(kSegmentsOffset) };
	};
	const auto program = [&](CloudProgram a_program) { return programs[static_cast<std::size_t>(a_program)].Get(); };
	const auto dispatch = [&](std::string a_name, CloudProgram a_program, std::uint32_t a_entry, std::vector<Bind> a_table,
							  std::uint32_t a_rect = kZeroRectOffset) {
		builder.Add(AnchorPasses::Dispatch{ .name = std::move(a_name), .program = program(a_program), .bindings = push(a_rect), .gate = a_entry, .table = std::move(a_table) });
	};
	const auto gatedClear = [&](std::string a_name, const FrameAnchors::Texture& a_target, const float (&a_value)[4], std::uint32_t a_entry) {
		const std::array<float, 4> value{ a_value[0], a_value[1], a_value[2], a_value[3] };
		builder.Add(AnchorPasses::Clear{ .name = std::move(a_name), .target = a_target, .value = value, .gate = a_entry });
	};
	// A gated copy of a whole texture of the twin's shape.
	const auto copy = [&](std::string a_name, const FrameAnchors::Texture& a_source, const FrameAnchors::Texture& a_target, std::uint32_t a_entry,
						  std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_depth = 1, std::uint32_t a_slices = 1) {
		builder.Add(AnchorPasses::Copy{ .name = std::move(a_name), .source = a_source, .target = a_target, .width = a_width, .height = a_height, .gate = a_entry, .depth = a_depth, .slices = a_slices });
	};
	const auto size2D = [](const eastl::unique_ptr<Texture2D>& a_member) { return std::array<std::uint32_t, 2>{ a_member->desc.Width, a_member->desc.Height }; };
	const auto copy2D = [&](std::string a_name, const FrameAnchors::Texture& a_source, const FrameAnchors::Texture& a_target, std::uint32_t a_entry,
							const eastl::unique_ptr<Texture2D>& a_shape) {
		const auto size = size2D(a_shape);
		copy(std::move(a_name), a_source, a_target, a_entry, size[0], size[1], 1, a_shape->desc.ArraySize);
	};
	// A copy between two screen targets, at the extent this key's revision binds. Their wrappers still hold the adopted one.
	const auto copyScreen = [&](std::string a_name, const FrameAnchors::Texture& a_source, const FrameAnchors::Texture& a_target, std::uint32_t a_entry) {
		copy(std::move(a_name), a_source, a_target, a_entry, a_clientKey.screen[0], a_clientKey.screen[1]);
	};

	// The neutral clears (ClearVolumetricToNeutral and SetupVolumetricResources' clears), before everything else of the run.
	{
		std::uint32_t i = 0;
		for (const auto& target : { volTr, lowTr, historyTr, filteredTr, cubeTr })
			gatedClear(std::format("neutral.tr{}", i++), target, PhysicalSky::kClearTransmittance, kNeutral);
		i = 0;
		for (const auto& target : { volLum, volAux, lowLum, lowAux, historyLum, historyAux, filteredLum, filteredAux, cubeLum, cubeAux, lightningMask,
				 lightningHistory, lightningChannels, lightningTrace, lightningCubeTrace, lightningCubeMask, lightningCubeHistory, lightningCubeLum, shadowMean })
			gatedClear(std::format("neutral.lum{}", i++), target, PhysicalSky::kClearLuminance, kNeutral);
		gatedClear("neutral.shadow-volume", shadowVolume, PhysicalSky::kClearShadowVolume, kNeutral);
		gatedClear("neutral.shadow-raw", shadowRaw, PhysicalSky::kClearShadowVolume, kNeutral);
	}

	// What D3D11 binds for both passes (RenderVolumetricClouds' srvs, samplers, the managers' maps and the voxel bindings).
	CloudTable base(null);
	base.Set(Slot::TexTransmittance, Bind::Published(ORGPhysicalSky::kTransmittanceLutId));
	base.Set(Slot::TexAerialPerspective, Bind::Published(ORGPhysicalSky::kApLutId));
	base.Set(Slot::TexDepth, Bind::Srv(depth));
	base.Set(Slot::TexCloudShapeNoise, srv(in.shapeNoise));
	base.Set(Slot::TexCloudHeight, srv(in.ndfHeight));
	base.Set(Slot::TexCloudModeling, srv(in.ndfModeling));
	base.Set(Slot::TexCloudAtmosphereMs, Bind::Published(ORGPhysicalSky::kMultiscatterLutId));
	base.Set(Slot::TexCirrusWeather, srv(in.cirrusWeather));
	base.Set(Slot::TexCirrusPatterns, srv(in.cirrusPatterns));
	base.Set(Slot::TexCloudProfileLUT, srv(in.profileLut));
	base.Set(Slot::TexCloudAdjustmentLUT, srv(in.adjustmentLut));
	base.Set(Slot::TexDiskPhaseLut, Bind::Published(ORGPhysicalSky::kDiskPhaseLutId));
	base.Set(Slot::TexMiddleWeather, srv(in.middleWeather));
	base.Set(Slot::TexMiddleWarp, srv(in.middleNoise[0]));
	base.Set(Slot::TexMiddleCell, srv(in.middleNoise[2]));
	base.Set(Slot::NoiseVoxel, srv(in.voxelNoise));
	for (std::uint32_t i = 0; i < 8; ++i) {
		base.Set(static_cast<Slot>(Slot::TexVoxelModeling0 + i), srvArray(in.voxelModeling[i]));
		base.Set(static_cast<Slot>(Slot::TexVoxelDistance0 + i), srvArray(in.voxelDistance[i]));
	}
	base.Set(Slot::TileableSampler, Bind::Handle(tileable.Index()));
	base.Set(Slot::SampTr, Bind::Handle(samplerTr));
	base.Set(Slot::SampSv, Bind::Handle(samplerSv));

	// The shadow volume pass also binds the local NDF and storm maps.
	CloudTable shadow = base;
	shadow.Set(Slot::TexLocalNdfHeight, srvArray(in.localHeight));
	shadow.Set(Slot::TexLocalNdfModeling, srvArray(in.localModeling));
	shadow.Set(Slot::TexLocalNdfMaximum, srvArray(in.localMaximum));
	shadow.Set(Slot::TexStormHeight, srvArray(in.stormHeight));
	shadow.Set(Slot::TexStormModeling, srvArray(in.stormModeling));
	{
		const std::array<std::pair<CloudProgram, std::uint32_t>, 5> variants{ { { CloudProgram::ShadowVoxel, kShadowVoxel },
			{ CloudProgram::ShadowColumns, kShadowColumns }, { CloudProgram::ShadowWeather, kShadowWeather }, { CloudProgram::ShadowVolcanic, kShadowVolcanic },
			{ CloudProgram::ShadowVolume, kShadowVolume } } };
		for (const auto& [variant, entry] : variants)
			dispatch(std::format("shadow-volume.{}", entry), variant, entry, shadow.With({ { Slot::RWShadowVolume, Bind::Uav(shadowRaw) } }));
		CloudTable resample = shadow;
		resample.Set(Slot::TexShadowVolume, Bind::Srv(shadowRaw));
		dispatch("shadow-volume.resample", CloudProgram::ShadowResample, kShadowResample, resample.With({ { Slot::RWShadowVolume, Bind::Uav(shadowVolume) } }));
		CloudTable mean = shadow;
		mean.Set(Slot::TexShadowVolume, Bind::Srv(shadowVolume));
		const std::array<std::pair<CloudProgram, std::uint32_t>, 4> means{ { { CloudProgram::ShadowMeanVoxel, kShadowMeanVoxel },
			{ CloudProgram::ShadowMeanWeather, kShadowMeanWeather }, { CloudProgram::ShadowMeanVolcanic, kShadowMeanVolcanic },
			{ CloudProgram::ShadowMean, kShadowMean } } };
		for (const auto& [variant, entry] : means)
			dispatch(std::format("shadow-mean.{}", entry), variant, entry, mean.With({ { Slot::RWCloudShadowMean, Bind::Uav(shadowMean) } }));
	}

	// The main view pass. Its start sees what the shadow pass's cleanup left: the base, without the local NDF and storm maps,
	// which it binds again after the ambient SH.
	CloudTable main = base;
	dispatch("voxel-lighting", CloudProgram::VoxelLighting, kVoxelLighting, main.With({ { Slot::RWVoxelLighting, uavArray(in.voxelLighting) } }));
	main.Set(Slot::TexVoxelLighting, srvArray(in.voxelLighting));
	{
		CloudTable sh = main;
		sh.Set(Slot::TexCloudGroundReflection, Bind::SrvCube(groundReflection));
		dispatch("ambient-sh", CloudProgram::AmbientSH, kAmbientSH, sh.With({ { Slot::RWCloudAmbientPartial, Bind::Uav(ambientPartial) } }));
		sh.Set(Slot::TexCloudAmbientPartial, Bind::Srv(ambientPartial));
		dispatch("ambient-sh.reduce", CloudProgram::AmbientReduce, kAmbientReduce, sh.With({ { Slot::RWCloudAmbientSH, Bind::Uav(ambientSH) } }));
		main.Set(Slot::TexCloudAmbientSH, Bind::Srv(ambientSH));
		dispatch("ambient-lighting", CloudProgram::AmbientLighting, kAmbientLighting, main.With({ { Slot::RWCloudAmbientLighting, Bind::Uav(ambientLighting) } }));
	}
	main.Set(Slot::TexCloudAmbientLighting, Bind::Srv(ambientLighting));
	main.Set(Slot::TexLocalNdfHeight, srvArray(in.localHeight));
	main.Set(Slot::TexLocalNdfModeling, srvArray(in.localModeling));
	main.Set(Slot::TexLocalNdfMaximum, srvArray(in.localMaximum));
	main.Set(Slot::TexStormHeight, srvArray(in.stormHeight));
	main.Set(Slot::TexStormModeling, srvArray(in.stormModeling));

	// The middle layer's bounds, a mip chain (MidCloudBounds.cs.hlsl), when its weather map changes.
	builder.Add(AnchorPasses::Dispatch{ .name = "middle-bounds.0",
		.program = middleBounds.Get(),
		.bindings = { srv(in.middleWeather), null, Bind::Uav(middleBoundsTex, 0) },
		.gate = kMiddleBounds });
	for (std::uint32_t mip = 1; mip < PhysicalSky::kMiddleBoundsLevels; ++mip)
		builder.Add(AnchorPasses::Dispatch{ .name = std::format("middle-bounds.{}", mip),
			.program = middleBoundsReduce.Get(),
			.bindings = { null, Bind::Srv(middleBoundsTex, mip - 1), Bind::Uav(middleBoundsTex, mip) },
			.gate = kMiddleBounds + mip });
	// D3D11 binds them while the middle layer is on, and the upper sun depth on frames that render it. Programs read them
	// only then, so they are in the table whenever the layer's maps are.
	if (in.middleWeather)
		main.Set(Slot::TexMiddleBounds, Bind::Srv(middleBoundsTex));
	dispatch("upper-sun-depth", CloudProgram::UpperSunDepth, kUpperSunDepth, main.With({ { Slot::RWUpperSunDepth, Bind::Uav(upperSunDepth) } }));
	if (in.middleWeather || in.cirrusWeather)
		main.Set(Slot::TexUpperSunDepth, Bind::Srv(upperSunDepth));

	// The height bounds, when the weather changed or the history is new.
	dispatch("height-bounds", CloudProgram::HeightBounds, kHeightBounds, main.With({ { Slot::RWCloudHeightTiles, Bind::Uav(heightTiles) } }));
	{
		CloudTable reduce = main;
		reduce.Set(Slot::TexCloudHeightBounds, Bind::Srv(heightTiles));
		reduce.Set(Slot::TexCloudHeightBoundsTiles, Bind::Srv(heightTiles));
		dispatch("height-bounds.reduce", CloudProgram::HeightBoundsReduce, kHeightBoundsReduce,
			reduce.With({ { Slot::RWCloudHeightBounds, Bind::Uav(heightBounds) }, { Slot::RWCloudWeatherBounds, Bind::Uav(weatherBounds) } }));
	}
	main.Set(Slot::TexCloudHeightBounds, Bind::Srv(heightBounds));
	main.Set(Slot::TexCloudWeatherBounds, Bind::Srv(weatherBounds));

	// The ambient transport, one cascade a frame. The ping-pong sources and layouts are fixed targets refreshed by copies.
	{
		const std::array<float, 4> zero{};
		for (std::uint32_t i = 0; i < 2; ++i) {
			builder.Add(AnchorPasses::Clear{ .name = std::format("ambient.reset{}", i), .target = ambientLayout[i], .value = zero, .gate = kAmbientReset });
		}
		dispatch("ambient.boundary", CloudProgram::AmbientBoundary, kAmbientBoundary, main.With({ { Slot::RWCloudAmbientBoundary, Bind::Uav(ambientBoundary) } }));
		CloudTable transport = main;
		transport.Set(Slot::TexCloudAmbientBoundary, Bind::Srv(ambientBoundary));
		const std::array<std::pair<CloudProgram, std::uint32_t>, 3> densities{ { { CloudProgram::AmbientDensityVoxel, kAmbientDensityVoxel },
			{ CloudProgram::AmbientDensityVolcanic, kAmbientDensityVolcanic }, { CloudProgram::AmbientDensity, kAmbientDensity } } };
		for (const auto& [variant, entry] : densities)
			dispatch(std::format("ambient.density.{}", entry), variant, entry, transport.With({ { Slot::RWCloudAmbientDensity, Bind::Uav(ambientDensity) } }));
		transport.Set(Slot::TexCloudAmbientDensity, Bind::Srv(ambientDensity));
		const std::array<Slot, PhysicalSky::kCloudAmbientCascadeCount> sourceSlots{ Slot::TexCloudAmbientSource0, Slot::TexCloudAmbientSource1,
			Slot::TexCloudAmbientSource2, Slot::TexCloudAmbientSource3 };
		for (std::uint32_t i = 0; i < PhysicalSky::kCloudAmbientCascadeCount; ++i)
			transport.Set(sourceSlots[i], Bind::Srv(ambientSource[i][0]));
		transport.Set(Slot::TexCloudAmbientPreviousLayout, Bind::Srv(ambientLayout[0]));
		dispatch("ambient.sweep", CloudProgram::AmbientSweep, kAmbientSweep, transport.With({ { Slot::RWCloudAmbientSweep, Bind::Uav(ambientSweep) } }));
		dispatch("ambient.sweep-far", CloudProgram::AmbientSweepFar, kAmbientSweepFar, transport.With({ { Slot::RWCloudAmbientSweep, Bind::Uav(ambientSweep) } }));
		transport.Set(Slot::TexCloudAmbientSweep, Bind::Srv(ambientSweep));
		const auto& sourceDesc = f.texCloudAmbientSource;
		for (std::uint32_t i = 0; i < PhysicalSky::kCloudAmbientCascadeCount; ++i) {
			dispatch(std::format("ambient.project{}", i), CloudProgram::AmbientProject, kAmbientProject + i,
				transport.With({ { Slot::RWCloudAmbientSource, Bind::Uav(ambientSource[i][1]) } }));
			const auto& shape = sourceDesc[i][1]->desc;
			copy(std::format("ambient.source{}", i), ambientSource[i][1], ambientSource[i][0], kAmbientSourceCopy + i, shape.Width, shape.Height, shape.Depth);
		}
		// D3D11 unbinds t41 to t49 before the layout update, so it reads only its own UAV.
		const auto layoutSize = size2D(f.texCloudAmbientLayout[0]);
		copy("ambient.layout-in", ambientLayout[0], ambientLayout[1], kAmbientLayoutIn, layoutSize[0], layoutSize[1]);
		CloudTable layout = main;
		layout.Set(Slot::TexCloudAmbientBoundary, null);
		dispatch("ambient.layout", CloudProgram::AmbientLayout, kAmbientLayout, layout.With({ { Slot::RWCloudAmbientLayout, Bind::Uav(ambientLayout[1]) } }));
		copy("ambient.layout-out", ambientLayout[1], ambientLayout[0], kAmbientLayoutOut, layoutSize[0], layoutSize[1]);
		// The view resolve and everything after it see the new sources, the layout and the boundary.
		for (std::uint32_t i = 0; i < PhysicalSky::kCloudAmbientCascadeCount; ++i)
			main.Set(sourceSlots[i], Bind::Srv(ambientSource[i][0]));
		main.Set(Slot::TexCloudAmbientLayout, Bind::Srv(ambientLayout[0]));
		main.Set(Slot::TexCloudAmbientBoundary, Bind::Srv(ambientBoundary));
		dispatch("ambient.view", CloudProgram::AmbientView, kAmbientView,
			main.With({ { Slot::RWCloudAmbientView0, Bind::Uav(ambientView[0]) }, { Slot::RWCloudAmbientView1, Bind::Uav(ambientView[1]) },
				{ Slot::RWCloudAmbientView2, Bind::Uav(ambientView[2]) }, { Slot::RWCloudAmbientView3, Bind::Uav(ambientView[3]) } }));
		const std::array<Slot, PhysicalSky::kCloudAmbientCascadeCount> viewSlots{ Slot::TexCloudAmbientView0, Slot::TexCloudAmbientView1,
			Slot::TexCloudAmbientView2, Slot::TexCloudAmbientView3 };
		for (std::uint32_t i = 0; i < PhysicalSky::kCloudAmbientCascadeCount; ++i)
			main.Set(viewSlots[i], Bind::Srv(ambientView[i]));
	}

	// The lightning query: one group finds the candidates, and a copy reads them back with the run's stamp.
	if (!candidates || !*candidates)
		builder.Fail("the lightning candidates buffer is missing");
	else {
		dispatch("lightning.query", CloudProgram::LightningQuery, kLightningQuery, main.With({ { Slot::LightningCandidates, Bind::Uav(*candidates) } }));
		readback->Bind(latch, kStampOffset);
		std::string why;
		auto pass = readback->BufferPass(0, candidates->Graph(), why);
		if (pass)
			builder.Add("lightning.readback", FrameAnchors::PassKind::Copy, std::move(pass));
		else
			builder.Fail(std::format("lightning.readback: {}", why));
	}

	// The boundary raster, into the low-resolution boundary target.
	{
		const auto& shape = f.texCloudBoundary->desc;
		AnchorPasses::Draw draw{ .name = "boundary",
			.program = boundary.Get(),
			.bindings = { Bind::Shared(), srv(in.ndfHeight), srv(in.ndfModeling), Bind::Handle(tileable.Index()), Bind::Latch(kBoundaryOffset) },
			.target = boundaryTarget,
			.viewport = { .width = shape.Width, .height = shape.Height },
			.load = rhi::LoadOp::Clear,
			.clear = { std::numeric_limits<float>::max(), 0.f, std::numeric_limits<float>::max(), 0.f },
			.gate = kBoundary };
		builder.Add(std::move(draw));
	}
	main.Set(Slot::TexCloudBoundary, Bind::Srv(boundaryTarget));

	// The main view, while the map menu is closed: the shadow volume, the AP shadow's inputs, the trace targets.
	CloudTable view = main;
	view.Set(Slot::TexShadowVolume, Bind::Srv(shadowVolume));
	CloudTable apView = view;
	apView.Set(Slot::TexDirectShadows, a_key.shadowMap ? Bind::SrvArray(a_key.shadowMap) : null);
	apView.Set(Slot::ShadowHeightTexture, a_key.terrain.empty() ? null : Bind::Published(a_key.terrain));
	apView.Set(Slot::CloudShadowsTexture,
		a_key.cloudLayer.empty() ? null : Bind::Published(a_key.cloudLayer, static_cast<std::uint32_t>(org::SRVViewType::TextureCube)));
	{
		const auto trace = std::initializer_list<std::pair<Slot, Bind>>{ { Slot::RWTexTr, Bind::Uav(lowTr) }, { Slot::RWTexLum, Bind::Uav(lowLum) },
			{ Slot::RWTexAux, Bind::Uav(lowAux) }, { Slot::RWLightningMask, Bind::Uav(lightningTrace) }, { Slot::RWStormMotion, Bind::Uav(stormMotion) } };
		dispatch("main.storm-tiles", CloudProgram::StormTiles, kStormTiles, view.With({ { Slot::RWStormTiles, Bind::Uav(stormTiles) } }), kStormRectOffset);
		CloudTable tiled = apView;
		tiled.Set(Slot::TexStormTiles, Bind::Srv(stormTiles));
		dispatch("main.weather", CloudProgram::MainWeather, kMainWeather, apView.With(trace), kStormRectOffset);
		dispatch("main.weather-tiles", CloudProgram::MainWeather, kMainWeatherTiles, tiled.With(trace), kStormRectOffset);
		dispatch("main.without-storm", CloudProgram::MainWithoutStorm, kMainWithoutStorm, apView.With(trace), kStormRectOffset);
		dispatch("main.without-storm-tiles", CloudProgram::MainWithoutStorm, kMainWithoutStormTiles, tiled.With(trace), kStormRectOffset);
		dispatch("main.storm", CloudProgram::MainView, kMainStorm, tiled.With(trace), kStormRectOffset);
		dispatch("main.volcanic", CloudProgram::MainVolcanic, kMainVolcanic, tiled.With(trace), kStormRectOffset);
		dispatch("main.single-volcanic", CloudProgram::MainSingleVolcanic, kMainSingleVolcanic, tiled.With(trace), kStormRectOffset);
	}
	// The voxel and upper views read the trace and write the second set of targets, which copies make the trace again.
	{
		CloudTable second = view;
		second.Set(Slot::TexVolLowTr, Bind::Srv(lowTr));
		second.Set(Slot::TexVolLowLum, Bind::Srv(lowLum));
		second.Set(Slot::TexVolLowAux, Bind::Srv(lowAux));
		second.Set(Slot::TexVolLowStormMotion, Bind::Srv(stormMotion));
		second.Set(Slot::TexLightningTrace, Bind::Srv(lightningTrace));
		CloudTable apSecond = apView;
		apSecond.Set(Slot::TexVolLowTr, Bind::Srv(lowTr));
		apSecond.Set(Slot::TexVolLowLum, Bind::Srv(lowLum));
		apSecond.Set(Slot::TexVolLowAux, Bind::Srv(lowAux));
		apSecond.Set(Slot::TexVolLowStormMotion, Bind::Srv(stormMotion));
		apSecond.Set(Slot::TexLightningTrace, Bind::Srv(lightningTrace));
		const auto outputs = std::initializer_list<std::pair<Slot, Bind>>{ { Slot::RWTexTr, Bind::Uav(voxelTr) }, { Slot::RWTexLum, Bind::Uav(voxelLum) },
			{ Slot::RWTexAux, Bind::Uav(voxelAux) }, { Slot::RWLightningMask, Bind::Uav(voxelLightning) }, { Slot::RWStormMotion, Bind::Uav(voxelMotion) } };
		const auto copyBack = [&](const char* a_step, std::uint32_t a_entry) {
			copy2D(std::format("{}.copy-tr", a_step), voxelTr, lowTr, a_entry, f.texVolLowTr);
			copy2D(std::format("{}.copy-lum", a_step), voxelLum, lowLum, a_entry, f.texVolLowLum);
			copy2D(std::format("{}.copy-aux", a_step), voxelAux, lowAux, a_entry, f.texVolLowAux);
			copy2D(std::format("{}.copy-motion", a_step), voxelMotion, stormMotion, a_entry, f.texVolStormMotion);
			copy2D(std::format("{}.copy-lightning", a_step), voxelLightning, lightningTrace, a_entry, f.texLightningTrace);
		};
		dispatch("voxel-view", CloudProgram::VoxelView, kVoxelView, apSecond.With(outputs), kVoxelRectOffset);
		dispatch("voxel-view.single", CloudProgram::VoxelViewSingle, kVoxelViewSingle, apSecond.With(outputs), kVoxelRectOffset);
		copyBack("voxel-view", kVoxelCopy);
		dispatch("upper.tiles", CloudProgram::UpperTiles, kUpperTiles, second.With({ { Slot::RWUpperTiles, Bind::Uav(upperTiles) } }));
		CloudTable upperTiled = second;
		upperTiled.Set(Slot::TexUpperTiles, Bind::Srv(upperTiles));
		dispatch("upper.view-tiles", CloudProgram::UpperViewTiles, kUpperViewTiles, upperTiled.With(outputs));
		dispatch("upper.view", CloudProgram::UpperView, kUpperView, second.With(outputs));
		copyBack("upper", kUpperCopy);
	}

	// Reprojection into the fixed current targets, from the histories the last frame's copies made.
	{
		CloudTable reproject = main;
		reproject.Set(Slot::TexVolHistoryTr, Bind::Srv(historyTr));
		reproject.Set(Slot::TexVolHistoryLum, Bind::Srv(historyLum));
		reproject.Set(Slot::TexVolHistoryAux, Bind::Srv(historyAux));
		reproject.Set(Slot::TexLightningTrace, Bind::Srv(lightningTrace));
		reproject.Set(Slot::TexLightningHistory, Bind::Srv(lightningHistory));
		reproject.Set(Slot::TexVolLowTr, Bind::Srv(lowTr));
		reproject.Set(Slot::TexVolLowLum, Bind::Srv(lowLum));
		reproject.Set(Slot::TexVolLowAux, Bind::Srv(lowAux));
		reproject.Set(Slot::TexVolLowStormMotion, Bind::Srv(stormMotion));
		dispatch("reproject", CloudProgram::Reproject, kReproject,
			reproject.With({ { Slot::RWTexTr, Bind::Uav(volTr) }, { Slot::RWTexLum, Bind::Uav(volLum) }, { Slot::RWTexAux, Bind::Uav(volAux) },
				{ Slot::RWLightningMask, Bind::Uav(lightningMask) } }));
	}
	main.Set(Slot::TexLightningTrace, Bind::Srv(lightningTrace));
	main.Set(Slot::TexLightningHistory, Bind::Srv(lightningHistory));
	{
		CloudTable filter = main;
		filter.Set(Slot::TexCloudResultTr, Bind::Srv(volTr));
		filter.Set(Slot::TexCloudResultLum, Bind::Srv(volLum));
		filter.Set(Slot::TexCloudResultAux, Bind::Srv(volAux));
		const auto outputs = std::initializer_list<std::pair<Slot, Bind>>{ { Slot::RWTexFilteredTr, Bind::Uav(filteredTr) },
			{ Slot::RWTexFilteredLum, Bind::Uav(filteredLum) }, { Slot::RWTexFilteredAux, Bind::Uav(filteredAux) } };
		dispatch("filter", CloudProgram::Filter, kFilter, filter.With(outputs));
		dispatch("filter.wide", CloudProgram::FilterWide, kFilterWide, filter.With(outputs));
		copyScreen("filter.copy-tr", volTr, filteredTr, kFilterCopy);
		copyScreen("filter.copy-lum", volLum, filteredLum, kFilterCopy);
		copyScreen("filter.copy-aux", volAux, filteredAux, kFilterCopy);
		// The map menu clears the main view's targets instead.
		gatedClear("map-menu.tr", volTr, PhysicalSky::kClearTransmittance, kMapMenuClear);
		gatedClear("map-menu.lum", volLum, PhysicalSky::kClearLuminance, kMapMenuClear);
		gatedClear("map-menu.aux", volAux, PhysicalSky::kClearLuminance, kMapMenuClear);
		gatedClear("map-menu.filtered-tr", filteredTr, PhysicalSky::kClearTransmittance, kMapMenuClear);
		gatedClear("map-menu.filtered-lum", filteredLum, PhysicalSky::kClearLuminance, kMapMenuClear);
		gatedClear("map-menu.filtered-aux", filteredAux, PhysicalSky::kClearLuminance, kMapMenuClear);
	}
	{
		CloudTable composite = main;
		composite.Set(Slot::LightningResolvedMask, Bind::Srv(lightningMask));
		dispatch("lightning.composite", CloudProgram::LightningComposite, kLightningComposite,
			composite.With({ { Slot::RWLightningChannels, Bind::Uav(lightningChannels) }, { Slot::RWLightningColor, Bind::Uav(filteredLum) } }));
	}

	// The cubemap: trace, reprojection from the cube histories, the lightning reflection.
	{
		CloudTable cube = main;
		cube.Set(Slot::TexCloudBoundary, null);
		cube.Set(Slot::TexCloudHeightBoundsTiles, Bind::Srv(heightTiles));
		const auto trace = std::initializer_list<std::pair<Slot, Bind>>{ { Slot::RWTexCubeTr, Bind::UavArray(cubeTraceTr) },
			{ Slot::RWTexCubeLum, Bind::UavArray(cubeTraceLum) }, { Slot::RWTexCubeAux, Bind::UavArray(cubeTraceAux) },
			{ Slot::RWLightningCubeMask, Bind::UavArray(lightningCubeTrace) }, { Slot::RWCubeStormMotion, Bind::UavArray(cubeStormMotion) } };
		dispatch("cube.voxel", CloudProgram::CubeVoxel, kCubeVoxel, cube.With(trace));
		dispatch("cube.volcanic", CloudProgram::CubeVolcanic, kCubeVolcanic, cube.With(trace));
		dispatch("cube", CloudProgram::Cube, kCube, cube.With(trace));
		CloudTable reproject = main;
		reproject.Set(Slot::TexCloudBoundary, null);
		reproject.Set(Slot::TexCubeHistoryTr, srvCube(cubeHistoryTr));
		reproject.Set(Slot::TexCubeHistoryLum, srvCube(cubeHistoryLum));
		reproject.Set(Slot::TexCubeHistoryAux, srvCube(cubeHistoryAux));
		reproject.Set(Slot::TexCubeTraceTr, srvCube(cubeTraceTr));
		reproject.Set(Slot::TexCubeTraceLum, srvCube(cubeTraceLum));
		reproject.Set(Slot::TexCubeTraceAux, srvCube(cubeTraceAux));
		reproject.Set(Slot::TexCubeStormMotion, srvCube(cubeStormMotion));
		reproject.Set(Slot::TexLightningCubeTrace, srvCube(lightningCubeTrace));
		reproject.Set(Slot::TexLightningCubeHistory, srvCube(lightningCubeHistory));
		dispatch("cube.reproject", CloudProgram::CubeReproject, kCubeReproject,
			reproject.With({ { Slot::RWTexCubeTr, Bind::UavArray(cubeTr) }, { Slot::RWTexCubeLum, Bind::UavArray(cubeLum) },
				{ Slot::RWTexCubeAux, Bind::UavArray(cubeAux) }, { Slot::RWLightningCubeMask, Bind::UavArray(lightningCubeMask) } }));
		CloudTable lightning = main;
		lightning.Set(Slot::TexCloudBoundary, null);
		lightning.Set(Slot::TexLightningCubeTrace, srvCube(lightningCubeTrace));
		lightning.Set(Slot::TexLightningCubeHistory, srvCube(lightningCubeHistory));
		lightning.Set(Slot::LightningResolvedCubeMask, srvCube(lightningCubeMask));
		dispatch("lightning.cube", CloudProgram::LightningCubeComposite, kLightningCube,
			lightning.With({ { Slot::RWLightningCubeColor, Bind::UavArray(lightningCubeLum) } }));
	}

	// The next frame's histories, in place of D3D11's pointer swaps.
	copyScreen("history.tr", volTr, historyTr, kHistories);
	copyScreen("history.lum", volLum, historyLum, kHistories);
	copyScreen("history.aux", volAux, historyAux, kHistories);
	copyScreen("history.lightning", lightningMask, lightningHistory, kHistories);
	copy2D("history.cube-tr", cubeTr, cubeHistoryTr, kHistories, f.texVolCubeTr);
	copy2D("history.cube-lum", cubeLum, cubeHistoryLum, kHistories, f.texVolCubeLum);
	copy2D("history.cube-aux", cubeAux, cubeHistoryAux, kHistories, f.texVolCubeAux);
	copy2D("history.lightning-cube", lightningCubeMask, lightningCubeHistory, kHistories, f.texLightningCubeMask);

	if (builder.Failed()) {
		logger::error("[ORG] Physical Sky clouds: {}; no clouds", builder.Failure());
		return false;
	}
	a_client.id = kCloudsClientId;
	a_client.sharedConstants = builder.ReadsSharedConstants();
	a_client.passes = builder.Take();
	a_client.gates = { gate };
	a_client.registerResources = [state = a_self](org::RenderGraph& a_graph) {
		for (const auto& [_, twin] : Twins().textures2D)
			twin->Register(a_graph);
		for (const auto& [_, twin] : Twins().textures3D)
			twin->Register(a_graph);
		for (const auto& [_, screen] : Twins().screens)
			screen.twin->Register(a_graph);
		if (state->candidates)
			state->candidates->Register(a_graph);
	};
	a_client.active = [state = a_self] { return state->Active(); };
	a_client.commit = [state = a_self](const FrameAnchors::Point&) { state->Commit(); };
	a_client.executed = [state = a_self](const FrameAnchors::Point&) { state->Executed(); };
	// Every registration commits the same run, so the one the schedule holds keeps running until a newer one is adopted (by id).
	logger::info("[ORG] Physical Sky clouds: a registration for the next revision ({}; {}x{}, shadow map {}, terrain heights {}, cloud layer {})", kCloudsClientId,
		a_clientKey.screen[0], a_clientKey.screen[1], a_key.shadowMap ? "imported" : "none", a_key.terrain.empty() ? "none" : a_key.terrain,
		a_key.cloudLayer.empty() ? "none" : a_key.cloudLayer);
	return true;
}

ORGVolumetricClouds::ORGVolumetricClouds() = default;
ORGVolumetricClouds::~ORGVolumetricClouds() = default;

namespace
{
	/** @brief The export flags a D3D11 description's bind flags ask for. */
	GraphResources::Export CloudExports(UINT a_bindFlags)
	{
		auto exports = GraphResources::Export::SRV;
		if (a_bindFlags & D3D11_BIND_UNORDERED_ACCESS)
			exports = exports | GraphResources::Export::UAV;
		if (a_bindFlags & D3D11_BIND_RENDER_TARGET)
			exports = exports | GraphResources::Export::RTV;
		return exports;
	}

	/**
	* @brief Keeps a new twin and its wrapper's registration under its name, replacing one made before, and notes the twin by the
	* wrapper object. Returns the wrapper object, which the feature owns.
	*/
	template <class Twin, class Wrapper>
	auto* CloudKeep(std::map<std::string, std::unique_ptr<Twin>>& a_store, std::map<std::string, GraphResources::D3D11Wrapper<Wrapper>>& a_wrappers,
		std::unique_ptr<Twin> a_twin, const char* a_name)
	{
		auto& twins = Twins();
		const auto name = a_twin->Name();
		if (const auto found = a_store.find(name); found != a_store.end()) {
			std::erase_if(twins.byWrapper, [&](const auto& a_entry) { return a_entry.second == found->second.get(); });
			a_wrappers.erase(name);
			a_store.erase(found);
		}
		auto wrapper = GraphResources::MakeWrapper(*a_twin, a_name);
		auto* object = wrapper.Get();
		twins.byWrapper[static_cast<const void*>(object)] = a_twin.get();
		a_wrappers.insert_or_assign(name, std::move(wrapper));
		a_store.emplace(name, std::move(a_twin));
		++twins.generation;
		return object;
	}
}

Texture2D* ORGVolumetricClouds::MakeTexture2D(const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name, std::string_view a_publishedId)
{
	if (!On())
		return nullptr;
	const std::string name = a_publishedId.empty() ? std::string(a_name) : std::string(a_publishedId);
	D3D11_TEXTURE2D_DESC desc = a_desc;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
	const auto exports = CloudExports(desc.BindFlags);
	// Every cloud texture is read in a later frame or written only on a change, and D3D11 binds them for engine draws.
	const GraphResources::Options options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = exports };
	auto texture = std::make_unique<GraphResources::Texture2D>(desc, name, options);
	if (!texture->D3D11() || !texture->SRV() || (GraphResources::Has(exports, GraphResources::Export::UAV) && !texture->UAV()) ||
		(GraphResources::Has(exports, GraphResources::Export::RTV) && !texture->RTV()))
		Fatal(fmt::format("{} could not be made on the render graph", name));
	if (!a_publishedId.empty() && !PublishedResources::Publish(a_publishedId, *texture))
		Fatal(fmt::format("{} could not be published", a_publishedId));
	// The twin keeps the image, the wrapper the D3D11 texture and its views.
	auto& twins = Twins();
	return CloudKeep(twins.textures2D, twins.wrappers2D, std::move(texture), a_name);
}

Texture3D* ORGVolumetricClouds::MakeTexture3D(const D3D11_TEXTURE3D_DESC& a_desc, const char* a_name, std::string_view a_publishedId)
{
	if (!On())
		return nullptr;
	const std::string name = a_publishedId.empty() ? std::string(a_name) : std::string(a_publishedId);
	D3D11_TEXTURE3D_DESC desc = a_desc;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;
	const GraphResources::Options options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = CloudExports(desc.BindFlags) };
	auto texture = std::make_unique<GraphResources::Texture3D>(desc, name, options);
	if (!texture->D3D11() || !texture->SRV() || ((desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) && !texture->UAV()))
		Fatal(fmt::format("{} could not be made on the render graph", name));
	if (!a_publishedId.empty() && !PublishedResources::Publish(a_publishedId, *texture))
		Fatal(fmt::format("{} could not be published", a_publishedId));
	auto& twins = Twins();
	return CloudKeep(twins.textures3D, twins.wrappers3D, std::move(texture), a_name);
}

void ORGVolumetricClouds::Setup(const PhysicalSky& a_feature)
{
	if (!BeginSetup())
		return;
	auto next = std::make_shared<Impl>();
	const auto& descs = CloudProgramDescs();
	for (std::size_t i = 0; i < kCloudProgramCount; ++i) {
		ComputeProgram::Desc desc{ .source = kVolumetricsShader, .entry = descs[i].entry, .constantWords = kCloudWords };
		for (const auto* define : descs[i].defines) {
			const std::wstring_view text(define);
			const auto equals = text.find(L'=');
			desc.defines.emplace_back(std::wstring(text.substr(0, equals)), equals == std::wstring_view::npos ? std::wstring() : std::wstring(text.substr(equals + 1)));
		}
		next->programs[i] = RenderGraph::Programs::Queue("VolumetricClouds", std::move(desc));
	}
	next->middleBounds = RenderGraph::Programs::Queue("VolumetricClouds", ComputeProgram::Desc{ .source = kMiddleBoundsShader, .constantWords = kMiddleBoundsWords });
	next->middleBoundsReduce = RenderGraph::Programs::Queue("VolumetricClouds",
		ComputeProgram::Desc{ .source = kMiddleBoundsShader, .entry = L"reduceBounds", .constantWords = kMiddleBoundsWords });
	RasterProgram::Desc boundary{ .pixelSource = kBoundaryShader,
		.pixelEntry = L"pixelMain",
		.vertexSource = kBoundaryShader,
		.vertexEntry = L"vertexMain",
		.constantWords = kBoundaryWords };
	// As D3D11's boundary states: MIN blending of every channel, no culling, the counter-clockwise face in front.
	rhi::BlendAttachment minimum{ .enable = true,
		.srcColor = rhi::BlendFactor::One,
		.dstColor = rhi::BlendFactor::One,
		.colorOp = rhi::BlendOp::Min,
		.srcAlpha = rhi::BlendFactor::One,
		.dstAlpha = rhi::BlendFactor::One,
		.alphaOp = rhi::BlendOp::Min };
	boundary.targets = { { .format = rhi::Format::R32G32B32A32_Float, .blend = minimum } };
	boundary.raster = { .cull = rhi::CullMode::None, .frontCCW = true };
	next->boundary = RenderGraph::Programs::Queue("VolumetricClouds", std::move(boundary));
	next->gate = RenderGraph::RunGate::Create("cs.physical-sky.clouds.gate", kEntryCount);
	next->latch = std::make_shared<org::LatchBlock>("cs.physical-sky.clouds.latch", kLatchBytes);
	next->readback = org::RunReadback::Create("PhysicalSky::LightningCandidates", { SkyLightning::kCandidates * static_cast<std::uint32_t>(sizeof(float4)) });
	D3D11_BUFFER_DESC candidates{};
	candidates.ByteWidth = SkyLightning::kCandidates * sizeof(float4);
	candidates.Usage = D3D11_USAGE_DEFAULT;
	candidates.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	candidates.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	candidates.StructureByteStride = sizeof(float4);
	// Written by the query and read back on later runs, so it keeps its contents.
	next->candidates = std::make_unique<GraphResources::StructuredBuffer>(candidates, SkyLightning::kCandidates, "PhysicalSky::LightningCandidates",
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::History });
	if (a_feature.sampTileable) {
		D3D11_SAMPLER_DESC sampler{};
		a_feature.sampTileable->GetDesc(&sampler);
		next->tileable = ShaderBindings::CreateSamplerFromD3D11(sampler);
	}
	if (!next->readback || !*next->candidates || !next->tileable)
		Fatal("the readback, candidates buffer or tileable sampler could not be made");
	// On the sky's and the clouds' toggles and the display size, with the inputs the last Stage found.
	using namespace RenderGraph::Frame;
	const auto& settings = a_feature.settings;
	const std::weak_ptr<Impl> weak = next;
	next->client = DeclareClient<CloudClientKey, const CloudClientKey>(
		kCloudsClientId, DependsOn(Vars::DisplaySize(), settings.enabled, settings.enableVolumetricClouds, next->observed.Var()),
		[weak](const Inputs& a_in) -> std::optional<CloudClientKey> {
			const auto state = weak.lock();
			const auto& sky = globals::features::physicalSky.settings;
			if (!state || !a_in.Get(sky.enabled) || !a_in.Get(sky.enableVolumetricClouds))
				return std::nullopt;
			const auto* seen = state->observed.Get(a_in);
			if (!seen)
				return std::nullopt;
			return CloudClientKey{ seen->Resolved(), CloudScreenExtent(a_in) };
		},
		[weak](const CloudClientKey& a_key, FrameAnchors::ClientDesc& a_client) -> std::shared_ptr<const CloudClientKey> {
			const auto state = weak.lock();
			if (!state || !state->Make(globals::features::physicalSky, a_key, state, a_client))
				return nullptr;
			return std::make_shared<const CloudClientKey>(a_key);
		});
	impl = next;
	logger::info("[ORG] Physical Sky clouds: {} programs loaded; the client follows the frame revisions once a frame found its inputs", kCloudProgramCount + 3);
}

void ORGVolumetricClouds::RequestNeutral()
{
	if (impl)
		impl->neutral = true;
}

bool ORGVolumetricClouds::OutputsWritten()
{
	// Every screen target is born neutral. A run of the client wrote the bound versions once Written says so, and its first run
	// cleared the plain twins (the shadow volume and its mean) to neutral.
	const auto& screens = Twins().screens;
	if (screens.empty())
		return false;
	const auto& adopted = RenderGraph::Frame::Revision::Adopted();
	return std::ranges::all_of(screens, [&](const auto& a_entry) { return adopted.Written(a_entry.second.twin); });
}

Texture2D* ORGVolumetricClouds::MakeScreenTarget(const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name, std::string_view a_publishedId,
	const std::array<float, 4>& a_neutral)
{
	if (!On())
		return nullptr;
	using namespace RenderGraph::Frame;
	const std::string name = a_publishedId.empty() ? std::string(a_name) : std::string(a_publishedId);
	auto& twins = Twins();
	if (twins.screens.contains(name))
		return nullptr;
	D3D11_TEXTURE2D_DESC desc = a_desc;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;
	const auto& settings = globals::features::physicalSky.settings;
	// History: read in a later frame. D3D11 binds them for engine draws. One texel while the clouds are off (the existence rule).
	// Every version is born neutral, as the D3D11 path clears new targets, so a new size or an on switch needs no clear.
	const GraphResources::Options options{ .lifetime = GraphResources::Lifetime::History,
		.d3d11 = CloudExports(desc.BindFlags),
		.birth = a_neutral == std::array<float, 4>{} ? Birth::Zero() : Birth::Value(a_neutral) };
	auto declared = DeclareTexture2D(name, DisplaySized(desc), options, ExistsWhen(settings.enabled, settings.enableVolumetricClouds));
	if (!declared->D3D11() || !declared->SRV() || !declared->UAV())
		Fatal(fmt::format("{} could not be made on the render graph", name));
	// Published once, for the session. Readers follow the versions.
	if (!a_publishedId.empty() && !PublishedResources::Publish(a_publishedId, *declared))
		Fatal(fmt::format("{} could not be published", a_publishedId));
	// The wrapper follows the bound version: RetargetWrappers re-points it at each adoption.
	auto wrapper = GraphResources::MakeWrapper(declared, a_name);
	auto* object = wrapper.Get();
	twins.byWrapper[static_cast<const void*>(object)] = declared.Get();
	twins.screens.emplace(name, CloudScreenTarget{ std::move(declared), std::move(wrapper) });
	++twins.generation;
	return object;
}

void ORGVolumetricClouds::FollowVersions(PhysicalSky& a_feature)
{
	// The wrappers were re-pointed at this frame's adoption. A screen target no run wrote yet holds a new version.
	bool changed = false;
	const auto& adopted = RenderGraph::Frame::Revision::Adopted();
	for (const auto& [_, screen] : Twins().screens)
		if (screen.twin)
			changed |= !adopted.Written(screen.twin);
	if (changed) {
		// New targets are born neutral, as D3D11 clears new ones (EnsureVolumetricScreenResources). The history restarts.
		a_feature.volMainHistoryValid = false;
		a_feature.volHistoryFrameDim = {};
	}
}

bool ORGVolumetricClouds::QueueLightningQuery()
{
	if (!impl || !impl->pending)
		return false;
	impl->pending->query = true;
	impl->Open(*impl->pending, kLightningQuery, 1);
	return true;
}

bool ORGVolumetricClouds::TakeLightningCandidates(std::array<float4, SkyLightning::kCandidates>& a_out)
{
	if (!impl || !impl->readback || !impl->queryStamp)
		return false;
	std::uint32_t stamp = 0;
	const auto bytes = impl->readback->Take(0, &stamp);
	// A copy from a run before the query's holds an older query's candidates.
	if (bytes.size() < sizeof(a_out) || stamp < *impl->queryStamp)
		return false;
	std::memcpy(a_out.data(), bytes.data(), sizeof(a_out));
	impl->queryStamp.reset();
	return true;
}

namespace
{
	/** @brief As RenderVolumetricClouds' cleanup binds them for the pixel shaders: the filtered result and the shadow volume. */
	void CloudBindPixel(const PhysicalSky& a_feature)
	{
		std::array<ID3D11ShaderResourceView*, 3> outputs{ a_feature.texVolFilteredTr ? a_feature.texVolFilteredTr->srv.get() : nullptr,
			a_feature.texVolFilteredLum ? a_feature.texVolFilteredLum->srv.get() : nullptr,
			a_feature.texShadowVolume ? a_feature.texShadowVolume->srv.get() : nullptr };
		globals::d3d::context->PSSetShaderResources(110, static_cast<UINT>(outputs.size()), outputs.data());
	}
}

void ORGVolumetricClouds::Stage(PhysicalSky& a_feature, const Frame& a_frame)
{
	auto& f = a_feature;
	CloudBindPixel(f);
	if (!impl || !a_frame.cloud)
		return;
	auto& state = *impl;
	state.pending.reset();
	if (!inputs)
		Fatal("no cloud inputs (ORGCloudInputs) are wired");
	CloudRegistrationKey key;
	CloudSeen seen;
	if (!inputs(f, key.inputs))
		return;

	// The AP shadow's inputs, as RenderVolumetricClouds binds them for the main view (t7, t36, t37, t98).
	CloudShadowLight light{};
	if (!globals::state->isMapMenuOpen) {
		const bool hasLight = globals::deferred && globals::deferred->GetDirectionalShadowLightData(light.light);
		light.lightCount = hasLight ? 1u : 0u;
		if (auto* shadowMap = globals::deferred ? globals::deferred->GetDirectionalShadowMap() : nullptr; shadowMap) {
			seen.shadowDepth = CloudEngineDepthOf(shadowMap);
			if (seen.shadowDepth == UINT32_MAX)
				seen.shadowOther.copy_from(shadowMap);
		}
		auto& terrainShadows = globals::features::terrainShadows;
		if (terrainShadows.IsShadowMapReady() && terrainShadows.texShadowHeight &&
			PublishedResources::IsExportOf(terrainShadows.texShadowHeight->srv.get(), ORGTerrainShadows::kShadowHeightId))
			key.terrain = ORGTerrainShadows::kShadowHeightId;
		if (globals::features::cloudShadows.loaded)
			key.cloudLayer = ORGCloudShadows::PublishedLayerId(CloudShadows::kMaxCloudLayers - 1);
	} else if (const auto* last = state.observed.Latest()) {
		seen.shadowDepth = last->shadowDepth;
		seen.shadowOther = last->shadowOther;
		key.terrain = last->key.terrain;
		key.cloudLayer = last->key.cloudLayer;
	}
	key.twins = Twins().generation;

	// What the client reads is not a variable yet. A change is an event: the next revision registers it again.
	seen.key = std::move(key);
	state.observed.Observe(seen);
	// Until the adopted revision holds a registration the clouds render nothing, and the targets keep their neutral clears.
	if (!state.client.Adopted())
		return;

	auto& frame = state.pending.emplace();
	state.pendingFrame = globals::state->frameCount;
	const auto& c = *a_frame.cloud;
	const bool mapMenu = globals::state->isMapMenuOpen;
	const auto& storms = f.cloudStormManager;
	const bool volcanic = storms.VolcanicOnly();

	// The latch's per-frame values.
	{
		auto cloud = c;
		if (cloud.stormCount > kMaxLatchStorms || cloud.voxelCount > kMaxLatchVoxels || cloud.localNdfAltitude.w > float(kMaxLatchWeights)) {
			static bool logged = false;
			if (!std::exchange(logged, true))
				logger::warn("[ORG] Physical Sky clouds: more storms ({}), voxel clouds ({}) or local layers ({}) than the latch holds; the rest are left out",
					cloud.stormCount, cloud.voxelCount, cloud.localNdfAltitude.w);
			cloud.stormCount = std::min(cloud.stormCount, kMaxLatchStorms);
			cloud.voxelCount = std::min(cloud.voxelCount, kMaxLatchVoxels);
			cloud.localNdfAltitude.w = std::min(cloud.localNdfAltitude.w, float(kMaxLatchWeights));
		}
		Impl::Put(frame, kCloudOffset, cloud);
		Impl::Put(frame, kShadowLightOffset, light);
		const auto& instances = storms.Instances();
		const auto stormCount = std::min<std::size_t>(instances.size(), kMaxLatchStorms);
		std::memcpy(frame.latch.data() + kStormsOffset, instances.data(), stormCount * sizeof(CloudStormData));
		if (instances.size() == 1)
			Impl::Put(frame, kSingleStormOffset, instances.front());
		const auto& weights = f.localNdfManager.LayerWeights();
		std::memcpy(frame.latch.data() + kWeightsOffset, weights.data(), std::min<std::size_t>(weights.size(), kMaxLatchWeights) * sizeof(float4));
		const auto& voxels = f.voxelCloudManager.Instances();
		std::memcpy(frame.latch.data() + kVoxelsOffset, voxels.data(), std::min<std::size_t>(voxels.size(), kMaxLatchVoxels) * sizeof(VoxelCloudData));
		const auto& channel = f.lightning.Channel();
		std::memcpy(frame.latch.data() + kSegmentsOffset, channel.data(), std::min<std::size_t>(channel.size(), SkyLightning::kMaxSegments) * sizeof(SkyLightning::Segment));
	}

	// The shadow volume pass, as RenderVolumetricClouds picks its dispatch.
	{
		constexpr auto W = PhysicalSky::kShadowVolW, H = PhysicalSky::kShadowVolH, D = PhysicalSky::kShadowVolD;
		const float shadowRangeGu = KilometersToGameUnits(f.settings.shadowVolumeRange);
		const float shadowThicknessGu = std::max(c.shadowVolumeTop - c.shadowVolumeBottom, 1.f);
		float3 ray = { -c.dirlightDir.x, -c.dirlightDir.y, -c.dirlightDir.z };
		ray.x *= W / shadowRangeGu;
		ray.y *= H / shadowRangeGu;
		ray.z *= D / shadowThicknessGu;
		const float largest = std::max({ std::abs(ray.x), std::abs(ray.y), std::abs(ray.z) });
		const bool horizontal = std::abs(ray.x) == largest || std::abs(ray.y) == largest;
		const bool serialColumns = !a_frame.voxelActive && c.stormCount == 0u && (!horizontal || c.middleLayer.z > 0.f);
		const std::uint32_t entry = a_frame.voxelActive ? kShadowVoxel :
		                            c.stormCount == 0u  ? (serialColumns ? kShadowColumns : kShadowWeather) :
		                            volcanic            ? kShadowVolcanic :
		                                                  kShadowVolume;
		if (serialColumns) {
			const std::uint32_t columnsY = horizontal ? D : H;
			state.Open(frame, entry, (W + 7u) / 8u, (columnsY + 7u) / 8u);
		} else if (horizontal) {
			state.Open(frame, entry, W, D);
		} else {
			constexpr std::uint32_t columnsPerGroup = W / D;
			state.Open(frame, entry, W / columnsPerGroup, H);
		}
		state.Open(frame, kShadowResample, (W + 7u) / 8u, (H + 7u) / 8u, (D + 3u) / 4u);
		const std::uint32_t mean = a_frame.voxelActive ? kShadowMeanVoxel : (c.stormCount == 0u ? kShadowMeanWeather : (volcanic ? kShadowMeanVolcanic : kShadowMean));
		state.Open(frame, mean, 1, PhysicalSky::kShadowMeanHeights);
	}

	// The main view and cubemap pass.
	if (a_frame.voxelActive)
		if (const auto update = f.voxelCloudManager.PlanLighting(c.dirlightDir)) {
			Impl::Put(frame, kVoxelUpdateOffset, std::array<std::uint32_t, 4>{ (*update)[0], (*update)[1], 0u, 0u });
			state.Open(frame, kVoxelLighting, 64, 64, ((*update)[1] + 3u) / 4u);
		}
	state.Open(frame, kAmbientSH, 8);
	state.Open(frame, kAmbientReduce, 1);
	const bool ambient = c.ambientStrength > 0.f && (c.msAlbedo > 0.f || c.stormCount > 0u);
	if (ambient)
		state.Open(frame, kAmbientLighting, 1, 5);
	if (c.middleLayer.z > 0.f && f.middleBoundsDirty) {
		state.Open(frame, kMiddleBounds, PhysicalSky::kMiddleBoundsSize, PhysicalSky::kMiddleBoundsSize);
		for (std::uint32_t mip = 1; mip < PhysicalSky::kMiddleBoundsLevels; ++mip) {
			const auto groups = ((PhysicalSky::kMiddleBoundsSize >> mip) + 7u) / 8u;
			state.Open(frame, kMiddleBounds + mip, groups, groups);
		}
		f.middleBoundsDirty = false;
	}
	if (c.middleLayer.z > 0.f || a_frame.renderCirrus)
		state.Open(frame, kUpperSunDepth, 32, 32);
	if (!f.volMainHistoryValid || f.cloudHeightBoundsDirty) {
		state.Open(frame, kHeightBounds, 16, 16);
		state.Open(frame, kHeightBoundsReduce, 1);
		f.cloudHeightBoundsDirty = false;
	}
	if (ambient) {
		const bool resetField = !f.cloudAmbientValid;
		if (resetField) {
			state.Open(frame, kAmbientReset, 1);
			f.cloudAmbientIterations.fill(0);
			f.cloudAmbientUpdateIndex = 0;
			f.cloudAmbientValid = true;
		}
		static constexpr std::uint32_t schedule[] = { 0, 1, 0, 2, 0, 3 };
		const std::uint32_t cascade = schedule[f.cloudAmbientUpdateIndex++ % std::size(schedule)];
		const std::uint32_t width = cascade == 0 ? PhysicalSky::kCloudAmbientVolumeWidth : PhysicalSky::kCloudAmbientVolumeWidth / 2;
		const std::uint32_t depth = width / 2;
		const PhysicalSky::CloudAmbientVolumeCB volume{
			.phase = a_frame.ambientPhase,
			.cascade = cascade,
			.width = width,
			.reset = resetField ? 1u : 0u,
			.iterations = ++f.cloudAmbientIterations[cascade]
		};
		Impl::Put(frame, kAmbientOffset, volume);
		state.Open(frame, kAmbientBoundary, 1, 5);
		const std::uint32_t density = a_frame.voxelActive ? kAmbientDensityVoxel : (volcanic ? kAmbientDensityVolcanic : kAmbientDensity);
		state.Open(frame, density, width / 4, width / 4, depth / 4);
		state.Open(frame, cascade == 0 ? kAmbientSweep : kAmbientSweepFar, 2 * width, 2 * width, 26);
		state.Open(frame, kAmbientProject + cascade, width / 4, width / 4, depth / 4);
		state.Open(frame, kAmbientSourceCopy + cascade, 1);
		state.Open(frame, kAmbientLayoutIn, 1);
		state.Open(frame, kAmbientLayout, 1);
		state.Open(frame, kAmbientLayoutOut, 1);
		constexpr std::uint32_t nearViewGroups = (PhysicalSky::kCloudAmbientVolumeWidth / 4) * (PhysicalSky::kCloudAmbientVolumeWidth / 4) * (PhysicalSky::kCloudAmbientVolumeDepth / 4);
		constexpr std::uint32_t farViewGroups = nearViewGroups / 8;
		state.Open(frame, kAmbientView, nearViewGroups + (PhysicalSky::kCloudAmbientCascadeCount - 1) * farViewGroups);
	} else {
		f.cloudAmbientValid = false;
	}

	const auto lowW = a_frame.lowW, lowH = a_frame.lowH;
	const auto voxelRect = f.voxelCloudManager.ScreenRect(c.bottomZ, (a_frame.renderW + 3u) / 4u, (a_frame.renderH + 3u) / 4u);
	const bool voxelVisible = voxelRect[2] > voxelRect[0] && voxelRect[3] > voxelRect[1] && !mapMenu;
	f.lightning.QuerySource(nullptr);
	if (!mapMenu) {
		const PhysicalSky::CloudBoundaryCB boundary = {
			.gridOriginSpacing = { a_frame.boundaryOrigin.x, a_frame.boundaryOrigin.y, a_frame.boundarySpacing.x, a_frame.boundarySpacing.y },
			.fieldFrequencyWind = { c.lowNdfFrequency.x, c.lowNdfFrequency.y, a_frame.noiseWindOffset.x, a_frame.noiseWindOffset.y },
			.shearAltitude = { c.cloudShapeShear.x, c.cloudShapeShear.y, c.lowCloudBaseAltitude, c.lowCloudTopAltitude },
			.evolution = c.cloudEvolution,
			.frameDimensions = { a_frame.textureDim.x, a_frame.textureDim.y, static_cast<float>(lowW), static_cast<float>(lowH) },
			.cloudCurvatureRadius = c.cloudCurvatureRadius,
			.bottomZ = c.bottomZ,
			.gridCellCount = PhysicalSky::kCloudBoundaryCells,
			.cloudFrameIndex = f.volFrameIndex,
			.localRect = c.localNdfRect,
			.localAltitude = c.localNdfAltitude
		};
		Impl::Put(frame, kBoundaryOffset, boundary);
		frame.boundary = true;

		const auto stormRect = storms.ScreenRect(c.bottomZ, lowW, lowH, { c.lowCloudTraceBottomAltitude, c.lowCloudTraceTopAltitude }, c.cloudCurvatureRadius);
		const bool stormVisible = stormRect[2] > stormRect[0] && stormRect[3] > stormRect[1];
		Impl::Put(frame, kStormRectOffset, stormRect);
		if (stormVisible)
			state.Open(frame, kStormTiles, (lowW + 63u) / 64u, (lowH + 63u) / 64u);
		// Ordinary pixels keep the storm-free shader; with a storm in view it skips the storm's tiles.
		const bool weather = c.localNdfAltitude.z <= 0.f;
		const std::uint32_t ordinary = weather ? (stormVisible ? kMainWeatherTiles : kMainWeather) : (stormVisible ? kMainWithoutStormTiles : kMainWithoutStorm);
		state.Open(frame, ordinary, (lowW + 7u) / 8u, (lowH + 3u) / 4u);
		if (stormVisible) {
			const bool single = volcanic && storms.Count() == 1;
			const std::uint32_t groupWidth = volcanic ? 16u : 8u;
			const std::uint32_t groupHeight = volcanic && !single ? 4u : 8u;
			state.Open(frame, single ? kMainSingleVolcanic : (volcanic ? kMainVolcanic : kMainStorm), (stormRect[2] - stormRect[0] + groupWidth - 1u) / groupWidth,
				(stormRect[3] - stormRect[1] + groupHeight - 1u) / groupHeight);
		}
		if (voxelVisible) {
			Impl::Put(frame, kVoxelRectOffset, voxelRect);
			state.Open(frame, f.voxelCloudManager.Count() == 1 ? kVoxelViewSingle : kVoxelView, (lowW + 7u) / 8u, (lowH + 7u) / 8u);
			state.Open(frame, kVoxelCopy, 1);
		}
		if (c.middleLayer.z > 0.f || a_frame.renderCirrus) {
			// Below the middle layer, opaque foreground can hide whole tiles.
			const bool classify = c.middleLayer.z > 0.f && a_frame.camera.z - c.bottomZ < c.middleLayer.x;
			if (classify)
				state.Open(frame, kUpperTiles, (lowW + 7u) / 8u, (lowH + 7u) / 8u);
			state.Open(frame, classify ? kUpperViewTiles : kUpperView, (lowW + 7u) / 8u, (lowH + 7u) / 8u);
			state.Open(frame, kUpperCopy, 1);
		}
		state.Open(frame, kReproject, (a_frame.renderW + 7u) >> 3, (a_frame.renderH + 7u) >> 3);
		const int blurRadius = std::clamp(f.settings.cloudBlurRadius, 0, 2);
		if (blurRadius == 0)
			state.Open(frame, kFilterCopy, 1);
		else
			state.Open(frame, blurRadius == 2 ? kFilterWide : kFilter, (a_frame.renderW + 7u) >> 3, (a_frame.renderH + 7u) >> 3);
	} else {
		state.Open(frame, kMapMenuClear, 1);
	}
	if (!mapMenu && f.cbData.lightningColorIntensity.w > 0.f)
		state.Open(frame, kLightningComposite, (a_frame.renderW + 7u) / 8u, (a_frame.renderH + 7u) / 8u);
	state.Open(frame, a_frame.voxelActive ? kCubeVoxel : (volcanic ? kCubeVolcanic : kCube), PhysicalSky::kVolCubeSize / 4u, PhysicalSky::kVolCubeSize / 4u, 6);
	state.Open(frame, kCubeReproject, (PhysicalSky::kVolCubeSize + 7u) >> 3, (PhysicalSky::kVolCubeSize + 7u) >> 3, 6);
	if (f.lightning.HasSource())
		state.Open(frame, kLightningCube, (PhysicalSky::kVolCubeSize + 7u) / 8u, (PhysicalSky::kVolCubeSize + 7u) / 8u, 6);
	state.Open(frame, kHistories, 1);

	// The feature's history state, as the D3D11 pass leaves it.
	f.lightning.CloudFrameComplete(f.volMainHistoryValid && !mapMenu);
	f.volMainHistoryValid = true;
	f.volHistoryWind = f.volWind;
	f.cloudStormManager.CommitMotion();
	f.volHistoryViewProj = globals::game::frameBufferCached.GetCameraViewProj();
	f.volHistoryCamera = a_frame.camera;
	f.volHistoryCurvatureRadius = c.cloudCurvatureRadius;
	f.volHistoryFrameDim = mapMenu ? float2{} : a_frame.frameDim;
	++f.volFrameIndex;
}

