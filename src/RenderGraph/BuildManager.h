#pragma once

// The graph host's structural builds, kept off the render thread.
//
// Every rebuild request goes through RenderGraph::Builds::Ask, queued to the frame's adoption point, and the host only marks the
// epoch. At each frame boundary the render thread polls the manager, which attaches finished builds and hands the remaining
// requests to its build thread.
// The old graph runs until the new one is attached.
// Staged epochs' builds are staged, not attached: a frame revision, or a fragment adoption, attaches them with the
// recordings made on them (frame revision plan step 10a).

#include <ORGModuleServices/StagedBuilds.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace RenderGraph
{
	/** @brief CS's build manager is OMS's staged builds. */
	using BuildManager = org::services::StagedBuilds;

	/**
	* @brief Makes the build manager from a_desc's host, epochs, labels and failure handler. Adds what CS gives every build: the
	* shared executor's preparation lane, the frame boundary census, the recipe trace, the world window as the steady state, and the log.
	*/
	std::unique_ptr<BuildManager> MakeBuildManager(BuildManager::Desc a_desc);

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
