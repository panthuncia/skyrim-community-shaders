#pragma once

// The graph host's structural builds, kept off the render thread.
//
// Every rebuild request goes through RenderGraph::Builds::Ask, queued to the frame's adoption point, and the host only marks the
// epoch. At each frame boundary the render thread polls the manager, which attaches finished builds and hands the remaining
// requests to its build thread.
// The old graph runs until the new one is attached.
// Staged epochs' builds are staged, not attached: a frame revision, or a fragment adoption, attaches them with the
// recordings made on them (frame revision plan step 10a).

#include <cstdint>
#include <functional>
#include <memory>
#include <source_location>
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
		struct Desc
		{
			org::GraphHost* host = nullptr;
			// Epochs whose finished builds are staged, not attached. Frame revisions (and fragment adoptions) record them and
			// attach them with AttachStaged.
			std::vector<std::uint32_t> staged;
			// Staged epochs that are never built at a submission (fragment sources'). Once SetBuildInput gave an input, their first build runs on
			// the build lane too, and attaches when it is staged, since there is no graph to keep. They form their own group at each
			// poll, and Settled() reads them. They must be in staged.
			std::vector<std::uint32_t> laneOnly;
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
		/** @brief Any thread. Runs a_change on the owner thread at the next poll while no build runs, then rebuilds every epoch.
		*  For changes to what a graph captures at its build, which must not race a build in progress. */
		void Change(std::function<void()> a_change, std::string_view a_reason);

		/**
		* @brief Owner thread, inside a frame revision's adoption. Attaches the staged graphs of a_epochs as one step
		* (GraphHost::AttachStagedEpochGraphs) and runs their deferred work. Empty when attached, else the refused epoch and why.
		*/
		std::string AttachStaged(std::span<const std::uint32_t> a_epochs);

		/**
		* @brief Owner thread: runs after every poll, with the epochs it staged or attached (a fragment source settles its build state there).
		* Replaces the last one.
		*/
		void SetPollListener(std::function<void(std::span<const std::uint32_t> a_changed)> a_listener);

		/**
		* @brief Owner thread: what builds are made from (a fragment source's build state). a_input runs on the owner thread when a build is handed
		* out, and the build carries the value to whatever thread runs it (Builds::Input). Replaces the last one.
		*/
		void SetBuildInput(std::function<std::shared_ptr<const void>()> a_input);
		/** @brief The input a build made on the calling thread now reads: SetBuildInput's value on the owner thread, else null. */
		std::shared_ptr<const void> OwnerInput() const;

		/** @brief Owner thread, at a frame boundary. Attaches or stages finished builds and hands every other request to its builder.
		*  Returns whether a graph was attached, staged or built. */
		bool Poll(std::string_view a_point);
		/**
		* @brief Owner thread. Whether no lane-only epoch is requested or building, and each has a graph once there is a build input.
		* Staged ones are settled.
		*/
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
		/** @brief What Ask asks for. The fields add up: the epochs, the graphs that looked an id up, or every epoch. */
		struct Wanted
		{
			std::vector<std::string> ids;       // the graphs that looked these ids up are built again (GraphHost::RequestRebuildFor)
			std::vector<std::uint32_t> epochs;  // these epochs' graphs are built again
			bool all = false;                   // every epoch's graph is built again
			// A resource identity no version covers changed, such as a replaced import. Logged as a structural rebuild. Asked
			// inside the frame on the render thread, outside the adoption point, it is a late change (Frame/BoundaryCheck.h).
			bool structural = false;
			// Runs on the owner thread at the poll after the request is made, with no build running, then every epoch is built.
			// For a change to what a graph captures at its build. Runs at once while the graph is inactive.
			std::function<void()> change;
		};

		/**
		* @brief Any thread. Asks for graph builds, off the render thread. The request is kept until the frame's adoption point
		* (Frame::Driver::OnBegin, IssueAdoptionRequests) and made there. Before the first frame, and inside the adoption point, it
		* is made at once. a_why is logged with the builds.
		*/
		void Ask(Wanted a_wanted, std::string_view a_why, const std::source_location& a_where = std::source_location::current());
		/** @brief Inside the frame's adoption point (Frame::Driver::OnBegin), on the render owner: makes every request Ask kept. */
		void IssueAdoptionRequests();

		/** @brief Inside an extension's build, defers a_attached to the graph's attach and a_failed to its failure. Neither runs if it is dropped.
		*  Returns false outside a managed build, where the caller applies its change at once. */
		bool Defer(std::function<void()> a_attached, std::function<void(const std::string&)> a_failed = {});
		/** @brief Inside an extension's build, runs a_staged when the graph is staged, before its attach. False outside a managed build. */
		bool DeferStaged(std::function<void()> a_staged);
		/**
		* @brief Inside an extension's build, what it is made from: the value BuildManager::SetBuildInput gave when the build was handed
		* out. The host's own builds on the owner thread take it then. Null elsewhere. An extension reads it once, at its make.
		*/
		std::shared_ptr<const void> Input();
	}
}
