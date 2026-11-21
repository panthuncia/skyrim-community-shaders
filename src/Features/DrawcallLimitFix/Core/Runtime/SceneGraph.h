#pragma once

#include <ORGModuleServices/Async/ArtifactResources.h>
#include <ORGModuleServices/Async/GraphScheduler.h>
#include <ORGModuleServices/Async/StateGraphTypes.h>
#include <ORGModuleServices/Async/VersionGrowthArtifact.h>
#include <ORGModuleServices/Async/VersionedBufferPosts.h>
#include <ORGModuleServices/Async/WindowGates.h>
#include <ORGModuleServices/VersionedBufferBuild.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace DCLF::Runtime
{
	/**
	 * @brief The artifact kinds of DCLF's state graph, described in dclf-architecture.md, "The design".
	 * SceneState, BufferVersion and PayloadPart requests are exact and never coalesced. The other kinds are replaceable,
	 * so a newer request supersedes an unstarted older one.
	 */
	enum class SceneArtifact : std::uint32_t
	{
		SceneState,         // the scene tables after a frame's events
		Lookups,            // material, pipeline and texture lookups
		FrameValues,        // a frame's placements, palettes, shading samples and visibility
		ShadowPayload,      // the per-segment payloads
		ZPrepassPayload,
		ColourPayload,
		ReflectionPayload,
		OcclusionPayload,
		Publication,        // what the render thread accepts: claims, filters and every segment's payload
		BufferVersion,      // a versioned buffer's next version, made and filled as graph work (growth: PostGrowth)
		PayloadPart,        // a scene payload part's next version, built from its predecessor (PostPayloadPart)
		TextureWindow,      // a streamed texture's window: read and made on the streaming domain, a newer one replacing an unstarted one
		TextureGate,        // a request waiting on a texture's base window (ReadyGateAtLeast), which then runs
		Count
	};

	/** @brief A growth as graph work: a BufferVersion made on the preparation pool, with done run on the coordinator lane. */
	using GrowthWork = org::async::VersionGrowthWork;

	/** @brief A payload part version's tag. built runs on the producer's thread once the version is made, before it is frozen. Must not throw. */
	struct PayloadPartTag
	{
		std::function<void(const org::services::PublishedBufferVersion& a_version)> built;
	};
	/** @brief A payload part's outcome on the coordinator lane: the version once GPU ready, or null and why. */
	using PayloadPartDone = org::services::VersionedBufferDone;

	class SceneGraph
	{
	public:
		using Graph = org::async::StateGraph<org::async::DefaultStateGraphTypes>;

		/** @brief a_scheduler must be the executor whose Layout the graph is built for. */
		explicit SceneGraph(std::shared_ptr<org::async::GraphScheduler> a_scheduler);
		~SceneGraph();
		SceneGraph(const SceneGraph&) = delete;
		SceneGraph& operator=(const SceneGraph&) = delete;

		Graph& Get();

		/**
		 * @brief Posts a growth, lock-free. Only the render thread posts, and it keeps the waits, dropping finished ones at its next post.
		 * Returns false when the graph is shutting down, and then done never runs.
		 */
		bool PostGrowth(GrowthWork a_work);
		/**
		 * @brief Posts a payload part's next version, lock-free. Built on the preparation pool, where an exhausted pool suspends it until a backing frees.
		 * Each version gets its own address, released once a_done runs at GPU readiness or its end. Only the snapshot builder posts.
		 * Returns false when the graph is shutting down, and then a_done never runs.
		 */
		bool PostPayloadPart(org::services::VersionedBufferBuildInput a_input, PayloadPartDone a_done);
		/**
		 * @brief Any thread, lock-free. a_build makes a texture's window on the streaming domain. A later revision replaces this one if not started.
		 * a_build returns what the window still waits for, such as its upload, or null when it is already GPU ready.
		 */
		bool PostTextureWindow(std::uint64_t a_texture, std::uint64_t a_revision, std::function<std::shared_ptr<const org::async::GpuSubmissionSet>()> a_build);
		/** @brief The texture is gone, so its window's address goes too. */
		void RetireTextureWindow(std::uint64_t a_texture);
		/** @brief Runs a_run once the texture's window reaches a_revision and is GPU ready. Returns false when shutting down, and then a_run never runs. */
		bool PostTextureGate(std::uint64_t a_texture, std::uint64_t a_revision, std::function<void()> a_run);

	private:
		std::unique_ptr<Graph> graph;
		std::unique_ptr<org::async::VersionGrowthPosts<org::async::DefaultStateGraphTypes>> growths;       // the posting thread's
		std::unique_ptr<org::services::VersionedBufferPosts<org::async::DefaultStateGraphTypes>> payloadParts;  // the snapshot builder's
		std::unique_ptr<org::async::WindowGates<org::async::DefaultStateGraphTypes>> textureGates;          // texture windows and their gates
	};
}
