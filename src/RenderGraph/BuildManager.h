#pragma once

// The graph host's structural builds, off the render thread (rule 1 of docs/development/org-native-frame-graph.md: no structural
// recompile on the hot render path).
//
// Every rebuild request in CS goes through RenderGraph::Builds (or reaches the host directly from ORG, an extension added or removed):
// the host only marks the epoch (GraphHost::SetExplicitBuilds). At each frame boundary the render thread polls the manager
// (RenderGraphRuntime::BuildIfRequested). The poll attaches the builds that finished (GraphHost::AttachEpochGraph) and hands every
// epoch still asked for to the manager's own build thread (GraphHost::BuildEpochGraph). The old graph runs until the new one is
// attached.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace org
{
	class GraphHost;
}

namespace RenderGraph
{
	class BuildManager
	{
	public:
		struct Desc
		{
			org::GraphHost* host = nullptr;
			// Epochs built on the render thread at the poll, not on the build thread (Drawcall Limit Fix's: its extensions read state the
			// render thread changes in place). Each such build is logged as synchronous after the first world frame.
			std::vector<std::uint32_t> ownerBuilt;
			// An epoch's name in the log lines.
			std::function<std::string(std::uint32_t)> label;
			// A build that failed and whose extensions left no handler of their own (Builds::Defer): the epoch's owner hears of it as of
			// any epoch failure (RenderGraphRuntime::OnEpochFailed). kept: the epoch still runs its previous graph.
			std::function<void(std::uint32_t, bool, const std::string&)> onFailed;
		};

		explicit BuildManager(Desc a_desc);
		~BuildManager();
		BuildManager(const BuildManager&) = delete;
		BuildManager& operator=(const BuildManager&) = delete;

		/** @brief Stops the build thread (it finishes the build it is in) and drops every build not attached. Owner thread. */
		void Shutdown();

		/** @brief Any thread: the epoch's graph is built again. Off the owner thread the request is applied at the next poll. */
		void Request(std::uint32_t a_epoch, std::string_view a_reason);
		/** @brief Any thread: every epoch's graph is built again. */
		void RequestAll(std::string_view a_reason);
		/** @brief Any thread: the graphs that looked a_id up are built again (GraphHost::RequestRebuildFor). */
		void RequestFor(std::string_view a_id, std::string_view a_reason);
		/**
		 * @brief Any thread: every graph is built again on the owner thread at the next poll (GraphHost::BuildIfRequested), with the
		 * host's thread held. For a change that needs the host's safe point (shader reload's pipeline swap), not for structure.
		 */
		void RequestAllSynchronous(std::string_view a_reason);

		/**
		 * @brief Owner thread, at a frame boundary: attaches the finished builds, builds the owner-built epochs that were asked for, and
		 * hands every other epoch asked for to the build thread. a_point names the caller. Returns whether a graph was attached or built.
		 */
		bool Poll(std::string_view a_point);
		/** @brief Owner thread: the owner-built epochs that were asked for, built now (mid-frame). Logged as synchronous. */
		bool BuildOwnerEpochsNow(std::string_view a_caller);
		/**
		 * @brief Owner thread, at setup: before the first world frame every graph asked for is built now (GraphHost::BuildIfRequested);
		 * after it, the same as Poll. Returns whether the builds were made now.
		 */
		bool BuildSetupGraphs(std::string_view a_caller);

		/** @brief Owner thread: no build queued, running or waiting to be attached. */
		bool Idle() const;
		/** @brief Owner thread, at every frame's end: the first-world-frame latch and the periodic statistics line. */
		void NoteFrameEnd();
		/** @brief Whether a world frame was seen: from then on a build on the render thread is a violation, and is logged. */
		bool WorldStarted() const;
		/** @brief Logs a build on the render thread after the first world frame (rate-limited): what was built, and by whom. */
		void WarnSynchronous(std::string_view a_what, std::string_view a_caller);

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};

	/**
	 * Requests routed through the runtime's build manager. Each is a no-op while the graph is inactive. Use these instead of
	 * org::GraphHost::RequestRebuild and RequestRebuildFor.
	 */
	namespace Builds
	{
		/** @brief Any thread: the epoch's graph is built again, off the render thread. */
		void Request(std::uint32_t a_epoch, std::string_view a_reason);
		/** @brief Any thread: every epoch's graph is built again, off the render thread. */
		void RequestAll(std::string_view a_reason);
		/** @brief Any thread: the graphs that looked a_id up are built again, off the render thread. */
		void RequestFor(std::string_view a_id, std::string_view a_reason);

		/**
		 * @brief Inside an extension's build (its factory, PrepareForBuild, Gather, its passes' setup): defers a_attached to the attach of
		 * the graph being built, on the render thread, and a_failed to its failure. The graph may be dropped (a newer request superseded
		 * it, the host's configuration changed): then neither runs. Returns false outside a managed build, where the graph is installed
		 * as soon as it is built: the caller applies its change at once.
		 */
		bool Defer(std::function<void()> a_attached, std::function<void(const std::string&)> a_failed = {});
		/** @brief Whether the calling thread is inside a build the manager makes (the build thread, or an owner-built epoch's build). */
		bool InManagedBuild();
	}
}
