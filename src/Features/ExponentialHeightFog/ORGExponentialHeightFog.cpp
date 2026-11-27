#include "ORGExponentialHeightFog.h"
#include "Snapshot/CoreFrame.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"

#include <winrt/base.h>

#include "RenderGraph/FrameClock.h"
#include "RenderGraph/Switches.h"

std::uint32_t ORGExponentialHeightFog::Debug()
{
	static const std::uint32_t s_mode = [] {
		const auto mode = Switches::Number("CS_ORG_FOG_DEBUG", 0);
		if (mode > 9 || mode == 3 || mode == 4) {
			logger::warn("[ORG] Exponential Height Fog: CS_ORG_FOG_DEBUG={} is not a mode (1, 2 or 5 to 9); it is off", mode);
			return 0u;
		}
		if (mode)
			logger::info("[ORG] Exponential Height Fog: debug mode {}", mode);
		return static_cast<std::uint32_t>(mode);
	}();
	return s_mode;
}

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/IBL.h"
#include "Features/IBL/ORGIBL.h"
#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/ORGLightCulling.h"
#include "Features/LightLimitFix/ORGLocalShadows.h"
#include "Features/PhysicalSky.h"
#include "Features/PhysicalSky/ORGPhysicalSky.h"
#include "Features/PhysicalSky/ORGVolumetricClouds.h"
#include "RenderGraph/Engine/Targets/TargetRef.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "Features/Skylighting.h"
#include "Features/Skylighting/ORGSkylighting.h"
#include "Features/TerrainBlending/ORGTerrainBlending.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainShadows/ORGTerrainShadows.h"
#include "Features/ViewAtmosphere.h"
#include "Features/ViewAtmosphere/ORGViewAtmosphere.h"
#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Logging.h"
#include "Utils/Game.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/VersionedTexture.h>

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace
{
	constexpr const char* kClientId = "cs.exp-height-fog";
	constexpr const char* kShaderDir = "ExponentialHeightFog/";
	// After IBL and Skylighting at the Prepass anchor. The graph orders the reads of their outputs anyway.
	constexpr std::int32_t kOrder = 300;

	/** @brief Mode 9 clears the histories. It also turns off the scattering's history guard, so the symptom shows as it was. */
	bool ClearsHistories() { return ORGExponentialHeightFog::Debug() == 9; }

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

	/** @brief What the client reads and whether it keeps history, as the last Prepass found them. Not variables yet. */
	struct FogSeen
	{
		bool farEnabled = false;
		bool temporal = false;
		std::array<Source, kInputs> inputs;

		bool operator==(const FogSeen&) const = default;
	};

	/** @brief The client's key. A change registers a new client. The volumes' extents are not in it: they are versions. */
	struct Shape
	{
		bool nearGroupReduce = false;
		bool farGroupReduce = false;
		FogSeen observed;

		bool operator==(const Shape&) const = default;
	};

	/** @brief Compiled as the D3D11 build compiles them. Queued at Setup. */
	struct Programs
	{
		RenderGraph::Programs::Fixed<ComputeProgram> depth, depthGroupReduce, material, scattering, integration;
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
	* @brief Declared at Setup on the volumes' variables (Frame/Declare.h). A new grid size is a new version of each, adopted with
	* its revision, so nothing rebuilds. While the volumes are off each is one texel. None is aliased, since a version's backing is
	* placed when it is made.
	*/
	struct VolumeTextures
	{
		RenderGraph::Frame::DeclaredTexture3D vBuffer, scattering, scatteringHistory, integrated;
		RenderGraph::Frame::DeclaredTexture2D conservativeDepth, conservativeDepthHistory;

		bool Complete() const
		{
			return vBuffer && scattering && scatteringHistory && integrated && integrated->SRV() && conservativeDepth && conservativeDepthHistory;
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

		/** @brief What the debug clears cover: the histories and the textures copied into them. */
		std::array<const GraphResources::Resource*, 4> Cleared() const
		{
			return { scattering.Get(), scatteringHistory.Get(), conservativeDepth.Get(), conservativeDepthHistory.Get() };
		}

		/** @brief The extent the frame's passes, group counts and constants use: the adopted versions'. All six share it. */
		DirectX::XMUINT4 Size() const
		{
			const auto extent = RenderGraph::Frame::Revision::Adopted().Extent(integrated);
			return { extent[0], extent[1], extent[2], 0 };
		}

		/** @brief Whether a submitted run copied into the bound histories. A new version holds only its birth (zero). */
		bool HistoryWritten() const
		{
			const auto& adopted = RenderGraph::Frame::Revision::Adopted();
			return adopted.Written(scatteringHistory) && adopted.Written(conservativeDepthHistory);
		}
	};

	/** @brief The versions a commit read, for its run's executed callback to mark written. Null: the run does not write it. */
	struct FogWrites
	{
		const void* integrated = nullptr;
		const void* scatteringHistory = nullptr;
		const void* depthHistory = nullptr;
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

		/** @brief As the D3D11 path dispatches it, for the adopted extent. Render thread. */
		std::array<std::uint32_t, 3> Groups(Step a_step) const
		{
			const auto size = textures->Size();
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
				b.inputs[kDepth] = Read(a_builder, state->shape.observed.inputs[kDepth]);
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
					if (i == kDepth || (isFar && local) || !state->shape.observed.inputs[i])
						continue;
					b.inputs[i] = Read(a_builder, state->shape.observed.inputs[i]);
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
		if (!a_view)
			return {};
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture)
			return {};
		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		a_view->GetDesc(&viewDesc);
		const bool wholeArray = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		// An engine field handle names its target. Identify takes handles only, so the view is asked first, then its texture.
		namespace Targets = RenderGraph::Engine::Targets;
		using RenderGraph::Engine::Tripwire::IsHandle;
		std::optional<Targets::TargetRef> target;
		if (IsHandle(a_view))
			target = Targets::Identify(static_cast<const ID3D11View*>(a_view));
		if (!target && IsHandle(texture.get()))
			target = Targets::Identify(static_cast<const ID3D11Resource*>(texture.get()));
		if (target && target->kind == Targets::Kind::Depth)
			return { FrameAnchors::EngineTexture(FrameAnchors::Depth(target->index)), {}, wholeArray };
		RE::BSGraphics::DepthStencilData imported{};
		imported.texture = texture.get();
		imported.depthSRV = a_view;
		return { FrameAnchors::ImportTexture(a_id, { .depth = &imported }), {}, wholeArray };
	}

	/** @brief As Util::GetCurrentSceneDepthSRV(true). */
	Source SceneDepth(ID3D11ShaderResourceView* a_view)
	{
		if (!a_view || !CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer))
			return {};
		using RenderGraph::Engine::Tripwire::Unwrap;
		if (Unwrap(a_view) == Unwrap(CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV))
			return { FrameAnchors::EngineTexture(FrameAnchors::Depth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY)), {} };
		return Published(ORGTerrainBlending::kBlendedDepth16Id, a_view);
	}

	/**
	* @brief The client's inputs before a Prepass saw any: the engine's depth alone. The key reads this at setup's build point, so a
	* world frame with the volumes on always has a client. Commit turns off the flags of the inputs it does not bind.
	*/
	FogSeen FirstObservation()
	{
		FogSeen first;
		first.temporal = Util::GetTemporal() && ORGExponentialHeightFog::Debug() != 8;
		first.inputs[kDepth] = { FrameAnchors::EngineTexture(FrameAnchors::Depth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY)), {} };
		if (!first.inputs[kDepth])
			RenderGraph::Fatal(ORGExponentialHeightFog::kName, "the engine's depth cannot be imported for the first client");
		return first;
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
	RenderGraph::Programs::Fixed<ComputeProgram> QueueProgram(const char* a_source, const std::vector<std::string>& a_defines, std::uint32_t a_words)
	{
		std::vector<std::pair<std::string, std::string>> defines;
		for (const auto& name : a_defines)
			defines.emplace_back(name, "");
		return RenderGraph::Programs::Queue("ExponentialHeightFog",
			ComputeProgram::Desc{ .source = std::string(kShaderDir) + a_source, .constantWords = a_words, .defines = RenderGraph::AnchorPasses::Defines(defines) });
	}

	void QueuePrograms(Programs& a_programs, bool a_far)
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

		a_programs.depth = QueueProgram("VolumetricFogConservativeDepthCS.hlsl", volumeDefines, ShaderBindings::Words<VolumeBlock>());
		a_programs.depthGroupReduce = QueueProgram("VolumetricFogConservativeDepthCS.hlsl", groupDefines, ShaderBindings::Words<VolumeBlock>());
		a_programs.material = QueueProgram("VolumetricFogMaterialCS.hlsl", volumeDefines, ShaderBindings::Words<VolumeBlock>());
		a_programs.scattering = QueueProgram("VolumetricFogLightScatteringCS.hlsl", scatteringDefines, ShaderBindings::Words<ScatteringBlock>());
		a_programs.integration = QueueProgram("VolumetricFogIntegrationCS.hlsl", volumeDefines, ShaderBindings::Words<IntegrationBlock>());
	}
}

struct ORGExponentialHeightFog::Impl
{
	std::shared_ptr<const Fixed> fixed;
	// Declared at Setup and kept. A new extent is a new version, a new shape a new client.
	std::shared_ptr<Textures> textures;
	RenderGraph::Frame::DeclaredClient declaredClient;
	// What the last Prepass found. The client's key reads it, so it is made before the client. Before the first, FirstObservation.
	RenderGraph::Frame::Observed<FogSeen> observed{ "ExponentialHeightFog.Inputs" };
	std::shared_ptr<const FogState> state;
	// Written at Prepass and latched by the commit.
	std::optional<LatchData> pending;
	bool hasScatteringHistory = false, hasDepthHistory = false, hasScatteringHistoryFar = false, hasDepthHistoryFar = false;
	std::uint64_t lastFrame = UINT64_MAX;  // the render frame (FrameClock) of the last run
	bool ranLastFrame = false;
	bool reportedConstants = false;  // the first graph frame's constants are logged once
	const FogState* reportedUnbound = nullptr;  // the last state Commit turned flags off for, logged once each
	// The versions the last commit read, near then far. Its run's executed callback marks them written (Frame::NoteWritten).
	// The D3D11 readers get a volume only once a run wrote its bound version, so they never read an unwritten one.
	std::optional<std::array<FogWrites, 2>> committedWrites;

	/** @brief Whether a submitted run wrote the bound versions of both integrated volumes. Render thread. */
	bool IntegratedWritten() const
	{
		if (!textures)
			return false;
		const auto& adopted = RenderGraph::Frame::Revision::Adopted();
		return adopted.Written(textures->nearVolume.integrated) && adopted.Written(textures->farVolume.integrated);
	}

	/** @brief After a submitted run. Marks what its commit opened as written: the integrations (or the far clear), and the histories. */
	void Executed()
	{
		const auto writes = std::exchange(committedWrites, std::nullopt);
		if (!writes || !textures)
			return;
		const bool before = IntegratedWritten();
		for (const bool isFar : { false, true }) {
			const auto& v = isFar ? textures->farVolume : textures->nearVolume;
			const auto& w = (*writes)[isFar ? 1 : 0];
			RenderGraph::Frame::NoteWritten(v.integrated, w.integrated);
			if (w.scatteringHistory)
				RenderGraph::Frame::NoteWritten(v.scatteringHistory, w.scatteringHistory);
			if (w.depthHistory)
				RenderGraph::Frame::NoteWritten(v.conservativeDepthHistory, w.depthHistory);
		}
		if (!before && IntegratedWritten())
			logger::info("[ORG] Exponential Height Fog: a run wrote the volumes at frame {}; the engine's draws and DCLF read them from the next Prepass",
				RenderGraph::FrameClock::BeginFrame());
	}

	void MakeVolume(Volume& a_volume, VolumeTextures& a_textures, bool a_groupReduce, bool a_far) const
	{
		const auto& programs = a_far ? fixed->farPrograms : fixed->nearPrograms;
		a_volume.textures = &a_textures;
		a_volume.groupReduce = a_groupReduce;
		a_volume.depthProgram = (a_groupReduce ? programs.depthGroupReduce : programs.depth).Get();
		a_volume.materialProgram = programs.material.Get();
		a_volume.scatteringProgram = programs.scattering.Get();
		a_volume.integrationProgram = programs.integration.Get();
	}

	/** @brief The volumes' declarations, for the session. Fatal when one cannot be made or published. */
	void DeclareTextures()
	{
		using namespace RenderGraph::Frame;
		const auto& fog = globals::features::exponentialHeightFog.settings;
		const auto& quality = globals::features::viewAtmosphere.settings;
		// The grid follows the display size (decision 6a) and the grid settings. The existence rule makes it one texel while the
		// volumes are off, and while Atmosphere and Fog's shared volume replaces them (view-atmosphere-port.md 3.3), so both switch
		// with one revision. The ids stay published.
		const auto dependencies = DependsOn(Vars::DisplaySize(), quality.nearPixelSize, quality.nearSlices, quality.middlePixelSize, quality.middleSlices);
		// Atmosphere and Fog sets up first (the feature list's order), so its switch can be read here.
		if (ORGViewAtmosphere::Get().SharedVolumeDependencies().empty())
			RenderGraph::Fatal(ORGExponentialHeightFog::kName, "Atmosphere and Fog's render graph path is not set up before the fog's");
		auto existenceDependencies = DependsOn(fog.enabled, fog.volumetricFogEnabled, fog.volumetricFogExtinctionScale, fog.fogDensity, fog.fogDensity2);
		for (const auto& dependency : ORGViewAtmosphere::Get().SharedVolumeDependencies())
			existenceDependencies.push_back(dependency);
		const auto exists = [&] {
			return ExistsWhen(existenceDependencies, [](const Inputs& a_in) {
				const auto& f = globals::features::exponentialHeightFog.settings;
				return ExponentialHeightFog::VolumesOn(a_in.Get(f.enabled), a_in.Get(f.volumetricFogEnabled), a_in.Get(f.volumetricFogExtinctionScale),
						   a_in.Get(f.fogDensity), a_in.Get(f.fogDensity2)) &&
				       !ORGViewAtmosphere::Get().SharedVolumeAt(a_in);
			});
		};
		const auto sizeOf = [](const Inputs& a_in, bool a_far) {
			const auto& q = globals::features::viewAtmosphere.settings;
			const auto display = a_in.Get(Vars::DisplaySize());
			const auto sizes = ExponentialHeightFog::GridSizesFor(float2{ static_cast<float>(display.width), static_cast<float>(display.height) },
				a_in.Get(q.nearPixelSize), a_in.Get(q.nearSlices), a_in.Get(q.middlePixelSize), a_in.Get(q.middleSlices));
			return a_far ? sizes.farSize : sizes.nearSize;
		};
		const auto volume = [&](bool a_far, const std::string& a_name, DXGI_FORMAT a_format, const GraphResources::Options& a_options) {
			return DeclareTexture3D(
				a_name, dependencies, [sizeOf, a_far, a_format](const Inputs& a_in) { return VolumeDesc(sizeOf(a_in, a_far), a_format); }, a_options, exists());
		};
		const auto depth = [&](bool a_far, const std::string& a_name, const GraphResources::Options& a_options) {
			return DeclareTexture2D(a_name, dependencies, [sizeOf, a_far](const Inputs& a_in) { return DepthDesc(sizeOf(a_in, a_far)); }, a_options, exists());
		};
		auto made = std::make_shared<Textures>();
		for (const bool isFar : { false, true }) {
			auto& v = isFar ? made->farVolume : made->nearVolume;
			const std::string prefix = isFar ? "ExponentialHeightFog::Far" : "ExponentialHeightFog::";
			const GraphResources::Options own{ .lifetime = GraphResources::Lifetime::Persistent };
			// Born zero (a History birth). Until a run copied into a version, Written keeps the passes from reading it.
			const GraphResources::Options history{ .lifetime = GraphResources::Lifetime::History };
			// The engine's draws read these through D3D11, so they are exported and never aliased.
			const GraphResources::Options exported{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV };
			v.vBuffer = volume(isFar, prefix + "VBufferA", DXGI_FORMAT_R16G16B16A16_FLOAT, own);
			v.scattering = volume(isFar, prefix + "LightScattering", DXGI_FORMAT_R32G32B32A32_FLOAT, own);
			v.scatteringHistory = volume(isFar, prefix + "LightScatteringHistory", DXGI_FORMAT_R32G32B32A32_FLOAT, history);
			v.integrated = volume(isFar, std::string(isFar ? ORGExponentialHeightFog::kFarIntegratedId : ORGExponentialHeightFog::kNearIntegratedId),
				DXGI_FORMAT_R16G16B16A16_FLOAT, exported);
			v.conservativeDepth = depth(isFar, prefix + "ConservativeDepth", own);
			v.conservativeDepthHistory = depth(isFar, prefix + "ConservativeDepthHistory", history);
			if (!v.Complete())
				RenderGraph::Fatal(ORGExponentialHeightFog::kName, fmt::format("the {} volume's textures could not be made", isFar ? "far" : "near"));
		}
		D3D11_BUFFER_DESC bufferDesc{};
		bufferDesc.Usage = D3D11_USAGE_DEFAULT;
		bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bufferDesc.StructureByteStride = sizeof(Deferred::DirectionalShadowLightData);
		bufferDesc.ByteWidth = sizeof(Deferred::DirectionalShadowLightData);
		made->directionalShadowLights = std::make_unique<GraphResources::StructuredBuffer>(bufferDesc, 1, "ExponentialHeightFog::DirectionalShadowLights",
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Transient });
		if (!*made->directionalShadowLights)
			RenderGraph::Fatal(ORGExponentialHeightFog::kName, "the shadow light buffer could not be made");
		// Published once, for the session. Readers follow the versions, so neither a resize nor turning the fog off publishes again.
		if (!PublishedResources::Publish(*made->nearVolume.integrated) || !PublishedResources::Publish(*made->farVolume.integrated))
			RenderGraph::Fatal(ORGExponentialHeightFog::kName, "the integrated volumes could not be published");
		textures = std::move(made);
	}

	/** @brief The client, declared on the settings its key reads. */
	void DeclareClient()
	{
		using namespace RenderGraph::Frame;
		const auto& fog = globals::features::exponentialHeightFog.settings;
		const auto& quality = globals::features::viewAtmosphere.settings;
		auto dependencies = DependsOn(quality.nearPixelSize, quality.middlePixelSize, fog.enabled, fog.volumetricFogEnabled, fog.volumetricFogExtinctionScale,
			fog.fogDensity, fog.fogDensity2, globals::features::ibl.settings.EnableIBL, observed.Var());
		// No client while the shared volume replaces the volumes: ViewAtmosphere::IsPrepared keeps the fog's Prepass from staging.
		for (const auto& dependency : ORGViewAtmosphere::Get().SharedVolumeDependencies())
			dependencies.push_back(dependency);
		declaredClient = RenderGraph::Frame::DeclareClient<Shape>(
			kClientId, std::move(dependencies),
			[this](const Inputs& a_in) -> std::optional<Shape> {
				const auto& f = globals::features::exponentialHeightFog.settings;
				const auto& q = globals::features::viewAtmosphere.settings;
				if (!ExponentialHeightFog::VolumesOn(a_in.Get(f.enabled), a_in.Get(f.volumetricFogEnabled), a_in.Get(f.volumetricFogExtinctionScale),
						a_in.Get(f.fogDensity), a_in.Get(f.fogDensity2)) ||
					ORGViewAtmosphere::Get().SharedVolumeAt(a_in))
					return std::nullopt;
				// The inputs come from the last Prepass. Before the first, the engine's depth alone.
				const auto* seen = observed.Get(a_in);
				const auto sizes = ExponentialHeightFog::GridSizesFor({ 1.0f, 1.0f }, a_in.Get(q.nearPixelSize), 16, a_in.Get(q.middlePixelSize), 16);
				Shape shape{ sizes.nearGroupReduce, sizes.farGroupReduce, seen ? *seen : FirstObservation() };
				// IBL's SH and Skylighting's probes are bound while their features are loaded: IBL from its EnableIBL, the probes
				// always. Both publish their ids at setup, for the session. A scene that disables IBL turns it off in the constants,
				// so the binding stays.
				const auto& ibl = globals::features::ibl;
				if (ibl.loaded && a_in.Get(ibl.settings.EnableIBL) != 0) {
					shape.observed.inputs[kEnvIBL] = { nullptr, std::string(ORGIBL::kEnvId) };
					shape.observed.inputs[kSkyIBL] = { nullptr, std::string(ORGIBL::kSkyId) };
				}
				if (globals::features::skylighting.loaded)
					shape.observed.inputs[kProbes] = { nullptr, std::string(ORGSkylighting::kProbeArrayId) };
				// Physical Sky's transmittance LUT and Light Limit Fix's culling lists are published at setup, for the session
				// (ORGPhysicalSky::MakeLut2D, ORGLightCulling). Whether a feature is loaded is fixed for the session too.
				if (globals::features::physicalSky.loaded)
					shape.observed.inputs[kPhysSkyTransmittance] = { nullptr, std::string(ORGPhysicalSky::kTransmittanceLutId) };
				if (globals::features::lightLimitFix.loaded) {
					shape.observed.inputs[kLights] = { nullptr, std::string(ORGLightCulling::kLightsId) };
					shape.observed.inputs[kLightList] = { nullptr, std::string(ORGLightCulling::kLightIndexListId) };
					shape.observed.inputs[kLightGrid] = { nullptr, std::string(ORGLightCulling::kLightGridId) };
				}
				return shape;
			},
			[this](const Shape& a_shape, FrameAnchors::ClientDesc& a_client) { return MakeClient(a_shape, a_client); });
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
		const auto& inputs = a_state.shape.observed.inputs;
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
		if (inputs[kShadowHeights])
			bound |= 256u;
		if (inputs[kCloudShadows])
			bound |= 512u;
		if (inputs[kPhysSkyTransmittance])
			bound |= 1024u;
		if (inputs[kPhysSkyShadowVolume] && inputs[kPhysSkyShadowMean])
			bound |= 2048u;
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
		if (a_state.shape.observed.farEnabled)
			open(a_state.farVolume, true);
		// What this run writes, as the versions bound now. The history copies run after the scattering this commit opened.
		const auto& adopted = RenderGraph::Frame::Revision::Adopted();
		const auto writes = [&](const VolumeTextures& a_textures, bool a_history) {
			FogWrites w{ .integrated = adopted.Version(a_textures.integrated) };
			if (a_history) {
				w.scatteringHistory = adopted.Version(a_textures.scatteringHistory);
				w.depthHistory = adopted.Version(a_textures.conservativeDepthHistory);
			}
			return w;
		};
		const bool temporalRun = a_state.shape.observed.temporal;
		committedWrites = std::array<FogWrites, 2>{ writes(a_state.textures->nearVolume, temporalRun),
			writes(a_state.textures->farVolume, temporalRun && a_state.shape.observed.farEnabled) };
		// The histories hold data only once a run with history passes copied into them. The state committed here is the one this
		// run executes, which lags a new registration by a build. Marking them at Prepass let the next frame read a history no pass
		// had written: recycled memory, kept by the temporal blend (huge but finite values that overflow the half-float volume).
		const auto& observedShape = a_state.shape.observed;
		const bool temporal = observedShape.temporal;
		hasScatteringHistory = temporal;
		hasDepthHistory = temporal && static_cast<bool>(observedShape.inputs[kDepth]);
		hasScatteringHistoryFar = temporal && observedShape.farEnabled;
		hasDepthHistoryFar = hasScatteringHistoryFar && static_cast<bool>(observedShape.inputs[kDepth]);
		lastFrame = RenderGraph::FrameClock::BeginFrame();
	}

	/** @brief Mode 9's clears of each volume's histories and the textures copied into them, to zero, every run. Nothing gates them. */
	static void AddClears(std::vector<FrameAnchors::PassDesc>& a_passes, const std::shared_ptr<const FogState>& a_state, std::string_view a_prefix,
		std::int32_t& a_order)
	{
		constexpr std::array<const char*, 4> names{ "scattering", "scattering-history", "depth", "depth-history" };
		for (const bool isFar : { false, true }) {
			if (isFar && !a_state->shape.observed.farEnabled)
				continue;
			for (std::uint32_t i = 0; i < names.size(); ++i)
				a_passes.push_back({ .name = std::format("{}.{}debug-clear-{}", a_prefix, isFar ? "far." : "near.", names[i]),
					.anchor = FrameAnchors::Anchor::Prepass,
					.order = a_order++,
					.kind = FrameAnchors::PassKind::Compute,
					.make = [a_state, isFar, i] {
						const auto& textures = isFar ? a_state->textures->farVolume : a_state->textures->nearVolume;
						return RenderGraph::AnchorPasses::MakeClear(RenderGraph::AnchorPasses::ClearOf("debug-clear", *textures.Cleared()[i]));
					} });
		}
	}

	/** @brief The registration for a new key, at the start of the revision that adopts it. A new extent alone is a version, never this. */
	bool MakeClient(const Shape& a_shape, FrameAnchors::ClientDesc& a_client)
	{
		auto built = std::make_shared<FogState>();
		built->shape = a_shape;
		built->fixed = fixed;
		built->textures = textures;
		MakeVolume(built->nearVolume, textures->nearVolume, a_shape.nearGroupReduce, false);
		MakeVolume(built->farVolume, textures->farVolume, a_shape.farGroupReduce, true);

		auto& client = a_client;
		client.id = kClientId;
		const std::shared_ptr<const FogState> shared = built;
		std::int32_t order = kOrder;
		const auto add = [&](std::string a_name, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make) {
			client.passes.push_back({ .name = "cs.exp-height-fog." + std::move(a_name), .anchor = FrameAnchors::Anchor::Prepass, .order = order++, .kind = a_kind, .make = std::move(a_make) });
		};
		add("upload", FrameAnchors::PassKind::Copy, [shared] { return std::make_shared<UploadPass>(shared); });
		const auto& observedShape = a_shape.observed;
		if (observedShape.temporal && ORGExponentialHeightFog::Debug() == 9)
			AddClears(client.passes, shared, "cs.exp-height-fog", order);
		const auto addVolume = [&](bool a_far) {
			const std::string volume = a_far ? "far." : "near.";
			constexpr std::array<std::pair<Step, const char*>, 4> steps{ { { Step::Depth, "depth" }, { Step::Material, "material" },
				{ Step::Scattering, "scattering" }, { Step::Integration, "integration" } } };
			for (const auto& entry : steps) {
				const Step step = entry.first;
				add(volume + entry.second, FrameAnchors::PassKind::Compute, [shared, a_far, step] { return std::make_shared<DispatchPass>(shared, a_far, step); });
			}
			if (observedShape.temporal)
				add(volume + "history", FrameAnchors::PassKind::Copy, [shared, a_far] { return std::make_shared<HistoryPass>(shared, a_far); });
		};
		addVolume(false);
		if (observedShape.farEnabled)
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
		client.executed = [this](const FrameAnchors::Point&) { Executed(); };
		// The fog has work only when Prepass gave the graph this frame.
		client.active = [this] { return pending.has_value(); };
		client.sharedConstants = true;
		client.gates = { fixed->gate };
		if (observedShape.temporal && ClearsHistories())
			logger::info("[ORG] Exponential Height Fog: debug mode 9 clears the histories every run, and the scattering's history guard is off");
		this->state = std::move(built);
		const auto nearSize = textures->nearVolume.Size();
		const auto farSize = textures->farVolume.Size();
		logger::info("[ORG] Exponential Height Fog: a client for the next revision (near volume now {}x{}x{}, far {}x{}x{}{}, {})", nearSize.x, nearSize.y, nearSize.z,
			farSize.x, farSize.y, farSize.z, observedShape.farEnabled ? "" : " off", observedShape.temporal ? "temporal" : "no history");
		return true;
	}
};

ORGExponentialHeightFog::ORGExponentialHeightFog() = default;
ORGExponentialHeightFog::~ORGExponentialHeightFog() = default;

void ORGExponentialHeightFog::Setup()
{
	// Once a session. The volumes are published, so they are never torn down.
	if (!BeginSetup())
		return;
	auto fixed = std::make_shared<Fixed>();
	QueuePrograms(fixed->nearPrograms, false);
	QueuePrograms(fixed->farPrograms, true);
	// Linear clamp, and the comparison sampler for the directional shadow map.
	fixed->linearSampler = ShaderBindings::CreateSamplerFromD3D11(ExponentialHeightFog::SamplerDesc(false));
	fixed->shadowSampler = ShaderBindings::CreateSamplerFromD3D11(ExponentialHeightFog::SamplerDesc(true));
	fixed->latch = std::make_shared<org::LatchBlock>("cs.exp-height-fog.latch", static_cast<std::uint32_t>(sizeof(LatchData)));
	fixed->gate = RenderGraph::RunGate::Create("cs.exp-height-fog.gate", 2 * static_cast<std::uint32_t>(Step::Count));
	if (!fixed->linearSampler || !fixed->shadowSampler)
		Fatal("a sampler could not be made");
	auto made = std::make_unique<Impl>();
	made->fixed = std::move(fixed);
	// The volumes exist from here on, for the session. The client follows the settings from the first revision.
	made->DeclareTextures();
	made->DeclareClient();
	impl = std::move(made);
	logger::info("[ORG] Exponential Height Fog: the volumetric fog runs on the render graph; its volumes and client follow the frame revisions");
}

bool ORGExponentialHeightFog::VolumesOn() const
{
	const auto& adopted = RenderGraph::Frame::Revision::Adopted();
	const auto& f = globals::features::exponentialHeightFog.settings;
	// The shared volume's revision has no fog client (DeclareClient).
	return ExponentialHeightFog::VolumesOn(adopted.Get(f.enabled), adopted.Get(f.volumetricFogEnabled), adopted.Get(f.volumetricFogExtinctionScale),
			   adopted.Get(f.fogDensity), adopted.Get(f.fogDensity2)) &&
	       !ORGViewAtmosphere::Get().SharedVolumeActive();
}

bool ORGExponentialHeightFog::Prepass()
{
	if (!IsActive())
		return false;
	auto& fog = globals::features::exponentialHeightFog;
	impl->pending.reset();

	// The adopted revision decides whether the volumes exist. Without them the frame has no volumetric fog.
	if (!VolumesOn()) {
		impl->ranLastFrame = false;
		return false;
	}

	// Graph mode has no D3D11 fog. An input the graph should always have is fatal when missing. An input that is not
	// there yet (another feature's first output) is left out: the client binds nothing for it and its flag is off.
	FogSeen seen;
	// Mode 8 runs without history, so no froxel reads a history texture.
	seen.temporal = Util::GetTemporal() && Debug() != 8;

	ExponentialHeightFog::ConstantInputs inputs;
	inputs.sizes.nearSize = impl->textures->nearVolume.Size();
	inputs.sizes.farSize = impl->textures->farVolume.Size();

	seen.inputs[kDepth] = SceneDepth(Util::GetCurrentSceneDepthSRV(true));
	if (!seen.inputs[kDepth])
		Fatal("the scene depth is missing or is not the graph's");
	inputs.depth = true;

	LatchData data{};
	const bool hasShadowLight = globals::deferred && globals::deferred->GetDirectionalShadowLightData(data.directionalShadow);
	// Null unless this frame drew the shadow mask, as the D3D11 path reads it.
	auto* directionalShadowMap = globals::deferred ? globals::deferred->GetDirectionalShadowMap() : nullptr;
	if (directionalShadowMap && hasShadowLight) {
		seen.inputs[kDirectionalShadowMap] = ImportDepthView(directionalShadowMap, "cs.exp-height-fog.directional-shadow-map");
		if (!seen.inputs[kDirectionalShadowMap])
			Fatal("the directional shadow map cannot be imported");
		inputs.directionalShadow = true;
	}

	// IBL's SH and Skylighting's probes are in the client's key (DeclareClient), from the variables.

	// Without the sky the shader skips terrain and cloud shadows, and Cloud Shadows binds nothing.
	const bool skyless = Util::IsInterior() || globals::state->isMapMenuOpen || Snapshot::Core().hideSky;

	auto& terrainShadows = globals::features::terrainShadows;
	if (terrainShadows.loaded && !skyless) {
		// None until Terrain Shadows made its heights for a heightmap. Then the scattering skips them (flag 256).
		if (auto* heights = terrainShadows.GetShadowHeightSRV()) {
			seen.inputs[kShadowHeights] = Published(ORGTerrainShadows::kShadowHeightId, heights);
			if (!seen.inputs[kShadowHeights])
				Fatal("Terrain Shadows' heights are on D3D11");
			inputs.terrainShadows = true;
		}
	}

	auto& cloudShadows = globals::features::cloudShadows;
	if (cloudShadows.loaded && !skyless) {
		// Cloud Shadows publishes its occlusion cubemap at setup, for the session (ORGCloudShadows).
		seen.inputs[kCloudShadows] = { nullptr, std::string(ORGCloudShadows::kOcclusionId) };
		inputs.cloudShadows = true;
	}

	// The scattering reads Physical Sky's transmittance LUT, and with the clouds on their shadow volume and its mean. The key binds
	// the LUT (DeclareClient).
	auto& physicalSky = globals::features::physicalSky;
	if (physicalSky.loaded) {
		inputs.physSkyTransmittance = true;
		// The shader reads them only with the clouds on (the adopted enableVolumetricClouds). Until a cloud run wrote them they are
		// left out, and the scattering skips them (flag 2048).
		if (physicalSky.VolumetricCloudsEnabled() && ORGVolumetricClouds::OutputsWritten()) {
			seen.inputs[kPhysSkyShadowVolume] = { nullptr, std::string(ORGVolumetricClouds::kShadowVolumeId) };
			seen.inputs[kPhysSkyShadowMean] = { nullptr, std::string(ORGVolumetricClouds::kCloudShadowMeanId) };
			inputs.physSkyShadowVolume = true;
		}
	}

	// Light Limit Fix's culling lists are in the key (DeclareClient).
	auto& lightLimitFix = globals::features::lightLimitFix;
	if (lightLimitFix.loaded) {
		inputs.localLights = true;
		if (lightLimitFix.IsLocalShadowCacheActive() && lightLimitFix.GetLocalShadowDataSRV()) {
			if (!PublishedResources::IsExportOf(lightLimitFix.GetLocalShadowDataSRV(), ORGLocalShadows::kDataId) ||
				!PublishedResources::IsExportOf(lightLimitFix.GetLocalShadowCacheSRV(), ORGLocalShadows::kCacheId))
				Fatal("Light Limit Fix's local shadows are on D3D11");
			seen.inputs[kLocalShadows] = { nullptr, std::string(ORGLocalShadows::kDataId) };
			seen.inputs[kLocalShadowMaps] = { nullptr, std::string(ORGLocalShadows::kCacheId), true };
			inputs.localShadows = true;
		}
		if (auto* view = fog.BuildEngineLocalShadowData(data.engineShadows)) {
			seen.inputs[kEngineShadowMaps] = ImportDepthView(view, "cs.exp-height-fog.engine-shadow-maps");
			if (!seen.inputs[kEngineShadowMaps])
				Fatal("the engine's shadow maps cannot be imported");
			inputs.engineLocalShadows = true;
		}
	}

	// The far extent depends only on the settings and near plane.
	bool farEnabled = false;
	fog.BuildConstants(inputs, farEnabled);
	// Mode 7 keeps the far volume cleared to no fog, so every reader sees only the near volume.
	seen.farEnabled = farEnabled && Debug() != 7;

	// New inputs make a new client, registered when the next revision starts. This frame runs the client it has.
	impl->observed.Observe(seen);
	// The key is never empty while the volumes are on (FirstObservation), and declarations adopt at setup's build point.
	if (!impl->state)
		Fatal("a world frame has the volumes on but no client");
	// The client's IBL and probe bindings, as its key found them. The scene's IBL switch is a constant.
	const auto& bound = impl->state->shape.observed.inputs;
	inputs.ibl = bound[kEnvIBL] && bound[kSkyIBL] && !globals::features::ibl.IsDisabledForCurrentScene();
	inputs.skylighting = static_cast<bool>(bound[kProbes]);

	// As the D3D11 Prepass keeps it. A history no run wrote yet holds none.
	const auto frame = RenderGraph::FrameClock::BeginFrame();
	const bool consecutive = impl->lastFrame != UINT64_MAX && frame == impl->lastFrame + 1u;
	const bool temporal = impl->state->shape.observed.temporal;
	inputs.temporal = temporal;
	inputs.historyValid = temporal && impl->hasScatteringHistory && consecutive && impl->textures->nearVolume.HistoryWritten();
	inputs.historyValidFar = temporal && impl->hasScatteringHistoryFar && consecutive && impl->textures->farVolume.HistoryWritten();
	inputs.depthHistory = impl->hasDepthHistory && impl->textures->nearVolume.HistoryWritten();
	inputs.depthHistoryFar = impl->hasDepthHistoryFar && impl->textures->farVolume.HistoryWritten();
	data.fog = fog.BuildConstants(inputs, farEnabled);
	if (ClearsHistories())
		data.fog.historyParameters.w = 1.0f;
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
			s.fogDensity.Get(), s.fogDensity2.Get(), s.volumetricFogExtinctionScale.Get(), s.volumetricFogAlbedo.w, f.gridSizeAndFlags.x, f.gridSizeAndFlags.y,
			f.gridSizeAndFlags.z, f.gridSizeAndFlags.w, f.gridZParams.x, f.gridZParams.y, f.gridZParams.z, f.farGridSizeAndFlags.x,
			f.farGridSizeAndFlags.y, f.farGridSizeAndFlags.z, f.farGridSizeAndFlags.w, f.farGridZParams.x, f.farGridZParams.y, f.farGridZParams.z,
			f.farRange.x, f.farRange.y, farEnabled ? "" : " (off)", camera.x, camera.y, camera.z, camera.w,
			seen.inputs[kDepth].resource ? "the engine's depth" : seen.inputs[kDepth].name);
	}

	// The commit marks the histories, for the state the run executes.
	impl->ranLastFrame = true;
	return true;
}

bool ORGExponentialHeightFog::Readable() const
{
	return IsActive() && impl->state && impl->ranLastFrame && impl->IntegratedWritten();
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
	return std::pair{ impl->textures->nearVolume.Size().z, impl->textures->farVolume.Size().z };
}

