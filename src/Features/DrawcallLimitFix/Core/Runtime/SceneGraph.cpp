// SceneGraph: the state graph's hooks, the growth producer and the payload parts' producer.
// Producers run on the preparation pool and their outcomes on the coordinator lane.
#include <mutex>
#include <vector>
#include "Features/DrawcallLimitFix/Core/Runtime/SceneGraph.h"
#include "RenderGraph/Async/Executor.h"

#include <ORGModuleServices/Async/StateGraph.h>
#include <ORGModuleServices/Async/VersionGrowthArtifact.h>
#include <ORGModuleServices/Async/VersionedBufferPosts.h>
#include <ORGModuleServices/Async/VersionedBufferProducer.h>
#include <ORGModuleServices/Async/WindowGates.h>

#include <array>
#include <span>

namespace DCLF::Runtime
{
	namespace
	{
		org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> Hooks()
		{
			org::async::StateGraphHooks<org::async::DefaultStateGraphTypes> hooks;
			hooks.scheduling = ::RenderGraph::Frame::Executor::Layout;
			hooks.producerDispatch = { org::async::TaskDispatch::Controlled, org::async::TaskDispatch::Cpu, org::async::TaskDispatch::Controlled, org::async::TaskDispatch::Cpu };  // per domain: coordinator, preparation, scene, streaming
			static_assert(static_cast<std::size_t>(SceneArtifact::Count) <= org::async::DefaultStateGraphTypes::kArtifactKindCount);
			// The scene's revisions are its events, in order, so none is replaced by a later one.
			hooks.artifactPolicies[static_cast<std::size_t>(SceneArtifact::SceneState)].allowCoalescing = false;
			// Each growth has its own address and is never replaced.
			hooks.artifactPolicies[static_cast<std::size_t>(SceneArtifact::BufferVersion)].allowCoalescing = false;
			// So does each payload part version, since a part's versions form a chain built one from the next.
			hooks.artifactPolicies[static_cast<std::size_t>(SceneArtifact::PayloadPart)].allowCoalescing = false;
			hooks.artifactPolicies[static_cast<std::size_t>(SceneArtifact::TextureGate)].allowCoalescing = false;
			return hooks;
		}

		constexpr auto kBufferVersion = static_cast<std::uint16_t>(SceneArtifact::BufferVersion);
		constexpr auto kPayloadPart = static_cast<std::uint16_t>(SceneArtifact::PayloadPart);
		constexpr auto kTextureWindow = static_cast<std::uint16_t>(SceneArtifact::TextureWindow);
		constexpr auto kTextureGate = static_cast<std::uint16_t>(SceneArtifact::TextureGate);
	}

	SceneGraph::SceneGraph(std::shared_ptr<org::async::GraphScheduler> a_scheduler) :
		graph(std::make_unique<Graph>(std::move(a_scheduler), "DCLF scene", Hooks())),
		growths(std::make_unique<org::async::VersionGrowthPosts<org::async::DefaultStateGraphTypes>>(kBufferVersion, ::RenderGraph::Frame::Executor::Coordinator.lane,
			::RenderGraph::Frame::Executor::Coordinator.domain)),
		payloadParts(std::make_unique<org::services::VersionedBufferPosts<org::async::DefaultStateGraphTypes>>(kPayloadPart, ::RenderGraph::Frame::Executor::Coordinator.lane,
			::RenderGraph::Frame::Executor::Coordinator.domain)),
		textureGates(std::make_unique<org::async::WindowGates<org::async::DefaultStateGraphTypes>>(kTextureWindow, kTextureGate))
	{
		// A window does blocking file reads and texture creation on the streaming domain. A failed read still completes, so its gate opens
		// on a texture that reads zero instead of waiting forever. A window the render graph uploads is GPU ready once its copy pass was submitted.
		textureGates->RegisterWindows(*graph, ::RenderGraph::Frame::Executor::Streaming.lane, ::RenderGraph::Frame::Executor::Streaming.domain, "DCLF texture window");
		// A gate runs its request once the base window is ready. The graph does the wake.
		textureGates->RegisterGates(*graph, ::RenderGraph::Frame::Executor::Preparation.lane, ::RenderGraph::Frame::Executor::Preparation.domain, "DCLF texture gate");
		// Growth runs on the preparation pool and is ready once its uploads complete.
		org::async::RegisterVersionGrowthProducer(*graph, kBufferVersion, ::RenderGraph::Frame::Executor::Preparation.lane, ::RenderGraph::Frame::Executor::Preparation.domain, "DCLF buffer version");
		// A payload part version is built from its predecessor on the preparation pool, ready once its copy and upload complete.
		// Its tag's built runs on the producer's thread before it is frozen.
		org::services::VersionedBufferProducerHooks<org::async::DefaultStateGraphTypes> hooks;
		hooks.decorate = [](org::services::PublishedBufferVersion& a_version, const org::services::VersionedBufferBuildInput& a_input) {
			if (const auto* tag = static_cast<const PayloadPartTag*>(a_input.tag.get()); tag && tag->built)
				tag->built(a_version);
		};
		static constexpr std::array<std::uint16_t, 1> kPayloadKinds{ kPayloadPart };
		org::services::RegisterVersionedBufferProducer(*graph, std::span<const std::uint16_t>(kPayloadKinds), ::RenderGraph::Frame::Executor::Preparation.lane,
			::RenderGraph::Frame::Executor::Preparation.domain, "DCLF payload part", std::move(hooks));
	}

	bool SceneGraph::PostTextureWindow(std::uint64_t a_texture, std::uint64_t a_revision, std::function<std::shared_ptr<const org::async::GpuSubmissionSet>()> a_build)
	{
		return textureGates->PostWindow(*graph, a_texture, a_revision, std::move(a_build));
	}

	void SceneGraph::RetireTextureWindow(std::uint64_t a_texture)
	{
		textureGates->RetireWindow(*graph, a_texture);
	}

	bool SceneGraph::PostTextureGate(std::uint64_t a_texture, std::uint64_t a_revision, std::function<void()> a_run)
	{
		return textureGates->PostGate(*graph, a_texture, a_revision, std::move(a_run));
	}

	bool SceneGraph::PostGrowth(GrowthWork a_work)
	{
		return org::async::PostVersionGrowth(*graph, *growths, std::move(a_work));
	}

	bool SceneGraph::PostPayloadPart(org::services::VersionedBufferBuildInput a_input, PayloadPartDone a_done)
	{
		return org::services::PostVersionedBuffer(*graph, *payloadParts, std::move(a_input), std::move(a_done));
	}

	SceneGraph::~SceneGraph()
	{
		if (graph)
			graph->Shutdown();
	}

	SceneGraph::Graph& SceneGraph::Get()
	{
		return *graph;
	}
}
