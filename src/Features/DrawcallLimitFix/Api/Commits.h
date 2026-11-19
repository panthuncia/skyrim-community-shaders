#pragma once

// Commit hooks write a feature's part of a commit's frame data on the render thread, before any pass of the commit's epoch runs.
// Frame constant rows are a feature's float4 in Core's feature rows of PS b13, latched by each main commit.

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/Commits.h.
	class CommitContext;

	/** @brief The commit a hook runs in. */
	enum class CommitKind : std::uint8_t
	{
		Depth,   // main depth epoch, whose graph also runs the next frame's state passes
		Colour,  // main colour epoch
		Shadow,  // the shadow epoch's shared inputs
	};

	/**
	 * @brief A commit hook. Runs on the render thread in each commit, after the vertex stream uploads, in registration order.
	 * A frame commits each kind at most once.
	 */
	struct CommitHook
	{
		std::string_view name;
		void (*commit)(const CommitContext& a_commit) = nullptr;
	};

	/**
	 * @brief A float4 every draw reads in Core's feature rows of PS b13, from c17 up to Extensions::kFrameConstantRowsEnd.
	 * sample runs in each main commit. Rows are latched in ascending row order, whatever the registration order.
	 */
	struct FrameConstantRow
	{
		std::string_view name;
		std::uint32_t row = ~0u;
		void (*sample)(float (&a_out)[4]) = nullptr;
	};
}
