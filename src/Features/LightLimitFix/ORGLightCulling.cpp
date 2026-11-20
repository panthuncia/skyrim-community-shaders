#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "ORGLightCulling.h"

#include "Globals.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <Render/Runtime/UploadServiceAccess.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/Buffers/VersionedBuffer.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <span>
#include <vector>

namespace
{
	namespace Frame = RenderGraph::Frame;

	constexpr const char* kExtensionId = "cs.llf.light-culling";
	constexpr const char* kBuildShader = "LightLimitFix/ClusterBuildingCS.hlsl";
	constexpr const char* kCullShader = "LightLimitFix/ClusterCullingCS.hlsl";
	// The culling's own buffers, which no other pass reads.
	constexpr const char* kClustersName = "LightLimitFix::Clusters";
	constexpr const char* kLightIndexCounterName = "LightLimitFix::LightIndexCounter";
	constexpr const char* kGateName = "LightLimitFix::CullingGate";

	// The gate's entries: the group counts of the bound buffers' grid, written by each run.
	constexpr uint32_t kBuildEntry = 0;
	constexpr uint32_t kCullEntry = 1;

	// Push block of both shaders, only view and latch indices. Must match LLFOrgConstants in OrgBindless.hlsli.
	struct alignas(16) OrgClusterConstants
	{
		uint32_t clustersIndex;
		uint32_t lightsIndex;
		uint32_t lightIndexCounterIndex;
		uint32_t lightIndexListIndex;
		uint32_t lightGridIndex;
		uint32_t latchIndex;    // set at record time from the run's latch region
		uint32_t latchOffset;
		uint32_t matrixOffset;  // offsetof(LLFLatch, cameraProjInverse) or offsetof(LLFLatch, cameraView)
	};
	static_assert(sizeof(OrgClusterConstants) == 32);
	constexpr uint32_t kConstantWords = sizeof(OrgClusterConstants) / sizeof(uint32_t);

	// One execution's values in its latch slot, written in the epoch after the slot's last use completed.
	// A copy recorded once moves the lights into the buffer D3D11 also reads.
	// The cluster grid and the bound buffers' grid are here too, so neither changes a recorded pass.
	struct LLFLatch
	{
		float cameraProjInverse[16];
		float cameraView[16];
		float lightsNear;
		float lightsFar;
		uint32_t lightCount;
		uint32_t padding;
		uint32_t clusterSize[4];
		uint32_t clusterCapacity[4];  // the bound buffers' grid. The shaders clamp the run's grid to it
	};
	static_assert(sizeof(LLFLatch) == 176);
	static_assert(offsetof(LLFLatch, clusterSize) == 144);
	static_assert(offsetof(LLFLatch, clusterCapacity) == 160);
	constexpr uint32_t kLatchLightsOffset = 256;
	static_assert(sizeof(LLFLatch) <= kLatchLightsOffset);

	// From LightLimitFix/Common.hlsli.
	constexpr uint32_t kCullGroupX = 16, kCullGroupY = 16, kCullGroupZ = 4;

	using Grid = std::array<uint32_t, 3>;

	uint32_t ClustersOf(const Grid& a_grid)
	{
		return a_grid[0] * a_grid[1] * a_grid[2];
	}

	// The culling's buffers for the session. The lights and the counter never change size. The clusters, the index list and the
	// grid are declared on the display size, so a new display size is a new version of each, adopted with its revision.
	// The lights, index list and grid are exported for D3D11 at t35-t37 and published by id.
	struct Resources
	{
		Grid setupGrid{};  // the grid Setup was given, used while the display size is zero
		Grid firstGrid{};  // the grid the first versions hold
		uint32_t lightBytes = 0;  // the light copy always moves the whole capacity
		std::unique_ptr<GraphResources::StructuredBuffer> lights;
		std::unique_ptr<GraphResources::StructuredBuffer> lightIndexCounter;
		Frame::DeclaredBuffer clusters;
		Frame::DeclaredBuffer lightIndexList;
		Frame::DeclaredBuffer lightGrid;
		std::shared_ptr<const ComputeProgram> build;
		std::shared_ptr<const ComputeProgram> cull;
		std::shared_ptr<org::LatchBlock> latch;
		std::shared_ptr<RenderGraph::RunGate> gate;
	};

	/** @brief The grid of a display size, 64 pixels a cluster. Setup's grid while the display size is zero. */
	Grid GridFor(const Grid& a_setupGrid, const Frame::Vars::Extent2D& a_display)
	{
		if (!a_display.width || !a_display.height)
			return a_setupGrid;
		return { (a_display.width + 63) / 64, (a_display.height + 63) / 64, a_setupGrid[2] };
	}

	/**
	* @brief The grid the bound buffers hold. Render thread. A version is made from the display size its revision carries, so after
	* an adoption it is the adopted display's grid. Before the first adoption the buffers hold their first versions.
	*/
	Grid BoundGrid(const Resources& a_resources)
	{
		const auto& adopted = Frame::Revision::Adopted();
		const auto bound = adopted.Elements(a_resources.clusters);
		auto grid = GridFor(a_resources.setupGrid, adopted.Get(Frame::Vars::DisplaySize()));
		if (ClustersOf(grid) != bound)
			grid = a_resources.firstGrid;
		if (ClustersOf(grid) != bound) {
			const auto message = std::format(
				"Community Shaders: the light culling's buffers hold {} clusters, which is neither the adopted display's grid nor their first one", bound);
			logger::critical("[ORG] {}", message);
			spdlog::default_logger()->flush();
			stl::report_and_fail(message);
		}
		return grid;
	}

	struct DispatchFrame
	{
		std::shared_ptr<const ComputeProgram> program;
		OrgClusterConstants constants{};
		uint32_t entry = 0;
		std::shared_ptr<const org::LatchBlock> latch;  // its run's region is found at record time
		std::shared_ptr<const RenderGraph::RunGate> gate;
	};

	bool Dispatches(const DispatchFrame& a_frame)
	{
		return a_frame.program && a_frame.latch && a_frame.gate;
	}

	// a_constants carry the pass's view indices. The group counts are the run's, in the gate.
	void RecordDispatch(const DispatchFrame& a_frame, OrgClusterConstants a_constants, org::PassRecordContext& a_recording)
	{
		auto constants = a_constants;
		const auto region = a_frame.latch->At(a_recording.Run());
		constants.latchIndex = region.srvIndex;
		constants.latchOffset = static_cast<uint32_t>(region.offset);
		a_frame.gate->Dispatch(a_recording, *a_frame.program, std::span<const uint32_t>(reinterpret_cast<const uint32_t*>(&constants), kConstantWords),
			a_frame.entry);
	}

	// Nothing about the buffers' size is recorded, so only the program tells revisions apart.
	void AppendProgramRevision(const ComputeProgram* a_program, std::vector<uint64_t>& a_out)
	{
		a_out.push_back(reinterpret_cast<uintptr_t>(a_program));
	}

	OrgClusterConstants BaseConstants(uint32_t a_matrixOffset)
	{
		OrgClusterConstants constants{};
		constants.matrixOffset = a_matrixOffset;
		return constants;
	}

	/** @brief A declared buffer's slot, which passes declare so its versions need no build. */
	org::ResourceResolverAndRange SlotOf(const Frame::DeclaredBuffer& a_buffer)
	{
		return org::ResourceResolverAndRange(*a_buffer->BufferVersions());
	}

	// Moves the lights from the latch into the lights buffer, which the culling, DCLF and D3D11 read.
	struct LatchCopyBindings
	{
		org::ResourceBindingToken lights;
	};

	struct LatchCopyFrame
	{
		std::shared_ptr<const org::LatchBlock> latch;
		uint32_t bytes = 0;
	};

	class LatchLightsPass final : public org::TypedRenderGraphPass<LatchLightsPass, LatchCopyFrame, LatchCopyBindings>
	{
	public:
		explicit LatchLightsPass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		LatchCopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.CopyDestination(resources->lights->Graph()) };
		}

		void Revision(std::vector<uint64_t>& a_out) const
		{
			a_out.push_back(resources->lightBytes);
		}

		LatchCopyFrame Prepare(const LatchCopyBindings&, const org::PassPrepareContext&) const
		{
			return { resources->latch, resources->lightBytes };
		}

		static void Record(const LatchCopyBindings& a_bindings, const LatchCopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.latch || !a_frame.bytes)
				return;
			const auto region = a_frame.latch->At(a_recording.Run());
			a_recording.Commands().CopyBufferRegion(a_recording.Resolve(a_bindings.lights).GetHandle(), 0,
				region.buffer->GetAPIResource().GetHandle(), region.offset + kLatchLightsOffset, a_frame.bytes);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	// 64 clusters a group. Also resets the culling counter.
	struct BuildBindings
	{
		org::DeclaredViewToken clusters, counter;
	};

	class BuildClustersPass final : public org::TypedRenderGraphPass<BuildClustersPass, DispatchFrame, BuildBindings>
	{
	public:
		explicit BuildClustersPass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		BuildBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.UnorderedAccess(SlotOf(resources->clusters)).View(), a_builder.UnorderedAccess(resources->lightIndexCounter->Graph()).View() };
		}

		// The grids and the projection are latched, and the group counts are in the gate.
		void Revision(std::vector<uint64_t>& a_out) const
		{
			AppendProgramRevision(resources->build.get(), a_out);
		}

		DispatchFrame Prepare(const BuildBindings&, const org::PassPrepareContext&) const
		{
			DispatchFrame frame{};
			frame.program = resources->build;
			frame.constants = BaseConstants(offsetof(LLFLatch, cameraProjInverse));
			frame.entry = kBuildEntry;
			frame.latch = resources->latch;
			frame.gate = resources->gate;
			return frame;
		}

		static void Record(const BuildBindings& a_bindings, const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!Dispatches(a_frame))
				return;
			auto constants = a_frame.constants;
			constants.clustersIndex = a_recording.Index(a_bindings.clusters);
			constants.lightIndexCounterIndex = a_recording.Index(a_bindings.counter);
			RecordDispatch(a_frame, constants, a_recording);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	// Per-cluster light lists into lightIndexList and lightGrid.
	struct CullBindings
	{
		org::DeclaredViewToken clusters, lights, counter, lightIndexList, lightGrid;
	};

	class CullLightsPass final : public org::TypedRenderGraphPass<CullLightsPass, DispatchFrame, CullBindings>
	{
	public:
		explicit CullLightsPass(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		CullBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return {
				a_builder.ShaderResource(SlotOf(resources->clusters)).View(),
				a_builder.ShaderResource(resources->lights->Graph()).View(),
				a_builder.UnorderedAccess(resources->lightIndexCounter->Graph()).View(),
				a_builder.UnorderedAccess(SlotOf(resources->lightIndexList)).View(),
				a_builder.UnorderedAccess(SlotOf(resources->lightGrid)).View(),
			};
		}

		void Revision(std::vector<uint64_t>& a_out) const
		{
			AppendProgramRevision(resources->cull.get(), a_out);
		}

		DispatchFrame Prepare(const CullBindings&, const org::PassPrepareContext&) const
		{
			DispatchFrame frame{};
			frame.program = resources->cull;
			frame.constants = BaseConstants(offsetof(LLFLatch, cameraView));
			frame.entry = kCullEntry;
			frame.latch = resources->latch;
			frame.gate = resources->gate;
			return frame;
		}

		static void Record(const CullBindings& a_bindings, const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!Dispatches(a_frame))
				return;
			auto constants = a_frame.constants;
			constants.clustersIndex = a_recording.Index(a_bindings.clusters);
			constants.lightsIndex = a_recording.Index(a_bindings.lights);
			constants.lightIndexCounterIndex = a_recording.Index(a_bindings.counter);
			constants.lightIndexListIndex = a_recording.Index(a_bindings.lightIndexList);
			constants.lightGridIndex = a_recording.Index(a_bindings.lightGrid);
			RecordDispatch(a_frame, constants, a_recording);
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	class LightCullingExtension final : public org::RenderGraph::IRenderGraphExtension
	{
	public:
		explicit LightCullingExtension(std::shared_ptr<Resources> a_resources) :
			resources(std::move(a_resources)) {}

		// Registering the published three here too makes sure the culling's graph has them whatever order the extensions run in.
		// A declared buffer registers its slot.
		void PrepareForBuild(org::RenderGraph& a_graph) override
		{
			resources->lights->Register(a_graph);
			resources->clusters->Register(a_graph);
			resources->lightIndexCounter->Register(a_graph);
			resources->lightIndexList->Register(a_graph);
			resources->lightGrid->Register(a_graph);
		}

		void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
		{
			// Native mode: the pre-main segment holds the passes and gathers them at its step.
			if (FrameAnchors::HoldsForeign(static_cast<std::uint32_t>(RenderGraphRuntime::Segment::LightCulling)))
				return;
			GatherPasses(resources, RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::LightCulling), a_out);
		}

		/** @brief The light copy, cluster build and cull, tagged with a_epoch. */
		static void GatherPasses(const std::shared_ptr<Resources>& a_resources, std::uint32_t a_epoch, std::vector<org::RenderGraph::ExternalPassDesc>& a_out)
		{
			// On the graphics queue with the culling. On the copy queue it would wait on DXVK's stream and split DXVK's command list.
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.llf.latch-lights",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<LatchLightsPass>(a_resources)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(a_epoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.llf.build-clusters",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildClustersPass>(a_resources)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(a_epoch));
			a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.llf.cull-lights",
				std::static_pointer_cast<org::RenderPass>(std::make_shared<CullLightsPass>(a_resources)))
					.PreferQueue(org::QueueKind::Graphics)
					.Epoch(a_epoch));
		}

	private:
		std::shared_ptr<Resources> resources;
	};

	D3D11_BUFFER_DESC StructuredDesc(uint32_t a_elements, uint32_t a_stride, bool a_unorderedAccess)
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = a_elements * a_stride;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (a_unorderedAccess ? D3D11_BIND_UNORDERED_ACCESS : 0u);
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = a_stride;
		return desc;
	}

	GraphResources::Options PersistentOptions(bool a_exported)
	{
		return { .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = a_exported ? GraphResources::Export::SRV : GraphResources::Export::None };
	}

	/** @brief Materialized now, living as long as the culling. a_exported adds the D3D11 SRV pixel shaders bind. */
	std::unique_ptr<GraphResources::StructuredBuffer> CreateStructured(uint32_t a_elements, uint32_t a_stride, bool a_unorderedAccess, bool a_exported,
		std::string_view a_name)
	{
		return std::make_unique<GraphResources::StructuredBuffer>(StructuredDesc(a_elements, a_stride, a_unorderedAccess), a_elements, std::string(a_name),
			PersistentOptions(a_exported));
	}

	/**
	* @brief A buffer of a_perCluster elements a cluster of the display size's grid, declared on the display size, so a new display
	* size is a new version. a_exported adds the D3D11 SRV pixel shaders bind.
	*/
	Frame::DeclaredBuffer DeclareClusterBuffer(const Grid& a_setupGrid, uint32_t a_perCluster, uint32_t a_stride, bool a_exported, std::string_view a_name)
	{
		return Frame::DeclareBuffer(
			std::string(a_name), Frame::DependsOn(Frame::Vars::DisplaySize()),
			[a_setupGrid, a_perCluster, a_stride](const Frame::Inputs& a_values) {
				return StructuredDesc(ClustersOf(GridFor(a_setupGrid, a_values.Get(Frame::Vars::DisplaySize()))) * a_perCluster, a_stride, true);
			},
			PersistentOptions(a_exported));
	}

	bool Made(const std::unique_ptr<GraphResources::StructuredBuffer>& a_buffer, bool a_exported)
	{
		return a_buffer && *a_buffer && (!a_exported || a_buffer->SRV());
	}

	bool Made(const Frame::DeclaredBuffer& a_buffer, bool a_exported)
	{
		return a_buffer && static_cast<bool>(*a_buffer) && a_buffer->BufferVersions() && (!a_exported || a_buffer->SRV());
	}

	/** @brief Readers return to D3D11. */
	void WithdrawBuffers()
	{
		PublishedResources::Withdraw(ORGLightCulling::kLightsId);
		PublishedResources::Withdraw(ORGLightCulling::kLightIndexListId);
		PublishedResources::Withdraw(ORGLightCulling::kLightGridId);
	}
}

struct ORGLightCulling::Impl
{
	std::shared_ptr<Resources> resources;
	uint32_t maxLights = 0;
	uint32_t lightStride = 0;
};

ORGLightCulling& ORGLightCulling::Get()
{
	static ORGLightCulling s_culling;
	return s_culling;
}

ORGLightCulling::~ORGLightCulling() = default;

bool ORGLightCulling::Setup(const std::array<uint32_t, 3>& a_clusterCapacity, uint32_t a_maxLights, uint32_t a_lightStride, uint32_t a_maxLightsPerCluster)
{
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host) {
		if (impl)
			WithdrawBuffers();
		impl.reset();
		return false;
	}
	// The cluster buffers follow the display size through their declarations, so one setup serves the session.
	if (impl && impl->resources)
		return true;

	try {
		auto state = std::make_unique<Impl>();
		state->maxLights = a_maxLights;
		state->lightStride = a_lightStride;
		auto resources = std::make_shared<Resources>();
		auto device = host->Device();
		// The descriptor-heap build of the D3D11 shaders (LLF_ORG_BINDLESS).
		const auto load = [&](const char* a_source) {
			return ComputeProgram::Load(device, { .source = a_source, .constantWords = kConstantWords, .defines = { { L"LLF_ORG_BINDLESS", L"1" } } });
		};
		resources->build = load(kBuildShader);
		resources->cull = load(kCullShader);
		resources->gate = RenderGraph::RunGate::Create(kGateName, 2);
		if (!resources->build || !resources->cull || !resources->gate) {
			logger::warn("[ORG] Light culling stays on D3D11: the SPIR-V programs or the gate could not be created");
			impl.reset();
			return false;
		}
		resources->setupGrid = a_clusterCapacity;
		// The grid the first versions are made for: the newest display size's, as the declarations' descriptions read it.
		resources->firstGrid = GridFor(a_clusterCapacity, Frame::Inputs(nullptr).Get(Frame::Vars::DisplaySize()));
		resources->lightBytes = a_maxLights * a_lightStride;
		resources->lights = CreateStructured(a_maxLights, a_lightStride, false, true, kLightsId);
		resources->latch = std::make_shared<org::LatchBlock>("cs.llf.latch", kLatchLightsOffset + a_maxLights * a_lightStride);
		resources->clusters = DeclareClusterBuffer(a_clusterCapacity, 1, sizeof(float) * 8, false, kClustersName);
		resources->lightIndexCounter = CreateStructured(1, sizeof(uint32_t), true, false, kLightIndexCounterName);
		resources->lightIndexList = DeclareClusterBuffer(a_clusterCapacity, a_maxLightsPerCluster, sizeof(uint32_t), true, kLightIndexListId);
		resources->lightGrid = DeclareClusterBuffer(a_clusterCapacity, 1, sizeof(uint32_t) * 4, true, kLightGridId);
		if (!Made(resources->lights, true) || !Made(resources->clusters, false) || !Made(resources->lightIndexCounter, false) ||
			!Made(resources->lightIndexList, true) || !Made(resources->lightGrid, true)) {
			logger::warn("[ORG] Light culling stays on D3D11: its buffers could not be created or shared with D3D11");
			if (impl)
				WithdrawBuffers();
			impl.reset();
			return false;
		}
		state->resources = resources;
		// Replacing the extension rebuilds the graph at the next epoch, after the old graph retires.
		host->AddExtension(kExtensionId, [resources] { return std::make_unique<LightCullingExtension>(resources); });
		// Native mode: the pre-main segment holds the passes, on while the culling is active.
		FrameAnchors::SetForeignPasses(
			static_cast<std::uint32_t>(RenderGraphRuntime::Segment::LightCulling),
			[resources](std::uint32_t a_epoch, void* a_passes) {
				LightCullingExtension::GatherPasses(resources, a_epoch, *static_cast<std::vector<org::RenderGraph::ExternalPassDesc>*>(a_passes));
			},
			[] {
				// Read at frame start, before the segment's description.
				return ORGLightCulling::Get().IsActive();
			});
		// Other epochs read them by id, ordered after the culling. The declared two are published as their slots.
		if (!PublishedResources::Publish(kLightsId, *resources->lights) || !PublishedResources::Publish(kLightIndexListId, *resources->lightIndexList) ||
			!PublishedResources::Publish(kLightGridId, *resources->lightGrid)) {
			logger::warn("[ORG] Light culling stays on D3D11: its buffers could not be published");
			host->RemoveExtension(kExtensionId);
			WithdrawBuffers();
			impl.reset();
			return false;
		}
		impl = std::move(state);
		const auto& grid = resources->firstGrid;
		logger::info("[ORG] Light culling runs on the render graph ({} clusters, grid at most {}x{}x{}; the buffers follow the display size)", ClustersOf(grid),
			grid[0], grid[1], grid[2]);
		return true;
	} catch (const std::exception& e) {
		logger::error("[ORG] Light culling setup failed, staying on D3D11: {}", e.what());
		host->RemoveExtension(kExtensionId);
		WithdrawBuffers();
		impl.reset();
		return false;
	}
}

bool ORGLightCulling::Execute(const FrameInputs& a_inputs)
{
	if (!IsActive())
		return false;
	RenderGraphRuntime::EpochBodyScope body(RenderGraphRuntime::Segment::LightCulling);
	const auto resources = impl->resources;
	const uint32_t lightCount = a_inputs.lights ? (std::min)(a_inputs.lightCount, impl->maxLights) : 0u;
	const uint32_t bytes = lightCount * impl->lightStride;
	// The grid of the adopted revision goes into the run's latch. A grid the bound buffers do not hold is a caller error.
	const auto capacity = BoundGrid(*resources);
	for (uint32_t axis = 0; axis < 3; ++axis) {
		if (a_inputs.clusterSize[axis] && a_inputs.clusterSize[axis] <= capacity[axis])
			continue;
		const auto message = std::format("Community Shaders: the light culling's cluster grid {}x{}x{} is outside its buffers' {}x{}x{}",
			a_inputs.clusterSize[0], a_inputs.clusterSize[1], a_inputs.clusterSize[2], capacity[0], capacity[1], capacity[2]);
		logger::critical("[ORG] {}", message);
		spdlog::default_logger()->flush();
		stl::report_and_fail(message);
	}
	const auto commit = [&] {
		const org::RunRef run = RenderGraphRuntime::Get().Host()->CurrentRun();
		LLFLatch latch{};
		std::memcpy(latch.cameraProjInverse, a_inputs.cameraProjInverse.data(), sizeof(latch.cameraProjInverse));
		std::memcpy(latch.cameraView, a_inputs.cameraView.data(), sizeof(latch.cameraView));
		latch.lightsNear = a_inputs.lightsNear;
		latch.lightsFar = a_inputs.lightsFar;
		latch.lightCount = lightCount;
		std::copy(a_inputs.clusterSize, a_inputs.clusterSize + 3, latch.clusterSize);
		std::copy(capacity.begin(), capacity.end(), latch.clusterCapacity);
		resources->latch->WriteValue(run, 0, latch);
		if (bytes)
			resources->latch->Write(run, kLatchLightsOffset, { reinterpret_cast<const std::byte*>(a_inputs.lights), bytes });
		// The dispatches cover the bound buffers' grid. The shaders skip clusters outside the run's grid.
		resources->gate->OpenDispatch(run, kBuildEntry, (ClustersOf(capacity) + 63) / 64);
		resources->gate->OpenDispatch(run, kCullEntry, (capacity[0] + kCullGroupX - 1) / kCullGroupX, (capacity[1] + kCullGroupY - 1) / kCullGroupY,
			(capacity[2] + kCullGroupZ - 1) / kCullGroupZ);
	};
	// In a declared segment the values go into the segment's run at this tap. A tap outside the segment is a tap error and draws nothing.
	const auto segment = static_cast<std::uint32_t>(RenderGraphRuntime::Segment::LightCulling);
	if (FrameAnchors::HoldsForeign(segment))
		return FrameAnchors::TapForeign(segment, commit).status != FrameAnchors::RunStatus::Failed;
	const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::LightCulling, [&](org::RenderGraph&) { commit(); });
	if (!ok) {
		// The graph faulted, so LLF and the buffers' readers return to D3D11.
		WithdrawBuffers();
		impl.reset();
	}
	return ok;
}

bool ORGLightCulling::IsActive() const
{
	return impl && impl->resources && RenderGraphRuntime::Get().IsActive();
}

bool ORGLightCulling::AdoptedClusterGrid(std::array<uint32_t, 3>& a_grid) const
{
	if (!IsActive())
		return false;
	const auto& adopted = Frame::Revision::Adopted();
	const auto display = adopted.Get(Frame::Vars::DisplaySize());
	const auto ratios = adopted.Get(Frame::Vars::DynamicRatios());
	const auto capacity = BoundGrid(*impl->resources);
	// Before the display size's first event, the bound buffers' grid.
	if (!display.width || !display.height) {
		a_grid = capacity;
		return true;
	}
	// As Util::ConvertToDynamic: the render size is the display size times the ratios. 64 pixels a cluster.
	const auto axis = [](std::uint32_t a_size, float a_ratio, std::uint32_t a_capacity) {
		const auto pixels = static_cast<std::uint32_t>(static_cast<float>(a_size) * a_ratio);
		return std::clamp((pixels + 63) / 64, 1u, a_capacity);
	};
	a_grid = { axis(display.width, ratios.x, capacity[0]), axis(display.height, ratios.y, capacity[1]), capacity[2] };
	return true;
}

ID3D11ShaderResourceView* ORGLightCulling::GetLightsSRV() const
{
	return impl ? impl->resources->lights->SRV() : nullptr;
}

ID3D11ShaderResourceView* ORGLightCulling::GetLightIndexListSRV() const
{
	return impl ? impl->resources->lightIndexList->SRV() : nullptr;
}

ID3D11ShaderResourceView* ORGLightCulling::GetLightGridSRV() const
{
	return impl ? impl->resources->lightGrid->SRV() : nullptr;
}

bool ORGLightCulling::GetShaderResourceIndices(uint32_t& a_lights, uint32_t& a_lightIndexList, uint32_t& a_lightGrid) const
{
	// Each reader declares the same view by published id, so the graph orders its reads after the culling.
	return IsActive() && PublishedResources::ShaderResourceIndex(kLightsId, a_lights) &&
	       PublishedResources::ShaderResourceIndex(kLightIndexListId, a_lightIndexList) && PublishedResources::ShaderResourceIndex(kLightGridId, a_lightGrid);
}


#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph Light Limit Fix always culls with D3D11.
#	include "ORGLightCulling.h"

struct ORGLightCulling::Impl
{};

ORGLightCulling& ORGLightCulling::Get()
{
	static ORGLightCulling s_culling;
	return s_culling;
}

ORGLightCulling::~ORGLightCulling() = default;
bool ORGLightCulling::Setup(const std::array<uint32_t, 3>&, uint32_t, uint32_t, uint32_t) { return false; }
bool ORGLightCulling::Execute(const FrameInputs&) { return false; }
bool ORGLightCulling::IsActive() const { return false; }
bool ORGLightCulling::AdoptedClusterGrid(std::array<uint32_t, 3>&) const { return false; }
ID3D11ShaderResourceView* ORGLightCulling::GetLightsSRV() const { return nullptr; }
ID3D11ShaderResourceView* ORGLightCulling::GetLightIndexListSRV() const { return nullptr; }
ID3D11ShaderResourceView* ORGLightCulling::GetLightGridSRV() const { return nullptr; }
bool ORGLightCulling::GetShaderResourceIndices(uint32_t&, uint32_t&, uint32_t&) const { return false; }
#endif
