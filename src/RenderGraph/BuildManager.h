#pragma once

// The graph host's structural builds, kept off the render thread.
//
// Every rebuild request goes through RenderGraph::Builds, and the host only marks the epoch. At each frame boundary the render
// thread polls the manager, which attaches finished builds and hands the remaining requests to its build thread.
// The old graph runs until the new one is attached.
// An owner can build its epochs on its own worker (DCLF on its scene worker). The manager still decides what is built, and
// those builds are staged: DCLF attaches them with the snapshot recorded on them at a Begin (frame revision plan step 10a).

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
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
		/** @brief An epoch owner that builds its graphs on its own worker. The manager keeps the policy and the logs. */
		struct ExternalBuilder
		{
			// Runs one epoch's build on the builder's worker. False means refused, and the epoch builds on the owner thread instead.
			std::function<bool(std::function<void()> a_build)> post;
			// Held around every build made on the owner thread, so the owner's extensions read a consistent state there too.
			std::function<std::shared_ptr<void>()> ownerScope;
			// Owner thread, after every poll. a_staged says whether one of its epochs was staged.
			std::function<void(bool a_staged)> boundary;
		};

		struct Desc
		{
			org::GraphHost* host = nullptr;
			// Epochs built on the render thread at the poll, such as DCLF's with CS_DCLF_ASYNC_BUILDS=0. Logged as synchronous after the first world frame.
			std::vector<std::uint32_t> ownerBuilt;
			// Epochs the external builder builds. Each finished group is taken at the next poll.
			std::vector<std::uint32_t> external;
			// Epochs whose finished builds are staged, not attached, on either thread. Frame revisions (and DCLF's snapshot adoption)
			// record them and attach them with AttachStaged. Owner-built and external epochs must be in it, since a poll inside a frame
			// stages them.
			std::vector<std::uint32_t> staged;
			// An epoch's name in the log lines.
			std::function<std::string(std::uint32_t)> label;
			// A failed build with no handler of its own goes to the epoch's owner like any epoch failure. kept means the old graph still runs.
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
		* @brief Owner thread, inside a frame revision's adoption. Rebuilds every graph now, with the host's thread held, so the shader
		* reload's staged pipelines are swapped in (the pipeline registry's extension publishes them at this build). The one
		* render-thread build left: a kept recording binds its pipelines at record time, so none may outlive the swap (plan Q5).
		* Returns whether a graph was built.
		*/
		bool SwapPipelines(std::string_view a_reason);
		/** @brief Any thread. Runs a_change on the owner thread at the next poll while no build runs, then rebuilds every epoch.
		*  For changes to what a graph captures at its build, which must not race a build in progress. */
		void Change(std::function<void()> a_change, std::string_view a_reason);

		/**
		* @brief Owner thread, inside a frame revision's adoption. Attaches the staged graphs of a_epochs as one step
		* (GraphHost::AttachStagedEpochGraphs) and runs their deferred work. Empty when attached, else the refused epoch and why.
		*/
		std::string AttachStaged(std::span<const std::uint32_t> a_epochs);

		/** @brief Owner thread: the external epochs' builder (replaces the last one). */
		void SetExternalBuilder(ExternalBuilder a_builder);

		/** @brief Owner thread, at a frame boundary. Attaches or stages finished builds and hands every other request to its builder.
		*  Returns whether a graph was attached, staged or built. */
		bool Poll(std::string_view a_point);
		/** @brief Owner thread. Whether no owner-built or external epoch is requested, or building. Staged ones are settled. */
		bool Settled() const;
		/** @brief Owner thread. Whether a rebuild was asked for an epoch whose builds attach at once. Staged epochs never leave a frame without a graph. */
		bool UnstagedRebuildRequested() const;
		/** @brief Owner thread, at setup. Builds every request now before the first world frame, else acts as Poll. */
		bool BuildSetupGraphs(std::string_view a_caller);

		/** @brief Owner thread: no build queued, running or waiting to be attached. */
		bool Idle() const;
		/** @brief Owner thread, at every frame's end: the first-world-frame latch and the periodic statistics line. */
		void NoteFrameEnd();
		/** @brief Logs a render-thread build after the first world frame, rate-limited. */
		void WarnSynchronous(std::string_view a_what, std::string_view a_caller);

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};

	/** Requests routed through the build manager, no-ops while the graph is inactive. Use these instead of the host's RequestRebuild. */
	namespace Builds
	{
		/** @brief Any thread: the epoch's graph is built again, off the render thread. */
		void Request(std::uint32_t a_epoch, std::string_view a_reason);
		/** @brief Any thread: every epoch's graph is built again, off the render thread. */
		void RequestAll(std::string_view a_reason);
		/** @brief Any thread: the graphs that looked a_id up are built again, off the render thread. */
		void RequestFor(std::string_view a_id, std::string_view a_reason);
		/** @brief Any thread. Runs a_change at the next poll with no build running, then rebuilds every epoch. At once while inactive. */
		void Change(std::function<void()> a_change, std::string_view a_reason);

		/** @brief Inside an extension's build, defers a_attached to the graph's attach and a_failed to its failure. Neither runs if it is dropped.
		*  Returns false outside a managed build, where the caller applies its change at once. */
		bool Defer(std::function<void()> a_attached, std::function<void(const std::string&)> a_failed = {});
		/** @brief Inside an extension's build, runs a_staged when the graph is staged, before its attach. False outside a managed build. */
		bool DeferStaged(std::function<void()> a_staged);
		/** @brief Render thread, inside a frame revision's adoption. BuildManager::AttachStaged. Empty when attached, else why not. */
		std::string AttachStaged(std::span<const std::uint32_t> a_epochs);
		/** @brief Whether the calling thread is inside a build the manager makes (the build thread, or an owner-built epoch's build). */
		bool InManagedBuild();
	}
}
