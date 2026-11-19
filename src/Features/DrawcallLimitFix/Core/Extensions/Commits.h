#pragma once

// Core's side of Api/Commits.h: what a commit hook gets and the call commits make, plus the feature rows of PS b13 for frame constant rows.

#include "Features/DrawcallLimitFix/Api/Commits.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace org::services
{
	class NamedVersions;
}

namespace DCLF
{
	struct SceneTables;
	namespace Draws
	{
		struct SceneBuffers;
		class CommitUploads;
		class LatchedUploads;
	}
}

namespace DCLF::Api
{
	/** @brief What a hook's commit gets on the render thread: the adopted set, the commit's uploads, its latched uploads and execution owners. */
	class CommitContext
	{
	public:
		CommitKind kind;
		const org::services::NamedVersions& adopted;  // the versions the commit writes into, and their capacities
		const SceneTables& tables;
		std::uint32_t frame;
		Draws::SceneBuffers& scene;
		Draws::CommitUploads& uploads;                // into a versioned buffer, the version adopted names
		Draws::LatchedUploads* latched;               // null for the shadow commit
		// Held until this commit's execution retires, for what only its own GPU work reads.
		// A descriptor a row names goes with the row instead, since later executions read the row too.
		std::vector<std::shared_ptr<const void>>& executionOwners;
	};
}

namespace DCLF::Extensions
{
	/** @brief Render thread. Every registered hook's commit, in order. */
	void RunCommitHooks(const Api::CommitContext& a_commit);

	/** @brief Core's feature rows of PS b13: c17 to c20, after the extras' pixel row at c16. */
	inline constexpr std::uint32_t kFrameConstantRowFirst = 17;
	inline constexpr std::uint32_t kFrameConstantRowsEnd = 21;
}
