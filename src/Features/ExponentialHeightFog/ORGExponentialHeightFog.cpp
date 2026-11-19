#include "ORGExponentialHeightFog.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

bool ORGExponentialHeightFog::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
	return requested;
}

std::uint32_t ORGExponentialHeightFog::Debug()
{
	static const std::uint32_t s_mode = [] {
		const auto mode = Switches::Number("CS_ORG_FOG_DEBUG", 0);
		if (mode > 8) {
			logger::warn("[ORG] Exponential Height Fog: CS_ORG_FOG_DEBUG={} is not a mode (1 to 8); it is off", mode);
			return 0u;
		}
		if (mode)
			logger::info("[ORG] Exponential Height Fog: debug mode {}", mode);
		return static_cast<std::uint32_t>(mode);
	}();
	return s_mode;
}

ORGExponentialHeightFog& ORGExponentialHeightFog::Get()
{
	static ORGExponentialHeightFog s_fog;
	return s_fog;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Deferred.h"
#	include "Features/CloudShadows.h"
#	include "Features/CloudShadows/ORGCloudShadows.h"
#	include "Features/ExponentialHeightFog.h"
#	include "Features/IBL.h"
#	include "Features/IBL/ORGIBL.h"
#	include "Features/LightLimitFix.h"
#	include "Features/LightLimitFix/ORGLightCulling.h"
#	include "Features/LightLimitFix/ORGLocalShadows.h"
#	include "Features/PhysicalSky.h"
#	include "Features/PhysicalSky/ORGPhysicalSky.h"
#	include "Features/PhysicalSky/ORGVolumetricClouds.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "Features/Skylighting.h"
#	include "Features/Skylighting/ORGSkylighting.h"
#	include "Features/TerrainBlending/ORGTerrainBlending.h"
#	include "Features/TerrainShadows.h"
#	include "Features/TerrainShadows/ORGTerrainShadows.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "State.h"
#	include "Utils/D3D.h"
#	include "Utils/Game.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/VersionedTexture.h>

#	include <array>
#	include <cstddef>
#	include <functional>
#	include <optional>
#	include <string>
#	include <tuple>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.exp-height-fog";
	constexpr const char* kShaderDir = "ExponentialHeightFog/";
	// After IBL and Skylighting at the Prepass anchor. The graph orders the reads of their outputs anyway.
	constexpr std::int32_t kOrder = 300;

	/** @brief Readers return to the D3D11 views. */
	void WithdrawIntegrated()
	{
		PublishedResources::Withdraw(ORGExponentialHeightFog::kNearIntegratedId);
		PublishedResources::Withdraw(ORGExponentialHeightFog::kFarIntegratedId);
	}

	using ShaderBindings::Handle;

	// The frame's LATCH entries, plus the directional shadow light the upload pass copies into its buffer (t98).
	struct LatchData
	{
		ExponentialHeightFog::VolumetricFogCB fog;
		ExponentialHeightFog::EngineLocalShadows engineShadows;
		Deferred::DirectionalShadowLightData directionalShadow;
	};
	static_assert(offsetof(LatchData, engineShadows) % 16 == 0 && offsetof(LatchData, directionalShadow) % 16 == 0);

	// Push blocks in each resource list's order.
	struct VolumeBlock
	{
		SharedConstants::Handles shared;
		ShaderBindings::Latch fog;
		Handle output, depth;
	};
	static_assert(sizeof(VolumeBlock) == 32);

	struct IntegrationBlock
	{
		SharedConstants::Handles shared;
		ShaderBindings::Latch fog;
		Handle lightScattering, integrated, depth;
	};
	static_assert(sizeof(IntegrationBlock) == 36);

	struct ScatteringBlock  // with Light Limit Fix's tail
	{
		SharedConstants::Handles shared;
		ShaderBindings::Latch fog;
		Handle linearSampler, shadowSampler;
		Handle vBuffer, directionalShadowMap, history, conservativeDepth, previousConservativeDepth;
		Handle lightScattering;
		Handle directionalShadowLights;
		Handle depth, envIBL, skyIBL, probes, shadowHeights, cloudShadows;
		Handle physSkyTransmittance, physSkyShadowVolume, physSkyShadowMean;  // with PHYSICAL_SKY
		Handle lights, lightList, lightGrid, localShadows, localShadowMaps;
		Handle engineShadowMaps;
		ShaderBindings::Latch engineShadows;
	};
	// The whole push block: 32 words.
	static_assert(sizeof(ScatteringBlock) == 128);

	/** @brief One the client holds or imported, or one another client publishes by name. */
	struct Source
	{
		FrameAnchors::Texture resource;
		std::string name;
		// The shaders read the whole Texture2DArray, so use the whole-array view rather than the default first slice.
		bool wholeArray = false;

		explicit operator bool() const { return resource || !name.empty(); }
		bool operator==(const Source&) const = default;
	};

	enum Input : std::uint32_t
	{
		kDepth,
		kDirectionalShadowMap,
		kEnvIBL,
		kSkyIBL,
		kProbes,
		kShadowHeights,
		kCloudShadows,
		kPhysSkyTransmittance,
		kPhysSkyShadowVolume,
		kPhysSkyShadowMean,
		kLights,
		kLightList,
		kLightGrid,
		kLocalShadows,
		kLocalShadowMaps,
		kEngineShadowMaps,
		kInputs
	};

	bool SameExtent(const DirectX::XMUINT4& a, const DirectX::XMUINT4& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

	/** @brief A change makes a new client. Extents are not part of it, since a new extent resizes the volumes in place. */
	struct Shape
	{
		ExponentialHeightFog::GridSizes sizes;
		bool farEnabled = false;
		bool temporal = false;
		std::array<Source, kInputs> inputs;

		bool operator==(const Shape& a_other) const
		{
			return sizes.nearGroupReduce == a_other.sizes.nearGroupReduce && sizes.farGroupReduce == a_other.sizes.farGroupReduce &&
			       farEnabled == a_other.farEnabled && temporal == a_other.temporal && inputs == a_other.inputs;
		}
	};

	/** @brief Compiled as the D3D11 build compiles them. */
	struct Programs
	{
		std::shared_ptr<const ComputeProgram> depth, depthGroupReduce, material, scattering, integration;
	};

	/** @brief Made once at Setup and shared by every shape. */
	struct Fixed
	{
		Programs nearPrograms, farPrograms;
		org::OwnedDescriptorBinding linearSampler, shadowSampler;
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
	};

	enum class Step : std::uint8_t
	{
		Depth,
		Material,
		Scattering,
		Integration,
		Count
	};

	/** @brief The near volume's four steps, then the far volume's. */
	constexpr std::uint32_t GateEntry(bool a_far, Step a_step)
	{
		return (a_far ? static_cast<std::uint32_t>(Step::Count) : 0u) + static_cast<std::uint32_t>(a_step);
	}

	/**
	* @brief Live as long as the fog's graph path. Every texture is versioned, so a new extent is a new version and nothing rebuilds.
	* None is aliased, because a version's backing is placed when it is made, not at a build.
	*/
	struct VolumeTextures
	{
		std::unique_ptr<GraphResources::Texture3D> vBuffer, scattering, scatteringHistory, integrated;
		std::unique_ptr<GraphResources::Texture2D> conservativeDepth, conservativeDepthHistory;
		DirectX::XMUINT4 size{};  // the extent this frame's passes and group counts use

		bool Complete() const
		{
			return vBuffer && *vBuffer && scattering && *scattering && scatteringHistory && *scatteringHistory && integrated && *integrated &&
			       integrated->SRV() && conservativeDepth && *conservativeDepth && conservativeDepthHistory && *conservativeDepthHistory;
		}

		void Register(org::RenderGraph& a_graph) const
		{
			vBuffer->Register(a_graph);
			scattering->Register(a_graph);
			scatteringHistory->Register(a_graph);
			integrated->Register(a_graph);
			conservativeDepth->Register(a_graph);
			conservativeDepthHistory->Register(a_graph);
		}

		/** @brief Taken at the next frame boundary as a new version of each slot. */
		void RequestSize(const DirectX::XMUINT4& a_size)
		{
			for (auto* volume : { vBuffer.get(), scattering.get(), scatteringHistory.get(), integrated.get() })
				volume->RequestSize(a_size.x, a_size.y, a_size.z);
			for (auto* depth : { conservativeDepth.get(), conservativeDepthHistory.get() })
				depth->RequestSize(a_size.x, a_size.y);
		}

		/** @brief All textures take a new extent at the same frame boundary. */
		DirectX::XMUINT4 Current() const
		{
			const auto& desc = vBuffer->Desc();
			return { desc.Width, desc.Height, desc.Depth, 0 };
		}
	};

	/** @brief Made once, then resized in place. */
	struct Textures
	{
		VolumeTextures nearVolume, farVolume;
		std::unique_ptr<GraphResources::StructuredBuffer> directionalShadowLights;
	};

	/** @brief Versioned twins are declared through their slot, read at each declaration so a new slot is picked up. */
	template <class Declare>
	auto DeclareTwin(const GraphResources::Resource& a_twin, Declare&& a_declare)
	{
		if (const auto& slot = a_twin.Versioned())
			return a_declare(org::ResourceResolverAndRange(*slot));
		return a_declare(a_twin.Graph());
	}

	struct Volume
	{
		std::shared_ptr<const ComputeProgram> depthProgram, materialProgram, scatteringProgram, integrationProgram;
		VolumeTextures* textures = nullptr;  // kept alive by FogState::textures
		bool groupReduce = false;

		/** @brief As the D3D11 path dispatches it. Render thread. */
		std::array<std::uint32_t, 3> Groups(Step a_step) const
		{
			const auto& size = textures->size;
			const std::uint32_t groupsX = (size.x + 7) / 8;
			const std::uint32_t groupsY = (size.y + 7) / 8;
			const std::uint32_t groupsZ = (size.z + 3) / 4;
			switch (a_step) {
			case Step::Depth:
				return groupReduce ? std::array<std::uint32_t, 3>{ size.x, size.y, 1 } : std::array<std::uint32_t, 3>{ groupsX, groupsY, 1 };
			case Step::Integration:
				return { groupsX, groupsY, 1 };
			default:
				return { groupsX, groupsY, groupsZ };
			}
		}
	};

	/** @brief Immutable once registered. */
	struct FogState
	{
		Shape shape;
		Volume nearVolume, farVolume;
		std::shared_ptr<Textures> textures;  // the same object across builds
		std::shared_ptr<const Fixed> fixed;
	};

	org::DeclaredViewToken Read(org::PassBuilder& a_builder, const Source& a_source)
	{
		org::SrvView view{};
		if (a_source.wholeArray)
			view.variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull);
		if (a_source.resource)
			return a_builder.ShaderResource(a_source.resource, view).View();
		return a_builder.ShaderResource(org::ResourceIdentifier(a_source.name), view).View();
	}

	// ---- Passes ----

	struct DispatchBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken output;
		std::array<org::DeclaredViewToken, 6> own{};
		std::array<org::DeclaredViewToken, kInputs> inputs{};
		std::array<bool, kInputs> hasInput{};
	};

	struct DispatchFrame
	{
		std::shared_ptr<const FogState> state;
		const Volume* volume = nullptr;
		Step step = Step::Material;
		bool isFar = false;
	};

	/** @brief Groups come from the run gate, which the commit opens on runs that have the fog's frame. */
	class DispatchPass final : public org::TypedRenderGraphPass<DispatchPass, DispatchFrame, DispatchBindings>
	{
	public:
		DispatchPass(std::shared_ptr<const FogState> a_state, bool a_far, Step a_step) :
			state(std::move(a_state)), isFar(a_far), step(a_step) {}

		DispatchBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& v = *Of().textures;
			const auto uav = [&](const GraphResources::Resource& a_twin) {
				return DeclareTwin(a_twin, [&](const auto& a_resource) { return a_builder.UnorderedAccess(a_resource).View(); });
			};
			const auto srv = [&](const GraphResources::Resource& a_twin) {
				return DeclareTwin(a_twin, [&](const auto& a_resource) { return a_builder.ShaderResource(a_resource).View(); });
			};
			DispatchBindings b{};
			b.shared = SharedConstants::Declare(a_builder);
			switch (step) {
			case Step::Depth:
				b.output = uav(*v.conservativeDepth);
				b.inputs[kDepth] = Read(a_builder, state->shape.inputs[kDepth]);
				b.hasInput[kDepth] = true;
				break;
			case Step::Material:
				b.output = uav(*v.vBuffer);
				break;
			case Step::Scattering:
				b.output = uav(*v.scattering);
				b.own[0] = srv(*v.vBuffer);
				b.own[1] = srv(*v.scatteringHistory);
				b.own[2] = srv(*v.conservativeDepth);
				b.own[3] = srv(*v.conservativeDepthHistory);
				b.own[4] = a_builder.ShaderResource(state->textures->directionalShadowLights->Graph()).View();
				// The far volume's shader leaves out local lights.
				for (std::uint32_t i = 0; i < kInputs; ++i) {
					const bool local = i >= kLights && i <= kEngineShadowMaps;
					if (i == kDepth || (isFar && local) || !state->shape.inputs[i])
						continue;
					b.inputs[i] = Read(a_builder, state->shape.inputs[i]);
					b.hasInput[i] = true;
				}
				break;
			default:
				b.output = uav(*v.integrated);
				b.own[0] = srv(*v.scattering);
				break;
			}
			return b;
		}

		// The recording depends only on what the pass was made with. The gate decides each run.
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(state.get())); }

		DispatchFrame Prepare(const DispatchBindings&, const org::PassPrepareContext&) const { return { state, &Of(), step, isFar }; }

		static void Record(const DispatchBindings& a_bindings, const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& s = *a_frame.state;
			const auto& v = *a_frame.volume;
			const auto& fixed = *s.fixed;
			const org::RunRef run = a_recording.Run();
			const auto shared = SharedConstants::Index(a_recording, a_bindings.shared);
			const auto fog = ShaderBindings::LatchOf(*fixed.latch, run, static_cast<std::uint32_t>(offsetof(LatchData, fog)));
			const auto input = [&](Input a_input) -> Handle { return a_bindings.hasInput[a_input] ? a_recording.Index(a_bindings.inputs[a_input]) : 0; };
			const std::uint32_t entry = GateEntry(a_frame.isFar, a_frame.step);
			switch (a_frame.step) {
			case Step::Depth:
				{
					const VolumeBlock block{ shared, fog, a_recording.Index(a_bindings.output), input(kDepth) };
					fixed.gate->Dispatch(a_recording, *v.depthProgram, ShaderBindings::Data(block), entry);
					break;
				}
			case Step::Material:
				{
					const VolumeBlock block{ shared, fog, a_recording.Index(a_bindings.output), 0 };
					fixed.gate->Dispatch(a_recording, *v.materialProgram, ShaderBindings::Data(block), entry);
					break;
				}
			case Step::Scattering:
				{
					ScatteringBlock block{};
					block.shared = shared;
					block.fog = fog;
					block.linearSampler = fixed.linearSampler.Index();
					block.shadowSampler = fixed.shadowSampler.Index();
					block.vBuffer = a_recording.Index(a_bindings.own[0]);
					block.directionalShadowMap = input(kDirectionalShadowMap);
					block.history = a_recording.Index(a_bindings.own[1]);
					block.conservativeDepth = a_recording.Index(a_bindings.own[2]);
					block.previousConservativeDepth = a_recording.Index(a_bindings.own[3]);
					block.lightScattering = a_recording.Index(a_bindings.output);
					block.directionalShadowLights = a_recording.Index(a_bindings.own[4]);
					block.envIBL = input(kEnvIBL);
					block.skyIBL = input(kSkyIBL);
					block.probes = input(kProbes);
					block.shadowHeights = input(kShadowHeights);
					block.cloudShadows = input(kCloudShadows);
					block.physSkyTransmittance = input(kPhysSkyTransmittance);
					block.physSkyShadowVolume = input(kPhysSkyShadowVolume);
					block.physSkyShadowMean = input(kPhysSkyShadowMean);
					block.lights = input(kLights);
					block.lightList = input(kLightList);
					block.lightGrid = input(kLightGrid);
					block.localShadows = input(kLocalShadows);
					block.localShadowMaps = input(kLocalShadowMaps);
					block.engineShadowMaps = input(kEngineShadowMaps);
					block.engineShadows = ShaderBindings::LatchOf(*fixed.latch, run, static_cast<std::uint32_t>(offsetof(LatchData, engineShadows)));
					fixed.gate->Dispatch(a_recording, *v.scatteringProgram, ShaderBindings::Data(block), entry);
					break;
				}
			default:
				{
					const IntegrationBlock block{ shared, fog, a_recording.Index(a_bindings.own[0]), a_recording.Index(a_bindings.output), 0 };
					fixed.gate->Dispatch(a_recording, *v.integrationProgram, ShaderBindings::Data(block), entry);
					break;
				}
			}
		}

	private:
		const Volume& Of() const { return isFar ? state->farVolume : state->nearVolume; }

		std::shared_ptr<const FogState> state;
		bool isFar;
		Step step;
	};

	// As Deferred::CopyShadowLightData maps it on D3D11. On a run without the fog's frame it copies zeros nobody reads.
	struct UploadBindings
	{
		org::ResourceBindingToken buffer;
	};

	struct UploadFrame
	{
		std::shared_ptr<const FogState> state;
	};

	class UploadPass final : public org::TypedRenderGraphPass<UploadPass, UploadFrame, UploadBindings>
	{
	public:
		explicit UploadPass(std::shared_ptr<const FogState> a_state) :
			state(std::move(a_state)) {}

		UploadBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.CopyDestination(state->textures->directionalShadowLights->Graph()) };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(state.get())); }

		UploadFrame Prepare(const UploadBindings&, const org::PassPrepareContext&) const { return { state }; }

		static void Record(const UploadBindings& a_bindings, const UploadFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto region = a_frame.state->fixed->latch->At(a_recording.Run());
			a_recording.Commands().CopyBufferRegion(a_recording.Resolve(a_bindings.buffer).GetHandle(), 0,
				region.buffer->GetAPIResource().GetHandle(), region.offset + offsetof(LatchData, directionalShadow),
				sizeof(Deferred::DirectionalShadowLightData));
		}

	private:
		std::shared_ptr<const FogState> state;
	};

	// Copies into the histories, as the D3D11 path does. On a run without the fog's frame the history is never read, since
	// the next graph frame is not consecutive. Each copy takes the whole version it resolves, so a resize needs no new pass.
	struct HistoryBindings
	{
		org::ResourceBindingToken scattering, scatteringHistory, depth, depthHistory;
	};

	struct HistoryFrame
	{};

	class HistoryPass final : public org::TypedRenderGraphPass<HistoryPass, HistoryFrame, HistoryBindings>
	{
	public:
		HistoryPass(std::shared_ptr<const FogState> a_state, bool a_far) :
			state(std::move(a_state)), isFar(a_far) {}

		HistoryBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& v = *Of().textures;
			const auto source = [&](const GraphResources::Resource& a_twin) {
				return DeclareTwin(a_twin, [&](const auto& a_resource) { return a_builder.CopySource(a_resource); });
			};
			const auto destination = [&](const GraphResources::Resource& a_twin) {
				return DeclareTwin(a_twin, [&](const auto& a_resource) { return a_builder.CopyDestination(a_resource); });
			};
			return { source(*v.scattering), destination(*v.scatteringHistory), source(*v.conservativeDepth), destination(*v.conservativeDepthHistory) };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(state.get())); }

		HistoryFrame Prepare(const HistoryBindings&, const org::PassPrepareContext&) const { return {}; }

		static void Record(const HistoryBindings& a_bindings, const HistoryFrame&, org::PassRecordContext& a_recording)
		{
			// Zero extents copy the whole mip in both backends. A texture and its history always match in size.
			auto& commands = a_recording.Commands();
			const auto copy = [&](org::ResourceBindingToken a_from, org::ResourceBindingToken a_to) {
				const rhi::TextureCopyRegion to{ .texture = a_recording.Resolve(a_to).GetHandle(), .depth = 0 };
				const rhi::TextureCopyRegion from{ .texture = a_recording.Resolve(a_from).GetHandle(), .depth = 0 };
				commands.CopyTextureRegion(to, from);
			};
			copy(a_bindings.scattering, a_bindings.scatteringHistory);
			copy(a_bindings.depth, a_bindings.depthHistory);
		}

	private:
		const Volume& Of() const { return isFar ? state->farVolume : state->nearVolume; }

		std::shared_ptr<const FogState> state;
		bool isFar;
	};

	// Clears the far integrated volume to no fog, as the D3D11 path does with the far volume off. Same result every run.
	struct ClearBindings
	{
		org::ResourceBindingToken resource;
		org::DeclaredViewToken shaderVisible, cpuVisible;
	};

	struct ClearFrame
	{};

	class ClearPass final : public org::TypedRenderGraphPass<ClearPass, ClearFrame, ClearBindings>
	{
	public:
		explicit ClearPass(std::shared_ptr<const FogState> a_state) :
			state(std::move(a_state)) {}

		ClearBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto use = DeclareTwin(*state->textures->farVolume.integrated,
				[&](const auto& a_resource) { return a_builder.UnorderedAccessClear(a_resource); });
			return { use.resources.front(), use.views[0], use.views[1] };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(state.get())); }

		ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const { return {}; }

		static void Record(const ClearBindings& a_bindings, const ClearFrame&, org::PassRecordContext& a_recording)
		{
			rhi::UavClearInfo info{};
			info.shaderVisible = a_recording.View(a_bindings.shaderVisible);
			info.cpuVisible = a_recording.View(a_bindings.cpuVisible);
			info.resource = a_recording.Resolve(a_bindings.resource);
			const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			a_recording.Commands().ClearUavFloat(info, rhi::UavClearFloat(clear));
		}

	private:
		std::shared_ptr<const FogState> state;
	};

	// ---- Inputs ----

	/** @brief By published id while a_view is that id's export, so the read is ordered after the producer. Empty otherwise. */
	Source Published(std::string_view a_id, ID3D11ShaderResourceView* a_view)
	{
		if (PublishedResources::IsExportOf(a_view, a_id))
			return { nullptr, std::string(a_id) };
		return {};
	}

	/** @brief The engine's depth-stencil import when it is one of the engine's, else its own. */
	Source ImportDepthView(ID3D11ShaderResourceView* a_view, const char* a_id)
	{
		if (!a_view || !globals::game::renderer)
			return {};
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture)
			return {};
		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		a_view->GetDesc(&viewDesc);
		const bool wholeArray = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		// Either side may be a tripwire handle or DXVK's object, so compare what they wrap.
		using RenderGraph::Engine::Tripwire::Unwrap;
		const auto& depthStencils = globals::game::renderer->GetDepthStencilData().depthStencils;
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); ++i)
			if (depthStencils[i].texture && Unwrap(depthStencils[i].texture) == Unwrap(texture.get()))
				return { FrameAnchors::EngineDepth(i), {}, wholeArray };
		RE::BSGraphics::DepthStencilData target{};
		target.texture = texture.get();
		target.depthSRV = a_view;
		return { FrameAnchors::ImportDepth(target, a_id), {}, wholeArray };
	}

	/** @brief As Util::GetCurrentSceneDepthSRV(true). */
	Source SceneDepth(ID3D11ShaderResourceView* a_view)
	{
		if (!a_view || !globals::game::renderer)
			return {};
		using RenderGraph::Engine::Tripwire::Unwrap;
		if (Unwrap(a_view) == Unwrap(globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV))
			return { FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY), {} };
		return Published(ORGTerrainBlending::kBlendedDepth16Id, a_view);
	}

	D3D11_TEXTURE3D_DESC VolumeDesc(const DirectX::XMUINT4& a_size, DXGI_FORMAT a_format)
	{
		D3D11_TEXTURE3D_DESC desc{};
		desc.Width = a_size.x;
		desc.Height = a_size.y;
		desc.Depth = a_size.z;
		desc.MipLevels = 1;
		desc.Format = a_format;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		return desc;
	}

	D3D11_TEXTURE2D_DESC DepthDesc(const DirectX::XMUINT4& a_size)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_size.x;
		desc.Height = a_size.y;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		return desc;
	}

	/** @brief ComputeProgram adds the D3D11 build's defines. */
	std::shared_ptr<const ComputeProgram> LoadProgram(const char* a_source, const std::vector<std::string>& a_defines, std::uint32_t a_words)
	{
		std::vector<std::pair<std::string, std::string>> defines;
		for (const auto& name : a_defines)
			defines.emplace_back(name, "");
		return ComputeProgram::Load(RenderGraphRuntime::Get().Host()->Device(),
			{ .source = std::string(kShaderDir) + a_source, .constantWords = a_words, .defines = RenderGraph::AnchorPasses::Defines(defines) });
	}

	/** @brief False, logged, when one does not build. */
	bool LoadPrograms(Programs& a_programs, bool a_far)
	{
		std::vector<std::string> volumeDefines;
		if (a_far)
			volumeDefines.emplace_back("VOLUMETRIC_FOG_FAR_GRID");
		auto scatteringDefines = volumeDefines;
		if (globals::features::lightLimitFix.loaded)
			scatteringDefines.emplace_back("LIGHT_LIMIT_FIX");
		if (globals::features::terrainShadows.loaded)
			scatteringDefines.emplace_back("TERRAIN_SHADOWS");
		if (globals::features::cloudShadows.loaded)
			scatteringDefines.emplace_back("CLOUD_SHADOWS");
		// As the D3D11 build. Its LUT and cloud shadow inputs are Physical Sky's published twins (Prepass).
		if (globals::features::physicalSky.loaded)
			scatteringDefines.emplace_back("PHYSICAL_SKY");
		auto groupDefines = volumeDefines;
		groupDefines.emplace_back("GROUP_REDUCE");

		a_programs.depth = LoadProgram("VolumetricFogConservativeDepthCS.hlsl", volumeDefines, ShaderBindings::Words<VolumeBlock>());
		a_programs.depthGroupReduce = LoadProgram("VolumetricFogConservativeDepthCS.hlsl", groupDefines, ShaderBindings::Words<VolumeBlock>());
		a_programs.material = LoadProgram("VolumetricFogMaterialCS.hlsl", volumeDefines, ShaderBindings::Words<VolumeBlock>());
		a_programs.scattering = LoadProgram("VolumetricFogLightScatteringCS.hlsl", scatteringDefines, ShaderBindings::Words<ScatteringBlock>());
		a_programs.integration = LoadProgram("VolumetricFogIntegrationCS.hlsl", volumeDefines, ShaderBindings::Words<IntegrationBlock>());
		if (!a_programs.depth || !a_programs.depthGroupReduce || !a_programs.material || !a_programs.scattering || !a_programs.integration) {
			logger::error("[ORG] Exponential Height Fog: a {} volume program could not be built", a_far ? "far" : "near");
			return false;
		}
		return true;
	}
}

struct ORGExponentialHeightFog::Impl
{
	std::shared_ptr<const Fixed> fixed;
	// Made at the first build and kept. A new extent resizes them, a new shape only makes new passes.
	std::shared_ptr<Textures> textures;
	std::shared_ptr<const FogState> state;
	// Written at Prepass and latched by the commit.
	std::optional<LatchData> pending;
	bool hasScatteringHistory = false, hasDepthHistory = false, hasScatteringHistoryFar = false, hasDepthHistoryFar = false;
	std::uint32_t lastFrame = UINT32_MAX;
	bool ranLastFrame = false;
	std::string fallbackReason;  // why the last frame ran on D3D11, logged when it changes
	bool reportedConstants = false;  // the first graph frame's constants are logged once
	const FogState* reportedUnbound = nullptr;  // the last state Commit turned flags off for, logged once each
	// The integrated volumes' versions that a submitted run wrote. A new image holds whatever its memory held.
	// The D3D11 readers get a version only after a run wrote it, so they never read an unwritten one.
	std::weak_ptr<org::Resource> writtenNear, writtenFar;

	/** @brief Whether a submitted run wrote the current versions of both integrated volumes. Render thread. */
	bool Written() const
	{
		if (!textures)
			return false;
		const auto same = [](const std::weak_ptr<org::Resource>& a_written, const std::shared_ptr<org::Resource>& a_current) {
			return a_current && !a_written.owner_before(a_current) && !a_current.owner_before(a_written);
		};
		return same(writtenNear, textures->nearVolume.integrated->Graph()) && same(writtenFar, textures->farVolume.integrated->Graph());
	}

	/** @brief After a submitted run of a_state. Its integration (or the far clear) wrote the volumes' current versions. */
	void Executed(const FogState& a_state)
	{
		const bool before = Written();
		writtenNear = a_state.textures->nearVolume.integrated->Graph();
		writtenFar = a_state.textures->farVolume.integrated->Graph();
		if (!before)
			logger::info("[ORG] Exponential Height Fog: a run wrote the volumes at frame {}; the engine's draws and DCLF read them from the next Prepass",
				globals::state->frameCount);
	}

	void MakeVolume(Volume& a_volume, VolumeTextures& a_textures, bool a_groupReduce, bool a_far) const
	{
		const auto& programs = a_far ? fixed->farPrograms : fixed->nearPrograms;
		a_volume.textures = &a_textures;
		a_volume.groupReduce = a_groupReduce;
		a_volume.depthProgram = a_groupReduce ? programs.depthGroupReduce : programs.depth;
		a_volume.materialProgram = programs.material;
		a_volume.scatteringProgram = programs.scattering;
		a_volume.integrationProgram = programs.integration;
	}

	/** @brief False, logged, when one could not be made. */
	static bool MakeVolumeTextures(VolumeTextures& a_volume, const DirectX::XMUINT4& a_size, bool a_far)
	{
		const std::string prefix = a_far ? "ExponentialHeightFog::Far" : "ExponentialHeightFog::";
		// Versioned so a new extent is a new version. Not aliased, since a version's backing is made with it.
		const GraphResources::Options own{ .lifetime = GraphResources::Lifetime::Persistent, .versioned = true };
		const GraphResources::Options history{ .lifetime = GraphResources::Lifetime::History, .versioned = true };
		// The engine's draws read these through D3D11, so they are exported and never aliased.
		const GraphResources::Options exported{ .lifetime = GraphResources::Lifetime::Transient, .d3d11 = GraphResources::Export::SRV, .versioned = true };
		a_volume.size = a_size;
		a_volume.vBuffer = std::make_unique<GraphResources::Texture3D>(VolumeDesc(a_size, DXGI_FORMAT_R16G16B16A16_FLOAT), prefix + "VBufferA", own);
		a_volume.scattering = std::make_unique<GraphResources::Texture3D>(VolumeDesc(a_size, DXGI_FORMAT_R32G32B32A32_FLOAT), prefix + "LightScattering", own);
		a_volume.scatteringHistory = std::make_unique<GraphResources::Texture3D>(VolumeDesc(a_size, DXGI_FORMAT_R32G32B32A32_FLOAT), prefix + "LightScatteringHistory", history);
		a_volume.integrated = std::make_unique<GraphResources::Texture3D>(VolumeDesc(a_size, DXGI_FORMAT_R16G16B16A16_FLOAT),
			std::string(a_far ? ORGExponentialHeightFog::kFarIntegratedId : ORGExponentialHeightFog::kNearIntegratedId), exported);
		a_volume.conservativeDepth = std::make_unique<GraphResources::Texture2D>(DepthDesc(a_size), prefix + "ConservativeDepth", own);
		a_volume.conservativeDepthHistory = std::make_unique<GraphResources::Texture2D>(DepthDesc(a_size), prefix + "ConservativeDepthHistory", history);
		if (!a_volume.Complete()) {
			logger::error("[ORG] Exponential Height Fog: the {} volume's textures could not be made", a_far ? "far" : "near");
			return false;
		}
		return true;
	}

	/** @brief Published once. Readers follow the versions, so a resize never publishes again. False, logged, on failure. */
	bool MakeTextures(const ExponentialHeightFog::GridSizes& a_sizes)
	{
		auto made = std::make_shared<Textures>();
		if (!MakeVolumeTextures(made->nearVolume, a_sizes.nearSize, false) || !MakeVolumeTextures(made->farVolume, a_sizes.farSize, true))
			return false;
		D3D11_BUFFER_DESC bufferDesc{};
		bufferDesc.Usage = D3D11_USAGE_DEFAULT;
		bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bufferDesc.StructureByteStride = sizeof(Deferred::DirectionalShadowLightData);
		bufferDesc.ByteWidth = sizeof(Deferred::DirectionalShadowLightData);
		made->directionalShadowLights = std::make_unique<GraphResources::StructuredBuffer>(bufferDesc, 1, "ExponentialHeightFog::DirectionalShadowLights",
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Transient });
		if (!*made->directionalShadowLights) {
			logger::error("[ORG] Exponential Height Fog: the shadow light buffer could not be made");
			return false;
		}
		// DCLF's colour passes read them by name, ordered after the integration.
		if (!PublishedResources::Publish(*made->nearVolume.integrated) || !PublishedResources::Publish(*made->farVolume.integrated)) {
			logger::error("[ORG] Exponential Height Fog: the integrated volumes could not be published");
			return false;
		}
		textures = std::move(made);
		ResetHistories();
		return true;
	}

	/** @brief The new sizes take effect at the next frame boundary. A volume that changed extent restarts its history. */
	void FollowSizes(const ExponentialHeightFog::GridSizes& a_sizes)
	{
		bool resized = false;
		for (auto [volume, size] : { std::pair{ &textures->nearVolume, a_sizes.nearSize }, std::pair{ &textures->farVolume, a_sizes.farSize } }) {
			volume->RequestSize(size);
			if (const auto current = volume->Current(); !SameExtent(volume->size, current)) {
				volume->size = current;
				resized = true;
			}
		}
		if (resized) {
			// The new versions hold nothing to reproject from.
			ResetHistories();
			const auto& nearSize = textures->nearVolume.size;
			const auto& farSize = textures->farVolume.size;
			logger::info("[ORG] Exponential Height Fog: the volumes are new versions (near {}x{}x{}, far {}x{}x{})", nearSize.x, nearSize.y, nearSize.z, farSize.x,
				farSize.y, farSize.z);
		}
	}

	void ResetHistories()
	{
		hasScatteringHistory = hasDepthHistory = hasScatteringHistoryFar = hasDepthHistoryFar = false;
		lastFrame = UINT32_MAX;
	}

	void Commit(const FogState& a_state)
	{
		if (!pending)
			return;
		const auto& shape = *a_state.fixed;
		// Prepass built the constants for the newest shape. This run executes a_state, which can be a build behind it.
		// Its push blocks hold handle 0 for an input it does not have, so its constants may only name inputs it binds.
		// Otherwise a flag reads heap slot 0 as that input: IBL's SH from there goes unbounded into the scattering.
		LatchData data = *pending;
		const auto& inputs = a_state.shape.inputs;
		std::uint32_t bound = 0;
		if (inputs[kDirectionalShadowMap])
			bound |= 1u;
		if (inputs[kDepth])
			bound |= 2u | 16u;
		if (inputs[kEnvIBL] && inputs[kSkyIBL])
			bound |= 4u;
		if (inputs[kProbes])
			bound |= 8u;
		if (inputs[kLights] && inputs[kLightList] && inputs[kLightGrid])
			bound |= 32u;
		if (inputs[kLocalShadows] && inputs[kLocalShadowMaps])
			bound |= 64u;
		if (inputs[kEngineShadowMaps])
			bound |= 128u;
		const std::uint32_t unbound = (data.fog.gridSizeAndFlags.w | data.fog.farGridSizeAndFlags.w) & ~bound;
		if (unbound && reportedUnbound != &a_state) {
			reportedUnbound = &a_state;
			logger::info("[ORG] Exponential Height Fog: this run executes a build without inputs its constants name (flags {:#x}); they are off for it",
				unbound);
		}
		data.fog.gridSizeAndFlags.w &= bound;
		data.fog.farGridSizeAndFlags.w &= bound;
		shape.latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentRun(), 0, data);
		pending.reset();
		const auto open = [&](const Volume& a_volume, bool a_far) {
			for (std::uint32_t step = 0; step < static_cast<std::uint32_t>(Step::Count); ++step) {
				const auto groups = a_volume.Groups(static_cast<Step>(step));
				shape.gate->OpenDispatch(GateEntry(a_far, static_cast<Step>(step)), groups[0], groups[1], groups[2]);
			}
		};
		open(a_state.nearVolume, false);
		if (a_state.shape.farEnabled)
			open(a_state.farVolume, true);
		// The histories hold data only once a run with history passes copied into them. The state committed here is the one this
		// run executes, which lags a new registration by a build. Marking them at Prepass let the next frame read a history no pass
		// had written: recycled memory, kept by the temporal blend (huge but finite values that overflow the half-float volume).
		const bool temporal = a_state.shape.temporal;
		hasScatteringHistory = temporal;
		hasDepthHistory = temporal && static_cast<bool>(a_state.shape.inputs[kDepth]);
		hasScatteringHistoryFar = temporal && a_state.shape.farEnabled;
		hasDepthHistoryFar = hasScatteringHistoryFar && static_cast<bool>(a_state.shape.inputs[kDepth]);
		lastFrame = globals::state->frameCount;
	}

	/** @brief Only a change of inputs, history or group reduction gets here. A new extent alone is FollowSizes. False, logged, on failure. */
	bool Build(const Shape& a_shape)
	{
		if (!textures && !MakeTextures(a_shape.sizes))
			return false;
		auto built = std::make_shared<FogState>();
		built->shape = a_shape;
		built->fixed = fixed;
		built->textures = textures;
		MakeVolume(built->nearVolume, textures->nearVolume, a_shape.sizes.nearGroupReduce, false);
		MakeVolume(built->farVolume, textures->farVolume, a_shape.sizes.farGroupReduce, true);

		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		const std::shared_ptr<const FogState> shared = built;
		std::int32_t order = kOrder;
		const auto add = [&](std::string a_name, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make) {
			client.passes.push_back({ .name = "cs.exp-height-fog." + std::move(a_name), .anchor = FrameAnchors::Anchor::Prepass, .order = order++, .kind = a_kind, .make = std::move(a_make) });
		};
		add("upload", FrameAnchors::PassKind::Copy, [shared] { return std::make_shared<UploadPass>(shared); });
		const auto addVolume = [&](bool a_far) {
			const std::string volume = a_far ? "far." : "near.";
			constexpr std::array<std::pair<Step, const char*>, 4> steps{ { { Step::Depth, "depth" }, { Step::Material, "material" },
				{ Step::Scattering, "scattering" }, { Step::Integration, "integration" } } };
			for (const auto& entry : steps) {
				const Step step = entry.first;
				add(volume + entry.second, FrameAnchors::PassKind::Compute, [shared, a_far, step] { return std::make_shared<DispatchPass>(shared, a_far, step); });
			}
			if (a_shape.temporal)
				add(volume + "history", FrameAnchors::PassKind::Copy, [shared, a_far] { return std::make_shared<HistoryPass>(shared, a_far); });
		};
		addVolume(false);
		if (a_shape.farEnabled)
			addVolume(true);
		else
			add("far.clear", FrameAnchors::PassKind::Compute, [shared] { return std::make_shared<ClearPass>(shared); });
		client.registerResources = [shared](org::RenderGraph& a_graph) {
			shared->textures->nearVolume.Register(a_graph);
			shared->textures->farVolume.Register(a_graph);
			shared->textures->directionalShadowLights->Register(a_graph);
		};
		client.commit = [this, shared](const FrameAnchors::Point&) { Commit(*shared); };
		// Only a submission confirms the volumes were written. It comes only for runs whose graph holds this registration.
		client.executed = [this, shared](const FrameAnchors::Point&) { Executed(*shared); };
		// The fog has work only when Prepass gave the graph this frame.
		client.active = [this] { return pending.has_value(); };
		client.sharedConstants = true;
		client.gates = { fixed->gate };
		if (!FrameAnchors::Register(std::move(client)))
			return false;
		this->state = std::move(built);
		logger::info("[ORG] Exponential Height Fog runs on the render graph (near {}x{}x{}, far {}x{}x{}{}, {})", a_shape.sizes.nearSize.x,
			a_shape.sizes.nearSize.y, a_shape.sizes.nearSize.z, a_shape.sizes.farSize.x, a_shape.sizes.farSize.y, a_shape.sizes.farSize.z,
			a_shape.farEnabled ? "" : " off", a_shape.temporal ? "temporal" : "no history");
		return true;
	}
};

ORGExponentialHeightFog::~ORGExponentialHeightFog() = default;

void ORGExponentialHeightFog::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Prepass))
		logger::warn("[ORG] Exponential Height Fog: the Prepass anchor could not be reserved; the fog stays on D3D11");
}

bool ORGExponentialHeightFog::Setup()
{
	impl.reset();
	if (!Requested() || failed)
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.IsActive() || !runtime.Host() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Prepass)) {
		logger::warn("[ORG] Exponential Height Fog: the render graph or its Prepass anchor is not available; the fog stays on D3D11");
		return false;
	}
	auto fixed = std::make_shared<Fixed>();
	if (!LoadPrograms(fixed->nearPrograms, false) || !LoadPrograms(fixed->farPrograms, true)) {
		logger::error("[ORG] Exponential Height Fog stays on D3D11");
		return false;
	}
	// Linear clamp, and the comparison sampler for the directional shadow map.
	fixed->linearSampler = ShaderBindings::CreateSamplerFromD3D11(ExponentialHeightFog::SamplerDesc(false));
	fixed->shadowSampler = ShaderBindings::CreateSamplerFromD3D11(ExponentialHeightFog::SamplerDesc(true));
	fixed->latch = std::make_shared<org::LatchBlock>("cs.exp-height-fog.latch", static_cast<std::uint32_t>(sizeof(LatchData)));
	fixed->gate = RenderGraph::RunGate::Create("cs.exp-height-fog.gate", 2 * static_cast<std::uint32_t>(Step::Count));
	if (!fixed->linearSampler || !fixed->shadowSampler || !fixed->gate) {
		logger::error("[ORG] Exponential Height Fog: a sampler or the run gate could not be made; the fog stays on D3D11");
		return false;
	}
	impl = std::make_unique<Impl>();
	impl->fixed = std::move(fixed);
	logger::info("[ORG] Exponential Height Fog: the volumetric fog can run on the render graph");
	return true;
}

bool ORGExponentialHeightFog::IsActive() const
{
	return impl && !failed && RenderGraphRuntime::Get().IsActive();
}

bool ORGExponentialHeightFog::Prepass()
{
	if (!IsActive() || Debug() == 3)
		return false;
	auto& fog = globals::features::exponentialHeightFog;
	impl->pending.reset();

	// A frame whose inputs the graph cannot read runs on D3D11.
	Shape shape;
	shape.sizes = fog.ComputeGridSizes();
	// Mode 8 runs without history, so no froxel reads a history texture.
	shape.temporal = Util::GetTemporal() && Debug() != 8;
	const auto fallback = [&](const char* a_reason) {
		if (impl->fallbackReason != a_reason) {
			logger::info("[ORG] Exponential Height Fog: this frame runs on D3D11 ({})", a_reason);
			impl->fallbackReason = a_reason;
		}
		impl->ranLastFrame = false;
		return false;
	};

	ExponentialHeightFog::ConstantInputs inputs;
	inputs.sizes = shape.sizes;

	shape.inputs[kDepth] = SceneDepth(Util::GetCurrentSceneDepthSRV(true));
	if (!shape.inputs[kDepth])
		return fallback("the scene depth is missing or cannot be imported");
	inputs.depth = true;

	LatchData data{};
	const bool hasShadowLight = globals::deferred && globals::deferred->GetDirectionalShadowLightData(data.directionalShadow);
	// Null unless this frame drew the shadow mask, as the D3D11 path reads it.
	auto* directionalShadowMap = globals::deferred ? globals::deferred->GetDirectionalShadowMap() : nullptr;
	if (directionalShadowMap && hasShadowLight) {
		shape.inputs[kDirectionalShadowMap] = ImportDepthView(directionalShadowMap, "cs.exp-height-fog.directional-shadow-map");
		if (!shape.inputs[kDirectionalShadowMap])
			return fallback("the directional shadow map cannot be imported");
		inputs.directionalShadow = true;
	}

	auto& ibl = globals::features::ibl;
	if (ibl.loaded && ibl.settings.EnableIBL != 0 && !ibl.IsDisabledForCurrentScene() && ibl.EnvIBLSRV() && ibl.SkyIBLSRV()) {
		shape.inputs[kEnvIBL] = Published(ORGIBL::kEnvId, ibl.EnvIBLSRV());
		shape.inputs[kSkyIBL] = Published(ORGIBL::kSkyId, ibl.SkyIBLSRV());
		if (shape.inputs[kEnvIBL] && shape.inputs[kSkyIBL])
			inputs.ibl = true;
		else {
			// IBL's SH views are not the exports, so the fog runs without IBL, which its constants turn off.
			shape.inputs[kEnvIBL] = {};
			shape.inputs[kSkyIBL] = {};
		}
	}

	auto& skylighting = globals::features::skylighting;
	if (skylighting.loaded && skylighting.ProbeArraySRV()) {
		// A 3D texture, so only the graph's own probe volume can be read.
		if (!PublishedResources::IsExportOf(skylighting.ProbeArraySRV(), ORGSkylighting::kProbeArrayId))
			return fallback("Skylighting's probes are on D3D11");
		shape.inputs[kProbes] = { nullptr, std::string(ORGSkylighting::kProbeArrayId) };
		inputs.skylighting = true;
	}

	// Without the sky the shader skips terrain and cloud shadows, and Cloud Shadows binds nothing.
	const bool skyless = Util::IsInterior() || globals::state->isMapMenuOpen ||
	                     (globals::game::sky && globals::game::sky->flags.any(RE::Sky::Flags::kHideSky));

	auto& terrainShadows = globals::features::terrainShadows;
	if (terrainShadows.loaded && !skyless) {
		shape.inputs[kShadowHeights] = Published(ORGTerrainShadows::kShadowHeightId, terrainShadows.GetShadowHeightSRV());
		// The shader samples the heights without a null check, so a frame without them runs on D3D11.
		if (!shape.inputs[kShadowHeights])
			return fallback("Terrain Shadows' heights are missing or not published");
	}

	auto& cloudShadows = globals::features::cloudShadows;
	if (cloudShadows.loaded && !skyless) {
		// Use the cubemap Cloud Shadows binds at t25 for the frame, not whatever the slot holds now.
		shape.inputs[kCloudShadows] = Published(ORGCloudShadows::kOcclusionId, cloudShadows.OcclusionSRV());
		if (!shape.inputs[kCloudShadows])
			return fallback("Cloud Shadows' texture is missing or not published");
	}

	// The scattering reads Physical Sky's transmittance LUT, and with the clouds on their shadow volume and its mean.
	auto& physicalSky = globals::features::physicalSky;
	if (physicalSky.loaded && physicalSky.texTrLut) {
		shape.inputs[kPhysSkyTransmittance] = Published(ORGPhysicalSky::kTransmittanceLutId, physicalSky.texTrLut->srv.get());
		if (!shape.inputs[kPhysSkyTransmittance])
			return fallback("Physical Sky's LUTs are on D3D11");
		// The shader reads them only with the clouds on (enableVolumetricClouds). They are the cloud renderer's twins.
		if (physicalSky.VolumetricCloudsEnabled() && physicalSky.texShadowVolume && physicalSky.texCloudShadowMean && ORGVolumetricClouds::OutputsWritten()) {
			shape.inputs[kPhysSkyShadowVolume] = Published(ORGVolumetricClouds::kShadowVolumeId, physicalSky.texShadowVolume->srv.get());
			shape.inputs[kPhysSkyShadowMean] = Published(ORGVolumetricClouds::kCloudShadowMeanId, physicalSky.texCloudShadowMean->srv.get());
			if (!shape.inputs[kPhysSkyShadowVolume] || !shape.inputs[kPhysSkyShadowMean])
				return fallback("Physical Sky's cloud shadows are not published");
		}
	}

	auto& lightLimitFix = globals::features::lightLimitFix;
	if (lightLimitFix.loaded && lightLimitFix.GetLightsSRV() && lightLimitFix.GetLightIndexListSRV() && lightLimitFix.GetLightGridSRV()) {
		if (!PublishedResources::IsExportOf(lightLimitFix.GetLightsSRV(), ORGLightCulling::kLightsId) ||
			!PublishedResources::IsExportOf(lightLimitFix.GetLightIndexListSRV(), ORGLightCulling::kLightIndexListId) ||
			!PublishedResources::IsExportOf(lightLimitFix.GetLightGridSRV(), ORGLightCulling::kLightGridId))
			return fallback("Light Limit Fix's light lists are on D3D11");
		shape.inputs[kLights] = { nullptr, std::string(ORGLightCulling::kLightsId) };
		shape.inputs[kLightList] = { nullptr, std::string(ORGLightCulling::kLightIndexListId) };
		shape.inputs[kLightGrid] = { nullptr, std::string(ORGLightCulling::kLightGridId) };
		inputs.localLights = true;
		if (lightLimitFix.IsLocalShadowCacheActive() && lightLimitFix.GetLocalShadowDataSRV()) {
			if (!PublishedResources::IsExportOf(lightLimitFix.GetLocalShadowDataSRV(), ORGLocalShadows::kDataId) ||
				!PublishedResources::IsExportOf(lightLimitFix.GetLocalShadowCacheSRV(), ORGLocalShadows::kCacheId))
				return fallback("Light Limit Fix's local shadows are on D3D11");
			shape.inputs[kLocalShadows] = { nullptr, std::string(ORGLocalShadows::kDataId) };
			shape.inputs[kLocalShadowMaps] = { nullptr, std::string(ORGLocalShadows::kCacheId), true };
			inputs.localShadows = true;
		}
		if (auto* view = fog.BuildEngineLocalShadowData(data.engineShadows)) {
			shape.inputs[kEngineShadowMaps] = ImportDepthView(view, "cs.exp-height-fog.engine-shadow-maps");
			if (!shape.inputs[kEngineShadowMaps])
				return fallback("the engine's shadow maps cannot be imported");
			inputs.engineLocalShadows = true;
		}
	}

	// The far extent depends only on the settings and near plane.
	bool farEnabled = false;
	fog.BuildConstants(inputs, farEnabled);
	// Mode 7 keeps the far volume cleared to no fog, so every reader sees only the near volume.
	shape.farEnabled = farEnabled && Debug() != 7;

	// A new extent alone becomes a new version at the next frame boundary, with no build. Other changes register new passes.
	if (impl->textures)
		impl->FollowSizes(shape.sizes);
	if (!impl->state || !(impl->state->shape == shape)) {
		if (!impl->Build(shape)) {
			logger::error("[ORG] Exponential Height Fog returns to D3D11 for good");
			FrameAnchors::Unregister(kClientId);
			WithdrawIntegrated();
			failed = true;
			impl.reset();
			return false;
		}
	}

	// As the D3D11 Prepass keeps it.
	const std::uint32_t frame = globals::state->frameCount;
	const bool consecutive = impl->lastFrame != UINT32_MAX && frame == impl->lastFrame + 1u;
	inputs.temporal = shape.temporal;
	inputs.historyValid = shape.temporal && impl->hasScatteringHistory && consecutive;
	inputs.historyValidFar = shape.temporal && impl->hasScatteringHistoryFar && consecutive;
	inputs.depthHistory = impl->hasDepthHistory;
	inputs.depthHistoryFar = impl->hasDepthHistoryFar;
	// May still be the old extents after a new size was asked for.
	inputs.sizes.nearSize = impl->textures->nearVolume.size;
	inputs.sizes.farSize = impl->textures->farVolume.size;
	data.fog = fog.BuildConstants(inputs, farEnabled);
	if (Debug() == 1) {
		// No conservative depth and no previous one, so no froxel is culled by the scene depth.
		data.fog.gridSizeAndFlags.w &= ~(2u | 16u);
		data.fog.farGridSizeAndFlags.w &= ~(2u | 16u);
	}
	impl->pending = data;
	if (!std::exchange(impl->reportedConstants, true)) {
		const auto& f = data.fog;
		const auto& s = fog.settings;
		const auto camera = Util::GetCameraData();
		logger::info(
			"[ORG] Exponential Height Fog: the first graph frame: density {:.4f}/{:.4f}, extinction scale {:.3f}, albedo a {:.3f}; "
			"near {}x{}x{} flags {:#x} z ({:.6g}, {:.6g}, {:.6g}); far {}x{}x{} flags {:#x} z ({:.6g}, {:.6g}, {:.6g}) range {:.0f}-{:.0f}{}; "
			"camera data ({:.1f}, {:.2f}, {:.1f}, {:.1f}); depth from {}",
			s.fogDensity, s.fogDensity2, s.volumetricFogExtinctionScale, s.volumetricFogAlbedo.w, f.gridSizeAndFlags.x, f.gridSizeAndFlags.y,
			f.gridSizeAndFlags.z, f.gridSizeAndFlags.w, f.gridZParams.x, f.gridZParams.y, f.gridZParams.z, f.farGridSizeAndFlags.x,
			f.farGridSizeAndFlags.y, f.farGridSizeAndFlags.z, f.farGridSizeAndFlags.w, f.farGridZParams.x, f.farGridZParams.y, f.farGridZParams.z,
			f.farRange.x, f.farRange.y, farEnabled ? "" : " (off)", camera.x, camera.y, camera.z, camera.w,
			shape.inputs[kDepth].resource ? "the engine's depth" : shape.inputs[kDepth].name);
	}

	// The commit marks the histories, for the state the run executes.
	impl->ranLastFrame = true;
	impl->fallbackReason.clear();
	return true;
}

bool ORGExponentialHeightFog::Readable() const
{
	// Mode 4 hands out versions no run wrote yet, as before the fix, to show what that does.
	return IsActive() && impl->state && impl->ranLastFrame && (impl->Written() || Debug() == 4);
}

ID3D11ShaderResourceView* ORGExponentialHeightFog::NearSRV() const
{
	return Readable() ? impl->textures->nearVolume.integrated->SRV() : nullptr;
}

ID3D11ShaderResourceView* ORGExponentialHeightFog::FarSRV() const
{
	return Readable() ? impl->textures->farVolume.integrated->SRV() : nullptr;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> ORGExponentialHeightFog::Slices() const
{
	if (!NearSRV())
		return std::nullopt;
	return std::pair{ impl->textures->nearVolume.integrated->Desc().Depth, impl->textures->farVolume.integrated->Desc().Depth };
}

void ORGExponentialHeightFog::Release()
{
	if (!impl || !impl->state)
		return;
	FrameAnchors::Unregister(kClientId);
	WithdrawIntegrated();
	impl->state.reset();
	impl->textures.reset();
	impl->pending.reset();
	impl->hasScatteringHistory = impl->hasDepthHistory = impl->hasScatteringHistoryFar = impl->hasDepthHistoryFar = false;
	impl->lastFrame = UINT32_MAX;
	impl->ranLastFrame = false;
	impl->fallbackReason.clear();
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph the volumetric fog always runs on D3D11.
struct ORGExponentialHeightFog::Impl
{};

ORGExponentialHeightFog::~ORGExponentialHeightFog() = default;
void ORGExponentialHeightFog::Reserve() {}
bool ORGExponentialHeightFog::Setup() { return false; }
bool ORGExponentialHeightFog::IsActive() const { return false; }
bool ORGExponentialHeightFog::Prepass() { return false; }
bool ORGExponentialHeightFog::Readable() const { return false; }
ID3D11ShaderResourceView* ORGExponentialHeightFog::NearSRV() const { return nullptr; }
ID3D11ShaderResourceView* ORGExponentialHeightFog::FarSRV() const { return nullptr; }
std::optional<std::pair<std::uint32_t, std::uint32_t>> ORGExponentialHeightFog::Slices() const { return std::nullopt; }
void ORGExponentialHeightFog::Release() {}

#endif
