#pragma once

// Core's side of Api/MaterialWrites.h: whether a feature patches the frame VS variable, and the texture transform moves the kept shadow build drains.

#include "Features/DrawcallLimitFix/Api/MaterialWrites.h"

#include <cstdint>
#include <vector>

namespace DCLF::Extensions
{
	/**
	 * @brief Any thread, lock-free. Whether a feature writes every material row's frame variables each frame.
	 * If so, texture transforms and those variables belong to no record.
	 */
	bool FrameVSVariablePatched();
	/** @brief Main thread, at install. Records that a feature patches the frame VS variable. */
	void NoteFrameVSVariablePatch();

	/**
	 * @brief Kept shadow build thread only. Takes every material whose texture transform moved since the last call.
	 * The kept build rewrites rows on events, so it needs every move. The plain build rereads every row and drains nothing.
	 * Returns true when moves were dropped, because hooks stop queueing while nothing drains. The caller then treats every material as moved.
	 */
	bool DrainTransformMoves(std::vector<const RE::BSShaderMaterial*>& a_out);

	/** @brief Transform move counts since startup: moves the full queue allocated for, and moves dropped while nothing drained. */
	struct TransformMoveCounts
	{
		std::uint64_t spilled = 0;
		std::uint64_t dropped = 0;
	};
	TransformMoveCounts TransformMoveStats();
}
