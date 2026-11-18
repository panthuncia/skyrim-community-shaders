#pragma once

// The graph host's structural builds, off the render thread (rule 1 of docs/development/org-native-frame-graph.md: no structural
// recompile on the hot render path).
//
// Every rebuild request in CS goes through RenderGraph::Builds (or reaches the host directly from ORG, an extension added or removed):
// the host only marks the epoch (GraphHost::SetExplicitBuilds). At each frame boundary the render thread polls the manager
// (RenderGraphRuntime::BuildIfRequested). The poll attaches the builds that finished (GraphHost::AttachEpochGraph) and hands every
// epoch still asked for to the manager's own build thread (GraphHost::BuildEpochGraph). The old graph runs until the new one is
// attached.
//
// An epoch owner can build its epochs on a worker of its own (ExternalBuilder: Drawcall Limit Fix builds on its scene worker from an
// immutable build state). The manager still decides what is built, supersedes, logs, and keeps the old graph on a failure; those builds
// attach only at a frame's end (Attach::All) or at the owner's own wait (Attach::External).

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
		/** @brief Which finished builds a poll attaches. */
		enum class Attach
		{
			Live,      // every epoch's but the external builder's (a frame's start)
			All,       // every epoch's (a frame's end)
			External,  // the external builder's alone (its owner's wait at a frame's start)
		};

		/**
		 * @brief An epoch owner that builds its epochs' graphs on a worker of its own (Drawcall Limit Fix: its scene worker, from its
		 * immutable build state). The manager keeps the policy: what is built, supersede, attach at a frame's end, failure, the logs.
		 */
		struct ExternalBuilder
		{
			// Owner thread, at a poll: runs a_build (one epoch's build) on the builder's worker. False: refused; that epoch is built on the
			// owner thread at this poll instead. Null: every external epoch is built on the owner thread.
			std::function<bool(std::function<void()> a_build)> post;
			// Owner thread: held around every build the manager makes on the owner thread (an owner-built or synchronous one), so the
			// owner's extensions read a consistent state there too. Null: nothing held.
			std::function<std::shared_ptr<void>()> ownerScope;
			// Owner thread, after each poll that may attach the external epochs (Attach::All and Attach::External); a_attached: one was.
			std::function<void(bool a_attached)> boundary;
		};

		struct Desc
		{
			org::GraphHost* host = nullptr;
			// Epochs built on the render thread at the poll, not on the build thread (Drawcall Limit Fix's with CS_DCLF_ASYNC_BUILDS=0).
			// Each such build is logged as synchronous after the first world frame.
			std::vector<std::uint32_t> ownerBuilt;
			// Epochs the external builder builds (Drawcall Limit Fix's), attached only at Attach::All and Attach::External polls.
			std::vector<std::uint32_t> external;
			// Epochs whose finished builds are staged, not attached (GraphHost::StageEpochGraph): the host's thread records them before
			// they run, and their owner switches them in at a frame boundary with SwitchStaged (CS's stages).
			std::vector<std::uint32_t> staged;
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
		 * @brief Any thread: a_change runs on the owner thread at the next poll, while no build runs on either thread, and then every
		 * epoch's graph is built again off the render thread. For a change of what a graph captures at its build (a resource's
		 * ownership) that must not race a build in progress.
		 */
		void Change(std::function<void()> a_change, std::string_view a_reason);

		/**
		 * @brief Owner thread, at a frame boundary: attaches the epoch's staged graph (GraphHost::AttachStagedEpochGraph) and runs what its
		 * build deferred to its attach (Builds::Defer). False when nothing is staged or the attach was refused.
		 */
		bool SwitchStaged(std::uint32_t a_epoch);

		/** @brief Owner thread: the external epochs' builder (replaces the last one). */
		void SetExternalBuilder(ExternalBuilder a_builder);

		/**
		 * @brief Owner thread, at a frame boundary: attaches the finished builds a_attach names, builds the owner-built epochs that were
		 * asked for, and hands every other epoch asked for to the build thread (the external ones to their builder). a_point names the
		 * caller. Returns whether a graph was attached or built.
		 */
		bool Poll(std::string_view a_point, Attach a_attach = Attach::Live);
		/**
		 * @brief Owner thread: the owner-built and external epochs that were asked for, built now on this thread (mid-frame), even one
		 * being built elsewhere (that build is then rejected at its attach). Logged as synchronous.
		 */
		bool BuildOwnerEpochsNow(std::string_view a_caller);
		/**
		 * @brief Owner thread: none of the owner-built or external epochs is asked for (with a graph to replace, and not known to fail as
		 * asked), being built, or built and waiting for its attach.
		 */
		bool Settled() const;
		/**
		 * @brief Owner thread: a rebuild was asked for an epoch whose builds attach at once (not a staged CS stage). A staged epoch keeps
		 * running its attached graph until its new build switches in whole, so its request never leaves a frame without its graph.
		 */
		bool UnstagedRebuildRequested() const;
		/**
		 * @brief Owner thread, at setup: before the first world frame every graph asked for is built now (GraphHost::BuildIfRequested);
		 * after it, the same as Poll. Returns whether the builds were made now.
		 */
		bool BuildSetupGraphs(std::string_view a_caller);

		/** @brief Owner thread: no build queued, running or waiting to be attached. */
		bool Idle() const;
		/** @brief Owner thread, at every frame's end: the first-world-frame latch and the periodic statistics line. */
		void NoteFrameEnd();
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
		 * @brief Any thread: a_change runs on the render thread at the next poll, with no build running, then every epoch's graph is
		 * built again off the render thread (BuildManager::Change). Runs a_change at once while the graph is inactive.
		 */
		void Change(std::function<void()> a_change, std::string_view a_reason);

		/**
		 * @brief Inside an extension's build (its factory, PrepareForBuild, Gather, its passes' setup): defers a_attached to the attach of
		 * the graph being built, on the render thread, and a_failed to its failure. The graph may be dropped (a newer request superseded
		 * it, the host's configuration changed): then neither runs. Returns false outside a managed build, where the graph is installed
		 * as soon as it is built: the caller applies its change at once.
		 */
		bool Defer(std::function<void()> a_attached, std::function<void(const std::string&)> a_failed = {});
		/**
		 * @brief Inside an extension's build: a_staged runs on the render thread when the graph being built is staged (BuildManager::Desc::
		 * staged), before its attach. Not called for a graph that is attached at once. False outside a managed build.
		 */
		bool DeferStaged(std::function<void()> a_staged);
		/** @brief Render thread, at a frame boundary: switches in the epoch's staged graph (BuildManager::SwitchStaged). */
		bool SwitchStaged(std::uint32_t a_epoch);
		/** @brief Whether the calling thread is inside a build the manager makes (the build thread, or an owner-built epoch's build). */
		bool InManagedBuild();
	}
}
