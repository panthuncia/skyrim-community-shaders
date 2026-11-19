#pragma once

// The tree manager's list loop, detoured for the stood-in trees. Included by the feature's sources.

namespace DCLF::Features::TreeWind::TreeList
{
	/**
	 * @brief Detours the tree manager's list loop (FUN_140437e50, from FUN_140437d40 on the main thread).
	 * The loop visits a tree only when its first child's multibound cull result is 1, so TreeAnimation sets the stood-in trees' results around it.
	 */
	void Install();
	bool Installed();
	/** @brief Checks the call before any hook is written. It must be a CALL, as the thunk chains. */
	void Preflight();
}
